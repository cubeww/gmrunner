#include "extensions/shim_gm82dx.h"

#include "base/log.h"
#include "graphics/dx9_shader_transpiler.h"
#include "graphics/gl_graphics_device.h"
#include "graphics/renderer2d.h"
#include "platform/clock.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace gm {

namespace {

constexpr char kHexDigits[16] = {
    '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'
};

void write_u32_hex(char* dst, u32 val) noexcept {
    u8 bytes[4];
    std::memcpy(bytes, &val, 4);
    for (int i = 0; i < 4; ++i) {
        dst[i * 2] = kHexDigits[(bytes[i] >> 4) & 0x0Fu];
        dst[i * 2 + 1] = kHexDigits[bytes[i] & 0x0Fu];
    }
}

void write_f64_hex(char* dst, f64 val) noexcept {
    u8 bytes[8];
    std::memcpy(bytes, &val, 8);
    for (int i = 0; i < 8; ++i) {
        dst[i * 2] = kHexDigits[(bytes[i] >> 4) & 0x0Fu];
        dst[i * 2 + 1] = kHexDigits[bytes[i] & 0x0Fu];
    }
}

// Strips "__gm82dx9_" or "__gm82dx8_" prefix to allow unified dispatch for both extensions.
std::string_view strip_dx_prefix(std::string_view fn) noexcept {
    if (fn.starts_with("__gm82dx9_")) return fn.substr(10);
    if (fn.starts_with("__gm82dx8_")) return fn.substr(10);
    return fn;
}

class Gm82DxShim final : public IDllShim {
public:
    explicit Gm82DxShim(std::shared_ptr<GmBufferStore> buffers)
        : buffers_(std::move(buffers)) {}

    std::string_view canonical_name() const noexcept override { return "gm82dx"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "gm82dx9" ||
               normalized_dll == "gm82dx8" ||
               normalized_dll == "d3dx9_43";
    }

    bool has_function(std::string_view) const noexcept override {
        return true;
    }

    void on_unload(IExtensionHost&) override {
        has_started_ = false;
        vx_ = vy_ = vz_ = 0.0f;
    }

