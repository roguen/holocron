// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/sample_convert.hpp
//
// Float to device-integer conversion, and the argument that it is lossless.
//
// THIS HEADER IS THE ANSWER TO ISSUE #36
//
// #36 asked whether AudioSink needs an integer passthrough for genuinely
// bit-perfect output, or whether a tested float round-trip suffices. The answer
// taken is the float round-trip, and it rests on a property that is provable
// rather than hopeful:
//
//   IEEE-754 binary32 has a 24-bit significand (23 stored bits plus the
//   implicit leading one). Every integer of magnitude <= 2^24 is therefore
//   representable EXACTLY. Scaling by a power of two changes only the exponent
//   and cannot perturb the significand.
//
// So for a 16- or 24-bit integer source:
//
//   float f = static_cast<float>(s) / 2^(bits-1);     exact
//   int   s = llround(f * 2^(bits-1));                recovers s exactly
//
// and the whole decode -> analysis-tap -> sink path can carry float without the
// output ceasing to be bit-perfect. tests/test_sample_convert.cpp proves it by
// exhaustion for 16-bit and by exhaustion over the full 24-bit range too --
// every value, not a sample of them, because "we tried a thousand and they were
// fine" is not the same claim.
//
// WHERE IT STOPS BEING TRUE, STATED RATHER THAN GLOSSED
//
// 32-bit INTEGER sources do not round-trip. 2^31 exceeds the 24-bit significand,
// so the bottom eight bits are lost on the way into float and cannot be
// recovered. That is a real limit of this decision and it is why the test pins
// it as a KNOWN failure rather than quietly testing only the cases that pass.
//
// In practice it does not bite: 32-bit integer PCM is vanishingly rare in music
// (24-bit is the studio ceiling, and 32-bit float files are float already, so
// they carry no integer to lose). If a 32-bit integer source ever matters, #36
// is reopened rather than worked around -- the passthrough path it proposed is
// the correct fix and this header is not a substitute for it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace holocron {

// The scale factor for a given integer depth. A power of two on purpose: any
// other scale (the common 32767.0f for 16-bit, say) is not exactly
// representable as a ratio and destroys the round-trip this header exists to
// guarantee.
constexpr float int_scale(int bits)
{
    // 2^(bits-1), built by repeated doubling so this is constexpr-clean and
    // obviously a power of two rather than a call to std::pow.
    float s = 1.0f;
    for (int i = 0; i < bits - 1; ++i) {
        s *= 2.0f;
    }
    return s;
}

// Integer sample -> float. Exact for bits <= 24.
constexpr float to_float(std::int32_t sample, int bits)
{
    return static_cast<float>(sample) / int_scale(bits);
}

// Float -> integer sample, clamped to the representable range.
//
// The asymmetry is deliberate and is not an off-by-one. Two's complement runs
// from -2^(bits-1) to +2^(bits-1)-1, so dividing by 2^(bits-1) maps the most
// negative sample to exactly -1.0 and the most positive to slightly under +1.0.
// Multiplying back and clamping at +2^(bits-1)-1 therefore loses nothing that
// was ever in the source: no real sample lands above the clamp. Only signal
// that was already outside the integer range is affected, and that signal was
// not bit-perfect to begin with.
inline std::int32_t from_float(float value, int bits)
{
    const float        scale = int_scale(bits);
    const std::int32_t lo    = -static_cast<std::int32_t>(scale);
    const std::int32_t hi    = static_cast<std::int32_t>(scale) - 1;

    // std::lround, not a cast: a cast truncates toward zero, which would turn
    // the exact value 3.0f into 3 but the exact value -3.0f into -3 only by
    // luck of sign, and would break the round-trip for every negative sample
    // whose float form carries any representation slack at all.
    const long r = std::lround(static_cast<double>(value) * static_cast<double>(scale));

    if (r < static_cast<long>(lo)) {
        return lo;
    }
    if (r > static_cast<long>(hi)) {
        return hi;
    }
    return static_cast<std::int32_t>(r);
}

// Write one sample into a packed 24-bit little-endian field, which is what
// WASAPI wants for a 24-bit exclusive-mode stream and what no standard integer
// type describes.
inline void write_int24(unsigned char* dst, std::int32_t sample)
{
    dst[0] = static_cast<unsigned char>(static_cast<std::uint32_t>(sample) & 0xFFu);
    dst[1] = static_cast<unsigned char>((static_cast<std::uint32_t>(sample) >> 8) & 0xFFu);
    dst[2] = static_cast<unsigned char>((static_cast<std::uint32_t>(sample) >> 16) & 0xFFu);
}

// Read one packed 24-bit little-endian field back, sign-extending.
inline std::int32_t read_int24(const unsigned char* src)
{
    const std::uint32_t raw = static_cast<std::uint32_t>(src[0]) |
                              (static_cast<std::uint32_t>(src[1]) << 8) |
                              (static_cast<std::uint32_t>(src[2]) << 16);
    // Sign-extend bit 23 without relying on implementation-defined behaviour of
    // shifting a signed value.
    if ((raw & 0x800000u) != 0) {
        return static_cast<std::int32_t>(raw | 0xFF000000u);
    }
    return static_cast<std::int32_t>(raw);
}

}  // namespace holocron
