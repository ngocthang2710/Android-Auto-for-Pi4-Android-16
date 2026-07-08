#include <AndroidAutoSession.hpp>
#include <AndroidVideoOutput.hpp>
#include <f1x/aasdk/USB/USBWrapper.hpp>
#include <libusb/libusb.h>
#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#define LOG_TAG "AaSdk_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace aasdk_android;
using namespace f1x::aasdk;

struct NativeContext {
    libusb_context* usbCtx{nullptr};
    std::unique_ptr<usb::USBWrapper> usbWrapper;
    AaSdkUsbSessionPtr session{nullptr, destroyAndroidAutoSession};
    ANativeWindow* pendingWindow{nullptr};
    std::mutex mu;

    // aasdk's USBTransport submits async libusb transfers but never pumps
    // libusb's event loop itself (USBWrapper::handleEvents() exists but has
    // no caller anywhere in this tree) -- without this, submitted transfers
    // never get their completion callbacks run, so nothing sent by the phone
    // (including the very first version response) is ever delivered.
    std::atomic<bool> stopUsbEvents{false};
    std::thread usbEventThread;

    ~NativeContext() {
        stopUsbEvents = true;
        if (usbEventThread.joinable()) { usbEventThread.join(); }
        session.reset();
        usbWrapper.reset();
        if (pendingWindow) { ANativeWindow_release(pendingWindow); }
        if (usbCtx) { libusb_exit(usbCtx); }
    }
};

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeCreate(JNIEnv*, jobject) {
    auto* ctx = new NativeContext();
    if (libusb_init(&ctx->usbCtx) != LIBUSB_SUCCESS) {
        LOGE("libusb_init failed");
        delete ctx;
        return 0;
    }
    ctx->usbWrapper = std::make_unique<usb::USBWrapper>(ctx->usbCtx);

    // Short timeout so the loop notices stopUsbEvents promptly on teardown;
    // otherwise this blocks in libusb waiting for the next USB event.
    ctx->usbEventThread = std::thread([ctx]() {
        struct timeval tv { 0, 50 * 1000 }; // 50ms
        while (!ctx->stopUsbEvents.load(std::memory_order_relaxed)) {
            libusb_handle_events_timeout(ctx->usbCtx, &tv);
        }
    });

    // Fire-and-forget: absorb the RPi4 V4L2 decoder's one-time cold-start
    // glitch now, before a phone is attached, so the real session's first
    // decoder instance isn't the driver's first one. Detached -- nothing
    // needs to join it, it just needs to run once.
    std::thread(&AndroidVideoOutput::warmup).detach();

    LOGI("nativeCreate OK");
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT void JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeDestroy(JNIEnv*, jobject, jlong handle) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    delete ctx; // destructor cleans up everything
    LOGI("nativeDestroy OK");
}

// Called from Java when the phone is confirmed in AOAP accessory mode and
// UsbDeviceConnection is open.
// fd: result of UsbDeviceConnection.getFileDescriptor()
JNIEXPORT void JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeOnAccessoryAttached(
        JNIEnv* env, jobject serviceObj, jlong handle, jint fd) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) { LOGE("null ctx"); return; }

    std::lock_guard<std::mutex> lk(ctx->mu);
    if (ctx->session) { LOGE("session already running"); return; }

    ctx->session = createAndroidAutoSession(
        env, serviceObj, ctx->usbCtx, *ctx->usbWrapper, static_cast<int>(fd));

    if (!ctx->session) { LOGE("session create failed"); return; }

    // Apply the latest known-good surface (may predate this session, or be
    // left over from a still-visible screen whose session just got
    // replaced) -- kept cached rather than consumed, see nativeSetSurface().
    if (ctx->pendingWindow) {
        sessionSetSurface(ctx->session.get(), ctx->pendingWindow);
    }
}

