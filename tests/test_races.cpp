// Call-flow and race-condition tests: real UserAgents talking over SimNetwork
// with a shared fake clock. Each test sets up a collision that happens on real
// telephony networks and checks the end state on BOTH sides plus the invariant
// that no illegal state transition was ever attempted.
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>

#include "sim_network.hpp"
#include "ua/user_agent.hpp"

using namespace ua;
using core::Endpoint;
using test::SimNetwork;

namespace {

struct Party {
    SimNetwork::Node& node;
    UserAgent ua;
    Party(SimNetwork& net, core::FakeTimers& t, const std::string& name, Endpoint ep, Config cfg)
        : node(net.add(name, ep)), ua(node, t, [&] { cfg.user = name; return cfg; }()) {
        node.rx = [this](const std::string& d, const Endpoint& s) { ua.on_datagram(d, s); };
    }
};

struct Races : ::testing::Test {
    core::FakeTimers t;
    SimNetwork net{t};
    std::unique_ptr<Party> alice, bob, carol;

    void make(uint64_t seed = 1, bool bob_auto = false, bool carol_auto = true) {
        Config a, b, c;
        a.seed = seed;
        b.seed = seed * 7919 + 1;
        b.auto_answer = bob_auto;
        c.auto_answer = carol_auto;
        alice = std::make_unique<Party>(net, t, "alice", Endpoint{"10.0.0.1", 5060}, a);
        bob = std::make_unique<Party>(net, t, "bob", Endpoint{"10.0.0.2", 5060}, b);
        carol = std::make_unique<Party>(net, t, "carol", Endpoint{"10.0.0.3", 5060}, c);
    }
    static CallState st(Party& p, int id) { return p.ua.info(id)->state; }
    int bob_call() const { return bob->ua.calls().empty() ? 0 : bob->ua.calls().back().id; }

    // alice calls bob; bob answers; returns {alice_id, bob_id} once both are Connected.
    std::pair<int, int> connect() {
        int a = alice->ua.dial("sip:bob@10.0.0.2:5060");
        t.advance(100);  // INVITE, 100, 180 have crossed
        int b = bob_call();
        EXPECT_EQ(st(*bob, b), CallState::Ringing);
        EXPECT_EQ(st(*alice, a), CallState::Ringing);
        EXPECT_TRUE(bob->ua.answer(b));
        t.advance(100);  // 200 and ACK
        EXPECT_EQ(st(*alice, a), CallState::Connected);
        EXPECT_EQ(st(*bob, b), CallState::Connected);
        return {a, b};
    }
    void TearDown() override {
        if (!alice) return;
        EXPECT_EQ(alice->ua.illegal_transitions(), 0u);
        EXPECT_EQ(bob->ua.illegal_transitions(), 0u);
        EXPECT_EQ(carol->ua.illegal_transitions(), 0u);
        if (HasFailure() || std::getenv("TU_LADDER"))  // TU_LADDER=1 prints the ladder for docs
            for (auto& l : net.ladder) std::printf("    %s\n", l.c_str());
    }
};

}  // namespace

TEST_F(Races, BasicCallHoldResumeHangup) {
    make();
    auto [a, b] = connect();
    ASSERT_TRUE(alice->ua.hold(a));
    t.advance(100);
    EXPECT_EQ(st(*alice, a), CallState::OnHold);
    EXPECT_TRUE(bob->ua.info(b)->remote_hold);
    ASSERT_TRUE(alice->ua.resume(a));
    t.advance(100);
    EXPECT_EQ(st(*alice, a), CallState::Connected);
    EXPECT_FALSE(bob->ua.info(b)->remote_hold);
    ASSERT_TRUE(alice->ua.hangup(a));
    t.advance(100);
    EXPECT_EQ(st(*bob, b), CallState::Terminated);
    EXPECT_EQ(bob->ua.info(b)->reason, "remote hangup");
    EXPECT_EQ(alice->ua.history(a),
              (std::vector<std::string>{"Idle->Calling", "Calling->Ringing", "Ringing->Connected",
                                        "Connected->OnHold", "OnHold->Connected", "Connected->Terminated"}));
    t.advance(70000);  // every transaction and lingering call is gone
    EXPECT_EQ(alice->ua.txn().live_count(), 0u);
    EXPECT_EQ(bob->ua.txn().live_count(), 0u);
    EXPECT_TRUE(alice->ua.calls().empty());
}

