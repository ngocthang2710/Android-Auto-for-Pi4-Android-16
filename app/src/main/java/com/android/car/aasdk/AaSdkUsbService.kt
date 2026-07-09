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
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.Surface
import java.io.IOException
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

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
// Matches AndroidAutoEntity::kWatchdogIntervalMs -- no point polling faster
// than the native watchdog itself can possibly flag a new fatal event.
private const val FATAL_CHECK_INTERVAL_MS = 2000L
// How long AaSdkWirelessConfirmActivity waits for a Start/Cancel tap before
// auto-declining. Shared (not private) so the Activity can drive the same
// on-screen countdown instead of guessing a duration independently.
const val WIRELESS_CONFIRM_TIMEOUT_MS = 18000L

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
    private external fun nativeCheckFatalError(handle: Long): Boolean

    inner class LocalBinder : Binder() {
        fun getService() = this@AaSdkUsbService
    }

    /** Notified when the phone disconnects, so the UI can leave the AA screen. */
    fun interface DetachListener {
        fun onAccessoryDetached()
    }

    /** Notified when a still-open wireless-start confirmation must close itself. */
    fun interface ConfirmDismissListener {
        fun onDismissRequested()
    }

    private val binder = LocalBinder()
    private var nativeHandle = 0L
    private var usbConnection: android.hardware.usb.UsbDeviceConnection? = null
    private var attachedDevice: UsbDevice? = null
    private var detachListener: DetachListener? = null
    private var confirmDismissListener: ConfirmDismissListener? = null
    private var wifiServerSocket: ServerSocket? = null
    private var wifiAcceptThread: Thread? = null
    private val softAp = AaSdkSoftApHotspot(this)
    private val btWireless = AaSdkBtWirelessHandshake(softAp) { requestWirelessStartConfirmation(it) }

    // Written from the BT accept thread (requestWirelessStartConfirmation),
    // read/counted-down from the main thread inside the confirm Activity's
    // binder calls -- must be @Volatile for cross-thread visibility, same as
    // AaSdkBtWirelessHandshake.activeSocket.
    @Volatile private var pendingConfirmLatch: CountDownLatch? = null
    @Volatile private var pendingConfirmResult: Boolean = false

    // Decodes into a permanent SurfaceTexture and blits each frame onto
    // whatever on-screen Surface AaSdkScreenActivity currently provides --
    // see VideoBlitter's kdoc for why the decoder's own output target must
    // never change or be swapped on this device.
    private var videoBlitter: VideoBlitter? = null

    // Confirmed live 2026-07-08: a session can go transport-dead (repeated
    // SSL_READ/WRITE errors, zero ping responses) with no USB detach
    // broadcast at all -- nothing else notices, so the HU sits frozen on
    // the last rendered frame until manually unplugged/replugged. The
    // native watchdog (AndroidAutoEntity) now detects this and stops
    // itself internally, but can't reach the UI on its own -- poll for
    // that flag and treat it exactly like an accessory detach.
    private val fatalCheckHandler = Handler(Looper.getMainLooper())
    private val fatalCheckRunnable = object : Runnable {
        override fun run() {
            if (nativeHandle != 0L && nativeCheckFatalError(nativeHandle)) {
                Log.e(TAG, "Native session reported fatal transport error, tearing down")
                onAccessoryDetached()
            }
            fatalCheckHandler.postDelayed(this, FATAL_CHECK_INTERVAL_MS)
        }
    }

    fun setDetachListener(listener: DetachListener?) {
        detachListener = listener
    }

    fun setConfirmDismissListener(listener: ConfirmDismissListener?) {
        confirmDismissListener = listener
    }

    // Called from AaSdkBtWirelessHandshake's own BT accept thread (never the
    // main thread) -- blocks that thread until the user answers the
    // on-screen prompt, the timeout elapses, or connectDevice() preempts it
    // because USB just attached. Returns false (no dialog shown at all) if
    // USB is already attached, since USB always wins outright.
    private fun requestWirelessStartConfirmation(timeoutMs: Long): Boolean {
        if (attachedDevice != null) {
            Log.i(TAG, "USB already attached, declining wireless AA without prompting")
            return false
        }
        // Confirmed live 2026-07-09: without this check, ANY BT reconnect of
        // an already-approved session (e.g. resetWirelessSessionForReentry()'s
        // own forceDisconnect-then-reconnect when the AA screen is simply
        // re-entered) re-triggered this whole prompt. The dialog then covered
        // AaSdkScreenActivity, which made IT look backgrounded, which on
        // resume called resetWirelessSessionForReentry() again -- an infinite
        // reconnect/reprompt loop (AP flapping every few seconds, "socket
        // closed" handshake failures) confirmed via logcat + activity-stack
        // dump. detachListener is non-null for as long as AaSdkScreenActivity
        // is alive (set in its onServiceConnected, cleared in its onDestroy),
        // regardless of foreground/background -- a reliable "this is a
        // reconnect within an existing engagement, not a new ask" signal.
        if (detachListener != null) {
            Log.i(TAG, "AA screen already active, auto-approving wireless reconnect")
            return true
        }
        val latch = CountDownLatch(1)
        pendingConfirmResult = false
        pendingConfirmLatch = latch
        startActivity(Intent(this, AaSdkWirelessConfirmActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        pendingConfirmLatch = null
        return pendingConfirmResult
    }

    /** Called by AaSdkWirelessConfirmActivity on Start/Cancel or its own countdown. */
    fun reportWirelessConfirmationResult(started: Boolean) {
        pendingConfirmResult = started
        pendingConfirmLatch?.countDown()
    }

    // Called from connectDevice() so a USB attach can preempt a still-open
    // confirmation prompt before the user answers it. No-op if none pending.
    private fun cancelPendingWirelessConfirmation() {
        val latch = pendingConfirmLatch ?: return
        pendingConfirmResult = false
        latch.countDown()
        confirmDismissListener?.onDismissRequested()
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
        fatalCheckHandler.postDelayed(fatalCheckRunnable, FATAL_CHECK_INTERVAL_MS)
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
        fatalCheckHandler.removeCallbacks(fatalCheckRunnable)
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
        // USB always wins over a wireless session or a still-pending
        // wireless-start prompt. All three calls are safe no-ops when
        // nothing wireless is active/pending, so no branching is needed.
        // Same nativeResetSession()-then-forceDisconnect() order as
        // resetWirelessSessionForReentry(), which established that both are
        // needed together (nativeResetSession alone left the phone's own AA
        // app thinking the link was still fine, never reconnecting):
        // - cancelPendingWirelessConfirmation() closes an open confirm dialog.
        // - nativeResetSession() clears any lingering native TCP session --
        //   without this, nativeOnAccessoryAttached()'s "session already
        //   running" guard would silently drop this USB attach.
        // - forceDisconnect() tears down the BT link + SoftAP (see its own
        //   comment) if a wireless session was running.
        cancelPendingWirelessConfirmation()
        nativeResetSession(nativeHandle)
        btWireless.forceDisconnect()
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
