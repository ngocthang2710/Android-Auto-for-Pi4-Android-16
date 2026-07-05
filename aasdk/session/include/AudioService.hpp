#pragma once
#include <f1x/aasdk/Channel/AV/IAudioServiceChannel.hpp>
#include <f1x/aasdk/Channel/AV/IAudioServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Messenger/IMessenger.hpp>
#include <ServiceDiscoveryResponseMessage.pb.h>
#include <AndroidAudioOutput.hpp>
#include <memory>
#include <boost/asio.hpp>

namespace aasdk_android {

enum class AudioChannelType { MEDIA, SPEECH, SYSTEM };

// Creates the appropriate AudioServiceChannel subtype internally so that
// the channel's strand& points to this object's own strand_ member.
class AudioService
    : public f1x::aasdk::channel::av::IAudioServiceChannelEventHandler,
      public std::enable_shared_from_this<AudioService> {
public:
    using Pointer = std::shared_ptr<AudioService>;

    AudioService(boost::asio::io_service& ioService,
                 f1x::aasdk::messenger::IMessenger::Pointer messenger,
                 AudioChannelType type,
                 std::shared_ptr<AndroidAudioOutput> output);

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
    void onChannelError(const f1x::aasdk::error::Error& e) override;

private:
    AudioChannelType type_;
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::av::IAudioServiceChannel::Pointer channel_;
    std::shared_ptr<AndroidAudioOutput> output_;
    int32_t session_{-1};
};

} // namespace aasdk_android
