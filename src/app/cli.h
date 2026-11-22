#pragma once

#include "base/log.h"
#include "base/types.h"

#include <string>

namespace gm {

struct CliOptions {
    std::string game_path;
    std::string save_dir;
    VersionMode version_override = VersionMode::Auto;
    u32 codepage = 0; // 0 = auto-detect from version/fonts
    bool inspect_only = false;
    bool compile_check_only = false;
    bool show_help = false;
    bool verbose = false;
    bool fullscreen = false;
    bool vsync = true;
    bool touch_overlay = false;
    i32 window_width = 800;
    i32 window_height = 608;
    TraceFlags trace{};
};

[[nodiscard]] Result<CliOptions> parse_cli(int argc, char** argv);
void print_cli_help();

} // namespace gm
