#include "sip/digest.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sip/message.hpp"

namespace sip {
namespace {

// Straight RFC 1321 MD5. Only used for digest auth, so speed is irrelevant.
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;

    static uint32_t rotl(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | (uint32_t(p[i * 4 + 3]) << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t F;
            int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3 * i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7 * i) % 16; }
            F += A + K[i] + M[g];
            A = D; D = C; C = B;
            B += rotl(F, S[i]);
        }
        a += A; b += B; c += C; d += D;
    }
};

std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

// Digest params are comma separated: Digest realm="x", nonce="y", qop="auth"
std::string challenge_param(const std::string& ch, const std::string& key) {
    size_t pos = 0;
    while (pos < ch.size()) {
        size_t eq = ch.find('=', pos);
        if (eq == std::string::npos) break;
        size_t ks = ch.find_last_of(" ,", eq);
        ks = ks == std::string::npos ? 0 : ks + 1;
        std::string k = ch.substr(ks, eq - ks);
        size_t vs = eq + 1, ve;
        if (vs < ch.size() && ch[vs] == '"') {
            ve = ch.find('"', vs + 1);
            ve = ve == std::string::npos ? ch.size() : ve + 1;
        } else {
            ve = ch.find(',', vs);
            if (ve == std::string::npos) ve = ch.size();
        }
        if (k == key) return unquote(ch.substr(vs, ve - vs));
        pos = ve;
    }
    return {};
}

}  // namespace

std::string md5_hex(const std::string& data) {
    Md5 h;
    std::string msg = data;
    uint64_t bits = uint64_t(data.size()) * 8;
    msg += char(0x80);
    while (msg.size() % 64 != 56) msg += char(0);
    for (int i = 0; i < 8; ++i) msg += char((bits >> (8 * i)) & 0xff);
    for (size_t off = 0; off < msg.size(); off += 64) h.block(reinterpret_cast<const uint8_t*>(msg.data() + off));
    char out[33];
    uint32_t w[4] = {h.a, h.b, h.c, h.d};
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", (w[i / 4] >> (8 * (i % 4))) & 0xff);
    return std::string(out, 32);
}

std::string digest_authorization(const std::string& challenge, const Credentials& cred,
                                 const std::string& method, const std::string& uri, unsigned nc,
                                 const std::string& cnonce_in) {
    const std::string realm = challenge_param(challenge, "realm");
    const std::string nonce = challenge_param(challenge, "nonce");
    const std::string opaque = challenge_param(challenge, "opaque");
    const std::string qop_opts = challenge_param(challenge, "qop");
    const bool qop_auth = qop_opts.find("auth") != std::string::npos;

    const std::string ha1 = md5_hex(cred.user + ":" + realm + ":" + cred.password);
    const std::string ha2 = md5_hex(method + ":" + uri);
    char ncs[9];
    std::snprintf(ncs, sizeof ncs, "%08x", nc);
    const std::string cnonce = cnonce_in.empty() ? random_token(16) : cnonce_in;

    std::string response = qop_auth ? md5_hex(ha1 + ":" + nonce + ":" + ncs + ":" + cnonce + ":auth:" + ha2)
                                    : md5_hex(ha1 + ":" + nonce + ":" + ha2);
    std::string v = "Digest username=\"" + cred.user + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
                    "\", uri=\"" + uri + "\", response=\"" + response + "\", algorithm=MD5";
    if (qop_auth) v += ", qop=auth, nc=" + std::string(ncs) + ", cnonce=\"" + cnonce + "\"";
    if (!opaque.empty()) v += ", opaque=\"" + opaque + "\"";
    return v;
}

}  // namespace sip
