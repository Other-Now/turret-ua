#include "sip/message.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <random>

namespace sip {
namespace {

constexpr size_t kMaxHeaders = 128;
constexpr size_t kMaxMessage = 64 * 1024;

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

struct NameMap {
    const char* compact;
    const char* canonical;
};
constexpr std::array<NameMap, 14> kCompact{{
    {"i", "Call-ID"},       {"m", "Contact"},     {"e", "Content-Encoding"},
    {"l", "Content-Length"}, {"c", "Content-Type"}, {"f", "From"},
    {"s", "Subject"},       {"k", "Supported"},   {"t", "To"},
    {"v", "Via"},           {"r", "Refer-To"},    {"o", "Event"},
    {"b", "Referred-By"},   {"u", "Allow-Events"},
}};
constexpr std::array<const char*, 24> kKnown{{
    "Via", "From", "To", "Call-ID", "CSeq", "Contact", "Max-Forwards", "Content-Type",
    "Content-Length", "Allow", "Supported", "Refer-To", "Referred-By", "Event",
    "Subscription-State", "Expires", "WWW-Authenticate", "Authorization",
    "Proxy-Authenticate", "Proxy-Authorization", "User-Agent", "Retry-After",
    "Record-Route", "Route",
}};

bool is_token_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || std::string_view("-.!%*_+`'~").find(c) != std::string_view::npos;
}

std::mt19937_64& rng() {
    thread_local std::mt19937_64 r{std::random_device{}()};
    return r;
}

}  // namespace

std::string canonical_name(std::string_view name) {
    if (name.size() == 1)
        for (const auto& m : kCompact)
            if (iequals(name, m.compact)) return m.canonical;
    for (const char* k : kKnown)
        if (iequals(name, k)) return k;
    return std::string(name);
}

const std::string* Message::get(std::string_view name) const {
    const std::string canon = canonical_name(name);
    for (const auto& h : headers)
        if (iequals(h.name, canon)) return &h.value;
    return nullptr;
}

std::vector<std::string> Message::get_all(std::string_view name) const {
    const std::string canon = canonical_name(name);
    std::vector<std::string> out;
    for (const auto& h : headers)
        if (iequals(h.name, canon)) out.push_back(h.value);
    return out;
}

void Message::set(std::string_view name, std::string value) {
    remove(name);
    headers.push_back({canonical_name(name), std::move(value)});
}

void Message::add(std::string_view name, std::string value) {
    headers.push_back({canonical_name(name), std::move(value)});
}

void Message::remove(std::string_view name) {
    const std::string canon = canonical_name(name);
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [&](const Header& h) { return iequals(h.name, canon); }),
                  headers.end());
}

std::string Message::serialize() const {
    std::string out;
    out.reserve(512 + body.size());
    if (is_request) {
        out += method; out += ' '; out += uri; out += " SIP/2.0\r\n";
    } else {
        out += "SIP/2.0 "; out += std::to_string(status); out += ' '; out += reason; out += "\r\n";
    }
    for (const auto& h : headers) {
        if (iequals(h.name, "Content-Length")) continue;
        out += h.name; out += ": "; out += h.value; out += "\r\n";
    }
    out += "Content-Length: "; out += std::to_string(body.size()); out += "\r\n\r\n";
    out += body;
    return out;
}

std::string Message::call_id() const {
    auto* v = get("Call-ID");
    return v ? *v : std::string();
}

uint32_t Message::cseq_num() const {
    auto* v = get("CSeq");
    if (!v) return 0;
    uint32_t n = 0;
    std::from_chars(v->data(), v->data() + v->size(), n);
    return n;
}

std::string Message::cseq_method() const {
    auto* v = get("CSeq");
    if (!v) return {};
    auto sp = v->find(' ');
    return sp == std::string::npos ? std::string() : std::string(trim(std::string_view(*v).substr(sp + 1)));
}

std::string Message::top_via() const {
    auto* v = get("Via");
    if (!v) return {};
    // A single Via header line may hold several comma-separated values.
    auto comma = v->find(',');
    return std::string(trim(std::string_view(*v).substr(0, comma)));
}

std::string Message::branch() const { return header_param(top_via(), "branch"); }

std::string Message::from_tag() const {
    auto* v = get("From");
    return v ? header_param(*v, "tag") : std::string();
}

std::string Message::to_tag() const {
    auto* v = get("To");
    return v ? header_param(*v, "tag") : std::string();
}

