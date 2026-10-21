#include "ua/user_agent.hpp"

#include <cstdlib>

namespace ua {

using sip::make_response;

const char* to_string(CallState s) {
    switch (s) {
        case CallState::Idle: return "Idle";
        case CallState::Calling: return "Calling";
        case CallState::Ringing: return "Ringing";
        case CallState::Connected: return "Connected";
        case CallState::OnHold: return "OnHold";
        case CallState::Transferring: return "Transferring";
        case CallState::Terminated: return "Terminated";
    }
    return "?";
}

bool transition_allowed(CallState from, CallState to) {
    using S = CallState;
    switch (from) {
        case S::Idle: return to == S::Calling || to == S::Ringing;
        case S::Calling: return to == S::Ringing || to == S::Connected || to == S::Terminated;
        case S::Ringing: return to == S::Connected || to == S::Terminated;
        case S::Connected: return to == S::OnHold || to == S::Transferring || to == S::Terminated;
        case S::OnHold: return to == S::Connected || to == S::Transferring || to == S::Terminated;
        case S::Transferring: return to == S::Connected || to == S::OnHold || to == S::Terminated;
        case S::Terminated: return false;  // nothing resurrects a call
    }
    return false;
}

UserAgent::UserAgent(core::Transport& tp, core::Timers& tm, Config cfg, sip::TimerConfig tcfg)
    : tp_(tp), tm_(tm), cfg_(std::move(cfg)), txn_(tp, tm, tcfg),
      rng_(cfg_.seed ? static_cast<std::mt19937::result_type>(cfg_.seed) : std::random_device{}()) {
    sip::TransactionLayer::Callbacks cb;
    cb.on_request = [this](TxnId st, const Message& m, const Endpoint& src) { on_request(st, m, src); };
    cb.on_response = [this](TxnId ct, const Message& m) { on_response(ct, m); };
    cb.on_timeout = [this](TxnId ct) { on_timeout(ct); };
    cb.on_server_timeout = [this](TxnId) {};  // non-2xx never ACKed: nothing left to clean up
    cb.on_stray_response = [this](const Message& m, const Endpoint& src) { on_stray_response(m, src); };
    txn_.set_callbacks(std::move(cb));
}

UserAgent::~UserAgent() {
    for (auto& [id, c] : calls_)
        for (core::TimerId t : {c.ok_timer, c.glare_timer, c.answer_timer, c.linger_timer})
            if (t) tm_.cancel(t);
    if (reg_timer_) tm_.cancel(reg_timer_);
}

// ------------------------------------------------------------------ helpers

std::string UserAgent::local_uri() const {
    if (!cfg_.registrar.empty())
        if (auto hp = sip::uri_hostport(cfg_.registrar)) return "sip:" + cfg_.user + "@" + hp->host;
    return "sip:" + cfg_.user + "@" + tp_.local().str();
}

std::string UserAgent::contact() const { return "<sip:" + cfg_.user + "@" + tp_.local().str() + ">"; }

std::string UserAgent::via() const {
    return "SIP/2.0/UDP " + tp_.local().str() + ";branch=" + sip::new_branch() + ";rport";
}

std::optional<Endpoint> UserAgent::resolve(const std::string& uri) const {
    auto hp = sip::uri_hostport(uri);
    if (!hp) return std::nullopt;
    return Endpoint{hp->host, hp->port};  // IP literals only; no DNS/NAPTR here
}

UserAgent::Call* UserAgent::find(int id) {
    auto it = calls_.find(id);
    return it == calls_.end() ? nullptr : &it->second;
}

UserAgent::Call* UserAgent::find_dialog(const std::string& call_id, const std::string& local_tag) {
    auto it = dialogs_.find(call_id + "|" + local_tag);
    return it == dialogs_.end() ? nullptr : find(it->second);
}

UserAgent::Call& UserAgent::new_call(Direction dir) {
    int id = ++next_call_;
    Call& c = calls_[id];
    c.id = id;
    c.dir = dir;
    return c;
}

void UserAgent::publish(const Call& c) {
    if (on_call_event) on_call_event(CallInfo{c.id, c.dir, c.state, c.remote_uri, c.remote_hold, c.reason, c.referred_from});
}

void UserAgent::set_state(Call& c, CallState s, const std::string& reason) {
    if (c.state == s) return;
    if (!transition_allowed(c.state, s)) {
        ++illegal_;
        c.history.push_back(std::string("ILLEGAL ") + to_string(c.state) + "->" + to_string(s));
        return;
    }
    c.history.push_back(std::string(to_string(c.state)) + "->" + to_string(s));
    c.state = s;
    if (!reason.empty()) c.reason = reason;

    // Transferee side of a REFER: report the new call's fate to the transferor.
    if (c.referred_from && !c.refer_notified) {
        if (Call* parent = find(c.referred_from)) {
            if (s == CallState::Connected) {
                c.refer_notified = true;
                send_notify(*parent, 200, "OK", true);
            } else if (s == CallState::Terminated) {
                c.refer_notified = true;
                int code = c.final_code ? c.final_code : 487;
                send_notify(*parent, code, sip::reason_phrase(code), true);
            }
        }
    }
    publish(c);
}

void UserAgent::finish(Call& c, const std::string& reason) {
    if (c.state == CallState::Terminated) return;
    for (core::TimerId* t : {&c.answer_timer, &c.glare_timer})
        if (*t) { tm_.cancel(*t); *t = 0; }
    // Deliberately NOT stopping an unACKed 2xx here: RFC 3261 13.3.1.4 keeps
    // retransmitting it until the ACK or 64*T1, even if a BYE arrives first.
    // Stopping early leaves the peer's re-INVITE client transaction stuck in
    // Proceeding forever (found by the RandomLossChaos test, seed 7).
    set_state(c, CallState::Terminated, reason);
    if (media_) media_->close(c.id);
    const int id = c.id;
    c.linger_timer = tm_.schedule(cfg_.linger_ms, [this, id] {
        Call* c = find(id);
        if (!c) return;
        dialogs_.erase(c->call_id + "|" + c->local_tag);
        calls_.erase(id);
    });
}

Message UserAgent::dialog_request(Call& c, const std::string& method) {
    Message r;
    r.method = method;
    r.uri = c.remote_target;
    r.add("Via", via());
    r.set("Max-Forwards", "70");
    r.set("From", "<" + c.local_uri + ">;tag=" + c.local_tag);
    r.set("To", "<" + c.remote_uri + ">" + (c.remote_tag.empty() ? "" : ";tag=" + c.remote_tag));
    r.set("Call-ID", c.call_id);
    r.set("CSeq", std::to_string(++c.local_cseq) + " " + method);
    r.set("Contact", contact());
    return r;
}

TxnId UserAgent::send_tracked(Call* c, Purpose p, const Message& req, const Endpoint& dst) {
    TxnId id = txn_.send_request(req, dst);
    client_[id] = {c ? c->id : 0, p};
    return id;
}

void UserAgent::respond(TxnId st, const Message& req, int code, Call* c, const std::string& body) {
    Message r = make_response(req, code, c ? c->local_tag : (code > 100 ? sip::random_token(8) : std::string()));
    if (c && req.method == "INVITE" && code < 300) r.set("Contact", contact());
    if (code == 405) r.set("Allow", "INVITE, ACK, CANCEL, BYE, REFER, NOTIFY, OPTIONS");
    if (code == 500) r.set("Retry-After", "1");
    if (!body.empty()) {
        r.set("Content-Type", "application/sdp");
        r.body = body;
    }
    txn_.respond(st, r);
}

std::string UserAgent::local_sdp(Call& c, sdp::Dir dir) {
    sdp::Session s;
    s.ip = cfg_.media_ip.empty() ? tp_.local().host : cfg_.media_ip;
    s.port = cfg_.sdp_port_override ? cfg_.sdp_port_override
           : c.rtp_port             ? c.rtp_port
                                    : static_cast<uint16_t>(40000 + 2 * c.id);  // placeholder without media
    s.payloads = {0};
    s.dir = dir;
    s.version = ++c.sdp_version;
    return sdp::build(s, cfg_.user);
}

sdp::Dir UserAgent::apply_remote_sdp(Call& c, const std::string& body, bool is_answer) {
    auto s = sdp::parse(body);
    if (!s) return sdp::Dir::Inactive;
    sdp::Dir ours;
    if (is_answer) {
        ours = sdp::answer_dir(s->dir, sdp::Dir::SendRecv);  // mirror of what they answered
    } else {
        c.remote_hold = s->dir == sdp::Dir::SendOnly || s->dir == sdp::Dir::Inactive;
        ours = sdp::answer_dir(s->dir, c.local_hold ? sdp::Dir::SendOnly : sdp::Dir::SendRecv);
    }
    if (media_ && c.state != CallState::Terminated) media_->update(c.id, Endpoint{s->ip, s->port}, ours);
    return ours;
}

void UserAgent::send_ack_2xx(Call& c, const Message& rsp) {
    // RFC 3261 13.2.2.4: the ACK for a 2xx is its own transaction (new branch)
    Message ack;
    ack.method = "ACK";
    ack.uri = c.remote_target;
    ack.add("Via", via());
    ack.set("Max-Forwards", "70");
    ack.set("From", "<" + c.local_uri + ">;tag=" + c.local_tag);
    ack.set("To", *rsp.get("To"));
    ack.set("Call-ID", c.call_id);
    ack.set("CSeq", std::to_string(rsp.cseq_num()) + " ACK");
    c.ack_wire = ack.serialize();
    c.ack_cseq = rsp.cseq_num();
    tp_.send(c.ack_wire, c.remote_ep);
}

void UserAgent::send_reinvite(Call& c, Pending kind) {
    c.pending = kind;
    Message r = dialog_request(c, "INVITE");
    r.set("Content-Type", "application/sdp");
    r.body = local_sdp(c, kind == Pending::Hold ? sdp::Dir::SendOnly : sdp::Dir::SendRecv);
    c.reinvite = send_tracked(&c, Purpose::ReInvite, r, c.remote_ep);
}

void UserAgent::send_cancel(Call& c) {
    // RFC 3261 9.1: same Request-URI, top Via (branch), Call-ID, From, To, CSeq number
    Message x;
    x.method = "CANCEL";
    x.uri = c.invite.uri;
    x.add("Via", c.invite.top_via());
    x.set("Max-Forwards", "70");
    x.set("From", *c.invite.get("From"));
    x.set("To", *c.invite.get("To"));
    x.set("Call-ID", c.call_id);
    x.set("CSeq", std::to_string(c.invite.cseq_num()) + " CANCEL");
    c.cancelling = true;
    send_tracked(&c, Purpose::Cancel, x, c.invite_dst);
}

void UserAgent::send_bye(Call& c) { send_tracked(&c, Purpose::Bye, dialog_request(c, "BYE"), c.remote_ep); }

void UserAgent::send_notify(Call& parent, int code, const std::string& reason, bool final) {
    if (parent.state == CallState::Terminated) return;  // transferor already hung up
    Message n = dialog_request(parent, "NOTIFY");
    n.set("Event", "refer");
    n.set("Subscription-State", final ? "terminated;reason=noresource" : "active;expires=60");
    n.set("Content-Type", "message/sipfrag;version=2.0");
    n.body = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    send_tracked(&parent, Purpose::Notify, n, parent.remote_ep);
}

void UserAgent::start_ok_retransmit(Call& c, const Message& ok) {
    // RFC 3261 13.3.1.4: resend the 2xx at T1, 2T1, ... capped at T2 until the ACK
    // arrives; give up after 64*T1 and tear the dialog down with a BYE.
    stop_ok_retransmit(c);
    c.ok_wire = ok.serialize();
    c.ok_cseq = ok.cseq_num();
    c.ok_interval = txn_.config().t1;
    c.ok_deadline = tm_.now_ms() + 64 * txn_.config().t1;
    const int id = c.id;
    c.ok_timer = tm_.schedule(c.ok_interval, [this, id] { ok_tick(id); });
}

void UserAgent::ok_tick(int id) {
    Call* c = find(id);
    if (!c) return;
    c->ok_timer = 0;
    const uint64_t now = tm_.now_ms();
    if (now >= c->ok_deadline) {
        c->ok_wire.clear();
        if (c->state == CallState::Terminated) return;  // already hung up; just stop
        ++stats_.no_ack_byes;
        send_bye(*c);
        finish(*c, "no ACK");
        return;
    }
    ++stats_.ok_retransmits;
    tp_.send(c->ok_wire, c->remote_ep);
    c->ok_interval = std::min(c->ok_interval * 2, txn_.config().t2);
    // Next retransmission, or the 64*T1 give-up point if that comes first.
    const uint64_t next = std::min(c->ok_interval, c->ok_deadline - now);
    c->ok_timer = tm_.schedule(next, [this, id] { ok_tick(id); });
}

void UserAgent::stop_ok_retransmit(Call& c) {
    if (c.ok_timer) { tm_.cancel(c.ok_timer); c.ok_timer = 0; }
    c.ok_wire.clear();
}

// ------------------------------------------------------------------ commands

int UserAgent::dial(const std::string& uri) {
    auto dst = resolve(uri);
    if (!dst) return 0;
    Call& c = new_call(Direction::Outgoing);
    ++stats_.calls_out;
    c.call_id = sip::random_token(16) + "@" + tp_.local().host;
    c.local_tag = sip::random_token(8);
    c.local_uri = local_uri();
    c.remote_uri = uri;
    c.remote_target = uri;
    c.remote_ep = *dst;
    c.invite_dst = *dst;
    dialogs_[c.call_id + "|" + c.local_tag] = c.id;
    if (media_) c.rtp_port = media_->open(c.id);

    Message r;
    r.method = "INVITE";
    r.uri = uri;
    r.add("Via", via());
    r.set("Max-Forwards", "70");
    r.set("From", "<" + c.local_uri + ">;tag=" + c.local_tag);
    r.set("To", "<" + uri + ">");
    r.set("Call-ID", c.call_id);
    r.set("CSeq", std::to_string(++c.local_cseq) + " INVITE");
    r.set("Contact", contact());
    r.set("Allow", "INVITE, ACK, CANCEL, BYE, REFER, NOTIFY, OPTIONS");
    r.set("Content-Type", "application/sdp");
    r.body = local_sdp(c, sdp::Dir::SendRecv);
    c.invite = r;
    set_state(c, CallState::Calling, "dialing");
    send_tracked(&c, Purpose::Invite, r, *dst);
    return c.id;
}

bool UserAgent::answer(int id) {
    Call* c = find(id);
    if (!c || c->dir != Direction::Incoming || c->state != CallState::Ringing) return false;
    if (!txn_.alive(c->invite_server)) return false;
    c->answer_timer = 0;
    sdp::Dir dir = apply_remote_sdp(*c, c->invite.body, false);
    Message ok = make_response(c->invite, 200, c->local_tag);
    ok.set("Contact", contact());
    ok.set("Allow", "INVITE, ACK, CANCEL, BYE, REFER, NOTIFY, OPTIONS");
    ok.set("Content-Type", "application/sdp");
    ok.body = local_sdp(*c, dir);
    txn_.respond(c->invite_server, ok);
    start_ok_retransmit(*c, ok);
    ++stats_.connected;
    set_state(*c, CallState::Connected, "answered");
    return true;
}

bool UserAgent::hangup(int id) {
    Call* c = find(id);
    if (!c || c->state == CallState::Terminated) return false;
    if (c->dir == Direction::Outgoing && (c->state == CallState::Calling || c->state == CallState::Ringing)) {
        // RFC 3261 9.1: no CANCEL before a provisional response; remember to send it.
        if (c->got_provisional) send_cancel(*c);
        else c->cancel_pending = true;
        finish(*c, "cancelled locally");
        return true;
    }
    if (c->dir == Direction::Incoming && c->state == CallState::Ringing) {
        respond(c->invite_server, c->invite, 603, c);
        finish(*c, "declined");
        return true;
    }
    send_bye(*c);
    finish(*c, "local hangup");
    return true;
}

bool UserAgent::hold(int id) {
    Call* c = find(id);
    if (!c || c->state != CallState::Connected || c->pending != Pending::None || c->ok_timer) return false;
    send_reinvite(*c, Pending::Hold);
    return true;
}

bool UserAgent::resume(int id) {
    Call* c = find(id);
    if (!c || c->state != CallState::OnHold || c->pending != Pending::None || c->ok_timer) return false;
    send_reinvite(*c, Pending::Resume);
    return true;
}

bool UserAgent::transfer(int id, const std::string& target) {
    Call* c = find(id);
    if (!c || (c->state != CallState::Connected && c->state != CallState::OnHold)) return false;
    if (!sip::uri_hostport(target)) return false;
    c->pre_transfer = c->state;
    Message r = dialog_request(*c, "REFER");
    r.set("Refer-To", "<" + target + ">");
    r.set("Referred-By", "<" + c->local_uri + ">");
    send_tracked(c, Purpose::Refer, r, c->remote_ep);
    set_state(*c, CallState::Transferring, "transferring to " + target);
    return true;
}

void UserAgent::register_now() {
    if (cfg_.registrar.empty()) return;
    auto dst = resolve(cfg_.registrar);
    if (!dst) return;
    if (reg_call_id_.empty()) reg_call_id_ = sip::random_token(16) + "@" + tp_.local().host;
    const std::string aor = local_uri();
    Message r;
    r.method = "REGISTER";
    r.uri = "sip:" + dst->str();
    r.add("Via", via());
    r.set("Max-Forwards", "70");
    r.set("From", "<" + aor + ">;tag=" + sip::random_token(8));
    r.set("To", "<" + aor + ">");
    r.set("Call-ID", reg_call_id_);
    r.set("CSeq", std::to_string(++reg_cseq_) + " REGISTER");
    r.set("Contact", contact());
    r.set("Expires", std::to_string(cfg_.register_expires));
    reg_req_ = r;
    reg_dst_ = *dst;
    send_tracked(nullptr, Purpose::Register, r, *dst);
}

void UserAgent::register_response(const Message& rsp) {
    const int code = rsp.status;
    if (code < 200) return;
    if (code < 300) {
        registered_ = true;
        reg_auth_attempts_ = 0;
        uint32_t expires = cfg_.register_expires;
        if (auto* e = rsp.get("Expires")) expires = static_cast<uint32_t>(std::strtoul(e->c_str(), nullptr, 10));
        if (auto* ct = rsp.get("Contact")) {
            auto p = sip::header_param(*ct, "expires");
            if (!p.empty()) expires = static_cast<uint32_t>(std::strtoul(p.c_str(), nullptr, 10));
        }
        if (reg_timer_) tm_.cancel(reg_timer_);
        if (expires) reg_timer_ = tm_.schedule(uint64_t(expires) * 500, [this] { reg_timer_ = 0; register_now(); });
        if (on_register) on_register(true, code);
        return;
    }
    if ((code == 401 || code == 407) && cfg_.creds && reg_auth_attempts_ == 0) {
        ++reg_auth_attempts_;
        ++stats_.auth_retries;
        Message r = reg_req_;
        r.remove("Via");
        r.add("Via", via());
        r.set("CSeq", std::to_string(++reg_cseq_) + " REGISTER");
        auto* chal = rsp.get(code == 401 ? "WWW-Authenticate" : "Proxy-Authenticate");
        if (chal)
            r.set(code == 401 ? "Authorization" : "Proxy-Authorization",
                  sip::digest_authorization(*chal, *cfg_.creds, "REGISTER", r.uri));
        reg_req_ = r;
        send_tracked(nullptr, Purpose::Register, r, reg_dst_);
        return;
    }
    registered_ = false;
    reg_auth_attempts_ = 0;
    if (on_register) on_register(false, code);
}

// ------------------------------------------------------------------ inbound requests

void UserAgent::on_request(TxnId st, const Message& req, const Endpoint& src) {
    if (req.method == "ACK") { handle_ack(req); return; }
    if (req.method == "CANCEL") { handle_cancel(st, req); return; }

    const std::string tt = req.to_tag();
    if (tt.empty()) {
        if (req.method == "INVITE") { handle_invite(st, req, src); return; }
        if (req.method == "OPTIONS") { respond(st, req, 200); return; }
        if (req.method == "BYE" || req.method == "REFER" || req.method == "NOTIFY") {
            ++stats_.sent_481;
            respond(st, req, 481);
            return;
        }
        respond(st, req, 405);
        return;
    }

    Call* c = find_dialog(req.call_id(), tt);
    if (!c) {
        ++stats_.sent_481;
        respond(st, req, 481);
        return;
    }
    // RFC 3261 12.2.2: in-dialog requests must arrive with increasing CSeq
    if (c->remote_cseq && req.cseq_num() <= c->remote_cseq) {
        respond(st, req, 500, c);
        return;
    }
    c->remote_cseq = req.cseq_num();

    if (req.method == "BYE") { handle_bye(*c, st, req); return; }
    if (c->state == CallState::Terminated) {
        // We already hung up (e.g. our BYE crossed their re-INVITE): the dialog is gone.
        ++stats_.sent_481;
        respond(st, req, 481, c);
        return;
    }
    if (req.method == "INVITE") handle_reinvite(*c, st, req);
    else if (req.method == "REFER") handle_refer(*c, st, req);
    else if (req.method == "NOTIFY") handle_notify(*c, st, req);
    else if (req.method == "OPTIONS") respond(st, req, 200, c);
    else respond(st, req, 405, c);
}

void UserAgent::handle_invite(TxnId st, const Message& req, const Endpoint& src) {
    if (req.body.empty() || !sdp::parse(req.body)) {  // no offerless INVITEs here
        respond(st, req, 488);
        return;
    }
    Call& c = new_call(Direction::Incoming);
    ++stats_.calls_in;
    c.call_id = req.call_id();
    c.remote_tag = req.from_tag();
    c.local_tag = sip::random_token(8);
    c.remote_uri = sip::addr_uri(*req.get("From"));
    c.local_uri = sip::addr_uri(*req.get("To"));
    auto* ct = req.get("Contact");
    c.remote_target = ct ? sip::addr_uri(*ct) : c.remote_uri;
    auto ep = resolve(c.remote_target);
    c.remote_ep = ep ? *ep : src;
    c.remote_cseq = req.cseq_num();
    c.invite = req;
    c.invite_server = st;
    dialogs_[c.call_id + "|" + c.local_tag] = c.id;
    if (media_) c.rtp_port = media_->open(c.id);

    respond(st, req, 180, &c);
    set_state(c, CallState::Ringing, "incoming");
    if (cfg_.auto_answer) {
        const int id = c.id;
        c.answer_timer = tm_.schedule(cfg_.answer_delay_ms, [this, id] { answer(id); });
    }
}

void UserAgent::handle_reinvite(Call& c, TxnId st, const Message& req) {
    if (c.reinvite) {
        // RFC 3261 14.2: our own re-INVITE is still pending -> glare
        ++stats_.sent_491;
        respond(st, req, 491, &c);
        return;
    }
    if (c.ok_timer) {  // previous 2xx not ACKed yet
        respond(st, req, 500, &c);
        return;
    }
    if (c.state == CallState::Ringing || c.state == CallState::Calling) {
        respond(st, req, 500, &c);
        return;
    }
    if (req.body.empty() || !sdp::parse(req.body)) {
        respond(st, req, 488, &c);
        return;
    }
    const bool was_held = c.remote_hold;
    sdp::Dir dir = apply_remote_sdp(c, req.body, false);
    Message ok = make_response(req, 200, c.local_tag);
    ok.set("Contact", contact());
    ok.set("Content-Type", "application/sdp");
    ok.body = local_sdp(c, dir);
    txn_.respond(st, ok);
    start_ok_retransmit(c, ok);
    if (was_held != c.remote_hold) {
        c.reason = c.remote_hold ? "remote hold" : "remote resume";
        publish(c);
    }
}

void UserAgent::handle_ack(const Message& req) {
    Call* c = find_dialog(req.call_id(), req.to_tag());
    if (c && c->ok_timer && req.cseq_num() == c->ok_cseq) stop_ok_retransmit(*c);
}

void UserAgent::handle_cancel(TxnId st, const Message& req) {
    Call* c = nullptr;
    for (auto& [id, call] : calls_)
        if (call.dir == Direction::Incoming && call.call_id == req.call_id() && call.remote_tag == req.from_tag() &&
            call.invite.branch() == req.branch()) {
            c = &call;
            break;
        }
    if (!c) {
        ++stats_.sent_481;
        respond(st, req, 481);
        return;
    }
    respond(st, req, 200, c);  // the CANCEL itself succeeds...
    if (c->state == CallState::Ringing && txn_.alive(c->invite_server)) {
        respond(c->invite_server, c->invite, 487, c);  // ...and kills the INVITE if it is still pending
        finish(*c, "cancelled by caller");
    }
    // else: we already sent 200 OK. RFC 3261 9.2 says the CANCEL has no effect;
    // the caller has to ACK our 200 and send BYE.
}

void UserAgent::handle_bye(Call& c, TxnId st, const Message& req) {
    respond(st, req, 200, &c);
    if (c.state == CallState::Ringing && c.dir == Direction::Incoming && txn_.alive(c.invite_server))
        respond(c.invite_server, c.invite, 487, &c);
    finish(c, "remote hangup");
}

void UserAgent::handle_refer(Call& c, TxnId st, const Message& req) {
    auto* rt = req.get("Refer-To");
    if (!rt || (c.state != CallState::Connected && c.state != CallState::OnHold)) {
        respond(st, req, rt ? 491 : 400, &c);
        return;
    }
    const std::string target = sip::addr_uri(*rt);
    respond(st, req, 202, &c);
    send_notify(c, 100, "Trying", false);
    const int parent = c.id;  // dial() inserts into calls_; std::map keeps &c valid, ids are safer anyway
    int nid = dial(target);
    if (!nid) {
        if (Call* p = find(parent)) send_notify(*p, 400, "Bad Request", true);
        return;
    }
    find(nid)->referred_from = parent;
}

void UserAgent::handle_notify(Call& c, TxnId st, const Message& req) {
    respond(st, req, 200, &c);
    auto* ev = req.get("Event");
    if (!ev || ev->rfind("refer", 0) != 0 || c.state != CallState::Transferring) return;
    // body is a sipfrag: "SIP/2.0 200 OK"
    if (req.body.size() < 11 || req.body.compare(0, 8, "SIP/2.0 ") != 0) return;
    int code = std::atoi(req.body.c_str() + 8);
    if (code < 200) return;
    if (code < 300) {
        ++stats_.transfers_done;
        send_bye(c);
        finish(c, "transferred");
    } else {
        set_state(c, c.pre_transfer, "transfer failed (" + std::to_string(code) + ")");
    }
}

// ------------------------------------------------------------------ inbound responses

void UserAgent::on_response(TxnId ct, const Message& rsp) {
    auto it = client_.find(ct);
    if (it == client_.end()) return;
    const auto [cid, purpose] = it->second;
    if (rsp.status >= 200) client_.erase(it);
    if (purpose == Purpose::Register) { register_response(rsp); return; }
    Call* c = find(cid);
    if (!c) return;
    switch (purpose) {
        case Purpose::Invite: invite_response(*c, ct, rsp); break;
        case Purpose::ReInvite:
            if (ct == c->reinvite) reinvite_response(*c, rsp);
            break;
        case Purpose::Refer:
            if (rsp.status >= 300 && c->state == CallState::Transferring)
                set_state(*c, c->pre_transfer, "transfer rejected (" + std::to_string(rsp.status) + ")");
            break;
        default: break;  // BYE / CANCEL / NOTIFY responses need no action
    }
}

void UserAgent::invite_response(Call& c, TxnId, const Message& rsp) {
    const int code = rsp.status;
    if (code < 200) {
        c.got_provisional = true;
        if (c.remote_tag.empty()) c.remote_tag = rsp.to_tag();  // early dialog
        if (c.cancel_pending) {
            c.cancel_pending = false;
            send_cancel(c);
        }
        if (code >= 180 && c.state == CallState::Calling) set_state(c, CallState::Ringing, "ringing");
        return;
    }
    if (code < 300) {
        c.remote_tag = rsp.to_tag();
        if (auto* ct = rsp.get("Contact")) {
            c.remote_target = sip::addr_uri(*ct);
            if (auto ep = resolve(c.remote_target)) c.remote_ep = *ep;
        }
        send_ack_2xx(c, rsp);
        if (c.state == CallState::Terminated) {
            // The 200 crossed our CANCEL (or we hung up first): the call exists at
            // the far end now, so ACK it and immediately BYE it (RFC 3261 15).
            ++stats_.cancel_lost_race;
            send_bye(c);
            return;
        }
        apply_remote_sdp(c, rsp.body, true);
        ++stats_.connected;
        set_state(c, CallState::Connected, "answered");
        return;
    }
    if ((code == 401 || code == 407) && cfg_.creds && c.auth_attempts == 0 && c.state != CallState::Terminated) {
        ++c.auth_attempts;
        ++stats_.auth_retries;
        Message inv = c.invite;
        inv.remove("Via");
        inv.add("Via", via());
        inv.set("CSeq", std::to_string(++c.local_cseq) + " INVITE");
        auto* chal = rsp.get(code == 401 ? "WWW-Authenticate" : "Proxy-Authenticate");
        if (chal)
            inv.set(code == 401 ? "Authorization" : "Proxy-Authorization",
                    sip::digest_authorization(*chal, *cfg_.creds, "INVITE", inv.uri));
        c.invite = inv;
        c.got_provisional = false;
        send_tracked(&c, Purpose::Invite, inv, c.invite_dst);
        return;
    }
    c.final_code = code;
    finish(c, code == 487 ? "cancelled" : std::to_string(code) + " " + rsp.reason);
}

void UserAgent::reinvite_response(Call& c, const Message& rsp) {
    const int code = rsp.status;
    if (code < 200) return;
    const Pending kind = c.pending;
    c.reinvite = 0;
    c.pending = Pending::None;
    if (code < 300) {
        send_ack_2xx(c, rsp);
        if (c.state == CallState::Terminated) return;  // a BYE crossed our re-INVITE
        apply_remote_sdp(c, rsp.body, true);
        c.local_hold = kind == Pending::Hold;
        CallState target = c.local_hold ? CallState::OnHold : CallState::Connected;
        if (c.state == CallState::Transferring) c.pre_transfer = target;  // REFER already in flight
        else set_state(c, target, c.local_hold ? "held" : "resumed");
        return;
    }
    if (code == 491) {
        // RFC 3261 14.1: back off and retry. The Call-ID owner (the original caller)
        // waits 2.1-4.0 s, the other side 0-2.0 s, in 10 ms units - so they cannot collide again.
        ++stats_.got_491;
        if (c.state == CallState::Terminated) return;
        uint64_t delay = c.dir == Direction::Outgoing ? 2100 + 10 * (rng_() % 191) : 10 * (rng_() % 201);
        c.pending = kind;
        const int id = c.id;
        c.glare_timer = tm_.schedule(delay, [this, id] {
            Call* c = find(id);
            if (!c || c->state == CallState::Terminated) return;
            c->glare_timer = 0;
            Pending k = c->pending;
            c->pending = Pending::None;
            // Their re-INVITE may already have produced the state we wanted.
            if ((k == Pending::Hold && c->local_hold) || (k == Pending::Resume && !c->local_hold)) return;
            ++stats_.glare_retries;
            send_reinvite(*c, k);
        });
        return;
    }
    if (code == 481 || code == 408) {  // RFC 3261 12.2.1.2: the dialog is gone
        finish(c, "dialog gone (" + std::to_string(code) + ")");
        return;
    }
    c.reason = "re-INVITE failed (" + std::to_string(code) + ")";
    publish(c);
}

void UserAgent::on_timeout(TxnId ct) {
    auto it = client_.find(ct);
    if (it == client_.end()) return;
    const auto [cid, purpose] = it->second;
    client_.erase(it);
    if (purpose == Purpose::Register) {
        registered_ = false;
        if (on_register) on_register(false, 408);
        return;
    }
    Call* c = find(cid);
    if (!c) return;
    switch (purpose) {
        case Purpose::Invite: c->final_code = 408; finish(*c, "timeout"); break;
        case Purpose::ReInvite:
            if (ct == c->reinvite) {
                c->reinvite = 0;
                c->pending = Pending::None;
                finish(*c, "re-INVITE timeout");
            }
            break;
        case Purpose::Refer:
            if (c->state == CallState::Transferring) set_state(*c, c->pre_transfer, "transfer timeout");
            break;
        default: break;
    }
}

void UserAgent::on_stray_response(const Message& rsp, const Endpoint&) {
    // A retransmitted 2xx to an INVITE whose client transaction is gone: our ACK was lost.
    if (rsp.status < 200 || rsp.status >= 300 || rsp.cseq_method() != "INVITE") return;
    Call* c = find_dialog(rsp.call_id(), rsp.from_tag());
    if (c && !c->ack_wire.empty() && c->ack_cseq == rsp.cseq_num()) {
        ++stats_.ack_resends;
        tp_.send(c->ack_wire, c->remote_ep);
    }
}

// ------------------------------------------------------------------ introspection

std::optional<CallInfo> UserAgent::info(int id) const {
    auto it = calls_.find(id);
    if (it == calls_.end()) return std::nullopt;
    const Call& c = it->second;
    return CallInfo{c.id, c.dir, c.state, c.remote_uri, c.remote_hold, c.reason, c.referred_from};
}

std::vector<CallInfo> UserAgent::calls() const {
    std::vector<CallInfo> v;
    for (const auto& [id, c] : calls_) v.push_back(*info(id));
    return v;
}

std::vector<std::string> UserAgent::history(int id) const {
    auto it = calls_.find(id);
    return it == calls_.end() ? std::vector<std::string>{} : it->second.history;
}

}  // namespace ua
