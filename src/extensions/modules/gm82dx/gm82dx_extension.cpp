#include "extensions/core/extension.h"
#include "extensions/shared/buffer_store.h"

#include "base/log.h"
#include "graphics/dx9_shader_transpiler.h"
#include "graphics/gl_graphics_device.h"
#include "graphics/renderer2d.h"
#include "platform/clock.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
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

struct CompoundTransformSpec {
    u8 rot_count = 0;
    char axes[3] = {'x', 'x', 'x'};
    bool has_scale = false;
};

RValue d3dx_preload_stub(void*, const CallContext&) {
    return RValue(1.0);
}

RValue invoke_compound_transform(void* instance, const CallContext& ctx) {
    const auto& spec = *static_cast<const CompoundTransformSpec*>(instance);
    auto& renderer = ctx.host.ext_renderer();
    usize arg_idx = 0;
    for (u8 i = 0; i < spec.rot_count; ++i) {
        const f32 deg = static_cast<f32>(ctx.real(arg_idx++, 0.0));
        if (spec.axes[i] == 'x') renderer.d3d_transform_add_rotation_x(deg);
        else if (spec.axes[i] == 'y') renderer.d3d_transform_add_rotation_y(deg);
        else renderer.d3d_transform_add_rotation_z(deg);
    }
    if (spec.has_scale) {
        const f32 sx = static_cast<f32>(ctx.real(arg_idx++, 1.0));
        const f32 sy = static_cast<f32>(ctx.real(arg_idx++, 1.0));
        const f32 sz = static_cast<f32>(ctx.real(arg_idx++, 1.0));
        renderer.d3d_transform_add_scaling(sx, sy, sz);
    }
    const f32 tx = static_cast<f32>(ctx.real(arg_idx++, 0.0));
    const f32 ty = static_cast<f32>(ctx.real(arg_idx++, 0.0));
    const f32 tz = static_cast<f32>(ctx.real(arg_idx++, 0.0));
    renderer.d3d_transform_add_translation(tx, ty, tz);
    return RValue(0.0);
}

