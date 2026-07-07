package com.android.car.aasdk

import android.app.Activity
import android.content.ComponentName
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.FrameLayout

private const val TAG = "AaSdk_Screen"

// AA video is always negotiated/decoded at 1920x1080 (see VideoService::
// kDefaultConfigIndex) -- the physical screen here is wider than 16:9, so
// the SurfaceView must be sized to this aspect ratio and centered (rather
// than stretched to fill the screen) or the image looks squashed/stretched
// horizontally.
private const val AA_ASPECT_W = 1920f
private const val AA_ASPECT_H = 1080f

// This Surface is purely a disposable on-screen blit target -- VideoBlitter
// (owned by the Service) decodes into its own permanent SurfaceTexture and
// draws each frame here via GL, so this Surface can be freely torn down and
// recreated on every exit/reenter without touching the decoder at all. See
// VideoBlitter's kdoc for the two approaches that were tried and failed
// before landing on this one.
class AaSdkScreenActivity : Activity(), SurfaceHolder.Callback {

    private var service: AaSdkUsbService? = null
    private var surfaceView: SurfaceView? = null
    private var surfaceWidth = 0
    private var surfaceHeight = 0

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            service = (binder as AaSdkUsbService.LocalBinder).getService()
            surfaceView?.holder?.surface?.let { service?.attachDisplaySurface(it) }
            service?.setDetachListener { goHomeAndFinish() }
        }
        override fun onServiceDisconnected(name: ComponentName?) { service = null }
    }

    // Phone unplugged mid-session -- leave the AA screen and return to the
    // car's home screen instead of sitting on a now-dead/black session.
    private fun goHomeAndFinish() {
        startActivity(Intent(Intent.ACTION_MAIN).apply {
            addCategory(Intent.CATEGORY_HOME)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
        finish()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val sv = SurfaceView(this)
        sv.layoutParams = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT)
        sv.holder.addCallback(this)

        // This activity is EDGE_TO_EDGE_ENFORCED (Android 15+ policy), so
        // content draws full-screen by default -- including underneath
        // AAOS's persistent BottomCarSystemBar (and any top status bar),
        // which then visually covers the bottom of the AA video. Apply
        // system bar insets as padding so the SurfaceView (and everything
        // rendered into it) only occupies the actually-visible area.
        val root = FrameLayout(this)
        root.addView(sv)
        root.setOnApplyWindowInsetsListener { view, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        // Re-fit sv to a centered 16:9 box every time root's usable (post-
        // padding) area changes, instead of letting it stretch to fill
        // whatever aspect ratio the physical screen happens to be.
        root.addOnLayoutChangeListener { view, _, _, _, _, _, _, _, _ ->
            val availW = view.width - view.paddingLeft - view.paddingRight
            val availH = view.height - view.paddingTop - view.paddingBottom
            if (availW <= 0 || availH <= 0) return@addOnLayoutChangeListener
            val scale = minOf(availW / AA_ASPECT_W, availH / AA_ASPECT_H)
            val targetW = (AA_ASPECT_W * scale).toInt()
            val targetH = (AA_ASPECT_H * scale).toInt()
            val lp = sv.layoutParams as FrameLayout.LayoutParams
            if (lp.width != targetW || lp.height != targetH || lp.gravity != Gravity.CENTER) {
                lp.width = targetW
                lp.height = targetH
                lp.gravity = Gravity.CENTER
                sv.layoutParams = lp
            }
        }
        setContentView(root)
        surfaceView = sv

        bindService(
            Intent(this, AaSdkUsbService::class.java),
            conn,
            BIND_AUTO_CREATE)
    }

    override fun onDestroy() {
        service?.setDetachListener(null)
        service?.attachDisplaySurface(null)
        unbindService(conn)
        super.onDestroy()
    }

    // SurfaceHolder.Callback
    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.d(TAG, "surfaceChanged ${width}x${height}")
        surfaceWidth = width
        surfaceHeight = height
        service?.attachDisplaySurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        service?.attachDisplaySurface(null)
    }

    // Forward touch events to the phone via JNI → InputService
    override fun onTouchEvent(event: MotionEvent): Boolean {
        Log.d(TAG, "onTouchEvent action=${event.actionMasked} raw=(${event.x},${event.y}) " +
            "surface=${surfaceWidth}x${surfaceHeight} service=${service != null}")
        val svc = service ?: return false
        val sv = surfaceView ?: return false
        if (surfaceWidth == 0 || surfaceHeight == 0) return false

        // event.x/y are in full-window coordinates, but sv is now a
        // centered, aspect-ratio-fitted box inside root (not full-window) --
        // subtract its actual on-screen position (root's padding plus the
        // pillarbox/letterbox centering offset) or every coordinate drifts.
        val localX = event.x - sv.left
        val localY = event.y - sv.top

        // Scale to AA resolution (1920x1080, matches InputService/VideoService)
        // from actual surface size.
        val aaX = localX * 1920f / surfaceWidth
        val aaY = localY * 1080f / surfaceHeight

        // Android action codes match AA touch action codes (DOWN=0, UP=1, MOVE=2)
        val action = event.actionMasked
        svc.sendTouchEvent(action, aaX, aaY)
        return true
    }
}
