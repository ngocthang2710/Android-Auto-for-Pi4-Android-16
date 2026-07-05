#include <VideoService.hpp>
#include <f1x/aasdk/Channel/AV/VideoServiceChannel.hpp>
#include <VideoFocusRequestMessage.pb.h>
#include <VideoFocusIndicationMessage.pb.h>
#include <android/log.h>

#define LOG_TAG "AaSdk_VideoSvc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

VideoService::VideoService(boost::asio::io_service& ios,
                           messenger::IMessenger::Pointer messenger,
                           std::shared_ptr<AndroidVideoOutput> output)
    : strand_(ios)
    , channel_(std::make_shared<channel::av::VideoServiceChannel>(strand_, std::move(messenger)))
    , output_(std::move(output)) {}

void VideoService::start() {
    strand_.dispatch([this, self = shared_from_this()]() {
        channel_->receive(shared_from_this());
    });
}

void VideoService::stop() {
    strand_.dispatch([this, self = shared_from_this()]() {
        output_->stop();
    });
}

namespace {
// Real screen is 1920x1080; a phone connecting to this head unit was
// observed requesting AVChannelSetupRequest.config_index=3 even though we
// used to advertise (and locally decode at) only one 800x480 entry --
// config_index=3 coincides with VideoResolution._1080p's own enum value,
// and the phone appears to encode at whatever resolution it wants
// regardless of what we say we support (MediaCodec/SurfaceFlinger silently
// upscale to fill the Surface, hiding the mismatch visually). Advertise and
// honor all four standard tiers so config_index (however the phone
// interprets it: list position or resolution enum) always resolves to a
// real, matching width/height -- critical for touch coordinates, which
// have no equivalent auto-correction.
struct VideoConfigEntry { int width; int height; int dpi; proto::enums::VideoResolution::Enum resolution; };
// dpi = diagonal_px / 10.1in, matching a common Pi touchscreen size, so
// each entry stays internally consistent (dpi=140 was tuned for the old
// 800x480 entry alone -- reusing it unchanged for 1920x1080 implied an
// unrealistic ~15.7in screen, which can throw off any touch/UI scaling
// GearHead derives from resolution+dpi together).
// margin_height was tried here to reserve space for AAOS's
// BottomCarSystemBar but the phone applied it as a TOP inset instead
// (confirmed visually), so the bottom-bar overlap is now handled purely
// on the HU side instead: AaSdkScreenActivity sizes its SurfaceView to
// avoid the bar directly, no protocol-level margin needed.
constexpr VideoConfigEntry kVideoConfigs[] = {
    {800, 480, 92, proto::enums::VideoResolution::_480p},
    {1280, 720, 145, proto::enums::VideoResolution::_720p},
    {1600, 900, 182, proto::enums::VideoResolution::_1080p},
    {1920, 1080, 218, proto::enums::VideoResolution::_1080p},
};
constexpr size_t kDefaultConfigIndex = 3; // 1920x1080, matches this head unit's real screen
} // namespace

void VideoService::fillFeatures(proto::messages::ServiceDiscoveryResponse& resp) {
    auto* ch = resp.add_channels();
    ch->set_channel_id(static_cast<uint32_t>(channel_->getId()));
    auto* av = ch->mutable_av_channel();
    av->set_codec_type(proto::enums::MediaCodecType::VIDEO_H264_BP);
    av->set_available_while_in_call(true);
    av->set_display_id(0);
    for (const auto& e : kVideoConfigs) {
        auto* cfg = av->add_video_configs();
        cfg->set_video_resolution(e.resolution);
        cfg->set_video_fps(proto::enums::VideoFPS::_60);
        cfg->set_margin_width(0);
        cfg->set_margin_height(0);
        cfg->set_dpi(e.dpi);
        cfg->set_video_codec_type(proto::enums::MediaCodecType::VIDEO_H264_BP);
    }
}

void VideoService::onChannelOpenRequest(const proto::messages::ChannelOpenRequest& req) {
    LOGI("open request priority=%d", req.priority());
    proto::messages::ChannelOpenResponse resp;
    resp.set_status(proto::enums::Status::OK);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) {
        LOGE("sendChannelOpenResponse error: %s", e.what());
    });
    channel_->sendChannelOpenResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void VideoService::onAVChannelSetupRequest(const proto::messages::AVChannelSetupRequest& req) {
    // Always decode/render at the real screen resolution (1920x1080)
    // regardless of which config_index the phone asked for: the phone
    // controls the actual encoded resolution and MediaCodec/SurfaceFlinger
    // silently rescale to fit our Surface either way, so pinning this to
    // one known value keeps it guaranteed consistent with the touchscreen
    // resolution InputService advertises (see InputService::fillFeatures).
    const size_t idx = kDefaultConfigIndex;
    LOGI("setup request config_index=%u, using %dx%d", req.config_index(),
         kVideoConfigs[idx].width, kVideoConfigs[idx].height);
    bool ok = output_->init(nullptr /*window set separately via nativeSetSurface*/,
                             kVideoConfigs[idx].width, kVideoConfigs[idx].height);
    proto::messages::AVChannelSetupResponse resp;
    resp.set_media_status(ok ? proto::enums::AVChannelSetupStatus::OK
                              : proto::enums::AVChannelSetupStatus::FAIL);
    resp.set_max_unacked(1);
    resp.add_configs(req.config_index());
    auto promise = channel::SendPromise::defer(strand_);
    promise->then(std::bind(&VideoService::sendVideoFocusIndication, shared_from_this()),
                  [](const error::Error& e) { LOGE("sendAVChannelSetupResponse error: %s", e.what()); });
    channel_->sendAVChannelSetupResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void VideoService::sendVideoFocusIndication() {
    proto::messages::VideoFocusIndication ind;
    ind.set_focus_mode(proto::enums::VideoFocusMode::FOCUSED);
    ind.set_unrequested(false);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("VideoFocusIndication error: %s", e.what()); });
    channel_->sendVideoFocusIndication(ind, std::move(promise));
}

void VideoService::onAVChannelStartIndication(const proto::messages::AVChannelStartIndication& ind) {
    LOGI("start, session=%d", ind.session());
    session_ = ind.session();
    channel_->receive(shared_from_this());
}

void VideoService::onAVChannelStopIndication(const proto::messages::AVChannelStopIndication&) {
    channel_->receive(shared_from_this());
}

void VideoService::onAVMediaWithTimestampIndication(
        messenger::Timestamp::ValueType, const common::DataConstBuffer& buf) {
    output_->write(buf.cdata, buf.size);
    proto::messages::AVMediaAckIndication ack;
    ack.set_session(session_);
    ack.set_value(1);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("ack error: %s", e.what()); });
    channel_->sendAVMediaAckIndication(ack, std::move(promise));
    channel_->receive(shared_from_this());
}

void VideoService::onAVMediaIndication(const common::DataConstBuffer& buf) {
    output_->write(buf.cdata, buf.size);
    proto::messages::AVMediaAckIndication ack;
    ack.set_session(session_);
    ack.set_value(1);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("ack error: %s", e.what()); });
    channel_->sendAVMediaAckIndication(ack, std::move(promise));
    channel_->receive(shared_from_this());
}

void VideoService::onVideoFocusRequest(const proto::messages::VideoFocusRequest&) {
    sendVideoFocusIndication();
    channel_->receive(shared_from_this());
}

void VideoService::onChannelError(const error::Error& e) {
    LOGE("channel error: %s", e.what());
}

} // namespace aasdk_android
