#pragma once
#include <f1x/aasdk/Channel/Sensor/SensorServiceChannel.hpp>
#include <f1x/aasdk/Channel/Sensor/ISensorServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Messenger/IMessenger.hpp>
#include <ServiceDiscoveryResponseMessage.pb.h>
#include <boost/asio.hpp>
#include <memory>

namespace aasdk_android {

class SensorService
    : public f1x::aasdk::channel::sensor::ISensorServiceChannelEventHandler,
      public std::enable_shared_from_this<SensorService> {
public:
    using Pointer = std::shared_ptr<SensorService>;

    SensorService(boost::asio::io_service& ioService,
                  f1x::aasdk::messenger::IMessenger::Pointer messenger);

    void start();
    void stop();
    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& resp);

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest& req) override;
    void onSensorStartRequest(const f1x::aasdk::proto::messages::SensorStartRequestMessage& req) override;
    void onChannelError(const f1x::aasdk::error::Error& e) override;

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::sensor::SensorServiceChannel::Pointer channel_;
};

} // namespace aasdk_android
