#include "base/encoding.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "base/single_byte_codepages.h"
#endif

#include <array>

namespace gm {

namespace {

// Windows-1252 high bytes 0x80..0xFF -> Unicode codepoints.
constexpr std::array<u16, 128> kCp1252Table = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

// Windows-1251 (Cyrillic) high bytes 0x80..0xFF -> Unicode codepoints.
constexpr std::array<u16, 128> kCp1251Table = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

// Windows-1250 (Central European) high bytes 0x80..0xFF -> Unicode codepoints.
constexpr std::array<u16, 128> kCp1250Table = {
    0x20AC, 0x0081, 0x201A, 0x0083, 0x201E, 0x2026, 0x2020, 0x2021,
    0x0088, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0098, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
    0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
    0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};

CodepageToUtf8Hook g_to_utf8_hook;
Utf8ToCodepageHook g_from_utf8_hook;

const std::array<u16, 128>* find_single_byte_table(u32 codepage) noexcept {
    switch (codepage) {
        case 1252: return &kCp1252Table;
        case 1251: return &kCp1251Table;
        case 1250: return &kCp1250Table;
        default:   return nullptr;
    }
}

} // namespace

u32 gdi_charset_to_codepage(u8 charset, u32 default_codepage) noexcept {
    switch (charset) {
        case 0:   return 1252; // ANSI_CHARSET
        case 128: return 932;  // SHIFTJIS_CHARSET
        case 129: return 949;  // HANGEUL_CHARSET
        case 130: return 1361; // JOHAB_CHARSET
        case 134: return 936;  // GB2312_CHARSET
        case 136: return 950;  // CHINESEBIG5_CHARSET
        case 161: return 1253; // GREEK_CHARSET
        case 162: return 1254; // TURKISH_CHARSET
        case 163: return 1258; // VIETNAMESE_CHARSET
        case 177: return 1255; // HEBREW_CHARSET
        case 178: return 1256; // ARABIC_CHARSET
        case 186: return 1257; // BALTIC_CHARSET
        case 204: return 1251; // RUSSIAN_CHARSET
        case 222: return 874;  // THAI_CHARSET
        case 238: return 1250; // EASTEUROPE_CHARSET
        default:  return default_codepage;
    }
}

u32 utf8_decode_next(std::string_view utf8, usize& offset) noexcept {
    if (offset >= utf8.size()) {
        return 0;
    }
    const u8 b0 = static_cast<u8>(utf8[offset]);
    if (b0 < 0x80u) {
        ++offset;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && offset + 1 < utf8.size()) {
        const u8 b1 = static_cast<u8>(utf8[offset + 1]);
        if ((b1 & 0xC0u) == 0x80u) {
            offset += 2;
            return ((static_cast<u32>(b0 & 0x1Fu) << 6u) | static_cast<u32>(b1 & 0x3Fu));
        }
    } else if ((b0 & 0xF0u) == 0xE0u && offset + 2 < utf8.size()) {
        const u8 b1 = static_cast<u8>(utf8[offset + 1]);
        const u8 b2 = static_cast<u8>(utf8[offset + 2]);
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
            offset += 3;
            return ((static_cast<u32>(b0 & 0x0Fu) << 12u) |
                    (static_cast<u32>(b1 & 0x3Fu) << 6u) |
                    static_cast<u32>(b2 & 0x3Fu));
        }
    } else if ((b0 & 0xF8u) == 0xF0u && offset + 3 < utf8.size()) {
        const u8 b1 = static_cast<u8>(utf8[offset + 1]);
        const u8 b2 = static_cast<u8>(utf8[offset + 2]);
        const u8 b3 = static_cast<u8>(utf8[offset + 3]);
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
            offset += 4;
            return ((static_cast<u32>(b0 & 0x07u) << 18u) |
                    (static_cast<u32>(b1 & 0x3Fu) << 12u) |
                    (static_cast<u32>(b2 & 0x3Fu) << 6u) |
                    static_cast<u32>(b3 & 0x3Fu));
        }
    }
    ++offset;
    return 0xFFFDu;
}

void utf8_append_codepoint(std::string& out, u32 cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.append("\xEF\xBF\xBD");
    }
}

usize utf8_char_count(std::string_view utf8) noexcept {
    usize count = 0;
    usize offset = 0;
    while (offset < utf8.size()) {
        (void)utf8_decode_next(utf8, offset);
        ++count;
    }
    return count;
}

std::u16string utf8_to_utf16(std::string_view utf8) {
    std::u16string out;
    out.reserve(utf8.size());
    usize offset = 0;
    while (offset < utf8.size()) {
        const u32 cp = utf8_decode_next(utf8, offset);
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            const u32 v = cp - 0x10000u;
            out.push_back(static_cast<char16_t>(0xD800u | (v >> 10u)));
            out.push_back(static_cast<char16_t>(0xDC00u | (v & 0x3FFu)));
        }
    }
    return out;
}

