#include "graphics/renderer2d.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "graphics/gl_headers.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
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

void mat4_identity(f32 m[16]) noexcept {
    std::memset(m, 0, sizeof(f32) * 16u);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Column-major 4x4 matrix multiply: out = a * b
void mat4_mul(f32 out[16], const f32 a[16], const f32 b[16]) noexcept {
    f32 r[16]{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

bool write_rgba_to_file(const u8* rgba, i32 w, i32 h, std::string_view host_path) {
    if (!rgba || w <= 0 || h <= 0) return false;
    const std::string path(host_path);
    const std::string lower = ascii_to_lower(host_path);
    if (lower.ends_with(".bmp")) {
        return stbi_write_bmp(path.c_str(), w, h, 4, rgba) != 0;
    }
    if (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) {
        return stbi_write_jpg(path.c_str(), w, h, 4, rgba, 90) != 0;
    }
    return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
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
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
uniform mat4 u_proj;
out vec2 v_uv;
out vec4 v_col;
void main() {
    v_uv = a_uv;
    v_col = a_col;
    gl_Position = u_proj * vec4(a_pos, 1.0);
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
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<const void*>(offsetof(Vertex2D, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<const void*>(offsetof(Vertex2D, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex2D), reinterpret_cast<const void*>(offsetof(Vertex2D, r)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    vertices_.reserve(kMaxBatchVertices);
    create_builtin_font_texture();

    // Identity default projection & model transform
    mat4_identity(proj_matrix_);
    mat4_identity(model_matrix_);

    initialized_ = true;
    return Result<void>::success();
}

void Renderer2D::shutdown() noexcept {
    if (!initialized_) return;

    if (screen_fbo_.valid()) {
        device_->destroy_framebuffer(screen_fbo_);
        screen_fbo_ = {};
    }
    if (screen_fbo_tex_.valid()) {
        device_->destroy_texture(screen_fbo_tex_);
        screen_fbo_tex_ = {};
    }
    screen_fbo_w_ = 0;
    screen_fbo_h_ = 0;
    using_screen_fbo_ = false;

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

    if (screen_fbo_.valid()) {
        device_->destroy_framebuffer(screen_fbo_);
        screen_fbo_ = {};
    }
    if (screen_fbo_tex_.valid()) {
        device_->destroy_texture(screen_fbo_tex_);
        screen_fbo_tex_ = {};
    }
    screen_fbo_w_ = 0;
    screen_fbo_h_ = 0;
    using_screen_fbo_ = false;

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
            upload_font(i, game.fonts[i], game.meta.detected_version);
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

void Renderer2D::upload_font(usize font_idx, const FontData& font, VersionMode version) {
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
    // GM8.0 takes the height of the first character in the font's range.
    if (version == VersionMode::GM80) {
        const usize first = static_cast<usize>(font.first);
        fg.line_height = first < font.glyphs.size() ? font.glyphs[first].h : 0;
    } else {
        fg.line_height = std::ranges::max_element(font.glyphs, {}, &GlyphData::h)->h;
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

void Renderer2D::ensure_screen_fbo(i32 width, i32 height) {
    const i32 w = std::max(1, width);
    const i32 h = std::max(1, height);
    if (screen_fbo_.valid() && screen_fbo_w_ == w && screen_fbo_h_ == h) {
        return;
    }
    if (screen_fbo_.valid()) {
        device_->destroy_framebuffer(screen_fbo_);
        screen_fbo_ = {};
    }
    if (screen_fbo_tex_.valid()) {
        device_->destroy_texture(screen_fbo_tex_);
        screen_fbo_tex_ = {};
    }
    TextureDesc desc{};
    desc.width = w;
    desc.height = h;
    desc.format = TextureFormat::RGBA8;
    desc.min_filter = linear_filter_ ? TextureFilter::Linear : TextureFilter::Nearest;
    desc.mag_filter = desc.min_filter;
    auto tex_res = device_->create_texture(desc);
    if (!tex_res) return;
    TextureHandle tex = tex_res.value();
    auto fbo_res = device_->create_framebuffer(std::span<const TextureHandle>(&tex, 1), true);
    if (!fbo_res) {
        device_->destroy_texture(tex);
        return;
    }
    screen_fbo_tex_ = tex;
    screen_fbo_ = fbo_res.value();
    screen_fbo_w_ = w;
    screen_fbo_h_ = h;
}

void Renderer2D::bind_active_target_fbo() {
    if (active_surface_id_ >= 0 && surfaces_.contains(active_surface_id_)) {
        device_->bind_framebuffer(surfaces_[active_surface_id_].fbo);
    } else if (using_screen_fbo_ && screen_fbo_.valid()) {
        device_->bind_framebuffer(screen_fbo_);
    } else {
        device_->bind_framebuffer(FramebufferHandle{});
    }
}

void Renderer2D::apply_d3d_depth_cull() {
    if (d3d_active_ && d3d_hidden_) {
        device_->set_depth_state(DepthState{true, d3d_zwrite_, CompareFunc::LessEqual});
    } else {
        device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    }
    if (d3d_active_ && d3d_culling_) {
        device_->set_cull_mode(rendering_to_fbo() ? CullMode::Front : CullMode::Back);
    } else {
        device_->set_cull_mode(CullMode::None);
    }
}

void Renderer2D::begin_frame(i32 backbuffer_w, i32 backbuffer_h) {
    vertices_.clear();
    using_screen_fbo_ = false;
    active_surface_id_ = -1;
    device_->bind_framebuffer(FramebufferHandle{});
    set_screen_region(backbuffer_w, backbuffer_h, backbuffer_w, backbuffer_h, {0, 0, backbuffer_w, backbuffer_h});
    apply_d3d_depth_cull();
    device_->set_blend_state(blend_state_);
}

void Renderer2D::begin_screen_pass(i32 canvas_w, i32 canvas_h, i32 backbuffer_w, i32 backbuffer_h, const ViewportRect& present_viewport) {
    flush();
    backbuffer_w_ = std::max(1, backbuffer_w);
    backbuffer_h_ = std::max(1, backbuffer_h);
    screen_w_ = std::max(1, canvas_w);
    screen_h_ = std::max(1, canvas_h);
    screen_viewport_ = present_viewport;

    if (active_surface_id_ < 0) {
        ensure_screen_fbo(screen_w_, screen_h_);
        using_screen_fbo_ = screen_fbo_.valid();
        bind_active_target_fbo();
        device_->set_scissor(false);
        device_->set_viewport(ViewportRect{0, 0, screen_w_, screen_h_});
        device_->clear(false, true, true, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0);
    }
    apply_d3d_depth_cull();
    device_->set_blend_state(blend_state_);
}

void Renderer2D::present_screen(u32 border_color_bgr) {
    if (!using_screen_fbo_ || !screen_fbo_tex_.valid() || active_surface_id_ >= 0) {
        flush();
        return;
    }
    flush();

    const bool saved_alpha_test = alpha_test_enabled_;
    const BlendState saved_blend = blend_state_;
    f32 saved_model[16];
    std::memcpy(saved_model, model_matrix_, sizeof(saved_model));

    using_screen_fbo_ = false;
    device_->bind_framebuffer(FramebufferHandle{});
    device_->set_scissor(false);
    device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    device_->set_cull_mode(CullMode::None);
    device_->set_color_mask(true, true, true, true);
    device_->set_viewport(ViewportRect{0, 0, backbuffer_w_, backbuffer_h_});
    clear_color(border_color_bgr, 1.0f);

    alpha_test_enabled_ = false;
    blend_state_.enabled = false;
    mat4_identity(model_matrix_);
    set_projection_ortho(0.0f, 0.0f, static_cast<f32>(screen_w_), static_cast<f32>(screen_h_), 0.0f,
                         screen_viewport_.x, screen_viewport_.y, screen_viewport_.width, screen_viewport_.height);
    const ColorRGBA white{255, 255, 255, 255};
    push_quad(screen_fbo_tex_,
              0.0f, 0.0f, 0.0f, 0.0f, white,
              static_cast<f32>(screen_w_), 0.0f, 1.0f, 0.0f, white,
              static_cast<f32>(screen_w_), static_cast<f32>(screen_h_), 1.0f, 1.0f, white,
              0.0f, static_cast<f32>(screen_h_), 0.0f, 1.0f, white);
    flush();

    std::memcpy(model_matrix_, saved_model, sizeof(saved_model));
    blend_state_ = saved_blend;
    alpha_test_enabled_ = saved_alpha_test;
    device_->set_blend_state(blend_state_);
    apply_d3d_depth_cull();
}

void Renderer2D::set_screen_region(i32 backbuffer_w, i32 backbuffer_h, i32 width, i32 height, const ViewportRect& viewport) {
    backbuffer_w_ = std::max(1, backbuffer_w);
    backbuffer_h_ = std::max(1, backbuffer_h);
    screen_w_ = width;
    screen_h_ = height;
    screen_viewport_ = viewport;
    if (initialized_ && active_surface_id_ < 0) {
        apply_screen_projection();
    }
}

void Renderer2D::end_frame() {
    flush();
}

void Renderer2D::flush() {
    if (vertices_.empty()) {
        return;
    }

    f32 mvp[16];
    mat4_mul(mvp, proj_matrix_, model_matrix_);

    device_->bind_shader(shader_);
    glUniformMatrix4fv(loc_proj_, 1, GL_FALSE, mvp);
    glUniform1i(loc_tex_, 0);
    glUniform1i(loc_alpha_test_, alpha_test_enabled_ ? 1 : 0);
    glUniform1f(loc_alpha_ref_, alpha_test_ref_);

    device_->set_blend_state(blend_state_);
    if (batch_tex_.valid()) {
        const TextureFilter f = linear_filter_ ? TextureFilter::Linear : TextureFilter::Nearest;
        const TextureWrap w = tex_repeat_ ? TextureWrap::Repeat : TextureWrap::ClampToEdge;
        device_->set_texture_sampler(batch_tex_, f, w, w);
    }
    device_->bind_texture(0, batch_tex_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const GLenum gl_topo = to_gl_topo(batch_topo_);
    usize offset = 0;
    while (offset < vertices_.size()) {
        const usize count = std::min(vertices_.size() - offset, kMaxBatchVertices);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(count * sizeof(Vertex2D)), vertices_.data() + offset);
        glDrawArrays(gl_topo, 0, static_cast<GLsizei>(count));
        offset += count;
    }
    glBindVertexArray(0);

    vertices_.clear();
}

void Renderer2D::set_projection_ortho(
    f32 view_x, f32 view_y, f32 view_w, f32 view_h, f32 angle_deg,
    i32 port_x, i32 port_y, i32 port_w, i32 port_h) {
    flush();

    const bool to_fbo = rendering_to_fbo();
    const i32 target_h = (active_surface_id_ >= 0) ? surface_height(active_surface_id_)
                         : (using_screen_fbo_ ? screen_fbo_h_ : backbuffer_h_);

    // OpenGL glViewport origin is bottom-left.
    // On backbuffer: top-down port_y -> gl_y = backbuffer_h - (port_y + port_h).
    // On FBO: world y=0 maps to FBO row 0 (gl_y = port_y) so surfaces sample right-side up!
    const i32 gl_y = to_fbo ? port_y : (target_h - (port_y + port_h));
    active_viewport_ = ViewportRect{port_x, port_y, std::max(1, port_w), std::max(1, port_h)};
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

    // Column-major 4x4: Scale(sx, sy, -1/16000) * RotZ(angle) * Translate(-cx, -cy, 0)
    std::memset(proj_matrix_, 0, sizeof(proj_matrix_));
    proj_matrix_[0] = c * sx;
    proj_matrix_[1] = -s * sy;
    proj_matrix_[4] = s * sx;
    proj_matrix_[5] = c * sy;
    proj_matrix_[10] = -1.0f / 16000.0f;
    proj_matrix_[12] = (-cx * c - cy * s) * sx;
    proj_matrix_[13] = (cx * s - cy * c) * sy;
    proj_matrix_[15] = 1.0f;
}

void Renderer2D::clear_color(u32 gm_bgr, f32 alpha) {
    flush();
    const ColorRGBA c = gm_bgr_to_rgba(gm_bgr);
    device_->clear(true, d3d_active_ && d3d_hidden_, false,
                   static_cast<f32>(c.r) / 255.0f,
                   static_cast<f32>(c.g) / 255.0f,
                   static_cast<f32>(c.b) / 255.0f,
                   alpha, 1.0f, 0);
}

void Renderer2D::clear_depth(f32 depth_val) {
    flush();
    device_->clear(false, true, false, 0.0f, 0.0f, 0.0f, 1.0f, depth_val, 0);
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
    vertices_.push_back(Vertex2D{x0, y0, current_draw_z_, u0, v0, c0.r, c0.g, c0.b, c0.a});
    vertices_.push_back(Vertex2D{x1, y1, current_draw_z_, u1, v1, c1.r, c1.g, c1.b, c1.a});
    vertices_.push_back(Vertex2D{x2, y2, current_draw_z_, u2, v2, c2.r, c2.g, c2.b, c2.a});
    vertices_.push_back(Vertex2D{x0, y0, current_draw_z_, u0, v0, c0.r, c0.g, c0.b, c0.a});
    vertices_.push_back(Vertex2D{x2, y2, current_draw_z_, u2, v2, c2.r, c2.g, c2.b, c2.a});
    vertices_.push_back(Vertex2D{x3, y3, current_draw_z_, u3, v3, c3.r, c3.g, c3.b, c3.a});
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

    return id;
}

void Renderer2D::surface_free(i32 id) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return;
    if (active_surface_id_ == id) {
        surface_reset_target();
    } else if (batch_tex_ == it->second.texture) {
        flush();
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
    if (active_surface_id_ == id) return true;
    flush();
    active_surface_id_ = id;
    device_->bind_framebuffer(it->second.fbo);
    device_->set_scissor(false);
    set_projection_ortho(0.0f, 0.0f, static_cast<f32>(it->second.width), static_cast<f32>(it->second.height),
                         0.0f, 0, 0, it->second.width, it->second.height);
    return true;
}

bool Renderer2D::surface_reset_target() {
    if (active_surface_id_ < 0) return false;
    flush();
    active_surface_id_ = -1;
    bind_active_target_fbo();
    apply_screen_projection();
    apply_d3d_depth_cull();
    return true;
}

void Renderer2D::apply_screen_projection() {
    // GM8 restores the whole logical screen even if a view was active before the surface.
    if (using_screen_fbo_) {
        set_projection_ortho(0.0f, 0.0f, static_cast<f32>(screen_w_), static_cast<f32>(screen_h_),
                             0.0f, 0, 0, screen_w_, screen_h_);
        device_->set_scissor(false);
    } else {
        const auto& port = screen_viewport_;
        set_projection_ortho(0.0f, 0.0f, static_cast<f32>(screen_w_), static_cast<f32>(screen_h_),
                             0.0f, port.x, port.y, port.width, port.height);
        device_->set_scissor(true, {port.x, backbuffer_h_ - port.y - port.height, port.width, port.height});
    }
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

void Renderer2D::draw_surface_tiled_ext(
    i32 id, f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha,
    f32 view_x, f32 view_y, f32 view_w, f32 view_h) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return;
    const f32 tw = std::fabs(static_cast<f32>(it->second.width) * xscale);
    const f32 th = std::fabs(static_cast<f32>(it->second.height) * yscale);
    if (tw < 1.0f || th < 1.0f) return;

    f32 sx = x - std::floor((x - view_x) / tw) * tw;
    if (sx > view_x) sx -= tw;
    f32 sy = y - std::floor((y - view_y) / th) * th;
    if (sy > view_y) sy -= th;

    const ColorRGBA col = make_vertex_color(color_bgr, alpha);
    for (f32 py = sy; py < view_y + view_h; py += th) {
        for (f32 px = sx; px < view_x + view_w; px += tw) {
            push_quad(it->second.texture,
                      px, py, 0.0f, 0.0f, col,
                      px + tw, py, 1.0f, 0.0f, col,
                      px + tw, py + th, 1.0f, 1.0f, col,
                      px, py + th, 0.0f, 1.0f, col);
        }
    }
}

void Renderer2D::draw_surface_general(
    i32 id, f32 left, f32 top, f32 w, f32 h, f32 x, f32 y,
    f32 xscale, f32 yscale, f32 rot_deg,
    u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha) {
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
    push_quad(it->second.texture,
              x0, y0, u0, v0, make_vertex_color(c1, alpha),
              x1, y1, u1, v0, make_vertex_color(c2, alpha),
              x2, y2, u1, v1, make_vertex_color(c3, alpha),
              x3, y3, u0, v1, make_vertex_color(c4, alpha));
}

bool Renderer2D::surface_read_pixels_bgra(i32 id, i32 x, i32 y, i32 w, i32 h, std::vector<u8>& out_bgra) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end() || w <= 0 || h <= 0) return false;
    flush();

    device_->bind_framebuffer(it->second.fbo);
    std::vector<u8> rgba(static_cast<usize>(w) * static_cast<usize>(h) * 4u);
    device_->read_pixels_rgba8(x, y, w, h, rgba);
    bind_active_target_fbo();

    out_bgra.resize(rgba.size());
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        out_bgra[i + 0] = rgba[i + 2];
        out_bgra[i + 1] = rgba[i + 1];
        out_bgra[i + 2] = rgba[i + 0];
        out_bgra[i + 3] = rgba[i + 3];
    }
    return true;
}

bool Renderer2D::screen_read_pixels_bgra(i32 x, i32 y, i32 w, i32 h, std::vector<u8>& out_bgra) {
    if (w <= 0 || h <= 0) return false;
    flush();

    std::vector<u8> rgba(static_cast<usize>(w) * static_cast<usize>(h) * 4u, 0);
    if (screen_fbo_.valid() && screen_fbo_w_ > 0 && screen_fbo_h_ > 0) {
        device_->bind_framebuffer(screen_fbo_);
        const i32 rx = std::clamp(x, 0, screen_fbo_w_);
        const i32 ry = std::clamp(y, 0, screen_fbo_h_);
        const i32 rw = std::clamp(w, 0, screen_fbo_w_ - rx);
        const i32 rh = std::clamp(h, 0, screen_fbo_h_ - ry);
        if (rw > 0 && rh > 0) {
            std::vector<u8> sub(static_cast<usize>(rw) * static_cast<usize>(rh) * 4u);
            device_->read_pixels_rgba8(rx, ry, rw, rh, sub);
            for (i32 row = 0; row < rh; ++row) {
                std::memcpy(rgba.data() + static_cast<usize>(row) * static_cast<usize>(w) * 4u,
                            sub.data() + static_cast<usize>(row) * static_cast<usize>(rw) * 4u,
                            static_cast<usize>(rw) * 4u);
            }
        }
        bind_active_target_fbo();
    } else {
        device_->bind_framebuffer(FramebufferHandle{});
        const i32 gl_y = std::max(0, backbuffer_h_ - (y + h));
        device_->read_pixels_rgba8(x, gl_y, w, h, rgba);
        bind_active_target_fbo();
        const usize row_bytes = static_cast<usize>(w) * 4u;
        std::vector<u8> tmp(row_bytes);
        for (i32 row = 0; row < h / 2; ++row) {
            u8* top = rgba.data() + static_cast<usize>(row) * row_bytes;
            u8* bot = rgba.data() + static_cast<usize>(h - 1 - row) * row_bytes;
            std::memcpy(tmp.data(), top, row_bytes);
            std::memcpy(top, bot, row_bytes);
            std::memcpy(bot, tmp.data(), row_bytes);
        }
    }

    out_bgra.resize(rgba.size());
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        out_bgra[i + 0] = rgba[i + 2];
        out_bgra[i + 1] = rgba[i + 1];
        out_bgra[i + 2] = rgba[i + 0];
        out_bgra[i + 3] = 255;
    }
    return true;
}

u32 Renderer2D::surface_getpixel(i32 id, i32 x, i32 y) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end() || x < 0 || y < 0 || x >= it->second.width || y >= it->second.height) return 0u;
    std::vector<u8> bgra;
    if (!surface_read_pixels_bgra(id, x, y, 1, 1, bgra) || bgra.size() < 4) return 0u;
    return static_cast<u32>(bgra[2]) | (static_cast<u32>(bgra[1]) << 8u) | (static_cast<u32>(bgra[0]) << 16u);
}

u32 Renderer2D::screen_getpixel(i32 x, i32 y) {
    if (active_surface_id_ >= 0) {
        return surface_getpixel(active_surface_id_, x, y);
    }
    std::vector<u8> bgra;
    if (!screen_read_pixels_bgra(x, y, 1, 1, bgra) || bgra.size() < 4) return 0u;
    return static_cast<u32>(bgra[2]) | (static_cast<u32>(bgra[1]) << 8u) | (static_cast<u32>(bgra[0]) << 16u);
}

void Renderer2D::surface_copy_part(i32 dst_id, f32 x, f32 y, i32 src_id, f32 xs, f32 ys, f32 ws, f32 hs) {
    if (!surfaces_.contains(dst_id) || !surfaces_.contains(src_id)) return;
    const i32 prev = active_surface_id_;
    const BlendState prev_blend = blend_state_;
    surface_set_target(dst_id);
    flush();
    blend_state_.enabled = false;
    draw_surface_part_ext(src_id, xs, ys, ws, hs, x, y, 1.0f, 1.0f, 0xFFFFFFu, 1.0f);
    flush();
    blend_state_ = prev_blend;
    if (prev >= 0) surface_set_target(prev);
    else surface_reset_target();
}

bool Renderer2D::save_bitmap_bgra(const Bitmap32& bmp, std::string_view host_path) {
    if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgra.size() < static_cast<usize>(bmp.width) * static_cast<usize>(bmp.height) * 4u) {
        return false;
    }
    std::vector<u8> rgba(bmp.bgra.size());
    for (usize i = 0; i + 3 < bmp.bgra.size(); i += 4) {
        rgba[i + 0] = bmp.bgra[i + 2];
        rgba[i + 1] = bmp.bgra[i + 1];
        rgba[i + 2] = bmp.bgra[i + 0];
        rgba[i + 3] = bmp.bgra[i + 3];
    }
    return write_rgba_to_file(rgba.data(), bmp.width, bmp.height, host_path);
}

bool Renderer2D::surface_save(i32 id, std::string_view host_path) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return false;
    return surface_save_part(id, host_path, 0, 0, it->second.width, it->second.height);
}

bool Renderer2D::surface_save_part(i32 id, std::string_view host_path, i32 x, i32 y, i32 w, i32 h) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end() || w <= 0 || h <= 0) return false;
    flush();
    device_->bind_framebuffer(it->second.fbo);
    std::vector<u8> rgba(static_cast<usize>(w) * static_cast<usize>(h) * 4u);
    device_->read_pixels_rgba8(x, y, w, h, rgba);
    bind_active_target_fbo();
    return write_rgba_to_file(rgba.data(), w, h, host_path);
}

bool Renderer2D::screen_save(std::string_view host_path) {
    const i32 w = (screen_fbo_.valid() && screen_fbo_w_ > 0) ? screen_fbo_w_ : backbuffer_w_;
    const i32 h = (screen_fbo_.valid() && screen_fbo_h_ > 0) ? screen_fbo_h_ : backbuffer_h_;
    return screen_save_part(host_path, 0, 0, w, h);
}

bool Renderer2D::screen_save_part(std::string_view host_path, i32 x, i32 y, i32 w, i32 h) {
    std::vector<u8> bgra;
    if (!screen_read_pixels_bgra(x, y, w, h, bgra)) return false;
    Bitmap32 bmp{w, h, std::move(bgra)};
    return save_bitmap_bgra(bmp, host_path);
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

f32 Renderer2D::measure_string_height(const GameData& game, std::string_view text, i32 sep, i32 max_w) const {
    const auto lines = split_gm_lines(game, text, max_w);
    if (lines.empty()) return 0.0f;
    const f32 lh = current_font_line_height(game);
    const f32 step = (sep < 0) ? lh : static_cast<f32>(sep);
    return lh + static_cast<f32>(lines.size() - 1) * step;
}

void Renderer2D::draw_text_ext_color(
    const GameData& game, f32 x, f32 y, std::string_view text,
    i32 sep, i32 max_w, u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha,
    std::optional<TextTransform> transform) {
    if (text.empty()) return;

    const auto lines = split_gm_lines(game, text, max_w);
    const f32 lh = current_font_line_height(game);
    const f32 line_step = (sep < 0) ? lh : static_cast<f32>(sep);
    const auto [xscale, yscale, angle_deg] = transform.value_or(TextTransform{});
    const f64 rad = deg_to_rad(angle_deg);
    const f64 cs = std::cos(rad);
    const f64 sn = std::sin(rad);
    auto aligned_extent = [](i32 alignment, f64 extent) {
        if (alignment == 1) return extent * 0.5;
        if (alignment == 2) return extent;
        return 0.0;
    };
    // Plain text uses integer division for centering. Transformed text rounds
    // the scaled offsets, even for an explicitly supplied identity transform.
    auto align_pixels = [&](f64 offset) {
        return transform ? delphi_round_f64(offset) : std::trunc(offset);
    };
    const f64 height_offset = aligned_extent(draw_valign_, static_cast<f64>(lines.size()) * line_step * yscale);
    const f64 base_x = x - align_pixels(height_offset * sn);
    const f64 base_y = y - align_pixels(height_offset * cs);

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
        const f64 width_offset = align_pixels(aligned_extent(draw_halign_, static_cast<f64>(lw) * xscale));
        const f64 line_x = base_x - width_offset * cs;
        const f64 line_y = base_y + width_offset * sn;
        auto xf = [&](f32 lx, f32 ly, f32& ox, f32& oy) {
            const f64 sx = static_cast<f64>(lx) * xscale;
            const f64 sy = static_cast<f64>(ly) * yscale;
            ox = static_cast<f32>(line_x + sx * cs + sy * sn);
            oy = static_cast<f32>(line_y - sx * sn + sy * cs);
        };
        f32 pen_x = 0.0f;
        const f32 pen_y = static_cast<f32>(li) * line_step;

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
    vertices_.push_back(Vertex2D{x1 + 0.5f, y1 + 0.5f, current_draw_z_, 0.0f, 0.0f, col1.r, col1.g, col1.b, col1.a});
    vertices_.push_back(Vertex2D{x2 + 0.5f, y2 + 0.5f, current_draw_z_, 0.0f, 0.0f, col2.r, col2.g, col2.b, col2.a});
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
    vertices_.push_back(Vertex2D{x1, y1, current_draw_z_, 0.0f, 0.0f, col1.r, col1.g, col1.b, col1.a});
    vertices_.push_back(Vertex2D{x2, y2, current_draw_z_, 0.0f, 0.0f, col2.r, col2.g, col2.b, col2.a});
    vertices_.push_back(Vertex2D{x3, y3, current_draw_z_, 0.0f, 0.0f, col3.r, col3.g, col3.b, col3.a});
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
        vertices_.push_back(Vertex2D{cx, cy, current_draw_z_, 0.0f, 0.0f, col_center.r, col_center.g, col_center.b, col_center.a});
        vertices_.push_back(Vertex2D{cx + std::cos(a0) * rx, cy + std::sin(a0) * ry, current_draw_z_, 0.0f, 0.0f, col_outer.r, col_outer.g, col_outer.b, col_outer.a});
        vertices_.push_back(Vertex2D{cx + std::cos(a1) * rx, cy + std::sin(a1) * ry, current_draw_z_, 0.0f, 0.0f, col_outer.r, col_outer.g, col_outer.b, col_outer.a});
    }
}

void Renderer2D::draw_circle_color(f32 x, f32 y, f32 r, u32 c1, u32 c2, bool outline, f32 alpha) {
    draw_ellipse_color(x - r, y - r, x + r, y + r, c1, c2, outline, alpha);
}

void Renderer2D::draw_sprite_pos(
    const GameData& game, i32 sprite_id, i32 subimg,
    f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3, f32 x4, f32 y4, f32 alpha) {
    if (sprite_id < 0 || static_cast<usize>(sprite_id) >= game.sprites.size()) return;
    if (!game.sprites[static_cast<usize>(sprite_id)].exists) return;
    const TextureHandle tex = sprite_texture(sprite_id, subimg);
    if (!tex.valid()) return;
    const ColorRGBA col = make_vertex_color(0xFFFFFFu, alpha);
    push_quad(tex,
              x1, y1, 0.0f, 0.0f, col,
              x2, y2, 1.0f, 0.0f, col,
              x3, y3, 1.0f, 1.0f, col,
              x4, y4, 0.0f, 1.0f, col);
}

void Renderer2D::draw_background_general(
    const GameData& game, i32 bg_id,
    f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale,
    f32 rot_deg, u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha) {
    if (bg_id < 0 || static_cast<usize>(bg_id) >= game.backgrounds.size()) return;
    const auto& bg = game.backgrounds[static_cast<usize>(bg_id)];
    if (!bg.exists || bg.width <= 0 || bg.height <= 0) return;
    const TextureHandle tex = background_texture(bg_id);
    if (!tex.valid()) return;

    const f32 inv_w = 1.0f / static_cast<f32>(bg.width);
    const f32 inv_h = 1.0f / static_cast<f32>(bg.height);
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

void Renderer2D::draw_arrow(f32 x1, f32 y1, f32 x2, f32 y2, f32 tip_size, u32 color_bgr, f32 alpha) {
    draw_line_color(x1, y1, x2, y2, color_bgr, color_bgr, alpha);
    const f32 dx = x2 - x1;
    const f32 dy = y2 - y1;
    const f32 len = std::hypot(dx, dy);
    if (len < 1e-4f || tip_size <= 0.0f) return;
    const f32 ux = dx / len;
    const f32 uy = dy / len;
    const f32 px = -uy;
    const f32 py = ux;
    const f32 bx = x2 - ux * tip_size;
    const f32 by = y2 - uy * tip_size;
    const f32 hw = tip_size * 0.35f;
    draw_triangle_color(x2, y2, bx + px * hw, by + py * hw, bx - px * hw, by - py * hw,
                        color_bgr, color_bgr, color_bgr, false, alpha);
}

void Renderer2D::draw_button(f32 x1, f32 y1, f32 x2, f32 y2, bool up, u32 color_bgr, f32 alpha) {
    draw_rectangle_color(x1, y1, x2, y2, color_bgr, color_bgr, color_bgr, color_bgr, false, alpha);
    const u32 hi = up ? 0xFFFFFFu : 0x404040u;
    const u32 lo = up ? 0x404040u : 0xFFFFFFu;
    draw_line_color(x1, y1, x2, y1, hi, hi, alpha);
    draw_line_color(x1, y1, x1, y2, hi, hi, alpha);
    draw_line_color(x1, y2, x2, y2, lo, lo, alpha);
    draw_line_color(x2, y1, x2, y2, lo, lo, alpha);
}

// ---- Immediate Primitive Builder ----

void Renderer2D::primitive_begin(i32 kind, TextureHandle tex) {
    in_primitive_ = true;
    prim_kind_ = kind;
    prim_tex_ = tex;
    prim_verts_.clear();
}

void Renderer2D::primitive_vertex(f32 x, f32 y, f32 u, f32 v, u32 color_bgr, f32 alpha) {
    primitive_vertex_3d(x, y, current_draw_z_, u, v, color_bgr, alpha);
}

void Renderer2D::primitive_vertex_3d(f32 x, f32 y, f32 z, f32 u, f32 v, u32 color_bgr, f32 alpha) {
    if (!in_primitive_) return;
    const ColorRGBA c = make_vertex_color(color_bgr, alpha);
    prim_verts_.push_back(Vertex2D{x, y, z, u, v, c.r, c.g, c.b, c.a});
}

// ---- Fixed-Function D3D Projection, Transforms & 3D Primitives ----

void Renderer2D::d3d_start() {
    flush();
    d3d_active_ = true;
    d3d_hidden_ = true;
    d3d_perspective_ = true;
    d3d_zwrite_ = true;
    apply_d3d_depth_cull();
}

void Renderer2D::d3d_end() {
    flush();
    d3d_active_ = false;
    d3d_hidden_ = false;
    d3d_perspective_ = false;
    d3d_culling_ = false;
    d3d_zwrite_ = true;
    current_draw_z_ = 0.0f;
    mat4_identity(model_matrix_);
    transform_stack_.clear();
    apply_d3d_depth_cull();
}

void Renderer2D::d3d_set_perspective(bool enable) {
    flush();
    d3d_perspective_ = enable;
}

void Renderer2D::d3d_set_hidden(bool enable) {
    flush();
    d3d_hidden_ = enable;
    apply_d3d_depth_cull();
}

void Renderer2D::d3d_set_zwriteenable(bool enable) {
    flush();
    d3d_zwrite_ = enable;
    apply_d3d_depth_cull();
}

void Renderer2D::d3d_set_culling(bool enable) {
    flush();
    d3d_culling_ = enable;
    apply_d3d_depth_cull();
}

void Renderer2D::d3d_set_projection(f32 xfrom, f32 yfrom, f32 zfrom, f32 xto, f32 yto, f32 zto, f32 xup, f32 yup, f32 zup) {
    const f32 aspect = (active_viewport_.height > 0)
                           ? (static_cast<f32>(active_viewport_.width) / static_cast<f32>(active_viewport_.height))
                           : (4.0f / 3.0f);
    d3d_set_projection_ext(xfrom, yfrom, zfrom, xto, yto, zto, xup, yup, zup, 45.0f, aspect, 1.0f, 32000.0f);
}

void Renderer2D::d3d_set_projection_ext(
    f32 xfrom, f32 yfrom, f32 zfrom,
    f32 xto, f32 yto, f32 zto,
    f32 xup, f32 yup, f32 zup,
    f32 angle_deg, f32 aspect, f32 znear, f32 zfar) {
    flush();

    // Left-handed LookAt matrix matching Direct3D8 D3DXMatrixLookAtLH
    f32 zx = xto - xfrom, zy = yto - yfrom, zz = zto - zfrom;
    const f32 zlen = std::sqrt(zx * zx + zy * zy + zz * zz);
    if (zlen > 1e-6f) { zx /= zlen; zy /= zlen; zz /= zlen; }
    else { zx = 0.0f; zy = 0.0f; zz = 1.0f; }

    f32 xx = yup * zz - zup * zy;
    f32 xy = zup * zx - xup * zz;
    f32 xz = xup * zy - yup * zx;
    const f32 xlen = std::sqrt(xx * xx + xy * xy + xz * xz);
    if (xlen > 1e-6f) { xx /= xlen; xy /= xlen; xz /= xlen; }
    else { xx = 1.0f; xy = 0.0f; xz = 0.0f; }

    const f32 yx = zy * xz - zz * xy;
    const f32 yy = zz * xx - zx * xz;
    const f32 yz = zx * xy - zy * xx;

    f32 view[16]{};
    view[0]  = xx; view[4]  = xy; view[8]  = xz; view[12] = -(xx * xfrom + xy * yfrom + xz * zfrom);
    view[1]  = yx; view[5]  = yy; view[9]  = yz; view[13] = -(yx * xfrom + yy * yfrom + yz * zfrom);
    view[2]  = zx; view[6]  = zy; view[10] = zz; view[14] = -(zx * xfrom + zy * yfrom + zz * zfrom);
    view[3]  = 0.0f; view[7] = 0.0f; view[11] = 0.0f; view[15] = 1.0f;

    const f32 fov = static_cast<f32>(deg_to_rad(std::clamp(angle_deg, 1.0f, 179.0f)));
    const f32 f = 1.0f / std::tan(fov * 0.5f);
    const f32 asp = (std::fabs(aspect) > 1e-4f) ? aspect : 1.0f;
    const f32 zn = std::max(0.01f, znear);
    const f32 zf = std::max(zn + 0.1f, zfar);

    // D3DXMatrixPerspectiveFovLH mapped to OpenGL NDC Z [-1, +1]
    // Flip Y when rendering to an FBO where row 0 is screen top.
    const f32 y_flip = rendering_to_fbo() ? -1.0f : 1.0f;
    f32 proj[16]{};
    proj[0]  = f / asp;
    proj[5]  = f * y_flip;
    proj[10] = (zf + zn) / (zf - zn);
    proj[11] = 1.0f;
    proj[14] = (-2.0f * zf * zn) / (zf - zn);

    mat4_mul(proj_matrix_, proj, view);
}

void Renderer2D::d3d_set_projection_ortho(f32 x, f32 y, f32 w, f32 h, f32 angle_deg) {
    set_projection_ortho(x, y, w, h, angle_deg,
                         active_viewport_.x, active_viewport_.y,
                         active_viewport_.width, active_viewport_.height);
}

void Renderer2D::d3d_set_projection_perspective(f32 x, f32 y, f32 w, f32 h, f32 angle_deg) {
    const f32 cx = x + w * 0.5f;
    const f32 cy = y + h * 0.5f;
    const f32 dist = h * 0.5f / std::tan(static_cast<f32>(deg_to_rad(22.5f)));
    const f32 rad = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 aspect = (std::fabs(h) > 1e-4f) ? (w / h) : 1.0f;
    d3d_set_projection_ext(cx, cy, -dist, cx, cy, 0.0f, std::sin(rad), -std::cos(rad), 0.0f, 45.0f, aspect, 1.0f, 32000.0f);
}

void Renderer2D::d3d_transform_set_identity() {
    flush();
    mat4_identity(model_matrix_);
}

void Renderer2D::d3d_transform_set_translation(f32 xt, f32 yt, f32 zt) {
    flush();
    mat4_identity(model_matrix_);
    model_matrix_[12] = xt;
    model_matrix_[13] = yt;
    model_matrix_[14] = zt;
}

void Renderer2D::d3d_transform_set_scaling(f32 xs, f32 ys, f32 zs) {
    flush();
    mat4_identity(model_matrix_);
    model_matrix_[0] = xs;
    model_matrix_[5] = ys;
    model_matrix_[10] = zs;
}

void Renderer2D::d3d_transform_set_rotation_x(f32 angle_deg) {
    flush();
    mat4_identity(model_matrix_);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    model_matrix_[5] = c;  model_matrix_[6] = -s;
    model_matrix_[9] = s;  model_matrix_[10] = c;
}

void Renderer2D::d3d_transform_set_rotation_y(f32 angle_deg) {
    flush();
    mat4_identity(model_matrix_);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    model_matrix_[0] = c;   model_matrix_[2] = s;
    model_matrix_[8] = -s;  model_matrix_[10] = c;
}

void Renderer2D::d3d_transform_set_rotation_z(f32 angle_deg) {
    flush();
    mat4_identity(model_matrix_);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    model_matrix_[0] = c;  model_matrix_[1] = -s;
    model_matrix_[4] = s;  model_matrix_[5] = c;
}

void Renderer2D::d3d_transform_set_rotation_axis(f32 xa, f32 ya, f32 za, f32 angle_deg) {
    flush();
    mat4_identity(model_matrix_);
    const f32 len = std::sqrt(xa * xa + ya * ya + za * za);
    if (len < 1e-6f) return;
    const f32 x = xa / len, y = ya / len, z = za / len;
    // Matches D3DXMatrixRotationAxis with negated angle (-deg_to_rad)
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = -std::sin(r), t = 1.0f - c;
    model_matrix_[0] = t * x * x + c;      model_matrix_[4] = t * x * y - s * z;  model_matrix_[8]  = t * x * z + s * y;
    model_matrix_[1] = t * x * y + s * z;  model_matrix_[5] = t * y * y + c;      model_matrix_[9]  = t * y * z - s * x;
    model_matrix_[2] = t * x * z - s * y;  model_matrix_[6] = t * y * z + s * x;  model_matrix_[10] = t * z * z + c;
}

void Renderer2D::d3d_transform_add_translation(f32 xt, f32 yt, f32 zt) {
    flush();
    f32 t[16];
    mat4_identity(t);
    t[12] = xt; t[13] = yt; t[14] = zt;
    mat4_mul(model_matrix_, t, model_matrix_);
}

void Renderer2D::d3d_transform_add_scaling(f32 xs, f32 ys, f32 zs) {
    flush();
    f32 t[16];
    mat4_identity(t);
    t[0] = xs; t[5] = ys; t[10] = zs;
    mat4_mul(model_matrix_, t, model_matrix_);
}

void Renderer2D::d3d_transform_add_rotation_x(f32 angle_deg) {
    flush();
    f32 t[16];
    mat4_identity(t);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    t[5] = c; t[6] = -s; t[9] = s; t[10] = c;
    mat4_mul(model_matrix_, t, model_matrix_);
}

void Renderer2D::d3d_transform_add_rotation_y(f32 angle_deg) {
    flush();
    f32 t[16];
    mat4_identity(t);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    t[0] = c; t[2] = s; t[8] = -s; t[10] = c;
    mat4_mul(model_matrix_, t, model_matrix_);
}

void Renderer2D::d3d_transform_add_rotation_z(f32 angle_deg) {
    flush();
    f32 t[16];
    mat4_identity(t);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    t[0] = c; t[1] = -s; t[4] = s; t[5] = c;
    mat4_mul(model_matrix_, t, model_matrix_);
}

void Renderer2D::d3d_transform_add_rotation_axis(f32 xa, f32 ya, f32 za, f32 angle_deg) {
    flush();
    const f32 len = std::sqrt(xa * xa + ya * ya + za * za);
    if (len < 1e-6f) return;
    const f32 x = xa / len, y = ya / len, z = za / len;
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = -std::sin(r), o = 1.0f - c;
    f32 t[16];
    mat4_identity(t);
    t[0] = o * x * x + c;      t[4] = o * x * y - s * z;  t[8]  = o * x * z + s * y;
    t[1] = o * x * y + s * z;  t[5] = o * y * y + c;      t[9]  = o * y * z - s * x;
    t[2] = o * x * z - s * y;  t[6] = o * y * z + s * x;  t[10] = o * z * z + c;
    mat4_mul(model_matrix_, t, model_matrix_);
}

void Renderer2D::d3d_transform_stack_clear() {
    transform_stack_.clear();
}

bool Renderer2D::d3d_transform_stack_push() {
    std::array<f32, 16> copy{};
    std::memcpy(copy.data(), model_matrix_, sizeof(model_matrix_));
    transform_stack_.push_back(copy);
    return true;
}

bool Renderer2D::d3d_transform_stack_pop() {
    if (transform_stack_.empty()) return false;
    flush();
    std::memcpy(model_matrix_, transform_stack_.back().data(), sizeof(model_matrix_));
    transform_stack_.pop_back();
    return true;
}

bool Renderer2D::d3d_transform_stack_top() {
    if (transform_stack_.empty()) return false;
    flush();
    std::memcpy(model_matrix_, transform_stack_.back().data(), sizeof(model_matrix_));
    return true;
}

bool Renderer2D::d3d_transform_stack_discard() {
    if (transform_stack_.empty()) return false;
    transform_stack_.pop_back();
    return true;
}

void Renderer2D::d3d_draw_block(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr) {
    const ColorRGBA col = make_vertex_color(draw_color_, draw_alpha_);
    auto quad = [&](f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz,
                    f32 cx, f32 cy, f32 cz, f32 dx, f32 dy, f32 dz) {
        ensure_batch(PrimitiveTopology::Triangles, tex);
        vertices_.push_back(Vertex2D{ax, ay, az, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{bx, by, bz, hr,   0.0f, col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{cx, cy, cz, hr,   vr,   col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{ax, ay, az, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{cx, cy, cz, hr,   vr,   col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{dx, dy, dz, 0.0f, vr,   col.r, col.g, col.b, col.a});
    };
    quad(x1, y1, z1, x2, y1, z1, x2, y2, z1, x1, y2, z1); // -Z
    quad(x2, y1, z2, x1, y1, z2, x1, y2, z2, x2, y2, z2); // +Z
    quad(x1, y1, z2, x2, y1, z2, x2, y1, z1, x1, y1, z1); // -Y
    quad(x1, y2, z1, x2, y2, z1, x2, y2, z2, x1, y2, z2); // +Y
    quad(x1, y1, z2, x1, y1, z1, x1, y2, z1, x1, y2, z2); // -X
    quad(x2, y1, z1, x2, y1, z2, x2, y2, z2, x2, y2, z1); // +X
}

void Renderer2D::d3d_draw_floor(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr) {
    const ColorRGBA col = make_vertex_color(draw_color_, draw_alpha_);
    ensure_batch(PrimitiveTopology::Triangles, tex);
    vertices_.push_back(Vertex2D{x1, y1, z1, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x1, y2, z1, 0.0f, vr,   col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x2, y2, z2, hr,   vr,   col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x1, y1, z1, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x2, y2, z2, hr,   vr,   col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x2, y1, z2, hr,   0.0f, col.r, col.g, col.b, col.a});
}

void Renderer2D::d3d_draw_wall(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr) {
    const ColorRGBA col = make_vertex_color(draw_color_, draw_alpha_);
    ensure_batch(PrimitiveTopology::Triangles, tex);
    vertices_.push_back(Vertex2D{x1, y1, z1, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x2, y2, z1, hr,   0.0f, col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x2, y2, z2, hr,   vr,   col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x1, y1, z1, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x2, y2, z2, hr,   vr,   col.r, col.g, col.b, col.a});
    vertices_.push_back(Vertex2D{x1, y1, z2, 0.0f, vr,   col.r, col.g, col.b, col.a});
}

void Renderer2D::d3d_draw_cylinder(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr, bool closed, i32 steps) {
    const i32 n = std::clamp(steps, 3, 128);
    const f32 cx = (x1 + x2) * 0.5f, cy = (y1 + y2) * 0.5f;
    const f32 rx = (x2 - x1) * 0.5f, ry = (y2 - y1) * 0.5f;
    const ColorRGBA col = make_vertex_color(draw_color_, draw_alpha_);
    for (i32 i = 0; i < n; ++i) {
        const f32 t0 = static_cast<f32>(i) / static_cast<f32>(n);
        const f32 t1 = static_cast<f32>(i + 1) / static_cast<f32>(n);
        const f32 a0 = t0 * static_cast<f32>(2.0 * kPi), a1 = t1 * static_cast<f32>(2.0 * kPi);
        const f32 px0 = cx + std::cos(a0) * rx, py0 = cy + std::sin(a0) * ry;
        const f32 px1 = cx + std::cos(a1) * rx, py1 = cy + std::sin(a1) * ry;
        ensure_batch(PrimitiveTopology::Triangles, tex);
        vertices_.push_back(Vertex2D{px0, py0, z1, t0 * hr, 0.0f, col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{px1, py1, z1, t1 * hr, 0.0f, col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{px1, py1, z2, t1 * hr, vr,   col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{px0, py0, z1, t0 * hr, 0.0f, col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{px1, py1, z2, t1 * hr, vr,   col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{px0, py0, z2, t0 * hr, vr,   col.r, col.g, col.b, col.a});
        if (closed) {
            ensure_batch(PrimitiveTopology::Triangles, tex);
            vertices_.push_back(Vertex2D{cx, cy, z1, 0.5f * hr, 0.5f * vr, col.r, col.g, col.b, col.a});
            vertices_.push_back(Vertex2D{px1, py1, z1, (0.5f + 0.5f * std::cos(a1)) * hr, (0.5f + 0.5f * std::sin(a1)) * vr, col.r, col.g, col.b, col.a});
            vertices_.push_back(Vertex2D{px0, py0, z1, (0.5f + 0.5f * std::cos(a0)) * hr, (0.5f + 0.5f * std::sin(a0)) * vr, col.r, col.g, col.b, col.a});
            ensure_batch(PrimitiveTopology::Triangles, tex);
            vertices_.push_back(Vertex2D{cx, cy, z2, 0.5f * hr, 0.5f * vr, col.r, col.g, col.b, col.a});
            vertices_.push_back(Vertex2D{px0, py0, z2, (0.5f + 0.5f * std::cos(a0)) * hr, (0.5f + 0.5f * std::sin(a0)) * vr, col.r, col.g, col.b, col.a});
            vertices_.push_back(Vertex2D{px1, py1, z2, (0.5f + 0.5f * std::cos(a1)) * hr, (0.5f + 0.5f * std::sin(a1)) * vr, col.r, col.g, col.b, col.a});
        }
    }
}

void Renderer2D::d3d_draw_cone(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr, bool closed, i32 steps) {
    const i32 n = std::clamp(steps, 3, 128);
    const f32 cx = (x1 + x2) * 0.5f, cy = (y1 + y2) * 0.5f;
    const f32 rx = (x2 - x1) * 0.5f, ry = (y2 - y1) * 0.5f;
    const ColorRGBA col = make_vertex_color(draw_color_, draw_alpha_);
    for (i32 i = 0; i < n; ++i) {
        const f32 t0 = static_cast<f32>(i) / static_cast<f32>(n);
        const f32 t1 = static_cast<f32>(i + 1) / static_cast<f32>(n);
        const f32 a0 = t0 * static_cast<f32>(2.0 * kPi), a1 = t1 * static_cast<f32>(2.0 * kPi);
        const f32 px0 = cx + std::cos(a0) * rx, py0 = cy + std::sin(a0) * ry;
        const f32 px1 = cx + std::cos(a1) * rx, py1 = cy + std::sin(a1) * ry;
        ensure_batch(PrimitiveTopology::Triangles, tex);
        vertices_.push_back(Vertex2D{cx, cy, z2, 0.5f * (t0 + t1) * hr, vr, col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{px0, py0, z1, t0 * hr, 0.0f, col.r, col.g, col.b, col.a});
        vertices_.push_back(Vertex2D{px1, py1, z1, t1 * hr, 0.0f, col.r, col.g, col.b, col.a});
        if (closed) {
            ensure_batch(PrimitiveTopology::Triangles, tex);
            vertices_.push_back(Vertex2D{cx, cy, z1, 0.5f * hr, 0.5f * vr, col.r, col.g, col.b, col.a});
            vertices_.push_back(Vertex2D{px1, py1, z1, (0.5f + 0.5f * std::cos(a1)) * hr, (0.5f + 0.5f * std::sin(a1)) * vr, col.r, col.g, col.b, col.a});
            vertices_.push_back(Vertex2D{px0, py0, z1, (0.5f + 0.5f * std::cos(a0)) * hr, (0.5f + 0.5f * std::sin(a0)) * vr, col.r, col.g, col.b, col.a});
        }
    }
}

void Renderer2D::d3d_draw_ellipsoid(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr, i32 steps) {
    const i32 stacks = std::clamp(steps / 2, 3, 64);
    const i32 slices = std::clamp(steps, 4, 128);
    const f32 cx = (x1 + x2) * 0.5f, cy = (y1 + y2) * 0.5f, cz = (z1 + z2) * 0.5f;
    const f32 rx = (x2 - x1) * 0.5f, ry = (y2 - y1) * 0.5f, rz = (z2 - z1) * 0.5f;
    const ColorRGBA col = make_vertex_color(draw_color_, draw_alpha_);
    for (i32 st = 0; st < stacks; ++st) {
        const f32 v0 = static_cast<f32>(st) / static_cast<f32>(stacks);
        const f32 v1 = static_cast<f32>(st + 1) / static_cast<f32>(stacks);
        const f32 p0 = (v0 - 0.5f) * static_cast<f32>(kPi);
        const f32 p1 = (v1 - 0.5f) * static_cast<f32>(kPi);
        for (i32 sl = 0; sl < slices; ++sl) {
            const f32 u0 = static_cast<f32>(sl) / static_cast<f32>(slices);
            const f32 u1 = static_cast<f32>(sl + 1) / static_cast<f32>(slices);
            const f32 t0 = u0 * static_cast<f32>(2.0 * kPi);
            const f32 t1 = u1 * static_cast<f32>(2.0 * kPi);
            auto pt = [&](f32 p, f32 t, f32 u, f32 v) {
                return Vertex2D{
                    cx + std::cos(p) * std::cos(t) * rx,
                    cy + std::cos(p) * std::sin(t) * ry,
                    cz + std::sin(p) * rz,
                    u * hr, v * vr, col.r, col.g, col.b, col.a
                };
            };
            ensure_batch(PrimitiveTopology::Triangles, tex);
            const Vertex2D a = pt(p0, t0, u0, v0);
            const Vertex2D b = pt(p0, t1, u1, v0);
            const Vertex2D c = pt(p1, t1, u1, v1);
            const Vertex2D d = pt(p1, t0, u0, v1);
            vertices_.push_back(a); vertices_.push_back(b); vertices_.push_back(c);
            vertices_.push_back(a); vertices_.push_back(c); vertices_.push_back(d);
        }
    }
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
