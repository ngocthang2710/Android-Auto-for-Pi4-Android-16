package com.android.car.aasdk

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Binder
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.Surface
import java.io.IOException
import java.net.ServerSocket

// Matches VideoService::kDefaultConfigIndex (see AndroidVideoOutput's callers) --
// the decoder always configures at this resolution regardless of the phone/screen.
private const val VIDEO_WIDTH = 1920
private const val VIDEO_HEIGHT = 1080

private const val TAG = "AaSdk_Svc"
private const val NOTIF_ID = 1
private const val CHANNEL_ID = "aasdk_channel"
// Same TCP port real Android Auto Wireless projection connects to, so this
// listener stays compatible with a future Bluetooth WiFi-handoff handshake.
private const val WIFI_PROJECTION_PORT = 5288

class AaSdkUsbService : Service() {

    companion object {
        const val EXTRA_USB_DEVICE = "extra_usb_device"

        init {
            System.loadLibrary("aasdk_jni")
        }
    }

    // JNI methods
    private external fun nativeCreate(): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeOnAccessoryAttached(handle: Long, fd: Int)
    private external fun nativeOnAccessoryDetached(handle: Long)
    private external fun nativeOnTcpAccepted(handle: Long, fd: Int)
    private external fun nativeSetSurface(handle: Long, surface: Surface?)
    private external fun nativeSendTouchEvent(handle: Long, action: Int, x: Float, y: Float)
    private external fun nativeResetSession(handle: Long)

    inner class LocalBinder : Binder() {
        fun getService() = this@AaSdkUsbService
    }

    /** Notified when the phone disconnects, so the UI can leave the AA screen. */
    fun interface DetachListener {
        fun onAccessoryDetached()
    }

    private val binder = LocalBinder()
    private var nativeHandle = 0L
    private var usbConnection: android.hardware.usb.UsbDeviceConnection? = null
    private var attachedDevice: UsbDevice? = null
    private var detachListener: DetachListener? = null
    private var wifiServerSocket: ServerSocket? = null
    private var wifiAcceptThread: Thread? = null
    private val softAp = AaSdkSoftApHotspot(this)
    private val btWireless = AaSdkBtWirelessHandshake(softAp)

    // Decodes into a permanent SurfaceTexture and blits each frame onto
    // whatever on-screen Surface AaSdkScreenActivity currently provides --
    // see VideoBlitter's kdoc for why the decoder's own output target must
    // never change or be swapped on this device.
    private var videoBlitter: VideoBlitter? = null

    fun setDetachListener(listener: DetachListener?) {
        detachListener = listener
    }

