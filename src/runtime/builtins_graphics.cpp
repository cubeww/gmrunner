#include "runtime/runner.h"

#include "base/encoding.h"
#include "base/math.h"
#include "graphics/renderer2d.h"
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace gm {

namespace {

i32 resolve_subimg(Runner& r, i32 self_id, i32 subimg_arg) noexcept {
    if (subimg_arg >= 0) return subimg_arg;
    if (const auto* inst = r.find_instance(self_id)) {
        return static_cast<i32>(std::floor(inst->image_index));
    }
    return 0;
}

std::string value_to_text(std::span<const RValue> args, usize i) {
    if (i >= args.size()) return {};
    if (args[i].is_string()) return args[i].as_string();
    const f64 v = args[i].as_real();
    const i64 r = delphi_round(v);
    if (std::fabs(v - static_cast<f64>(r)) < kGmlEpsilon && std::fabs(v) < 1e15) {
        return std::to_string(r);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

u32 hsv_to_gm_bgr(f64 h255, f64 s255, f64 v255) noexcept {
    const f64 h = std::fmod(std::fmod(h255, 255.0) + 255.0, 255.0) / 255.0 * 6.0;
    const f64 s = std::clamp(s255 / 255.0, 0.0, 1.0);
    const f64 v = std::clamp(v255 / 255.0, 0.0, 1.0);
    const i32 sector = static_cast<i32>(std::floor(h)) % 6;
    const f64 f = h - std::floor(h);
    const f64 p = v * (1.0 - s);
    const f64 q = v * (1.0 - s * f);
    const f64 t = v * (1.0 - s * (1.0 - f));
    f64 r = 0.0, g = 0.0, b = 0.0;
    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return rgba_to_gm_bgr(
        static_cast<u8>(std::clamp(r * 255.0 + 0.5, 0.0, 255.0)),
        static_cast<u8>(std::clamp(g * 255.0 + 0.5, 0.0, 255.0)),
        static_cast<u8>(std::clamp(b * 255.0 + 0.5, 0.0, 255.0)));
}

void gm_bgr_to_hsv(u32 bgr, f64& out_h, f64& out_s, f64& out_v) noexcept {
    const ColorRGBA c = gm_bgr_to_rgba(bgr);
    const f64 r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const f64 mx = std::max({r, g, b});
    const f64 mn = std::min({r, g, b});
    const f64 d = mx - mn;
    out_v = mx * 255.0;
    out_s = (mx > 0.0) ? (d / mx * 255.0) : 0.0;
    if (d <= 0.0) {
        out_h = 0.0;
    } else if (mx == r) {
        out_h = std::fmod((g - b) / d + 6.0, 6.0) / 6.0 * 255.0;
    } else if (mx == g) {
        out_h = ((b - r) / d + 2.0) / 6.0 * 255.0;
    } else {
        out_h = ((r - g) / d + 4.0) / 6.0 * 255.0;
    }
}

ActiveTile* find_tile(Runner& r, i32 id) noexcept {
    for (auto& t : r.tiles()) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

bool tile_contains(const ActiveTile& t, f64 x, f64 y) noexcept {
    return x >= t.x && x < t.x + t.w * t.xscale && y >= t.y && y < t.y + t.h * t.yscale;
}

} // namespace

void Runner::register_builtins_graphics() {
    // -----------------------------------------------------------------------
    // Draw State & Colors
    // -----------------------------------------------------------------------
    register_builtin("draw_set_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_color(static_cast<u32>(arg_i32(a, 0)));
        return RValue(0.0);
    });
    register_builtin("draw_get_color", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.renderer().color());
    });
    register_builtin("draw_set_alpha", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_alpha(static_cast<f32>(arg_real(a, 0, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_get_alpha", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(static_cast<f64>(r.renderer().alpha()));
    });
    register_builtin("draw_set_font", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_font(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("draw_set_halign", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_halign(arg_i32(a, 0, 0));
        return RValue(0.0);
    });
    register_builtin("draw_set_valign", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_valign(arg_i32(a, 0, 0));
        return RValue(0.0);
    });
    register_builtin("draw_set_blend_mode", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_blend_mode(arg_i32(a, 0, 0));
        return RValue(0.0);
    });
    register_builtin("draw_set_blend_mode_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_blend_mode_ext(arg_i32(a, 0, 5), arg_i32(a, 1, 6));
        return RValue(0.0);
    });
    register_builtin("draw_set_circle_precision", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_circle_precision(arg_i32(a, 0, 24));
        return RValue(0.0);
    });
    register_builtin("draw_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().clear_color(static_cast<u32>(arg_i32(a, 0)), 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_clear_alpha", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().clear_color(static_cast<u32>(arg_i32(a, 0)), static_cast<f32>(arg_real(a, 1, 1.0)));
        return RValue(0.0);
    });

    register_builtin("make_color_rgb", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(rgba_to_gm_bgr(
            static_cast<u8>(std::clamp(arg_i32(a, 0), 0, 255)),
            static_cast<u8>(std::clamp(arg_i32(a, 1), 0, 255)),
            static_cast<u8>(std::clamp(arg_i32(a, 2), 0, 255))));
    });
    register_builtin("make_color_hsv", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(hsv_to_gm_bgr(arg_real(a, 0), arg_real(a, 1), arg_real(a, 2)));
    });
    register_builtin("color_get_red", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<i32>(gm_bgr_to_rgba(static_cast<u32>(arg_i32(a, 0))).r));
    });
    register_builtin("color_get_green", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<i32>(gm_bgr_to_rgba(static_cast<u32>(arg_i32(a, 0))).g));
    });
    register_builtin("color_get_blue", [](Runner&, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<i32>(gm_bgr_to_rgba(static_cast<u32>(arg_i32(a, 0))).b));
    });
    register_builtin("color_get_hue", [](Runner&, i32, i32, std::span<const RValue> a) {
        f64 h, s, v; gm_bgr_to_hsv(static_cast<u32>(arg_i32(a, 0)), h, s, v);
        return RValue(std::floor(h));
    });
    register_builtin("color_get_saturation", [](Runner&, i32, i32, std::span<const RValue> a) {
        f64 h, s, v; gm_bgr_to_hsv(static_cast<u32>(arg_i32(a, 0)), h, s, v);
        return RValue(std::floor(s));
    });
    register_builtin("color_get_value", [](Runner&, i32, i32, std::span<const RValue> a) {
        f64 h, s, v; gm_bgr_to_hsv(static_cast<u32>(arg_i32(a, 0)), h, s, v);
        return RValue(std::floor(v));
    });
    register_builtin("merge_color", [](Runner&, i32, i32, std::span<const RValue> a) {
        const ColorRGBA c1 = gm_bgr_to_rgba(static_cast<u32>(arg_i32(a, 0)));
        const ColorRGBA c2 = gm_bgr_to_rgba(static_cast<u32>(arg_i32(a, 1)));
        const f64 t = std::clamp(arg_real(a, 2), 0.0, 1.0);
        return RValue(rgba_to_gm_bgr(
            static_cast<u8>(c1.r + (c2.r - c1.r) * t + 0.5),
            static_cast<u8>(c1.g + (c2.g - c1.g) * t + 0.5),
            static_cast<u8>(c1.b + (c2.b - c1.b) * t + 0.5)));
    });

    // -----------------------------------------------------------------------
    // Sprites & Backgrounds
    // -----------------------------------------------------------------------
    register_builtin("draw_self", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (const auto* inst = r.find_instance(self_id)) r.draw_instance_default(*inst);
        return RValue(0.0);
    });
    register_builtin("draw_sprite", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.renderer().draw_sprite_ext(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_sprite_ext", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.renderer().draw_sprite_ext(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4, 1.0)), static_cast<f32>(arg_real(a, 5, 1.0)),
            static_cast<f32>(arg_real(a, 6, 0.0)),
            static_cast<u32>(arg_i32(a, 7, 0xFFFFFF)), static_cast<f32>(arg_real(a, 8, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_sprite_stretched", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.renderer().draw_sprite_stretched_ext(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_sprite_stretched_ext", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.renderer().draw_sprite_stretched_ext(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            static_cast<u32>(arg_i32(a, 6, 0xFFFFFF)), static_cast<f32>(arg_real(a, 7, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_sprite_tiled", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto& v = r.views_[static_cast<usize>(std::clamp(r.view_current_, 0, 7))];
        const f32 vx = r.view_enabled_ ? static_cast<f32>(v.xview) : 0.0f;
        const f32 vy = r.view_enabled_ ? static_cast<f32>(v.yview) : 0.0f;
        const f32 vw = r.view_enabled_ ? static_cast<f32>(v.wview) : static_cast<f32>(r.room_width_);
        const f32 vh = r.view_enabled_ ? static_cast<f32>(v.hview) : static_cast<f32>(r.room_height_);
        r.renderer().draw_sprite_tiled_ext(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            1.0f, 1.0f, 0xFFFFFFu, 1.0f, vx, vy, vw, vh);
        return RValue(0.0);
    });
    register_builtin("draw_sprite_tiled_ext", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto& v = r.views_[static_cast<usize>(std::clamp(r.view_current_, 0, 7))];
        const f32 vx = r.view_enabled_ ? static_cast<f32>(v.xview) : 0.0f;
        const f32 vy = r.view_enabled_ ? static_cast<f32>(v.yview) : 0.0f;
        const f32 vw = r.view_enabled_ ? static_cast<f32>(v.wview) : static_cast<f32>(r.room_width_);
        const f32 vh = r.view_enabled_ ? static_cast<f32>(v.hview) : static_cast<f32>(r.room_height_);
        r.renderer().draw_sprite_tiled_ext(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4, 1.0)), static_cast<f32>(arg_real(a, 5, 1.0)),
            static_cast<u32>(arg_i32(a, 6, 0xFFFFFF)), static_cast<f32>(arg_real(a, 7, 1.0)),
            vx, vy, vw, vh);
        return RValue(0.0);
    });
    register_builtin("draw_sprite_part", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.renderer().draw_sprite_part_ext(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            static_cast<f32>(arg_real(a, 6)), static_cast<f32>(arg_real(a, 7)),
            1.0f, 1.0f, 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_sprite_part_ext", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.renderer().draw_sprite_part_ext(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            static_cast<f32>(arg_real(a, 6)), static_cast<f32>(arg_real(a, 7)),
            static_cast<f32>(arg_real(a, 8, 1.0)), static_cast<f32>(arg_real(a, 9, 1.0)),
            static_cast<u32>(arg_i32(a, 10, 0xFFFFFF)), static_cast<f32>(arg_real(a, 11, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_sprite_general", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.renderer().draw_sprite_general(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            static_cast<f32>(arg_real(a, 6)), static_cast<f32>(arg_real(a, 7)),
            static_cast<f32>(arg_real(a, 8, 1.0)), static_cast<f32>(arg_real(a, 9, 1.0)),
            static_cast<f32>(arg_real(a, 10, 0.0)),
            static_cast<u32>(arg_i32(a, 11, 0xFFFFFF)), static_cast<u32>(arg_i32(a, 12, 0xFFFFFF)),
            static_cast<u32>(arg_i32(a, 13, 0xFFFFFF)), static_cast<u32>(arg_i32(a, 14, 0xFFFFFF)),
            static_cast<f32>(arg_real(a, 15, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_sprite_pos", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.renderer().draw_sprite_pos(
            r.game(), arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            static_cast<f32>(arg_real(a, 6)), static_cast<f32>(arg_real(a, 7)),
            static_cast<f32>(arg_real(a, 8)), static_cast<f32>(arg_real(a, 9)),
            static_cast<f32>(arg_real(a, 10, 1.0)));
        return RValue(0.0);
    });

    register_builtin("draw_background", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_background_ext(r.game(), arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_background_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_background_ext(
            r.game(), arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3, 1.0)), static_cast<f32>(arg_real(a, 4, 1.0)),
            static_cast<f32>(arg_real(a, 5, 0.0)),
            static_cast<u32>(arg_i32(a, 6, 0xFFFFFF)), static_cast<f32>(arg_real(a, 7, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_background_stretched", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_background_stretched_ext(
            r.game(), arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_background_stretched_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_background_stretched_ext(
            r.game(), arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
            static_cast<u32>(arg_i32(a, 5, 0xFFFFFF)), static_cast<f32>(arg_real(a, 6, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_background_tiled", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto& v = r.views_[static_cast<usize>(std::clamp(r.view_current_, 0, 7))];
        const f32 vx = r.view_enabled_ ? static_cast<f32>(v.xview) : 0.0f;
        const f32 vy = r.view_enabled_ ? static_cast<f32>(v.yview) : 0.0f;
        const f32 vw = r.view_enabled_ ? static_cast<f32>(v.wview) : static_cast<f32>(r.room_width_);
        const f32 vh = r.view_enabled_ ? static_cast<f32>(v.hview) : static_cast<f32>(r.room_height_);
        r.renderer().draw_background_tiled_ext(
            r.game(), arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            1.0f, 1.0f, 0xFFFFFFu, 1.0f, vx, vy, vw, vh, true, true);
        return RValue(0.0);
    });
    register_builtin("draw_background_tiled_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto& v = r.views_[static_cast<usize>(std::clamp(r.view_current_, 0, 7))];
        const f32 vx = r.view_enabled_ ? static_cast<f32>(v.xview) : 0.0f;
        const f32 vy = r.view_enabled_ ? static_cast<f32>(v.yview) : 0.0f;
        const f32 vw = r.view_enabled_ ? static_cast<f32>(v.wview) : static_cast<f32>(r.room_width_);
        const f32 vh = r.view_enabled_ ? static_cast<f32>(v.hview) : static_cast<f32>(r.room_height_);
        r.renderer().draw_background_tiled_ext(
            r.game(), arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3, 1.0)), static_cast<f32>(arg_real(a, 4, 1.0)),
            static_cast<u32>(arg_i32(a, 5, 0xFFFFFF)), static_cast<f32>(arg_real(a, 6, 1.0)),
            vx, vy, vw, vh, true, true);
        return RValue(0.0);
    });
    register_builtin("draw_background_part", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_background_part_ext(
            r.game(), arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
            static_cast<f32>(arg_real(a, 5)), static_cast<f32>(arg_real(a, 6)),
            1.0f, 1.0f, 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_background_part_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_background_part_ext(
            r.game(), arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
            static_cast<f32>(arg_real(a, 5)), static_cast<f32>(arg_real(a, 6)),
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)),
            static_cast<u32>(arg_i32(a, 9, 0xFFFFFF)), static_cast<f32>(arg_real(a, 10, 1.0)));
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Text & Measurements
    // -----------------------------------------------------------------------
    register_builtin("string_width", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<f64>(r.renderer().measure_string_width(r.game(), value_to_text(a, 0))));
    });
    register_builtin("string_height", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<f64>(r.renderer().measure_string_height(r.game(), value_to_text(a, 0))));
    });
    register_builtin("string_width_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<f64>(r.renderer().measure_string_width(r.game(), value_to_text(a, 0))));
    });
    register_builtin("string_height_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<f64>(r.renderer().measure_string_height(
            r.game(), value_to_text(a, 0), arg_i32(a, 1, -1), arg_i32(a, 2, -1))));
    });
    register_builtin("draw_text", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_text_ext_color(
            r.game(), static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), value_to_text(a, 2),
            -1, -1, c, c, c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_text_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_text_ext_color(
            r.game(), static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), value_to_text(a, 2),
            arg_i32(a, 3, -1), arg_i32(a, 4, -1),
            c, c, c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_text_transformed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_text_ext_color(
            r.game(), static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), value_to_text(a, 2),
            -1, -1, c, c, c, c, r.renderer().alpha(),
            TextTransform{static_cast<f32>(arg_real(a, 3, 1.0)), static_cast<f32>(arg_real(a, 4, 1.0)),
                          static_cast<f32>(arg_real(a, 5, 0.0))});
        return RValue(0.0);
    });
    register_builtin("draw_text_ext_transformed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_text_ext_color(
            r.game(), static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), value_to_text(a, 2),
            arg_i32(a, 3, -1), arg_i32(a, 4, -1),
            c, c, c, c, r.renderer().alpha(),
            TextTransform{static_cast<f32>(arg_real(a, 5, 1.0)), static_cast<f32>(arg_real(a, 6, 1.0)),
                          static_cast<f32>(arg_real(a, 7, 0.0))});
        return RValue(0.0);
    });
    register_builtin("draw_text_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_text_ext_color(
            r.game(), static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), value_to_text(a, 2),
            -1, -1,
            static_cast<u32>(arg_i32(a, 3)), static_cast<u32>(arg_i32(a, 4)),
            static_cast<u32>(arg_i32(a, 5)), static_cast<u32>(arg_i32(a, 6)),
            static_cast<f32>(arg_real(a, 7, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_text_ext_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_text_ext_color(
            r.game(), static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), value_to_text(a, 2),
            arg_i32(a, 3, -1), arg_i32(a, 4, -1),
            static_cast<u32>(arg_i32(a, 5)), static_cast<u32>(arg_i32(a, 6)),
            static_cast<u32>(arg_i32(a, 7)), static_cast<u32>(arg_i32(a, 8)),
            static_cast<f32>(arg_real(a, 9, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_text_transformed_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_text_ext_color(
            r.game(), static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), value_to_text(a, 2),
            -1, -1, static_cast<u32>(arg_i32(a, 6)), static_cast<u32>(arg_i32(a, 7)),
            static_cast<u32>(arg_i32(a, 8)), static_cast<u32>(arg_i32(a, 9)),
            static_cast<f32>(arg_real(a, 10, 1.0)),
            TextTransform{static_cast<f32>(arg_real(a, 3, 1.0)), static_cast<f32>(arg_real(a, 4, 1.0)),
                          static_cast<f32>(arg_real(a, 5, 0.0))});
        return RValue(0.0);
    });
    register_builtin("draw_text_ext_transformed_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_text_ext_color(
            r.game(), static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), value_to_text(a, 2),
            arg_i32(a, 3, -1), arg_i32(a, 4, -1),
            static_cast<u32>(arg_i32(a, 8)), static_cast<u32>(arg_i32(a, 9)),
            static_cast<u32>(arg_i32(a, 10)), static_cast<u32>(arg_i32(a, 11)),
            static_cast<f32>(arg_real(a, 12, 1.0)),
            TextTransform{static_cast<f32>(arg_real(a, 5, 1.0)), static_cast<f32>(arg_real(a, 6, 1.0)),
                          static_cast<f32>(arg_real(a, 7, 0.0))});
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Primitives & Healthbar
    // -----------------------------------------------------------------------
    register_builtin("draw_point", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_point_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_point_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_point_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<u32>(arg_i32(a, 2)), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_line", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_line_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_line_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_line_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<u32>(arg_i32(a, 4)), static_cast<u32>(arg_i32(a, 5)), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_line_width", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_line_width_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4, 1.0)), c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_line_width_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_line_width_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4, 1.0)), static_cast<u32>(arg_i32(a, 5)), static_cast<u32>(arg_i32(a, 6)), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_rectangle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_rectangle_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), c, c, c, c, arg_bool(a, 4), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_rectangle_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_rectangle_color(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<u32>(arg_i32(a, 4)), static_cast<u32>(arg_i32(a, 5)),
            static_cast<u32>(arg_i32(a, 6)), static_cast<u32>(arg_i32(a, 7)),
            arg_bool(a, 8), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_roundrect", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_rectangle_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), c, c, c, c, arg_bool(a, 4), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_roundrect_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c1 = static_cast<u32>(arg_i32(a, 4));
        const u32 c2 = static_cast<u32>(arg_i32(a, 5));
        r.renderer().draw_rectangle_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), c1, c1, c2, c2, arg_bool(a, 6), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_triangle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_triangle_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)), c, c, c, arg_bool(a, 6), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_triangle_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_triangle_color(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)),
            static_cast<u32>(arg_i32(a, 6)), static_cast<u32>(arg_i32(a, 7)), static_cast<u32>(arg_i32(a, 8)),
            arg_bool(a, 9), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_circle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_circle_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), c, c, arg_bool(a, 3), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_circle_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_circle_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<u32>(arg_i32(a, 3)), static_cast<u32>(arg_i32(a, 4)), arg_bool(a, 5), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_ellipse", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const u32 c = r.renderer().color();
        r.renderer().draw_ellipse_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), c, c, arg_bool(a, 4), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_ellipse_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_ellipse_color(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<u32>(arg_i32(a, 4)), static_cast<u32>(arg_i32(a, 5)), arg_bool(a, 6), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_healthbar", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f32 x1 = static_cast<f32>(arg_real(a, 0)), y1 = static_cast<f32>(arg_real(a, 1));
        const f32 x2 = static_cast<f32>(arg_real(a, 2)), y2 = static_cast<f32>(arg_real(a, 3));
        const f64 amt = std::clamp(arg_real(a, 4), 0.0, 100.0) / 100.0;
        const u32 backcol = static_cast<u32>(arg_i32(a, 5));
        const ColorRGBA mn = gm_bgr_to_rgba(static_cast<u32>(arg_i32(a, 6)));
        const ColorRGBA mx = gm_bgr_to_rgba(static_cast<u32>(arg_i32(a, 7)));
        const i32 dir = arg_i32(a, 8, 0);
        const bool showback = arg_bool(a, 9, true);
        const bool showborder = arg_bool(a, 10, true);
        const f32 alpha = r.renderer().alpha();
        if (showback) r.renderer().draw_rectangle_color(x1, y1, x2, y2, backcol, backcol, backcol, backcol, false, alpha);
        if (amt > 0.0) {
            const u32 barcol = rgba_to_gm_bgr(
                static_cast<u8>(mn.r + (mx.r - mn.r) * amt + 0.5),
                static_cast<u8>(mn.g + (mx.g - mn.g) * amt + 0.5),
                static_cast<u8>(mn.b + (mx.b - mn.b) * amt + 0.5));
            f32 bx1 = x1, by1 = y1, bx2 = x2, by2 = y2;
            switch (dir) {
                case 0: bx2 = x1 + (x2 - x1) * static_cast<f32>(amt); break;
                case 1: bx1 = x2 - (x2 - x1) * static_cast<f32>(amt); break;
                case 2: by2 = y1 + (y2 - y1) * static_cast<f32>(amt); break;
                case 3: by1 = y2 - (y2 - y1) * static_cast<f32>(amt); break;
                default: break;
            }
            r.renderer().draw_rectangle_color(bx1, by1, bx2, by2, barcol, barcol, barcol, barcol, false, alpha);
        }
        if (showborder) r.renderer().draw_rectangle_color(x1, y1, x2, y2, 0u, 0u, 0u, 0u, true, alpha);
        return RValue(0.0);
    });
    register_builtin("make_color", [](Runner&, i32, i32, std::span<const RValue> a) {
        const u8 r = static_cast<u8>(std::clamp(arg_i32(a, 0), 0, 255));
        const u8 g = static_cast<u8>(std::clamp(arg_i32(a, 1), 0, 255));
        const u8 b = static_cast<u8>(std::clamp(arg_i32(a, 2), 0, 255));
        return RValue(rgba_to_gm_bgr(r, g, b));
    });
    register_builtin("draw_getpixel", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.renderer().screen_getpixel(arg_i32(a, 0), arg_i32(a, 1)));
    });
    register_builtin("draw_background_general", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_background_general(
            r.game(), arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
            static_cast<f32>(arg_real(a, 5)), static_cast<f32>(arg_real(a, 6)),
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)),
            static_cast<f32>(arg_real(a, 9, 0.0)),
            static_cast<u32>(arg_i32(a, 10, 0xFFFFFF)), static_cast<u32>(arg_i32(a, 11, 0xFFFFFF)),
            static_cast<u32>(arg_i32(a, 12, 0xFFFFFF)), static_cast<u32>(arg_i32(a, 13, 0xFFFFFF)),
            static_cast<f32>(arg_real(a, 14, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_arrow", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_arrow(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            static_cast<f32>(arg_real(a, 4, 10.0)), r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_button", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_button(
            static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)),
            static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)),
            arg_bool(a, 4, true), r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_path", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 pid = arg_i32(a, 0, -1);
        if (pid < 0 || static_cast<usize>(pid) >= r.game().paths.size() || !r.game().paths[static_cast<usize>(pid)].exists) return RValue(0.0);
        const auto& p = r.game().paths[static_cast<usize>(pid)];
        const i64 steps = delphi_round(p.length() / 4.0);
        if (steps <= 0) return RValue(0.0);
        PathPoint prev = p.sample(0.0);
        const f64 ox = arg_bool(a, 3) ? 0.0 : arg_real(a, 1) - prev.x;
        const f64 oy = arg_bool(a, 3) ? 0.0 : arg_real(a, 2) - prev.y;
        const u32 col = r.renderer().color();
        const f32 alpha = r.renderer().alpha();
        for (i64 i = 1; i <= steps; ++i) {
            const auto next = p.sample(static_cast<f64>(i) / static_cast<f64>(steps));
            r.renderer().draw_line_color(
                static_cast<f32>(prev.x + ox), static_cast<f32>(prev.y + oy),
                static_cast<f32>(next.x + ox), static_cast<f32>(next.y + oy), col, col, alpha);
            prev = next;
        }
        return RValue(0.0);
    });
    register_builtin("draw_highscore", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f32 x1 = static_cast<f32>(arg_real(a, 0)), y1 = static_cast<f32>(arg_real(a, 1));
        const f32 x2 = static_cast<f32>(arg_real(a, 2)), y2 = static_cast<f32>(arg_real(a, 3));
        const f32 row_h = std::max(12.0f, (y2 - y1) / 10.0f);
        const u32 col = r.renderer().color();
        const f32 alpha = r.renderer().alpha();
        for (usize i = 0; i < r.highscores_.size(); ++i) {
            const f32 ry = y1 + static_cast<f32>(i) * row_h;
            const std::string line = std::to_string(i + 1) + ". " + r.highscores_[i].name + " - " +
                                     std::to_string(static_cast<i64>(delphi_round(r.highscores_[i].score)));
            r.renderer().draw_text_ext_color(r.game(), x1, ry, line, -1, -1, col, col, col, col, alpha);
        }
        return RValue(0.0);
    });

    // Immediate primitive builders
    register_builtin("draw_primitive_begin", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_begin(arg_i32(a, 0, 4), TextureHandle{});
        return RValue(0.0);
    });
    register_builtin("draw_primitive_begin_texture", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 tid = arg_i32(a, 1, -1);
        r.renderer().primitive_begin(arg_i32(a, 0, 4), tid > 0 ? TextureHandle{static_cast<u32>(tid)} : TextureHandle{});
        return RValue(0.0);
    });
    register_builtin("draw_vertex", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), 0.0f, 0.0f, r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_vertex_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), 0.0f, 0.0f, static_cast<u32>(arg_i32(a, 2)), static_cast<f32>(arg_real(a, 3, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_vertex_texture", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), r.renderer().color(), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("draw_vertex_texture_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().primitive_vertex(static_cast<f32>(arg_real(a, 0)), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<u32>(arg_i32(a, 4)), static_cast<f32>(arg_real(a, 5, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_primitive_end", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.renderer().primitive_end();
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Surfaces & Textures
    // -----------------------------------------------------------------------
    register_builtin("surface_create", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.renderer().surface_create(arg_i32(a, 0), arg_i32(a, 1)));
    });
    register_builtin("surface_create_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.renderer().surface_create(arg_i32(a, 1), arg_i32(a, 2)));
    });
    register_builtin("surface_free", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().surface_free(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("surface_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.renderer().surface_exists(arg_i32(a, 0, -1)));
    });
    register_builtin("surface_get_width", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.renderer().surface_width(arg_i32(a, 0, -1)));
    });
    register_builtin("surface_get_height", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.renderer().surface_height(arg_i32(a, 0, -1)));
    });
    register_builtin("surface_get_texture", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<u32>(r.renderer().surface_texture(arg_i32(a, 0, -1)).id));
    });
    register_builtin("surface_set_target", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.renderer().surface_set_target(arg_i32(a, 0, -1)));
    });
    register_builtin("surface_reset_target", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.renderer().surface_reset_target());
    });
    register_builtin("draw_surface", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_surface_ext(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), 1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_surface_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_surface_ext(
            arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3, 1.0)), static_cast<f32>(arg_real(a, 4, 1.0)),
            static_cast<f32>(arg_real(a, 5, 0.0)),
            static_cast<u32>(arg_i32(a, 6, 0xFFFFFF)), static_cast<f32>(arg_real(a, 7, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_surface_stretched", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_surface_stretched_ext(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_surface_stretched_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_surface_stretched_ext(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<u32>(arg_i32(a, 5, 0xFFFFFF)), static_cast<f32>(arg_real(a, 6, 1.0)));
        return RValue(0.0);
    });
    register_builtin("draw_surface_part", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_surface_part_ext(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), static_cast<f32>(arg_real(a, 5)), static_cast<f32>(arg_real(a, 6)), 1.0f, 1.0f, 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("draw_surface_part_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_surface_part_ext(
            arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
            static_cast<f32>(arg_real(a, 5)), static_cast<f32>(arg_real(a, 6)),
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)),
            static_cast<u32>(arg_i32(a, 9, 0xFFFFFF)), static_cast<f32>(arg_real(a, 10, 1.0)));
        return RValue(0.0);
    });
    register_builtin("surface_save", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto dst = r.vfs().resolve_for_write(arg_str(a, 1));
        return RValue(r.renderer().surface_save(arg_i32(a, 0, -1), Vfs::path_to_utf8(dst)));
    });
    register_builtin("surface_save_part", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto dst = r.vfs().resolve_for_write(arg_str(a, 1));
        return RValue(r.renderer().surface_save_part(arg_i32(a, 0, -1), Vfs::path_to_utf8(dst),
                                                     arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4), arg_i32(a, 5)));
    });
    register_builtin("surface_getpixel", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.renderer().surface_getpixel(arg_i32(a, 0, -1), arg_i32(a, 1), arg_i32(a, 2)));
    });
    register_builtin("draw_surface_tiled", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto& v = r.views_[static_cast<usize>(std::clamp(r.view_current_, 0, 7))];
        const f32 vx = r.view_enabled_ ? static_cast<f32>(v.xview) : 0.0f;
        const f32 vy = r.view_enabled_ ? static_cast<f32>(v.yview) : 0.0f;
        const f32 vw = r.view_enabled_ ? static_cast<f32>(v.wview) : static_cast<f32>(r.room_width_);
        const f32 vh = r.view_enabled_ ? static_cast<f32>(v.hview) : static_cast<f32>(r.room_height_);
        r.renderer().draw_surface_tiled_ext(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                            1.0f, 1.0f, 0xFFFFFFu, 1.0f, vx, vy, vw, vh);
        return RValue(0.0);
    });
    register_builtin("draw_surface_tiled_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto& v = r.views_[static_cast<usize>(std::clamp(r.view_current_, 0, 7))];
        const f32 vx = r.view_enabled_ ? static_cast<f32>(v.xview) : 0.0f;
        const f32 vy = r.view_enabled_ ? static_cast<f32>(v.yview) : 0.0f;
        const f32 vw = r.view_enabled_ ? static_cast<f32>(v.wview) : static_cast<f32>(r.room_width_);
        const f32 vh = r.view_enabled_ ? static_cast<f32>(v.hview) : static_cast<f32>(r.room_height_);
        r.renderer().draw_surface_tiled_ext(
            arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3, 1.0)), static_cast<f32>(arg_real(a, 4, 1.0)),
            static_cast<u32>(arg_i32(a, 5, 0xFFFFFF)), static_cast<f32>(arg_real(a, 6, 1.0)),
            vx, vy, vw, vh);
        return RValue(0.0);
    });
    register_builtin("draw_surface_general", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().draw_surface_general(
            arg_i32(a, 0, -1),
            static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
            static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
            static_cast<f32>(arg_real(a, 5)), static_cast<f32>(arg_real(a, 6)),
            static_cast<f32>(arg_real(a, 7, 1.0)), static_cast<f32>(arg_real(a, 8, 1.0)),
            static_cast<f32>(arg_real(a, 9, 0.0)),
            static_cast<u32>(arg_i32(a, 10, 0xFFFFFF)), static_cast<u32>(arg_i32(a, 11, 0xFFFFFF)),
            static_cast<u32>(arg_i32(a, 12, 0xFFFFFF)), static_cast<u32>(arg_i32(a, 13, 0xFFFFFF)),
            static_cast<f32>(arg_real(a, 14, 1.0)));
        return RValue(0.0);
    });
    register_builtin("surface_copy", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1), src = arg_i32(a, 3, -1);
        r.renderer().surface_copy_part(dst, arg_i32(a, 1), arg_i32(a, 2), src, 0, 0,
                                       r.renderer().surface_width(src), r.renderer().surface_height(src));
        return RValue(0.0);
    });
    register_builtin("surface_copy_part", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().surface_copy_part(arg_i32(a, 0, -1), arg_i32(a, 1), arg_i32(a, 2),
                                       arg_i32(a, 3, -1), arg_i32(a, 4), arg_i32(a, 5),
                                       arg_i32(a, 6), arg_i32(a, 7));
        return RValue(0.0);
    });

    register_builtin("sprite_get_texture", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        return RValue(static_cast<u32>(r.renderer().sprite_texture(arg_i32(a, 0, -1), resolve_subimg(r, self_id, arg_i32(a, 1))).id));
    });
    register_builtin("background_get_texture", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(static_cast<u32>(r.renderer().background_texture(arg_i32(a, 0, -1)).id));
    });
    register_builtin("texture_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 tid = arg_i32(a, 0, -1);
        return RValue(tid > 0 && r.renderer().texture_exists(TextureHandle{static_cast<u32>(tid)}));
    });
    register_builtin("texture_get_width", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 tid = arg_i32(a, 0, -1);
        const i32 w = tid > 0 ? r.renderer().texture_width(TextureHandle{static_cast<u32>(tid)}) : 0;
        return RValue(w > 0 ? 1.0 : 0.0); // GM8 texture_get_width returns UV ratio (1.0 for NPOT textures)
    });
    register_builtin("texture_get_height", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 tid = arg_i32(a, 0, -1);
        const i32 h = tid > 0 ? r.renderer().texture_height(TextureHandle{static_cast<u32>(tid)}) : 0;
        return RValue(h > 0 ? 1.0 : 0.0);
    });
    register_builtin("texture_set_priority", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("texture_set_interpolation", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_interpolation(arg_bool(a, 0));
        return RValue(0.0);
    });
    register_builtin("texture_set_repeat", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_texture_repeat(arg_bool(a, 0));
        return RValue(0.0);
    });
    register_builtin("texture_set_blending", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_texture_blending(arg_bool(a, 0, true));
        return RValue(0.0);
    });
    register_builtin("texture_preload", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });

    // -----------------------------------------------------------------------
    // Tiles
    // -----------------------------------------------------------------------
    register_builtin("tile_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        ActiveTile t{};
        t.id = r.alloc_tile_id();
        t.background_index = arg_i32(a, 0, -1);
        t.xo = arg_i32(a, 1); t.yo = arg_i32(a, 2);
        t.w = arg_i32(a, 3);  t.h = arg_i32(a, 4);
        t.x = arg_real(a, 5); t.y = arg_real(a, 6);
        t.depth = arg_real(a, 7);
        r.tiles().push_back(t);
        return RValue(t.id);
    });
    register_builtin("tile_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        auto& vec = r.tiles();
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const ActiveTile& t) { return t.id == id; }), vec.end());
        return RValue(0.0);
    });
    register_builtin("tile_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(find_tile(r, arg_i32(a, 0, -1)) != nullptr); });
    register_builtin("tile_get_x", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->x : 0.0); });
    register_builtin("tile_get_y", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->y : 0.0); });
    register_builtin("tile_get_depth", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->depth : 0.0); });
    register_builtin("tile_get_visible", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t && t->visible); });
    register_builtin("tile_set_position", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = find_tile(r, arg_i32(a, 0, -1))) { t->x = arg_real(a, 1); t->y = arg_real(a, 2); }
        return RValue(0.0);
    });
    register_builtin("tile_set_visible", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = find_tile(r, arg_i32(a, 0, -1))) t->visible = arg_bool(a, 1);
        return RValue(0.0);
    });
    register_builtin("tile_set_depth", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = find_tile(r, arg_i32(a, 0, -1))) t->depth = arg_real(a, 1);
        return RValue(0.0);
    });
    register_builtin("tile_set_alpha", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = find_tile(r, arg_i32(a, 0, -1))) t->alpha = arg_real(a, 1, 1.0);
        return RValue(0.0);
    });
    register_builtin("tile_set_blend", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = find_tile(r, arg_i32(a, 0, -1))) t->blend = static_cast<u32>(arg_i32(a, 1)) & 0xFFFFFFu;
        return RValue(0.0);
    });
    register_builtin("tile_layer_hide", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 d = arg_real(a, 0);
        for (auto& t : r.tiles()) if (gml_equals(t.depth, d)) t.visible = false;
        return RValue(0.0);
    });
    register_builtin("tile_layer_show", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 d = arg_real(a, 0);
        for (auto& t : r.tiles()) if (gml_equals(t.depth, d)) t.visible = true;
        return RValue(0.0);
    });
    register_builtin("tile_layer_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 d = arg_real(a, 0);
        auto& vec = r.tiles();
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const ActiveTile& t) { return gml_equals(t.depth, d); }), vec.end());
        return RValue(0.0);
    });
    register_builtin("tile_layer_shift", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 d = arg_real(a, 0), dx = arg_real(a, 1), dy = arg_real(a, 2);
        for (auto& t : r.tiles()) if (gml_equals(t.depth, d)) { t.x += dx; t.y += dy; }
        return RValue(0.0);
    });
    register_builtin("tile_layer_find", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 d = arg_real(a, 0), px = arg_real(a, 1), py = arg_real(a, 2);
        for (const auto& t : r.tiles()) {
            if (t.depth == d && tile_contains(t, px, py)) {
                return RValue(t.id);
            }
        }
        return RValue(-1);
    });
    register_builtin("tile_find", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 px = arg_i32(a, 0), py = arg_i32(a, 1);
        const bool foreground = arg_bool(a, 2);
        for (const auto& t : r.tiles()) {
            if ((t.depth < 0.0) == foreground && tile_contains(t, px, py)) {
                return RValue(t.id);
            }
        }
        return RValue(-1);
    });
    register_builtin("tile_delete_at", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 px = arg_i32(a, 0), py = arg_i32(a, 1);
        const bool foreground = arg_bool(a, 2);
        auto& vec = r.tiles();
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const ActiveTile& t) {
            // GM includes both far edges and accepts depth zero for either group when deleting.
            return (foreground ? t.depth <= 0.0 : t.depth >= 0.0) &&
                   px >= t.x && px <= t.x + t.w * t.xscale && py >= t.y && py <= t.y + t.h * t.yscale;
        }), vec.end());
        return RValue(0.0);
    });
    register_builtin("tile_layer_delete_at", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 d = arg_real(a, 0), px = arg_real(a, 1), py = arg_real(a, 2);
        auto& vec = r.tiles();
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const ActiveTile& t) {
            return t.depth == d && tile_contains(t, px, py);
        }), vec.end());
        return RValue(0.0);
    });
    register_builtin("tile_layer_depth", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 old_d = arg_real(a, 0), new_d = arg_real(a, 1);
        for (auto& t : r.tiles()) if (gml_equals(t.depth, old_d)) t.depth = new_d;
        return RValue(0.0);
    });
    register_builtin("tile_get_background", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->background_index : -1); });
    register_builtin("tile_get_left", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->xo : 0); });
    register_builtin("tile_get_top", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->yo : 0); });
    register_builtin("tile_get_width", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->w : 0); });
    register_builtin("tile_get_height", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->h : 0); });
    register_builtin("tile_get_xscale", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->xscale : 1.0); });
    register_builtin("tile_get_yscale", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->yscale : 1.0); });
    register_builtin("tile_get_alpha", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->alpha : 1.0); });
    register_builtin("tile_get_blend", [](Runner& r, i32, i32, std::span<const RValue> a) { auto* t = find_tile(r, arg_i32(a, 0, -1)); return RValue(t ? t->blend : 0xFFFFFFu); });
    register_builtin("tile_set_background", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = find_tile(r, arg_i32(a, 0, -1))) t->background_index = arg_i32(a, 1, -1);
        return RValue(0.0);
    });
    register_builtin("tile_set_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = find_tile(r, arg_i32(a, 0, -1))) {
            t->xo = arg_i32(a, 1); t->yo = arg_i32(a, 2);
            t->w = arg_i32(a, 3);  t->h = arg_i32(a, 4);
        }
        return RValue(0.0);
    });
    register_builtin("tile_set_scale", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = find_tile(r, arg_i32(a, 0, -1))) { t->xscale = arg_real(a, 1, 1.0); t->yscale = arg_real(a, 2, 1.0); }
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Window, Display & Screen
    // -----------------------------------------------------------------------
    register_builtin("window_set_caption", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_title(codepage_to_utf8(arg_str(a, 0), r.game().meta.string_codepage()));
        return RValue(0.0);
    });
    register_builtin("window_get_caption", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(utf8_to_codepage(r.window().title(), r.game().meta.string_codepage()));
    });
    register_builtin("window_set_fullscreen", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_fullscreen(arg_bool(a, 0));
        return RValue(0.0);
    });
    register_builtin("window_get_fullscreen", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().is_fullscreen()); });
    register_builtin("window_set_size", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.set_window_size(std::max(1, arg_i32(a, 0, 640)), std::max(1, arg_i32(a, 1, 480)));
        return RValue(0.0);
    });
    register_builtin("window_get_width", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().width()); });
    register_builtin("window_get_height", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().height()); });
    register_builtin("window_set_position", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_position(arg_i32(a, 0), arg_i32(a, 1));
        return RValue(0.0);
    });
    register_builtin("window_get_x", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().x()); });
    register_builtin("window_get_y", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().y()); });
    register_builtin("window_set_rectangle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_position(arg_i32(a, 0), arg_i32(a, 1));
        r.set_window_size(std::max(1, arg_i32(a, 2, 640)), std::max(1, arg_i32(a, 3, 480)));
        return RValue(0.0);
    });
    register_builtin("window_center", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.window().center();
        return RValue(0.0);
    });
    register_builtin("window_default", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.window_default();
        return RValue(0.0);
    });
    register_builtin("window_set_cursor", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window_cursor_ = arg_i32(a, 0, 0);
        r.window().set_cursor(r.window_cursor_);
        return RValue(0.0);
    });
    register_builtin("window_get_cursor", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window_cursor_); });
    register_builtin("window_set_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window_color_ = static_cast<u32>(arg_i32(a, 0)) & 0xFFFFFFu;
        return RValue(0.0);
    });
    register_builtin("window_get_color", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window_color_); });
    register_builtin("window_mouse_get_x", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(static_cast<f64>(r.window().input().mouse_x)); });
    register_builtin("window_mouse_get_y", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(static_cast<f64>(r.window().input().mouse_y)); });
    register_builtin("window_mouse_set", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_mouse_position(arg_i32(a, 0), arg_i32(a, 1));
        r.update_mouse_room_coords();
        return RValue(0.0);
    });
    register_builtin("window_views_mouse_get_x", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.mouse_x_); });
    register_builtin("window_views_mouse_get_y", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.mouse_y_); });
    register_builtin("window_views_mouse_set", [](Runner& r, i32, i32, std::span<const RValue> a) {
        i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
        r.compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
        const f64 dpi_x = static_cast<f64>(r.window().drawable_width()) / static_cast<f64>(std::max(1, r.window().width()));
        const f64 dpi_y = static_cast<f64>(r.window().drawable_height()) / static_cast<f64>(std::max(1, r.window().height()));
        const f64 rx = arg_real(a, 0), ry = arg_real(a, 1);
        f64 cx = rx, cy = ry;
        if (r.view_enabled_) {
            for (const auto& v : r.views_) {
                if (!v.visible) continue;
                cx = v.xport + (rx - v.xview) * static_cast<f64>(v.wport) / std::max(1.0, v.wview);
                cy = v.yport + (ry - v.yview) * static_cast<f64>(v.hport) / std::max(1.0, v.hview);
                break;
            }
        }
        const i32 wx = static_cast<i32>((static_cast<f64>(vx) + cx * static_cast<f64>(vw) / static_cast<f64>(cw)) / dpi_x + 0.5);
        const i32 wy = static_cast<i32>((static_cast<f64>(vy) + cy * static_cast<f64>(vh) / static_cast<f64>(ch)) / dpi_y + 0.5);
        r.window().set_mouse_position(wx, wy);
        r.update_mouse_room_coords();
        return RValue(0.0);
    });
    register_builtin("window_view_mouse_get_x", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.mouse_x_); });
    register_builtin("window_view_mouse_get_y", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.mouse_y_); });
    register_builtin("window_view_mouse_set", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 vi = std::clamp(arg_i32(a, 0, 0), 0, 7);
        const auto& v = r.views_[static_cast<usize>(vi)];
        i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
        r.compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
        const f64 dpi_x = static_cast<f64>(r.window().drawable_width()) / static_cast<f64>(std::max(1, r.window().width()));
        const f64 dpi_y = static_cast<f64>(r.window().drawable_height()) / static_cast<f64>(std::max(1, r.window().height()));
        const f64 cx = r.view_enabled_ ? (v.xport + (arg_real(a, 1) - v.xview) * static_cast<f64>(v.wport) / std::max(1.0, v.wview)) : arg_real(a, 1);
        const f64 cy = r.view_enabled_ ? (v.yport + (arg_real(a, 2) - v.yview) * static_cast<f64>(v.hport) / std::max(1.0, v.hview)) : arg_real(a, 2);
        const i32 wx = static_cast<i32>((static_cast<f64>(vx) + cx * static_cast<f64>(vw) / static_cast<f64>(cw)) / dpi_x + 0.5);
        const i32 wy = static_cast<i32>((static_cast<f64>(vy) + cy * static_cast<f64>(vh) / static_cast<f64>(ch)) / dpi_y + 0.5);
        r.window().set_mouse_position(wx, wy);
        r.update_mouse_room_coords();
        return RValue(0.0);
    });
    register_builtin("window_set_visible", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_visible(arg_bool(a, 0, true));
        return RValue(0.0);
    });
    register_builtin("window_get_visible", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().is_visible()); });
    register_builtin("window_set_showborder", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window_showborder_ = arg_bool(a, 0, true);
        r.window().set_borderless(!r.window_showborder_);
        return RValue(0.0);
    });
    register_builtin("window_get_showborder", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window_showborder_); });
    register_builtin("window_set_showicons", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_show_icons(arg_bool(a, 0, true));
        return RValue(0.0);
    });
    register_builtin("window_get_showicons", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().shows_icons()); });
    register_builtin("window_set_stayontop", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_stay_on_top(arg_bool(a, 0, false));
        return RValue(0.0);
    });
    register_builtin("window_get_stayontop", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().is_stay_on_top()); });
    register_builtin("window_set_sizeable", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().set_resizable(arg_bool(a, 0, true));
        return RValue(0.0);
    });
    register_builtin("window_get_sizeable", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().is_resizable()); });
    register_builtin("window_set_region_scale", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window_region_scale_ = arg_real(a, 0, -1.0);
        r.adapt_window_to_region(arg_bool(a, 1));
        return RValue(0.0);
    });
    register_builtin("window_get_region_scale", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window_region_scale_); });
    register_builtin("window_set_region_size", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 w = arg_i32(a, 0);
        const i32 h = arg_i32(a, 1);
        if (w > 0) r.window_region_w_ = w;
        if (h > 0) r.window_region_h_ = h;
        r.adapt_window_to_region(arg_bool(a, 2));
        return RValue(0.0);
    });
    register_builtin("window_get_region_width", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.window_region_w_ > 0 ? r.window_region_w_ : r.window().width());
    });
    register_builtin("window_get_region_height", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.window_region_h_ > 0 ? r.window_region_h_ : r.window().height());
    });
    register_builtin("display_get_width", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().display_width()); });
    register_builtin("display_get_height", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.window().display_height()); });
    register_builtin("display_get_colordepth", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(32); });
    register_builtin("display_get_frequency", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(60); });
    register_builtin("display_set_size", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(true); });
    register_builtin("display_set_colordepth", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(true); });
    register_builtin("display_set_frequency", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(true); });
    register_builtin("display_set_all", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(true); });
    register_builtin("display_test_all", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(true); });
    register_builtin("display_reset", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("display_mouse_get_x", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(PlatformWindow::display_mouse_x()); });
    register_builtin("display_mouse_get_y", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(PlatformWindow::display_mouse_y()); });
    register_builtin("display_mouse_set", [](Runner&, i32, i32, std::span<const RValue> a) {
        PlatformWindow::set_display_mouse_position(arg_i32(a, 0), arg_i32(a, 1));
        return RValue(0.0);
    });

    register_builtin("screen_redraw", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.render_current_frame();
        r.present_current_frame();
        return RValue(0.0);
    });
    register_builtin("screen_refresh", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.present_current_frame();
        return RValue(0.0);
    });
    register_builtin("screen_wait_vsync", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("screen_save", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto dst = r.vfs().resolve_for_write(arg_str(a, 0));
        return RValue(r.renderer().screen_save(Vfs::path_to_utf8(dst)));
    });
    register_builtin("screen_save_part", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto dst = r.vfs().resolve_for_write(arg_str(a, 0));
        return RValue(r.renderer().screen_save_part(Vfs::path_to_utf8(dst), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_i32(a, 4)));
    });
    register_builtin("set_automatic_draw", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.auto_draw_ = arg_bool(a, 0, true);
        return RValue(0.0);
    });
    register_builtin("set_synchronization", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.sync_vsync_ = arg_bool(a, 0, false);
        r.window().set_vsync(r.sync_vsync_);
        return RValue(0.0);
    });
    register_builtin("set_application_title", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (!a.empty() && a[0].is_string()) {
            Platform::set_application_title(codepage_to_utf8(a[0].as_string(), r.game().meta.string_codepage()));
        }
        return RValue(0.0);
    });
}

} // namespace gm
