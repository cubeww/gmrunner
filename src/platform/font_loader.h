#pragma once

#include "base/types.h"

#include <string_view>
#include <vector>

namespace gm {

struct SystemFontBlob {
    std::vector<u8> data;
    i32 face_index = 0;
    bool native_bold = false;
    bool native_italic = false;
};

// Loads raw TrueType / OpenType (.ttf / .ttc / .otf) bytes for a requested
// GM8 font family, style, and GDI charset on native platforms (Windows, macOS,
// Linux, Android). Returns false on WebAssembly (which uses HTML5 Canvas 2D
// glyph rasterization) or when no matching font file is installed.
[[nodiscard]] bool load_system_font(
    std::string_view family_name,
    bool bold,
    bool italic,
    u8 charset,
    SystemFontBlob& out);

// Loads ordered fallback font files (CJK / Unicode / Sans) used when a glyph is
// missing from the primary font face.
[[nodiscard]] std::vector<SystemFontBlob> load_system_fallback_fonts(u8 charset);

} // namespace gm
