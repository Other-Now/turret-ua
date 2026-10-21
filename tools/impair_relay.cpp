// impair-relay: one-way UDP relay that adds delay, jitter and loss - a
// userspace stand-in for `tc qdisc ... netem`, which needs root (not available
// in this WSL box; CI can use the real thing).
//
//   impair-relay --listen 127.0.0.1:47000 --to 127.0.0.1:40002 --delay 20 --jitter 15 --loss 0.01 --seed 7
//
// Per packet: dropped with probability `loss`, otherwise forwarded after
// delay + |N(0, jitter)| ms. Large jitter therefore also reorders packets,
// which is what a real congested path does to RTP.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "core/event_loop.hpp"
#include "core/udp_transport.hpp"

namespace {
core::EventLoop* g_loop = nullptr;
void on_signal(int) { if (g_loop) g_loop->stop(); }

bool split(const std::string& s, core::Endpoint& ep) {
    auto c = s.rfind(':');
    if (c == std::string::npos) return false;
    ep = {s.substr(0, c), static_cast<uint16_t>(std::atoi(s.c_str() + c + 1))};
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    core::Endpoint listen, to;
    double loss = 0, delay = 0, jitter = 0;
    long duration = -1;
    unsigned seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i], v = argv[i + 1];
        if (a == "--listen") split(v, listen);
        else if (a == "--to") split(v, to);
        else if (a == "--loss") loss = std::atof(v.c_str());
        else if (a == "--delay") delay = std::atof(v.c_str());
        else if (a == "--jitter") jitter = std::atof(v.c_str());
        else if (a == "--seed") seed = static_cast<unsigned>(std::atoi(v.c_str()));
        else if (a == "--duration") duration = std::atol(v.c_str());
    }
    if (listen.port == 0 || to.port == 0) {
        std::puts("impair-relay --listen IP:PORT --to IP:PORT [--delay MS] [--jitter MS] [--loss P] [--seed N]");
        return 2;
    }
    core::EventLoop loop;
    g_loop = &loop;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    core::Endpoint bound;
    int in = core::open_udp(listen.host, listen.port, &bound);
    int out = core::open_udp(listen.host, 0, nullptr);
    if (in < 0 || out < 0) { std::perror("bind"); return 1; }
    sockaddr_in dst;
    core::to_sockaddr(to, &dst);

    std::mt19937 rng(seed);
    std::normal_distribution<double> norm(0, jitter > 0 ? jitter : 1);
    std::uniform_real_distribution<double> uni(0, 1);
    uint64_t rx = 0, dropped = 0, fwd = 0;

    loop.add_fd(in, [&] {
        char buf[2048];
        for (;;) {
            ssize_t n = ::recv(in, buf, sizeof buf, 0);
            if (n <= 0) return;
            ++rx;
            if (uni(rng) < loss) { ++dropped; continue; }
            double d = delay + (jitter > 0 ? std::fabs(norm(rng)) : 0);
            std::string pkt(buf, static_cast<size_t>(n));
            loop.schedule(static_cast<uint64_t>(std::lround(d)), [&, pkt] {
                ::sendto(out, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst);
                ++fwd;
            });
        }
    });
    if (duration > 0) loop.schedule(static_cast<uint64_t>(duration), [&] { loop.stop(); });
    std::printf("impair-relay %s -> %s delay=%.0fms jitter=%.0fms loss=%.3f\n", bound.str().c_str(),
                to.str().c_str(), delay, jitter, loss);
    std::fflush(stdout);
    loop.run();
    std::printf("{\"rx\": %llu, \"dropped\": %llu, \"forwarded\": %llu}\n", static_cast<unsigned long long>(rx),
                static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(fwd));
    ::close(in);
    ::close(out);
    return 0;
}
