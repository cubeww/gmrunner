#include "extensions/core/extension.h"
#include "extensions/shared/buffer_store.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "platform/clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#elif defined(__EMSCRIPTEN__) || defined(__ANDROID__)
#if defined(__ANDROID__)
#include <unistd.h>
#endif
#else
#include <SDL3/SDL_process.h>
#include <cerrno>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif
#endif

namespace gm {

namespace {

// Improved Noise 2002 Ken Perlin permutation table (matching gm82core/source/perlin.c).
constexpr int kPerlinPerm[512] = {
    151,160,137,91 ,90 ,15 ,131,13 ,201,95 ,96 ,53 ,194,233,7  ,225,140,36 ,103,
    30 ,69 ,142,8  ,99 ,37 ,240,21 ,10 ,23 ,190,6  ,148,247,120,234,75 ,0  ,26 ,
    197,62 ,94 ,252,219,203,117,35 ,11 ,32 ,57 ,177,33 ,88 ,237,149,56 ,87 ,174,
    20 ,125,136,171,168,68 ,175,74 ,165,71 ,134,139,48 ,27 ,166,77 ,146,158,231,
    83 ,111,229,122,60 ,211,133,230,220,105,92 ,41 ,55 ,46 ,245,40 ,244,102,143,
    54 ,65 ,25 ,63 ,161,1  ,216,80 ,73 ,209,76 ,132,187,208,89 ,18 ,169,200,196,
    135,130,116,188,159,86 ,164,100,109,198,173,186,3  ,64 ,52 ,217,226,250,124,
    123,5  ,202,38 ,147,118,126,255,82 ,85 ,212,207,206,59 ,227,47 ,16 ,58 ,17 ,
    182,189,28 ,42 ,223,183,170,213,119,248,152,2  ,44 ,154,163,70 ,221,153,101,
    155,167,43 ,172,9  ,129,22 ,39 ,253,19 ,98 ,108,110,79 ,113,224,232,178,185,
    112,104,218,246,97 ,228,251,34 ,242,193,238,210,144,12 ,191,179,162,241,81 ,
    51 ,145,235,249,14 ,239,107,49 ,192,214,31 ,181,199,106,157,184,84 ,204,176,
    115,121,50 ,45 ,127,4  ,150,254,138,236,205,93 ,222,114,67 ,29 ,24 ,72 ,243,
    141,128,195,78 ,66 ,215,61 ,156,180,

    151,160,137,91 ,90 ,15 ,131,13 ,201,95 ,96 ,53 ,194,233,7  ,225,140,36 ,103,
    30 ,69 ,142,8  ,99 ,37 ,240,21 ,10 ,23 ,190,6  ,148,247,120,234,75 ,0  ,26 ,
    197,62 ,94 ,252,219,203,117,35 ,11 ,32 ,57 ,177,33 ,88 ,237,149,56 ,87 ,174,
    20 ,125,136,171,168,68 ,175,74 ,165,71 ,134,139,48 ,27 ,166,77 ,146,158,231,
    83 ,111,229,122,60 ,211,133,230,220,105,92 ,41 ,55 ,46 ,245,40 ,244,102,143,
    54 ,65 ,25 ,63 ,161,1  ,216,80 ,73 ,209,76 ,132,187,208,89 ,18 ,169,200,196,
    135,130,116,188,159,86 ,164,100,109,198,173,186,3  ,64 ,52 ,217,226,250,124,
    123,5  ,202,38 ,147,118,126,255,82 ,85 ,212,207,206,59 ,227,47 ,16 ,58 ,17 ,
    182,189,28 ,42 ,223,183,170,213,119,248,152,2  ,44 ,154,163,70 ,221,153,101,
    155,167,43 ,172,9  ,129,22 ,39 ,253,19 ,98 ,108,110,79 ,113,224,232,178,185,
    112,104,218,246,97 ,228,251,34 ,242,193,238,210,144,12 ,191,179,162,241,81 ,
    51 ,145,235,249,14 ,239,107,49 ,192,214,31 ,181,199,106,157,184,84 ,204,176,
    115,121,50 ,45 ,127,4  ,150,254,138,236,205,93 ,222,114,67 ,29 ,24 ,72 ,243,
    141,128,195,78 ,66 ,215,61 ,156,180
};

inline f64 perlin_fade(f64 t) noexcept { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
inline f64 perlin_lerp(f64 t, f64 a, f64 b) noexcept { return a + t * (b - a); }
inline f64 perlin_grad(int hash, f64 x, f64 y, f64 z) noexcept {
    const int h = hash & 15;
    const f64 u = (h < 8) ? x : y;
    const f64 v = (h < 4) ? y : ((h == 12 || h == 14) ? x : z);
    return (((h & 1) == 0) ? u : -u) + (((h & 2) == 0) ? v : -v);
}

f64 eval_perlin_noise(f64 x, f64 y, f64 z) noexcept {
    const int X = static_cast<int>(std::floor(x)) & 255;
    const int Y = static_cast<int>(std::floor(y)) & 255;
    const int Z = static_cast<int>(std::floor(z)) & 255;
    x -= std::floor(x);
    y -= std::floor(y);
    z -= std::floor(z);
    const f64 u = perlin_fade(x);
    const f64 v = perlin_fade(y);
    const f64 w = perlin_fade(z);
    const int A = kPerlinPerm[X] + Y, AA = kPerlinPerm[A] + Z, AB = kPerlinPerm[A + 1] + Z;
    const int B = kPerlinPerm[X + 1] + Y, BA = kPerlinPerm[B] + Z, BB = kPerlinPerm[B + 1] + Z;
    return 0.5 + 0.5 *
        perlin_lerp(w,
            perlin_lerp(v,
                perlin_lerp(u, perlin_grad(kPerlinPerm[AA], x, y, z),
                               perlin_grad(kPerlinPerm[BA], x - 1.0, y, z)),
                perlin_lerp(u, perlin_grad(kPerlinPerm[AB], x, y - 1.0, z),
                               perlin_grad(kPerlinPerm[BB], x - 1.0, y - 1.0, z))),
            perlin_lerp(v,
                perlin_lerp(u, perlin_grad(kPerlinPerm[AA + 1], x, y, z - 1.0),
                               perlin_grad(kPerlinPerm[BA + 1], x - 1.0, y, z - 1.0)),
                perlin_lerp(u, perlin_grad(kPerlinPerm[AB + 1], x, y - 1.0, z - 1.0),
                               perlin_grad(kPerlinPerm[BB + 1], x - 1.0, y - 1.0, z - 1.0))));
}

inline f64 gm_modwrap(f64 val, f64 minv, f64 maxv) noexcept {
    const f64 f = val - minv;
    const f64 w = maxv - minv;
    if (w == 0.0) return minv;
    return f - std::floor(f / w) * w + minv;
}

inline f64 gm_signnum(f64 x) noexcept {
    if (x > 0.0) return 1.0;
    if (x < 0.0) return -1.0;
    return x;
}

inline f64 gm_dcos(f64 deg) noexcept { return std::cos(deg * (kPi / 180.0)); }
inline f64 gm_dsin(f64 deg) noexcept { return std::sin(deg * (kPi / 180.0)); }
inline f64 gm_pointdir(f64 x1, f64 y1, f64 x2, f64 y2) noexcept {
    return gm_modwrap(std::atan2(-(y2 - y1), x2 - x1) * (180.0 / kPi), 0.0, 360.0);
}

inline f64 gm_dot3d_norm(f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2) noexcept {
    const f64 a = std::sqrt(x1 * x1 + y1 * y1 + z1 * z1);
    const f64 b = std::sqrt(x2 * x2 + y2 * y2 + z2 * z2);
    if (a * b == 0.0) return 1.0;
    return (x1 / a) * (x2 / b) + (y1 / a) * (y2 / b) + (z1 / a) * (z2 / b);
}

inline f64 gm_point_line_lerp(f64 px, f64 py, f64 x1, f64 y1, f64 x2, f64 y2, f64 seg) noexcept {
    const f64 dx = x2 - x1;
    const f64 dy = y2 - y1;
    if (dx == 0.0 && dy == 0.0) return 0.0;
    f64 t = (dx * (px - x1) + dy * (py - y1)) / (dx * dx + dy * dy);
    if (seg > 0.5) t = std::clamp(t, 0.0, 1.0);
    return t;
}

f64 power_next_val(f64 x) noexcept {
    if (x < 0.0) return 0.0;
    if (x <= 1.0) return 1.0;
    const f64 p = std::ceil(std::log2(x));
    return std::pow(2.0, p);
}

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
std::filesystem::path local_program(const Vfs& vfs, std::string_view name) {
    const auto find_file = [&](std::string_view candidate) -> std::filesystem::path {
        std::error_code ec;
        auto path = vfs.resolve_for_read(candidate);
        if (std::filesystem::is_regular_file(path, ec)) return path;
#if defined(_WIN32)
        if (!path.has_extension()) {
            path = vfs.resolve_for_read(std::string(candidate) + ".exe");
            if (std::filesystem::is_regular_file(path, ec)) return path;
        }
#endif
        return {};
    };
    // CreateProcess searches the game executable's directory before its current directory.
    if (name.find_first_of("/\\:") == name.npos && !vfs.game_root().empty()) {
        auto path = find_file(vfs.program_directory_gm() + "/" + std::string(name));
        if (!path.empty()) return path;
    }
    return find_file(name);
}
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
// CreateProcess receives a Windows command line. SDL on other platforms takes argv.
std::vector<std::string> split_process_args(std::string_view cmd) {
    std::vector<std::string> args;
    for (usize i = 0; (i = cmd.find_first_not_of(" \t", i)) != cmd.npos;) {
        std::string arg;
        bool quoted = false;
        while (i < cmd.size() && (quoted || (cmd[i] != ' ' && cmd[i] != '\t'))) {
            const usize begin = i;
            while (i < cmd.size() && cmd[i] == '\\') ++i;
            const usize slashes = i - begin;
            if (i < cmd.size() && cmd[i] == '"') {
                arg.append(slashes / 2, '\\');
                ++i;
                if (slashes % 2) {
                    arg += '"';
                } else if (quoted && i < cmd.size() && cmd[i] == '"') {
                    arg += '"';
                    ++i;
                } else {
                    quoted = !quoted;
                }
            } else {
                arg.append(slashes, '\\');
                if (i < cmd.size() && (quoted || (cmd[i] != ' ' && cmd[i] != '\t'))) arg += cmd[i++];
            }
        }
        args.push_back(std::move(arg));
    }
    return args;
}
#endif

class SilentProcess : public NonCopyable {
public:
    ~SilentProcess() { release(); }
    [[nodiscard]] bool active() const { return process_ != nullptr || failed_start_; }

    bool start(const Vfs& vfs, std::string_view command, bool detached) {
        // The DLL occupies its single tracked slot even for a failed CreateProcess.
        if (!detached) failed_start_ = true;
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
        (void)vfs;
        (void)command;
        return false;
#else
        const auto cwd = vfs.resolve_for_read(".");
#if defined(_WIN32)
        const auto begin = command.find_first_not_of(" \t");
        if (begin == command.npos) return false;
        const bool quoted = command[begin] == '"';
        const auto first = begin + static_cast<usize>(quoted);
        auto end = quoted ? command.find('"', first) : command.find_first_of(" \t", first);
        auto name = command.substr(first, end - first);
        if (name.empty()) return false;
        auto program = local_program(vfs, name);
        // CreateProcess also tries successive word boundaries for an unquoted path with spaces.
        while (program.empty() && !quoted && end != command.npos) {
            end = command.find_first_of(" \t", end + 1);
            program = local_program(vfs, command.substr(first, end - first));
        }
        if (program.empty() && name.find_first_of("/\\:") != name.npos) return false;
        std::wstring cmd = Vfs::path_from_utf8(command).wstring();
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(program.empty() ? nullptr : program.c_str(), cmd.data(), nullptr, nullptr,
                            TRUE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
            return false;
        }
        CloseHandle(pi.hThread);
        if (detached) CloseHandle(pi.hProcess);
        else process_ = pi.hProcess;
#else
        auto args = split_process_args(command);
        if (args.empty() || args.front().empty()) return false;
        const auto program = local_program(vfs, args.front());
        if (!program.empty()) args.front() = Vfs::path_to_utf8(program);
        std::vector<const char*> argv;
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        const auto props = SDL_CreateProperties();
        if (!props) return false;
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, Vfs::path_to_utf8(cwd).c_str());
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, detached);
        auto* child = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (!child) return false;
        if (detached) SDL_DestroyProcess(child);
        else process_ = child;
#endif
        if (!detached) failed_start_ = false;
        return true;
#endif
    }

