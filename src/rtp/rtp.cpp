#include "rtp/rtp.hpp"

#include <cmath>

namespace rtp {
namespace {
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xff); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x >> 16); put16(v, x & 0xffff); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) << 16 | get16(p + 2); }
}  // namespace

std::vector<uint8_t> build(const Header& h, const uint8_t* payload, size_t n) {
    std::vector<uint8_t> v;
    v.reserve(kHeaderSize + n);
    v.push_back(0x80);  // V=2, P=0, X=0, CC=0
    v.push_back(static_cast<uint8_t>((h.marker ? 0x80 : 0) | (h.pt & 0x7f)));
    put16(v, h.seq);
    put32(v, h.ts);
    put32(v, h.ssrc);
    v.insert(v.end(), payload, payload + n);
    return v;
}

std::optional<Header> parse(const uint8_t* b, size_t n, size_t* off) {
    if (n < kHeaderSize || (b[0] >> 6) != 2) return std::nullopt;
    const size_t cc = b[0] & 0x0f;
    size_t o = kHeaderSize + 4 * cc;
    if (b[0] & 0x10) {  // header extension
        if (n < o + 4) return std::nullopt;
        o += 4 + 4 * size_t(get16(b + o + 2));
    }
    if (o > n) return std::nullopt;
    if (b[0] & 0x20) {  // padding
        uint8_t pad = b[n - 1];
        if (pad == 0 || o + pad > n) return std::nullopt;
        n -= pad;
    }
    Header h;
    h.marker = b[1] & 0x80;
    h.pt = b[1] & 0x7f;
    h.seq = get16(b + 2);
    h.ts = get32(b + 4);
    h.ssrc = get32(b + 8);
    if (off) *off = o;
    return h;
}

void ReceiverStats::on_packet(uint16_t seq, uint32_t rtp_ts, uint32_t arrival, bool marker) {
    constexpr uint16_t kMaxDropout = 3000;
    if (!started_) {
        started_ = true;
        base_seq_ = seq;
        max_seq_ = seq;
    } else {
        uint16_t udelta = static_cast<uint16_t>(seq - max_seq_);
        if (udelta == 0) {
            // duplicate
        } else if (udelta < kMaxDropout) {
            if (seq < max_seq_) cycles_ += 65536;  // wrapped
            max_seq_ = seq;
        } else {
            ++reordered_;  // older than max: late/reordered (or a huge jump we do not resync on)
        }
    }
    ++received_;

    // RFC 3550 A.8: J += (|D| - J) / 16, D from consecutive arrivals' relative transit
    int64_t transit = int64_t(arrival) - int64_t(rtp_ts);
    if (have_transit_ && !marker) {
        double d = std::fabs(double(transit - last_transit_));
        jitter_ += (d - jitter_) / 16.0;
    }
    last_transit_ = transit;
    have_transit_ = true;
}

uint8_t ReceiverStats::fraction_lost_and_reset() {
    uint32_t exp = expected();
    uint32_t exp_int = exp - expected_prior_;
    uint32_t rec_int = received_ - received_prior_;
    expected_prior_ = exp;
    received_prior_ = received_;
    int64_t lost_int = int64_t(exp_int) - int64_t(rec_int);
    if (exp_int == 0 || lost_int <= 0) return 0;
    return static_cast<uint8_t>((lost_int << 8) / exp_int);
}

std::vector<uint8_t> build_rr(uint32_t sender_ssrc, const ReportBlock& b) {
    std::vector<uint8_t> v;
    v.push_back(0x81);  // V=2, RC=1
    v.push_back(201);   // RR
    put16(v, 7);        // length in 32-bit words minus one
    put32(v, sender_ssrc);
    put32(v, b.ssrc);
    int32_t cl = b.cumulative_lost;
    if (cl > 0x7fffff) cl = 0x7fffff;
    if (cl < -0x800000) cl = -0x800000;
    put32(v, uint32_t(b.fraction_lost) << 24 | (uint32_t(cl) & 0xffffff));
    put32(v, b.ext_max_seq);
    put32(v, b.jitter);
    put32(v, 0);  // LSR (no SR received)
    put32(v, 0);  // DLSR
    return v;
}

std::optional<ReportBlock> parse_rr(const uint8_t* p, size_t n) {
    if (n < 32 || (p[0] >> 6) != 2 || p[1] != 201 || (p[0] & 0x1f) < 1) return std::nullopt;
    ReportBlock b;
    b.ssrc = get32(p + 8);
    b.fraction_lost = p[12];
    uint32_t cl = get32(p + 12) & 0xffffff;
    b.cumulative_lost = (cl & 0x800000) ? int32_t(cl | 0xff000000) : int32_t(cl);
    b.ext_max_seq = get32(p + 16);
    b.jitter = get32(p + 20);
    return b;
}

}  // namespace rtp
