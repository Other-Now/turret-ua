#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

#include "rtp/g711.hpp"
#include "rtp/jitter_buffer.hpp"
#include "rtp/rtp.hpp"

using namespace rtp;

TEST(G711, KnownCodes) {
    EXPECT_EQ(linear_to_ulaw(0), 0xFF);
    EXPECT_EQ(ulaw_to_linear(0xFF), 0);
    EXPECT_EQ(ulaw_to_linear(0x00), -32124);
    EXPECT_EQ(ulaw_to_linear(0x80), 32124);
}

TEST(G711, RoundTripErrorIsWithinOneQuantisationStep) {
    // mu-law step size grows with amplitude: error <= ~1/16 of |x| plus a small floor
    for (int x = -32000; x <= 32000; x += 7) {
        int y = ulaw_to_linear(linear_to_ulaw(static_cast<int16_t>(x)));
        EXPECT_LE(std::abs(x - y), std::abs(x) / 16 + 8) << x;
    }
}

TEST(Rtp, BuildParse) {
    Header h;
    h.pt = 0;
    h.marker = true;
    h.seq = 65535;
    h.ts = 0xdeadbeef;
    h.ssrc = 0x01020304;
    uint8_t pl[160] = {1, 2, 3};
    auto v = build(h, pl, sizeof pl);
    ASSERT_EQ(v.size(), 172u);
    size_t off = 0;
    auto p = parse(v.data(), v.size(), &off);
    ASSERT_TRUE(p);
    EXPECT_EQ(off, 12u);
    EXPECT_EQ(p->seq, 65535);
    EXPECT_EQ(p->ts, 0xdeadbeefu);
    EXPECT_EQ(p->ssrc, 0x01020304u);
    EXPECT_TRUE(p->marker);
}

TEST(Rtp, ParseSkipsCsrcAndExtensionRejectsBadPadding) {
    std::vector<uint8_t> v = {0x91, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1,  // V=2 X=1 CC=1
                              0, 0, 0, 9,                                   // CSRC
                              0xbe, 0xde, 0, 1, 1, 2, 3, 4,                 // ext, 1 word
                              0x55};
    size_t off = 0;
    ASSERT_TRUE(parse(v.data(), v.size(), &off));
    EXPECT_EQ(off, 24u);
    v[0] |= 0x20;  // padding bit, last byte says 0x55 bytes of padding -> invalid
    EXPECT_FALSE(parse(v.data(), v.size(), &off));
    uint8_t v1[12] = {0x40};
    EXPECT_FALSE(parse(v1, 12, &off));  // version 1
}

TEST(RtpStats, CountsLossAcrossWrap) {
    ReceiverStats s;
    uint16_t seq = 65530;
    for (int i = 0; i < 20; ++i, ++seq)
        if (i != 3 && i != 10) s.on_packet(seq, i * 160, 1000 + i * 160);
    EXPECT_EQ(s.received(), 18u);
    EXPECT_EQ(s.expected(), 20u);
    EXPECT_EQ(s.lost(), 2);
    EXPECT_EQ(s.ext_max_seq(), 65536u + 13);
}

TEST(RtpStats, JitterZeroForConstantDelayAndConvergesForAlternating) {
    ReceiverStats a, b;
    for (int i = 0; i < 500; ++i) {
        a.on_packet(static_cast<uint16_t>(i), i * 160, 5000 + i * 160);
        // transit alternates between +0 and +80 ts units (10 ms): |D| = 80 every packet
        b.on_packet(static_cast<uint16_t>(i), i * 160, 5000 + i * 160 + (i % 2) * 80);
    }
    EXPECT_DOUBLE_EQ(a.jitter_ts(), 0.0);
    EXPECT_NEAR(b.jitter_ts(), 80.0, 0.5);
    EXPECT_NEAR(b.jitter_ms(), 10.0, 0.1);
}

TEST(RtpStats, FractionLost) {
    ReceiverStats s;
    for (int i = 0; i < 100; ++i)
        if (i % 4 != 0) s.on_packet(static_cast<uint16_t>(i), 0, 0);
    // first packet (seq 0) was "lost" but base starts at 1, so 99 expected, 75 received
    EXPECT_EQ(s.fraction_lost_and_reset(), static_cast<uint8_t>((24 << 8) / 99));
    EXPECT_EQ(s.fraction_lost_and_reset(), 0);
}

TEST(Rtcp, ReceiverReportRoundTrip) {
    ReportBlock b;
    b.ssrc = 42;
    b.fraction_lost = 25;
    b.cumulative_lost = -3;  // duplicates can make it negative
    b.ext_max_seq = 70000;
    b.jitter = 123;
    auto v = build_rr(7, b);
    ASSERT_EQ(v.size(), 32u);
    auto p = parse_rr(v.data(), v.size());
    ASSERT_TRUE(p);
    EXPECT_EQ(p->ssrc, 42u);
    EXPECT_EQ(p->fraction_lost, 25);
    EXPECT_EQ(p->cumulative_lost, -3);
    EXPECT_EQ(p->ext_max_seq, 70000u);
    EXPECT_EQ(p->jitter, 123u);
}

TEST(JitterBuffer, PlaysInOrderAfterDepth) {
    JitterBuffer jb(60);
    uint8_t p[1] = {0};
    jb.push(100, p, 1, 1000);
    EXPECT_FALSE(jb.pop(1059));
    EXPECT_EQ(jb.playout_start_ms(), 1060u);
    jb.push(101, p, 1, 1020);
    auto f = jb.pop(1060);
    ASSERT_TRUE(f);
    EXPECT_TRUE(f->real);
    EXPECT_EQ(f->seq, 100);
    EXPECT_TRUE(jb.pop(1080)->real);
    EXPECT_FALSE(jb.pop(1100)->real);  // 102 never arrived -> concealed
    EXPECT_EQ(jb.stats().concealed, 1u);
}