    f64 poll() {
        if (!process_) {
            // Poll consumes a failed start. The DLL reads an uninitialized exit code here.
            failed_start_ = false;
            return 75;
        }
#if defined(_WIN32)
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return 259;
        DWORD code = 75;
        GetExitCodeProcess(process_, &code);
        release();
        return static_cast<f64>(code);
#elif defined(__EMSCRIPTEN__) || defined(__ANDROID__)
        return 75;
#else
        int code = 0;
        if (!SDL_WaitProcess(process_, false, &code)) return 259;
        SDL_DestroyProcess(process_);
        process_ = nullptr;
        return static_cast<f64>(code);
#endif
    }

    void release() {
        failed_start_ = false;
        if (!process_) return;
#if defined(_WIN32)
        CloseHandle(process_);
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
        const auto pid = static_cast<pid_t>(SDL_GetNumberProperty(
            SDL_GetProcessProperties(process_), SDL_PROP_PROCESS_PID_NUMBER, 0));
        const bool exited = SDL_WaitProcess(process_, false, nullptr);
        SDL_DestroyProcess(process_);
        // SDL may shut down immediately after unload. Only the OS wait outlives the shim.
        if (!exited && pid > 0) {
            std::thread([pid] {
                while (waitpid(pid, nullptr, 0) == -1 && errno == EINTR) {}
            }).detach();
        }
#endif
        process_ = nullptr;
    }

private:
#if defined(_WIN32)
    HANDLE process_ = nullptr;
#elif defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    void* process_ = nullptr;
#else
    SDL_Process* process_ = nullptr;
#endif
    bool failed_start_ = false;
};

class Gm82CoreExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "gm82core"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_alias("gm82core");
        buffers_ = &reg.services().get_or_create<GmBufferStore>();

