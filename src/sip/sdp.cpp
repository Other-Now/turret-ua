#include "sip/sdp.hpp"

#include <sstream>

namespace sdp {

const char* to_string(Dir d) {
    switch (d) {
        case Dir::SendRecv: return "sendrecv";
        case Dir::SendOnly: return "sendonly";
        case Dir::RecvOnly: return "recvonly";
        case Dir::Inactive: return "inactive";
    }
    return "sendrecv";
}

std::string build(const Session& s, const std::string& user) {
    std::ostringstream o;
    o << "v=0\r\n"
      << "o=" << (user.empty() ? "-" : user) << " 1 " << s.version << " IN IP4 " << s.ip << "\r\n"
      << "s=turret-ua\r\n"
      << "c=IN IP4 " << s.ip << "\r\n"
      << "t=0 0\r\n"
      << "m=audio " << s.port << " RTP/AVP";
    for (int pt : s.payloads) o << ' ' << pt;
    o << "\r\n";
    for (int pt : s.payloads) {
        if (pt == 0) o << "a=rtpmap:0 PCMU/8000\r\n";
        if (pt == 101) o << "a=rtpmap:101 telephone-event/8000\r\n";
    }
    o << "a=ptime:20\r\n"
      << "a=" << to_string(s.dir) << "\r\n";
    return o.str();
}

std::optional<Session> parse(const std::string& body) {
    Session s;
    bool have_m = false, have_c = false;
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2 || line[1] != '=') continue;
        const std::string v = line.substr(2);
        switch (line[0]) {
            case 'c': {
                // c=IN IP4 1.2.3.4   (session- or media-level; last one wins, fine for one stream)
                auto sp = v.rfind(' ');
                if (sp == std::string::npos) return std::nullopt;
                s.ip = v.substr(sp + 1);
                have_c = true;
                break;
            }
            case 'm': {
                if (have_m) break;  // only the first stream matters to us
                std::istringstream ms(v);
                std::string media, proto;
                unsigned port = 0;
                if (!(ms >> media >> port >> proto) || port > 65535) return std::nullopt;
                if (media != "audio") break;
                s.port = static_cast<uint16_t>(port);
                int pt;
                while (ms >> pt) s.payloads.push_back(pt);
                have_m = true;
                break;
            }
            case 'a':
                if (v == "sendrecv") s.dir = Dir::SendRecv;
                else if (v == "sendonly") s.dir = Dir::SendOnly;
                else if (v == "recvonly") s.dir = Dir::RecvOnly;
                else if (v == "inactive") s.dir = Dir::Inactive;
                break;
            default: break;
        }
    }
    if (!have_m || !have_c) return std::nullopt;
    // RFC 3264 8.4 legacy hold: c=0.0.0.0 means "do not send to me"
    if (s.ip == "0.0.0.0" && s.dir == Dir::SendRecv) s.dir = Dir::SendOnly;
    return s;
}

Dir answer_dir(Dir offered, Dir local_wish) {
    // Mirror of the offer...
    Dir mirror = offered == Dir::SendOnly ? Dir::RecvOnly
               : offered == Dir::RecvOnly ? Dir::SendOnly
               : offered;
    // ...intersected with what we are willing to do.
    bool send = sends(mirror) && sends(local_wish);
    bool recv = recvs(mirror) && recvs(local_wish);
    if (send && recv) return Dir::SendRecv;
    if (send) return Dir::SendOnly;
    if (recv) return Dir::RecvOnly;
    return Dir::Inactive;
}

}  // namespace sdp
