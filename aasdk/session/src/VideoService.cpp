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
// Real screen is 1920x1080. onAVChannelSetupRequest() below never actually
// indexes this array by req.config_index() -- it always renders at
// kDefaultConfigIndex regardless of what the phone asks for -- so the only
// thing that matters about this list is what GearHead itself does with it.
//
// Confirmed live (phone-side logcat via wireless adb debugging, 2026-07-08):
// GearHead's own window manager (CAR.WM) locks its *entire* window/touch
// layout to list index 0 of this array -- "DisplayParams(selectedIndex=0,
// codecWidth=800, codecHeight=480 ... dpi=92)" matched the old {800,480,92}
// first entry exactly, and every Dashboard/GhFacetBar window it created
// stayed confined to that ~800x480 canvas for the entire live session (not
// just the startup loading screen). Meanwhile AVChannelSetupRequest.config_index
// was 3 for the actual video codec negotiation -- a *different* GearHead
// subsystem reading the *same* list with different semantics. With four
// tiers, those two never agreed on a resolution, so every touch coordinate
// (sent in the 1920x1080 space InputService/VideoService/AaSdkScreenActivity
// all use) fell far outside GearHead's own ~800x480 window bounds --
// "UpDown touch event (x,y) does not correspond to a window" for every
// single touch, regardless of screen crashes or channel errors.
//
// Fix: collapse to the one resolution we actually ever render, at index 0,
// so whichever subsystem reads "index 0" or "the tier we asked for" lands on
// the same 1920x1080 GearHead uses for its own window layout and we use for
// touch. dpi = diagonal_px / 10.1in (matches a common Pi touchscreen size).
struct VideoConfigEntry { int width; int height; int dpi; proto::enums::VideoResolution::Enum resolution; };
// margin_height was tried here to reserve space for AAOS's
// BottomCarSystemBar but the phone applied it as a TOP inset instead
// (confirmed visually), so the bottom-bar overlap is now handled purely
// on the HU side instead: AaSdkScreenActivity sizes its SurfaceView to
// avoid the bar directly, no protocol-level margin needed.
constexpr VideoConfigEntry kVideoConfigs[] = {
    {1920, 1080, 218, proto::enums::VideoResolution::_1080p},
};
constexpr size_t kDefaultConfigIndex = 0; // the only entry: 1920x1080, matches this head unit's real screen
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
    // Do NOT call output_->init(nullptr, ...) here anymore. This used to be
    // harmless under the old design, where the Activity's own Surface always
    // arrived *after* this call (Activity/window creation lag reliably beat
    // the phone's protocol round trip), so init(nullptr,...)'s effect (detach
    // the window) was immediately overwritten moments later by the real
    // nativeSetSurface call. Now the decoder's window is the persistent
    // VideoBlitter input Surface, attached once via nativeSetSurface at
    // Service startup -- long before any session/AVChannelSetupRequest even
    // exists -- so this call would run *after* the real window is already
    // attached and rip it back out, permanently zeroing AndroidVideoOutput's
    // window_ (codec keeps decoding, but every releaseOutputBuffer's render
    // flag is false forever -- confirmed live via write()/drainOutput()
    // counters: outputBufferCount climbing steadily, render=0 on every call).
    // Width/height are already fixed constants (kVideoConfigs) matching the
    // VideoBlitter's persistent SurfaceTexture size, so there's nothing this
    // call needs to (re)configure.
    bool ok = true;
    proto::messages::AVChannelSetupResponse resp;
    resp.set_media_status(ok ? proto::enums::AVChannelSetupStatus::OK
                              : proto::enums::AVChannelSetupStatus::FAIL);
    resp.set_max_unacked(1);
    // openauto (our reference implementation) hardcodes config index 0 here
    // regardless of what the phone requested, rather than echoing
    // req.config_index() back -- aligning with that now since the phone
    // stalling right after this response (never sending AVChannelStartIndication)
    // is the open bug this session is chasing.
    resp.add_configs(0);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then(std::bind(&VideoService::sendVideoFocusIndication, shared_from_this(), true),
                  [](const error::Error& e) { LOGE("sendAVChannelSetupResponse error: %s", e.what()); });
    channel_->sendAVChannelSetupResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void VideoService::sendVideoFocusIndication(bool unrequested) {
    // unrequested=true: this HU is proactively granting focus right after
    // setup, without the phone having asked (VideoFocusRequest) first.
    // unrequested=false: this is a direct reply to the phone's own request
    // (see onVideoFocusRequest). The previous code always sent false, even
    // for the proactive post-setup push -- mislabeling an unsolicited grant
    // as "answering a request that was never made," which the phone may
    // have silently ignored, explaining why it never followed up with
    // AVChannelStartIndication.
    LOGI("sending VideoFocusIndication(FOCUSED, unrequested=%d)", (int)unrequested);
    proto::messages::VideoFocusIndication ind;
    ind.set_focus_mode(proto::enums::VideoFocusMode::FOCUSED);
    ind.set_unrequested(unrequested);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() { LOGI("VideoFocusIndication sent OK"); },
                  [](const error::Error& e) { LOGE("VideoFocusIndication error: %s", e.what()); });
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

void VideoService::onVideoFocusRequest(const proto::messages::VideoFocusRequest& req) {
    LOGI("video focus request mode=%d reason=%d", (int)req.focus_mode(), (int)req.focus_reason());
    sendVideoFocusIndication(false);
    channel_->receive(shared_from_this());
}

void VideoService::onChannelError(const error::Error& e) {
    // See AndroidAutoEntity::onChannelError for why this re-arm is required
    // -- without it, a single transient receive error permanently stops new
    // video frames from ever being processed again (session looks alive,
    // screen just never updates again).
    LOGE("channel error: %s", e.what());
    channel_->receive(shared_from_this());
}

} // namespace aasdk_android
