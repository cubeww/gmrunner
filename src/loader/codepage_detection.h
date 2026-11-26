#pragma once

#include "loader/game_data.h"

namespace gm {

[[nodiscard]] GameEncoding select_game_encoding(const GameData& game, u32 override_cp);

} // namespace gm
