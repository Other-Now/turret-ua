// RFC 3261 section 17: the four transaction state machines over UDP.
//
//   INVITE client (ICT)      Calling -> Proceeding -> Completed -> Terminated
//   non-INVITE client (NICT) Trying  -> Proceeding -> Completed -> Terminated
//   INVITE server (IST)      Proceeding -> Completed -> Confirmed -> Terminated
//   non-INVITE server (NIST) Trying  -> Proceeding -> Completed -> Terminated
//
// The layer owns retransmission (Timers A/E/G) and timeouts (B/F/H), absorbs
// retransmitted requests/responses, and builds the ACK for non-2xx final
// responses. 2xx retransmission and the ACK for 2xx belong to the UA core
// (RFC 3261 13.3.1.4 / 13.2.2.4), so they are not here.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/interfaces.hpp"
#include "sip/message.hpp"

namespace sip {

using core::Endpoint;

struct TimerConfig {
    uint64_t t1 = 500;   // RTT estimate
    uint64_t t2 = 4000;  // max retransmit interval for non-INVITE
    uint64_t t4 = 5000;  // max time a message stays in the network
    // Guard for an INVITE client transaction stuck in Proceeding (no final ever
    // arrives). RFC 3261 only defines this for proxies (Timer C, > 3 min).
    uint64_t timer_c = 180000;
};

enum class TxnKind { InviteClient, NonInviteClient, InviteServer, NonInviteServer };
enum class TxnState { Calling, Trying, Proceeding, Completed, Confirmed, Terminated };
const char* to_string(TxnState s);

using TxnId = uint64_t;  // 0 = none

// Build a response to `req` (RFC 3261 8.2.6): copies Via/From/To/Call-ID/CSeq and
// adds the To tag if one is given and the request had none.
Message make_response(const Message& req, int status, const std::string& to_tag = {});

class TransactionLayer {
public:
    struct Callbacks {
        // A new request created a server transaction. ACK for a 2xx has no
        // transaction and arrives with server == 0.
        std::function<void(TxnId server, const Message& req, const Endpoint& src)> on_request;
        std::function<void(TxnId client, const Message& rsp)> on_response;
        std::function<void(TxnId client)> on_timeout;                 // Timer B / F
        std::function<void(TxnId server)> on_server_timeout;          // Timer H (no ACK)
        std::function<void(const Message& rsp, const Endpoint& src)> on_stray_response;  // e.g. 2xx retransmits
    };
    struct Stats {
        uint64_t sent = 0, retransmits = 0, absorbed = 0, parse_errors = 0, timeouts = 0;
    };

    TransactionLayer(core::Transport& tp, core::Timers& tm, TimerConfig cfg = {});
    ~TransactionLayer();
    void set_callbacks(Callbacks cb) { cb_ = std::move(cb); }

    TxnId send_request(const Message& req, const Endpoint& dst);   // client transaction
    void send_stateless(const Message& m, const Endpoint& dst);     // ACK for 2xx, 2xx retransmits
    void respond(TxnId server, const Message& rsp);
    void on_datagram(const std::string& data, const Endpoint& src);

    // CANCEL matching (RFC 3261 9.2): the INVITE server transaction with this branch.
    TxnId find_invite_server(const std::string& branch) const;
    bool alive(TxnId id) const { return txns_.count(id) != 0; }
    TxnState state(TxnId id) const;
    const Message* request(TxnId id) const;
    size_t live_count() const { return txns_.size(); }
    std::vector<std::string> dump() const;  // "INVITE client Proceeding z9hG4bK..." per live txn
    const Stats& stats() const { return stats_; }
    const TimerConfig& config() const { return cfg_; }

private:
    struct Txn {
        TxnKind kind;
        TxnState state;
        std::string key;
        Message req;
        Endpoint peer;
        std::string last_rsp;  // serialised, for server retransmits
        std::string ack;       // serialised ACK for non-2xx (ICT)
        uint64_t interval = 0;
        core::TimerId retrans = 0, timeout = 0;
    };

    void transmit(const std::string& data, const Endpoint& to);
    void arm_retrans(TxnId id, uint64_t delay);
    void arm_timeout(TxnId id, uint64_t delay, bool report);
    void terminate(TxnId id);
    void client_response(TxnId id, const Message& rsp);
    void server_request(TxnId id, const Message& req);
    Message build_ack(const Message& invite, const Message& rsp) const;

    core::Transport& tp_;
    core::Timers& tm_;
    TimerConfig cfg_;
    Callbacks cb_;
    Stats stats_;
    TxnId next_id_ = 0;
    std::map<TxnId, Txn> txns_;
    std::unordered_map<std::string, TxnId> index_;
};

}  // namespace sip
