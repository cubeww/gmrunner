#include "app/cli.h"
#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "gml/compiler.h"
#include "graphics/gl_graphics_device.h"
#include "loader/exe_loader.h"
#include "platform/clock.h"
#include "platform/platform.h"
#include "platform/vfs.h"
#include "platform/web_bridge.h"
#include "platform/window.h"
#include "runtime/runner.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(__ANDROID__)
// SDL supplies UTF-8 argv on Windows and bridges Java SDLActivity -> SDL_main on Android.
#include <SDL3/SDL_main.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

namespace {

struct AppState {
    gm::Vfs vfs;
    gm::PlatformWindow window;
    gm::GlGraphicsDevice gfx;
    std::unique_ptr<gm::Runner> runner;
    bool running = true;
    bool loop_active = false;
    bool in_tick = false;
    gm::u64 frame_count = 0;
    gm::u64 start_us = 0;
    gm::u64 last_raf_us = 0;
    gm::u64 accum_us = 0;
    gm::u64 last_status_us = 0;
    gm::u64 target_frame_us = 16667; // 60 FPS default for idle mode
};

std::string game_storage_key(std::string_view name) {
    std::string key;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : name) {
        if (c >= 128 || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            key.push_back(static_cast<char>(c));
        } else {
            key += '~';
            key += hex[c >> 4];
            key += hex[c & 15];
        }
    }
    // Leave room for the layout file extension even for full-length archive names.
    if (key.size() > 240) {
        gm::u64 hash = 14695981039346656037ull;
        for (unsigned char c : name) hash = (hash ^ c) * 1099511628211ull;
        size_t end = 214;
        while ((static_cast<unsigned char>(key[end]) & 0xc0) == 0x80) --end;
        key.resize(end);
        key += "~h" + std::to_string(hash);
    }
    return key.empty() ? "default" : key;
}

bool is_ignorable_exe(std::string_view lower_name) noexcept {
    return lower_name.starts_with("unins") ||
           lower_name.starts_with("vcredist") ||
           lower_name == "dxsetup.exe" ||
           lower_name == "crashreporter.exe";
}

std::string find_exe_in_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::string best_name;
    gm::u64 best_size = 0;
    if (!std::filesystem::is_directory(dir, ec) || ec) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = gm::Vfs::path_to_utf8(entry.path().filename());
        const std::string lower = gm::ascii_to_lower(name);
        if (!lower.ends_with(".exe") || is_ignorable_exe(lower)) continue;
        const gm::u64 sz = static_cast<gm::u64>(entry.file_size(ec));
        if (!ec && (best_name.empty() || sz > best_size)) {
            best_name = name;
            best_size = sz;
        }
    }
    return best_name;
}

void compute_initial_window_size(const gm::GameData& game, gm::i32& out_w, gm::i32& out_h) {
    if (game.room_order.empty()) return;
    const gm::i32 first_id = game.room_order[0];
    if (first_id < 0 || static_cast<gm::usize>(first_id) >= game.rooms.size() ||
        !game.rooms[static_cast<gm::usize>(first_id)].exists) {
        return;
    }
    const auto& rm = game.rooms[static_cast<gm::usize>(first_id)];
    gm::i32 w = rm.width;
    gm::i32 h = rm.height;
    if (rm.enable_views) {
        gm::i32 max_r = 0, max_b = 0;
        for (const auto& v : rm.views) {
            if (!v.visible) continue;
            max_r = std::max(max_r, v.xport + v.wport);
            max_b = std::max(max_b, v.yport + v.hport);
        }
        if (max_r > 0 && max_b > 0) {
            w = max_r;
            h = max_b;
        }
    }
    out_w = std::max(1, w);
    out_h = std::max(1, h);
}

std::filesystem::path user_save_root() {
#if defined(__ANDROID__)
    if (const char* path = SDL_GetAndroidInternalStoragePath()) return gm::Vfs::path_from_utf8(path) / "saves";
#endif
    if (char* pref = SDL_GetPrefPath("gmrunner", "saves")) {
        auto dir = gm::Vfs::path_from_utf8(pref);
        SDL_free(pref);
        return dir;
    }
    return {};
}

