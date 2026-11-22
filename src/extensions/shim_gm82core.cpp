#include "extensions/shim_gm82core.h"

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
#include <thread>

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
#else
#include <SDL3/SDL_process.h>
#include <cerrno>
#include <sys/wait.h>
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

#if !defined(_WIN32)
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
#else
        int code = 0;
        if (!SDL_WaitProcess(process_, false, &code)) return 259;
        SDL_DestroyProcess(process_);
        process_ = nullptr;
#endif
        return static_cast<f64>(code);
    }

    void release() {
        failed_start_ = false;
        if (!process_) return;
#if defined(_WIN32)
        CloseHandle(process_);
#else
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
#else
    SDL_Process* process_ = nullptr;
#endif
    bool failed_start_ = false;
};

class Gm82CoreShim final : public IDllShim {
public:
    explicit Gm82CoreShim(std::shared_ptr<GmBufferStore> buffers)
        : buffers_(std::move(buffers)) {}

    std::string_view canonical_name() const noexcept override { return "gm82core"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "gm82core";
    }

    bool has_function(std::string_view) const noexcept override { return true; }

    void on_unload(IExtensionHost&) override {
        has_started_ = false;
        token_store_.clear();
        token_sep_.clear();
        token_pos_ = 0;
        token_done_ = true;
        hrt_last_us_ = 0;
        process_.release();
    }

    RValue invoke(
        IExtensionHost& host,
        std::string_view fn,
        std::span<const RValue> args,
        ExternalValueType expected_return) override {
        // ---- Core / Color / Tokenizer ----
        if (fn == "__gm82core_dllcheck") {
            return RValue(820.0);
        }
        if (fn == "color_reverse") {
            const u32 col = static_cast<u32>(std::llround(shim_real(args, 0, 0.0)));
            const u32 rev = ((col & 0xFFu) << 16u) | (col & 0xFF00u) | ((col & 0xFF0000u) >> 16u);
            return RValue(static_cast<f64>(rev));
        }
        if (fn == "color_inverse") {
            const i64 col = static_cast<i64>(std::llround(shim_real(args, 0, 0.0)));
            return RValue(static_cast<f64>(0xFFFFFF - col));
        }
        if (fn == "string_token_start") {
            token_store_ = shim_str(args, 0);
            token_sep_ = shim_str(args, 1);
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
        }
        if (fn == "string_token_next") {
            return RValue(next_token());
        }
        if (fn == "string_token_real") {
            const std::string tok = next_token();
            if (tok.empty()) return RValue(0.0);
            return RValue(std::strtod(tok.c_str(), nullptr));
        }

        // ---- High-Resolution Timer ----
        if (fn == "__hrt_init" || fn == "__gm82core_hrt_init") {
            hrt_last_us_ = Clock::now_us();
            return RValue(1.0);
        }
        if (fn == "hrt_now" || fn == "get_timer" || fn == "hrt_time_now") {
            return RValue(static_cast<f64>(Clock::now_us()));
        }
        if (fn == "hrt_delta" || fn == "get_delta" || fn == "hrt_time_delta") {
            const u64 now = Clock::now_us();
            const u64 diff = (hrt_last_us_ > 0 && now >= hrt_last_us_) ? (now - hrt_last_us_) : 0u;
            hrt_last_us_ = now;
            return RValue(static_cast<f64>(diff));
        }

        // ---- Lovey01 Helpers ----
        if (fn == "power_next") {
            return RValue(power_next_val(shim_real(args, 0, 0.0)));
        }
        if (fn == "file_size") {
            auto sz = host.ext_vfs().file_size(shim_str(args, 0));
            return RValue(sz ? static_cast<f64>(sz.value()) : -1.0);
        }
        if (fn == "real_hex") {
            const std::string s = shim_str(args, 0);
            u64 val = 0;
            for (char c : s) {
                u64 nib = 0;
                if (c >= '0' && c <= '9') nib = static_cast<u64>(c - '0');
                else if (c >= 'a' && c <= 'f') nib = static_cast<u64>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') nib = static_cast<u64>(c - 'A' + 10);
                val = (val << 4u) | nib;
            }
            return RValue(static_cast<f64>(val));
        }
        if (fn == "string_hex") {
            const u64 num = static_cast<u64>(shim_real(args, 0, 0.0));
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(num));
            return RValue(std::string(buf));
        }

