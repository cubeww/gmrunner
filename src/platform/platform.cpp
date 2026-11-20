#include "platform/platform.h"

#include "base/encoding.h"
#include "base/log.h"
#include "platform/clock.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gm {

namespace {

bool g_platform_initialized = false;

#if defined(_WIN32)
bool win32_codepage_to_utf8(std::string_view bytes, u32 codepage, std::string& out) {
    if (bytes.empty()) {
        out.clear();
        return true;
    }
    const int wide_len = MultiByteToWideChar(
        codepage,
        0,
        bytes.data(),
        static_cast<int>(bytes.size()),
        nullptr,
        0);
    if (wide_len <= 0) {
        return false;
    }
    std::u16string wide(static_cast<usize>(wide_len), u'\0');
    if (MultiByteToWideChar(
            codepage,
            0,
            bytes.data(),
            static_cast<int>(bytes.size()),
            reinterpret_cast<LPWSTR>(wide.data()),
            wide_len) <= 0) {
        return false;
    }
    out = utf16_to_utf8(wide);
    return true;
}

bool win32_utf8_to_codepage(std::string_view utf8, u32 codepage, std::string& out) {
    if (utf8.empty()) {
        out.clear();
        return true;
    }
    std::u16string wide = utf8_to_utf16(utf8);
    const int mb_len = WideCharToMultiByte(
        codepage,
        0,
        reinterpret_cast<LPCWCH>(wide.data()),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (mb_len <= 0) {
        return false;
    }
    out.resize(static_cast<usize>(mb_len));
    if (WideCharToMultiByte(
            codepage,
            0,
            reinterpret_cast<LPCWCH>(wide.data()),
            static_cast<int>(wide.size()),
            out.data(),
            mb_len,
            nullptr,
            nullptr) <= 0) {
        return false;
    }
    return true;
}
#endif

} // namespace

Result<void> Platform::init() {
    if (g_platform_initialized) {
        return Result<void>::success();
    }

    Clock::init();

#if defined(_WIN32)
    register_platform_codepage_hooks(&win32_codepage_to_utf8, &win32_utf8_to_codepage);
#endif

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        Clock::shutdown();
        return Error{std::string("SDL_Init failed: ") + SDL_GetError()};
    }

    g_platform_initialized = true;
    GM_LOG_INFO("platform", "Initialized (%s, SDL %d.%d.%d)",
                platform_name(),
                SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    return Result<void>::success();
}

void Platform::shutdown() noexcept {
    if (!g_platform_initialized) {
        return;
    }
    SDL_Quit();
    Clock::shutdown();
    g_platform_initialized = false;
    GM_LOG_INFO("platform", "Shutdown complete");
}

bool Platform::is_initialized() noexcept {
    return g_platform_initialized;
}

const char* Platform::platform_name() noexcept {
#if defined(_WIN32)
    return "Windows";
#elif defined(__EMSCRIPTEN__)
    return "WebAssembly";
#elif defined(__ANDROID__)
    return "Android";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

} // namespace gm
