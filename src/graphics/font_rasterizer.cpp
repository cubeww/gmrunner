#include "graphics/font_rasterizer.h"

#include "base/encoding.h"
#include "base/math.h"
#include "graphics/builtin_font.h"
#include "platform/font_loader.h"
#include "platform/web_bridge.h"

#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace gm {

namespace {

struct TempGlyph {
    i32 code = 0;
    i32 bw = 0;       // bitmap width
    i32 bh = 0;       // bitmap height
    i32 top_y = 0;    // vertical offset from top of cell to bitmap top
    i32 cell_w = 0;   // glyph quad width (g.w)
    i32 cell_h = 0;   // glyph quad height (g.h)
    i32 offset = 0;   // horizontal draw offset (g.offset)
    i32 shift = 0;    // horizontal advance (g.shift)
    std::vector<u8> alpha;
};

u32 byte_to_codepoint(u8 b, u32 codepage) {
    if (b < 128u) return static_cast<u32>(b);
    if (codepage == 65001u) return static_cast<u32>(b);
    const char ch = static_cast<char>(b);
    const std::string utf8 = codepage_to_utf8(std::string_view(&ch, 1), codepage);
    if (utf8.empty()) return static_cast<u32>(b);
    usize off = 0;
    return utf8_decode_next(utf8, off);
}

void apply_faux_bold(std::vector<u8>& alpha, i32& w, i32 h, i32 extra_px) {
    if (w <= 0 || h <= 0 || extra_px <= 0) return;
    const i32 nw = w + extra_px;
    std::vector<u8> out(static_cast<usize>(nw) * static_cast<usize>(h), 0u);
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < nw; ++x) {
            u8 v = 0;
            for (i32 k = 0; k <= extra_px; ++k) {
                const i32 sx = x - k;
                if (sx >= 0 && sx < w) {
                    v = std::max(v, alpha[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(sx)]);
                }
            }
            out[static_cast<usize>(y) * static_cast<usize>(nw) + static_cast<usize>(x)] = v;
        }
    }
    alpha = std::move(out);
    w = nw;
}

