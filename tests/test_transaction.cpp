// Transaction-layer timers checked against RFC 3261 section 17 with a mock
// transport and a fake clock: exact retransmission schedules, absorption of
// duplicates, ACK generation for non-2xx, and the timeouts.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "sip/transaction.hpp"

using namespace sip;
using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::StartsWith;

namespace {

class MockTransport : public core::Transport {
public:
    MOCK_METHOD(void, send, (const std::string& data, const core::Endpoint& to), (override));
    core::Endpoint local() const override { return {"10.0.0.1", 5060}; }
};

const core::Endpoint kPeer{"10.0.0.2", 5060};

Message request(const std::string& method, const std::string& branch = "z9hG4bKtest") {
    Message m;
    m.method = method;
    m.uri = "sip:bob@10.0.0.2";
    m.add("Via", "SIP/2.0/UDP 10.0.0.1:5060;branch=" + branch);
    m.set("From", "<sip:alice@10.0.0.1>;tag=a1");
    m.set("To", "<sip:bob@10.0.0.2>");
    m.set("Call-ID", "cid-1");
    m.set("CSeq", "1 " + method);
    return m;
}

std::string response(const Message& req, int code) { return make_response(req, code, "b1").serialize(); }

struct Fixture : ::testing::Test {
    core::FakeTimers timers;
    NiceMock<MockTransport> tp;
    TransactionLayer layer{tp, timers};
    std::vector<uint64_t> send_times;
    int responses = 0, timeouts = 0, server_timeouts = 0, requests = 0, strays = 0;
    TxnId last_server = 0;

    void SetUp() override {
        TransactionLayer::Callbacks cb;
        cb.on_response = [this](TxnId, const Message&) { ++responses; };
        cb.on_timeout = [this](TxnId) { ++timeouts; };
        cb.on_server_timeout = [this](TxnId) { ++server_timeouts; };
        cb.on_request = [this](TxnId id, const Message&, const core::Endpoint&) { ++requests; last_server = id; };
        cb.on_stray_response = [this](const Message&, const core::Endpoint&) { ++strays; };
        layer.set_callbacks(cb);
    }
    void record(const std::string& prefix) {
        ON_CALL(tp, send(StartsWith(prefix), _)).WillByDefault(Invoke([this](const std::string&, const core::Endpoint&) {
            send_times.push_back(timers.now_ms());
        }));
    }
};

}  // namespace

TEST_F(Fixture, InviteClientTimerADoublesUntilTimerB) {
    record("INVITE");
    EXPECT_CALL(tp, send(StartsWith("INVITE"), kPeer)).Times(7);
    layer.send_request(request("INVITE"), kPeer);
    timers.advance(40000);
    // Timer A: T1, 2T1, 4T1 ... ; Timer B = 64*T1 = 32 s
    EXPECT_EQ(send_times, (std::vector<uint64_t>{0, 500, 1500, 3500, 7500, 15500, 31500}));
    EXPECT_EQ(timeouts, 1);
    EXPECT_EQ(layer.live_count(), 0u);
}

TEST_F(Fixture, ProvisionalStopsInviteRetransmissionAndTimerB) {
    EXPECT_CALL(tp, send(StartsWith("INVITE"), _)).Times(1);
    auto inv = request("INVITE");
    layer.send_request(inv, kPeer);
    layer.on_datagram(response(inv, 100), kPeer);
    timers.advance(60000);
    EXPECT_EQ(timeouts, 0);  // Proceeding waits for the TU to CANCEL; no Timer B
    EXPECT_EQ(layer.live_count(), 1u);
    timers.advance(120000);  // ...but not forever: Timer C guard at 180 s
    EXPECT_EQ(timeouts, 1);
    EXPECT_EQ(layer.live_count(), 0u);
}

TEST_F(Fixture, InviteClientAcksNon2xxAndAbsorbsRetransmits) {
    auto inv = request("INVITE");
    EXPECT_CALL(tp, send(StartsWith("INVITE"), _)).Times(1);
    EXPECT_CALL(tp, send(StartsWith("ACK sip:bob@10.0.0.2 SIP/2.0"), kPeer)).Times(2);
    auto id = layer.send_request(inv, kPeer);
    layer.on_datagram(response(inv, 486), kPeer);
    EXPECT_EQ(layer.state(id), TxnState::Completed);
    layer.on_datagram(response(inv, 486), kPeer);  // retransmitted final: ACK again, TU not told twice
    EXPECT_EQ(responses, 1);
    EXPECT_EQ(layer.stats().absorbed, 1u);
    timers.advance(32000);  // Timer D
    EXPECT_EQ(layer.live_count(), 0u);
}

