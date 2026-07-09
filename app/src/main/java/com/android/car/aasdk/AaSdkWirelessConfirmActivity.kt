package com.android.car.aasdk

import android.app.Activity
import android.content.ComponentName
import android.content.Intent
import android.content.ServiceConnection
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView

private const val TICK_MS = 1000L

// Card palette -- dark modal on a black backdrop, single blue accent shared
// by the icon ring, the countdown value, and the Start button, matching the
// approved mock (see PR discussion 2026-07-10).
private const val COLOR_BACKDROP = 0xFF000000.toInt()
private const val COLOR_CARD_BG = 0xFF14161C.toInt()
private const val COLOR_CARD_BORDER = 0xFF262A33.toInt()
private const val COLOR_ICON_BG = 0xFF122036.toInt()
private const val COLOR_ACCENT = 0xFF2F7BFF.toInt()
private const val COLOR_SUBTITLE = 0xFF98A2B3.toInt()
private const val COLOR_PILL_BG = 0xFF1B1E26.toInt()
private const val COLOR_PILL_BORDER = 0xFF2A2E38.toInt()
private const val COLOR_PILL_LABEL = 0xFF9AA3B2.toInt()
private const val COLOR_DIVIDER = 0xFF252932.toInt()
private const val COLOR_CANCEL_BORDER = 0xFF3A3F4B.toInt()
private const val COLOR_CANCEL_TEXT = 0xFFE5E7EB.toInt()

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
    private var countdownValueView: TextView? = null
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
            countdownValueView?.text = "${remainingMs / 1000}s"
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

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private fun roundedDrawable(
        fillColor: Int, strokeColor: Int? = null, strokeWidthDp: Int = 1, radiusDp: Int = 24,
    ) = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(radiusDp).toFloat()
        setColor(fillColor)
        strokeColor?.let { setStroke(dp(strokeWidthDp), it) }
    }

    private fun buildContentView(): ViewGroup {
        val root = FrameLayout(this).apply {
            setBackgroundColor(COLOR_BACKDROP)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }

        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            background = roundedDrawable(COLOR_CARD_BG, COLOR_CARD_BORDER, radiusDp = 28)
            setPadding(dp(40), dp(44), dp(40), dp(32))
            layoutParams = FrameLayout.LayoutParams(dp(520), ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.CENTER
            }
        }
        root.addView(card)

        val iconCircle = FrameLayout(this).apply {
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(COLOR_ICON_BG)
                setStroke(dp(2), COLOR_ACCENT)
            }
            layoutParams = LinearLayout.LayoutParams(dp(88), dp(88)).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                bottomMargin = dp(24)
            }
        }
        iconCircle.addView(ImageView(this).apply {
            setImageResource(R.mipmap.ic_launcher)
            layoutParams = FrameLayout.LayoutParams(dp(48), dp(48)).apply { gravity = Gravity.CENTER }
        })
        card.addView(iconCircle)

        card.addView(TextView(this).apply {
            text = "Start Android Auto (Wireless)?"
            setTextColor(Color.WHITE)
            textSize = 24f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        })

        card.addView(TextView(this).apply {
            text = "Android Auto will start on your phone."
            setTextColor(COLOR_SUBTITLE)
            textSize = 15f
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(10)
            }
        })

        val pill = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = roundedDrawable(COLOR_PILL_BG, COLOR_PILL_BORDER, radiusDp = 100)
            setPadding(dp(16), dp(10), dp(16), dp(10))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                topMargin = dp(24)
            }
        }
        pill.addView(TextView(this).apply {
            text = "⏱"
            setTextColor(COLOR_ACCENT)
            textSize = 15f
            setPadding(0, 0, dp(8), 0)
        })
        pill.addView(TextView(this).apply {
            text = "Auto-cancel in"
            setTextColor(COLOR_PILL_LABEL)
            textSize = 15f
        })
        pill.addView(TextView(this).apply {
            text = "${remainingMs / 1000}s"
            setTextColor(COLOR_ACCENT)
            textSize = 15f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            setPadding(dp(6), 0, 0, 0)
            countdownValueView = this
        })
        card.addView(pill)

        card.addView(View(this).apply {
            setBackgroundColor(COLOR_DIVIDER)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1)).apply {
                topMargin = dp(32)
                bottomMargin = dp(24)
            }
        })

        val buttons = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }

        buttons.addView(TextView(this).apply {
            text = "✕  Cancel"
            setTextColor(COLOR_CANCEL_TEXT)
            textSize = 17f
            gravity = Gravity.CENTER
            background = roundedDrawable(Color.TRANSPARENT, COLOR_CANCEL_BORDER, radiusDp = 16)
            setPadding(dp(24), dp(18), dp(24), dp(18))
            isClickable = true
            isFocusable = true
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginEnd = dp(12)
            }
            setOnClickListener { answer(started = false) }
        })

        buttons.addView(TextView(this).apply {
            text = "▶  Start"
            setTextColor(Color.WHITE)
            textSize = 17f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            gravity = Gravity.CENTER
            background = roundedDrawable(COLOR_ACCENT, radiusDp = 16)
            setPadding(dp(24), dp(18), dp(24), dp(18))
            isClickable = true
            isFocusable = true
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = dp(12)
            }
            setOnClickListener { answer(started = true) }
        })

        card.addView(buttons)
        return root
    }
}
