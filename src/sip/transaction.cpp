#include "sip/transaction.hpp"

#include <algorithm>

namespace sip {
namespace {

std::string client_key(const Message& m) { return "c|" + m.branch() + "|" + m.cseq_method(); }
std::string server_key(const Message& req) {
    // ACK for a non-2xx belongs to the INVITE server transaction (same branch).
    std::string method = req.method == "ACK" ? "INVITE" : req.method;
    return "s|" + req.branch() + "|" + method;
}

}  // namespace

const char* to_string(TxnState s) {
    switch (s) {
        case TxnState::Calling: return "Calling";
        case TxnState::Trying: return "Trying";
        case TxnState::Proceeding: return "Proceeding";
        case TxnState::Completed: return "Completed";
        case TxnState::Confirmed: return "Confirmed";
        case TxnState::Terminated: return "Terminated";
    }
    return "?";
}

Message make_response(const Message& req, int status, const std::string& to_tag) {
    Message r;
    r.is_request = false;
    r.status = status;
    r.reason = reason_phrase(status);
    for (const auto& v : req.get_all("Via")) r.add("Via", v);
    r.set("From", *req.get("From"));
    std::string to = *req.get("To");
    if (!to_tag.empty() && header_param(to, "tag").empty()) to += ";tag=" + to_tag;
    r.set("To", to);
    r.set("Call-ID", req.call_id());
    r.set("CSeq", *req.get("CSeq"));
    return r;
}

TransactionLayer::TransactionLayer(core::Transport& tp, core::Timers& tm, TimerConfig cfg)
    : tp_(tp), tm_(tm), cfg_(cfg) {}

TransactionLayer::~TransactionLayer() {
    for (auto& [id, t] : txns_) {
        if (t.retrans) tm_.cancel(t.retrans);
        if (t.timeout) tm_.cancel(t.timeout);
    }
}

TxnState TransactionLayer::state(TxnId id) const {
    auto it = txns_.find(id);
    return it == txns_.end() ? TxnState::Terminated : it->second.state;
}

const Message* TransactionLayer::request(TxnId id) const {
    auto it = txns_.find(id);
    return it == txns_.end() ? nullptr : &it->second.req;
}

std::vector<std::string> TransactionLayer::dump() const {
    static const char* kinds[] = {"ICT", "NICT", "IST", "NIST"};
    std::vector<std::string> v;
    for (const auto& [id, t] : txns_)
        v.push_back(std::string(kinds[int(t.kind)]) + " " + t.req.method + " " + to_string(t.state) + " " + t.key);
    return v;
}

TxnId TransactionLayer::find_invite_server(const std::string& branch) const {
    auto it = index_.find("s|" + branch + "|INVITE");
    return it == index_.end() ? 0 : it->second;
}

void TransactionLayer::transmit(const std::string& data, const Endpoint& to) {
    ++stats_.sent;
    tp_.send(data, to);
}

void TransactionLayer::send_stateless(const Message& m, const Endpoint& dst) { transmit(m.serialize(), dst); }

void TransactionLayer::arm_retrans(TxnId id, uint64_t delay) {
    auto& t = txns_.at(id);
    t.interval = delay;
    t.retrans = tm_.schedule(delay, [this, id] {
        auto it = txns_.find(id);
        if (it == txns_.end()) return;
        Txn& t = it->second;
        t.retrans = 0;
        switch (t.kind) {
            case TxnKind::InviteClient:  // Timer A: doubles without cap while Calling
                if (t.state != TxnState::Calling) return;
                ++stats_.retransmits;
                transmit(t.req.serialize(), t.peer);
                arm_retrans(id, t.interval * 2);
                break;
            case TxnKind::NonInviteClient:  // Timer E: doubles up to T2; T2 once Proceeding
                if (t.state != TxnState::Trying && t.state != TxnState::Proceeding) return;
                ++stats_.retransmits;
                transmit(t.req.serialize(), t.peer);
                arm_retrans(id, t.state == TxnState::Trying ? std::min(t.interval * 2, cfg_.t2) : cfg_.t2);
                break;
            case TxnKind::InviteServer:  // Timer G: resend the non-2xx final until ACK
                if (t.state != TxnState::Completed) return;
                ++stats_.retransmits;
                transmit(t.last_rsp, t.peer);
                arm_retrans(id, std::min(t.interval * 2, cfg_.t2));
                break;
            case TxnKind::NonInviteServer: break;
        }
    });
}

void TransactionLayer::arm_timeout(TxnId id, uint64_t delay, bool report) {
    auto& t = txns_.at(id);
    if (t.timeout) tm_.cancel(t.timeout);
    t.timeout = tm_.schedule(delay, [this, id, report] {
        auto it = txns_.find(id);
        if (it == txns_.end()) return;
        it->second.timeout = 0;
        TxnKind kind = it->second.kind;
        terminate(id);
        if (!report) return;  // Timers D, I, J, K: quiet end of the wait state
        ++stats_.timeouts;
        if (kind == TxnKind::InviteServer) {
            if (cb_.on_server_timeout) cb_.on_server_timeout(id);
        } else if (cb_.on_timeout) {
            cb_.on_timeout(id);
        }
    });
}

void TransactionLayer::terminate(TxnId id) {
    auto it = txns_.find(id);
    if (it == txns_.end()) return;
    if (it->second.retrans) tm_.cancel(it->second.retrans);
    if (it->second.timeout) tm_.cancel(it->second.timeout);
    index_.erase(it->second.key);
    txns_.erase(it);
}

TxnId TransactionLayer::send_request(const Message& req, const Endpoint& dst) {
    TxnId id = ++next_id_;
    const bool invite = req.method == "INVITE";
    Txn t{invite ? TxnKind::InviteClient : TxnKind::NonInviteClient,
          invite ? TxnState::Calling : TxnState::Trying,
          client_key(req), req, dst, {}, {}, 0, 0, 0};
    index_[t.key] = id;
    txns_.emplace(id, std::move(t));
    transmit(req.serialize(), dst);
    arm_retrans(id, cfg_.t1);
    arm_timeout(id, 64 * cfg_.t1, true);  // Timer B / F
    return id;
}

Message TransactionLayer::build_ack(const Message& invite, const Message& rsp) const {
    // RFC 3261 17.1.1.3
    Message ack;
    ack.method = "ACK";
    ack.uri = invite.uri;
    ack.add("Via", invite.top_via());
    ack.set("Max-Forwards", "70");
    ack.set("From", *invite.get("From"));
    ack.set("To", *rsp.get("To"));
    ack.set("Call-ID", invite.call_id());
    ack.set("CSeq", std::to_string(invite.cseq_num()) + " ACK");
    for (const auto& r : invite.get_all("Route")) ack.add("Route", r);
    return ack;
}

void TransactionLayer::client_response(TxnId id, const Message& rsp) {
    Txn& t = txns_.at(id);
    const int code = rsp.status;
    if (t.kind == TxnKind::InviteClient) {
        switch (t.state) {
            case TxnState::Calling:
            case TxnState::Proceeding:
                if (code < 200) {
                    if (t.state == TxnState::Calling)  // Timer B only runs in Calling; RFC 3261 has no
                        arm_timeout(id, cfg_.timer_c, true);  // timeout in Proceeding, so guard it (Timer C)
                    t.state = TxnState::Proceeding;
                } else if (code < 300) {
                    terminate(id);  // the UA core ACKs a 2xx itself
                } else {
                    t.state = TxnState::Completed;
                    t.ack = build_ack(t.req, rsp).serialize();
                    transmit(t.ack, t.peer);
                    arm_timeout(id, 32000, false);  // Timer D
                }
                if (cb_.on_response) cb_.on_response(id, rsp);
                return;
            case TxnState::Completed:
                ++stats_.absorbed;
                transmit(t.ack, t.peer);  // our ACK was lost; resend, do not tell the TU again
                return;
            default: return;
        }
    }
    // non-INVITE client
    switch (t.state) {
        case TxnState::Trying:
        case TxnState::Proceeding:
            if (code < 200) {
                t.state = TxnState::Proceeding;
            } else {
                t.state = TxnState::Completed;
                if (t.retrans) { tm_.cancel(t.retrans); t.retrans = 0; }
                arm_timeout(id, cfg_.t4, false);  // Timer K
            }
            if (cb_.on_response) cb_.on_response(id, rsp);
            return;
        default:
            ++stats_.absorbed;
            return;
    }
}

void TransactionLayer::server_request(TxnId id, const Message& req) {
    Txn& t = txns_.at(id);
    if (t.kind == TxnKind::InviteServer && req.method == "ACK") {
        if (t.state == TxnState::Completed) {
            t.state = TxnState::Confirmed;
            if (t.retrans) { tm_.cancel(t.retrans); t.retrans = 0; }
            arm_timeout(id, cfg_.t4, false);  // Timer I
        } else {
            ++stats_.absorbed;
        }
        return;
    }
    // A retransmitted request: replay whatever we last answered.
    ++stats_.absorbed;
    if (!t.last_rsp.empty() && t.state != TxnState::Confirmed) transmit(t.last_rsp, t.peer);
}

void TransactionLayer::respond(TxnId id, const Message& rsp) {
    auto it = txns_.find(id);
    if (it == txns_.end()) return;
    Txn& t = it->second;
    if (t.state == TxnState::Completed || t.state == TxnState::Confirmed) return;  // already final
    t.last_rsp = rsp.serialize();
    transmit(t.last_rsp, t.peer);
    const int code = rsp.status;
    if (code < 200) {
        t.state = TxnState::Proceeding;
        return;
    }
    if (t.kind == TxnKind::InviteServer) {
        if (code < 300) {
            terminate(id);  // 2xx: the UA core retransmits it until the ACK arrives
        } else {
            t.state = TxnState::Completed;
            arm_retrans(id, cfg_.t1);                // Timer G
            arm_timeout(id, 64 * cfg_.t1, true);     // Timer H
        }
    } else {
        t.state = TxnState::Completed;
        arm_timeout(id, 64 * cfg_.t1, false);        // Timer J
    }
}

void TransactionLayer::on_datagram(const std::string& data, const Endpoint& src) {
    auto msg = parse(data);
    if (!msg) { ++stats_.parse_errors; return; }
    const Message& m = *msg;

    if (!m.is_request) {
        auto it = index_.find(client_key(m));
        if (it == index_.end()) {
            if (cb_.on_stray_response) cb_.on_stray_response(m, src);
            return;
        }
        client_response(it->second, m);
        return;
    }

    auto it = index_.find(server_key(m));
    if (it != index_.end()) {
        server_request(it->second, m);
        return;
    }
    if (m.method == "ACK") {  // ACK for a 2xx: end-to-end, no transaction
        if (cb_.on_request) cb_.on_request(0, m, src);
        return;
    }
    TxnId id = ++next_id_;
    const bool invite = m.method == "INVITE";
    Txn t{invite ? TxnKind::InviteServer : TxnKind::NonInviteServer,
          invite ? TxnState::Proceeding : TxnState::Trying,
          server_key(m), m, src, {}, {}, 0, 0, 0};
    index_[t.key] = id;
    txns_.emplace(id, std::move(t));
    if (invite) respond(id, make_response(m, 100));  // stop the client's Timer A right away
    if (cb_.on_request) cb_.on_request(id, m, src);
}

}  // namespace sip
