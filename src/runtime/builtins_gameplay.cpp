#include "runtime/runner.h"

#include "base/log.h"
#include "base/math.h"
#include "runtime/collision.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
    register_builtin("instance_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 target = arg_i32(a, 0, -4);
        if (target == -4) return RValue(false);
        if (target >= 100000) {
            const auto* inst = r.find_instance(target);
            return RValue(inst != nullptr && !inst->destroyed && !inst->deactivated);
        }
        for (const auto& up : r.instances()) {
            if (!up->destroyed && !up->deactivated && r.object_matches(up->object_index, target)) {
                return RValue(true);
            }
        }
        return RValue(false);
    });
    register_builtin("instance_number", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 target = arg_i32(a, 0, -4);
        if (target == -4) return RValue(0);
        i32 cnt = 0;
        for (const auto& up : r.instances()) {
            if (!up->destroyed && !up->deactivated && r.object_matches(up->object_index, target)) {
                ++cnt;
            }
        }
        return RValue(cnt);
    });
    register_builtin("instance_find", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 target = arg_i32(a, 0, -4);
        const i32 n = arg_i32(a, 1, 0);
        if (target == -4 || n < 0) return RValue(-4);
        i32 cur = 0;
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated) continue;
            if (r.object_matches(up->object_index, target)) {
                if (cur == n) return RValue(up->id);
                ++cur;
            }
        }
        return RValue(-4);
    });
    register_builtin("instance_position", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const i32 target = arg_i32(a, 2, -3);
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            if (check_point_collision(*up, px, py, true, r.game(), r.version_mode())) {
                return RValue(up->id);
            }
        }
        return RValue(-4);
    });
    register_builtin("instance_place", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(-4);
        return RValue(r.find_collision_instance(*self, arg_real(a, 0), arg_real(a, 1), arg_i32(a, 2, -3), false));
    });
    register_builtin("instance_nearest", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const i32 target = arg_i32(a, 2, -3);
        i32 best_id = -4;
        f64 best_d2 = std::numeric_limits<f64>::infinity();
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            const f64 dx = up->x - px;
            const f64 dy = up->y - py;
            const f64 d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_id = up->id;
            }
        }
        return RValue(best_id);
    });
    register_builtin("instance_furthest", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const i32 target = arg_i32(a, 2, -3);
        i32 best_id = -4;
        f64 best_d2 = -1.0;
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            const f64 dx = up->x - px;
            const f64 dy = up->y - py;
            const f64 d2 = dx * dx + dy * dy;
            if (d2 > best_d2) {
                best_d2 = d2;
                best_id = up->id;
            }
        }
        return RValue(best_id);
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
    register_builtin("instance_deactivate_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.deactivate_object(arg_i32(a, 0, -4), self_id);
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
    register_builtin("instance_activate_object", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.activate_object(arg_i32(a, 0, -4));
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
    register_builtin("place_meeting", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        return RValue(self ? r.check_place_meeting(*self, arg_real(a, 0), arg_real(a, 1), arg_i32(a, 2, -3)) : false);
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
    register_builtin("distance_to_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(1000000.0);
        ensure_instance_bbox(*self, r.game());
        const i32 target = arg_i32(a, 0, -4);
        f64 best = 1000000.0;
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated || up->id == self_id) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            ensure_instance_bbox(*up, r.game());
            const f64 dx = std::max({0.0, static_cast<f64>(self->bbox_left - up->bbox_right), static_cast<f64>(up->bbox_left - self->bbox_right)});
            const f64 dy = std::max({0.0, static_cast<f64>(self->bbox_top - up->bbox_bottom), static_cast<f64>(up->bbox_top - self->bbox_bottom)});
            best = std::min(best, std::hypot(dx, dy));
        }
        return RValue(best);
    });
    register_builtin("position_empty", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        for (const auto& up : r.instances()) {
            if (!up->destroyed && !up->deactivated && check_point_collision(*up, px, py, true, r.game(), r.version_mode())) {
                return RValue(false);
            }
        }
        return RValue(true);
    });
    register_builtin("position_meeting", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const i32 target = arg_i32(a, 2, -3);
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            if (check_point_collision(*up, px, py, true, r.game(), r.version_mode())) return RValue(true);
        }
        return RValue(false);
    });
    register_builtin("position_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        std::vector<i32> hits;
        for (const auto& up : r.instances()) {
            if (!up->destroyed && !up->deactivated && check_point_collision(*up, px, py, true, r.game(), r.version_mode())) {
                hits.push_back(up->id);
            }
        }
        for (i32 id : hits) r.destroy_instance(id, true);
        return RValue(0.0);
    });
    register_builtin("position_change", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 px = arg_real(a, 0);
        const f64 py = arg_real(a, 1);
        const i32 obj = arg_i32(a, 2, -1);
        const bool perf = arg_bool(a, 3, true);
        std::vector<i32> hits;
        for (const auto& up : r.instances()) {
            if (!up->destroyed && !up->deactivated && check_point_collision(*up, px, py, true, r.game(), r.version_mode())) {
                hits.push_back(up->id);
            }
        }
        for (i32 id : hits) r.change_instance(id, obj, perf);
        return RValue(0.0);
    });

    // Shape collision queries
    register_builtin("collision_point", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 x = arg_real(a, 0), y = arg_real(a, 1);
        const i32 target = arg_i32(a, 2, -3);
        const bool prec = arg_bool(a, 3, true), notme = arg_bool(a, 4, true);
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated || (notme && up->id == self_id)) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            if (check_point_collision(*up, x, y, prec, r.game(), r.version_mode())) return RValue(up->id);
        }
        return RValue(-4);
    });
    register_builtin("collision_rectangle", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 x1 = arg_real(a, 0), y1 = arg_real(a, 1), x2 = arg_real(a, 2), y2 = arg_real(a, 3);
        const i32 target = arg_i32(a, 4, -3);
        const bool prec = arg_bool(a, 5, true), notme = arg_bool(a, 6, true);
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated || (notme && up->id == self_id)) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            if (check_rect_collision(*up, x1, y1, x2, y2, prec, r.game(), r.version_mode())) return RValue(up->id);
        }
        return RValue(-4);
    });
    register_builtin("collision_circle", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 cx = arg_real(a, 0), cy = arg_real(a, 1), rad = arg_real(a, 2);
        const i32 target = arg_i32(a, 3, -3);
        const bool prec = arg_bool(a, 4, true), notme = arg_bool(a, 5, true);
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated || (notme && up->id == self_id)) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            if (check_circle_collision(*up, cx, cy, rad, prec, r.game(), r.version_mode())) return RValue(up->id);
        }
        return RValue(-4);
    });
    register_builtin("collision_ellipse", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 x1 = arg_real(a, 0), y1 = arg_real(a, 1), x2 = arg_real(a, 2), y2 = arg_real(a, 3);
        const i32 target = arg_i32(a, 4, -3);
        const bool prec = arg_bool(a, 5, true), notme = arg_bool(a, 6, true);
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated || (notme && up->id == self_id)) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            if (check_ellipse_collision(*up, x1, y1, x2, y2, prec, r.game(), r.version_mode())) return RValue(up->id);
        }
        return RValue(-4);
    });
    register_builtin("collision_line", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const f64 x1 = arg_real(a, 0), y1 = arg_real(a, 1), x2 = arg_real(a, 2), y2 = arg_real(a, 3);
        const i32 target = arg_i32(a, 4, -3);
        const bool prec = arg_bool(a, 5, true), notme = arg_bool(a, 6, true);
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated || (notme && up->id == self_id)) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            if (check_line_collision(*up, x1, y1, x2, y2, prec, r.game(), r.version_mode())) return RValue(up->id);
        }
        return RValue(-4);
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
            return RValue(compute_path_length(r.game().paths[static_cast<usize>(id)]));
        }
        return RValue(0.0);
    });
    register_builtin("path_get_x", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            f64 px = 0.0, py = 0.0;
            evaluate_path_point(r.game().paths[static_cast<usize>(id)], arg_real(a, 1), px, py);
            return RValue(px);
        }
        return RValue(0.0);
    });
    register_builtin("path_get_y", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            f64 px = 0.0, py = 0.0;
            evaluate_path_point(r.game().paths[static_cast<usize>(id)], arg_real(a, 1), px, py);
            return RValue(py);
        }
        return RValue(0.0);
    });
    register_builtin("path_get_number", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size() && r.game().paths[static_cast<usize>(id)].exists) {
            return RValue(static_cast<i32>(r.game().paths[static_cast<usize>(id)].points.size()));
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
            r.game().paths[static_cast<usize>(id)].points.push_back(PathPoint{arg_real(a, 1), arg_real(a, 2), arg_real(a, 3, 100.0)});
        }
        return RValue(0.0);
    });
    register_builtin("path_clear_points", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().paths.size()) r.game().paths[static_cast<usize>(id)].points.clear();
        return RValue(0.0);
    });

    // MP Step & Grid
    register_builtin("mp_linear_step", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        const f64 gx = arg_real(a, 0), gy = arg_real(a, 1), sp = arg_real(a, 2);
        const bool checkall = arg_bool(a, 3, false);
        const f64 dx = gx - self->x, dy = gy - self->y;
        const f64 dist = std::hypot(dx, dy);
        if (dist <= sp) {
            if (r.find_collision_instance(*self, gx, gy, -3, !checkall) == -4) {
                self->x = gx; self->y = gy; self->mark_bbox_dirty();
                return RValue(true);
            }
            return RValue(false);
        }
        const f64 nx = self->x + dx * sp / dist;
        const f64 ny = self->y + dy * sp / dist;
        if (r.find_collision_instance(*self, nx, ny, -3, !checkall) == -4) {
            self->x = nx; self->y = ny; self->mark_bbox_dirty();
        }
        return RValue(false);
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
    register_builtin("mp_grid_add_instances", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 gid = arg_i32(a, 0);
        const i32 target = arg_i32(a, 1, -3);
        const bool prec = arg_bool(a, 2, false);
        auto* g = r.mp_grids().get(gid);
        if (!g) return RValue(0.0);
        for (const auto& up : r.instances()) {
            if (up->destroyed || up->deactivated) continue;
            if (target >= 100000 ? (up->id != target) : !r.object_matches(up->object_index, target)) continue;
            ensure_instance_bbox(*up, r.game());
            if (!prec) {
                r.mp_grids().add_rectangle(gid, up->bbox_left, up->bbox_top, up->bbox_right, up->bbox_bottom);
            } else {
                const i32 x0 = std::max(0, static_cast<i32>(std::floor((up->bbox_left - g->left) / g->cell_w)));
                const i32 y0 = std::max(0, static_cast<i32>(std::floor((up->bbox_top - g->top) / g->cell_h)));
                const i32 x1 = std::min(g->hcells - 1, static_cast<i32>(std::floor((up->bbox_right - g->left) / g->cell_w)));
                const i32 y1 = std::min(g->vcells - 1, static_cast<i32>(std::floor((up->bbox_bottom - g->top) / g->cell_h)));
                for (i32 cy = y0; cy <= y1; ++cy) {
                    for (i32 cx = x0; cx <= x1; ++cx) {
                        const f64 rx1 = g->left + cx * g->cell_w;
                        const f64 ry1 = g->top + cy * g->cell_h;
                        if (check_rect_collision(*up, rx1, ry1, rx1 + g->cell_w, ry1 + g->cell_h, true, r.game(), r.version_mode())) {
                            r.mp_grids().add_cell(gid, cx, cy);
                        }
                    }
                }
            }
        }
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
        ri.id = ++r.game().room_max_id;
        r.game().rooms[static_cast<usize>(id)].instances.push_back(std::move(ri));
        return RValue(r.game().room_max_id);
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
        r.game().room_order.push_back(id);
        return RValue(id);
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
        return RValue(check_key_array(r.window().input().key_down, arg_i32(a, 0)));
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
        return RValue(0.0);
    });
    register_builtin("joystick_exists", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(false); });
    register_builtin("joystick_buttons", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0); });
    register_builtin("joystick_check_button", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(false); });
    register_builtin("joystick_direction", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(101); }); // vk_numpad5 = center
    register_builtin("joystick_xpos", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("joystick_ypos", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("joystick_zpos", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("joystick_rpos", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("joystick_upos", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("joystick_vpos", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("joystick_pov", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(-1.0); });

    // -----------------------------------------------------------------------
    // Sound
    // -----------------------------------------------------------------------
    register_builtin("sound_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.audio().sound_exists(arg_i32(a, 0, -1)));
    });
    register_builtin("sound_get_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sounds.size() && r.game().sounds[static_cast<usize>(id)].exists) {
            return RValue(r.game().sounds[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    });
    register_builtin("sound_get_kind", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().sounds.size() && r.game().sounds[static_cast<usize>(id)].exists) {
            return RValue(r.game().sounds[static_cast<usize>(id)].kind);
        }
        return RValue(0);
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
    register_builtin("sound_add", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 0));
        if (!bytes) return RValue(-1);
        return RValue(r.audio().add_sound_from_memory(bytes.value(), arg_i32(a, 1), arg_str(a, 0)));
    });
    register_builtin("sound_replace", [](Runner& r, i32, i32, std::span<const RValue> a) {
        auto bytes = r.vfs().read_all_bytes(arg_str(a, 1));
        if (!bytes) return RValue(false);
        return RValue(r.audio().replace_sound_from_memory(arg_i32(a, 0, -1), bytes.value(), arg_i32(a, 2), arg_str(a, 1)));
    });
    register_builtin("sound_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().delete_sound(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("sound_discard", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("sound_restore", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("sound_effect_set", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });

    // -----------------------------------------------------------------------
    // Particles & Simple Effects
    // -----------------------------------------------------------------------
    register_builtin("effect_create_below", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().effect_create(false, arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_i32(a, 3), static_cast<u32>(arg_i32(a, 4)) & 0xFFFFFFu);
        return RValue(0.0);
    });
    register_builtin("effect_create_above", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.particles().effect_create(true, arg_i32(a, 0), arg_real(a, 1), arg_real(a, 2), arg_i32(a, 3), static_cast<u32>(arg_i32(a, 4)) & 0xFFFFFFu);
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
    register_builtin("part_emitter_create", [](Runner& r, i32, i32, std::span<const RValue> a) { return RValue(r.particles().emitter_create(arg_i32(a, 0))); });
    register_builtin("part_emitter_destroy", [](Runner& r, i32, i32, std::span<const RValue> a) { r.particles().emitter_destroy(arg_i32(a, 0), arg_i32(a, 1)); return RValue(0.0); });
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
        if (r.object_exists(id)) r.game().objects[static_cast<usize>(id)].parent_index = arg_i32(a, 1, -100);
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

    // Fonts & Timelines
    register_builtin("font_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists);
    });
    register_builtin("font_get_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().fonts.size() && r.game().fonts[static_cast<usize>(id)].exists) {
            return RValue(r.game().fonts[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
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
    register_builtin("font_delete", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().fonts.size()) r.game().fonts[static_cast<usize>(id)].exists = false;
        return RValue(0.0);
    });
    register_builtin("timeline_exists", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        return RValue(id >= 0 && static_cast<usize>(id) < r.game().timelines.size() && r.game().timelines[static_cast<usize>(id)].exists);
    });
    register_builtin("timeline_get_name", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 id = arg_i32(a, 0, -1);
        if (id >= 0 && static_cast<usize>(id) < r.game().timelines.size() && r.game().timelines[static_cast<usize>(id)].exists) {
            return RValue(r.game().timelines[static_cast<usize>(id)].name);
        }
        return RValue(std::string("<undefined>"));
    });
}

} // namespace gm
