#include <AVInputService.hpp>
#include <f1x/aasdk/Channel/AV/AVInputServiceChannel.hpp>
#include <android/log.h>

#define LOG_TAG "AaSdk_AVInputSvc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

AVInputService::AVInputService(boost::asio::io_service& ios,
                               messenger::IMessenger::Pointer messenger)
    : strand_(ios)
    , channel_(std::make_shared<channel::av::AVInputServiceChannel>(strand_, std::move(messenger))) {}

void AVInputService::start() {
    strand_.dispatch([this, self = shared_from_this()]() {
        channel_->receive(shared_from_this());
    });
}

void AVInputService::stop() {}

void AVInputService::fillFeatures(proto::messages::ServiceDiscoveryResponse& resp) {
    auto* ch = resp.add_channels();
    ch->set_channel_id(static_cast<uint32_t>(channel_->getId()));
    auto* input = ch->mutable_av_input_channel();
    input->set_stream_type(proto::enums::AVStreamType::AUDIO);
    input->set_available_while_in_call(true);
    auto* cfg = input->mutable_audio_config();
    cfg->set_sample_rate(16000);
    cfg->set_bit_depth(16);
    cfg->set_channel_count(1);
}

void AVInputService::onChannelOpenRequest(const proto::messages::ChannelOpenRequest&) {
    LOGI("open request");
    proto::messages::ChannelOpenResponse resp;
    resp.set_status(proto::enums::Status::OK);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("openResp: %s", e.what()); });
    channel_->sendChannelOpenResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void AVInputService::onAVChannelSetupRequest(const proto::messages::AVChannelSetupRequest&) {
    proto::messages::AVChannelSetupResponse resp;
    resp.set_media_status(proto::enums::AVChannelSetupStatus::OK);
    resp.set_max_unacked(1);
    resp.add_configs(0);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("setupResp: %s", e.what()); });
    channel_->sendAVChannelSetupResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void AVInputService::onAVInputOpenRequest(const proto::messages::AVInputOpenRequest& req) {
    LOGI("mic open request: open=%d", (int)req.open());
    proto::messages::AVInputOpenResponse resp;
    resp.set_session(++session_);
    resp.set_value(req.open() ? 1 : 0);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("inputOpenResp: %s", e.what()); });
    channel_->sendAVInputOpenResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void AVInputService::onAVMediaAckIndication(const proto::messages::AVMediaAckIndication&) {
    channel_->receive(shared_from_this());
}

void AVInputService::onChannelError(const error::Error& e) {
    LOGE("channel error: %s", e.what());
}

} // namespace aasdk_android
