#pragma once
#include <f1x/aasdk/Channel/Bluetooth/BluetoothServiceChannel.hpp>
#include <f1x/aasdk/Channel/Bluetooth/IBluetoothServiceChannelEventHandler.hpp>
#include <f1x/aasdk/Messenger/IMessenger.hpp>
#include <ServiceDiscoveryResponseMessage.pb.h>
#include <boost/asio.hpp>
#include <memory>

namespace aasdk_android {

// Reports BT unavailable — wireless AA pairing deferred.
class BluetoothService
    : public f1x::aasdk::channel::bluetooth::IBluetoothServiceChannelEventHandler,
      public std::enable_shared_from_this<BluetoothService> {
public:
    using Pointer = std::shared_ptr<BluetoothService>;

    BluetoothService(boost::asio::io_service& ioService,
                     f1x::aasdk::messenger::IMessenger::Pointer messenger);

    void start();
    void stop();
    void fillFeatures(f1x::aasdk::proto::messages::ServiceDiscoveryResponse& resp);

    void onChannelOpenRequest(const f1x::aasdk::proto::messages::ChannelOpenRequest& req) override;
    void onBluetoothPairingRequest(const f1x::aasdk::proto::messages::BluetoothPairingRequest& req) override;
    void onChannelError(const f1x::aasdk::error::Error& e) override;

private:
    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::bluetooth::BluetoothServiceChannel::Pointer channel_;
};

} // namespace aasdk_android