class Gm82DxExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "gm82dx"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_aliases({"gm82dx9", "gm82dx8", "d3dx9_43"});
        reg.add_prefix_aliases({"__gm82dx9_", "__gm82dx8_"});
        buffers_ = &reg.services().get_or_create<GmBufferStore>();

        // -------------------------------------------------------------------
        // Lifecycle & checks
        // -------------------------------------------------------------------
        reg.bind_const("dllcheck", 820.0);
        reg.bind_const({"dll_init", "init", "not_xp"}, 1.0);
        reg.bind_stub({"testfunc", "setalphabuffer", "setpointscale", "setzscale", "getvendor"}, 0.0);
        reg.bind_const("getvideomem", 1073741824.0);
        reg.bind_const({"getmaxwidth", "getmaxheight"}, 16384.0);

        reg.bind("checkstart", [this]() {
            if (has_started_) return 1.0;
            has_started_ = true;
            return 0.0;
        });
        reg.bind("abort", [](IExtensionHost& host) {
            GM_LOG_ERROR("ext", "gm82dx: conflicting DX8 and DX9 extensions detected");
            host.ext_game_end();
        });
        reg.bind("time_now", []() {
            return static_cast<f64>(Clock::now_us()) / 1000.0;
        });

        // -------------------------------------------------------------------
        // Color packing helpers
        // -------------------------------------------------------------------
        reg.bind("argb_get_color", [](f64 raw) {
            const u32 col = static_cast<u32>(raw) & 0x00FFFFFFu;
            const u32 bgr = ((col & 0xFF0000u) >> 16) | (col & 0x00FF00u) | ((col & 0x0000FFu) << 16);
            return static_cast<f64>(bgr);
        });
        reg.bind("argb_get_alpha", [](f64 raw) {
            const u32 col = static_cast<u32>(raw);
            return static_cast<f64>((col >> 24) & 0xFFu) / 255.0;
        });
        reg.bind({"argb_make_color", "color_to_d3dcolor"}, [](const CallContext& ctx) {
            const u32 col = static_cast<u32>(ctx.real(0, 0.0)) & 0x00FFFFFFu;
            const u32 alpha = static_cast<u32>(std::clamp(ctx.real(1, 1.0), 0.0, 1.0) * 255.0);
            const u32 argb = (alpha << 24) |
                             ((col & 0x0000FFu) << 16) |
                             (col & 0x00FF00u) |
                             ((col & 0xFF0000u) >> 16);
            return RValue(static_cast<f64>(argb));
        });

        // -------------------------------------------------------------------
        // Surfaces & buffer/grid pixel transfer
        // -------------------------------------------------------------------
        reg.bind("surface_get_count", [](IExtensionHost& host) {
            return static_cast<f64>(host.ext_renderer().surface_count());
        });
        reg.bind("surface_ptr", [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            const i32 id = ctx.i32_arg(0, -1);
            if (!renderer.surface_exists(id)) return RValue(-4.0);
            return RValue(static_cast<f64>(renderer.surface_unique_token(id)));
        });
        reg.bind("surface_set_depth", [](const CallContext& ctx) {
            return RValue(ctx.host.ext_renderer().surface_set_depth(ctx.i32_arg(0, -1)) ? 0.0 : -1.0);
        });
        reg.bind("surface_set_target_ext", [](const CallContext& ctx) {
            return RValue(ctx.host.ext_renderer().surface_set_target_ext(
                ctx.i32_arg(0, 1), ctx.i32_arg(1, -1)) ? 1.0 : 0.0);
        });
        reg.bind("buffer_to_surface", [this](const CallContext& ctx) {
            const u32 addr = static_cast<u32>(ctx.real(0, 0.0));
            const i32 surf_id = ctx.i32_arg(1, -1);
            const i32 w = ctx.i32_arg(2, 0);
            const i32 h = ctx.i32_arg(3, 0);
            if (!buffers_ || w <= 0 || h <= 0) return RValue(-1.0);
            auto* buf = buffers_->find_by_address(addr);
            if (!buf) return RValue(-1.0);
            const usize need = static_cast<usize>(w) * static_cast<usize>(h) * 4;
            if (buf->data.size() < need) return RValue(-1.0);
            const bool ok = ctx.host.ext_renderer().surface_write_pixels_bgra(
                surf_id, w, h, std::span<const u8>(buf->data.data(), need));
            return RValue(ok ? 0.0 : -1.0);
        });
        reg.bind("surface_to_buffer", [this](const CallContext& ctx) {
            const u32 addr = static_cast<u32>(ctx.real(0, 0.0));
            const i32 surf_id = ctx.i32_arg(1, -1);
            const i32 w = ctx.i32_arg(2, 0);
            const i32 h = ctx.i32_arg(3, 0);
            if (!buffers_ || w <= 0 || h <= 0) return RValue(-1.0);
            auto* buf = buffers_->find_by_address(addr);
            if (!buf) return RValue(-1.0);
            std::vector<u8> bgra;
            if (!ctx.host.ext_renderer().surface_read_pixels_bgra(surf_id, 0, 0, w, h, bgra)) {
                return RValue(-1.0);
            }
            const usize need = static_cast<usize>(w) * static_cast<usize>(h) * 4;
            if (buf->data.size() < need) buf->data.resize(need, 0);
            std::memcpy(buf->data.data(), bgra.data(), need);
            return RValue(0.0);
        });
        reg.bind("surface_to_grid", [](const CallContext& ctx) {
            const i32 surf_id = ctx.i32_arg(0, -1);
            const i32 x = ctx.i32_arg(1, 0);
            const i32 y = ctx.i32_arg(2, 0);
            const i32 w = ctx.i32_arg(3, 0);
            const i32 h = ctx.i32_arg(4, 0);
            if (w <= 0 || h <= 0) return RValue(std::string{});
            std::vector<u8> bgra;
            if (!ctx.host.ext_renderer().surface_read_pixels_bgra(surf_id, x, y, w, h, bgra)) {
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
                    write_f64_hex(&out[(16 + idx * 16) * 2], static_cast<f64>(packed));
                    ++idx;
                }
            }
            return RValue(std::move(out));
        });

        // -------------------------------------------------------------------
        // Render state & device control
        // -------------------------------------------------------------------
        reg.bind("set_color_mask", [](const CallContext& ctx) {
            ctx.host.ext_renderer().set_color_write_enable(
                ctx.real(0, 1.0) >= 0.5,
                ctx.real(1, 1.0) >= 0.5,
                ctx.real(2, 1.0) >= 0.5,
                ctx.real(3, 1.0) >= 0.5);
            return RValue(0.0);
        });
        reg.bind("set_fill_mode", [](const CallContext& ctx) {
            const f64 mode = ctx.real(0, 2.0);
            const i32 pm = (mode < 0.5) ? 0 : (mode < 1.5 ? 1 : 2);
            ctx.host.ext_renderer().flush();
            ctx.host.ext_gfx().set_polygon_mode(pm);
            return RValue(0.0);
        });
        reg.bind("set_cull_mode", [](const CallContext& ctx) {
            const f64 mode = ctx.real(0, 0.0);
            const i32 cm = (mode < 0.5) ? 0 : (mode < 1.5 ? 1 : 2);
            ctx.host.ext_renderer().set_cull_mode_ext(cm);
            return RValue(0.0);
        });
        reg.bind("set_zcomp_mode", [](const CallContext& ctx) {
            ctx.host.ext_renderer().set_zcomp_mode(ctx.i32_arg(0, 4));
            return RValue(0.0);
        });
        reg.bind("set_zbias", [](const CallContext& ctx) {
            ctx.host.ext_renderer().flush();
            ctx.host.ext_gfx().set_depth_bias(static_cast<f32>(ctx.real(0, 0.0)));
            return RValue(0.0);
        });
        reg.bind("set_alphatest", [](const CallContext& ctx) {
            const bool en = ctx.real(0, 0.0) > 0.5;
            const u8 ref = static_cast<u8>(std::clamp(ctx.i32_arg(1, 0), 0, 255));
            const i32 cmp = ctx.i32_arg(2, 5);
            ctx.host.ext_renderer().set_alpha_test_ext(en, ref, cmp);
            return RValue(0.0);
        });
        reg.bind("cleardepth", [](IExtensionHost& host) {
            host.ext_renderer().clear_depth(1.0f);
            return 1.0;
        });
        reg.bind("setfullscreen", [](const CallContext& ctx) {
            ctx.host.ext_window().set_fullscreen(ctx.i32_arg(0, 0) != 0);
            return RValue(1.0);
        });
        reg.bind("resize_backbuffer", [](const CallContext& ctx) {
            const i32 w = static_cast<i32>(std::llround(ctx.real(0, 800.0)));
            const i32 h = static_cast<i32>(std::llround(ctx.real(1, 608.0)));
            ctx.host.ext_resize_backbuffer(w, h);
            return RValue(0.0);
        });
        reg.bind("setsepalphablend", [](const CallContext& ctx) {
            ctx.host.ext_renderer().set_separate_alpha_blend(
                ctx.real(0, 0.0) > 0.5,
                ctx.i32_arg(1, 5),
                ctx.i32_arg(2, 6));
            return RValue(0.0);
        });
        reg.bind("setviewport", [](const CallContext& ctx) {
            ctx.host.ext_renderer().set_viewport_ext(
                ctx.i32_arg(0, 0),
                ctx.i32_arg(1, 0),
                ctx.i32_arg(2, 1),
                ctx.i32_arg(3, 1));
            return RValue(0.0);
        });
        reg.bind("getviewportx", [](IExtensionHost& host) { return static_cast<f64>(host.ext_renderer().active_viewport().x); });
        reg.bind("getviewporty", [](IExtensionHost& host) { return static_cast<f64>(host.ext_renderer().active_viewport().y); });
        reg.bind("getviewportw", [](IExtensionHost& host) { return static_cast<f64>(host.ext_renderer().active_viewport().width); });
        reg.bind("getviewporth", [](IExtensionHost& host) { return static_cast<f64>(host.ext_renderer().active_viewport().height); });

        // -------------------------------------------------------------------
        // Lighting & Fog extensions and queries
        // -------------------------------------------------------------------
        reg.bind("d3d_light_enabled", [](const CallContext& ctx) {
            return RValue(ctx.host.ext_renderer().d3d_light_is_enabled(ctx.i32_arg(0, 0)) ? 1.0 : 0.0);
        });
        reg.bind("d3d_get_lighting", [](IExtensionHost& host) {
            return host.ext_renderer().d3d_lighting();
        });
        reg.bind("d3d_light_get_ambient", [](IExtensionHost& host) {
            return static_cast<f64>(host.ext_renderer().d3d_light_ambient());
        });
        reg.bind("buffer_get_lights", [this](const CallContext& ctx) {
            const u32 addr = static_cast<u32>(ctx.real(0, 0.0));
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
            auto& renderer = ctx.host.ext_renderer();
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
        });
        reg.bind("set_light", [](const CallContext& ctx) {
            ctx.host.ext_renderer().d3d_light_define_ext(
                ctx.i32_arg(0, 0),
                ctx.i32_arg(1, 3),
                static_cast<f32>(ctx.real(2, 0.0)),
                static_cast<f32>(ctx.real(3, 0.0)),
                static_cast<f32>(ctx.real(4, 0.0)),
                static_cast<f32>(ctx.real(5, 0.0)),
                static_cast<f32>(ctx.real(6, 0.0)),
                static_cast<f32>(ctx.real(7, 1.0)),
                static_cast<f32>(ctx.real(8, 1.0)),
                static_cast<f32>(ctx.real(9, 0.0)),
                static_cast<f32>(ctx.real(10, 0.0)),
                static_cast<u32>(std::llround(ctx.real(11, 0.0))),
                static_cast<f32>(ctx.real(12, 1.0)),
                static_cast<u32>(std::llround(ctx.real(13, 0.0))),
                static_cast<f32>(ctx.real(14, 10.0)));
            return RValue(0.0);
        });
        reg.bind("setrangefog", [](const CallContext& ctx) {
            ctx.host.ext_renderer().d3d_set_fog_ext(
                ctx.i32_arg(0, 0),
                static_cast<u32>(std::llround(ctx.real(1, 0.0))),
                static_cast<f32>(ctx.real(2, 0.0)),
                static_cast<f32>(ctx.real(3, 1.0)));
            return RValue(0.0);
        });
        reg.bind("d3d_get_fog_enabled", [](IExtensionHost& host) { return host.ext_renderer().d3d_fog().mode > 0; });
        reg.bind("d3d_get_fog_color", [](IExtensionHost& host) { return static_cast<f64>(host.ext_renderer().d3d_fog().color_bgr); });
        reg.bind("d3d_get_fog_start", [](IExtensionHost& host) { return static_cast<f64>(host.ext_renderer().d3d_fog().start); });
        reg.bind("d3d_get_fog_end", [](IExtensionHost& host) { return static_cast<f64>(host.ext_renderer().d3d_fog().end); });

        // -------------------------------------------------------------------
        // Vertex projection & matrix queries/transforms
        // -------------------------------------------------------------------
        reg.bind("transformvertex", [this](const CallContext& ctx) {
            ctx.host.ext_renderer().d3d_transform_vertex(
                static_cast<f32>(ctx.real(0, 0.0)),
                static_cast<f32>(ctx.real(1, 0.0)),
                static_cast<f32>(ctx.real(2, 0.0)),
                vx_, vy_, vz_);
            return RValue(static_cast<f64>(vx_));
        });
        reg.bind("geteyepos", [this](IExtensionHost& host) {
            host.ext_renderer().d3d_get_eye_pos(vx_, vy_, vz_);
            return static_cast<f64>(vx_);
        });
        reg.bind("projectvertex", [this](const CallContext& ctx) {
            ctx.host.ext_renderer().d3d_project_vertex(
                static_cast<f32>(ctx.real(0, 0.0)),
                static_cast<f32>(ctx.real(1, 0.0)),
                static_cast<f32>(ctx.real(2, 0.0)),
                vx_, vy_, vz_);
            return RValue(static_cast<f64>(vx_));
        });
        reg.bind("getvertexy", [this]() { return static_cast<f64>(vy_); });
        reg.bind("getvertexz", [this]() { return static_cast<f64>(vz_); });

        reg.bind("d3d_transform_set_matrix", [](const CallContext& ctx) {
            f32 m[16];
            for (usize i = 0; i < 16; ++i) {
                m[i] = static_cast<f32>(ctx.real(i, (i % 5 == 0) ? 1.0 : 0.0));
            }
            ctx.host.ext_renderer().d3d_transform_set_matrix(m);
            return RValue(0.0);
        });
        reg.bind("set_matrix_from_buffer", [this](const CallContext& ctx) {
            const u32 addr = static_cast<u32>(ctx.real(0, 0.0));
            if (buffers_) {
                if (auto* buf = buffers_->find_by_address(addr); buf && buf->data.size() >= 64) {
                    f32 m[16];
                    std::memcpy(m, buf->data.data(), 64);
                    ctx.host.ext_renderer().d3d_transform_set_matrix(m);
                }
            }
            return RValue(0.0);
        });
        reg.bind("d3d_transform_add_stack_top", [](IExtensionHost& host) {
            host.ext_renderer().d3d_transform_add_stack_top();
        });
        reg.bind("d3d_transform_get_determinant", [](IExtensionHost& host) {
            return static_cast<f64>(host.ext_renderer().d3d_world_determinant());
        });
        reg.bind("d3d_projection_get_determinant", [](IExtensionHost& host) {
            return static_cast<f64>(host.ext_renderer().d3d_projection_determinant());
        });

        // -------------------------------------------------------------------
        // Texture & Multi-stage Sampler state (PS stages 0..7, VS stages 0..3)
        // -------------------------------------------------------------------
        reg.bind("texture_get_interpolation", [](IExtensionHost& host) {
            auto& renderer = host.ext_renderer();
            const auto& st0 = renderer.dx9().ps_sampler(0);
            return st0.has_explicit_filter ? st0.linear_filter : renderer.interpolation();
        });
        reg.bind("texture_stage_set", [](const CallContext& ctx) {
            const i32 stage = ctx.i32_arg(0, 0);
            if (stage >= 0 && stage <= 7) {
                const i32 tid = ctx.i32_arg(1, -1);
                auto& renderer = ctx.host.ext_renderer();
                renderer.flush();
                renderer.dx9().set_ps_sampler_tex(stage, tid > 0 ? TextureHandle{static_cast<u32>(tid)} : TextureHandle{});
            }
            return RValue(0.0);
        });
        reg.bind("texture_stage_vertex_set", [](const CallContext& ctx) {
            const i32 stage = ctx.i32_arg(0, 0);
            if (stage >= 0 && stage <= 3) {
                const i32 tid = ctx.i32_arg(1, -1);
                auto& renderer = ctx.host.ext_renderer();
                renderer.flush();
                renderer.dx9().set_vs_sampler_tex(stage, tid > 0 ? TextureHandle{static_cast<u32>(tid)} : TextureHandle{});
            }
            return RValue(0.0);
        });
        reg.bind("texture_set_stage_interpolation", [](const CallContext& ctx) {
            const i32 stage = ctx.i32_arg(0, 0);
            if (stage >= 0 && stage <= 7) {
                auto& renderer = ctx.host.ext_renderer();
                renderer.flush();
                renderer.dx9().set_ps_sampler_filter(stage, ctx.real(1, 0.0) >= 0.5);
            }
            return RValue(0.0);
        });
        reg.bind("texture_set_stage_vertex_interpolation", [](const CallContext& ctx) {
            const i32 stage = ctx.i32_arg(0, 0);
            if (stage >= 0 && stage <= 3) {
                auto& renderer = ctx.host.ext_renderer();
                renderer.flush();
                renderer.dx9().set_vs_sampler_filter(stage, ctx.real(1, 0.0) >= 0.5);
            }
            return RValue(0.0);
        });
        reg.bind("texture_set_stage_repeat", [](const CallContext& ctx) {
            const i32 stage = ctx.i32_arg(0, 0);
            if (stage >= 0 && stage <= 7) {
                i32 hr = ctx.i32_arg(1, 0);
                i32 vr = ctx.i32_arg(2, 0);
                if (hr == 0) hr = 3; // false/tr_clamp -> D3DTADDRESS_CLAMP
                if (vr == 0) vr = 3;
                const u32 border = static_cast<u32>(std::llround(ctx.real(3, 0.0))) & 0x00FFFFFFu;
                auto& renderer = ctx.host.ext_renderer();
                renderer.flush();
                renderer.dx9().set_ps_sampler_wrap(stage, hr, vr, border);
            }
            return RValue(0.0);
        });
        reg.bind("texture_set_stage_vertex_repeat", [](const CallContext& ctx) {
            const i32 stage = ctx.i32_arg(0, 0);
            if (stage >= 0 && stage <= 3) {
                i32 hr = ctx.i32_arg(1, 0);
                i32 vr = ctx.i32_arg(2, 0);
                if (hr == 0) hr = 3;
                if (vr == 0) vr = 3;
                const u32 border = static_cast<u32>(std::llround(ctx.real(3, 0.0))) & 0x00FFFFFFu;
                auto& renderer = ctx.host.ext_renderer();
                renderer.flush();
                renderer.dx9().set_vs_sampler_wrap(stage, hr, vr, border);
            }
            return RValue(0.0);
        });
        reg.bind("texture_set_mode", [](const CallContext& ctx) {
            ctx.host.ext_renderer().set_texcoord_mode(ctx.i32_arg(0));
            return RValue(0.0);
        });

        // -------------------------------------------------------------------
        // DX9 Programmable Shaders, CTAB Reflection & Uniform Register Banks
        // -------------------------------------------------------------------
        reg.bind("shader_vertex_create_file", [](const CallContext& ctx) {
            auto bytes = ctx.host.ext_vfs().read_all_bytes(ctx.str(0));
            if (!bytes || !dx9_shader_bounds_check(bytes.value())) return RValue(-2.0);
            return RValue(static_cast<f64>(ctx.host.ext_renderer().dx9().create_vertex_shader(bytes.value())));
        });
        reg.bind("shader_pixel_create_file", [](const CallContext& ctx) {
            auto bytes = ctx.host.ext_vfs().read_all_bytes(ctx.str(0));
            if (!bytes || !dx9_shader_bounds_check(bytes.value())) return RValue(-2.0);
            return RValue(static_cast<f64>(ctx.host.ext_renderer().dx9().create_pixel_shader(bytes.value())));
        });
        reg.bind<&Gm82DxExtension::shader_create_buffer<true>>(this, "shader_vertex_create_buffer");
        reg.bind<&Gm82DxExtension::shader_create_buffer<false>>(this, "shader_pixel_create_buffer");

        reg.bind("shader_vertex_destroy", [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            return RValue(renderer.dx9().destroy_vertex_shader(ctx.i32_arg(0, -1)) ? 0.0 : 1.0);
        });
        reg.bind("shader_pixel_destroy", [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            return RValue(renderer.dx9().destroy_pixel_shader(ctx.i32_arg(0, -1)) ? 0.0 : 1.0);
        });
        reg.bind("shader_vertex_set", [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            return RValue(renderer.dx9().set_vertex_shader(ctx.i32_arg(0, -1)) ? 0.0 : 1.0);
        });
        reg.bind("shader_pixel_set", [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            return RValue(renderer.dx9().set_pixel_shader(ctx.i32_arg(0, -1)) ? 0.0 : 1.0);
        });
        reg.bind("shader_set", [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            renderer.dx9().set_pixel_shader(ctx.i32_arg(1, -1));
            renderer.dx9().set_vertex_shader(ctx.i32_arg(0, -1));
            return RValue(0.0);
        });
        reg.bind("shader_vertex_reset", [](IExtensionHost& host) {
            auto& renderer = host.ext_renderer();
            renderer.flush();
            renderer.dx9().reset_vertex_shader();
        });
        reg.bind("shader_pixel_reset", [](IExtensionHost& host) {
            auto& renderer = host.ext_renderer();
            renderer.flush();
            renderer.dx9().reset_pixel_shader();
        });
        reg.bind("shader_reset", [](IExtensionHost& host) {
            auto& renderer = host.ext_renderer();
            renderer.flush();
            renderer.dx9().reset_pixel_shader();
            renderer.dx9().reset_vertex_shader();
        });
        reg.bind("get_vconst", [](IExtensionHost& host, std::string_view name) {
            return static_cast<f64>(host.ext_renderer().dx9().get_vconst(name));
        });
        reg.bind("get_vconst_size", [](IExtensionHost& host, std::string_view name) {
            return static_cast<f64>(host.ext_renderer().dx9().get_vconst_size(name));
        });
        reg.bind("get_pconst", [](IExtensionHost& host, std::string_view name) {
            return static_cast<f64>(host.ext_renderer().dx9().get_pconst(name));
        });
        reg.bind("get_pconst_size", [](IExtensionHost& host, std::string_view name) {
            return static_cast<f64>(host.ext_renderer().dx9().get_pconst_size(name));
        });

        reg.bind<&Gm82DxExtension::shader_uniform_4f<true>>(this, "shader_vertex_uniform_4f");
        reg.bind<&Gm82DxExtension::shader_uniform_4f<false>>(this, "shader_pixel_uniform_4f");
        reg.bind<&Gm82DxExtension::shader_uniform_4i<true>>(this, "shader_vertex_uniform_4i");
        reg.bind<&Gm82DxExtension::shader_uniform_4i<false>>(this, "shader_pixel_uniform_4i");
        reg.bind<&Gm82DxExtension::shader_uniform_b<true>>(this, "shader_vertex_uniform_b");
        reg.bind<&Gm82DxExtension::shader_uniform_b<false>>(this, "shader_pixel_uniform_b");
        reg.bind<&Gm82DxExtension::shader_uniform_4b<true>>(this, "shader_vertex_uniform_4b");
        reg.bind<&Gm82DxExtension::shader_uniform_4b<false>>(this, "shader_pixel_uniform_4b");
        reg.bind<&Gm82DxExtension::shader_uniform_matrix<true>>(this, "shader_vertex_uniform_matrix");
        reg.bind<&Gm82DxExtension::shader_uniform_matrix<false>>(this, "shader_pixel_uniform_matrix");
        reg.bind<&Gm82DxExtension::shader_uniform_f_buffer<true>>(this, "shader_vertex_uniform_f_buffer");
        reg.bind<&Gm82DxExtension::shader_uniform_f_buffer<false>>(this, "shader_pixel_uniform_f_buffer");
        reg.bind<&Gm82DxExtension::shader_uniform_i_buffer<true>>(this, "shader_vertex_uniform_i_buffer");
        reg.bind<&Gm82DxExtension::shader_uniform_i_buffer<false>>(this, "shader_pixel_uniform_i_buffer");
        reg.bind<&Gm82DxExtension::shader_uniform_b_buffer<true>>(this, "shader_vertex_uniform_b_buffer");
        reg.bind<&Gm82DxExtension::shader_uniform_b_buffer<false>>(this, "shader_pixel_uniform_b_buffer");

        // -------------------------------------------------------------------
        // Custom Vertex Formats, Vertex Buffers, Index Buffers & Instancing
        // -------------------------------------------------------------------
        reg.bind("vertex_format_begin", [](IExtensionHost& host) {
            host.ext_renderer().dx9().vertex_format_begin();
        });
        reg.bind("vertex_format_add_custom", [](const CallContext& ctx) {
            ctx.host.ext_renderer().dx9().vertex_format_add_custom(
                ctx.i32_arg(0, 0), ctx.i32_arg(1, 0), ctx.i32_arg(2, 0));
            return RValue(0.0);
        });
        reg.bind("vertex_format_end", [](IExtensionHost& host) {
            return static_cast<f64>(host.ext_renderer().dx9().vertex_format_end());
        });
        reg.bind("vertex_format_get_size", [](const CallContext& ctx) {
            return RValue(static_cast<f64>(ctx.host.ext_renderer().dx9().vertex_format_get_size(
                ctx.i32_arg(0, -1), ctx.i32_arg(1, 0))));
        });
        reg.bind("vertex_format_delete", [](const CallContext& ctx) {
            return RValue(ctx.host.ext_renderer().dx9().vertex_format_delete(ctx.i32_arg(0, -1)) ? 0.0 : 1.0);
        });
        reg.bind("vertex_create_buffer_from_buffer", [this](const CallContext& ctx) {
            const u32 addr = static_cast<u32>(ctx.real(0, 0.0));
            const usize len = static_cast<usize>(std::max(0, ctx.i32_arg(1, 0)));
            const u32 stride = static_cast<u32>(std::max(1, ctx.i32_arg(2, 1)));
            std::vector<u8> bytes(len, 0);
            if (buffers_ && len > 0) {
                if (auto* buf = buffers_->find_by_address(addr)) {
                    const usize n = std::min(len, buf->data.size());
                    std::memcpy(bytes.data(), buf->data.data(), n);
                }
            }
            return RValue(static_cast<f64>(ctx.host.ext_renderer().dx9().create_vertex_buffer(bytes, stride)));
        });
        reg.bind("vertex_buffer_get_size", [](const CallContext& ctx) {
            return RValue(static_cast<f64>(ctx.host.ext_renderer().dx9().vertex_buffer_get_size(ctx.i32_arg(0, -1))));
        });
        reg.bind({"vertex_bind_buffer", "vertex_buffer_bind"}, [](const CallContext& ctx) {
            return RValue(ctx.host.ext_renderer().dx9().vertex_bind_buffer(
                ctx.i32_arg(0, -1), ctx.i32_arg(1, 0)) ? 0.0 : 1.0);
        });
        reg.bind({"vertex_delete_buffer", "vertex_buffer_delete"}, [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            return RValue(renderer.dx9().vertex_delete_buffer(ctx.i32_arg(0, -1)) ? 0.0 : 1.0);
        });
        reg.bind("vertex_set_instance_count", [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            renderer.dx9().set_instance_count(static_cast<u32>(std::max(0, ctx.i32_arg(0, 0))));
            return RValue(0.0);
        });
        reg.bind("vertex_set_instances_per_vertex", [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            renderer.dx9().set_instances_per_vertex(
                ctx.i32_arg(0, 0),
                static_cast<u32>(std::max(0, ctx.i32_arg(1, 0))));
            return RValue(0.0);
        });
        reg.bind("vertex_instance_reset", [](IExtensionHost& host) {
            auto& renderer = host.ext_renderer();
            renderer.flush();
            renderer.dx9().reset_instancing();
        });
        reg.bind("index_create_buffer_from_buffer", [this](const CallContext& ctx) {
            const u32 addr = static_cast<u32>(ctx.real(0, 0.0));
            const usize len = static_cast<usize>(std::max(0, ctx.i32_arg(1, 0)));
            const i32 fmt = ctx.i32_arg(2, 101);
            std::vector<u8> bytes(len, 0);
            if (buffers_ && len > 0) {
                if (auto* buf = buffers_->find_by_address(addr)) {
                    const usize n = std::min(len, buf->data.size());
                    std::memcpy(bytes.data(), buf->data.data(), n);
                }
            }
            return RValue(static_cast<f64>(ctx.host.ext_renderer().dx9().create_index_buffer(bytes, fmt)));
        });
        reg.bind("index_buffer_get_size", [](const CallContext& ctx) {
            return RValue(static_cast<f64>(ctx.host.ext_renderer().dx9().index_buffer_get_size(ctx.i32_arg(0, -1))));
        });
        reg.bind("index_buffer_get_format", [](const CallContext& ctx) {
            return RValue(static_cast<f64>(ctx.host.ext_renderer().dx9().index_buffer_get_format(ctx.i32_arg(0, -1))));
        });
        reg.bind({"index_set_buffer", "index_buffer_set"}, [](const CallContext& ctx) {
            return RValue(ctx.host.ext_renderer().dx9().index_set_buffer(ctx.i32_arg(0, -1)) ? 0.0 : 1.0);
        });
        reg.bind({"index_delete_buffer", "index_buffer_delete"}, [](const CallContext& ctx) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            return RValue(renderer.dx9().index_delete_buffer(ctx.i32_arg(0, -1)) ? 0.0 : 1.0);
        });
        reg.bind("vertex_draw_buffer", [](const CallContext& ctx) {
            const i32 vbuf_id = ctx.i32_arg(0, -1);
            const i32 vfmt_id = ctx.i32_arg(1, -1);
            const i32 prim_type = ctx.i32_arg(2, 4);
            const i32 tid = ctx.i32_arg(3, -1);
            const i32 vert_count = ctx.i32_arg(4, 0);
            const bool indexed = ctx.real(5, 0.0) >= 0.5;
            const TextureHandle tex = (tid > 0) ? TextureHandle{static_cast<u32>(tid)} : TextureHandle{};
            const bool ok = ctx.host.ext_renderer().dx9_draw_vertex_buffer(
                vbuf_id, vfmt_id, prim_type, tex, vert_count, indexed);
            return RValue(ok ? 0.0 : 1.0);
        });
    }

    std::optional<ExportedFunction> resolve_dynamic(std::string_view fn) override {
        // D3DX9_43 preload functions (e.g. D3DXCheckVersion / D3DXGetErrorStringA).
        if (fn.starts_with("D3DX")) {
            return ExportedFunction{&d3dx_preload_stub, nullptr, 0};
        }

        // Compound transform exports: d3d_transform_[xyz]{1,3}s?t
        if (fn.starts_with("d3d_transform_") && fn.size() >= 16 && fn.size() <= 19) {
            const std::string_view suffix = fn.substr(14);
            if (!suffix.empty() && suffix.back() == 't') {
                CompoundTransformSpec spec{};
                usize pos = 0;
                while (pos < suffix.size() && pos < 3 &&
                       (suffix[pos] == 'x' || suffix[pos] == 'y' || suffix[pos] == 'z')) {
                    spec.axes[pos] = suffix[pos];
                    ++pos;
                }
                spec.rot_count = static_cast<u8>(pos);
                if (pos < suffix.size() && suffix[pos] == 's') {
                    spec.has_scale = true;
                    ++pos;
                }
                if (spec.rot_count >= 1 && spec.rot_count <= 3 && pos + 1 == suffix.size() && suffix[pos] == 't') {
                    transform_specs_.push_back(spec);
                    return ExportedFunction{&invoke_compound_transform, &transform_specs_.back(), 0};
                }
            }
        }

        return std::nullopt;
    }

    void on_unload(IExtensionHost&) override {
        has_started_ = false;
        vx_ = vy_ = vz_ = 0.0f;
    }