#if !defined(__EMSCRIPTEN__)
void apply_faux_italic(std::vector<u8>& alpha, i32& w, i32 h, i32& xoff, i32 yoff) {
    if (w <= 0 || h <= 0) return;
    constexpr f32 kShear = 0.22f;
    const f32 shift_top = static_cast<f32>(-yoff) * kShear;
    const f32 shift_bot = static_cast<f32>(-(yoff + h - 1)) * kShear;
    const i32 min_shift = static_cast<i32>(std::floor(std::min(shift_top, shift_bot)));
    const i32 max_shift = static_cast<i32>(std::ceil(std::max(shift_top, shift_bot)));
    const i32 nw = w + (max_shift - min_shift) + 1;
    std::vector<u8> out(static_cast<usize>(nw) * static_cast<usize>(h), 0u);

    for (i32 y = 0; y < h; ++y) {
        const f32 s = static_cast<f32>(-(yoff + y)) * kShear - static_cast<f32>(min_shift);
        const i32 s0 = static_cast<i32>(std::floor(s));
        const f32 frac = s - static_cast<f32>(s0);
        for (i32 x = 0; x < w; ++x) {
            const u8 a = alpha[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
            if (a == 0) continue;
            const i32 dx0 = x + s0;
            const i32 dx1 = dx0 + 1;
            if (dx0 >= 0 && dx0 < nw) {
                const usize idx = static_cast<usize>(y) * static_cast<usize>(nw) + static_cast<usize>(dx0);
                out[idx] = static_cast<u8>(std::min<i32>(255, out[idx] + static_cast<i32>(a * (1.0f - frac) + 0.5f)));
            }
            if (dx1 >= 0 && dx1 < nw) {
                const usize idx = static_cast<usize>(y) * static_cast<usize>(nw) + static_cast<usize>(dx1);
                out[idx] = static_cast<u8>(std::min<i32>(255, out[idx] + static_cast<i32>(a * frac + 0.5f)));
            }
        }
    }
    alpha = std::move(out);
    w = nw;
    xoff += min_shift;
}

struct TtfFace {
    SystemFontBlob blob;
    stbtt_fontinfo info{};
    bool valid = false;
};
#endif

void rasterize_builtin_fallback_glyph(TempGlyph& g, u32 cp, f32 em_px, bool bold, i32 ascent_px) {
    const u32 ascii = (cp >= 32u && cp <= 126u) ? cp : static_cast<u32>('?');
    const i32 scale_px = std::max(1, static_cast<i32>(std::round(em_px / 9.0f)));
    i32 gw = 8 * scale_px;
    const i32 gh = 8 * scale_px;
    std::vector<u8> alpha(static_cast<usize>(gw) * static_cast<usize>(gh), 0u);
    const auto& bits = kBuiltinFont8x8[ascii - 32u];
    for (i32 row = 0; row < 8; ++row) {
        for (i32 col = 0; col < 8; ++col) {
            if ((bits[row] & (1u << col)) != 0u) {
                for (i32 dy = 0; dy < scale_px; ++dy) {
                    for (i32 dx = 0; dx < scale_px; ++dx) {
                        const usize p = static_cast<usize>(row * scale_px + dy) * static_cast<usize>(gw) +
                                        static_cast<usize>(col * scale_px + dx);
                        alpha[p] = 255u;
                    }
                }
            }
        }
    }
    i32 adv = gw;
    if (bold) {
        const i32 extra = std::max(1, scale_px / 2);
        apply_faux_bold(alpha, gw, gh, extra);
        adv += extra;
    }
    g.bw = gw;
    g.bh = gh;
    g.top_y = std::max(0, ascent_px - gh + std::max(1, scale_px));
    g.cell_w = gw;
    g.cell_h = g.top_y + gh;
    g.offset = 0;
    g.shift = adv;
    g.alpha = std::move(alpha);
}

bool try_pack_shelf(const std::vector<TempGlyph>& glyphs, i32 tex_w, i32 tex_h,
                    std::vector<std::pair<i32, i32>>& positions) {
    constexpr i32 kPad = 2;
    positions.resize(glyphs.size());
    i32 cx = 0;
    i32 cy = 0;
    i32 row_h = 0;

    for (usize i = 0; i < glyphs.size(); ++i) {
        const auto& g = glyphs[i];
        const i32 w = g.cell_w;
        const i32 h = g.cell_h;
        if (w <= 0 || h <= 0) {
            positions[i] = {0, 0};
            continue;
        }
        if (w > tex_w || h > tex_h) {
            return false;
        }
        if (cx + w > tex_w) {
            cx = 0;
            cy += row_h + kPad;
            row_h = 0;
        }
        if (cy + h > tex_h) {
            return false;
        }
        positions[i] = {cx, cy};
        cx += w + kPad;
        row_h = std::max(row_h, h);
    }
    return true;
}

} // namespace

