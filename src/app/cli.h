#pragma once

#include "base/log.h"
#include "base/types.h"

#include <optional>
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
    bool sharp_scale = false;
    std::optional<bool> vsync_override;
    bool touch_overlay = false;
    std::string touch_layout_path;
    i32 window_width = 800;
    i32 window_height = 608;
    TraceFlags trace{};
};

[[nodiscard]] Result<CliOptions> parse_cli(int argc, char** argv);
void print_cli_help();

} // namespace gm
