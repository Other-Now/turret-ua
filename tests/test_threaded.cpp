// Real threads, real UDP sockets. These are the tests ThreadSanitizer is for:
// a "UI" thread (the test body) fires commands at two UAs that each live on
// their own EventLoop thread, exactly the shape of the Qt app. All UA state is
// read back through the loop (on_loop), never directly from the test thread.
#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <thread>

#include "core/event_loop.hpp"
#include "core/udp_transport.hpp"
#include "rtp/media.hpp"
#include "ua/user_agent.hpp"

using namespace std::chrono_literals;

namespace {

struct LoopThread {
    core::EventLoop loop;
    std::thread th;
    LoopThread() : th([this] { loop.run(); }) {}
    ~LoopThread() {
        loop.stop();
        th.join();
    }
    template <class F>
    auto call(F f) -> decltype(f()) {
        std::promise<decltype(f())> p;
        auto fut = p.get_future();
        loop.post([&] {
            if constexpr (std::is_void_v<decltype(f())>) { f(); p.set_value(); }
            else p.set_value(f());
        });
        return fut.get();
    }
};

struct Phone {
    LoopThread lt;
    std::unique_ptr<core::UdpTransport> tp;
    std::unique_ptr<ua::UserAgent> ua;
    std::atomic<int> connected{0}, terminated{0}, held{0};
    explicit Phone(bool auto_answer) {
        lt.call([&] {
            tp = std::make_unique<core::UdpTransport>(lt.loop, "127.0.0.1", 0);
            ua::Config cfg;
            cfg.auto_answer = auto_answer;
            cfg.linger_ms = 2000;
            ua = std::make_unique<ua::UserAgent>(*tp, lt.loop, cfg);
            tp->on_datagram = [this](const std::string& d, const core::Endpoint& s) { ua->on_datagram(d, s); };
            ua->on_call_event = [this](const ua::CallInfo& c) {
                if (c.state == ua::CallState::Connected && c.reason == "answered") ++connected;
                if (c.state == ua::CallState::Terminated) ++terminated;
                if (c.state == ua::CallState::OnHold) ++held;
            };
        });
    }
    ~Phone() {
        lt.call([&] { ua.reset(); tp.reset(); });
    }
    std::string uri() { return "sip:x@" + lt.call([&] { return tp->local().str(); }); }
};

bool wait_for(const std::function<bool()>& pred, std::chrono::milliseconds limit = 10s) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

}  // namespace

TEST(EventLoop, PostsFromManyThreadsRunOnTheLoopThread) {
    LoopThread lt;
    int counter = 0;  // deliberately not atomic: only the loop thread touches it
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t)
        producers.emplace_back([&] {
            for (int i = 0; i < 10000; ++i) lt.loop.post([&] { ++counter; });
        });
    for (auto& p : producers) p.join();
    EXPECT_EQ(lt.call([&] { return counter; }), 40000);
}

TEST(EventLoop, TimersFireInOrder) {
    LoopThread lt;
    std::vector<int> order;
    lt.call([&] {
        lt.loop.schedule(30, [&] { order.push_back(3); });
        lt.loop.schedule(10, [&] { order.push_back(1); });
        auto id = lt.loop.schedule(20, [&] { order.push_back(99); });
        lt.loop.schedule(20, [&] { order.push_back(2); });
        lt.loop.cancel(id);
    });
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(lt.call([&] { return order; }), (std::vector<int>{1, 2, 3}));
}

TEST(Threaded, UiThreadDrivesCallsOverRealUdp) {
    Phone alice(false), bob(true);
    const std::string bob_uri = bob.uri();
    constexpr int kCalls = 25;
    for (int i = 0; i < kCalls; ++i) {
        int id = alice.lt.call([&] { return alice.ua->dial(bob_uri); });
        ASSERT_GT(id, 0);
        ASSERT_TRUE(wait_for([&] { return alice.connected.load() == i + 1; })) << "call " << i;
        // Hold / resume / hangup fired from this (UI) thread without waiting for
        // each to complete: they queue on the loop and race the network replies.
        alice.lt.loop.post([&, id] { alice.ua->hold(id); });
        ASSERT_TRUE(wait_for([&] { return alice.held.load() == i + 1; }));
        alice.lt.loop.post([&, id] { alice.ua->resume(id); });
        alice.lt.loop.post([&, id] { alice.ua->hangup(id); });
        ASSERT_TRUE(wait_for([&] { return alice.terminated.load() == i + 1 && bob.terminated.load() == i + 1; }));
    }
    EXPECT_EQ(alice.lt.call([&] { return alice.ua->illegal_transitions(); }), 0u);
    EXPECT_EQ(bob.lt.call([&] { return bob.ua->illegal_transitions(); }), 0u);
    EXPECT_EQ(bob.lt.call([&] { return bob.ua->stats().connected; }), uint64_t(kCalls));
}

TEST(Threaded, BothSidesHangUpAtOnce) {
    // BYE crosses BYE over real sockets, from two different threads.
    Phone alice(false), bob(true);
    const std::string bob_uri = bob.uri();
    int a = alice.lt.call([&] { return alice.ua->dial(bob_uri); });
    ASSERT_TRUE(wait_for([&] { return alice.connected.load() == 1 && bob.connected.load() == 1; }));
    int b = bob.lt.call([&] { return bob.ua->calls().back().id; });
    std::thread t1([&] { alice.lt.loop.post([&] { alice.ua->hangup(a); }); });
    std::thread t2([&] { bob.lt.loop.post([&] { bob.ua->hangup(b); }); });
    t1.join();
    t2.join();
    ASSERT_TRUE(wait_for([&] { return alice.terminated.load() == 1 && bob.terminated.load() == 1; }));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(alice.lt.call([&] { return alice.ua->illegal_transitions(); }), 0u);
    EXPECT_EQ(bob.lt.call([&] { return bob.ua->illegal_transitions(); }), 0u);
}

TEST(Threaded, RtpLoopbackOneSecond) {
    LoopThread lt;
    std::unique_ptr<rtp::MediaManager> a, b;
    lt.call([&] {
        a = std::make_unique<rtp::MediaManager>(lt.loop, "127.0.0.1", 41000, 40);
        b = std::make_unique<rtp::MediaManager>(lt.loop, "127.0.0.1", 42000, 40);
        uint16_t pa = a->open(1), pb = b->open(1);
        a->update(1, {"127.0.0.1", pb}, sdp::Dir::SendRecv);
        b->update(1, {"127.0.0.1", pa}, sdp::Dir::RecvOnly);  // b is "on hold": receives only
    });
    std::this_thread::sleep_for(1000ms);
    auto ra = lt.call([&] { return *a->report(1); });
    auto rb = lt.call([&] { return *b->report(1); });
    EXPECT_GE(rb.received, 40u);
    EXPECT_EQ(rb.lost, 0);
    EXPECT_EQ(rb.sent, 0u);      // recvonly: nothing sent
    EXPECT_EQ(ra.received, 0u);
    EXPECT_LT(rb.jitter_ms, 20.0);  // loopback; a sanity bound - real numbers come from CI (local box is shared)
    EXPECT_GT(rb.jb.played, 30u);
    lt.call([&] { a.reset(); b.reset(); });
}