    RValue invoke(
        IExtensionHost& host,
        std::string_view fn,
        std::span<const RValue> args,
        ExternalValueType) override {
        auto& renderer = host.ext_renderer();
        auto& gfx = host.ext_gfx();
        auto& dx9 = renderer.dx9();

        // D3DX9_43 preload functions (e.g. D3DXCheckVersion / D3DXGetErrorStringA).
        if (fn.starts_with("D3DX")) {
            return RValue(1.0);
        }

        const std::string_view s = strip_dx_prefix(fn);

        // -------------------------------------------------------------------
        // Lifecycle & checks
        // -------------------------------------------------------------------
        if (s == "dllcheck") {
            return RValue(820.0);
        }
        if (s == "dll_init" || s == "init") {
            return RValue(1.0);
        }
        if (s == "checkstart") {
            if (has_started_) return RValue(1.0);
            has_started_ = true;
            return RValue(0.0);
        }
        if (s == "abort") {
            GM_LOG_ERROR("ext", "gm82dx: conflicting DX8 and DX9 extensions detected");
            host.ext_game_end();
            return RValue(0.0);
        }
        if (s == "testfunc") {
            return RValue(0.0);
        }
        if (s == "not_xp") {
            return RValue(1.0);
        }
        if (s == "time_now") {
            return RValue(static_cast<f64>(Clock::now_us()) / 1000.0);
        }

        // -------------------------------------------------------------------
        // Color packing helpers
        // -------------------------------------------------------------------
        if (fn == "argb_get_color") {
            const u32 col = static_cast<u32>(shim_real(args, 0, 0.0)) & 0x00FFFFFFu;
            const u32 bgr = ((col & 0xFF0000u) >> 16) | (col & 0x00FF00u) | ((col & 0x0000FFu) << 16);
            return RValue(static_cast<f64>(bgr));
        }
        if (fn == "argb_get_alpha") {
            const u32 col = static_cast<u32>(shim_real(args, 0, 0.0));
            return RValue(static_cast<f64>((col >> 24) & 0xFFu) / 255.0);
        }
        if (fn == "argb_make_color" || fn == "color_to_d3dcolor") {
            const u32 col = static_cast<u32>(shim_real(args, 0, 0.0)) & 0x00FFFFFFu;
            const u32 alpha = static_cast<u32>(std::clamp(shim_real(args, 1, 1.0), 0.0, 1.0) * 255.0);
            const u32 argb = (alpha << 24) |
                             ((col & 0x0000FFu) << 16) |
                             (col & 0x00FF00u) |
                             ((col & 0xFF0000u) >> 16);
            return RValue(static_cast<f64>(argb));
        }

        // -------------------------------------------------------------------
        // Surfaces & buffer/grid pixel transfer
        // -------------------------------------------------------------------
        if (fn == "surface_get_count") {
            return RValue(static_cast<f64>(renderer.surface_count()));
        }
        if (s == "surface_ptr") {
            const i32 id = shim_i32(args, 0, -1);
            if (!renderer.surface_exists(id)) return RValue(-4.0);
            return RValue(static_cast<f64>(renderer.surface_unique_token(id)));
        }
        if (s == "surface_set_depth") {
            const bool ok = renderer.surface_set_depth(shim_i32(args, 0, -1));
            return RValue(ok ? 0.0 : -1.0);
        }
        if (s == "surface_set_target_ext") {
            const bool ok = renderer.surface_set_target_ext(
                shim_i32(args, 0, 1),
                shim_i32(args, 1, -1));
            return RValue(ok ? 1.0 : 0.0);
        }
        if (s == "buffer_to_surface") {
            const u32 addr = static_cast<u32>(shim_real(args, 0, 0.0));
            const i32 surf_id = shim_i32(args, 1, -1);
            const i32 w = shim_i32(args, 2, 0);
            const i32 h = shim_i32(args, 3, 0);
            if (!buffers_ || w <= 0 || h <= 0) return RValue(-1.0);
            auto* buf = buffers_->find_by_address(addr);
            if (!buf) return RValue(-1.0);
            const usize need = static_cast<usize>(w) * static_cast<usize>(h) * 4;
            if (buf->data.size() < need) return RValue(-1.0);
            const bool ok = renderer.surface_write_pixels_bgra(
                surf_id, w, h, std::span<const u8>(buf->data.data(), need));
            return RValue(ok ? 0.0 : -1.0);
        }
        if (s == "surface_to_buffer") {
            const u32 addr = static_cast<u32>(shim_real(args, 0, 0.0));
            const i32 surf_id = shim_i32(args, 1, -1);
            const i32 w = shim_i32(args, 2, 0);
            const i32 h = shim_i32(args, 3, 0);
            if (!buffers_ || w <= 0 || h <= 0) return RValue(-1.0);
            auto* buf = buffers_->find_by_address(addr);
            if (!buf) return RValue(-1.0);
            std::vector<u8> bgra;
            if (!renderer.surface_read_pixels_bgra(surf_id, 0, 0, w, h, bgra)) {
                return RValue(-1.0);
            }
            const usize need = static_cast<usize>(w) * static_cast<usize>(h) * 4;
            if (buf->data.size() < need) buf->data.resize(need, 0);
            std::memcpy(buf->data.data(), bgra.data(), need);
            return RValue(0.0);
        }
        if (s == "surface_to_grid") {
            const i32 surf_id = shim_i32(args, 0, -1);
            const i32 x = shim_i32(args, 1, 0);
            const i32 y = shim_i32(args, 2, 0);
            const i32 w = shim_i32(args, 3, 0);
            const i32 h = shim_i32(args, 4, 0);
            if (w <= 0 || h <= 0) return RValue(std::string{});
            std::vector<u8> bgra;
            if (!renderer.surface_read_pixels_bgra(surf_id, x, y, w, h, bgra)) {
                return RValue(std::string{});
            }
            // Match GM8 ds_grid_read v601 hex format: 12-byte header + w*h 16-byte RValues (column-major).
            const usize cell_count = static_cast<usize>(w) * static_cast<usize>(h);
            std::string out((cell_count * 16 + 12) * 2, '0');
            write_u32_hex(&out[0], 601u);
            write_u32_hex(&out[8], static_cast<u32>(w));
            write_u32_hex(&out[16], static_cast<u32>(h));
            usize idx = 0;
            for (i32 gx = 0; gx < w; ++gx) {
                for (i32 gy = 0; gy < h; ++gy) {
                    const usize px_off = (static_cast<usize>(gy) * static_cast<usize>(w) + static_cast<usize>(gx)) * 4;
                    const u32 b = bgra[px_off + 0];
                    const u32 g = bgra[px_off + 1];
                    const u32 r = bgra[px_off + 2];
                    const u32 a = bgra[px_off + 3];
                    const u32 packed = (a << 24) | (b << 16) | (g << 8) | r;
                    // RValue layout in GM8 hex string: 4 bytes type (0=real) + 4 pad + 8 bytes f64.
                    // Header = 12 bytes, + 4 bytes type/pad = offset 16 bytes for the first f64?
                    // Wait: exact gm82dx9.cpp:529 writes f64 at byte offset (16 + idx * 16) -> 4-byte padding after 12-byte header.
                    write_f64_hex(&out[(16 + idx * 16) * 2], static_cast<f64>(packed));
                    ++idx;
                }
            }
            return RValue(std::move(out));
        }

        // -------------------------------------------------------------------
        // Render state & device control
        // -------------------------------------------------------------------
        if (s == "set_color_mask") {
            renderer.set_color_write_enable(
                shim_real(args, 0, 1.0) >= 0.5,
                shim_real(args, 1, 1.0) >= 0.5,
                shim_real(args, 2, 1.0) >= 0.5,
                shim_real(args, 3, 1.0) >= 0.5);
            return RValue(0.0);
        }
        if (s == "set_fill_mode") {
            const f64 mode = shim_real(args, 0, 2.0);
            const i32 pm = (mode < 0.5) ? 0 : (mode < 1.5 ? 1 : 2);
            renderer.flush();
            gfx.set_polygon_mode(pm);
            return RValue(0.0);
        }
        if (s == "set_cull_mode") {
            const f64 mode = shim_real(args, 0, 0.0);
            const i32 cm = (mode < 0.5) ? 0 : (mode < 1.5 ? 1 : 2);
            renderer.set_cull_mode_ext(cm);
            return RValue(0.0);
        }
        if (s == "set_zcomp_mode") {
            renderer.set_zcomp_mode(shim_i32(args, 0, 4));
            return RValue(0.0);
        }
        if (s == "set_zbias") {
            renderer.flush();
            gfx.set_depth_bias(static_cast<f32>(shim_real(args, 0, 0.0)));
            return RValue(0.0);
        }
        if (s == "set_alphatest") {
            const bool en = shim_real(args, 0, 0.0) > 0.5;
            const u8 ref = static_cast<u8>(std::clamp(shim_i32(args, 1, 0), 0, 255));
            const i32 cmp = shim_i32(args, 2, 5);
            renderer.set_alpha_test_ext(en, ref, cmp);
            return RValue(0.0);
        }
        if (s == "cleardepth") {
            renderer.clear_depth(1.0f);
            return RValue(1.0);
        }
        if (s == "setfullscreen") {
            const i32 hz = shim_i32(args, 0, 0);
            host.ext_window().set_fullscreen(hz != 0);
            return RValue(1.0);
        }
        if (s == "setalphabuffer") {
            return RValue(0.0);
        }
        if (s == "resize_backbuffer") {
            const i32 w = static_cast<i32>(std::llround(shim_real(args, 0, 800.0)));
            const i32 h = static_cast<i32>(std::llround(shim_real(args, 1, 608.0)));
            host.ext_resize_backbuffer(w, h);
            return RValue(0.0);
        }
        if (s == "setpointscale") {
            return RValue(0.0);
        }
        if (s == "setsepalphablend") {
            renderer.set_separate_alpha_blend(
                shim_real(args, 0, 0.0) > 0.5,
                shim_i32(args, 1, 5),
                shim_i32(args, 2, 6));
            return RValue(0.0);
        }
        if (s == "setviewport") {
            renderer.set_viewport_ext(
                shim_i32(args, 0, 0),
                shim_i32(args, 1, 0),
                shim_i32(args, 2, 1),
                shim_i32(args, 3, 1));
            return RValue(0.0);
        }
        if (s == "getviewportx") return RValue(static_cast<f64>(renderer.active_viewport().x));
        if (s == "getviewporty") return RValue(static_cast<f64>(renderer.active_viewport().y));
        if (s == "getviewportw") return RValue(static_cast<f64>(renderer.active_viewport().width));
        if (s == "getviewporth") return RValue(static_cast<f64>(renderer.active_viewport().height));
        if (s == "setzscale") return RValue(0.0);
        if (s == "getvideomem") return RValue(1073741824.0);
        if (s == "getvendor") return RValue(0.0);
        if (s == "getmaxwidth" || s == "getmaxheight") return RValue(16384.0);

        // -------------------------------------------------------------------
        // Lighting & Fog extensions and queries
        // -------------------------------------------------------------------
        if (fn == "d3d_light_enabled") {
            return RValue(renderer.d3d_light_is_enabled(shim_i32(args, 0, 0)) ? 1.0 : 0.0);
        }
        if (fn == "d3d_get_lighting") {
            return RValue(renderer.d3d_lighting() ? 1.0 : 0.0);
        }
        if (fn == "d3d_light_get_ambient") {
            return RValue(static_cast<f64>(renderer.d3d_light_ambient()));
        }
        if (s == "buffer_get_lights") {
            const u32 addr = static_cast<u32>(shim_real(args, 0, 0.0));
            if (!buffers_) return RValue(0.0);
            auto* buf = buffers_->find_by_address(addr);
            if (!buf) return RValue(0.0);
            if (buf->data.size() < 8 * 104) buf->data.resize(8 * 104, 0);
            struct D3dLight9Packed {
                u32 type;
                f32 diffuse[4];
                f32 specular[4];
                f32 ambient[4];
                f32 pos[3];
                f32 dir[3];
                f32 range;
                f32 falloff;
                f32 atten0;
                f32 atten1;
                f32 atten2;
                f32 theta;
                f32 phi;
            };
            static_assert(sizeof(D3dLight9Packed) == 104);
            for (i32 i = 0; i < 8; ++i) {
                const auto& l = renderer.d3d_light(i);
                D3dLight9Packed p{};
                p.type = static_cast<u32>(l.type);
                p.diffuse[0] = l.diffuse[0];
                p.diffuse[1] = l.diffuse[1];
                p.diffuse[2] = l.diffuse[2];
                p.diffuse[3] = 1.0f;
                p.specular[0] = l.specular[0];
                p.specular[1] = l.specular[1];
                p.specular[2] = l.specular[2];
                p.specular[3] = 1.0f;
                p.pos[0] = l.pos[0];
                p.pos[1] = l.pos[1];
                p.pos[2] = l.pos[2];
                p.dir[0] = l.dir[0];
                p.dir[1] = l.dir[1];
                p.dir[2] = l.dir[2];
                p.range = l.range;
                p.falloff = l.falloff;
                p.atten0 = l.atten[0];
                p.atten1 = l.atten[1];
                p.atten2 = l.atten[2];
                p.theta = l.theta;
                p.phi = l.phi;
                std::memcpy(buf->data.data() + static_cast<usize>(i) * 104, &p, 104);
            }
            return RValue(0.0);
        }
        if (s == "set_light") {
            renderer.d3d_light_define_ext(
                shim_i32(args, 0, 0),
                shim_i32(args, 1, 3),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 0.0)),
                static_cast<f32>(shim_real(args, 4, 0.0)),
                static_cast<f32>(shim_real(args, 5, 0.0)),
                static_cast<f32>(shim_real(args, 6, 0.0)),
                static_cast<f32>(shim_real(args, 7, 1.0)),
                static_cast<f32>(shim_real(args, 8, 1.0)),
                static_cast<f32>(shim_real(args, 9, 0.0)),
                static_cast<f32>(shim_real(args, 10, 0.0)),
                static_cast<u32>(std::llround(shim_real(args, 11, 0.0))),
                static_cast<f32>(shim_real(args, 12, 1.0)),
                static_cast<u32>(std::llround(shim_real(args, 13, 0.0))),
                static_cast<f32>(shim_real(args, 14, 10.0)));
            return RValue(0.0);
        }
        if (s == "setrangefog") {
            renderer.d3d_set_fog_ext(
                shim_i32(args, 0, 0),
                static_cast<u32>(std::llround(shim_real(args, 1, 0.0))),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                static_cast<f32>(shim_real(args, 3, 1.0)));
            return RValue(0.0);
        }
        if (fn == "d3d_get_fog_enabled") {
            return RValue(renderer.d3d_fog().mode > 0 ? 1.0 : 0.0);
        }
        if (fn == "d3d_get_fog_color") {
            return RValue(static_cast<f64>(renderer.d3d_fog().color_bgr));
        }
        if (fn == "d3d_get_fog_start") {
            return RValue(static_cast<f64>(renderer.d3d_fog().start));
        }
        if (fn == "d3d_get_fog_end") {
            return RValue(static_cast<f64>(renderer.d3d_fog().end));
        }

