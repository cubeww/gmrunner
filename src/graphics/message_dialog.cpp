#include "graphics/message_dialog.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "graphics/builtin_font.h"
#include "graphics/gl_headers.h"
#include "platform/font_loader.h"
#include "platform/web_bridge.h"

#include <stb_image.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace gm {

namespace {

constexpr i32 kAtlasSize = 512;
constexpr f32 kTitleBarHeight = 22.0f;
constexpr f32 kDialogFrameBorder = 3.0f;
constexpr f32 kCloseBtnW = 16.0f;
constexpr f32 kCloseBtnH = 14.0f;

struct DialogVertex {
    f32 x, y;
    f32 u, v;
    f32 r, g, b, a;
};

struct ColorF {
    f32 r, g, b, a;
};

ColorF bgr_to_colorf(u32 bgr, f32 alpha) noexcept {
    const f32 r = static_cast<f32>(bgr & 0xFFu) / 255.0f;
    const f32 g = static_cast<f32>((bgr >> 8u) & 0xFFu) / 255.0f;
    const f32 b = static_cast<f32>((bgr >> 16u) & 0xFFu) / 255.0f;
    return ColorF{r, g, b, std::clamp(alpha, 0.0f, 1.0f)};
}

ColorF rgb_to_colorf(u8 r, u8 g, u8 b, f32 alpha) noexcept {
    return ColorF{
        static_cast<f32>(r) / 255.0f,
        static_cast<f32>(g) / 255.0f,
        static_cast<f32>(b) / 255.0f,
        std::clamp(alpha, 0.0f, 1.0f)
    };
}

u32 ascii_upper_cp(u32 cp) noexcept {
    if (cp >= 'a' && cp <= 'z') return cp - ('a' - 'A');
    return cp;
}

std::vector<u32> utf8_to_codepoints(std::string_view utf8) {
    std::vector<u32> out;
    out.reserve(utf8.size());
    usize off = 0;
    while (off < utf8.size()) {
        out.push_back(utf8_decode_next(utf8, off));
    }
    return out;
}

std::string codepoints_to_utf8(std::span<const u32> cps) {
    std::string out;
    out.reserve(cps.size());
    for (u32 cp : cps) {
        utf8_append_codepoint(out, cp);
    }
    return out;
}

// Expands GM8 '#' line breaks ('\#' -> literal '#', '#' -> '\n', '\r\n'/'\r' -> '\n').
std::string expand_gm_message_newlines(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (usize i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\\' && i + 1 < raw.size() && raw[i + 1] == '#') {
            out.push_back('#');
            ++i;
        } else if (c == '#') {
            out.push_back('\n');
        } else if (c == '\r') {
            out.push_back('\n');
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++i;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool is_word_char(u32 cp) noexcept {
    if (cp < 128) {
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
               (cp >= '0' && cp <= '9') || cp == '_';
    }
    return cp > 0x7Fu && cp != 0x3000u;
}

// Applies GM8 TMessageForm color-key transparency rule:
// If the bottom-left pixel has alpha == 0, pixels matching its RGB (or alpha == 0)
// become transparent and all other pixels become opaque. Otherwise the whole image is opaque.
Bitmap32 process_gm_message_bitmap(const Bitmap32& src, bool force_check_transparency, bool* out_was_transparent = nullptr) {
    Bitmap32 dst = src;
    if (dst.width <= 0 || dst.height <= 0 ||
        dst.bgra.size() < static_cast<usize>(dst.width) * static_cast<usize>(dst.height) * 4u) {
        if (out_was_transparent) *out_was_transparent = false;
        return dst;
    }

    const usize bl_idx = (static_cast<usize>(dst.height - 1) * static_cast<usize>(dst.width)) * 4u;
    const u8 key_b = dst.bgra[bl_idx + 0];
    const u8 key_g = dst.bgra[bl_idx + 1];
    const u8 key_r = dst.bgra[bl_idx + 2];
    const bool transparent = force_check_transparency && (dst.bgra[bl_idx + 3] == 0u);
    if (out_was_transparent) *out_was_transparent = transparent;

    const usize count = static_cast<usize>(dst.width) * static_cast<usize>(dst.height);
    for (usize i = 0; i < count; ++i) {
        const usize p = i * 4u;
        if (transparent &&
            (dst.bgra[p + 3] == 0u ||
             (dst.bgra[p + 0] == key_b && dst.bgra[p + 1] == key_g && dst.bgra[p + 2] == key_r))) {
            dst.bgra[p + 3] = 0u;
        } else {
            dst.bgra[p + 3] = 255u;
        }
    }

    // Bleed adjacent opaque RGB into transparent color-key pixels so bilinear stretching
    // never creates magenta/green fringe halos along transparent cutout edges.
    if (transparent) {
        const auto copy = dst.bgra;
        for (i32 y = 0; y < dst.height; ++y) {
            for (i32 x = 0; x < dst.width; ++x) {
                const usize p = (static_cast<usize>(y) * static_cast<usize>(dst.width) + static_cast<usize>(x)) * 4u;
                if (copy[p + 3] != 0u) continue;
                u32 sb = 0, sg = 0, sr = 0, n = 0;
                for (i32 dy = -1; dy <= 1; ++dy) {
                    const i32 ny = y + dy;
                    if (ny < 0 || ny >= dst.height) continue;
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 nx = x + dx;
                        if (nx < 0 || nx >= dst.width) continue;
                        const usize np = (static_cast<usize>(ny) * static_cast<usize>(dst.width) + static_cast<usize>(nx)) * 4u;
                        if (copy[np + 3] != 0u) {
                            sb += copy[np + 0];
                            sg += copy[np + 1];
                            sr += copy[np + 2];
                            ++n;
                        }
                    }
                }
                if (n > 0) {
                    dst.bgra[p + 0] = static_cast<u8>(sb / n);
                    dst.bgra[p + 1] = static_cast<u8>(sg / n);
                    dst.bgra[p + 2] = static_cast<u8>(sr / n);
                } else {
                    dst.bgra[p + 0] = 0u;
                    dst.bgra[p + 1] = 0u;
                    dst.bgra[p + 2] = 0u;
                }
            }
        }
    }
    return dst;
}

// Fallback procedural recreation of the original GM8 320x140 TMessageForm background.
Bitmap32 make_procedural_bg_bitmap() {
    Bitmap32 bmp{};
    bmp.width = 320;
    bmp.height = 140;
    bmp.bgra.resize(static_cast<usize>(bmp.width) * static_cast<usize>(bmp.height) * 4u);

    auto set_px = [&](i32 x, i32 y, u8 r, u8 g, u8 b) {
        if (x < 0 || x >= bmp.width || y < 0 || y >= bmp.height) return;
        const usize p = (static_cast<usize>(y) * static_cast<usize>(bmp.width) + static_cast<usize>(x)) * 4u;
        bmp.bgra[p + 0] = b;
        bmp.bgra[p + 1] = g;
        bmp.bgra[p + 2] = r;
        bmp.bgra[p + 3] = 255u;
    };

    for (i32 y = 0; y < bmp.height; ++y) {
        for (i32 x = 0; x < bmp.width; ++x) {
            set_px(x, y, 90, 90, 90);
        }
    }

    // 3-pixel sunken outer bevel matching original GM8 BackgroundImage.
    constexpr std::array<u8, 3> tl_shades = {12, 34, 54};
    constexpr std::array<u8, 3> br_shades = {104, 120, 176};
    for (i32 d = 0; d < 3; ++d) {
        const u8 tl = tl_shades[static_cast<usize>(d)];
        const u8 br = br_shades[static_cast<usize>(d)];
        for (i32 x = d; x < bmp.width - d; ++x) {
            set_px(x, d, tl, tl, tl);
            set_px(x, bmp.height - 1 - d, br, br, br);
        }
        for (i32 y = d; y < bmp.height - d; ++y) {
            set_px(d, y, tl, tl, tl);
            set_px(bmp.width - 1 - d, y, br, br, br);
        }
    }
    return bmp;
}

// Fallback procedural recreation of the original GM8 80x25 3-state button strip.
Bitmap32 make_procedural_button_bitmap(i32 state) {
    struct Palette {
        u8 fill[3];
        u8 hi[3];
        u8 lo[3];
    };
    static constexpr std::array<Palette, 3> kPalettes = {{
        {{71, 71, 71},  {111, 111, 111}, {37, 37, 37}},  // 0: normal dark gray
        {{136, 0, 0},   {153, 29, 29},   {62, 0, 0}},    // 1: hover dark crimson
        {{181, 24, 24}, {198, 74, 74},   {107, 8, 8}},   // 2: pressed bright red
    }};
    const Palette& pal = kPalettes[static_cast<usize>(std::clamp(state, 0, 2))];

    Bitmap32 bmp{};
    bmp.width = 80;
    bmp.height = 25;
    bmp.bgra.resize(static_cast<usize>(bmp.width) * static_cast<usize>(bmp.height) * 4u);

    auto set_px = [&](i32 x, i32 y, const u8 c[3]) {
        const usize p = (static_cast<usize>(y) * static_cast<usize>(bmp.width) + static_cast<usize>(x)) * 4u;
        bmp.bgra[p + 0] = c[2];
        bmp.bgra[p + 1] = c[1];
        bmp.bgra[p + 2] = c[0];
        bmp.bgra[p + 3] = 255u;
    };

    for (i32 y = 0; y < bmp.height; ++y) {
        for (i32 x = 0; x < bmp.width; ++x) {
            set_px(x, y, pal.fill);
        }
    }

    auto draw_bevel = [&](i32 inset, const u8 tl[3], const u8 br[3]) {
        const i32 x0 = inset;
        const i32 y0 = inset;
        const i32 x1 = bmp.width - 1 - inset;
        const i32 y1 = bmp.height - 1 - inset;
        for (i32 x = x0; x <= x1; ++x) {
            set_px(x, y0, tl);
            set_px(x, y1, br);
        }
        for (i32 y = y0; y <= y1; ++y) {
            set_px(x0, y, tl);
            set_px(x1, y, br);
        }
    };

    // Outer raised 1px bevel, 1px fill groove, inner sunken 1px bevel.
    draw_bevel(0, pal.hi, pal.lo);
    draw_bevel(2, pal.lo, pal.hi);
    return bmp;
}

// ---------------------------------------------------------------------------
// Dynamic Glyph Atlas & Font Cache
// ---------------------------------------------------------------------------

struct GlyphEntry {
    i32 atlas_page = -1;
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    i32 w = 0;
    i32 h = 0;
    i32 xoff = 0;
    i32 yoff = 0;
    f32 advance = 0.0f;
};

struct AtlasPage {
    TextureHandle tex{};
    i32 cursor_x = 1;
    i32 cursor_y = 1;
    i32 row_h = 0;
};

struct FontBlobCacheKey {
    std::string name_lower;
    bool bold = false;
    bool italic = false;
    u8 charset = 1;
    bool operator==(const FontBlobCacheKey&) const = default;
};

struct FontBlobCacheHash {
    usize operator()(const FontBlobCacheKey& k) const noexcept {
        usize h = std::hash<std::string>{}(k.name_lower);
        h ^= (static_cast<usize>(k.bold) << 1u) ^ (static_cast<usize>(k.italic) << 2u) ^
             (static_cast<usize>(k.charset) << 3u);
        return h;
    }
};

struct FaceKey {
    std::string name_lower;
    i32 em_px_q4 = 44; // quantized pixel height * 4
    bool bold = false;
    bool italic = false;
    u8 charset = 1;
    bool operator==(const FaceKey&) const = default;
};

struct FaceKeyHash {
    usize operator()(const FaceKey& k) const noexcept {
        usize h = std::hash<std::string>{}(k.name_lower);
        h ^= static_cast<usize>(k.em_px_q4) * 2654435761u;
        h ^= (static_cast<usize>(k.bold) << 1u) ^ (static_cast<usize>(k.italic) << 2u) ^
             (static_cast<usize>(k.charset) << 3u);
        return h;
    }
};

struct LoadedTtfFace {
    SystemFontBlob blob;
    stbtt_fontinfo info{};
    bool valid = false;
};

struct CachedFontFace {
    std::string family_name;
    f32 em_px = 11.0f;
    bool bold = false;
    bool italic = false;
    u8 charset = 1;

    const LoadedTtfFace* primary = nullptr;
    f32 stb_scale = 0.0f;
    bool faux_bold = false;
    bool faux_italic = false;

    i32 ascent = 9;
    i32 descent = 2;
    i32 line_height = 13;

    std::unordered_map<u32, GlyphEntry> glyphs;
};

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
    // Shear top rows to the right relative to baseline (y_rel = yoff + y).
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
#endif

} // namespace

struct MessageDialog::Impl {
    GlGraphicsDevice* device = nullptr;
    std::vector<AtlasPage> atlas_pages;
    std::unordered_map<FontBlobCacheKey, std::unique_ptr<LoadedTtfFace>, FontBlobCacheHash> primary_blobs;
    std::unordered_map<u8, std::vector<std::unique_ptr<LoadedTtfFace>>> fallback_blobs;
    std::unordered_map<FaceKey, std::unique_ptr<CachedFontFace>, FaceKeyHash> faces;
    std::vector<DialogVertex> batch_verts;
    TextureHandle batch_tex{};