std::filesystem::path default_touch_layout_path(std::string_view game_key) {
    if (char* pref = SDL_GetPrefPath("gmrunner", "config")) {
        auto root = gm::Vfs::path_from_utf8(pref) / "touch_layouts";
        SDL_free(pref);
        return root / gm::Vfs::path_from_utf8(game_storage_key(game_key) + ".cfg");
    }
    return {};
}

struct MountedTarget {
    std::string exe_lookup_path;
    std::string game_key;
};

gm::Result<MountedTarget> mount_native_game_target(
    gm::Vfs& vfs,
    const std::string& target_path,
    const std::string& save_dir_override,
    bool allow_sdl_pref_fallback) {
    std::filesystem::path game_root = std::filesystem::current_path();
    std::string exe_lookup_path;
    std::string game_key = "default";

    if (!target_path.empty()) {
        const auto fs_path = gm::Vfs::path_from_utf8(target_path);
        const std::string lower_target = gm::ascii_to_lower(target_path);
        std::error_code ec;

        if (lower_target.ends_with(".zip") && std::filesystem::is_regular_file(fs_path, ec)) {
            if (auto res = vfs.mount_zip(fs_path); !res) {
                return res.error();
            }
            exe_lookup_path = vfs.default_exe_path();
            if (exe_lookup_path.empty()) {
                return gm::Error{"No GameMaker .exe found inside ZIP archive: " + target_path};
            }
            game_key = gm::Vfs::path_to_utf8(fs_path.stem());
        } else if (std::filesystem::is_directory(fs_path, ec)) {
            game_root = fs_path;
            if (auto res = vfs.mount_game_root(game_root); !res) {
                return res.error();
            }
            exe_lookup_path = find_exe_in_dir(game_root);
            if (exe_lookup_path.empty()) {
                return gm::Error{"No GameMaker .exe found in directory: " + target_path};
            }
            game_key = gm::Vfs::path_to_utf8(game_root.filename());
        } else {
            if (fs_path.has_parent_path()) {
                game_root = fs_path.parent_path();
                exe_lookup_path = gm::Vfs::path_to_utf8(fs_path.filename());
            } else {
                exe_lookup_path = target_path;
            }
            if (auto res = vfs.mount_game_root(game_root); !res) {
                return res.error();
            }
            game_key = gm::Vfs::path_to_utf8(fs_path.stem());
        }
    } else {
        (void)vfs.mount_game_root(game_root);
    }

    // Configure writable save overlay directory.
    std::filesystem::path save_root;
    if (!save_dir_override.empty()) {
        save_root = gm::Vfs::path_from_utf8(save_dir_override);
#if !defined(__ANDROID__)
    } else if (!vfs.has_virtual_root() && gm::Vfs::is_directory_writable(game_root)) {
        save_root = game_root;
#endif
    } else if (allow_sdl_pref_fallback) {
        const auto root = user_save_root();
        if (!root.empty()) {
            save_root = root / gm::Vfs::path_from_utf8(game_storage_key(game_key));
        }
    }

    if (!save_root.empty()) {
        if (auto res = vfs.mount_save_root(save_root); !res) {
            GM_LOG_WARN("app", "%s", res.error_message().c_str());
        }
    }
    return MountedTarget{std::move(exe_lookup_path), std::move(game_key)};
}

void tick_idle_frame(AppState& app) {
    const gm::WindowEvents events = app.window.poll_events();
    if (events.quit_requested) {
        app.running = false;
        return;
    }

    const gm::InputState& input = app.window.input();
    if (input.key_pressed[27]) { // vk_escape
        app.running = false;
        return;
    }
    if (input.key_pressed[122]) { // vk_f11
        app.window.set_fullscreen(!app.window.is_fullscreen());
    }

    const gm::i32 dw = app.window.drawable_width();
    const gm::i32 dh = app.window.drawable_height();
    app.gfx.set_viewport(gm::ViewportRect{0, 0, dw, dh});

    const gm::f64 t = static_cast<gm::f64>(gm::Clock::now_us() - app.start_us) * 1e-6;
    const gm::f32 r = 0.08f + 0.04f * static_cast<gm::f32>(std::sin(t * 1.1));
    const gm::f32 g = 0.11f + 0.04f * static_cast<gm::f32>(std::sin(t * 1.3 + 1.0));
    const gm::f32 b = 0.16f + 0.05f * static_cast<gm::f32>(std::sin(t * 0.9 + 2.0));
    app.gfx.clear(true, true, false, r, g, b, 1.0f);

    app.window.swap_buffers();
    ++app.frame_count;
}