std::string header_param(std::string_view value, std::string_view name) {
    // Skip anything inside <...> (URI parameters) and "..." (display names).
    size_t i = 0;
    bool in_angle = false, in_quote = false;
    while (i < value.size()) {
        char c = value[i];
        if (in_quote) { if (c == '\\') ++i; else if (c == '"') in_quote = false; ++i; continue; }
        if (c == '"') { in_quote = true; ++i; continue; }
        if (c == '<') { in_angle = true; ++i; continue; }
        if (c == '>') { in_angle = false; ++i; continue; }
        if (c == ';' && !in_angle) {
            size_t start = i + 1;
            size_t end = start;
            while (end < value.size() && value[end] != ';' && value[end] != ',') ++end;
            std::string_view p = trim(value.substr(start, end - start));
            auto eq = p.find('=');
            std::string_view key = trim(p.substr(0, eq));
            if (iequals(key, name)) return eq == std::string_view::npos ? std::string() : std::string(trim(p.substr(eq + 1)));
            i = end;
            continue;
        }
        ++i;
    }
    return {};
}

std::string addr_uri(std::string_view na) {
    auto lt = na.find('<');
    if (lt != std::string_view::npos) {
        auto gt = na.find('>', lt);
        if (gt == std::string_view::npos) return {};
        return std::string(na.substr(lt + 1, gt - lt - 1));
    }
    // addr-spec form: params after ';' belong to the header, not the URI
    na = trim(na);
    return std::string(na.substr(0, na.find(';')));
}

std::optional<HostPort> uri_hostport(std::string_view uri) {
    if (uri.size() < 4 || !iequals(uri.substr(0, 4), "sip:")) return std::nullopt;
    uri.remove_prefix(4);
    auto at = uri.find('@');
    if (at != std::string_view::npos) uri.remove_prefix(at + 1);
    auto end = uri.find_first_of(";?>");
    uri = uri.substr(0, end);
    HostPort hp;
    auto colon = uri.rfind(':');
    if (colon != std::string_view::npos) {
        unsigned p = 0;
        auto sv = uri.substr(colon + 1);
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), p);
        if (ec != std::errc() || ptr != sv.data() + sv.size() || p == 0 || p > 65535) return std::nullopt;
        hp.port = static_cast<uint16_t>(p);
        uri = uri.substr(0, colon);
    }
    if (uri.empty()) return std::nullopt;
    hp.host = std::string(uri);
    return hp;
}

std::string uri_user(std::string_view uri) {
    if (uri.size() < 4) return {};
    uri.remove_prefix(4);
    auto at = uri.find('@');
    return at == std::string_view::npos ? std::string() : std::string(uri.substr(0, at));
}

std::string random_token(size_t n) {
    static constexpr char kAlpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string s(n, '0');
    for (auto& c : s) c = kAlpha[rng()() % 36];
    return s;
}

std::string new_branch() { return "z9hG4bK" + random_token(12); }

const char* reason_phrase(int status) {
    switch (status) {
        case 100: return "Trying";
        case 180: return "Ringing";
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 481: return "Call/Transaction Does Not Exist";
        case 486: return "Busy Here";
        case 487: return "Request Terminated";
        case 488: return "Not Acceptable Here";
        case 491: return "Request Pending";
        case 500: return "Server Internal Error";
        case 603: return "Decline";
        default: return status < 300 ? "OK" : "Error";
    }
}

