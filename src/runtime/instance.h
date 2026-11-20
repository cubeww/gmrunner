#pragma once

#include "base/types.h"
#include "gml/vm.h"
#include "loader/game_data.h"

#include <array>
#include <string>
#include <vector>

namespace gm {

struct Instance {
    i32 id = 0;
    i32 object_index = -1;

    f64 x = 0.0;
    f64 y = 0.0;
    f64 xprevious = 0.0;
    f64 yprevious = 0.0;
    f64 xstart = 0.0;
    f64 ystart = 0.0;

    f64 hspeed = 0.0;
    f64 vspeed = 0.0;
    f64 direction = 0.0;
    f64 speed = 0.0;
    f64 friction = 0.0;
    f64 gravity = 0.0;
    f64 gravity_direction = 270.0;

    std::array<i32, 12> alarm{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    bool solid = false;
    bool visible = true;
    bool persistent = false;
    f64 depth = 0.0;

    i32 sprite_index = -1;
    f64 image_index = 0.0;
    f64 image_single = -1.0;
    f64 image_xscale = 1.0;
    f64 image_yscale = 1.0;
    f64 image_angle = 0.0;
    f64 image_alpha = 1.0;
    u32 image_blend = 0xFFFFFFu;
    f64 image_speed = 1.0;
    i32 mask_index = -1;

    // Cached bounding box (updated lazily when bbox_dirty == true)
    mutable i32 bbox_left = 0;
    mutable i32 bbox_right = 0;
    mutable i32 bbox_top = 0;
    mutable i32 bbox_bottom = 0;
    mutable bool bbox_dirty = true;

    // Path state
    i32 path_index = -1;
    f64 path_position = 0.0;
    f64 path_positionprevious = 0.0;
    f64 path_speed = 0.0;
    f64 path_scale = 1.0;
    f64 path_orientation = 0.0;
    i32 path_endaction = 0;
    f64 path_xstart = 0.0;
    f64 path_ystart = 0.0;
    bool path_absolute = false;

    // Timeline state
    i32 timeline_index = -1;
    f64 timeline_position = 0.0;
    f64 timeline_speed = 1.0;
    bool timeline_running = true;
    bool timeline_loop = false;

    // Lifecycle & event guards
    u64 create_counter = 0;
    bool destroyed = false;
    bool deactivated = false;
    bool outside_room_prev = false;
    bool mouse_inside_prev = false;

    VariableTable vars;

    void mark_bbox_dirty() noexcept { bbox_dirty = true; }
    void sync_hv_from_polar() noexcept;
    void sync_polar_from_hv() noexcept;
};

struct ActiveTile {
    i32 id = 0;
    i32 background_index = -1;
    i32 xo = 0;
    i32 yo = 0;
    i32 w = 0;
    i32 h = 0;
    f64 x = 0.0;
    f64 y = 0.0;
    f64 depth = 0.0;
    f64 xscale = 1.0;
    f64 yscale = 1.0;
    u32 blend = 0xFFFFFFu;
    f64 alpha = 1.0;
    bool visible = true;
};

struct ActiveBackground {
    bool visible = false;
    bool foreground = false;
    i32 index = -1;
    f64 x = 0.0;
    f64 y = 0.0;
    bool htiled = true;
    bool vtiled = true;
    f64 xscale = 1.0;
    f64 yscale = 1.0;
    f64 hspeed = 0.0;
    f64 vspeed = 0.0;
    u32 blend = 0xFFFFFFu;
    f64 alpha = 1.0;
    bool stretch = false;
};

struct ActiveView {
    bool visible = false;
    f64 xview = 0.0;
    f64 yview = 0.0;
    f64 wview = 640.0;
    f64 hview = 480.0;
    i32 xport = 0;
    i32 yport = 0;
    i32 wport = 640;
    i32 hport = 480;
    f64 angle = 0.0;
    i32 hborder = 32;
    i32 vborder = 32;
    i32 hspeed = -1;
    i32 vspeed = -1;
    i32 follow_object = -1;
};

struct SavedRoomState {
    std::vector<Instance> instances;
    std::vector<ActiveTile> tiles;
    std::array<ActiveBackground, 8> backgrounds{};
    std::array<ActiveView, 8> views{};
    bool view_enabled = false;
    u32 background_color = 0xC0C0C0u;
    bool background_showcolor = true;
    i32 width = 640;
    i32 height = 480;
    i32 speed = 30;
    std::string caption;
};

} // namespace gm
