#include "platform/font_loader.h"

#include "base/encoding.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gm {

namespace {

#if !defined(__EMSCRIPTEN__)

u16 read_be16(const u8* p) noexcept {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8u) | static_cast<u16>(p[1]));
}

u32 read_be32(const u8* p) noexcept {
    return (static_cast<u32>(p[0]) << 24u) |
           (static_cast<u32>(p[1]) << 16u) |
           (static_cast<u32>(p[2]) << 8u) |
           static_cast<u32>(p[3]);
}

// Inspects SFNT `head` / `OS/2` tables to determine whether the actual font
// outline is natively bold or italic (Win32 GDI TEXTMETRIC can report simulated
// bold/italic even when GetFontData returns a Regular font file).
void inspect_sfnt_style(const std::vector<u8>& bytes, i32 face_index, bool& out_bold, bool& out_italic) noexcept {
    out_bold = false;
    out_italic = false;
    if (bytes.size() < 12) return;

    usize sfnt_off = 0;
    if (std::memcmp(bytes.data(), "ttcf", 4) == 0) {
        if (bytes.size() < 16) return;
        const u32 num_fonts = read_be32(bytes.data() + 8);
        const u32 idx = static_cast<u32>(std::max(0, face_index));
        if (idx >= num_fonts || 12u + static_cast<usize>(idx + 1u) * 4u > bytes.size()) return;
        sfnt_off = read_be32(bytes.data() + 12u + static_cast<usize>(idx) * 4u);
    }

    if (sfnt_off + 12u > bytes.size()) return;
    const u16 num_tables = read_be16(bytes.data() + sfnt_off + 4u);
    const usize dir_off = sfnt_off + 12u;
    if (dir_off + static_cast<usize>(num_tables) * 16u > bytes.size()) return;

    for (u16 i = 0; i < num_tables; ++i) {
        const u8* rec = bytes.data() + dir_off + static_cast<usize>(i) * 16u;
        const u32 tab_off = read_be32(rec + 8u);
        const u32 tab_len = read_be32(rec + 12u);
        if (static_cast<usize>(tab_off) + static_cast<usize>(tab_len) > bytes.size()) continue;
        if (std::memcmp(rec, "head", 4) == 0 && tab_len >= 46u) {
            const u16 mac_style = read_be16(bytes.data() + tab_off + 44u);
            if ((mac_style & 0x0001u) != 0) out_bold = true;
            if ((mac_style & 0x0002u) != 0) out_italic = true;
        } else if (std::memcmp(rec, "OS/2", 4) == 0 && tab_len >= 64u) {
            const u16 weight = read_be16(bytes.data() + tab_off + 4u);
            const u16 fs_sel = read_be16(bytes.data() + tab_off + 62u);
            if (weight >= 600u || (fs_sel & 0x0020u) != 0) out_bold = true;
            if ((fs_sel & 0x0001u) != 0) out_italic = true;
        }
    }
}

std::string normalize_font_key(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
bool read_file_bytes(const std::filesystem::path& path, std::vector<u8>& out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0 || sz > 64 * 1024 * 1024) return false;
    out.resize(static_cast<usize>(sz));
    ifs.seekg(0, std::ios::beg);
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(out.data()), sz));
}
#endif

#if defined(_WIN32)