TEST_F(Races, CancelBeforeAnswerGets487) {
    make();
    int a = alice->ua.dial("sip:bob@10.0.0.2:5060");
    t.advance(100);
    int b = bob_call();
    ASSERT_TRUE(alice->ua.hangup(a));
    t.advance(100);
    EXPECT_EQ(st(*bob, b), CallState::Terminated);
    EXPECT_EQ(bob->ua.info(b)->reason, "cancelled by caller");
    EXPECT_EQ(net.count("487 INVITE", "bob", "alice"), 1);
    EXPECT_EQ(net.count("ACK", "alice", "bob"), 1);  // ACK for the 487, built by the transaction layer
    EXPECT_EQ(net.count("BYE"), 0);
}

TEST_F(Races, CancelBeforeAnyProvisionalIsDeferred) {
    make();
    // Lose the 100 and 180 once, so alice hangs up before any provisional response.
    int lost = 0;
    net.drop = [&](const sip::Message& m, const std::string&, const std::string&) {
        return !m.is_request && m.status < 200 && lost++ < 2;
    };
    int a = alice->ua.dial("sip:bob@10.0.0.2:5060");
    t.advance(100);
    ASSERT_TRUE(alice->ua.hangup(a));
    EXPECT_EQ(net.count("CANCEL"), 0);  // RFC 3261 9.1: not allowed yet
    t.advance(1000);                    // INVITE retransmit -> 100 replayed -> CANCEL goes out
    EXPECT_EQ(net.count("CANCEL"), 1);
    t.advance(200);
    EXPECT_EQ(st(*bob, bob_call()), CallState::Terminated);
}

TEST_F(Races, CancelCrossesTwoHundredOk) {
    make();
    int a = alice->ua.dial("sip:bob@10.0.0.2:5060");
    t.advance(100);
    int b = bob_call();
    // Same instant: bob answers, alice hangs up. The 200 and the CANCEL cross on the wire.
    ASSERT_TRUE(bob->ua.answer(b));
    ASSERT_TRUE(alice->ua.hangup(a));
    t.advance(500);
    // bob got the CANCEL after answering: 200 to the CANCEL, no 487
    EXPECT_EQ(net.count("200 CANCEL", "bob"), 1);
    EXPECT_EQ(net.count("487 INVITE"), 0);
    // alice got a 200 for a call she already hung up: ACK it, then BYE it
    EXPECT_EQ(net.count("ACK", "alice"), 1);
    EXPECT_EQ(net.count("BYE", "alice"), 1);
    EXPECT_EQ(alice->ua.stats().cancel_lost_race, 1u);
    EXPECT_EQ(st(*alice, a), CallState::Terminated);
    EXPECT_EQ(st(*bob, b), CallState::Terminated);
    EXPECT_EQ(bob->ua.info(b)->reason, "remote hangup");
    EXPECT_EQ(bob->ua.stats().ok_retransmits, 0u);  // the ACK did stop bob's 2xx timer
}

TEST_F(Races, ByeCrossesReInvite) {
    make();
    auto [a, b] = connect();
    // alice puts bob on hold at the same instant bob hangs up.
    ASSERT_TRUE(alice->ua.hold(a));
    ASSERT_TRUE(bob->ua.hangup(b));
    t.advance(500);
    EXPECT_EQ(st(*alice, a), CallState::Terminated);
    EXPECT_EQ(alice->ua.info(a)->reason, "remote hangup");  // not "dialog gone": the BYE won
    EXPECT_EQ(st(*bob, b), CallState::Terminated);
    EXPECT_EQ(net.count("481 INVITE", "bob", "alice"), 1);  // bob's dialog was already gone
    EXPECT_EQ(net.count("200 BYE", "alice", "bob"), 1);
    // nothing resurrected alice's call and nothing retransmits forever
    t.advance(70000);
    EXPECT_EQ(alice->ua.txn().live_count(), 0u);
    EXPECT_EQ(bob->ua.txn().live_count(), 0u);
}