std::optional<Message> parse(std::string_view data, std::string* err) {
    auto fail = [&](const char* why) -> std::optional<Message> {
        if (err) *err = why;
        return std::nullopt;
    };
    if (data.size() > kMaxMessage) return fail("message too large");
    // RFC 3261 7.5: ignore leading CRLFs (keep-alives)
    while (data.size() >= 2 && data[0] == '\r' && data[1] == '\n') data.remove_prefix(2);

    auto hdr_end = data.find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) return fail("no end of headers");
    std::string_view head = data.substr(0, hdr_end + 2);
    std::string_view rest = data.substr(hdr_end + 4);

    Message m;
    // ---- start line ----
    auto eol = head.find("\r\n");
    std::string_view start = head.substr(0, eol);
    head.remove_prefix(eol + 2);
    if (start.substr(0, 8) == "SIP/2.0 ") {
        m.is_request = false;
        std::string_view s = start.substr(8);
        if (s.size() < 3) return fail("short status line");
        int code = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + 3, code);
        if (ec != std::errc() || p != s.data() + 3 || code < 100 || code > 699) return fail("bad status code");
        if (s.size() > 3 && s[3] != ' ') return fail("bad status line");
        m.status = code;
        m.reason = s.size() > 4 ? std::string(s.substr(4)) : std::string();
    } else {
        auto sp1 = start.find(' ');
        auto sp2 = start.rfind(' ');
        if (sp1 == std::string_view::npos || sp1 == sp2) return fail("bad request line");
        m.method = std::string(start.substr(0, sp1));
        m.uri = std::string(start.substr(sp1 + 1, sp2 - sp1 - 1));
        if (start.substr(sp2 + 1) != "SIP/2.0") return fail("bad SIP version");
        if (m.method.empty() || !std::all_of(m.method.begin(), m.method.end(), is_token_char))
            return fail("bad method");
        if (m.uri.empty() || m.uri.find(' ') != std::string::npos) return fail("bad request-uri");
    }

    // ---- headers (with RFC 3261 7.3.1 line folding) ----
    while (!head.empty()) {
        eol = head.find("\r\n");
        std::string_view line = head.substr(0, eol);
        head.remove_prefix(eol + 2);
        if (line.empty()) continue;
        // RFC 3261 25.1: header values are TEXT-UTF8 / LWS - no control characters.
        // A raw NUL here would truncate the value in anything that treats it as a
        // C string (logs, a DB, a peer's parser). Found by fuzz_sip_parser.
        for (char ch : line) {
            auto u = static_cast<unsigned char>(ch);
            if ((u < 0x20 && u != '\t') || u == 0x7f) return fail("control character in header");
        }
        if (line[0] == ' ' || line[0] == '\t') {
            if (m.headers.empty()) return fail("continuation before first header");
            // A fold is equivalent to one SP, but leading whitespace of a value is
            // not part of it: "Call-ID:\r\n abc" is Call-ID "abc", not " abc"
            // (found by fuzz_sip_parser's round-trip check).
            std::string& v = m.headers.back().value;
            std::string_view cont = trim(line);
            if (cont.empty()) continue;  // whitespace-only fold adds nothing (fuzz finding #2)
            if (!v.empty()) v += ' ';
            v += cont;
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string_view::npos) return fail("header without colon");
        std::string_view name = trim(line.substr(0, colon));
        if (name.empty() || !std::all_of(name.begin(), name.end(), is_token_char)) return fail("bad header name");
        std::string canon = canonical_name(name);
        if (canon == "Content-Length") {
            // Two lengths make the body boundary ambiguous (the SIP twin of HTTP
            // request smuggling): reject rather than pick one.
            if (m.get("Content-Length")) return fail("duplicate Content-Length");
        } else if (m.headers.size() >= kMaxHeaders - 1) {
            // serialize() always writes one Content-Length, so leave room for it;
            // otherwise our own output could exceed our own limit (fuzz finding #3).
            return fail("too many headers");
        }
        m.headers.push_back({std::move(canon), std::string(trim(line.substr(colon + 1)))});
    }

    // ---- mandatory headers (RFC 3261 8.1.1) ----
    for (const char* h : {"Via", "From", "To", "Call-ID", "CSeq"})
        if (!m.get(h)) return fail("missing mandatory header");
    if (m.call_id().empty()) return fail("empty Call-ID");
    {
        const std::string& cs = *m.get("CSeq");
        auto sp = cs.find(' ');
        if (sp == std::string::npos || sp == 0 || sp > 10) return fail("bad CSeq");
        for (size_t i = 0; i < sp; ++i)
            if (!std::isdigit(static_cast<unsigned char>(cs[i]))) return fail("bad CSeq number");
        uint64_t n = std::stoull(cs.substr(0, sp));
        if (n >= (1ull << 31)) return fail("CSeq out of range");  // RFC 3261 8.1.1.5
        if (m.cseq_method().empty()) return fail("bad CSeq method");
        if (m.is_request && m.cseq_method() != m.method) return fail("CSeq method mismatch");
    }
    if (m.branch().empty()) return fail("Via without branch");

    // ---- body ----
    if (auto* cl = m.get("Content-Length")) {
        size_t n = 0;
        auto [p, ec] = std::from_chars(cl->data(), cl->data() + cl->size(), n);
        if (ec != std::errc() || p != cl->data() + cl->size()) return fail("bad Content-Length");
        if (n > rest.size()) return fail("body shorter than Content-Length");
        rest = rest.substr(0, n);  // UDP: extra bytes are ignored (RFC 3261 18.3)
    }
    m.body = std::string(rest);
    return m;
}

}  // namespace sip
