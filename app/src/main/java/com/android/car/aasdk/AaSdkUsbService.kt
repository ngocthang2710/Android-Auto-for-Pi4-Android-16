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
import android.util.Log
import android.view.Surface

private const val TAG = "AaSdk_Svc"
private const val NOTIF_ID = 1
private const val CHANNEL_ID = "aasdk_channel"

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
    private external fun nativeSetSurface(handle: Long, surface: Surface?)
    private external fun nativeSendTouchEvent(handle: Long, action: Int, x: Float, y: Float)

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
        registerReceiver(detachReceiver, IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val device = intent?.getParcelableExtra<UsbDevice>(EXTRA_USB_DEVICE) ?: return START_NOT_STICKY
        connectDevice(device)
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        unregisterReceiver(detachReceiver)
        usbConnection?.close()
        usbConnection = null
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
        super.onDestroy()
    }

    private fun onAccessoryDetached() {
        if (nativeHandle != 0L) nativeOnAccessoryDetached(nativeHandle)
        usbConnection?.close()
        usbConnection = null
        attachedDevice = null
        detachListener?.onAccessoryDetached()
    }

    fun setSurface(surface: Surface?) {
        if (nativeHandle != 0L) nativeSetSurface(nativeHandle, surface)
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