    void clear_gpu_atlases(bool delete_textures) {
        if (delete_textures && device) {
            for (auto& page : atlas_pages) {
                if (page.tex.valid()) {
                    device->destroy_texture(page.tex);
                }
            }
        }
        atlas_pages.clear();
        for (auto& [k, face] : faces) {
            (void)k;
            face->glyphs.clear();
        }
    }

    const LoadedTtfFace* get_primary_ttf(std::string_view name, bool bold, bool italic, u8 charset) {
        FontBlobCacheKey key{ascii_to_lower(name), bold, italic, charset};
        if (auto it = primary_blobs.find(key); it != primary_blobs.end()) {
            return it->second->valid ? it->second.get() : nullptr;
        }
        auto entry = std::make_unique<LoadedTtfFace>();
        if (load_system_font(name, bold, italic, charset, entry->blob) && !entry->blob.data.empty()) {
            const i32 offset = stbtt_GetFontOffsetForIndex(entry->blob.data.data(), entry->blob.face_index);
            if (offset >= 0 && stbtt_InitFont(&entry->info, entry->blob.data.data(), offset) != 0) {
                entry->valid = true;
            }
        }
        const LoadedTtfFace* ptr = entry->valid ? entry.get() : nullptr;
        primary_blobs.emplace(std::move(key), std::move(entry));
        return ptr;
    }

    const std::vector<std::unique_ptr<LoadedTtfFace>>& get_fallback_ttfs(u8 charset) {
        if (auto it = fallback_blobs.find(charset); it != fallback_blobs.end()) {
            return it->second;
        }
        std::vector<std::unique_ptr<LoadedTtfFace>> list;
        auto blobs = load_system_fallback_fonts(charset);
        for (auto& b : blobs) {
            if (b.data.empty()) continue;
            auto entry = std::make_unique<LoadedTtfFace>();
            entry->blob = std::move(b);
            const i32 offset = stbtt_GetFontOffsetForIndex(entry->blob.data.data(), entry->blob.face_index);
            if (offset >= 0 && stbtt_InitFont(&entry->info, entry->blob.data.data(), offset) != 0) {
                entry->valid = true;
                list.push_back(std::move(entry));
            }
        }
        auto [it, _] = fallback_blobs.emplace(charset, std::move(list));
        return it->second;
    }

    CachedFontFace& get_face(const MessageFontSpec& spec, f32 raster_scale) {
        const f32 pt = static_cast<f32>(std::clamp(spec.size, 1, 200));
        const f32 logical_em = std::max(5.0f, static_cast<f32>(delphi_round(static_cast<f64>(pt) * 96.0 / 72.0)));
        const f32 em_px = std::clamp(logical_em * raster_scale, 5.0f, 160.0f);
        const i32 em_q4 = static_cast<i32>(std::round(em_px * 4.0f));

        FaceKey key{ascii_to_lower(spec.name), em_q4, spec.bold, spec.italic, spec.charset};
        if (auto it = faces.find(key); it != faces.end()) {
            return *it->second;
        }

        auto face = std::make_unique<CachedFontFace>();
        face->family_name = spec.name;
        face->em_px = static_cast<f32>(em_q4) * 0.25f;
        face->bold = spec.bold;
        face->italic = spec.italic;
        face->charset = spec.charset;

#if defined(__EMSCRIPTEN__)
        if (!web_measure_font_metrics(face->family_name, face->em_px, face->bold, face->italic,
                                      face->ascent, face->descent, face->line_height)) {
            face->ascent = std::max(1, static_cast<i32>(std::ceil(face->em_px * 0.85f)));
            face->descent = std::max(0, static_cast<i32>(std::ceil(face->em_px * 0.22f)));
            face->line_height = std::max(face->ascent + face->descent, static_cast<i32>(std::ceil(face->em_px * 1.18f)));
        }
#else
        face->primary = get_primary_ttf(spec.name, spec.bold, spec.italic, spec.charset);
        if (face->primary) {
            face->stb_scale = stbtt_ScaleForMappingEmToPixels(&face->primary->info, face->em_px);
            int a = 0, d = 0, lg = 0;
            stbtt_GetFontVMetrics(&face->primary->info, &a, &d, &lg);
            face->ascent = std::max(1, static_cast<i32>(std::ceil(static_cast<f32>(a) * face->stb_scale)));
            face->descent = std::max(0, static_cast<i32>(std::ceil(static_cast<f32>(-d) * face->stb_scale)));
            const i32 gap = std::max(0, static_cast<i32>(std::round(static_cast<f32>(lg) * face->stb_scale)));
            face->line_height = std::max(face->ascent + face->descent + gap,
                                         static_cast<i32>(std::round(face->em_px * 1.18f)));
            face->faux_bold = spec.bold && !face->primary->blob.native_bold;
            face->faux_italic = spec.italic && !face->primary->blob.native_italic;
        } else {
            face->ascent = std::max(1, static_cast<i32>(std::ceil(face->em_px * 0.85f)));
            face->descent = std::max(0, static_cast<i32>(std::ceil(face->em_px * 0.20f)));
            face->line_height = std::max(face->ascent + face->descent, static_cast<i32>(std::ceil(face->em_px * 1.18f)));
            face->faux_bold = spec.bold;
            face->faux_italic = spec.italic;
        }
#endif

        CachedFontFace& ref = *face;
        faces.emplace(std::move(key), std::move(face));
        return ref;
    }

    bool pack_glyph_into_atlas(i32 gw, i32 gh, const u8* alpha, GlyphEntry& entry) {
        if (!device || gw <= 0 || gh <= 0 || !alpha) {
            entry.atlas_page = -1;
            return true;
        }
        if (gw > kAtlasSize - 2 || gh > kAtlasSize - 2) {
            entry.atlas_page = -1;
            return false;
        }

        auto try_alloc = [&](AtlasPage& page, i32& out_x, i32& out_y) -> bool {
            if (page.cursor_x + gw + 1 > kAtlasSize) {
                page.cursor_x = 1;
                page.cursor_y += page.row_h + 1;
                page.row_h = 0;
            }
            if (page.cursor_y + gh + 1 > kAtlasSize) {
                return false;
            }
            out_x = page.cursor_x;
            out_y = page.cursor_y;
            page.cursor_x += gw + 1;
            page.row_h = std::max(page.row_h, gh);
            return true;
        };

        i32 px = 0, py = 0;
        i32 page_idx = -1;
        for (usize i = 0; i < atlas_pages.size(); ++i) {
            if (try_alloc(atlas_pages[i], px, py)) {
                page_idx = static_cast<i32>(i);
                break;
            }
        }
        if (page_idx < 0) {
            AtlasPage new_page{};
            TextureDesc desc{};
            desc.width = kAtlasSize;
            desc.height = kAtlasSize;
            desc.format = TextureFormat::RGBA8;
            desc.min_filter = TextureFilter::Linear;
            desc.mag_filter = TextureFilter::Linear;
            std::vector<u8> zeros(static_cast<usize>(kAtlasSize) * static_cast<usize>(kAtlasSize) * 4u, 0u);
            auto tex_res = device->create_texture(desc, zeros);
            if (!tex_res) return false;
            new_page.tex = tex_res.value();
            if (!try_alloc(new_page, px, py)) return false;
            page_idx = static_cast<i32>(atlas_pages.size());
            atlas_pages.push_back(new_page);
        }

        std::vector<u8> rgba(static_cast<usize>(gw) * static_cast<usize>(gh) * 4u);
        for (i32 i = 0; i < gw * gh; ++i) {
            rgba[static_cast<usize>(i) * 4u + 0] = 255u;
            rgba[static_cast<usize>(i) * 4u + 1] = 255u;
            rgba[static_cast<usize>(i) * 4u + 2] = 255u;
            rgba[static_cast<usize>(i) * 4u + 3] = alpha[i];
        }
        device->update_texture(atlas_pages[static_cast<usize>(page_idx)].tex, px, py, gw, gh,
                               TextureFormat::RGBA8, rgba);

        const f32 inv = 1.0f / static_cast<f32>(kAtlasSize);
        entry.atlas_page = page_idx;
        entry.u0 = static_cast<f32>(px) * inv;
        entry.v0 = static_cast<f32>(py) * inv;
        entry.u1 = static_cast<f32>(px + gw) * inv;
        entry.v1 = static_cast<f32>(py + gh) * inv;
        return true;
    }

