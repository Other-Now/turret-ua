// turret-cli: headless, scriptable softphone. Used by the interop scripts
// (SIPp, Asterisk), the impairment experiment and the 3-party transfer demo.
//
//   turret-cli --bind 127.0.0.1:5070 --auto-answer
//   turret-cli --bind 127.0.0.1:5080 --call sip:bob@127.0.0.1:5070 --hold-at 2000 --resume-at 4000 --hangup-at 6000
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "core/event_loop.hpp"
#include "core/udp_transport.hpp"
#include "rtp/media.hpp"
#include "ua/user_agent.hpp"
#include "util/pcap_writer.hpp"

namespace {

core::EventLoop* g_loop = nullptr;
void on_signal(int) {
    if (g_loop) g_loop->stop();
}

struct Opts {
    std::string bind = "127.0.0.1:5060", user = "turret", media_ip, call, transfer_to, registrar;
    std::string auth_user, password, pcap, wav, json;
    uint16_t rtp_base = 40000, sdp_port = 0;
    uint32_t jb = 60;
    bool auto_answer = false, no_media = false, quiet = false;
    long answer_delay = 0, hold_at = -1, resume_at = -1, transfer_at = -1, hangup_at = -1, duration = -1;
    long exit_after_calls = -1, t1 = 500;
};

void usage() {
    std::puts(
        "turret-cli --bind IP:PORT [--user NAME] [--media-ip IP] [--rtp-base PORT] [--sdp-port PORT] [--jb MS]\n"
        "           [--auto-answer] [--answer-delay MS] [--registrar sip:IP:PORT --auth-user U --password P]\n"
        "           [--call URI] [--hold-at MS] [--resume-at MS] [--transfer-at MS --transfer-to URI]\n"
        "           [--hangup-at MS] [--duration MS] [--exit-after-calls N] [--no-media] [--t1 MS]\n"
        "           [--pcap FILE] [--wav PREFIX] [--json FILE] [--quiet]\n"
        "Times for --hold-at/--resume-at/--transfer-at/--hangup-at are relative to the dialed call connecting.");
}

bool parse_opts(int argc, char** argv, Opts& o) {
    std::map<std::string, std::string*> str{{"--bind", &o.bind},         {"--user", &o.user},
                                            {"--media-ip", &o.media_ip}, {"--call", &o.call},
                                            {"--transfer-to", &o.transfer_to}, {"--registrar", &o.registrar},
                                            {"--auth-user", &o.auth_user}, {"--password", &o.password},
                                            {"--pcap", &o.pcap},         {"--wav", &o.wav},
                                            {"--json", &o.json}};
    std::map<std::string, long*> num{{"--answer-delay", &o.answer_delay}, {"--hold-at", &o.hold_at},
                                     {"--resume-at", &o.resume_at},       {"--transfer-at", &o.transfer_at},
                                     {"--hangup-at", &o.hangup_at},       {"--duration", &o.duration},
                                     {"--exit-after-calls", &o.exit_after_calls}, {"--t1", &o.t1}};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--auto-answer") o.auto_answer = true;
        else if (a == "--no-media") o.no_media = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--rtp-base") { auto v = need(); if (!v) return false; o.rtp_base = uint16_t(std::atoi(v)); }
        else if (a == "--sdp-port") { auto v = need(); if (!v) return false; o.sdp_port = uint16_t(std::atoi(v)); }
        else if (a == "--jb") { auto v = need(); if (!v) return false; o.jb = uint32_t(std::atoi(v)); }
        else if (str.count(a)) { auto v = need(); if (!v) return false; *str[a] = v; }
        else if (num.count(a)) { auto v = need(); if (!v) return false; *num[a] = std::atol(v); }
        else return false;
    }
    return true;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) o += c;
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    if (!parse_opts(argc, argv, o)) {
        usage();
        return 2;
    }
    auto colon = o.bind.rfind(':');
    if (colon == std::string::npos) { usage(); return 2; }
    const std::string ip = o.bind.substr(0, colon);
    const uint16_t port = static_cast<uint16_t>(std::atoi(o.bind.c_str() + colon + 1));

    core::EventLoop loop;
    g_loop = &loop;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::unique_ptr<util::PcapWriter> pcap;
    if (!o.pcap.empty()) pcap = std::make_unique<util::PcapWriter>(o.pcap);
    core::UdpTransport tp(loop, ip, port, pcap.get());
    if (!tp.ok()) return 1;

    ua::Config cfg;
    cfg.user = o.user;
    cfg.media_ip = o.media_ip;
    cfg.sdp_port_override = o.sdp_port;
    cfg.auto_answer = o.auto_answer;
    cfg.answer_delay_ms = static_cast<uint64_t>(o.answer_delay);
    cfg.registrar = o.registrar;
    if (!o.password.empty()) cfg.creds = sip::Credentials{o.auth_user.empty() ? o.user : o.auth_user, o.password};
    sip::TimerConfig tcfg;
    tcfg.t1 = static_cast<uint64_t>(o.t1);
    auto ua = std::make_unique<ua::UserAgent>(tp, loop, cfg, tcfg);
    tp.on_datagram = [&](const std::string& d, const core::Endpoint& s) { ua->on_datagram(d, s); };

    std::unique_ptr<rtp::MediaManager> media;
    if (!o.no_media) {
        media = std::make_unique<rtp::MediaManager>(loop, o.media_ip.empty() ? ip : o.media_ip, o.rtp_base, o.jb,
                                                    pcap.get());
        media->wav_prefix = o.wav;
        ua->set_media(media.get());
    }

    const uint64_t t0 = loop.now_ms();
    int dialed = 0;
    long terminated = 0;
    bool scheduled = false;
    std::map<int, std::string> final_state;

    auto finish_soon = [&] { loop.schedule(800, [&] { loop.stop(); }); };

    ua->on_call_event = [&](const ua::CallInfo& c) {
        if (!o.quiet)
            std::printf("[%6llu ms] call %d %-12s %s%s\n", static_cast<unsigned long long>(loop.now_ms() - t0), c.id,
                        ua::to_string(c.state), c.reason.c_str(), c.remote_hold ? " (remote hold)" : "");
        std::fflush(stdout);
        final_state[c.id] = ua::to_string(c.state);
        if (c.id == dialed && c.state == ua::CallState::Connected && !scheduled) {
            scheduled = true;
            const int id = c.id;
            if (o.hold_at >= 0) loop.schedule(o.hold_at, [&, id] { ua->hold(id); });
            if (o.resume_at >= 0) loop.schedule(o.resume_at, [&, id] { ua->resume(id); });
            if (o.transfer_at >= 0 && !o.transfer_to.empty())
                loop.schedule(o.transfer_at, [&, id] { ua->transfer(id, o.transfer_to); });
            if (o.hangup_at >= 0) loop.schedule(o.hangup_at, [&, id] { ua->hangup(id); });
        }
        if (c.state == ua::CallState::Terminated) {
            ++terminated;
            if (c.id == dialed && o.exit_after_calls < 0) finish_soon();
            if (o.exit_after_calls > 0 && terminated >= o.exit_after_calls) finish_soon();
        }
    };

    auto start_call = [&] {
        if (o.call.empty()) return;
        dialed = ua->dial(o.call);
        if (!dialed) {
            std::fprintf(stderr, "bad URI %s\n", o.call.c_str());
            loop.stop();
        }
    };

    if (!o.registrar.empty()) {
        ua->on_register = [&](bool ok, int status) {
            std::printf("[%6llu ms] register %s (%d)\n", static_cast<unsigned long long>(loop.now_ms() - t0),
                        ok ? "ok" : "FAILED", status);
            std::fflush(stdout);
            if (ok && !dialed) start_call();
        };
        loop.post([&] { ua->register_now(); });
    } else {
        loop.post(start_call);
    }
    if (o.duration > 0) loop.schedule(static_cast<uint64_t>(o.duration), [&] { loop.stop(); });

    std::printf("turret-cli %s listening on %s\n", o.user.c_str(), tp.local().str().c_str());
    std::fflush(stdout);
    loop.run();

    // ---- summary ----
    const auto& s = ua->stats();
    const auto& ts = ua->txn().stats();
    std::ostringstream j;
    j << "{\n  \"user\": \"" << json_escape(o.user) << "\",\n"
      << "  \"calls_out\": " << s.calls_out << ", \"calls_in\": " << s.calls_in << ", \"connected\": " << s.connected
      << ", \"terminated\": " << terminated << ",\n"
      << "  \"illegal_transitions\": " << ua->illegal_transitions() << ",\n"
      << "  \"sent_491\": " << s.sent_491 << ", \"got_491\": " << s.got_491 << ", \"sent_481\": " << s.sent_481
      << ", \"glare_retries\": " << s.glare_retries << ", \"cancel_lost_race\": " << s.cancel_lost_race
      << ", \"transfers_done\": " << s.transfers_done << ", \"auth_retries\": " << s.auth_retries << ",\n"
      << "  \"ok_retransmits\": " << s.ok_retransmits << ", \"ack_resends\": " << s.ack_resends << ",\n"
      << "  \"sip_sent\": " << ts.sent << ", \"sip_retransmits\": " << ts.retransmits
      << ", \"sip_absorbed\": " << ts.absorbed << ", \"sip_parse_errors\": " << ts.parse_errors
      << ", \"sip_timeouts\": " << ts.timeouts << ",\n  \"calls\": [";
    bool first = true;
    for (auto& [id, state] : final_state) {
        j << (first ? "\n" : ",\n") << "    {\"id\": " << id << ", \"state\": \"" << state << "\"";
        first = false;
        if (media)
            if (auto r = media->report(id)) {
                j << ", \"rtp\": {\"sent\": " << r->sent << ", \"received\": " << r->received << ", \"lost\": "
                  << r->lost << ", \"jitter_ms\": " << r->jitter_ms << ", \"max_jitter_ms\": " << r->max_jitter_ms
                  << ", \"jb_depth_ms\": " << r->jb_depth_ms << ", \"jb_played\": " << r->jb.played
                  << ", \"jb_concealed\": " << r->jb.concealed << ", \"jb_late\": " << r->jb.late
                  << ", \"jb_resyncs\": " << r->jb.resyncs << ", \"ssrcs\": " << r->ssrcs
                  << ", \"jb_jump_dropped\": " << r->jb.jump_dropped
                  << ", \"rtcp_rr_received\": " << r->rr_received << ", \"peer_jitter_ms\": " << r->peer_jitter_ms
                  << ", \"peer_lost\": " << r->peer_lost << "}";
            }
        j << "}";
    }
    j << "\n  ]\n}\n";
    if (!o.quiet) std::fputs(j.str().c_str(), stdout);
    if (!o.json.empty()) std::ofstream(o.json) << j.str();

    // Tear down on the loop thread's terms: media and UA cancel their timers.
    const bool clean = ua->illegal_transitions() == 0;
    ua.reset();
    media.reset();
    return clean ? 0 : 3;
}
