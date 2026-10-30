// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Pipeline key canonicalization: depth bias values the PSO cannot tell apart give one key.
// Build: cl /std:c++20 /EHsc /Isrc tools\xbox\tests\key-canon.cpp

#undef NDEBUG
#include <bit>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include "video_core/renderer_d3d12/d3d12_key_canon.h"

namespace {
std::uint32_t Bits(float value) { return std::bit_cast<std::uint32_t>(value); }
float Float(std::uint32_t bits) { return std::bit_cast<float>(bits); }
} // namespace

int main() {
    using D3D12::CanonicalDepthBias;
    using D3D12::CanonicalSlope;
    // The PSO gets (INT)(bias / 2): -10.0 and -10.02 both give -5.
    assert(CanonicalDepthBias(Bits(-10.0f)) == CanonicalDepthBias(Bits(-10.023439f)));
    assert(Float(CanonicalDepthBias(Bits(-10.4f))) == -10.0f);
    assert(Float(CanonicalDepthBias(Bits(3.9f))) == 2.0f);
    assert(static_cast<int>(Float(CanonicalDepthBias(Bits(7.0f))) / 2.0f) == static_cast<int>(7.0f / 2.0f));
    assert(CanonicalDepthBias(Bits(-0.5f)) == 0); // -0.0 is 0
    assert(CanonicalDepthBias(Bits(0.0f)) == 0);
    const float inf = std::numeric_limits<float>::infinity();
    assert(CanonicalDepthBias(Bits(inf)) == Bits(inf));

    // Slopes within 0.2% share a key; clearly different ones do not.
    assert(CanonicalSlope(Bits(-0.5f)) == Bits(-0.5f));
    assert(CanonicalSlope(Bits(-0.5f)) == CanonicalSlope(Bits(-0.50068f)));
    assert(CanonicalSlope(Bits(-0.5f)) != CanonicalSlope(Bits(-0.5023f)));
    assert(CanonicalSlope(Bits(-0.5f)) != CanonicalSlope(Bits(-0.5419f)));
    for (float s : {-0.5023439f, -0.5419271f, 1.75f, 123.456f, 1e-6f}) {
        const float r = Float(CanonicalSlope(Bits(s)));
        assert(std::fabs(r - s) <= std::fabs(s) * 0.002f);
    }
    assert(CanonicalSlope(Bits(-0.0f)) == 0);
    assert(CanonicalSlope(Bits(inf)) == Bits(inf));
    // Rounding up across a power of two stays exact.
    assert(Float(CanonicalSlope(Bits(std::nextafter(2.0f, 0.0f)))) == 2.0f);
    std::cout << "key-canon: ok\n";
}
