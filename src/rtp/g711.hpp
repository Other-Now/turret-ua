// G.711 mu-law (PCMU, RTP payload type 0): 8 kHz, 8 bits/sample, 64 kbit/s.
#pragma once
#include <cstdint>

namespace rtp {

uint8_t linear_to_ulaw(int16_t pcm);
int16_t ulaw_to_linear(uint8_t u);

}  // namespace rtp