        // ---- Perlin Noise ----
        if (fn == "perlin_noise") {
            return RValue(eval_perlin_noise(shim_real(args, 0), shim_real(args, 1), shim_real(args, 2)));
        }

        // ---- Runner Internals (terrible_gm8_hacking.c) ----
        if (fn == "error_is_enabled") {
            return RValue(host.ext_error_is_enabled() ? 1.0 : 0.0);
        }
        if (fn == "error_set_enabled") {
            host.ext_error_set_enabled(shim_real(args, 0, 1.0) > 0.5);
            return RValue(0.0);
        }
        if (fn == "io_set_roomend_clear") {
            host.ext_io_set_roomend_clear(shim_bool(args, 0, true));
            return RValue(0.0);
        }
        if (fn == "window_set_fixed") {
            host.ext_window_set_fixed(shim_bool(args, 0, false));
            return RValue(0.0);
        }
        if (fn == "include_file_count") {
            return RValue(static_cast<f64>(host.ext_game().included_files.size()));
        }
        if (fn == "include_file_exists") {
            const std::string name = shim_str(args, 0);
            for (const auto& inc : host.ext_game().included_files) {
                if (inc.exists && inc.filename == name) return RValue(1.0);
            }
            return RValue(0.0);
        }
        if (fn == "include_file_name") {
            const i32 idx = shim_i32(args, 0, -1);
            const auto& incs = host.ext_game().included_files;
            if (idx >= 0 && static_cast<usize>(idx) < incs.size() && incs[static_cast<usize>(idx)].exists) {
                return RValue(incs[static_cast<usize>(idx)].filename);
            }
            return RValue(std::string{});
        }
        if (fn == "include_file_size") {
            const std::string name = shim_str(args, 0);
            for (const auto& inc : host.ext_game().included_files) {
                if (inc.exists && inc.filename == name) {
                    return RValue(static_cast<f64>(inc.store ? inc.data.size() : static_cast<usize>(std::max(0, inc.size))));
                }
            }
            return RValue(0.0);
        }
        if (fn == "include_file_get_string") {
            const std::string name = shim_str(args, 0);
            for (const auto& inc : host.ext_game().included_files) {
                if (inc.exists && inc.filename == name && inc.store && !inc.data.empty()) {
                    return RValue(std::string(reinterpret_cast<const char*>(inc.data.data()), inc.data.size()));
                }
            }
            return RValue(std::string{});
        }
        if (fn == "__gm82core_include_file_get_buffer") {
            const std::string name = shim_str(args, 0);
            const u32 addr = static_cast<u32>(shim_real(args, 1, 0.0));
            GmBuffer* buf = buffers_ ? buffers_->find_by_address(addr) : nullptr;
            if (!buf) return RValue(0.0);
            for (const auto& inc : host.ext_game().included_files) {
                if (inc.exists && inc.filename == name && inc.store && !inc.data.empty()) {
                    if (buf->data.size() < inc.data.size()) {
                        buf->data.resize(inc.data.size());
                    }
                    std::memcpy(buf->data.data(), inc.data.data(), inc.data.size());
                    return RValue(1.0);
                }
            }
            return RValue(0.0);
        }
        if (fn == "game_get_state") {
            return RValue(static_cast<f64>(host.ext_game_get_state()));
        }
        if (fn == "room_goto_cancel") {
            host.ext_room_goto_cancel();
            return RValue(0.0);
        }