TEST(JitterBuffer, ReorderedWithinDepthIsPlayedLateIsDropped) {
    JitterBuffer jb(40);
    uint8_t p[1] = {0};
    jb.push(10, p, 1, 0);
    jb.push(12, p, 1, 30);
    jb.push(11, p, 1, 35);  // reordered but before its slot at 60
    EXPECT_TRUE(jb.pop(40)->real);
    EXPECT_TRUE(jb.pop(60)->real);
    EXPECT_TRUE(jb.pop(80)->real);
    EXPECT_FALSE(jb.pop(100)->real);
    jb.push(13, p, 1, 101);  // slot was at 100: late
    EXPECT_EQ(jb.stats().late, 1u);
    EXPECT_EQ(jb.stats().played, 3u);
}

TEST(JitterBuffer, SequenceWrap) {
    JitterBuffer jb(20);
    uint8_t p[1] = {0};
    uint64_t t = 0;
    for (uint16_t s = 65534, i = 0; i < 6; ++i, ++s, t += 20) jb.push(s, p, 1, t);
    int real = 0;
    for (int i = 0; i < 6; ++i)
        if (jb.pop(20 + 20 * i)->real) ++real;
    EXPECT_EQ(real, 6);
    EXPECT_EQ(jb.stats().late, 0u);
}

TEST(JitterBuffer, PausesWhenDryAndReanchorsAfterHold) {
    // Regression: a 2 s hold used to leave playout 100 frames ahead of the
    // resumed stream, so every packet after the resume was discarded as late.
    JitterBuffer jb(60);
    uint8_t p[1] = {0};
    uint64_t t = 0;
    uint16_t seq = 1000;
    for (int i = 0; i < 50; ++i, t += 20) {  // 1 s of talk
        jb.push(seq++, p, 1, t);
        jb.pop(t);
    }
    for (int i = 0; i < 100; ++i, t += 20) jb.pop(t);  // 2 s hold: nothing arrives
    EXPECT_TRUE(jb.paused());
    EXPECT_EQ(jb.stats().concealed, uint64_t(JitterBuffer::kDryFrames));  // not 100
    const uint64_t played_before = jb.stats().played;
    for (int i = 0; i < 50; ++i, t += 20) {  // resume: seq continues where it left off
        jb.push(seq++, p, 1, t);
        jb.pop(t);
    }
    EXPECT_EQ(jb.stats().late, 0u);
    EXPECT_EQ(jb.stats().resyncs, 1u);
    EXPECT_GE(jb.stats().played - played_before, 46u);  // all but the 60 ms re-buffer
}

TEST(JitterBuffer, NewSsrcReanchorsAndStrayJumpIsDropped) {
    // Regression for the Asterisk capture: during hold Asterisk switched to a new
    // SSRC whose seq was ~40000 away, then back, with one stray packet carrying the
    // other stream's seq. Before the fix the buffer kept those packets far ahead of
    // the playout point and concealed every frame for the rest of the call.
    JitterBuffer jb(40);
    uint8_t p[1] = {0};
    uint64_t t = 0;
    uint16_t a = 56293, b = 16517;
    for (int i = 0; i < 20; ++i, t += 20) { jb.push(a++, p, 1, t, 0xA); jb.pop(t); }
    for (int i = 0; i < 20; ++i, t += 20) { jb.push(b++, p, 1, t, 0xB); jb.pop(t); }  // new source
    jb.push(a++, p, 1, t, 0xA);                                                      // back to A
    jb.push(b++, p, 1, t, 0xA);  // stray: A's SSRC, B's seq
    jb.pop(t);
    t += 20;
    for (int i = 0; i < 20; ++i, t += 20) { jb.push(a++, p, 1, t, 0xA); jb.pop(t); }
    for (int i = 0; i < 5; ++i, t += 20) jb.pop(t);  // drain
    EXPECT_EQ(jb.stats().ssrc_changes, 2u);
    EXPECT_EQ(jb.stats().jump_dropped, 1u);
    EXPECT_GE(jb.stats().played, 55u);  // 61 real packets; only the re-buffer gaps are lost
    EXPECT_EQ(jb.stats().late, 0u);
}

TEST(JitterBuffer, ConfirmedSequenceRestartReanchors) {
    JitterBuffer jb(20);
    uint8_t p[1] = {0};
    uint64_t t = 0;
    for (uint16_t s = 100; s < 110; ++s, t += 20) { jb.push(s, p, 1, t, 7); jb.pop(t); }
    for (uint16_t s = 40000; s < 40010; ++s, t += 20) { jb.push(s, p, 1, t, 7); jb.pop(t); }
    EXPECT_EQ(jb.stats().jump_dropped, 1u);  // first packet of the jump waits for confirmation
    EXPECT_EQ(jb.stats().resyncs, 1u);
    EXPECT_GE(jb.stats().played, 16u);
}

TEST(RtpStats, MarkerPacketWithTimestampJumpDoesNotPoisonJitter) {
    // Regression for the Asterisk capture: one packet with the marker bit carried a
    // timestamp 1,138,507,800 ticks away; J jumped to 8,894,580 ms.
    ReceiverStats s;
    for (int i = 0; i < 50; ++i) s.on_packet(static_cast<uint16_t>(i), i * 160, 1000 + i * 160);
    s.on_packet(50, 50 * 160 + 1138507800u, 1000 + 50 * 160, /*marker=*/true);
    for (int i = 51; i < 60; ++i) s.on_packet(static_cast<uint16_t>(i), i * 160 + 1138507800u, 1000 + i * 160);
    EXPECT_LT(s.jitter_ms(), 1.0);
}
