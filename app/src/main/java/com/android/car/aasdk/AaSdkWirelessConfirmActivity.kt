package com.android.car.aasdk

import android.app.Activity
import android.content.ComponentName
import android.content.Intent
import android.content.ServiceConnection
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

private const val TICK_MS = 1000L

/**
 * Full-screen "Start Android Auto (Wireless)?" prompt, shown by
 * AaSdkUsbService.requestWirelessStartConfirmation() whenever a phone
 * connects to the BT AA-discovery socket with no USB device already
 * attached. Reports the user's choice back to the service, which is
 * blocking a background thread (the BT accept thread) waiting for it.
 *
 * Launched the same way AaSdkScreenActivity already is from this service
 * (FLAG_ACTIVITY_NEW_TASK from a background thread), so it interrupts
 * whatever's currently on screen -- consistent with existing precedent, no
 * extra permissions needed.
 */
class AaSdkWirelessConfirmActivity : Activity() {

    private var service: AaSdkUsbService? = null
    private val handler = Handler(Looper.getMainLooper())
    private var answered = false
    private var countdownView: TextView? = null
    private var remainingMs = WIRELESS_CONFIRM_TIMEOUT_MS

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            service = (binder as AaSdkUsbService.LocalBinder).getService()
            service?.setConfirmDismissListener { finish() }
        }
        override fun onServiceDisconnected(name: ComponentName?) { service = null }
    }

    private val countdownRunnable = object : Runnable {
        override fun run() {
            remainingMs -= TICK_MS
            if (remainingMs <= 0) {
                answer(started = false)
                return
            }
            countdownView?.text = "Auto-cancel in ${remainingMs / 1000}s"
            handler.postDelayed(this, TICK_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildContentView())
        bindService(Intent(this, AaSdkUsbService::class.java), conn, BIND_AUTO_CREATE)
        handler.postDelayed(countdownRunnable, TICK_MS)
    }

    override fun onDestroy() {
        // Covers back-press or any other dismissal that didn't go through a
        // button tap or the countdown -- treat it as Cancel, not silence.
        answer(started = false)
        service?.setConfirmDismissListener(null)
        unbindService(conn)
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    private fun answer(started: Boolean) {
        if (answered) return
        answered = true
        service?.reportWirelessConfirmationResult(started)
        finish()
    }

    private fun buildContentView(): ViewGroup {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(Color.BLACK)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }

        val title = TextView(this).apply {
            text = "Start Android Auto (Wireless)?"
            setTextColor(Color.WHITE)
            textSize = 28f
            gravity = Gravity.CENTER
        }
        root.addView(title)

        val countdown = TextView(this).apply {
            text = "Auto-cancel in ${WIRELESS_CONFIRM_TIMEOUT_MS / 1000}s"
            setTextColor(Color.LTGRAY)
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(0, 24, 0, 48)
        }
        root.addView(countdown)
        countdownView = countdown

        val buttons = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }
        fun newButtonParams() = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            marginStart = 24
            marginEnd = 24
        }

        buttons.addView(Button(this).apply {
            text = "Cancel"
            layoutParams = newButtonParams()
            setOnClickListener { answer(started = false) }
        })
        buttons.addView(Button(this).apply {
            text = "Start"
            layoutParams = newButtonParams()
            setOnClickListener { answer(started = true) }
        })
        root.addView(buttons)

        return root
    }
}
