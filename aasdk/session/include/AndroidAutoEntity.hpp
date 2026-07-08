#pragma once
#include <f1x/aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <f1x/aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Messenger/ICryptor.hpp>
#include <f1x/aasdk/Transport/ITransport.hpp>
#include <f1x/aasdk/Messenger/IMessenger.hpp>
#include <VideoService.hpp>
#include <AudioService.hpp>
#include <SensorService.hpp>
#include <InputService.hpp>
#include <BluetoothService.hpp>
#include <AVInputService.hpp>
#include <Pinger.hpp>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>
#include <boost/asio.hpp>

namespace aasdk_android {

class AndroidAutoEntity
    : public f1x::aasdk::channel::control::IControlServiceChannelEventHandler,
      public std::enable_shared_from_this<AndroidAutoEntity> {
public:
    using Pointer = std::shared_ptr<AndroidAutoEntity>;

    AndroidAutoEntity(
        boost::asio::io_service& ioService,
        f1x::aasdk::messenger::ICryptor::Pointer cryptor,
        f1x::aasdk::transport::ITransport::Pointer transport,
        f1x::aasdk::messenger::IMessenger::Pointer messenger,
        VideoService::Pointer videoSvc,
        AudioService::Pointer mediaSvc,
        AudioService::Pointer speechSvc,
        SensorService::Pointer sensorSvc,
        InputService::Pointer inputSvc,
        BluetoothService::Pointer btSvc,
        AVInputService::Pointer avInputSvc);

    void start();
    void stop();

    // Invoked (from this entity's strand) when the watchdog below decides
    // the transport is fatally, not just transiently, broken. Caller uses
    // this to tear down the outer Java-side session (mirrors what happens
    // on a real USB accessory detach) -- stop() alone only quiesces this
    // C++ object, it can't reach the Activity/UI on its own.
    void setFatalErrorCallback(std::function<void()> cb);

    // IControlServiceChannelEventHandler
    void onVersionResponse(uint16_t major, uint16_t minor,
                           f1x::aasdk::proto::enums::VersionResponseStatus::Enum status) override;
    void onHandshake(const f1x::aasdk::common::DataConstBuffer& payload) override;
    void onServiceDiscoveryRequest(const f1x::aasdk::proto::messages::ServiceDiscoveryRequest& req) override;
    void onAudioFocusRequest(const f1x::aasdk::proto::messages::AudioFocusRequest& req) override;
    void onShutdownRequest(const f1x::aasdk::proto::messages::ShutdownRequest& req) override;
    void onShutdownResponse(const f1x::aasdk::proto::messages::ShutdownResponse& resp) override;
    void onNavigationFocusRequest(const f1x::aasdk::proto::messages::NavigationFocusRequest& req) override;
    void onPingRequest(const f1x::aasdk::proto::messages::PingRequest& req) override;
    void onPingResponse(const f1x::aasdk::proto::messages::PingResponse& resp) override;
    void onChannelError(const f1x::aasdk::error::Error& e) override;

private:
    void scheduleWatchdog();

    boost::asio::io_service::strand strand_;
    f1x::aasdk::messenger::ICryptor::Pointer cryptor_;
    f1x::aasdk::transport::ITransport::Pointer transport_;
    f1x::aasdk::messenger::IMessenger::Pointer messenger_;
    f1x::aasdk::channel::control::ControlServiceChannel::Pointer controlChannel_;
    Pinger::Pointer pinger_;

    // Detects a transport that's fatally dead rather than transiently
    // erroring: Pinger keeps sending every 2s regardless of channel errors
    // (by design, see Pinger.cpp/onChannelError's re-arm-on-error fixes),
    // so a session that never gets a pong back just spins those errors
    // forever with no self-recovery. Confirmed live 2026-07-08: 58+s of
    // "ping sent" with zero "ping response", hundreds of repeating
    // SSL_READ/SSL_WRITE channel errors across all 7 services, no USB
    // detach broadcast (so nothing else notices) -- the whole HU sat
    // frozen until the user manually unplugged/replugged.
    boost::asio::steady_timer watchdog_;
    std::chrono::steady_clock::time_point lastPongTime_;
    std::function<void()> fatalErrorCallback_;
    std::atomic<bool> stopped_{false};
    static constexpr int kWatchdogIntervalMs = 2000;
    // ~4 missed 2s ping intervals -- generous margin over a single
    // transient hiccup, short enough the user isn't stuck for long.
    static constexpr int kFatalTimeoutMs = 8000;

    VideoService::Pointer videoSvc_;
    AudioService::Pointer mediaSvc_;
    AudioService::Pointer speechSvc_;
    SensorService::Pointer sensorSvc_;
    InputService::Pointer inputSvc_;
    BluetoothService::Pointer btSvc_;
    AVInputService::Pointer avInputSvc_;
};

} // namespace aasdk_android