bool try_load_win32_gdi_font(std::wstring_view face, bool bold, bool italic, u8 charset, SystemFontBlob& out) {
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;

    LOGFONTW lf{};
    lf.lfHeight = -16;
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = italic ? TRUE : FALSE;
    lf.lfCharSet = charset;
    lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = DEFAULT_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    const usize copy_len = std::min<usize>(face.size(), LF_FACESIZE - 1);
    std::wmemcpy(lf.lfFaceName, face.data(), copy_len);
    lf.lfFaceName[copy_len] = L'\0';

    HFONT font = CreateFontIndirectW(&lf);
    if (!font) {
        DeleteDC(hdc);
        return false;
    }

    HGDIOBJ old_obj = SelectObject(hdc, font);
    bool ok = false;

    // For TrueType Collections (.ttc, e.g. Microsoft YaHei / SimSun / Yu Gothic),
    // dwTable=0 returns a sliced stream whose internal table offsets remain relative
    // to byte 0 of the .ttc file. Query the full 'ttcf' collection stream first!
    constexpr DWORD kTtcfTag = 0x66637474u; // 'ttcf' in Win32 GetFontData byte order
    const DWORD ttc_size = GetFontData(hdc, kTtcfTag, 0, nullptr, 0);
    if (ttc_size != GDI_ERROR && ttc_size > 16 && ttc_size <= 64u * 1024u * 1024u) {
        out.data.resize(ttc_size);
        if (GetFontData(hdc, kTtcfTag, 0, out.data.data(), ttc_size) == ttc_size) {
            out.face_index = 0;
            // Identify which face inside the TTC matches the selected HFONT by comparing
            // the first 28 bytes (SFNT header + first table record) against dwTable=0.
            u8 sub_hdr[28]{};
            if (GetFontData(hdc, 0, 0, sub_hdr, sizeof(sub_hdr)) == sizeof(sub_hdr) &&
                std::memcmp(out.data.data(), "ttcf", 4) == 0) {
                const u32 num_fonts = read_be32(out.data.data() + 8u);
                for (u32 idx = 0; idx < num_fonts && 12u + static_cast<usize>(idx + 1u) * 4u <= out.data.size(); ++idx) {
                    const u32 off = read_be32(out.data.data() + 12u + static_cast<usize>(idx) * 4u);
                    if (static_cast<usize>(off) + sizeof(sub_hdr) <= out.data.size() &&
                        std::memcmp(out.data.data() + off, sub_hdr, sizeof(sub_hdr)) == 0) {
                        out.face_index = static_cast<i32>(idx);
                        break;
                    }
                }
            }
            inspect_sfnt_style(out.data, out.face_index, out.native_bold, out.native_italic);
            ok = true;
        } else {
            out.data.clear();
        }
    } else {
        const DWORD size = GetFontData(hdc, 0, 0, nullptr, 0);
        if (size != GDI_ERROR && size > 12 && size <= 64u * 1024u * 1024u) {
            out.data.resize(size);
            if (GetFontData(hdc, 0, 0, out.data.data(), size) == size) {
                out.face_index = 0;
                inspect_sfnt_style(out.data, 0, out.native_bold, out.native_italic);
                ok = true;
            } else {
                out.data.clear();
            }
        }
    }

    SelectObject(hdc, old_obj);
    DeleteObject(font);
    DeleteDC(hdc);
    return ok;
}

std::wstring utf8_to_wstring(std::string_view utf8) {
    const std::u16string u16 = utf8_to_utf16(utf8);
    return std::wstring(u16.begin(), u16.end());
}

#elif !defined(__EMSCRIPTEN__)

struct IndexedFontFile {
    std::filesystem::path path;
    std::string stem_norm;
    bool is_bold = false;
    bool is_italic = false;
};

const std::vector<IndexedFontFile>& indexed_system_fonts() {
    static const std::vector<IndexedFontFile> files = []() {
        std::vector<IndexedFontFile> list;
        std::vector<std::filesystem::path> roots;
#if defined(__APPLE__)
        roots = {
            "/System/Library/Fonts",
            "/System/Library/Fonts/Supplemental",
            "/Library/Fonts",
        };
        if (const char* home = std::getenv("HOME")) {
            roots.push_back(std::filesystem::path(home) / "Library" / "Fonts");
        }
#elif defined(__ANDROID__)
        roots = {"/system/fonts", "/product/fonts", "/vendor/fonts"};
#else
        roots = {
            "/usr/share/fonts",
            "/usr/local/share/fonts",
        };
        if (const char* home = std::getenv("HOME")) {
            roots.push_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
            roots.push_back(std::filesystem::path(home) / ".fonts");
        }
#endif
        std::error_code ec;
        for (const auto& root : roots) {
            if (!std::filesystem::exists(root, ec) || ec) continue;
            auto it = std::filesystem::recursive_directory_iterator(
                root, std::filesystem::directory_options::skip_permission_denied, ec);
            for (const auto& entry : it) {
                if (ec) { ec.clear(); continue; }
                if (!entry.is_regular_file(ec) || ec) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
                const std::string stem = normalize_font_key(entry.path().stem().string());
                IndexedFontFile f{};
                f.path = entry.path();
                f.stem_norm = stem;
                f.is_bold = stem.find("bold") != std::string::npos ||
                            stem.ends_with("bd") || stem.ends_with("bi");
                f.is_italic = stem.find("italic") != std::string::npos ||
                              stem.find("oblique") != std::string::npos ||
                              stem.ends_with("bi");
                list.push_back(std::move(f));
            }
        }
        return list;
    }();
    return files;
}

