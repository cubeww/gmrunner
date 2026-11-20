#include "platform/clock.h"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace gm {

namespace {

bool g_timer_period_enabled = false;

} // namespace

void Clock::init() noexcept {
#if defined(_WIN32)
    if (!g_timer_period_enabled) {
        if (timeBeginPeriod(1u) == TIMERR_NOERROR) {
            g_timer_period_enabled = true;
        }
    }
#endif
}

void Clock::shutdown() noexcept {
#if defined(_WIN32)
    if (g_timer_period_enabled) {
        timeEndPeriod(1u);
        g_timer_period_enabled = false;
    }
#endif
}

u64 Clock::now_us() noexcept {
    using namespace std::chrono;
    return static_cast<u64>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

f64 Clock::now_seconds() noexcept {
    return static_cast<f64>(now_us()) * 1e-6;
}

void Clock::sleep_until_us(u64 target_us, const std::function<void()>& pump) {
#if defined(__EMSCRIPTEN__)
    (void)target_us;
    (void)pump;
#else
    for (;;) {
        const u64 now = now_us();
        if (now >= target_us) {
            break;
        }
        const u64 remaining = target_us - now;
        if (remaining >= 2000u) {
            const u64 sleep_ms = (remaining - 1000u) / 1000u;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            if (pump) {
                pump();
            }
        } else {
            if (pump) {
                pump();
            }
            std::this_thread::yield();
        }
    }
#endif
}

} // namespace gm
