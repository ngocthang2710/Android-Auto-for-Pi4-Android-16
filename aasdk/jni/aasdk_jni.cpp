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

    // Apply any surface that arrived before the phone was attached
    if (ctx->pendingWindow) {
        sessionSetSurface(ctx->session.get(), ctx->pendingWindow);
        ANativeWindow_release(ctx->pendingWindow);
        ctx->pendingWindow = nullptr;
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

JNIEXPORT void JNICALL
Java_com_android_car_aasdk_AaSdkUsbService_nativeSetSurface(
        JNIEnv* env, jobject, jlong handle, jobject surface) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;

    std::lock_guard<std::mutex> lk(ctx->mu);

    ANativeWindow* newWin = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;

    if (ctx->session) {
        sessionSetSurface(ctx->session.get(), newWin);
    } else {
        // Cache for when session starts
        if (ctx->pendingWindow) ANativeWindow_release(ctx->pendingWindow);
        ctx->pendingWindow = newWin;
    }
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
