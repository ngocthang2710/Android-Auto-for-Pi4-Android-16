#pragma once
#include <jni.h>
#include <f1x/aasdk/USB/IUSBWrapper.hpp>
#include <libusb/libusb.h>
#include <android/native_window.h>
#include <memory>

namespace aasdk_android {

struct AaSdkUsbSession;

// AaSdkUsbSession's definition is only visible in AndroidAutoSession.cpp, so
// callers (e.g. aasdk_jni.cpp) hold it behind a unique_ptr with an explicit
// deleter function -- the default deleter would need the complete type at
// the point of destruction, which callers don't have.
void destroyAndroidAutoSession(AaSdkUsbSession* session);

using AaSdkUsbSessionPtr = std::unique_ptr<AaSdkUsbSession, void(*)(AaSdkUsbSession*)>;

// Creates the full AA session.
//   usbCtx:   the libusb_context that was used to initialise usbWrapper.
//             libusb_wrap_sys_device() will be called with this context so
//             that the resulting handle and the wrapper share the same ctx.
//   usbWrapper: concrete USBWrapper built on usbCtx (caller owns).
//   fd:       UsbDeviceConnection.getFileDescriptor() of the already-opened
//             accessory-mode device.
AaSdkUsbSessionPtr createAndroidAutoSession(
    JNIEnv* env,
    jobject serviceObj,
    libusb_context* usbCtx,
    f1x::aasdk::usb::IUSBWrapper& usbWrapper,
    int fd);

void sessionSetSurface(AaSdkUsbSession* session, ANativeWindow* window);
void sessionSendTouchEvent(AaSdkUsbSession* session, int action, float x, float y);

} // namespace aasdk_android