    const GlyphEntry& get_glyph(CachedFontFace& face, u32 cp, f32 raster_scale) {
        if (auto it = face.glyphs.find(cp); it != face.glyphs.end()) {
            return it->second;
        }

        GlyphEntry entry{};
        std::vector<u8> alpha;
        bool rasterized = false;

#if defined(__EMSCRIPTEN__)
        WebGlyphBitmap web_bmp{};
        if (web_rasterize_glyph(face.family_name, face.em_px, face.bold, face.italic, cp, web_bmp)) {
            entry.w = web_bmp.width;
            entry.h = web_bmp.height;
            entry.xoff = web_bmp.xoff;
            entry.yoff = web_bmp.yoff;
            entry.advance = web_bmp.advance;
            alpha = std::move(web_bmp.alpha);
            rasterized = true;
        }
#else
        const LoadedTtfFace* ttf = nullptr;
        int glyph_idx = 0;
        if (face.primary) {
            glyph_idx = stbtt_FindGlyphIndex(&face.primary->info, static_cast<int>(cp));
            if (glyph_idx != 0) {
                ttf = face.primary;
            }
        }
        if (!ttf && cp >= 32u) {
            const auto& fallbacks = get_fallback_ttfs(face.charset);
            for (const auto& fb : fallbacks) {
                if (!fb || !fb->valid) continue;
                const int idx = stbtt_FindGlyphIndex(&fb->info, static_cast<int>(cp));
                if (idx != 0) {
                    ttf = fb.get();
                    glyph_idx = idx;
                    break;
                }
            }
        }

        if (ttf && glyph_idx != 0) {
            const f32 sc = (ttf == face.primary)
                               ? face.stb_scale
                               : stbtt_ScaleForMappingEmToPixels(&ttf->info, face.em_px);
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&ttf->info, glyph_idx, &adv, &lsb);
            entry.advance = static_cast<f32>(adv) * sc;

            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&ttf->info, glyph_idx, sc, sc, &x0, &y0, &x1, &y1);
            const i32 gw = x1 - x0;
            const i32 gh = y1 - y0;
            entry.w = gw;
            entry.h = gh;
            entry.xoff = x0;
            entry.yoff = y0;
            if (gw > 0 && gh > 0) {
                alpha.assign(static_cast<usize>(gw) * static_cast<usize>(gh), 0u);
                stbtt_MakeGlyphBitmap(&ttf->info, alpha.data(), gw, gh, gw, sc, sc, glyph_idx);
                const bool need_bold = face.bold && !ttf->blob.native_bold;
                const bool need_italic = face.italic && !ttf->blob.native_italic;
                if (need_bold) {
                    const i32 extra = std::max(1, static_cast<i32>(std::round(raster_scale)));
                    apply_faux_bold(alpha, entry.w, entry.h, extra);
                    entry.advance += static_cast<f32>(extra);
                }
                if (need_italic) {
                    apply_faux_italic(alpha, entry.w, entry.h, entry.xoff, entry.yoff);
                }
            }
            rasterized = true;
        }
#endif

