#pragma once

#include "base/types.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gm {

inline constexpr f64 kPi = 3.14159265358979323846;
inline constexpr f64 kDegToRad = kPi / 180.0;
inline constexpr f64 kRadToDeg = 180.0 / kPi;

// Verified from gm81.exe.c (sub_572ED8): GML real comparison epsilon is 1e-13.
inline constexpr f64 kGmlEpsilon = 1e-13;
// Verified from gm81.exe.c (dbl_688828): default ds_set_precision epsilon is 1e-7.
inline constexpr f64 kDefaultDsPrecision = 1e-7;

// Exact Delphi Round() semantics: IEEE-754 round-half-to-even (banker's rounding),
// independent of ambient FPU rounding mode.
[[nodiscard]] i64 delphi_round(f64 v) noexcept;
[[nodiscard]] f64 delphi_round_f64(f64 v) noexcept;

// Verified from gm81.exe.c (sub_572ED8): GML truthiness is >= 0.5.
[[nodiscard]] inline constexpr bool gml_truthy(f64 v) noexcept {
    return v >= 0.5;
}

[[nodiscard]] inline bool gml_equals(f64 a, f64 b, f64 eps = kGmlEpsilon) noexcept {
    return std::fabs(a - b) < eps;
}

[[nodiscard]] inline bool gml_less(f64 a, f64 b, f64 eps = kGmlEpsilon) noexcept {
    return (a - b) < -eps;
}

[[nodiscard]] inline bool gml_less_eq(f64 a, f64 b, f64 eps = kGmlEpsilon) noexcept {
    return (a - b) <= eps;
}

[[nodiscard]] inline bool gml_greater(f64 a, f64 b, f64 eps = kGmlEpsilon) noexcept {
    return (a - b) > eps;
}

[[nodiscard]] inline bool gml_greater_eq(f64 a, f64 b, f64 eps = kGmlEpsilon) noexcept {
    return (a - b) >= -eps;
}

// GML integer div and mod truncate quotient toward zero (matching sub_572ED8).
[[nodiscard]] inline f64 gml_div(f64 a, f64 b) noexcept {
    return std::trunc(a / b);
}

[[nodiscard]] inline f64 gml_mod(f64 a, f64 b) noexcept {
    return a - std::trunc(a / b) * b;
}

[[nodiscard]] inline constexpr f64 deg_to_rad(f64 deg) noexcept {
    return deg * kDegToRad;
}

[[nodiscard]] inline constexpr f64 rad_to_deg(f64 rad) noexcept {
    return rad * kRadToDeg;
}

[[nodiscard]] inline constexpr u32 next_power_of_two(u32 v) noexcept {
    if (v <= 1u) return 1u;
    --v;
    v |= v >> 1u;
    v |= v >> 2u;
    v |= v >> 4u;
    v |= v >> 8u;
    v |= v >> 16u;
    return v + 1u;
}

struct ColorRGBA {
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
    u8 a = 255;
};

// GM8 colors are 24-bit BGR: R = bits[7:0], G = bits[15:8], B = bits[23:16].
[[nodiscard]] inline constexpr ColorRGBA gm_bgr_to_rgba(u32 bgr, u8 alpha = 255) noexcept {
    return ColorRGBA{
        static_cast<u8>(bgr & 0xFFu),
        static_cast<u8>((bgr >> 8u) & 0xFFu),
        static_cast<u8>((bgr >> 16u) & 0xFFu),
        alpha,
    };
}

[[nodiscard]] inline constexpr u32 rgba_to_gm_bgr(u8 r, u8 g, u8 b) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8u) | (static_cast<u32>(b) << 16u);
}

} // namespace gm