        // -------------------------------------------------------------------
        // Vertex projection & matrix queries/transforms
        // -------------------------------------------------------------------
        if (s == "transformvertex") {
            renderer.d3d_transform_vertex(
                static_cast<f32>(shim_real(args, 0, 0.0)),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                vx_, vy_, vz_);
            return RValue(static_cast<f64>(vx_));
        }
        if (s == "geteyepos") {
            renderer.d3d_get_eye_pos(vx_, vy_, vz_);
            return RValue(static_cast<f64>(vx_));
        }
        if (s == "projectvertex") {
            renderer.d3d_project_vertex(
                static_cast<f32>(shim_real(args, 0, 0.0)),
                static_cast<f32>(shim_real(args, 1, 0.0)),
                static_cast<f32>(shim_real(args, 2, 0.0)),
                vx_, vy_, vz_);
            return RValue(static_cast<f64>(vx_));
        }
        if (s == "getvertexy") return RValue(static_cast<f64>(vy_));
        if (s == "getvertexz") return RValue(static_cast<f64>(vz_));

        if (fn == "d3d_transform_set_matrix") {
            f32 m[16];
            for (usize i = 0; i < 16; ++i) {
                m[i] = static_cast<f32>(shim_real(args, i, (i % 5 == 0) ? 1.0 : 0.0));
            }
            renderer.d3d_transform_set_matrix(m);
            return RValue(0.0);
        }
        if (s == "set_matrix_from_buffer") {
            const u32 addr = static_cast<u32>(shim_real(args, 0, 0.0));
            if (buffers_) {
                if (auto* buf = buffers_->find_by_address(addr); buf && buf->data.size() >= 64) {
                    f32 m[16];
                    std::memcpy(m, buf->data.data(), 64);
                    renderer.d3d_transform_set_matrix(m);
                }
            }
            return RValue(0.0);
        }
        if (fn == "d3d_transform_add_stack_top") {
            renderer.d3d_transform_add_stack_top();
            return RValue(0.0);
        }
        if (fn == "d3d_transform_get_determinant") {
            return RValue(static_cast<f64>(renderer.d3d_world_determinant()));
        }
        if (fn == "d3d_projection_get_determinant") {
            return RValue(static_cast<f64>(renderer.d3d_projection_determinant()));
        }
        if (fn.starts_with("d3d_transform_") && fn.size() >= 16 && fn.size() <= 19) {
            const std::string_view suffix = fn.substr(14);
            if (!suffix.empty() && suffix.back() == 't') {
                usize pos = 0;
                bool valid = true;
                usize arg_idx = 0;
                // Parse 1..3 rotation axes (x/y/z), optional 's', and final 't'.
                while (pos < suffix.size() && (suffix[pos] == 'x' || suffix[pos] == 'y' || suffix[pos] == 'z')) {
                    ++pos;
                }
                const usize rot_count = pos;
                const bool has_scale = (pos < suffix.size() && suffix[pos] == 's');
                if (has_scale) ++pos;
                if (rot_count >= 1 && rot_count <= 3 && pos + 1 == suffix.size() && suffix[pos] == 't') {
                    for (usize i = 0; i < rot_count; ++i) {
                        const f32 deg = static_cast<f32>(shim_real(args, arg_idx++, 0.0));
                        if (suffix[i] == 'x') renderer.d3d_transform_add_rotation_x(deg);
                        else if (suffix[i] == 'y') renderer.d3d_transform_add_rotation_y(deg);
                        else renderer.d3d_transform_add_rotation_z(deg);
                    }
                    if (has_scale) {
                        const f32 sx = static_cast<f32>(shim_real(args, arg_idx++, 1.0));
                        const f32 sy = static_cast<f32>(shim_real(args, arg_idx++, 1.0));
                        const f32 sz = static_cast<f32>(shim_real(args, arg_idx++, 1.0));
                        renderer.d3d_transform_add_scaling(sx, sy, sz);
                    }
                    const f32 tx = static_cast<f32>(shim_real(args, arg_idx++, 0.0));
                    const f32 ty = static_cast<f32>(shim_real(args, arg_idx++, 0.0));
                    const f32 tz = static_cast<f32>(shim_real(args, arg_idx++, 0.0));
                    renderer.d3d_transform_add_translation(tx, ty, tz);
                    return RValue(0.0);
                }
                (void)valid;
            }
        }