TEST_F(Races, HangupRightAfterReInviteWithLost200) {
    // Regression for chaos seed 7: alice sends a re-INVITE and hangs up at once;
    // bob's 200 to the re-INVITE is lost and bob then gets the BYE. Bob must keep
    // retransmitting that 200 (RFC 3261 13.3.1.4) so alice's re-INVITE client
    // transaction - stuck in Proceeding after the 100 - can complete.
    make();
    auto [a, b] = connect();
    int lost = 0;
    net.drop = [&](const sip::Message& m, const std::string& f, const std::string&) {
        return !m.is_request && m.status == 200 && m.cseq_method() == "INVITE" && f == "bob" && lost++ == 0;
    };
    ASSERT_TRUE(alice->ua.hold(a));
    ASSERT_TRUE(alice->ua.hangup(a));
    t.advance(1000);
    EXPECT_EQ(st(*bob, b), CallState::Terminated);
    EXPECT_GE(net.count("200 INVITE", "bob", "alice"), 3);  // initial + lost one + retransmission
    t.advance(6000);  // past Timer K for the BYE
    EXPECT_EQ(alice->ua.txn().live_count(), 0u);  // the re-INVITE transaction got its final response
    EXPECT_EQ(bob->ua.stats().ok_retransmits, 1u);  // and bob stopped once the ACK arrived
}

TEST_F(Races, ReInviteGlareResolvesWith491AndBackoff) {
    make();
    auto [a, b] = connect();
    ASSERT_TRUE(alice->ua.hold(a));
    ASSERT_TRUE(bob->ua.hold(b));
    t.advance(100);
    EXPECT_EQ(net.count("491 INVITE", "alice", "bob"), 1);
    EXPECT_EQ(net.count("491 INVITE", "bob", "alice"), 1);
    EXPECT_EQ(st(*alice, a), CallState::Connected);
    t.advance(5000);  // bob (not Call-ID owner) retries in 0-2 s, alice in 2.1-4 s
    EXPECT_EQ(st(*alice, a), CallState::OnHold);
    EXPECT_EQ(st(*bob, b), CallState::OnHold);
    EXPECT_EQ(alice->ua.stats().glare_retries + bob->ua.stats().glare_retries, 2u);
}

TEST_F(Races, GlareConvergesForEverySeed) {
    // The back-off windows do not overlap, so a second collision is impossible.
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        core::FakeTimers tt;
        SimNetwork n{tt};
        Config ca, cb;
        ca.seed = seed;
        cb.seed = seed * 31 + 7;
        Party pa(n, tt, "alice", {"10.0.0.1", 5060}, ca);
        Party pb(n, tt, "bob", {"10.0.0.2", 5060}, cb);
        int a = pa.ua.dial("sip:bob@10.0.0.2:5060");
        tt.advance(100);
        int b = pb.ua.calls().back().id;
        pb.ua.answer(b);
        tt.advance(100);
        pa.ua.hold(a);
        pb.ua.hold(b);
        tt.advance(10000);
        ASSERT_EQ(pa.ua.info(a)->state, CallState::OnHold) << "seed " << seed;
        ASSERT_EQ(pb.ua.info(b)->state, CallState::OnHold) << "seed " << seed;
        ASSERT_EQ(n.count("491 INVITE"), 2) << "seed " << seed;
        ASSERT_EQ(pa.ua.illegal_transitions() + pb.ua.illegal_transitions(), 0u);
    }
}

TEST_F(Races, ReferWhileOnHold) {
    make();
    auto [a, b] = connect();
    ASSERT_TRUE(alice->ua.hold(a));
    t.advance(100);
    ASSERT_EQ(st(*alice, a), CallState::OnHold);
    ASSERT_TRUE(alice->ua.transfer(a, "sip:carol@10.0.0.3:5060"));
    t.advance(1000);
    // alice: OnHold -> Transferring -> Terminated (BYE after the NOTIFY 200)
    EXPECT_EQ(st(*alice, a), CallState::Terminated);
    EXPECT_EQ(alice->ua.info(a)->reason, "transferred");
    EXPECT_EQ(alice->ua.stats().transfers_done, 1u);
    // bob: original call ended by alice, new call to carol is up
    EXPECT_EQ(st(*bob, b), CallState::Terminated);
    auto calls = bob->ua.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1].state, CallState::Connected);
    EXPECT_EQ(calls[1].referred_from, b);
    EXPECT_EQ(carol->ua.calls().back().state, CallState::Connected);
    EXPECT_EQ(net.count("202 REFER", "bob", "alice"), 1);
    EXPECT_EQ(net.count("NOTIFY", "bob", "alice"), 2);  // 100 Trying, then 200 OK
}

