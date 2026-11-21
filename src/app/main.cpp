#include "app/cli.h"
#include "base/log.h"
#include "base/math.h"
#include "gml/compiler.h"
#include "graphics/gl_graphics_device.h"
#include "loader/exe_loader.h"
#include "platform/clock.h"
#include "platform/platform.h"
#include "platform/vfs.h"
#include "platform/window.h"
#include "runtime/runner.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

#if defined(_WIN32)
// SDL obtains the wide command line and supplies UTF-8 argv on Windows.
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
    gm::u64 frame_count = 0;
    gm::u64 start_us = 0;
    gm::u64 target_frame_us = 16667; // 60 FPS default for idle mode
};

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
void emscripten_frame_callback(void* userdata) {
    auto* app = static_cast<AppState*>(userdata);
    if (!app->running) {
        emscripten_cancel_main_loop();
        return;
    }
    tick_app_frame(*app);
}
#endif

} // namespace

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

    GM_LOG_INFO("app", "gmrunner Phase 3 (version_override=%s, codepage=%u)",
                std::string(gm::version_mode_name(opts.version_override)).c_str(),
                opts.codepage);

    AppState app;

    // Mount VFS roots (game folder if provided, otherwise current working directory).
    std::filesystem::path game_root = std::filesystem::current_path();
    std::string exe_lookup_path = opts.game_path;
    if (!opts.game_path.empty()) {
        const auto exe_path = gm::Vfs::path_from_utf8(opts.game_path);
        if (exe_path.has_parent_path()) {
            game_root = exe_path.parent_path();
            exe_lookup_path = gm::Vfs::path_to_utf8(exe_path.filename());
        }
        GM_LOG_INFO("app", "Target game: %s", opts.game_path.c_str());
    }

    if (auto res = app.vfs.mount_game_root(game_root); !res) {
        GM_LOG_ERROR("app", "%s", res.error_message().c_str());
    }
    if (auto res = app.vfs.mount_save_root(game_root); !res) {
        GM_LOG_WARN("app", "%s", res.error_message().c_str());
    }

    // Load game executable if provided.
    std::optional<gm::GameData> loaded_game;
    if (!opts.game_path.empty()) {
        gm::LoadOptions load_opts{};
        load_opts.version_override = opts.version_override;
        load_opts.codepage_override = opts.codepage;

        const gm::u64 t0 = gm::Clock::now_us();
        auto game_res = gm::ExeLoader::load_from_vfs(app.vfs, exe_lookup_path, load_opts);
        const gm::f64 elapsed_ms = static_cast<gm::f64>(gm::Clock::now_us() - t0) * 1e-3;
        if (!game_res) {
            GM_LOG_FATAL("loader", "Failed to load '%s': %s",
                         opts.game_path.c_str(), game_res.error_message().c_str());
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
            GM_LOG_ERROR("app", "--inspect requires a game executable path");
            return 1;
        }
        gm::print_game_inspect(*loaded_game, opts.verbose);
        return 0;
    }

    if (opts.compile_check_only) {
        if (!loaded_game) {
            GM_LOG_ERROR("app", "--compile-check requires a game executable path");
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

    if (auto init_res = gm::Platform::init(); !init_res) {
        GM_LOG_FATAL("app", "%s", init_res.error_message().c_str());
        return 1;
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
    if (loaded_game) {
        win_cfg.fullscreen = opts.fullscreen || loaded_game->options.fullscreen;
        win_cfg.borderless = loaded_game->options.no_border;
        win_cfg.show_cursor = loaded_game->options.show_cursor;
        win_cfg.resizable = true;
        win_cfg.vsync = opts.vsync || (loaded_game->options.sync_vertex != 0);

        if (!loaded_game->room_order.empty()) {
            const gm::i32 first_id = loaded_game->room_order[0];
            if (first_id >= 0 && static_cast<gm::usize>(first_id) < loaded_game->rooms.size() &&
                loaded_game->rooms[static_cast<gm::usize>(first_id)].exists) {
                const auto& rm = loaded_game->rooms[static_cast<gm::usize>(first_id)];
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
                win_cfg.width = std::max(1, w);
                win_cfg.height = std::max(1, h);
            }
        }
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
        GM_LOG_INFO("app", "Game started (room_speed=%d FPS)", app.runner->room_speed());
    } else {
        GM_LOG_INFO("app", "No game executable specified. Press ESC to quit or F11 to toggle fullscreen.");
    }

    app.start_us = gm::Clock::now_us();

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop_arg(&emscripten_frame_callback, &app, 0, 1);
#else
    gm::u64 next_frame_us = gm::Clock::now_us();
    while (app.running) {
        tick_app_frame(app);
        if (!app.running) {
            break;
        }
        const gm::u64 step_us = app.runner ? app.runner->target_frame_us() : app.target_frame_us;
        next_frame_us += step_us;
        const gm::u64 now = gm::Clock::now_us();
        if (next_frame_us < now) {
            next_frame_us = now;
        }
        gm::Clock::sleep_until_us(next_frame_us);
    }
#endif

    if (app.runner) {
        app.runner->shutdown();
        app.runner.reset();
    }
    app.gfx.shutdown();
    app.window.destroy();
    app.vfs.cleanup_temp_dir();
    gm::Platform::shutdown();
    return 0;
}