        // ---- OS / Window / Process / Registry / Filesystem (windows.c) ----
        if (fn == "__gm82core_checkstart") {
            if (has_started_) return RValue(0.0);
            has_started_ = true;
            return RValue(1.0);
        }
        if (fn == "__gm82core_getmaximized" || fn == "window_is_maximized") {
            return RValue(host.ext_window().is_maximized() ? 1.0 : 0.0);
        }
        if (fn == "__gm82core_getminimized" || fn == "window_is_minimized") {
            return RValue(host.ext_window().is_minimized() ? 1.0 : 0.0);
        }
        if (fn == "__gm82core_setmaximized" || fn == "window_maximize") {
            host.ext_window().maximize();
            return RValue(0.0);
        }
        if (fn == "__gm82core_setminimized" || fn == "window_minimize") {
            host.ext_window().minimize();
            return RValue(0.0);
        }
        if (fn == "window_restore") {
            host.ext_window().restore();
            return RValue(0.0);
        }
        if (fn == "__gm82core_set_foreground" || fn == "window_set_foreground") {
            host.ext_window().raise_focus();
            return RValue(0.0);
        }
        if (fn == "get_foreground_window" || fn == "__gm82core_getfore") {
            return RValue(host.ext_window().has_focus() ? 1.0 : 0.0);
        }
        if (fn == "window_set_close_enabled" || fn == "window_set_dpiaware") {
            return RValue(fn == "window_set_close_enabled" ? 1.0 : 0.0);
        }
        if (fn == "window_exists") {
            const std::string cap = shim_str(args, 0);
            if (!cap.empty() && cap == host.ext_window().title()) return RValue(1.0);
#if defined(_WIN32)
            return RValue(FindWindowA(nullptr, cap.c_str()) != nullptr ? 1.0 : 0.0);
#else
            return RValue(0.0);
#endif
        }
        if (fn == "get_window_col" || fn == "window_get_caption_color") {
            return RValue(static_cast<f64>(0xFFFFFFu));
        }
        if (fn == "get_windows_version") {
            return RValue(10.0);
        }
        if (fn == "__gm82core_addfonttemp" || fn == "__gm82core_remfonttemp") {
            return RValue(1.0);
        }
        if (fn == "__gm82core_shortfn" || fn == "filename_short") {
            return RValue(shim_str(args, 0));
        }
        if (fn == "__gm82core_execute_program_silent") {
            if (process_.active()) return RValue(75.0);
            return RValue(process_.start(host.ext_vfs(), shim_str(args, 0), false));
        }
        if (fn == "__gm82core_execute_program_silent_exitcode") {
            return RValue(process_.poll());
        }
        if (fn == "__gm82core_execute_program_silent_async" || fn == "execute_program_silent_async") {
            (void)process_.start(host.ext_vfs(), shim_str(args, 0), true);
            return RValue(0.0);
        }
        if (fn == "get_capslock" || fn == "keyboard_get_capslock") {
            return RValue(PlatformWindow::capslock_on() ? 1.0 : 0.0);
        }
        if (fn == "get_scrolllock" || fn == "keyboard_get_scrolllock") {
            return RValue(PlatformWindow::scrolllock_on() ? 1.0 : 0.0);
        }
        if (fn == "get_ram_usage") {
#if defined(_WIN32)
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                     sizeof(pmc))) {
                return RValue(static_cast<f64>(pmc.PrivateUsage));
            }
