// UA core (RFC 3261 sections 8, 12-15) + the per-call state machine.
//
// Threading: a UserAgent is NOT thread-safe on purpose. Every input - datagrams,
// timer expiries, user commands - must arrive on one thread (EventLoop does this
// in production; tests drive it directly). That single serialisation point is
// what makes the races below deterministic to reason about: two events for the
// same call can never interleave inside a handler.
//
// Call states:  Idle -> Calling -> Ringing -> Connected <-> OnHold
//                                               |  ^         |
//                                               v  |         v
//                                             Transferring --+--> Terminated
// Every transition goes through set_state(), which checks it against a table and
// counts anything illegal; the race tests assert that count stays zero.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/interfaces.hpp"
#include "sip/digest.hpp"
#include "sip/sdp.hpp"
#include "sip/transaction.hpp"

namespace ua {

using core::Endpoint;
using sip::Message;
using sip::TxnId;

enum class CallState { Idle, Calling, Ringing, Connected, OnHold, Transferring, Terminated };
enum class Direction { Outgoing, Incoming };
const char* to_string(CallState s);
bool transition_allowed(CallState from, CallState to);

struct CallInfo {
    int id = 0;
    Direction dir = Direction::Outgoing;
    CallState state = CallState::Idle;
    std::string peer;         // remote URI
    bool remote_hold = false; // the peer put us on hold
    std::string reason;       // why it ended / last notable event
    int referred_from = 0;    // call that REFERred us into this one (0 = none)
};

// Media is plugged in from outside so the SIP core can be tested without sockets.
class MediaHandler {
public:
    virtual ~MediaHandler() = default;
    virtual uint16_t open(int call) = 0;  // returns the local RTP port to advertise
    virtual void update(int call, const Endpoint& remote, sdp::Dir dir) = 0;
    virtual void close(int call) = 0;
};

struct Config {
    std::string user = "turret";
    std::string media_ip;          // c= address; defaults to the SIP transport IP
    uint16_t sdp_port_override = 0;// advertise this RTP port instead of the real one (impairment relay)
    bool auto_answer = false;
    uint64_t answer_delay_ms = 0;
    std::optional<sip::Credentials> creds;
    std::string registrar;         // "sip:host:port"; empty = no registration
    uint32_t register_expires = 300;
    uint64_t linger_ms = 32000;    // keep a terminated call around to answer stragglers
    uint64_t seed = 0;             // glare back-off RNG (0 = random_device)
};

struct Stats {
    uint64_t calls_out = 0, calls_in = 0, connected = 0;
    uint64_t sent_491 = 0, got_491 = 0, sent_481 = 0, glare_retries = 0;
    uint64_t cancel_lost_race = 0;   // 200 OK beat our CANCEL -> ACK + BYE
    uint64_t ok_retransmits = 0, ack_resends = 0, no_ack_byes = 0;
    uint64_t transfers_done = 0, auth_retries = 0;
};

class UserAgent {
public:
    UserAgent(core::Transport& tp, core::Timers& tm, Config cfg, sip::TimerConfig tcfg = {});
    ~UserAgent();

    void set_media(MediaHandler* m) { media_ = m; }
    std::function<void(const CallInfo&)> on_call_event;
    std::function<void(bool ok, int status)> on_register;

    void on_datagram(const std::string& data, const Endpoint& src) { txn_.on_datagram(data, src); }

    // ---- user commands (return false if not valid in the current state) ----
    int dial(const std::string& uri);
    bool answer(int id);
    bool hangup(int id);  // CANCEL, BYE or decline depending on state
    bool hold(int id);
    bool resume(int id);
    bool transfer(int id, const std::string& target_uri);  // blind transfer (REFER)
    void register_now();

    // ---- introspection ----
    std::optional<CallInfo> info(int id) const;
    std::vector<CallInfo> calls() const;
    std::vector<std::string> history(int id) const;  // "Calling->Ringing", ...
    uint64_t illegal_transitions() const { return illegal_; }
    const Stats& stats() const { return stats_; }
    sip::TransactionLayer& txn() { return txn_; }
    bool registered() const { return registered_; }
    std::string local_uri() const;

private:
    enum class Purpose { Invite, ReInvite, Bye, Cancel, Refer, Notify, Register, Other };
    enum class Pending { None, Hold, Resume };

