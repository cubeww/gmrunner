#include "app/cli.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace gm {

void print_cli_help() {
    std::printf(
        "gmrunner - Cross-platform GameMaker 8.0 / 8.1 / 8.2 Runner\n"
        "\n"
        "Usage:\n"
        "  gmrunner [options] [game.exe | game.zip | game_dir]\n"
        "\n"
        "Options:\n"
        "  -h, --help             Show this help message\n"
        "  --inspect              Parse game executable and print asset/extension summary\n"
        "  --compile-check        Compile all GML/D&D scripts and exit\n"
        "  --save-dir <path>      Override writable save overlay directory\n"
        "  --version <80|81|82>   Override auto-detected GameMaker version profile\n"
        "  --codepage <id>        Windows codepage for GM 8.0 strings (auto: font charset, fallback 1252)\n"
        "  --width <px>           Initial window width (default: 800)\n"
        "  --height <px>          Initial window height (default: 608)\n"
        "  --fullscreen           Start in fullscreen mode\n"
        "  --sharp-scale          Use Sharp pixel-art scaling for the final game image\n"
        "  --vsync / --no-vsync   Override initial native vertical sync (default: game setting)\n"
        "  --touch-overlay        Enable on-screen virtual touch controller\n"
        "  --touch-layout <path>  Load/save custom virtual touch layout from file\n"
        "  --verbose              Enable debug-level logging\n"
        "  --trace-events         Trace runtime event dispatch\n"
        "  --trace-dll            Trace external DLL / extension shim calls\n"
        "  --trace-vfs            Trace virtual filesystem lookups\n");
}

Result<CliOptions> parse_cli(int argc, char** argv) {
    CliOptions opts{};

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto require_value = [&](std::string_view flag) -> Result<std::string_view> {
            if (i + 1 >= argc) {
                return Error{"Missing argument for " + std::string(flag)};
            }
            return std::string_view(argv[++i]);
        };

        if (arg == "-h" || arg == "--help") {
            opts.show_help = true;
        } else if (arg == "--inspect") {
            opts.inspect_only = true;
        } else if (arg == "--compile-check") {
            opts.compile_check_only = true;
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--sharp-scale") {
            opts.sharp_scale = true;
        } else if (arg == "--vsync") {
            opts.vsync_override = true;
        } else if (arg == "--no-vsync") {
            opts.vsync_override = false;
        } else if (arg == "--touch-overlay") {
            opts.touch_overlay = true;
        } else if (arg == "--touch-layout") {
            auto val = require_value(arg);
            if (!val) return val.error();
            opts.touch_layout_path = std::string(val.value());
        } else if (arg == "--trace-events") {
            opts.trace.events = true;
        } else if (arg == "--trace-dll") {
            opts.trace.dll_calls = true;
        } else if (arg == "--trace-vfs") {
            opts.trace.vfs = true;
        } else if (arg == "--save-dir") {
            auto val = require_value(arg);
            if (!val) return val.error();
            opts.save_dir = std::string(val.value());
        } else if (arg == "--version") {
            auto val = require_value(arg);
            if (!val) return val.error();
            if (val.value() == "80" || val.value() == "8.0") {
                opts.version_override = VersionMode::GM80;
            } else if (val.value() == "81" || val.value() == "8.1") {
                opts.version_override = VersionMode::GM81;
            } else if (val.value() == "82" || val.value() == "8.2") {
                opts.version_override = VersionMode::GM82;
            } else {
                return Error{"Invalid --version value (expected 80, 81, or 82)"};
            }
        } else if (arg == "--codepage") {
            auto val = require_value(arg);
            if (!val) return val.error();
            opts.codepage = static_cast<u32>(std::strtoul(std::string(val.value()).c_str(), nullptr, 10));
        } else if (arg == "--width") {
            auto val = require_value(arg);
            if (!val) return val.error();
            opts.window_width = static_cast<i32>(std::strtol(std::string(val.value()).c_str(), nullptr, 10));
        } else if (arg == "--height") {
            auto val = require_value(arg);
            if (!val) return val.error();
            opts.window_height = static_cast<i32>(std::strtol(std::string(val.value()).c_str(), nullptr, 10));
        } else if (!arg.empty() && arg[0] == '-') {
            return Error{"Unknown option: " + std::string(arg)};
        } else {
            opts.game_path = std::string(arg);
        }
    }

    return opts;
}

} // namespace gm
