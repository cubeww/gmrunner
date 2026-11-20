#include "app/cli.h"
#include "base/log.h"
#include "base/math.h"
#include "graphics/gl_graphics_device.h"
#include "platform/clock.h"
#include "platform/platform.h"
#include "platform/vfs.h"
#include "platform/window.h"

#include <cmath>
#include <filesystem>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

namespace {

struct AppState {
    gm::Vfs vfs;
    gm::PlatformWindow window;
    gm::GlGraphicsDevice gfx;
    bool running = true;
    gm::u64 frame_count = 0;
    gm::u64 start_us = 0;
    gm::u64 target_frame_us = 16667; // 60 FPS default
};

void tick_frame(AppState& app) {
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

#if defined(__EMSCRIPTEN__)
void emscripten_frame_callback(void* userdata) {
    auto* app = static_cast<AppState*>(userdata);
    if (!app->running) {
        emscripten_cancel_main_loop();
        return;
    }
    tick_frame(*app);
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

    GM_LOG_INFO("app", "gmrunner Phase 0 (version_override=%s, codepage=%u)",
                std::string(gm::version_mode_name(opts.version_override)).c_str(),
                opts.codepage);

    if (auto init_res = gm::Platform::init(); !init_res) {
        GM_LOG_FATAL("app", "%s", init_res.error_message().c_str());
        return 1;
    }

    AppState app;

    // Mount VFS roots (game folder if provided, otherwise current working directory).
    std::filesystem::path game_root = std::filesystem::current_path();
    if (!opts.game_path.empty()) {
        const auto exe_path = gm::Vfs::path_from_utf8(opts.game_path);
        if (exe_path.has_parent_path()) {
            game_root = exe_path.parent_path();
        }
        GM_LOG_INFO("app", "Target game: %s", opts.game_path.c_str());
    }

    if (auto res = app.vfs.mount_game_root(game_root); !res) {
        GM_LOG_ERROR("app", "%s", res.error_message().c_str());
    }
    if (auto res = app.vfs.init_temp_dir(); !res) {
        GM_LOG_WARN("app", "%s", res.error_message().c_str());
    }

    if (opts.inspect_only || opts.compile_check_only) {
        GM_LOG_WARN("app", "--inspect and --compile-check will be wired in Phases 1 and 2.");
        gm::Platform::shutdown();
        return 0;
    }

    gm::WindowConfig win_cfg{};
    win_cfg.title = "gmrunner — Phase 0 Platform & OpenGL Skeleton";
    win_cfg.width = opts.window_width;
    win_cfg.height = opts.window_height;
    win_cfg.fullscreen = opts.fullscreen;
    win_cfg.vsync = opts.vsync;

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

    GM_LOG_INFO("app", "Ready. Press ESC to quit or F11 to toggle fullscreen.");
    app.start_us = gm::Clock::now_us();

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop_arg(&emscripten_frame_callback, &app, 0, 1);
#else
    gm::u64 next_frame_us = gm::Clock::now_us();
    while (app.running) {
        tick_frame(app);
        if (!app.running) {
            break;
        }
        next_frame_us += app.target_frame_us;
        const gm::u64 now = gm::Clock::now_us();
        if (next_frame_us < now) {
            next_frame_us = now;
        }
        gm::Clock::sleep_until_us(next_frame_us);
    }
#endif

    app.gfx.shutdown();
    app.window.destroy();
    app.vfs.cleanup_temp_dir();
    gm::Platform::shutdown();
    return 0;
}