TEST_F(Races, ReferRacesItsOwnHoldReInvite) {
    // The operator hits HOLD then TRANSFER faster than one round trip:
    // the REFER is sent while the hold re-INVITE is still in flight.
    make();
    auto [a, b] = connect();
    ASSERT_TRUE(alice->ua.hold(a));
    t.advance(5);
    ASSERT_EQ(st(*alice, a), CallState::Connected);  // hold not confirmed yet
    ASSERT_TRUE(alice->ua.transfer(a, "sip:carol@10.0.0.3:5060"));
    t.advance(1000);
    EXPECT_EQ(st(*alice, a), CallState::Terminated);
    EXPECT_EQ(alice->ua.info(a)->reason, "transferred");
    auto h = alice->ua.history(a);
    EXPECT_EQ(h, (std::vector<std::string>{"Idle->Calling", "Calling->Ringing", "Ringing->Connected",
                                           "Connected->Transferring", "Transferring->Terminated"}));
    EXPECT_EQ(bob->ua.calls().back().state, CallState::Connected);
}

TEST_F(Races, TransferToBusyTargetFallsBackToHold) {
    make(1, false, /*carol_auto=*/false);
    auto [a, b] = connect();
    alice->ua.hold(a);
    t.advance(100);
    alice->ua.transfer(a, "sip:carol@10.0.0.3:5060");
    t.advance(100);
    int c = carol->ua.calls().back().id;
    carol->ua.hangup(c);  // carol declines (603)
    t.advance(500);
    EXPECT_EQ(st(*alice, a), CallState::OnHold);
    EXPECT_EQ(alice->ua.info(a)->reason, "transfer failed (603)");
    EXPECT_EQ(st(*bob, b), CallState::Connected);  // bob still on the original (held) call
}

TEST_F(Races, LostAckIsRecoveredBy2xxRetransmission) {
    make();
    int dropped = 0;
    net.drop = [&](const sip::Message& m, const std::string& f, const std::string&) {
        return m.is_request && m.method == "ACK" && f == "alice" && dropped++ == 0;
    };
    auto [a, b] = connect();
    t.advance(2000);
    EXPECT_GE(bob->ua.stats().ok_retransmits, 1u);
    EXPECT_EQ(alice->ua.stats().ack_resends, bob->ua.stats().ok_retransmits);
    EXPECT_EQ(st(*bob, b), CallState::Connected);
    t.advance(40000);
    EXPECT_EQ(st(*bob, b), CallState::Connected);  // retransmissions stopped, no "no ACK" BYE
    (void)a;
}

TEST_F(Races, NoAckAtAllEndsCallAfter64T1) {
    make();
    net.drop = [](const sip::Message& m, const std::string& f, const std::string&) {
        return m.is_request && m.method == "ACK" && f == "alice";
    };
    int a = alice->ua.dial("sip:bob@10.0.0.2:5060");
    t.advance(100);
    int b = bob_call();
    bob->ua.answer(b);
    t.advance(31000);
    EXPECT_EQ(st(*bob, b), CallState::Connected);
    t.advance(2000);
    EXPECT_EQ(st(*bob, b), CallState::Terminated);
    EXPECT_EQ(bob->ua.info(b)->reason, "no ACK");
    EXPECT_EQ(st(*alice, a), CallState::Terminated);
}

TEST_F(Races, StaleInDialogRequestGets481) {
    make();
    auto [a, b] = connect();
    alice->ua.hangup(a);
    t.advance(40000);  // linger expires on both sides: the dialog is forgotten
    EXPECT_TRUE(bob->ua.calls().empty());
    sip::Message bye;
    bye.method = "BYE";
    bye.uri = "sip:bob@10.0.0.2:5060";
    bye.add("Via", "SIP/2.0/UDP 10.0.0.1:5060;branch=" + sip::new_branch());
    bye.set("From", "<sip:alice@10.0.0.1>;tag=old");
    bye.set("To", "<sip:bob@10.0.0.2>;tag=gone");
    bye.set("Call-ID", "stale@10.0.0.1");
    bye.set("CSeq", "9 BYE");
    alice->node.send(bye.serialize(), {"10.0.0.2", 5060});
    t.advance(100);
    EXPECT_EQ(net.count("481 BYE", "bob", "alice"), 1);
    (void)b;
}

