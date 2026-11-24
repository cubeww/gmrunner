#pragma once

#include "base/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace gm {

struct RtfRun {
    std::string text_utf8;
    std::string font_name = "Arial";
    i32 font_size_pt = 10;
    u32 color_bgr = 0x000000u; // GM8 BGR
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikeout = false;
    u8 charset = 1; // DEFAULT_CHARSET
};

struct RtfParagraph {
    i32 align = 0; // 0 = left, 1 = center, 2 = right
    i32 left_indent_px = 0;
    i32 first_indent_px = 0;
    std::vector<RtfRun> runs;
};

struct RtfDocument {
    std::vector<RtfParagraph> paragraphs;
};

// Parses an RTF document (beginning with "{\rtf") or plain text into styled
// UTF-8 paragraphs suitable for rendering in Game Info (show_info) and text
// splash screens (splash_show_text).
[[nodiscard]] RtfDocument parse_rtf_or_plain_text(
    std::string_view raw,
    u32 default_codepage,
    u32 default_color_bgr = 0x000000u);

} // namespace gm
