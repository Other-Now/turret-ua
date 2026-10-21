// jb-sweep: offline, deterministic jitter-buffer experiment.
//
// Runs the real rtp::JitterBuffer and rtp::ReceiverStats over simulated arrival
// times (60 s of 20 ms G.711 frames, 1% random loss, 10 seeds per cell) for
// several network jitter models, and reports for each playout depth:
//   late%      packets that arrived after their playout slot (discarded)
//   unplayed%  late + network loss = what the listener actually misses
// plus the RFC 3550 jitter estimate J (mean over the run) that RTCP RRs carry,
// and the smallest depth that keeps late loss under 1%, expressed in units of J.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "rtp/jitter_buffer.hpp"
#include "rtp/rtp.hpp"

namespace {

struct Model {
    std::string name;
    double sd_ms;        // gaussian jitter (folded to >= 0)
    double spike_p;      // probability of a delay spike
    double spike_ms;     // spike size (uniform 0.5x..1x)
};

struct Cell {
    double late_pct = 0, unplayed_pct = 0, j_ms = 0;
};

Cell run(const Model& m, uint32_t depth_ms, unsigned seed) {
    constexpr int kPackets = 3000;  // 60 s
    constexpr double kLoss = 0.01, kBase = 20.0;
    std::mt19937 rng(seed);
    std::normal_distribution<double> norm(0, m.sd_ms > 0 ? m.sd_ms : 1);
    std::uniform_real_distribution<double> uni(0, 1);

    struct Arrival { uint64_t at_us; uint16_t seq; uint32_t ts; };
    std::vector<Arrival> arr;
    for (int i = 0; i < kPackets; ++i) {
        if (uni(rng) < kLoss) continue;
        double d = kBase + (m.sd_ms > 0 ? std::fabs(norm(rng)) : 0);
        if (uni(rng) < m.spike_p) d += m.spike_ms * (0.5 + 0.5 * uni(rng));
        arr.push_back({static_cast<uint64_t>((i * 20.0 + d) * 1000), static_cast<uint16_t>(i), uint32_t(i) * 160});
    }
    std::sort(arr.begin(), arr.end(), [](const Arrival& a, const Arrival& b) { return a.at_us < b.at_us; });

    // Times are in microseconds; the buffer only compares them, so pass depth in us too.
    rtp::JitterBuffer jb(depth_ms * 1000);
    rtp::ReceiverStats st;
    uint8_t payload[160] = {};
    size_t next = 0;
    uint64_t play_at = 0;
    double j_sum = 0;
    int frames = 0;
    while (frames < kPackets) {
        if (next < arr.size() && (!jb.started() || arr[next].at_us <= play_at)) {
            const auto& a = arr[next++];
            st.on_packet(a.seq, a.ts, static_cast<uint32_t>(a.at_us * 8 / 1000));
            j_sum += st.jitter_ms();
            bool first = !jb.started();
            jb.push(a.seq, payload, sizeof payload, a.at_us);
            if (first) play_at = jb.playout_start_ms();
            continue;
        }
        if (!jb.started()) break;
        jb.pop(play_at);
        play_at += 20000;
        ++frames;
    }
    const auto& s = jb.stats();
    Cell c;
    c.late_pct = 100.0 * double(s.late) / kPackets;
    c.unplayed_pct = 100.0 * double(kPackets - s.played) / kPackets;
    c.j_ms = arr.empty() ? 0 : j_sum / arr.size();  // mean of J over the run (RTCP would sample it)
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<Model> models = {
        {"lan (sd 2 ms)", 2, 0, 0},
        {"wan (sd 10 ms)", 10, 0, 0},
        {"congested (sd 25 ms)", 25, 0, 0},
        {"wifi spikes (sd 5 + 2% x150 ms)", 5, 0.02, 150},
    };
    const std::vector<uint32_t> depths = {20, 40, 60, 80, 100, 140, 200};
    const int seeds = 10;
    std::FILE* csv = argc > 1 ? std::fopen(argv[1], "w") : nullptr;
    if (csv) std::fprintf(csv, "model,depth_ms,late_pct,unplayed_pct,rfc3550_j_ms\n");

    std::printf("60 s per run, 1%% network loss, %d seeds per cell (mean)\n\n", seeds);
    for (const auto& m : models) {
        std::printf("### %s\n\n| depth ms | late %% | unplayed %% (late + lost) | mean RFC 3550 J ms |\n|---:|---:|---:|---:|\n",
                    m.name.c_str());
        double j_ms = 0;
        uint32_t need = 0;
        for (uint32_t d : depths) {
            Cell avg;
            for (int s = 1; s <= seeds; ++s) {
                Cell c = run(m, d, static_cast<unsigned>(s * 101 + d));
                avg.late_pct += c.late_pct / seeds;
                avg.unplayed_pct += c.unplayed_pct / seeds;
                avg.j_ms += c.j_ms / seeds;
            }
            j_ms = avg.j_ms;
            if (!need && avg.late_pct < 1.0) need = d;
            std::printf("| %u | %.2f | %.2f | %.1f |\n", d, avg.late_pct, avg.unplayed_pct, avg.j_ms);
            if (csv) std::fprintf(csv, "%s,%u,%.3f,%.3f,%.2f\n", m.name.c_str(), d, avg.late_pct, avg.unplayed_pct, avg.j_ms);
        }
        if (need) std::printf("\nsmallest depth with late < 1%%: %u ms = %.1f x J\n\n", need, need / j_ms);
        else std::printf("\nno depth up to %u ms keeps late < 1%%\n\n", depths.back());
    }
    if (csv) std::fclose(csv);
    return 0;
}
