// Minimal SDP (RFC 4566) for one G.711 audio stream, with the RFC 3264
// offer/answer direction rules that hold/resume depend on.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sdp {

enum class Dir { SendRecv, SendOnly, RecvOnly, Inactive };
const char* to_string(Dir d);

struct Session {
    std::string ip;           // c= address
    uint16_t port = 0;        // m=audio port (0 = stream rejected)
    std::vector<int> payloads;// m= payload types, preference order
    Dir dir = Dir::SendRecv;
    uint64_t version = 1;     // o= version, bumped on every new offer
};

std::string build(const Session& s, const std::string& user);
std::optional<Session> parse(const std::string& body);

// RFC 3264 6.1: the answer's direction is the mirror of the offer's, limited by
// what we want locally (e.g. we are on hold ourselves).
Dir answer_dir(Dir offered, Dir local_wish);

// Does this side send / receive media in the given direction?
inline bool sends(Dir d) { return d == Dir::SendRecv || d == Dir::SendOnly; }
inline bool recvs(Dir d) { return d == Dir::SendRecv || d == Dir::RecvOnly; }

}  // namespace sdp
