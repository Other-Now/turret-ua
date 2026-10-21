// HTTP Digest (RFC 2617, MD5, qop=auth) as SIP uses it for REGISTER/INVITE
// challenges (401 WWW-Authenticate / 407 Proxy-Authenticate).
#pragma once
#include <string>

namespace sip {

std::string md5_hex(const std::string& data);

struct Credentials {
    std::string user;
    std::string password;
};

// Builds the Authorization / Proxy-Authorization value answering `challenge`
// (the WWW-Authenticate value). `nc` is the nonce count for qop=auth.
std::string digest_authorization(const std::string& challenge, const Credentials& cred,
                                 const std::string& method, const std::string& uri,
                                 unsigned nc = 1, const std::string& cnonce = "");

}  // namespace sip
