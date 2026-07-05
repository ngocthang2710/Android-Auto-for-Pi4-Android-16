#include <AndroidAutoSession.hpp>
#include <AndroidAutoEntity.hpp>
#include <VideoService.hpp>
#include <AudioService.hpp>
#include <SensorService.hpp>
#include <InputService.hpp>
#include <BluetoothService.hpp>
#include <AVInputService.hpp>
#include <AndroidVideoOutput.hpp>
#include <AndroidAudioOutput.hpp>
#include <f1x/aasdk/USB/AOAPDevice.hpp>
#include <f1x/aasdk/USB/USBWrapper.hpp>
#include <f1x/aasdk/Transport/SSLWrapper.hpp>
#include <f1x/aasdk/Transport/USBTransport.hpp>
#include <f1x/aasdk/Messenger/Cryptor.hpp>
#include <f1x/aasdk/Messenger/MessageInStream.hpp>
#include <f1x/aasdk/Messenger/MessageOutStream.hpp>
#include <f1x/aasdk/Messenger/Messenger.hpp>
#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <android/log.h>

#define LOG_TAG "AaSdk_Session"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

struct AaSdkUsbSession {
    boost::asio::io_service ioService;
    boost::asio::io_service::work work{ioService};
    std::vector<std::thread> workers;
    AndroidAutoEntity::Pointer entity;
    std::shared_ptr<AndroidVideoOutput> videoOut;
    std::shared_ptr<InputService> inputSvc;
    ANativeWindow* pendingWindow{nullptr};

    ~AaSdkUsbSession() {
        if (entity) { entity->stop(); entity.reset(); }
        ioService.stop();
        for (auto& t : workers) { if (t.joinable()) t.join(); }
        if (pendingWindow) { ANativeWindow_release(pendingWindow); }
    }
};

void destroyAndroidAutoSession(AaSdkUsbSession* session) {
    delete session;
}

AaSdkUsbSessionPtr createAndroidAutoSession(
        JNIEnv* /*env*/,
        jobject /*serviceObj*/,
        libusb_context* usbCtx,
        usb::IUSBWrapper& usbWrapper,
        int fd) {

    // Wrap the Android-opened fd as a libusb handle using the shared context
    libusb_device_handle* rawHandle = nullptr;
    int r = libusb_wrap_sys_device(usbCtx, static_cast<intptr_t>(fd), &rawHandle);
    if (r != LIBUSB_SUCCESS || !rawHandle) {
        LOGE("libusb_wrap_sys_device(%d) failed: %s", fd, libusb_error_name(r));
        return AaSdkUsbSessionPtr(nullptr, destroyAndroidAutoSession);
    }

    // DeviceHandle is shared_ptr<libusb_device_handle>
    usb::DeviceHandle deviceHandle(rawHandle, [](libusb_device_handle* h) {
        libusb_close(h);
    });

    AaSdkUsbSessionPtr session(new AaSdkUsbSession(), destroyAndroidAutoSession);

    usb::IAOAPDevice::Pointer aoapDevice;
    try {
        aoapDevice = usb::AOAPDevice::create(usbWrapper, session->ioService,
                                             std::move(deviceHandle));
    } catch (const std::exception& ex) {
        LOGE("AOAPDevice::create: %s", ex.what());
        return AaSdkUsbSessionPtr(nullptr, destroyAndroidAutoSession);
    }

    // Build the full transport + messenger + crypto stack
    auto transport  = std::make_shared<transport::USBTransport>(session->ioService,
                                                                std::move(aoapDevice));
    auto sslWrapper = std::make_shared<transport::SSLWrapper>();
    auto cryptor    = std::make_shared<messenger::Cryptor>(std::move(sslWrapper));
    cryptor->init();
    auto inStream   = std::make_shared<messenger::MessageInStream>(
        session->ioService, transport, cryptor);
    auto outStream  = std::make_shared<messenger::MessageOutStream>(
        session->ioService, transport, cryptor);
    auto msg = std::make_shared<messenger::Messenger>(
        session->ioService, std::move(inStream), std::move(outStream));

    // Android-native media sinks
    session->videoOut  = std::make_shared<AndroidVideoOutput>();
    auto mediaAudio    = std::make_shared<AndroidAudioOutput>(AudioChannel::MEDIA);
    auto speechAudio   = std::make_shared<AndroidAudioOutput>(AudioChannel::SPEECH);

    // Protocol services
    auto videoSvc   = std::make_shared<VideoService>(session->ioService, msg, session->videoOut);
    auto mediaSvc   = std::make_shared<AudioService>(
        session->ioService, msg, AudioChannelType::MEDIA, mediaAudio);
    auto speechSvc  = std::make_shared<AudioService>(
        session->ioService, msg, AudioChannelType::SPEECH, speechAudio);
    auto sensorSvc  = std::make_shared<SensorService>(session->ioService, msg);
    session->inputSvc = std::make_shared<InputService>(session->ioService, msg);
    auto btSvc      = std::make_shared<BluetoothService>(session->ioService, msg);
    auto avInputSvc = std::make_shared<AVInputService>(session->ioService, msg);

    session->entity = std::make_shared<AndroidAutoEntity>(
        session->ioService,
        std::move(cryptor),
        std::move(transport),
        std::move(msg),
        std::move(videoSvc),
        std::move(mediaSvc),
        std::move(speechSvc),
        std::move(sensorSvc),
        session->inputSvc,
        std::move(btSvc),
        std::move(avInputSvc));

    // 4 IO threads: prevents AAudio blocking in one channel from starving others
    for (int i = 0; i < 4; ++i) {
        session->workers.emplace_back([s = session.get()]() {
            s->ioService.run();
        });
    }

    session->entity->start();
    LOGI("AA session started");
    return session;
}

void sessionSetSurface(AaSdkUsbSession* session, ANativeWindow* window) {
    if (!session) return;
    if (session->pendingWindow) {
        ANativeWindow_release(session->pendingWindow);
        session->pendingWindow = nullptr;
    }
    if (session->videoOut && window) {
        // Must match VideoService's kDefaultConfigIndex resolution and
        // InputService's advertised touchscreen size (1920x1080).
        session->videoOut->init(window, 1920, 1080);
    } else {
        // Store for when videoOut becomes available
        session->pendingWindow = window;
        if (window) ANativeWindow_acquire(window);
    }
}

void sessionSendTouchEvent(AaSdkUsbSession* session, int action, float x, float y) {
    if (session && session->inputSvc) {
        session->inputSvc->injectTouchEvent(action, x, y);
    }
}

} // namespace aasdk_android
