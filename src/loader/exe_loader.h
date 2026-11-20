#pragma once

#include "base/types.h"
#include "loader/game_data.h"
#include "platform/vfs.h"

#include <string_view>
#include <vector>

namespace gm {

struct LoadOptions {
    VersionMode version_override = VersionMode::Auto;
    u32 codepage_override = 0; // 0 = auto-detect from version/fonts
};

class ExeLoader {
public:
    [[nodiscard]] static Result<GameData> load_from_bytes(
        std::vector<u8> exe_bytes,
        const LoadOptions& options = {});

    [[nodiscard]] static Result<GameData> load_from_vfs(
        Vfs& vfs,
        std::string_view exe_path,
        const LoadOptions& options = {});
};

// Prints a structured human-readable summary of a loaded GameData (--inspect).
void print_game_inspect(const GameData& game, bool verbose = false);

} // namespace gm
