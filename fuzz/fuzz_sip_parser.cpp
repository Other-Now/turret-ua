// Parser fuzz target. Besides "does not crash under ASan/UBSan", it checks a
// round-trip property: anything parse() accepts must serialise to something
// parse() accepts again, and serialising that must be a fixed point.
#include <cstdint>
#include <cstdio>
#include <string>

#include "sip/message.hpp"
#include "sip/sdp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string in(reinterpret_cast<const char*>(data), size);
    auto m = sip::parse(in);
    if (!m) return 0;
    // exercise accessors
    (void)m->branch(); (void)m->from_tag(); (void)m->to_tag(); (void)m->cseq_num();
    if (auto* c = m->get("Contact")) (void)sip::uri_hostport(sip::addr_uri(*c));
    (void)sdp::parse(m->body);

    const std::string s1 = m->serialize();
    std::string err;
    auto m2 = sip::parse(s1, &err);
    if (!m2) {
        std::fprintf(stderr, "re-parse failed: %s\n---\n%s\n---\n", err.c_str(), s1.c_str());
        __builtin_trap();
    }
    if (m2->serialize() != s1) {
        std::fprintf(stderr, "serialise not a fixed point\n%s\n---\n%s\n", s1.c_str(), m2->serialize().c_str());
        __builtin_trap();
    }
    return 0;
}