        // ---- Core / Color / Tokenizer ----
        reg.bind_const("__gm82core_dllcheck", 820.0);
        reg.bind("color_reverse", [](f64 raw) {
            const u32 col = static_cast<u32>(std::llround(raw));
            const u32 rev = ((col & 0xFFu) << 16u) | (col & 0xFF00u) | ((col & 0xFF0000u) >> 16u);
            return static_cast<f64>(rev);
        });
        reg.bind("color_inverse", [](f64 raw) {
            const i64 col = static_cast<i64>(std::llround(raw));
            return static_cast<f64>(0xFFFFFF - col);
        });
        reg.bind("string_token_start", [this](const CallContext& ctx) {
            token_store_ = ctx.str(0);
            token_sep_ = ctx.str(1);
            if (token_sep_.size() > 255) token_sep_.resize(255);
            token_pos_ = 0;
            token_done_ = false;
            if (token_sep_.empty()) return RValue(1.0);
            usize count = 0;
            usize pos = 0;
            const usize seplen = token_sep_.size();
            while (pos < token_store_.size()) {
                const usize found = token_store_.find(token_sep_, pos);
                if (found == std::string::npos) break;
                if (found + seplen < token_store_.size()) {
                    ++count;
                }
                pos = found + seplen;
            }
            return RValue(static_cast<f64>(count + 1));
        });
        reg.bind("string_token_next", [this]() {
            return next_token();
        });
        reg.bind("string_token_real", [this]() {
            const std::string tok = next_token();
            if (tok.empty()) return 0.0;
            return std::strtod(tok.c_str(), nullptr);
        });

