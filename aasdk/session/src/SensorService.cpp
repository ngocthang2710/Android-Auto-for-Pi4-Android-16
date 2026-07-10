#include <SensorService.hpp>
#include <DrivingStatusEnum.pb.h>
#include <android/log.h>

#define LOG_TAG "AaSdk_SensorSvc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

SensorService::SensorService(boost::asio::io_service& ios,
                             messenger::IMessenger::Pointer messenger)
    : strand_(ios)
    , channel_(std::make_shared<channel::sensor::SensorServiceChannel>(strand_, std::move(messenger))) {}

void SensorService::start() {
    strand_.dispatch([this, self = shared_from_this()]() {
        channel_->receive(shared_from_this());
    });
}

void SensorService::stop() {}

void SensorService::fillFeatures(proto::messages::ServiceDiscoveryResponse& resp) {
    auto* ch = resp.add_channels();
    ch->set_channel_id(static_cast<uint32_t>(channel_->getId()));
    auto* sc = ch->mutable_sensor_channel();
    sc->add_sensors()->set_type(proto::enums::SensorType::DRIVING_STATUS);
    sc->add_sensors()->set_type(proto::enums::SensorType::NIGHT_DATA);
}

void SensorService::onChannelOpenRequest(const proto::messages::ChannelOpenRequest&) {
    LOGI("open request");
    proto::messages::ChannelOpenResponse resp;
    resp.set_status(proto::enums::Status::OK);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("openResp: %s", e.what()); });
    channel_->sendChannelOpenResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void SensorService::onSensorStartRequest(const proto::messages::SensorStartRequestMessage& req) {
    LOGI("sensor start request type=%d", (int)req.sensor_type());
    proto::messages::SensorStartResponseMessage resp;
    resp.set_status(proto::enums::Status::OK);

    auto promise = channel::SendPromise::defer(strand_);
    if (req.sensor_type() == proto::enums::SensorType::DRIVING_STATUS) {
        promise->then([this, self = shared_from_this()]() {
            proto::messages::SensorEventIndication ind;
            ind.add_driving_status()->set_status(proto::enums::DrivingStatus::UNRESTRICTED);
            auto p = channel::SendPromise::defer(strand_);
            p->then([]() { LOGI("drivingStatus sent OK (UNRESTRICTED)"); },
                    [](const error::Error& e) { LOGE("drivingStatus: %s", e.what()); });
            channel_->sendSensorEventIndication(ind, std::move(p));
        }, [](const error::Error& e) { LOGE("sensorStartResp: %s", e.what()); });
    } else if (req.sensor_type() == proto::enums::SensorType::NIGHT_DATA) {
        promise->then([this, self = shared_from_this()]() {
            proto::messages::SensorEventIndication ind;
            ind.add_night_mode()->set_is_night(false);
            auto p = channel::SendPromise::defer(strand_);
            p->then([]() {}, [](const error::Error& e) { LOGE("nightData: %s", e.what()); });
            channel_->sendSensorEventIndication(ind, std::move(p));
        }, [](const error::Error& e) { LOGE("sensorStartResp: %s", e.what()); });
    } else {
        promise->then([]() {}, [](const error::Error& e) { LOGE("sensorStart: %s", e.what()); });
    }

    channel_->sendSensorStartResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void SensorService::onChannelError(const error::Error& e) {
    // See AndroidAutoEntity::onChannelError for why this re-arm is required.
    LOGE("channel error: %s", e.what());
    channel_->receive(shared_from_this());
}

} // namespace aasdk_android
