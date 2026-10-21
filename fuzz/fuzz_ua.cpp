// Whole-stack fuzz target: a UserAgent with a fake clock receives a sequence
// of fuzzer-chosen datagrams (split on "\n#\n"), time advances between them,
// and every ringing call is answered. Crashes, sanitizer reports, an illegal
// state transition, or a transaction surviving all its timers are bugs.
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/interfaces.hpp"
#include "ua/user_agent.hpp"

namespace {
struct NullTransport : core::Transport {
    void send(const std::string&, const core::Endpoint&) override {}
    core::Endpoint local() const override { return {"10.0.0.2", 5060}; }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    core::FakeTimers t;
    NullTransport tp;
    ua::Config cfg;
    cfg.seed = 1;
    ua::UserAgent ua(tp, t, cfg);
    std::string in(reinterpret_cast<const char*>(data), size);
    const core::Endpoint peer{"10.0.0.1", 5060};
    size_t pos = 0;
    int step = 0;
    while (pos <= in.size()) {
        size_t end = in.find("\n#\n", pos);
        if (end == std::string::npos) end = in.size();
        ua.on_datagram(in.substr(pos, end - pos), peer);
        t.advance(50 + 450 * (step++ % 3));
        for (const auto& c : ua.calls()) {
            if (c.state == ua::CallState::Ringing && c.dir == ua::Direction::Incoming) ua.answer(c.id);
            else if (c.state == ua::CallState::Connected && step % 2) ua.hold(c.id);
            else if (c.state == ua::CallState::OnHold) ua.transfer(c.id, "sip:carol@10.0.0.3");
        }
        pos = end + 3;
    }
    for (const auto& c : ua.calls()) ua.hangup(c.id);
    t.advance(300000);
    if (ua.illegal_transitions() != 0) {
        std::fprintf(stderr, "illegal transition\n");
        __builtin_trap();
    }
    if (ua.txn().live_count() != 0 || !ua.calls().empty()) {
        std::fprintf(stderr, "leak: %zu txns, %zu calls\n", ua.txn().live_count(), ua.calls().size());
        __builtin_trap();
    }
    return 0;
}