bool load_best_posix_match(const std::vector<std::string>& candidate_keys, bool bold, bool italic, SystemFontBlob& out) {
    const auto& files = indexed_system_fonts();
    if (files.empty()) return false;

    const IndexedFontFile* best = nullptr;
    i32 best_score = -10000;
    for (usize ci = 0; ci < candidate_keys.size(); ++ci) {
        const std::string& key = candidate_keys[ci];
        if (key.empty()) continue;
        const i32 base_priority = static_cast<i32>((candidate_keys.size() - ci) * 100);
        for (const auto& f : files) {
            i32 match_bonus = -1;
            if (f.stem_norm == key) {
                match_bonus = 60;
            } else if (f.stem_norm.starts_with(key)) {
                match_bonus = 40;
            } else if (f.stem_norm.find(key) != std::string::npos) {
                match_bonus = 20;
            }
            if (match_bonus < 0) continue;

            // Avoid condensed / narrow / mono / symbol variants when looking for standard sans.
            i32 penalty = 0;
            if (key.find("condensed") == std::string::npos && f.stem_norm.find("condensed") != std::string::npos) penalty -= 15;
            if (key.find("narrow") == std::string::npos && f.stem_norm.find("narrow") != std::string::npos) penalty -= 15;
            if (key.find("black") == std::string::npos && f.stem_norm.find("black") != std::string::npos) penalty -= 12;
            if (key.find("light") == std::string::npos && f.stem_norm.find("light") != std::string::npos) penalty -= 10;
            if (key.find("thin") == std::string::npos && f.stem_norm.find("thin") != std::string::npos) penalty -= 12;

            i32 style_bonus = 0;
            style_bonus += (f.is_bold == bold) ? 25 : -10;
            style_bonus += (f.is_italic == italic) ? 20 : -8;

            const i32 score = base_priority + match_bonus + style_bonus + penalty;
            if (score > best_score) {
                best_score = score;
                best = &f;
            }
        }
    }

    if (!best) return false;
    if (!read_file_bytes(best->path, out.data)) return false;
    out.face_index = 0;
    inspect_sfnt_style(out.data, out.face_index, out.native_bold, out.native_italic);
    return true;
}

#endif // !_WIN32

#endif // !__EMSCRIPTEN__

} // namespace

