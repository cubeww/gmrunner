#include "runtime/runner.h"

#include "base/log.h"
#include "base/math.h"
#include "loader/exe_loader.h"
#include "runtime/collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gm {

namespace {

bool check_key_array(const std::array<bool, 256>& arr, i32 key) noexcept {
    if (key == 0) { // vk_nokey
        for (usize k = 2; k < 256; ++k) {
            if (arr[k]) return false;
        }
        return true;
    }
    if (key == 1) { // vk_anykey
        for (usize k = 2; k < 256; ++k) {
            if (arr[k]) return true;
        }
        return false;
    }
    if (key == 16) return arr[16] || arr[160] || arr[161]; // vk_shift
    if (key == 17) return arr[17] || arr[162] || arr[163]; // vk_control
    if (key == 18) return arr[18] || arr[164] || arr[165]; // vk_alt
    if (key >= 0 && key < 256) return arr[static_cast<usize>(key)];
    return false;
}

bool check_mouse_array(const std::array<bool, 8>& arr, i32 btn) noexcept {
    if (btn == -1) return arr[1] || arr[2] || arr[3]; // mb_any
    if (btn == 0) return !arr[1] && !arr[2] && !arr[3]; // mb_none
    if (btn >= 1 && btn < 8) return arr[static_cast<usize>(btn)];
    return false;
}

CollisionMask build_mask_from_bitmap(const Bitmap32& bmp, bool precise) {
    CollisionMask m{};
    m.width = bmp.width;
    m.height = bmp.height;
    m.pixels.assign(static_cast<usize>(bmp.width) * static_cast<usize>(bmp.height), 0u);
    m.bbox_left = bmp.width;
    m.bbox_top = bmp.height;
    m.bbox_right = 0;
    m.bbox_bottom = 0;

    bool any = false;
    for (i32 y = 0; y < bmp.height; ++y) {
        for (i32 x = 0; x < bmp.width; ++x) {
            const usize idx = static_cast<usize>(y) * static_cast<usize>(bmp.width) + static_cast<usize>(x);
            const u8 alpha = bmp.bgra[idx * 4u + 3u];
            if (alpha > 0) {
                any = true;
                m.pixels[idx] = 1u;
                if (x < m.bbox_left) m.bbox_left = x;
                if (x > m.bbox_right) m.bbox_right = x;
                if (y < m.bbox_top) m.bbox_top = y;
                if (y > m.bbox_bottom) m.bbox_bottom = y;
            }
        }
    }
    if (!any) {
        m.bbox_left = m.bbox_top = m.bbox_right = m.bbox_bottom = 0;
    } else if (!precise) {
        for (i32 y = m.bbox_top; y <= m.bbox_bottom; ++y) {
            for (i32 x = m.bbox_left; x <= m.bbox_right; ++x) {
                m.pixels[static_cast<usize>(y) * static_cast<usize>(bmp.width) + static_cast<usize>(x)] = 1u;
            }
        }
    }
    return m;
}

void remove_bottom_left_bg_color(Bitmap32& bmp) {
    if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgra.size() < 4) return;
    const usize bl_idx = static_cast<usize>(bmp.height - 1) * static_cast<usize>(bmp.width) * 4u;
    const u8 tb = bmp.bgra[bl_idx + 0];
    const u8 tg = bmp.bgra[bl_idx + 1];
    const u8 tr = bmp.bgra[bl_idx + 2];
    const usize count = static_cast<usize>(bmp.width) * static_cast<usize>(bmp.height);
    for (usize i = 0; i < count; ++i) {
        if (bmp.bgra[i * 4 + 0] == tb && bmp.bgra[i * 4 + 1] == tg && bmp.bgra[i * 4 + 2] == tr) {
            bmp.bgra[i * 4 + 3] = 0;
        }
    }
}

bool build_sprite_from_strip(
    const Bitmap32& strip, i32 imgnumb, bool removeback, i32 xorig, i32 yorig, SpriteData& out) {
    if (strip.width <= 0 || strip.height <= 0) return false;
    const i32 count = std::max(1, imgnumb);
    const i32 sub_w = std::max(1, strip.width / count);
    const i32 sub_h = strip.height;

    out.exists = true;
    out.width = sub_w;
    out.height = sub_h;
    out.origin_x = xorig;
    out.origin_y = yorig;
    out.separate_masks = false;
    out.images.clear();
    out.masks.clear();
    out.images.reserve(static_cast<usize>(count));

    for (i32 f = 0; f < count; ++f) {
        Bitmap32 sub{};
        sub.width = sub_w;
        sub.height = sub_h;
        sub.bgra.resize(static_cast<usize>(sub_w) * static_cast<usize>(sub_h) * 4u);
        for (i32 y = 0; y < sub_h; ++y) {
            const u8* src_row = strip.bgra.data() + (static_cast<usize>(y) * static_cast<usize>(strip.width) + static_cast<usize>(f * sub_w)) * 4u;
            u8* dst_row = sub.bgra.data() + static_cast<usize>(y) * static_cast<usize>(sub_w) * 4u;
            std::copy_n(src_row, static_cast<usize>(sub_w) * 4u, dst_row);
        }
        if (removeback) remove_bottom_left_bg_color(sub);
        out.images.push_back(std::move(sub));
    }

    CollisionMask mask = build_mask_from_bitmap(out.images[0], true);
    out.bbox_left = mask.bbox_left;
    out.bbox_right = mask.bbox_right;
    out.bbox_top = mask.bbox_top;
    out.bbox_bottom = mask.bbox_bottom;
    out.masks.push_back(std::move(mask));
    return true;
}

bool load_sprite_file(Runner& r, std::string_view path, SpriteData& sp) {
    auto bytes = r.vfs().read_all_bytes(path);
    if (!bytes) return false;
    auto settings = read_gmspr(bytes.value(), sp);
    if (!settings) return false;
    Runner::rebuild_sprite_masks(sp, settings.value().alpha_tolerance, settings.value().bbox_mode,
                                 sp.bbox_left, sp.bbox_right, sp.bbox_top, sp.bbox_bottom);
    return true;
}

i32 extreme_distance_instance(const Runner& r, i32 target, f64 x, f64 y, bool nearest) {
    if (target >= 100000) return -4; // These two functions accept only an object or all.
    return r.with_instance_selection(target, [&](auto& instances) {
        f64 best_d2 = nearest ? 1.0e10 : -1.0;
        i32 best_id = -4;
        for (const Instance& inst : instances) {
            const f64 dx = inst.x - x, dy = inst.y - y;
            const f64 d2 = dx * dx + dy * dy;
            if (nearest ? d2 < best_d2 : d2 > best_d2) {
                best_d2 = d2;
                best_id = inst.id;
            }
        }
        return best_id;
    });
}

} // namespace