        // -------------------------------------------------------------------
        // Texture & Multi-stage Sampler state (PS stages 0..7, VS stages 0..3)
        // -------------------------------------------------------------------
        if (fn == "texture_get_interpolation") {
            const auto& st0 = dx9.ps_sampler(0);
            const bool lin = st0.has_explicit_filter ? st0.linear_filter : renderer.interpolation();
            return RValue(lin ? 1.0 : 0.0);
        }
        if (s == "texture_stage_set") {
            const i32 stage = shim_i32(args, 0, 0);
            if (stage >= 0 && stage <= 7) {
                const i32 tid = shim_i32(args, 1, -1);
                renderer.flush();
                dx9.set_ps_sampler_tex(stage, tid > 0 ? TextureHandle{static_cast<u32>(tid)} : TextureHandle{});
            }
            return RValue(0.0);
        }
        if (s == "texture_stage_vertex_set") {
            const i32 stage = shim_i32(args, 0, 0);
            if (stage >= 0 && stage <= 3) {
                const i32 tid = shim_i32(args, 1, -1);
                renderer.flush();
                dx9.set_vs_sampler_tex(stage, tid > 0 ? TextureHandle{static_cast<u32>(tid)} : TextureHandle{});
            }
            return RValue(0.0);
        }
        if (s == "texture_set_stage_interpolation") {
            const i32 stage = shim_i32(args, 0, 0);
            if (stage >= 0 && stage <= 7) {
                const bool lin = shim_real(args, 1, 0.0) >= 0.5;
                renderer.flush();
                dx9.set_ps_sampler_filter(stage, lin);
            }
            return RValue(0.0);
        }
        if (s == "texture_set_stage_vertex_interpolation") {
            const i32 stage = shim_i32(args, 0, 0);
            if (stage >= 0 && stage <= 3) {
                const bool lin = shim_real(args, 1, 0.0) >= 0.5;
                renderer.flush();
                dx9.set_vs_sampler_filter(stage, lin);
            }
            return RValue(0.0);
        }
        if (s == "texture_set_stage_repeat") {
            const i32 stage = shim_i32(args, 0, 0);
            if (stage >= 0 && stage <= 7) {
                i32 hr = shim_i32(args, 1, 0);
                i32 vr = shim_i32(args, 2, 0);
                if (hr == 0) hr = 3; // false/tr_clamp -> D3DTADDRESS_CLAMP
                if (vr == 0) vr = 3;
                const u32 border = static_cast<u32>(std::llround(shim_real(args, 3, 0.0))) & 0x00FFFFFFu;
                renderer.flush();
                dx9.set_ps_sampler_wrap(stage, hr, vr, border);
            }
            return RValue(0.0);
        }
        if (s == "texture_set_stage_vertex_repeat") {
            const i32 stage = shim_i32(args, 0, 0);
            if (stage >= 0 && stage <= 3) {
                i32 hr = shim_i32(args, 1, 0);
                i32 vr = shim_i32(args, 2, 0);
                if (hr == 0) hr = 3;
                if (vr == 0) vr = 3;
                const u32 border = static_cast<u32>(std::llround(shim_real(args, 3, 0.0))) & 0x00FFFFFFu;
                renderer.flush();
                dx9.set_vs_sampler_wrap(stage, hr, vr, border);
            }
            return RValue(0.0);
        }
        if (s == "texture_set_mode") {
            renderer.set_texcoord_mode(shim_i32(args, 0));
            return RValue(0.0);
        }

