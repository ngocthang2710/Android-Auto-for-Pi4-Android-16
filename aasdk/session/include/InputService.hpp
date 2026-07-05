#pragma once
#include <f1x/aasdk/Channel/Input/InputServiceChannel.hpp>
#include <f1x/aasdk/Channel/Input/IInputServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Messenger/IMessenger.hpp>
#include <ServiceDiscoveryResponseMessage.pb.h>
#include <boost/asio.hpp>
#include <memory>

namespace aasdk_android {

class InputService
    : public f1x::aasdk::channel::input::IInputServiceChannelEventHandler,
      public std::enable_shared_from_this<InputService> {
public:
    using Pointer = std::shared_ptr<InputService>;

    InputService(boost::asio::io_service& ioService,
                 f1x::aasdk::messenger::IMessenger::Pointer messenger);

    void start();
    void stop();
    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& resp);
    void injectTouchEvent(int action, float x, float y);

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest& req) override;
    void onBindingRequest(const f1x::aasdk::proto::messages::BindingRequest& req) override;
    void onChannelError(const f1x::aasdk::error::Error& e) override;

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::input::InputServiceChannel::Pointer channel_;
};

} // namespace aasdk_android
