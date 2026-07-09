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

// Same session (SSL/messenger/services/entity), built over an already-connected
// TCP socket fd instead of a USB accessory -- e.g. a socket accepted from a
// wireless-projection TCP listener.
// fd: a connected, native socket fd (e.g. from ParcelFileDescriptor.detachFd()
//     on the Java side after accepting a connection).
AaSdkUsbSessionPtr createAndroidAutoSessionTcp(
    JNIEnv* env,
    jobject serviceObj,
    int fd);

void sessionSetSurface(AaSdkUsbSession* session, ANativeWindow* window);
void sessionSendTouchEvent(AaSdkUsbSession* session, int action, float x, float y);

// Polled from Java (see AaSdkUsbService's fatal-error check loop) rather than
// pushed via a native->Java callback: AndroidAutoEntity's watchdog fires on
// one of this session's own io_service worker threads, and safely calling
// back into Java from there would need a JavaVM attach/detach dance this
// tree doesn't otherwise use anywhere. A plain atomic flag, read-and-cleared
// from Java's own thread, avoids that entirely. Returns true at most once
// per fatal event (exchange-and-clear).
bool sessionCheckAndConsumeFatalError(AaSdkUsbSession* session);

} // namespace aasdk_android
