// Writes every datagram the app sends or receives into a pcap file with a
// synthesised IPv4/UDP header (LINKTYPE_RAW), so tshark/Wireshark can decode
// the SIP ladder and RTP streams without root (no capture privileges in WSL).
#pragma once
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/interfaces.hpp"

namespace util {

class PcapWriter {
public:
    explicit PcapWriter(const std::string& path);
    ~PcapWriter();
    bool ok() const { return f_ != nullptr; }
    void write(const core::Endpoint& src, const core::Endpoint& dst, const void* data, size_t n);

private:
    std::FILE* f_ = nullptr;
    std::mutex mu_;
    uint16_t ip_id_ = 0;
};

}  // namespace util
