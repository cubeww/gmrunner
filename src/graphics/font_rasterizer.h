#pragma once

#include "base/types.h"
#include "loader/game_data.h"

namespace gm {

// Rasterizes glyphs [font.first .. font.last] (clamped to 0..255) from a system
// TrueType/OpenType font (or WebAssembly Canvas 2D / built-in 8x8 fallback) into
// font.glyphs, font.tex_width, font.tex_height, and font.alpha_map, matching GM8
// CFont.Create_0 / sub_57DEDC layout conventions.
bool rasterize_gm_font(FontData& font, u32 default_codepage);

} // namespace gm
