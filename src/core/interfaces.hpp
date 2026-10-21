// The two seams that make the SIP stack testable: time and the network.
// Production code gets EventLoop (real clock + UDP); tests get FakeTimers and a
// gmock transport, so every retransmission and every race is deterministic.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace core {

struct Endpoint {
    std::string host;  // dotted IPv4
    uint16_t port = 0;
    bool operator==(const Endpoint& o) const { return host == o.host && port == o.port; }
    bool operator<(const Endpoint& o) const { return host < o.host || (host == o.host && port < o.port); }
    std::string str() const { return host + ":" + std::to_string(port); }
};

using TimerId = uint64_t;

class Timers {
public:
    virtual ~Timers() = default;
    virtual uint64_t now_ms() const = 0;
    virtual TimerId schedule(uint64_t delay_ms, std::function<void()> fn) = 0;
    virtual void cancel(TimerId id) = 0;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const std::string& data, const Endpoint& to) = 0;
    virtual Endpoint local() const = 0;
};

// Manually advanced clock. Timers fire in deadline order (FIFO for ties), and a
// callback may schedule or cancel further timers while advance() runs.
class FakeTimers : public Timers {
public:
    uint64_t now_ms() const override { return now_; }
    TimerId schedule(uint64_t delay_ms, std::function<void()> fn) override {
        TimerId id = ++next_;
        q_.emplace(Key{now_ + delay_ms, id}, std::move(fn));
        return id;
    }
    void cancel(TimerId id) override {
        for (auto it = q_.begin(); it != q_.end(); ++it)
            if (it->first.id == id) { q_.erase(it); return; }
    }
    void advance(uint64_t ms) {
        const uint64_t until = now_ + ms;
        while (!q_.empty() && q_.begin()->first.at <= until) {
            auto it = q_.begin();
            now_ = it->first.at;
            auto fn = std::move(it->second);
            q_.erase(it);
            fn();
        }
        now_ = until;
    }
    size_t pending() const { return q_.size(); }

private:
    struct Key {
        uint64_t at;
        TimerId id;
        bool operator<(const Key& o) const { return at < o.at || (at == o.at && id < o.id); }
    };
    uint64_t now_ = 0;
    TimerId next_ = 0;
    std::map<Key, std::function<void()>> q_;
};

}  // namespace core