std::string utf16_to_utf8(std::u16string_view utf16) {
    std::string out;
    out.reserve(utf16.size());
    for (usize i = 0; i < utf16.size(); ++i) {
        u32 cp = static_cast<u16>(utf16[i]);
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < utf16.size()) {
            const u32 lo = static_cast<u16>(utf16[i + 1]);
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10u) | (lo - 0xDC00u));
                ++i;
            }
        }
        // A UTF-16 slice may contain just one half of a surrogate pair.
        utf8_append_codepoint(out, cp >= 0xD800u && cp <= 0xDFFFu ? 0xFFFDu : cp);
    }
    return out;
}

void register_platform_codepage_hooks(CodepageToUtf8Hook to_utf8, Utf8ToCodepageHook from_utf8) {
    g_to_utf8_hook = std::move(to_utf8);
    g_from_utf8_hook = std::move(from_utf8);
}

u32 system_ansi_codepage() noexcept {
#ifdef _WIN32
    return GetACP();
#else
    return 1252;
#endif
}

std::string codepage_byte_to_utf8(u8 byte, u32 codepage) {
    const char raw = static_cast<char>(byte);
#ifdef _WIN32
    // CP_ACP is zero. The explicit input length preserves chr(0).
    wchar_t wide = 0;
    if (MultiByteToWideChar(codepage, 0, &raw, 1, &wide, 1) != 1) return {};
    const u32 cp = static_cast<u16>(wide);
#else
    if (byte < 0x80) return std::string(1, raw);
    if (codepage == 0) codepage = system_ansi_codepage();
    const auto* table = find_single_byte_table(codepage);
    switch (codepage) {
        case 874:  table = &detail::kChrCp874; break;
        case 932:  table = &detail::kChrCp932; break;
        case 936:  table = &detail::kChrCp936; break;
        case 949:  table = &detail::kChrCp949; break;
        case 950:  table = &detail::kChrCp950; break;
        case 1361: table = &detail::kChrCp1361; break;
        case 1253: table = &detail::kChrCp1253; break;
        case 1254: table = &detail::kChrCp1254; break;
        case 1255: table = &detail::kChrCp1255; break;
        case 1256: table = &detail::kChrCp1256; break;
        case 1257: table = &detail::kChrCp1257; break;
        case 1258: table = &detail::kChrCp1258; break;
    }
    if (codepage == 65001) return "\xEF\xBF\xBD";
    if (!table) return codepage_to_utf8(std::string_view(&raw, 1), codepage);
    const u32 cp = (*table)[byte - 0x80];
#endif
    std::string out;
    utf8_append_codepoint(out, cp);
    return out;
}

std::string codepage_to_utf8(std::string_view bytes, u32 codepage) {
    if (codepage == 65001u) {
        return std::string(bytes);
    }
    if (const auto* table = find_single_byte_table(codepage)) {
        std::string out;
        out.reserve(bytes.size());
        for (const char ch : bytes) {
            const u8 b = static_cast<u8>(ch);
            if (b < 0x80u) {
                out.push_back(ch);
            } else {
                utf8_append_codepoint(out, (*table)[b - 0x80u]);
            }
        }
        return out;
    }
    if (g_to_utf8_hook) {
        std::string out;
        if (g_to_utf8_hook(bytes, codepage, out)) {
            return out;
        }
    }
    // Fallback: Latin-1 zero-extension
    std::string out;
    out.reserve(bytes.size());
    for (const char ch : bytes) {
        utf8_append_codepoint(out, static_cast<u8>(ch));
    }
    return out;
}

std::string utf8_to_codepage(std::string_view utf8, u32 codepage) {
    if (codepage == 65001u) {
        return std::string(utf8);
    }
    if (const auto* table = find_single_byte_table(codepage)) {
        std::string out;
        out.reserve(utf8.size());
        usize offset = 0;
        while (offset < utf8.size()) {
            const u32 cp = utf8_decode_next(utf8, offset);
            if (cp < 0x80u) {
                out.push_back(static_cast<char>(cp));
            } else {
                char mapped = '?';
                for (usize i = 0; i < table->size(); ++i) {
                    if ((*table)[i] == cp) {
                        mapped = static_cast<char>(0x80u + i);
                        break;
                    }
                }
                out.push_back(mapped);
            }
        }
        return out;
    }
    if (g_from_utf8_hook) {
        std::string out;
        if (g_from_utf8_hook(utf8, codepage, out)) {
            return out;
        }
    }
    return std::string(utf8);
}

std::string ascii_to_lower(std::string_view str) {
    std::string out;
    out.reserve(str.size());
    for (const char ch : str) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

} // namespace gm