private:
    template <bool IsVertex>
    RValue shader_create_buffer(const CallContext& ctx) {
        const u32 addr = static_cast<u32>(ctx.real(0, 0.0));
        const i32 len = ctx.i32_arg(1, 0);
        if (!buffers_) return RValue(-1.0);
        auto* buf = buffers_->find_by_address(addr);
        if (!buf) return RValue(-1.0);
        if (len <= 0 || static_cast<usize>(len) > buf->data.size()) return RValue(-3.0);
        const std::span<const u8> bytes(buf->data.data(), static_cast<usize>(len));
        if (!dx9_shader_bounds_check(bytes)) return RValue(-3.0);
        auto& dx9 = ctx.host.ext_renderer().dx9();
        if constexpr (IsVertex) {
            return RValue(static_cast<f64>(dx9.create_vertex_shader(bytes)));
        } else {
            return RValue(static_cast<f64>(dx9.create_pixel_shader(bytes)));
        }
    }

    template <bool IsVertex>
    RValue shader_uniform_4f(const CallContext& ctx) {
        const i32 reg = ctx.i32_arg(0, -1);
        if (reg >= 0) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            const f32 v[4] = {
                static_cast<f32>(ctx.real(1, 0.0)),
                static_cast<f32>(ctx.real(2, 0.0)),
                static_cast<f32>(ctx.real(3, 0.0)),
                static_cast<f32>(ctx.real(4, 0.0)),
            };
            if constexpr (IsVertex) renderer.dx9().set_vs_const_f(static_cast<u32>(reg), v, 1);
            else                    renderer.dx9().set_ps_const_f(static_cast<u32>(reg), v, 1);
        }
        return RValue(0.0);
    }

    template <bool IsVertex>
    RValue shader_uniform_4i(const CallContext& ctx) {
        const i32 reg = ctx.i32_arg(0, -1);
        if (reg >= 0) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            const i32 v[4] = {
                ctx.i32_arg(1, 0),
                ctx.i32_arg(2, 0),
                ctx.i32_arg(3, 0),
                ctx.i32_arg(4, 0),
            };
            if constexpr (IsVertex) renderer.dx9().set_vs_const_i(static_cast<u32>(reg), v, 1);
            else                    renderer.dx9().set_ps_const_i(static_cast<u32>(reg), v, 1);
        }
        return RValue(0.0);
    }

    template <bool IsVertex>
    RValue shader_uniform_b(const CallContext& ctx) {
        const i32 reg = ctx.i32_arg(0, -1);
        if (reg >= 0) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            const bool v = ctx.real(1, 0.0) >= 0.5;
            if constexpr (IsVertex) renderer.dx9().set_vs_const_b(static_cast<u32>(reg), &v, 1);
            else                    renderer.dx9().set_ps_const_b(static_cast<u32>(reg), &v, 1);
        }
        return RValue(0.0);
    }

    template <bool IsVertex>
    RValue shader_uniform_4b(const CallContext& ctx) {
        const i32 reg = ctx.i32_arg(0, -1);
        if (reg >= 0) {
            auto& renderer = ctx.host.ext_renderer();
            renderer.flush();
            const bool v[4] = {
                ctx.real(1, 0.0) >= 0.5,
                ctx.real(2, 0.0) >= 0.5,
                ctx.real(3, 0.0) >= 0.5,
                ctx.real(4, 0.0) >= 0.5,
            };
            if constexpr (IsVertex) renderer.dx9().set_vs_const_b(static_cast<u32>(reg), v, 4);
            else                    renderer.dx9().set_ps_const_b(static_cast<u32>(reg), v, 4);
        }
        return RValue(0.0);
    }

    template <bool IsVertex>
    RValue shader_uniform_matrix(const CallContext& ctx) {
        const i32 reg = ctx.i32_arg(0, -1);
        if (reg >= 0) {
            ctx.host.ext_renderer().dx9_set_shader_matrix(
                IsVertex, static_cast<u32>(reg), ctx.i32_arg(1, 0), ctx.i32_arg(2, 4));
        }
        return RValue(0.0);
    }

    template <bool IsVertex>
    RValue shader_uniform_f_buffer(const CallContext& ctx) {
        const i32 reg = ctx.i32_arg(0, -1);
        const u32 addr = static_cast<u32>(ctx.real(1, 0.0));
        const i32 size = ctx.i32_arg(2, 0);
        if (reg >= 0 && buffers_ && size >= 16) {
            if (auto* buf = buffers_->find_by_address(addr)) {
                const usize avail = std::min(static_cast<usize>(size), buf->data.size());
                const u32 count = static_cast<u32>(avail / 16u);
                if (count > 0) {
                    auto& renderer = ctx.host.ext_renderer();
                    renderer.flush();
                    std::vector<f32> tmp(static_cast<usize>(count) * 4u);
                    std::memcpy(tmp.data(), buf->data.data(), static_cast<usize>(count) * 16u);
                    if constexpr (IsVertex) renderer.dx9().set_vs_const_f(static_cast<u32>(reg), tmp.data(), count);
                    else                    renderer.dx9().set_ps_const_f(static_cast<u32>(reg), tmp.data(), count);
                }
            }
        }
        return RValue(0.0);
    }

    template <bool IsVertex>
    RValue shader_uniform_i_buffer(const CallContext& ctx) {
        const i32 reg = ctx.i32_arg(0, -1);
        const u32 addr = static_cast<u32>(ctx.real(1, 0.0));
        const i32 size = ctx.i32_arg(2, 0);
        if (reg >= 0 && buffers_ && size >= 16) {
            if (auto* buf = buffers_->find_by_address(addr)) {
                const usize avail = std::min(static_cast<usize>(size), buf->data.size());
                const u32 count = static_cast<u32>(avail / 16u);
                if (count > 0) {
                    auto& renderer = ctx.host.ext_renderer();
                    renderer.flush();
                    std::vector<i32> tmp(static_cast<usize>(count) * 4u);
                    std::memcpy(tmp.data(), buf->data.data(), static_cast<usize>(count) * 16u);
                    if constexpr (IsVertex) renderer.dx9().set_vs_const_i(static_cast<u32>(reg), tmp.data(), count);
                    else                    renderer.dx9().set_ps_const_i(static_cast<u32>(reg), tmp.data(), count);
                }
            }
        }
        return RValue(0.0);
    }

    template <bool IsVertex>
    RValue shader_uniform_b_buffer(const CallContext& ctx) {
        const i32 reg = ctx.i32_arg(0, -1);
        const u32 addr = static_cast<u32>(ctx.real(1, 0.0));
        const i32 size = ctx.i32_arg(2, 0);
        if (reg >= 0 && buffers_ && size >= 16) {
            if (auto* buf = buffers_->find_by_address(addr)) {
                const usize avail = std::min(static_cast<usize>(size), buf->data.size());
                const u32 count = std::min<u32>(16u, static_cast<u32>(avail / 16u));
                if (count > 0) {
                    auto& renderer = ctx.host.ext_renderer();
                    renderer.flush();
                    std::array<bool, 16> tmp{};
                    for (u32 i = 0; i < count; ++i) {
                        u32 raw = 0;
                        std::memcpy(&raw, buf->data.data() + static_cast<usize>(i) * 4u, 4u);
                        tmp[i] = (raw != 0u);
                    }
                    if constexpr (IsVertex) renderer.dx9().set_vs_const_b(static_cast<u32>(reg), tmp.data(), count);
                    else                    renderer.dx9().set_ps_const_b(static_cast<u32>(reg), tmp.data(), count);
                }
            }
        }
        return RValue(0.0);
    }

    GmBufferStore* buffers_ = nullptr;
    std::deque<CompoundTransformSpec> transform_specs_;
    bool has_started_ = false;
    f32 vx_ = 0.0f;
    f32 vy_ = 0.0f;
    f32 vz_ = 0.0f;
};

} // namespace

std::unique_ptr<IExtension> create_gm82dx_extension() {
    return std::make_unique<Gm82DxExtension>();
}

} // namespace gm