TEST_F(Races, RegisterWithDigestChallenge) {
    // A tiny registrar on the sim network: challenge once, then accept a correct digest.
    Config cfg;
    cfg.registrar = "sip:10.0.0.9:5060";
    cfg.creds = sip::Credentials{"alice", "s3cret"};
    alice = std::make_unique<Party>(net, t, "alice", Endpoint{"10.0.0.1", 5060}, cfg);
    auto& reg = net.add("registrar", {"10.0.0.9", 5060});
    bool ok = false;
    int challenges = 0;
    reg.rx = [&](const std::string& d, const Endpoint& src) {
        auto m = sip::parse(d);
        ASSERT_TRUE(m);
        auto* auth = m->get("Authorization");
        sip::Message r;
        if (!auth) {
            ++challenges;
            r = sip::make_response(*m, 401, "r1");
            r.set("WWW-Authenticate", "Digest realm=\"turret\", nonce=\"n0nce\", qop=\"auth\", algorithm=MD5");
        } else {
            // recompute with the client's cnonce/nc and compare
            auto pos = auth->find("cnonce=\"");
            std::string cn = auth->substr(pos + 8, auth->find('"', pos + 8) - pos - 8);
            std::string expect = sip::digest_authorization(
                "Digest realm=\"turret\", nonce=\"n0nce\", qop=\"auth\"", {"alice", "s3cret"}, "REGISTER",
                m->uri, 1, cn);
            bool good = expect == *auth;
            r = sip::make_response(*m, good ? 200 : 403, "r1");
            r.set("Contact", *m->get("Contact") + ";expires=60");
        }
        reg.send(r.serialize(), src);
    };
    alice->ua.on_register = [&](bool success, int) { ok = success; };
    alice->ua.register_now();
    t.advance(200);
    EXPECT_TRUE(ok);
    EXPECT_EQ(challenges, 1);
    t.advance(31000);  // refresh at expires/2
    EXPECT_EQ(challenges, 2);
    EXPECT_TRUE(alice->ua.registered());
    bob = std::make_unique<Party>(net, t, "bob", Endpoint{"10.0.0.2", 5060}, Config{});
    carol = std::make_unique<Party>(net, t, "carol", Endpoint{"10.0.0.3", 5060}, Config{});
}

// Property test: random loss on every message, random user actions. Whatever
// happens, (1) no illegal transition, (2) every call ends Terminated on both
// sides after hangup + timers, (3) no transaction outlives its timers.
TEST_F(Races, RandomLossChaos) {
    int connected_runs = 0;
    for (uint64_t seed = 1; seed <= 300; ++seed) {
        core::FakeTimers tt;
        SimNetwork n{tt};
        n.rng.seed(static_cast<unsigned>(seed));
        n.loss = 0.05 + 0.25 * (seed % 5) / 4.0;  // 5% .. 30%
        Config ca, cb;
        ca.seed = seed;
        cb.seed = seed + 1000;
        cb.auto_answer = true;
        Party pa(n, tt, "alice", {"10.0.0.1", 5060}, ca);
        Party pb(n, tt, "bob", {"10.0.0.2", 5060}, cb);
        std::mt19937 r(static_cast<unsigned>(seed));
        int a = pa.ua.dial("sip:bob@10.0.0.2:5060");
        for (int step = 0; step < 12; ++step) {
            tt.advance(100 + r() % 3000);
            int b = pb.ua.calls().empty() ? 0 : pb.ua.calls().back().id;
            switch (r() % 5) {
                case 0: pa.ua.hold(a); break;
                case 1: pa.ua.resume(a); break;
                case 2: if (b) pb.ua.hold(b); break;
                case 3: if (b) pb.ua.resume(b); break;
                default: break;
            }
        }
        if (pa.ua.info(a)->state != CallState::Terminated) ++connected_runs;
        pa.ua.hangup(a);
        tt.advance(200000);  // > 64*T1 for every transaction + linger
        ASSERT_EQ(pa.ua.illegal_transitions(), 0u) << "seed " << seed;
        ASSERT_EQ(pb.ua.illegal_transitions(), 0u) << "seed " << seed;
        if (pa.ua.txn().live_count() || pb.ua.txn().live_count()) {
            for (auto& d : pa.ua.txn().dump()) std::printf("    alice txn: %s\n", d.c_str());
            for (auto& d : pb.ua.txn().dump()) std::printf("    bob txn: %s\n", d.c_str());
            for (auto& l : n.ladder) std::printf("    %s\n", l.c_str());
        }
        ASSERT_EQ(pa.ua.txn().live_count(), 0u) << "seed " << seed;
        ASSERT_EQ(pb.ua.txn().live_count(), 0u) << "seed " << seed;
        ASSERT_TRUE(pa.ua.calls().empty()) << "seed " << seed;
        ASSERT_TRUE(pb.ua.calls().empty()) << "seed " << seed;
    }
    EXPECT_GT(connected_runs, 200);  // most runs really did set up a call despite the loss
    std::printf("    chaos: %d/300 runs were connected at hangup time\n", connected_runs);
}
