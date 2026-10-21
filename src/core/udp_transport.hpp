#pragma once
#include <functional>
#include <string>

#include "core/event_loop.hpp"
#include "util/pcap_writer.hpp"

namespace core {

// Non-blocking UDP socket registered with an EventLoop. Must be created, used
// and destroyed on the loop thread.
class UdpTransport : public Transport {
public:
    UdpTransport(EventLoop& loop, const std::string& ip, uint16_t port, util::PcapWriter* pcap = nullptr);
    ~UdpTransport() override;
    void send(const std::string& data, const Endpoint& to) override;
    Endpoint local() const override { return local_; }
    bool ok() const { return fd_ >= 0; }

    std::function<void(const std::string&, const Endpoint&)> on_datagram;
    uint64_t rx = 0, tx = 0, tx_errors = 0;

private:
    void readable();
    EventLoop& loop_;
    int fd_ = -1;
    Endpoint local_;
    util::PcapWriter* pcap_;
};

// Helper shared with the RTP code.
int open_udp(const std::string& ip, uint16_t port, Endpoint* bound);
bool to_sockaddr(const Endpoint& ep, void* sin);

}  // namespace core
