#include <gtest/gtest.h>

#include "sip/message.hpp"

using namespace sip;

namespace {
const char* kInvite =
    "INVITE sip:bob@10.0.0.2:5060 SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bKabc;rport\r\n"
    "Max-Forwards: 70\r\n"
    "From: \"Alice\" <sip:alice@10.0.0.1>;tag=1928\r\n"
    "To: <sip:bob@10.0.0.2>\r\n"
    "Call-ID: a84b4c76e66710\r\n"
    "CSeq: 314159 INVITE\r\n"
    "Contact: <sip:alice@10.0.0.1:5060>\r\n"
    "Content-Type: application/sdp\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "v=0\n";
}

TEST(Message, ParsesRequest) {
    std::string err;
    auto m = parse(kInvite, &err);
    ASSERT_TRUE(m) << err;
    EXPECT_TRUE(m->is_request);
    EXPECT_EQ(m->method, "INVITE");
    EXPECT_EQ(m->uri, "sip:bob@10.0.0.2:5060");
    EXPECT_EQ(m->call_id(), "a84b4c76e66710");
    EXPECT_EQ(m->cseq_num(), 314159u);
    EXPECT_EQ(m->cseq_method(), "INVITE");
    EXPECT_EQ(m->branch(), "z9hG4bKabc");
    EXPECT_EQ(m->from_tag(), "1928");
    EXPECT_EQ(m->to_tag(), "");
    EXPECT_EQ(m->body, "v=0\n");
}

TEST(Message, CompactFormsAndCaseInsensitive) {
    std::string s =
        "SIP/2.0 180 Ringing\r\n"
        "v: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
        "f: <sip:a@h>;tag=x\r\n"
        "t: <sip:b@h>;tag=y\r\n"
        "i: cid\r\n"
        "cseq: 1 INVITE\r\n"
        "l: 0\r\n\r\n";
    auto m = parse(s);
    ASSERT_TRUE(m);
    EXPECT_FALSE(m->is_request);
    EXPECT_EQ(m->status, 180);
    EXPECT_EQ(m->call_id(), "cid");
    EXPECT_EQ(m->to_tag(), "y");
    EXPECT_NE(m->get("CALL-ID"), nullptr);
}

TEST(Message, HeaderFolding) {
    std::string s =
        "OPTIONS sip:b@h SIP/2.0\r\n"
        "Via: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
        "From: <sip:a@h>\r\n"
        " ;tag=folded\r\n"
        "To: <sip:b@h>\r\nCall-ID: c\r\nCSeq: 1 OPTIONS\r\n\r\n";
    auto m = parse(s);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->from_tag(), "folded");
}

TEST(Message, FoldOntoEmptyValueHasNoLeadingSpace) {
    // Regression (fuzz_sip_parser): the value starts on the continuation line.
    // Before the fix the Call-ID parsed as " abc", so the dialog lookup failed.
    std::string s =
        "BYE sip:b@h SIP/2.0\r\n"
        "Via: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
        "From: <sip:a@h>;tag=1\r\nTo: <sip:b@h>;tag=2\r\n"
        "Call-ID:\r\n abc\r\n"
        "CSeq: 2 BYE\r\n\r\n";
    auto m = parse(s);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->call_id(), "abc");
    EXPECT_EQ(parse(m->serialize())->serialize(), m->serialize());
}

TEST(Message, WhitespaceFoldAndControlCharacters) {
    // Regressions (fuzz_sip_parser, second run): a whitespace-only continuation
    // line used to add a trailing space; raw NULs inside a value were accepted.
    std::string base =
        "BYE sip:b@h SIP/2.0\r\n"
        "Via: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
        "From: <sip:a@h>;tag=1\r\nTo: <sip:b@h>;tag=2\r\nCall-ID: abc\r\n"
        "CSeq: 2 BYE\r\n \r\n\r\n";
    auto m = parse(base);
    ASSERT_TRUE(m);
    EXPECT_EQ(*m->get("CSeq"), "2 BYE");
    std::string nul = base;
    nul.replace(nul.find("abc"), 3, std::string("a\0c", 3));
    std::string err;
    EXPECT_FALSE(parse(nul, &err));
    EXPECT_EQ(err, "control character in header");
}

