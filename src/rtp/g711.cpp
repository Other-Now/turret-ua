#include "rtp/g711.hpp"

namespace rtp {

// Classic ITU-T G.711 mu-law: bias 0x84, clip at 32635, 8 segments.
uint8_t linear_to_ulaw(int16_t pcm) {
    constexpr int kBias = 0x84;
    constexpr int kClip = 32635;
    int sample = pcm;
    int sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > kClip) sample = kClip;
    sample += kBias;
    int exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) --exponent;
    int mantissa = (sample >> (exponent + 3)) & 0x0f;
    return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
}

int16_t ulaw_to_linear(uint8_t u) {
    u = static_cast<uint8_t>(~u);
    int sign = u & 0x80;
    int exponent = (u >> 4) & 0x07;
    int mantissa = u & 0x0f;
    int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
    return static_cast<int16_t>(sign ? -sample : sample);
}

}  // namespace rtp
