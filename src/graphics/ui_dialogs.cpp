#include "graphics/ui_dialogs.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "graphics/builtin_font.h"
#include "graphics/gl_headers.h"
#include "platform/font_loader.h"
#include "platform/web_bridge.h"

#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace gm {

namespace {

constexpr i32 kUiAtlasSize = 512;
constexpr f32 kUiTitleBarH = 22.0f;
constexpr f32 kUiBorder = 3.0f;
constexpr f32 kUiCloseW = 16.0f;
constexpr f32 kUiCloseH = 14.0f;
constexpr f32 kUiScrollbarW = 16.0f;

struct UiVertex {
    f32 x, y;
    f32 u, v;
    f32 r, g, b, a;
};

struct UiColor {
    f32 r, g, b, a;
};

struct UiRect {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;
    [[nodiscard]] bool contains(f32 px, f32 py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct UiFontSpec {
    std::string name = "Arial";
    i32 size_pt = 10;
    bool bold = false;
    bool italic = false;
    u8 charset = 1;
};

UiColor ui_rgb(u8 r, u8 g, u8 b, f32 a = 1.0f) noexcept {
    return UiColor{
        static_cast<f32>(r) / 255.0f,
        static_cast<f32>(g) / 255.0f,
        static_cast<f32>(b) / 255.0f,
        std::clamp(a, 0.0f, 1.0f),
    };
}

UiColor ui_bgr(u32 bgr, f32 a = 1.0f) noexcept {
    return ui_rgb(
        static_cast<u8>(bgr & 0xFFu),
        static_cast<u8>((bgr >> 8u) & 0xFFu),
        static_cast<u8>((bgr >> 16u) & 0xFFu),
        a);
}

u32 ascii_upper(u32 cp) noexcept {
    return (cp >= 'a' && cp <= 'z') ? (cp - ('a' - 'A')) : cp;
}

std::vector<u32> utf8_to_cps(std::string_view utf8) {
    std::vector<u32> out;
    out.reserve(utf8.size());
    usize off = 0;
    while (off < utf8.size()) {
        out.push_back(utf8_decode_next(utf8, off));
    }
    return out;
}

std::string cps_to_utf8(std::span<const u32> cps) {
    std::string out;
    out.reserve(cps.size());
    for (u32 cp : cps) {
        utf8_append_codepoint(out, cp);
    }
    return out;
}

void hsv_to_rgb(f32 h_deg, f32 s, f32 v, u8& out_r, u8& out_g, u8& out_b) noexcept {
    const f32 h = std::fmod(std::fmod(h_deg, 360.0f) + 360.0f, 360.0f);
    const f32 c = v * s;
    const f32 x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const f32 m = v - c;
    f32 rf = 0.0f, gf = 0.0f, bf = 0.0f;
    if (h < 60.0f)       { rf = c; gf = x; bf = 0.0f; }
    else if (h < 120.0f) { rf = x; gf = c; bf = 0.0f; }
    else if (h < 180.0f) { rf = 0.0f; gf = c; bf = x; }
    else if (h < 240.0f) { rf = 0.0f; gf = x; bf = c; }
    else if (h < 300.0f) { rf = x; gf = 0.0f; bf = c; }
    else                 { rf = c; gf = 0.0f; bf = x; }
    out_r = static_cast<u8>(std::clamp(static_cast<i32>(std::round((rf + m) * 255.0f)), 0, 255));
    out_g = static_cast<u8>(std::clamp(static_cast<i32>(std::round((gf + m) * 255.0f)), 0, 255));
    out_b = static_cast<u8>(std::clamp(static_cast<i32>(std::round((bf + m) * 255.0f)), 0, 255));
}

void rgb_to_hsv(u8 r, u8 g, u8 b, f32& out_h, f32& out_s, f32& out_v) noexcept {
    const f32 rf = static_cast<f32>(r) / 255.0f;
    const f32 gf = static_cast<f32>(g) / 255.0f;
    const f32 bf = static_cast<f32>(b) / 255.0f;
    const f32 mx = std::max({rf, gf, bf});
    const f32 mn = std::min({rf, gf, bf});
    const f32 d = mx - mn;
    out_v = mx;
    out_s = (mx > 1e-6f) ? (d / mx) : 0.0f;
    if (d <= 1e-6f) {
        out_h = 0.0f;
    } else if (mx == rf) {
        out_h = 60.0f * std::fmod(((gf - bf) / d) + 6.0f, 6.0f);
    } else if (mx == gf) {
        out_h = 60.0f * (((bf - rf) / d) + 2.0f);
    } else {
        out_h = 60.0f * (((rf - gf) / d) + 4.0f);
    }
}

// Classic 48 Windows ChooseColor / Delphi TColorDialog basic colors (0xRRGGBB).
constexpr std::array<u32, 48> kBasicColorsRgb = {{
    0xFF8080, 0xFFFF80, 0x80FF80, 0x00FF80, 0x80FFFF, 0x0080FF, 0xFF80C0, 0xFF80FF,
    0xFF0000, 0xFFFF00, 0x80FF00, 0x00FF40, 0x00FFFF, 0x0080C0, 0x8080C0, 0xFF00FF,
    0x804040, 0xFF8040, 0x00FF00, 0x008080, 0x004080, 0x8080FF, 0x800040, 0xFF0080,
    0x800000, 0xFF8000, 0x008000, 0x008040, 0x0000FF, 0x0000A0, 0x800080, 0x8000FF,
    0x400000, 0x804000, 0x004000, 0x004040, 0x000080, 0x000040, 0x400040, 0x400080,
    0x000000, 0x808000, 0x808040, 0x808080, 0x408080, 0xC0C0C0, 0x400040, 0xFFFFFF,
}};

void apply_faux_bold_ui(std::vector<u8>& alpha, i32& w, i32 h, i32 extra_px) {
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
void apply_faux_italic_ui(std::vector<u8>& alpha, i32& w, i32 h, i32& xoff, i32 yoff) {
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
#endif

// Dynamic glyph atlas cache (shared across all 5 GM modal dialog modes).
struct UiGlyph {
    i32 page = -1;
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    i32 w = 0, h = 0;
    i32 xoff = 0, yoff = 0;
    f32 advance = 0.0f;
};

struct UiAtlasPage {
    TextureHandle tex{};
    i32 cx = 1;
    i32 cy = 1;
    i32 row_h = 0;
};

struct UiBlobKey {
    std::string name_lower;
    bool bold = false;
    bool italic = false;
    u8 charset = 1;
    bool operator==(const UiBlobKey&) const = default;
};

struct UiBlobHash {
    usize operator()(const UiBlobKey& k) const noexcept {
        usize h = std::hash<std::string>{}(k.name_lower);
        h ^= (static_cast<usize>(k.bold) << 1u) ^ (static_cast<usize>(k.italic) << 2u) ^
             (static_cast<usize>(k.charset) << 3u);
        return h;
    }
};

struct UiFaceKey {
    std::string name_lower;
    i32 em_q4 = 44;
    bool bold = false;
    bool italic = false;
    u8 charset = 1;
    bool operator==(const UiFaceKey&) const = default;
};

struct UiFaceHash {
    usize operator()(const UiFaceKey& k) const noexcept {
        usize h = std::hash<std::string>{}(k.name_lower);
        h ^= static_cast<usize>(k.em_q4) * 2654435761u;
        h ^= (static_cast<usize>(k.bold) << 1u) ^ (static_cast<usize>(k.italic) << 2u) ^
             (static_cast<usize>(k.charset) << 3u);
        return h;
    }
};

struct UiTtfFace {
    SystemFontBlob blob;
    stbtt_fontinfo info{};
    bool valid = false;
};

struct UiFontFace {
    std::string family;
    f32 em_px = 13.0f;
    bool bold = false;
    bool italic = false;
    u8 charset = 1;
    const UiTtfFace* primary = nullptr;
    f32 stb_scale = 0.0f;
    i32 ascent = 10;
    i32 descent = 3;
    i32 line_height = 15;
    std::unordered_map<u32, UiGlyph> glyphs;
};

enum class DialogMode : u8 {
    None = 0,
    Highscore,
    Menu,
    ColorPicker,
    Splash,
    GameInfo,
};

struct ParsedMenuItem {
    bool is_separator = false;
    i32 index = -1;
    std::vector<u32> display_cps;
    i32 underline_idx = -1;
    u32 shortcut_upper = 0;
};

struct WrappedRtfSegment {
    std::vector<u32> cps;
    UiFontSpec font;
    u32 color_bgr = 0;
    bool underline = false;
    bool strikeout = false;
    f32 x_offset = 0.0f;
    f32 width = 0.0f;
    f32 ascent = 10.0f;
    f32 line_h = 14.0f;
};

struct WrappedRtfLine {
    f32 y = 0.0f;
    f32 height = 14.0f;
    f32 baseline = 11.0f;
    f32 total_w = 0.0f;
    f32 left_indent = 0.0f;
    i32 align = 0;
    std::vector<WrappedRtfSegment> segments;
};

bool is_cjk_or_breakable(u32 cp) noexcept {
    return (cp >= 0x2E80u && cp <= 0x9FFFu) ||
           (cp >= 0xAC00u && cp <= 0xD7AFu) ||
           (cp >= 0xF900u && cp <= 0xFAFFu) ||
           (cp >= 0xFF00u && cp <= 0xFFEFu);
}

} // namespace

struct GmModalDialogs::Impl {
    GlGraphicsDevice* device = nullptr;
    bool initialized = false;

    ShaderHandle shader{};
    u32 vao = 0;
    u32 vbo = 0;
    i32 loc_proj = -1;
    i32 loc_tex = -1;
    TextureHandle white_tex{};

    std::vector<UiAtlasPage> pages;
    std::unordered_map<UiBlobKey, std::unique_ptr<UiTtfFace>, UiBlobHash> primary_blobs;
    std::unordered_map<u8, std::vector<std::unique_ptr<UiTtfFace>>> fallback_blobs;
    std::unordered_map<UiFaceKey, std::unique_ptr<UiFontFace>, UiFaceHash> faces;
    std::vector<UiVertex> verts;
    TextureHandle batch_tex{};

    // Active session image texture (Highscore background or Splash image)
    Bitmap32 session_bitmap;
    TextureHandle session_tex{};

    DialogMode mode = DialogMode::None;
    bool active = false;

    // Shared window frame drag / close button state
    bool close_hovered = false;
    bool close_pressed = false;
    bool dragging_title = false;
    f32 drag_off_x = 0.0f;
    f32 drag_off_y = 0.0f;
    bool has_drag_pos = false;
    f32 drag_x = 0.0f;
    f32 drag_y = 0.0f;

    // Highscore state
    HighscoreDialogParams hs_params;
    std::vector<u32> hs_edit_cps;
    u64 hs_caret_reset_us = 0;

    // Menu state
    MenuDialogParams menu_params;
    std::vector<ParsedMenuItem> menu_items;
    i32 menu_hover_row = -1;
    i32 menu_result = -1;

    // Color picker state
    u32 cp_initial_bgr = 0;
    u8 cp_r = 0, cp_g = 0, cp_b = 0;
    f32 cp_h = 0.0f, cp_s = 0.0f, cp_v = 0.0f;
    bool cp_accepted = false;
    i32 cp_drag_widget = 0; // 0=none, 1=sv, 2=hue, 3=R, 4=G, 5=B
    bool cp_ok_hover = false, cp_ok_press = false;
    bool cp_cancel_hover = false, cp_cancel_press = false;

    // Splash state
    SplashDialogParams splash_params;
    u64 splash_start_us = 0;
    bool splash_x_hover = false;
    bool splash_x_press = false;

    // Game Info (and Splash Text) RTF layout & scroll state
    GameInfoDialogParams info_params;
    std::vector<WrappedRtfLine> rtf_lines;
    f32 rtf_content_h = 0.0f;
    f32 rtf_layout_width = -1.0f;
    f32 rtf_layout_rscale = -1.0f;
    f32 scroll_y = 0.0f;
    bool sb_dragging = false;
    f32 sb_drag_offset = 0.0f;
    bool touch_scrolling = false;
    f32 touch_last_y = 0.0f;

    void clear_gpu_atlases(bool delete_gl) {
        if (delete_gl && device) {
            for (auto& p : pages) {
                if (p.tex.valid()) device->destroy_texture(p.tex);
            }
        }
        pages.clear();
        for (auto& [k, f] : faces) {
            (void)k;
            f->glyphs.clear();
        }
    }

    void destroy_session_texture(bool delete_gl) {
        if (delete_gl && device && session_tex.valid()) {
            device->destroy_texture(session_tex);
        }
        session_tex = {};
    }

    void upload_session_bitmap() {
        destroy_session_texture(true);
        if (!device || session_bitmap.width <= 0 || session_bitmap.height <= 0 || session_bitmap.bgra.empty()) {
            return;
        }
        TextureDesc desc{};
        desc.width = session_bitmap.width;
        desc.height = session_bitmap.height;
        desc.format = TextureFormat::BGRA8;
        desc.min_filter = TextureFilter::Linear;
        desc.mag_filter = TextureFilter::Linear;
        if (auto res = device->create_texture(desc, session_bitmap.bgra); res) {
            session_tex = res.value();
        }
    }

    Result<void> create_gpu_pipeline() {
        if (!device) return Error{"GmModalDialogs: null graphics device"};
        const bool is_es = device->caps().is_gles;
        const std::string vs_hdr = is_es ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n";
        const std::string fs_hdr = is_es ? "#version 300 es\nprecision mediump float;\n" : "#version 330 core\n";

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

        auto sh = device->create_shader(vs_src, fs_src);
        if (!sh) return sh.error();
        shader = sh.value();
        loc_proj = glGetUniformLocation(shader.id, "u_proj");
        loc_tex = glGetUniformLocation(shader.id, "u_tex");

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex),
                              reinterpret_cast<const void*>(offsetof(UiVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex),
                              reinterpret_cast<const void*>(offsetof(UiVertex, u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(UiVertex),
                              reinterpret_cast<const void*>(offsetof(UiVertex, r)));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        const std::array<u8, 4> white_px = {255u, 255u, 255u, 255u};
        TextureDesc wdesc{};
        wdesc.width = 1;
        wdesc.height = 1;
        wdesc.format = TextureFormat::RGBA8;
        wdesc.min_filter = TextureFilter::Nearest;
        wdesc.mag_filter = TextureFilter::Nearest;
        auto wtex = device->create_texture(wdesc, white_px);
        if (!wtex) return wtex.error();
        white_tex = wtex.value();

        upload_session_bitmap();
        return Result<void>::success();
    }

    void destroy_gpu_resources(bool delete_gl) noexcept {
        clear_gpu_atlases(delete_gl);
        destroy_session_texture(delete_gl);
        if (delete_gl && device) {
            if (white_tex.valid()) device->destroy_texture(white_tex);
            if (shader.valid()) device->destroy_shader(shader);
            if (vbo != 0) glDeleteBuffers(1, &vbo);
            if (vao != 0) glDeleteVertexArrays(1, &vao);
        }
        white_tex = {};
        shader = {};
        vao = 0;
        vbo = 0;
        loc_proj = -1;
        loc_tex = -1;
    }

    const UiTtfFace* get_primary_ttf(std::string_view name, bool bold, bool italic, u8 charset) {
        UiBlobKey key{ascii_to_lower(name), bold, italic, charset};
        if (auto it = primary_blobs.find(key); it != primary_blobs.end()) {
            return it->second->valid ? it->second.get() : nullptr;
        }
        auto entry = std::make_unique<UiTtfFace>();
        if (load_system_font(name, bold, italic, charset, entry->blob) && !entry->blob.data.empty()) {
            const i32 off = stbtt_GetFontOffsetForIndex(entry->blob.data.data(), entry->blob.face_index);
            if (off >= 0 && stbtt_InitFont(&entry->info, entry->blob.data.data(), off) != 0) {
                entry->valid = true;
            }
        }
        const UiTtfFace* ptr = entry->valid ? entry.get() : nullptr;
        primary_blobs.emplace(std::move(key), std::move(entry));
        return ptr;
    }

    const std::vector<std::unique_ptr<UiTtfFace>>& get_fallback_ttfs(u8 charset) {
        if (auto it = fallback_blobs.find(charset); it != fallback_blobs.end()) {
            return it->second;
        }
        std::vector<std::unique_ptr<UiTtfFace>> list;
        auto blobs = load_system_fallback_fonts(charset);
        for (auto& b : blobs) {
            if (b.data.empty()) continue;
            auto entry = std::make_unique<UiTtfFace>();
            entry->blob = std::move(b);
            const i32 off = stbtt_GetFontOffsetForIndex(entry->blob.data.data(), entry->blob.face_index);
            if (off >= 0 && stbtt_InitFont(&entry->info, entry->blob.data.data(), off) != 0) {
                entry->valid = true;
                list.push_back(std::move(entry));
            }
        }
        auto [it, _] = fallback_blobs.emplace(charset, std::move(list));
        return it->second;
    }

    UiFontFace& get_face(const UiFontSpec& spec, f32 raster_scale) {
        const f32 pt = static_cast<f32>(std::clamp(spec.size_pt, 4, 160));
        const f32 logical_em = std::max(6.0f, static_cast<f32>(delphi_round(static_cast<f64>(pt) * 96.0 / 72.0)));
        const f32 em_px = std::clamp(logical_em * raster_scale, 6.0f, 160.0f);
        const i32 em_q4 = static_cast<i32>(std::round(em_px * 4.0f));

        UiFaceKey key{ascii_to_lower(spec.name), em_q4, spec.bold, spec.italic, spec.charset};
        if (auto it = faces.find(key); it != faces.end()) {
            return *it->second;
        }

        auto face = std::make_unique<UiFontFace>();
        face->family = spec.name;
        face->em_px = static_cast<f32>(em_q4) * 0.25f;
        face->bold = spec.bold;
        face->italic = spec.italic;
        face->charset = spec.charset;

#if defined(__EMSCRIPTEN__)
        if (!web_measure_font_metrics(face->family, face->em_px, face->bold, face->italic,
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
        } else {
            face->ascent = std::max(1, static_cast<i32>(std::ceil(face->em_px * 0.85f)));
            face->descent = std::max(0, static_cast<i32>(std::ceil(face->em_px * 0.20f)));
            face->line_height = std::max(face->ascent + face->descent, static_cast<i32>(std::ceil(face->em_px * 1.18f)));
        }
#endif
        UiFontFace& ref = *face;
        faces.emplace(std::move(key), std::move(face));
        return ref;
    }

    bool pack_glyph(i32 gw, i32 gh, const u8* alpha, UiGlyph& g) {
        if (!device || gw <= 0 || gh <= 0 || !alpha) {
            g.page = -1;
            return true;
        }
        if (gw > kUiAtlasSize - 2 || gh > kUiAtlasSize - 2) {
            g.page = -1;
            return false;
        }
        auto alloc = [&](UiAtlasPage& pg, i32& ox, i32& oy) -> bool {
            if (pg.cx + gw + 1 > kUiAtlasSize) {
                pg.cx = 1;
                pg.cy += pg.row_h + 1;
                pg.row_h = 0;
            }
            if (pg.cy + gh + 1 > kUiAtlasSize) return false;
            ox = pg.cx;
            oy = pg.cy;
            pg.cx += gw + 1;
            pg.row_h = std::max(pg.row_h, gh);
            return true;
        };

        i32 px = 0, py = 0, pidx = -1;
        for (usize i = 0; i < pages.size(); ++i) {
            if (alloc(pages[i], px, py)) {
                pidx = static_cast<i32>(i);
                break;
            }
        }
        if (pidx < 0) {
            UiAtlasPage np{};
            TextureDesc desc{};
            desc.width = kUiAtlasSize;
            desc.height = kUiAtlasSize;
            desc.format = TextureFormat::RGBA8;
            desc.min_filter = TextureFilter::Linear;
            desc.mag_filter = TextureFilter::Linear;
            std::vector<u8> zeros(static_cast<usize>(kUiAtlasSize) * static_cast<usize>(kUiAtlasSize) * 4u, 0u);
            auto tr = device->create_texture(desc, zeros);
            if (!tr) return false;
            np.tex = tr.value();
            if (!alloc(np, px, py)) return false;
            pidx = static_cast<i32>(pages.size());
            pages.push_back(np);
        }

        std::vector<u8> rgba(static_cast<usize>(gw) * static_cast<usize>(gh) * 4u);
        for (i32 i = 0; i < gw * gh; ++i) {
            rgba[static_cast<usize>(i) * 4u + 0] = 255u;
            rgba[static_cast<usize>(i) * 4u + 1] = 255u;
            rgba[static_cast<usize>(i) * 4u + 2] = 255u;
            rgba[static_cast<usize>(i) * 4u + 3] = alpha[i];
        }
        device->update_texture(pages[static_cast<usize>(pidx)].tex, px, py, gw, gh, TextureFormat::RGBA8, rgba);

        const f32 inv = 1.0f / static_cast<f32>(kUiAtlasSize);
        g.page = pidx;
        g.u0 = static_cast<f32>(px) * inv;
        g.v0 = static_cast<f32>(py) * inv;
        g.u1 = static_cast<f32>(px + gw) * inv;
        g.v1 = static_cast<f32>(py + gh) * inv;
        return true;
    }

    const UiGlyph& get_glyph(UiFontFace& face, u32 cp, f32 raster_scale) {
        if (auto it = face.glyphs.find(cp); it != face.glyphs.end()) {
            return it->second;
        }
        UiGlyph g{};
        std::vector<u8> alpha;
        bool ok = false;

#if defined(__EMSCRIPTEN__)
        WebGlyphBitmap wb{};
        if (web_rasterize_glyph(face.family, face.em_px, face.bold, face.italic, cp, wb)) {
            g.w = wb.width;
            g.h = wb.height;
            g.xoff = wb.xoff;
            g.yoff = wb.yoff;
            g.advance = wb.advance;
            alpha = std::move(wb.alpha);
            ok = true;
        }
#else
        const UiTtfFace* ttf = nullptr;
        int gidx = 0;
        if (face.primary) {
            gidx = stbtt_FindGlyphIndex(&face.primary->info, static_cast<int>(cp));
            if (gidx != 0) ttf = face.primary;
        }
        if (!ttf && cp >= 32u) {
            for (const auto& fb : get_fallback_ttfs(face.charset)) {
                if (!fb || !fb->valid) continue;
                const int idx = stbtt_FindGlyphIndex(&fb->info, static_cast<int>(cp));
                if (idx != 0) {
                    ttf = fb.get();
                    gidx = idx;
                    break;
                }
            }
        }
        if (ttf && gidx != 0) {
            const f32 sc = (ttf == face.primary) ? face.stb_scale
                                                 : stbtt_ScaleForMappingEmToPixels(&ttf->info, face.em_px);
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&ttf->info, gidx, &adv, &lsb);
            g.advance = static_cast<f32>(adv) * sc;
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&ttf->info, gidx, sc, sc, &x0, &y0, &x1, &y1);
            g.w = x1 - x0;
            g.h = y1 - y0;
            g.xoff = x0;
            g.yoff = y0;
            if (g.w > 0 && g.h > 0) {
                alpha.assign(static_cast<usize>(g.w) * static_cast<usize>(g.h), 0u);
                stbtt_MakeGlyphBitmap(&ttf->info, alpha.data(), g.w, g.h, g.w, sc, sc, gidx);
                if (face.bold && !ttf->blob.native_bold) {
                    const i32 extra = std::max(1, static_cast<i32>(std::round(raster_scale)));
                    apply_faux_bold_ui(alpha, g.w, g.h, extra);
                    g.advance += static_cast<f32>(extra);
                }
                if (face.italic && !ttf->blob.native_italic) {
                    apply_faux_italic_ui(alpha, g.w, g.h, g.xoff, g.yoff);
                }
            }
            ok = true;
        }
#endif

        if (!ok) {
            const u32 ascii = (cp >= 32u && cp <= 126u) ? cp : static_cast<u32>('?');
            const i32 spx = std::max(1, static_cast<i32>(std::round(face.em_px / 9.0f)));
            g.w = 8 * spx;
            g.h = 8 * spx;
            g.xoff = 0;
            g.yoff = -g.h + std::max(1, spx);
            g.advance = static_cast<f32>(g.w);
            alpha.assign(static_cast<usize>(g.w) * static_cast<usize>(g.h), 0u);
            const auto& bits = kBuiltinFont8x8[ascii - 32u];
            for (i32 row = 0; row < 8; ++row) {
                for (i32 col = 0; col < 8; ++col) {
                    if ((bits[row] & (1u << col)) != 0u) {
                        for (i32 dy = 0; dy < spx; ++dy) {
                            for (i32 dx = 0; dx < spx; ++dx) {
                                alpha[static_cast<usize>(row * spx + dy) * static_cast<usize>(g.w) +
                                      static_cast<usize>(col * spx + dx)] = 255u;
                            }
                        }
                    }
                }
            }
            if (face.bold) {
                const i32 extra = std::max(1, spx / 2);
                apply_faux_bold_ui(alpha, g.w, g.h, extra);
                g.advance += static_cast<f32>(extra);
            }
        }

        if (!alpha.empty() && g.w > 0 && g.h > 0) {
            (void)pack_glyph(g.w, g.h, alpha.data(), g);
        }
        auto [it, _] = face.glyphs.emplace(cp, g);
        return it->second;
    }

    f32 measure_cps(UiFontFace& face, std::span<const u32> cps, f32 raster_scale) {
        f32 w = 0.0f;
        for (u32 cp : cps) {
            w += get_glyph(face, cp, raster_scale).advance;
        }
        return w / raster_scale;
    }

    // Batch drawing helpers
    void flush_batch() {
        if (verts.empty() || !shader.valid() || !batch_tex.valid()) {
            verts.clear();
            return;
        }
        device->bind_texture(0, batch_tex);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(verts.size() * sizeof(UiVertex)),
                     verts.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
        verts.clear();
    }

    void push_quad(TextureHandle tex,
                   f32 x0, f32 y0, f32 u0, f32 v0, UiColor c0,
                   f32 x1, f32 y1, f32 u1, f32 v1, UiColor c1,
                   f32 x2, f32 y2, f32 u2, f32 v2, UiColor c2,
                   f32 x3, f32 y3, f32 u3, f32 v3, UiColor c3) {
        if (!tex.valid()) tex = white_tex;
        if (batch_tex.id != tex.id) {
            flush_batch();
            batch_tex = tex;
        }
        verts.push_back(UiVertex{x0, y0, u0, v0, c0.r, c0.g, c0.b, c0.a});
        verts.push_back(UiVertex{x1, y1, u1, v1, c1.r, c1.g, c1.b, c1.a});
        verts.push_back(UiVertex{x2, y2, u2, v2, c2.r, c2.g, c2.b, c2.a});
        verts.push_back(UiVertex{x0, y0, u0, v0, c0.r, c0.g, c0.b, c0.a});
        verts.push_back(UiVertex{x2, y2, u2, v2, c2.r, c2.g, c2.b, c2.a});
        verts.push_back(UiVertex{x3, y3, u3, v3, c3.r, c3.g, c3.b, c3.a});
    }

    void fill_rect(f32 x, f32 y, f32 w, f32 h, UiColor c, f32 scale) {
        if (w <= 0.0f || h <= 0.0f) return;
        const f32 x0 = x * scale, y0 = y * scale;
        const f32 x1 = (x + w) * scale, y1 = (y + h) * scale;
        push_quad(white_tex,
                  x0, y0, 0, 0, c, x1, y0, 1, 0, c,
                  x1, y1, 1, 1, c, x0, y1, 0, 1, c);
    }

    void fill_rect_grad_h(f32 x, f32 y, f32 w, f32 h, UiColor left, UiColor right, f32 scale) {
        const f32 x0 = x * scale, y0 = y * scale;
        const f32 x1 = (x + w) * scale, y1 = (y + h) * scale;
        push_quad(white_tex,
                  x0, y0, 0, 0, left, x1, y0, 1, 0, right,
                  x1, y1, 1, 1, right, x0, y1, 0, 1, left);
    }

    void fill_rect_grad_v(f32 x, f32 y, f32 w, f32 h, UiColor top, UiColor bot, f32 scale) {
        const f32 x0 = x * scale, y0 = y * scale;
        const f32 x1 = (x + w) * scale, y1 = (y + h) * scale;
        push_quad(white_tex,
                  x0, y0, 0, 0, top, x1, y0, 1, 0, top,
                  x1, y1, 1, 1, bot, x0, y1, 0, 1, bot);
    }

    void draw_tex_rect(TextureHandle tex, f32 x, f32 y, f32 w, f32 h, UiColor tint, f32 scale) {
        if (!tex.valid() || w <= 0.0f || h <= 0.0f) return;
        const f32 x0 = x * scale, y0 = y * scale;
        const f32 x1 = (x + w) * scale, y1 = (y + h) * scale;
        push_quad(tex,
                  x0, y0, 0, 0, tint, x1, y0, 1, 0, tint,
                  x1, y1, 1, 1, tint, x0, y1, 0, 1, tint);
    }

    void draw_bevel(const UiRect& r, bool raised, f32 scale) {
        const UiColor hi1 = raised ? ui_rgb(255, 255, 255) : ui_rgb(128, 128, 128);
        const UiColor hi2 = raised ? ui_rgb(224, 224, 224) : ui_rgb(64, 64, 64);
        const UiColor lo1 = raised ? ui_rgb(64, 64, 64)    : ui_rgb(255, 255, 255);
        const UiColor lo2 = raised ? ui_rgb(128, 128, 128) : ui_rgb(224, 224, 224);
        fill_rect(r.x, r.y, r.w, 1.0f, hi1, scale);
        fill_rect(r.x, r.y, 1.0f, r.h, hi1, scale);
        fill_rect(r.x, r.y + r.h - 1.0f, r.w, 1.0f, lo1, scale);
        fill_rect(r.x + r.w - 1.0f, r.y, 1.0f, r.h, lo1, scale);
        if (r.w > 2.0f && r.h > 2.0f) {
            fill_rect(r.x + 1.0f, r.y + 1.0f, r.w - 2.0f, 1.0f, hi2, scale);
            fill_rect(r.x + 1.0f, r.y + 1.0f, 1.0f, r.h - 2.0f, hi2, scale);
            fill_rect(r.x + 1.0f, r.y + r.h - 2.0f, r.w - 2.0f, 1.0f, lo2, scale);
            fill_rect(r.x + r.w - 2.0f, r.y + 1.0f, 1.0f, r.h - 2.0f, lo2, scale);
        }
    }

    void draw_close_x_button(const UiRect& r, bool hovered, bool pressed, f32 scale) {
        const UiColor fill = pressed ? ui_rgb(180, 180, 180)
                                     : (hovered ? ui_rgb(232, 200, 200) : ui_rgb(212, 208, 200));
        fill_rect(r.x, r.y, r.w, r.h, fill, scale);
        draw_bevel(r, !pressed, scale);
        const f32 cx = r.x + r.w * 0.5f + (pressed ? 0.5f : 0.0f);
        const f32 cy = r.y + r.h * 0.5f + (pressed ? 0.5f : 0.0f);
        const UiColor ink = ui_rgb(20, 20, 20);
        for (i32 d = -3; d <= 3; ++d) {
            fill_rect(cx + static_cast<f32>(d) - 0.75f, cy + static_cast<f32>(d) - 0.75f, 1.5f, 1.5f, ink, scale);
            fill_rect(cx + static_cast<f32>(d) - 0.75f, cy - static_cast<f32>(d) - 0.75f, 1.5f, 1.5f, ink, scale);
        }
    }

    f32 draw_text_cps(UiFontFace& face, std::span<const u32> cps,
                      f32 x, f32 baseline_y, UiColor col, f32 scale, f32 raster_scale,
                      bool underline = false, bool strikeout = false, i32 underline_cp = -1) {
        const f32 inv_rs = 1.0f / raster_scale;
        f32 pen_x = x;
        for (usize i = 0; i < cps.size(); ++i) {
            const UiGlyph& g = get_glyph(face, cps[i], raster_scale);
            const f32 adv = g.advance * inv_rs;
            if (g.page >= 0 && static_cast<usize>(g.page) < pages.size() && g.w > 0 && g.h > 0) {
                const f32 gx0 = (pen_x + static_cast<f32>(g.xoff) * inv_rs) * scale;
                const f32 gy0 = (baseline_y + static_cast<f32>(g.yoff) * inv_rs) * scale;
                const f32 gx1 = gx0 + static_cast<f32>(g.w) * inv_rs * scale;
                const f32 gy1 = gy0 + static_cast<f32>(g.h) * inv_rs * scale;
                push_quad(pages[static_cast<usize>(g.page)].tex,
                          gx0, gy0, g.u0, g.v0, col,
                          gx1, gy0, g.u1, g.v0, col,
                          gx1, gy1, g.u1, g.v1, col,
                          gx0, gy1, g.u0, g.v1, col);
            }
            if (underline_cp >= 0 && static_cast<i32>(i) == underline_cp) {
                fill_rect(pen_x, baseline_y + 1.5f, std::max(3.0f, adv - 1.0f), 1.0f, col, scale);
            }
            pen_x += adv;
        }
        const f32 total_w = pen_x - x;
        if (underline && total_w > 0.0f) {
            fill_rect(x, baseline_y + 1.5f, total_w, 1.0f, col, scale);
        }
        if (strikeout && total_w > 0.0f) {
            const f32 strike_y = baseline_y - static_cast<f32>(face.ascent) * inv_rs * 0.36f;
            fill_rect(x, strike_y, total_w, 1.0f, col, scale);
        }
        return total_w;
    }

    void draw_title_bar(const UiRect& bar, const UiRect& close_btn, std::string_view title_utf8,
                        f32 scale, f32 raster_scale) {
        fill_rect_grad_h(bar.x, bar.y, bar.w, bar.h, ui_rgb(10, 36, 106), ui_rgb(166, 202, 240), scale);
        UiFontSpec title_spec{"MS Sans Serif", 9, true, false, 1};
        UiFontFace& tf = get_face(title_spec, raster_scale);
        const auto cps = utf8_to_cps(title_utf8);
        const f32 base_y = bar.y + (bar.h - static_cast<f32>(tf.line_height) / raster_scale) * 0.5f +
                           static_cast<f32>(tf.ascent) / raster_scale;
        draw_text_cps(tf, cps, bar.x + 6.0f, base_y, ui_rgb(255, 255, 255), scale, raster_scale);
        draw_close_x_button(close_btn, close_hovered, close_pressed, scale);
    }

    void draw_button(const UiRect& r, std::string_view label, bool hovered, bool pressed,
                     f32 scale, f32 raster_scale) {
        const UiColor bg = pressed ? ui_rgb(192, 190, 184)
                                   : (hovered ? ui_rgb(228, 225, 218) : ui_rgb(212, 208, 200));
        fill_rect(r.x, r.y, r.w, r.h, bg, scale);
        draw_bevel(r, !pressed, scale);
        UiFontSpec spec{"MS Sans Serif", 9, false, false, 1};
        UiFontFace& f = get_face(spec, raster_scale);
        const auto cps = utf8_to_cps(label);
        const f32 tw = measure_cps(f, cps, raster_scale);
        const f32 tx = r.x + (r.w - tw) * 0.5f + (pressed ? 1.0f : 0.0f);
        const f32 ty = r.y + (r.h - static_cast<f32>(f.line_height) / raster_scale) * 0.5f +
                       static_cast<f32>(f.ascent) / raster_scale + (pressed ? 1.0f : 0.0f);
        draw_text_cps(f, cps, tx, ty, ui_rgb(0, 0, 0), scale, raster_scale);
    }

    void set_scissor_logical(const UiRect& r, f32 scale, i32 drawable_h) {
        flush_batch();
        const i32 sx = std::max(0, static_cast<i32>(std::floor(r.x * scale)));
        const i32 sy_top = std::max(0, static_cast<i32>(std::floor(r.y * scale)));
        const i32 sw = std::max(0, static_cast<i32>(std::ceil(r.w * scale)));
        const i32 sh = std::max(0, static_cast<i32>(std::ceil(r.h * scale)));
        const i32 gl_y = std::max(0, drawable_h - (sy_top + sh));
        glEnable(GL_SCISSOR_TEST);
        glScissor(sx, gl_y, sw, sh);
    }

    void clear_scissor() {
        flush_batch();
        glDisable(GL_SCISSOR_TEST);
    }

    void compute_scale_metrics(i32 win_w, i32 win_h, i32 drawable_w, i32 drawable_h,
                               f32 display_scale, bool mobile_touch,
                               f32 need_w, f32 need_h,
                               f32& out_scale, f32& out_rscale, f32& out_log_w, f32& out_log_h,
                               f32& out_w2p_x, f32& out_w2p_y) const noexcept {
        out_w2p_x = static_cast<f32>(std::max(1, drawable_w)) / static_cast<f32>(std::max(1, win_w));
        out_w2p_y = static_cast<f32>(std::max(1, drawable_h)) / static_cast<f32>(std::max(1, win_h));
        const f32 dpi = std::max(1.0f, std::max(display_scale, std::min(out_w2p_x, out_w2p_y)));
        f32 s = mobile_touch ? (dpi * 1.15f) : dpi;
        if (need_w > 0.0f && need_h > 0.0f) {
            const f32 max_sx = static_cast<f32>(drawable_w) / std::max(1.0f, need_w);
            const f32 max_sy = static_cast<f32>(drawable_h) / std::max(1.0f, need_h);
            s = std::min(s, std::min(max_sx, max_sy));
        }
        s = std::clamp(s, 0.35f, 4.0f);
        out_scale = s;
        out_rscale = std::clamp(std::round(s * 4.0f) * 0.25f, 0.5f, 4.0f);
        out_log_w = static_cast<f32>(drawable_w) / s;
        out_log_h = static_cast<f32>(drawable_h) / s;
    }

    // ---- RTF word-wrap layout builder ----
    void ensure_rtf_layout(const RtfDocument& doc, f32 content_w, f32 raster_scale) {
        const f32 eff_w = std::max(40.0f, content_w);
        if (std::fabs(rtf_layout_width - eff_w) < 0.5f && std::fabs(rtf_layout_rscale - raster_scale) < 0.01f) {
            return;
        }
        rtf_layout_width = eff_w;
        rtf_layout_rscale = raster_scale;
        rtf_lines.clear();
        f32 cur_y = 0.0f;

        for (const auto& para : doc.paragraphs) {
            if (para.runs.empty()) {
                UiFontSpec def_spec{"Arial", 10, false, false, 1};
                UiFontFace& f = get_face(def_spec, raster_scale);
                const f32 lh = static_cast<f32>(f.line_height) / raster_scale;
                WrappedRtfLine empty_line{};
                empty_line.y = cur_y;
                empty_line.height = lh;
                empty_line.baseline = static_cast<f32>(f.ascent) / raster_scale;
                empty_line.align = para.align;
                rtf_lines.push_back(std::move(empty_line));
                cur_y += lh;
                continue;
            }

            // Tokenize runs into words / whitespace / CJK single chars so wrapping respects run boundaries.
            struct StyledToken {
                std::vector<u32> cps;
                UiFontSpec font;
                u32 color_bgr = 0;
                bool underline = false;
                bool strikeout = false;
                f32 width = 0.0f;
                f32 ascent = 10.0f;
                f32 line_h = 14.0f;
                bool is_space = false;
            };
            std::vector<StyledToken> tokens;

            for (const auto& run : para.runs) {
                UiFontSpec spec{run.font_name, run.font_size_pt, run.bold, run.italic, run.charset};
                UiFontFace& face = get_face(spec, raster_scale);
                const f32 asc = static_cast<f32>(face.ascent) / raster_scale;
                const f32 lh = static_cast<f32>(face.line_height) / raster_scale;
                const auto cps = utf8_to_cps(run.text_utf8);

                usize i = 0;
                while (i < cps.size()) {
                    StyledToken tok{};
                    tok.font = spec;
                    tok.color_bgr = run.color_bgr;
                    tok.underline = run.underline;
                    tok.strikeout = run.strikeout;
                    tok.ascent = asc;
                    tok.line_h = lh;

                    if (cps[i] == ' ' || cps[i] == '\t') {
                        tok.is_space = true;
                        while (i < cps.size() && (cps[i] == ' ' || cps[i] == '\t')) {
                            tok.cps.push_back(' ');
                            ++i;
                        }
                    } else if (is_cjk_or_breakable(cps[i])) {
                        tok.cps.push_back(cps[i++]);
                    } else {
                        while (i < cps.size() && cps[i] != ' ' && cps[i] != '\t' && !is_cjk_or_breakable(cps[i])) {
                            tok.cps.push_back(cps[i++]);
                        }
                    }
                    tok.width = measure_cps(face, tok.cps, raster_scale);
                    tokens.push_back(std::move(tok));
                }
            }

            bool first_line = true;
            WrappedRtfLine cur_line{};
            auto init_line = [&]() {
                cur_line = WrappedRtfLine{};
                cur_line.align = para.align;
                const f32 indent = static_cast<f32>(para.left_indent_px + (first_line ? para.first_indent_px : 0));
                cur_line.left_indent = std::clamp(indent, 0.0f, eff_w * 0.5f);
                cur_line.total_w = 0.0f;
                cur_line.height = 0.0f;
                cur_line.baseline = 0.0f;
            };
            auto commit_line = [&]() {
                // Trim trailing spaces from wrapped line
                while (!cur_line.segments.empty()) {
                    auto& seg = cur_line.segments.back();
                    bool all_sp = !seg.cps.empty();
                    for (u32 cp : seg.cps) if (cp != ' ') { all_sp = false; break; }
                    if (all_sp) {
                        cur_line.total_w = std::max(0.0f, cur_line.total_w - seg.width);
                        cur_line.segments.pop_back();
                    } else {
                        break;
                    }
                }
                if (cur_line.height < 8.0f) cur_line.height = 14.0f;
                if (cur_line.baseline < 4.0f) cur_line.baseline = 11.0f;
                cur_line.y = cur_y;
                cur_y += cur_line.height;
                rtf_lines.push_back(std::move(cur_line));
                first_line = false;
                init_line();
            };

            init_line();
            for (const auto& tok : tokens) {
                const f32 avail_w = std::max(24.0f, eff_w - cur_line.left_indent);

                if (cur_line.total_w > 0.0f && cur_line.total_w + tok.width > avail_w) {
                    if (tok.is_space) {
                        commit_line();
                        continue;
                    }
                    commit_line();
                }

                // If a single unbroken token still exceeds line width, split by codepoints.
                if (tok.width > std::max(24.0f, eff_w - cur_line.left_indent) && tok.cps.size() > 1) {
                    UiFontFace& face = get_face(tok.font, raster_scale);
                    usize start = 0;
                    while (start < tok.cps.size()) {
                        const f32 rem_w = std::max(16.0f, eff_w - cur_line.left_indent - cur_line.total_w);
                        usize end = start;
                        f32 w_acc = 0.0f;
                        while (end < tok.cps.size()) {
                            const f32 cw = get_glyph(face, tok.cps[end], raster_scale).advance / raster_scale;
                            if (end > start && w_acc + cw > rem_w) break;
                            w_acc += cw;
                            ++end;
                        }
                        if (end == start) ++end;
                        WrappedRtfSegment seg{};
                        seg.cps.assign(tok.cps.begin() + static_cast<std::ptrdiff_t>(start),
                                       tok.cps.begin() + static_cast<std::ptrdiff_t>(end));
                        seg.font = tok.font;
                        seg.color_bgr = tok.color_bgr;
                        seg.underline = tok.underline;
                        seg.strikeout = tok.strikeout;
                        seg.x_offset = cur_line.total_w;
                        seg.width = w_acc;
                        seg.ascent = tok.ascent;
                        seg.line_h = tok.line_h;
                        cur_line.total_w += w_acc;
                        cur_line.baseline = std::max(cur_line.baseline, tok.ascent);
                        cur_line.height = std::max(cur_line.height, tok.line_h);
                        cur_line.segments.push_back(std::move(seg));
                        start = end;
                        if (start < tok.cps.size()) {
                            commit_line();
                        }
                    }
                } else {
                    if (tok.is_space && cur_line.segments.empty() && !first_line) {
                        continue; // Skip leading whitespace on wrapped continuation lines
                    }
                    WrappedRtfSegment seg{};
                    seg.cps = tok.cps;
                    seg.font = tok.font;
                    seg.color_bgr = tok.color_bgr;
                    seg.underline = tok.underline;
                    seg.strikeout = tok.strikeout;
                    seg.x_offset = cur_line.total_w;
                    seg.width = tok.width;
                    seg.ascent = tok.ascent;
                    seg.line_h = tok.line_h;
                    cur_line.total_w += tok.width;
                    cur_line.baseline = std::max(cur_line.baseline, tok.ascent);
                    cur_line.height = std::max(cur_line.height, tok.line_h);
                    cur_line.segments.push_back(std::move(seg));
                }
            }
            if (!cur_line.segments.empty() || first_line) {
                commit_line();
            }
        }
        rtf_content_h = cur_y;
    }

    void render_rtf_region(const RtfDocument& doc, const UiRect& view_rect, f32 pad,
                           f32 scale, f32 raster_scale, i32 drawable_h, bool draw_scrollbar) {
        const f32 inner_w = view_rect.w - pad * 2.0f - (draw_scrollbar ? kUiScrollbarW : 0.0f);
        ensure_rtf_layout(doc, inner_w, raster_scale);
        const f32 view_h = std::max(1.0f, view_rect.h - pad * 2.0f);
        const f32 max_scroll = std::max(0.0f, rtf_content_h - view_h);
        scroll_y = std::clamp(scroll_y, 0.0f, max_scroll);

        const UiRect clip_r{view_rect.x, view_rect.y,
                            view_rect.w - (draw_scrollbar && max_scroll > 0.5f ? kUiScrollbarW : 0.0f),
                            view_rect.h};
        set_scissor_logical(clip_r, scale, drawable_h);

        const f32 vis_top = scroll_y - 40.0f;
        const f32 vis_bot = scroll_y + view_h + 40.0f;
        for (const auto& line : rtf_lines) {
            if (line.y + line.height < vis_top) continue;
            if (line.y > vis_bot) break;

            f32 line_x = view_rect.x + pad + line.left_indent;
            const f32 avail = std::max(1.0f, inner_w - line.left_indent);
            if (line.align == 1) {
                line_x += std::max(0.0f, (avail - line.total_w) * 0.5f);
            } else if (line.align == 2) {
                line_x += std::max(0.0f, avail - line.total_w);
            }
            const f32 base_y = view_rect.y + pad + line.y - scroll_y + line.baseline;
            for (const auto& seg : line.segments) {
                UiFontFace& face = get_face(seg.font, raster_scale);
                draw_text_cps(face, seg.cps, line_x + seg.x_offset, base_y,
                              ui_bgr(seg.color_bgr), scale, raster_scale,
                              seg.underline, seg.strikeout);
            }
        }
        clear_scissor();

        if (draw_scrollbar && max_scroll > 0.5f) {
            const UiRect sb{view_rect.x + view_rect.w - kUiScrollbarW, view_rect.y, kUiScrollbarW, view_rect.h};
            fill_rect(sb.x, sb.y, sb.w, sb.h, ui_rgb(236, 233, 216), scale);
            fill_rect(sb.x, sb.y, 1.0f, sb.h, ui_rgb(172, 168, 153), scale);

            const UiRect btn_up{sb.x, sb.y, sb.w, kUiScrollbarW};
            const UiRect btn_dn{sb.x, sb.y + sb.h - kUiScrollbarW, sb.w, kUiScrollbarW};
            fill_rect(btn_up.x, btn_up.y, btn_up.w, btn_up.h, ui_rgb(212, 208, 200), scale);
            draw_bevel(btn_up, true, scale);
            fill_rect(btn_dn.x, btn_dn.y, btn_dn.w, btn_dn.h, ui_rgb(212, 208, 200), scale);
            draw_bevel(btn_dn, true, scale);
            // Tiny up/down arrow triangles
            const UiColor arrow_c = ui_rgb(40, 40, 40);
            for (i32 r = 0; r < 4; ++r) {
                const f32 w = static_cast<f32>(1 + r * 2);
                fill_rect(btn_up.x + (btn_up.w - w) * 0.5f, btn_up.y + 5.0f + static_cast<f32>(r), w, 1.0f, arrow_c, scale);
                fill_rect(btn_dn.x + (btn_dn.w - w) * 0.5f, btn_dn.y + 10.0f - static_cast<f32>(r), w, 1.0f, arrow_c, scale);
            }

            const f32 track_y = sb.y + kUiScrollbarW;
            const f32 track_h = std::max(1.0f, sb.h - kUiScrollbarW * 2.0f);
            const f32 thumb_h = std::clamp(track_h * (view_h / std::max(view_h, rtf_content_h)), 20.0f, track_h);
            const f32 thumb_y = track_y + (track_h - thumb_h) * (scroll_y / max_scroll);
            const UiRect thumb{sb.x + 1.0f, thumb_y, sb.w - 2.0f, thumb_h};
            fill_rect(thumb.x, thumb.y, thumb.w, thumb.h,
                      sb_dragging ? ui_rgb(176, 174, 168) : ui_rgb(212, 208, 200), scale);
            draw_bevel(thumb, true, scale);
        }
    }

    // ---- Layout computations for each mode ----
    void compute_window_frame(f32 log_w, f32 log_h, f32 client_w, f32 client_h,
                              bool has_border, f32 req_x, f32 req_y,
                              UiRect& outer, UiRect& title_bar, UiRect& close_btn, UiRect& client) const noexcept {
        const f32 bw = has_border ? kUiBorder : 0.0f;
        const f32 th = has_border ? kUiTitleBarH : 0.0f;
        const f32 ow = std::min(log_w, client_w + bw * 2.0f);
        const f32 oh = std::min(log_h, client_h + th + bw * 2.0f);
        f32 ox = (req_x >= 0.0f) ? req_x : (log_w - ow) * 0.5f;
        f32 oy = (req_y >= 0.0f) ? req_y : (log_h - oh) * 0.5f;
        if (has_drag_pos) {
            ox = drag_x;
            oy = drag_y;
        }
        ox = std::clamp(ox, 0.0f, std::max(0.0f, log_w - ow));
        oy = std::clamp(oy, 0.0f, std::max(0.0f, log_h - oh));

        outer = UiRect{ox, oy, ow, oh};
        title_bar = has_border ? UiRect{ox + bw, oy + bw, ow - bw * 2.0f, th} : UiRect{};
        close_btn = has_border
            ? UiRect{title_bar.x + title_bar.w - kUiCloseW - 3.0f,
                     title_bar.y + (th - kUiCloseH) * 0.5f, kUiCloseW, kUiCloseH}
            : UiRect{};
        client = UiRect{ox + bw, oy + bw + th, std::max(1.0f, ow - bw * 2.0f),
                        std::max(1.0f, oh - th - bw * 2.0f)};
    }

    bool handle_title_bar_input(const ModalInputFrame& input, f32 mx, f32 my,
                                const UiRect& outer, const UiRect& title_bar, const UiRect& close_btn) noexcept {
        close_hovered = close_btn.w > 0.0f && close_btn.contains(mx, my);
        if (input.left_pressed) {
            if (close_hovered) {
                close_pressed = true;
                return false;
            }
            if (title_bar.w > 0.0f && title_bar.contains(mx, my)) {
                dragging_title = true;
                drag_off_x = mx - outer.x;
                drag_off_y = my - outer.y;
            }
        }
        if (dragging_title && input.left_down) {
            has_drag_pos = true;
            drag_x = mx - drag_off_x;
            drag_y = my - drag_off_y;
        }
        if (input.left_released) {
            dragging_title = false;
            if (close_pressed) {
                close_pressed = false;
                if (close_hovered) {
                    return true; // Close requested
                }
            }
        }
        return false;
    }

    // ---- Color picker layout helper ----
    struct ColorPickerLayout {
        UiRect outer, title_bar, close_btn, client;
        std::array<UiRect, 48> swatches{};
        UiRect preview_new, preview_orig;
        UiRect sv_rect, hue_rect;
        std::array<UiRect, 3> rgb_sliders{};
        UiRect ok_btn, cancel_btn;
    };

    ColorPickerLayout compute_color_picker_layout(f32 log_w, f32 log_h) const noexcept {
        ColorPickerLayout l{};
        constexpr f32 kW = 456.0f;
        constexpr f32 kH = 316.0f;
        compute_window_frame(log_w, log_h, kW, kH, true, -1.0f, -1.0f,
                             l.outer, l.title_bar, l.close_btn, l.client);
        const f32 cx = l.client.x;
        const f32 cy = l.client.y;

        // 8x6 Basic color swatches on left
        for (i32 row = 0; row < 6; ++row) {
            for (i32 col = 0; col < 8; ++col) {
                l.swatches[static_cast<usize>(row * 8 + col)] = UiRect{
                    cx + 12.0f + static_cast<f32>(col) * 25.0f,
                    cy + 28.0f + static_cast<f32>(row) * 22.0f,
                    21.0f, 18.0f,
                };
            }
        }
        l.preview_new = UiRect{cx + 12.0f, cy + 178.0f, 96.0f, 44.0f};
        l.preview_orig = UiRect{cx + 108.0f, cy + 178.0f, 96.0f, 44.0f};

        // SV square & Hue bar on right
        l.sv_rect = UiRect{cx + 224.0f, cy + 28.0f, 184.0f, 148.0f};
        l.hue_rect = UiRect{cx + 418.0f, cy + 28.0f, 22.0f, 148.0f};

        // R, G, B sliders
        for (i32 i = 0; i < 3; ++i) {
            l.rgb_sliders[static_cast<usize>(i)] = UiRect{
                cx + 244.0f, cy + 190.0f + static_cast<f32>(i) * 24.0f, 158.0f, 14.0f,
            };
        }

        l.ok_btn = UiRect{cx + kW - 172.0f, cy + kH - 36.0f, 76.0f, 24.0f};
        l.cancel_btn = UiRect{cx + kW - 88.0f, cy + kH - 36.0f, 76.0f, 24.0f};
        return l;
    }

    // ---- Menu layout helper ----
    struct MenuLayout {
        UiRect panel;
        std::vector<UiRect> row_rects;
    };

    MenuLayout compute_menu_layout(f32 log_w, f32 log_h, f32 w2p_x, f32 w2p_y, f32 scale, f32 raster_scale) {
        MenuLayout l{};
        UiFontSpec spec{"MS Sans Serif", 9, false, false, menu_params.charset};
        UiFontFace& f = get_face(spec, raster_scale);
        const f32 item_h = std::max(22.0f, static_cast<f32>(f.line_height) / raster_scale + 7.0f);
        constexpr f32 kSepH = 9.0f;

        f32 max_w = 110.0f;
        f32 total_h = 6.0f;
        for (const auto& item : menu_items) {
            if (item.is_separator) {
                total_h += kSepH;
            } else {
                max_w = std::max(max_w, measure_cps(f, item.display_cps, raster_scale) + 34.0f);
                total_h += item_h;
            }
        }
        max_w = std::min(max_w, std::max(110.0f, log_w - 8.0f));
        total_h = std::min(total_h, std::max(24.0f, log_h - 8.0f));

        f32 px = (static_cast<f32>(menu_params.anchor_win_x) * w2p_x) / scale;
        f32 py = (static_cast<f32>(menu_params.anchor_win_y) * w2p_y) / scale;
        if (px + max_w > log_w - 4.0f) px = log_w - max_w - 4.0f;
        if (py + total_h > log_h - 4.0f) py = log_h - total_h - 4.0f;
        px = std::max(4.0f, px);
        py = std::max(4.0f, py);

        l.panel = UiRect{px, py, max_w, total_h};
        l.row_rects.reserve(menu_items.size());
        f32 cy = py + 3.0f;
        for (const auto& item : menu_items) {
            const f32 h = item.is_separator ? kSepH : item_h;
            l.row_rects.push_back(UiRect{px + 3.0f, cy, max_w - 6.0f, h});
            cy += h;
        }
        return l;
    }

    // ---- Splash layout helper ----
    void compute_splash_layout(f32 log_w, f32 log_h,
                               UiRect& outer, UiRect& title_bar, UiRect& close_btn,
                               UiRect& client, UiRect& splash_x) const noexcept {
        if (splash_params.in_main_window || splash_params.fullscreen) {
            outer = UiRect{0.0f, 0.0f, log_w, log_h};
            title_bar = {};
            close_btn = {};
            client = outer;
        } else {
            f32 cw = static_cast<f32>(splash_params.size_w > 0 ? splash_params.size_w : 640);
            f32 ch = static_cast<f32>(splash_params.size_h > 0 ? splash_params.size_h : 480);
            if (splash_params.is_image && splash_params.adapt &&
                session_bitmap.width > 0 && session_bitmap.height > 0) {
                const f64 sc = (splash_params.scale > 0.0) ? splash_params.scale : 1.0;
                cw = static_cast<f32>(std::max(64.0, std::round(session_bitmap.width * sc)));
                ch = static_cast<f32>(std::max(64.0, std::round(session_bitmap.height * sc)));
            }
            const f32 bw = splash_params.show_border ? kUiBorder : 0.0f;
            const f32 th = splash_params.show_border ? kUiTitleBarH : 0.0f;
            cw = std::min(cw, std::max(64.0f, log_w - bw * 2.0f));
            ch = std::min(ch, std::max(64.0f, log_h - th - bw * 2.0f));
            compute_window_frame(log_w, log_h, cw, ch, splash_params.show_border,
                                 static_cast<f32>(splash_params.pos_x),
                                 static_cast<f32>(splash_params.pos_y),
                                 outer, title_bar, close_btn, client);
        }

        if (splash_params.close_button) {
            splash_x = UiRect{client.x + client.w - 22.0f, client.y + 4.0f, 18.0f, 18.0f};
        } else {
            splash_x = {};
        }
    }

    // ---- Game Info layout helper ----
    void compute_info_layout(f32 log_w, f32 log_h, i32 win_x, i32 win_y,
                             UiRect& outer, UiRect& title_bar, UiRect& close_btn, UiRect& client) const noexcept {
        if (info_params.mimic) {
            const f32 bw = info_params.border ? kUiBorder : 0.0f;
            const f32 th = info_params.border ? kUiTitleBarH : 0.0f;
            compute_window_frame(log_w, log_h,
                                 std::max(200.0f, log_w - bw * 2.0f),
                                 std::max(140.0f, log_h - th - bw * 2.0f),
                                 info_params.border, 0.0f, 0.0f,
                                 outer, title_bar, close_btn, client);
            return;
        }
        const f32 bw = info_params.border ? kUiBorder : 0.0f;
        const f32 th = info_params.border ? kUiTitleBarH : 0.0f;
        const f32 cw = std::clamp(static_cast<f32>(info_params.width > 0 ? info_params.width : 600),
                                  220.0f, std::max(220.0f, log_w - bw * 2.0f));
        const f32 ch = std::clamp(static_cast<f32>(info_params.height > 0 ? info_params.height : 400),
                                  160.0f, std::max(160.0f, log_h - th - bw * 2.0f));
        const f32 rx = (info_params.left >= 0) ? static_cast<f32>(info_params.left - win_x) : -1.0f;
        const f32 ry = (info_params.top >= 0) ? static_cast<f32>(info_params.top - win_y) : -1.0f;
        compute_window_frame(log_w, log_h, cw, ch, info_params.border, rx, ry,
                             outer, title_bar, close_btn, client);
    }

    // ---- Highscore layout helper ----
    void compute_highscore_layout(f32 log_w, f32 log_h,
                                  UiRect& outer, UiRect& title_bar, UiRect& close_btn, UiRect& client) const noexcept {
        f32 cw = 440.0f;
        f32 ch = 360.0f;
        if (session_bitmap.width > 0 && session_bitmap.height > 0) {
            cw = std::clamp(static_cast<f32>(session_bitmap.width), 320.0f, 800.0f);
            ch = std::clamp(static_cast<f32>(session_bitmap.height), 260.0f, 640.0f);
        }
        const f32 bw = hs_params.show_border ? kUiBorder : 0.0f;
        const f32 th = hs_params.show_border ? kUiTitleBarH : 0.0f;
        cw = std::min(cw, std::max(260.0f, log_w - bw * 2.0f));
        ch = std::min(ch, std::max(220.0f, log_h - th - bw * 2.0f));
        compute_window_frame(log_w, log_h, cw, ch, hs_params.show_border, -1.0f, -1.0f,
                             outer, title_bar, close_btn, client);
    }
};

GmModalDialogs::GmModalDialogs() : impl_(std::make_unique<Impl>()) {}

GmModalDialogs::~GmModalDialogs() {
    shutdown();
}

Result<void> GmModalDialogs::init(GlGraphicsDevice& device) {
    if (impl_->initialized) return Result<void>::success();
    impl_->device = &device;
    auto res = impl_->create_gpu_pipeline();
    if (!res) {
        impl_->destroy_gpu_resources(true);
        impl_->device = nullptr;
        return res;
    }
    impl_->initialized = true;
    return Result<void>::success();
}

void GmModalDialogs::shutdown() noexcept {
    if (!impl_) return;
    impl_->destroy_gpu_resources(true);
    impl_->device = nullptr;
    impl_->initialized = false;
    impl_->active = false;
    impl_->mode = DialogMode::None;
}

Result<void> GmModalDialogs::reload_gpu_resources() {
    if (!impl_->device) return Result<void>::success();
    impl_->destroy_gpu_resources(false);
    auto res = impl_->create_gpu_pipeline();
    impl_->initialized = static_cast<bool>(res);
    return res;
}

void GmModalDialogs::begin_highscore(const HighscoreDialogParams& params) {
    impl_->mode = DialogMode::Highscore;
    impl_->active = true;
    impl_->hs_params = params;
    impl_->hs_edit_cps.clear();
    impl_->hs_caret_reset_us = 0;
    impl_->close_hovered = impl_->close_pressed = impl_->dragging_title = impl_->has_drag_pos = false;

    if (params.bg_bitmap.has_value() && params.bg_bitmap->width > 0 && params.bg_bitmap->height > 0) {
        impl_->session_bitmap = *params.bg_bitmap;
    } else {
        impl_->session_bitmap = Bitmap32{};
    }
    impl_->upload_session_bitmap();
}

void GmModalDialogs::begin_menu(const MenuDialogParams& params) {
    impl_->mode = DialogMode::Menu;
    impl_->active = true;
    impl_->menu_params = params;
    impl_->menu_items.clear();
    impl_->menu_hover_row = -1;
    impl_->menu_result = -1;
    impl_->destroy_session_texture(true);
    impl_->session_bitmap = Bitmap32{};

    i32 next_idx = 0;
    for (const auto& raw : params.items_utf8) {
        ParsedMenuItem item{};
        if (raw == "-") {
            item.is_separator = true;
        } else {
            item.index = next_idx++;
            const auto raw_cps = utf8_to_cps(raw);
            for (usize i = 0; i < raw_cps.size(); ++i) {
                if (raw_cps[i] == '&' && i + 1 < raw_cps.size()) {
                    if (raw_cps[i + 1] == '&') {
                        item.display_cps.push_back('&');
                        ++i;
                    } else {
                        if (item.underline_idx < 0) {
                            item.underline_idx = static_cast<i32>(item.display_cps.size());
                            item.shortcut_upper = ascii_upper(raw_cps[i + 1]);
                        }
                        item.display_cps.push_back(raw_cps[i + 1]);
                        ++i;
                    }
                } else {
                    item.display_cps.push_back(raw_cps[i]);
                }
            }
        }
        impl_->menu_items.push_back(std::move(item));
    }
    if (impl_->menu_items.empty()) {
        impl_->active = false;
    }
}

void GmModalDialogs::begin_color_picker(const ColorPickerParams& params) {
    impl_->mode = DialogMode::ColorPicker;
    impl_->active = true;
    impl_->cp_initial_bgr = params.initial_color_bgr & 0xFFFFFFu;
    impl_->cp_r = static_cast<u8>(impl_->cp_initial_bgr & 0xFFu);
    impl_->cp_g = static_cast<u8>((impl_->cp_initial_bgr >> 8u) & 0xFFu);
    impl_->cp_b = static_cast<u8>((impl_->cp_initial_bgr >> 16u) & 0xFFu);
    rgb_to_hsv(impl_->cp_r, impl_->cp_g, impl_->cp_b, impl_->cp_h, impl_->cp_s, impl_->cp_v);
    impl_->cp_accepted = false;
    impl_->cp_drag_widget = 0;
    impl_->cp_ok_hover = impl_->cp_ok_press = false;
    impl_->cp_cancel_hover = impl_->cp_cancel_press = false;
    impl_->close_hovered = impl_->close_pressed = impl_->dragging_title = impl_->has_drag_pos = false;
    impl_->destroy_session_texture(true);
    impl_->session_bitmap = Bitmap32{};
}

void GmModalDialogs::begin_splash(const SplashDialogParams& params, u64 now_us) {
    impl_->mode = DialogMode::Splash;
    impl_->active = true;
    impl_->splash_params = params;
    impl_->splash_start_us = now_us;
    impl_->splash_x_hover = impl_->splash_x_press = false;
    impl_->close_hovered = impl_->close_pressed = impl_->dragging_title = impl_->has_drag_pos = false;
    impl_->rtf_layout_width = -1.0f;
    impl_->scroll_y = 0.0f;

    if (params.is_image && params.image_bitmap.width > 0 && params.image_bitmap.height > 0) {
        impl_->session_bitmap = params.image_bitmap;
    } else {
        impl_->session_bitmap = Bitmap32{};
    }
    impl_->upload_session_bitmap();
}

void GmModalDialogs::begin_game_info(const GameInfoDialogParams& params) {
    impl_->mode = DialogMode::GameInfo;
    impl_->active = true;
    impl_->info_params = params;
    impl_->rtf_layout_width = -1.0f;
    impl_->scroll_y = 0.0f;
    impl_->sb_dragging = false;
    impl_->touch_scrolling = false;
    impl_->close_hovered = impl_->close_pressed = impl_->dragging_title = impl_->has_drag_pos = false;
    impl_->destroy_session_texture(true);
    impl_->session_bitmap = Bitmap32{};
}

bool GmModalDialogs::wants_text_input() const noexcept {
    return impl_->active && impl_->mode == DialogMode::Highscore && impl_->hs_params.edit_rank >= 0;
}

std::string GmModalDialogs::result_highscore_name_utf8() const {
    return cps_to_utf8(impl_->hs_edit_cps);
}

i32 GmModalDialogs::result_menu_index() const noexcept {
    return impl_->menu_result;
}

bool GmModalDialogs::result_color_accepted() const noexcept {
    return impl_->cp_accepted;
}

u32 GmModalDialogs::result_color_bgr() const noexcept {
    return static_cast<u32>(impl_->cp_r) |
           (static_cast<u32>(impl_->cp_g) << 8u) |
           (static_cast<u32>(impl_->cp_b) << 16u);
}

bool GmModalDialogs::update(
    const ModalInputFrame& input,
    i32 win_w, i32 win_h,
    i32 drawable_w, i32 drawable_h,
    i32 win_x, i32 win_y,
    f32 display_scale,
    bool mobile_touch,
    u64 now_us) {
    if (!impl_->active || !impl_->initialized) return false;

    f32 scale = 1.0f, rscale = 1.0f, log_w = 640.0f, log_h = 480.0f, w2p_x = 1.0f, w2p_y = 1.0f;
    f32 need_w = 0.0f, need_h = 0.0f;
    if (impl_->mode == DialogMode::ColorPicker) { need_w = 472.0f; need_h = 356.0f; }
    else if (impl_->mode == DialogMode::Highscore) { need_w = 460.0f; need_h = 400.0f; }
    impl_->compute_scale_metrics(win_w, win_h, drawable_w, drawable_h, display_scale, mobile_touch,
                                 need_w, need_h, scale, rscale, log_w, log_h, w2p_x, w2p_y);

    const f32 mx = (input.mouse_x * w2p_x) / scale;
    const f32 my = (input.mouse_y * w2p_y) / scale;

    switch (impl_->mode) {
        case DialogMode::Highscore: {
            UiRect outer, title_bar, close_btn, client;
            impl_->compute_highscore_layout(log_w, log_h, outer, title_bar, close_btn, client);
            if (impl_->handle_title_bar_input(input, mx, my, outer, title_bar, close_btn)) {
                impl_->active = false;
                return false;
            }
            for (const auto& ev : input.keyboard_events) {
                if (const auto* ke = std::get_if<ModalKeyEvent>(&ev)) {
                    if (ke->vk == 27 || ke->vk == 13) { // Escape or Enter (TScoreForm.FormKeyPress)
                        impl_->active = false;
                        return false;
                    }
                    if (ke->vk == 8 && impl_->hs_params.edit_rank >= 0 && !impl_->hs_edit_cps.empty()) {
                        impl_->hs_edit_cps.pop_back();
                        impl_->hs_caret_reset_us = now_us;
                    }
                } else if (const auto* txt = std::get_if<std::string>(&ev)) {
                    if (impl_->hs_params.edit_rank >= 0) {
                        for (u32 cp : utf8_to_cps(*txt)) {
                            if (cp >= 32u && impl_->hs_edit_cps.size() < 40u) {
                                impl_->hs_edit_cps.push_back(cp);
                            }
                        }
                        impl_->hs_caret_reset_us = now_us;
                    }
                }
            }
            return impl_->active;
        }

        case DialogMode::Menu: {
            const auto ml = impl_->compute_menu_layout(log_w, log_h, w2p_x, w2p_y, scale, rscale);
            if (input.mouse_moved || input.left_pressed) {
                i32 hov = -1;
                for (usize i = 0; i < ml.row_rects.size(); ++i) {
                    if (!impl_->menu_items[i].is_separator && ml.row_rects[i].contains(mx, my)) {
                        hov = static_cast<i32>(i);
                        break;
                    }
                }
                if (hov >= 0 || ml.panel.contains(mx, my)) {
                    impl_->menu_hover_row = hov;
                }
            }

            if (input.any_mouse_pressed) {
                if (!ml.panel.contains(mx, my)) {
                    impl_->menu_result = -1;
                    impl_->active = false;
                    return false;
                }
            }
            if (input.left_released) {
                for (usize i = 0; i < ml.row_rects.size(); ++i) {
                    if (!impl_->menu_items[i].is_separator && ml.row_rects[i].contains(mx, my)) {
                        impl_->menu_result = impl_->menu_items[i].index;
                        impl_->active = false;
                        return false;
                    }
                }
            }

            for (const auto& ev : input.keyboard_events) {
                if (const auto* ke = std::get_if<ModalKeyEvent>(&ev)) {
                    if (ke->vk == 27) { // Escape
                        impl_->menu_result = -1;
                        impl_->active = false;
                        return false;
                    }
                    if (ke->vk == 13 && impl_->menu_hover_row >= 0 &&
                        static_cast<usize>(impl_->menu_hover_row) < impl_->menu_items.size() &&
                        !impl_->menu_items[static_cast<usize>(impl_->menu_hover_row)].is_separator) {
                        impl_->menu_result = impl_->menu_items[static_cast<usize>(impl_->menu_hover_row)].index;
                        impl_->active = false;
                        return false;
                    }
                    if (ke->vk == 38 || ke->vk == 40) { // Up / Down
                        const i32 dir = (ke->vk == 40) ? 1 : -1;
                        const i32 cnt = static_cast<i32>(impl_->menu_items.size());
                        i32 cur = impl_->menu_hover_row;
                        for (i32 step = 0; step < cnt; ++step) {
                            cur = (cur < 0) ? (dir > 0 ? 0 : cnt - 1) : ((cur + dir + cnt) % cnt);
                            if (!impl_->menu_items[static_cast<usize>(cur)].is_separator) {
                                impl_->menu_hover_row = cur;
                                break;
                            }
                        }
                    } else if ((ke->vk >= '0' && ke->vk <= '9') || (ke->vk >= 'A' && ke->vk <= 'Z')) {
                        const u32 ch = static_cast<u32>(ke->vk);
                        for (const auto& item : impl_->menu_items) {
                            if (!item.is_separator && item.shortcut_upper == ch) {
                                impl_->menu_result = item.index;
                                impl_->active = false;
                                return false;
                            }
                        }
                    }
                }
            }
            return impl_->active;
        }

        case DialogMode::ColorPicker: {
            const auto cl = impl_->compute_color_picker_layout(log_w, log_h);
            if (impl_->handle_title_bar_input(input, mx, my, cl.outer, cl.title_bar, cl.close_btn)) {
                impl_->cp_accepted = false;
                impl_->active = false;
                return false;
            }

            impl_->cp_ok_hover = cl.ok_btn.contains(mx, my);
            impl_->cp_cancel_hover = cl.cancel_btn.contains(mx, my);

            auto update_from_sv = [&](f32 px, f32 py) {
                impl_->cp_s = std::clamp((px - cl.sv_rect.x) / std::max(1.0f, cl.sv_rect.w), 0.0f, 1.0f);
                impl_->cp_v = 1.0f - std::clamp((py - cl.sv_rect.y) / std::max(1.0f, cl.sv_rect.h), 0.0f, 1.0f);
                hsv_to_rgb(impl_->cp_h, impl_->cp_s, impl_->cp_v, impl_->cp_r, impl_->cp_g, impl_->cp_b);
            };
            auto update_from_hue = [&](f32 py) {
                const f32 t = std::clamp((py - cl.hue_rect.y) / std::max(1.0f, cl.hue_rect.h), 0.0f, 1.0f);
                impl_->cp_h = t * 360.0f;
                hsv_to_rgb(impl_->cp_h, impl_->cp_s, impl_->cp_v, impl_->cp_r, impl_->cp_g, impl_->cp_b);
            };
            auto update_from_rgb_slider = [&](i32 idx, f32 px) {
                const auto& sr = cl.rgb_sliders[static_cast<usize>(idx)];
                const f32 t = std::clamp((px - sr.x) / std::max(1.0f, sr.w), 0.0f, 1.0f);
                const u8 val = static_cast<u8>(std::round(t * 255.0f));
                if (idx == 0) impl_->cp_r = val;
                else if (idx == 1) impl_->cp_g = val;
                else impl_->cp_b = val;
                rgb_to_hsv(impl_->cp_r, impl_->cp_g, impl_->cp_b, impl_->cp_h, impl_->cp_s, impl_->cp_v);
            };

            if (input.left_pressed) {
                if (impl_->cp_ok_hover) {
                    impl_->cp_ok_press = true;
                } else if (impl_->cp_cancel_hover) {
                    impl_->cp_cancel_press = true;
                } else if (cl.sv_rect.contains(mx, my)) {
                    impl_->cp_drag_widget = 1;
                    update_from_sv(mx, my);
                } else if (cl.hue_rect.contains(mx, my)) {
                    impl_->cp_drag_widget = 2;
                    update_from_hue(my);
                } else if (cl.preview_orig.contains(mx, my)) {
                    impl_->cp_r = static_cast<u8>(impl_->cp_initial_bgr & 0xFFu);
                    impl_->cp_g = static_cast<u8>((impl_->cp_initial_bgr >> 8u) & 0xFFu);
                    impl_->cp_b = static_cast<u8>((impl_->cp_initial_bgr >> 16u) & 0xFFu);
                    rgb_to_hsv(impl_->cp_r, impl_->cp_g, impl_->cp_b, impl_->cp_h, impl_->cp_s, impl_->cp_v);
                } else {
                    for (i32 i = 0; i < 3; ++i) {
                        UiRect hit = cl.rgb_sliders[static_cast<usize>(i)];
                        hit.y -= 3.0f;
                        hit.h += 6.0f;
                        if (hit.contains(mx, my)) {
                            impl_->cp_drag_widget = 3 + i;
                            update_from_rgb_slider(i, mx);
                            break;
                        }
                    }
                    if (impl_->cp_drag_widget == 0) {
                        for (usize i = 0; i < cl.swatches.size(); ++i) {
                            if (cl.swatches[i].contains(mx, my)) {
                                const u32 rgb = kBasicColorsRgb[i];
                                impl_->cp_r = static_cast<u8>((rgb >> 16u) & 0xFFu);
                                impl_->cp_g = static_cast<u8>((rgb >> 8u) & 0xFFu);
                                impl_->cp_b = static_cast<u8>(rgb & 0xFFu);
                                rgb_to_hsv(impl_->cp_r, impl_->cp_g, impl_->cp_b, impl_->cp_h, impl_->cp_s, impl_->cp_v);
                                if (input.left_double_click) {
                                    impl_->cp_accepted = true;
                                    impl_->active = false;
                                    return false;
                                }
                                break;
                            }
                        }
                    }
                }
            }

            if (input.left_down && impl_->cp_drag_widget != 0) {
                if (impl_->cp_drag_widget == 1) update_from_sv(mx, my);
                else if (impl_->cp_drag_widget == 2) update_from_hue(my);
                else if (impl_->cp_drag_widget >= 3 && impl_->cp_drag_widget <= 5) {
                    update_from_rgb_slider(impl_->cp_drag_widget - 3, mx);
                }
            }

            if (input.left_released) {
                impl_->cp_drag_widget = 0;
                if (impl_->cp_ok_press) {
                    impl_->cp_ok_press = false;
                    if (impl_->cp_ok_hover) {
                        impl_->cp_accepted = true;
                        impl_->active = false;
                        return false;
                    }
                }
                if (impl_->cp_cancel_press) {
                    impl_->cp_cancel_press = false;
                    if (impl_->cp_cancel_hover) {
                        impl_->cp_accepted = false;
                        impl_->active = false;
                        return false;
                    }
                }
            }

            for (const auto& ev : input.keyboard_events) {
                if (const auto* ke = std::get_if<ModalKeyEvent>(&ev)) {
                    if (ke->vk == 13) {
                        impl_->cp_accepted = true;
                        impl_->active = false;
                        return false;
                    }
                    if (ke->vk == 27) {
                        impl_->cp_accepted = false;
                        impl_->active = false;
                        return false;
                    }
                }
            }
            return impl_->active;
        }

        case DialogMode::Splash: {
            if (impl_->splash_params.delay_ms > 0 && now_us >= impl_->splash_start_us) {
                const u64 elapsed_us = now_us - impl_->splash_start_us;
                if (elapsed_us >= static_cast<u64>(impl_->splash_params.delay_ms) * 1000u) {
                    impl_->active = false;
                    return false;
                }
            }

            UiRect outer, title_bar, close_btn, client, splash_x;
            impl_->compute_splash_layout(log_w, log_h, outer, title_bar, close_btn, client, splash_x);

            if (title_bar.w > 0.0f) {
                if (impl_->handle_title_bar_input(input, mx, my, outer, title_bar, close_btn)) {
                    impl_->active = false;
                    return false;
                }
            }

            impl_->splash_x_hover = splash_x.w > 0.0f && splash_x.contains(mx, my);
            if (input.left_pressed && impl_->splash_x_hover) {
                impl_->splash_x_press = true;
            }
            if (input.left_released && impl_->splash_x_press) {
                impl_->splash_x_press = false;
                if (impl_->splash_x_hover) {
                    impl_->active = false;
                    return false;
                }
            }

            if (impl_->splash_params.stop_mouse && input.any_mouse_pressed &&
                !impl_->splash_x_hover && !(title_bar.w > 0.0f && title_bar.contains(mx, my)) &&
                client.contains(mx, my)) {
                impl_->active = false;
                return false;
            }

            if (std::fabs(input.wheel_y) > 1e-4f && !impl_->splash_params.is_image) {
                impl_->scroll_y -= input.wheel_y * 42.0f;
            }

            for (const auto& ev : input.keyboard_events) {
                if (const auto* ke = std::get_if<ModalKeyEvent>(&ev)) {
                    if (ke->vk == 27 && impl_->splash_params.stop_key) {
                        impl_->active = false;
                        return false;
                    }
                }
            }
            return impl_->active;
        }

        case DialogMode::GameInfo: {
            UiRect outer, title_bar, close_btn, client;
            impl_->compute_info_layout(log_w, log_h, win_x, win_y, outer, title_bar, close_btn, client);
            if (impl_->handle_title_bar_input(input, mx, my, outer, title_bar, close_btn)) {
                impl_->active = false;
                return false;
            }

            constexpr f32 kPad = 10.0f;
            const f32 view_h = std::max(1.0f, client.h - kPad * 2.0f);
            impl_->ensure_rtf_layout(impl_->info_params.doc, client.w - kPad * 2.0f - kUiScrollbarW, rscale);
            const f32 max_scroll = std::max(0.0f, impl_->rtf_content_h - view_h);

            if (std::fabs(input.wheel_y) > 1e-4f) {
                impl_->scroll_y = std::clamp(impl_->scroll_y - input.wheel_y * 48.0f, 0.0f, max_scroll);
            }

            // Scrollbar mouse / touch interaction
            if (max_scroll > 0.5f) {
                const UiRect sb{client.x + client.w - kUiScrollbarW, client.y, kUiScrollbarW, client.h};
                const f32 track_y = sb.y + kUiScrollbarW;
                const f32 track_h = std::max(1.0f, sb.h - kUiScrollbarW * 2.0f);
                const f32 thumb_h = std::clamp(track_h * (view_h / std::max(view_h, impl_->rtf_content_h)), 20.0f, track_h);
                const f32 thumb_y = track_y + (track_h - thumb_h) * (impl_->scroll_y / max_scroll);
                const UiRect thumb{sb.x, thumb_y, sb.w, thumb_h};

                if (input.left_pressed) {
                    if (sb.contains(mx, my)) {
                        if (my < track_y) {
                            impl_->scroll_y = std::max(0.0f, impl_->scroll_y - 36.0f);
                        } else if (my >= sb.y + sb.h - kUiScrollbarW) {
                            impl_->scroll_y = std::min(max_scroll, impl_->scroll_y + 36.0f);
                        } else if (thumb.contains(mx, my)) {
                            impl_->sb_dragging = true;
                            impl_->sb_drag_offset = my - thumb_y;
                        } else if (my < thumb_y) {
                            impl_->scroll_y = std::max(0.0f, impl_->scroll_y - view_h * 0.85f);
                        } else {
                            impl_->scroll_y = std::min(max_scroll, impl_->scroll_y + view_h * 0.85f);
                        }
                    } else if (input.pointer_is_touch && client.contains(mx, my)) {
                        impl_->touch_scrolling = true;
                        impl_->touch_last_y = my;
                    }
                }
                if (input.left_down) {
                    if (impl_->sb_dragging && track_h > thumb_h + 0.5f) {
                        const f32 t = std::clamp((my - impl_->sb_drag_offset - track_y) / (track_h - thumb_h), 0.0f, 1.0f);
                        impl_->scroll_y = t * max_scroll;
                    } else if (impl_->touch_scrolling) {
                        impl_->scroll_y = std::clamp(impl_->scroll_y - (my - impl_->touch_last_y), 0.0f, max_scroll);
                        impl_->touch_last_y = my;
                    }
                }
                if (input.left_released) {
                    impl_->sb_dragging = false;
                    impl_->touch_scrolling = false;
                }
            }

            for (const auto& ev : input.keyboard_events) {
                if (const auto* ke = std::get_if<ModalKeyEvent>(&ev)) {
                    if (ke->vk == 27) { // Escape (THelpForm.FormKeyDown)
                        impl_->active = false;
                        return false;
                    }
                    if (ke->vk == 38) impl_->scroll_y = std::max(0.0f, impl_->scroll_y - 28.0f);
                    else if (ke->vk == 40) impl_->scroll_y = std::min(max_scroll, impl_->scroll_y + 28.0f);
                    else if (ke->vk == 33) impl_->scroll_y = std::max(0.0f, impl_->scroll_y - view_h * 0.85f);
                    else if (ke->vk == 34) impl_->scroll_y = std::min(max_scroll, impl_->scroll_y + view_h * 0.85f);
                    else if (ke->vk == 36) impl_->scroll_y = 0.0f;
                    else if (ke->vk == 35) impl_->scroll_y = max_scroll;
                }
            }
            return impl_->active;
        }

        default:
            return false;
    }
}

void GmModalDialogs::render(
    i32 win_w, i32 win_h,
    i32 drawable_w, i32 drawable_h,
    i32 win_x, i32 win_y,
    f32 display_scale,
    bool mobile_touch,
    u64 now_us) {
    if (!impl_->active || !impl_->initialized || !impl_->device || !impl_->shader.valid()) {
        return;
    }

    f32 scale = 1.0f, rscale = 1.0f, log_w = 640.0f, log_h = 480.0f, w2p_x = 1.0f, w2p_y = 1.0f;
    f32 need_w = 0.0f, need_h = 0.0f;
    if (impl_->mode == DialogMode::ColorPicker) { need_w = 472.0f; need_h = 356.0f; }
    else if (impl_->mode == DialogMode::Highscore) { need_w = 460.0f; need_h = 400.0f; }
    impl_->compute_scale_metrics(win_w, win_h, drawable_w, drawable_h, display_scale, mobile_touch,
                                 need_w, need_h, scale, rscale, log_w, log_h, w2p_x, w2p_y);

    const auto saved = impl_->device->save_render_state();
    impl_->device->bind_framebuffer(FramebufferHandle{});
    impl_->device->set_viewport(ViewportRect{0, 0, drawable_w, drawable_h});
    impl_->device->set_scissor(false);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    impl_->device->bind_shader(impl_->shader);
    const f32 proj[16] = {
        2.0f / static_cast<f32>(std::max(1, drawable_w)), 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f / static_cast<f32>(std::max(1, drawable_h)), 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f,
    };
    glUniformMatrix4fv(impl_->loc_proj, 1, GL_FALSE, proj);
    glUniform1i(impl_->loc_tex, 0);

    impl_->verts.clear();
    impl_->batch_tex = {};

    switch (impl_->mode) {
        case DialogMode::Highscore: {
            UiRect outer, title_bar, close_btn, client;
            impl_->compute_highscore_layout(log_w, log_h, outer, title_bar, close_btn, client);

            // Subtle modal backdrop scrim + drop shadow
            impl_->fill_rect(0.0f, 0.0f, log_w, log_h, ui_rgb(0, 0, 0, 0.25f), scale);
            impl_->fill_rect(outer.x + 4.0f, outer.y + 4.0f, outer.w, outer.h, ui_rgb(0, 0, 0, 0.35f), scale);

            if (impl_->hs_params.show_border) {
                impl_->fill_rect(outer.x, outer.y, outer.w, outer.h, ui_rgb(212, 208, 200), scale);
                impl_->draw_bevel(outer, true, scale);
                impl_->draw_title_bar(title_bar, close_btn, impl_->hs_params.caption_utf8, scale, rscale);
            }

            if (impl_->session_tex.valid()) {
                impl_->draw_tex_rect(impl_->session_tex, client.x, client.y, client.w, client.h, ui_rgb(255, 255, 255), scale);
            } else {
                impl_->fill_rect(client.x, client.y, client.w, client.h, ui_bgr(impl_->hs_params.col_back_bgr), scale);
                if (!impl_->hs_params.show_border) {
                    impl_->draw_bevel(client, false, scale);
                }
            }

            const bool bold = (impl_->hs_params.font_style & 1) != 0;
            const bool italic = (impl_->hs_params.font_style & 2) != 0;
            const bool underline = (impl_->hs_params.font_style & 4) != 0;
            const bool strikeout = (impl_->hs_params.font_style & 8) != 0;
            UiFontSpec row_spec{
                impl_->hs_params.font_name.empty() ? std::string("Arial") : impl_->hs_params.font_name,
                std::clamp(impl_->hs_params.font_size, 6, 48),
                bold, italic, impl_->hs_params.charset,
            };
            UiFontFace& rf = impl_->get_face(row_spec, rscale);
            const f32 row_lh = static_cast<f32>(rf.line_height) / rscale;
            const f32 row_asc = static_cast<f32>(rf.ascent) / rscale;

            UiFontSpec esc_spec{"Arial", 9, false, false, impl_->hs_params.charset};
            UiFontFace& ef = impl_->get_face(esc_spec, rscale);
            const f32 footer_h = static_cast<f32>(ef.line_height) / rscale + 10.0f;

            const f32 list_top = client.y + 14.0f;
            const f32 list_h = std::max(100.0f, client.h - 22.0f - footer_h);
            const f32 step_y = list_h / 10.0f;
            const f32 rank_right_x = client.x + 48.0f;
            const f32 name_left_x = client.x + 60.0f;
            const f32 score_right_x = client.x + client.w - 20.0f;

            const bool blink_on = (((now_us - impl_->hs_caret_reset_us) / 530000u) % 2u) == 0u;

            for (i32 i = 0; i < 10; ++i) {
                const bool is_new = (i == impl_->hs_params.edit_rank);
                const UiColor col = ui_bgr(is_new ? impl_->hs_params.col_new_bgr : impl_->hs_params.col_other_bgr);
                const f32 row_y = list_top + static_cast<f32>(i) * step_y;
                const f32 base_y = row_y + (step_y - row_lh) * 0.5f + row_asc;

                const auto rank_cps = utf8_to_cps(std::to_string(i + 1) + ".");
                const f32 rank_w = impl_->measure_cps(rf, rank_cps, rscale);
                impl_->draw_text_cps(rf, rank_cps, rank_right_x - rank_w, base_y, col, scale, rscale, underline, strikeout);

                const auto score_cps = utf8_to_cps(std::to_string(impl_->hs_params.rows[static_cast<usize>(i)].score));
                const f32 score_w = impl_->measure_cps(rf, score_cps, rscale);
                impl_->draw_text_cps(rf, score_cps, score_right_x - score_w, base_y, col, scale, rscale, underline, strikeout);

                std::vector<u32> name_cps;
                if (is_new) {
                    name_cps = impl_->hs_edit_cps;
                    if (blink_on) name_cps.push_back('|');
                } else {
                    name_cps = utf8_to_cps(impl_->hs_params.rows[static_cast<usize>(i)].name_utf8);
                }

                // Clip name column so long names never collide with the score column.
                const f32 name_max_w = std::max(24.0f, score_right_x - score_w - 14.0f - name_left_x);
                impl_->set_scissor_logical(UiRect{name_left_x, row_y, name_max_w, step_y}, scale, drawable_h);
                impl_->draw_text_cps(rf, name_cps, name_left_x, base_y, col, scale, rscale, underline, strikeout);
                impl_->clear_scissor();
            }

            if (!impl_->hs_params.escape_text_utf8.empty()) {
                const auto esc_cps = utf8_to_cps(impl_->hs_params.escape_text_utf8);
                const f32 ew = impl_->measure_cps(ef, esc_cps, rscale);
                const f32 ex = client.x + (client.w - ew) * 0.5f;
                const f32 ey = client.y + client.h - 10.0f - static_cast<f32>(ef.descent) / rscale;
                impl_->draw_text_cps(ef, esc_cps, ex, ey, ui_bgr(impl_->hs_params.col_other_bgr), scale, rscale);
            }
            break;
        }

        case DialogMode::Menu: {
            const auto ml = impl_->compute_menu_layout(log_w, log_h, w2p_x, w2p_y, scale, rscale);
            impl_->fill_rect(ml.panel.x + 3.0f, ml.panel.y + 3.0f, ml.panel.w, ml.panel.h, ui_rgb(0, 0, 0, 0.30f), scale);
            impl_->fill_rect(ml.panel.x, ml.panel.y, ml.panel.w, ml.panel.h, ui_rgb(240, 240, 240), scale);
            impl_->draw_bevel(ml.panel, true, scale);

            UiFontSpec spec{"MS Sans Serif", 9, false, false, impl_->menu_params.charset};
            UiFontFace& f = impl_->get_face(spec, rscale);
            const f32 lh = static_cast<f32>(f.line_height) / rscale;
            const f32 asc = static_cast<f32>(f.ascent) / rscale;

            for (usize i = 0; i < impl_->menu_items.size(); ++i) {
                const auto& item = impl_->menu_items[i];
                const auto& rr = ml.row_rects[i];
                if (item.is_separator) {
                    const f32 sy = rr.y + rr.h * 0.5f;
                    impl_->fill_rect(rr.x + 3.0f, sy - 1.0f, rr.w - 6.0f, 1.0f, ui_rgb(160, 160, 160), scale);
                    impl_->fill_rect(rr.x + 3.0f, sy, rr.w - 6.0f, 1.0f, ui_rgb(255, 255, 255), scale);
                } else {
                    const bool hi = (static_cast<i32>(i) == impl_->menu_hover_row);
                    if (hi) {
                        impl_->fill_rect(rr.x, rr.y, rr.w, rr.h, ui_rgb(10, 36, 106), scale);
                    }
                    const f32 base_y = rr.y + (rr.h - lh) * 0.5f + asc;
                    impl_->draw_text_cps(f, item.display_cps, rr.x + 14.0f, base_y,
                                         hi ? ui_rgb(255, 255, 255) : ui_rgb(0, 0, 0),
                                         scale, rscale, false, false, item.underline_idx);
                }
            }
            break;
        }

        case DialogMode::ColorPicker: {
            const auto cl = impl_->compute_color_picker_layout(log_w, log_h);
            impl_->fill_rect(0.0f, 0.0f, log_w, log_h, ui_rgb(0, 0, 0, 0.25f), scale);
            impl_->fill_rect(cl.outer.x + 4.0f, cl.outer.y + 4.0f, cl.outer.w, cl.outer.h, ui_rgb(0, 0, 0, 0.35f), scale);
            impl_->fill_rect(cl.outer.x, cl.outer.y, cl.outer.w, cl.outer.h, ui_rgb(212, 208, 200), scale);
            impl_->draw_bevel(cl.outer, true, scale);
            impl_->draw_title_bar(cl.title_bar, cl.close_btn, "Color", scale, rscale);

            UiFontSpec lbl_spec{"MS Sans Serif", 9, false, false, 1};
            UiFontFace& lf = impl_->get_face(lbl_spec, rscale);
            const f32 lbl_asc = static_cast<f32>(lf.ascent) / rscale;
            const UiColor black = ui_rgb(0, 0, 0);

            impl_->draw_text_cps(lf, utf8_to_cps("Basic colors:"), cl.client.x + 12.0f, cl.client.y + 10.0f + lbl_asc, black, scale, rscale);

            // 48 Basic color swatches
            const u32 cur_rgb = (static_cast<u32>(impl_->cp_r) << 16u) |
                                (static_cast<u32>(impl_->cp_g) << 8u) |
                                static_cast<u32>(impl_->cp_b);
            for (usize i = 0; i < cl.swatches.size(); ++i) {
                const auto& r = cl.swatches[i];
                const u32 rgb = kBasicColorsRgb[i];
                impl_->fill_rect(r.x, r.y, r.w, r.h,
                                 ui_rgb(static_cast<u8>((rgb >> 16u) & 0xFFu),
                                        static_cast<u8>((rgb >> 8u) & 0xFFu),
                                        static_cast<u8>(rgb & 0xFFu)),
                                 scale);
                impl_->draw_bevel(r, false, scale);
                if (rgb == cur_rgb) {
                    const UiRect ring{r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f};
                    impl_->fill_rect(ring.x, ring.y, ring.w, 1.0f, black, scale);
                    impl_->fill_rect(ring.x, ring.y + ring.h - 1.0f, ring.w, 1.0f, black, scale);
                    impl_->fill_rect(ring.x, ring.y, 1.0f, ring.h, black, scale);
                    impl_->fill_rect(ring.x + ring.w - 1.0f, ring.y, 1.0f, ring.h, black, scale);
                }
            }

            // Preview swatch (New | Original)
            impl_->draw_text_cps(lf, utf8_to_cps("New / Original:"), cl.preview_new.x, cl.preview_new.y - 14.0f + lbl_asc, black, scale, rscale);
            impl_->fill_rect(cl.preview_new.x, cl.preview_new.y, cl.preview_new.w, cl.preview_new.h,
                             ui_rgb(impl_->cp_r, impl_->cp_g, impl_->cp_b), scale);
            impl_->fill_rect(cl.preview_orig.x, cl.preview_orig.y, cl.preview_orig.w, cl.preview_orig.h,
                             ui_bgr(impl_->cp_initial_bgr), scale);
            impl_->draw_bevel(UiRect{cl.preview_new.x, cl.preview_new.y,
                                     cl.preview_new.w + cl.preview_orig.w, cl.preview_new.h}, false, scale);

            char hex_buf[64];
            std::snprintf(hex_buf, sizeof(hex_buf), "#%02X%02X%02X   RGB(%u, %u, %u)",
                          impl_->cp_r, impl_->cp_g, impl_->cp_b, impl_->cp_r, impl_->cp_g, impl_->cp_b);
            impl_->draw_text_cps(lf, utf8_to_cps(hex_buf), cl.preview_new.x, cl.preview_new.y + cl.preview_new.h + 8.0f + lbl_asc, black, scale, rscale);

            // Smooth 16x16 HSV Saturation-Value square
            constexpr i32 kSteps = 16;
            const f32 cell_w = cl.sv_rect.w / static_cast<f32>(kSteps);
            const f32 cell_h = cl.sv_rect.h / static_cast<f32>(kSteps);
            for (i32 gy = 0; gy < kSteps; ++gy) {
                const f32 v0 = 1.0f - static_cast<f32>(gy) / static_cast<f32>(kSteps);
                const f32 v1 = 1.0f - static_cast<f32>(gy + 1) / static_cast<f32>(kSteps);
                const f32 py0 = (cl.sv_rect.y + static_cast<f32>(gy) * cell_h) * scale;
                const f32 py1 = (cl.sv_rect.y + static_cast<f32>(gy + 1) * cell_h) * scale;
                for (i32 gx = 0; gx < kSteps; ++gx) {
                    const f32 s0 = static_cast<f32>(gx) / static_cast<f32>(kSteps);
                    const f32 s1 = static_cast<f32>(gx + 1) / static_cast<f32>(kSteps);
                    const f32 px0 = (cl.sv_rect.x + static_cast<f32>(gx) * cell_w) * scale;
                    const f32 px1 = (cl.sv_rect.x + static_cast<f32>(gx + 1) * cell_w) * scale;
                    u8 r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
                    hsv_to_rgb(impl_->cp_h, s0, v0, r0, g0, b0);
                    hsv_to_rgb(impl_->cp_h, s1, v0, r1, g1, b1);
                    hsv_to_rgb(impl_->cp_h, s1, v1, r2, g2, b2);
                    hsv_to_rgb(impl_->cp_h, s0, v1, r3, g3, b3);
                    impl_->push_quad(impl_->white_tex,
                                     px0, py0, 0, 0, ui_rgb(r0, g0, b0),
                                     px1, py0, 1, 0, ui_rgb(r1, g1, b1),
                                     px1, py1, 1, 1, ui_rgb(r2, g2, b2),
                                     px0, py1, 0, 1, ui_rgb(r3, g3, b3));
                }
            }
            impl_->draw_bevel(cl.sv_rect, false, scale);

            // SV crosshair
            const f32 sv_cx = cl.sv_rect.x + impl_->cp_s * cl.sv_rect.w;
            const f32 sv_cy = cl.sv_rect.y + (1.0f - impl_->cp_v) * cl.sv_rect.h;
            const UiColor cross_col = (impl_->cp_v > 0.55f && impl_->cp_s < 0.55f) ? black : ui_rgb(255, 255, 255);
            impl_->fill_rect(sv_cx - 6.0f, sv_cy - 1.0f, 4.0f, 2.0f, cross_col, scale);
            impl_->fill_rect(sv_cx + 2.0f, sv_cy - 1.0f, 4.0f, 2.0f, cross_col, scale);
            impl_->fill_rect(sv_cx - 1.0f, sv_cy - 6.0f, 2.0f, 4.0f, cross_col, scale);
            impl_->fill_rect(sv_cx - 1.0f, sv_cy + 2.0f, 2.0f, 4.0f, cross_col, scale);

            // Vertical Hue spectrum bar (6 segments)
            static constexpr std::array<u8, 21> kHueStops = {
                255,0,0,  255,255,0,  0,255,0,  0,255,255,  0,0,255,  255,0,255,  255,0,0
            };
            const f32 seg_h = cl.hue_rect.h / 6.0f;
            for (i32 s = 0; s < 6; ++s) {
                const UiColor top = ui_rgb(kHueStops[static_cast<usize>(s * 3)],
                                           kHueStops[static_cast<usize>(s * 3 + 1)],
                                           kHueStops[static_cast<usize>(s * 3 + 2)]);
                const UiColor bot = ui_rgb(kHueStops[static_cast<usize>((s + 1) * 3)],
                                           kHueStops[static_cast<usize>((s + 1) * 3 + 1)],
                                           kHueStops[static_cast<usize>((s + 1) * 3 + 2)]);
                impl_->fill_rect_grad_v(cl.hue_rect.x, cl.hue_rect.y + static_cast<f32>(s) * seg_h,
                                        cl.hue_rect.w, seg_h + 0.5f, top, bot, scale);
            }
            impl_->draw_bevel(cl.hue_rect, false, scale);
            const f32 hy = cl.hue_rect.y + std::clamp(impl_->cp_h / 360.0f, 0.0f, 1.0f) * cl.hue_rect.h;
            impl_->fill_rect(cl.hue_rect.x - 2.0f, hy - 2.0f, cl.hue_rect.w + 4.0f, 4.0f, black, scale);
            impl_->fill_rect(cl.hue_rect.x - 1.0f, hy - 1.0f, cl.hue_rect.w + 2.0f, 2.0f, ui_rgb(255, 255, 255), scale);

            // R, G, B sliders
            const char* ch_lbls[3] = {"R:", "G:", "B:"};
            const u8 ch_vals[3] = {impl_->cp_r, impl_->cp_g, impl_->cp_b};
            for (i32 i = 0; i < 3; ++i) {
                const auto& sr = cl.rgb_sliders[static_cast<usize>(i)];
                impl_->draw_text_cps(lf, utf8_to_cps(ch_lbls[i]), sr.x - 18.0f, sr.y + lbl_asc - 1.0f, black, scale, rscale);
                const UiColor c_lo = ui_rgb(i == 0 ? 0 : impl_->cp_r, i == 1 ? 0 : impl_->cp_g, i == 2 ? 0 : impl_->cp_b);
                const UiColor c_hi = ui_rgb(i == 0 ? 255 : impl_->cp_r, i == 1 ? 255 : impl_->cp_g, i == 2 ? 255 : impl_->cp_b);
                impl_->fill_rect_grad_h(sr.x, sr.y, sr.w, sr.h, c_lo, c_hi, scale);
                impl_->draw_bevel(sr, false, scale);

                const f32 tx = sr.x + (static_cast<f32>(ch_vals[i]) / 255.0f) * sr.w;
                impl_->fill_rect(tx - 3.0f, sr.y - 2.0f, 6.0f, sr.h + 4.0f, ui_rgb(212, 208, 200), scale);
                impl_->draw_bevel(UiRect{tx - 3.0f, sr.y - 2.0f, 6.0f, sr.h + 4.0f}, true, scale);

                impl_->draw_text_cps(lf, utf8_to_cps(std::to_string(ch_vals[i])),
                                     sr.x + sr.w + 8.0f, sr.y + lbl_asc - 1.0f, black, scale, rscale);
            }

            impl_->draw_button(cl.ok_btn, "OK", impl_->cp_ok_hover, impl_->cp_ok_press, scale, rscale);
            impl_->draw_button(cl.cancel_btn, "Cancel", impl_->cp_cancel_hover, impl_->cp_cancel_press, scale, rscale);
            break;
        }

        case DialogMode::Splash: {
            UiRect outer, title_bar, close_btn, client, splash_x;
            impl_->compute_splash_layout(log_w, log_h, outer, title_bar, close_btn, client, splash_x);

            if (!impl_->splash_params.in_main_window && !impl_->splash_params.fullscreen) {
                impl_->fill_rect(0.0f, 0.0f, log_w, log_h, ui_rgb(0, 0, 0, 0.30f), scale);
                impl_->fill_rect(outer.x + 4.0f, outer.y + 4.0f, outer.w, outer.h, ui_rgb(0, 0, 0, 0.40f), scale);
                if (impl_->splash_params.show_border) {
                    impl_->fill_rect(outer.x, outer.y, outer.w, outer.h, ui_rgb(212, 208, 200), scale);
                    impl_->draw_bevel(outer, true, scale);
                    impl_->draw_title_bar(title_bar, close_btn, impl_->splash_params.caption_utf8, scale, rscale);
                }
            }

            impl_->fill_rect(client.x, client.y, client.w, client.h, ui_bgr(impl_->splash_params.bg_color_bgr), scale);

            if (impl_->splash_params.is_image && impl_->session_tex.valid() &&
                impl_->session_bitmap.width > 0 && impl_->session_bitmap.height > 0) {
                const f32 iw = static_cast<f32>(impl_->session_bitmap.width);
                const f32 ih = static_cast<f32>(impl_->session_bitmap.height);
                f32 sc = 1.0f;
                if (impl_->splash_params.scale > 0.0) {
                    sc = static_cast<f32>(impl_->splash_params.scale);
                } else {
                    sc = std::min(client.w / iw, client.h / ih);
                }
                const f32 dw = iw * sc;
                const f32 dh = ih * sc;
                const f32 dx = client.x + (client.w - dw) * 0.5f;
                const f32 dy = client.y + (client.h - dh) * 0.5f;
                impl_->set_scissor_logical(client, scale, drawable_h);
                impl_->draw_tex_rect(impl_->session_tex, dx, dy, dw, dh, ui_rgb(255, 255, 255), scale);
                impl_->clear_scissor();
            } else if (!impl_->splash_params.is_image) {
                impl_->render_rtf_region(impl_->splash_params.text_doc, client, 10.0f, scale, rscale, drawable_h, false);
            }

            if (splash_x.w > 0.0f) {
                impl_->draw_close_x_button(splash_x, impl_->splash_x_hover, impl_->splash_x_press, scale);
            }
            break;
        }

        case DialogMode::GameInfo: {
            UiRect outer, title_bar, close_btn, client;
            impl_->compute_info_layout(log_w, log_h, win_x, win_y, outer, title_bar, close_btn, client);

            impl_->fill_rect(0.0f, 0.0f, log_w, log_h, ui_rgb(0, 0, 0, 0.25f), scale);
            impl_->fill_rect(outer.x + 4.0f, outer.y + 4.0f, outer.w, outer.h, ui_rgb(0, 0, 0, 0.35f), scale);

            if (impl_->info_params.border) {
                impl_->fill_rect(outer.x, outer.y, outer.w, outer.h, ui_rgb(212, 208, 200), scale);
                impl_->draw_bevel(outer, true, scale);
                impl_->draw_title_bar(title_bar, close_btn, impl_->info_params.caption_utf8, scale, rscale);
            }

            impl_->fill_rect(client.x, client.y, client.w, client.h, ui_bgr(impl_->info_params.bg_color_bgr), scale);
            impl_->render_rtf_region(impl_->info_params.doc, client, 10.0f, scale, rscale, drawable_h, true);
            break;
        }

        default:
            break;
    }

    impl_->flush_batch();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    impl_->device->restore_render_state(saved);
}

} // namespace gm
