package com.android.car.aasdk

import android.car.AoapService
import android.hardware.usb.UsbDevice

/**
 * Bound by android.car.usb.handler (CarUsbHandler) while it is deciding how to
 * handle a newly-attached USB device. Declared as this app's AOAP handler in
 * device_filter.xml so CarUsbHandler can auto-select and auto-switch the phone
 * to accessory mode without showing its multi-app disambiguation dialog.
 */
class AaSdkAoapService : AoapService() {
    override fun isDeviceSupported(device: UsbDevice): Int = RESULT_OK
}
