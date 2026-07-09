#include <AndroidAutoEntity.hpp>
#include <f1x/aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <AuthCompleteIndicationMessage.pb.h>
#include <ServiceDiscoveryResponseMessage.pb.h>
#include <AudioFocusResponseMessage.pb.h>
#include <NavigationFocusResponseMessage.pb.h>
#include <ShutdownResponseMessage.pb.h>
#include <android/log.h>
#include <functional>
#include <string>

#define LOG_TAG "AaSdk_Entity"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

AndroidAutoEntity::AndroidAutoEntity(
        boost::asio::io_service& ios,
        messenger::ICryptor::Pointer cryptor,
        transport::ITransport::Pointer transport,
        messenger::IMessenger::Pointer messenger,
        VideoService::Pointer videoSvc,
        AudioService::Pointer mediaSvc,
        AudioService::Pointer speechSvc,
        SensorService::Pointer sensorSvc,
        InputService::Pointer inputSvc,
        BluetoothService::Pointer btSvc,
        AVInputService::Pointer avInputSvc)
    : strand_(ios)
    , cryptor_(std::move(cryptor))
    , transport_(std::move(transport))
    , messenger_(std::move(messenger))
    , controlChannel_(std::make_shared<channel::control::ControlServiceChannel>(strand_, messenger_))
    , pinger_(std::make_shared<Pinger>(ios, controlChannel_))
    , watchdog_(ios)
    , videoSvc_(std::move(videoSvc))
    , mediaSvc_(std::move(mediaSvc))
    , speechSvc_(std::move(speechSvc))
    , sensorSvc_(std::move(sensorSvc))
    , inputSvc_(std::move(inputSvc))
    , btSvc_(std::move(btSvc))
    , avInputSvc_(std::move(avInputSvc)) {}

void AndroidAutoEntity::start() {
    strand_.dispatch([this, self = shared_from_this()]() {
        LOGI("start — sending version request");
        videoSvc_->start();
        mediaSvc_->start();
        speechSvc_->start();
        sensorSvc_->start();
        inputSvc_->start();
        btSvc_->start();
        avInputSvc_->start();

        auto promise = channel::SendPromise::defer(strand_);
        promise->then([]() {},
                      std::bind(&AndroidAutoEntity::onChannelError, shared_from_this(),
                                std::placeholders::_1));
        controlChannel_->sendVersionRequest(std::move(promise));
        controlChannel_->receive(shared_from_this());
    });
}

void AndroidAutoEntity::setFatalErrorCallback(std::function<void()> cb) {
    fatalErrorCallback_ = std::move(cb);
}

void AndroidAutoEntity::stop() {
    // Idempotent: the watchdog below and a genuine ShutdownRequest/Response
    // (or a version mismatch) can now both call stop() for the same
    // session; without this guard the second call would double-cancel/
    // double-deinit things that aren't necessarily safe to touch twice.
    if (stopped_.exchange(true)) return;
    strand_.dispatch([this, self = shared_from_this()]() {
        LOGI("stop");
        boost::system::error_code ec;
        watchdog_.cancel(ec);
        pinger_->cancel();
        videoSvc_->stop();
        mediaSvc_->stop();
        speechSvc_->stop();
        sensorSvc_->stop();
        inputSvc_->stop();
        btSvc_->stop();
        avInputSvc_->stop();
        messenger_->stop();
        transport_->stop();
        cryptor_->deinit();
    });
}

void AndroidAutoEntity::onVersionResponse(uint16_t major, uint16_t minor,
        proto::enums::VersionResponseStatus::Enum status) {
    LOGI("version %u.%u status=%d", major, minor, (int)status);
    if (status == proto::enums::VersionResponseStatus::MISMATCH) {
        LOGE("version mismatch"); stop(); return;
    }
    try {
        cryptor_->doHandshake();
        auto promise = channel::SendPromise::defer(strand_);
        promise->then([]() {},
                      std::bind(&AndroidAutoEntity::onChannelError, shared_from_this(),
                                std::placeholders::_1));
        controlChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(promise));
        controlChannel_->receive(shared_from_this());
    } catch (const error::Error& e) { onChannelError(e); }
}