bool rasterize_gm_font(FontData& font, u32 default_codepage) {
    font.first = std::clamp(font.first, 0, 255);
    font.last = std::clamp(font.last, 0, 255);
    font.glyphs.fill(GlyphData{});
    font.alpha_map.clear();
    font.tex_width = 0;
    font.tex_height = 0;

    if (font.first > font.last) {
        return true;
    }

    const f32 pt = static_cast<f32>(std::clamp(font.size, 1, 200));
    // GM8 TFont uses 96 DPI: pixel_em = round(pt * 96 / 72).
    const f32 em_px = std::max(5.0f, static_cast<f32>(delphi_round(static_cast<f64>(pt) * 96.0 / 72.0)));
    const u32 codepage = gdi_charset_to_codepage(font.charset, default_codepage);

    i32 ascent_px = std::max(1, static_cast<i32>(std::ceil(em_px * 0.85f)));
    i32 descent_px = std::max(0, static_cast<i32>(std::ceil(em_px * 0.20f)));
    i32 tm_height = std::max(ascent_px + descent_px, static_cast<i32>(std::ceil(em_px * 1.18f)));

#if defined(__EMSCRIPTEN__)
    (void)web_measure_font_metrics(font.font_name, em_px, font.bold, font.italic,
                                   ascent_px, descent_px, tm_height);
#else
    TtfFace primary{};
    if (load_system_font(font.font_name, font.bold, font.italic, font.charset, primary.blob) &&
        !primary.blob.data.empty()) {
        const i32 off = stbtt_GetFontOffsetForIndex(primary.blob.data.data(), primary.blob.face_index);
        if (off >= 0 && stbtt_InitFont(&primary.info, primary.blob.data.data(), off) != 0) {
            primary.valid = true;
        }
    }

    f32 primary_scale = 0.0f;
    if (primary.valid) {
        primary_scale = stbtt_ScaleForMappingEmToPixels(&primary.info, em_px);
        int asc = 0, desc = 0, lg = 0;
        stbtt_GetFontVMetrics(&primary.info, &asc, &desc, &lg);
        ascent_px = std::max(1, static_cast<i32>(std::ceil(static_cast<f32>(asc) * primary_scale)));
        descent_px = std::max(0, static_cast<i32>(std::ceil(static_cast<f32>(-desc) * primary_scale)));
        const i32 gap = std::max(0, static_cast<i32>(std::round(static_cast<f32>(lg) * primary_scale)));
        tm_height = std::max(ascent_px + descent_px + gap, static_cast<i32>(std::round(em_px * 1.15f)));
    }

    std::vector<TtfFace> fallbacks;
    bool fallbacks_loaded = false;
    auto ensure_fallbacks = [&]() {
        if (fallbacks_loaded) return;
        fallbacks_loaded = true;
        auto blobs = load_system_fallback_fonts(font.charset);
        for (auto& b : blobs) {
            if (b.data.empty()) continue;
            TtfFace f{};
            f.blob = std::move(b);
            const i32 off = stbtt_GetFontOffsetForIndex(f.blob.data.data(), f.blob.face_index);
            if (off >= 0 && stbtt_InitFont(&f.info, f.blob.data.data(), off) != 0) {
                f.valid = true;
                fallbacks.push_back(std::move(f));
            }
        }
    };
#endif

    std::vector<TempGlyph> temp_glyphs;
    temp_glyphs.reserve(static_cast<usize>(font.last - font.first + 1));

    i32 space_advance = std::max(2, static_cast<i32>(std::round(em_px * 0.30f)));
#if !defined(__EMSCRIPTEN__)
    if (primary.valid) {
        const int sp_idx = stbtt_FindGlyphIndex(&primary.info, 32);
        if (sp_idx != 0) {
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&primary.info, sp_idx, &adv, &lsb);
            const i32 sp_px = static_cast<i32>(std::round(static_cast<f32>(adv) * primary_scale));
            if (sp_px > 0) space_advance = sp_px;
        }
    }
#endif

    for (i32 c = font.first; c <= font.last; ++c) {
        TempGlyph g{};
        g.code = c;
        const u32 cp = byte_to_codepoint(static_cast<u8>(c), codepage);

        if (c == 32) {
            // Match GM8 sub_57DEDC: space carries tmHeight and space advance with empty alpha.
#if defined(__EMSCRIPTEN__)
            WebGlyphBitmap wbmp{};
            if (web_rasterize_glyph(font.font_name, em_px, font.bold, font.italic, 32u, wbmp) &&
                wbmp.advance >= 1.0f) {
                space_advance = static_cast<i32>(std::round(wbmp.advance));
            }
#endif
            g.cell_w = std::max(1, space_advance);
            g.cell_h = std::max(1, tm_height);
            g.shift = g.cell_w;
            g.offset = 0;
            temp_glyphs.push_back(std::move(g));
            continue;
        }

        if (c < 32) {
            // Control characters have zero-size glyphs in GM8.
            temp_glyphs.push_back(std::move(g));
            continue;
        }

        bool rasterized = false;

#if defined(__EMSCRIPTEN__)
        WebGlyphBitmap wbmp{};
        if (web_rasterize_glyph(font.font_name, em_px, font.bold, font.italic, cp, wbmp)) {
            g.bw = wbmp.width;
            g.bh = wbmp.height;
            g.top_y = std::max(0, ascent_px + wbmp.yoff);
            g.cell_w = wbmp.width;
            g.cell_h = (wbmp.width > 0 && wbmp.height > 0) ? (g.top_y + wbmp.height) : 0;
            g.offset = wbmp.xoff;
            const i32 adv_px = std::max(0, static_cast<i32>(std::round(wbmp.advance)));
            g.shift = adv_px - std::min(0, g.offset);
            g.alpha = std::move(wbmp.alpha);
            rasterized = true;
        }
#else
        const TtfFace* face = nullptr;
        int glyph_idx = 0;
        if (primary.valid) {
            glyph_idx = stbtt_FindGlyphIndex(&primary.info, static_cast<int>(cp));
            if (glyph_idx != 0) {
                face = &primary;
            }
        }
        if (!face) {
            ensure_fallbacks();
            for (const auto& fb : fallbacks) {
                const int idx = stbtt_FindGlyphIndex(&fb.info, static_cast<int>(cp));
                if (idx != 0) {
                    face = &fb;
                    glyph_idx = idx;
                    break;
                }
            }
        }

        if (face && glyph_idx != 0) {
            const f32 sc = (face == &primary)
                               ? primary_scale
                               : stbtt_ScaleForMappingEmToPixels(&face->info, em_px);
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&face->info, glyph_idx, &adv, &lsb);
            i32 cell_inc_x = std::max(0, static_cast<i32>(std::round(static_cast<f32>(adv) * sc)));

            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&face->info, glyph_idx, sc, sc, &x0, &y0, &x1, &y1);
            i32 gw = x1 - x0;
            i32 gh = y1 - y0;
            i32 xoff = x0;
            if (gw > 0 && gh > 0) {
                g.alpha.assign(static_cast<usize>(gw) * static_cast<usize>(gh), 0u);
                stbtt_MakeGlyphBitmap(&face->info, g.alpha.data(), gw, gh, gw, sc, sc, glyph_idx);
                if (font.bold && !face->blob.native_bold) {
                    const i32 extra = 1;
                    apply_faux_bold(g.alpha, gw, gh, extra);
                    cell_inc_x += extra;
                }
                if (font.italic && !face->blob.native_italic) {
                    apply_faux_italic(g.alpha, gw, gh, xoff, y0);
                }
            }

            g.bw = gw;
            g.bh = gh;
            g.top_y = std::max(0, ascent_px + y0);
            g.cell_w = gw;
            g.cell_h = (gw > 0 && gh > 0) ? (g.top_y + gh) : 0;
            g.offset = xoff;
            g.shift = cell_inc_x - std::min(0, xoff);
            rasterized = true;
        }