void tick_app_frame(AppState& app) {
    if (app.runner) {
        app.runner->tick_frame();
        if (!app.runner->is_running()) {
            app.running = false;
        }
    } else {
        tick_idle_frame(app);
    }
}

#if defined(__EMSCRIPTEN__)

std::unique_ptr<AppState> g_web_app;

void stop_web_game_internal() {
    if (!g_web_app) return;
    if (g_web_app->loop_active) {
        g_web_app->loop_active = false;
        emscripten_cancel_main_loop();
    }
    if (g_web_app->runner) {
        g_web_app->runner->shutdown();
        g_web_app->runner.reset();
    }
    gm::web_flush_persist_sync();
    g_web_app->running = false;
    g_web_app->last_raf_us = 0;
    g_web_app->accum_us = 0;
}

#endif // __EMSCRIPTEN__

} // namespace

#if defined(__EMSCRIPTEN__)

extern "C" {

EMSCRIPTEN_KEEPALIVE
void gmrunner_web_frame(void* userdata) {
    auto* app = static_cast<AppState*>(userdata);
    if (!app || app->in_tick) {
        // Prevent Asyncify/JSPI re-entrancy while an async asset read or sleep is suspended.
        return;
    }
    if (!app->running) {
        if (app->loop_active) {
            app->loop_active = false;
            emscripten_cancel_main_loop();
        }
        if (app->runner) {
            app->runner->shutdown();
            app->runner.reset();
        }
        gm::web_flush_persist_sync();
        gm::web_report_status("Game ended");
        return;
    }

    struct TickGuard {
        bool& flag;
        ~TickGuard() { flag = false; }
    } guard{app->in_tick};
    app->in_tick = true;

    const gm::u64 now = gm::Clock::now_us();
    if (app->last_raf_us == 0) {
        app->last_raf_us = now;
    }
    const gm::u64 dt = std::min<gm::u64>(now - app->last_raf_us, 100000u);
    app->last_raf_us = now;
    app->accum_us += dt;

    const gm::u64 step_us = app->runner ? app->runner->target_frame_us() : app->target_frame_us;
    // Allow a small 1.5 ms tolerance so 60 FPS games lock 1:1 to 60 Hz requestAnimationFrame.
    const gm::u64 threshold_us = (step_us > 1500u) ? (step_us - 1500u) : step_us;
    int steps = 0;
    while (app->accum_us >= threshold_us && steps < 2 && app->running) {
        app->accum_us = (app->accum_us >= step_us) ? (app->accum_us - step_us) : 0u;
        tick_app_frame(*app);
        ++steps;
    }
    if (app->accum_us > step_us * 2u) {
        app->accum_us = 0u;
    }

    if (app->runner && (now - app->last_status_us) >= 1000000u) {
        app->last_status_us = now;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Running | %d FPS (target %d)",
                      app->runner->current_fps(), app->runner->room_speed());
        gm::web_report_status(buf);
    }
}

EMSCRIPTEN_KEEPALIVE
void gmrunner_web_stop() {
    stop_web_game_internal();
    gm::web_report_status("Stopped");
}

EMSCRIPTEN_KEEPALIVE
void gmrunner_web_set_touch_overlay(int enabled) {
    if (g_web_app && g_web_app->runner) {
        g_web_app->runner->set_touch_overlay_enabled(enabled != 0);
    }
}

