#include "graphics/rtf_document.h"

#include "base/encoding.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace gm {

namespace {

struct RtfFontDef {
    std::string name = "Arial";
    u8 charset = 1;
};

struct RtfState {
    i32 font_idx = 0;
    i32 size_half_pt = 20; // 10pt
    i32 color_idx = 0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikeout = false;
    i32 align = 0;
    i32 left_indent_twips = 0;
    i32 first_indent_twips = 0;
    i32 uc_skip = 1;
    bool skip_group = false;
    bool in_fonttbl = false;
    bool in_colortbl = false;
};

i32 hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool is_ignorable_destination(std::string_view word) noexcept {
    static constexpr std::string_view kIgnored[] = {
        "stylesheet", "info", "pict", "object", "objdata", "result",
        "header", "footer", "headerl", "headerr", "headerf",
        "footerl", "footerr", "footerf", "footnote", "generator",
        "listtable", "listoverridetable", "revtbl", "rsidtbl",
        "themedata", "colorschememapping", "datastore", "latentstyles",
        "mmathPr", "fldinst", "xmlnstbl", "panose", "falt", "bkmkstart",
        "bkmkend", "xe", "tc", "rxe", "datafield", "themedata", "blipuid",
    };
    for (std::string_view ign : kIgnored) {
        if (word == ign) return true;
    }
    return false;
}

std::string trim_font_name(std::string_view s) {
    usize b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    usize e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == ';')) --e;
    return std::string(s.substr(b, e - b));
}

bool runs_compatible(const RtfRun& a, const RtfRun& b) noexcept {
    return a.font_size_pt == b.font_size_pt &&
           a.color_bgr == b.color_bgr &&
           a.bold == b.bold &&
           a.italic == b.italic &&
           a.underline == b.underline &&
           a.strikeout == b.strikeout &&
           a.charset == b.charset &&
           a.font_name == b.font_name;
}

RtfDocument parse_plain_text(std::string_view raw, u32 default_codepage, u32 default_color_bgr) {
    RtfDocument doc{};
    const std::string utf8 = (default_codepage == 65001u)
        ? std::string(raw)
        : codepage_to_utf8(raw, default_codepage);

    usize pos = 0;
    while (pos <= utf8.size()) {
        usize end = pos;
        while (end < utf8.size() && utf8[end] != '\n' && utf8[end] != '\r') {
            ++end;
        }
        RtfParagraph para{};
        if (end > pos) {
            RtfRun run{};
            run.text_utf8 = utf8.substr(pos, end - pos);
            run.font_name = "Arial";
            run.font_size_pt = 10;
            run.color_bgr = default_color_bgr & 0xFFFFFFu;
            para.runs.push_back(std::move(run));
        }
        doc.paragraphs.push_back(std::move(para));

        if (end >= utf8.size()) break;
        if (utf8[end] == '\r' && end + 1 < utf8.size() && utf8[end + 1] == '\n') {
            pos = end + 2;
        } else {
            pos = end + 1;
        }
        if (pos == utf8.size()) {
            // Trailing newline -> do not invent an extra blank paragraph.
            break;
        }
    }
    return doc;
}

} // namespace

