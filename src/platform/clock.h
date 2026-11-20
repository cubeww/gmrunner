#pragma once

#include "base/types.h"

#include <functional>

namespace gm {

class Clock {
public:
    static void init() noexcept;
    static void shutdown() noexcept;

    // Monotonic elapsed time in microseconds since an arbitrary epoch.
    [[nodiscard]] static u64 now_us() noexcept;

    // Monotonic elapsed time in seconds.
    [[nodiscard]] static f64 now_seconds() noexcept;

    // Hybrid coarse sleep + microsecond busy-wait until target_us, pumping events
    // periodically on desktop (matching GM 8.2 sub_5611B4). No-op on Emscripten.
    static void sleep_until_us(u64 target_us, const std::function<void()>& pump = {});
};

} // namespace gm