EMSCRIPTEN_KEEPALIVE
void gmrunner_web_set_touch_edit_mode(int editing) {
    if (g_web_app && g_web_app->runner) {
        if (editing != 0) {
            g_web_app->runner->set_touch_overlay_enabled(true);
        }
        g_web_app->runner->set_touch_overlay_editing(editing != 0);
    }
}

EMSCRIPTEN_KEEPALIVE
int gmrunner_web_is_touch_editing() {
    return (g_web_app && g_web_app->runner && g_web_app->runner->is_touch_overlay_editing()) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void gmrunner_web_set_muted(int muted) {
    if (g_web_app && g_web_app->runner) {
        g_web_app->runner->set_audio_muted(muted != 0);
    }
}

EMSCRIPTEN_KEEPALIVE
int gmrunner_web_launch(const char* mode_cstr, const char* path_cstr, const char* game_key_cstr) {
    if (!g_web_app) return 1;
    stop_web_game_internal();

    const std::string mode = mode_cstr ? mode_cstr : "memfs";
    const std::string path_arg = path_cstr ? path_cstr : "";
    const std::string_view name = game_key_cstr && game_key_cstr[0] ? game_key_cstr : "web_game";
    const auto game_key = game_storage_key(name);

    gm::web_report_status("Mounting persistent storage...");
    std::string persist_dir = "/persist/" + game_key;
    if (auto p_res = gm::web_mount_persistent_storage(game_key); p_res) {
        persist_dir = p_res.value();
    } else {
        GM_LOG_WARN("web", "%s", p_res.error_message().c_str());
    }

    auto& app = *g_web_app;
    (void)app.vfs.mount_save_root(gm::Vfs::path_from_utf8(persist_dir));
    app.vfs.set_registry_dir(gm::Vfs::path_from_utf8(persist_dir + "/registry"));
    app.vfs.set_persist_callback(&gm::web_request_persist_sync);

    std::string exe_lookup_path;
    if (mode == "folder") {
        auto src_res = gm::create_web_lazy_folder_source();
        if (!src_res) {
            GM_LOG_ERROR("web", "%s", src_res.error_message().c_str());
            gm::web_report_status("Error: " + src_res.error_message());
            return 2;
        }
        (void)app.vfs.mount_read_source(std::move(src_res.value()), std::filesystem::path("/game"));
        exe_lookup_path = !path_arg.empty() ? path_arg : app.vfs.default_exe_path();
    } else if (mode == "zip") {
        gm::web_report_status("Indexing ZIP archive...");
        auto src_res = gm::create_web_lazy_zip_source();
        if (!src_res) {
            GM_LOG_ERROR("web", "%s", src_res.error_message().c_str());
            gm::web_report_status("Error: " + src_res.error_message());
            return 3;
        }
        (void)app.vfs.mount_read_source(std::move(src_res.value()), std::filesystem::path("/game"));
        exe_lookup_path = !path_arg.empty() ? path_arg : app.vfs.default_exe_path();
    } else {
        auto m_res = mount_native_game_target(app.vfs, path_arg, persist_dir, false);
        if (!m_res) {
            GM_LOG_ERROR("web", "%s", m_res.error_message().c_str());
            gm::web_report_status("Error: " + m_res.error_message());
            return 4;
        }
        exe_lookup_path = m_res.value().exe_lookup_path;
    }

    if (exe_lookup_path.empty()) {
        GM_LOG_ERROR("web", "No GameMaker executable found to launch");
        gm::web_report_status("Error: No .exe found");
        return 5;
    }

    gm::web_report_status("Loading " + exe_lookup_path + "...");
    gm::LoadOptions load_opts{};
    const gm::u64 t0 = gm::Clock::now_us();
    auto game_res = gm::ExeLoader::load_from_vfs(app.vfs, exe_lookup_path, load_opts);
    if (!game_res) {
        GM_LOG_ERROR("loader", "Failed to load '%s': %s", exe_lookup_path.c_str(), game_res.error_message().c_str());
        gm::web_report_status("Load failed: " + game_res.error_message());
        return 6;
    }
    gm::GameData loaded_game = std::move(game_res.value());
    GM_LOG_INFO("loader", "Loaded '%s' (GM %.*s) in %.1f ms",
                exe_lookup_path.c_str(),
                static_cast<int>(gm::version_mode_name(loaded_game.meta.detected_version).size()),
                gm::version_mode_name(loaded_game.meta.detected_version).data(),
                static_cast<gm::f64>(gm::Clock::now_us() - t0) * 1e-3);

    gm::web_report_status("Compiling GML bytecode...");
    const gm::u64 tc0 = gm::Clock::now_us();
    gm::CompiledProgram compiled = gm::GmlCompiler::compile_game(loaded_game);
    GM_LOG_INFO("gml", "Compiled %zu instructions (%zu errors) in %.1f ms",
                compiled.stats.total_instructions,
                compiled.stats.total_errors,
                static_cast<gm::f64>(gm::Clock::now_us() - tc0) * 1e-3);

    gm::i32 win_w = 800, win_h = 608;
    compute_initial_window_size(loaded_game, win_w, win_h);
    app.window.set_size(win_w, win_h);

    std::vector<std::string> cmd_args = {exe_lookup_path};
    app.runner = std::make_unique<gm::Runner>(app.vfs, app.window, app.gfx);
    if (auto res = app.runner->init(std::move(loaded_game), std::move(compiled), std::move(cmd_args)); !res) {
        GM_LOG_ERROR("runtime", "Runner initialization failed: %s", res.error_message().c_str());
        app.runner.reset();
        gm::web_report_status("Init failed: " + res.error_message());
        return 7;
    }
    app.runner->set_touch_layout_path(gm::Vfs::path_from_utf8(persist_dir + "/registry/touch_layout.cfg"));

    app.running = true;
    app.start_us = gm::Clock::now_us();
    app.last_raf_us = app.start_us;
    app.last_status_us = app.start_us;
    app.accum_us = 0;
    app.loop_active = true;

    gm::web_report_status("Running");
    emscripten_set_main_loop_arg(&gmrunner_web_frame, &app, 0, 0);
    return 0;
}

} // extern "C"

