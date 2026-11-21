#include "runtime/runner.h"

#include "base/math.h"
#include "platform/clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace gm {

namespace {

bool compare_dnd_op(f64 lhs, f64 rhs, i32 op) noexcept {
    switch (op) {
        case 0: return gml_equals(lhs, rhs);
        case 1: return gml_less(lhs, rhs);
        case 2: return gml_greater(lhs, rhs);
        case 3: return gml_less_eq(lhs, rhs);
        case 4: return gml_greater_eq(lhs, rhs);
        default: return false;
    }
}

std::string dnd_val_to_str(const RValue& v) {
    if (v.is_string()) return v.as_string();
    const f64 d = v.as_real();
    const i64 r = delphi_round(d);
    if (std::fabs(d - static_cast<f64>(r)) < kGmlEpsilon && std::fabs(d) < 1e15) {
        return std::to_string(r);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", d);
    return std::string(buf);
}

} // namespace

void Runner::register_builtins_actions() {
    register_builtin("action_set_relative", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.action_relative_ = arg_bool(a, 0);
        return RValue(0.0);
    });

    // Movement actions
    register_builtin("action_move", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        auto* inst = r.find_instance(self_id);
        if (!inst) return RValue(0.0);
        const std::string_view mask = arg_str(a, 0);
        const f64 sp = arg_real(a, 1);
        static constexpr f64 kDirs[9] = {225.0, 270.0, 315.0, 180.0, -1.0, 0.0, 135.0, 90.0, 45.0};
        std::vector<i32> choices;
        for (usize i = 0; i < std::min<usize>(mask.size(), 9); ++i) {
            if (mask[i] == '1') choices.push_back(static_cast<i32>(i));
        }
        if (choices.empty()) return RValue(0.0);
        const i32 pick = choices[static_cast<usize>(r.random_int_inclusive(static_cast<i64>(choices.size() - 1)))];
        if (pick == 4) { // center = stop
            inst->speed = 0.0;
            inst->hspeed = 0.0;
            inst->vspeed = 0.0;
        } else {
            const f64 dir = kDirs[pick];
            if (r.action_relative_) {
                const f64 rad = deg_to_rad(dir);
                inst->hspeed += sp * std::cos(rad);
                inst->vspeed -= sp * std::sin(rad);
                inst->sync_polar_from_hv();
            } else {
                inst->direction = dir;
                inst->speed = sp;
                inst->sync_hv_from_polar();
            }
        }
        return RValue(0.0);
    });
    register_builtin("action_set_motion", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            const f64 dir = arg_real(a, 0);
            const f64 sp = arg_real(a, 1);
            if (r.action_relative_) {
                const f64 rad = deg_to_rad(dir);
                inst->hspeed += sp * std::cos(rad);
                inst->vspeed -= sp * std::sin(rad);
                inst->sync_polar_from_hv();
            } else {
                inst->direction = std::fmod(std::fmod(dir, 360.0) + 360.0, 360.0);
                inst->speed = sp;
                inst->sync_hv_from_polar();
            }
        }
        return RValue(0.0);
    });
    register_builtin("action_set_hspeed", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->hspeed = r.action_relative_ ? (inst->hspeed + arg_real(a, 0)) : arg_real(a, 0);
            inst->sync_polar_from_hv();
        }
        return RValue(0.0);
    });
    register_builtin("action_set_vspeed", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->vspeed = r.action_relative_ ? (inst->vspeed + arg_real(a, 0)) : arg_real(a, 0);
            inst->sync_polar_from_hv();
        }
        return RValue(0.0);
    });
    register_builtin("action_set_gravity", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            if (r.action_relative_) {
                inst->gravity_direction += arg_real(a, 0);
                inst->gravity += arg_real(a, 1);
            } else {
                inst->gravity_direction = arg_real(a, 0);
                inst->gravity = arg_real(a, 1);
            }
        }
        return RValue(0.0);
    });
    register_builtin("action_set_friction", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->friction = r.action_relative_ ? (inst->friction + arg_real(a, 0)) : arg_real(a, 0);
        }
        return RValue(0.0);
    });
    register_builtin("action_move_point", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            const f64 tx = r.action_relative_ ? (inst->x + arg_real(a, 0)) : arg_real(a, 0);
            const f64 ty = r.action_relative_ ? (inst->y + arg_real(a, 1)) : arg_real(a, 1);
            f64 deg = rad_to_deg(std::atan2(-(ty - inst->y), tx - inst->x));
            if (deg < 0.0) deg += 360.0;
            inst->direction = std::fmod(deg, 360.0);
            inst->speed = arg_real(a, 2);
            inst->sync_hv_from_polar();
        }
        return RValue(0.0);
    });
    register_builtin("action_move_to", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->x = r.action_relative_ ? (inst->x + arg_real(a, 0)) : arg_real(a, 0);
            inst->y = r.action_relative_ ? (inst->y + arg_real(a, 1)) : arg_real(a, 1);
            inst->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("action_move_start", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->x = inst->xstart;
            inst->y = inst->ystart;
            inst->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("action_move_random", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            const f64 hs = std::max(1.0, arg_real(a, 0, 1.0));
            const f64 vs = std::max(1.0, arg_real(a, 1, 1.0));
            inst->x = std::floor(r.random_uniform(static_cast<f64>(r.room_width_)) / hs) * hs;
            inst->y = std::floor(r.random_uniform(static_cast<f64>(r.room_height_)) / vs) * vs;
            inst->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("action_snap", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            const f64 hs = arg_real(a, 0, 1.0), vs = arg_real(a, 1, 1.0);
            if (hs > 0.0) inst->x = std::floor(inst->x / hs + 0.5) * hs;
            if (vs > 0.0) inst->y = std::floor(inst->y / vs + 0.5) * vs;
            inst->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("action_wrap", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            const i32 dir = arg_i32(a, 0, 2); // 0=hor, 1=ver, 2=both
            if (dir == 0 || dir == 2) {
                if (inst->hspeed < 0.0 && inst->x < 0.0) inst->x += r.room_width_;
                else if (inst->hspeed > 0.0 && inst->x > r.room_width_) inst->x -= r.room_width_;
            }
            if (dir == 1 || dir == 2) {
                if (inst->vspeed < 0.0 && inst->y < 0.0) inst->y += r.room_height_;
                else if (inst->vspeed > 0.0 && inst->y > r.room_height_) inst->y -= r.room_height_;
            }
            inst->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("action_reverse_xdir", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (auto* inst = r.find_instance(self_id)) { inst->hspeed = -inst->hspeed; inst->sync_polar_from_hv(); }
        return RValue(0.0);
    });
    register_builtin("action_reverse_ydir", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (auto* inst = r.find_instance(self_id)) { inst->vspeed = -inst->vspeed; inst->sync_polar_from_hv(); }
        return RValue(0.0);
    });
    register_builtin("action_move_contact", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            r.move_contact(*inst, arg_real(a, 0), arg_real(a, 1, 1000.0), arg_i32(a, 2, 0) == 0);
        }
        return RValue(0.0);
    });
    register_builtin("action_bounce", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            r.move_bounce(*inst, arg_bool(a, 0), arg_i32(a, 1, 0) == 0);
        }
        return RValue(0.0);
    });
    register_builtin("action_path", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            r.start_path(*inst, arg_i32(a, 0, -1), arg_real(a, 1), arg_i32(a, 2), arg_bool(a, 3));
        }
        return RValue(0.0);
    });
    register_builtin("action_path_end", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (auto* inst = r.find_instance(self_id)) r.end_path(*inst);
        return RValue(0.0);
    });
    register_builtin("action_path_position", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->path_position = r.action_relative_ ? (inst->path_position + arg_real(a, 0)) : arg_real(a, 0);
        }
        return RValue(0.0);
    });
    register_builtin("action_path_speed", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->path_speed = r.action_relative_ ? (inst->path_speed + arg_real(a, 0)) : arg_real(a, 0);
        }
        return RValue(0.0);
    });

    // Instance & Sprite actions
    register_builtin("action_kill_object", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        r.destroy_instance(self_id, true);
        return RValue(0.0);
    });
    register_builtin("action_create_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        return RValue(r.create_instance(bx + arg_real(a, 1), by + arg_real(a, 2), arg_i32(a, 0, -1), true));
    });
    register_builtin("action_create_object_motion", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        const i32 nid = r.create_instance(bx + arg_real(a, 1), by + arg_real(a, 2), arg_i32(a, 0, -1), true);
        if (auto* ni = r.find_instance(nid)) {
            ni->speed = arg_real(a, 3);
            ni->direction = arg_real(a, 4);
            ni->sync_hv_from_polar();
        }
        return RValue(nid);
    });
    register_builtin("action_create_object_random", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        std::vector<i32> objs;
        for (usize i = 0; i < 4; ++i) {
            const i32 oid = arg_i32(a, i, -1);
            if (r.object_exists(oid)) objs.push_back(oid);
        }
        if (objs.empty()) return RValue(-4);
        const i32 pick = objs[static_cast<usize>(r.random_int_inclusive(static_cast<i64>(objs.size() - 1)))];
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        return RValue(r.create_instance(bx + arg_real(a, 4), by + arg_real(a, 5), pick, true));
    });
    register_builtin("action_change_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        r.change_instance(self_id, arg_i32(a, 0, -1), arg_bool(a, 1, true));
        return RValue(0.0);
    });
    register_builtin("action_kill_position", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 px = ((r.action_relative_ && self) ? self->x : 0.0) + arg_real(a, 0);
        const f64 py = ((r.action_relative_ && self) ? self->y : 0.0) + arg_real(a, 1);
        r.for_instances_at_position(px, py, [&](i32 handle) { r.destroy_instance(handle, true); });
        return RValue(0.0);
    });
    register_builtin("action_sprite_set", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->sprite_index = arg_i32(a, 0, -1);
            const f64 sub = arg_real(a, 1, 0.0);
            if (sub >= 0.0) inst->image_index = sub;
            inst->image_speed = arg_real(a, 2, 1.0);
            inst->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("action_set_sprite", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->sprite_index = arg_i32(a, 0, -1);
            inst->image_xscale = inst->image_yscale = arg_real(a, 1, 1.0);
            inst->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("action_sprite_transform", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            f64 sx = arg_real(a, 0, 1.0), sy = arg_real(a, 1, 1.0);
            const i32 mirror = arg_i32(a, 3, 0);
            if (mirror == 1 || mirror == 3) sx = -sx;
            if (mirror == 2 || mirror == 3) sy = -sy;
            inst->image_xscale = sx;
            inst->image_yscale = sy;
            inst->image_angle = arg_real(a, 2, 0.0);
            inst->mark_bbox_dirty();
        }
        return RValue(0.0);
    });
    register_builtin("action_sprite_color", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->image_blend = static_cast<u32>(arg_i32(a, 0, 0xFFFFFF)) & 0xFFFFFFu;
            inst->image_alpha = arg_real(a, 1, 1.0);
        }
        return RValue(0.0);
    });

    // Sound, Rooms, Alarms, Timelines, Game control
    register_builtin("action_sound", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 snd = arg_i32(a, 0, -1);
        if (arg_bool(a, 1)) r.audio().sound_loop(snd);
        else r.audio().sound_play(snd);
        return RValue(0.0);
    });
    register_builtin("action_end_sound", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.audio().sound_stop(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("action_if_sound", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.audio().sound_isplaying(arg_i32(a, 0, -1)));
    });
    register_builtin("action_another_room", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.transition_kind_ = arg_i32(a, 1, 0);
        r.request_room_goto(arg_i32(a, 0, -1));
        return RValue(0.0);
    });
    register_builtin("action_current_room", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.transition_kind_ = arg_i32(a, 0, 0);
        r.request_room_restart();
        return RValue(0.0);
    });
    register_builtin("action_previous_room", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.transition_kind_ = arg_i32(a, 0, 0);
        const i32 prev = r.room_previous_id(r.current_room_);
        if (prev >= 0) r.request_room_goto(prev);
        return RValue(0.0);
    });
    register_builtin("action_next_room", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.transition_kind_ = arg_i32(a, 0, 0);
        const i32 nxt = r.room_next_id(r.current_room_);
        if (nxt >= 0) r.request_room_goto(nxt);
        return RValue(0.0);
    });
    register_builtin("action_if_previous_room", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.room_previous_id(r.current_room_) >= 0);
    });
    register_builtin("action_if_next_room", [](Runner& r, i32, i32, std::span<const RValue>) {
        return RValue(r.room_next_id(r.current_room_) >= 0);
    });
    register_builtin("action_set_alarm", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            const i32 idx = std::clamp(arg_i32(a, 1, 0), 0, 11);
            const i32 val = arg_i32(a, 0, -1);
            inst->alarm[static_cast<usize>(idx)] =
                r.action_relative_ ? (inst->alarm[static_cast<usize>(idx)] + val) : val;
        }
        return RValue(0.0);
    });
    register_builtin("action_sleep", [](Runner& r, i32, i32, std::span<const RValue> a) {
        if (arg_bool(a, 1, false)) {
            r.render_current_frame();
            r.window().swap_buffers();
        }
        const f64 ms = std::max(0.0, arg_real(a, 0));
        if (ms > 0.0) Clock::sleep_until_us(Clock::now_us() + static_cast<u64>(ms * 1000.0));
        return RValue(0.0);
    });
    register_builtin("action_set_timeline", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->timeline_index = arg_i32(a, 0, -1);
            inst->timeline_position = arg_real(a, 1, 0.0);
            inst->timeline_running = true;
        }
        return RValue(0.0);
    });
    register_builtin("action_timeline_set", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->timeline_index = arg_i32(a, 0, -1);
            inst->timeline_position = arg_real(a, 1, 0.0);
            inst->timeline_running = (arg_i32(a, 2, 0) == 0);
            inst->timeline_loop = (arg_i32(a, 3, 0) != 0);
        }
        return RValue(0.0);
    });
    register_builtin("action_timeline_start", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (auto* inst = r.find_instance(self_id)) inst->timeline_running = true;
        return RValue(0.0);
    });
    register_builtin("action_timeline_pause", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (auto* inst = r.find_instance(self_id)) inst->timeline_running = false;
        return RValue(0.0);
    });
    register_builtin("action_timeline_stop", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        if (auto* inst = r.find_instance(self_id)) { inst->timeline_running = false; inst->timeline_position = 0.0; }
        return RValue(0.0);
    });
    register_builtin("action_set_timeline_position", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->timeline_position = r.action_relative_ ? (inst->timeline_position + arg_real(a, 0)) : arg_real(a, 0);
        }
        return RValue(0.0);
    });
    register_builtin("action_set_timeline_speed", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        if (auto* inst = r.find_instance(self_id)) {
            inst->timeline_speed = r.action_relative_ ? (inst->timeline_speed + arg_real(a, 0)) : arg_real(a, 0);
        }
        return RValue(0.0);
    });
    register_builtin("action_message", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.window().show_message(r.room_caption_.empty() ? "GameMaker" : r.room_caption_, a.empty() ? std::string{} : dnd_val_to_str(a[0]));
        return RValue(0.0);
    });
    register_builtin("action_end_game", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.request_game_end();
        return RValue(0.0);
    });
    register_builtin("action_restart_game", [](Runner& r, i32, i32, std::span<const RValue>) {
        r.request_game_restart();
        return RValue(0.0);
    });
    register_builtin("action_inherited", [](Runner& r, i32 self_id, i32, std::span<const RValue>) {
        r.perform_event_inherited(self_id);
        return RValue(0.0);
    });
    register_builtin("action_execute_script", [](Runner& r, i32 self_id, i32 other_id, std::span<const RValue> a) {
        if (a.empty()) return RValue(0.0);
        const i32 sid = arg_i32(a, 0, -1);
        if (sid < 0) return RValue(0.0);
        auto res = r.vm().execute_script(static_cast<u32>(sid), self_id, other_id, a.subspan(1));
        return res ? res.value() : RValue(0.0);
    });

    // Conditional (question) actions
    register_builtin("action_if", [](Runner&, i32, i32, std::span<const RValue> a) { return RValue(arg_bool(a, 0)); });
    register_builtin("action_if_variable", [](Runner&, i32, i32, std::span<const RValue> a) {
        if (a.size() < 3) return RValue(false);
        const i32 op = arg_i32(a, 2, 0);
        if (a[0].is_string() || a[1].is_string()) {
            const i32 cmp = RValue::compare_for_ds(a[0], a[1], kGmlEpsilon);
            if (op == 0) return RValue(cmp == 0);
            if (op == 1) return RValue(cmp < 0);
            if (op == 2) return RValue(cmp > 0);
            return RValue(false);
        }
        return RValue(compare_dnd_op(a[0].as_real(), a[1].as_real(), op));
    });
    register_builtin("action_if_empty", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(true);
        const f64 tx = (r.action_relative_ ? self->x : 0.0) + arg_real(a, 0);
        const f64 ty = (r.action_relative_ ? self->y : 0.0) + arg_real(a, 1);
        return RValue(arg_i32(a, 2, 0) == 0 ? r.check_place_free(*self, tx, ty) : r.check_place_empty(*self, tx, ty));
    });
    register_builtin("action_if_collision", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        const f64 tx = (r.action_relative_ ? self->x : 0.0) + arg_real(a, 0);
        const f64 ty = (r.action_relative_ ? self->y : 0.0) + arg_real(a, 1);
        return RValue(arg_i32(a, 2, 0) == 0 ? !r.check_place_free(*self, tx, ty) : !r.check_place_empty(*self, tx, ty));
    });
    register_builtin("action_if_object", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        const f64 tx = (r.action_relative_ ? self->x : 0.0) + arg_real(a, 1);
        const f64 ty = (r.action_relative_ ? self->y : 0.0) + arg_real(a, 2);
        return RValue(r.check_place_meeting(*self, tx, ty, arg_i32(a, 0, -3)));
    });
    register_builtin("action_if_number", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 obj = arg_i32(a, 0, -4);
        return r.with_instance_selection(obj, [&](auto& group) {
            return RValue(compare_dnd_op(static_cast<f64>(std::ranges::distance(group)), arg_real(a, 1), arg_i32(a, 2, 0)));
        });
    });
    register_builtin("action_if_question", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(r.window().show_question(r.room_caption_.empty() ? "GameMaker" : r.room_caption_, arg_str(a, 0)));
    });
    register_builtin("action_if_dice", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 sides = arg_real(a, 0, 1.0);
        return RValue(sides > 0.0 && r.random_uniform(sides) < 1.0);
    });
    register_builtin("action_if_mouse", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 b = arg_i32(a, 0, 1);
        if (b == 0) return RValue(!r.window().input().mouse_down[1] && !r.window().input().mouse_down[2] && !r.window().input().mouse_down[3]);
        return RValue(b >= 1 && b <= 3 && r.window().input().mouse_down[static_cast<usize>(b)]);
    });
    register_builtin("action_if_aligned", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        if (!self) return RValue(false);
        const f64 hs = std::max(1.0, arg_real(a, 0, 1.0)), vs = std::max(1.0, arg_real(a, 1, 1.0));
        return RValue(std::fabs(std::fmod(self->x, hs)) < 1e-6 && std::fabs(std::fmod(self->y, vs)) < 1e-6);
    });

    // Score, Lives, Health & Draw actions
    register_builtin("action_set_score", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.score_ = r.action_relative_ ? (r.score_ + arg_real(a, 0)) : arg_real(a, 0);
        return RValue(0.0);
    });
    register_builtin("action_if_score", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(compare_dnd_op(r.score_, arg_real(a, 0), arg_i32(a, 1, 0)));
    });
    register_builtin("action_set_life", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 nv = r.action_relative_ ? (r.lives_ + arg_real(a, 0)) : arg_real(a, 0);
        (void)r.set_builtin_global(25u, 0, false, RValue(nv));
        return RValue(0.0);
    });
    register_builtin("action_if_life", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(compare_dnd_op(r.lives_, arg_real(a, 0), arg_i32(a, 1, 0)));
    });
    register_builtin("action_set_health", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const f64 nv = r.action_relative_ ? (r.health_ + arg_real(a, 0)) : arg_real(a, 0);
        (void)r.set_builtin_global(26u, 0, false, RValue(nv));
        return RValue(0.0);
    });
    register_builtin("action_if_health", [](Runner& r, i32, i32, std::span<const RValue> a) {
        return RValue(compare_dnd_op(r.health_, arg_real(a, 0), arg_i32(a, 1, 0)));
    });
    register_builtin("action_set_caption", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.show_score_explicit_ = true;
        r.show_score_ = arg_i32(a, 0) == 1; r.caption_score_ = std::string(arg_str(a, 1));
        r.show_lives_ = arg_i32(a, 2) == 1; r.caption_lives_ = std::string(arg_str(a, 3));
        r.show_health_ = arg_i32(a, 4) == 1; r.caption_health_ = std::string(arg_str(a, 5));
        return RValue(0.0);
    });
    register_builtin("action_color", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_color(static_cast<u32>(arg_i32(a, 0)));
        return RValue(0.0);
    });
    register_builtin("action_font", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.renderer().set_font(arg_i32(a, 0, -1));
        r.renderer().set_halign(arg_i32(a, 1, 0));
        return RValue(0.0);
    });
    register_builtin("action_draw_sprite", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        f64 sub = arg_real(a, 3, -1);
        if (sub < 0.0) sub = self ? self->image_index : 0.0;
        r.renderer().draw_sprite_ext(r.game(), arg_i32(a, 0, -1), static_cast<i32>(std::floor(sub)),
                                     static_cast<f32>(bx + arg_real(a, 1)), static_cast<f32>(by + arg_real(a, 2)),
                                     1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("action_draw_background", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        r.renderer().draw_background_ext(r.game(), arg_i32(a, 0, -1),
                                         static_cast<f32>(bx + arg_real(a, 1)), static_cast<f32>(by + arg_real(a, 2)),
                                         1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        return RValue(0.0);
    });
    register_builtin("action_draw_text", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        const u32 c = r.renderer().color();
        r.renderer().draw_text_ext_transformed_color(
            r.game(), static_cast<f32>(bx + arg_real(a, 1)), static_cast<f32>(by + arg_real(a, 2)),
            a.empty() ? std::string{} : dnd_val_to_str(a[0]),
            -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, c, c, c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("action_draw_variable", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        const u32 c = r.renderer().color();
        r.renderer().draw_text_ext_transformed_color(
            r.game(), static_cast<f32>(bx + arg_real(a, 1)), static_cast<f32>(by + arg_real(a, 2)),
            a.empty() ? std::string{} : dnd_val_to_str(a[0]),
            -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, c, c, c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("action_draw_score", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        const u32 c = r.renderer().color();
        const std::string text = std::string(arg_str(a, 2)) + dnd_val_to_str(RValue(r.score_));
        r.renderer().draw_text_ext_transformed_color(
            r.game(), static_cast<f32>(bx + arg_real(a, 0)), static_cast<f32>(by + arg_real(a, 1)),
            text, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, c, c, c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("action_draw_life", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        const u32 c = r.renderer().color();
        const std::string text = std::string(arg_str(a, 2)) + dnd_val_to_str(RValue(r.lives_));
        r.renderer().draw_text_ext_transformed_color(
            r.game(), static_cast<f32>(bx + arg_real(a, 0)), static_cast<f32>(by + arg_real(a, 1)),
            text, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, c, c, c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("action_draw_rectangle", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        const u32 c = r.renderer().color();
        r.renderer().draw_rectangle_color(
            static_cast<f32>(bx + arg_real(a, 0)), static_cast<f32>(by + arg_real(a, 1)),
            static_cast<f32>(bx + arg_real(a, 2)), static_cast<f32>(by + arg_real(a, 3)),
            c, c, c, c, arg_bool(a, 4), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("action_draw_ellipse", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        const u32 c = r.renderer().color();
        r.renderer().draw_ellipse_color(
            static_cast<f32>(bx + arg_real(a, 0)), static_cast<f32>(by + arg_real(a, 1)),
            static_cast<f32>(bx + arg_real(a, 2)), static_cast<f32>(by + arg_real(a, 3)),
            c, c, arg_bool(a, 4), r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("action_draw_line", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        const u32 c = r.renderer().color();
        r.renderer().draw_line_color(
            static_cast<f32>(bx + arg_real(a, 0)), static_cast<f32>(by + arg_real(a, 1)),
            static_cast<f32>(bx + arg_real(a, 2)), static_cast<f32>(by + arg_real(a, 3)),
            c, c, r.renderer().alpha());
        return RValue(0.0);
    });
    register_builtin("action_fullscreen", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const i32 mode = arg_i32(a, 0, 0);
        if (mode == 0) r.window().set_fullscreen(!r.window().is_fullscreen());
        else if (mode == 1) r.window().set_fullscreen(false);
        else r.window().set_fullscreen(true);
        return RValue(0.0);
    });
    register_builtin("action_snapshot", [](Runner& r, i32, i32, std::span<const RValue> a) {
        const auto dst = r.vfs().resolve_for_write(arg_str(a, 0));
        (void)r.renderer().screen_save(Vfs::path_to_utf8(dst));
        return RValue(0.0);
    });
    register_builtin("action_effect", [](Runner& r, i32 self_id, i32, std::span<const RValue> a) {
        const auto* self = r.find_instance(self_id);
        const f64 bx = (r.action_relative_ && self) ? self->x : 0.0;
        const f64 by = (r.action_relative_ && self) ? self->y : 0.0;
        r.particles().effect_create(arg_i32(a, 5, 1) != 0, arg_i32(a, 0), bx + arg_real(a, 1), by + arg_real(a, 2), arg_i32(a, 3, 1), static_cast<u32>(arg_i32(a, 4)) & 0xFFFFFFu);
        return RValue(0.0);
    });
    register_builtin("action_set_cursor", [](Runner& r, i32, i32, std::span<const RValue> a) {
        r.cursor_sprite_ = arg_i32(a, 0, -1);
        r.window().set_cursor_visible(arg_bool(a, 1, true));
        return RValue(0.0);
    });
    register_builtin("action_webpage", [](Runner&, i32, i32, std::span<const RValue> a) {
        (void)PlatformWindow::open_url(arg_str(a, 0));
        return RValue(0.0);
    });
    register_builtin("action_highscore_clear", [](Runner& r, i32, i32, std::span<const RValue>) {
        for (auto& h : r.highscores_) h = HighscoreEntry{};
        return RValue(0.0);
    });
    register_builtin("action_highscore_show", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("action_show_info", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("action_save_game", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
    register_builtin("action_load_game", [](Runner&, i32, i32, std::span<const RValue>) { return RValue(0.0); });
}

} // namespace gm