bool load_system_font(
    std::string_view family_name,
    bool bold,
    bool italic,
    u8 charset,
    SystemFontBlob& out) {
#if defined(__EMSCRIPTEN__)
    (void)family_name;
    (void)bold;
    (void)italic;
    (void)charset;
    (void)out;
    return false;
#elif defined(_WIN32)
    const std::string norm = normalize_font_key(family_name);
    std::vector<std::wstring> candidates;

    if (norm.empty() || norm == "mssansserif" || norm == "system" || norm == "default") {
        // Original GM8 uses 8pt MS Sans Serif (a .fon raster font). Tahoma has both
        // Regular and Bold TTF faces with matching classic Windows UI metrics, while
        // Microsoft Sans Serif matches the regular outline closely.
        if (bold && !italic) {
            candidates = {L"Tahoma", L"Segoe UI", L"Arial", L"Microsoft Sans Serif"};
        } else if (!bold && !italic) {
            candidates = {L"Tahoma", L"Microsoft Sans Serif", L"Segoe UI", L"Arial"};
        } else {
            candidates = {L"Segoe UI", L"Arial", L"Tahoma", L"Microsoft Sans Serif"};
        }
    } else {
        candidates.push_back(utf8_to_wstring(family_name));
        candidates.push_back(L"Tahoma");
        candidates.push_back(L"Segoe UI");
        candidates.push_back(L"Arial");
    }

    for (const auto& name : candidates) {
        if (try_load_win32_gdi_font(name, bold, italic, charset, out)) {
            return true;
        }
        if (charset != DEFAULT_CHARSET &&
            try_load_win32_gdi_font(name, bold, italic, DEFAULT_CHARSET, out)) {
            return true;
        }
    }
    return false;
#else
    const std::string norm = normalize_font_key(family_name);
    std::vector<std::string> candidates;
    if (!norm.empty() && norm != "mssansserif" && norm != "microsoftsansserif" && norm != "system") {
        candidates.push_back(norm);
    }

    if (norm.find("courier") != std::string::npos || norm.find("consolas") != std::string::npos ||
        norm.find("mono") != std::string::npos) {
        candidates.insert(candidates.end(), {
            "couriernew", "menlo", "liberationmono", "dejavusansmono",
            "notosansmono", "droidsansmono", "freemono"
        });
    } else if (norm.find("times") != std::string::npos || norm.find("georgia") != std::string::npos ||
               (norm.find("serif") != std::string::npos && norm.find("sans") == std::string::npos)) {
        candidates.insert(candidates.end(), {
            "timesnewroman", "times", "liberationserif", "dejavuserif",
            "notoserif", "freeserif"
        });
    } else {
        candidates.insert(candidates.end(), {
            "tahoma", "arial", "helvetica", "sfns", "roboto",
            "liberationsans", "notosans", "dejavusans", "ubuntu", "freesans"
        });
    }

    // Prioritize CJK primary font when a CJK GDI charset is explicitly requested.
    if (charset == 134 || charset == 136 || charset == 128 || charset == 129 || charset == 130) {
        candidates.insert(candidates.end(), {
            "notosanscjk", "pingfang", "hiraginosans", "wenquanyi", "droidsansfallback"
        });
    }

    return load_best_posix_match(candidates, bold, italic, out);
#endif
}

std::vector<SystemFontBlob> load_system_fallback_fonts(u8 charset) {
    std::vector<SystemFontBlob> result;
#if defined(__EMSCRIPTEN__)
    (void)charset;
    return result;
#elif defined(_WIN32)
    std::vector<std::wstring> faces;
    if (charset == 128) { // SHIFTJIS_CHARSET
        faces = {L"Yu Gothic UI", L"Meiryo", L"MS UI Gothic", L"Microsoft YaHei", L"Malgun Gothic", L"Arial", L"Segoe UI Symbol"};
    } else if (charset == 129 || charset == 130) { // HANGEUL / JOHAB
        faces = {L"Malgun Gothic", L"Microsoft YaHei", L"Yu Gothic UI", L"Arial", L"Segoe UI Symbol"};
    } else if (charset == 136) { // CHINESEBIG5
        faces = {L"Microsoft JhengHei", L"Microsoft YaHei", L"Yu Gothic UI", L"Malgun Gothic", L"Arial", L"Segoe UI Symbol"};
    } else {
        faces = {L"Microsoft YaHei", L"SimSun", L"Yu Gothic UI", L"Malgun Gothic", L"Segoe UI", L"Arial", L"Segoe UI Symbol"};
    }

    std::unordered_set<usize> seen_sizes;
    for (const auto& f : faces) {
        SystemFontBlob blob{};
        if (try_load_win32_gdi_font(f, false, false, DEFAULT_CHARSET, blob)) {
            if (seen_sizes.insert(blob.data.size()).second) {
                result.push_back(std::move(blob));
            }
        }
    }
    return result;
#else
    (void)charset;
    static const std::array<const char*, 9> kFallbackKeys = {
        "notosanscjk", "pingfang", "hiraginosans", "wenquanyi",
        "droidsansfallback", "arialunicode", "notosans", "dejavusans", "roboto"
    };
    std::unordered_set<usize> seen_sizes;
    for (const char* key : kFallbackKeys) {
        SystemFontBlob blob{};
        if (load_best_posix_match({key}, false, false, blob)) {
            if (seen_sizes.insert(blob.data.size()).second) {
                result.push_back(std::move(blob));
            }
        }
    }
    return result;
#endif
}

} // namespace gm
