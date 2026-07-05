#pragma once
#include <f1x/aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <f1x/aasdk/Channel/Promise.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <atomic>

namespace aasdk_android {

class Pinger : public std::enable_shared_from_this<Pinger> {
public:
    using Pointer = std::shared_ptr<Pinger>;

    Pinger(boost::asio::io_service& ioService,
           f1x::aasdk::channel::control::ControlServiceChannel::Pointer channel);

    void start();
    void cancel();

private:
    void schedulePing();

    boost::asio::io_service::strand strand_;
    f1x::aasdk::channel::control::ControlServiceChannel::Pointer channel_;
    boost::asio::steady_timer timer_;
    std::atomic<bool> running_{false};

    static constexpr int kIntervalMs = 2000;
};

} // namespace aasdk_android
