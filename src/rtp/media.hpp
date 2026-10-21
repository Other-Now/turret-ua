// Live media for the CLI and the GUI: one RTP + RTCP socket pair per call on
// the EventLoop, sending a G.711-encoded tone every 20 ms, receiving into the
// jitter buffer, and exchanging RTCP Receiver Reports every 5 s.
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "core/event_loop.hpp"
#include "rtp/jitter_buffer.hpp"
#include "rtp/rtp.hpp"
#include "ua/user_agent.hpp"
#include "util/pcap_writer.hpp"

namespace rtp {

struct SessionReport {
    uint16_t local_port = 0;
    uint32_t sent = 0, received = 0;
    int32_t lost = 0;           // RFC 3550 cumulative (expected - received)
    double jitter_ms = 0;       // RFC 3550 interarrival jitter at close
    double max_jitter_ms = 0;
    uint32_t ssrcs = 0;         // distinct RTP sources seen on this call
    JitterBuffer::Stats jb;
    uint32_t jb_depth_ms = 0;
    uint32_t rr_received = 0;   // RTCP RRs from the peer
    double peer_jitter_ms = 0;  // what the peer reported about OUR stream
    int32_t peer_lost = 0;
    std::string dir = "inactive";
};

class Session;

class MediaManager : public ua::MediaHandler {
public:
    MediaManager(core::EventLoop& loop, std::string ip, uint16_t base_port, uint32_t jb_depth_ms,
                 util::PcapWriter* pcap = nullptr);
    ~MediaManager() override;

    uint16_t open(int call) override;
    void update(int call, const core::Endpoint& remote, sdp::Dir dir) override;
    void close(int call) override;

    std::optional<SessionReport> report(int call) const;  // live or final
    std::string wav_prefix;  // if set: write received audio to <prefix>-<call>.wav

private:
    core::EventLoop& loop_;
    std::string ip_;
    uint16_t next_port_;
    uint32_t depth_;
    util::PcapWriter* pcap_;
    std::map<int, std::unique_ptr<Session>> live_;
    std::map<int, SessionReport> done_;
};

}  // namespace rtp
