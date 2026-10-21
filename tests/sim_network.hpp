// Deterministic in-memory network for the race tests. Every datagram is
// delivered through FakeTimers after `latency` ms, so "both sides act at t=0"
// really does put two requests on the wire at the same time, and the order they
// cross in is fixed. Individual messages can be dropped or delayed.
#pragma once
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "core/interfaces.hpp"
#include "sip/message.hpp"

namespace test {

class SimNetwork {
public:
    struct Node : core::Transport {
        SimNetwork* net = nullptr;
        core::Endpoint ep;
        std::function<void(const std::string&, const core::Endpoint&)> rx;
        void send(const std::string& d, const core::Endpoint& to) override { net->send(ep, to, d); }
        core::Endpoint local() const override { return ep; }
    };

    explicit SimNetwork(core::FakeTimers& t) : t_(t) {}

    Node& add(const std::string& name, core::Endpoint ep) {
        auto n = std::make_unique<Node>();
        n->net = this;
        n->ep = ep;
        names_[ep] = name;
        auto& ref = *n;
        nodes_.push_back(std::move(n));
        return ref;
    }

    uint64_t latency = 20;
    // Return true to drop. Called for every datagram.
    std::function<bool(const sip::Message&, const std::string& from, const std::string& to)> drop;
    // Random loss on top of `drop` (0..1), deterministic per seed.
    double loss = 0.0;
    std::mt19937 rng{1};
    // Ladder diagram of everything sent: "alice -> bob  INVITE" / "bob -> alice  200 INVITE"
    std::vector<std::string> ladder;

    std::string name(const core::Endpoint& ep) const {
        auto it = names_.find(ep);
        return it == names_.end() ? ep.str() : it->second;
    }

    static std::string label(const sip::Message& m) {
        return m.is_request ? m.method : std::to_string(m.status) + " " + m.cseq_method();
    }

    // Count of datagrams with this label sent from -> to (empty = any).
    int count(const std::string& lbl, const std::string& from = {}, const std::string& to = {}) const {
        int n = 0;
        for (const auto& e : sent_)
            if (e.label == lbl && (from.empty() || e.from == from) && (to.empty() || e.to == to)) ++n;
        return n;
    }

private:
    struct Sent { std::string from, to, label; };

    void send(const core::Endpoint& from, const core::Endpoint& to, const std::string& data) {
        auto m = sip::parse(data);
        const std::string f = name(from), d = name(to);
        const std::string lbl = m ? label(*m) : "<garbage>";
        sent_.push_back({f, d, lbl});
        bool dropped = (m && drop && drop(*m, f, d)) || (loss > 0 && std::uniform_real_distribution<>(0, 1)(rng) < loss);
        ladder.push_back(f + " -> " + d + "  " + lbl + (dropped ? "  (lost)" : ""));
        if (dropped) return;
        t_.schedule(latency, [this, from, to, data] {
            for (auto& n : nodes_)
                if (n->ep == to && n->rx) n->rx(data, from);
        });
    }

    core::FakeTimers& t_;
    std::vector<std::unique_ptr<Node>> nodes_;
    std::map<core::Endpoint, std::string> names_;
    std::vector<Sent> sent_;
};

}  // namespace test