#endif // __EMSCRIPTEN__

int main(int argc, char** argv) {
    auto cli_res = gm::parse_cli(argc, argv);
    if (!cli_res) {
        GM_LOG_ERROR("app", "%s", cli_res.error_message().c_str());
        gm::print_cli_help();
        return 1;
    }

    const gm::CliOptions& opts = cli_res.value();
    if (opts.show_help) {
        gm::print_cli_help();
        return 0;
    }

    if (opts.verbose || opts.trace.events || opts.trace.dll_calls || opts.trace.vfs) {
        gm::Logger::set_level(opts.trace.events || opts.trace.dll_calls || opts.trace.vfs
                                  ? gm::LogLevel::Trace
                                  : gm::LogLevel::Debug);
    }
    gm::Logger::set_trace_flags(opts.trace);

    GM_LOG_INFO("app", "gmrunner (%s, version_override=%s, codepage=%u)",
                gm::Platform::platform_name(),
                std::string(gm::version_mode_name(opts.version_override)).c_str(),
                opts.codepage);

#if defined(__EMSCRIPTEN__)
    if (auto init_res = gm::Platform::init(); !init_res) {
        GM_LOG_FATAL("app", "%s", init_res.error_message().c_str());
        return 1;
    }

    g_web_app = std::make_unique<AppState>();
    if (auto res = g_web_app->vfs.init_temp_dir(); !res) {
        GM_LOG_WARN("app", "%s", res.error_message().c_str());
    }

    gm::WindowConfig win_cfg{};
    win_cfg.title = "gmrunner";
    win_cfg.width = opts.window_width;
    win_cfg.height = opts.window_height;
    win_cfg.resizable = true;
    win_cfg.vsync = true;

    if (auto res = g_web_app->window.create(win_cfg); !res) {
        GM_LOG_FATAL("app", "%s", res.error_message().c_str());
        return 1;
    }
    if (auto res = g_web_app->gfx.init(&gm::PlatformWindow::gl_get_proc_address); !res) {
        GM_LOG_FATAL("app", "%s", res.error_message().c_str());
        return 1;
    }

    if (!opts.game_path.empty()) {
        const auto p = gm::Vfs::path_from_utf8(opts.game_path);
        const std::string key = gm::Vfs::path_to_utf8(p.stem());
        (void)gmrunner_web_launch("memfs", opts.game_path.c_str(), key.c_str());
    } else {
        gm::web_report_status("Ready — select a game folder, .zip archive, or .exe");
    }

    emscripten_exit_with_live_runtime();
    return 0;
#else
    AppState app;

    // Headless CLI modes (--inspect / --compile-check) do not require SDL video init.
    const bool headless_cli = opts.inspect_only || opts.compile_check_only;
    if (!headless_cli) {
        if (auto init_res = gm::Platform::init(); !init_res) {
            GM_LOG_FATAL("app", "%s", init_res.error_message().c_str());
            return 1;
        }
    }

    auto mount_res = mount_native_game_target(app.vfs, opts.game_path, opts.save_dir, !headless_cli);
    if (!mount_res) {
        GM_LOG_FATAL("app", "%s", mount_res.error_message().c_str());
        if (!headless_cli) gm::Platform::shutdown();
        return 1;
    }
    const std::string exe_lookup_path = mount_res.value().exe_lookup_path;
    const std::string game_key = mount_res.value().game_key;

    // Load game executable if provided.
    std::optional<gm::GameData> loaded_game;
    if (!exe_lookup_path.empty()) {
        gm::LoadOptions load_opts{};
        load_opts.version_override = opts.version_override;
        load_opts.codepage_override = opts.codepage;

        const gm::u64 t0 = gm::Clock::now_us();
        auto game_res = gm::ExeLoader::load_from_vfs(app.vfs, exe_lookup_path, load_opts);
        const gm::f64 elapsed_ms = static_cast<gm::f64>(gm::Clock::now_us() - t0) * 1e-3;
        if (!game_res) {
            GM_LOG_FATAL("loader", "Failed to load '%s': %s",
                         opts.game_path.c_str(), game_res.error_message().c_str());
            if (!headless_cli) gm::Platform::shutdown();
            return 1;
        }
        loaded_game = std::move(game_res.value());
        GM_LOG_INFO("loader", "Loaded '%s' (GM %.*s) in %.1f ms",
                    opts.game_path.c_str(),
                    static_cast<int>(gm::version_mode_name(loaded_game->meta.detected_version).size()),
                    gm::version_mode_name(loaded_game->meta.detected_version).data(),
                    elapsed_ms);
    }

    if (opts.inspect_only) {
        if (!loaded_game) {
            GM_LOG_ERROR("app", "--inspect requires a game executable, directory, or .zip archive");
            return 1;
        }
        gm::print_game_inspect(*loaded_game, opts.verbose);
        return 0;
    }

    if (opts.compile_check_only) {
        if (!loaded_game) {
            GM_LOG_ERROR("app", "--compile-check requires a game executable, directory, or .zip archive");
            return 1;
        }
        const gm::u64 tc0 = gm::Clock::now_us();
        auto prog = gm::GmlCompiler::compile_game(*loaded_game);
        const gm::f64 compile_ms = static_cast<gm::f64>(gm::Clock::now_us() - tc0) * 1e-3;
        GM_LOG_INFO("gml", "Compiled '%s' in %.1f ms", opts.game_path.c_str(), compile_ms);
        gm::print_compile_summary(prog, opts.verbose);
        return prog.stats.total_errors == 0 ? 0 : 1;
    }

    std::optional<gm::CompiledProgram> compiled_prog;
    if (loaded_game) {
        const gm::u64 tc0 = gm::Clock::now_us();
        compiled_prog = gm::GmlCompiler::compile_game(*loaded_game);
        const gm::f64 compile_ms = static_cast<gm::f64>(gm::Clock::now_us() - tc0) * 1e-3;
        GM_LOG_INFO("gml", "Compiled %zu instructions (%zu errors) in %.1f ms",
                    compiled_prog->stats.total_instructions,
                    compiled_prog->stats.total_errors,
                    compile_ms);
        if (compiled_prog->stats.total_errors > 0) {
            gm::print_compile_summary(*compiled_prog, opts.verbose);
        }
    }

    if (auto res = app.vfs.init_temp_dir(); !res) {
        GM_LOG_WARN("app", "%s", res.error_message().c_str());
    }

    gm::WindowConfig win_cfg{};
    // GM sets the game window caption after the first Draw.
    win_cfg.title = loaded_game ? "" : "gmrunner";
    win_cfg.width = opts.window_width;
    win_cfg.height = opts.window_height;
    win_cfg.fullscreen = opts.fullscreen;
    win_cfg.vsync = opts.vsync;
#if defined(__ANDROID__)
    win_cfg.fullscreen = true;
#endif
    if (loaded_game) {
        win_cfg.fullscreen = win_cfg.fullscreen || loaded_game->options.fullscreen;
        win_cfg.borderless = loaded_game->options.no_border;
        win_cfg.show_cursor = loaded_game->options.show_cursor;
        win_cfg.resizable = true;
        win_cfg.vsync = opts.vsync || (loaded_game->options.sync_vertex != 0);
        compute_initial_window_size(*loaded_game, win_cfg.width, win_cfg.height);
    }

    if (auto res = app.window.create(win_cfg); !res) {
        GM_LOG_FATAL("app", "%s", res.error_message().c_str());
        gm::Platform::shutdown();
        return 1;
    }

    if (auto res = app.gfx.init(&gm::PlatformWindow::gl_get_proc_address); !res) {
        GM_LOG_FATAL("app", "%s", res.error_message().c_str());
        app.window.destroy();
        gm::Platform::shutdown();
        return 1;
    }

    if (loaded_game && compiled_prog) {
        std::vector<std::string> cmd_args;
        cmd_args.reserve(static_cast<gm::usize>(argc));
        for (int i = 0; i < argc; ++i) {
            cmd_args.emplace_back(argv[i] ? argv[i] : "");
        }
        app.runner = std::make_unique<gm::Runner>(app.vfs, app.window, app.gfx);
        if (auto res = app.runner->init(std::move(*loaded_game), std::move(*compiled_prog), std::move(cmd_args)); !res) {
            GM_LOG_FATAL("runtime", "Runner initialization failed: %s", res.error_message().c_str());
            app.runner.reset();
            app.gfx.shutdown();
            app.window.destroy();
            app.vfs.cleanup_temp_dir();
            gm::Platform::shutdown();
            return 1;
        }
        const std::filesystem::path layout_path = !opts.touch_layout_path.empty()
            ? gm::Vfs::path_from_utf8(opts.touch_layout_path)
            : default_touch_layout_path(game_key);
        if (!layout_path.empty()) {
            app.runner->set_touch_layout_path(layout_path);
        }
        if (opts.touch_overlay) {
            app.runner->set_touch_overlay_enabled(true);
        }
        GM_LOG_INFO("app", "Game started (room_speed=%d FPS)", app.runner->room_speed());
    } else {
        GM_LOG_INFO("app", "No game executable specified. Press ESC to quit or F11 to toggle fullscreen.");
    }

    app.start_us = gm::Clock::now_us();

    gm::u64 next_frame_us = gm::Clock::now_us();
    while (app.running) {
        tick_app_frame(app);
        if (!app.running) {
            break;
        }
        if (app.runner && app.runner->is_paused_in_background()) {
            gm::Clock::yield_ms(50);
            next_frame_us = gm::Clock::now_us();
            continue;
        }
        const gm::u64 step_us = app.runner ? app.runner->target_frame_us() : app.target_frame_us;
        next_frame_us += step_us;
        const gm::u64 now = gm::Clock::now_us();
        if (next_frame_us < now) {
            next_frame_us = now;
        }
        gm::Clock::sleep_until_us(next_frame_us);
    }

    if (app.runner) {
        app.runner->shutdown();
        app.runner.reset();
    }
    app.gfx.shutdown();
    app.window.destroy();
    app.vfs.cleanup_temp_dir();
    gm::Platform::shutdown();
    return 0;
#endif
}