// Called from Java after accepting a TCP connection on the wireless-projection
// listener socket. fd is a connected native socket fd (see
// ParcelFileDescriptor.detachFd() on the Java side). Shares the same
// single-session slot as USB.
//
// A new TCP accept while a session is already running means the previous
// connection is no longer wanted by the phone (it wouldn't be retrying the
// whole BT+WiFi handshake otherwise) -- tear down the stale session instead
// of rejecting the new one. Rejecting used to leak the new fd (nothing ever
// closed it, since ownership was already handed over via detachFd() on the
// Java side) and left the phone's new TCP connection unanswered, which it
// read as a hang and retried forever.
JNIEXPORT void JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeOnTcpAccepted(
        JNIEnv* env, jobject serviceObj, jlong handle, jint fd) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) { LOGE("null ctx"); return; }

    std::lock_guard<std::mutex> lk(ctx->mu);
    if (ctx->session) {
        LOGI("replacing stale session for new TCP connection");
        ctx->session.reset();
    }

    ctx->session = createAndroidAutoSessionTcp(env, serviceObj, static_cast<int>(fd));

    if (!ctx->session) { LOGE("tcp session create failed"); return; }

    // Apply the latest known-good surface -- kept cached rather than
    // consumed. This matters a lot here specifically: a phone-initiated
    // reconnect (new BT+WiFi handshake -> new TCP accept) can replace the
    // session while AaSdkScreenActivity is still resumed with its Surface
    // untouched, so Kotlin never re-fires surfaceChanged/nativeSetSurface
    // for the new session -- without this cache, the new session's
    // videoOut would never get a window at all (session decrypts/decodes
    // fine, screen just stays black forever).
    if (ctx->pendingWindow) {
        sessionSetSurface(ctx->session.get(), ctx->pendingWindow);
    }
}

// Called from Java when the accessory disconnects (USB_DEVICE_DETACHED), so a
// subsequent nativeOnAccessoryAttached on re-plug doesn't hit the "session
// already running" guard against a session whose USB fd is already dead.
JNIEXPORT void JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeOnAccessoryDetached(
        JNIEnv*, jobject, jlong handle) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;

    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->session.reset();
    LOGI("session torn down on accessory detach");
}

// Called from Java when AaSdkScreenActivity is re-entered after having been
// backgrounded (see surfaceChanged in AaSdkScreenActivity.kt). Wireless (TCP)
// sessions have been observed going silent in the background -- the socket
// stays ESTABLISHED but the phone stops answering anything, including
// pings -- with no local signal that anything is wrong. Force-closing our
// end here (destroying the session tears down the transport, which closes
// the underlying TCP socket) makes the phone notice the disconnect and
// reconnect on its own; nativeOnTcpAccepted already replaces a stale session
// cleanly when that new connection comes in. Only called for wireless
// sessions -- see AaSdkUsbService.resetWirelessSessionForReentry().
JNIEXPORT void JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeResetSession(
        JNIEnv*, jobject, jlong handle) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;

    std::lock_guard<std::mutex> lk(ctx->mu);
    if (ctx->session) {
        ctx->session.reset();
        LOGI("session force-reset on screen re-entry");
    }
}

JNIEXPORT void JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeSetSurface(
        JNIEnv* env, jobject, jlong handle, jobject surface) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;

    std::lock_guard<std::mutex> lk(ctx->mu);

    ANativeWindow* newWin = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;

    // Always cache the latest known-good window, not just when no session
    // exists yet. A session can be replaced later (TCP reconnect, USB
    // re-attach) while this exact Surface is still alive and unchanged --
    // Kotlin only re-fires surfaceChanged/nativeSetSurface when the Surface
    // itself is torn down and recreated, not when the native session
    // underneath it gets swapped out, so the next session needs this cache
    // to find out about a surface it was never directly told about.
    if (ctx->pendingWindow) ANativeWindow_release(ctx->pendingWindow);
    ctx->pendingWindow = newWin;

    if (ctx->session) {
        sessionSetSurface(ctx->session.get(), newWin);
    }
}

// Polled periodically from Java (see AaSdkUsbService's fatal-error check
// loop) instead of pushed via a native->Java callback -- see
// sessionCheckAndConsumeFatalError's own comment for why. Returns true at
// most once per fatal event; safe to call with no session (returns false).
JNIEXPORT jboolean JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeCheckFatalError(
        JNIEnv*, jobject, jlong handle) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return JNI_FALSE;
    std::lock_guard<std::mutex> lk(ctx->mu);
    return sessionCheckAndConsumeFatalError(ctx->session.get()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeSendTouchEvent(
        JNIEnv*, jobject, jlong handle, jint action, jfloat x, jfloat y) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    // No lock needed: sessionSendTouchEvent dispatches on a strand
    if (ctx->session) {
        LOGI("nativeSendTouchEvent action=%d x=%.1f y=%.1f", (int)action, (float)x, (float)y);
        sessionSendTouchEvent(ctx->session.get(), static_cast<int>(action),
                              static_cast<float>(x), static_cast<float>(y));
    } else {
        LOGE("nativeSendTouchEvent dropped: no session");
    }
}

} // extern "C"