void Runner::register_builtins_gameplay() {
    // -----------------------------------------------------------------------
    // Instances
    // -----------------------------------------------------------------------
    register_builtin("instance_create", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.create_instance(arg_real(a, 0), arg_real(a, 1), arg_i32(a, 2), true));
    });
    register_builtin("instance_destroy", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        r.destroy_instance(self_id, true);
        return RValue(0.0);
    });
    register_builtin("instance_exists", [](Runner& r, i32 self, i32 other, std::span<const RValue> a) {
        const i32 target = arg_i32(a, 0, -4);
        if (target == -1 || target == -2) {
            const auto* inst = r.find_instance(target == -1 ? self : other);
            return RValue(inst && !inst->destroyed);
        }
        return r.with_instance_selection(target, [](auto& group) {
            return RValue(!std::ranges::empty(group));
        });
    });
    register_builtin("instance_number", [](Runner& r, i32 self, i32 other, std::span<const RValue> a) {
        const i32 target = arg_i32(a, 0, -4);
        if (target == -1 || target == -2) {
            const auto* inst = r.find_instance(target == -1 ? self : other);
            return RValue(inst && !inst->destroyed);
        }
        return r.with_instance_selection(target, [](auto& group) {
            return RValue(static_cast<i32>(std::ranges::distance(group)));
        });
    });
    register_builtin("instance_find", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 target = arg_i32(a, 0, -4);
        const i32 n = arg_i32(a, 1, 0);
        if (target == -4 || n < 0) return RValue(-4);
        i32 index = 0;
        return RValue(r.find_matching_instance(target, [&](const Instance&) { return index++ == n; }));
    });
    register_builtin("instance_position", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const i32 target = arg_i32(a, 2, -3);
        return RValue(r.find_matching_instance(target, [&](const Instance& inst) {
            return check_point_collision(inst, px, py, true, r.game(), r.version_mode());
        }));
    });
    register_builtin("instance_place", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(-4);
        return RValue(r.find_collision_instance(*self, arg_real(a, 0), arg_real(a, 1), arg_i32(a, 2, -3), false));
    });
    register_builtin("instance_nearest", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(extreme_distance_instance(r, arg_i32(a, 2, -3), arg_real(a, 0), arg_real(a, 1), true));
    });
    register_builtin("instance_furthest", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(extreme_distance_instance(r, arg_i32(a, 2, -3), arg_real(a, 0), arg_real(a, 1), false));
    });
    register_builtin("instance_copy", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        return RValue(r.copy_instance(self_id, arg_bool(a, 0, true)));
    });
    register_builtin("instance_change", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.change_instance(self_id, arg_i32(a, 0, -1), arg_bool(a, 1, true));
        return RValue(0.0);
    });
    register_builtin("instance_deactivate_all", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.deactivate_all(arg_bool(a, 0, true), self_id);
        return RValue(0.0);
    });
    register_builtin("instance_deactivate_object", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        r.deactivate_object(arg_i32(a, 0, -4), self_id, other_id);
        return RValue(0.0);
    });
    register_builtin("instance_deactivate_region", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.deactivate_region(arg_real(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3),
                            arg_bool(a, 4, true), arg_bool(a, 5, true), self_id);
        return RValue(0.0);
    });
    register_builtin("instance_activate_all", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.activate_all();
        return RValue(0.0);
    });
    register_builtin("instance_activate_object", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        r.activate_object(arg_i32(a, 0, -4), self_id, other_id);
        return RValue(0.0);
    });
    register_builtin("instance_activate_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.activate_region(arg_real(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), arg_bool(a, 4, true));
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Motion & Collisions
    // -----------------------------------------------------------------------
    register_builtin("motion_set", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            f64 d = std::fmod(arg_real(a, 0), 360.0);
            if (d < 0.0) d += 360.0;
            inst->direction = d;
            inst->speed = arg_real(a, 1);
            inst->sync_hv_from_polar();
        }
        return RValue(0.0);
    });
    register_builtin("motion_add", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            const f64 rad = deg_to_rad(arg_real(a, 0));
            const f64 sp = arg_real(a, 1);
            inst->hspeed += sp * std::cos(rad);
            inst->vspeed -= sp * std::sin(rad);
            inst->sync_polar_from_hv();
        }
        return RValue(0.0);
    });
    register_builtin("place_free", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        return RValue(self ? r.check_place_free(*self, arg_real(a, 0), arg_real(a, 1)) : true);
    });
    register_builtin("place_empty", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        return RValue(self ? r.check_place_empty(*self, arg_real(a, 0), arg_real(a, 1)) : true);
    });
    register_builtin("place_meeting", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        return RValue(self ? r.check_place_meeting(*self, arg_real(a, 0), arg_real(a, 1), arg_i32(a, 2, -3), r.find_instance(other_id)) : false);
    });
    register_builtin("place_snapped", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        const f64 hs = std::max(1.0, arg_real(a, 0, 1.0));
        const f64 vs = std::max(1.0, arg_real(a, 1, 1.0));
        return RValue(std::fabs(std::fmod(self->x, hs)) < 1e-6 && std::fabs(std::fmod(self->y, vs)) < 1e-6);
    });
    register_builtin("move_snap", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) {
            const f64 hs = arg_real(a, 0, 1.0);
            const f64 vs = arg_real(a, 1, 1.0);
            if (hs > 0.0) self->x = std::floor(self->x / hs + 0.5) * hs;
            if (vs > 0.0) self->y = std::floor(self->y / vs + 0.5) * vs;
            self->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("move_random", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) {
            const f64 hs = std::max(1.0, arg_real(a, 0, 1.0));
            const f64 vs = std::max(1.0, arg_real(a, 1, 1.0));
            self->x = std::floor(r.random_uniform(static_cast<f64>(r.room_width_)) / hs) * hs;
            self->y = std::floor(r.random_uniform(static_cast<f64>(r.room_height_)) / vs) * vs;
            self->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("move_towards_point", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) {
            const f64 dx = arg_real(a, 0) - self->x;
            const f64 dy = arg_real(a, 1) - self->y;
            const f64 sp = arg_real(a, 2);
            f64 deg = rad_to_deg(std::atan2(-dy, dx));
            if (deg < 0.0) deg += 360.0;
            self->direction = std::fmod(deg, 360.0);
            self->speed = sp;
            self->sync_hv_from_polar();
        }
        return RValue(0.0);
    });
    register_builtin("move_contact_solid", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) r.move_contact(*self, arg_real(a, 0), arg_real(a, 1, 1000.0), true);
        return RValue(0.0);
    });
    register_builtin("move_contact_all", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) r.move_contact(*self, arg_real(a, 0), arg_real(a, 1, 1000.0), false);
        return RValue(0.0);
    });
    register_builtin("move_outside_solid", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) r.move_outside(*self, arg_real(a, 0), arg_real(a, 1, 1000.0), true);
        return RValue(0.0);
    });
    register_builtin("move_outside_all", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) r.move_outside(*self, arg_real(a, 0), arg_real(a, 1, 1000.0), false);
        return RValue(0.0);
    });
    register_builtin("move_bounce_solid", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) r.move_bounce(*self, arg_bool(a, 0, false), true);
        return RValue(0.0);
    });
    register_builtin("move_bounce_all", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) r.move_bounce(*self, arg_bool(a, 0, false), false);
        return RValue(0.0);
    });
    register_builtin("move_wrap", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) {
            const bool hor = arg_bool(a, 0, true);
            const bool ver = arg_bool(a, 1, true);
            const f64 margin = arg_real(a, 2, 0.0);
            if (hor) {
                if (self->hspeed < 0.0 && self->x < -margin) self->x += r.room_width_ + 2.0 * margin;
                else if (self->hspeed > 0.0 && self->x > r.room_width_ + margin) self->x -= r.room_width_ + 2.0 * margin;
            }
            if (ver) {
                if (self->vspeed < 0.0 && self->y < -margin) self->y += r.room_height_ + 2.0 * margin;
                else if (self->vspeed > 0.0 && self->y > r.room_height_ + margin) self->y -= r.room_height_ + 2.0 * margin;
            }
            self->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("distance_to_point", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(0.0);
        ensure_instance_bbox(*self, r.game());
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const f64 dx = std::max({0.0, static_cast<f64>(self->bbox_left) - px, px - static_cast<f64>(self->bbox_right)});
        const f64 dy = std::max({0.0, static_cast<f64>(self->bbox_top) - py, py - static_cast<f64>(self->bbox_bottom)});
        return RValue(std::hypot(dx, dy));
    });
    register_builtin("distance_to_object", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(1000000.0);
        ensure_instance_bbox(*self, r.game());
        const i32 target = arg_i32(a, 0, -4);
        f64 best = 1000000.0;
        const auto measure = [&](const Instance& inst) {
            if (&inst == self) return;
            ensure_instance_bbox(inst, r.game());
            const f64 dx = std::max({0.0, static_cast<f64>(self->bbox_left) - inst.bbox_right,
                                     static_cast<f64>(inst.bbox_left) - self->bbox_right});
            const f64 dy = std::max({0.0, static_cast<f64>(self->bbox_top) - inst.bbox_bottom,
                                     static_cast<f64>(inst.bbox_top) - self->bbox_bottom});
            best = std::min(best, std::hypot(dx, dy));
        };
        if (target >= 100000 || target == -2) {
            // Like with(id), this query stops at the first room slot with the
            // requested ID even if instance_change has marked that slot dead.
            const i32 handle = target == -2 ? other_id : r.target_instance_at(target, 0, TargetUse::With);
            if (const Instance* inst = r.find_instance(handle); inst && !inst->destroyed) measure(*inst);
            return RValue(best);
        }
        return r.with_instance_selection(target, [&](auto& instances) {
            for (const Instance& inst : instances) measure(inst);
            return RValue(best);
        });
    });
    register_builtin("position_empty", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        return RValue(r.find_matching_instance(-3, [&](const Instance& inst) {
            return check_point_collision(inst, px, py, true, r.game(), r.version_mode());
        }) == -4);
    });
    register_builtin("position_meeting", [](Runner& r, i32 self, i32 other, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const i32 target = arg_i32(a, 2, -3);
        return RValue(r.find_matching_instance(target, [&](const Instance& inst) {
            return check_point_collision(inst, px, py, true, r.game(), r.version_mode());
        }, r.find_instance(self), r.find_instance(other)) != -4);
    });
    register_builtin("position_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        r.for_instances_at_position(px, py, [&](i32 handle) { r.destroy_instance(handle, true); });
        return RValue(0.0);
    });
    register_builtin("position_change", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const i32 obj = arg_i32(a, 2, -1);
        const bool perf = arg_bool(a, 3, true);
        r.for_instances_at_position(px, py, [&](i32 handle) { r.change_instance(handle, obj, perf); });
        return RValue(0.0);
    });

    // Shape collision queries
    register_builtin("collision_point", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 x = arg_real(a, 0), y = arg_real(a, 1);
        const i32 target = arg_i32(a, 2, -3);
        const bool prec = arg_bool(a, 3, true), notme = arg_bool(a, 4, true);
        const Instance* excluded = notme ? r.find_instance(self_id) : nullptr;
        return RValue(r.find_matching_instance(target, [&](const Instance& inst) {
            return &inst != excluded && check_point_collision(inst, x, y, prec, r.game(), r.version_mode());
        }));
    });
    register_builtin("collision_rectangle", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 x1 = arg_real(a, 0), y1 = arg_real(a, 1), x2 = arg_real(a, 2), y2 = arg_real(a, 3);
        const i32 target = arg_i32(a, 4, -3);
        const bool prec = arg_bool(a, 5, true), notme = arg_bool(a, 6, true);
        const Instance* excluded = notme ? r.find_instance(self_id) : nullptr;
        return RValue(r.find_matching_instance(target, [&](const Instance& inst) {
            return &inst != excluded && check_rect_collision(inst, x1, y1, x2, y2, prec, r.game(), r.version_mode());
        }));
    });
    register_builtin("collision_circle", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 cx = arg_real(a, 0), cy = arg_real(a, 1), rad = arg_real(a, 2);
        const i32 target = arg_i32(a, 3, -3);
        const bool prec = arg_bool(a, 4, true), notme = arg_bool(a, 5, true);
        const Instance* excluded = notme ? r.find_instance(self_id) : nullptr;
        return RValue(r.find_matching_instance(target, [&](const Instance& inst) {
            return &inst != excluded && check_circle_collision(inst, cx, cy, rad, prec, r.game(), r.version_mode());
        }));
    });
    register_builtin("collision_ellipse", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 x1 = arg_real(a, 0), y1 = arg_real(a, 1), x2 = arg_real(a, 2), y2 = arg_real(a, 3);
        const i32 target = arg_i32(a, 4, -3);
        const bool prec = arg_bool(a, 5, true), notme = arg_bool(a, 6, true);
        const Instance* excluded = notme ? r.find_instance(self_id) : nullptr;
        return RValue(r.find_matching_instance(target, [&](const Instance& inst) {
            return &inst != excluded && check_ellipse_collision(inst, x1, y1, x2, y2, prec, r.game(), r.version_mode());
        }));
    });
    register_builtin("collision_line", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 x1 = arg_real(a, 0), y1 = arg_real(a, 1), x2 = arg_real(a, 2), y2 = arg_real(a, 3);
        const i32 target = arg_i32(a, 4, -3);
        const bool prec = arg_bool(a, 5, true), notme = arg_bool(a, 6, true);
        const Instance* excluded = notme ? r.find_instance(self_id) : nullptr;
        return RValue(r.find_matching_instance(target, [&](const Instance& inst) {
            return &inst != excluded && check_line_collision(inst, x1, y1, x2, y2, prec, r.game(), r.version_mode());
        }));
    });

    // -----------------------------------------------------------------------
    // Paths & Motion Planning
    // -----------------------------------------------------------------------
    register_builtin("path_start", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) {
            r.start_path(*self, arg_i32(a, 0, -1), arg_real(a, 1), arg_i32(a, 2), arg_bool(a, 3));
        }
        return RValue(0.0);
    });
    register_builtin("path_end", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (auto* self = r.find_instance(self_id)) r.end_path(*self);
        return RValue(0.0);
    });
    register_builtin("path_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists);
    });
    register_builtin("path_get_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            return RValue(r.game().paths[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    });
    register_builtin("path_get_length", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            return RValue(r.game().paths[static_cast<usize>(id)].length());
        }
        return RValue(-1.0);
    });
    register_builtin("path_get_x", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            return RValue(r.game().paths[static_cast<usize>(id)].sample(arg_real(a, 1)).x);
        }
        return RValue(-1.0);
    });
    register_builtin("path_get_y", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            return RValue(r.game().paths[static_cast<usize>(id)].sample(arg_real(a, 1)).y);
        }
        return RValue(-1.0);
    });
    register_builtin("path_get_number", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            return RValue(static_cast<i32>(r.game().paths[static_cast<usize>(id)].geometry().points.size()));
        }
        return RValue(0);
    });
    register_builtin("path_add", [](Runner& r, i32, i32, std::span<const RValue>) {
        PathData p{};
        p.exists = true;
        const i32 id = static_cast<i32>(r.game().paths.size());
        r.game().paths.push_back(std::move(p));
        return RValue(id);
    });
    register_builtin("path_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size()) r.game().paths[static_cast<usize>(id)].exists = false;
        return RValue(0.0);
    });
    register_builtin("path_add_point", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            r.game().paths[static_cast<usize>(id)].edit_geometry().points.push_back(PathPoint{arg_real(a, 1), arg_real(a, 2), arg_real(a, 3, 100.0)});
        }
        return RValue(0.0);
    });
    register_builtin("path_clear_points", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size()) r.game().paths[static_cast<usize>(id)].edit_geometry().points.clear();
        return RValue(0.0);
    });
    register_builtin("path_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            return RValue(r.game().paths[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    });
    register_builtin("path_get_kind", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists)
                          ? r.game().paths[static_cast<usize>(id)].geometry().kind : 0);
    });
    register_builtin("path_get_closed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists &&
                      r.game().paths[static_cast<usize>(id)].geometry().closed);
    });
    register_builtin("path_get_precision", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists)
                          ? r.game().paths[static_cast<usize>(id)].geometry().precision : 4);
    });
    register_builtin("path_get_speed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().paths.size() || !r.game().paths[static_cast<usize>(id)].exists) return RValue(-1.0);
        return RValue(r.game().paths[static_cast<usize>(id)].sample(arg_real(a, 1)).speed);
    });
    register_builtin("path_get_point_x", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1), n = arg_i32(a, 1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists &&
            n >= 0 && static_cast<usize>(n) < r.game().paths[static_cast<usize>(id)].geometry().points.size()) {
            return RValue(r.game().paths[static_cast<usize>(id)].geometry().points[static_cast<usize>(n)].x);
        }
        return RValue(0.0);
    });
    register_builtin("path_get_point_y", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1), n = arg_i32(a, 1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists &&
            n >= 0 && static_cast<usize>(n) < r.game().paths[static_cast<usize>(id)].geometry().points.size()) {
            return RValue(r.game().paths[static_cast<usize>(id)].geometry().points[static_cast<usize>(n)].y);
        }
        return RValue(0.0);
    });
    register_builtin("path_get_point_speed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1), n = arg_i32(a, 1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists &&
            n >= 0 && static_cast<usize>(n) < r.game().paths[static_cast<usize>(id)].geometry().points.size()) {
            return RValue(r.game().paths[static_cast<usize>(id)].geometry().points[static_cast<usize>(n)].speed);
        }
        return RValue(0.0);
    });
    register_builtin("path_set_kind", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists)
            r.game().paths[static_cast<usize>(id)].edit_geometry().kind = arg_i32(a, 1) == 1 ? 1 : 0;
        return RValue(0.0);
    });
    register_builtin("path_set_closed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists)
            r.game().paths[static_cast<usize>(id)].edit_geometry().closed = arg_bool(a, 1);
        return RValue(0.0);
    });
    register_builtin("path_set_precision", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists)
            r.game().paths[static_cast<usize>(id)].edit_geometry().precision = std::clamp(arg_i32(a, 1, 4), 0, 8);
        return RValue(0.0);
    });
    register_builtin("path_duplicate", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().paths.size() || !r.game().paths[static_cast<usize>(id)].exists) return RValue(-1);
        PathData copy = r.game().paths[static_cast<usize>(id)];
        const i32 nid = static_cast<i32>(r.game().paths.size());
        r.game().paths.push_back(std::move(copy));
        return RValue(nid);
    });
    register_builtin("path_assign", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1), src = arg_i32(a, 1, -1);
        if (dst >= 0 && static_cast<usize>(dst) < r.game().paths.size() &&
            src >= 0 && static_cast<usize>(src) < r.game().paths.size() && r.game().paths[static_cast<usize>(src)].exists) {
            std::string keep_name = r.game().paths[static_cast<usize>(dst)].name;
            r.game().paths[static_cast<usize>(dst)] = r.game().paths[static_cast<usize>(src)];
            r.game().paths[static_cast<usize>(dst)].name = std::move(keep_name);
        }
        return RValue(0.0);
    });
    register_builtin("path_append", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1), src = arg_i32(a, 1, -1);
        if (dst >= 0 && static_cast<usize>(dst) < r.game().paths.size() && r.game().paths[static_cast<usize>(dst)].exists &&
            src >= 0 && static_cast<usize>(src) < r.game().paths.size() && r.game().paths[static_cast<usize>(src)].exists) {
            const auto src_pts = r.game().paths[static_cast<usize>(src)].geometry().points;
            auto& dst_pts = r.game().paths[static_cast<usize>(dst)].edit_geometry().points;
            dst_pts.insert(dst_pts.end(), src_pts.begin(), src_pts.end());
        }
        return RValue(0.0);
    });
    register_builtin("path_insert_point", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1), n = arg_i32(a, 1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            auto& pts = r.game().paths[static_cast<usize>(id)].edit_geometry().points;
            const usize idx = static_cast<usize>(std::clamp(n, 0, static_cast<i32>(pts.size())));
            pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(idx), PathPoint{arg_real(a, 2), arg_real(a, 3), arg_real(a, 4, 100.0)});
        }
        return RValue(0.0);
    });
    register_builtin("path_change_point", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1), n = arg_i32(a, 1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists &&
            n >= 0 && static_cast<usize>(n) < r.game().paths[static_cast<usize>(id)].edit_geometry().points.size()) {
            r.game().paths[static_cast<usize>(id)].edit_geometry().points[static_cast<usize>(n)] = PathPoint{arg_real(a, 2), arg_real(a, 3), arg_real(a, 4, 100.0)};
        }
        return RValue(0.0);
    });
    register_builtin("path_delete_point", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1), n = arg_i32(a, 1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists &&
            n >= 0 && static_cast<usize>(n) < r.game().paths[static_cast<usize>(id)].edit_geometry().points.size()) {
            r.game().paths[static_cast<usize>(id)].edit_geometry().points.erase(r.game().paths[static_cast<usize>(id)].edit_geometry().points.begin() + n);
        }
        return RValue(0.0);
    });
    register_builtin("path_reverse", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            auto& pts = r.game().paths[static_cast<usize>(id)].edit_geometry().points;
            std::reverse(pts.begin(), pts.end());
        }
        return RValue(0.0);
    });
    auto path_center = [](const PathGeometry& p, f64& cx, f64& cy) {
        cx = cy = 0.0;
        if (p.points.empty()) return;
        f64 x0 = p.points[0].x, x1 = x0, y0 = p.points[0].y, y1 = y0;
        for (const auto& pt : p.points) {
            x0 = std::min(x0, pt.x); x1 = std::max(x1, pt.x);
            y0 = std::min(y0, pt.y); y1 = std::max(y1, pt.y);
        }
        cx = (x0 + x1) * 0.5;
        cy = (y0 + y1) * 0.5;
    };
    register_builtin("path_mirror", [path_center](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            auto& p = r.game().paths[static_cast<usize>(id)].edit_geometry();
            f64 cx = 0, cy = 0; path_center(p, cx, cy);
            for (auto& pt : p.points) pt.x = 2.0 * cx - pt.x;
        }
        return RValue(0.0);
    });
    register_builtin("path_flip", [path_center](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            auto& p = r.game().paths[static_cast<usize>(id)].edit_geometry();
            f64 cx = 0, cy = 0; path_center(p, cx, cy);
            for (auto& pt : p.points) pt.y = 2.0 * cy - pt.y;
        }
        return RValue(0.0);
    });
    register_builtin("path_rotate", [path_center](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            auto& p = r.game().paths[static_cast<usize>(id)].edit_geometry();
            f64 cx = 0, cy = 0; path_center(p, cx, cy);
            const f64 rad = deg_to_rad(arg_real(a, 1));
            const f64 c = std::cos(rad), s = std::sin(rad);
            for (auto& pt : p.points) {
                const f64 dx = pt.x - cx, dy = pt.y - cy;
                pt.x = cx + dx * c + dy * s;
                pt.y = cy - dx * s + dy * c;
            }
        }
        return RValue(0.0);
    });
    register_builtin("path_scale", [path_center](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            auto& p = r.game().paths[static_cast<usize>(id)].edit_geometry();
            f64 cx = 0, cy = 0; path_center(p, cx, cy);
            const f64 sx = arg_real(a, 1, 1.0), sy = arg_real(a, 2, 1.0);
            for (auto& pt : p.points) {
                pt.x = cx + (pt.x - cx) * sx;
                pt.y = cy + (pt.y - cy) * sy;
            }
        }
        return RValue(0.0);
    });
    register_builtin("path_shift", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            const f64 dx = arg_real(a, 1), dy = arg_real(a, 2);
            for (auto& pt : r.game().paths[static_cast<usize>(id)].edit_geometry().points) { pt.x += dx; pt.y += dy; }
        }
        return RValue(0.0);
    });

    // Legacy movement & instance helper aliases
    register_builtin("move_contact", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) r.move_contact(*self, arg_real(a, 0), arg_real(a, 1, -1.0), true);
        return RValue(0.0);
    });
    register_builtin("move_bounce", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* self = r.find_instance(self_id)) r.move_bounce(*self, arg_bool(a, 0, false), true);
        return RValue(0.0);
    });
    register_builtin("instance_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto* inst = r.find_instance(arg_i32(a, 0, -4));
        return RValue(inst ? inst->sprite_index : -1);
    });
    register_builtin("transition_define", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.transition_scripts_[arg_i32(a, 0)] = arg_i32(a, 1, -1);
        return RValue(0.0);
    });
    register_builtin("transition_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 k = arg_i32(a, 0);
        return RValue((k >= 0 && k <= 21) || r.transition_scripts_.contains(k));
    });

    // MP Step, Potential & Grid
    register_builtin("mp_linear_step", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        return RValue(r.mp_linear_step(*self, arg_real(a, 0), arg_real(a, 1), arg_real(a, 2), arg_bool(a, 3, false), -3));
    });
    register_builtin("mp_linear_step_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        return RValue(r.mp_linear_step(*self, arg_real(a, 0), arg_real(a, 1), arg_real(a, 2), true, arg_i32(a, 3, -3)));
    });
    register_builtin("mp_linear_path", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        return RValue(r.mp_linear_path(*self, arg_i32(a, 0, -1), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), arg_bool(a, 4, false), -3));
    });
    register_builtin("mp_linear_path_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        return RValue(r.mp_linear_path(*self, arg_i32(a, 0, -1), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), true, arg_i32(a, 4, -3)));
    });
    register_builtin("mp_potential_settings", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.mp_potential_maxrot_ = std::max(1.0, arg_real(a, 0, 30.0));
        r.mp_potential_rotstep_ = std::max(1.0, arg_real(a, 1, 10.0));
        r.mp_potential_ahead_ = std::max(1.0, arg_real(a, 2, 3.0));
        r.mp_potential_onspot_ = arg_bool(a, 3, true);
        return RValue(0.0);
    });
    register_builtin("mp_potential_step", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        return RValue(r.mp_potential_step(*self, arg_real(a, 0), arg_real(a, 1), arg_real(a, 2), arg_bool(a, 3, false), -3));
    });
    register_builtin("mp_potential_step_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        return RValue(r.mp_potential_step(*self, arg_real(a, 0), arg_real(a, 1), arg_real(a, 2), true, arg_i32(a, 3, -3)));
    });
    register_builtin("mp_potential_path", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        return RValue(r.mp_potential_path(*self, arg_i32(a, 0, -1), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4, 4.0), arg_bool(a, 5, false), -3));
    });
    register_builtin("mp_potential_path_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        return RValue(r.mp_potential_path(*self, arg_i32(a, 0, -1), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4, 4.0), true, arg_i32(a, 5, -3)));
    });
    register_builtin("mp_grid_create", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.mp_grids().create(arg_real(a, 0), arg_real(a, 1), arg_i32(a, 2), arg_i32(a, 3), arg_real(a, 4), arg_real(a, 5)));
    });
    register_builtin("mp_grid_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.mp_grids().destroy(arg_i32(a, 0));
        return RValue(0.0);
    });
    register_builtin("mp_grid_clear_all", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.mp_grids().clear_all(arg_i32(a, 0));
        return RValue(0.0);
    });
    register_builtin("mp_grid_clear_cell", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.mp_grids().clear_cell(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2));
        return RValue(0.0);
    });
    register_builtin("mp_grid_clear_rectangle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.mp_grids().clear_rectangle(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4));
        return RValue(0.0);
    });
    register_builtin("mp_grid_add_cell", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.mp_grids().add_cell(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2));
        return RValue(0.0);
    });
    register_builtin("mp_grid_add_rectangle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.mp_grids().add_rectangle(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4));
        return RValue(0.0);
    });
    register_builtin("mp_grid_add_instances", [](Runner& r, i32 self, i32 other, std::span<const RValue> a) {
        const i32 gid = arg_i32(a, 0);
        const i32 target = arg_i32(a, 1, -3);
        const bool prec = arg_bool(a, 2, false);
        auto* g = r.mp_grids().get(gid);
        if (!g) return RValue(0.0);
        r.with_instance_selection(target, [&](auto& instances) {
            for (const Instance& inst : instances) {
                ensure_instance_bbox(inst, r.game());
                if (!prec) {
                    r.mp_grids().add_rectangle(gid, inst.bbox_left, inst.bbox_top, inst.bbox_right, inst.bbox_bottom);
                } else {
                    const i32 x0 = std::max(0, static_cast<i32>(std::floor((inst.bbox_left - g->left) / g->cell_w)));
                    const i32 y0 = std::max(0, static_cast<i32>(std::floor((inst.bbox_top - g->top) / g->cell_h)));
                    const i32 x1 = std::min(g->hcells - 1, static_cast<i32>(std::floor((inst.bbox_right - g->left) / g->cell_w)));
                    const i32 y1 = std::min(g->vcells - 1, static_cast<i32>(std::floor((inst.bbox_bottom - g->top) / g->cell_h)));
                    for (i32 cy = y0; cy <= y1; ++cy) {
                        for (i32 cx = x0; cx <= x1; ++cx) {
                            const f64 rx1 = g->left + cx * g->cell_w;
                            const f64 ry1 = g->top + cy * g->cell_h;
                            if (check_rect_collision(inst, rx1, ry1, rx1 + g->cell_w, ry1 + g->cell_h, true, r.game(), r.version_mode())) {
                                r.mp_grids().add_cell(gid, cx, cy);
                            }
                        }
                    }
                }
            }
        }, r.find_instance(self), r.find_instance(other));
        return RValue(0.0);
    });
    register_builtin("mp_grid_path", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 gid = arg_i32(a, 0);
        const i32 pid = arg_i32(a, 1, -1);
        if (pid < 0 || static_cast<usize>(pid) >= r.game().paths.size() || !r.game().paths[static_cast<usize>(pid)].exists) {
            return RValue(false);
        }
        return RValue(r.mp_grids().find_path(gid, r.game().paths[static_cast<usize>(pid)],
                                             arg_real(a, 2), arg_real(a, 3), arg_real(a, 4), arg_real(a, 5),
                                             arg_bool(a, 6, true)));
    });
    register_builtin("mp_grid_draw", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto* g = r.mp_grids().get(arg_i32(a, 0));
        if (!g) return RValue(false);
        const f32 alpha = r.renderer().alpha();
        for (i32 cy = 0; cy < g->vcells; ++cy) {
            for (i32 cx = 0; cx < g->hcells; ++cx) {
                const bool blocked = g->cells[static_cast<usize>(cy) * static_cast<usize>(g->hcells) + static_cast<usize>(cx)] != 0;
                const u32 col = blocked ? 0x0000FFu : 0x00FF00u; // red if blocked, lime if free
                const f32 x1 = static_cast<f32>(g->left + cx * g->cell_w);
                const f32 y1 = static_cast<f32>(g->top + cy * g->cell_h);
                const f32 x2 = x1 + static_cast<f32>(g->cell_w) - 1.0f;
                const f32 y2 = y1 + static_cast<f32>(g->cell_h) - 1.0f;
                r.renderer().draw_rectangle_color(x1, y1, x2, y2, col, col, col, col, false, alpha);
            }
        }
        return RValue(true);
    });

    // -----------------------------------------------------------------------
    // Rooms & Events
    // -----------------------------------------------------------------------
    register_builtin("room_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.room_exists(arg_i32(a, 0, -1)));
    });
    register_builtin("room_get_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.room_exists(id) ? r.game().rooms[static_cast<usize>(id)].name : std::string("<undefined>"));
    });
    register_builtin("room_goto", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.request_room_goto(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("room_goto_previous", [](Runner& r, i32, i32, std::span<const RValue>) {
        const i32 prev = r.room_previous_id(r.current_room_);
        if (prev >= 0) r.request_room_goto(prev);
        return RValue(0.0);
    });
    register_builtin("room_goto_next", [](Runner& r, i32, i32, std::span<const RValue>) {
        const i32 nxt = r.room_next_id(r.current_room_);
        if (nxt >= 0) r.request_room_goto(nxt);
        return RValue(0.0);
    });
    register_builtin("room_restart", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.request_room_restart();
        return RValue(0.0);
    });
    register_builtin("room_previous", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.room_previous_id(arg_i32(a, 0, -1)));
    });
    register_builtin("room_next", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.room_next_id(arg_i32(a, 0, -1)));
    });
    register_builtin("room_set_width", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.room_exists(id)) r.game().rooms[static_cast<usize>(id)].width = std::max(1, arg_i32(a, 1));
        return RValue(0.0);
    });
    register_builtin("room_set_height", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.room_exists(id)) r.game().rooms[static_cast<usize>(id)].height = std::max(1, arg_i32(a, 1));
        return RValue(0.0);
    });
    register_builtin("room_set_caption", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.room_exists(id)) r.game().rooms[static_cast<usize>(id)].caption = std::string(arg_str(a, 1));
        return RValue(0.0);
    });
    register_builtin("room_set_persistent", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.room_exists(id)) r.game().rooms[static_cast<usize>(id)].persistent = arg_bool(a, 1);
        return RValue(0.0);
    });
    register_builtin("room_set_background_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.room_exists(id)) {
            r.game().rooms[static_cast<usize>(id)].color = static_cast<u32>(arg_i32(a, 1)) & 0x00FFFFFFu;
            r.game().rooms[static_cast<usize>(id)].show_color = arg_bool(a, 2, true);
        }
        return RValue(0.0);
    });
    register_builtin("room_set_view_enabled", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.room_exists(id)) r.game().rooms[static_cast<usize>(id)].enable_views = arg_bool(a, 1);
        return RValue(0.0);
    });
    register_builtin("room_instance_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (!r.room_exists(id)) return RValue(-4);
        RoomInstance ri{};
        ri.x = arg_i32(a, 1);
        ri.y = arg_i32(a, 2);
        ri.object_index = arg_i32(a, 3, -1);
        ri.id = r.alloc_instance_id();
        r.game().room_max_id = std::max(r.game().room_max_id, ri.id);
        const i32 assigned_id = ri.id;
        r.game().rooms[static_cast<usize>(id)].instances.push_back(std::move(ri));
        return RValue(assigned_id);
    });
    register_builtin("room_instance_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.room_exists(id)) r.game().rooms[static_cast<usize>(id)].instances.clear();
        return RValue(0.0);
    });
    register_builtin("room_add", [](Runner& r, i32, i32, std::span<const RValue>) {
        RoomData rm{};
        rm.exists = true;
        const i32 id = static_cast<i32>(r.game().rooms.size());
        r.game().rooms.push_back(std::move(rm));
        return RValue(id);
    });
    register_builtin("room_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.room_exists(id) ? r.game().rooms[static_cast<usize>(id)].name : std::string("<undefined>"));
    });
    register_builtin("room_duplicate", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (!r.room_exists(id)) return RValue(-1);
        RoomData copy = r.game().rooms[static_cast<usize>(id)];
        const i32 nid = static_cast<i32>(r.game().rooms.size());
        r.game().rooms.push_back(std::move(copy));
        if (static_cast<usize>(id) < r.program().room_creation_chunks.size()) {
            if (r.program().room_creation_chunks.size() <= static_cast<usize>(nid)) {
                r.program().room_creation_chunks.resize(static_cast<usize>(nid) + 1);
            }
            r.program().room_creation_chunks[static_cast<usize>(nid)] = r.program().room_creation_chunks[static_cast<usize>(id)];
        }
        return RValue(nid);
    });
    register_builtin("room_assign", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1), src = arg_i32(a, 1, -1);
        if (r.room_exists(dst) && r.room_exists(src)) {
            std::string keep_name = r.game().rooms[static_cast<usize>(dst)].name;
            r.game().rooms[static_cast<usize>(dst)] = r.game().rooms[static_cast<usize>(src)];
            r.game().rooms[static_cast<usize>(dst)].name = std::move(keep_name);
            if (static_cast<usize>(src) < r.program().room_creation_chunks.size()) {
                if (r.program().room_creation_chunks.size() <= static_cast<usize>(dst)) {
                    r.program().room_creation_chunks.resize(static_cast<usize>(dst) + 1);
                }
                r.program().room_creation_chunks[static_cast<usize>(dst)] = r.program().room_creation_chunks[static_cast<usize>(src)];
            }
        }
        return RValue(0.0);
    });
    register_builtin("room_set_code", [](Runner& r, i32, i32, std::span<const RValue> a) {
        (void)r.room_set_code(arg_i32(a, 0, -1), arg_str(a, 1));
        return RValue(0.0);
    });
    register_builtin("room_set_background", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        const i32 bidx = arg_i32(a, 1);
        if (!r.room_exists(id) || bidx < 0 || bidx >= 8) return RValue(0.0);
        auto& bgs = r.game().rooms[static_cast<usize>(id)].backgrounds;
        if (bgs.size() < 8) bgs.resize(8);
        auto& b = bgs[static_cast<usize>(bidx)];
        b.visible = arg_bool(a, 2);
        b.foreground = arg_bool(a, 3);
        b.background_index = arg_i32(a, 4, -1);
        b.x = arg_i32(a, 5);
        b.y = arg_i32(a, 6);
        b.htiled = arg_bool(a, 7);
        b.vtiled = arg_bool(a, 8);
        b.hspeed = arg_i32(a, 9);
        b.vspeed = arg_i32(a, 10);
        b.stretch = arg_bool(a, 11);
        return RValue(0.0);
    });
    register_builtin("room_set_view", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        const i32 vidx = arg_i32(a, 1);
        if (!r.room_exists(id) || vidx < 0 || vidx >= 8) return RValue(0.0);
        auto& vs = r.game().rooms[static_cast<usize>(id)].views;
        if (vs.size() < 8) vs.resize(8);
        auto& v = vs[static_cast<usize>(vidx)];
        v.visible = arg_bool(a, 2);
        v.xview = arg_i32(a, 3);
        v.yview = arg_i32(a, 4);
        v.wview = arg_i32(a, 5);
        v.hview = arg_i32(a, 6);
        v.xport = arg_i32(a, 7);
        v.yport = arg_i32(a, 8);
        v.wport = arg_i32(a, 9);
        v.hport = arg_i32(a, 10);
        v.hborder = arg_i32(a, 11);
        v.vborder = arg_i32(a, 12);
        v.hspeed = arg_i32(a, 13, -1);
        v.vspeed = arg_i32(a, 14, -1);
        v.follow_object = arg_i32(a, 15, -1);
        return RValue(0.0);
    });
    register_builtin("room_tile_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (!r.room_exists(id)) return RValue(-1);
        RoomTile t{};
        t.background_index = arg_i32(a, 1, -1);
        t.xo = arg_i32(a, 2); t.yo = arg_i32(a, 3);
        t.w = arg_i32(a, 4); t.h = arg_i32(a, 5);
        t.x = arg_i32(a, 6); t.y = arg_i32(a, 7);
        t.depth = arg_i32(a, 8);
        t.id = ++r.game().room_max_tile_id;
        r.game().rooms[static_cast<usize>(id)].tiles.push_back(t);
        return RValue(t.id);
    });
    register_builtin("room_tile_add_ext", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (!r.room_exists(id)) return RValue(-1);
        RoomTile t{};
        t.background_index = arg_i32(a, 1, -1);
        t.xo = arg_i32(a, 2); t.yo = arg_i32(a, 3);
        t.w = arg_i32(a, 4); t.h = arg_i32(a, 5);
        t.x = arg_i32(a, 6); t.y = arg_i32(a, 7);
        t.depth = arg_i32(a, 8);
        t.scale_x = arg_real(a, 9, 1.0);
        t.scale_y = arg_real(a, 10, 1.0);
        t.alpha = arg_real(a, 11, 1.0);
        t.id = ++r.game().room_max_tile_id;
        r.game().rooms[static_cast<usize>(id)].tiles.push_back(t);
        return RValue(t.id);
    });
    register_builtin("room_tile_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.room_exists(id)) r.game().rooms[static_cast<usize>(id)].tiles.clear();
        return RValue(0.0);
    });

    register_builtin("event_perform", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        r.perform_event_on_instance(self_id, -1, arg_i32(a, 0), arg_i32(a, 1), other_id);
        return RValue(0.0);
    });
    register_builtin("event_perform_object", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        r.perform_event_on_instance(self_id, arg_i32(a, 0, -1), arg_i32(a, 1), arg_i32(a, 2), other_id);
        return RValue(0.0);
    });
    register_builtin("event_user", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        r.perform_event_on_instance(self_id, -1, 7, 10 + std::clamp(arg_i32(a, 0), 0, 15), other_id);
        return RValue(0.0);
    });
    register_builtin("event_inherited", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        r.perform_event_inherited(self_id);
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Input (Keyboard, Mouse, Joystick)
    // -----------------------------------------------------------------------
    register_builtin("keyboard_check", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(check_key_array(r.window().input().key_down, arg_i32(a, 0)));
    });
    register_builtin("keyboard_check_pressed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(check_key_array(r.window().input().key_pressed, arg_i32(a, 0)));
    });
    register_builtin("keyboard_check_released", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(check_key_array(r.window().input().key_released, arg_i32(a, 0)));
    });
    register_builtin("keyboard_check_direct", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 key = arg_i32(a, 0);
        return RValue(PlatformWindow::keyboard_check_direct(key) || r.touch_overlay_.key_down(key));
    });
    register_builtin("keyboard_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 k = arg_i32(a, 0);
        if (k >= 0 && k < 256) {
            r.window().input_mut().key_down[static_cast<usize>(k)] = false;
            r.window().input_mut().key_pressed[static_cast<usize>(k)] = false;
            r.window().input_mut().key_released[static_cast<usize>(k)] = false;
        }
        return RValue(0.0);
    });
    register_builtin("keyboard_key_press", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 k = arg_i32(a, 0);
        if (k >= 0 && k < 256) {
            if (!r.window().input_mut().key_down[static_cast<usize>(k)]) {
                r.window().input_mut().key_pressed[static_cast<usize>(k)] = true;
            }
            r.window().input_mut().key_down[static_cast<usize>(k)] = true;
        }
        return RValue(0.0);
    });
    register_builtin("keyboard_key_release", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 k = arg_i32(a, 0);
        if (k >= 0 && k < 256) {
            if (r.window().input_mut().key_down[static_cast<usize>(k)]) {
                r.window().input_mut().key_released[static_cast<usize>(k)] = true;
            }
            r.window().input_mut().key_down[static_cast<usize>(k)] = false;
        }
        return RValue(0.0);
    });
    register_builtin("keyboard_get_numlock", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(PlatformWindow::numlock_on());
    });
    register_builtin("keyboard_get_capslock", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(PlatformWindow::capslock_on());
    });
    register_builtin("keyboard_get_scrolllock", [](Runner&, i32, i32, std::span<const RValue>) {
        return RValue(PlatformWindow::scrolllock_on());
    });
    register_builtin("keyboard_set_numlock", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("keyboard_set_capslock", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("keyboard_set_scrolllock", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("keyboard_set_map", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("keyboard_get_map", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(arg_i32(a, 0)); });
    register_builtin("keyboard_unset_map", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("mouse_check_button", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(check_mouse_array(r.window().input().mouse_down, arg_i32(a, 0)));
    });
    register_builtin("mouse_check_button_pressed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(check_mouse_array(r.window().input().mouse_pressed, arg_i32(a, 0)));
    });
    register_builtin("mouse_check_button_released", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(check_mouse_array(r.window().input().mouse_released, arg_i32(a, 0)));
    });
    register_builtin("mouse_wheel_up", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.window().input().mouse_wheel_up);
    });
    register_builtin("mouse_wheel_down", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.window().input().mouse_wheel_down);
    });
    register_builtin("mouse_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 b = arg_i32(a, 0);
        if (b >= 1 && b < 8) {
            r.window().input_mut().mouse_down[static_cast<usize>(b)] = false;
            r.window().input_mut().mouse_pressed[static_cast<usize>(b)] = false;
            r.window().input_mut().mouse_released[static_cast<usize>(b)] = false;
        }
        return RValue(0.0);
    });
    register_builtin("io_clear", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.window().input_mut().clear_all();
        r.mouse_button_ = 0;
        r.mouse_lastbutton_ = 0;
        return RValue(0.0);
    });
    register_builtin("io_handle", [](Runner& r, i32, i32, std::span<const RValue>) {
        (void)r.window().poll_events();
        r.update_mouse_room_coords();
        return RValue(0.0);
    });
    register_builtin("keyboard_wait", [](Runner& r, i32, i32, std::span<const RValue>) -> Result<RValue> {
        if (!r.wait_for_key()) return Error{"Window close requested during keyboard_wait"};
        return RValue(0.0);
    });
    register_builtin("mouse_wait", [](Runner& r, i32, i32, std::span<const RValue>) -> Result<RValue> {
        if (!r.wait_for_mouse()) return Error{"Window close requested during mouse_wait"};
        return RValue(0.0);
    });
    // GM 8.2 stubs all 14 built-in joystick_* functions (gm82joy overrides them via GEX when included).
    register_builtin("joystick_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (r.version_mode() == VersionMode::GM82) return RValue(false);
        return RValue(PlatformWindow::joystick_exists(arg_i32(a, 0, 1)));
    });
    register_builtin("joystick_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (r.version_mode() == VersionMode::GM82) return RValue(std::string{});
        return RValue(PlatformWindow::joystick_name(arg_i32(a, 0, 1)));
    });
    register_builtin("joystick_axes", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (r.version_mode() == VersionMode::GM82) return RValue(0);
        return RValue(PlatformWindow::joystick_axes(arg_i32(a, 0, 1)));
    });
    register_builtin("joystick_buttons", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (r.version_mode() == VersionMode::GM82) return RValue(0);
        return RValue(PlatformWindow::joystick_buttons(arg_i32(a, 0, 1)));
    });
    register_builtin("joystick_has_pov", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (r.version_mode() == VersionMode::GM82) return RValue(false);
        return RValue(PlatformWindow::joystick_has_pov(arg_i32(a, 0, 1)));
    });
    register_builtin("joystick_direction", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (r.version_mode() == VersionMode::GM82) return RValue(101);
        return RValue(PlatformWindow::joystick_direction(arg_i32(a, 0, 1)));
    });
    register_builtin("joystick_check_button", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (r.version_mode() == VersionMode::GM82) return RValue(false);
        return RValue(PlatformWindow::joystick_check_button(arg_i32(a, 0, 1), arg_i32(a, 1, 1)));
    });
    register_builtin("joystick_xpos", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.version_mode() == VersionMode::GM82 ? 0.0 : PlatformWindow::joystick_axis(arg_i32(a, 0, 1), 0));
    });
    register_builtin("joystick_ypos", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.version_mode() == VersionMode::GM82 ? 0.0 : PlatformWindow::joystick_axis(arg_i32(a, 0, 1), 1));
    });
    register_builtin("joystick_zpos", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.version_mode() == VersionMode::GM82 ? 0.0 : PlatformWindow::joystick_axis(arg_i32(a, 0, 1), 2));
    });
    register_builtin("joystick_rpos", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.version_mode() == VersionMode::GM82 ? 0.0 : PlatformWindow::joystick_axis(arg_i32(a, 0, 1), 3));
    });
    register_builtin("joystick_upos", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.version_mode() == VersionMode::GM82 ? 0.0 : PlatformWindow::joystick_axis(arg_i32(a, 0, 1), 4));
    });
    register_builtin("joystick_vpos", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.version_mode() == VersionMode::GM82 ? 0.0 : PlatformWindow::joystick_axis(arg_i32(a, 0, 1), 5));
    });
    register_builtin("joystick_pov", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (r.version_mode() == VersionMode::GM82) return RValue(-1.0);
        return RValue(PlatformWindow::joystick_pov(arg_i32(a, 0, 1)));
    });

    // -----------------------------------------------------------------------
    // Sound
    // -----------------------------------------------------------------------
    register_builtin("sound_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.audio().sound_exists(arg_i32(a, 0, -1)));
    });
    auto fn_sound_name = [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sounds.size() && r.game().sounds[static_cast<usize>(id)].exists) {
            return RValue(r.game().sounds[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    };
    register_builtin("sound_get_name", fn_sound_name);
    register_builtin("sound_name", fn_sound_name);
    register_builtin("sound_get_kind", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sounds.size() && r.game().sounds[static_cast<usize>(id)].exists) {
            return RValue(r.game().sounds[static_cast<usize>(id)].kind);
        }
        return RValue(0);
    });
    register_builtin("sound_get_preload", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().sounds.size() &&
                      r.game().sounds[static_cast<usize>(id)].exists &&
                      r.game().sounds[static_cast<usize>(id)].preload);
    });
    register_builtin("sound_play", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_play(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("sound_loop", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_loop(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("sound_stop", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_stop(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("sound_stop_all", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.audio().sound_stop_all();
        return RValue(0.0);
    });
    register_builtin("sound_isplaying", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.audio().sound_isplaying(arg_i32(a, 0, -1)));
    });
    register_builtin("sound_volume", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_volume(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1, 1.0)));
        return RValue(0.0);
    });
    register_builtin("sound_global_volume", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_global_volume(static_cast<f32>(arg_real(a, 0, 1.0)));
        return RValue(0.0);
    });
    register_builtin("sound_fade", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_fade(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1, 1.0)), static_cast<f32>(arg_real(a, 2, 0.0)));
        return RValue(0.0);
    });
    register_builtin("sound_pan", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_pan(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1, 0.0)));
        return RValue(0.0);
    });
    register_builtin("sound_background_tempo", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_background_tempo(static_cast<f32>(arg_real(a, 0, 1.0)));
        return RValue(0.0);
    });
    register_builtin("sound_set_search_directory", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_set_search_directory(arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("sound_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        std::string path(arg_str(a, 0));
        auto bytes = r.vfs().read_all_bytes(path);
        if (!bytes && !r.audio().sound_search_directory().empty()) {
            bytes = r.vfs().read_all_bytes(r.audio().sound_search_directory() + "/" + path);
        }
        if (!bytes) return RValue(-1);
        return RValue(r.audio().add_sound_from_memory(bytes.value(), arg_i32(a, 1), path));
    });
    register_builtin("sound_replace", [](Runner& r, i32, i32, std::span<const RValue> a) {
        std::string path(arg_str(a, 1));
        auto bytes = r.vfs().read_all_bytes(path);
        if (!bytes && !r.audio().sound_search_directory().empty()) {
            bytes = r.vfs().read_all_bytes(r.audio().sound_search_directory() + "/" + path);
        }
        if (!bytes) return RValue(false);
        return RValue(r.audio().replace_sound_from_memory(arg_i32(a, 0, -1), bytes.value(), arg_i32(a, 2), path));
    });
    register_builtin("sound_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().delete_sound(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("sound_discard", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_discard(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("sound_restore", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_restore(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("sound_effect_set", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_effect_set(arg_i32(a, 0, -1), static_cast<u32>(arg_i32(a, 1)));
        return RValue(0.0);
    });
    register_builtin("sound_effect_chorus", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_effect_chorus(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                      static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
                                      arg_i32(a, 5), static_cast<f32>(arg_real(a, 6)), arg_i32(a, 7));
        return RValue(0.0);
    });
    register_builtin("sound_effect_compressor", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_effect_compressor(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                          static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
                                          static_cast<f32>(arg_real(a, 5)), static_cast<f32>(arg_real(a, 6)));
        return RValue(0.0);
    });
    register_builtin("sound_effect_echo", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_effect_echo(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                    static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)), arg_i32(a, 5));
        return RValue(0.0);
    });
    register_builtin("sound_effect_flanger", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_effect_flanger(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                       static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
                                       arg_i32(a, 5), static_cast<f32>(arg_real(a, 6)), arg_i32(a, 7));
        return RValue(0.0);
    });
    register_builtin("sound_effect_gargle", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_effect_gargle(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), arg_i32(a, 2));
        return RValue(0.0);
    });
    register_builtin("sound_effect_equalizer", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_effect_equalizer(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)),
                                         static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)));
        return RValue(0.0);
    });
    register_builtin("sound_effect_reverb", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_effect_reverb(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                      static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)));
        return RValue(0.0);
    });
    register_builtin("sound_3d_set_sound_position", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_3d_set_position(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)),
                                        static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)));
        return RValue(0.0);
    });
    register_builtin("sound_3d_set_sound_velocity", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_3d_set_velocity(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)),
                                        static_cast<f32>(arg_real(a, 2)), static_cast<f32>(arg_real(a, 3)));
        return RValue(0.0);
    });
    register_builtin("sound_3d_set_sound_distance", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_3d_set_distance(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)));
        return RValue(0.0);
    });
    register_builtin("sound_3d_set_sound_cone", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_3d_set_cone(arg_i32(a, 0, -1), static_cast<f32>(arg_real(a, 1)), static_cast<f32>(arg_real(a, 2)),
                                    static_cast<f32>(arg_real(a, 3)), static_cast<f32>(arg_real(a, 4)),
                                    static_cast<f32>(arg_real(a, 5)), static_cast<f32>(arg_real(a, 6)));
        return RValue(0.0);
    });

    // -----------------------------------------------------------------------
    // Particles & Simple Effects
    // -----------------------------------------------------------------------
    register_builtin("effect_create_below", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().effect_create(false, arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_i32(a, 3), static_cast<u32>(arg_i32(a, 4)) & 0xFFFFFFu, r.effect_speed_factor(), r.room_width_, r.room_height_);
        return RValue(0.0);
    });
    register_builtin("effect_create_above", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().effect_create(true, arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_i32(a, 3), static_cast<u32>(arg_i32(a, 4)) & 0xFFFFFFu, r.effect_speed_factor(), r.room_width_, r.room_height_);
        return RValue(0.0);
    });
    register_builtin("effect_clear", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.particles().effect_clear();
        return RValue(0.0);
    });
    register_builtin("part_system_create", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.particles().system_create()); });
    register_builtin("part_system_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().system_destroy(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_system_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().system_exists(arg_i32(a, 0))); });
    register_builtin("part_system_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().system_clear(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_system_depth", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().system_depth(arg_i32(a, 0), arg_real(a, 1)); return RValue(0.0); });
    register_builtin("part_system_position", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().system_position(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2)); return RValue(0.0); });
    register_builtin("part_system_automatic_update", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().system_automatic_update(arg_i32(a, 0), arg_bool(a, 1)); return RValue(0.0); });
    register_builtin("part_system_automatic_draw", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().system_automatic_draw(arg_i32(a, 0), arg_bool(a, 1)); return RValue(0.0); });
    register_builtin("part_system_draw_order", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().system_draw_order(arg_i32(a, 0), arg_bool(a, 1, true)); return RValue(0.0); });
    register_builtin("part_system_update", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().update_system(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_system_drawit", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().draw_system(arg_i32(a, 0), r.renderer(), r.game()); return RValue(0.0); });
    register_builtin("part_particles_create", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().particles_create(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_i32(a, 3), arg_i32(a, 4));
        return RValue(0.0);
    });
    register_builtin("part_particles_create_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().particles_create(arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_i32(a, 3), arg_i32(a, 5), true, static_cast<u32>(arg_i32(a, 4)) & 0xFFFFFFu);
        return RValue(0.0);
    });
    register_builtin("part_particles_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().particles_clear(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_particles_count", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().particles_count(arg_i32(a, 0))); });
    register_builtin("part_type_create", [](Runner& r, i32, i32, std::span<const RValue>) { return RValue(r.particles().type_create()); });
    register_builtin("part_type_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().type_destroy(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_type_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().type_exists(arg_i32(a, 0))); });
    register_builtin("part_type_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().type_clear(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_type_shape", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) { t->shape = arg_i32(a, 1); t->sprite_index = -1; }
        return RValue(0.0);
    });
    register_builtin("part_type_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->sprite_index = arg_i32(a, 1, -1);
            t->sprite_anim = arg_bool(a, 2);
            t->sprite_stretch = arg_bool(a, 3);
            t->sprite_random = arg_bool(a, 4);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_size", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->size_min = arg_real(a, 1, 1.0); t->size_max = arg_real(a, 2, 1.0);
            t->size_incr = arg_real(a, 3, 0.0); t->size_wiggle = arg_real(a, 4, 0.0);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_scale", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) { t->xscale = arg_real(a, 1, 1.0); t->yscale = arg_real(a, 2, 1.0); }
        return RValue(0.0);
    });
    register_builtin("part_type_orientation", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->ang_min = arg_real(a, 1); t->ang_max = arg_real(a, 2);
            t->ang_incr = arg_real(a, 3); t->ang_wiggle = arg_real(a, 4);
            t->ang_relative = arg_bool(a, 5);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_color1", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) { t->col_mode = 0; t->col1 = static_cast<u32>(arg_i32(a, 1)) & 0xFFFFFFu; }
        return RValue(0.0);
    });
    register_builtin("part_type_color2", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->col_mode = 1;
            t->col1 = static_cast<u32>(arg_i32(a, 1)) & 0xFFFFFFu;
            t->col2 = static_cast<u32>(arg_i32(a, 2)) & 0xFFFFFFu;
        }
        return RValue(0.0);
    });
    register_builtin("part_type_color3", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->col_mode = 2;
            t->col1 = static_cast<u32>(arg_i32(a, 1)) & 0xFFFFFFu;
            t->col2 = static_cast<u32>(arg_i32(a, 2)) & 0xFFFFFFu;
            t->col3 = static_cast<u32>(arg_i32(a, 3)) & 0xFFFFFFu;
        }
        return RValue(0.0);
    });
    register_builtin("part_type_alpha1", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) t->alpha1 = t->alpha2 = t->alpha3 = arg_real(a, 1, 1.0);
        return RValue(0.0);
    });
    register_builtin("part_type_alpha2", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) { t->alpha1 = arg_real(a, 1, 1.0); t->alpha2 = t->alpha3 = arg_real(a, 2, 1.0); }
        return RValue(0.0);
    });
    register_builtin("part_type_alpha3", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->alpha1 = arg_real(a, 1, 1.0); t->alpha2 = arg_real(a, 2, 1.0); t->alpha3 = arg_real(a, 3, 1.0);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_blend", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) t->additive = arg_bool(a, 1);
        return RValue(0.0);
    });
    register_builtin("part_type_life", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) { t->life_min = arg_i32(a, 1, 30); t->life_max = arg_i32(a, 2, 60); }
        return RValue(0.0);
    });
    register_builtin("part_type_speed", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->speed_min = arg_real(a, 1); t->speed_max = arg_real(a, 2);
            t->speed_incr = arg_real(a, 3); t->speed_wiggle = arg_real(a, 4);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_direction", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->dir_min = arg_real(a, 1); t->dir_max = arg_real(a, 2);
            t->dir_incr = arg_real(a, 3); t->dir_wiggle = arg_real(a, 4);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_gravity", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) { t->grav_amount = arg_real(a, 1); t->grav_dir = arg_real(a, 2, 270.0); }
        return RValue(0.0);
    });
    register_builtin("part_type_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) { t->col_mode = 0; t->col1 = static_cast<u32>(arg_i32(a, 1)) & 0xFFFFFFu; }
        return RValue(0.0);
    });
    register_builtin("part_type_color_mix", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->col_mode = 3;
            t->col1 = static_cast<u32>(arg_i32(a, 1)) & 0xFFFFFFu;
            t->col2 = static_cast<u32>(arg_i32(a, 2)) & 0xFFFFFFu;
        }
        return RValue(0.0);
    });
    register_builtin("part_type_color_rgb", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->col_mode = 4;
            t->rmin = arg_i32(a, 1); t->rmax = arg_i32(a, 2);
            t->gmin = arg_i32(a, 3); t->gmax = arg_i32(a, 4);
            t->bmin = arg_i32(a, 5); t->bmax = arg_i32(a, 6);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_color_hsv", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->col_mode = 5;
            t->hmin = arg_i32(a, 1); t->hmax = arg_i32(a, 2);
            t->smin = arg_i32(a, 3); t->smax = arg_i32(a, 4);
            t->vmin = arg_i32(a, 5); t->vmax = arg_i32(a, 6);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_alpha", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->alpha1 = t->alpha2 = t->alpha3 = arg_real(a, 1, 1.0);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_step", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->step_number = arg_i32(a, 1);
            t->step_type = arg_i32(a, 2, -1);
        }
        return RValue(0.0);
    });
    register_builtin("part_type_death", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (auto* t = r.particles().get_type(arg_i32(a, 0))) {
            t->death_number = arg_i32(a, 1);
            t->death_type = arg_i32(a, 2, -1);
        }
        return RValue(0.0);
    });
    register_builtin("part_emitter_create", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().emitter_create(arg_i32(a, 0))); });
    register_builtin("part_emitter_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().emitter_destroy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_emitter_destroy_all", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().emitter_destroy_all(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_emitter_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().emitter_exists(arg_i32(a, 0), arg_i32(a, 1))); });
    register_builtin("part_emitter_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().emitter_clear(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_emitter_region", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().emitter_region(arg_i32(a, 0), arg_i32(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4), arg_real(a, 5), arg_i32(a, 6), arg_i32(a, 7));
        return RValue(0.0);
    });
    register_builtin("part_emitter_burst", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().emitter_burst(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3));
        return RValue(0.0);
    });
    register_builtin("part_emitter_stream", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().emitter_stream(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3));
        return RValue(0.0);
    });
    register_builtin("part_attractor_create", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().attractor_create(arg_i32(a, 0))); });
    register_builtin("part_attractor_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().attractor_destroy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_attractor_destroy_all", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().attractor_destroy_all(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_attractor_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().attractor_exists(arg_i32(a, 0), arg_i32(a, 1))); });
    register_builtin("part_attractor_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().attractor_clear(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_attractor_position", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().attractor_position(arg_i32(a, 0), arg_i32(a, 1), arg_real(a, 2), arg_real(a, 3)); return RValue(0.0); });
    register_builtin("part_attractor_force", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().attractor_force(arg_i32(a, 0), arg_i32(a, 1), arg_real(a, 2), arg_real(a, 3), arg_i32(a, 4), arg_bool(a, 5)); return RValue(0.0); });
    register_builtin("part_destroyer_create", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().destroyer_create(arg_i32(a, 0))); });
    register_builtin("part_destroyer_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().destroyer_destroy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_destroyer_destroy_all", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().destroyer_destroy_all(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_destroyer_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().destroyer_exists(arg_i32(a, 0), arg_i32(a, 1))); });
    register_builtin("part_destroyer_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().destroyer_clear(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_destroyer_region", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().destroyer_region(arg_i32(a, 0), arg_i32(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4), arg_real(a, 5), arg_i32(a, 6)); return RValue(0.0); });
    register_builtin("part_deflector_create", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().deflector_create(arg_i32(a, 0))); });
    register_builtin("part_deflector_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().deflector_destroy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_deflector_destroy_all", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().deflector_destroy_all(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_deflector_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().deflector_exists(arg_i32(a, 0), arg_i32(a, 1))); });
    register_builtin("part_deflector_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().deflector_clear(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_deflector_region", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().deflector_region(arg_i32(a, 0), arg_i32(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4), arg_real(a, 5)); return RValue(0.0); });
    register_builtin("part_deflector_kind", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().deflector_kind(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2)); return RValue(0.0); });
    register_builtin("part_deflector_friction", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().deflector_friction(arg_i32(a, 0), arg_i32(a, 1), arg_real(a, 2)); return RValue(0.0); });
    register_builtin("part_changer_create", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().changer_create(arg_i32(a, 0))); });
    register_builtin("part_changer_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().changer_destroy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_changer_destroy_all", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().changer_destroy_all(arg_i32(a, 0)); return RValue(0.0); });
    register_builtin("part_changer_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().changer_exists(arg_i32(a, 0), arg_i32(a, 1))); });
    register_builtin("part_changer_clear", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().changer_clear(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
    register_builtin("part_changer_region", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().changer_region(arg_i32(a, 0), arg_i32(a, 1), arg_real(a, 2), arg_real(a, 3), arg_real(a, 4), arg_real(a, 5), arg_i32(a, 6)); return RValue(0.0); });
    register_builtin("part_changer_kind", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().changer_kind(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2)); return RValue(0.0); });
    register_builtin("part_changer_types", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().changer_types(arg_i32(a, 0), arg_i32(a, 1), arg_i32(a, 2), arg_i32(a, 3)); return RValue(0.0); });

    // -----------------------------------------------------------------------
    // Resource Queries & Dynamic Mutations (Objects, Sprites, Backgrounds, Fonts, Timelines)
    // -----------------------------------------------------------------------
    register_builtin("object_exists", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.object_exists(arg_i32(a, 0, -1))); });
    register_builtin("object_get_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) ? r.game().objects[static_cast<usize>(id)].name : std::string("<undefined>"));
    });
    register_builtin("object_get_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) ? r.game().objects[static_cast<usize>(id)].sprite_index : -1);
    });
    register_builtin("object_get_solid", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) && r.game().objects[static_cast<usize>(id)].solid);
    });
    register_builtin("object_get_visible", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) && r.game().objects[static_cast<usize>(id)].visible);
    });
    register_builtin("object_get_depth", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) ? r.game().objects[static_cast<usize>(id)].depth : 0);
    });
    register_builtin("object_get_persistent", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) && r.game().objects[static_cast<usize>(id)].persistent);
    });
    register_builtin("object_get_mask", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) ? r.game().objects[static_cast<usize>(id)].mask_index : -1);
    });
    register_builtin("object_get_parent", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) ? r.game().objects[static_cast<usize>(id)].parent_index : -100);
    });
    register_builtin("object_is_ancestor", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.object_is_ancestor(arg_i32(a, 0, -1), arg_i32(a, 1, -1)));
    });
    register_builtin("object_set_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.object_exists(id)) r.game().objects[static_cast<usize>(id)].sprite_index = arg_i32(a, 1, -1);
        return RValue(0.0);
    });
    register_builtin("object_set_solid", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.object_exists(id)) r.game().objects[static_cast<usize>(id)].solid = arg_bool(a, 1);
        return RValue(0.0);
    });
    register_builtin("object_set_visible", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.object_exists(id)) r.game().objects[static_cast<usize>(id)].visible = arg_bool(a, 1);
        return RValue(0.0);
    });
    register_builtin("object_set_depth", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.object_exists(id)) r.game().objects[static_cast<usize>(id)].depth = arg_i32(a, 1);
        return RValue(0.0);
    });
    register_builtin("object_set_persistent", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.object_exists(id)) r.game().objects[static_cast<usize>(id)].persistent = arg_bool(a, 1);
        return RValue(0.0);
    });
    register_builtin("object_set_mask", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.object_exists(id)) r.game().objects[static_cast<usize>(id)].mask_index = arg_i32(a, 1, -1);
        return RValue(0.0);
    });
    register_builtin("object_set_parent", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        r.set_object_parent(id, arg_i32(a, 1, -100));
        return RValue(0.0);
    });
    register_builtin("object_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(r.object_exists(id) ? r.game().objects[static_cast<usize>(id)].name : std::string("<undefined>"));
    });
    register_builtin("object_add", [](Runner& r, i32, i32, std::span<const RValue>) {
        ObjectData obj{};
        obj.exists = true;
        const i32 id = static_cast<i32>(r.game().objects.size());
        r.game().objects.push_back(std::move(obj));
        if (r.program().object_event_chunks.size() <= static_cast<usize>(id)) {
            r.program().object_event_chunks.resize(static_cast<usize>(id) + 1);
        }
        r.invalidate_event_tables();
        return RValue(id);
    });
    register_builtin("object_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (r.object_exists(id)) {
            r.game().objects[static_cast<usize>(id)].exists = false;
            r.invalidate_event_tables();
        }
        return RValue(0.0);
    });
    register_builtin("object_event_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        (void)r.object_event_add(arg_i32(a, 0, -1), arg_i32(a, 1), arg_i32(a, 2), arg_str(a, 3));
        return RValue(0.0);
    });
    register_builtin("object_event_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        (void)r.object_event_clear(arg_i32(a, 0, -1), arg_i32(a, 1), arg_i32(a, 2));
        return RValue(0.0);
    });

    // Sprites
    register_builtin("sprite_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists);
    });
    register_builtin("sprite_get_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists) {
            return RValue(r.game().sprites[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    });
    register_builtin("sprite_get_number", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists) {
            return RValue(static_cast<i32>(r.game().sprites[static_cast<usize>(id)].images.size()));
        }
        return RValue(0);
    });
    register_builtin("sprite_get_width", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists)
                          ? r.game().sprites[static_cast<usize>(id)].width : 0);
    });
    register_builtin("sprite_get_height", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists)
                          ? r.game().sprites[static_cast<usize>(id)].height : 0);
    });
    register_builtin("sprite_get_xoffset", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists)
                          ? r.game().sprites[static_cast<usize>(id)].origin_x : 0);
    });
    register_builtin("sprite_get_yoffset", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists)
                          ? r.game().sprites[static_cast<usize>(id)].origin_y : 0);
    });
    register_builtin("sprite_get_bbox_left", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists)
                          ? r.game().sprites[static_cast<usize>(id)].bbox_left : 0);
    });
    register_builtin("sprite_get_bbox_right", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists)
                          ? r.game().sprites[static_cast<usize>(id)].bbox_right : 0);
    });
    register_builtin("sprite_get_bbox_top", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists)
                          ? r.game().sprites[static_cast<usize>(id)].bbox_top : 0);
    });
    register_builtin("sprite_get_bbox_bottom", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists)
                          ? r.game().sprites[static_cast<usize>(id)].bbox_bottom : 0);
    });
    register_builtin("sprite_set_offset", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists) {
            r.game().sprites[static_cast<usize>(id)].origin_x = arg_i32(a, 1);
            r.game().sprites[static_cast<usize>(id)].origin_y = arg_i32(a, 2);
        }
        return RValue(0.0);
    });
    register_builtin("sprite_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(-1);
        auto bmp = Renderer2D::decode_image_to_bgra(bytes.value());
        if (!bmp) return RValue(-1);
        SpriteData sp{};
        if (!build_sprite_from_strip(bmp.value(), arg_i32(a, 1, 1), arg_bool(a, 2, false), arg_i32(a, 4), arg_i32(a, 5), sp)) {
            return RValue(-1);
        }
        const usize id = r.game().sprites.size();
        r.game().sprites.push_back(std::move(sp));
        r.renderer().upload_sprite(id, r.game().sprites[id]);
        return RValue(static_cast<i32>(id));
    });
    register_builtin("sprite_replace", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().sprites.size()) return RValue(false);
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 1));
        if (!bytes) return RValue(false);
        auto bmp = Renderer2D::decode_image_to_bgra(bytes.value());
        if (!bmp) return RValue(false);
        SpriteData sp{};
        if (!build_sprite_from_strip(bmp.value(), arg_i32(a, 2, 1), arg_bool(a, 3, false), arg_i32(a, 5), arg_i32(a, 6), sp)) {
            return RValue(false);
        }
        sp.name = r.game().sprites[static_cast<usize>(id)].name;
        r.game().sprites[static_cast<usize>(id)] = std::move(sp);
        r.renderer().upload_sprite(static_cast<usize>(id), r.game().sprites[static_cast<usize>(id)]);
        return RValue(true);
    });
    register_builtin("sprite_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sprites.size()) {
            r.game().sprites[static_cast<usize>(id)].exists = false;
            r.renderer().free_sprite_textures(static_cast<usize>(id));
        }
        return RValue(0.0);
    });
    register_builtin("sprite_duplicate", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().sprites.size() || !r.game().sprites[static_cast<usize>(id)].exists) {
            return RValue(-1);
        }
        SpriteData copy = r.game().sprites[static_cast<usize>(id)];
        const usize new_id = r.game().sprites.size();
        r.game().sprites.push_back(std::move(copy));
        r.renderer().upload_sprite(new_id, r.game().sprites[new_id]);
        return RValue(static_cast<i32>(new_id));
    });
    register_builtin("sprite_assign", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1);
        const i32 src = arg_i32(a, 1, -1);
        if (dst >= 0 && static_cast<usize>(dst) < r.game().sprites.size() &&
            src >= 0 && static_cast<usize>(src) < r.game().sprites.size() &&
            r.game().sprites[static_cast<usize>(src)].exists) {
            std::string keep_name = r.game().sprites[static_cast<usize>(dst)].name;
            r.game().sprites[static_cast<usize>(dst)] = r.game().sprites[static_cast<usize>(src)];
            r.game().sprites[static_cast<usize>(dst)].name = std::move(keep_name);
            r.renderer().upload_sprite(static_cast<usize>(dst), r.game().sprites[static_cast<usize>(dst)]);
        }
        return RValue(0.0);
    });
    register_builtin("sprite_add_from_surface", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 sp_id = arg_i32(a, 0, -1);
        const i32 sid = arg_i32(a, 1, -1);
        if (sp_id < 0 || static_cast<usize>(sp_id) >= r.game().sprites.size() || !r.game().sprites[static_cast<usize>(sp_id)].exists) {
            return RValue(0.0);
        }
        const i32 x = arg_i32(a, 2), y = arg_i32(a, 3), w = arg_i32(a, 4), h = arg_i32(a, 5);
        std::vector<u8> bgra;
        if (!r.renderer().surface_read_pixels_bgra(sid, x, y, w, h, bgra)) return RValue(0.0);
        Bitmap32 bmp{w, h, std::move(bgra)};
        if (arg_bool(a, 6, false)) remove_bottom_left_bg_color(bmp);
        auto& sp = r.game().sprites[static_cast<usize>(sp_id)];
        sp.images.push_back(std::move(bmp));
        r.renderer().upload_sprite(static_cast<usize>(sp_id), sp);
        return RValue(0.0);
    });
    register_builtin("sprite_create_from_surface", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 sid = arg_i32(a, 0, -1);
        const i32 x = arg_i32(a, 1), y = arg_i32(a, 2), w = arg_i32(a, 3), h = arg_i32(a, 4);
        std::vector<u8> bgra;
        if (!r.renderer().surface_read_pixels_bgra(sid, x, y, w, h, bgra)) return RValue(-1);
        Bitmap32 bmp{w, h, std::move(bgra)};
        SpriteData sp{};
        if (!build_sprite_from_strip(bmp, 1, arg_bool(a, 5, false), arg_i32(a, 7), arg_i32(a, 8), sp)) return RValue(-1);
        const usize id = r.game().sprites.size();
        r.game().sprites.push_back(std::move(sp));
        r.renderer().upload_sprite(id, r.game().sprites[id]);
        return RValue(static_cast<i32>(id));
    });
    register_builtin("sprite_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists) {
            return RValue(r.game().sprites[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    });
    register_builtin("sprite_create_from_screen", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 x = arg_i32(a, 0), y = arg_i32(a, 1), w = arg_i32(a, 2), h = arg_i32(a, 3);
        std::vector<u8> bgra;
        if (!r.renderer().screen_read_pixels_bgra(x, y, w, h, bgra)) return RValue(-1);
        return RValue(r.create_sprite_from_bitmap(Bitmap32{w, h, std::move(bgra)}, arg_bool(a, 4, false),
                                                  arg_bool(a, 5, false), arg_i32(a, 6), arg_i32(a, 7)));
    });
    register_builtin("sprite_add_from_screen", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 sp_id = arg_i32(a, 0, -1);
        if (sp_id < 0 || static_cast<usize>(sp_id) >= r.game().sprites.size() || !r.game().sprites[static_cast<usize>(sp_id)].exists) return RValue(0.0);
        const i32 x = arg_i32(a, 1), y = arg_i32(a, 2), w = arg_i32(a, 3), h = arg_i32(a, 4);
        std::vector<u8> bgra;
        if (!r.renderer().screen_read_pixels_bgra(x, y, w, h, bgra)) return RValue(0.0);
        Bitmap32 bmp{w, h, std::move(bgra)};
        Runner::apply_removeback_and_smooth(bmp, arg_bool(a, 5, false), arg_bool(a, 6, false));
        auto& sp = r.game().sprites[static_cast<usize>(sp_id)];
        sp.images.push_back(std::move(bmp));
        Runner::rebuild_sprite_masks(sp);
        r.renderer().upload_sprite(static_cast<usize>(sp_id), sp);
        return RValue(0.0);
    });
    register_builtin("sprite_add_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        SpriteData sp{};
        if (!load_sprite_file(r, arg_str(a, 0), sp)) return RValue(-1);
        const usize id = r.game().sprites.size();
        sp.name = "__newsprite" + std::to_string(id);
        r.game().sprites.push_back(std::move(sp));
        r.renderer().upload_sprite(id, r.game().sprites[id]);
        return RValue(static_cast<i32>(id));
    });
    register_builtin("sprite_replace_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) -> Result<RValue> {
        const i32 id = arg_i32(a, 0, -1);
        auto& sprites = r.game().sprites;
        if (id < 0 || static_cast<usize>(id) >= sprites.size() || !sprites[static_cast<usize>(id)].exists)
            return Error{"Trying to replace non-existing sprite."};
        auto& sp = sprites[static_cast<usize>(id)];
        const bool had_images = !sp.images.empty();
        const bool loaded = load_sprite_file(r, arg_str(a, 1), sp);
        if (loaded || (had_images && sp.images.empty())) {
            r.renderer().flush();
            r.renderer().upload_sprite(static_cast<usize>(id), sp);
        }
        return RValue(loaded);
    });
    register_builtin("sprite_merge", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1), src = arg_i32(a, 1, -1);
        if (dst >= 0 && static_cast<usize>(dst) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(dst)].exists &&
            src >= 0 && static_cast<usize>(src) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(src)].exists) {
            auto& d = r.game().sprites[static_cast<usize>(dst)];
            const auto& s = r.game().sprites[static_cast<usize>(src)];
            const auto src_imgs = s.images;
            for (const auto& img : src_imgs) d.images.push_back(img);
            Runner::rebuild_sprite_masks(d);
            r.renderer().upload_sprite(static_cast<usize>(dst), d);
        }
        return RValue(0.0);
    });
    register_builtin("sprite_set_alpha_from_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1), src = arg_i32(a, 1, -1);
        if (dst >= 0 && static_cast<usize>(dst) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(dst)].exists &&
            src >= 0 && static_cast<usize>(src) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(src)].exists) {
            auto& d = r.game().sprites[static_cast<usize>(dst)];
            const auto& s = r.game().sprites[static_cast<usize>(src)];
            if (s.images.empty()) return RValue(0.0);
            for (usize i = 0; i < d.images.size(); ++i) {
                auto& di = d.images[i];
                const auto& si = s.images[i % s.images.size()];
                const i32 mw = std::min(di.width, si.width);
                const i32 mh = std::min(di.height, si.height);
                for (i32 y = 0; y < mh; ++y) {
                    for (i32 x = 0; x < mw; ++x) {
                        const usize dp = (static_cast<usize>(y) * static_cast<usize>(di.width) + static_cast<usize>(x)) * 4u;
                        const usize sp = (static_cast<usize>(y) * static_cast<usize>(si.width) + static_cast<usize>(x)) * 4u;
                        const u8 lum = static_cast<u8>((static_cast<u32>(si.bgra[sp + 2]) * 77u +
                                                        static_cast<u32>(si.bgra[sp + 1]) * 150u +
                                                        static_cast<u32>(si.bgra[sp + 0]) * 29u) >> 8u);
                        di.bgra[dp + 3] = lum;
                    }
                }
            }
            Runner::rebuild_sprite_masks(d);
            r.renderer().upload_sprite(static_cast<usize>(dst), d);
        }
        return RValue(0.0);
    });
    register_builtin("sprite_collision_mask", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sprites.size() && r.game().sprites[static_cast<usize>(id)].exists) {
            auto& sp = r.game().sprites[static_cast<usize>(id)];
            sp.separate_masks = arg_bool(a, 1);
            const i32 bbox_mode = arg_i32(a, 2);
            const i32 bleft = arg_i32(a, 3), btop = arg_i32(a, 4), bright = arg_i32(a, 5), bbottom = arg_i32(a, 6);
            sp.bbox_shape = arg_i32(a, 7);
            const i32 tol = arg_i32(a, 8, 0);
            Runner::rebuild_sprite_masks(sp, tol, bbox_mode, bleft, bright, btop, bbottom);
        }
        return RValue(0.0);
    });
    register_builtin("sprite_save", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        const i32 sub = arg_i32(a, 1, 0);
        if (id < 0 || static_cast<usize>(id) >= r.game().sprites.size() || !r.game().sprites[static_cast<usize>(id)].exists) return RValue(false);
        const auto& sp = r.game().sprites[static_cast<usize>(id)];
        if (sp.images.empty()) return RValue(false);
        const usize idx = static_cast<usize>((sub % static_cast<i32>(sp.images.size()) + static_cast<i32>(sp.images.size())) % static_cast<i32>(sp.images.size()));
        const auto host_path = Vfs::path_to_utf8(r.vfs().resolve_for_write(arg_str(a, 2)));
        return RValue(Renderer2D::save_bitmap_bgra(sp.images[idx], host_path));
    });
    register_builtin("sprite_save_strip", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().sprites.size() || !r.game().sprites[static_cast<usize>(id)].exists) return RValue(false);
        const auto& sp = r.game().sprites[static_cast<usize>(id)];
        if (sp.images.empty() || sp.width <= 0 || sp.height <= 0) return RValue(false);
        const i32 sw = sp.width * static_cast<i32>(sp.images.size());
        const i32 sh = sp.height;
        Bitmap32 strip{sw, sh, std::vector<u8>(static_cast<usize>(sw) * static_cast<usize>(sh) * 4u, 0)};
        for (usize f = 0; f < sp.images.size(); ++f) {
            const auto& img = sp.images[f];
            const i32 cw = std::min(sp.width, img.width);
            const i32 ch = std::min(sp.height, img.height);
            for (i32 y = 0; y < ch; ++y) {
                std::memcpy(&strip.bgra[(static_cast<usize>(y) * static_cast<usize>(sw) + f * static_cast<usize>(sp.width)) * 4u],
                            &img.bgra[static_cast<usize>(y) * static_cast<usize>(img.width) * 4u],
                            static_cast<usize>(cw) * 4u);
            }
        }
        const auto host_path = Vfs::path_to_utf8(r.vfs().resolve_for_write(arg_str(a, 1)));
        return RValue(Renderer2D::save_bitmap_bgra(strip, host_path));
    });
    register_builtin("sprite_set_cache_size", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("sprite_set_cache_size_ext", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });

    // Backgrounds
    register_builtin("background_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().backgrounds.size() && r.game().backgrounds[static_cast<usize>(id)].exists);
    });
    register_builtin("background_get_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().backgrounds.size() && r.game().backgrounds[static_cast<usize>(id)].exists) {
            return RValue(r.game().backgrounds[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    });
    register_builtin("background_get_width", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().backgrounds.size() && r.game().backgrounds[static_cast<usize>(id)].exists)
                          ? r.game().backgrounds[static_cast<usize>(id)].width : 0);
    });
    register_builtin("background_get_height", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().backgrounds.size() && r.game().backgrounds[static_cast<usize>(id)].exists)
                          ? r.game().backgrounds[static_cast<usize>(id)].height : 0);
    });
    register_builtin("background_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(-1);
        auto bmp = Renderer2D::decode_image_to_bgra(bytes.value());
        if (!bmp) return RValue(-1);
        if (arg_bool(a, 1, false)) remove_bottom_left_bg_color(bmp.value());
        BackgroundData bg{};
        bg.exists = true;
        bg.width = bmp.value().width;
        bg.height = bmp.value().height;
        bg.bitmap = std::move(bmp.value());
        const usize id = r.game().backgrounds.size();
        r.game().backgrounds.push_back(std::move(bg));
        r.renderer().upload_background(id, r.game().backgrounds[id]);
        return RValue(static_cast<i32>(id));
    });
    register_builtin("background_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().backgrounds.size()) {
            r.game().backgrounds[static_cast<usize>(id)].exists = false;
            r.renderer().free_background_texture(static_cast<usize>(id));
        }
        return RValue(0.0);
    });
    register_builtin("background_duplicate", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().backgrounds.size() || !r.game().backgrounds[static_cast<usize>(id)].exists) {
            return RValue(-1);
        }
        BackgroundData copy = r.game().backgrounds[static_cast<usize>(id)];
        const usize new_id = r.game().backgrounds.size();
        r.game().backgrounds.push_back(std::move(copy));
        r.renderer().upload_background(new_id, r.game().backgrounds[new_id]);
        return RValue(static_cast<i32>(new_id));
    });
    register_builtin("background_assign", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1);
        const i32 src = arg_i32(a, 1, -1);
        if (dst >= 0 && static_cast<usize>(dst) < r.game().backgrounds.size() &&
            src >= 0 && static_cast<usize>(src) < r.game().backgrounds.size() &&
            r.game().backgrounds[static_cast<usize>(src)].exists) {
            std::string keep_name = r.game().backgrounds[static_cast<usize>(dst)].name;
            r.game().backgrounds[static_cast<usize>(dst)] = r.game().backgrounds[static_cast<usize>(src)];
            r.game().backgrounds[static_cast<usize>(dst)].name = std::move(keep_name);
            r.renderer().upload_background(static_cast<usize>(dst), r.game().backgrounds[static_cast<usize>(dst)]);
        }
        return RValue(0.0);
    });
    register_builtin("background_replace", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().backgrounds.size()) return RValue(false);
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 1));
        if (!bytes) return RValue(false);
        auto bmp = Renderer2D::decode_image_to_bgra(bytes.value());
        if (!bmp) return RValue(false);
        if (arg_bool(a, 2, false)) remove_bottom_left_bg_color(bmp.value());
        auto& bg = r.game().backgrounds[static_cast<usize>(id)];
        bg.exists = true;
        bg.width = bmp.value().width;
        bg.height = bmp.value().height;
        bg.bitmap = std::move(bmp.value());
        r.renderer().upload_background(static_cast<usize>(id), bg);
        return RValue(true);
    });
    register_builtin("background_create_from_surface", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 sid = arg_i32(a, 0, -1);
        const i32 x = arg_i32(a, 1), y = arg_i32(a, 2), w = arg_i32(a, 3), h = arg_i32(a, 4);
        std::vector<u8> bgra;
        if (!r.renderer().surface_read_pixels_bgra(sid, x, y, w, h, bgra)) return RValue(-1);
        Bitmap32 bmp{w, h, std::move(bgra)};
        if (arg_bool(a, 5, false)) remove_bottom_left_bg_color(bmp);
        BackgroundData bg{};
        bg.exists = true;
        bg.width = w;
        bg.height = h;
        bg.bitmap = std::move(bmp);
        const usize id = r.game().backgrounds.size();
        r.game().backgrounds.push_back(std::move(bg));
        r.renderer().upload_background(id, r.game().backgrounds[id]);
        return RValue(static_cast<i32>(id));
    });
    register_builtin("background_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().backgrounds.size() && r.game().backgrounds[static_cast<usize>(id)].exists) {
            return RValue(r.game().backgrounds[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    });
    register_builtin("background_create_from_screen", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 x = arg_i32(a, 0), y = arg_i32(a, 1), w = arg_i32(a, 2), h = arg_i32(a, 3);
        std::vector<u8> bgra;
        if (!r.renderer().screen_read_pixels_bgra(x, y, w, h, bgra)) return RValue(-1);
        return RValue(r.create_background_from_bitmap(Bitmap32{w, h, std::move(bgra)}, arg_bool(a, 4, false), arg_bool(a, 5, false)));
    });
    register_builtin("background_create_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 w = std::max(1, arg_i32(a, 0, 1)), h = std::max(1, arg_i32(a, 1, 1));
        const u32 col = static_cast<u32>(arg_i32(a, 2));
        const u8 cr = static_cast<u8>(col & 0xFFu), cg = static_cast<u8>((col >> 8) & 0xFFu), cb = static_cast<u8>((col >> 16) & 0xFFu);
        Bitmap32 bmp{w, h, std::vector<u8>(static_cast<usize>(w) * static_cast<usize>(h) * 4u, 255u)};
        for (usize p = 0; p + 3 < bmp.bgra.size(); p += 4) {
            bmp.bgra[p + 0] = cb; bmp.bgra[p + 1] = cg; bmp.bgra[p + 2] = cr; bmp.bgra[p + 3] = 255u;
        }
        return RValue(r.create_background_from_bitmap(std::move(bmp), false, false));
    });
    register_builtin("background_create_gradient", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 w = std::max(1, arg_i32(a, 0, 1)), h = std::max(1, arg_i32(a, 1, 1));
        const u32 c1 = static_cast<u32>(arg_i32(a, 2)), c2 = static_cast<u32>(arg_i32(a, 3));
        const i32 kind = arg_i32(a, 4, 0);
        const f64 r1 = static_cast<f64>(c1 & 0xFFu), g1 = static_cast<f64>((c1 >> 8) & 0xFFu), b1 = static_cast<f64>((c1 >> 16) & 0xFFu);
        const f64 r2 = static_cast<f64>(c2 & 0xFFu), g2 = static_cast<f64>((c2 >> 8) & 0xFFu), b2 = static_cast<f64>((c2 >> 16) & 0xFFu);
        Bitmap32 bmp{w, h, std::vector<u8>(static_cast<usize>(w) * static_cast<usize>(h) * 4u, 255u)};
        for (i32 y = 0; y < h; ++y) {
            for (i32 x = 0; x < w; ++x) {
                const f64 tx = (w > 1) ? static_cast<f64>(x) / static_cast<f64>(w - 1) : 0.0;
                const f64 ty = (h > 1) ? static_cast<f64>(y) / static_cast<f64>(h - 1) : 0.0;
                f64 t = tx;
                if (kind == 1) t = ty;
                else if (kind == 2) t = std::max(std::fabs(tx * 2.0 - 1.0), std::fabs(ty * 2.0 - 1.0));
                else if (kind == 3) t = std::min(1.0, std::hypot(tx * 2.0 - 1.0, ty * 2.0 - 1.0));
                else if (kind == 4) t = std::fabs(tx * 2.0 - 1.0);
                else if (kind == 5) t = std::fabs(ty * 2.0 - 1.0);
                t = std::clamp(t, 0.0, 1.0);
                const usize p = (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4u;
                bmp.bgra[p + 0] = static_cast<u8>(b1 + (b2 - b1) * t + 0.5);
                bmp.bgra[p + 1] = static_cast<u8>(g1 + (g2 - g1) * t + 0.5);
                bmp.bgra[p + 2] = static_cast<u8>(r1 + (r2 - r1) * t + 0.5);
                bmp.bgra[p + 3] = 255u;
            }
        }
        return RValue(r.create_background_from_bitmap(std::move(bmp), false, false));
    });
    register_builtin("background_add_background", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(-1);
        auto bmp = Renderer2D::decode_image_to_bgra(bytes.value());
        if (!bmp) return RValue(-1);
        return RValue(r.create_background_from_bitmap(std::move(bmp.value()), false, false));
    });
    register_builtin("background_replace_background", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().backgrounds.size()) return RValue(false);
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 1));
        if (!bytes) return RValue(false);
        auto bmp = Renderer2D::decode_image_to_bgra(bytes.value());
        if (!bmp) return RValue(false);
        auto& bg = r.game().backgrounds[static_cast<usize>(id)];
        bg.exists = true;
        bg.width = bmp.value().width;
        bg.height = bmp.value().height;
        bg.bitmap = std::move(bmp.value());
        r.renderer().upload_background(static_cast<usize>(id), bg);
        return RValue(true);
    });
    register_builtin("background_set_alpha_from_background", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 dst = arg_i32(a, 0, -1), src = arg_i32(a, 1, -1);
        if (dst >= 0 && static_cast<usize>(dst) < r.game().backgrounds.size() && r.game().backgrounds[static_cast<usize>(dst)].exists &&
            src >= 0 && static_cast<usize>(src) < r.game().backgrounds.size() && r.game().backgrounds[static_cast<usize>(src)].exists) {
            auto& d = r.game().backgrounds[static_cast<usize>(dst)];
            const auto& s = r.game().backgrounds[static_cast<usize>(src)];
            const i32 mw = std::min(d.bitmap.width, s.bitmap.width);
            const i32 mh = std::min(d.bitmap.height, s.bitmap.height);
            for (i32 y = 0; y < mh; ++y) {
                for (i32 x = 0; x < mw; ++x) {
                    const usize dp = (static_cast<usize>(y) * static_cast<usize>(d.bitmap.width) + static_cast<usize>(x)) * 4u;
                    const usize sp = (static_cast<usize>(y) * static_cast<usize>(s.bitmap.width) + static_cast<usize>(x)) * 4u;
                    const u8 lum = static_cast<u8>((static_cast<u32>(s.bitmap.bgra[sp + 2]) * 77u +
                                                    static_cast<u32>(s.bitmap.bgra[sp + 1]) * 150u +
                                                    static_cast<u32>(s.bitmap.bgra[sp + 0]) * 29u) >> 8u);
                    d.bitmap.bgra[dp + 3] = lum;
                }
            }
            r.renderer().upload_background(static_cast<usize>(dst), d);
        }
        return RValue(0.0);
    });
    register_builtin("background_save", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id < 0 || static_cast<usize>(id) >= r.game().backgrounds.size() || !r.game().backgrounds[static_cast<usize>(id)].exists) return RValue(false);
        const auto host_path = Vfs::path_to_utf8(r.vfs().resolve_for_write(arg_str(a, 1)));
        return RValue(Renderer2D::save_bitmap_bgra(r.game().backgrounds[static_cast<usize>(id)].bitmap, host_path));
    });

    // Fonts & Timelines
    register_builtin("font_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists);
    });
    auto fn_font_name = [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists) {
            return RValue(r.game().fonts[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    };
    register_builtin("font_get_name", fn_font_name);
    register_builtin("font_name", fn_font_name);
    register_builtin("font_get_fontname", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists)
                          ? r.game().fonts[static_cast<usize>(id)].font_name : std::string{});
    });
    register_builtin("font_get_size", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists)
                          ? r.game().fonts[static_cast<usize>(id)].size : 0);
    });
    register_builtin("font_get_bold", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists &&
                      r.game().fonts[static_cast<usize>(id)].bold);
    });
    register_builtin("font_get_italic", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists &&
                      r.game().fonts[static_cast<usize>(id)].italic);
    });
    register_builtin("font_get_first", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists)
                          ? r.game().fonts[static_cast<usize>(id)].first : 0);
    });
    register_builtin("font_get_last", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue((id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists)
                          ? r.game().fonts[static_cast<usize>(id)].last : 0);
    });
    register_builtin("font_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        FontData f{};
        f.exists = true;
        f.font_name = std::string(arg_str(a, 0));
        f.size = std::max(1, arg_i32(a, 1, 12));
        f.bold = arg_bool(a, 2, false);
        f.italic = arg_bool(a, 3, false);
        f.first = arg_i32(a, 4, 32);
        f.last = arg_i32(a, 5, 127);
        const usize id = r.game().fonts.size();
        r.game().fonts.push_back(std::move(f));
        return RValue(static_cast<i32>(id));
    });
    register_builtin("font_replace", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().fonts.size()) {
            auto& f = r.game().fonts[static_cast<usize>(id)];
            f.exists = true;
            f.font_name = std::string(arg_str(a, 1));
            f.size = std::max(1, arg_i32(a, 2, 12));
            f.bold = arg_bool(a, 3, false);
            f.italic = arg_bool(a, 4, false);
            f.first = arg_i32(a, 5, 32);
            f.last = arg_i32(a, 6, 127);
        }
        return RValue(0.0);
    });
    register_builtin("font_add_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        FontData f{};
        f.exists = true;
        f.first = arg_i32(a, 1, 32);
        const usize id = r.game().fonts.size();
        r.game().fonts.push_back(std::move(f));
        r.renderer().register_sprite_font(id, arg_i32(a, 0, -1), arg_i32(a, 1, 32), arg_bool(a, 2, false), arg_i32(a, 3, 0), r.game());
        return RValue(static_cast<i32>(id));
    });
    register_builtin("font_replace_sprite", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().fonts.size()) {
            r.game().fonts[static_cast<usize>(id)].exists = true;
            r.game().fonts[static_cast<usize>(id)].first = arg_i32(a, 2, 32);
            r.renderer().register_sprite_font(static_cast<usize>(id), arg_i32(a, 1, -1), arg_i32(a, 2, 32),
                                              arg_bool(a, 3, false), arg_i32(a, 4, 0), r.game());
        }
        return RValue(0.0);
    });
    register_builtin("font_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().fonts.size()) r.game().fonts[static_cast<usize>(id)].exists = false;
        return RValue(0.0);
    });
    register_builtin("timeline_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().timelines.size() && r.game().timelines[static_cast<usize>(id)].exists);
    });
    auto fn_timeline_name = [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().timelines.size() && r.game().timelines[static_cast<usize>(id)].exists) {
            return RValue(r.game().timelines[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    };
    register_builtin("timeline_get_name", fn_timeline_name);
    register_builtin("timeline_name", fn_timeline_name);
    register_builtin("timeline_add", [](Runner& r, i32, i32, std::span<const RValue>) {
        TimelineData tl{};
        tl.exists = true;
        const i32 id = static_cast<i32>(r.game().timelines.size());
        r.game().timelines.push_back(std::move(tl));
        if (r.program().timeline_chunks.size() <= static_cast<usize>(id)) {
            r.program().timeline_chunks.resize(static_cast<usize>(id) + 1);
        }
        return RValue(id);
    });
    register_builtin("timeline_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().timelines.size()) {
            r.timeline_clear(id);
            r.game().timelines[static_cast<usize>(id)].exists = false;
        }
        return RValue(0.0);
    });
    register_builtin("timeline_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.timeline_clear(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("timeline_moment_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        (void)r.timeline_moment_add(arg_i32(a, 0, -1), arg_i32(a, 1), arg_str(a, 2));
        return RValue(0.0);
    });
    register_builtin("timeline_moment_clear", [](Runner& r, i32, i32, std::span<const RValue> a) {
        (void)r.timeline_moment_clear(arg_i32(a, 0, -1), arg_i32(a, 1));
        return RValue(0.0);
    });
}

} // namespace gm
