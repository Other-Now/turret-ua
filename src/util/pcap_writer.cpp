#include "util/pcap_writer.hpp"

#include <arpa/inet.h>
#include <sys/time.h>

#include <cstring>
#include <vector>

namespace util {

PcapWriter::PcapWriter(const std::string& path) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return;
    struct {
        uint32_t magic = 0xa1b2c3d4;
        uint16_t vmaj = 2, vmin = 4;
        int32_t zone = 0;
        uint32_t sigfigs = 0, snaplen = 65535, linktype = 101;  // LINKTYPE_RAW
    } hdr;
    std::fwrite(&hdr, sizeof hdr, 1, f_);
}

PcapWriter::~PcapWriter() {
    if (f_) std::fclose(f_);
}

void PcapWriter::write(const core::Endpoint& src, const core::Endpoint& dst, const void* data, size_t n) {
    if (!f_ || n > 65000) return;
    std::vector<uint8_t> pkt(28 + n);
    uint8_t* ip = pkt.data();
    ip[0] = 0x45;
    uint16_t total = htons(static_cast<uint16_t>(pkt.size()));
    std::memcpy(ip + 2, &total, 2);
    ip[8] = 64;
    ip[9] = 17;  // UDP
    in_addr s{}, d{};
    inet_pton(AF_INET, src.host.c_str(), &s);
    inet_pton(AF_INET, dst.host.c_str(), &d);
    std::memcpy(ip + 12, &s, 4);
    std::memcpy(ip + 16, &d, 4);
    uint8_t* udp = ip + 20;
    uint16_t sp = htons(src.port), dp = htons(dst.port), ul = htons(static_cast<uint16_t>(8 + n));
    std::memcpy(udp, &sp, 2);
    std::memcpy(udp + 2, &dp, 2);
    std::memcpy(udp + 4, &ul, 2);  // checksum 0 = none (allowed for UDP over IPv4)
    std::memcpy(udp + 8, data, n);

    std::lock_guard<std::mutex> lk(mu_);
    uint16_t id = htons(ip_id_++);
    std::memcpy(ip + 4, &id, 2);
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += (ip[i] << 8) | ip[i + 1];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16_t csum = htons(static_cast<uint16_t>(~sum));
    std::memcpy(ip + 10, &csum, 2);

    timeval tv;
    gettimeofday(&tv, nullptr);
    uint32_t rec[4] = {static_cast<uint32_t>(tv.tv_sec), static_cast<uint32_t>(tv.tv_usec),
                       static_cast<uint32_t>(pkt.size()), static_cast<uint32_t>(pkt.size())};
    std::fwrite(rec, sizeof rec, 1, f_);
    std::fwrite(pkt.data(), 1, pkt.size(), f_);
}

}  // namespace util
