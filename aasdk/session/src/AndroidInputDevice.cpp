#include <AndroidInputDevice.hpp>
#include <android/log.h>

#define LOG_TAG "AaSdk_Input"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

AndroidInputDevice::AndroidInputDevice(JNIEnv* env, jobject serviceObj)
    : env_(env), serviceRef_(serviceObj) {
    jclass cls = env_->GetObjectClass(serviceRef_);
    onTouchMethod_ = env_->GetMethodID(cls, "onTouchEvent", "(IFF)V");
    if (!onTouchMethod_) {
        LOGE("onTouchEvent method not found on service object");
    }
}

void AndroidInputDevice::sendTouchEvent(int action, float x, float y) {
    if (!onTouchMethod_) return;
    env_->CallVoidMethod(serviceRef_, onTouchMethod_,
                         (jint)action, (jfloat)x, (jfloat)y);
}

} // namespace aasdk_android
