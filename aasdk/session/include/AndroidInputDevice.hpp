#pragma once
#include <jni.h>

namespace aasdk_android {

// Forwards touch events back to the AA phone by calling
// AaSdkUsbService.onTouchEvent(x, y, action) via JNI.
class AndroidInputDevice {
public:
    AndroidInputDevice(JNIEnv* env, jobject serviceObj);

    void sendTouchEvent(int action, float x, float y);

private:
    JNIEnv* env_;
    jobject serviceRef_;
    jmethodID onTouchMethod_{nullptr};
};

} // namespace aasdk_android
