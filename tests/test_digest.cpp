#include <gtest/gtest.h>

#include "sip/digest.hpp"

using namespace sip;

TEST(Digest, Md5Vectors) {  // RFC 1321 appendix A.5
    EXPECT_EQ(md5_hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(md5_hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(md5_hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    EXPECT_EQ(md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
              "57edf4a22be3c955ac49da2e2107b67a");
}

TEST(Digest, Rfc2617Example) {
    // RFC 2617 section 3.5
    std::string ch = "Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", "
                     "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";
    std::string v = digest_authorization(ch, {"Mufasa", "Circle Of Life"}, "GET", "/dir/index.html", 1, "0a4f113b");
    EXPECT_NE(v.find("response=\"6629fae49393a05397450978507c4ef1\""), std::string::npos) << v;
    EXPECT_NE(v.find("opaque=\"5ccc069c403ebaf9f0171e9517f40e41\""), std::string::npos);
    EXPECT_NE(v.find("nc=00000001"), std::string::npos);
}

TEST(Digest, NoQop) {
    std::string ch = "Digest realm=\"asterisk\", nonce=\"abc\"";
    std::string v = digest_authorization(ch, {"bob", "pw"}, "REGISTER", "sip:127.0.0.1");
    std::string ha1 = md5_hex("bob:asterisk:pw");
    std::string ha2 = md5_hex("REGISTER:sip:127.0.0.1");
    EXPECT_NE(v.find("response=\"" + md5_hex(ha1 + ":abc:" + ha2) + "\""), std::string::npos);
    EXPECT_EQ(v.find("qop"), std::string::npos);
}
