// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <bit>
#include <cmath>
#include <cstdint>

namespace D3D12 {

/// Maxwell depth bias (f32 bits) as the PSO uses it: D3D12's DepthBias is an integer of half
/// Maxwell's units (Describe truncates bias / 2), so every value that truncates alike is one key.
[[nodiscard]] inline std::uint32_t CanonicalDepthBias(std::uint32_t bits) noexcept {
    const float value = std::bit_cast<float>(bits);
    if (!std::isfinite(value)) {
        return bits;
    }
    // + 0.0f turns -0.0 into 0.0.
    return std::bit_cast<std::uint32_t>(std::trunc(value / 2.0f) * 2.0f + 0.0f);
}

/// Slope-scaled depth bias (f32 bits) rounded to 8 mantissa bits (at most 0.2% off). TOTK sets
/// a slightly different slope on nearly every draw: 1552 values made 15389 pipeline keys of 1177
/// shader sets, and 8 bits leave 6589.
[[nodiscard]] constexpr std::uint32_t CanonicalSlope(std::uint32_t bits) noexcept {
    constexpr std::uint32_t DROP = 23 - 8;
    if ((bits & 0x7f800000u) == 0x7f800000u) {
        return bits; // infinity or NaN
    }
    bits = (bits + (1u << (DROP - 1))) & ~((1u << DROP) - 1);
    return (bits & 0x7fffffffu) == 0 ? 0 : bits;
}

} // namespace D3D12