    private val detachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
            if (device.deviceName != attachedDevice?.deviceName) return
            Log.i(TAG, "Accessory detached, tearing down session")
            onAccessoryDetached()
        }
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIF_ID, buildNotification())
        nativeHandle = nativeCreate()
        if (nativeHandle == 0L) Log.e(TAG, "nativeCreate failed")
        val blitter = VideoBlitter(VIDEO_WIDTH, VIDEO_HEIGHT)
        videoBlitter = blitter
        if (nativeHandle != 0L) nativeSetSurface(nativeHandle, blitter.getInputSurface())
        registerReceiver(detachReceiver, IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED))
        startWifiListener()
        // Starting this does NOT turn on the AP by itself -- AaSdkBtWirelessHandshake
        // only starts AaSdkSoftApHotspot once a phone actually BT-connects asking
        // for wireless AA, so a plain wired-USB session never touches WiFi.
        btWireless.start(WIFI_PROJECTION_PORT)
    }

    /** Called by AaSdkScreenActivity with its current Surface, or null when torn down. */
    fun attachDisplaySurface(surface: Surface?) {
        videoBlitter?.setOutputSurface(surface)
    }

    // Wireless (TCP) sessions have been observed going silent while the HU
    // screen is backgrounded -- the socket stays connected but the phone
    // never responds to anything again, leaving a permanently black screen
    // on return with no local error. USB sessions don't need this: the
    // phone doesn't reconnect just because our Activity was recreated (see
    // AndroidVideoOutput::init()), and forcing a reset would mean a real
    // USB AOAP replug, which this can't trigger anyway -- attachedDevice
    // being null is exactly the signal that the current session is TCP.
    //
    // First attempt only closed the native TCP session (nativeResetSession)
    // -- confirmed by log NOT to work: the phone's own AA app never
    // reconnected afterwards (no further "TCP client connected" ever
    // logged), because from the phone's side the BT link and WiFi AP both
    // still looked fine, so it had no signal to redo discovery. Must tear
    // down the BT+AP link too (btWireless.forceDisconnect(), which tears
    // down the SoftAP via its own teardown path) -- that's what a full app
    // process restart did by accident in earlier testing, and it was the
    // one thing observed to reliably make the phone redo its full 5-stage
    // handshake and reconnect.
    fun resetWirelessSessionForReentry() {
        if (nativeHandle != 0L && attachedDevice == null) {
            Log.i(TAG, "Re-entering AA screen on a wireless session -- forcing reconnect")
            nativeResetSession(nativeHandle)
            btWireless.forceDisconnect()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val device = intent?.getParcelableExtra<UsbDevice>(EXTRA_USB_DEVICE) ?: return START_NOT_STICKY
        connectDevice(device)
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        unregisterReceiver(detachReceiver)
        stopWifiListener()
        btWireless.stop() // also tears down the AP if one is up (see its own stop())
        usbConnection?.close()
        usbConnection = null
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
        super.onDestroy()
    }

    // Accepts wireless-projection TCP connections and hands each one to the
    // same native session slot USB uses (createAndroidAutoSessionTcp). This is
    // a secondary, optional path -- any failure here must stay contained to
    // this thread and never take down the service (and with it the USB
    // flow), so this catches Throwable rather than just IOException.
    private fun startWifiListener() {
        wifiAcceptThread = Thread {
            try {
                ServerSocket(WIFI_PROJECTION_PORT).use { server ->
                    wifiServerSocket = server
                    Log.i(TAG, "WiFi AA listener on port $WIFI_PROJECTION_PORT")
                    while (!Thread.currentThread().isInterrupted) {
                        val socket = server.accept()
                        Log.i(TAG, "TCP client connected: ${socket.inetAddress}")
                        val fd = ParcelFileDescriptor.fromSocket(socket).detachFd()
                        socket.close()
                        if (nativeHandle != 0L) {
                            nativeOnTcpAccepted(nativeHandle, fd)
                            startActivity(Intent(this, AaSdkScreenActivity::class.java).apply {
                                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                            })
                        }
                    }
                }
            } catch (e: Throwable) {
                Log.e(TAG, "WiFi listener stopped: ${e.message}", e)
            }
        }.apply { isDaemon = true; start() }
    }

    private fun stopWifiListener() {
        wifiAcceptThread?.interrupt()
        try {
            wifiServerSocket?.close()
        } catch (e: IOException) {
            // already closed
        }
        wifiServerSocket = null
        wifiAcceptThread = null
    }

    private fun onAccessoryDetached() {
        if (nativeHandle != 0L) nativeOnAccessoryDetached(nativeHandle)
        usbConnection?.close()
        usbConnection = null
        attachedDevice = null
        detachListener?.onAccessoryDetached()
    }

    fun sendTouchEvent(action: Int, x: Float, y: Float) {
        if (nativeHandle != 0L) {
            Log.d(TAG, "sendTouchEvent action=$action x=$x y=$y handle=$nativeHandle")
            nativeSendTouchEvent(nativeHandle, action, x, y)
        } else {
            Log.w(TAG, "sendTouchEvent dropped: nativeHandle is 0")
        }
    }

    // Called from JNI (InputService touch ack) — currently unused from C++ side
    @Suppress("unused")
    fun onTouchEvent(action: Int, x: Float, y: Float) {
        // Reserved for reverse touch (phone→HU direction)
    }

    private fun connectDevice(device: UsbDevice) {
        if (nativeHandle == 0L) return
        val usbManager = getSystemService(USB_SERVICE) as UsbManager
        val conn = usbManager.openDevice(device) ?: run {
            Log.e(TAG, "Cannot open device"); return
        }
        usbConnection = conn
        attachedDevice = device
        val fd = conn.fileDescriptor
        Log.i(TAG, "Accessory fd=$fd, starting C++ session")
        nativeOnAccessoryAttached(nativeHandle, fd)
        startActivity(Intent(this, AaSdkScreenActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
    }

    private fun createNotificationChannel() {
        val ch = NotificationChannel(CHANNEL_ID, "Android Auto HU", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
    }

    private fun buildNotification(): Notification =
        Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Android Auto")
            .setContentText("Head unit active")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .build()
}