#endif

        if (!rasterized) {
            rasterize_builtin_fallback_glyph(g, cp, em_px, font.bold, ascent_px);
        }

        temp_glyphs.push_back(std::move(g));
    }

    // Select the tightest power-of-2 atlas dimensions up to 2048x2048.
    static constexpr std::array<std::pair<i32, i32>, 11> kAtlasCandidates = {{
        {64, 64}, {128, 64}, {128, 128}, {256, 128}, {256, 256},
        {512, 256}, {512, 512}, {1024, 512}, {1024, 1024}, {2048, 1024}, {2048, 2048},
    }};

    std::vector<std::pair<i32, i32>> positions;
    i32 chosen_w = 0;
    i32 chosen_h = 0;
    for (const auto& [cand_w, cand_h] : kAtlasCandidates) {
        if (try_pack_shelf(temp_glyphs, cand_w, cand_h, positions)) {
            chosen_w = cand_w;
            chosen_h = cand_h;
            break;
        }
    }
    if (chosen_w <= 0 || chosen_h <= 0) {
        return false;
    }

    font.tex_width = chosen_w;
    font.tex_height = chosen_h;
    font.alpha_map.assign(static_cast<usize>(chosen_w) * static_cast<usize>(chosen_h), 0u);

    for (usize i = 0; i < temp_glyphs.size(); ++i) {
        const auto& tg = temp_glyphs[i];
        const auto [px, py] = positions[i];
        auto& out = font.glyphs[static_cast<usize>(tg.code)];
        out.x = px;
        out.y = py;
        out.w = tg.cell_w;
        out.h = tg.cell_h;
        out.shift = tg.shift;
        out.offset = tg.offset;

        if (!tg.alpha.empty() && tg.bw > 0 && tg.bh > 0) {
            for (i32 row = 0; row < tg.bh; ++row) {
                const i32 dy = py + tg.top_y + row;
                if (dy < 0 || dy >= chosen_h) continue;
                const i32 copy_w = std::min(tg.bw, chosen_w - px);
                if (copy_w <= 0) continue;
                std::memcpy(
                    font.alpha_map.data() + static_cast<usize>(dy) * static_cast<usize>(chosen_w) + static_cast<usize>(px),
                    tg.alpha.data() + static_cast<usize>(row) * static_cast<usize>(tg.bw),
                    static_cast<usize>(copy_w));
            }
        }
    }

    return true;
}

} // namespace gm