void AndroidAutoEntity::onHandshake(const common::DataConstBuffer& payload) {
    LOGI("handshake payload size=%zu", payload.size);
    try {
        cryptor_->writeHandshakeBuffer(payload);
        if (!cryptor_->doHandshake()) {
            auto promise = channel::SendPromise::defer(strand_);
            promise->then([]() {},
                          std::bind(&AndroidAutoEntity::onChannelError, shared_from_this(),
                                    std::placeholders::_1));
            controlChannel_->sendHandshake(cryptor_->readHandshakeBuffer(), std::move(promise));
        } else {
            LOGI("SSL auth complete");
            proto::messages::AuthCompleteIndication authOk;
            authOk.set_status(proto::enums::Status::OK);
            auto promise = channel::SendPromise::defer(strand_);
            promise->then([]() {},
                          std::bind(&AndroidAutoEntity::onChannelError, shared_from_this(),
                                    std::placeholders::_1));
            controlChannel_->sendAuthComplete(authOk, std::move(promise));
            pinger_->start();
            lastPongTime_ = std::chrono::steady_clock::now();
            scheduleWatchdog();
        }
        controlChannel_->receive(shared_from_this());
    } catch (const error::Error& e) { onChannelError(e); }
}

void AndroidAutoEntity::onServiceDiscoveryRequest(
        const proto::messages::ServiceDiscoveryRequest& req) {
    LOGI("service discovery from '%s'", req.device_name().c_str());

    proto::messages::ServiceDiscoveryResponse resp;
    resp.mutable_channels()->Reserve(32);
    // Flat fields are deprecated-but-still-read by the modern AA app; kept
    // for backward compat. headunit_info (field 17) is what current phones
    // actually consult.
    resp.set_head_unit_name("AAOS-Pi4");
    resp.set_car_model("Raspberry Pi 4");
    resp.set_car_year("2024");
    resp.set_car_serial("rpi4-aaos");
    resp.set_driver_position(0); // modern DriverPosition enum: 0 = LEFT
    resp.set_headunit_manufacturer("Raspberry");
    resp.set_headunit_model("Pi 4");
    resp.set_sw_build("1");
    resp.set_sw_version("1.0");
    resp.set_can_play_native_media_during_vr(false);
    resp.set_display_name("AAOS-Pi4");
    resp.set_probe_for_support(false);

    auto* info = resp.mutable_headunit_info();
    info->set_make("Raspberry");
    info->set_model("Pi 4");
    info->set_vehicle_id("rpi4-aaos");
    info->set_head_unit_make("Raspberry");
    info->set_head_unit_model("Pi 4");
    info->set_head_unit_software_build("1");
    info->set_head_unit_software_version("1.0");

    videoSvc_->fillFeatures(resp);
    mediaSvc_->fillFeatures(resp);
    speechSvc_->fillFeatures(resp);
    sensorSvc_->fillFeatures(resp);
    inputSvc_->fillFeatures(resp);
    avInputSvc_->fillFeatures(resp);
    // Bluetooth deliberately not advertised: the modern BluetoothService
    // schema requires a non-empty car_address, and this head unit has no
    // real BT stack to back one -- omitting the channel entirely (rather
    // than sending a fake/empty one) is what a real BT-less head unit does.

    LOGI("sending service discovery response, %d channels, serialized size=%zu",
         resp.channels_size(), resp.ByteSizeLong());
    {
        const std::string serialized = resp.SerializeAsString();
        std::string hex;
        hex.reserve(serialized.size() * 2);
        static const char* digits = "0123456789abcdef";
        for (unsigned char c : serialized) {
            hex.push_back(digits[c >> 4]);
            hex.push_back(digits[c & 0xF]);
        }
        LOGI("SD response bytes (%zu): %s", serialized.size(), hex.c_str());
    }

    auto promise = channel::SendPromise::defer(strand_);
    promise->then([this, self = shared_from_this()]() { LOGI("service discovery response sent OK"); },
                  std::bind(&AndroidAutoEntity::onChannelError, shared_from_this(),
                            std::placeholders::_1));
    controlChannel_->sendServiceDiscoveryResponse(resp, std::move(promise));
    controlChannel_->receive(shared_from_this());
}

