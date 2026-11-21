#pragma once

#include "gml/lexer.h"
#include "loader/game_data.h"

#include <string>

namespace gm {

// Formats a D&D event as a single GML listing for compiler diagnostics.
[[nodiscard]] std::string lower_dnd_event_to_gml(const EventData& event, const GameData& game);

// Each Execute Code card in a compound event is a separate GML execution frame.
[[nodiscard]] Result<LexResult> lex_dnd_event(const EventData& event, const GameData& game);

} // namespace gm
