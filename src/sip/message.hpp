// SIP message model + parser (RFC 3261 section 7).
// Text in, struct out. No allocation tricks: SIP is a control protocol and a few
// hundred messages per call is nothing; correctness and strictness matter more.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sip {

struct Header {
    std::string name;   // canonical long form, e.g. "Call-ID"
    std::string value;  // trimmed, folded continuation lines joined with one space
};

struct Message {
    bool is_request = true;
    // request line
    std::string method;
    std::string uri;
    // status line
    int status = 0;
    std::string reason;

    std::vector<Header> headers;
    std::string body;

    // Header access is case-insensitive and understands compact forms (v, f, t, i, ...).
    const std::string* get(std::string_view name) const;
    std::vector<std::string> get_all(std::string_view name) const;
    void set(std::string_view name, std::string value);  // replace all, or append
    void add(std::string_view name, std::string value);  // append (e.g. a second Via)
    void remove(std::string_view name);

    // Serialises with a correct Content-Length (the stored one is ignored).
    std::string serialize() const;

    // Convenience accessors. They assume the message passed parse()'s validation.
    std::string call_id() const;
    uint32_t cseq_num() const;
    std::string cseq_method() const;
    std::string top_via() const;
    std::string branch() const;    // branch param of the top Via
    std::string from_tag() const;
    std::string to_tag() const;
};

// Parses one datagram. Returns nullopt (and fills *err) on anything malformed:
// bad start line, header without ':', missing mandatory header, CSeq method that
// does not match the request method, body shorter than Content-Length, ...
std::optional<Message> parse(std::string_view data, std::string* err = nullptr);

// ---- small helpers shared by the transaction and UA layers ----

// Canonical header name: expands compact forms, fixes case for known headers.
std::string canonical_name(std::string_view name);

// Value of ";name=value" parameter inside a header value ("" if absent).
// Only looks at parameters outside <...>, so URI params do not leak in.
std::string header_param(std::string_view value, std::string_view name);

// "Bob" <sip:bob@1.2.3.4:5060>;tag=x  ->  sip:bob@1.2.3.4:5060
std::string addr_uri(std::string_view name_addr);

struct HostPort {
    std::string host;
    uint16_t port = 5060;
};
// sip:user@host:port;params -> {host, port}. nullopt if not a sip: URI.
std::optional<HostPort> uri_hostport(std::string_view uri);
std::string uri_user(std::string_view uri);

// Random tokens for tags and branches. Branches carry the RFC 3261 magic cookie.
std::string random_token(size_t n = 10);
std::string new_branch();

const char* reason_phrase(int status);

}  // namespace sip