void AndroidAutoEntity::onAudioFocusRequest(const proto::messages::AudioFocusRequest& req) {
    LOGI("audio focus request type=%d", (int)req.audio_focus_type());
    auto state = (req.audio_focus_type() == proto::enums::AudioFocusType::RELEASE)
                 ? proto::enums::AudioFocusState::LOSS
                 : proto::enums::AudioFocusState::GAIN;
    proto::messages::AudioFocusResponse resp;
    resp.set_audio_focus_state(state);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {},
                  std::bind(&AndroidAutoEntity::onChannelError, shared_from_this(),
                            std::placeholders::_1));
    controlChannel_->sendAudioFocusResponse(resp, std::move(promise));
    controlChannel_->receive(shared_from_this());
}

void AndroidAutoEntity::onShutdownRequest(const proto::messages::ShutdownRequest& req) {
    LOGI("shutdown request");
    proto::messages::ShutdownResponse resp;
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([this, self = shared_from_this()]() { stop(); },
                  [this, self = shared_from_this()](const error::Error&) { stop(); });
    controlChannel_->sendShutdownResponse(resp, std::move(promise));
}

void AndroidAutoEntity::onShutdownResponse(const proto::messages::ShutdownResponse&) {
    stop();
}

void AndroidAutoEntity::onNavigationFocusRequest(
        const proto::messages::NavigationFocusRequest& req) {
    LOGI("navigation focus request type=%d", req.type());
    proto::messages::NavigationFocusResponse resp;
    resp.set_type(2);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {},
                  std::bind(&AndroidAutoEntity::onChannelError, shared_from_this(),
                            std::placeholders::_1));
    controlChannel_->sendNavigationFocusResponse(resp, std::move(promise));
    controlChannel_->receive(shared_from_this());
}

void AndroidAutoEntity::onPingRequest(const proto::messages::PingRequest& req) {
    // Wireless AA pings in both directions (unlike wired, where only the head
    // unit pings the phone via Pinger) -- confirmed live: this request was
    // previously unhandled ("message not handled: 11" in ControlServiceChannel),
    // and the phone tore down the whole TCP session ~2.7s after sending it and
    // getting no reply, right after service discovery/audio focus succeeded.
    LOGI("ping request timestamp=%lld", (long long)req.timestamp());
    proto::messages::PingResponse resp;
    resp.set_timestamp(req.timestamp());
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() { LOGI("ping response sent OK"); },
                  std::bind(&AndroidAutoEntity::onChannelError, shared_from_this(),
                            std::placeholders::_1));
    controlChannel_->sendPingResponse(resp, std::move(promise));
    controlChannel_->receive(shared_from_this());
}

void AndroidAutoEntity::onPingResponse(const proto::messages::PingResponse&) {
    LOGI("ping response");
    lastPongTime_ = std::chrono::steady_clock::now();
    controlChannel_->receive(shared_from_this());
}

void AndroidAutoEntity::scheduleWatchdog() {
    watchdog_.expires_after(std::chrono::milliseconds(kWatchdogIntervalMs));
    watchdog_.async_wait(strand_.wrap([this, self = shared_from_this()]
                                      (const boost::system::error_code& ec) {
        if (ec || stopped_.load()) return; // cancelled (e.g. by stop()) or already stopped
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastPongTime_).count();
        if (elapsedMs > kFatalTimeoutMs) {
            LOGE("no ping response for %lldms -- transport considered fatally dead, stopping",
                 (long long)elapsedMs);
            auto cb = fatalErrorCallback_;
            stop();
            if (cb) cb();
            return;
        }
        scheduleWatchdog();
    }));
}

void AndroidAutoEntity::onChannelError(const error::Error& e) {
    // Live-confirmed 2026-07-08: this used to just log and stop -- the
    // control channel's receive loop only re-arms itself from each message
    // handler (see onPingRequest/onServiceDiscoveryRequest/etc. above, all
    // ending in controlChannel_->receive(...)), so a single transient error
    // (e.g. MESSENGER_INTERTWINED_CHANNELS) permanently silenced this
    // channel: outbound sends (Pinger, touch) kept "succeeding" locally
    // forever after, while nothing arriving from the phone (ping responses,
    // new requests) was ever processed again -- a session that looks alive
    // but is actually deaf. Re-arm so a transient error can recover instead
    // of silently killing the control channel for the rest of the session.
    LOGE("control channel error: %s", e.what());
    controlChannel_->receive(shared_from_this());
}

} // namespace aasdk_android