TEST(Message, HeaderLimitLeavesRoomForContentLengthAndRejectsDuplicates) {
    // Regression (fuzz_sip_parser, third run): 128 headers parsed, but serialize()
    // added Content-Length -> 129 -> our own parser rejected our own output.
    std::string s = "OPTIONS sip:b@h SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
                    "From: <sip:a@h>;tag=1\r\nTo: <sip:b@h>\r\nCall-ID: c\r\nCSeq: 1 OPTIONS\r\n";
    for (int i = 0; i < 200; ++i) s += "X-Pad: " + std::to_string(i) + "\r\n";
    s += "\r\n";
    std::string err;
    EXPECT_FALSE(parse(s, &err));
    EXPECT_EQ(err, "too many headers");
    std::string two = "OPTIONS sip:b@h SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
                      "From: <sip:a@h>;tag=1\r\nTo: <sip:b@h>\r\nCall-ID: c\r\nCSeq: 1 OPTIONS\r\n"
                      "Content-Length: 0\r\nl: 4\r\n\r\nabcd";
    EXPECT_FALSE(parse(two, &err));
    EXPECT_EQ(err, "duplicate Content-Length");
}

TEST(Message, RejectsMalformed) {
    std::string base(kInvite);
    auto without = [&](const std::string& hdr) {
        std::string s = base;
        auto p = s.find(hdr);
        s.erase(p, s.find("\r\n", p) + 2 - p);
        return s;
    };
    for (const char* h : {"Via:", "From:", "To:", "Call-ID:", "CSeq:"}) {
        std::string err;
        EXPECT_FALSE(parse(without(h), &err)) << h;
        EXPECT_EQ(err, "missing mandatory header");
    }
    std::string err;
    std::string mismatch = base;
    mismatch.replace(mismatch.find("314159 INVITE"), 13, "314159 BYE");
    EXPECT_FALSE(parse(mismatch, &err));
    EXPECT_EQ(err, "CSeq method mismatch");

    std::string short_body = base;
    short_body.replace(short_body.find("Content-Length: 4"), 17, "Content-Length: 40");
    EXPECT_FALSE(parse(short_body, &err));

    EXPECT_FALSE(parse("SIP/2.0 99 Low\r\nVia: x\r\n\r\n"));
    EXPECT_FALSE(parse("INVITE sip:x SIP/3.0\r\n\r\n"));
    EXPECT_FALSE(parse("garbage"));
}

TEST(Message, IgnoresBytesBeyondContentLength) {
    std::string s = std::string(kInvite) + "trailing junk";
    auto m = parse(s);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->body, "v=0\n");
}

TEST(Message, RoundTrip) {
    auto m = parse(kInvite);
    ASSERT_TRUE(m);
    auto again = parse(m->serialize());
    ASSERT_TRUE(again);
    EXPECT_EQ(again->serialize(), m->serialize());
}

TEST(Message, ParamsIgnoreUriParams) {
    // ;tag inside <...> belongs to the URI, not the header
    EXPECT_EQ(header_param("<sip:a@h;tag=wrong>;tag=right", "tag"), "right");
    EXPECT_EQ(header_param("\"x;tag=q\" <sip:a@h>", "tag"), "");
    EXPECT_EQ(header_param("SIP/2.0/UDP h;branch=z9hG4bKx;rport", "branch"), "z9hG4bKx");
    EXPECT_EQ(header_param("SIP/2.0/UDP h;branch=z9hG4bKx;rport", "rport"), "");
}

TEST(Message, UriHelpers) {
    auto hp = uri_hostport("sip:bob@10.0.0.2:5062;transport=udp");
    ASSERT_TRUE(hp);
    EXPECT_EQ(hp->host, "10.0.0.2");
    EXPECT_EQ(hp->port, 5062);
    EXPECT_EQ(uri_hostport("sip:10.0.0.9")->port, 5060);
    EXPECT_FALSE(uri_hostport("tel:+123"));
    EXPECT_FALSE(uri_hostport("sip:bob@h:99999"));
    EXPECT_EQ(addr_uri("\"Bob\" <sip:bob@h:5060>;tag=1"), "sip:bob@h:5060");
    EXPECT_EQ(addr_uri("sip:bob@h;tag=1"), "sip:bob@h");
    EXPECT_EQ(uri_user("sip:bob@h"), "bob");
}

TEST(Message, BranchHasMagicCookie) {
    EXPECT_EQ(new_branch().rfind("z9hG4bK", 0), 0u);
    EXPECT_NE(new_branch(), new_branch());
}
