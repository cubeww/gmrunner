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
    // periodically on desktop (matching GM 8.2 sub_5611B4). Yields via emscripten_sleep on WebAssembly.
    static void sleep_until_us(u64 target_us, const std::function<void()>& pump = {});

    // Yields execution for at least ms milliseconds (uses emscripten_sleep on WebAssembly).
    static void yield_ms(u32 ms);
};

} // namespace gm