        if (!rasterized) {
            // Universal last-resort fallback using kBuiltinFont8x8 (guarantees readability
            // even on minimal systems without installed TrueType fonts).
            const u32 ascii = (cp >= 32u && cp <= 126u) ? cp : static_cast<u32>('?');
            const i32 scale_px = std::max(1, static_cast<i32>(std::round(face.em_px / 9.0f)));
            const i32 gw = 8 * scale_px;
            const i32 gh = 8 * scale_px;
            entry.w = gw;
            entry.h = gh;
            entry.xoff = 0;
            entry.yoff = -gh + std::max(1, scale_px);
            entry.advance = static_cast<f32>(gw);
            alpha.assign(static_cast<usize>(gw) * static_cast<usize>(gh), 0u);
            const auto& bits = kBuiltinFont8x8[ascii - 32u];
            for (i32 row = 0; row < 8; ++row) {
                for (i32 col = 0; col < 8; ++col) {
                    if ((bits[row] & (1u << col)) != 0) {
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
            if (face.bold) {
                const i32 extra = std::max(1, scale_px / 2);
                apply_faux_bold(alpha, entry.w, entry.h, extra);
                entry.advance += static_cast<f32>(extra);
            }
        }

        if (!alpha.empty() && entry.w > 0 && entry.h > 0) {
            (void)pack_glyph_into_atlas(entry.w, entry.h, alpha.data(), entry);
        }

        auto [it, _] = face.glyphs.emplace(cp, entry);
        return it->second;
    }

    f32 measure_cps(CachedFontFace& face, std::span<const u32> cps, f32 raster_scale) {
        f32 w = 0.0f;
        for (u32 cp : cps) {
            w += get_glyph(face, cp, raster_scale).advance;
        }
        return w / raster_scale;
    }
};

MessageDialog::MessageDialog()
    : impl_(std::make_unique<Impl>()) {}

MessageDialog::~MessageDialog() {
    shutdown();
}

void MessageDialog::create_default_assets(const GameData& game) {
    // 1. Default background: decode from game's TMessageForm DFM JPEG if available.
    bg_bitmap_ = Bitmap32{};
    if (!game.message_background_image.empty()) {
        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = stbi_load_from_memory(
            game.message_background_image.data(),
            static_cast<int>(game.message_background_image.size()),
            &w, &h, &comp, 4);
        if (pixels && w > 0 && h > 0) {
            bg_bitmap_.width = w;
            bg_bitmap_.height = h;
            bg_bitmap_.bgra.resize(static_cast<usize>(w) * static_cast<usize>(h) * 4u);
            for (usize i = 0; i < static_cast<usize>(w) * static_cast<usize>(h); ++i) {
                bg_bitmap_.bgra[i * 4u + 0] = pixels[i * 4u + 2];
                bg_bitmap_.bgra[i * 4u + 1] = pixels[i * 4u + 1];
                bg_bitmap_.bgra[i * 4u + 2] = pixels[i * 4u + 0];
                bg_bitmap_.bgra[i * 4u + 3] = 255u;
            }
            stbi_image_free(pixels);
        }
    }
    if (bg_bitmap_.width <= 0 || bg_bitmap_.height <= 0) {
        bg_bitmap_ = make_procedural_bg_bitmap();
    }

    // 2. Default buttons: copy from game's TMessageForm TheImages if available.
    bool have_buttons = true;
    for (usize i = 0; i < 3; ++i) {
        if (game.message_buttons[i].width <= 0 || game.message_buttons[i].height <= 0 ||
            game.message_buttons[i].bgra.empty()) {
            have_buttons = false;
            break;
        }
    }
    if (have_buttons) {
        button_bitmaps_ = game.message_buttons;
    } else {
        for (i32 i = 0; i < 3; ++i) {
            button_bitmaps_[static_cast<usize>(i)] = make_procedural_button_bitmap(i);
        }
    }
}

Result<void> MessageDialog::create_gpu_pipeline() {
    if (!device_) return Error{"MessageDialog: null graphics device"};

    const bool is_es = device_->caps().is_gles;
    const std::string vs_hdr = is_es
        ? "#version 300 es\nprecision highp float;\n"
        : "#version 330 core\n";
    const std::string fs_hdr = is_es
        ? "#version 300 es\nprecision mediump float;\n"
        : "#version 330 core\n";

    const std::string vs_src = vs_hdr + R"(
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
uniform mat4 u_proj;
out vec2 v_uv;
out vec4 v_col;
void main() {
    v_uv = a_uv;
    v_col = a_col;
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
}
)";
    const std::string fs_src = fs_hdr + R"(
in vec2 v_uv;
in vec4 v_col;
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    vec4 t = texture(u_tex, v_uv);
    o_color = vec4(t.rgb * v_col.rgb, t.a * v_col.a);
}
)";

    auto sh_res = device_->create_shader(vs_src, fs_src);
    if (!sh_res) return sh_res.error();
    shader_ = sh_res.value();
    loc_proj_ = glGetUniformLocation(shader_.id, "u_proj");
    loc_tex_ = glGetUniformLocation(shader_.id, "u_tex");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(DialogVertex),
                          reinterpret_cast<const void*>(offsetof(DialogVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(DialogVertex),
                          reinterpret_cast<const void*>(offsetof(DialogVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(DialogVertex),
                          reinterpret_cast<const void*>(offsetof(DialogVertex, r)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    const std::array<u8, 4> white_px = {255u, 255u, 255u, 255u};
    TextureDesc wdesc{};
    wdesc.width = 1;
    wdesc.height = 1;
    wdesc.format = TextureFormat::RGBA8;
    wdesc.min_filter = TextureFilter::Nearest;
    wdesc.mag_filter = TextureFilter::Nearest;
    auto wtex_res = device_->create_texture(wdesc, white_px);
    if (!wtex_res) return wtex_res.error();
    white_tex_ = wtex_res.value();

    upload_bg_texture();
    upload_button_textures();
    return Result<void>::success();
}

void MessageDialog::destroy_gpu_resources(bool delete_gl_objects) noexcept {
    if (impl_) {
        impl_->clear_gpu_atlases(delete_gl_objects);
    }
    if (delete_gl_objects && device_) {
        if (bg_tex_.valid()) device_->destroy_texture(bg_tex_);
        for (auto& t : button_tex_) {
            if (t.valid()) device_->destroy_texture(t);
        }
        if (white_tex_.valid()) device_->destroy_texture(white_tex_);
        if (shader_.valid()) device_->destroy_shader(shader_);
        if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
        if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    }
    bg_tex_ = {};
    button_tex_ = {};
    white_tex_ = {};
    shader_ = {};
    vao_ = 0;
    vbo_ = 0;
    loc_proj_ = -1;
    loc_tex_ = -1;
}

void MessageDialog::upload_bg_texture() {
    if (!device_ || bg_bitmap_.width <= 0 || bg_bitmap_.height <= 0 || bg_bitmap_.bgra.empty()) return;
    if (bg_tex_.valid()) {
        device_->destroy_texture(bg_tex_);
        bg_tex_ = {};
    }
    TextureDesc desc{};
    desc.width = bg_bitmap_.width;
    desc.height = bg_bitmap_.height;
    desc.format = TextureFormat::BGRA8;
    desc.min_filter = TextureFilter::Linear;
    desc.mag_filter = TextureFilter::Linear;
    if (auto res = device_->create_texture(desc, bg_bitmap_.bgra); res) {
        bg_tex_ = res.value();
    }
}

void MessageDialog::upload_button_textures() {
    if (!device_) return;
    for (usize i = 0; i < 3; ++i) {
        if (button_tex_[i].valid()) {
            device_->destroy_texture(button_tex_[i]);
            button_tex_[i] = {};
        }
        const auto& bmp = button_bitmaps_[i];
        if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgra.empty()) continue;
        TextureDesc desc{};
        desc.width = bmp.width;
        desc.height = bmp.height;
        desc.format = TextureFormat::BGRA8;
        desc.min_filter = TextureFilter::Nearest;
        desc.mag_filter = TextureFilter::Nearest;
        if (auto res = device_->create_texture(desc, bmp.bgra); res) {
            button_tex_[i] = res.value();
        }
    }
}

Result<void> MessageDialog::init(GlGraphicsDevice& device, const GameData& game) {
    shutdown();
    device_ = &device;
    impl_->device = &device;
    style_ = MessageDialogStyle{};
    create_default_assets(game);
    if (auto res = create_gpu_pipeline(); !res) {
        return res;
    }
    initialized_ = true;
    return Result<void>::success();
}

void MessageDialog::shutdown() noexcept {
    if (!initialized_) return;
    destroy_gpu_resources(true);
    device_ = nullptr;
    if (impl_) impl_->device = nullptr;
    initialized_ = false;
}

Result<void> MessageDialog::reload_gpu_resources() {
    if (!device_) return Result<void>::success();
    destroy_gpu_resources(false);
    return create_gpu_pipeline();
}

// ---------------------------------------------------------------------------
// GM8 message_* Style Setters
// ---------------------------------------------------------------------------

void MessageDialog::set_background(const Bitmap32& bmp) {
    if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgra.empty()) return;
    bg_bitmap_ = process_gm_message_bitmap(bmp, true);
    if (initialized_) {
        upload_bg_texture();
    }
}

void MessageDialog::set_alpha(f64 alpha) noexcept {
    const i32 a = std::clamp(static_cast<i32>(delphi_round(alpha * 255.0)), 0, 255);
    style_.alpha = static_cast<f32>(a) / 255.0f;
}

void MessageDialog::set_button(const SpriteData& spr) {
    if (!spr.exists || spr.images.empty()) return;
    const Bitmap32& img0 = spr.images[0];
    if (img0.width <= 0 || img0.height <= 0 || img0.bgra.empty()) return;

    const Bitmap32& img1 = (spr.images.size() > 1 && !spr.images[1].bgra.empty()) ? spr.images[1] : img0;
    const Bitmap32& img2 = (spr.images.size() > 2 && !spr.images[2].bgra.empty()) ? spr.images[2] : img1;

    // GM8 checks subimage 0's bottom-left pixel alpha to determine ImageList.Masked.
    bool masked = false;
    button_bitmaps_[0] = process_gm_message_bitmap(img0, true, &masked);
    button_bitmaps_[1] = process_gm_message_bitmap(img1, masked);
    button_bitmaps_[2] = process_gm_message_bitmap(img2, masked);

    if (initialized_) {
        upload_button_textures();
    }
}

namespace {

void apply_font_spec_update(MessageFontSpec& dst, std::string_view name, f64 size, f64 color, i32 style) {
    if (!name.empty()) {
        dst.name = std::string(name);
    }
    if (size >= 0.0) {
        dst.size = std::max(1, static_cast<i32>(delphi_round(size)));
    }
    if (color >= 0.0) {
        dst.color_bgr = static_cast<u32>(static_cast<i64>(delphi_round(color))) & 0xFFFFFFu;
    }
    if (style >= 0 && style <= 3) {
        dst.bold = (style & 1) != 0;
        dst.italic = (style & 2) != 0;
    }
}

} // namespace

void MessageDialog::set_text_font(std::string_view name, f64 size, f64 color, i32 style) {
    apply_font_spec_update(style_.text_font, name, size, color, style);
}

void MessageDialog::set_button_font(std::string_view name, f64 size, f64 color, i32 style) {
    apply_font_spec_update(style_.button_font, name, size, color, style);
}

void MessageDialog::set_input_font(std::string_view name, f64 size, f64 color, i32 style) {
    apply_font_spec_update(style_.input_font, name, size, color, style);
}

void MessageDialog::set_charset(i32 type, u8 charset) noexcept {
    if (type == 0) style_.text_font.charset = charset;
    else if (type == 1) style_.button_font.charset = charset;
    else if (type == 2) style_.input_font.charset = charset;
}

void MessageDialog::set_caption(bool show, std::string_view caption) {
    style_.show_caption = show;
    style_.caption = std::string(caption);
}

// ---------------------------------------------------------------------------
// Dialog Session Setup & Accelerator Parsing
// ---------------------------------------------------------------------------

namespace {

void parse_button_caption(std::string_view utf8,
                          bool& out_visible,
                          std::string& out_raw,
                          std::vector<u32>& out_cps,
                          i32& out_underline_idx,
                          u32& out_shortcut_upper) {
    out_raw = std::string(utf8);
    out_visible = !utf8.empty();
    out_cps.clear();
    out_underline_idx = -1;
    out_shortcut_upper = 0;
    if (!out_visible) return;

    const auto raw_cps = utf8_to_codepoints(utf8);
    out_cps.reserve(raw_cps.size());
    for (usize i = 0; i < raw_cps.size(); ++i) {
        if (raw_cps[i] == '&') {
            if (i + 1 < raw_cps.size()) {
                if (raw_cps[i + 1] == '&') {
                    out_cps.push_back('&');
                    ++i;
                } else {
                    const u32 ch = raw_cps[i + 1];
                    out_underline_idx = static_cast<i32>(out_cps.size());
                    out_shortcut_upper = ascii_upper_cp(ch);
                    out_cps.push_back(ch);
                    ++i;
                }
            }
        } else {
            out_cps.push_back(raw_cps[i]);
        }
    }
}

} // namespace

void MessageDialog::begin_message(
    std::string_view text_utf8,
    std::string_view but1_utf8,
    std::string_view but2_utf8,
    std::string_view but3_utf8) {
    active_ = true;
    with_input_ = false;
    message_text_ = expand_gm_message_newlines(text_utf8);
    result_button_ = 0;
    pressed_button_idx_ = -1;
    close_hovered_ = false;
    close_pressed_ = false;
    dragging_ = false;
    has_custom_drag_pos_ = false;
    input_cps_.clear();
    caret_ = 0;
    sel_anchor_ = 0;
    input_scroll_x_ = 0.0f;
    input_selecting_ = false;
    request_text_input_ = false;
    ime_text_.clear();
    ime_start_ = 0;
    ime_length_ = 0;

    const std::array<std::string_view, 3> labels = {but1_utf8, but2_utf8, but3_utf8};
    for (usize i = 0; i < 3; ++i) {
        buttons_[i] = ParsedButton{};
        parse_button_caption(labels[i],
                             buttons_[i].visible,
                             buttons_[i].raw_text,
                             buttons_[i].display_cps,
                             buttons_[i].underline_cp_idx,
                             buttons_[i].shortcut_upper);
    }
}

void MessageDialog::begin_input(std::string_view text_utf8, std::string_view default_utf8) {
    begin_message(text_utf8, "", "OK", "");
    with_input_ = true;
    input_cps_ = utf8_to_codepoints(default_utf8);
    // Strip control newlines if any were passed in default_utf8.
    input_cps_.erase(std::remove_if(input_cps_.begin(), input_cps_.end(),
                                    [](u32 cp) { return cp == '\r' || cp == '\n'; }),
                     input_cps_.end());
    // Delphi TMessageForm calls TextEdit.SelectAll before ShowModal.
    sel_anchor_ = 0;
    caret_ = input_cps_.size();
    input_scroll_x_ = 0.0f;
    caret_reset_us_ = 0;
}

#if defined(__EMSCRIPTEN__)
void MessageDialog::begin_web_editor() {
    web_begin_dialog_editor(result_text_utf8(), message_text_);
}

void MessageDialog::poll_web_editor(ModalInputFrame& input) {
    int anchor, caret, action;
    std::string value;
    if (!web_poll_dialog_editor(value, anchor, caret, action)) return;
    input_cps_ = utf8_to_codepoints(value);
    sel_anchor_ = std::min(static_cast<usize>(anchor), input_cps_.size());
    caret_ = std::min(static_cast<usize>(caret), input_cps_.size());
    caret_reset_us_ = 0;
    if (action) input.keyboard_events.emplace_back(ModalKeyEvent{static_cast<u8>(action)});
}
#endif

std::string MessageDialog::result_text_utf8() const {
    return codepoints_to_utf8(input_cps_);
}

// ---------------------------------------------------------------------------
// Layout Calculation (faithful to TMessageForm.CalculatePositions at 0x5B5B30)
// ---------------------------------------------------------------------------

MessageDialog::Layout MessageDialog::compute_layout(
    i32 win_w, i32 win_h,
    i32 drawable_w, i32 drawable_h,
    i32 win_x, i32 win_y,
    f32 display_scale,
    bool mobile_touch) {
    Layout L{};
    const i32 dw = std::max(1, drawable_w);
    const i32 dh = std::max(1, drawable_h);
    const i32 ww = std::max(1, win_w);
    const i32 wh = std::max(1, win_h);
    L.win_to_phys_x = static_cast<f32>(dw) / static_cast<f32>(ww);
    L.win_to_phys_y = static_cast<f32>(dh) / static_cast<f32>(wh);

    f32 base_scale = std::max(1.0f, L.win_to_phys_x);
    if (mobile_touch) {
        const f32 touch_min = static_cast<f32>(std::min(dw, dh)) / 460.0f;
        base_scale = std::max({base_scale, display_scale, touch_min});
    }

    // Natural logical client width & button size.
    const f32 bg_w = static_cast<f32>(std::max(1, bg_bitmap_.width));
    const f32 btn_w = static_cast<f32>(std::max(1, button_bitmaps_[0].width));
    const f32 btn_h = static_cast<f32>(std::max(1, button_bitmaps_[0].height));
    const f32 client_w = (style_.size_w > 0) ? static_cast<f32>(style_.size_w) : bg_w;
    const f32 max_text_w = std::max(1.0f, client_w - 48.0f);

    // Measure text wrap at quantized raster scale.
    f32 est_raster = std::clamp(std::round(base_scale * 4.0f) * 0.25f, 1.0f, 4.0f);
    auto& text_face = impl_->get_face(style_.text_font, est_raster);
    auto& btn_face = impl_->get_face(style_.button_font, est_raster);
    auto& input_face = impl_->get_face(style_.input_font, est_raster);

    L.text_line_h = std::max(10.0f, static_cast<f32>(text_face.line_height) / est_raster);
    L.button_line_h = std::max(10.0f, static_cast<f32>(btn_face.line_height) / est_raster);
    L.input_line_h = std::max(10.0f, static_cast<f32>(input_face.line_height) / est_raster);

    // Word-wrap message_text_ into wrapped_lines.
    L.wrapped_lines.clear();
    const auto all_cps = utf8_to_codepoints(message_text_);
    usize pos = 0;
    while (pos <= all_cps.size()) {
        usize nl = pos;
        while (nl < all_cps.size() && all_cps[nl] != '\n') ++nl;
        const std::span<const u32> para(all_cps.data() + pos, nl - pos);

        if (para.empty()) {
            L.wrapped_lines.emplace_back();
        } else {
            std::vector<u32> line;
            f32 line_w = 0.0f;
            usize i = 0;
            while (i < para.size()) {
                // Collect either a run of spaces or a word token.
                const bool is_space = (para[i] == ' ' || para[i] == '\t');
                usize tok_end = i + 1;
                if (is_space) {
                    while (tok_end < para.size() && (para[tok_end] == ' ' || para[tok_end] == '\t')) ++tok_end;
                } else {
                    // Break between CJK/wide glyphs or at whitespace so long strings wrap naturally.
                    if (para[i] >= 0x2E80u) {
                        tok_end = i + 1;
                    } else {
                        while (tok_end < para.size() && para[tok_end] != ' ' &&
                               para[tok_end] != '\t' && para[tok_end] < 0x2E80u) {
                            ++tok_end;
                        }
                    }
                }

                const std::span<const u32> tok(para.data() + i, tok_end - i);
                const f32 tok_w = impl_->measure_cps(text_face, tok, est_raster);

                if (line.empty()) {
                    if (tok_w <= max_text_w || tok.size() == 1) {
                        line.insert(line.end(), tok.begin(), tok.end());
                        line_w = tok_w;
                    } else {
                        // Long unbroken word: split at character boundary.
                        for (u32 cp : tok) {
                            const f32 cw = impl_->get_glyph(text_face, cp, est_raster).advance / est_raster;
                            if (!line.empty() && line_w + cw > max_text_w) {
                                L.wrapped_lines.push_back(std::move(line));
                                line.clear();
                                line_w = 0.0f;
                            }
                            line.push_back(cp);
                            line_w += cw;
                        }
                    }
                } else if (line_w + tok_w <= max_text_w) {
                    line.insert(line.end(), tok.begin(), tok.end());
                    line_w += tok_w;
                } else {
                    L.wrapped_lines.push_back(std::move(line));
                    line.clear();
                    line_w = 0.0f;
                    if (!is_space) {
                        if (tok_w <= max_text_w || tok.size() == 1) {
                            line.insert(line.end(), tok.begin(), tok.end());
                            line_w = tok_w;
                        } else {
                            for (u32 cp : tok) {
                                const f32 cw = impl_->get_glyph(text_face, cp, est_raster).advance / est_raster;
                                if (!line.empty() && line_w + cw > max_text_w) {
                                    L.wrapped_lines.push_back(std::move(line));
                                    line.clear();
                                    line_w = 0.0f;
                                }
                                line.push_back(cp);
                                line_w += cw;
                            }
                        }
                    }
                }
                i = tok_end;
            }
            if (!line.empty()) {
                L.wrapped_lines.push_back(std::move(line));
            }
        }

        if (nl >= all_cps.size()) break;
        pos = nl + 1;
    }
    if (L.wrapped_lines.empty()) {
        L.wrapped_lines.emplace_back();
    }

    const f32 text_h = std::max(L.text_line_h, static_cast<f32>(L.wrapped_lines.size()) * L.text_line_h);
    const f32 input_h = std::max(21.0f, std::ceil(L.input_line_h) + 8.0f);

    f32 client_h = 0.0f;
    if (style_.size_h > 0) {
        client_h = static_cast<f32>(style_.size_h);
    } else {
        client_h = btn_h + text_h + 48.0f;
        if (with_input_) {
            client_h += input_h + 16.0f;
        }
    }

    const f32 border = style_.show_caption ? kDialogFrameBorder : 0.0f;
    const f32 title_h = style_.show_caption ? kTitleBarHeight : 0.0f;
    const f32 outer_w = client_w + border * 2.0f;
    const f32 outer_h = client_h + title_h + border * 2.0f;

    // Clamp scale so the entire dialog fits comfortably inside the drawable area.
    const f32 max_fit_w = (static_cast<f32>(dw) * 0.94f) / std::max(1.0f, outer_w);
    const f32 max_fit_h = (static_cast<f32>(dh) * 0.94f) / std::max(1.0f, outer_h);
    L.scale = std::max(0.25f, std::min({base_scale, max_fit_w, max_fit_h}));
    L.raster_scale = std::clamp(std::round(L.scale * 4.0f) * 0.25f, 1.0f, 4.0f);

    const f32 logical_view_w = static_cast<f32>(dw) / L.scale;
    const f32 logical_view_h = static_cast<f32>(dh) / L.scale;

    // Resolve dialog top-left position in logical view coordinates.
    f32 ox = 0.0f;
    f32 oy = 0.0f;
    if (has_custom_drag_pos_) {
        ox = dragged_x_;
        oy = dragged_y_;
    } else {
        if (style_.pos_x >= 0) {
            f32 px = static_cast<f32>(style_.pos_x);
            if (win_x > 0 && px >= static_cast<f32>(win_x) && px <= static_cast<f32>(win_x + ww)) {
                px -= static_cast<f32>(win_x);
            }
            ox = (px * L.win_to_phys_x) / L.scale;
        } else {
            ox = std::floor((logical_view_w - outer_w) * 0.5f);
        }

        if (style_.pos_y >= 0) {
            f32 py = static_cast<f32>(style_.pos_y);
            if (win_y > 0 && py >= static_cast<f32>(win_y) && py <= static_cast<f32>(win_y + wh)) {
                py -= static_cast<f32>(win_y);
            }
            oy = (py * L.win_to_phys_y) / L.scale;
        } else if (with_input_ && mobile_touch) {
            // Keep input dialog in the upper portion so mobile virtual keyboards do not cover it.
            oy = std::floor(std::max(8.0f, (logical_view_h * 0.52f - outer_h) * 0.5f));
        } else {
            oy = std::floor((logical_view_h - outer_h) * 0.5f);
        }
    }

    ox = std::clamp(ox, 0.0f, std::max(0.0f, logical_view_w - outer_w));
    oy = std::clamp(oy, 0.0f, std::max(0.0f, logical_view_h - outer_h));

    L.outer = RectF{ox, oy, outer_w, outer_h};
    if (style_.show_caption) {
        L.title_bar = RectF{ox + border, oy + border, client_w, title_h};
        L.close_btn = RectF{
            L.title_bar.x + L.title_bar.w - kCloseBtnW - 3.0f,
            L.title_bar.y + std::floor((title_h - kCloseBtnH) * 0.5f),
            kCloseBtnW,
            kCloseBtnH
        };
    }
    L.client = RectF{ox + border, oy + border + title_h, client_w, client_h};

    // Exact TMessageForm.CalculatePositions offsets (margins: 24px H, 16px V).
    L.text_rect = RectF{L.client.x + 24.0f, L.client.y + 16.0f, max_text_w, text_h};

    const f32 btn_y = L.client.y + client_h - 16.0f - btn_h;
    L.btn_rects[0] = RectF{L.client.x + 24.0f, btn_y, btn_w, btn_h};
    L.btn_rects[1] = RectF{L.client.x + std::floor((client_w - btn_w) * 0.5f), btn_y, btn_w, btn_h};
    L.btn_rects[2] = RectF{L.client.x + client_w - btn_w - 24.0f, btn_y, btn_w, btn_h};

    L.input_rect = RectF{L.client.x + 24.0f, btn_y - input_h - 16.0f, max_text_w, input_h};
    return L;
}

// ---------------------------------------------------------------------------
// Text Input Editing Helpers
// ---------------------------------------------------------------------------

void MessageDialog::delete_selection() {
    if (!has_selection()) return;
    const usize lo = std::min(caret_, sel_anchor_);
    const usize hi = std::max(caret_, sel_anchor_);
    input_cps_.erase(input_cps_.begin() + static_cast<std::ptrdiff_t>(lo),
                     input_cps_.begin() + static_cast<std::ptrdiff_t>(hi));
    caret_ = lo;
    sel_anchor_ = lo;
}

void MessageDialog::insert_utf8_at_caret(std::string_view utf8) {
    if (utf8.empty()) return;
    auto cps = utf8_to_codepoints(utf8);
    // Single-line input: strip control characters and newlines.
    cps.erase(std::remove_if(cps.begin(), cps.end(),
                             [](u32 cp) { return cp < 0x20u || cp == 0x7Fu; }),
              cps.end());
    if (cps.empty()) return;
    delete_selection();
    input_cps_.insert(input_cps_.begin() + static_cast<std::ptrdiff_t>(caret_), cps.begin(), cps.end());
    caret_ += cps.size();
    sel_anchor_ = caret_;
}

void MessageDialog::ensure_caret_visible(const Layout& L, f32 raster_scale) {
    auto& face = impl_->get_face(style_.input_font, raster_scale);
    const f32 inner_w = std::max(4.0f, L.input_rect.w - 10.0f);
    const f32 caret_x = impl_->measure_cps(face, std::span<const u32>(input_cps_.data(), caret_), raster_scale);
    const f32 total_w = impl_->measure_cps(face, input_cps_, raster_scale);

    if (total_w <= inner_w) {
        input_scroll_x_ = 0.0f;
        return;
    }
    if (caret_x - input_scroll_x_ > inner_w - 2.0f) {
        input_scroll_x_ = caret_x - (inner_w - 2.0f);
    } else if (caret_x - input_scroll_x_ < 0.0f) {
        input_scroll_x_ = caret_x;
    }
    input_scroll_x_ = std::clamp(input_scroll_x_, 0.0f, std::max(0.0f, total_w - inner_w + 4.0f));
}

usize MessageDialog::hit_test_input_caret(const Layout& L, f32 local_x) {
    auto& face = impl_->get_face(style_.input_font, L.raster_scale);
    const f32 text_x = local_x - (L.input_rect.x + 5.0f) + input_scroll_x_;
    if (text_x <= 0.0f) return 0;

    f32 acc = 0.0f;
    for (usize i = 0; i < input_cps_.size(); ++i) {
        const f32 cw = impl_->get_glyph(face, input_cps_[i], L.raster_scale).advance / L.raster_scale;
        if (text_x < acc + cw * 0.5f) {
            return i;
        }
        acc += cw;
    }
    return input_cps_.size();
}

void MessageDialog::trigger_first_visible_button() noexcept {
    for (i32 i = 0; i < 3; ++i) {
        if (buttons_[static_cast<usize>(i)].visible) {
            result_button_ = i + 1;
            active_ = false;
            return;
        }
    }
}

bool MessageDialog::get_input_window_rect(
    i32 win_w, i32 win_h,
    i32 drawable_w, i32 drawable_h,
    i32 win_x, i32 win_y,
    f32 display_scale,
    bool mobile_touch,
    i32& out_x, i32& out_y, i32& out_w, i32& out_h, i32& out_caret_px) {
    if (!with_input_) return false;
    const Layout L = compute_layout(win_w, win_h, drawable_w, drawable_h, win_x, win_y, display_scale, mobile_touch);
    const f32 to_win_x = L.scale / std::max(0.001f, L.win_to_phys_x);
    const f32 to_win_y = L.scale / std::max(0.001f, L.win_to_phys_y);
    out_x = static_cast<i32>(std::floor(L.input_rect.x * to_win_x));
    out_y = static_cast<i32>(std::floor(L.input_rect.y * to_win_y));
    out_w = std::max(1, static_cast<i32>(std::ceil(L.input_rect.w * to_win_x)));
    out_h = std::max(1, static_cast<i32>(std::ceil(L.input_rect.h * to_win_y)));

    auto& face = impl_->get_face(style_.input_font, L.raster_scale);
    const f32 caret_logical = impl_->measure_cps(face, std::span<const u32>(input_cps_.data(), caret_), L.raster_scale) - input_scroll_x_;
    out_caret_px = std::clamp(static_cast<i32>(std::round((caret_logical + 5.0f) * to_win_x)), 0, out_w);
    return true;
}

// ---------------------------------------------------------------------------
// Modal Input Handling
// ---------------------------------------------------------------------------

bool MessageDialog::update(
    const ModalInputFrame& input,
    i32 win_w, i32 win_h,
    i32 drawable_w, i32 drawable_h,
    i32 win_x, i32 win_y,
    f32 display_scale,
    bool mobile_touch,
    u64 now_us) {
    if (!active_) return false;
    const Layout L = compute_layout(win_w, win_h, drawable_w, drawable_h, win_x, win_y, display_scale, mobile_touch);
    if (caret_reset_us_ == 0) {
        caret_reset_us_ = now_us;
        if (with_input_) ensure_caret_visible(L, L.raster_scale);
    }
    const f32 mx = (input.mouse_x * L.win_to_phys_x) / L.scale;
    const f32 my = (input.mouse_y * L.win_to_phys_y) / L.scale;

    for (const auto& event : input.keyboard_events) {
        if (const auto* composition = std::get_if<ModalComposition>(&event)) {
            if (with_input_) {
                ime_text_ = composition->text;
                ime_start_ = composition->start;
                ime_length_ = composition->length;
                caret_reset_us_ = now_us;
            }
            continue;
        }
        if (const auto* text = std::get_if<std::string>(&event)) {
            if (with_input_) {
                ime_text_.clear();
                insert_utf8_at_caret(*text);
                caret_reset_us_ = now_us;
                ensure_caret_visible(L, L.raster_scale);
            } else {
                // Also check text_input characters against non-ASCII/layout button accelerators.
                const auto cps = utf8_to_codepoints(*text);
                for (u32 cp : cps) {
                    const u32 up = ascii_upper_cp(cp);
                    for (i32 i = 0; i < 3; ++i) {
                        const auto& b = buttons_[static_cast<usize>(i)];
                        if (b.visible && b.shortcut_upper != 0 && b.shortcut_upper == up) {
                            result_button_ = i + 1;
                            active_ = false;
                            return false;
                        }
                    }
                }
            }
            continue;
        }

        const auto& ke = std::get<ModalKeyEvent>(event);
        const bool shortcut_mod = ke.ctrl || ke.gui;

        // Escape always dismisses with result 0.
        if (ke.vk == 27 && !ke.repeat && ime_text_.empty()) {
            result_button_ = 0;
            active_ = false;
            return false;
        }

        // Enter (and Space when no input box is shown) triggers the first visible button.
        if ((ke.vk == 13 || (ke.vk == 32 && !with_input_)) && !ke.repeat) {
            if (ime_text_.empty()) {
                trigger_first_visible_button();
                if (!active_) return false;
            }
            continue;
        }

        // Alt + accelerator shortcut (works both with and without input box).
        if (ke.alt && !ke.ctrl && !ke.repeat && ke.keycode >= 32 && ke.keycode < 0x110000u) {
            const u32 up = ascii_upper_cp(ke.keycode);
            for (i32 i = 0; i < 3; ++i) {
                const auto& b = buttons_[static_cast<usize>(i)];
                if (b.visible && b.shortcut_upper != 0 && b.shortcut_upper == up) {
                    result_button_ = i + 1;
                    active_ = false;
                    return false;
                }
            }
            continue;
        }

        // Unmodified single-key accelerator shortcut when no input box is visible.
        if (!with_input_ && !ke.alt && !ke.ctrl && !ke.gui && !ke.repeat &&
            ke.keycode >= 32 && ke.keycode < 0x110000u) {
            const u32 up = ascii_upper_cp(ke.keycode);
            for (i32 i = 0; i < 3; ++i) {
                const auto& b = buttons_[static_cast<usize>(i)];
                if (b.visible && b.shortcut_upper != 0 && b.shortcut_upper == up) {
                    result_button_ = i + 1;
                    active_ = false;
                    return false;
                }
            }
            continue;
        }

        if (!with_input_) continue;

        // Input field editing keys (supporting key repeat!).
        if (shortcut_mod && !ke.alt) {
            if (ke.vk == 'A') {
                sel_anchor_ = 0;
                caret_ = input_cps_.size();
                caret_reset_us_ = now_us;
                ensure_caret_visible(L, L.raster_scale);
                continue;
            }
            if (ke.vk == 'C' || ke.vk == 'X') {
                if (has_selection()) {
                    const usize lo = std::min(caret_, sel_anchor_);
                    const usize hi = std::max(caret_, sel_anchor_);
                    PlatformWindow::clipboard_set_text(
                        codepoints_to_utf8(std::span<const u32>(input_cps_.data() + lo, hi - lo)));
                    if (ke.vk == 'X') {
                        delete_selection();
                        ensure_caret_visible(L, L.raster_scale);
                    }
                    caret_reset_us_ = now_us;
                }
                continue;
            }
            if (ke.vk == 'V') {
                std::string clip = PlatformWindow::clipboard_get_text();
                // Single-line TEdit truncates paste at first newline.
                if (const usize nl = clip.find_first_of("\r\n"); nl != std::string::npos) {
                    clip.resize(nl);
                }
                insert_utf8_at_caret(clip);
                caret_reset_us_ = now_us;
                ensure_caret_visible(L, L.raster_scale);
                continue;
            }
        }

        if (ke.vk == 8) { // Backspace
            if (has_selection()) {
                delete_selection();
            } else if (caret_ > 0) {
                usize del_from = caret_ - 1;
                if (shortcut_mod) {
                    while (del_from > 0 && !is_word_char(input_cps_[del_from])) --del_from;
                    while (del_from > 0 && is_word_char(input_cps_[del_from - 1])) --del_from;
                }
                input_cps_.erase(input_cps_.begin() + static_cast<std::ptrdiff_t>(del_from),
                                 input_cps_.begin() + static_cast<std::ptrdiff_t>(caret_));
                caret_ = del_from;
                sel_anchor_ = caret_;
            }
            caret_reset_us_ = now_us;
            ensure_caret_visible(L, L.raster_scale);
        } else if (ke.vk == 46) { // Delete
            if (has_selection()) {
                delete_selection();
            } else if (caret_ < input_cps_.size()) {
                usize del_to = caret_ + 1;
                if (shortcut_mod) {
                    while (del_to < input_cps_.size() && !is_word_char(input_cps_[del_to])) ++del_to;
                    while (del_to < input_cps_.size() && is_word_char(input_cps_[del_to])) ++del_to;
                }
                input_cps_.erase(input_cps_.begin() + static_cast<std::ptrdiff_t>(caret_),
                                 input_cps_.begin() + static_cast<std::ptrdiff_t>(del_to));
            }
            caret_reset_us_ = now_us;
            ensure_caret_visible(L, L.raster_scale);
        } else if (ke.vk == 37) { // Left
            if (!ke.shift && has_selection()) {
                caret_ = std::min(caret_, sel_anchor_);
                sel_anchor_ = caret_;
            } else if (caret_ > 0) {
                if (shortcut_mod) {
                    while (caret_ > 0 && !is_word_char(input_cps_[caret_ - 1])) --caret_;
                    while (caret_ > 0 && is_word_char(input_cps_[caret_ - 1])) --caret_;
                } else {
                    --caret_;
                }
                if (!ke.shift) sel_anchor_ = caret_;
            }
            caret_reset_us_ = now_us;
            ensure_caret_visible(L, L.raster_scale);
        } else if (ke.vk == 39) { // Right
            if (!ke.shift && has_selection()) {
                caret_ = std::max(caret_, sel_anchor_);
                sel_anchor_ = caret_;
            } else if (caret_ < input_cps_.size()) {
                if (shortcut_mod) {
                    while (caret_ < input_cps_.size() && !is_word_char(input_cps_[caret_])) ++caret_;
                    while (caret_ < input_cps_.size() && is_word_char(input_cps_[caret_])) ++caret_;
                } else {
                    ++caret_;
                }
                if (!ke.shift) sel_anchor_ = caret_;
            }
            caret_reset_us_ = now_us;
            ensure_caret_visible(L, L.raster_scale);
        } else if (ke.vk == 36) { // Home
            caret_ = 0;
            if (!ke.shift) sel_anchor_ = caret_;
            caret_reset_us_ = now_us;
            ensure_caret_visible(L, L.raster_scale);
        } else if (ke.vk == 35) { // End
            caret_ = input_cps_.size();
            if (!ke.shift) sel_anchor_ = caret_;
            caret_reset_us_ = now_us;
            ensure_caret_visible(L, L.raster_scale);
        }
    }

    // 4. Pointer (mouse / touch) interactions
    const bool show_hover = !input.pointer_is_touch || input.left_down;
    close_hovered_ = style_.show_caption && show_hover && L.close_btn.contains(mx, my);
    for (usize i = 0; i < 3; ++i) {
        buttons_[i].hovered = buttons_[i].visible && show_hover && L.btn_rects[i].contains(mx, my);
    }

    if (input.left_pressed) {
        pressed_button_idx_ = -1;
        close_pressed_ = false;
        dragging_ = false;
        input_selecting_ = false;

        if (style_.show_caption && L.close_btn.contains(mx, my)) {
            close_pressed_ = true;
        } else if (style_.show_caption && L.title_bar.contains(mx, my)) {
            dragging_ = true;
            drag_offset_x_ = mx - L.outer.x;
            drag_offset_y_ = my - L.outer.y;
            has_custom_drag_pos_ = true;
            dragged_x_ = L.outer.x;
            dragged_y_ = L.outer.y;
        } else if (with_input_ && L.input_rect.contains(mx, my)) {
            request_text_input_ = true;
            if (input.left_double_click) {
                sel_anchor_ = 0;
                caret_ = input_cps_.size();
                input_selecting_ = false;
            } else {
                caret_ = hit_test_input_caret(L, mx);
                sel_anchor_ = caret_;
                input_selecting_ = true;
            }
            caret_reset_us_ = now_us;
            ensure_caret_visible(L, L.raster_scale);
        } else {
            for (i32 i = 0; i < 3; ++i) {
                if (buttons_[static_cast<usize>(i)].visible && L.btn_rects[static_cast<usize>(i)].contains(mx, my)) {
                    pressed_button_idx_ = i;
                    break;
                }
            }
        }
    }

    if (input.left_down) {
        if (dragging_) {
            const f32 logical_view_w = static_cast<f32>(std::max(1, drawable_w)) / L.scale;
            const f32 logical_view_h = static_cast<f32>(std::max(1, drawable_h)) / L.scale;
            dragged_x_ = std::clamp(mx - drag_offset_x_, 0.0f, std::max(0.0f, logical_view_w - L.outer.w));
            dragged_y_ = std::clamp(my - drag_offset_y_, 0.0f, std::max(0.0f, logical_view_h - L.outer.h));
        } else if (input_selecting_ && with_input_) {
            caret_ = hit_test_input_caret(L, mx);
            caret_reset_us_ = now_us;
            ensure_caret_visible(L, L.raster_scale);
        }
    }

    for (i32 i = 0; i < 3; ++i) {
        buttons_[static_cast<usize>(i)].pressed =
            (pressed_button_idx_ == i) && input.left_down && buttons_[static_cast<usize>(i)].hovered;
    }

    if (input.left_released) {
        if (close_pressed_ && style_.show_caption && L.close_btn.contains(mx, my)) {
            result_button_ = 0;
            active_ = false;
            close_pressed_ = false;
            return false;
        }
        if (pressed_button_idx_ >= 0 && pressed_button_idx_ < 3 &&
            L.btn_rects[static_cast<usize>(pressed_button_idx_)].contains(mx, my)) {
            result_button_ = pressed_button_idx_ + 1;
            active_ = false;
            pressed_button_idx_ = -1;
            return false;
        }
        close_pressed_ = false;
        pressed_button_idx_ = -1;
        dragging_ = false;
        input_selecting_ = false;
        if (input.pointer_is_touch) {
            close_hovered_ = false;
            for (auto& b : buttons_) {
                b.hovered = false;
                b.pressed = false;
            }
        }
    }

    return active_;
}

// ---------------------------------------------------------------------------
// Self-Drawn OpenGL Renderer
// ---------------------------------------------------------------------------

void MessageDialog::render(
    i32 win_w, i32 win_h,
    i32 drawable_w, i32 drawable_h,
    i32 win_x, i32 win_y,
    f32 display_scale,
    bool mobile_touch,
    u64 now_us) {
    if (!initialized_ || !active_ || !device_ || drawable_w <= 0 || drawable_h <= 0) {
        return;
    }

    const Layout L = compute_layout(win_w, win_h, drawable_w, drawable_h, win_x, win_y, display_scale, mobile_touch);
    const f32 S = L.scale;
    const f32 dlg_alpha = style_.alpha;
    if (dlg_alpha <= 0.0f) return;

    const auto saved_state = device_->save_render_state();

    device_->bind_framebuffer(FramebufferHandle{});
    device_->set_viewport(ViewportRect{0, 0, drawable_w, drawable_h});
    device_->set_scissor(false);
    device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    device_->set_cull_mode(CullMode::None);
    device_->set_polygon_mode(2);
    device_->set_color_mask(true, true, true, true);
    device_->set_blend_state(BlendState{
        true,
        BlendFactor::SrcAlpha, BlendFactor::InvSrcAlpha,
        BlendFactor::One, BlendFactor::InvSrcAlpha,
        BlendOp::Add, BlendOp::Add
    });
    device_->bind_shader(shader_);

    // Orthographic projection in physical drawable pixels (0..drawable_w, 0..drawable_h top-down).
    const f32 dw_f = static_cast<f32>(drawable_w);
    const f32 dh_f = static_cast<f32>(drawable_h);
    const f32 proj[16] = {
        2.0f / dw_f, 0.0f,         0.0f, 0.0f,
        0.0f,       -2.0f / dh_f,  0.0f, 0.0f,
        0.0f,        0.0f,        -1.0f, 0.0f,
        -1.0f,       1.0f,         0.0f, 1.0f
    };
    if (loc_proj_ >= 0) glUniformMatrix4fv(loc_proj_, 1, GL_FALSE, proj);
    if (loc_tex_ >= 0) glUniform1i(loc_tex_, 0);

    auto& verts = impl_->batch_verts;
    verts.clear();
    impl_->batch_tex = {};

    auto flush_batch = [&]() {
        if (verts.empty() || !impl_->batch_tex.valid()) {
            verts.clear();
            return;
        }
        device_->bind_texture(0, impl_->batch_tex);
        const bool is_int_scale = std::fabs(S - std::round(S)) < 0.01f;
        const bool is_btn_tex = (impl_->batch_tex == button_tex_[0] ||
                                 impl_->batch_tex == button_tex_[1] ||
                                 impl_->batch_tex == button_tex_[2]);
        const bool is_nearest = (impl_->batch_tex == white_tex_) || (is_btn_tex && is_int_scale);
        device_->set_texture_sampler(0, is_nearest ? TextureFilter::Nearest : TextureFilter::Linear,
                                     TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(verts.size() * sizeof(DialogVertex)),
                     verts.data(),
                     GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        verts.clear();
    };

    auto push_quad_grad = [&](TextureHandle tex,
                              f32 x0, f32 y0, f32 x1, f32 y1,
                              f32 u0, f32 v0, f32 u1, f32 v1,
                              ColorF c_left, ColorF c_right) {
        if (!tex.valid()) return;
        if (impl_->batch_tex != tex && !verts.empty()) {
            flush_batch();
        }
        impl_->batch_tex = tex;
        verts.push_back(DialogVertex{x0, y0, u0, v0, c_left.r, c_left.g, c_left.b, c_left.a});
        verts.push_back(DialogVertex{x1, y0, u1, v0, c_right.r, c_right.g, c_right.b, c_right.a});
        verts.push_back(DialogVertex{x1, y1, u1, v1, c_right.r, c_right.g, c_right.b, c_right.a});
        verts.push_back(DialogVertex{x0, y0, u0, v0, c_left.r, c_left.g, c_left.b, c_left.a});
        verts.push_back(DialogVertex{x1, y1, u1, v1, c_right.r, c_right.g, c_right.b, c_right.a});
        verts.push_back(DialogVertex{x0, y1, u0, v1, c_left.r, c_left.g, c_left.b, c_left.a});
    };

    auto push_rect = [&](f32 x, f32 y, f32 w, f32 h, ColorF col) {
        push_quad_grad(white_tex_, x * S, y * S, (x + w) * S, (y + h) * S,
                       0.0f, 0.0f, 1.0f, 1.0f, col, col);
    };

    auto push_rect_grad_h = [&](f32 x, f32 y, f32 w, f32 h, ColorF cl, ColorF cr) {
        push_quad_grad(white_tex_, x * S, y * S, (x + w) * S, (y + h) * S,
                       0.0f, 0.0f, 1.0f, 1.0f, cl, cr);
    };

    auto push_bevel_box = [&](f32 x, f32 y, f32 w, f32 h, f32 t, ColorF tl, ColorF br) {
        push_rect(x, y, w, t, tl);
        push_rect(x, y, t, h, tl);
        push_rect(x, y + h - t, w, t, br);
        push_rect(x + w - t, y, t, h, br);
    };

    auto push_tex = [&](TextureHandle tex, f32 x, f32 y, f32 w, f32 h, ColorF col) {
        push_quad_grad(tex, x * S, y * S, (x + w) * S, (y + h) * S,
                       0.0f, 0.0f, 1.0f, 1.0f, col, col);
    };

    auto draw_text_run = [&](CachedFontFace& face, std::span<const u32> cps,
                             f32 lx, f32 ly, ColorF col, i32 underline_idx = -1,
                             bool underline_all = false) -> f32 {
        const f32 inv_r = 1.0f / L.raster_scale;
        const f32 baseline = ly + static_cast<f32>(face.ascent) * inv_r;
        f32 pen_x = lx;
        for (usize i = 0; i < cps.size(); ++i) {
            const GlyphEntry& g = impl_->get_glyph(face, cps[i], L.raster_scale);
            const f32 adv = g.advance * inv_r;
            if (g.atlas_page >= 0 && g.w > 0 && g.h > 0 &&
                static_cast<usize>(g.atlas_page) < impl_->atlas_pages.size()) {
                const f32 gx0 = std::floor((pen_x + static_cast<f32>(g.xoff) * inv_r) * S + 0.5f);
                const f32 gy0 = std::floor((baseline + static_cast<f32>(g.yoff) * inv_r) * S + 0.5f);
                const f32 gx1 = gx0 + static_cast<f32>(g.w) * (S * inv_r);
                const f32 gy1 = gy0 + static_cast<f32>(g.h) * (S * inv_r);
                push_quad_grad(impl_->atlas_pages[static_cast<usize>(g.atlas_page)].tex,
                               gx0, gy0, gx1, gy1, g.u0, g.v0, g.u1, g.v1, col, col);
            }
            if (underline_all || static_cast<i32>(i) == underline_idx) {
                const f32 uy = baseline + 1.2f;
                push_rect(pen_x, uy, std::max(1.0f, adv), 1.0f, col);
            }
            pen_x += adv;
        }
        return pen_x - lx;
    };

    // 1. Optional classic Windows bsDialog frame & title bar (message_caption(1, str))
    if (style_.show_caption) {
        const RectF& o = L.outer;
        push_rect(o.x, o.y, o.w, o.h, rgb_to_colorf(212, 208, 200, dlg_alpha));
        push_bevel_box(o.x, o.y, o.w, o.h, 1.0f,
                       rgb_to_colorf(212, 208, 200, dlg_alpha),
                       rgb_to_colorf(64, 64, 64, dlg_alpha));
        push_bevel_box(o.x + 1.0f, o.y + 1.0f, o.w - 2.0f, o.h - 2.0f, 1.0f,
                       rgb_to_colorf(255, 255, 255, dlg_alpha),
                       rgb_to_colorf(128, 128, 128, dlg_alpha));

        // Active Windows classic blue gradient title bar
        push_rect_grad_h(L.title_bar.x, L.title_bar.y, L.title_bar.w, L.title_bar.h,
                         rgb_to_colorf(10, 36, 106, dlg_alpha),
                         rgb_to_colorf(166, 202, 240, dlg_alpha));

        // Caption text (clipped before close button)
        if (!style_.caption.empty()) {
            flush_batch();
            const f32 cap_clip_w = std::max(0.0f, L.close_btn.x - L.title_bar.x - 8.0f);
            const i32 sx = std::max(0, static_cast<i32>(std::floor((L.title_bar.x + 4.0f) * S)));
            const i32 sy_top = std::max(0, static_cast<i32>(std::floor(L.title_bar.y * S)));
            const i32 sw = std::max(0, static_cast<i32>(std::ceil(cap_clip_w * S)));
            const i32 sh = std::max(0, static_cast<i32>(std::ceil(L.title_bar.h * S)));
            device_->set_scissor(true, ViewportRect{sx, std::max(0, drawable_h - (sy_top + sh)), sw, sh});

            MessageFontSpec cap_spec{"MS Sans Serif", 8, 0xFFFFFFu, true, false, 1};
            auto& cap_face = impl_->get_face(cap_spec, L.raster_scale);
            const f32 cap_h = static_cast<f32>(cap_face.line_height) / L.raster_scale;
            const auto cap_cps = utf8_to_codepoints(style_.caption);
            draw_text_run(cap_face, cap_cps,
                          L.title_bar.x + 5.0f,
                          L.title_bar.y + std::floor((L.title_bar.h - cap_h) * 0.5f),
                          rgb_to_colorf(255, 255, 255, dlg_alpha));
            flush_batch();
            device_->set_scissor(false);
        }

        // Classic raised/sunken Windows [X] close button
        const RectF& cb = L.close_btn;
        const bool sunken = close_pressed_ && close_hovered_;
        push_rect(cb.x, cb.y, cb.w, cb.h,
                  close_hovered_ ? rgb_to_colorf(224, 220, 212, dlg_alpha)
                                 : rgb_to_colorf(212, 208, 200, dlg_alpha));
        push_bevel_box(cb.x, cb.y, cb.w, cb.h, 1.0f,
                       sunken ? rgb_to_colorf(64, 64, 64, dlg_alpha) : rgb_to_colorf(255, 255, 255, dlg_alpha),
                       sunken ? rgb_to_colorf(255, 255, 255, dlg_alpha) : rgb_to_colorf(64, 64, 64, dlg_alpha));
        push_bevel_box(cb.x + 1.0f, cb.y + 1.0f, cb.w - 2.0f, cb.h - 2.0f, 1.0f,
                       sunken ? rgb_to_colorf(128, 128, 128, dlg_alpha) : rgb_to_colorf(223, 223, 223, dlg_alpha),
                       sunken ? rgb_to_colorf(223, 223, 223, dlg_alpha) : rgb_to_colorf(128, 128, 128, dlg_alpha));

        // Crisp 8x7 X glyph inside close button
        const f32 xo = cb.x + 4.0f + (sunken ? 1.0f : 0.0f);
        const f32 yo = cb.y + 3.0f + (sunken ? 1.0f : 0.0f);
        const ColorF xcol = rgb_to_colorf(0, 0, 0, dlg_alpha);
        for (i32 d = 0; d < 7; ++d) {
            push_rect(xo + static_cast<f32>(d), yo + static_cast<f32>(d), 2.0f, 1.0f, xcol);
            push_rect(xo + static_cast<f32>(6 - d), yo + static_cast<f32>(d), 2.0f, 1.0f, xcol);
        }
    }

    // 2. BackgroundImage (stretched to client rect, with message_alpha + transparency key)
    if (bg_tex_.valid()) {
        push_tex(bg_tex_, L.client.x, L.client.y, L.client.w, L.client.h,
                 ColorF{1.0f, 1.0f, 1.0f, dlg_alpha});
    } else {
        push_rect(L.client.x, L.client.y, L.client.w, L.client.h,
                  rgb_to_colorf(90, 90, 90, dlg_alpha));
    }

    // 3. TextLabel (wrapped lines, clipped to client area)
    {
        flush_batch();
        const i32 csx = std::max(0, static_cast<i32>(std::floor(L.client.x * S)));
        const i32 csy_top = std::max(0, static_cast<i32>(std::floor(L.client.y * S)));
        const i32 csw = std::max(0, static_cast<i32>(std::ceil(L.client.w * S)));
        const i32 csh = std::max(0, static_cast<i32>(std::ceil(L.client.h * S)));
        device_->set_scissor(true, ViewportRect{csx, std::max(0, drawable_h - (csy_top + csh)), csw, csh});

        auto& text_face = impl_->get_face(style_.text_font, L.raster_scale);
        const ColorF tcol = bgr_to_colorf(style_.text_font.color_bgr, dlg_alpha);
        f32 ly = L.text_rect.y;
        const f32 max_y = L.client.y + L.client.h - 4.0f;
        for (const auto& line : L.wrapped_lines) {
            if (ly >= max_y) break;
            if (!line.empty()) {
                draw_text_run(text_face, line, L.text_rect.x, ly, tcol);
            }
            ly += L.text_line_h;
        }
        flush_batch();
        device_->set_scissor(false);
    }

    // 4. TextEdit input box (for get_string / get_integer)
    if (with_input_) {
        const RectF& ir = L.input_rect;
        // Fill + classic Windows 2px sunken 3D border (WS_EX_CLIENTEDGE)
        push_rect(ir.x, ir.y, ir.w, ir.h, bgr_to_colorf(style_.input_color_bgr, dlg_alpha));
        push_bevel_box(ir.x, ir.y, ir.w, ir.h, 1.0f,
                       rgb_to_colorf(128, 128, 128, dlg_alpha),
                       rgb_to_colorf(255, 255, 255, dlg_alpha));
        push_bevel_box(ir.x + 1.0f, ir.y + 1.0f, ir.w - 2.0f, ir.h - 2.0f, 1.0f,
                       rgb_to_colorf(64, 64, 64, dlg_alpha),
                       rgb_to_colorf(212, 208, 200, dlg_alpha));
        flush_batch();

        // Scissor-clip inside inner border
        const f32 pad_x = 3.0f;
        const f32 pad_y = 2.0f;
        const i32 sx = std::max(0, static_cast<i32>(std::floor((ir.x + pad_x) * S)));
        const i32 sy_top = std::max(0, static_cast<i32>(std::floor((ir.y + pad_y) * S)));
        const i32 sw = std::max(0, static_cast<i32>(std::ceil((ir.w - pad_x * 2.0f) * S)));
        const i32 sh = std::max(0, static_cast<i32>(std::ceil((ir.h - pad_y * 2.0f) * S)));
        device_->set_scissor(true, ViewportRect{sx, std::max(0, drawable_h - (sy_top + sh)), sw, sh});

        auto& input_face = impl_->get_face(style_.input_font, L.raster_scale);
        const f32 text_x0 = ir.x + 5.0f - input_scroll_x_;
        const f32 text_y = ir.y + std::floor((ir.h - L.input_line_h) * 0.5f);
        const ColorF icol = bgr_to_colorf(style_.input_font.color_bgr, dlg_alpha);
        const ColorF sel_bg = rgb_to_colorf(49, 106, 197, dlg_alpha);
        const ColorF sel_fg = rgb_to_colorf(255, 255, 255, dlg_alpha);

        const usize lo = std::min(caret_, sel_anchor_);
        const usize hi = std::max(caret_, sel_anchor_);

        if (lo != hi) {
            const f32 sel_x0 = text_x0 + impl_->measure_cps(input_face, std::span<const u32>(input_cps_.data(), lo), L.raster_scale);
            const f32 sel_w = impl_->measure_cps(input_face, std::span<const u32>(input_cps_.data() + lo, hi - lo), L.raster_scale);
            push_rect(sel_x0, ir.y + 3.0f, sel_w, std::max(2.0f, ir.h - 6.0f), sel_bg);
        }

        // Draw prefix before caret/selection, selection, IME composition, and suffix.
        f32 pen_x = text_x0;
        if (lo > 0) {
            pen_x += draw_text_run(input_face, std::span<const u32>(input_cps_.data(), lo), pen_x, text_y, icol);
        }
        if (hi > lo) {
            pen_x += draw_text_run(input_face, std::span<const u32>(input_cps_.data() + lo, hi - lo), pen_x, text_y, sel_fg);
        }

        f32 caret_draw_x = text_x0 + impl_->measure_cps(input_face, std::span<const u32>(input_cps_.data(), caret_), L.raster_scale);

        if (!ime_text_.empty()) {
            const auto ime_cps = utf8_to_codepoints(ime_text_);
            const f32 ime_w = draw_text_run(input_face, ime_cps, pen_x, text_y, icol, -1, true);
            pen_x += ime_w;
            caret_draw_x = pen_x;
        }
        if (hi < input_cps_.size()) {
            draw_text_run(input_face, std::span<const u32>(input_cps_.data() + hi, input_cps_.size() - hi),
                          pen_x, text_y, icol);
        }

        // Blinking caret (~530ms half-period)
        const u64 elapsed_ms = (now_us >= caret_reset_us_) ? ((now_us - caret_reset_us_) / 1000u) : 0u;
        if (((elapsed_ms / 530u) & 1u) == 0u) {
            push_rect(caret_draw_x, ir.y + 3.0f, 1.0f, std::max(2.0f, ir.h - 6.0f), icol);
        }

        flush_batch();
        device_->set_scissor(false);
    }

    // 5. Buttons 1..3 (TMyButton)
    {
        auto& btn_face = impl_->get_face(style_.button_font, L.raster_scale);
        for (usize i = 0; i < 3; ++i) {
            const auto& b = buttons_[i];
            if (!b.visible) continue;
            const RectF& br = L.btn_rects[i];
            const usize state_idx = (b.pressed && b.hovered) ? 2u : (b.hovered ? 1u : 0u);
            if (button_tex_[state_idx].valid()) {
                push_tex(button_tex_[state_idx], br.x, br.y, br.w, br.h,
                         ColorF{1.0f, 1.0f, 1.0f, dlg_alpha});
            } else {
                const ColorF fallback_bg = (state_idx == 2u) ? rgb_to_colorf(181, 24, 24, dlg_alpha)
                                         : (state_idx == 1u) ? rgb_to_colorf(136, 0, 0, dlg_alpha)
                                                             : rgb_to_colorf(71, 71, 71, dlg_alpha);
                push_rect(br.x, br.y, br.w, br.h, fallback_bg);
            }

            if (!b.display_cps.empty()) {
                const u32 col_bgr = b.hovered ? style_.mouse_color_bgr : style_.button_font.color_bgr;
                const f32 tw = impl_->measure_cps(btn_face, b.display_cps, L.raster_scale);
                const f32 tx = br.x + std::floor((br.w - tw) * 0.5f);
                const f32 ty = br.y + std::floor((br.h - L.button_line_h) * 0.5f);
                draw_text_run(btn_face, b.display_cps, tx, ty, bgr_to_colorf(col_bgr, dlg_alpha), b.underline_cp_idx);
            }
        }
    }

    flush_batch();
    device_->restore_render_state(saved_state);
}

} // namespace gm
