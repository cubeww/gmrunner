#pragma once

#include "base/types.h"
#include "loader/game_data.h"
#include "runtime/instance.h"

#include <unordered_map>
#include <vector>

namespace gm {

// Resolves the sprite & collision mask currently active on an instance (respecting mask_index & subimage).
[[nodiscard]] const SpriteData* instance_mask_sprite(const Instance& inst, const GameData& game) noexcept;
[[nodiscard]] const CollisionMask* instance_mask(const Instance& inst, const GameData& game) noexcept;

// Updates inst.bbox_left/right/top/bottom if inst.bbox_dirty is set (matching GM8 Delphi rounding).
void ensure_instance_bbox(const Instance& inst, const GameData& game) noexcept;

// Exact GM8 instance-vs-instance collision (AABB broadphase + TMask fast/scaled/rotated pixel checks).
// In VersionMode::GM82, zero-scale instances (image_xscale * image_yscale == 0) never collide.
[[nodiscard]] bool check_instance_collision(
    const Instance& a, f64 ax, f64 ay,
    const Instance& b,
    const GameData& game,
    VersionMode ver_mode) noexcept;

// Shape collision queries (GM8 collision_point / rectangle / circle / ellipse / line).
[[nodiscard]] bool check_point_collision(
    const Instance& inst, f64 px, f64 py, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept;

[[nodiscard]] bool check_rect_collision(
    const Instance& inst, f64 x1, f64 y1, f64 x2, f64 y2, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept;

[[nodiscard]] bool check_circle_collision(
    const Instance& inst, f64 cx, f64 cy, f64 radius, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept;

[[nodiscard]] bool check_ellipse_collision(
    const Instance& inst, f64 x1, f64 y1, f64 x2, f64 y2, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept;

[[nodiscard]] bool check_line_collision(
    const Instance& inst, f64 x1, f64 y1, f64 x2, f64 y2, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept;

// Path sampling helper (straight & smooth quadratic Bezier evaluation at pos in [0, 1]).
void evaluate_path_point(const PathData& path, f64 pos, f64& out_x, f64& out_y) noexcept;
[[nodiscard]] f64 compute_path_length(const PathData& path) noexcept;

// Motion planning grid (mp_grid_*).
struct MpGrid {
    f64 left = 0.0;
    f64 top = 0.0;
    i32 hcells = 0;
    i32 vcells = 0;
    f64 cell_w = 1.0;
    f64 cell_h = 1.0;
    std::vector<u8> cells; // 0 = free, 1 = forbidden
};

class MpGridManager {
public:
    i32 create(f64 left, f64 top, i32 hcells, i32 vcells, f64 cell_w, f64 cell_h);
    void destroy(i32 id);
    void destroy_all();
    [[nodiscard]] MpGrid* get(i32 id);

    void clear_all(i32 id);
    void clear_cell(i32 id, i32 h, i32 v);
    void clear_rectangle(i32 id, f64 left, f64 top, f64 right, f64 bottom);
    void add_cell(i32 id, i32 h, i32 v);
    void add_rectangle(i32 id, f64 left, f64 top, f64 right, f64 bottom);

    // 4-way or 8-way Dijkstra/BFS shortest path writing waypoints into out_path.
    [[nodiscard]] bool find_path(
        i32 id, PathData& out_path,
        f64 xstart, f64 ystart, f64 xgoal, f64 ygoal,
        bool allow_diag);

private:
    std::unordered_map<i32, MpGrid> grids_;
    i32 next_id_ = 0;
};

} // namespace gm
