#include "graphics/renderer2d.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "graphics/builtin_font.h"
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

// Computes inverse-transpose of the upper-left 3x3 of a column-major 4x4 matrix.
void compute_normal_mat3(const f32 m[16], f32 out3[9]) noexcept {
    const f32 a00 = m[0], a01 = m[4], a02 = m[8];
    const f32 a10 = m[1], a11 = m[5], a12 = m[9];
    const f32 a20 = m[2], a21 = m[6], a22 = m[10];

    const f32 c00 = a11 * a22 - a12 * a21;
    const f32 c01 = -(a10 * a22 - a12 * a20);
    const f32 c02 = a10 * a21 - a11 * a20;

    const f32 c10 = -(a01 * a22 - a02 * a21);
    const f32 c11 = a00 * a22 - a02 * a20;
    const f32 c12 = -(a00 * a21 - a01 * a20);

    const f32 c20 = a01 * a12 - a02 * a11;
    const f32 c21 = -(a00 * a12 - a02 * a10);
    const f32 c22 = a00 * a11 - a01 * a10;

    const f32 det = a00 * c00 + a01 * c01 + a02 * c02;
    if (std::fabs(det) < 1e-12f) {
        out3[0] = 1.0f; out3[1] = 0.0f; out3[2] = 0.0f;
        out3[3] = 0.0f; out3[4] = 1.0f; out3[5] = 0.0f;
        out3[6] = 0.0f; out3[7] = 0.0f; out3[8] = 1.0f;
        return;
    }
    const f32 inv_det = 1.0f / det;
    // Column-major storage of cofactor matrix (transpose of adjugate = inverse-transpose!)
    out3[0] = c00 * inv_det; out3[1] = c10 * inv_det; out3[2] = c20 * inv_det;
    out3[3] = c01 * inv_det; out3[4] = c11 * inv_det; out3[5] = c21 * inv_det;
    out3[6] = c02 * inv_det; out3[7] = c12 * inv_det; out3[8] = c22 * inv_det;
}

