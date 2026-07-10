#include <AudioService.hpp>
#include <f1x/aasdk/Channel/AV/MediaAudioServiceChannel.hpp>
#include <f1x/aasdk/Channel/AV/SpeechAudioServiceChannel.hpp>
#include <f1x/aasdk/Channel/AV/SystemAudioServiceChannel.hpp>
#include <android/log.h>
#include <cstring>

#define LOG_TAG "AaSdk_AudioSvc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

static channel::av::IAudioServiceChannel::Pointer makeChannel(
        AudioChannelType type,
        boost::asio::io_service::strand& strand,
        messenger::IMessenger::Pointer messenger) {
    switch (type) {
    case AudioChannelType::MEDIA:
        return std::make_shared<channel::av::MediaAudioServiceChannel>(strand, std::move(messenger));
    case AudioChannelType::SPEECH:
        return std::make_shared<channel::av::SpeechAudioServiceChannel>(strand, std::move(messenger));
    case AudioChannelType::SYSTEM:
        return std::make_shared<channel::av::SystemAudioServiceChannel>(strand, std::move(messenger));
    }
    return nullptr; // unreachable
}

AudioService::AudioService(boost::asio::io_service& ios,
                           messenger::IMessenger::Pointer messenger,
                           AudioChannelType type,
                           std::shared_ptr<AndroidAudioOutput> output)
    : type_(type)
    , strand_(ios)
    , channel_(makeChannel(type, strand_, std::move(messenger)))
    , output_(std::move(output)) {}

void AudioService::start() {
    strand_.dispatch([this, self = shared_from_this()]() {
        output_->init();
        channel_->receive(shared_from_this());
    });
}

void AudioService::stop() {
    strand_.dispatch([this, self = shared_from_this()]() {
        output_->stop();
    });
}

void AudioService::fillFeatures(proto::messages::ServiceDiscoveryResponse& resp) {
    auto* ch = resp.add_channels();
    ch->set_channel_id(static_cast<uint32_t>(channel_->getId()));
    auto* av = ch->mutable_av_channel();
    av->set_codec_type(proto::enums::MediaCodecType::AUDIO_PCM);
    av->set_audio_type(type_ == AudioChannelType::MEDIA ? proto::enums::AudioStreamType::MEDIA
                        : type_ == AudioChannelType::SPEECH ? proto::enums::AudioStreamType::GUIDANCE
                        : proto::enums::AudioStreamType::SYSTEM_AUDIO);
    auto* cfg = av->add_audio_configs();
    if (type_ == AudioChannelType::MEDIA) {
        cfg->set_sample_rate(48000);
        cfg->set_bit_depth(16);
        cfg->set_channel_count(2);
    } else {
        cfg->set_sample_rate(16000);
        cfg->set_bit_depth(16);
        cfg->set_channel_count(1);
    }
}

void AudioService::onChannelOpenRequest(const proto::messages::ChannelOpenRequest&) {
    LOGI("open request (type=%d)", (int)type_);
    proto::messages::ChannelOpenResponse resp;
    resp.set_status(proto::enums::Status::OK);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("openResp: %s", e.what()); });
    channel_->sendChannelOpenResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void AudioService::onAVChannelSetupRequest(const proto::messages::AVChannelSetupRequest&) {
    proto::messages::AVChannelSetupResponse resp;
    resp.set_media_status(proto::enums::AVChannelSetupStatus::OK);
    resp.set_max_unacked(1);
    resp.add_configs(0);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("setupResp: %s", e.what()); });
    channel_->sendAVChannelSetupResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void AudioService::onAVChannelStartIndication(const proto::messages::AVChannelStartIndication& ind) {
    session_ = ind.session();
    channel_->receive(shared_from_this());
}

void AudioService::onAVChannelStopIndication(const proto::messages::AVChannelStopIndication&) {
    channel_->receive(shared_from_this());
}

void AudioService::onAVMediaWithTimestampIndication(
        messenger::Timestamp::ValueType, const common::DataConstBuffer& buf) {
    output_->write(reinterpret_cast<const int16_t*>(buf.cdata), buf.size / sizeof(int16_t));

    proto::messages::AVMediaAckIndication ack;
    ack.set_session(session_);
    ack.set_value(1);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("ack: %s", e.what()); });
    channel_->sendAVMediaAckIndication(ack, std::move(promise));
    channel_->receive(shared_from_this());
}

void AudioService::onAVMediaIndication(const common::DataConstBuffer& buf) {
    output_->write(reinterpret_cast<const int16_t*>(buf.cdata), buf.size / sizeof(int16_t));
    channel_->receive(shared_from_this());
}

void AudioService::onChannelError(const error::Error& e) {
    // See AndroidAutoEntity::onChannelError for why this re-arm is required.
    LOGE("channel error: %s", e.what());
    channel_->receive(shared_from_this());
}

} // namespace aasdk_android
