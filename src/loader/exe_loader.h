#pragma once

#include "base/types.h"
#include "loader/game_data.h"
#include "platform/vfs.h"

#include <span>
#include <string_view>
#include <vector>

namespace gm {

struct LoadOptions {
    VersionMode version_override = VersionMode::Auto;
    u32 codepage_override = 0; // 0 = auto-detect from version/fonts
};

struct SpriteMaskSettings {
    i32 alpha_tolerance;
    i32 bbox_mode;
};

// An external GM8 sprite stores mask settings instead of precomputed masks.
// Once the compressed stream opens, even an unsupported version clears the old sprite.
[[nodiscard]] Result<SpriteMaskSettings> read_gmspr(std::span<const u8> bytes, SpriteData& sprite);

// External GM8 background (.gmbck) container loader (gm81.exe.c:CBackground.LoadFromBackgroundFile).
// Once the compressed stream opens, even an unsupported version clears the old background.
[[nodiscard]] Result<void> read_gmbck(std::span<const u8> bytes, BackgroundData& background);

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
