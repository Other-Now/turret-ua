// Fixed-delay playout (jitter) buffer.
//
// Playout starts `depth_ms` after the first packet arrives; after that one frame
// is pulled every frame_ms. A packet that arrives after its slot was played (or
// concealed) is "late" and discarded - to the listener it is as lost as a packet
// the network dropped. Depth trades mouth-to-ear delay against late loss; the
// bench (bench/jb_sweep.cpp) measures that trade-off.
//
// If the stream goes quiet (hold, silence suppression, a sender restart) the
// buffer runs dry; after kDryFrames consecutive concealments it pauses, and the
// next packet re-anchors playout (new start = arrival + depth). Without this, a
// 2 s hold left playout 100 frames ahead of the resumed stream and every later
// packet was discarded as late (see docs/NOTES.md, "hold killed the audio").
//
// A new SSRC (Asterisk switches source when it re-bridges a held call) also
// re-anchors playout. A sequence jump of more than kMaxJump on the same SSRC is
// only believed once a second, consecutive packet confirms it (RFC 3550 A.1
// probation); a single stray packet is dropped instead of derailing playout.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace rtp {

class JitterBuffer {
public:
    explicit JitterBuffer(uint32_t depth_ms, uint32_t frame_ms = 20) : depth_(depth_ms), frame_(frame_ms) {}

    static constexpr uint32_t kDryFrames = 10;  // 200 ms of nothing -> pause playout

    static constexpr int kMaxJump = 3000;

    void push(uint16_t seq, const uint8_t* payload, size_t n, uint64_t now_ms, uint32_t ssrc = 0);

    struct Frame {
        bool real = false;  // false = concealment (packet missing at its playout time)
        uint16_t seq = 0;
        std::vector<uint8_t> payload;
    };
    // Pull the next frame. nullopt until playout has started.
    std::optional<Frame> pop(uint64_t now_ms);

    bool started() const { return have_first_; }
    bool paused() const { return paused_; }
    uint64_t playout_start_ms() const { return start_ms_; }
    uint32_t depth_ms() const { return depth_; }

    struct Stats {
        uint64_t pushed = 0, played = 0, concealed = 0, late = 0, duplicates = 0, overflow = 0, resyncs = 0;
        uint64_t ssrc_changes = 0, jump_dropped = 0;
    };
    const Stats& stats() const { return s_; }

private:
    uint32_t depth_, frame_;
    bool have_first_ = false, paused_ = false;
    uint32_t dry_ = 0;
    uint32_t ssrc_ = 0;
    int32_t bad_seq_ = -1;  // probation: the seq that would confirm a jump
    void anchor(uint16_t seq, uint64_t now_ms);
    uint64_t start_ms_ = 0;
    uint32_t next_ext_ = 0;   // extended seq of the next frame to play
    uint32_t cycles_ = 0;
    uint16_t last_seq_ = 0;
    std::map<uint32_t, std::vector<uint8_t>> q_;
    Stats s_;
    uint32_t extend(uint16_t seq);
};

}  // namespace rtp