    struct Call {
        int id = 0;
        Direction dir = Direction::Outgoing;
        CallState state = CallState::Idle;
        std::string reason;
        std::vector<std::string> history;
        // dialog state (RFC 3261 12)
        std::string call_id, local_tag, remote_tag, local_uri, remote_uri, remote_target;
        Endpoint remote_ep;
        uint32_t local_cseq = 0, remote_cseq = 0;
        // initial INVITE
        Message invite;            // last INVITE we sent (UAC) or received (UAS)
        Endpoint invite_dst;
        TxnId invite_server = 0;
        bool got_provisional = false, cancel_pending = false, cancelling = false;
        unsigned auth_attempts = 0;
        std::string ack_wire;      // ACK for the 2xx, resent on 2xx retransmits
        uint32_t ack_cseq = 0;
        // 2xx retransmission while waiting for ACK (UAS)
        std::string ok_wire;
        uint32_t ok_cseq = 0;
        core::TimerId ok_timer = 0;
        uint64_t ok_interval = 0, ok_deadline = 0;
        // re-INVITE (hold / resume)
        TxnId reinvite = 0;
        Pending pending = Pending::None;
        core::TimerId glare_timer = 0;
        // media
        uint16_t rtp_port = 0;
        uint64_t sdp_version = 1;
        bool local_hold = false, remote_hold = false;
        // transfer
        CallState pre_transfer = CallState::Connected;
        int referred_from = 0;      // transferee: original call to NOTIFY
        int final_code = 0;         // final failure status of the INVITE
        bool refer_notified = false;
        core::TimerId answer_timer = 0, linger_timer = 0;
    };

    // dispatch
    void on_request(TxnId st, const Message& req, const Endpoint& src);
    void on_response(TxnId ct, const Message& rsp);
    void on_timeout(TxnId ct);
    void on_stray_response(const Message& rsp, const Endpoint& src);

    void handle_invite(TxnId st, const Message& req, const Endpoint& src);
    void handle_reinvite(Call& c, TxnId st, const Message& req);
    void handle_ack(const Message& req);
    void handle_cancel(TxnId st, const Message& req);
    void handle_bye(Call& c, TxnId st, const Message& req);
    void handle_refer(Call& c, TxnId st, const Message& req);
    void handle_notify(Call& c, TxnId st, const Message& req);

    void invite_response(Call& c, TxnId ct, const Message& rsp);
    void reinvite_response(Call& c, const Message& rsp);

    // helpers
    Call* find(int id);
    Call* find_dialog(const std::string& call_id, const std::string& local_tag);
    Call& new_call(Direction dir);
    void set_state(Call& c, CallState s, const std::string& reason = {});
    void publish(const Call& c);
    void finish(Call& c, const std::string& reason);  // -> Terminated + cleanup
    Message dialog_request(Call& c, const std::string& method);
    TxnId send_tracked(Call* c, Purpose p, const Message& req, const Endpoint& dst);
    void send_ack_2xx(Call& c, const Message& rsp);
    void send_reinvite(Call& c, Pending kind);
    void send_cancel(Call& c);
    void send_bye(Call& c);
    void send_notify(Call& transferee_parent, int code, const std::string& reason, bool final);
    void respond(TxnId st, const Message& req, int code, Call* c = nullptr, const std::string& body = {});
    void start_ok_retransmit(Call& c, const Message& ok);
    void ok_tick(int id);
    void register_response(const Message& rsp);
    void stop_ok_retransmit(Call& c);
    std::string local_sdp(Call& c, sdp::Dir dir);
    sdp::Dir apply_remote_sdp(Call& c, const std::string& body, bool is_answer);
    std::string contact() const;
    std::string via() const;
    std::optional<Endpoint> resolve(const std::string& uri) const;

    core::Transport& tp_;
    core::Timers& tm_;
    Config cfg_;
    sip::TransactionLayer txn_;
    MediaHandler* media_ = nullptr;
    std::mt19937 rng_;

    int next_call_ = 0;
    std::map<int, Call> calls_;
    std::unordered_map<std::string, int> dialogs_;  // call_id|local_tag -> call
    std::unordered_map<TxnId, std::pair<int, Purpose>> client_;
    uint64_t illegal_ = 0;
    Stats stats_;

    // registration
    std::string reg_call_id_;
    uint32_t reg_cseq_ = 0;
    unsigned reg_auth_attempts_ = 0;
    Message reg_req_;
    Endpoint reg_dst_;
    bool registered_ = false;
    core::TimerId reg_timer_ = 0;
};

}  // namespace ua
