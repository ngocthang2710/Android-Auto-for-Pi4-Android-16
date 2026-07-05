#pragma once
#include <f1x/aasdk/Channel/AV/IAVInputServiceChannel.hpp>
#include <f1x/aasdk/Channel/AV/IAVInputServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Messenger/IMessenger.hpp>
#include <ServiceDiscoveryResponseMessage.pb.h>
#include <memory>
#include <boost/asio.hpp>

namespace aasdk_android {

// Mic channel (ChannelId::AV_INPUT). No physical mic is wired up on this
// head unit yet: this advertises the channel and completes the open/setup
// handshake (real AA phones require a head unit to expose a mic channel at
// all, even one that never actually streams audio, or they refuse to bring
// up any endpoints -- see GEARHEAD_ENDPOINTS_FAILED investigation notes).
class AVInputService
    : public f1x::aasdk::channel::av::IAVInputServiceChannelEventHandler,
      public std::enable_shared_from_this<AVInputService> {
public:
    using Pointer = std::shared_ptr<AVInputService>;

    AVInputService(boost::asio::io_service& ioService,
                   f1x::aasdk::messenger::IMessenger::Pointer messenger);

    void start();
    void stop();
    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& resp);

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest& req) override;
    void onAVChannelSetupRequest(const f1x::aasdk::proto::messages::AVChannelSetupRequest& req) override;
    void onAVInputOpenRequest(const f1x::aasdk::proto::messages::AVInputOpenRequest& req) override;
    void onAVMediaAckIndication(const f1x::aasdk::proto::messages::AVMediaAckIndication& ind) override;
    void onChannelError(const f1x::aasdk::error::Error& e) override;

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::av::IAVInputServiceChannel::Pointer channel_;
    int32_t session_{-1};
};

} // namespace aasdk_android
