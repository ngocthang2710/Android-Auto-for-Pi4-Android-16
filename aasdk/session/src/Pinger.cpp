#include <Pinger.hpp>
#include <PingRequestMessage.pb.h>
#include <android/log.h>
#include <chrono>

#define LOG_TAG "AaSdk_Pinger"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

Pinger::Pinger(boost::asio::io_service& ios,
               channel::control::ControlServiceChannel::Pointer channel)
    : strand_(ios)
    , channel_(std::move(channel))
    , timer_(ios) {}

void Pinger::start() {
    LOGI("started");
    running_ = true;
    schedulePing();
}

void Pinger::cancel() {
    running_ = false;
    strand_.dispatch([this, self = shared_from_this()]() {
        boost::system::error_code ec;
        timer_.cancel(ec);
    });
}

void Pinger::schedulePing() {
    if (!running_) return;
    timer_.expires_after(std::chrono::milliseconds(kIntervalMs));
    timer_.async_wait(strand_.wrap([this, self = shared_from_this()]
                                   (const boost::system::error_code& ec) {
        if (ec || !running_) return;
        proto::messages::PingRequest req;
        // timestamp=0 never gets serialized (proto3 omits default values),
        // so the phone received an empty payload and rejected it every
        // 2s with "missing required fields" -- send a real clock reading.
        req.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        auto promise = channel::SendPromise::defer(strand_);
        promise->then([this, self]() { LOGI("ping sent"); schedulePing(); },
                      [](const error::Error& e) { LOGE("ping error: %s", e.what()); });
        channel_->sendPingRequest(req, std::move(promise));
    }));
}

} // namespace aasdk_android
