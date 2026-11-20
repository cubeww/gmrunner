#include "base/math.h"

#include <cmath>
#include <limits>

namespace gm {

i64 delphi_round(f64 v) noexcept {
    if (!std::isfinite(v)) {
        return 0;
    }
    constexpr f64 kMaxI64 = 9223372036854774784.0;  // largest double < 2^63
    constexpr f64 kMinI64 = -9223372036854775808.0; // -2^63
    if (v >= kMaxI64) {
        return std::numeric_limits<i64>::max();
    }
    if (v <= kMinI64) {
        return std::numeric_limits<i64>::min();
    }

    const f64 floored = std::floor(v);
    const f64 frac = v - floored;
    if (frac < 0.5) {
        return static_cast<i64>(floored);
    }
    if (frac > 0.5) {
        return static_cast<i64>(floored) + 1;
    }
    const i64 int_floor = static_cast<i64>(floored);
    return (int_floor & 1) != 0 ? (int_floor + 1) : int_floor;
}

f64 delphi_round_f64(f64 v) noexcept {
    if (!std::isfinite(v)) {
        return v;
    }
    // Doubles with |v| >= 2^52 are already exact integers.
    if (std::fabs(v) >= 4503599627370496.0) {
        return v;
    }
    return static_cast<f64>(delphi_round(v));
}

} // namespace gm