TEST_F(Fixture, InviteClient2xxGoesToTuWithoutAck) {
    auto inv = request("INVITE");
    EXPECT_CALL(tp, send(StartsWith("INVITE"), _)).Times(1);
    EXPECT_CALL(tp, send(StartsWith("ACK"), _)).Times(0);  // ACK for 2xx is the UA core's job
    layer.send_request(inv, kPeer);
    layer.on_datagram(response(inv, 200), kPeer);
    EXPECT_EQ(responses, 1);
    EXPECT_EQ(layer.live_count(), 0u);
    layer.on_datagram(response(inv, 200), kPeer);  // retransmitted 2xx -> stray, so the UA can re-ACK
    EXPECT_EQ(strays, 1);
}

TEST_F(Fixture, NonInviteTimerECapsAtT2) {
    record("BYE");
    layer.send_request(request("BYE"), kPeer);
    timers.advance(40000);
    EXPECT_EQ(send_times, (std::vector<uint64_t>{0, 500, 1500, 3500, 7500, 11500, 15500, 19500, 23500, 27500, 31500}));
    EXPECT_EQ(timeouts, 1);  // Timer F
}

TEST_F(Fixture, NonInviteProceedingRetransmitsAtT2) {
    record("BYE");
    auto bye = request("BYE");
    layer.send_request(bye, kPeer);
    timers.advance(600);  // one retransmission at 500
    layer.on_datagram(response(bye, 100), kPeer);
    timers.advance(10000);
    // after the 1xx, next retransmission is at 500+1000 (already armed), then every T2
    EXPECT_EQ(send_times, (std::vector<uint64_t>{0, 500, 1500, 5500, 9500}));
}

TEST_F(Fixture, NonInviteFinalThenTimerK) {
    auto bye = request("BYE");
    auto id = layer.send_request(bye, kPeer);
    layer.on_datagram(response(bye, 200), kPeer);
    EXPECT_EQ(layer.state(id), TxnState::Completed);
    layer.on_datagram(response(bye, 200), kPeer);
    EXPECT_EQ(responses, 1);
    timers.advance(5000);  // Timer K = T4
    EXPECT_FALSE(layer.alive(id));
}

TEST_F(Fixture, InviteServerSends100AndReplaysOnRetransmit) {
    auto inv = request("INVITE");
    EXPECT_CALL(tp, send(StartsWith("SIP/2.0 100"), _)).Times(2);
    layer.on_datagram(inv.serialize(), kPeer);
    layer.on_datagram(inv.serialize(), kPeer);  // retransmission is absorbed, 100 replayed
    EXPECT_EQ(requests, 1);
}

TEST_F(Fixture, InviteServerTimerGUntilAck) {
    auto inv = request("INVITE");
    layer.on_datagram(inv.serialize(), kPeer);
    record("SIP/2.0 486");
    layer.respond(last_server, make_response(inv, 486, "b1"));
    timers.advance(9000);
    // Timer G: T1, 2T1, 4T1, then capped at T2
    EXPECT_EQ(send_times, (std::vector<uint64_t>{0, 500, 1500, 3500, 7500}));
    Message ack = request("ACK");
    ack.set("To", "<sip:bob@10.0.0.2>;tag=b1");
    ack.set("CSeq", "1 ACK");
    layer.on_datagram(ack.serialize(), kPeer);
    EXPECT_EQ(layer.state(last_server), TxnState::Confirmed);
    timers.advance(20000);
    EXPECT_EQ(send_times.size(), 5u);  // no more retransmissions after the ACK
    EXPECT_EQ(server_timeouts, 0);
    EXPECT_EQ(layer.live_count(), 0u);  // Timer I
}

TEST_F(Fixture, InviteServerTimerHWithoutAck) {
    auto inv = request("INVITE");
    layer.on_datagram(inv.serialize(), kPeer);
    layer.respond(last_server, make_response(inv, 486, "b1"));
    timers.advance(32000);
    EXPECT_EQ(server_timeouts, 1);
}

TEST_F(Fixture, NonInviteServerReplaysFinal) {
    auto bye = request("BYE");
    EXPECT_CALL(tp, send(StartsWith("SIP/2.0 200"), _)).Times(2);
    layer.on_datagram(bye.serialize(), kPeer);
    layer.respond(last_server, make_response(bye, 200, "b1"));
    layer.on_datagram(bye.serialize(), kPeer);
    EXPECT_EQ(requests, 1);
    timers.advance(32000);  // Timer J
    EXPECT_EQ(layer.live_count(), 0u);
}

TEST_F(Fixture, CancelIsItsOwnTransactionWithTheInviteBranch) {
    auto inv = request("INVITE", "z9hG4bKsame");
    auto cancel = request("CANCEL", "z9hG4bKsame");
    layer.on_datagram(inv.serialize(), kPeer);
    TxnId ist = last_server;
    layer.on_datagram(cancel.serialize(), kPeer);
    EXPECT_NE(last_server, ist);
    EXPECT_EQ(requests, 2);
    EXPECT_EQ(layer.find_invite_server("z9hG4bKsame"), ist);
}

TEST_F(Fixture, GarbageIsCountedNotFatal) {
    layer.on_datagram("hello", kPeer);
    layer.on_datagram(std::string(70000, 'x'), kPeer);
    EXPECT_EQ(layer.stats().parse_errors, 2u);
}