#endif
            return RValue(0.0);
        }
        if (fn == "get_cpu_usage") {
            return RValue(0.0);
        }
        if (fn == "set_working_directory") {
            host.ext_vfs().set_working_directory(shim_str(args, 0));
            return RValue(0.0);
        }
        if (fn == "set_dll_loaddir" || fn == "__gm82core_setdir") {
            return RValue(0.0);
        }
        if (fn == "io_get_language" || fn == "get_system_language" || fn == "os_get_language") {
#if defined(_WIN32)
            return RValue(static_cast<f64>(GetUserDefaultUILanguage()));
#else
            return RValue(1033.0); // en-US
#endif
        }
        if (fn == "get_system_region" || fn == "os_get_region") {
#if defined(_WIN32)
            return RValue(static_cast<f64>(GetUserDefaultLCID()));
#else
            return RValue(1033.0);
#endif
        }
        if (fn == "sleep_ext") {
            const f64 ms = std::max(0.0, shim_real(args, 0, 0.0));
            if (ms > 0.0) {
                Clock::sleep_until_us(Clock::now_us() + static_cast<u64>(ms * 1000.0));
            }
            return RValue(0.0);
        }
        if (fn == "__registry_read_dword") {
            const std::string dir = shim_str(args, 0);
            const std::string key = shim_str(args, 1);
            if (!host.ext_vfs().registry_exists(dir, key)) return RValue(-4.0);
            return RValue(host.ext_vfs().registry_read_real(dir, key));
        }
        if (fn == "__registry_read_sz") {
            const std::string dir = shim_str(args, 0);
            const std::string key = shim_str(args, 1);
            if (!host.ext_vfs().registry_exists(dir, key)) return RValue(std::string("<undefined>"));
            return RValue(host.ext_vfs().registry_read_string(dir, key));
        }
        if (fn == "__registry_write_dword") {
            const std::string dir = shim_str(args, 0);
            const std::string key = shim_str(args, 1);
            const i32 val = shim_i32(args, 2, 0);
            host.ext_vfs().registry_write_real(dir, key, static_cast<f64>(val));
            return RValue(static_cast<f64>(val));
        }
        if (fn == "file_get_timestamp") {
            const auto path = host.ext_vfs().resolve_for_read(shim_str(args, 0));
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || ec) return RValue(-1.0);
            const auto ftime = std::filesystem::last_write_time(path, ec);
            if (ec) return RValue(-1.0);
            const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            const f64 unix_sec = static_cast<f64>(std::chrono::duration_cast<std::chrono::seconds>(
                sctp.time_since_epoch()).count());
            // Delphi TDateTime epoch is 1899-12-30 (25569 days before 1970-01-01).
            return RValue(unix_sec / 86400.0 + 25569.0);
        }
        if (fn == "date_get_current_timezone") {
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
            return RValue(static_cast<f64>(diff_sec / 3600));
        }
        if (fn == "get_battery_level") {
#if defined(_WIN32)
            SYSTEM_POWER_STATUS sps{};
            if (GetSystemPowerStatus(&sps)) {
                if (sps.BatteryFlag >= 128) return RValue(0.0);
                return RValue(static_cast<f64>(sps.BatteryLifePercent));
            }
#endif
            return RValue(100.0);
        }
        if (fn == "get_battery_status") {
#if defined(_WIN32)
            SYSTEM_POWER_STATUS sps{};
            if (GetSystemPowerStatus(&sps)) {
                if (sps.BatteryFlag >= 128) return RValue(0.0);
                if (sps.ACLineStatus == 1) return RValue(2.0);
                return RValue(1.0);
            }
#endif
            return RValue(0.0);
        }
        if (fn == "file_create") {
            const std::string p = shim_str(args, 0);
            if (host.ext_vfs().exists(p)) return RValue(1.0);
            return RValue(host.ext_vfs().write_all_bytes(p, {}) ? 1.0 : 0.0);
        }
        if (fn == "process_exists") {
#if defined(_WIN32)
            const std::string target = ascii_to_lower(shim_str(args, 0));
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) return RValue(0.0);
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
            return RValue(found ? 1.0 : 0.0);
#else
            return RValue(0.0);