f32 mat4_det(const f32 m[16]) noexcept {
    const f32 s0 = m[0] * m[5] - m[4] * m[1];
    const f32 s1 = m[0] * m[6] - m[4] * m[2];
    const f32 s2 = m[0] * m[7] - m[4] * m[3];
    const f32 s3 = m[1] * m[6] - m[5] * m[2];
    const f32 s4 = m[1] * m[7] - m[5] * m[3];
    const f32 s5 = m[2] * m[7] - m[6] * m[3];

    const f32 c5 = m[10] * m[15] - m[14] * m[11];
    const f32 c4 = m[9]  * m[15] - m[13] * m[11];
    const f32 c3 = m[9]  * m[14] - m[13] * m[10];
    const f32 c2 = m[8]  * m[15] - m[12] * m[11];
    const f32 c1 = m[8]  * m[14] - m[12] * m[10];
    const f32 c0 = m[8]  * m[13] - m[12] * m[9];

    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

bool mat4_invert(const f32 m[16], f32 inv[16]) noexcept {
    const f32 s0 = m[0] * m[5] - m[4] * m[1];
    const f32 s1 = m[0] * m[6] - m[4] * m[2];
    const f32 s2 = m[0] * m[7] - m[4] * m[3];
    const f32 s3 = m[1] * m[6] - m[5] * m[2];
    const f32 s4 = m[1] * m[7] - m[5] * m[3];
    const f32 s5 = m[2] * m[7] - m[6] * m[3];

    const f32 c5 = m[10] * m[15] - m[14] * m[11];
    const f32 c4 = m[9]  * m[15] - m[13] * m[11];
    const f32 c3 = m[9]  * m[14] - m[13] * m[10];
    const f32 c2 = m[8]  * m[15] - m[12] * m[11];
    const f32 c1 = m[8]  * m[14] - m[12] * m[10];
    const f32 c0 = m[8]  * m[13] - m[12] * m[9];

    const f32 det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (std::fabs(det) < 1e-12f) return false;
    const f32 id = 1.0f / det;

    inv[0]  = ( m[5] * c5 - m[6] * c4 + m[7] * c3) * id;
    inv[1]  = (-m[1] * c5 + m[2] * c4 - m[3] * c3) * id;
    inv[2]  = ( m[13] * s5 - m[14] * s4 + m[15] * s3) * id;
    inv[3]  = (-m[9] * s5 + m[10] * s4 - m[11] * s3) * id;

    inv[4]  = (-m[4] * c5 + m[6] * c2 - m[7] * c1) * id;
    inv[5]  = ( m[0] * c5 - m[2] * c2 + m[3] * c1) * id;
    inv[6]  = (-m[12] * s5 + m[14] * s2 - m[15] * s1) * id;
    inv[7]  = ( m[8] * s5 - m[10] * s2 + m[11] * s1) * id;

    inv[8]  = ( m[4] * c4 - m[5] * c2 + m[7] * c0) * id;
    inv[9]  = (-m[0] * c4 + m[1] * c2 - m[3] * c0) * id;
    inv[10] = ( m[12] * s4 - m[13] * s2 + m[15] * s0) * id;
    inv[11] = (-m[8] * s4 + m[9] * s2 - m[11] * s0) * id;

    inv[12] = (-m[4] * c3 + m[5] * c1 - m[6] * c0) * id;
    inv[13] = ( m[0] * c3 - m[1] * c1 + m[2] * c0) * id;
    inv[14] = (-m[12] * s3 + m[13] * s1 - m[14] * s0) * id;
    inv[15] = ( m[8] * s3 - m[9] * s1 + m[10] * s0) * id;
    return true;
}

void build_look_at_lh(f32 view[16],
                      f32 xfrom, f32 yfrom, f32 zfrom,
                      f32 xto, f32 yto, f32 zto,
                      f32 xup, f32 yup, f32 zup) noexcept {
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

    view[0]  = xx; view[4]  = xy; view[8]  = xz; view[12] = -(xx * xfrom + xy * yfrom + xz * zfrom);
    view[1]  = yx; view[5]  = yy; view[9]  = yz; view[13] = -(yx * xfrom + yy * yfrom + yz * zfrom);
    view[2]  = zx; view[6]  = zy; view[10] = zz; view[14] = -(zx * xfrom + zy * yfrom + zz * zfrom);
    view[3]  = 0.0f; view[7] = 0.0f; view[11] = 0.0f; view[15] = 1.0f;
}

// D3DXMatrixOrthoLH mapped to OpenGL NDC Z [-1, +1] (unflipped Y: +Y is screen top).
void build_ortho_lh(f32 proj[16], f32 w, f32 h, f32 zn, f32 zf) noexcept {
    std::memset(proj, 0, sizeof(f32) * 16u);
    const f32 rw = (std::fabs(w) > 1e-6f) ? w : 1.0f;
    const f32 rh = (std::fabs(h) > 1e-6f) ? h : 1.0f;
    const f32 dz = (std::fabs(zf - zn) > 1e-6f) ? (zf - zn) : 1.0f;
    proj[0]  = 2.0f / rw;
    proj[5]  = 2.0f / rh;
    proj[10] = 2.0f / dz;
    proj[14] = -(zf + zn) / dz;
    proj[15] = 1.0f;
}

// D3DXMatrixPerspectiveLH mapped to OpenGL NDC Z [-1, +1] (unflipped Y).
void build_perspective_lh(f32 proj[16], f32 w, f32 h, f32 zn, f32 zf) noexcept {
    std::memset(proj, 0, sizeof(f32) * 16u);
    const f32 rw = (std::fabs(w) > 1e-6f) ? w : 1.0f;
    const f32 rh = (std::fabs(h) > 1e-6f) ? h : 1.0f;
    const f32 dz = (std::fabs(zf - zn) > 1e-6f) ? (zf - zn) : 1.0f;
    proj[0]  = (2.0f * zn) / rw;
    proj[5]  = (2.0f * zn) / rh;
    proj[10] = (zf + zn) / dz;
    proj[11] = 1.0f;
    proj[14] = (-2.0f * zf * zn) / dz;
}

// D3DXMatrixPerspectiveFovLH mapped to OpenGL NDC Z [-1, +1] (unflipped Y).
void build_perspective_fov_lh(f32 proj[16], f32 fov_deg, f32 aspect, f32 zn, f32 zf) noexcept {
    std::memset(proj, 0, sizeof(f32) * 16u);
    const f32 fov = static_cast<f32>(deg_to_rad(std::clamp(fov_deg, 0.1f, 179.9f)));
    const f32 yscale = 1.0f / std::tan(fov * 0.5f);
    const f32 asp = (std::fabs(aspect) > 1e-6f) ? aspect : 1.0f;
    const f32 dz = (std::fabs(zf - zn) > 1e-6f) ? (zf - zn) : 1.0f;
    proj[0]  = yscale / asp;
    proj[5]  = yscale;
    proj[10] = (zf + zn) / dz;
    proj[11] = 1.0f;
    proj[14] = (-2.0f * zf * zn) / dz;
}

CompareFunc d3d_cmp_to_compare_func(i32 d3d_cmp) noexcept {
    switch (d3d_cmp) {
        case 1: return CompareFunc::Never;
        case 2: return CompareFunc::Less;
        case 3: return CompareFunc::Equal;
        case 4: return CompareFunc::LessEqual;
        case 5: return CompareFunc::Greater;
        case 6: return CompareFunc::NotEqual;
        case 7: return CompareFunc::GreaterEqual;
        case 8: return CompareFunc::Always;
        default: return CompareFunc::LessEqual;
    }
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
    const std::string vs_header = is_es
        ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
        : "#version 330 core\n";
    const std::string fs_header = is_es
        ? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
        : "#version 330 core\n";

    const std::string vs_src = vs_header + R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_norm;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_col;

uniform mat4 u_world;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec2 u_pixel_center_offset;
uniform mat3 u_normal_mat;
uniform vec4 u_model_tint;

uniform int u_lighting;
uniform vec3 u_ambient;
uniform int u_light_enabled[8];
uniform int u_light_type[8];
uniform vec3 u_light_pos[8];
uniform vec3 u_light_dir[8];
uniform vec3 u_light_col[8];
uniform float u_light_range[8];
uniform vec3 u_light_atten[8];
uniform vec3 u_spot_cone[8]; // cos(theta/2), cos(phi/2), falloff

out vec2 v_uv;
out vec4 v_col_smooth;
flat out vec4 v_col_flat;
out vec3 v_view_pos;
)" + ff_texcoord_glsl() + R"(
void main() {
    vec4 world_pos = u_world * vec4(a_pos, 1.0);
    vec4 view_pos = u_view * world_pos;
    v_view_pos = view_pos.xyz;
    v_uv = ff_texcoord(a_uv, view_pos.xyz, a_norm).xy;

    vec4 base_col = a_col * u_model_tint;
    vec4 lit_col = base_col;

    if (u_lighting != 0) {
        vec3 raw_n = u_normal_mat * a_norm;
        float nlen = length(raw_n);
        vec3 N = (nlen > 1e-6) ? (raw_n / nlen) : vec3(0.0);
        vec3 diff_acc = vec3(0.0);
        for (int i = 0; i < 8; ++i) {
            if (u_light_enabled[i] == 0) continue;
            vec3 L = vec3(0.0);
            float atten = 0.0;
            if (u_light_type[i] == 3) {
                float dlen = length(u_light_dir[i]);
                if (dlen > 1e-6) {
                    L = -u_light_dir[i] / dlen;
                    atten = 1.0;
                }
            } else {
                vec3 V = u_light_pos[i] - world_pos.xyz;
                float d = length(V);
                if (u_light_range[i] > 0.0 && d <= u_light_range[i]) {
                    L = (d > 1e-6) ? (V / d) : vec3(0.0);
                    float denom = u_light_atten[i].x + u_light_atten[i].y * d + u_light_atten[i].z * d * d;
                    atten = 1.0 / max(1e-6, denom);
                    if (u_light_type[i] == 2) {
                        float dir_len = length(u_light_dir[i]);
                        float rho = (dir_len > 0.0) ? dot(-L, u_light_dir[i] / dir_len) : -1.0;
                        vec3 cone = u_spot_cone[i];
                        if (rho < cone.x) {
                            atten *= (rho <= cone.y) ? 0.0 : pow((rho - cone.y) / (cone.x - cone.y), cone.z);
                        }
                    }
                }
            }
            float ndotl = max(0.0, dot(N, L));
            diff_acc += u_light_col[i] * (atten * ndotl);
        }
        lit_col.rgb = clamp(base_col.rgb * diff_acc + u_ambient, 0.0, 1.0);
        lit_col.a = base_col.a;
    }

    v_col_smooth = lit_col;
    v_col_flat = lit_col;
    gl_Position = u_proj * view_pos;
    gl_Position.xy += u_pixel_center_offset * gl_Position.w;
    gl_PointSize = 1.0;
}
)";

    const std::string fs_src = fs_header + (sharp_scale_ ? "#define SHARP_SCALE\n" : "") + R"(
in vec2 v_uv;
in vec4 v_col_smooth;
flat in vec4 v_col_flat;
in vec3 v_view_pos;

uniform sampler2D u_tex;
#ifdef SHARP_SCALE
uniform float u_sharp_factor;
#endif
uniform int u_tex_blending;
uniform int u_flat_shading;
uniform int u_alpha_test;
uniform float u_alpha_ref;
uniform int u_alpha_func;
uniform int u_fog_mode;
uniform vec3 u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;

out vec4 o_color;

bool pass_alpha_test(float a, float ref_v, int fn) {
    if (fn == 1) return false;                  // NEVER
    if (fn == 2) return a < ref_v;              // LESS
    if (fn == 3) return abs(a - ref_v) < 0.002; // EQUAL
    if (fn == 4) return a <= ref_v;             // LESSEQUAL
    if (fn == 5) return a > ref_v;              // GREATER
    if (fn == 6) return abs(a - ref_v) >= 0.002;// NOTEQUAL
    if (fn == 7) return a >= ref_v;             // GREATEREQUAL
    return true;                                // ALWAYS
}

void main() {
    vec2 uv = v_uv;
#ifdef SHARP_SCALE
    if (u_sharp_factor > 0.0) {
        // Equivalent to nearest integer enlargement followed by a bilinear sample.
        vec2 size = vec2(textureSize(u_tex, 0));
        vec2 pos = uv * size - 0.5;
        vec2 weight = clamp((fract(pos) - 0.5) * u_sharp_factor + 0.5, 0.0, 1.0);
        uv = (floor(pos) + 0.5 + weight) / size;
    }
#endif
    vec4 texel = texture(u_tex, uv);
    vec4 v_col = (u_flat_shading != 0) ? v_col_flat : v_col_smooth;
    vec4 c = (u_tex_blending != 0) ? (texel * v_col) : texel;

    if (u_alpha_test != 0 && !pass_alpha_test(c.a, u_alpha_ref, u_alpha_func)) {
        discard;
    }

    if (u_fog_mode != 0) {
        float d = (u_fog_mode == 2) ? length(v_view_pos) : v_view_pos.z;
        float denom = u_fog_end - u_fog_start;
        float f = (abs(denom) > 1e-6)
            ? clamp((u_fog_end - d) / denom, 0.0, 1.0)
            : ((d < u_fog_end) ? 1.0 : 0.0);
        c.rgb = mix(u_fog_color, c.rgb, f);
    }

    o_color = c;
}
)";

    auto sh_res = device_->create_shader(vs_src, fs_src);
    if (!sh_res) return sh_res.error();
    shader_ = sh_res.value();

    loc_world_ = glGetUniformLocation(shader_.id, "u_world");
    loc_view_ = glGetUniformLocation(shader_.id, "u_view");
    loc_proj_ = glGetUniformLocation(shader_.id, "u_proj");
    loc_pixel_center_offset_ = glGetUniformLocation(shader_.id, "u_pixel_center_offset");
    loc_normal_mat_ = glGetUniformLocation(shader_.id, "u_normal_mat");
    loc_model_tint_ = glGetUniformLocation(shader_.id, "u_model_tint");
    loc_tex_ = glGetUniformLocation(shader_.id, "u_tex");
    loc_sharp_factor_ = glGetUniformLocation(shader_.id, "u_sharp_factor");
    loc_tex_blending_ = glGetUniformLocation(shader_.id, "u_tex_blending");
    loc_texcoord_mode_ = glGetUniformLocation(shader_.id, "u_texcoord_mode");
    loc_alpha_test_ = glGetUniformLocation(shader_.id, "u_alpha_test");
    loc_alpha_ref_ = glGetUniformLocation(shader_.id, "u_alpha_ref");
    loc_alpha_func_ = glGetUniformLocation(shader_.id, "u_alpha_func");
    loc_flat_shading_ = glGetUniformLocation(shader_.id, "u_flat_shading");
    loc_lighting_ = glGetUniformLocation(shader_.id, "u_lighting");
    loc_ambient_ = glGetUniformLocation(shader_.id, "u_ambient");
    loc_light_enabled_ = glGetUniformLocation(shader_.id, "u_light_enabled");
    loc_light_type_ = glGetUniformLocation(shader_.id, "u_light_type");
    loc_light_pos_ = glGetUniformLocation(shader_.id, "u_light_pos");
    loc_light_dir_ = glGetUniformLocation(shader_.id, "u_light_dir");
    loc_light_col_ = glGetUniformLocation(shader_.id, "u_light_col");
    loc_light_range_ = glGetUniformLocation(shader_.id, "u_light_range");
    loc_light_atten_ = glGetUniformLocation(shader_.id, "u_light_atten");
    loc_spot_cone_ = glGetUniformLocation(shader_.id, "u_spot_cone");
    loc_fog_mode_ = glGetUniformLocation(shader_.id, "u_fog_mode");
    loc_fog_color_ = glGetUniformLocation(shader_.id, "u_fog_color");
    loc_fog_start_ = glGetUniformLocation(shader_.id, "u_fog_start");
    loc_fog_end_ = glGetUniformLocation(shader_.id, "u_fog_end");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex3D), reinterpret_cast<const void*>(offsetof(Vertex3D, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex3D), reinterpret_cast<const void*>(offsetof(Vertex3D, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex3D), reinterpret_cast<const void*>(offsetof(Vertex3D, u)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex3D), reinterpret_cast<const void*>(offsetof(Vertex3D, r)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    vertices_.reserve(kMaxBatchVertices);
    create_builtin_font_texture();

    mat4_identity(world_matrix_);
    mat4_identity(view_matrix_);
    mat4_identity(proj_matrix_);

    ff_uniform_cache_ = {};
    dx9_.init(*device_);

    initialized_ = true;
    return Result<void>::success();
}

Result<void> Renderer2D::reload_gpu_resources(const GameData& game) {
    if (!device_) return Error{"Renderer2D::reload_gpu_resources called without device"};
    // Clear stale GPU IDs without calling glDelete* on a lost GL context.
    vao_ = 0;
    vbo_ = 0;
    shader_ = {};
    builtin_font_tex_ = {};
    screen_fbo_ = {};
    screen_fbo_tex_ = {};
    screen_fbo_w_ = 0;
    screen_fbo_h_ = 0;
    using_screen_fbo_ = false;
    active_surface_id_ = -1;
    sprites_.clear();
    backgrounds_.clear();
    fonts_.clear();
    surfaces_.clear();
    surface_tokens_.clear();
    particle_shapes_.fill({});
    vertices_.clear();
    ff_uniform_cache_ = {};
    initialized_ = false;

    auto res = init(*device_);
    if (!res) return res;
    upload_game_assets(game);
    return Result<void>::success();
}

void Renderer2D::shutdown() noexcept {
    if (!initialized_) return;

    dx9_.shutdown();
    models_.shutdown();

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
    for (auto tex : particle_shapes_) device_->destroy_texture(tex);
    particle_shapes_.fill({});

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
    surface_tokens_.clear();

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
    recorded_d3d_ = game.meta.detected_version == VersionMode::GM81 || game.meta.detected_version == VersionMode::GM82;
    for (usize i = 0; i < game.particle_shapes.size(); ++i) {
        auto& tex = particle_shapes_[i];
        device_->destroy_texture(tex);
        tex = {};
        const auto& shape = game.particle_shapes[i];
        if (shape.bgra.empty()) continue;
        TextureDesc desc{};
        desc.width = shape.width;
        desc.height = shape.height;
        if (auto res = device_->create_texture(desc, shape.bgra); res) tex = res.value();
    }
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

bool Renderer2D::ensure_screen_fbo(i32 width, i32 height) {
    const i32 w = std::max(1, width);
    const i32 h = std::max(1, height);
    if (screen_fbo_.valid() && screen_fbo_w_ == w && screen_fbo_h_ == h) {
        return true;
    }
    TextureDesc desc{};
    desc.width = w;
    desc.height = h;
    desc.format = TextureFormat::RGBA8;
    desc.min_filter = linear_filter_ ? TextureFilter::Linear : TextureFilter::Nearest;
    desc.mag_filter = desc.min_filter;
    auto tex_res = device_->create_texture(desc);
    if (!tex_res) return false;
    TextureHandle tex = tex_res.value();
    auto fbo_res = device_->create_framebuffer(std::span<const TextureHandle>(&tex, 1), true);
    if (!fbo_res) {
        device_->destroy_texture(tex);
        return false;
    }
    if (screen_fbo_.valid()) device_->destroy_framebuffer(screen_fbo_);
    if (screen_fbo_tex_.valid()) device_->destroy_texture(screen_fbo_tex_);
    screen_fbo_tex_ = tex;
    screen_fbo_ = fbo_res.value();
    screen_fbo_w_ = w;
    screen_fbo_h_ = h;
    return true;
}

FramebufferHandle Renderer2D::active_target_fbo() const noexcept {
    if (active_surface_id_ >= 0) {
        if (auto it = surfaces_.find(active_surface_id_); it != surfaces_.end()) {
            return it->second.fbo;
        }
    }
    if (using_screen_fbo_ && screen_fbo_.valid()) {
        return screen_fbo_;
    }
    return FramebufferHandle{};
}

void Renderer2D::reset_active_fbo_ext_state() {
    const FramebufferHandle fbo = active_target_fbo();
    if (fbo.valid()) {
        if (active_fbo_has_ext_mrt_) {
            for (u32 slot = 1; slot <= 3; ++slot) {
                device_->set_framebuffer_mrt_target(fbo, slot, TextureHandle{});
            }
        }
        if (active_fbo_has_custom_depth_) {
            device_->set_framebuffer_depth_from(fbo, fbo);
        }
    }
    active_fbo_has_ext_mrt_ = false;
    active_fbo_has_custom_depth_ = false;
}

void Renderer2D::bind_active_target_fbo() {
    device_->bind_framebuffer(active_target_fbo());
}

void Renderer2D::apply_d3d_depth_cull() {
    if (d3d_active_ && d3d_hidden_) {
        device_->set_depth_state(DepthState{true, d3d_zwrite_, d3d_zfunc_});
    } else {
        device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    }

    // D3D8 D3DCULL_CCW (mode 2) culls counter-clockwise triangles in Y-up NDC (keeping CW).
    // OpenGL default front-face is GL_CCW, so on the backbuffer GL_FRONT = CCW (CullMode::Front).
    // On an FBO we flip NDC Y (-1), reversing triangle winding, so culling CCW uses CullMode::Back.
    if (d3d_cull_mode_ == 0) {
        device_->set_cull_mode(CullMode::None);
    } else if (d3d_cull_mode_ == 2) { // D3DCULL_CCW (default d3d_set_culling(true))
        device_->set_cull_mode(rendering_to_fbo() ? CullMode::Back : CullMode::Front);
    } else { // D3DCULL_CW (mode 1)
        device_->set_cull_mode(rendering_to_fbo() ? CullMode::Front : CullMode::Back);
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
        using_screen_fbo_ = ensure_screen_fbo(screen_w_, screen_h_);
        bind_active_target_fbo();
        device_->set_scissor(false);
        device_->set_viewport(ViewportRect{0, 0, screen_w_, screen_h_});
        device_->clear(false, true, true, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0);
    }
    apply_d3d_depth_cull();
    device_->set_blend_state(blend_state_);
}

void Renderer2D::present_screen(u32 border_color_bgr) {
    flush();
    if (!screen_fbo_tex_.valid() || (!using_screen_fbo_ && active_surface_id_ < 0)) {
        device_->bind_framebuffer(FramebufferHandle{});
        return;
    }

    const i32 saved_surface = active_surface_id_;
    const bool saved_screen_fbo = using_screen_fbo_;
    const ViewportRect saved_viewport = active_viewport_;
    const bool saved_scissor = device_->scissor_enabled();
    const ViewportRect saved_scissor_rect = device_->scissor_rect();
    const bool saved_alpha_test = alpha_test_explicit_;
    const bool saved_d3d_active = d3d_active_;
    const bool saved_d3d_hidden = d3d_hidden_;
    const bool saved_lighting = d3d_lighting_;
    const bool saved_flat = d3d_flat_shading_;
    const i32 saved_fill_mode = device_->polygon_mode();
    const bool saved_tex_blending = tex_blending_;
    const i32 saved_texcoord_mode = texcoord_mode_;
    const D3dFogState saved_fog = fog_;
    const BlendState saved_blend = blend_state_;
    const f32 saved_draw_z = current_draw_z_;
    const i32 saved_dx9_vs = dx9_.active_vertex_shader();
    const i32 saved_dx9_ps = dx9_.active_pixel_shader();
    f32 saved_world[16], saved_view[16], saved_proj[16];
    std::memcpy(saved_world, world_matrix_, sizeof(saved_world));
    std::memcpy(saved_view, view_matrix_, sizeof(saved_view));
    std::memcpy(saved_proj, proj_matrix_, sizeof(saved_proj));

    dx9_.set_active_shaders_raw(0, 0);
    active_surface_id_ = -1;
    using_screen_fbo_ = false;
    device_->bind_framebuffer(FramebufferHandle{});
    device_->set_scissor(false);
    device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    device_->set_cull_mode(CullMode::None);
    device_->set_polygon_mode(2);
    device_->set_color_mask(true, true, true, true);
    device_->set_viewport(ViewportRect{0, 0, backbuffer_w_, backbuffer_h_});
    clear_color(border_color_bgr, 1.0f);

    alpha_test_explicit_ = false;
    d3d_active_ = false;
    d3d_hidden_ = false;
    d3d_lighting_ = false;
    d3d_flat_shading_ = false;
    tex_blending_ = true;
    texcoord_mode_ = 0; // The presentation quad always uses its own UVs.
    fog_.mode = 0;
    blend_state_.enabled = false;
    current_draw_z_ = 0.0f;
    mat4_identity(world_matrix_);
    set_projection_ortho(0.0f, 0.0f, static_cast<f32>(screen_w_), static_cast<f32>(screen_h_), 0.0f,
                         screen_viewport_.x, screen_viewport_.y, screen_viewport_.width, screen_viewport_.height);
    const ColorRGBA white{255, 255, 255, 255};
    const bool saved_filter = linear_filter_;
    if (sharp_scale_) {
        linear_filter_ = screen_viewport_.width % screen_fbo_w_ != 0 ||
                         screen_viewport_.height % screen_fbo_h_ != 0;
        if (linear_filter_) {
            const f32 scale_x = static_cast<f32>(screen_viewport_.width) / screen_fbo_w_;
            const f32 scale_y = static_cast<f32>(screen_viewport_.height) / screen_fbo_h_;
            const f32 factor = std::clamp(std::round(std::max(scale_x, scale_y)), 2.0f, 8.0f);
            device_->bind_shader(shader_);
            glUniform1f(loc_sharp_factor_, factor);
        }
    }
    push_quad(screen_fbo_tex_,
              0.0f, 0.0f, 0.0f, 0.0f, white,
              static_cast<f32>(screen_w_), 0.0f, 1.0f, 0.0f, white,
              static_cast<f32>(screen_w_), static_cast<f32>(screen_h_), 1.0f, 1.0f, white,
              0.0f, static_cast<f32>(screen_h_), 0.0f, 1.0f, white);
    flush();

    if (sharp_scale_ && linear_filter_) glUniform1f(loc_sharp_factor_, 0.0f);
    linear_filter_ = saved_filter;
    std::memcpy(world_matrix_, saved_world, sizeof(saved_world));
    std::memcpy(view_matrix_, saved_view, sizeof(saved_view));
    std::memcpy(proj_matrix_, saved_proj, sizeof(saved_proj));
    current_draw_z_ = saved_draw_z;
    blend_state_ = saved_blend;
    fog_ = saved_fog;
    tex_blending_ = saved_tex_blending;
    texcoord_mode_ = saved_texcoord_mode;
    d3d_flat_shading_ = saved_flat;
    d3d_lighting_ = saved_lighting;
    d3d_hidden_ = saved_d3d_hidden;
    d3d_active_ = saved_d3d_active;
    alpha_test_explicit_ = saved_alpha_test;
    active_surface_id_ = saved_surface;
    using_screen_fbo_ = saved_screen_fbo;
    active_viewport_ = saved_viewport;
    device_->set_viewport(saved_viewport);
    device_->set_scissor(saved_scissor, saved_scissor_rect);
    dx9_.set_active_shaders_raw(saved_dx9_vs, saved_dx9_ps);
    device_->set_color_mask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);
    device_->set_polygon_mode(saved_fill_mode);
    device_->set_blend_state(blend_state_);
    apply_d3d_depth_cull();
}

void Renderer2D::set_screen_region(i32 backbuffer_w, i32 backbuffer_h, i32 width, i32 height, const ViewportRect& viewport) {
    // Room creation code can draw and present before the next screen pass.
    if (using_screen_fbo_ && (screen_fbo_w_ != width || screen_fbo_h_ != height)) {
        flush();
        using_screen_fbo_ = ensure_screen_fbo(width, height);
        bind_active_target_fbo();
        if (active_surface_id_ < 0) {
            active_fbo_has_ext_mrt_ = false;
            active_fbo_has_custom_depth_ = false;
        }
    }
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
    // SDL swaps the default framebuffer. Subsequent GML draws use the previous target.
    bind_active_target_fbo();
}

bool Renderer2D::resize_backbuffer(i32 width, i32 height) {
    if (width <= 0 || height <= 0) return false;
    if (screen_fbo_.valid() && screen_fbo_w_ == width && screen_fbo_h_ == height &&
        screen_w_ == width && screen_h_ == height) {
        return true;
    }

    flush();
    if (!ensure_screen_fbo(width, height)) return false;
    screen_w_ = width;
    screen_h_ = height;
    using_screen_fbo_ = true;
    if (active_surface_id_ < 0) {
        active_fbo_has_ext_mrt_ = false;
        active_fbo_has_custom_depth_ = false;
        bind_active_target_fbo();
    }
    // GM resets the viewport and projection even when drawing to a user surface.
    apply_screen_projection();
    apply_d3d_depth_cull();
    return true;
}

void Renderer2D::upload_shader_uniforms(const f32 tint_rgba[4]) {
    f32 gl_proj[16];
    std::memcpy(gl_proj, proj_matrix_, sizeof(gl_proj));
    if (rendering_to_fbo()) {
        // Flip NDC Y so row 0 in FBO textures corresponds to screen top.
        gl_proj[1] = -gl_proj[1];
        gl_proj[5] = -gl_proj[5];
        gl_proj[9] = -gl_proj[9];
        gl_proj[13] = -gl_proj[13];
    }

    device_->bind_shader(shader_);

    auto& c = ff_uniform_cache_;
    const bool force = !c.valid;
    if (force) {
        glUniform1i(loc_tex_, 0);
    }

    if (force || std::memcmp(c.world, world_matrix_, sizeof(c.world)) != 0) {
        std::memcpy(c.world, world_matrix_, sizeof(c.world));
        glUniformMatrix4fv(loc_world_, 1, GL_FALSE, world_matrix_);
        f32 normal_mat[9];
        compute_normal_mat3(world_matrix_, normal_mat);
        glUniformMatrix3fv(loc_normal_mat_, 1, GL_FALSE, normal_mat);
    }
    if (force || std::memcmp(c.view, view_matrix_, sizeof(c.view)) != 0) {
        std::memcpy(c.view, view_matrix_, sizeof(c.view));
        glUniformMatrix4fv(loc_view_, 1, GL_FALSE, view_matrix_);
    }
    if (force || std::memcmp(c.proj, gl_proj, sizeof(c.proj)) != 0) {
        std::memcpy(c.proj, gl_proj, sizeof(c.proj));
        glUniformMatrix4fv(loc_proj_, 1, GL_FALSE, gl_proj);
    }
    if (force || c.pixel_center[0] != pixel_center_offset_[0] || c.pixel_center[1] != pixel_center_offset_[1]) {
        c.pixel_center[0] = pixel_center_offset_[0];
        c.pixel_center[1] = pixel_center_offset_[1];
        glUniform2fv(loc_pixel_center_offset_, 1, pixel_center_offset_.data());
    }
    if (force || std::memcmp(c.tint, tint_rgba, sizeof(c.tint)) != 0) {
        std::memcpy(c.tint, tint_rgba, sizeof(c.tint));
        glUniform4fv(loc_model_tint_, 1, tint_rgba);
    }

    const i32 tb = tex_blending_ ? 1 : 0;
    if (force || c.tex_blending != tb) {
        c.tex_blending = tb;
        glUniform1i(loc_tex_blending_, tb);
    }
    if (force || c.texcoord_mode != texcoord_mode_) {
        c.texcoord_mode = texcoord_mode_;
        glUniform1i(loc_texcoord_mode_, texcoord_mode_);
    }
    const i32 fs = d3d_flat_shading_ ? 1 : 0;
    if (force || c.flat_shading != fs) {
        c.flat_shading = fs;
        glUniform1i(loc_flat_shading_, fs);
    }

    // GM8 enables alpha test (GREATER 0) automatically when 3D hidden surface removal is on.
    const i32 effective_alpha_test = (alpha_test_explicit_ || (d3d_active_ && d3d_hidden_)) ? 1 : 0;
    const f32 effective_alpha_ref = alpha_test_explicit_ ? alpha_test_ref_ : 0.0f;
    const i32 effective_alpha_func = alpha_test_explicit_ ? alpha_test_func_ : 5; // D3DCMP_GREATER
    if (force || c.alpha_test != effective_alpha_test) {
        c.alpha_test = effective_alpha_test;
        glUniform1i(loc_alpha_test_, effective_alpha_test);
    }
    if (force || c.alpha_ref != effective_alpha_ref) {
        c.alpha_ref = effective_alpha_ref;
        glUniform1f(loc_alpha_ref_, effective_alpha_ref);
    }
    if (force || c.alpha_func != effective_alpha_func) {
        c.alpha_func = effective_alpha_func;
        glUniform1i(loc_alpha_func_, effective_alpha_func);
    }

    const i32 li = d3d_lighting_ ? 1 : 0;
    if (force || c.lighting != li) {
        c.lighting = li;
        glUniform1i(loc_lighting_, li);
    }
    if (d3d_lighting_) {
        if (force || !c.lights_uploaded || c.ambient_bgr != ambient_bgr_) {
            c.ambient_bgr = ambient_bgr_;
            const ColorRGBA amb = gm_bgr_to_rgba(ambient_bgr_);
            glUniform3f(loc_ambient_, amb.r / 255.0f, amb.g / 255.0f, amb.b / 255.0f);
        }
        if (force || !c.lights_uploaded || std::memcmp(c.lights.data(), lights_.data(), sizeof(c.lights)) != 0) {
            c.lights = lights_;
            c.lights_uploaded = true;
            i32 en[8]{};
            i32 ty[8]{};
            f32 pos[24]{};
            f32 dir[24]{};
            f32 col[24]{};
            f32 rng[8]{};
            f32 att[24]{};
            f32 cone[24]{};
            for (usize i = 0; i < 8; ++i) {
                const auto& l = lights_[i];
                en[i] = l.enabled ? 1 : 0;
                ty[i] = l.type;
                pos[i * 3 + 0] = l.pos[0]; pos[i * 3 + 1] = l.pos[1]; pos[i * 3 + 2] = l.pos[2];
                dir[i * 3 + 0] = l.dir[0]; dir[i * 3 + 1] = l.dir[1]; dir[i * 3 + 2] = l.dir[2];
                col[i * 3 + 0] = l.diffuse[0]; col[i * 3 + 1] = l.diffuse[1]; col[i * 3 + 2] = l.diffuse[2];
                rng[i] = l.range;
                att[i * 3 + 0] = l.atten[0]; att[i * 3 + 1] = l.atten[1]; att[i * 3 + 2] = l.atten[2];
                if (l.type == 2) {
                    cone[i * 3] = std::cos(l.theta * 0.5f);
                    cone[i * 3 + 1] = std::cos(l.phi * 0.5f);
                    cone[i * 3 + 2] = l.falloff;
                }
            }
            glUniform1iv(loc_light_enabled_, 8, en);
            glUniform1iv(loc_light_type_, 8, ty);
            glUniform3fv(loc_light_pos_, 8, pos);
            glUniform3fv(loc_light_dir_, 8, dir);
            glUniform3fv(loc_light_col_, 8, col);
            glUniform1fv(loc_light_range_, 8, rng);
            glUniform3fv(loc_light_atten_, 8, att);
            glUniform3fv(loc_spot_cone_, 8, cone);
        }
    }

    if (force || !c.fog_uploaded || c.fog.mode != fog_.mode ||
        (fog_.mode != 0 && (c.fog.color_bgr != fog_.color_bgr || c.fog.start != fog_.start || c.fog.end != fog_.end))) {
        c.fog = fog_;
        c.fog_uploaded = true;
        glUniform1i(loc_fog_mode_, fog_.mode);
        if (fog_.mode != 0) {
            const ColorRGBA fc = gm_bgr_to_rgba(fog_.color_bgr);
            glUniform3f(loc_fog_color_, fc.r / 255.0f, fc.g / 255.0f, fc.b / 255.0f);
            glUniform1f(loc_fog_start_, fog_.start);
            glUniform1f(loc_fog_end_, fog_.end);
        }
    }

    c.valid = true;
}

Dx9DrawContext Renderer2D::make_dx9_draw_context(const f32 tint_rgba[4]) const noexcept {
    Dx9DrawContext ctx{};
    ctx.world_matrix = world_matrix_;
    ctx.view_matrix = view_matrix_;
    ctx.proj_matrix = proj_matrix_;
    ctx.model_tint = tint_rgba;
    ctx.rendering_to_fbo = rendering_to_fbo();
    ctx.viewport = active_viewport_;
    ctx.pixel_center_offset = pixel_center_offset_;
    ctx.tex_blending = tex_blending_;
    ctx.texcoord_mode = texcoord_mode_;
    ctx.alpha_test_enabled = alpha_test_explicit_ || (d3d_active_ && d3d_hidden_);
    ctx.alpha_test_ref = alpha_test_explicit_ ? alpha_test_ref_ : 0.0f;
    ctx.alpha_test_func = alpha_test_explicit_ ? alpha_test_func_ : 5;
    ctx.global_linear_filter = linear_filter_;
    ctx.global_tex_repeat = tex_repeat_;
    ctx.flat_shading = d3d_flat_shading_;
    return ctx;
}

void Renderer2D::apply_stage0_ff_sampler(TextureHandle tex) {
    const auto& st0 = dx9_.ps_sampler(0);
    const bool is_present_tex = tex.valid() && tex == screen_fbo_tex_;
    const bool lin = (!is_present_tex && st0.has_explicit_filter) ? st0.linear_filter : linear_filter_;
    if (!is_present_tex && st0.has_explicit_wrap) {
        device_->set_texture_sampler_ext(
            0, lin ? TextureFilter::Linear : TextureFilter::Nearest,
            st0.wrap_u, st0.wrap_v, st0.border_bgr);
    } else {
        const TextureFilter f = lin ? TextureFilter::Linear : TextureFilter::Nearest;
        const TextureWrap w = (!is_present_tex && tex_repeat_) ? TextureWrap::Repeat : TextureWrap::ClampToEdge;
        device_->set_texture_sampler(0, f, w, w);
    }
}

void Renderer2D::flush() {
    if (vertices_.empty()) {
        return;
    }

    static constexpr f32 kWhiteTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    device_->set_blend_state(blend_state_);

    bool used_custom_shader = false;
    if (dx9_.has_active_shader()) {
        used_custom_shader = dx9_.bind_and_upload(make_dx9_draw_context(kWhiteTint), batch_tex_);
    }
    if (!used_custom_shader) {
        upload_shader_uniforms(kWhiteTint);
        apply_stage0_ff_sampler(batch_tex_);
        device_->bind_texture(0, batch_tex_);
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const usize count = vertices_.size();
    const auto bytes = static_cast<GLsizeiptr>(count * sizeof(Vertex3D));
    // Give each batch a fresh data store so earlier draws can finish independently.
    glBufferData(GL_ARRAY_BUFFER, bytes, vertices_.data(), GL_STREAM_DRAW);
    device_->draw_primitives(batch_topo_, static_cast<i32>(count), d3d_flat_shading_ && !used_custom_shader);
    glBindVertexArray(0);

    vertices_.clear();
}

void Renderer2D::set_view_projection(f32 x, f32 y, f32 width, f32 height, f32 angle, const ViewportRect& port) {
    if (d3d_active_ && d3d_perspective_) {
        set_projection_perspective(x, y, width, height, angle, port.x, port.y, port.width, port.height);
    } else {
        set_projection_ortho(x, y, width, height, angle, port.x, port.y, port.width, port.height);
    }
}

void Renderer2D::set_projection_ortho(
    f32 view_x, f32 view_y, f32 view_w, f32 view_h, f32 angle_deg,
    i32 port_x, i32 port_y, i32 port_w, i32 port_h) {
    flush();

    const bool to_fbo = rendering_to_fbo();
    const i32 target_h = (active_surface_id_ >= 0) ? surface_height(active_surface_id_)
                         : (using_screen_fbo_ ? screen_fbo_h_ : backbuffer_h_);

    const i32 gl_y = to_fbo ? port_y : (target_h - (port_y + port_h));
    active_viewport_ = ViewportRect{port_x, port_y, std::max(1, port_w), std::max(1, port_h)};
    device_->set_viewport(ViewportRect{port_x, gl_y, std::max(1, port_w), std::max(1, port_h)});

    d3d_set_projection_ortho(view_x, view_y, view_w, view_h, angle_deg);
}

void Renderer2D::set_projection_perspective(
    f32 view_x, f32 view_y, f32 view_w, f32 view_h, f32 angle_deg,
    i32 port_x, i32 port_y, i32 port_w, i32 port_h) {
    flush();

    const bool to_fbo = rendering_to_fbo();
    const i32 target_h = (active_surface_id_ >= 0) ? surface_height(active_surface_id_)
                         : (using_screen_fbo_ ? screen_fbo_h_ : backbuffer_h_);

    const i32 gl_y = to_fbo ? port_y : (target_h - (port_y + port_h));
    active_viewport_ = ViewportRect{port_x, port_y, std::max(1, port_w), std::max(1, port_h)};
    device_->set_viewport(ViewportRect{port_x, gl_y, std::max(1, port_w), std::max(1, port_h)});

    d3d_set_projection_perspective(view_x, view_y, view_w, view_h, angle_deg);
}

void Renderer2D::clear_color(u32 gm_bgr, f32 alpha) {
    flush();
    const ColorRGBA c = gm_bgr_to_rgba(gm_bgr);
    device_->clear(true, true, false,
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
    blend_state_.op_rgb = BlendOp::Add;
    blend_state_.op_alpha = BlendOp::Add;
    switch (bm_mode) {
        case 1: // bm_add
            blend_state_.src_rgb = BlendFactor::SrcAlpha;
            blend_state_.dst_rgb = BlendFactor::One;
            if (!separate_alpha_blend_) {
                blend_state_.src_alpha = BlendFactor::SrcAlpha;
                blend_state_.dst_alpha = BlendFactor::One;
            }
            break;
        case 2: // bm_max
            blend_state_.src_rgb = BlendFactor::SrcAlpha;
            blend_state_.dst_rgb = BlendFactor::InvSrcColor;
            if (!separate_alpha_blend_) {
                blend_state_.src_alpha = BlendFactor::SrcAlpha;
                blend_state_.dst_alpha = BlendFactor::InvSrcAlpha;
            }
            break;
        case 3: // bm_subtract
            blend_state_.src_rgb = BlendFactor::Zero;
            blend_state_.dst_rgb = BlendFactor::InvSrcColor;
            if (!separate_alpha_blend_) {
                blend_state_.src_alpha = BlendFactor::Zero;
                blend_state_.dst_alpha = BlendFactor::InvSrcAlpha;
            }
            break;
        default: // bm_normal (0)
            blend_state_.src_rgb = BlendFactor::SrcAlpha;
            blend_state_.dst_rgb = BlendFactor::InvSrcAlpha;
            if (!separate_alpha_blend_) {
                blend_state_.src_alpha = BlendFactor::SrcAlpha;
                blend_state_.dst_alpha = BlendFactor::InvSrcAlpha;
            }
            break;
    }
}

void Renderer2D::set_blend_mode_ext(i32 src_bm, i32 dst_bm) {
    flush();
    blend_state_.op_rgb = BlendOp::Add;
    blend_state_.op_alpha = BlendOp::Add;
    blend_state_.src_rgb = gm_bm_to_factor(src_bm);
    blend_state_.dst_rgb = gm_bm_to_factor(dst_bm);
    if (!separate_alpha_blend_) {
        blend_state_.src_alpha = blend_state_.src_rgb;
        blend_state_.dst_alpha = blend_state_.dst_rgb;
    }
}

void Renderer2D::set_alpha_blend_enable(bool enabled) {
    flush();
    blend_state_.enabled = enabled;
}

void Renderer2D::set_separate_alpha_blend(bool enabled, i32 src_bm, i32 dst_bm) {
    flush();
    separate_alpha_blend_ = enabled;
    if (enabled) {
        blend_state_.src_alpha = gm_bm_to_factor(src_bm);
        blend_state_.dst_alpha = gm_bm_to_factor(dst_bm);
    } else {
        blend_state_.src_alpha = blend_state_.src_rgb;
        blend_state_.dst_alpha = blend_state_.dst_rgb;
    }
}

void Renderer2D::set_interpolation(bool linear) {
    if (linear_filter_ != linear || dx9_.ps_sampler(0).has_explicit_filter) {
        flush();
        linear_filter_ = linear;
        dx9_.clear_stage0_filter_override();
    }
}

void Renderer2D::set_texcoord_mode(i32 mode) {
    // D3DTSS_TEXCOORDINDEX stores the coordinate generation mode in the high word.
    const i32 generation = static_cast<u32>(mode) >> 16;
    if (generation > 4 || generation == texcoord_mode_) return;
    flush();
    texcoord_mode_ = generation;
}

void Renderer2D::set_texture_repeat(bool repeat) {
    if (tex_repeat_ != repeat || dx9_.ps_sampler(0).has_explicit_wrap) {
        flush();
        tex_repeat_ = repeat;
        dx9_.clear_stage0_wrap_override();
    }
}

void Renderer2D::set_texture_blending(bool enabled) {
    if (tex_blending_ != enabled) {
        flush();
        tex_blending_ = enabled;
    }
}

void Renderer2D::set_alpha_test(bool enabled, u8 ref_val) {
    flush();
    alpha_test_explicit_ = enabled;
    alpha_test_ref_ = static_cast<f32>(ref_val) / 255.0f;
    alpha_test_func_ = 5; // D3DCMP_GREATER
}

void Renderer2D::set_alpha_test_ext(bool enabled, u8 ref_val, i32 d3d_cmp_func) {
    flush();
    alpha_test_explicit_ = enabled;
    alpha_test_ref_ = static_cast<f32>(ref_val) / 255.0f;
    alpha_test_func_ = std::clamp(d3d_cmp_func, 1, 8);
}

void Renderer2D::set_color_write_enable(bool r, bool g, bool b, bool a) {
    flush();
    color_mask_ = {r, g, b, a};
    device_->set_color_mask(r, g, b, a);
}

void Renderer2D::set_cull_mode_ext(i32 mode) {
    flush();
    d3d_cull_mode_ = std::clamp(mode, 0, 2);
    apply_d3d_depth_cull();
}

void Renderer2D::set_zcomp_mode(i32 d3d_cmp_func) {
    flush();
    d3d_zfunc_ = d3d_cmp_to_compare_func(d3d_cmp_func);
    apply_d3d_depth_cull();
}

void Renderer2D::set_viewport_ext(i32 x, i32 y, i32 w, i32 h) {
    flush();
    const bool to_fbo = rendering_to_fbo();
    const i32 target_h = (active_surface_id_ >= 0) ? surface_height(active_surface_id_)
                         : (using_screen_fbo_ ? screen_fbo_h_ : backbuffer_h_);
    const i32 pw = std::max(1, w);
    const i32 ph = std::max(1, h);
    const i32 gl_y = to_fbo ? y : (target_h - (y + ph));
    active_viewport_ = ViewportRect{x, y, pw, ph};
    device_->set_viewport(ViewportRect{x, gl_y, pw, ph});
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
    // GM's textured 2D quads set ADDRESSU/V to CLAMP on the device without changing the GML repeat flag.
    if (tex.valid() && tex != screen_fbo_tex_) {
        const auto& sampler = dx9_.ps_sampler(0);
        if (sampler.has_explicit_wrap ? (sampler.wrap_u != 3 || sampler.wrap_v != 3) : tex_repeat_) {
            flush();
            dx9_.set_ps_sampler_wrap(0, 3, 3, sampler.border_bgr);
        }
    }
    ensure_batch(PrimitiveTopology::Triangles, tex);
    vertices_.push_back(Vertex3D{x0, y0, current_draw_z_, 0.0f, 0.0f, 0.0f, u0, v0, c0.r, c0.g, c0.b, c0.a});
    vertices_.push_back(Vertex3D{x1, y1, current_draw_z_, 0.0f, 0.0f, 0.0f, u1, v1, c1.r, c1.g, c1.b, c1.a});
    vertices_.push_back(Vertex3D{x2, y2, current_draw_z_, 0.0f, 0.0f, 0.0f, u2, v2, c2.r, c2.g, c2.b, c2.a});
    vertices_.push_back(Vertex3D{x0, y0, current_draw_z_, 0.0f, 0.0f, 0.0f, u0, v0, c0.r, c0.g, c0.b, c0.a});
    vertices_.push_back(Vertex3D{x2, y2, current_draw_z_, 0.0f, 0.0f, 0.0f, u2, v2, c2.r, c2.g, c2.b, c2.a});
    vertices_.push_back(Vertex3D{x3, y3, current_draw_z_, 0.0f, 0.0f, 0.0f, u3, v3, c3.r, c3.g, c3.b, c3.a});
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

    // Reuse lowest free 0-based surface ID (mirrors GM8 surface array & surface_backup_restore).
    i32 id = 0;
    while (surfaces_.contains(id)) {
        ++id;
    }
    surfaces_[id] = SurfaceObject{width, height, tex_res.value(), fbo_res.value()};
    surface_tokens_[id] = next_surface_token_++;
    if (next_surface_token_ == 0) next_surface_token_ = 1;

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
    surface_tokens_.erase(id);
}

bool Renderer2D::surface_exists(i32 id) const noexcept {
    return surfaces_.contains(id);
}

i32 Renderer2D::surface_count() const noexcept {
    i32 max_id = -1;
    for (const auto& [id, _] : surfaces_) {
        if (id > max_id) max_id = id;
    }
    return max_id + 1;
}

u32 Renderer2D::surface_unique_token(i32 id) const noexcept {
    const auto it = surface_tokens_.find(id);
    return it != surface_tokens_.end() ? it->second : 0u;
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
    reset_active_fbo_ext_state();
    active_surface_id_ = id;
    device_->bind_framebuffer(it->second.fbo);
    device_->set_scissor(false);
    set_view_projection(0.0f, 0.0f, static_cast<f32>(it->second.width), static_cast<f32>(it->second.height),
                        0.0f, {0, 0, it->second.width, it->second.height});
    apply_d3d_depth_cull();
    return true;
}

bool Renderer2D::surface_set_target_ext(i32 target, i32 surf_id) {
    flush();
    const u32 slot = static_cast<u32>(std::clamp(target, 1, 3));
    const FramebufferHandle fbo = active_target_fbo();
    if (!fbo.valid()) {
        return false;
    }
    if (surf_id < 0) {
        device_->set_framebuffer_mrt_target(fbo, slot, TextureHandle{});
        return true;
    }
    const auto it = surfaces_.find(surf_id);
    if (it == surfaces_.end()) {
        return false;
    }
    device_->set_framebuffer_mrt_target(fbo, slot, it->second.texture);
    active_fbo_has_ext_mrt_ = true;
    return true;
}

bool Renderer2D::surface_set_depth(i32 surf_id) {
    flush();
    const FramebufferHandle dst_fbo = active_target_fbo();
    if (!dst_fbo.valid()) {
        return false;
    }
    if (surf_id < 0) {
        const bool ok = device_->set_framebuffer_depth_from(dst_fbo, dst_fbo);
        active_fbo_has_custom_depth_ = false;
        return ok;
    }
    const auto it = surfaces_.find(surf_id);
    if (it == surfaces_.end()) {
        return false;
    }
    const bool ok = device_->set_framebuffer_depth_from(dst_fbo, it->second.fbo);
    if (ok) {
        active_fbo_has_custom_depth_ = (it->second.fbo != dst_fbo);
    }
    return ok;
}

bool Renderer2D::surface_reset_target() {
    if (active_surface_id_ < 0) return false;
    flush();
    reset_active_fbo_ext_state();
    active_surface_id_ = -1;
    if (screen_w_ > 0 && screen_h_ > 0) {
        using_screen_fbo_ = ensure_screen_fbo(screen_w_, screen_h_);
    }
    bind_active_target_fbo();
    apply_screen_projection();
    apply_d3d_depth_cull();
    return true;
}

void Renderer2D::surface_fill_alpha_opaque(i32 id) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end() || !device_) return;
    flush();
    const auto saved = device_->save_render_state();
    device_->bind_framebuffer(it->second.fbo);
    device_->set_scissor(false);
    device_->set_viewport(ViewportRect{0, 0, it->second.width, it->second.height});
    device_->set_color_mask(false, false, false, true);
    device_->clear(true, false, false, 0.0f, 0.0f, 0.0f, 1.0f);
    device_->restore_render_state(saved);
}

void Renderer2D::draw_builtin_transition(
    i32 kind, i32 surf_old, i32 surf_new,
    i32 width, i32 height, i32 steps, f64 fraction) {
    if (!device_ || width <= 0 || height <= 0) return;
    flush();

    const ViewportRect saved_viewport = active_viewport_;
    const bool saved_scissor = device_->scissor_enabled();
    const ViewportRect saved_scissor_rect = device_->scissor_rect();
    const bool saved_alpha_test = alpha_test_explicit_;
    const bool saved_d3d_active = d3d_active_;
    const bool saved_d3d_hidden = d3d_hidden_;
    const bool saved_lighting = d3d_lighting_;
    const bool saved_flat = d3d_flat_shading_;
    const i32 saved_fill_mode = device_->polygon_mode();
    const bool saved_tex_blending = tex_blending_;
    const i32 saved_texcoord_mode = texcoord_mode_;
    const D3dFogState saved_fog = fog_;
    const BlendState saved_blend = blend_state_;
    const bool saved_sep_alpha = separate_alpha_blend_;
    const f32 saved_draw_z = current_draw_z_;
    const i32 saved_dx9_vs = dx9_.active_vertex_shader();
    const i32 saved_dx9_ps = dx9_.active_pixel_shader();
    f32 saved_world[16], saved_view[16], saved_proj[16];
    std::memcpy(saved_world, world_matrix_, sizeof(saved_world));
    std::memcpy(saved_view, view_matrix_, sizeof(saved_view));
    std::memcpy(saved_proj, proj_matrix_, sizeof(saved_proj));

    dx9_.set_active_shaders_raw(0, 0);
    device_->set_scissor(false);
    device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    device_->set_cull_mode(CullMode::None);
    device_->set_polygon_mode(2);
    device_->set_color_mask(true, true, true, true);

    alpha_test_explicit_ = false;
    d3d_active_ = false;
    d3d_hidden_ = false;
    d3d_lighting_ = false;
    d3d_flat_shading_ = false;
    tex_blending_ = true;
    texcoord_mode_ = 0;
    fog_.mode = 0;
    current_draw_z_ = 0.0f;
    separate_alpha_blend_ = false;
    blend_state_ = BlendState{
        true,
        BlendFactor::SrcAlpha,
        BlendFactor::InvSrcAlpha,
        BlendFactor::SrcAlpha,
        BlendFactor::InvSrcAlpha,
        BlendOp::Add,
        BlendOp::Add,
    };
    device_->set_blend_state(blend_state_);
    mat4_identity(world_matrix_);

    const ViewportRect port = using_screen_fbo_ ? ViewportRect{0, 0, screen_w_, screen_h_} : screen_viewport_;
    set_projection_ortho(0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height), 0.0f,
                         port.x, port.y, port.width, port.height);

    const f64 t = std::clamp(fraction, 0.0, 1.0);
    const i32 clamped_steps = std::max(1, steps);

    auto draw_create_edge = [&](i32 dx, i32 dy, f64 frac) {
        draw_surface_ext(surf_old, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        i32 left = 0;
        i32 rw = width;
        if (dx == 1) {
            left = 0;
            rw = static_cast<i32>(delphi_round(static_cast<f64>(width) * frac));
        } else if (dx == -1) {
            rw = static_cast<i32>(delphi_round(static_cast<f64>(width) * frac));
            left = width - rw;
        }
        i32 top = 0;
        i32 rh = height;
        if (dy == 1) {
            top = 0;
            rh = static_cast<i32>(delphi_round(static_cast<f64>(height) * frac));
        } else if (dy == -1) {
            rh = static_cast<i32>(delphi_round(static_cast<f64>(height) * frac));
            top = height - rh;
        }
        if (rw > 0 && rh > 0) {
            draw_surface_part_ext(surf_new,
                                  static_cast<f32>(left), static_cast<f32>(top),
                                  static_cast<f32>(rw), static_cast<f32>(rh),
                                  static_cast<f32>(left), static_cast<f32>(top),
                                  1.0f, 1.0f, 0xFFFFFFu, 1.0f);
        }
    };

    auto draw_shift_edge = [&](i32 dx, i32 dy, f64 frac) {
        draw_surface_ext(surf_old, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        const f32 x = static_cast<f32>(dx * (static_cast<i32>(delphi_round(static_cast<f64>(width) * frac)) - width));
        const f32 y = static_cast<f32>(dy * (static_cast<i32>(delphi_round(static_cast<f64>(height) * frac)) - height));
        draw_surface_ext(surf_new, x, y, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
    };

    auto draw_push_edge = [&](i32 dx, i32 dy, f64 frac) {
        const i32 ox = static_cast<i32>(delphi_round(static_cast<f64>(width) * frac));
        const i32 oy = static_cast<i32>(delphi_round(static_cast<f64>(height) * frac));
        draw_surface_ext(surf_old, static_cast<f32>(dx * ox), static_cast<f32>(dy * oy),
                         1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        draw_surface_ext(surf_new, static_cast<f32>(dx * (ox - width)), static_cast<f32>(dy * (oy - height)),
                         1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
    };

    auto draw_interlaced = [&](i32 dx, i32 dy) {
        const i32 cur_step = static_cast<i32>(delphi_round(static_cast<f64>(clamped_steps) * t));
        if (cur_step >= clamped_steps) {
            draw_surface_ext(surf_new, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
            return;
        }
        const i32 half = clamped_steps / 2;
        if (cur_step <= half) {
            draw_surface_ext(surf_old, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        } else {
            draw_create_edge(dx, dy, static_cast<f64>(cur_step - half) / static_cast<f64>(std::max(1, half)));
        }
        for (i32 k = 0; k <= half; ++k) {
            if ((dx > 0 && k <= cur_step) || (dx < 0 && k >= half - cur_step)) {
                const f32 sx = static_cast<f32>(delphi_round(static_cast<f64>(width) / static_cast<f64>(clamped_steps) * static_cast<f64>(2 * k)));
                const f32 sw = static_cast<f32>(delphi_round(static_cast<f64>(width) / static_cast<f64>(clamped_steps)));
                if (sw > 0.0f) {
                    draw_surface_part_ext(surf_new, sx, 0.0f, sw, static_cast<f32>(height),
                                          sx, 0.0f, 1.0f, 1.0f, 0xFFFFFFu, 1.0f);
                }
            }
            if ((dy > 0 && k <= cur_step) || (dy < 0 && k >= half - cur_step)) {
                const f32 sy = static_cast<f32>(delphi_round(static_cast<f64>(height) / static_cast<f64>(clamped_steps) * static_cast<f64>(2 * k)));
                const f32 sh = static_cast<f32>(delphi_round(static_cast<f64>(height) / static_cast<f64>(clamped_steps)));
                if (sh > 0.0f) {
                    draw_surface_part_ext(surf_new, 0.0f, sy, static_cast<f32>(width), sh,
                                          0.0f, sy, 1.0f, 1.0f, 0xFFFFFFu, 1.0f);
                }
            }
        }
    };

    auto draw_rotate = [&](bool clockwise) {
        draw_surface_ext(surf_old, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        if (t <= 0.0) return;
        f64 angle_rad = std::sqrt(t) * (2.0 * kPi);
        if (clockwise) angle_rad = -angle_rad;
        const f64 c = std::cos(angle_rad);
        const f64 sn = std::sin(angle_rad);
        const f64 hw = static_cast<f64>(width) * 0.5;
        const f64 hh = static_cast<f64>(height) * 0.5;
        const f32 px = static_cast<f32>(hw - sn * t * hh - c * t * hw);
        const f32 py = static_cast<f32>(hh - c * t * hh + sn * t * hw);
        const f32 rot_deg = static_cast<f32>(angle_rad * kRadToDeg);
        draw_surface_ext(surf_new, px, py, static_cast<f32>(t), static_cast<f32>(t),
                         rot_deg, 0xFFFFFFu, 1.0f);
    };

    clear_color(0x000000u, 1.0f);

    // Exact GM 8.0 / 8.1 / 8.2 transition_kind dispatch (gm80.exe.c:sub_51E708 / gm81.exe.c:sub_5F8FD0).
    switch (kind) {
        case 1:  draw_create_edge(1, 0, t); break;   // Create from left
        case 2:  draw_create_edge(-1, 0, t); break;  // Create from right
        case 3:  draw_create_edge(0, 1, t); break;   // Create from top
        case 4:  draw_create_edge(0, -1, t); break;  // Create from bottom
        case 5: {                                    // Create from center
            draw_surface_ext(surf_old, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
            const i32 left = static_cast<i32>(delphi_round((static_cast<f64>(width) - static_cast<f64>(width) * t) * 0.5));
            const i32 rw = static_cast<i32>(delphi_round(static_cast<f64>(width) * t));
            const i32 top = static_cast<i32>(delphi_round((static_cast<f64>(height) - static_cast<f64>(height) * t) * 0.5));
            const i32 rh = static_cast<i32>(delphi_round(static_cast<f64>(height) * t));
            if (rw > 0 && rh > 0) {
                draw_surface_part_ext(surf_new,
                                      static_cast<f32>(left), static_cast<f32>(top),
                                      static_cast<f32>(rw), static_cast<f32>(rh),
                                      static_cast<f32>(left), static_cast<f32>(top),
                                      1.0f, 1.0f, 0xFFFFFFu, 1.0f);
            }
            break;
        }
        case 6:  draw_shift_edge(1, 0, t); break;    // Shift from left
        case 7:  draw_shift_edge(-1, 0, t); break;   // Shift from right
        case 8:  draw_shift_edge(0, 1, t); break;    // Shift from top
        case 9:  draw_shift_edge(0, -1, t); break;   // Shift from bottom
        case 10: draw_interlaced(1, 0); break;       // Interlaced from left
        case 11: draw_interlaced(-1, 0); break;      // Interlaced from right
        case 12: draw_interlaced(0, 1); break;       // Interlaced from top
        case 13: draw_interlaced(0, -1); break;      // Interlaced from bottom
        case 14: draw_push_edge(1, 0, t); break;     // Push from left
        case 15: draw_push_edge(-1, 0, t); break;    // Push from right
        case 16: draw_push_edge(0, 1, t); break;     // Push from top
        case 17: draw_push_edge(0, -1, t); break;    // Push from bottom
        case 18: draw_rotate(false); break;          // Rotate from left
        case 19: draw_rotate(true); break;           // Rotate from right
        case 20:                                     // Blend
            draw_surface_ext(surf_old, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
            draw_surface_ext(surf_new, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, static_cast<f32>(t));
            break;
        case 21:                                     // Fade out and in
            if (t > 0.5) {
                draw_surface_ext(surf_new, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu,
                                 static_cast<f32>(std::clamp(2.0 * t - 1.0, 0.0, 1.0)));
            } else {
                draw_surface_ext(surf_old, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu,
                                 static_cast<f32>(std::clamp(1.0 - 2.0 * t, 0.0, 1.0)));
            }
            break;
        default:
            draw_surface_ext(surf_new, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
            break;
    }

    flush();

    std::memcpy(world_matrix_, saved_world, sizeof(saved_world));
    std::memcpy(view_matrix_, saved_view, sizeof(saved_view));
    std::memcpy(proj_matrix_, saved_proj, sizeof(saved_proj));
    current_draw_z_ = saved_draw_z;
    separate_alpha_blend_ = saved_sep_alpha;
    blend_state_ = saved_blend;
    fog_ = saved_fog;
    tex_blending_ = saved_tex_blending;
    texcoord_mode_ = saved_texcoord_mode;
    d3d_flat_shading_ = saved_flat;
    d3d_lighting_ = saved_lighting;
    d3d_hidden_ = saved_d3d_hidden;
    d3d_active_ = saved_d3d_active;
    alpha_test_explicit_ = saved_alpha_test;
    active_viewport_ = saved_viewport;
    device_->set_viewport(saved_viewport);
    device_->set_scissor(saved_scissor, saved_scissor_rect);
    dx9_.set_active_shaders_raw(saved_dx9_vs, saved_dx9_ps);
    device_->set_color_mask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);
    device_->set_polygon_mode(saved_fill_mode);
    device_->set_blend_state(blend_state_);
    apply_d3d_depth_cull();
}

void Renderer2D::apply_screen_projection() {
    // GM8 restores the whole logical screen even if a view was active before the surface.
    const ViewportRect port = using_screen_fbo_ ? ViewportRect{0, 0, screen_w_, screen_h_} : screen_viewport_;
    set_view_projection(0.0f, 0.0f, static_cast<f32>(screen_w_), static_cast<f32>(screen_h_), 0.0f, port);
    if (using_screen_fbo_) {
        device_->set_scissor(false);
    } else {
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

bool Renderer2D::surface_write_pixels_bgra(i32 id, i32 w, i32 h, std::span<const u8> bgra) {
    const auto it = surfaces_.find(id);
    if (it == surfaces_.end() || w <= 0 || h <= 0) return false;
    const i32 cw = std::min(w, it->second.width);
    const i32 ch = std::min(h, it->second.height);
    if (bgra.size() < static_cast<usize>(cw) * static_cast<usize>(ch) * 4u) return false;
    flush();
    device_->update_texture(it->second.texture, 0, 0, cw, ch, TextureFormat::BGRA8,
                            bgra.subspan(0, static_cast<usize>(cw) * static_cast<usize>(ch) * 4u));
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

void Renderer2D::draw_particle_shape(i32 shape, f32 x, f32 y, f32 xscale, f32 yscale, f32 rot_deg,
                                     u32 color_bgr, f32 alpha) {
    if (shape < 0 || static_cast<usize>(shape) >= particle_shapes_.size()) return;
    const auto tex = particle_shapes_[static_cast<usize>(shape)];
    if (!tex.valid()) {
        draw_circle_color(x, y, std::max(1.0f, xscale * 4.0f), color_bgr, color_bgr, false, alpha);
        return;
    }
    const f32 hw = 32.0f * xscale, hh = 32.0f * yscale;
    draw_texture_ext(tex, x, y, -hw, -hh, hw, hh, rot_deg, color_bgr, alpha);
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

    draw_texture_ext(tex, x, y, lx0, ly0, lx1, ly1, rot_deg, color_bgr, alpha);
}

void Renderer2D::draw_texture_ext(TextureHandle tex, f32 x, f32 y, f32 lx0, f32 ly0, f32 lx1, f32 ly1,
                                  f32 rot_deg, u32 color_bgr, f32 alpha) {
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
        // GM offsets sprite vertices by half a pixel before rotating them.
        ox = x + 0.5f + (lx - 0.5f) * c + (ly - 0.5f) * s;
        oy = y + 0.5f - (lx - 0.5f) * s + (ly - 0.5f) * c;
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

u32 Renderer2D::font_codepage(const GameData& game) const noexcept {
    const u32 ansi = system_ansi_codepage();
    if (draw_font_ < 0 || static_cast<usize>(draw_font_) >= game.fonts.size() || using_sprite_font()) return ansi;
    const auto& font = game.fonts[static_cast<usize>(draw_font_)];
    return font.exists ? gdi_charset_to_codepage(font.charset, ansi) : ansi;
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

    // GM8.1+ converts each Unicode line to the selected font's byte-indexed glyphs.
    if (game.meta.encoding == EncodingMode::Utf8) {
        const u32 codepage = font_codepage(game);
        for (auto& line : hard_lines) {
            const auto nul = line.find('\0');
            if (nul != std::string::npos) line.resize(nul);
            if (std::ranges::any_of(line, [](char ch) { return static_cast<u8>(ch) >= 0x80; })) {
                line = utf8_to_codepage(line, codepage);
            }
        }
    }

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
    vertices_.push_back(Vertex3D{x1 + 0.5f, y1 + 0.5f, current_draw_z_, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, col1.r, col1.g, col1.b, col1.a});
    vertices_.push_back(Vertex3D{x2 + 0.5f, y2 + 0.5f, current_draw_z_, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, col2.r, col2.g, col2.b, col2.a});
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
    vertices_.push_back(Vertex3D{x1, y1, current_draw_z_, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, col1.r, col1.g, col1.b, col1.a});
    vertices_.push_back(Vertex3D{x2, y2, current_draw_z_, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, col2.r, col2.g, col2.b, col2.a});
    vertices_.push_back(Vertex3D{x3, y3, current_draw_z_, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, col3.r, col3.g, col3.b, col3.a});
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
        vertices_.push_back(Vertex3D{cx, cy, current_draw_z_, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, col_center.r, col_center.g, col_center.b, col_center.a});
        vertices_.push_back(Vertex3D{cx + std::cos(a0) * rx, cy + std::sin(a0) * ry, current_draw_z_, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, col_outer.r, col_outer.g, col_outer.b, col_outer.a});
        vertices_.push_back(Vertex3D{cx + std::cos(a1) * rx, cy + std::sin(a1) * ry, current_draw_z_, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, col_outer.r, col_outer.g, col_outer.b, col_outer.a});
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
    primitive_vertex_normal_3d(x, y, z, 0.0f, 0.0f, 0.0f, u, v, color_bgr, alpha);
}

void Renderer2D::primitive_vertex_normal_3d(f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz, f32 u, f32 v, u32 color_bgr, f32 alpha) {
    if (!in_primitive_) return;
    if (prim_verts_.size() >= 32001) return; // GM8 cap
    const ColorRGBA c = make_vertex_color(color_bgr, alpha);
    prim_verts_.push_back(Vertex3D{x, y, z, nx, ny, nz, u, v, c.r, c.g, c.b, c.a});
}

// ---- Fixed-Function D3D Mode, Lighting, Fog, Projection, Transforms & 3D Shapes ----

void Renderer2D::d3d_start() {
    if (d3d_active_) return;
    flush();
    d3d_active_ = true;
    d3d_perspective_ = true;
    d3d_hidden_ = true;
    apply_d3d_depth_cull();
}

void Renderer2D::d3d_end() {
    if (!d3d_active_) return;
    flush();
    d3d_active_ = false;
    d3d_perspective_ = false;
    d3d_hidden_ = false;
    current_draw_z_ = 0.0f;
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

void Renderer2D::d3d_set_depth(f32 depth) noexcept {
    current_draw_z_ = d3d_active_ ? std::clamp(depth, -16000.0f, 16000.0f) : 0.0f;
}

void Renderer2D::d3d_set_zwriteenable(bool enable) {
    if (!d3d_active_) return; // GM8 00563B44 no-op when !d3d_active
    flush();
    d3d_zwrite_ = enable;
    apply_d3d_depth_cull();
}

void Renderer2D::d3d_set_culling(bool enable) {
    flush();
    d3d_cull_mode_ = enable ? 2 : 0; // 2 = D3DCULL_CCW, 0 = D3DCULL_NONE
    apply_d3d_depth_cull();
}

void Renderer2D::d3d_set_lighting(bool enable) {
    if (d3d_lighting_ != enable) {
        flush();
        d3d_lighting_ = enable;
    }
}

void Renderer2D::d3d_set_shading(bool smooth) {
    const bool flat = !smooth;
    if (d3d_flat_shading_ != flat) {
        flush();
        d3d_flat_shading_ = flat;
    }
}

void Renderer2D::d3d_set_fog(bool enable, u32 color_bgr, f32 start, f32 end) {
    d3d_set_fog_ext(enable ? 1 : 0, color_bgr, start, end);
}

void Renderer2D::d3d_set_fog_ext(i32 mode, u32 color_bgr, f32 start, f32 end) {
    flush();
    fog_.mode = std::clamp(mode, 0, 2);
    if (fog_.mode != 0) {
        fog_.color_bgr = color_bgr & 0x00FFFFFFu;
        fog_.start = start;
        fog_.end = end;
    }
}

void Renderer2D::d3d_light_define_ambient(u32 color_bgr) {
    flush();
    ambient_bgr_ = color_bgr & 0x00FFFFFFu;
}

void Renderer2D::d3d_light_define_direction(i32 ind, f32 dx, f32 dy, f32 dz, u32 color_bgr) {
    if (ind < 0 || ind >= 8) return;
    flush();
    auto& l = lights_[static_cast<usize>(ind)];
    const ColorRGBA c = gm_bgr_to_rgba(color_bgr);
    l.type = 3; // D3DLIGHT_DIRECTIONAL
    l.dir[0] = dx; l.dir[1] = dy; l.dir[2] = dz;
    l.diffuse[0] = c.r / 255.0f; l.diffuse[1] = c.g / 255.0f; l.diffuse[2] = c.b / 255.0f;
    l.specular[0] = l.specular[1] = l.specular[2] = 0.0f;
    l.atten[0] = 1.0f; l.atten[1] = 0.0f; l.atten[2] = 0.0f;
}

void Renderer2D::d3d_light_define_point(i32 ind, f32 x, f32 y, f32 z, f32 range, u32 color_bgr) {
    if (ind < 0 || ind >= 8) return;
    flush();
    auto& l = lights_[static_cast<usize>(ind)];
    const ColorRGBA c = gm_bgr_to_rgba(color_bgr);
    l.type = 1; // D3DLIGHT_POINT
    l.pos[0] = x; l.pos[1] = y; l.pos[2] = z;
    l.range = range;
    l.diffuse[0] = c.r / 255.0f; l.diffuse[1] = c.g / 255.0f; l.diffuse[2] = c.b / 255.0f;
    l.specular[0] = l.specular[1] = l.specular[2] = 0.0f;
    // Exact GM8 sub_564864 attenuation: a0 = 1.0, a1 = 4.0 / range, a2 = 0.0.
    l.atten[0] = 1.0f;
    l.atten[1] = (range > 0.0f) ? (4.0f / range) : 0.0f;
    l.atten[2] = 0.0f;
}

void Renderer2D::d3d_light_define_ext(
    i32 ind, i32 type, f32 x, f32 y, f32 z, f32 dx, f32 dy, f32 dz,
    f32 range, f32 inner_deg, f32 outer_deg, u32 diff_bgr, f32 diff_factor,
    u32 spec_bgr, f32 /*spec_factor*/) {
    if (ind < 0 || ind >= 8) return;
    if (range <= 0.0f && type != 3) return;
    flush();
    auto& l = lights_[static_cast<usize>(ind)];
    const ColorRGBA dc = gm_bgr_to_rgba(diff_bgr);
    const ColorRGBA sc = gm_bgr_to_rgba(spec_bgr);
    l.type = type;
    l.pos[0] = x; l.pos[1] = y; l.pos[2] = z;
    l.dir[0] = dx; l.dir[1] = dy; l.dir[2] = dz;
    l.range = range;
    l.theta = static_cast<f32>(deg_to_rad(inner_deg));
    l.phi = static_cast<f32>(deg_to_rad(outer_deg));
    l.falloff = 1.0f;
    l.atten[0] = 0.8f;
    l.atten[1] = 0.0f;
    l.atten[2] = (range > 0.0f) ? (0.1f / range) : 0.0f;
    l.diffuse[0] = (dc.r / 255.0f) * diff_factor;
    l.diffuse[1] = (dc.g / 255.0f) * diff_factor;
    l.diffuse[2] = (dc.b / 255.0f) * diff_factor;
    l.specular[0] = sc.r / 255.0f;
    l.specular[1] = sc.g / 255.0f;
    l.specular[2] = sc.b / 255.0f;
    l.enabled = true;
}

void Renderer2D::d3d_light_enable(i32 ind, bool enable) {
    if (ind < 0 || ind >= 8) return;
    flush();
    lights_[static_cast<usize>(ind)].enabled = enable;
}

bool Renderer2D::d3d_light_is_enabled(i32 ind) const noexcept {
    if (ind < 0 || ind >= 8) return false;
    return lights_[static_cast<usize>(ind)].enabled;
}

void Renderer2D::d3d_set_projection(f32 xfrom, f32 yfrom, f32 zfrom, f32 xto, f32 yto, f32 zto, f32 xup, f32 yup, f32 zup) {
    // Exact GM8 sub_563B84: sets D3DTS_VIEW ONLY, preserving D3DTS_PROJECTION.
    flush();
    build_look_at_lh(view_matrix_, xfrom, yfrom, zfrom, xto, yto, zto, xup, yup, zup);
}

void Renderer2D::d3d_set_projection_ext(
    f32 xfrom, f32 yfrom, f32 zfrom,
    f32 xto, f32 yto, f32 zto,
    f32 xup, f32 yup, f32 zup,
    f32 angle_deg, f32 aspect, f32 znear, f32 zfar) {
    flush();
    build_look_at_lh(view_matrix_, xfrom, yfrom, zfrom, xto, yto, zto, xup, yup, zup);
    if (znear != zfar) {
        build_perspective_fov_lh(proj_matrix_, angle_deg, aspect, znear, zfar);
    }
}

void Renderer2D::d3d_set_projection_ortho(f32 x, f32 y, f32 w, f32 h, f32 angle_deg) {
    // Exact GM8 sub_563D70: LookAtLH(cx,cy,-16000 -> cx,cy,0, up=(sin(-a),cos(-a),0)) + OrthoLH(w, -h, 1, 32000).
    flush();
    const f32 rw = (std::fabs(w) > 1e-4f) ? w : 1.0f;
    const f32 rh = (std::fabs(h) > 1e-4f) ? h : 1.0f;
    const f32 cx = x + rw * 0.5f;
    const f32 cy = y + rh * 0.5f;
    const f32 rad = static_cast<f32>(deg_to_rad(-angle_deg));
    build_look_at_lh(view_matrix_, cx, cy, -16000.0f, cx, cy, 0.0f, std::sin(rad), std::cos(rad), 0.0f);
    build_ortho_lh(proj_matrix_, rw, -rh, 1.0f, 32000.0f);
}

void Renderer2D::d3d_set_projection_perspective(f32 x, f32 y, f32 w, f32 h, f32 angle_deg) {
    // Exact GM8 sub_563ECC: LookAtLH(cx,cy,-w -> cx,cy,0, up=(sin(-a),cos(-a),0)) + PerspectiveLH(1, h/w, 1, 32000).
    flush();
    const f32 rw = (std::fabs(w) > 1e-4f) ? w : 1.0f;
    const f32 rh = (std::fabs(h) > 1e-4f) ? h : 1.0f;
    const f32 cx = x + rw * 0.5f;
    const f32 cy = y + rh * 0.5f;
    const f32 rad = static_cast<f32>(deg_to_rad(-angle_deg));
    build_look_at_lh(view_matrix_, cx, cy, -rw, cx, cy, 0.0f, std::sin(rad), std::cos(rad), 0.0f);
    build_perspective_lh(proj_matrix_, 1.0f, rh / rw, 1.0f, 32000.0f);
}

void Renderer2D::d3d_transform_set_identity() {
    flush();
    mat4_identity(world_matrix_);
}

void Renderer2D::d3d_transform_set_matrix(const f32 col_major[16]) {
    flush();
    std::memcpy(world_matrix_, col_major, sizeof(world_matrix_));
}

void Renderer2D::d3d_transform_add_matrix(const f32 col_major[16]) {
    flush();
    mat4_mul(world_matrix_, col_major, world_matrix_);
}

void Renderer2D::d3d_transform_set_translation(f32 xt, f32 yt, f32 zt) {
    flush();
    mat4_identity(world_matrix_);
    world_matrix_[12] = xt;
    world_matrix_[13] = yt;
    world_matrix_[14] = zt;
}

void Renderer2D::d3d_transform_set_scaling(f32 xs, f32 ys, f32 zs) {
    flush();
    mat4_identity(world_matrix_);
    world_matrix_[0] = xs;
    world_matrix_[5] = ys;
    world_matrix_[10] = zs;
}

void Renderer2D::d3d_transform_set_rotation_x(f32 angle_deg) {
    flush();
    mat4_identity(world_matrix_);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    world_matrix_[5] = c;  world_matrix_[6] = -s;
    world_matrix_[9] = s;  world_matrix_[10] = c;
}

void Renderer2D::d3d_transform_set_rotation_y(f32 angle_deg) {
    flush();
    mat4_identity(world_matrix_);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    world_matrix_[0] = c;   world_matrix_[2] = s;
    world_matrix_[8] = -s;  world_matrix_[10] = c;
}

void Renderer2D::d3d_transform_set_rotation_z(f32 angle_deg) {
    flush();
    mat4_identity(world_matrix_);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    world_matrix_[0] = c;  world_matrix_[1] = -s;
    world_matrix_[4] = s;  world_matrix_[5] = c;
}

void Renderer2D::d3d_transform_set_rotation_axis(f32 xa, f32 ya, f32 za, f32 angle_deg) {
    flush();
    mat4_identity(world_matrix_);
    const f32 len = std::sqrt(xa * xa + ya * ya + za * za);
    if (len < 1e-6f) return;
    const f32 x = xa / len, y = ya / len, z = za / len;
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = -std::sin(r), t = 1.0f - c;
    world_matrix_[0] = t * x * x + c;      world_matrix_[4] = t * x * y - s * z;  world_matrix_[8]  = t * x * z + s * y;
    world_matrix_[1] = t * x * y + s * z;  world_matrix_[5] = t * y * y + c;      world_matrix_[9]  = t * y * z - s * x;
    world_matrix_[2] = t * x * z - s * y;  world_matrix_[6] = t * y * z + s * x;  world_matrix_[10] = t * z * z + c;
}

void Renderer2D::d3d_transform_add_translation(f32 xt, f32 yt, f32 zt) {
    flush();
    f32 t[16];
    mat4_identity(t);
    t[12] = xt; t[13] = yt; t[14] = zt;
    mat4_mul(world_matrix_, t, world_matrix_);
}

void Renderer2D::d3d_transform_add_scaling(f32 xs, f32 ys, f32 zs) {
    flush();
    f32 t[16];
    mat4_identity(t);
    t[0] = xs; t[5] = ys; t[10] = zs;
    mat4_mul(world_matrix_, t, world_matrix_);
}

void Renderer2D::d3d_transform_add_rotation_x(f32 angle_deg) {
    flush();
    f32 t[16];
    mat4_identity(t);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    t[5] = c; t[6] = -s; t[9] = s; t[10] = c;
    mat4_mul(world_matrix_, t, world_matrix_);
}

void Renderer2D::d3d_transform_add_rotation_y(f32 angle_deg) {
    flush();
    f32 t[16];
    mat4_identity(t);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    t[0] = c; t[2] = s; t[8] = -s; t[10] = c;
    mat4_mul(world_matrix_, t, world_matrix_);
}

void Renderer2D::d3d_transform_add_rotation_z(f32 angle_deg) {
    flush();
    f32 t[16];
    mat4_identity(t);
    const f32 r = static_cast<f32>(deg_to_rad(angle_deg));
    const f32 c = std::cos(r), s = std::sin(r);
    t[0] = c; t[1] = -s; t[4] = s; t[5] = c;
    mat4_mul(world_matrix_, t, world_matrix_);
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
    mat4_mul(world_matrix_, t, world_matrix_);
}

void Renderer2D::d3d_transform_stack_clear() {
    transform_stack_.clear();
}

bool Renderer2D::d3d_transform_stack_push() {
    if (transform_stack_.size() >= 1001) return false; // GM8 cap
    std::array<f32, 16> copy{};
    std::memcpy(copy.data(), world_matrix_, sizeof(world_matrix_));
    transform_stack_.push_back(copy);
    return true;
}

bool Renderer2D::d3d_transform_stack_pop() {
    if (transform_stack_.empty()) return false;
    flush();
    std::memcpy(world_matrix_, transform_stack_.back().data(), sizeof(world_matrix_));
    transform_stack_.pop_back();
    return true;
}

bool Renderer2D::d3d_transform_stack_top() {
    if (transform_stack_.empty()) return false;
    flush();
    std::memcpy(world_matrix_, transform_stack_.back().data(), sizeof(world_matrix_));
    return true;
}

bool Renderer2D::d3d_transform_stack_discard() {
    if (transform_stack_.empty()) return false;
    transform_stack_.pop_back();
    return true;
}

bool Renderer2D::d3d_transform_add_stack_top() {
    if (transform_stack_.empty()) return false;
    flush();
    mat4_mul(world_matrix_, transform_stack_.back().data(), world_matrix_);
    return true;
}

void Renderer2D::d3d_transform_vertex(f32 x, f32 y, f32 z, f32& ox, f32& oy, f32& oz) const noexcept {
    const f32 rx = world_matrix_[0] * x + world_matrix_[4] * y + world_matrix_[8]  * z + world_matrix_[12];
    const f32 ry = world_matrix_[1] * x + world_matrix_[5] * y + world_matrix_[9]  * z + world_matrix_[13];
    const f32 rz = world_matrix_[2] * x + world_matrix_[6] * y + world_matrix_[10] * z + world_matrix_[14];
    const f32 rw = world_matrix_[3] * x + world_matrix_[7] * y + world_matrix_[11] * z + world_matrix_[15];
    if (std::fabs(rw) > 1e-6f) {
        ox = rx / rw; oy = ry / rw; oz = rz / rw;
    } else {
        ox = rx; oy = ry; oz = rz;
    }
}

void Renderer2D::d3d_project_vertex(f32 x, f32 y, f32 z, f32& ox, f32& oy, f32& oz) const noexcept {
    f32 vp[16];
    mat4_mul(vp, proj_matrix_, view_matrix_);
    const f32 rx = vp[0] * x + vp[4] * y + vp[8]  * z + vp[12];
    const f32 ry = vp[1] * x + vp[5] * y + vp[9]  * z + vp[13];
    const f32 rz = vp[2] * x + vp[6] * y + vp[10] * z + vp[14];
    const f32 rw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (std::fabs(rw) > 1e-6f) {
        ox = rx / rw;
        oy = ry / rw;
        // Convert OpenGL NDC Z [-1, +1] to D3D NDC Z [0, 1] for gm82dx9 d3d_project_vertex (z < 1 check).
        oz = 0.5f * (rz / rw) + 0.5f;
    } else {
        ox = 0.0f; oy = 0.0f; oz = 2.0f;
    }
}

void Renderer2D::d3d_get_eye_pos(f32& ox, f32& oy, f32& oz) const noexcept {
    f32 inv[16];
    if (mat4_invert(view_matrix_, inv)) {
        ox = inv[12];
        oy = inv[13];
        oz = inv[14];
    } else {
        ox = oy = oz = 0.0f;
    }
}

f32 Renderer2D::d3d_world_determinant() const noexcept {
    return mat4_det(world_matrix_);
}

f32 Renderer2D::d3d_projection_determinant() const noexcept {
    return mat4_det(proj_matrix_);
}

void Renderer2D::d3d_draw_block(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr) {
    const bool prev_rep = tex_repeat_;
    set_texture_repeat(true);
    std::vector<Vertex3D> tris;
    tris.reserve(36);
    append_d3d_block(tris, x1, y1, z1, x2, y2, z2, hr, vr, make_vertex_color(d3d_vertex_color(), draw_alpha_));
    ensure_batch(PrimitiveTopology::Triangles, tex);
    vertices_.insert(vertices_.end(), tris.begin(), tris.end());
    set_texture_repeat(prev_rep);
}

void Renderer2D::d3d_draw_floor(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr) {
    const bool prev_rep = tex_repeat_;
    set_texture_repeat(true);
    std::vector<Vertex3D> tris;
    tris.reserve(6);
    append_d3d_floor(tris, x1, y1, z1, x2, y2, z2, hr, vr, make_vertex_color(d3d_vertex_color(), draw_alpha_));
    ensure_batch(PrimitiveTopology::Triangles, tex);
    vertices_.insert(vertices_.end(), tris.begin(), tris.end());
    set_texture_repeat(prev_rep);
}

void Renderer2D::d3d_draw_wall(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr) {
    if (std::hypot(x2 - x1, y2 - y1) == 0.0f) return;
    const bool prev_rep = tex_repeat_;
    set_texture_repeat(true);
    std::vector<Vertex3D> tris;
    tris.reserve(6);
    append_d3d_wall(tris, x1, y1, z1, x2, y2, z2, hr, vr, make_vertex_color(d3d_vertex_color(), draw_alpha_));
    ensure_batch(PrimitiveTopology::Triangles, tex);
    vertices_.insert(vertices_.end(), tris.begin(), tris.end());
    set_texture_repeat(prev_rep);
}

void Renderer2D::d3d_draw_cylinder(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr, bool closed, i32 steps) {
    const bool prev_rep = tex_repeat_;
    set_texture_repeat(true);
    std::vector<Vertex3D> tris;
    append_d3d_cylinder(tris, x1, y1, z1, x2, y2, z2, hr, vr, closed, steps, make_vertex_color(d3d_vertex_color(), draw_alpha_));
    for (usize i = 0; i < tris.size(); i += 3) {
        ensure_batch(PrimitiveTopology::Triangles, tex);
        vertices_.push_back(tris[i]);
        vertices_.push_back(tris[i + 1]);
        vertices_.push_back(tris[i + 2]);
    }
    set_texture_repeat(prev_rep);
}

void Renderer2D::d3d_draw_cone(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr, bool closed, i32 steps) {
    const bool prev_rep = tex_repeat_;
    set_texture_repeat(true);
    std::vector<Vertex3D> tris;
    append_d3d_cone(tris, x1, y1, z1, x2, y2, z2, hr, vr, closed, steps, make_vertex_color(d3d_vertex_color(), draw_alpha_));
    for (usize i = 0; i < tris.size(); i += 3) {
        ensure_batch(PrimitiveTopology::Triangles, tex);
        vertices_.push_back(tris[i]);
        vertices_.push_back(tris[i + 1]);
        vertices_.push_back(tris[i + 2]);
    }
    set_texture_repeat(prev_rep);
}

void Renderer2D::d3d_draw_ellipsoid(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hr, f32 vr, i32 steps) {
    const bool prev_rep = tex_repeat_;
    set_texture_repeat(true);
    std::vector<Vertex3D> tris;
    append_d3d_ellipsoid(tris, x1, y1, z1, x2, y2, z2, hr, vr, steps, make_vertex_color(d3d_vertex_color(), draw_alpha_));
    for (usize i = 0; i < tris.size(); i += 3) {
        ensure_batch(PrimitiveTopology::Triangles, tex);
        vertices_.push_back(tris[i]);
        vertices_.push_back(tris[i + 1]);
        vertices_.push_back(tris[i + 2]);
    }
    set_texture_repeat(prev_rep);
}

void Renderer2D::draw_model_bucket(const Model3DMeshBucket& bucket, PrimitiveTopology topo, TextureHandle tex) {
    if (bucket.verts.empty()) return;

    const ColorRGBA cur_col = make_vertex_color(d3d_vertex_color(), draw_alpha_);
    if (bucket.gpu_uploaded && bucket.vao != 0) {
        f32 tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (bucket.has_uncolored && !bucket.has_explicit) {
            tint[0] = cur_col.r / 255.0f;
            tint[1] = cur_col.g / 255.0f;
            tint[2] = cur_col.b / 255.0f;
            tint[3] = cur_col.a / 255.0f;
        }
        device_->set_blend_state(blend_state_);
        bool used_custom_shader = false;
        if (dx9_.has_active_shader()) {
            used_custom_shader = dx9_.bind_and_upload(make_dx9_draw_context(tint), tex);
        }
        if (!used_custom_shader) {
            upload_shader_uniforms(tint);
            apply_stage0_ff_sampler(tex);
            device_->bind_texture(0, tex);
        }
        glBindVertexArray(bucket.vao);
        device_->draw_primitives(topo, static_cast<i32>(bucket.verts.size()), d3d_flat_shading_ && !used_custom_shader);
        glBindVertexArray(0);
        return;
    }

    // Fallback for models mixing explicit-colored and default-colored vertices.
    const usize step = (topo == PrimitiveTopology::Triangles) ? 3u : (topo == PrimitiveTopology::Lines ? 2u : 1u);
    for (usize i = 0; i + step <= bucket.verts.size(); i += step) {
        ensure_batch(topo, tex);
        for (usize j = 0; j < step; ++j) {
            Vertex3D v = bucket.verts[i + j];
            if (bucket.explicit_color[i + j] == 0u) {
                v.r = cur_col.r;
                v.g = cur_col.g;
                v.b = cur_col.b;
                v.a = cur_col.a;
            }
            vertices_.push_back(v);
        }
    }
    flush();
}

void Renderer2D::d3d_model_draw(i32 id, f32 x, f32 y, f32 z, TextureHandle tex) {
    Model3DData* model = models_.prepare_for_draw(id, recorded_d3d_);
    if (!model) return;
    if (model->triangles.verts.empty() && model->lines.verts.empty() && model->points.verts.empty()) {
        return;
    }

    flush();

    // Exact GM8 sub_568AA8 transform order: W_new = W_cur * T(x, y, z) (OpenGL col-major = D3D T * W_cur).
    f32 saved_world[16];
    std::memcpy(saved_world, world_matrix_, sizeof(saved_world));
    if (x != 0.0f || y != 0.0f || z != 0.0f) {
        f32 trans[16];
        mat4_identity(trans);
        trans[12] = x; trans[13] = y; trans[14] = z;
        mat4_mul(world_matrix_, saved_world, trans);
    }

    // Model vertices use D3D's integer pixel centers. Shift by half a viewport
    // pixel after projection so scaling and perspective do not change the offset.
    pixel_center_offset_ = {1.0f / active_viewport_.width,
                            (rendering_to_fbo() ? 1.0f : -1.0f) / active_viewport_.height};
    draw_model_bucket(model->triangles, PrimitiveTopology::Triangles, tex);
    draw_model_bucket(model->lines, PrimitiveTopology::Lines, tex);
    draw_model_bucket(model->points, PrimitiveTopology::Points, tex);
    pixel_center_offset_ = {};

    std::memcpy(world_matrix_, saved_world, sizeof(saved_world));
}

void Renderer2D::primitive_end() {
    if (!in_primitive_) return;
    in_primitive_ = false;
    const usize n = prim_verts_.size();
    if (n == 0) return;

    // Exact GM 8.2 zero-primitive guard (00564F30 / 0056788C).
    PrimitiveTopology topo = PrimitiveTopology::Triangles;
    usize min_verts = 1;
    switch (prim_kind_) {
        case 1: topo = PrimitiveTopology::Points;        min_verts = 1; break;
        case 2: topo = PrimitiveTopology::Lines;         min_verts = 2; break;
        case 3: topo = PrimitiveTopology::LineStrip;     min_verts = 2; break;
        case 4: topo = PrimitiveTopology::Triangles;     min_verts = 3; break;
        case 5: topo = PrimitiveTopology::TriangleStrip; min_verts = 3; break;
        case 6: topo = PrimitiveTopology::TriangleFan;   min_verts = 3; break;
        default: topo = PrimitiveTopology::Triangles;    min_verts = 3; break;
    }
    if (n < min_verts) {
        prim_verts_.clear();
        return;
    }

    flush();
    batch_topo_ = topo;
    batch_tex_ = prim_tex_;
    vertices_ = std::move(prim_verts_);
    prim_verts_.clear();
    flush();
}

void Renderer2D::dx9_set_shader_matrix(bool is_vs, u32 reg, i32 mask, i32 size) {
    flush();
    f32 m[16];
    if ((mask & 1) != 0) {
        std::memcpy(m, world_matrix_, sizeof(m));
    } else {
        mat4_identity(m);
    }
    if ((mask & 2) != 0) {
        mat4_mul(m, view_matrix_, m);
    }
    if ((mask & 4) != 0) {
        mat4_mul(m, proj_matrix_, m);
    }
    const u32 count = static_cast<u32>(std::clamp(size, 1, 4));
    if (is_vs) {
        dx9_.set_vs_const_f(reg, m, count);
    } else {
        dx9_.set_ps_const_f(reg, m, count);
    }
}

bool Renderer2D::dx9_draw_vertex_buffer(
    i32 vbuf_id, i32 vformat_id, i32 prim_type, TextureHandle tex, i32 vert_count, bool indexed) {
    flush();
    static constexpr f32 kWhiteTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    device_->set_blend_state(blend_state_);
    if (!dx9_.has_active_shader()) {
        upload_shader_uniforms(kWhiteTint);
    }
    return dx9_.draw_vertex_buffer(
        vbuf_id, vformat_id, prim_type, tex, vert_count, indexed,
        make_dx9_draw_context(kWhiteTint), shader_);
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
