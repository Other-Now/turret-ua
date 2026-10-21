// RTP packets, RFC 3550 receiver statistics (A.1 sequence tracking, A.8
// interarrival jitter) and the RTCP Receiver Report that carries them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rtp {

struct Header {
    uint8_t pt = 0;
    bool marker = false;
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;
};

constexpr size_t kHeaderSize = 12;

std::vector<uint8_t> build(const Header& h, const uint8_t* payload, size_t n);
// Returns the header and sets *payload_off; nullopt if not RTP version 2.
std::optional<Header> parse(const uint8_t* buf, size_t n, size_t* payload_off);

class ReceiverStats {
public:
    // arrival is the local receive time converted to RTP timestamp units (8 kHz).
    // A marker packet starts a new talkspurt: its timestamp may jump (Asterisk
    // relaying another leg's clock), so it re-baselines transit time instead of
    // feeding |D| into J - same rule Wireshark's RTP analysis uses.
    void on_packet(uint16_t seq, uint32_t rtp_ts, uint32_t arrival, bool marker = false);

    uint32_t received() const { return received_; }
    uint32_t ext_max_seq() const { return cycles_ + max_seq_; }
    uint32_t expected() const { return started_ ? ext_max_seq() - base_seq_ + 1 : 0; }
    int32_t lost() const { return static_cast<int32_t>(expected()) - static_cast<int32_t>(received_); }
    double jitter_ts() const { return jitter_; }                // RTP timestamp units
    double jitter_ms() const { return jitter_ / 8.0; }          // 8 kHz clock
    uint32_t reordered() const { return reordered_; }
    // RFC 3550 6.4.1 fraction lost since the previous report (8-bit fixed point).
    uint8_t fraction_lost_and_reset();

private:
    bool started_ = false;
    uint16_t max_seq_ = 0;
    uint32_t cycles_ = 0, base_seq_ = 0, received_ = 0, reordered_ = 0;
    uint32_t expected_prior_ = 0, received_prior_ = 0;
    int64_t last_transit_ = 0;
    bool have_transit_ = false;
    double jitter_ = 0;
};

struct ReportBlock {
    uint32_t ssrc = 0;
    uint8_t fraction_lost = 0;
    int32_t cumulative_lost = 0;
    uint32_t ext_max_seq = 0;
    uint32_t jitter = 0;  // timestamp units
};

std::vector<uint8_t> build_rr(uint32_t sender_ssrc, const ReportBlock& b);
std::optional<ReportBlock> parse_rr(const uint8_t* buf, size_t n);

}  // namespace rtp
