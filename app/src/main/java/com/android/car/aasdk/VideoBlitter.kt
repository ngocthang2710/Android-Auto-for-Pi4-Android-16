package com.android.car.aasdk

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.CountDownLatch

private const val TAG = "AaSdk_Blitter"

private const val VERTEX_SHADER = """
    attribute vec4 aPosition;
    attribute vec4 aTexCoord;
    uniform mat4 uTexMatrix;
    varying vec2 vTexCoord;
    void main() {
        gl_Position = aPosition;
        vTexCoord = (uTexMatrix * aTexCoord).xy;
    }
"""

private const val FRAGMENT_SHADER = """
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    varying vec2 vTexCoord;
    uniform samplerExternalOES uTexture;
    void main() {
        gl_FragColor = texture2D(uTexture, vTexCoord);
    }
"""

// Full-screen quad, position xy + texcoord uv interleaved.
private val QUAD = floatArrayOf(
    // x,    y,    u,   v
    -1f, -1f, 0f, 0f,
     1f, -1f, 1f, 0f,
    -1f,  1f, 0f, 1f,
     1f,  1f, 1f, 1f,
)

/**
 * Decouples the AMediaCodec video decoder's output target from whatever
 * on-screen Surface the currently-visible Activity happens to provide.
 *
 * The decoder always renders into a single persistent SurfaceTexture owned
 * by this class (created once, never recreated or swapped) -- getInputSurface()
 * is handed to the native decoder exactly once, for the lifetime of the
 * Service. A dedicated GL thread here owns the *only* GL context that ever
 * touches that SurfaceTexture; whenever a frame arrives it is blitted (a
 * plain textured full-screen quad draw) onto whatever on-screen EGLSurface
 * is currently attached via setOutputSurface().
 *
 * This exists because two more "obvious" approaches were tried first and
 * both failed on this device's c2.v4l2.avc.decoder / this Android build:
 *  - AMediaCodec_setOutputSurface(): returns OK but silently renders nothing
 *    on this device's V4L2-backed decoder.
 *  - Destroying/recreating the codec on every Activity recreate: loses
 *    decode state the phone assumes is still live (the video channel is
 *    never reopened just because the HU's Surface changed), so every
 *    subsequent P-frame is undecodable -- permanently black after the first
 *    exit.
 *  - Reusing one SurfaceTexture directly via TextureView.setSurfaceTexture()
 *    across separate Activity/TextureView instances: violates that API's
 *    documented requirement that the SurfaceTexture be detached from any
 *    prior GL context first, and broke rendering even on the very first
 *    connection.
 *
 * Routing through a single, permanently-owned GL context here means the
 * decoder's target truly never changes (fixes the first two) and the only
 * GL context that ever attaches to it is this class's own, created once
 * (fixes the third) -- the Activity's Surface is a cheap, disposable blit
 * target recreated freely on every exit/reenter.
 */
class VideoBlitter(private val width: Int, private val height: Int) {

    private val thread = HandlerThread("AaSdkVideoBlit").apply { start() }
    private val handler = Handler(thread.looper)

    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglConfig: EGLConfig? = null
    private var outputSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    // Kept current on this thread whenever no real on-screen surface is
    // attached -- see setOutputSurface()'s detach branch for why this must
    // never be EGL_NO_CONTEXT/EGL_NO_SURFACE instead.
    private var pbufferSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    // The on-screen surface's own pixel size -- NOT the same as width/height
    // above (the decoder's fixed 1920x1080 output resolution). AaSdkScreenActivity
    // pillarboxes its SurfaceView to a smaller on-screen box (e.g. 1664x936),
    // so glViewport must target *this* size or the quad gets clipped instead
    // of scaled to fit (confirmed live: viewport hardcoded to 1920x1080 while
    // the real surface was 1664x936 cropped the image instead of fitting it).
    private var outputWidth = 0
    private var outputHeight = 0

    private var program = 0
    private var texMatrixHandle = 0
    private var positionHandle = 0
    private var texCoordHandle = 0
    private val texMatrix = FloatArray(16)
    private lateinit var quadBuffer: FloatBuffer

