#include "rtp/jitter_buffer.hpp"

#include <cstdlib>

namespace rtp {

uint32_t JitterBuffer::extend(uint16_t seq) {
    // Map a 16-bit sequence number to the 32-bit space closest to the last one seen.
    int16_t diff = static_cast<int16_t>(seq - last_seq_);
    if (diff > 0 && seq < last_seq_) cycles_ += 65536;
    uint32_t ext = cycles_ + seq;
    if (diff < 0 && seq > last_seq_) ext -= 65536;  // older packet from before the wrap
    if (diff > 0) last_seq_ = seq;
    return ext;
}

void JitterBuffer::anchor(uint16_t seq, uint64_t now_ms) {
    if (have_first_) ++s_.resyncs;
    have_first_ = true;
    paused_ = false;
    dry_ = 0;
    bad_seq_ = -1;
    q_.clear();
    start_ms_ = now_ms + depth_;
    last_seq_ = seq;
    cycles_ = 0;
    next_ext_ = seq;
}

void JitterBuffer::push(uint16_t seq, const uint8_t* payload, size_t n, uint64_t now_ms, uint32_t ssrc) {
    ++s_.pushed;
    if (have_first_ && ssrc != ssrc_) ++s_.ssrc_changes;
    if (!have_first_ || paused_ || ssrc != ssrc_) {
        ssrc_ = ssrc;
        anchor(seq, now_ms);
    } else if (std::abs(static_cast<int16_t>(seq - last_seq_)) > kMaxJump) {
        if (static_cast<int32_t>(seq) == bad_seq_) {
            anchor(seq, now_ms);  // two in a row: the sender really restarted its sequence
        } else {
            bad_seq_ = static_cast<uint16_t>(seq + 1);
            ++s_.jump_dropped;
            return;
        }
    }
    uint32_t ext = extend(seq);
    if (ext < next_ext_) { ++s_.late; return; }       // its slot already went by
    if (q_.count(ext)) { ++s_.duplicates; return; }
    if (q_.size() > 500) { ++s_.overflow; return; }    // sender far ahead of playout clock
    q_.emplace(ext, std::vector<uint8_t>(payload, payload + n));
}

std::optional<JitterBuffer::Frame> JitterBuffer::pop(uint64_t now_ms) {
    if (!have_first_ || paused_ || now_ms < start_ms_) return std::nullopt;
    Frame f;
    f.seq = static_cast<uint16_t>(next_ext_);
    auto it = q_.find(next_ext_);
    if (it != q_.end()) {
        f.real = true;
        f.payload = std::move(it->second);
        q_.erase(it);
        ++s_.played;
        dry_ = 0;
    } else {
        ++s_.concealed;
        if (++dry_ >= kDryFrames && q_.empty()) paused_ = true;  // stream stopped: wait and re-anchor
    }
    ++next_ext_;
    // Anything older than the playout point can never be played.
    while (!q_.empty() && q_.begin()->first < next_ext_) q_.erase(q_.begin());
    return f;
}

}  // namespace rtp
