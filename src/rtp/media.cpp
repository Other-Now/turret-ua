#include "rtp/media.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

#include "core/udp_transport.hpp"
#include "rtp/g711.hpp"

namespace rtp {
namespace {

uint64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

class WavWriter {
public:
    explicit WavWriter(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
        if (f_) std::fseek(f_, 44, SEEK_SET);
    }
    ~WavWriter() {
        if (!f_) return;
        uint32_t data = samples_ * 2, riff = 36 + data, fmt = 16, rate = 8000, byte_rate = 16000;
        uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
        std::fseek(f_, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f_); std::fwrite(&riff, 4, 1, f_); std::fwrite("WAVEfmt ", 1, 8, f_);
        std::fwrite(&fmt, 4, 1, f_); std::fwrite(&pcm, 2, 1, f_); std::fwrite(&ch, 2, 1, f_);
        std::fwrite(&rate, 4, 1, f_); std::fwrite(&byte_rate, 4, 1, f_); std::fwrite(&align, 2, 1, f_);
        std::fwrite(&bits, 2, 1, f_); std::fwrite("data", 1, 4, f_); std::fwrite(&data, 4, 1, f_);
        std::fclose(f_);
    }
    void write(const int16_t* s, size_t n) {
        if (!f_) return;
        std::fwrite(s, 2, n, f_);
        samples_ += static_cast<uint32_t>(n);
    }

private:
    std::FILE* f_;
    uint32_t samples_ = 0;
};

}  // namespace

class Session {
public:
    Session(core::EventLoop& loop, const std::string& ip, uint16_t port, uint32_t depth, util::PcapWriter* pcap,
            const std::string& wav)
        : loop_(loop), jb_(depth), pcap_(pcap) {
        std::random_device rd;
        ssrc_ = rd();
        seq_ = static_cast<uint16_t>(rd());
        ts_ = rd();
        rtp_fd_ = core::open_udp(ip, port, &local_);
        rtcp_fd_ = core::open_udp(ip, port + 1, &local_rtcp_);
        if (rtp_fd_ >= 0) loop_.add_fd(rtp_fd_, [this] { on_rtp(); });
        if (rtcp_fd_ >= 0) loop_.add_fd(rtcp_fd_, [this] { on_rtcp(); });
        if (!wav.empty()) wav_ = std::make_unique<WavWriter>(wav);
        rep_.local_port = local_.port;
        rep_.jb_depth_ms = depth;
        rtcp_timer_ = loop_.schedule(5000, [this] { send_rr(); });
    }
    ~Session() {
        for (core::TimerId t : {send_timer_, play_timer_, rtcp_timer_})
            if (t) loop_.cancel(t);
        for (int fd : {rtp_fd_, rtcp_fd_})
            if (fd >= 0) { loop_.remove_fd(fd); ::close(fd); }
    }
    bool ok() const { return rtp_fd_ >= 0 && rtcp_fd_ >= 0; }
    uint16_t port() const { return local_.port; }

    void set_remote(const core::Endpoint& remote, sdp::Dir dir) {
        remote_ = remote;
        dir_ = dir;
        rep_.dir = sdp::to_string(dir);
        const bool send = sdp::sends(dir) && remote.port != 0;
        if (send && !send_timer_) {
            // The RTP timestamp is the sampling clock (RFC 3550 5.1): it must keep
            // running while we are not sending. Frames sit on a fixed 20 ms grid from
            // the first one, so after a hold the timestamp jumps by the paused time
            // and the first packet carries the marker bit (start of a talkspurt).
            const uint64_t now = now_us();
            if (grid0_us_ == 0) grid0_us_ = now;
            next_send_us_ = grid0_us_ + (now - grid0_us_ + 19999) / 20000 * 20000;
            spurt_start_ = true;
            send_frame();
        } else if (!send && send_timer_) {
            loop_.cancel(send_timer_);
            send_timer_ = 0;
        }
    }

    SessionReport report() const {
        SessionReport r = rep_;
        for (const auto& [ssrc, st] : stats_) {
            r.received += st.received();
            r.lost += st.lost();
        }
        if (auto it = stats_.find(cur_ssrc_); it != stats_.end()) r.jitter_ms = it->second.jitter_ms();
        r.ssrcs = static_cast<uint32_t>(stats_.size());
        r.jb = jb_.stats();
        return r;
    }

private:
    void sendto(int fd, const core::Endpoint& to, const std::vector<uint8_t>& p, const core::Endpoint& from) {
        sockaddr_in sin;
        if (!core::to_sockaddr(to, &sin)) return;
        ::sendto(fd, p.data(), p.size(), 0, reinterpret_cast<sockaddr*>(&sin), sizeof sin);
        if (pcap_) pcap_->write(from, to, p.data(), p.size());
    }

    void send_frame() {
        // 160 samples = 20 ms of a 440 Hz tone at 8 kHz
        uint8_t pcmu[160];
        for (int i = 0; i < 160; ++i, ++phase_)
            pcmu[i] = linear_to_ulaw(static_cast<int16_t>(8000 * std::sin(2 * M_PI * 440 * phase_ / 8000.0)));
        Header h;
        h.pt = 0;
        h.marker = spurt_start_;
        spurt_start_ = false;
        h.seq = seq_++;
        h.ts = ts_ + static_cast<uint32_t>((next_send_us_ - grid0_us_) / 20000 * 160);
        h.ssrc = ssrc_;
        sendto(rtp_fd_, remote_, build(h, pcmu, sizeof pcmu), local_);
        ++rep_.sent;
        // Schedule against an absolute clock so timer slop does not accumulate.
        next_send_us_ += 20000;
        uint64_t now = now_us();
        uint64_t delay_ms = next_send_us_ > now ? (next_send_us_ - now) / 1000 : 0;
        send_timer_ = loop_.schedule(delay_ms, [this] {
            send_timer_ = 0;
            send_frame();
        });
    }