        // ---- High-Resolution Timer ----
        reg.bind({"__hrt_init", "__gm82core_hrt_init"}, [this]() {
            hrt_last_us_ = Clock::now_us();
            return 1.0;
        });
        reg.bind({"hrt_now", "get_timer", "hrt_time_now"}, []() {
            return static_cast<f64>(Clock::now_us());
        });
        reg.bind({"hrt_delta", "get_delta", "hrt_time_delta"}, [this]() {
            const u64 now = Clock::now_us();
            const u64 diff = (hrt_last_us_ > 0 && now >= hrt_last_us_) ? (now - hrt_last_us_) : 0u;
            hrt_last_us_ = now;
            return static_cast<f64>(diff);
        });

        // ---- Lovey01 Helpers ----
        reg.bind<&power_next_val>("power_next");
        reg.bind("file_size", [](IExtensionHost& host, std::string_view path) {
            auto sz = host.ext_vfs().file_size(path);
            return sz ? static_cast<f64>(sz.value()) : -1.0;
        });
        reg.bind("real_hex", [](std::string_view s) {
            u64 val = 0;
            for (char c : s) {
                u64 nib = 0;
                if (c >= '0' && c <= '9') nib = static_cast<u64>(c - '0');
                else if (c >= 'a' && c <= 'f') nib = static_cast<u64>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') nib = static_cast<u64>(c - 'A' + 10);
                val = (val << 4u) | nib;
            }
            return static_cast<f64>(val);
        });
        reg.bind("string_hex", [](f64 raw) {
            const u64 num = static_cast<u64>(raw);
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(num));
            return std::string(buf);
        });

        // ---- Perlin Noise ----
        reg.bind<&eval_perlin_noise>("perlin_noise");

        // ---- Runner Internals (terrible_gm8_hacking.c) ----
        reg.bind("error_is_enabled", [](IExtensionHost& host) {
            return host.ext_error_is_enabled();
        });
        reg.bind("error_set_enabled", [](const CallContext& ctx) {
            ctx.host.ext_error_set_enabled(ctx.real(0, 1.0) > 0.5);
            return RValue(0.0);
        });
        reg.bind("io_set_roomend_clear", [](const CallContext& ctx) {
            ctx.host.ext_io_set_roomend_clear(ctx.boolean(0, true));
            return RValue(0.0);
        });
        reg.bind("window_set_fixed", [](const CallContext& ctx) {
            ctx.host.ext_window_set_fixed(ctx.boolean(0, false));
            return RValue(0.0);
        });
        reg.bind("include_file_count", [](IExtensionHost& host) {
            return static_cast<f64>(host.ext_game().included_files.size());
        });
        reg.bind("include_file_exists", [](IExtensionHost& host, std::string_view name) {
            for (const auto& inc : host.ext_game().included_files) {
                if (inc.exists && inc.filename == name) return 1.0;
            }
            return 0.0;
        });
        reg.bind("include_file_name", [](const CallContext& ctx) {
            const i32 idx = ctx.i32_arg(0, -1);
            const auto& incs = ctx.host.ext_game().included_files;
            if (idx >= 0 && static_cast<usize>(idx) < incs.size() && incs[static_cast<usize>(idx)].exists) {
                return RValue(incs[static_cast<usize>(idx)].filename);
            }
            return RValue(std::string{});
        });
        reg.bind("include_file_size", [](IExtensionHost& host, std::string_view name) {
            for (const auto& inc : host.ext_game().included_files) {
                if (inc.exists && inc.filename == name) {
                    return static_cast<f64>(inc.store ? inc.data.size() : static_cast<usize>(std::max(0, inc.size)));
                }
            }
            return 0.0;
        });
        reg.bind("include_file_get_string", [](IExtensionHost& host, std::string_view name) {
            for (const auto& inc : host.ext_game().included_files) {
                if (inc.exists && inc.filename == name && inc.store && !inc.data.empty()) {
                    return std::string(reinterpret_cast<const char*>(inc.data.data()), inc.data.size());
                }
            }
            return std::string{};
        });
        reg.bind("__gm82core_include_file_get_buffer", [this](const CallContext& ctx) {
            const std::string name = ctx.str(0);
            const u32 addr = static_cast<u32>(ctx.real(1, 0.0));
            GmBuffer* buf = buffers_ ? buffers_->find_by_address(addr) : nullptr;
            if (!buf) return RValue(0.0);
            for (const auto& inc : ctx.host.ext_game().included_files) {
                if (inc.exists && inc.filename == name && inc.store && !inc.data.empty()) {
                    if (buf->data.size() < inc.data.size()) {
                        buf->data.resize(inc.data.size());
                    }
                    std::memcpy(buf->data.data(), inc.data.data(), inc.data.size());
                    return RValue(1.0);
                }
            }
            return RValue(0.0);
        });
        reg.bind("game_get_state", [](IExtensionHost& host) {
            return static_cast<f64>(host.ext_game_get_state());
        });
        reg.bind("room_goto_cancel", [](IExtensionHost& host) {
            host.ext_room_goto_cancel();
        });

        // ---- OS / Window / Process / Registry / Filesystem (windows.c) ----
        reg.bind("__gm82core_checkstart", [this]() {
            if (has_started_) return 0.0;
            has_started_ = true;
            return 1.0;
        });
        reg.bind({"__gm82core_getmaximized", "window_is_maximized"}, [](IExtensionHost& host) {
            return host.ext_window().is_maximized();
        });
        reg.bind({"__gm82core_getminimized", "window_is_minimized"}, [](IExtensionHost& host) {
            return host.ext_window().is_minimized();
        });
        reg.bind({"__gm82core_setmaximized", "window_maximize"}, [](IExtensionHost& host) {
            host.ext_window().maximize();
        });
        reg.bind({"__gm82core_setminimized", "window_minimize"}, [](IExtensionHost& host) {
            host.ext_window().minimize();
        });
        reg.bind("window_restore", [](IExtensionHost& host) {
            host.ext_window().restore();
        });
        reg.bind({"__gm82core_set_foreground", "window_set_foreground"}, [](IExtensionHost& host) {
            host.ext_window().raise_focus();
        });
        reg.bind({"get_foreground_window", "__gm82core_getfore"}, [](IExtensionHost& host) {
            return host.ext_window().has_focus();
        });
        reg.bind_const("window_set_close_enabled", 1.0);
        reg.bind_const("window_set_dpiaware", 0.0);
        reg.bind("window_exists", [](IExtensionHost& host, std::string_view cap) {
            if (!cap.empty() && cap == host.ext_window().title()) return 1.0;
#if defined(_WIN32)
            std::string owned(cap);
            return FindWindowA(nullptr, owned.c_str()) != nullptr ? 1.0 : 0.0;
#else
            return 0.0;
#endif
        });
        reg.bind_const({"get_window_col", "window_get_caption_color"}, static_cast<f64>(0xFFFFFFu));
        reg.bind_const("get_windows_version", 10.0);
        reg.bind_const({"__gm82core_addfonttemp", "__gm82core_remfonttemp"}, 1.0);
        reg.bind({"__gm82core_shortfn", "filename_short"}, [](std::string_view s) {
            return std::string(s);
        });
        reg.bind("__gm82core_execute_program_silent", [this](const CallContext& ctx) {
            if (process_.active()) return RValue(75.0);
            return RValue(process_.start(ctx.host.ext_vfs(), ctx.str(0), false));
        });
        reg.bind("__gm82core_execute_program_silent_exitcode", [this]() {
            return process_.poll();
        });
        reg.bind({"__gm82core_execute_program_silent_async", "execute_program_silent_async"}, [this](const CallContext& ctx) {
            (void)process_.start(ctx.host.ext_vfs(), ctx.str(0), true);
            return RValue(0.0);
        });
        reg.bind({"get_capslock", "keyboard_get_capslock"}, []() {
            return PlatformWindow::capslock_on();
        });
        reg.bind({"get_scrolllock", "keyboard_get_scrolllock"}, []() {
            return PlatformWindow::scrolllock_on();
        });
        reg.bind("get_ram_usage", []() -> f64 {
#if defined(_WIN32)
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                     sizeof(pmc))) {
                return static_cast<f64>(pmc.PrivateUsage);
            }
