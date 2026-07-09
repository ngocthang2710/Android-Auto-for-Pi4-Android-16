package com.android.car.aasdk

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

// Confirmed live 2026-07-09: without this, AaSdkUsbService (and with it the
// RFCOMM listener that makes a BT-only connect show the wireless confirm
// dialog at all) only ever started via AaSdkScreenActivity binding to it --
// itself only reachable by a real USB attach, a manual tap on the app icon,
// or the car launcher's Dock resurrecting a previously-used app. On a fresh
// boot with none of those having happened yet, dumpsys showed the service
// simply wasn't running at all ("(nothing)") -- connecting BT had nothing
// listening on the RFCOMM socket to respond to. Starting the service here
// (dry-run verified live via `am start-foreground-service`: WiFi/RFCOMM
// listeners come up immediately and the process stays foreground-exempt,
// not frozen) makes BT-connect-shows-dialog work from the very first boot,
// with no user action required first.
class AaSdkBootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        context.startForegroundService(Intent(context, AaSdkUsbService::class.java))
    }
}
