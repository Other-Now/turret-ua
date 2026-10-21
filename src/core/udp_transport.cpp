#include "core/udp_transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace core {

bool to_sockaddr(const Endpoint& ep, void* out) {
    auto* sin = static_cast<sockaddr_in*>(out);
    std::memset(sin, 0, sizeof *sin);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(ep.port);
    return inet_pton(AF_INET, ep.host.c_str(), &sin->sin_addr) == 1;
}

int open_udp(const std::string& ip, uint16_t port, Endpoint* bound) {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in sin;
    if (!to_sockaddr({ip, port}, &sin) || ::bind(fd, reinterpret_cast<sockaddr*>(&sin), sizeof sin) < 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof sin;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&sin), &len);
    if (bound) *bound = {ip, ntohs(sin.sin_port)};
    return fd;
}

UdpTransport::UdpTransport(EventLoop& loop, const std::string& ip, uint16_t port, util::PcapWriter* pcap)
    : loop_(loop), pcap_(pcap) {
    fd_ = open_udp(ip, port, &local_);
    if (fd_ < 0) {
        std::perror("sip bind");
        return;
    }
    loop_.add_fd(fd_, [this] { readable(); });
}

UdpTransport::~UdpTransport() {
    if (fd_ >= 0) {
        loop_.remove_fd(fd_);
        ::close(fd_);
    }
}

void UdpTransport::send(const std::string& data, const Endpoint& to) {
    sockaddr_in sin;
    if (!to_sockaddr(to, &sin)) { ++tx_errors; return; }
    if (::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&sin), sizeof sin) < 0) {
        ++tx_errors;
        return;
    }
    ++tx;
    if (pcap_) pcap_->write(local_, to, data.data(), data.size());
}

void UdpTransport::readable() {
    char buf[65536];
    for (;;) {
        sockaddr_in sin;
        socklen_t len = sizeof sin;
        ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&sin), &len);
        if (n < 0) return;  // EAGAIN (or ICMP error surfaced as ECONNREFUSED): done
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin.sin_addr, ip, sizeof ip);
        Endpoint src{ip, ntohs(sin.sin_port)};
        ++rx;
        if (pcap_) pcap_->write(src, local_, buf, static_cast<size_t>(n));
        if (on_datagram) on_datagram(std::string(buf, static_cast<size_t>(n)), src);
    }
}

}  // namespace core
