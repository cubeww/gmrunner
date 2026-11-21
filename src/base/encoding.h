#pragma once

#include "base/types.h"

#include <functional>
#include <string>
#include <string_view>

namespace gm {

// Maps Windows GDI FONT CHARSET ID (stored in GMFont (First >> 16) & 0xFF)
// to its corresponding Windows codepage number.
[[nodiscard]] u32 gdi_charset_to_codepage(u8 charset, u32 default_codepage = 1252) noexcept;

// UTF-8 <-> UTF-16 & codepoint utilities.
[[nodiscard]] usize utf8_char_count(std::string_view utf8) noexcept;
[[nodiscard]] u32 utf8_decode_next(std::string_view utf8, usize& offset) noexcept;
void utf8_append_codepoint(std::string& out, u32 cp);

[[nodiscard]] std::u16string utf8_to_utf16(std::string_view utf8);
[[nodiscard]] std::string utf16_to_utf8(std::u16string_view utf16);

// Transcodes raw bytes in a Windows codepage (e.g., 1252, 936, 932, 1251) to UTF-8.
// Built-in pure C++ tables cover CP1252, CP1251, CP1250, and UTF-8 (65001).
// Platform layer may register a native multi-byte fallback (e.g., for CJK CP936/CP932).
[[nodiscard]] std::string codepage_to_utf8(std::string_view bytes, u32 codepage);
[[nodiscard]] std::string utf8_to_codepage(std::string_view utf8, u32 codepage);

[[nodiscard]] u32 system_ansi_codepage() noexcept;

// Decode one byte as GM8.1 chr() does. Codepage 0 uses the system ANSI
// codepage on Windows and Windows-1252 on other platforms.
[[nodiscard]] std::string codepage_byte_to_utf8(u8 byte, u32 codepage);

using CodepageToUtf8Hook = std::function<bool(std::string_view, u32, std::string&)>;
using Utf8ToCodepageHook = std::function<bool(std::string_view, u32, std::string&)>;

void register_platform_codepage_hooks(CodepageToUtf8Hook to_utf8, Utf8ToCodepageHook from_utf8);

[[nodiscard]] std::string ascii_to_lower(std::string_view str);

} // namespace gm