        // -------------------------------------------------------------------
        // DX9 Programmable Shaders, CTAB Reflection & Uniform Register Banks
        // -------------------------------------------------------------------
        if (s == "shader_vertex_create_file") {
            auto bytes = host.ext_vfs().read_all_bytes(shim_str(args, 0));
            if (!bytes || !dx9_shader_bounds_check(bytes.value())) return RValue(-2.0);
            return RValue(static_cast<f64>(dx9.create_vertex_shader(bytes.value())));
        }
        if (s == "shader_pixel_create_file") {
            auto bytes = host.ext_vfs().read_all_bytes(shim_str(args, 0));
            if (!bytes || !dx9_shader_bounds_check(bytes.value())) return RValue(-2.0);
            return RValue(static_cast<f64>(dx9.create_pixel_shader(bytes.value())));
        }
        if (s == "shader_vertex_create_buffer") {
            const u32 addr = static_cast<u32>(shim_real(args, 0, 0.0));
            const i32 len = shim_i32(args, 1, 0);
            if (!buffers_) return RValue(-1.0);
            auto* buf = buffers_->find_by_address(addr);
            if (!buf) return RValue(-1.0);
            if (len <= 0 || static_cast<usize>(len) > buf->data.size()) return RValue(-3.0);
            const std::span<const u8> bytes(buf->data.data(), static_cast<usize>(len));
            if (!dx9_shader_bounds_check(bytes)) return RValue(-3.0);
            return RValue(static_cast<f64>(dx9.create_vertex_shader(bytes)));
        }
        if (s == "shader_pixel_create_buffer") {
            const u32 addr = static_cast<u32>(shim_real(args, 0, 0.0));
            const i32 len = shim_i32(args, 1, 0);
            if (!buffers_) return RValue(-1.0);
            auto* buf = buffers_->find_by_address(addr);
            if (!buf) return RValue(-1.0);
            if (len <= 0 || static_cast<usize>(len) > buf->data.size()) return RValue(-3.0);
            const std::span<const u8> bytes(buf->data.data(), static_cast<usize>(len));
            if (!dx9_shader_bounds_check(bytes)) return RValue(-3.0);
            return RValue(static_cast<f64>(dx9.create_pixel_shader(bytes)));
        }
        if (s == "shader_vertex_destroy") {
            renderer.flush();
            return RValue(dx9.destroy_vertex_shader(shim_i32(args, 0, -1)) ? 0.0 : 1.0);
        }
        if (s == "shader_pixel_destroy") {
            renderer.flush();
            return RValue(dx9.destroy_pixel_shader(shim_i32(args, 0, -1)) ? 0.0 : 1.0);
        }
        if (s == "shader_vertex_set") {
            renderer.flush();
            return RValue(dx9.set_vertex_shader(shim_i32(args, 0, -1)) ? 0.0 : 1.0);
        }
        if (s == "shader_pixel_set") {
            renderer.flush();
            return RValue(dx9.set_pixel_shader(shim_i32(args, 0, -1)) ? 0.0 : 1.0);
        }
        if (s == "shader_set") {
            renderer.flush();
            dx9.set_pixel_shader(shim_i32(args, 1, -1));
            dx9.set_vertex_shader(shim_i32(args, 0, -1));
            return RValue(0.0);
        }
        if (s == "shader_vertex_reset") {
            renderer.flush();
            dx9.reset_vertex_shader();
            return RValue(0.0);
        }
        if (s == "shader_pixel_reset") {
            renderer.flush();
            dx9.reset_pixel_shader();
            return RValue(0.0);
        }
        if (s == "shader_reset") {
            renderer.flush();
            dx9.reset_pixel_shader();
            dx9.reset_vertex_shader();
            return RValue(0.0);
        }
        if (s == "get_vconst") {
            return RValue(static_cast<f64>(dx9.get_vconst(shim_str(args, 0))));
        }
        if (s == "get_vconst_size") {
            return RValue(static_cast<f64>(dx9.get_vconst_size(shim_str(args, 0))));
        }
        if (s == "get_pconst") {
            return RValue(static_cast<f64>(dx9.get_pconst(shim_str(args, 0))));
        }
        if (s == "get_pconst_size") {
            return RValue(static_cast<f64>(dx9.get_pconst_size(shim_str(args, 0))));
        }
        if (s == "shader_vertex_uniform_4f" || s == "shader_pixel_uniform_4f") {
            const i32 reg = shim_i32(args, 0, -1);
            if (reg >= 0) {
                renderer.flush();
                const f32 v[4] = {
                    static_cast<f32>(shim_real(args, 1, 0.0)),
                    static_cast<f32>(shim_real(args, 2, 0.0)),
                    static_cast<f32>(shim_real(args, 3, 0.0)),
                    static_cast<f32>(shim_real(args, 4, 0.0)),
                };
                if (s[7] == 'v') dx9.set_vs_const_f(static_cast<u32>(reg), v, 1);
                else             dx9.set_ps_const_f(static_cast<u32>(reg), v, 1);
            }
            return RValue(0.0);
        }
        if (s == "shader_vertex_uniform_4i" || s == "shader_pixel_uniform_4i") {
            const i32 reg = shim_i32(args, 0, -1);
            if (reg >= 0) {
                renderer.flush();
                const i32 v[4] = {
                    shim_i32(args, 1, 0),
                    shim_i32(args, 2, 0),
                    shim_i32(args, 3, 0),
                    shim_i32(args, 4, 0),
                };
                if (s[7] == 'v') dx9.set_vs_const_i(static_cast<u32>(reg), v, 1);
                else             dx9.set_ps_const_i(static_cast<u32>(reg), v, 1);
            }
            return RValue(0.0);
        }
        if (s == "shader_vertex_uniform_b" || s == "shader_pixel_uniform_b") {
            const i32 reg = shim_i32(args, 0, -1);
            if (reg >= 0) {
                renderer.flush();
                const bool v = shim_real(args, 1, 0.0) >= 0.5;
                if (s[7] == 'v') dx9.set_vs_const_b(static_cast<u32>(reg), &v, 1);
                else             dx9.set_ps_const_b(static_cast<u32>(reg), &v, 1);
            }
            return RValue(0.0);
        }
        if (s == "shader_vertex_uniform_4b" || s == "shader_pixel_uniform_4b") {
            const i32 reg = shim_i32(args, 0, -1);
            if (reg >= 0) {
                renderer.flush();
                const bool v[4] = {
                    shim_real(args, 1, 0.0) >= 0.5,
                    shim_real(args, 2, 0.0) >= 0.5,
                    shim_real(args, 3, 0.0) >= 0.5,
                    shim_real(args, 4, 0.0) >= 0.5,
                };
                if (s[7] == 'v') dx9.set_vs_const_b(static_cast<u32>(reg), v, 4);
                else             dx9.set_ps_const_b(static_cast<u32>(reg), v, 4);
            }
            return RValue(0.0);
        }
        if (s == "shader_vertex_uniform_matrix" || s == "shader_pixel_uniform_matrix") {
            const i32 reg = shim_i32(args, 0, -1);
            if (reg >= 0) {
                renderer.dx9_set_shader_matrix(
                    s[7] == 'v',
                    static_cast<u32>(reg),
                    shim_i32(args, 1, 0),
                    shim_i32(args, 2, 4));
            }
            return RValue(0.0);
        }
        if (s == "shader_vertex_uniform_f_buffer" || s == "shader_pixel_uniform_f_buffer") {
            const i32 reg = shim_i32(args, 0, -1);
            const u32 addr = static_cast<u32>(shim_real(args, 1, 0.0));
            const i32 size = shim_i32(args, 2, 0);
            if (reg >= 0 && buffers_ && size >= 16) {
                if (auto* buf = buffers_->find_by_address(addr)) {
                    const usize avail = std::min(static_cast<usize>(size), buf->data.size());
                    const u32 count = static_cast<u32>(avail / 16u);
                    if (count > 0) {
                        renderer.flush();
                        std::vector<f32> tmp(static_cast<usize>(count) * 4u);
                        std::memcpy(tmp.data(), buf->data.data(), static_cast<usize>(count) * 16u);
                        if (s[7] == 'v') dx9.set_vs_const_f(static_cast<u32>(reg), tmp.data(), count);
                        else             dx9.set_ps_const_f(static_cast<u32>(reg), tmp.data(), count);
                    }
                }
            }
            return RValue(0.0);
        }
        if (s == "shader_vertex_uniform_i_buffer" || s == "shader_pixel_uniform_i_buffer") {
            const i32 reg = shim_i32(args, 0, -1);
            const u32 addr = static_cast<u32>(shim_real(args, 1, 0.0));
            const i32 size = shim_i32(args, 2, 0);
            if (reg >= 0 && buffers_ && size >= 16) {
                if (auto* buf = buffers_->find_by_address(addr)) {
                    const usize avail = std::min(static_cast<usize>(size), buf->data.size());
                    const u32 count = static_cast<u32>(avail / 16u);
                    if (count > 0) {
                        renderer.flush();
                        std::vector<i32> tmp(static_cast<usize>(count) * 4u);
                        std::memcpy(tmp.data(), buf->data.data(), static_cast<usize>(count) * 16u);
                        if (s[7] == 'v') dx9.set_vs_const_i(static_cast<u32>(reg), tmp.data(), count);
                        else             dx9.set_ps_const_i(static_cast<u32>(reg), tmp.data(), count);
                    }
                }
            }
            return RValue(0.0);
        }
        if (s == "shader_vertex_uniform_b_buffer" || s == "shader_pixel_uniform_b_buffer") {
            const i32 reg = shim_i32(args, 0, -1);
            const u32 addr = static_cast<u32>(shim_real(args, 1, 0.0));
            const i32 size = shim_i32(args, 2, 0);
            if (reg >= 0 && buffers_ && size >= 16) {
                if (auto* buf = buffers_->find_by_address(addr)) {
                    const usize avail = std::min(static_cast<usize>(size), buf->data.size());
                    const u32 count = std::min<u32>(16u, static_cast<u32>(avail / 16u));
                    if (count > 0) {
                        renderer.flush();
                        std::array<bool, 16> tmp{};
                        for (u32 i = 0; i < count; ++i) {
                            u32 raw = 0;
                            std::memcpy(&raw, buf->data.data() + static_cast<usize>(i) * 4u, 4u);
                            tmp[i] = (raw != 0u);
                        }
                        if (s[7] == 'v') dx9.set_vs_const_b(static_cast<u32>(reg), tmp.data(), count);
                        else             dx9.set_ps_const_b(static_cast<u32>(reg), tmp.data(), count);
                    }
                }
            }
            return RValue(0.0);
        }