#elif defined(__APPLE__)
            mach_task_basic_info info{};
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                          reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
                return static_cast<f64>(info.resident_size);
            }
#elif defined(__linux__) || defined(__ANDROID__)
            long rss_pages = 0;
            if (FILE* f = std::fopen("/proc/self/statm", "r")) {
                long total_pages = 0;
                if (std::fscanf(f, "%ld %ld", &total_pages, &rss_pages) == 2) {
                    std::fclose(f);
                    return static_cast<f64>(rss_pages) * static_cast<f64>(sysconf(_SC_PAGESIZE));
                }
                std::fclose(f);
            }
#endif
            return 0.0;
        });
        reg.bind_stub({"get_cpu_usage", "set_dll_loaddir", "__gm82core_setdir"}, 0.0);
        reg.bind("set_working_directory", [](IExtensionHost& host, std::string_view dir) {
            host.ext_vfs().set_working_directory(dir);
        });
        reg.bind({"io_get_language", "get_system_language", "os_get_language"}, []() -> f64 {
#if defined(_WIN32)
            return static_cast<f64>(GetUserDefaultUILanguage());
#else
            return 1033.0; // en-US
#endif
        });
        reg.bind({"get_system_region", "os_get_region"}, []() -> f64 {
#if defined(_WIN32)
            return static_cast<f64>(GetUserDefaultLCID());
#else
            return 1033.0;
#endif
        });
        reg.bind("sleep_ext", [](f64 raw_ms) {
            const f64 ms = std::max(0.0, raw_ms);
            if (ms > 0.0) {
                Clock::sleep_until_us(Clock::now_us() + static_cast<u64>(ms * 1000.0));
            }
        });
        reg.bind("__registry_read_dword", [](IExtensionHost& host, std::string_view dir, std::string_view key) {
            if (!host.ext_vfs().registry_exists(dir, key)) return -4.0;
            return host.ext_vfs().registry_read_real(dir, key);
        });
        reg.bind("__registry_read_sz", [](IExtensionHost& host, std::string_view dir, std::string_view key) {
            if (!host.ext_vfs().registry_exists(dir, key)) return std::string("<undefined>");
            return host.ext_vfs().registry_read_string(dir, key);
        });
        reg.bind("__registry_write_dword", [](IExtensionHost& host, std::string_view dir, std::string_view key, i32 val) {
            host.ext_vfs().registry_write_real(dir, key, static_cast<f64>(val));
            return static_cast<f64>(val);
        });
        reg.bind("file_get_timestamp", [](IExtensionHost& host, std::string_view path) {
            const auto ts = host.ext_vfs().last_write_time_unix(path);
            if (!ts) return -1.0;
            // Delphi TDateTime epoch is 1899-12-30 (25569 days before 1970-01-01).
            return static_cast<f64>(*ts) / 86400.0 + 25569.0;
        });
        reg.bind("date_get_current_timezone", []() {
            const std::time_t now = std::time(nullptr);
            std::tm local_tm{}, utc_tm{};
#if defined(_WIN32)
            localtime_s(&local_tm, &now);
            gmtime_s(&utc_tm, &now);
#else
            localtime_r(&now, &local_tm);
            gmtime_r(&now, &utc_tm);
#endif
            const i64 diff_sec = static_cast<i64>(std::mktime(&local_tm) - std::mktime(&utc_tm));
            return static_cast<f64>(diff_sec / 3600);
        });
        reg.bind("get_battery_level", []() -> f64 {
#if defined(_WIN32)
            SYSTEM_POWER_STATUS sps{};
            if (GetSystemPowerStatus(&sps)) {
                if (sps.BatteryFlag >= 128) return 0.0;
                return static_cast<f64>(sps.BatteryLifePercent);
            }
#else
            int pct = -1;
            const SDL_PowerState st = SDL_GetPowerInfo(nullptr, &pct);
            if (st == SDL_POWERSTATE_NO_BATTERY) return 0.0;
            if (pct >= 0) return static_cast<f64>(pct);
#endif
            return 100.0;
        });
        reg.bind("get_battery_status", []() -> f64 {
#if defined(_WIN32)
            SYSTEM_POWER_STATUS sps{};
            if (GetSystemPowerStatus(&sps)) {
                if (sps.BatteryFlag >= 128) return 0.0;
                if (sps.ACLineStatus == 1) return 2.0;
                return 1.0;
            }
#else
            const SDL_PowerState st = SDL_GetPowerInfo(nullptr, nullptr);
            if (st == SDL_POWERSTATE_ON_BATTERY) return 1.0;
            if (st == SDL_POWERSTATE_CHARGING || st == SDL_POWERSTATE_CHARGED) return 2.0;
#endif
            return 0.0;
        });
        reg.bind("file_create", [](IExtensionHost& host, std::string_view p) {
            if (host.ext_vfs().exists(p)) return 1.0;
            return host.ext_vfs().write_all_bytes(p, {}) ? 1.0 : 0.0;
        });
        reg.bind("process_exists", [](std::string_view proc) -> f64 {
#if defined(_WIN32)
            const std::string target = ascii_to_lower(proc);
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) return 0.0;
            PROCESSENTRY32 pe{};
            pe.dwSize = sizeof(pe);
            bool found = false;
            if (Process32First(snap, &pe)) {
                do {
                    if (ascii_to_lower(pe.szExeFile) == target) {
                        found = true;
                        break;
                    }
                } while (Process32Next(snap, &pe));
            }
            CloseHandle(snap);
            return found ? 1.0 : 0.0;
#else
            (void)proc;
            return 0.0;
#endif
        });

        // ---- Math, Trig, Angles, Geometry, Bit Packing (math.c) ----
        reg.bind<&gm_modwrap>("modwrap");
        reg.bind("lerp2", [](f64 fa, f64 fb, f64 ta, f64 tb, f64 v) {
            return ((v - fa) / (fb - fa)) * (tb - ta) + ta;
        });
        reg.bind("clerp2", [](f64 fa, f64 fb, f64 ta, f64 tb, f64 v) {
            const f64 t = std::clamp((v - fa) / (fb - fa), 0.0, 1.0);
            return t * (tb - ta) + ta;
        });
        reg.bind("unlerp", [](f64 a, f64 b, f64 v) {
            return (v - a) / (b - a);
        });
        reg.bind("unclerp", [](f64 a, f64 b, f64 v) {
            return std::clamp((v - a) / (b - a), 0.0, 1.0);
        });
        reg.bind("esign", [](f64 x, f64 def) {
            return x == 0.0 ? def : (x > 0.0 ? 1.0 : -1.0);
        });
        reg.bind("saturate", [](f64 x) {
            return std::clamp(x, 0.0, 1.0);
        });
        reg.bind("smoothstep", [](f64 mn, f64 mx, f64 v) {
            const f64 t = std::clamp((v - mn) / (mx - mn), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        });
        reg.bind({"approach", "inch"}, [](f64 val, f64 go, f64 step) {
            return val < go ? std::min(go, val + step) : std::max(go, val - step);
        });
        reg.bind("lerproach", [](f64 val, f64 go, f64 l, f64 a) {
            val = val * (1.0 - l) + go * l;
            return val < go ? std::min(go, val + a) : std::max(go, val - a);
        });
        reg.bind("round_unbiased", [](f64 x) {
            return std::floor(x + 0.5);
        });
        reg.bind("roundto", [](const CallContext& ctx) {
            const f64 val = ctx.real(0), to = ctx.real(1, 1.0);
            return RValue(std::round(val / to) * to);
        });
        reg.bind("roundto_unbiased", [](const CallContext& ctx) {
            const f64 val = ctx.real(0), to = ctx.real(1, 1.0);
            return RValue(std::floor(val / to + 0.5) * to);
        });
        reg.bind("floorto", [](const CallContext& ctx) {
            const f64 val = ctx.real(0), to = ctx.real(1, 1.0);
            return RValue(std::floor(val / to) * to);
        });
        reg.bind("ceilto", [](const CallContext& ctx) {
            const f64 val = ctx.real(0), to = ctx.real(1, 1.0);
            return RValue(std::ceil(val / to) * to);
        });
        reg.bind("cosine", [](f64 a, f64 b, f64 amt) {
            const f64 mu = (1.0 - std::cos(amt * kPi)) * 0.5;
            return a * (1.0 - mu) + b * mu;
        });
        reg.bind("angle_difference", [](f64 a1, f64 a2) {
            return gm_modwrap(a2 - a1 + 540.0, 0.0, 360.0) - 180.0;
        });
        reg.bind("angle_mean", [](f64 a1, f64 a2) {
            return a1 + 0.5 * (gm_modwrap(a2 - a1 + 540.0, 0.0, 360.0) - 180.0);
        });
        reg.bind({"approach_angle", "inch_angle"}, [](f64 a1, f64 a2, f64 step) {
            const f64 d = std::fmod(a2 - a1 + 540.0, 360.0) - 180.0;
            if (d > 0.0) return std::fmod(a1 + std::min(d, step), 360.0);
            return std::fmod(360.0 - std::fmod(360.0 - (a1 + std::max(-step, d)), 360.0), 360.0);
        });
        reg.bind("approach_angle_biased", [](f64 a1, f64 a2, f64 step, f64 bias) {
            const f64 d = std::fmod(a2 - a1 + 540.0, 360.0) - 180.0;
            if (std::fabs(std::fabs(d) - 180.0) < std::fabs(bias)) {
                return gm_modwrap(a1 + std::min(std::fabs(d), step) * gm_signnum(bias), 0.0, 360.0);
            }
            if (d > 0.0) return std::fmod(a1 + std::min(d, step), 360.0);
            return std::fmod(360.0 - std::fmod(360.0 - (a1 + std::max(-step, d)), 360.0), 360.0);
        });
        reg.bind("lerp_angle", [](f64 from, f64 to, f64 amt) {
            return from + (gm_modwrap(to - from + 540.0, 0.0, 360.0) - 180.0) * amt;
        });
        reg.bind("angle_abs", [](f64 a) {
            return std::fabs(gm_modwrap(a + 540.0, 0.0, 360.0) - 180.0);
        });
        reg.bind("darccos", [](f64 x) { return std::acos(std::clamp(x, -1.0, 1.0)) * (180.0 / kPi); });
        reg.bind("darcsin", [](f64 x) { return std::asin(std::clamp(x, -1.0, 1.0)) * (180.0 / kPi); });
        reg.bind("darctan", [](f64 x) { return std::atan(x) * (180.0 / kPi); });
        reg.bind("darctan2", [](f64 y, f64 x) { return std::atan2(y, x) * (180.0 / kPi); });
        reg.bind<&gm_dcos>("dcos");
        reg.bind<&gm_dsin>("dsin");
        reg.bind("dtan", [](f64 deg) { return std::tan(deg * (kPi / 180.0)); });
        reg.bind("secant", [](f64 rad) { return 1.0 / std::cos(rad); });
        reg.bind("dsecant", [](f64 deg) { return 1.0 / gm_dcos(deg); });
        reg.bind("range_collision", [](f64 a1, f64 a2, f64 b1, f64 b2) {
            return std::max(a1, a2) > std::min(b1, b2) && std::max(b1, b2) > std::min(a1, a2);
        });
        reg.bind("triangle_is_clockwise", [](f64 x0, f64 y0, f64 x1, f64 y1, f64 x2, f64 y2) {
            int cw = 0;
            if (x0 != x1 || y0 != y1) {
                if (x0 == x1) {
                    cw = (x2 < x1) ^ (y0 > y1);
                } else {
                    const f64 m = (y0 - y1) / (x0 - x1);
                    cw = (y2 > (m * x2 + y0 - m * x0)) ^ (x0 > x1);
                }
            }
            return static_cast<f64>(cw);
        });
        reg.bind("lengthdir_zx", [](f64 len, f64 dir, f64 pitch) {
            return len * gm_dcos(pitch) * gm_dcos(dir);
        });
        reg.bind("lengthdir_zy", [](f64 len, f64 dir, f64 pitch) {
            return -len * gm_dcos(pitch) * gm_dsin(dir);
        });
        reg.bind("lengthdir_zz", [](f64 len, f64, f64 pitch) {
            return -len * gm_dsin(pitch);
        });
        reg.bind("point_direction_pitch", [](f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2) {
            return gm_pointdir(0.0, z1, std::hypot(x2 - x1, y2 - y1), z2);
        });
        reg.bind("pivot_pos_x", [](f64 px, f64 py, f64 dir) {
            return px * gm_dcos(dir) + py * gm_dcos(dir - 90.0);
        });
        reg.bind("pivot_pos_y", [](f64 px, f64 py, f64 dir) {
            return -px * gm_dsin(dir) - py * gm_dsin(dir - 90.0);
        });
        reg.bind("point_in_circle", [](f64 px, f64 py, f64 cx, f64 cy, f64 r) {
            return std::hypot(cx - px, cy - py) < r;
        });
        reg.bind("circle_in_circle", [](f64 x1, f64 y1, f64 r1, f64 x2, f64 y2, f64 r2) {
            return std::hypot(x2 - x1, y2 - y1) < (r1 + r2);
        });
        reg.bind("rectangle_in_circle", [](f64 x1, f64 y1, f64 x2, f64 y2, f64 cx, f64 cy, f64 cr) {
            const f64 tx = std::clamp(cx, x1, x2);
            const f64 ty = std::clamp(cy, y1, y2);
            return std::hypot(cx - tx, cy - ty) < cr;
        });
        reg.bind("point_in_rectangle", [](f64 px, f64 py, f64 x1, f64 y1, f64 x2, f64 y2) {
            return px >= x1 && px < x2 && py >= y1 && py < y2;
        });
        reg.bind("rectangle_in_rectangle", [](f64 ax1, f64 ay1, f64 ax2, f64 ay2, f64 bx1, f64 by1, f64 bx2, f64 by2) {
            if (ax1 > ax2) std::swap(ax1, ax2);
            if (ay1 > ay2) std::swap(ay1, ay2);
            if (bx1 > bx2) std::swap(bx1, bx2);
            if (by1 > by2) std::swap(by1, by2);
            if (ax1 >= bx1 && ax2 <= bx2 && ay1 >= by1 && ay2 <= by2) return 1.0;
            if (ax1 > bx2 || ax2 < bx1 || ay1 > by2 || ay2 < by1) return 0.0;
            return 2.0;
        });
        reg.bind("point_in_triangle", [](f64 x0, f64 y0, f64 x1, f64 y1, f64 x2, f64 y2, f64 x3, f64 y3) {
            const f64 a = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            const f64 b = (x2 - x0) * (y3 - y0) - (x3 - x0) * (y2 - y0);
            const f64 c = (x3 - x0) * (y1 - y0) - (x1 - x0) * (y3 - y0);
            return gm_signnum(a) == gm_signnum(b) && gm_signnum(b) == gm_signnum(c);
        });
        reg.bind("in_range", [](f64 v, f64 mn, f64 mx) {
            return v >= mn && v <= mx;
        });
        reg.bind<&gm_point_line_lerp>("point_line_lerp");
        reg.bind("point_line_distance", [](f64 px, f64 py, f64 x1, f64 y1, f64 x2, f64 y2, f64 seg) {
            const f64 t = gm_point_line_lerp(px, py, x1, y1, x2, y2, seg);
            return std::hypot(px - (x1 + t * (x2 - x1)), py - (y1 + t * (y2 - y1)));
        });
        reg.bind<&gm_dot3d_norm>("dot_product_3d_normalized");
        reg.bind("dot_product_normalized", [](f64 x1, f64 y1, f64 x2, f64 y2) {
            const f64 a = std::hypot(x1, y1), b = std::hypot(x2, y2);
            if (a * b == 0.0) return 1.0;
            return (x1 / a) * (x2 / b) + (y1 / a) * (y2 / b);
        });
        reg.bind("angle_difference_3d", [](f64 x1, f64 y1, f64 z1, f64 x2, f64 y2, f64 z2) {
            const f64 d = std::clamp(gm_dot3d_norm(x1, y1, z1, x2, y2, z2), -1.0, 1.0);
            return std::acos(d) * (180.0 / kPi);
        });
        reg.bind("box_distance", [](f64 len, f64 ang) {
            return (1.0 / gm_dcos(45.0 - std::fabs(45.0 - std::fmod(ang, 90.0)))) * len;
        });
        reg.bind("lines_intersect", [](f64 x1, f64 y1, f64 x2, f64 y2, f64 x3, f64 y3, f64 x4, f64 y4, f64 seg) {
            const f64 ux = x2 - x1, uy = y2 - y1;
            const f64 vx = x4 - x3, vy = y4 - y3;
            const f64 wx = x1 - x3, wy = y1 - y3;
            const f64 ud = vy * ux - vx * uy;
            f64 ua = 0.0;
            if (ud != 0.0) {
                ua = (vx * wy - vy * wx) / ud;
                if (seg >= 0.5) {
                    const f64 ub = (ux * wy - uy * wx) / ud;
                    if (ua < 0.0 || ua > 1.0 || ub < 0.0 || ub > 1.0) ua = 0.0;
                }
            }
            return ua;
        });
        reg.bind("pack_bools", [](bool b0, bool b1, bool b2, bool b3, bool b4, bool b5, bool b6, bool b7) {
            u32 v = 0;
            if (b0) v |= 128u;
            if (b1) v |= 64u;
            if (b2) v |= 32u;
            if (b3) v |= 16u;
            if (b4) v |= 8u;
            if (b5) v |= 4u;
            if (b6) v |= 2u;
            if (b7) v |= 1u;
            return static_cast<f64>(v);
        });
        reg.bind("unpack_bool", [](f64 packed, f64 bit) {
            const u8 p = static_cast<u8>(packed);
            const u8 w = static_cast<u8>(bit);
            return static_cast<f64>(p & (1u << w));
        });
    }

    void on_unload(IExtensionHost&) override {
        has_started_ = false;
        token_store_.clear();
        token_sep_.clear();
        token_pos_ = 0;
        token_done_ = true;
        hrt_last_us_ = 0;
        process_.release();
    }

private:
    std::string next_token() {
        if (token_done_ || token_pos_ > token_store_.size()) {
            return {};
        }
        if (token_sep_.empty()) {
            token_done_ = true;
            return token_store_.substr(token_pos_);
        }
        const usize found = token_store_.find(token_sep_, token_pos_);
        if (found == std::string::npos) {
            token_done_ = true;
            return token_store_.substr(token_pos_);
        }
        std::string tok = token_store_.substr(token_pos_, found - token_pos_);
        token_pos_ = found + token_sep_.size();
        if (token_pos_ >= token_store_.size()) {
            token_done_ = true;
        }
        return tok;
    }

    GmBufferStore* buffers_ = nullptr;
    bool has_started_ = false;
    std::string token_store_;
    std::string token_sep_;
    usize token_pos_ = 0;
    bool token_done_ = true;
    u64 hrt_last_us_ = 0;
    SilentProcess process_;
};

} // namespace

std::unique_ptr<IExtension> create_gm82core_extension() {
    return std::make_unique<Gm82CoreExtension>();
}

} // namespace gm
