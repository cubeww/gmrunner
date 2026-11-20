#pragma once

#include "loader/game_data.h"

#include <string>

namespace gm {

// Lowers a GM8 D&D EventData (sequence of ActionData) into equivalent GML source code.
// Preserves action_set_relative() stack, UseApplyTo (with), IsQuestion/IsNot/ACT_ELSE,
// ACT_BEGIN/ACT_END, ACT_REPEAT, ACT_EXIT, ACT_VARIABLE, ACT_CODE (with unclosed comment guard),
// and action_execute_script.
[[nodiscard]] std::string lower_dnd_event_to_gml(const EventData& event, const GameData& game);

} // namespace gm