        // -------------------------------------------------------------------
        // Custom Vertex Formats, Vertex Buffers, Index Buffers & Instancing
        // -------------------------------------------------------------------
        if (s == "vertex_format_begin") {
            dx9.vertex_format_begin();
            return RValue(0.0);
        }
        if (s == "vertex_format_add_custom") {
            dx9.vertex_format_add_custom(
                shim_i32(args, 0, 0),
                shim_i32(args, 1, 0),
                shim_i32(args, 2, 0));
            return RValue(0.0);
        }
        if (s == "vertex_format_end") {
            return RValue(static_cast<f64>(dx9.vertex_format_end()));
        }
        if (s == "vertex_format_get_size") {
            return RValue(static_cast<f64>(dx9.vertex_format_get_size(
                shim_i32(args, 0, -1),
                shim_i32(args, 1, 0))));
        }
        if (s == "vertex_format_delete") {
            return RValue(dx9.vertex_format_delete(shim_i32(args, 0, -1)) ? 0.0 : 1.0);
        }
        if (s == "vertex_create_buffer_from_buffer") {
            const u32 addr = static_cast<u32>(shim_real(args, 0, 0.0));
            const usize len = static_cast<usize>(std::max(0, shim_i32(args, 1, 0)));
            const u32 stride = static_cast<u32>(std::max(1, shim_i32(args, 2, 1)));
            std::vector<u8> bytes(len, 0);
            if (buffers_ && len > 0) {
                if (auto* buf = buffers_->find_by_address(addr)) {
                    const usize n = std::min(len, buf->data.size());
                    std::memcpy(bytes.data(), buf->data.data(), n);
                }
            }
            return RValue(static_cast<f64>(dx9.create_vertex_buffer(bytes, stride)));
        }
        if (s == "vertex_buffer_get_size") {
            return RValue(static_cast<f64>(dx9.vertex_buffer_get_size(shim_i32(args, 0, -1))));
        }
        if (s == "vertex_bind_buffer" || s == "vertex_buffer_bind") {
            return RValue(dx9.vertex_bind_buffer(shim_i32(args, 0, -1), shim_i32(args, 1, 0)) ? 0.0 : 1.0);
        }
        if (s == "vertex_delete_buffer" || s == "vertex_buffer_delete") {
            renderer.flush();
            return RValue(dx9.vertex_delete_buffer(shim_i32(args, 0, -1)) ? 0.0 : 1.0);
        }
        if (s == "vertex_set_instance_count") {
            renderer.flush();
            dx9.set_instance_count(static_cast<u32>(std::max(0, shim_i32(args, 0, 0))));
            return RValue(0.0);
        }
        if (s == "vertex_set_instances_per_vertex") {
            renderer.flush();
            dx9.set_instances_per_vertex(
                shim_i32(args, 0, 0),
                static_cast<u32>(std::max(0, shim_i32(args, 1, 0))));
            return RValue(0.0);
        }
        if (s == "vertex_instance_reset") {
            renderer.flush();
            dx9.reset_instancing();
            return RValue(0.0);
        }
        if (s == "index_create_buffer_from_buffer") {
            const u32 addr = static_cast<u32>(shim_real(args, 0, 0.0));
            const usize len = static_cast<usize>(std::max(0, shim_i32(args, 1, 0)));
            const i32 fmt = shim_i32(args, 2, 101);
            std::vector<u8> bytes(len, 0);
            if (buffers_ && len > 0) {
                if (auto* buf = buffers_->find_by_address(addr)) {
                    const usize n = std::min(len, buf->data.size());
                    std::memcpy(bytes.data(), buf->data.data(), n);
                }
            }
            return RValue(static_cast<f64>(dx9.create_index_buffer(bytes, fmt)));
        }
        if (s == "index_buffer_get_size") {
            return RValue(static_cast<f64>(dx9.index_buffer_get_size(shim_i32(args, 0, -1))));
        }
        if (s == "index_buffer_get_format") {
            return RValue(static_cast<f64>(dx9.index_buffer_get_format(shim_i32(args, 0, -1))));
        }
        if (s == "index_set_buffer" || s == "index_buffer_set") {
            return RValue(dx9.index_set_buffer(shim_i32(args, 0, -1)) ? 0.0 : 1.0);
        }
        if (s == "index_delete_buffer" || s == "index_buffer_delete") {
            renderer.flush();
            return RValue(dx9.index_delete_buffer(shim_i32(args, 0, -1)) ? 0.0 : 1.0);
        }
        if (s == "vertex_draw_buffer") {
            const i32 vbuf_id = shim_i32(args, 0, -1);
            const i32 vfmt_id = shim_i32(args, 1, -1);
            const i32 prim_type = shim_i32(args, 2, 4);
            const i32 tid = shim_i32(args, 3, -1);
            const i32 vert_count = shim_i32(args, 4, 0);
            const bool indexed = shim_real(args, 5, 0.0) >= 0.5;
            const TextureHandle tex = (tid > 0) ? TextureHandle{static_cast<u32>(tid)} : TextureHandle{};
            const bool ok = renderer.dx9_draw_vertex_buffer(vbuf_id, vfmt_id, prim_type, tex, vert_count, indexed);
            return RValue(ok ? 0.0 : 1.0);
        }

        GM_LOG_WARN("ext", "gm82dx: unhandled export '%.*s' (returning 0)",
                    static_cast<int>(fn.size()), fn.data());
        return RValue(0.0);
    }

private:
    std::shared_ptr<GmBufferStore> buffers_;
    bool has_started_ = false;
    f32 vx_ = 0.0f;
    f32 vy_ = 0.0f;
    f32 vz_ = 0.0f;
};

} // namespace

std::unique_ptr<IDllShim> create_gm82dx_shim(std::shared_ptr<GmBufferStore> buffers) {
    return std::make_unique<Gm82DxShim>(std::move(buffers));
}

} // namespace gm
