#include <gtest/gtest.h>

#include "sip/sdp.hpp"

using namespace sdp;

TEST(Sdp, BuildParseRoundTrip) {
    Session s;
    s.ip = "10.0.0.1";
    s.port = 40002;
    s.payloads = {0};
    s.dir = Dir::SendOnly;
    auto p = parse(build(s, "alice"));
    ASSERT_TRUE(p);
    EXPECT_EQ(p->ip, "10.0.0.1");
    EXPECT_EQ(p->port, 40002);
    EXPECT_EQ(p->payloads, std::vector<int>{0});
    EXPECT_EQ(p->dir, Dir::SendOnly);
}

TEST(Sdp, DefaultsToSendRecv) {
    auto p = parse("v=0\r\nc=IN IP4 1.2.3.4\r\nm=audio 5000 RTP/AVP 0 8\r\n");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->dir, Dir::SendRecv);
    EXPECT_EQ(p->payloads.size(), 2u);
}

TEST(Sdp, LegacyHoldAddress) {
    // RFC 2543 style hold: c=0.0.0.0 with no direction attribute
    auto p = parse("v=0\r\nc=IN IP4 0.0.0.0\r\nm=audio 5000 RTP/AVP 0\r\n");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->dir, Dir::SendOnly);
}

TEST(Sdp, RejectsWithoutMediaOrConnection) {
    EXPECT_FALSE(parse("v=0\r\nc=IN IP4 1.2.3.4\r\n"));
    EXPECT_FALSE(parse("v=0\r\nm=audio 5000 RTP/AVP 0\r\n"));
    EXPECT_FALSE(parse("v=0\r\nc=IN IP4 1.2.3.4\r\nm=audio 99999 RTP/AVP 0\r\n"));
}

TEST(Sdp, AnswerDirectionTable) {
    // RFC 3264 6.1, us willing to do anything
    EXPECT_EQ(answer_dir(Dir::SendRecv, Dir::SendRecv), Dir::SendRecv);
    EXPECT_EQ(answer_dir(Dir::SendOnly, Dir::SendRecv), Dir::RecvOnly);  // they hold us
    EXPECT_EQ(answer_dir(Dir::RecvOnly, Dir::SendRecv), Dir::SendOnly);
    EXPECT_EQ(answer_dir(Dir::Inactive, Dir::SendRecv), Dir::Inactive);
    // we are holding them, they offer sendrecv -> we only send
    EXPECT_EQ(answer_dir(Dir::SendRecv, Dir::SendOnly), Dir::SendOnly);
    // both sides holding -> nothing flows
    EXPECT_EQ(answer_dir(Dir::SendOnly, Dir::SendOnly), Dir::Inactive);
}
