package com.android.car.aasdk

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.util.Log

private const val TAG = "AaSdk_Attach"
private const val ACTION_USB_PERMISSION = "com.android.car.aasdk.USB_PERMISSION"

// AOA accessory mode product IDs
private const val AOA_PID_ACC     = 0x2D00
private const val AOA_PID_ACC_ADB = 0x2D01

// AOA vendor ID (Google)
private const val GOOGLE_VID = 0x18D1

class UsbAttachActivity : Activity() {

    private lateinit var usbManager: UsbManager
    private var pendingDevice: UsbDevice? = null

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            unregisterReceiver(this)
            val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
            if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                handleDeviceWithPermission(device)
            } else {
                Log.e(TAG, "USB permission denied for ${device.deviceName}")
            }
            finish()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        usbManager = getSystemService(USB_SERVICE) as UsbManager

        val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: run {
            finish(); return
        }

        if (isInAccessoryMode(device)) {
            // Already in AOAP mode — hand off to service immediately
            startService(Intent(this, AaSdkUsbService::class.java).apply {
                putExtra(AaSdkUsbService.EXTRA_USB_DEVICE, device)
            })
            finish()
            return
        }

        // Phone is not yet in accessory mode — request permission to do mode switch
        if (usbManager.hasPermission(device)) {
            switchToAccessoryMode(device)
            finish()
        } else {
            pendingDevice = device
            val permIntent = PendingIntent.getBroadcast(
                this, 0,
                Intent(ACTION_USB_PERMISSION).apply { setPackage(packageName) },
                PendingIntent.FLAG_IMMUTABLE)
            registerReceiver(permissionReceiver, IntentFilter(ACTION_USB_PERMISSION))
            usbManager.requestPermission(device, permIntent)
        }
    }

    private fun handleDeviceWithPermission(device: UsbDevice) {
        if (isInAccessoryMode(device)) {
            startService(Intent(this, AaSdkUsbService::class.java).apply {
                putExtra(AaSdkUsbService.EXTRA_USB_DEVICE, device)
            })
        } else {
            switchToAccessoryMode(device)
        }
    }

    private fun isInAccessoryMode(device: UsbDevice): Boolean {
        return device.vendorId == GOOGLE_VID &&
               (device.productId == AOA_PID_ACC || device.productId == AOA_PID_ACC_ADB)
    }

    private fun switchToAccessoryMode(device: UsbDevice) {
        val conn = usbManager.openDevice(device) ?: run {
            Log.e(TAG, "Cannot open device ${device.deviceName}"); return
        }

        // AOA protocol: send manufacturer/model/description/version/URI/serial strings.
        // manufacturer/model must be exactly "Android"/"Android Auto" -- the phone's
        // Android Auto app only launches to handle the accessory when these match its
        // own USB_ACCESSORY_ATTACHED intent-filter; anything else and the phone silently
        // ignores the accessory (no consent dialog, no protocol response).
        val strings = arrayOf(
            "Android",           // manufacturer
            "Android Auto",      // model
            "Android Auto Head Unit on AOSP Pi4",  // description
            "1.0",               // version
            "https://accessories.android.com/",     // URI
            "rpi4-aaos-001"     // serial
        )
        strings.forEachIndexed { index, str ->
            conn.controlTransfer(
                0x40,      // requestType: vendor | host-to-device | device
                52,        // request: ACCESSORY_SEND_STRING
                0,         // value
                index,     // index = string ID (0..5)
                str.toByteArray(Charsets.UTF_8),
                str.length,
                1000)
        }

        // Tell the phone to switch to accessory mode
        conn.controlTransfer(
            0x40,
            53,    // ACCESSORY_START
            0, 0, null, 0, 1000)

        conn.close()
        Log.i(TAG, "Sent AOAP mode-switch to ${device.deviceName}; waiting for re-enumeration")
        // The phone will disconnect+reconnect as accessory — Android will fire
        // USB_DEVICE_ATTACHED again, which will trigger UsbAttachActivity again.
    }
}