#endif
        }

        // ---- Math, Trig, Angles, Geometry, Bit Packing (math.c) ----
        if (fn == "modwrap") {
            return RValue(gm_modwrap(shim_real(args, 0), shim_real(args, 1), shim_real(args, 2)));
        }
        if (fn == "lerp2") {
            const f64 fa = shim_real(args, 0), fb = shim_real(args, 1);
            const f64 ta = shim_real(args, 2), tb = shim_real(args, 3), v = shim_real(args, 4);
            return RValue(((v - fa) / (fb - fa)) * (tb - ta) + ta);
        }
        if (fn == "clerp2") {
            const f64 fa = shim_real(args, 0), fb = shim_real(args, 1);
            const f64 ta = shim_real(args, 2), tb = shim_real(args, 3), v = shim_real(args, 4);
            const f64 t = std::clamp((v - fa) / (fb - fa), 0.0, 1.0);
            return RValue(t * (tb - ta) + ta);
        }
        if (fn == "unlerp") {
            const f64 a = shim_real(args, 0), b = shim_real(args, 1), v = shim_real(args, 2);
            return RValue((v - a) / (b - a));
        }
        if (fn == "unclerp") {
            const f64 a = shim_real(args, 0), b = shim_real(args, 1), v = shim_real(args, 2);
            return RValue(std::clamp((v - a) / (b - a), 0.0, 1.0));
        }
        if (fn == "esign") {
            const f64 x = shim_real(args, 0), def = shim_real(args, 1);
            return RValue(x == 0.0 ? def : (x > 0.0 ? 1.0 : -1.0));
        }
        if (fn == "saturate") {
            return RValue(std::clamp(shim_real(args, 0), 0.0, 1.0));
        }
        if (fn == "smoothstep") {
            const f64 mn = shim_real(args, 0), mx = shim_real(args, 1), v = shim_real(args, 2);
            const f64 t = std::clamp((v - mn) / (mx - mn), 0.0, 1.0);
            return RValue(t * t * (3.0 - 2.0 * t));
        }
        if (fn == "approach" || fn == "inch") {
            const f64 val = shim_real(args, 0), go = shim_real(args, 1), step = shim_real(args, 2);
            return RValue(val < go ? std::min(go, val + step) : std::max(go, val - step));
        }
        if (fn == "lerproach") {
            f64 val = shim_real(args, 0);
            const f64 go = shim_real(args, 1), l = shim_real(args, 2), a = shim_real(args, 3);
            val = val * (1.0 - l) + go * l;
            return RValue(val < go ? std::min(go, val + a) : std::max(go, val - a));
        }
        if (fn == "round_unbiased") {
            return RValue(std::floor(shim_real(args, 0) + 0.5));
        }
        if (fn == "roundto") {
            const f64 val = shim_real(args, 0), to = shim_real(args, 1, 1.0);
            return RValue(std::round(val / to) * to);
        }
        if (fn == "roundto_unbiased") {
            const f64 val = shim_real(args, 0), to = shim_real(args, 1, 1.0);
            return RValue(std::floor(val / to + 0.5) * to);
        }
        if (fn == "floorto") {
            const f64 val = shim_real(args, 0), to = shim_real(args, 1, 1.0);
            return RValue(std::floor(val / to) * to);
        }
        if (fn == "ceilto") {
            const f64 val = shim_real(args, 0), to = shim_real(args, 1, 1.0);
            return RValue(std::ceil(val / to) * to);
        }
        if (fn == "cosine") {
            const f64 a = shim_real(args, 0), b = shim_real(args, 1), amt = shim_real(args, 2);
            const f64 mu = (1.0 - std::cos(amt * kPi)) * 0.5;
            return RValue(a * (1.0 - mu) + b * mu);
        }
        if (fn == "angle_difference") {
            return RValue(gm_modwrap(shim_real(args, 1) - shim_real(args, 0) + 540.0, 0.0, 360.0) - 180.0);
        }
        if (fn == "angle_mean") {
            const f64 a1 = shim_real(args, 0), a2 = shim_real(args, 1);
            return RValue(a1 + 0.5 * (gm_modwrap(a2 - a1 + 540.0, 0.0, 360.0) - 180.0));
        }
        if (fn == "approach_angle" || fn == "inch_angle") {
            const f64 a1 = shim_real(args, 0), a2 = shim_real(args, 1), step = shim_real(args, 2);
            const f64 d = std::fmod(a2 - a1 + 540.0, 360.0) - 180.0;
            if (d > 0.0) return RValue(std::fmod(a1 + std::min(d, step), 360.0));
            return RValue(std::fmod(360.0 - std::fmod(360.0 - (a1 + std::max(-step, d)), 360.0), 360.0));
        }
        if (fn == "approach_angle_biased") {
            const f64 a1 = shim_real(args, 0), a2 = shim_real(args, 1);
            const f64 step = shim_real(args, 2), bias = shim_real(args, 3);
            const f64 d = std::fmod(a2 - a1 + 540.0, 360.0) - 180.0;
            if (std::fabs(std::fabs(d) - 180.0) < std::fabs(bias)) {
                return RValue(gm_modwrap(a1 + std::min(std::fabs(d), step) * gm_signnum(bias), 0.0, 360.0));
            }
            if (d > 0.0) return RValue(std::fmod(a1 + std::min(d, step), 360.0));
            return RValue(std::fmod(360.0 - std::fmod(360.0 - (a1 + std::max(-step, d)), 360.0), 360.0));
        }
        if (fn == "lerp_angle") {
            const f64 from = shim_real(args, 0), to = shim_real(args, 1), amt = shim_real(args, 2);
            return RValue(from + (gm_modwrap(to - from + 540.0, 0.0, 360.0) - 180.0) * amt);
        }
        if (fn == "angle_abs") {
            return RValue(std::fabs(gm_modwrap(shim_real(args, 0) + 540.0, 0.0, 360.0) - 180.0));
        }
        if (fn == "darccos") return RValue(std::acos(std::clamp(shim_real(args, 0), -1.0, 1.0)) * (180.0 / kPi));
        if (fn == "darcsin") return RValue(std::asin(std::clamp(shim_real(args, 0), -1.0, 1.0)) * (180.0 / kPi));
        if (fn == "darctan") return RValue(std::atan(shim_real(args, 0)) * (180.0 / kPi));
        if (fn == "darctan2") return RValue(std::atan2(shim_real(args, 0), shim_real(args, 1)) * (180.0 / kPi));
        if (fn == "dcos") return RValue(gm_dcos(shim_real(args, 0)));
        if (fn == "dsin") return RValue(gm_dsin(shim_real(args, 0)));
        if (fn == "dtan") return RValue(std::tan(shim_real(args, 0) * (kPi / 180.0)));
        if (fn == "secant") return RValue(1.0 / std::cos(shim_real(args, 0)));
        if (fn == "dsecant") return RValue(1.0 / gm_dcos(shim_real(args, 0)));
        if (fn == "range_collision") {
            const f64 a1 = shim_real(args, 0), a2 = shim_real(args, 1);
            const f64 b1 = shim_real(args, 2), b2 = shim_real(args, 3);
            return RValue((std::max(a1, a2) > std::min(b1, b2) && std::max(b1, b2) > std::min(a1, a2)) ? 1.0 : 0.0);
        }
        if (fn == "triangle_is_clockwise") {
            const f64 x0 = shim_real(args, 0), y0 = shim_real(args, 1);
            const f64 x1 = shim_real(args, 2), y1 = shim_real(args, 3);
            const f64 x2 = shim_real(args, 4), y2 = shim_real(args, 5);
            int cw = 0;
            if (x0 != x1 || y0 != y1) {
                if (x0 == x1) {
                    cw = (x2 < x1) ^ (y0 > y1);
                } else {
                    const f64 m = (y0 - y1) / (x0 - x1);
                    cw = (y2 > (m * x2 + y0 - m * x0)) ^ (x0 > x1);
                }
            }
            return RValue(static_cast<f64>(cw));
        }
        if (fn == "lengthdir_zx") {
            return RValue(shim_real(args, 0) * gm_dcos(shim_real(args, 2)) * gm_dcos(shim_real(args, 1)));
        }
        if (fn == "lengthdir_zy") {
            return RValue(-shim_real(args, 0) * gm_dcos(shim_real(args, 2)) * gm_dsin(shim_real(args, 1)));
        }
        if (fn == "lengthdir_zz") {
            return RValue(-shim_real(args, 0) * gm_dsin(shim_real(args, 2)));
        }
        if (fn == "point_direction_pitch") {
            const f64 x1 = shim_real(args, 0), y1 = shim_real(args, 1), z1 = shim_real(args, 2);
            const f64 x2 = shim_real(args, 3), y2 = shim_real(args, 4), z2 = shim_real(args, 5);
            return RValue(gm_pointdir(0.0, z1, std::hypot(x2 - x1, y2 - y1), z2));
        }
        if (fn == "pivot_pos_x") {
            const f64 px = shim_real(args, 0), py = shim_real(args, 1), dir = shim_real(args, 2);
            return RValue(px * gm_dcos(dir) + py * gm_dcos(dir - 90.0));
        }
        if (fn == "pivot_pos_y") {
            const f64 px = shim_real(args, 0), py = shim_real(args, 1), dir = shim_real(args, 2);
            return RValue(-px * gm_dsin(dir) - py * gm_dsin(dir - 90.0));
        }
        if (fn == "point_in_circle") {
            return RValue(std::hypot(shim_real(args, 2) - shim_real(args, 0),
                                     shim_real(args, 3) - shim_real(args, 1)) < shim_real(args, 4) ? 1.0 : 0.0);
        }
        if (fn == "circle_in_circle") {
            return RValue(std::hypot(shim_real(args, 3) - shim_real(args, 0),
                                     shim_real(args, 4) - shim_real(args, 1)) < (shim_real(args, 2) + shim_real(args, 5)) ? 1.0 : 0.0);
        }
        if (fn == "rectangle_in_circle") {
            const f64 x1 = shim_real(args, 0), y1 = shim_real(args, 1);
            const f64 x2 = shim_real(args, 2), y2 = shim_real(args, 3);
            const f64 cx = shim_real(args, 4), cy = shim_real(args, 5), cr = shim_real(args, 6);
            const f64 tx = std::clamp(cx, x1, x2);
            const f64 ty = std::clamp(cy, y1, y2);
            return RValue(std::hypot(cx - tx, cy - ty) < cr ? 1.0 : 0.0);
        }
        if (fn == "point_in_rectangle") {
            const f64 px = shim_real(args, 0), py = shim_real(args, 1);
            return RValue((px >= shim_real(args, 2) && px < shim_real(args, 4) &&
                           py >= shim_real(args, 3) && py < shim_real(args, 5)) ? 1.0 : 0.0);
        }
        if (fn == "rectangle_in_rectangle") {
            f64 ax1 = shim_real(args, 0), ay1 = shim_real(args, 1);
            f64 ax2 = shim_real(args, 2), ay2 = shim_real(args, 3);
            f64 bx1 = shim_real(args, 4), by1 = shim_real(args, 5);
            f64 bx2 = shim_real(args, 6), by2 = shim_real(args, 7);
            if (ax1 > ax2) std::swap(ax1, ax2);
            if (ay1 > ay2) std::swap(ay1, ay2);
            if (bx1 > bx2) std::swap(bx1, bx2);
            if (by1 > by2) std::swap(by1, by2);
            if (ax1 >= bx1 && ax2 <= bx2 && ay1 >= by1 && ay2 <= by2) return RValue(1.0);
            if (ax1 > bx2 || ax2 < bx1 || ay1 > by2 || ay2 < by1) return RValue(0.0);
            return RValue(2.0);
        }
        if (fn == "point_in_triangle") {
            const f64 x0 = shim_real(args, 0), y0 = shim_real(args, 1);
            const f64 x1 = shim_real(args, 2), y1 = shim_real(args, 3);
            const f64 x2 = shim_real(args, 4), y2 = shim_real(args, 5);
            const f64 x3 = shim_real(args, 6), y3 = shim_real(args, 7);
            const f64 a = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            const f64 b = (x2 - x0) * (y3 - y0) - (x3 - x0) * (y2 - y0);
            const f64 c = (x3 - x0) * (y1 - y0) - (x1 - x0) * (y3 - y0);
            return RValue((gm_signnum(a) == gm_signnum(b) && gm_signnum(b) == gm_signnum(c)) ? 1.0 : 0.0);
        }
        if (fn == "in_range") {
            const f64 v = shim_real(args, 0);
            return RValue((v >= shim_real(args, 1) && v <= shim_real(args, 2)) ? 1.0 : 0.0);
        }
        if (fn == "point_line_lerp") {
            return RValue(gm_point_line_lerp(
                shim_real(args, 0), shim_real(args, 1),
                shim_real(args, 2), shim_real(args, 3),
                shim_real(args, 4), shim_real(args, 5), shim_real(args, 6)));
        }
        if (fn == "point_line_distance") {
            const f64 px = shim_real(args, 0), py = shim_real(args, 1);
            const f64 x1 = shim_real(args, 2), y1 = shim_real(args, 3);
            const f64 x2 = shim_real(args, 4), y2 = shim_real(args, 5);
            const f64 t = gm_point_line_lerp(px, py, x1, y1, x2, y2, shim_real(args, 6));
            return RValue(std::hypot(px - (x1 + t * (x2 - x1)), py - (y1 + t * (y2 - y1))));
        }
        if (fn == "dot_product_3d_normalized") {
            return RValue(gm_dot3d_norm(
                shim_real(args, 0), shim_real(args, 1), shim_real(args, 2),
                shim_real(args, 3), shim_real(args, 4), shim_real(args, 5)));
        }
        if (fn == "dot_product_normalized") {
            const f64 x1 = shim_real(args, 0), y1 = shim_real(args, 1);
            const f64 x2 = shim_real(args, 2), y2 = shim_real(args, 3);
            const f64 a = std::hypot(x1, y1), b = std::hypot(x2, y2);
            if (a * b == 0.0) return RValue(1.0);
            return RValue((x1 / a) * (x2 / b) + (y1 / a) * (y2 / b));
        }
        if (fn == "angle_difference_3d") {
            const f64 d = std::clamp(gm_dot3d_norm(
                shim_real(args, 0), shim_real(args, 1), shim_real(args, 2),
                shim_real(args, 3), shim_real(args, 4), shim_real(args, 5)), -1.0, 1.0);
            return RValue(std::acos(d) * (180.0 / kPi));
        }
        if (fn == "box_distance") {
            const f64 len = shim_real(args, 0), ang = shim_real(args, 1);
            return RValue((1.0 / gm_dcos(45.0 - std::fabs(45.0 - std::fmod(ang, 90.0)))) * len);
        }
        if (fn == "lines_intersect") {
            const f64 x1 = shim_real(args, 0), y1 = shim_real(args, 1);
            const f64 x2 = shim_real(args, 2), y2 = shim_real(args, 3);
            const f64 x3 = shim_real(args, 4), y3 = shim_real(args, 5);
            const f64 x4 = shim_real(args, 6), y4 = shim_real(args, 7);
            const f64 seg = shim_real(args, 8);
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
            return RValue(ua);
        }
        if (fn == "pack_bools") {
            u32 v = 0;
            if (shim_bool(args, 0)) v |= 128u;
            if (shim_bool(args, 1)) v |= 64u;
            if (shim_bool(args, 2)) v |= 32u;
            if (shim_bool(args, 3)) v |= 16u;
            if (shim_bool(args, 4)) v |= 8u;
            if (shim_bool(args, 5)) v |= 4u;
            if (shim_bool(args, 6)) v |= 2u;
            if (shim_bool(args, 7)) v |= 1u;
            return RValue(static_cast<f64>(v));
        }
        if (fn == "unpack_bool") {
            const u8 p = static_cast<u8>(shim_real(args, 0));
            const u8 w = static_cast<u8>(shim_real(args, 1));
            return RValue(static_cast<f64>(p & (1u << w)));
        }

        GM_LOG_DEBUG("ext", "gm82core: unhandled export '%.*s'", static_cast<int>(fn.size()), fn.data());
        return expected_return == ExternalValueType::String ? RValue(std::string{}) : RValue(0.0);
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

    std::shared_ptr<GmBufferStore> buffers_;
    bool has_started_ = false;
    std::string token_store_;
    std::string token_sep_;
    usize token_pos_ = 0;
    bool token_done_ = true;
    u64 hrt_last_us_ = 0;
    SilentProcess process_;
};

} // namespace

std::unique_ptr<IDllShim> create_gm82core_shim(std::shared_ptr<GmBufferStore> buffers) {
    return std::make_unique<Gm82CoreShim>(std::move(buffers));
}

} // namespace gm
