#pragma once
#include <f1x/aasdk/Channel/AV/VideoServiceChannel.hpp>
#include <f1x/aasdk/Channel/AV/IVideoServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Messenger/IMessenger.hpp>
#include <ServiceDiscoveryResponseMessage.pb.h>
#include <AndroidVideoOutput.hpp>
#include <memory>
#include <boost/asio.hpp>

namespace aasdk_android {

class VideoService
    : public f1x::aasdk::channel::av::IVideoServiceChannelEventHandler,
      public std::enable_shared_from_this<VideoService> {
public:
    using Pointer = std::shared_ptr<VideoService>;

    VideoService(boost::asio::io_service& ioService,
                 f1x::aasdk::messenger::IMessenger::Pointer messenger,
                 std::shared_ptr<AndroidVideoOutput> output);

    void start();
    void stop();
    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& resp);

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest& req) override;
    void onAVChannelSetupRequest(const f1x::aasdk::proto::messages::AVChannelSetupRequest& req) override;
    void onAVChannelStartIndication(const f1x::aasdk::proto::messages::AVChannelStartIndication& ind) override;
    void onAVChannelStopIndication(const f1x::aasdk::proto::messages::AVChannelStopIndication& ind) override;
    void onAVMediaWithTimestampIndication(f1x::aasdk::messenger::Timestamp::ValueType ts,
                                          const f1x::aasdk::common::DataConstBuffer& buf) override;
    void onAVMediaIndication(const f1x::aasdk::common::DataConstBuffer& buf) override;
    void onVideoFocusRequest(const f1x::aasdk::proto::messages::VideoFocusRequest& req) override;
    void onChannelError(const f1x::aasdk::error::Error& e) override;

private:
    void sendVideoFocusIndication(bool unrequested);

    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::av::VideoServiceChannel::Pointer channel_;
    std::shared_ptr<AndroidVideoOutput> output_;
    int32_t session_{-1};
};

} // namespace aasdk_android