    private lateinit var decodeTexture: SurfaceTexture
    private var decodeTexId = 0
    private lateinit var inputSurface: Surface
    private val ready = CountDownLatch(1)
    private var frameCount = 0L
    private var droppedCount = 0L

    init {
        handler.post {
            setupEgl()
            decodeTexId = createExternalTexture()
            decodeTexture = SurfaceTexture(decodeTexId).apply {
                setDefaultBufferSize(width, height)
                setOnFrameAvailableListener({ onFrameAvailable() }, handler)
            }
            inputSurface = Surface(decodeTexture)
            program = buildProgram()
            positionHandle = GLES20.glGetAttribLocation(program, "aPosition")
            texCoordHandle = GLES20.glGetAttribLocation(program, "aTexCoord")
            texMatrixHandle = GLES20.glGetUniformLocation(program, "uTexMatrix")
            quadBuffer = ByteBuffer.allocateDirect(QUAD.size * 4)
                .order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
                    put(QUAD); position(0)
                }
            Log.i(TAG, "Init OK: eglContext=$eglContext decodeTexId=$decodeTexId program=$program " +
                "posHandle=$positionHandle texCoordHandle=$texCoordHandle texMatrixHandle=$texMatrixHandle")
            ready.countDown()
        }
    }

    /** Blocking; only ever called once, right after construction. */
    fun getInputSurface(): Surface {
        ready.await()
        return inputSurface
    }

    /** Call with the Activity's current Surface, or null when it's torn down. */
    fun setOutputSurface(surface: Surface?) {
        handler.post {
            if (outputSurface != EGL14.EGL_NO_SURFACE) {
                // Fall back to the persistent pbuffer rather than
                // EGL_NO_CONTEXT: the decoder keeps producing frames in the
                // background even with no on-screen surface attached (see
                // AndroidVideoOutput's kdoc), and onFrameAvailable() below
                // unconditionally calls decodeTexture.updateTexImage(),
                // which throws IllegalStateException with no GL context
                // current on this thread -- an uncaught exception on this
                // HandlerThread crashes the whole process. Confirmed live:
                // recurring "FATAL EXCEPTION: AaSdkVideoBlit ... Unable to
                // update texture contents" crashes, always right after an
                // "Output surface detached" log line.
                EGL14.eglMakeCurrent(eglDisplay, pbufferSurface, pbufferSurface, eglContext)
                EGL14.eglDestroySurface(eglDisplay, outputSurface)
                outputSurface = EGL14.EGL_NO_SURFACE
            }
            if (surface != null && !surface.isValid) {
                // Surface died between the Activity posting this call and the
                // GL thread actually running it (e.g. Activity torn down/
                // recreated mid-handshake) -- eglCreateWindowSurface below
                // would throw for the same reason isValid() is already false.
                Log.e(TAG, "Output surface no longer valid, skipping attach")
            } else if (surface != null) {
                outputSurface = try {
                    EGL14.eglCreateWindowSurface(
                        eglDisplay, eglConfig, surface, intArrayOf(EGL14.EGL_NONE), 0)
                } catch (e: IllegalArgumentException) {
                    // Confirmed live: "Make sure the SurfaceView or associated
                    // SurfaceHolder has a valid Surface" -- same race as the
                    // isValid() check above, just lost narrowly: the Surface
                    // went invalid in between. Uncaught, this throws on the
                    // dedicated AaSdkVideoBlit HandlerThread and kills the
                    // whole process (not just this attach attempt).
                    Log.e(TAG, "eglCreateWindowSurface threw: ${e.message}")
                    EGL14.EGL_NO_SURFACE
                }
                if (outputSurface == EGL14.EGL_NO_SURFACE) {
                    if (EGL14.eglGetError() != EGL14.EGL_SUCCESS) {
                        Log.e(TAG, "eglCreateWindowSurface failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
                    }
                } else {
                    val dims = IntArray(2)
                    EGL14.eglQuerySurface(eglDisplay, outputSurface, EGL14.EGL_WIDTH, dims, 0)
                    EGL14.eglQuerySurface(eglDisplay, outputSurface, EGL14.EGL_HEIGHT, dims, 1)
                    outputWidth = dims[0]
                    outputHeight = dims[1]
                    Log.i(TAG, "Output surface attached: $outputSurface size=${outputWidth}x${outputHeight}")
                }
            } else {
                Log.i(TAG, "Output surface detached")
            }
        }
    }

    private fun onFrameAvailable() {
        decodeTexture.updateTexImage()
        if (outputSurface == EGL14.EGL_NO_SURFACE) {
            droppedCount++
            if (droppedCount <= 3 || droppedCount % 100L == 0L) {
                Log.i(TAG, "Frame decoded but dropped (no output surface attached), count=$droppedCount")
            }
            return
        }
        decodeTexture.getTransformMatrix(texMatrix)

        if (!EGL14.eglMakeCurrent(eglDisplay, outputSurface, outputSurface, eglContext)) {
            Log.e(TAG, "eglMakeCurrent failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
            return
        }
        GLES20.glViewport(0, 0, outputWidth, outputHeight)
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, decodeTexId)
        GLES20.glUniformMatrix4fv(texMatrixHandle, 1, false, texMatrix, 0)

        quadBuffer.position(0)
        GLES20.glEnableVertexAttribArray(positionHandle)
        GLES20.glVertexAttribPointer(positionHandle, 2, GLES20.GL_FLOAT, false, 16, quadBuffer)
        quadBuffer.position(2)
        GLES20.glEnableVertexAttribArray(texCoordHandle)
        GLES20.glVertexAttribPointer(texCoordHandle, 2, GLES20.GL_FLOAT, false, 16, quadBuffer)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        val glErr = GLES20.glGetError()
        if (glErr != GLES20.GL_NO_ERROR) {
            Log.e(TAG, "GL error after draw: 0x${Integer.toHexString(glErr)}")
        }

        GLES20.glDisableVertexAttribArray(positionHandle)
        GLES20.glDisableVertexAttribArray(texCoordHandle)

        if (!EGL14.eglSwapBuffers(eglDisplay, outputSurface)) {
            Log.e(TAG, "eglSwapBuffers failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
            return
        }
        frameCount++
        if (frameCount <= 3 || frameCount % 100L == 0L) {
            Log.i(TAG, "Frame rendered OK, count=$frameCount")
        }
    }

    private fun setupEgl() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) {
            Log.e(TAG, "eglGetDisplay failed")
            return
        }
        val version = IntArray(2)
        if (!EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) {
            Log.e(TAG, "eglInitialize failed")
            return
        }
        val attribList = intArrayOf(
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_NONE
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val numConfigs = IntArray(1)
        if (!EGL14.eglChooseConfig(eglDisplay, attribList, 0, configs, 0, 1, numConfigs, 0) ||
            numConfigs[0] == 0) {
            Log.e(TAG, "eglChooseConfig failed")
            return
        }
        eglConfig = configs[0]
        val ctxAttribs = intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE)
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT, ctxAttribs, 0)
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            Log.e(TAG, "eglCreateContext failed")
        }
        // Texture creation/attachment below needs a current context, even
        // though we have no window surface yet -- use a 1x1 pbuffer.
        val pbufferAttribs = intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE)
        pbufferSurface = EGL14.eglCreatePbufferSurface(eglDisplay, eglConfig, pbufferAttribs, 0)
        EGL14.eglMakeCurrent(eglDisplay, pbufferSurface, pbufferSurface, eglContext)
    }

    private fun createExternalTexture(): Int {
        val texIds = IntArray(1)
        GLES20.glGenTextures(1, texIds, 0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texIds[0])
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        return texIds[0]
    }

    private fun buildProgram(): Int {
        val vs = compileShader(GLES20.GL_VERTEX_SHADER, VERTEX_SHADER)
        val fs = compileShader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT_SHADER)
        val prog = GLES20.glCreateProgram()
        GLES20.glAttachShader(prog, vs)
        GLES20.glAttachShader(prog, fs)
        GLES20.glLinkProgram(prog)
        val status = IntArray(1)
        GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) {
            Log.e(TAG, "Program link failed: ${GLES20.glGetProgramInfoLog(prog)}")
        }
        return prog
    }

    private fun compileShader(type: Int, src: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, src)
        GLES20.glCompileShader(shader)
        val status = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            Log.e(TAG, "Shader compile failed: ${GLES20.glGetShaderInfoLog(shader)}")
        }
        return shader
    }
}
