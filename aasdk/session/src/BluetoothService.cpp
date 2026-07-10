#include <BluetoothService.hpp>
#include <android/log.h>

#define LOG_TAG "AaSdk_BtSvc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aasdk_android {

using namespace f1x::aasdk;

BluetoothService::BluetoothService(boost::asio::io_service& ios,
                                   messenger::IMessenger::Pointer messenger)
    : strand_(ios)
    , channel_(std::make_shared<channel::bluetooth::BluetoothServiceChannel>(strand_, std::move(messenger))) {}

void BluetoothService::start() {
    strand_.dispatch([this, self = shared_from_this()]() {
        channel_->receive(shared_from_this());
    });
}

void BluetoothService::stop() {}

void BluetoothService::fillFeatures(proto::messages::ServiceDiscoveryResponse& resp) {
    auto* ch = resp.add_channels();
    ch->set_channel_id(static_cast<uint32_t>(channel_->getId()));
    // Report BT unavailable; phone will fall back to USB-only.
    // Empty adapter_address / no supported_pairing_methods advertises no BT capability.
    ch->mutable_bluetooth_channel();
}

void BluetoothService::onChannelOpenRequest(const proto::messages::ChannelOpenRequest&) {
    LOGI("open request");
    proto::messages::ChannelOpenResponse resp;
    resp.set_status(proto::enums::Status::OK);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("openResp: %s", e.what()); });
    channel_->sendChannelOpenResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void BluetoothService::onBluetoothPairingRequest(const proto::messages::BluetoothPairingRequest&) {
    proto::messages::BluetoothPairingResponse resp;
    resp.set_status(proto::enums::BluetoothPairingStatus::FAIL);
    auto promise = channel::SendPromise::defer(strand_);
    promise->then([]() {}, [](const error::Error& e) { LOGE("pairingResp: %s", e.what()); });
    channel_->sendBluetoothPairingResponse(resp, std::move(promise));
    channel_->receive(shared_from_this());
}

void BluetoothService::onChannelError(const error::Error& e) {
    // See AndroidAutoEntity::onChannelError for why this re-arm is required.
    LOGE("channel error: %s", e.what());
    channel_->receive(shared_from_this());
}

} // namespace aasdk_android
