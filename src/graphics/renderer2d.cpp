#include "graphics/renderer2d.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "graphics/gl_headers.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gm {

namespace {

constexpr usize kMaxBatchVertices = 16384;

// Minimal 8x8 ASCII 32..127 bitmap font for GM8 default font (-1).
// Compact 8-byte-per-glyph table covering space through '~'.
constexpr u8 kBuiltinFont8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // '#'
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // '$'
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // '%'
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // '&'
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '''
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // '('
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ','
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // '.'
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // '/'
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // '0'
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // '1'
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // '2'
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // '3'
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // '4'
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // '5'
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // '6'
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // '7'
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // '8'
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // '9'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // ':'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ';'
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // '<'
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // '='
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // '>'
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // '?'
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // '@'
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 'A'
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 'B'
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 'C'
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 'D'
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 'E'
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 'F'
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 'G'
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 'H'
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'I'
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 'J'
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 'K'
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 'L'
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 'M'
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 'N'
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 'O'
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 'P'
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 'Q'
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 'R'
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 'S'
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'T'
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 'U'
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'W'
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 'X'
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 'Y'
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 'Z'
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // '['
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // '\'
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ']'
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 'a'
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 'b'
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 'c'
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // 'd'
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // 'e'
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // 'f'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 'g'
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 'h'
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 'i'
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 'j'
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 'k'
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 'l'
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 'm'
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 'n'
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 'o'
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 'p'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 'q'
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 'r'
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 's'
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 't'
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 'u'
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 'v'
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 'w'
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 'x'
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 'y'
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 'z'
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // '}'
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

BlendFactor gm_bm_to_factor(i32 bm) noexcept {
    switch (bm) {
        case 1:  return BlendFactor::Zero;
        case 2:  return BlendFactor::One;
        case 3:  return BlendFactor::SrcColor;
        case 4:  return BlendFactor::InvSrcColor;
        case 5:  return BlendFactor::SrcAlpha;
        case 6:  return BlendFactor::InvSrcAlpha;
        case 7:  return BlendFactor::DstAlpha;
        case 8:  return BlendFactor::InvDstAlpha;
        case 9:  return BlendFactor::DstColor;
        case 10: return BlendFactor::InvDstColor;
        case 11: return BlendFactor::SrcAlphaSat;
        default: return BlendFactor::SrcAlpha;
    }
}

GLenum to_gl_topo(PrimitiveTopology t) noexcept {
    switch (t) {
        case PrimitiveTopology::Points:        return GL_POINTS;
        case PrimitiveTopology::Lines:         return GL_LINES;
        case PrimitiveTopology::LineStrip:     return GL_LINE_STRIP;
        case PrimitiveTopology::Triangles:     return GL_TRIANGLES;
        case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
        case PrimitiveTopology::TriangleFan:   return GL_TRIANGLE_FAN;
    }
    return GL_TRIANGLES;
}

ColorRGBA make_vertex_color(u32 gm_bgr, f32 alpha) noexcept {
    const f32 clamped = std::clamp(alpha, 0.0f, 1.0f);
    return gm_bgr_to_rgba(gm_bgr, static_cast<u8>(clamped * 255.0f + 0.5f));
}

} // namespace

Renderer2D::~Renderer2D() {
    shutdown();
}

Result<void> Renderer2D::init(GlGraphicsDevice& device) {
    if (initialized_) {
        return Result<void>::success();
    }
    device_ = &device;

    const bool is_es = device.caps().is_gles;
    const std::string header = is_es ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n";

    const std::string vs_src = header + R"(
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

    const std::string fs_src = header + R"(
in vec2 v_uv;
in vec4 v_col;
uniform sampler2D u_tex;
uniform int u_alpha_test;
uniform float u_alpha_ref;
out vec4 o_color;
void main() {
    vec4 texel = texture(u_tex, v_uv);
    vec4 c = texel * v_col;
    if (u_alpha_test != 0 && c.a <= u_alpha_ref) {
        discard;
    }
    o_color = c;
}
)";

    auto sh_res = device_->create_shader(vs_src, fs_src);
    if (!sh_res) return sh_res.error();
    shader_ = sh_res.value();

    loc_proj_ = glGetUniformLocation(shader_.id, "u_proj");
    loc_tex_ = glGetUniformLocation(shader_.id, "u_tex");
    loc_alpha_test_ = glGetUniformLocation(shader_.id, "u_alpha_test");
    loc_alpha_ref_ = glGetUniformLocation(shader_.id, "u_alpha_ref");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(kMaxBatchVertices * sizeof(Vertex2D)), nullptr, GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<const void*>(offsetof(Vertex2D, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<const void*>(offsetof(Vertex2D, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex2D), reinterpret_cast<const void*>(offsetof(Vertex2D, r)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    vertices_.reserve(kMaxBatchVertices);
    create_builtin_font_texture();

    // Identity default projection
    std::memset(proj_matrix_, 0, sizeof(proj_matrix_));
    proj_matrix_[0] = proj_matrix_[5] = proj_matrix_[10] = proj_matrix_[15] = 1.0f;

    initialized_ = true;
    return Result<void>::success();
}

void Renderer2D::shutdown() noexcept {
    if (!initialized_) return;

    for (auto& sp : sprites_) {
        for (auto t : sp.subimages) device_->destroy_texture(t);
    }
    sprites_.clear();

    for (auto t : backgrounds_) device_->destroy_texture(t);
    backgrounds_.clear();

    for (auto& f : fonts_) device_->destroy_texture(f.texture);
    fonts_.clear();

    for (auto& [id, s] : surfaces_) {
        (void)id;
        device_->destroy_framebuffer(s.fbo);
        device_->destroy_texture(s.texture);
    }
    surfaces_.clear();

    if (builtin_font_tex_.valid()) {
        device_->destroy_texture(builtin_font_tex_);
        builtin_font_tex_ = {};
    }
    if (shader_.valid()) {
        device_->destroy_shader(shader_);
        shader_ = {};
    }
    if (vbo_ != 0) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_ != 0) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }

    initialized_ = false;
}

void Renderer2D::create_builtin_font_texture() {
    // 16 x 6 grid of 8x8 glyphs = 128x48 RGBA8 texture
    constexpr i32 kW = 128;
    constexpr i32 kH = 48;
    std::vector<u8> pixels(static_cast<usize>(kW) * static_cast<usize>(kH) * 4u, 0);

    for (i32 g = 0; g < 96; ++g) {
        const i32 gx = (g % 16) * 8;
        const i32 gy = (g / 16) * 8;
        for (i32 row = 0; row < 8; ++row) {
            const u8 bits = kBuiltinFont8x8[g][row];
            for (i32 col = 0; col < 8; ++col) {
                if ((bits & (1u << col)) != 0u) {
                    const usize idx = (static_cast<usize>(gy + row) * kW + static_cast<usize>(gx + col)) * 4u;
                    pixels[idx + 0] = 255;
                    pixels[idx + 1] = 255;
                    pixels[idx + 2] = 255;
                    pixels[idx + 3] = 255;
                }
            }
        }
    }

    TextureDesc desc{};
    desc.width = kW;
    desc.height = kH;
    desc.format = TextureFormat::RGBA8;
    desc.min_filter = TextureFilter::Nearest;
    desc.mag_filter = TextureFilter::Nearest;
    if (auto res = device_->create_texture(desc, pixels); res) {
        builtin_font_tex_ = res.value();
    }
}

void Renderer2D::upload_game_assets(const GameData& game) {
    sprites_.resize(game.sprites.size());
    for (usize i = 0; i < game.sprites.size(); ++i) {
        if (game.sprites[i].exists) {
            upload_sprite(i, game.sprites[i]);
        }
    }

    backgrounds_.resize(game.backgrounds.size());
    for (usize i = 0; i < game.backgrounds.size(); ++i) {
        if (game.backgrounds[i].exists) {
            upload_background(i, game.backgrounds[i]);
        }
    }

    fonts_.resize(game.fonts.size());
    for (usize i = 0; i < game.fonts.size(); ++i) {
        if (game.fonts[i].exists) {
            upload_font(i, game.fonts[i]);
        }
    }
}

void Renderer2D::upload_sprite(usize sprite_idx, const SpriteData& spr) {
    if (sprite_idx >= sprites_.size()) {
        sprites_.resize(sprite_idx + 1);
    }
    free_sprite_textures(sprite_idx);

    auto& dst = sprites_[sprite_idx];
    dst.subimages.reserve(spr.images.size());
    for (const auto& img : spr.images) {
        if (img.width <= 0 || img.height <= 0 || img.bgra.empty()) {
            dst.subimages.push_back(TextureHandle{});
            continue;
        }
        TextureDesc desc{};
        desc.width = img.width;
        desc.height = img.height;
        desc.format = TextureFormat::BGRA8;
        desc.min_filter = linear_filter_ ? TextureFilter::Linear : TextureFilter::Nearest;
        desc.mag_filter = desc.min_filter;
        desc.wrap_u = TextureWrap::ClampToEdge;
        desc.wrap_v = TextureWrap::ClampToEdge;
        auto res = device_->create_texture(desc, img.bgra);
        dst.subimages.push_back(res ? res.value() : TextureHandle{});
    }
}

void Renderer2D::free_sprite_textures(usize sprite_idx) {
    if (sprite_idx >= sprites_.size()) return;
    for (auto t : sprites_[sprite_idx].subimages) {
        device_->destroy_texture(t);
    }
    sprites_[sprite_idx].subimages.clear();
}

void Renderer2D::upload_background(usize bg_idx, const BackgroundData& bg) {
    if (bg_idx >= backgrounds_.size()) {
        backgrounds_.resize(bg_idx + 1);
    }
    free_background_texture(bg_idx);

    if (bg.bitmap.width <= 0 || bg.bitmap.height <= 0 || bg.bitmap.bgra.empty()) {
        return;
    }
    TextureDesc desc{};
    desc.width = bg.bitmap.width;
    desc.height = bg.bitmap.height;
    desc.format = TextureFormat::BGRA8;
    desc.min_filter = linear_filter_ ? TextureFilter::Linear : TextureFilter::Nearest;
    desc.mag_filter = desc.min_filter;
    desc.wrap_u = TextureWrap::ClampToEdge;
    desc.wrap_v = TextureWrap::ClampToEdge;
    if (auto res = device_->create_texture(desc, bg.bitmap.bgra); res) {
        backgrounds_[bg_idx] = res.value();
    }
}

void Renderer2D::free_background_texture(usize bg_idx) {
    if (bg_idx >= backgrounds_.size()) return;
    device_->destroy_texture(backgrounds_[bg_idx]);
    backgrounds_[bg_idx] = {};
}

void Renderer2D::upload_font(usize font_idx, const FontData& font) {
    if (font_idx >= fonts_.size()) {
        fonts_.resize(font_idx + 1);
    }
    if (fonts_[font_idx].texture.valid()) {
        device_->destroy_texture(fonts_[font_idx].texture);
        fonts_[font_idx] = {};
    }

    auto& fg = fonts_[font_idx];
    fg.tex_width = font.tex_width;
    fg.tex_height = font.tex_height;
    fg.line_height = 1;
    for (const auto& g : font.glyphs) {
        if (g.h > fg.line_height) fg.line_height = g.h;
    }

    if (font.tex_width <= 0 || font.tex_height <= 0 || font.alpha_map.empty()) {
        return;
    }

    const usize pixel_cnt = static_cast<usize>(font.tex_width) * static_cast<usize>(font.tex_height);
    std::vector<u8> rgba(pixel_cnt * 4u);
    const usize avail = std::min(pixel_cnt, font.alpha_map.size());
    for (usize i = 0; i < avail; ++i) {
        rgba[i * 4u + 0] = 255;
        rgba[i * 4u + 1] = 255;
        rgba[i * 4u + 2] = 255;
        rgba[i * 4u + 3] = font.alpha_map[i];
    }

    TextureDesc desc{};
    desc.width = font.tex_width;
    desc.height = font.tex_height;
    desc.format = TextureFormat::RGBA8;
    desc.min_filter = TextureFilter::Nearest;
    desc.mag_filter = TextureFilter::Nearest;
    if (auto res = device_->create_texture(desc, rgba); res) {
        fg.texture = res.value();
    }
}

void Renderer2D::register_sprite_font(usize font_idx, i32 sprite_idx, i32 first_char, bool prop, i32 sep, const GameData& game) {
    if (font_idx >= fonts_.size()) {
        fonts_.resize(font_idx + 1);
    }
    auto& fg = fonts_[font_idx];
    fg.is_sprite_font = true;
    fg.sprite_index = sprite_idx;
    fg.sprite_first = first_char;
    fg.sprite_prop = prop;
    fg.sprite_sep = sep;
    fg.line_height = 12;
    if (sprite_idx >= 0 && static_cast<usize>(sprite_idx) < game.sprites.size() &&
        game.sprites[static_cast<usize>(sprite_idx)].exists) {
        fg.line_height = std::max(1, game.sprites[static_cast<usize>(sprite_idx)].height);
    }
}

void Renderer2D::begin_frame(i32 backbuffer_w, i32 backbuffer_h) {
    backbuffer_w_ = std::max(1, backbuffer_w);
    backbuffer_h_ = std::max(1, backbuffer_h);
    vertices_.clear();
    device_->bind_framebuffer(FramebufferHandle{});
    active_surface_id_ = -1;
    device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    device_->set_cull_mode(CullMode::None);
    device_->set_blend_state(blend_state_);
}

void Renderer2D::end_frame() {
    flush();
}

void Renderer2D::flush() {
    if (vertices_.empty()) {
        return;
    }

    device_->bind_shader(shader_);
    glUniformMatrix4fv(loc_proj_, 1, GL_FALSE, proj_matrix_);
    glUniform1i(loc_tex_, 0);
    glUniform1i(loc_alpha_test_, alpha_test_enabled_ ? 1 : 0);
    glUniform1f(loc_alpha_ref_, alpha_test_ref_);

    device_->set_blend_state(blend_state_);
    device_->bind_texture(0, batch_tex_);
    if (batch_tex_.valid()) {
        const TextureFilter f = linear_filter_ ? TextureFilter::Linear : TextureFilter::Nearest;
        const TextureWrap w = tex_repeat_ ? TextureWrap::Repeat : TextureWrap::ClampToEdge;
        device_->set_texture_sampler(batch_tex_, f, w, w);
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex2D)), vertices_.data());
    glDrawArrays(to_gl_topo(batch_topo_), 0, static_cast<GLsizei>(vertices_.size()));
    glBindVertexArray(0);

    vertices_.clear();
}

void Renderer2D::set_projection_ortho(
    f32 view_x, f32 view_y, f32 view_w, f32 view_h, f32 angle_deg,
    i32 port_x, i32 port_y, i32 port_w, i32 port_h) {
    flush();

    const bool to_fbo = (active_surface_id_ >= 0);
    const i32 target_h = to_fbo ? surface_height(active_surface_id_) : backbuffer_h_;

    // OpenGL glViewport origin is bottom-left.
    // On backbuffer: top-down port_y -> gl_y = backbuffer_h - (port_y + port_h).
    // On FBO: world y=0 maps to FBO row 0 (gl_y = port_y) so surfaces sample right-side up!
    const i32 gl_y = to_fbo ? port_y : (target_h - (port_y + port_h));
    device_->set_viewport(ViewportRect{port_x, gl_y, std::max(1, port_w), std::max(1, port_h)});

    const f32 w = (std::fabs(view_w) > 1e-4f) ? view_w : 1.0f;
    const f32 h = (std::fabs(view_h) > 1e-4f) ? view_h : 1.0f;
    const f32 cx = view_x + w * 0.5f;
    const f32 cy = view_y + h * 0.5f;

    const f32 sx = 2.0f / w;
    const f32 sy = to_fbo ? (2.0f / h) : (-2.0f / h);

    const f32 rad = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(rad);
    const f32 s = std::sin(rad);

    // Column-major 4x4: Scale(sx, sy) * RotZ(angle) * Translate(-cx, -cy)
    std::memset(proj_matrix_, 0, sizeof(proj_matrix_));
    proj_matrix_[0] = c * sx;
    proj_matrix_[1] = -s * sy;
    proj_matrix_[4] = s * sx;
    proj_matrix_[5] = c * sy;
    proj_matrix_[10] = 1.0f;
    proj_matrix_[12] = (-cx * c - cy * s) * sx;
    proj_matrix_[13] = (cx * s - cy * c) * sy;
    proj_matrix_[15] = 1.0f;
}

void Renderer2D::clear_color(u32 gm_bgr, f32 alpha) {
    flush();
    const ColorRGBA c = gm_bgr_to_rgba(gm_bgr);
    device_->clear(true, false, false,
                   static_cast<f32>(c.r) / 255.0f,
                   static_cast<f32>(c.g) / 255.0f,
                   static_cast<f32>(c.b) / 255.0f,
                   alpha);
}

void Renderer2D::set_blend_mode(i32 bm_mode) {
    flush();
    blend_state_.enabled = true;
    blend_state_.op_rgb = BlendOp::Add;
    blend_state_.op_alpha = BlendOp::Add;
    switch (bm_mode) {
        case 1: // bm_add
            blend_state_.src_rgb = BlendFactor::SrcAlpha;
            blend_state_.dst_rgb = BlendFactor::One;
            blend_state_.src_alpha = BlendFactor::SrcAlpha;
            blend_state_.dst_alpha = BlendFactor::One;
            break;
        case 2: // bm_max
            blend_state_.src_rgb = BlendFactor::SrcAlpha;
            blend_state_.dst_rgb = BlendFactor::InvSrcColor;
            blend_state_.src_alpha = BlendFactor::SrcAlpha;
            blend_state_.dst_alpha = BlendFactor::InvSrcAlpha;
            break;
        case 3: // bm_subtract
            blend_state_.src_rgb = BlendFactor::Zero;
            blend_state_.dst_rgb = BlendFactor::InvSrcColor;
            blend_state_.src_alpha = BlendFactor::Zero;
            blend_state_.dst_alpha = BlendFactor::InvSrcAlpha;
            break;
        default: // bm_normal (0)
            blend_state_.src_rgb = BlendFactor::SrcAlpha;
            blend_state_.dst_rgb = BlendFactor::InvSrcAlpha;
            blend_state_.src_alpha = BlendFactor::SrcAlpha;
            blend_state_.dst_alpha = BlendFactor::InvSrcAlpha;
            break;
    }
}

void Renderer2D::set_blend_mode_ext(i32 src_bm, i32 dst_bm) {
    flush();
    blend_state_.enabled = true;
    blend_state_.op_rgb = BlendOp::Add;
    blend_state_.op_alpha = BlendOp::Add;
    blend_state_.src_rgb = gm_bm_to_factor(src_bm);
    blend_state_.dst_rgb = gm_bm_to_factor(dst_bm);
    blend_state_.src_alpha = blend_state_.src_rgb;
    blend_state_.dst_alpha = blend_state_.dst_rgb;
}

void Renderer2D::set_interpolation(bool linear) {
    if (linear_filter_ != linear) {
        flush();
        linear_filter_ = linear;
    }
}

void Renderer2D::set_texture_repeat(bool repeat) {
    if (tex_repeat_ != repeat) {
        flush();
        tex_repeat_ = repeat;
    }
}

void Renderer2D::set_alpha_test(bool enabled, u8 ref_val) {
    flush();
    alpha_test_enabled_ = enabled;
    alpha_test_ref_ = static_cast<f32>(ref_val) / 255.0f;
}

void Renderer2D::set_color_write_enable(bool r, bool g, bool b, bool a) {
    flush();
    device_->set_color_mask(r, g, b, a);
}

void Renderer2D::ensure_batch(PrimitiveTopology topo, TextureHandle tex) {
    if (vertices_.size() + 6 >= kMaxBatchVertices || batch_topo_ != topo || batch_tex_ != tex) {
        flush();
        batch_topo_ = topo;
        batch_tex_ = tex;
    }
}

void Renderer2D::push_quad(
    TextureHandle tex,
    f32 x0, f32 y0, f32 u0, f32 v0, ColorRGBA c0,
    f32 x1, f32 y1, f32 u1, f32 v1, ColorRGBA c1,
    f32 x2, f32 y2, f32 u2, f32 v2, ColorRGBA c2,
    f32 x3, f32 y3, f32 u3, f32 v3, ColorRGBA c3) {
    ensure_batch(PrimitiveTopology::Triangles, tex);
    vertices_.push_back(Vertex2D{x0, y0, u0, v0, c0.r, c0.g, c0.b, c0.a});
    vertices_.push_back(Vertex2D{x1, y1, u1, v1, c1.r, c1.g, c1.b, c1.a});
    vertices_.push_back(Vertex2D{x2, y2, u2, v2, c2.r, c2.g, c2.b, c2.a});
    vertices_.push_back(Vertex2D{x0, y0, u0, v0, c0.r, c0.g, c0.b, c0.a});
    vertices_.push_back(Vertex2D{x2, y2, u2, v2, c2.r, c2.g, c2.b, c2.a});
    vertices_.push_back(Vertex2D{x3, y3, u3, v3, c3.r, c3.g, c3.b, c3.a});
}

// ---- Surfaces ----

i32 Renderer2D::surface_create(i32 width, i32 height) {
    if (width <= 0 || height <= 0) return -1;
    TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = TextureFormat::RGBA8;
    desc.min_filter = TextureFilter::Nearest;
    desc.mag_filter = TextureFilter::Nearest;
    auto tex_res = device_->create_texture(desc);
    if (!tex_res) return -1;

    TextureHandle targets[1] = {tex_res.value()};
    auto fbo_res = device_->create_framebuffer(targets, true);
    if (!fbo_res) {
        device_->destroy_texture(tex_res.value());
        return -1;
    }

    const i32 id = next_surface_id_++;
    surfaces_[id] = SurfaceObject{width, height, tex_res.value(), fbo_res.value()};

    // Clear newly created surface to transparent black.
    const i32 prev = active_surface_id_;
    flush();
    device_->bind_framebuffer(fbo_res.value());
    device_->set_viewport(ViewportRect{0, 0, width, height});
    device_->clear(true, true, true, 0.0f, 0.0f, 0.0f, 0.0f);
    if (prev >= 0 && surfaces_.contains(prev)) {
        device_->bind_framebuffer(surfaces_[prev].fbo);
    } else {
        device_->bind_framebuffer(FramebufferHandle{});
    }
    return id;
}

void Renderer2D::surface_free(i32 id) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return;
    if (active_surface_id_ == id) {
        surface_reset_target();
    }
    device_->destroy_framebuffer(it->second.fbo);
    device_->destroy_texture(it->second.texture);
    surfaces_.erase(it);
}

bool Renderer2D::surface_exists(i32 id) const noexcept {
    return surfaces_.contains(id);
}

i32 Renderer2D::surface_width(i32 id) const noexcept {
    const auto it = surfaces_.find(id);
    return it != surfaces_.end() ? it->second.width : 0;
}

i32 Renderer2D::surface_height(i32 id) const noexcept {
    const auto it = surfaces_.find(id);
    return it != surfaces_.end() ? it->second.height : 0;
}

bool Renderer2D::surface_set_target(i32 id) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return false;
    flush();
    active_surface_id_ = id;
    device_->bind_framebuffer(it->second.fbo);
    set_projection_ortho(0.0f, 0.0f, static_cast<f32>(it->second.width), static_cast<f32>(it->second.height),
                         0.0f, 0, 0, it->second.width, it->second.height);
    return true;
}

void Renderer2D::surface_reset_target() {
    flush();
    active_surface_id_ = -1;
    device_->bind_framebuffer(FramebufferHandle{});
}

void Renderer2D::draw_surface_ext(i32 id, f32 x, f32 y, f32 xscale, f32 yscale, f32 rot_deg, u32 color_bgr, f32 alpha) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return;

    const f32 w = static_cast<f32>(it->second.width) * xscale;
    const f32 h = static_cast<f32>(it->second.height) * yscale;
    const f32 rad = static_cast<f32>(deg_to_rad(rot_deg));
    const f32 c = std::cos(rad);
    const f32 s = std::sin(rad);

    auto xf = [&](f32 lx, f32 ly, f32& ox, f32& oy) {
        ox = x + lx * c + ly * s;
        oy = y - lx * s + ly * c;
    };

    f32 x0, y0, x1, y1, x2, y2, x3, y3;
    xf(0.0f, 0.0f, x0, y0);
    xf(w,    0.0f, x1, y1);
    xf(w,    h,    x2, y2);
    xf(0.0f, h,    x3, y3);

    const ColorRGBA col = make_vertex_color(color_bgr, alpha);
    push_quad(it->second.texture,
              x0, y0, 0.0f, 0.0f, col,
              x1, y1, 1.0f, 0.0f, col,
              x2, y2, 1.0f, 1.0f, col,
              x3, y3, 0.0f, 1.0f, col);
}

void Renderer2D::draw_surface_part_ext(i32 id, f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end() || it->second.width <= 0 || it->second.height <= 0) return;
    const f32 inv_w = 1.0f / static_cast<f32>(it->second.width);
    const f32 inv_h = 1.0f / static_cast<f32>(it->second.height);
    const f32 u0 = left * inv_w;
    const f32 v0 = top * inv_h;
    const f32 u1 = (left + w) * inv_w;
    const f32 v1 = (top + h) * inv_h;
    const f32 dw = w * xscale;
    const f32 dh = h * yscale;
    const ColorRGBA col = make_vertex_color(color_bgr, alpha);
    push_quad(it->second.texture,
              x, y, u0, v0, col,
              x + dw, y, u1, v0, col,
              x + dw, y + dh, u1, v1, col,
              x, y + dh, u0, v1, col);
}

void Renderer2D::draw_surface_stretched_ext(i32 id, f32 x, f32 y, f32 w, f32 h, u32 color_bgr, f32 alpha) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return;
    const ColorRGBA col = make_vertex_color(color_bgr, alpha);
    push_quad(it->second.texture,
              x, y, 0.0f, 0.0f, col,
              x + w, y, 1.0f, 0.0f, col,
              x + w, y + h, 1.0f, 1.0f, col,
              x, y + h, 0.0f, 1.0f, col);
}

bool Renderer2D::surface_read_pixels_bgra(i32 id, i32 x, i32 y, i32 w, i32 h, std::vector<u8>& out_bgra) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end() || w <= 0 || h <= 0) return false;
    flush();

    const i32 prev = active_surface_id_;
    device_->bind_framebuffer(it->second.fbo);

    std::vector<u8> rgba(static_cast<usize>(w) * static_cast<usize>(h) * 4u);
    device_->read_pixels_rgba8(x, y, w, h, rgba);

    if (prev >= 0 && surfaces_.contains(prev)) {
        device_->bind_framebuffer(surfaces_[prev].fbo);
    } else {
        device_->bind_framebuffer(FramebufferHandle{});
    }

    out_bgra.resize(rgba.size());
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        out_bgra[i + 0] = rgba[i + 2];
        out_bgra[i + 1] = rgba[i + 1];
        out_bgra[i + 2] = rgba[i + 0];
        out_bgra[i + 3] = rgba[i + 3];
    }
    return true;
}

bool Renderer2D::surface_save(i32 id, std::string_view host_path) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return false;
    flush();

    const i32 prev = active_surface_id_;
    device_->bind_framebuffer(it->second.fbo);
    std::vector<u8> rgba(static_cast<usize>(it->second.width) * static_cast<usize>(it->second.height) * 4u);
    device_->read_pixels_rgba8(0, 0, it->second.width, it->second.height, rgba);
    if (prev >= 0 && surfaces_.contains(prev)) {
        device_->bind_framebuffer(surfaces_[prev].fbo);
    } else {
        device_->bind_framebuffer(FramebufferHandle{});
    }

    const std::string path(host_path);
    const std::string lower = ascii_to_lower(host_path);
    if (lower.ends_with(".bmp")) {
        return stbi_write_bmp(path.c_str(), it->second.width, it->second.height, 4, rgba.data()) != 0;
    }
    return stbi_write_png(path.c_str(), it->second.width, it->second.height, 4, rgba.data(), it->second.width * 4) != 0;
}

bool Renderer2D::screen_save(std::string_view host_path) {
    flush();
    const i32 prev = active_surface_id_;
    device_->bind_framebuffer(FramebufferHandle{});

    std::vector<u8> rgba(static_cast<usize>(backbuffer_w_) * static_cast<usize>(backbuffer_h_) * 4u);
    device_->read_pixels_rgba8(0, 0, backbuffer_w_, backbuffer_h_, rgba);

    if (prev >= 0 && surfaces_.contains(prev)) {
        device_->bind_framebuffer(surfaces_[prev].fbo);
    }

    // Default backbuffer glReadPixels is bottom-up; flip rows vertically to top-down.
    const usize row_bytes = static_cast<usize>(backbuffer_w_) * 4u;
    std::vector<u8> tmp(row_bytes);
    for (i32 y = 0; y < backbuffer_h_ / 2; ++y) {
        u8* top = rgba.data() + static_cast<usize>(y) * row_bytes;
        u8* bot = rgba.data() + static_cast<usize>(backbuffer_h_ - 1 - y) * row_bytes;
        std::memcpy(tmp.data(), top, row_bytes);
        std::memcpy(top, bot, row_bytes);
        std::memcpy(bot, tmp.data(), row_bytes);
    }

    const std::string path(host_path);
    const std::string lower = ascii_to_lower(host_path);
    if (lower.ends_with(".bmp")) {
        return stbi_write_bmp(path.c_str(), backbuffer_w_, backbuffer_h_, 4, rgba.data()) != 0;
    }
    return stbi_write_png(path.c_str(), backbuffer_w_, backbuffer_h_, 4, rgba.data(), backbuffer_w_ * 4) != 0;
}

// ---- Sprites / Backgrounds / Tiles ----

TextureHandle Renderer2D::sprite_texture(i32 sprite_id, i32 subimg) const noexcept {
    if (sprite_id < 0 || static_cast<usize>(sprite_id) >= sprites_.size()) return {};
    const auto& sub = sprites_[static_cast<usize>(sprite_id)].subimages;
    if (sub.empty()) return {};
    i32 idx = subimg % static_cast<i32>(sub.size());
    if (idx < 0) idx += static_cast<i32>(sub.size());
    return sub[static_cast<usize>(idx)];
}

TextureHandle Renderer2D::background_texture(i32 bg_id) const noexcept {
    if (bg_id < 0 || static_cast<usize>(bg_id) >= backgrounds_.size()) return {};
    return backgrounds_[static_cast<usize>(bg_id)];
}

TextureHandle Renderer2D::surface_texture(i32 surf_id) const noexcept {
    const auto it = surfaces_.find(surf_id);
    return it != surfaces_.end() ? it->second.texture : TextureHandle{};
}

void Renderer2D::draw_sprite_ext(
    const GameData& game, i32 sprite_id, i32 subimg,
    f32 x, f32 y, f32 xscale, f32 yscale, f32 rot_deg, u32 color_bgr, f32 alpha) {
    if (sprite_id < 0 || static_cast<usize>(sprite_id) >= game.sprites.size()) return;
    const auto& sp = game.sprites[static_cast<usize>(sprite_id)];
    if (!sp.exists || sp.images.empty()) return;
    const TextureHandle tex = sprite_texture(sprite_id, subimg);
    if (!tex.valid()) return;

    const f32 lx0 = -static_cast<f32>(sp.origin_x) * xscale;
    const f32 ly0 = -static_cast<f32>(sp.origin_y) * yscale;
    const f32 lx1 = static_cast<f32>(sp.width - sp.origin_x) * xscale;
    const f32 ly1 = static_cast<f32>(sp.height - sp.origin_y) * yscale;

    const ColorRGBA col = make_vertex_color(color_bgr, alpha);
    if (std::fabs(rot_deg) < 1e-5f) {
        push_quad(tex,
                  x + lx0, y + ly0, 0.0f, 0.0f, col,
                  x + lx1, y + ly0, 1.0f, 0.0f, col,
                  x + lx1, y + ly1, 1.0f, 1.0f, col,
                  x + lx0, y + ly1, 0.0f, 1.0f, col);
        return;
    }

    const f32 rad = static_cast<f32>(deg_to_rad(rot_deg));
    const f32 c = std::cos(rad);
    const f32 s = std::sin(rad);
    auto xf = [&](f32 lx, f32 ly, f32& ox, f32& oy) {
        ox = x + lx * c + ly * s;
        oy = y - lx * s + ly * c;
    };

    f32 x0, y0, x1, y1, x2, y2, x3, y3;
    xf(lx0, ly0, x0, y0);
    xf(lx1, ly0, x1, y1);
    xf(lx1, ly1, x2, y2);
    xf(lx0, ly1, x3, y3);
    push_quad(tex,
              x0, y0, 0.0f, 0.0f, col,
              x1, y1, 1.0f, 0.0f, col,
              x2, y2, 1.0f, 1.0f, col,
              x3, y3, 0.0f, 1.0f, col);
}

void Renderer2D::draw_sprite_part_ext(
    const GameData& game, i32 sprite_id, i32 subimg,
    f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha) {
    draw_sprite_general(game, sprite_id, subimg, left, top, w, h, x, y, xscale, yscale, 0.0f,
                        color_bgr, color_bgr, color_bgr, color_bgr, alpha);
}

void Renderer2D::draw_sprite_stretched_ext(
    const GameData& game, i32 sprite_id, i32 subimg,
    f32 x, f32 y, f32 w, f32 h, u32 color_bgr, f32 alpha) {
    const TextureHandle tex = sprite_texture(sprite_id, subimg);
    if (!tex.valid()) return;
    const ColorRGBA col = make_vertex_color(color_bgr, alpha);
    push_quad(tex,
              x, y, 0.0f, 0.0f, col,
              x + w, y, 1.0f, 0.0f, col,
              x + w, y + h, 1.0f, 1.0f, col,
              x, y + h, 0.0f, 1.0f, col);
}

void Renderer2D::draw_sprite_tiled_ext(
    const GameData& game, i32 sprite_id, i32 subimg,
    f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha,
    f32 view_x, f32 view_y, f32 view_w, f32 view_h) {
    if (sprite_id < 0 || static_cast<usize>(sprite_id) >= game.sprites.size()) return;
    const auto& sp = game.sprites[static_cast<usize>(sprite_id)];
    if (!sp.exists || sp.width <= 0 || sp.height <= 0) return;

    const f32 step_x = std::fabs(static_cast<f32>(sp.width) * xscale);
    const f32 step_y = std::fabs(static_cast<f32>(sp.height) * yscale);
    if (step_x < 1.0f || step_y < 1.0f) return;

    f32 start_x = x - std::floor((x - view_x) / step_x) * step_x - step_x;
    f32 start_y = y - std::floor((y - view_y) / step_y) * step_y - step_y;
    for (f32 ty = start_y; ty < view_y + view_h + step_y; ty += step_y) {
        for (f32 tx = start_x; tx < view_x + view_w + step_x; tx += step_x) {
            draw_sprite_ext(game, sprite_id, subimg, tx, ty, xscale, yscale, 0.0f, color_bgr, alpha);
        }
    }
}

void Renderer2D::draw_sprite_general(
    const GameData& game, i32 sprite_id, i32 subimg,
    f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale,
    f32 rot_deg, u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha) {
    if (sprite_id < 0 || static_cast<usize>(sprite_id) >= game.sprites.size()) return;
    const auto& sp = game.sprites[static_cast<usize>(sprite_id)];
    if (!sp.exists || sp.width <= 0 || sp.height <= 0) return;
    const TextureHandle tex = sprite_texture(sprite_id, subimg);
    if (!tex.valid()) return;

    const f32 inv_w = 1.0f / static_cast<f32>(sp.width);
    const f32 inv_h = 1.0f / static_cast<f32>(sp.height);
    const f32 u0 = left * inv_w;
    const f32 v0 = top * inv_h;
    const f32 u1 = (left + w) * inv_w;
    const f32 v1 = (top + h) * inv_h;
    const f32 dw = w * xscale;
    const f32 dh = h * yscale;

    const f32 rad = static_cast<f32>(deg_to_rad(rot_deg));
    const f32 c = std::cos(rad);
    const f32 s = std::sin(rad);
    auto xf = [&](f32 lx, f32 ly, f32& ox, f32& oy) {
        ox = x + lx * c + ly * s;
        oy = y - lx * s + ly * c;
    };

    f32 x0, y0, x1, y1, x2, y2, x3, y3;
    xf(0.0f, 0.0f, x0, y0);
    xf(dw,   0.0f, x1, y1);
    xf(dw,   dh,   x2, y2);
    xf(0.0f, dh,   x3, y3);

    push_quad(tex,
              x0, y0, u0, v0, make_vertex_color(c1, alpha),
              x1, y1, u1, v0, make_vertex_color(c2, alpha),
              x2, y2, u1, v1, make_vertex_color(c3, alpha),
              x3, y3, u0, v1, make_vertex_color(c4, alpha));
}

void Renderer2D::draw_background_ext(
    const GameData& game, i32 bg_id,
    f32 x, f32 y, f32 xscale, f32 yscale, f32 rot_deg, u32 color_bgr, f32 alpha) {
    if (bg_id < 0 || static_cast<usize>(bg_id) >= game.backgrounds.size()) return;
    const auto& bg = game.backgrounds[static_cast<usize>(bg_id)];
    if (!bg.exists || bg.width <= 0 || bg.height <= 0) return;
    const TextureHandle tex = background_texture(bg_id);
    if (!tex.valid()) return;

    const f32 dw = static_cast<f32>(bg.width) * xscale;
    const f32 dh = static_cast<f32>(bg.height) * yscale;
    const f32 rad = static_cast<f32>(deg_to_rad(rot_deg));
    const f32 c = std::cos(rad);
    const f32 s = std::sin(rad);
    auto xf = [&](f32 lx, f32 ly, f32& ox, f32& oy) {
        ox = x + lx * c + ly * s;
        oy = y - lx * s + ly * c;
    };

    f32 x0, y0, x1, y1, x2, y2, x3, y3;
    xf(0.0f, 0.0f, x0, y0);
    xf(dw,   0.0f, x1, y1);
    xf(dw,   dh,   x2, y2);
    xf(0.0f, dh,   x3, y3);

    const ColorRGBA col = make_vertex_color(color_bgr, alpha);
    push_quad(tex,
              x0, y0, 0.0f, 0.0f, col,
              x1, y1, 1.0f, 0.0f, col,
              x2, y2, 1.0f, 1.0f, col,
              x3, y3, 0.0f, 1.0f, col);
}

void Renderer2D::draw_background_part_ext(
    const GameData& game, i32 bg_id,
    f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha) {
    draw_tile(game, bg_id, left, top, w, h, x, y, xscale, yscale, color_bgr, alpha);
}

void Renderer2D::draw_background_stretched_ext(
    const GameData& game, i32 bg_id,
    f32 x, f32 y, f32 w, f32 h, u32 color_bgr, f32 alpha) {
    const TextureHandle tex = background_texture(bg_id);
    if (!tex.valid()) return;
    const ColorRGBA col = make_vertex_color(color_bgr, alpha);
    push_quad(tex,
              x, y, 0.0f, 0.0f, col,
              x + w, y, 1.0f, 0.0f, col,
              x + w, y + h, 1.0f, 1.0f, col,
              x, y + h, 0.0f, 1.0f, col);
}

void Renderer2D::draw_background_tiled_ext(
    const GameData& game, i32 bg_id,
    f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha,
    f32 view_x, f32 view_y, f32 view_w, f32 view_h, bool htiled, bool vtiled) {
    if (bg_id < 0 || static_cast<usize>(bg_id) >= game.backgrounds.size()) return;
    const auto& bg = game.backgrounds[static_cast<usize>(bg_id)];
    if (!bg.exists || bg.width <= 0 || bg.height <= 0) return;

    const f32 step_x = std::fabs(static_cast<f32>(bg.width) * xscale);
    const f32 step_y = std::fabs(static_cast<f32>(bg.height) * yscale);
    if (step_x < 1.0f || step_y < 1.0f) return;

    const f32 x_begin = htiled ? (x - std::floor((x - view_x) / step_x) * step_x - step_x) : x;
    const f32 x_end = htiled ? (view_x + view_w + step_x) : (x + 0.5f);
    const f32 y_begin = vtiled ? (y - std::floor((y - view_y) / step_y) * step_y - step_y) : y;
    const f32 y_end = vtiled ? (view_y + view_h + step_y) : (y + 0.5f);

    for (f32 ty = y_begin; ty < y_end; ty += step_y) {
        for (f32 tx = x_begin; tx < x_end; tx += step_x) {
            draw_background_ext(game, bg_id, tx, ty, xscale, yscale, 0.0f, color_bgr, alpha);
        }
    }
}

void Renderer2D::draw_tile(
    const GameData& game, i32 bg_id,
    f32 xo, f32 yo, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale, u32 blend_bgr, f32 alpha) {
    if (bg_id < 0 || static_cast<usize>(bg_id) >= game.backgrounds.size()) return;
    const auto& bg = game.backgrounds[static_cast<usize>(bg_id)];
    if (!bg.exists || bg.width <= 0 || bg.height <= 0) return;
    const TextureHandle tex = background_texture(bg_id);
    if (!tex.valid()) return;

    const f32 inv_w = 1.0f / static_cast<f32>(bg.width);
    const f32 inv_h = 1.0f / static_cast<f32>(bg.height);
    const f32 u0 = xo * inv_w;
    const f32 v0 = yo * inv_h;
    const f32 u1 = (xo + w) * inv_w;
    const f32 v1 = (yo + h) * inv_h;
    const f32 dw = w * xscale;
    const f32 dh = h * yscale;

    const ColorRGBA col = make_vertex_color(blend_bgr, alpha);
    push_quad(tex,
              x, y, u0, v0, col,
              x + dw, y, u1, v0, col,
              x + dw, y + dh, u1, v1, col,
              x, y + dh, u0, v1, col);
}

// ---- Text & Fonts ----

f32 Renderer2D::current_font_line_height(const GameData& game) const {
    if (draw_font_ >= 0 && static_cast<usize>(draw_font_) < fonts_.size() &&
        (fonts_[static_cast<usize>(draw_font_)].texture.valid() || fonts_[static_cast<usize>(draw_font_)].is_sprite_font)) {
        return static_cast<f32>(fonts_[static_cast<usize>(draw_font_)].line_height);
    }
    (void)game;
    return 12.0f;
}

f32 Renderer2D::measure_single_line_width(const GameData& game, std::string_view line) const {
    f32 width = 0.0f;
    if (draw_font_ >= 0 && static_cast<usize>(draw_font_) < fonts_.size()) {
        const auto& fg = fonts_[static_cast<usize>(draw_font_)];
        if (fg.is_sprite_font) {
            if (fg.sprite_index >= 0 && static_cast<usize>(fg.sprite_index) < game.sprites.size()) {
                const f32 gw = static_cast<f32>(game.sprites[static_cast<usize>(fg.sprite_index)].width + fg.sprite_sep);
                return static_cast<f32>(line.size()) * gw;
            }
        } else if (fg.texture.valid() && static_cast<usize>(draw_font_) < game.fonts.size()) {
            const auto& f = game.fonts[static_cast<usize>(draw_font_)];
            for (char ch : line) {
                const u8 code = static_cast<u8>(ch);
                width += static_cast<f32>(f.glyphs[code].shift);
            }
            return width;
        }
    }
    return static_cast<f32>(line.size()) * 8.0f;
}

std::vector<std::string> Renderer2D::split_gm_lines(const GameData& game, std::string_view text, f32 max_w) const {
    // Decode GM8 '#' newlines & '\#' literal '#' first.
    std::vector<std::string> hard_lines;
    std::string cur;
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == '#') {
            cur.push_back('#');
            ++i;
        } else if (c == '#' || c == '\n') {
            hard_lines.push_back(std::move(cur));
            cur.clear();
        } else if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            hard_lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    hard_lines.push_back(std::move(cur));

    if (max_w < 0.0f) {
        return hard_lines;
    }

    // Word wrap lines exceeding max_w.
    std::vector<std::string> wrapped;
    for (const auto& hl : hard_lines) {
        if (measure_single_line_width(game, hl) <= max_w || hl.empty()) {
            wrapped.push_back(hl);
            continue;
        }
        std::string line;
        usize pos = 0;
        while (pos < hl.size()) {
            usize next_sp = hl.find(' ', pos);
            if (next_sp == std::string::npos) next_sp = hl.size();
            const std::string_view word(hl.data() + pos, next_sp - pos);
            const std::string cand = line.empty() ? std::string(word) : (line + " " + std::string(word));
            if (!line.empty() && measure_single_line_width(game, cand) > max_w) {
                wrapped.push_back(std::move(line));
                line = std::string(word);
            } else {
                line = cand;
            }
            pos = (next_sp < hl.size()) ? (next_sp + 1) : hl.size();
        }
        wrapped.push_back(std::move(line));
    }
    return wrapped;
}

f32 Renderer2D::measure_string_width(const GameData& game, std::string_view text) const {
    const auto lines = split_gm_lines(game, text, -1.0f);
    f32 max_w = 0.0f;
    for (const auto& l : lines) {
        max_w = std::max(max_w, measure_single_line_width(game, l));
    }
    return max_w;
}

f32 Renderer2D::measure_string_height(const GameData& game, std::string_view text, f32 sep, f32 max_w) const {
    const auto lines = split_gm_lines(game, text, max_w);
    if (lines.empty()) return 0.0f;
    const f32 lh = current_font_line_height(game);
    const f32 step = (sep < 0.0f) ? lh : sep;
    return lh + static_cast<f32>(lines.size() - 1) * step;
}

void Renderer2D::draw_text_ext_transformed_color(
    const GameData& game, f32 x, f32 y, std::string_view text,
    f32 sep, f32 max_w, f32 xscale, f32 yscale, f32 angle_deg,
    u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha) {
    if (text.empty()) return;

    const auto lines = split_gm_lines(game, text, max_w);
    const f32 lh = current_font_line_height(game);
    const f32 line_step = (sep < 0.0f) ? lh : sep;
    const f32 total_h = lh + static_cast<f32>(lines.size() - 1) * line_step;

    f32 base_y = 0.0f;
    if (draw_valign_ == 1) base_y = -total_h * 0.5f;
    else if (draw_valign_ == 2) base_y = -total_h;

    const f32 rad = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 cs = std::cos(rad);
    const f32 sn = std::sin(rad);
    auto xf = [&](f32 lx, f32 ly, f32& ox, f32& oy) {
        const f32 sx = lx * xscale;
        const f32 sy = ly * yscale;
        ox = x + sx * cs + sy * sn;
        oy = y - sx * sn + sy * cs;
    };

    const ColorRGBA col_tl = make_vertex_color(c1, alpha);
    const ColorRGBA col_tr = make_vertex_color(c2, alpha);
    const ColorRGBA col_br = make_vertex_color(c3, alpha);
    const ColorRGBA col_bl = make_vertex_color(c4, alpha);

    const bool has_gm_font = (draw_font_ >= 0 && static_cast<usize>(draw_font_) < fonts_.size() &&
                              static_cast<usize>(draw_font_) < game.fonts.size() &&
                              fonts_[static_cast<usize>(draw_font_)].texture.valid());
    const bool has_spr_font = (draw_font_ >= 0 && static_cast<usize>(draw_font_) < fonts_.size() &&
                               fonts_[static_cast<usize>(draw_font_)].is_sprite_font);

    for (usize li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        const f32 lw = measure_single_line_width(game, line);
        f32 pen_x = 0.0f;
        if (draw_halign_ == 1) pen_x = -lw * 0.5f;
        else if (draw_halign_ == 2) pen_x = -lw;
        const f32 pen_y = base_y + static_cast<f32>(li) * line_step;

        if (has_spr_font) {
            const auto& fg = fonts_[static_cast<usize>(draw_font_)];
            if (fg.sprite_index >= 0 && static_cast<usize>(fg.sprite_index) < game.sprites.size()) {
                const auto& sp = game.sprites[static_cast<usize>(fg.sprite_index)];
                for (char ch : line) {
                    const i32 sub = static_cast<u8>(ch) - fg.sprite_first;
                    if (sub >= 0 && static_cast<usize>(sub) < sp.images.size()) {
                        f32 gx, gy;
                        xf(pen_x, pen_y, gx, gy);
                        draw_sprite_ext(game, fg.sprite_index, sub, gx, gy, xscale, yscale, angle_deg, c1, alpha);
                    }
                    pen_x += static_cast<f32>(sp.width + fg.sprite_sep);
                }
            }
        } else if (has_gm_font) {
            const auto& fg = fonts_[static_cast<usize>(draw_font_)];
            const auto& fd = game.fonts[static_cast<usize>(draw_font_)];
            const f32 inv_w = 1.0f / static_cast<f32>(std::max(1, fg.tex_width));
            const f32 inv_h = 1.0f / static_cast<f32>(std::max(1, fg.tex_height));

            for (char ch : line) {
                const u8 code = static_cast<u8>(ch);
                const auto& g = fd.glyphs[code];
                if (g.w > 0 && g.h > 0) {
                    const f32 gx0 = pen_x + static_cast<f32>(g.offset);
                    const f32 gy0 = pen_y;
                    const f32 gx1 = gx0 + static_cast<f32>(g.w);
                    const f32 gy1 = gy0 + static_cast<f32>(g.h);
                    const f32 u0 = static_cast<f32>(g.x) * inv_w;
                    const f32 v0 = static_cast<f32>(g.y) * inv_h;
                    const f32 u1 = static_cast<f32>(g.x + g.w) * inv_w;
                    const f32 v1 = static_cast<f32>(g.y + g.h) * inv_h;

                    f32 x0, y0, x1, y1, x2, y2, x3, y3;
                    xf(gx0, gy0, x0, y0);
                    xf(gx1, gy0, x1, y1);
                    xf(gx1, gy1, x2, y2);
                    xf(gx0, gy1, x3, y3);
                    push_quad(fg.texture,
                              x0, y0, u0, v0, col_tl,
                              x1, y1, u1, v0, col_tr,
                              x2, y2, u1, v1, col_br,
                              x3, y3, u0, v1, col_bl);
                }
                pen_x += static_cast<f32>(g.shift);
            }
        } else {
            // Built-in 8x8 fallback font (font = -1)
            for (char ch : line) {
                const u8 code = static_cast<u8>(ch);
                if (code >= 32 && code < 128) {
                    const i32 gi = code - 32;
                    const f32 u0 = static_cast<f32>((gi % 16) * 8) / 128.0f;
                    const f32 v0 = static_cast<f32>((gi / 16) * 8) / 48.0f;
                    const f32 u1 = u0 + 8.0f / 128.0f;
                    const f32 v1 = v0 + 8.0f / 48.0f;

                    f32 x0, y0, x1, y1, x2, y2, x3, y3;
                    xf(pen_x, pen_y + 2.0f, x0, y0);
                    xf(pen_x + 8.0f, pen_y + 2.0f, x1, y1);
                    xf(pen_x + 8.0f, pen_y + 10.0f, x2, y2);
                    xf(pen_x, pen_y + 10.0f, x3, y3);
                    push_quad(builtin_font_tex_,
                              x0, y0, u0, v0, col_tl,
                              x1, y1, u1, v0, col_tr,
                              x2, y2, u1, v1, col_br,
                              x3, y3, u0, v1, col_bl);
                }
                pen_x += 8.0f;
            }
        }
    }
}

// ---- Primitives ----

void Renderer2D::draw_point_color(f32 x, f32 y, u32 color_bgr, f32 alpha) {
    const ColorRGBA c = make_vertex_color(color_bgr, alpha);
    push_quad(TextureHandle{},
              x, y, 0.0f, 0.0f, c,
              x + 1.0f, y, 0.0f, 0.0f, c,
              x + 1.0f, y + 1.0f, 0.0f, 0.0f, c,
              x, y + 1.0f, 0.0f, 0.0f, c);
}

void Renderer2D::draw_line_color(f32 x1, f32 y1, f32 x2, f32 y2, u32 c1, u32 c2, f32 alpha) {
    ensure_batch(PrimitiveTopology::Lines, TextureHandle{});
    const ColorRGBA col1 = make_vertex_color(c1, alpha);
    const ColorRGBA col2 = make_vertex_color(c2, alpha);
    vertices_.push_back(Vertex2D{x1 + 0.5f, y1 + 0.5f, 0.0f, 0.0f, col1.r, col1.g, col1.b, col1.a});
    vertices_.push_back(Vertex2D{x2 + 0.5f, y2 + 0.5f, 0.0f, 0.0f, col2.r, col2.g, col2.b, col2.a});
}

void Renderer2D::draw_line_width_color(f32 x1, f32 y1, f32 x2, f32 y2, f32 width, u32 c1, u32 c2, f32 alpha) {
    const f32 dx = x2 - x1;
    const f32 dy = y2 - y1;
    const f32 len = std::hypot(dx, dy);
    if (len < 1e-5f) {
        draw_point_color(x1, y1, c1, alpha);
        return;
    }
    const f32 nx = -dy / len * (width * 0.5f);
    const f32 ny = dx / len * (width * 0.5f);
    const ColorRGBA col1 = make_vertex_color(c1, alpha);
    const ColorRGBA col2 = make_vertex_color(c2, alpha);
    push_quad(TextureHandle{},
              x1 + nx, y1 + ny, 0.0f, 0.0f, col1,
              x2 + nx, y2 + ny, 0.0f, 0.0f, col2,
              x2 - nx, y2 - ny, 0.0f, 0.0f, col2,
              x1 - nx, y1 - ny, 0.0f, 0.0f, col1);
}

void Renderer2D::draw_rectangle_color(f32 x1, f32 y1, f32 x2, f32 y2, u32 c1, u32 c2, u32 c3, u32 c4, bool outline, f32 alpha) {
    if (outline) {
        draw_line_color(x1, y1, x2, y1, c1, c2, alpha);
        draw_line_color(x2, y1, x2, y2, c2, c3, alpha);
        draw_line_color(x2, y2, x1, y2, c3, c4, alpha);
        draw_line_color(x1, y2, x1, y1, c4, c1, alpha);
        return;
    }
    push_quad(TextureHandle{},
              x1, y1, 0.0f, 0.0f, make_vertex_color(c1, alpha),
              x2 + 1.0f, y1, 0.0f, 0.0f, make_vertex_color(c2, alpha),
              x2 + 1.0f, y2 + 1.0f, 0.0f, 0.0f, make_vertex_color(c3, alpha),
              x1, y2 + 1.0f, 0.0f, 0.0f, make_vertex_color(c4, alpha));
}

void Renderer2D::draw_triangle_color(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3, u32 c1, u32 c2, u32 c3, bool outline, f32 alpha) {
    if (outline) {
        draw_line_color(x1, y1, x2, y2, c1, c2, alpha);
        draw_line_color(x2, y2, x3, y3, c2, c3, alpha);
        draw_line_color(x3, y3, x1, y1, c3, c1, alpha);
        return;
    }
    ensure_batch(PrimitiveTopology::Triangles, TextureHandle{});
    const ColorRGBA col1 = make_vertex_color(c1, alpha);
    const ColorRGBA col2 = make_vertex_color(c2, alpha);
    const ColorRGBA col3 = make_vertex_color(c3, alpha);
    vertices_.push_back(Vertex2D{x1, y1, 0.0f, 0.0f, col1.r, col1.g, col1.b, col1.a});
    vertices_.push_back(Vertex2D{x2, y2, 0.0f, 0.0f, col2.r, col2.g, col2.b, col2.a});
    vertices_.push_back(Vertex2D{x3, y3, 0.0f, 0.0f, col3.r, col3.g, col3.b, col3.a});
}

void Renderer2D::draw_ellipse_color(f32 x1, f32 y1, f32 x2, f32 y2, u32 c1, u32 c2, bool outline, f32 alpha) {
    const f32 cx = (x1 + x2) * 0.5f;
    const f32 cy = (y1 + y2) * 0.5f;
    const f32 rx = std::fabs(x2 - x1) * 0.5f;
    const f32 ry = std::fabs(y2 - y1) * 0.5f;
    const i32 segs = std::max(4, circle_precision_);
    const f32 step = static_cast<f32>(2.0 * kPi) / static_cast<f32>(segs);

    if (outline) {
        for (i32 i = 0; i < segs; ++i) {
            const f32 a0 = static_cast<f32>(i) * step;
            const f32 a1 = static_cast<f32>(i + 1) * step;
            draw_line_color(cx + std::cos(a0) * rx, cy + std::sin(a0) * ry,
                            cx + std::cos(a1) * rx, cy + std::sin(a1) * ry, c2, c2, alpha);
        }
        return;
    }

    const ColorRGBA col_center = make_vertex_color(c1, alpha);
    const ColorRGBA col_outer = make_vertex_color(c2, alpha);
    for (i32 i = 0; i < segs; ++i) {
        ensure_batch(PrimitiveTopology::Triangles, TextureHandle{});
        const f32 a0 = static_cast<f32>(i) * step;
        const f32 a1 = static_cast<f32>(i + 1) * step;
        vertices_.push_back(Vertex2D{cx, cy, 0.0f, 0.0f, col_center.r, col_center.g, col_center.b, col_center.a});
        vertices_.push_back(Vertex2D{cx + std::cos(a0) * rx, cy + std::sin(a0) * ry, 0.0f, 0.0f, col_outer.r, col_outer.g, col_outer.b, col_outer.a});
        vertices_.push_back(Vertex2D{cx + std::cos(a1) * rx, cy + std::sin(a1) * ry, 0.0f, 0.0f, col_outer.r, col_outer.g, col_outer.b, col_outer.a});
    }
}

void Renderer2D::draw_circle_color(f32 x, f32 y, f32 r, u32 c1, u32 c2, bool outline, f32 alpha) {
    draw_ellipse_color(x - r, y - r, x + r, y + r, c1, c2, outline, alpha);
}

// ---- Immediate Primitive Builder ----

void Renderer2D::primitive_begin(i32 kind, TextureHandle tex) {
    in_primitive_ = true;
    prim_kind_ = kind;
    prim_tex_ = tex;
    prim_verts_.clear();
}

void Renderer2D::primitive_vertex(f32 x, f32 y, f32 u, f32 v, u32 color_bgr, f32 alpha) {
    if (!in_primitive_) return;
    const ColorRGBA c = make_vertex_color(color_bgr, alpha);
    prim_verts_.push_back(Vertex2D{x, y, u, v, c.r, c.g, c.b, c.a});
}

void Renderer2D::primitive_end() {
    if (!in_primitive_) return;
    in_primitive_ = false;
    if (prim_verts_.empty()) return;

    PrimitiveTopology topo = PrimitiveTopology::Triangles;
    switch (prim_kind_) {
        case 1: topo = PrimitiveTopology::Points; break;
        case 2: topo = PrimitiveTopology::Lines; break;
        case 3: topo = PrimitiveTopology::LineStrip; break;
        case 4: topo = PrimitiveTopology::Triangles; break;
        case 5: topo = PrimitiveTopology::TriangleStrip; break;
        case 6: topo = PrimitiveTopology::TriangleFan; break;
        default: topo = PrimitiveTopology::Triangles; break;
    }

    flush();
    batch_topo_ = topo;
    batch_tex_ = prim_tex_;
    vertices_ = std::move(prim_verts_);
    prim_verts_.clear();
    flush();
}

Result<Bitmap32> Renderer2D::decode_image_to_bgra(std::span<const u8> encoded_bytes) {
    if (encoded_bytes.empty()) {
        return Error{"Empty image data"};
    }
    int w = 0, h = 0, ch = 0;
    stbi_uc* rgba = stbi_load_from_memory(
        encoded_bytes.data(), static_cast<int>(encoded_bytes.size()), &w, &h, &ch, 4);
    if (!rgba) {
        return Error{std::string("stbi_load_from_memory failed: ") + stbi_failure_reason()};
    }

    Bitmap32 bmp{};
    bmp.width = w;
    bmp.height = h;
    const usize nbytes = static_cast<usize>(w) * static_cast<usize>(h) * 4u;
    bmp.bgra.resize(nbytes);
    for (usize i = 0; i < nbytes; i += 4) {
        bmp.bgra[i + 0] = rgba[i + 2];
        bmp.bgra[i + 1] = rgba[i + 1];
        bmp.bgra[i + 2] = rgba[i + 0];
        bmp.bgra[i + 3] = rgba[i + 3];
    }
    stbi_image_free(rgba);
    return bmp;
}

} // namespace gm
