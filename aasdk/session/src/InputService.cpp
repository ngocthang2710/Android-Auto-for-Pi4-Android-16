#include <InputService.hpp>
#include <InputEventIndicationMessage.pb.h>
#include <android/log.h>
#include <chrono>

#define LOG_TAG "AaSdk_InputSvc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

namespace {
// Android MotionEvent action codes (ACTION_DOWN=0/UP=1/MOVE=2) line up with
// TouchAction's PRESS/RELEASE/DRAG; anything else (CANCEL, POINTER_DOWN/UP,
// multi-touch) has no direct AA equivalent for this single-pointer HU, so
// treat it as a DRAG rather than silently defaulting to PRESS.
proto::enums::TouchAction::Enum toTouchAction(int androidAction) {
    switch (androidAction) {
        case 0: return proto::enums::TouchAction::PRESS;
        case 1: return proto::enums::TouchAction::RELEASE;
        case 2: return proto::enums::TouchAction::DRAG;
        default: return proto::enums::TouchAction::DRAG;
    }
}
} // namespace

InputService::InputService(boost::asio::io_service& ios,
                           messenger::IMessenger::Pointer messenger)
    : strand_(ios)
    , channel_(std::make_shared<channel::input::InputServiceChannel>(strand_, std::move(messenger))) {}

void InputService::start() {
    strand_.dispatch([this, self = shared_from_this()]() {
        channel_->receive(shared_from_this());
    });
}

void InputService::stop() {}

void InputService::fillFeatures(proto::messages::ServiceDiscoveryResponse& resp) {
    auto* ch = resp.add_channels();
    ch->set_channel_id(static_cast<uint32_t>(channel_->getId()));
    auto* ic = ch->mutable_input_channel();
    ic->set_display_id(0);
    // Must match the resolution VideoService actually decodes/renders at
    // (kDefaultConfigIndex -> 1920x1080), or touch coordinates land on the
    // wrong on-screen element even though every send succeeds.
    auto* ts = ic->add_touchscreen();
    ts->set_width(1920);
    ts->set_height(1080);
}

void InputService::onChannelOpenRequest(const proto::messages::ChannelOpenRequest&) {
    LOGI("open request");
    proto::messages::ChannelOpenResponse resp;
    resp.set_status(proto::enums::Status::OK);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("openResp: %s", e.what()); });
    channel_->sendChannelOpenResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void InputService::onBindingRequest(const proto::messages::BindingRequest& req) {
    proto::messages::BindingResponse resp;
    resp.set_status(proto::enums::Status::OK);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("bindingResp: %s", e.what()); });
    channel_->sendBindingResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void InputService::injectTouchEvent(int action, float x, float y) {
    LOGI("injectTouchEvent action=%d x=%.1f y=%.1f", action, x, y);
    strand_.dispatch([this, self = shared_from_this(), action, x, y]() {
        proto::messages::InputEventIndication ind;
        // Modern AA wire schema expects a monotonic timestamp per event;
        // phones silently drop touch indications carrying timestamp=0.
        ind.set_timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        // Which channel's rendered content this touch applies to -- must
        // be VideoService's channel id (ChannelId::VIDEO = 3), or the
        // phone has no way to know this event targets the video surface
        // it's displaying (0, the unset default, is the control channel).
        ind.set_disp_channel(3);
        auto* touch = ind.mutable_touch_event();
        auto* loc = touch->add_touch_location();
        loc->set_x(static_cast<uint32_t>(x));
        loc->set_y(static_cast<uint32_t>(y));
        loc->set_pointer_id(0);

        touch->set_action_index(0);
        touch->set_touch_action(toTouchAction(action));

        {
            const std::string serialized = ind.SerializeAsString();
            std::string hex;
            hex.reserve(serialized.size() * 2);
            static const char* digits = "0123456789abcdef";
            for (unsigned char c : serialized) {
                hex.push_back(digits[c >> 4]);
                hex.push_back(digits[c & 0xF]);
            }
            LOGI("InputEventIndication bytes (%zu): %s", serialized.size(), hex.c_str());
        }

        auto promise = channel::SendPromise::defer(strand_);
        promise->then([]() { LOGI("touchEvent sent OK"); },
                      [](const error::Error& e) { LOGE("touchEvent: %s", e.what()); });
        channel_->sendInputEventIndication(ind, std::move(promise));
    });
}

void InputService::onChannelError(const error::Error& e) {
    // See AndroidAutoEntity::onChannelError for why this re-arm is required.
    LOGE("channel error: %s", e.what());
    channel_->receive(shared_from_this());
}

} // namespace aasdk_android