RtfDocument parse_rtf_or_plain_text(std::string_view raw, u32 default_codepage, u32 default_color_bgr) {
    usize lead = 0;
    while (lead < raw.size() && (raw[lead] == ' ' || raw[lead] == '\t' || raw[lead] == '\r' || raw[lead] == '\n')) {
        ++lead;
    }
    // Strip UTF-8 BOM if present on plain text files.
    if (lead + 3 <= raw.size() &&
        static_cast<u8>(raw[lead]) == 0xEFu &&
        static_cast<u8>(raw[lead + 1]) == 0xBBu &&
        static_cast<u8>(raw[lead + 2]) == 0xBFu) {
        lead += 3;
    }

    if (lead >= raw.size()) {
        return RtfDocument{};
    }

    const std::string_view trimmed = raw.substr(lead);
    if (!trimmed.starts_with("{\\rtf")) {
        return parse_plain_text(trimmed, default_codepage, default_color_bgr);
    }

    RtfDocument doc{};
    doc.paragraphs.emplace_back();

    u32 doc_codepage = (default_codepage == 65001u) ? 1252u : default_codepage;
    i32 default_font_idx = 0;

    std::unordered_map<i32, RtfFontDef> font_table;
    std::vector<u32> color_table;
    color_table.push_back(default_color_bgr & 0xFFFFFFu); // color index 0 = auto/default

    std::vector<RtfState> stack;
    RtfState cur{};

    // Font-table builder state
    i32 ft_cur_idx = 0;
    u8 ft_cur_charset = 1;
    std::string ft_cur_name;

    // Color-table builder state
    i32 ct_r = 0, ct_g = 0, ct_b = 0;
    bool ct_has_rgb = false;

    // Pending single/multi-byte ANSI bytes accumulated from consecutive \'hh escapes
    std::string pending_ansi;
    u16 pending_high_surrogate = 0;
    i32 skip_chars = 0;

    auto effective_font = [&](const RtfState& st) -> RtfFontDef {
        if (auto it = font_table.find(st.font_idx); it != font_table.end()) {
            return it->second;
        }
        if (auto it = font_table.find(default_font_idx); it != font_table.end()) {
            return it->second;
        }
        return RtfFontDef{};
    };

    auto effective_codepage = [&](const RtfState& st) -> u32 {
        const auto fd = effective_font(st);
        if (fd.charset != 0 && fd.charset != 1) {
            return gdi_charset_to_codepage(fd.charset, doc_codepage);
        }
        return doc_codepage;
    };

    auto append_utf8 = [&](std::string_view utf8) {
        if (utf8.empty() || cur.skip_group || cur.in_fonttbl || cur.in_colortbl) return;
        auto& para = doc.paragraphs.back();
        para.align = cur.align;
        para.left_indent_px = std::max(0, cur.left_indent_twips / 15);
        para.first_indent_px = cur.first_indent_twips / 15;

        const auto fd = effective_font(cur);
        RtfRun run{};
        run.text_utf8 = std::string(utf8);
        run.font_name = fd.name.empty() ? std::string("Arial") : fd.name;
        run.font_size_pt = std::max(4, cur.size_half_pt / 2);
        if (cur.color_idx >= 0 && static_cast<usize>(cur.color_idx) < color_table.size()) {
            run.color_bgr = color_table[static_cast<usize>(cur.color_idx)];
        } else {
            run.color_bgr = default_color_bgr & 0xFFFFFFu;
        }
        run.bold = cur.bold;
        run.italic = cur.italic;
        run.underline = cur.underline;
        run.strikeout = cur.strikeout;
        run.charset = fd.charset;

        if (!para.runs.empty() && runs_compatible(para.runs.back(), run)) {
            para.runs.back().text_utf8 += run.text_utf8;
        } else {
            para.runs.push_back(std::move(run));
        }
    };

    auto flush_pending_ansi = [&]() {
        if (pending_ansi.empty()) return;
        const std::string converted = codepage_to_utf8(pending_ansi, effective_codepage(cur));
        pending_ansi.clear();
        if (cur.in_fonttbl) {
            ft_cur_name += converted;
        } else {
            append_utf8(converted);
        }
    };

    auto flush_surrogate = [&]() {
        if (pending_high_surrogate != 0) {
            std::string tmp;
            utf8_append_codepoint(tmp, static_cast<u32>(pending_high_surrogate));
            pending_high_surrogate = 0;
            append_utf8(tmp);
        }
    };

    auto commit_font_entry = [&]() {
        const std::string name = trim_font_name(ft_cur_name);
        if (!name.empty()) {
            font_table[ft_cur_idx] = RtfFontDef{name, ft_cur_charset};
        }
        ft_cur_name.clear();
        ft_cur_charset = 1;
    };

    auto start_new_paragraph = [&]() {
        flush_pending_ansi();
        flush_surrogate();
        auto& prev = doc.paragraphs.back();
        prev.align = cur.align;
        prev.left_indent_px = std::max(0, cur.left_indent_twips / 15);
        prev.first_indent_px = cur.first_indent_twips / 15;
        RtfParagraph next{};
        next.align = cur.align;
        next.left_indent_px = std::max(0, cur.left_indent_twips / 15);
        next.first_indent_px = cur.first_indent_twips / 15;
        doc.paragraphs.push_back(std::move(next));
    };

    const usize n = trimmed.size();
    usize i = 0;
    while (i < n) {
        const char ch = trimmed[i];

        if (ch == '{') {
            flush_pending_ansi();
            flush_surrogate();
            stack.push_back(cur);
            if (cur.in_fonttbl) {
                ft_cur_name.clear();
                ft_cur_charset = 1;
            }
            ++i;
            continue;
        }

        if (ch == '}') {
            flush_pending_ansi();
            flush_surrogate();
            if (cur.in_fonttbl) {
                commit_font_entry();
            }
            if (!stack.empty()) {
                cur = stack.back();
                stack.pop_back();
            }
            ++i;
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            ++i;
            continue;
        }

        if (ch == '\\') {
            ++i;
            if (i >= n) break;
            const char c = trimmed[i];

            // Control symbol (non-ASCII-letter)
            if (!std::isalpha(static_cast<unsigned char>(c))) {
                ++i;
                if (c == '\'') {
                    if (i + 1 < n) {
                        const i32 hi = hex_digit(trimmed[i]);
                        const i32 lo = hex_digit(trimmed[i + 1]);
                        if (hi >= 0 && lo >= 0) {
                            i += 2;
                            if (skip_chars > 0) {
                                --skip_chars;
                            } else if (!cur.skip_group && !cur.in_colortbl) {
                                flush_surrogate();
                                pending_ansi.push_back(static_cast<char>((hi << 4) | lo));
                            }
                        }
                    }
                    continue;
                }

                flush_pending_ansi();
                flush_surrogate();

                if (c == '*') {
                    cur.skip_group = true;
                } else if (c == '\\' || c == '{' || c == '}') {
                    if (skip_chars > 0) {
                        --skip_chars;
                    } else if (cur.in_fonttbl) {
                        ft_cur_name.push_back(c);
                    } else {
                        append_utf8(std::string_view(&c, 1));
                    }
                } else if (c == '~') {
                    append_utf8(" ");
                } else if (c == '_') {
                    append_utf8("-");
                } else if (c == '\r' || c == '\n') {
                    if (!cur.skip_group && !cur.in_fonttbl && !cur.in_colortbl) {
                        start_new_paragraph();
                    }
                }
                continue;
            }

            // Control word: [A-Za-z]+ followed by optional signed integer parameter and optional delimiter space.
            const usize wstart = i;
            while (i < n && std::isalpha(static_cast<unsigned char>(trimmed[i]))) {
                ++i;
            }
            const std::string_view word = trimmed.substr(wstart, i - wstart);

            bool has_param = false;
            bool neg = false;
            i32 param = 0;
            if (i < n && trimmed[i] == '-') {
                neg = true;
                ++i;
            }
            if (i < n && std::isdigit(static_cast<unsigned char>(trimmed[i]))) {
                has_param = true;
                i64 val = 0;
                while (i < n && std::isdigit(static_cast<unsigned char>(trimmed[i]))) {
                    val = val * 10 + (trimmed[i] - '0');
                    if (val > 10000000) val = 10000000;
                    ++i;
                }
                param = static_cast<i32>(neg ? -val : val);
            }
            if (i < n && trimmed[i] == ' ') {
                ++i; // RTF spec: single space delimits control word and is consumed
            }

            // Handle \uN before flushing pending high surrogate so surrogate pairs combine cleanly!
            if (word == "u" && has_param) {
                flush_pending_ansi();
                if (!cur.skip_group && !cur.in_fonttbl && !cur.in_colortbl) {
                    const u32 cu = static_cast<u32>(param < 0 ? param + 65536 : param);
                    if (cu >= 0xD800u && cu <= 0xDBFFu) {
                        flush_surrogate();
                        pending_high_surrogate = static_cast<u16>(cu);
                    } else if (cu >= 0xDC00u && cu <= 0xDFFFu && pending_high_surrogate != 0) {
                        const u32 full_cp = 0x10000u +
                            ((static_cast<u32>(pending_high_surrogate) - 0xD800u) << 10u) +
                            (cu - 0xDC00u);
                        pending_high_surrogate = 0;
                        std::string tmp;
                        utf8_append_codepoint(tmp, full_cp);
                        append_utf8(tmp);
                    } else {
                        flush_surrogate();
                        std::string tmp;
                        utf8_append_codepoint(tmp, cu);
                        append_utf8(tmp);
                    }
                }
                skip_chars = cur.uc_skip;
                continue;
            }

            flush_pending_ansi();
            flush_surrogate();

            if (cur.skip_group) {
                continue;
            }

            if (word == "fonttbl") {
                cur.in_fonttbl = true;
                ft_cur_idx = 0;
                ft_cur_charset = 1;
                ft_cur_name.clear();
            } else if (word == "colortbl") {
                cur.in_colortbl = true;
                color_table.clear();
                ct_r = ct_g = ct_b = 0;
                ct_has_rgb = false;
            } else if (is_ignorable_destination(word)) {
                cur.skip_group = true;
            } else if (cur.in_fonttbl) {
                if (word == "f" && has_param) {
                    ft_cur_idx = param;
                } else if (word == "fcharset" && has_param) {
                    ft_cur_charset = static_cast<u8>(std::clamp(param, 0, 255));
                }
            } else if (cur.in_colortbl) {
                if (word == "red" && has_param) {
                    ct_r = std::clamp(param, 0, 255);
                    ct_has_rgb = true;
                } else if (word == "green" && has_param) {
                    ct_g = std::clamp(param, 0, 255);
                    ct_has_rgb = true;
                } else if (word == "blue" && has_param) {
                    ct_b = std::clamp(param, 0, 255);
                    ct_has_rgb = true;
                }
            } else {
                // Normal body control words
                if (word == "ansicpg" && has_param && param > 0) {
                    doc_codepage = static_cast<u32>(param);
                } else if (word == "deff" && has_param) {
                    default_font_idx = param;
                    cur.font_idx = param;
                } else if (word == "uc" && has_param) {
                    cur.uc_skip = std::max(0, param);
                } else if (word == "f" && has_param) {
                    cur.font_idx = param;
                } else if (word == "fs" && has_param) {
                    cur.size_half_pt = std::clamp(param, 8, 240);
                } else if (word == "cf") {
                    cur.color_idx = has_param ? param : 0;
                } else if (word == "b") {
                    cur.bold = has_param ? (param != 0) : true;
                } else if (word == "i") {
                    cur.italic = has_param ? (param != 0) : true;
                } else if (word == "ul") {
                    cur.underline = has_param ? (param != 0) : true;
                } else if (word == "ulnone") {
                    cur.underline = false;
                } else if (word == "strike") {
                    cur.strikeout = has_param ? (param != 0) : true;
                } else if (word == "plain") {
                    cur.font_idx = default_font_idx;
                    cur.size_half_pt = 20;
                    cur.color_idx = 0;
                    cur.bold = false;
                    cur.italic = false;
                    cur.underline = false;
                    cur.strikeout = false;
                } else if (word == "pard") {
                    cur.align = 0;
                    cur.left_indent_twips = 0;
                    cur.first_indent_twips = 0;
                } else if (word == "ql" || word == "qj") {
                    cur.align = 0;
                } else if (word == "qc") {
                    cur.align = 1;
                } else if (word == "qr") {
                    cur.align = 2;
                } else if (word == "li" && has_param) {
                    cur.left_indent_twips = std::max(0, param);
                } else if (word == "fi" && has_param) {
                    cur.first_indent_twips = param;
                } else if (word == "par" || word == "line") {
                    start_new_paragraph();
                } else if (word == "tab") {
                    append_utf8("    ");
                } else if (word == "bullet") {
                    append_utf8("\xE2\x80\xA2");
                } else if (word == "endash") {
                    append_utf8("\xE2\x80\x93");
                } else if (word == "emdash") {
                    append_utf8("\xE2\x80\x94");
                } else if (word == "lquote") {
                    append_utf8("\xE2\x80\x98");
                } else if (word == "rquote") {
                    append_utf8("\xE2\x80\x99");
                } else if (word == "ldblquote") {
                    append_utf8("\xE2\x80\x9C");
                } else if (word == "rdblquote") {
                    append_utf8("\xE2\x80\x9D");
                }
            }
            continue;
        }

        // Plain character
        ++i;
        if (skip_chars > 0) {
            --skip_chars;
            continue;
        }
        if (cur.skip_group) {
            continue;
        }

        if (cur.in_fonttbl) {
            flush_pending_ansi();
            if (ch == ';') {
                commit_font_entry();
            } else {
                ft_cur_name.push_back(ch);
            }
            continue;
        }

        if (cur.in_colortbl) {
            if (ch == ';') {
                if (ct_has_rgb) {
                    const u32 bgr = static_cast<u32>(ct_r) |
                                    (static_cast<u32>(ct_g) << 8u) |
                                    (static_cast<u32>(ct_b) << 16u);
                    color_table.push_back(bgr);
                } else {
                    color_table.push_back(default_color_bgr & 0xFFFFFFu);
                }
                ct_r = ct_g = ct_b = 0;
                ct_has_rgb = false;
            }
            continue;
        }

        flush_surrogate();
        if (static_cast<u8>(ch) < 0x80u) {
            flush_pending_ansi();
            append_utf8(std::string_view(&ch, 1));
        } else {
            pending_ansi.push_back(ch);
        }
    }

    flush_pending_ansi();
    flush_surrogate();

    // Remove a single trailing empty paragraph produced by a final "\par}".
    if (doc.paragraphs.size() > 1 && doc.paragraphs.back().runs.empty()) {
        doc.paragraphs.pop_back();
    }
    return doc;
}

} // namespace gm