    void on_rtp() {
        uint8_t buf[2048];
        for (;;) {
            sockaddr_in sin;
            socklen_t len = sizeof sin;
            ssize_t n = ::recvfrom(rtp_fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&sin), &len);
            if (n <= 0) return;
            if (pcap_) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sin.sin_addr, ip, sizeof ip);
                pcap_->write({ip, ntohs(sin.sin_port)}, local_, buf, static_cast<size_t>(n));
            }
            size_t off = 0;
            auto h = parse(buf, static_cast<size_t>(n), &off);
            if (!h || h->pt != 0) continue;
            uint64_t us = now_us();
            // RFC 3550 statistics are per source: a new SSRC is a new stream.
            cur_ssrc_ = h->ssrc;
            ReceiverStats& st = stats_[h->ssrc];
            st.on_packet(h->seq, h->ts, static_cast<uint32_t>(us * 8 / 1000), h->marker);
            if (st.jitter_ms() > rep_.max_jitter_ms) rep_.max_jitter_ms = st.jitter_ms();
            bool first = !jb_.started();
            jb_.push(h->seq, buf + off, static_cast<size_t>(n) - off, us / 1000, h->ssrc);
            if (first) {
                uint64_t start = jb_.playout_start_ms(), now = loop_.now_ms();
                play_timer_ = loop_.schedule(start > now ? start - now : 0, [this] { play(); });
                next_play_ms_ = start;
            }
        }
    }

    void play() {
        play_timer_ = 0;
        auto f = jb_.pop(now_us() / 1000);
        if (f && wav_) {
            int16_t pcm[160] = {};
            if (f->real)
                for (size_t i = 0; i < f->payload.size() && i < 160; ++i) pcm[i] = ulaw_to_linear(f->payload[i]);
            wav_->write(pcm, 160);
        }
        next_play_ms_ += 20;
        uint64_t now = loop_.now_ms();
        play_timer_ = loop_.schedule(next_play_ms_ > now ? next_play_ms_ - now : 0, [this] { play(); });
    }

    void on_rtcp() {
        uint8_t buf[1500];
        for (;;) {
            ssize_t n = ::recv(rtcp_fd_, buf, sizeof buf, 0);
            if (n <= 0) return;
            if (auto rr = parse_rr(buf, static_cast<size_t>(n))) {
                ++rep_.rr_received;
                rep_.peer_jitter_ms = rr->jitter / 8.0;
                rep_.peer_lost = rr->cumulative_lost;
            }
        }
    }

    void send_rr() {
        rtcp_timer_ = loop_.schedule(5000, [this] { send_rr(); });
        auto it = stats_.find(cur_ssrc_);
        if (it == stats_.end() || remote_.port == 0) return;
        ReceiverStats& st = it->second;
        ReportBlock b;
        b.ssrc = cur_ssrc_;
        b.fraction_lost = st.fraction_lost_and_reset();
        b.cumulative_lost = st.lost();
        b.ext_max_seq = st.ext_max_seq();
        b.jitter = static_cast<uint32_t>(st.jitter_ts());
        core::Endpoint rtcp{remote_.host, static_cast<uint16_t>(remote_.port + 1)};
        sendto(rtcp_fd_, rtcp, build_rr(ssrc_, b), local_rtcp_);
    }

    core::EventLoop& loop_;
    JitterBuffer jb_;
    std::map<uint32_t, ReceiverStats> stats_;  // per SSRC
    uint32_t cur_ssrc_ = 0;
    util::PcapWriter* pcap_;
    std::unique_ptr<WavWriter> wav_;
    int rtp_fd_ = -1, rtcp_fd_ = -1;
    core::Endpoint local_, local_rtcp_, remote_;
    sdp::Dir dir_ = sdp::Dir::Inactive;
    uint32_t ssrc_ = 0, ts_ = 0;
    uint16_t seq_ = 0;
    uint64_t phase_ = 0, next_send_us_ = 0, next_play_ms_ = 0, grid0_us_ = 0;
    bool spurt_start_ = false;
    core::TimerId send_timer_ = 0, play_timer_ = 0, rtcp_timer_ = 0;
    SessionReport rep_;
};

MediaManager::MediaManager(core::EventLoop& loop, std::string ip, uint16_t base_port, uint32_t depth,
                           util::PcapWriter* pcap)
    : loop_(loop), ip_(std::move(ip)), next_port_(base_port), depth_(depth), pcap_(pcap) {}

MediaManager::~MediaManager() = default;

uint16_t MediaManager::open(int call) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        uint16_t port = next_port_;
        next_port_ = static_cast<uint16_t>(next_port_ + 2);
        std::string wav = wav_prefix.empty() ? "" : wav_prefix + "-" + std::to_string(call) + ".wav";
        auto s = std::make_unique<Session>(loop_, ip_, port, depth_, pcap_, wav);
        if (!s->ok()) continue;  // port pair busy: try the next one
        uint16_t p = s->port();
        live_[call] = std::move(s);
        return p;
    }
    return 0;
}

void MediaManager::update(int call, const core::Endpoint& remote, sdp::Dir dir) {
    auto it = live_.find(call);
    if (it != live_.end()) it->second->set_remote(remote, dir);
}

void MediaManager::close(int call) {
    auto it = live_.find(call);
    if (it == live_.end()) return;
    done_[call] = it->second->report();
    live_.erase(it);
}

std::optional<SessionReport> MediaManager::report(int call) const {
    if (auto it = live_.find(call); it != live_.end()) return it->second->report();
    if (auto it = done_.find(call); it != done_.end()) return it->second;
    return std::nullopt;
}

}  // namespace rtp
