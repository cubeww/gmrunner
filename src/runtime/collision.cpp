#include "runtime/collision.h"

#include "base/math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace gm {

namespace {

inline bool is_zero_scale(const Instance& inst) noexcept {
    return (inst.image_xscale * inst.image_yscale) == 0.0;
}

inline bool is_untransformed(const Instance& inst) noexcept {
    return inst.image_xscale == 1.0 && inst.image_yscale == 1.0 && inst.image_angle == 0.0;
}

inline bool mask_pixel(const CollisionMask& mask, i32 lx, i32 ly) noexcept {
    if (lx < 0 || ly < 0 || lx >= mask.width || ly >= mask.height) {
        return false;
    }
    return mask.pixels[static_cast<usize>(ly) * static_cast<usize>(mask.width) + static_cast<usize>(lx)] != 0u;
}

// Inverse-transforms world point (wx, wy) into local sprite mask coordinates (lx, ly).
// Matches gm81.exe.c TMask.Collision* inverse transform: floor((dx*cos + dy*sin)/scale + origin).
inline bool sample_instance_mask_at(
    const Instance& inst,
    f64 ix, f64 iy,
    const SpriteData& sp,
    const CollisionMask& mask,
    f64 wx, f64 wy,
    f64 cos_a, f64 sin_a,
    f64 inv_sx, f64 inv_sy) noexcept {
    const f64 dx = wx - ix;
    const f64 dy = wy - iy;
    const f64 rx = (dx * cos_a - dy * sin_a) * inv_sx + static_cast<f64>(sp.origin_x);
    const f64 ry = (dx * sin_a + dy * cos_a) * inv_sy + static_cast<f64>(sp.origin_y);
    const i32 lx = static_cast<i32>(std::floor(rx));
    const i32 ly = static_cast<i32>(std::floor(ry));
    return mask_pixel(mask, lx, ly);
}

void compute_bbox_at(
    const Instance& inst, f64 ix, f64 iy,
    const GameData& game,
    i32& out_l, i32& out_t, i32& out_r, i32& out_b) noexcept {
    const SpriteData* sp = instance_mask_sprite(inst, game);
    if (!sp) {
        const i32 px = static_cast<i32>(delphi_round(ix));
        const i32 py = static_cast<i32>(delphi_round(iy));
        out_l = out_r = px;
        out_t = out_b = py;
        return;
    }

    // GM makes the far edges inclusive after scaling, before mirroring or rotating the box.
    const f64 l0 = (static_cast<f64>(sp->bbox_left) - sp->origin_x) * inst.image_xscale;
    const f64 r0 = (static_cast<f64>(sp->bbox_right) + 1.0 - sp->origin_x) * inst.image_xscale - 1.0;
    const f64 t0 = (static_cast<f64>(sp->bbox_top) - sp->origin_y) * inst.image_yscale;
    const f64 b0 = (static_cast<f64>(sp->bbox_bottom) + 1.0 - sp->origin_y) * inst.image_yscale - 1.0;

    f64 min_x, max_x, min_y, max_y;
    if (inst.image_angle == 0.0) {
        min_x = ix + std::min(l0, r0);
        max_x = ix + std::max(l0, r0);
        min_y = iy + std::min(t0, b0);
        max_y = iy + std::max(t0, b0);
    } else {
        const f64 rad = deg_to_rad(inst.image_angle);
        const f64 c = std::cos(rad);
        const f64 s = std::sin(rad);
        const f64 cx[4] = {
            ix + l0 * c + t0 * s,
            ix + r0 * c + t0 * s,
            ix + r0 * c + b0 * s,
            ix + l0 * c + b0 * s,
        };
        const f64 cy[4] = {
            iy - l0 * s + t0 * c,
            iy - r0 * s + t0 * c,
            iy - r0 * s + b0 * c,
            iy - l0 * s + b0 * c,
        };
        min_x = std::min({cx[0], cx[1], cx[2], cx[3]});
        max_x = std::max({cx[0], cx[1], cx[2], cx[3]});
        min_y = std::min({cy[0], cy[1], cy[2], cy[3]});
        max_y = std::max({cy[0], cy[1], cy[2], cy[3]});
    }

    out_l = static_cast<i32>(delphi_round(min_x));
    out_t = static_cast<i32>(delphi_round(min_y));
    out_r = static_cast<i32>(delphi_round(max_x));
    out_b = static_cast<i32>(delphi_round(max_y));
}

bool bbox_overlaps(const Instance& inst, f64 left, f64 top, f64 right, f64 bottom) noexcept {
    // bbox_right/bottom are inclusive pixel indices; the last pixel occupies a whole unit.
    return inst.bbox_left <= inst.bbox_right && inst.bbox_top <= inst.bbox_bottom &&
           left < static_cast<f64>(inst.bbox_right) + 1.0 && right >= inst.bbox_left &&
           top < static_cast<f64>(inst.bbox_bottom) + 1.0 && bottom >= inst.bbox_top;
}

} // namespace

void Instance::sync_hv_from_polar() noexcept {
    const f64 rad = deg_to_rad(direction);
    hspeed = speed * std::cos(rad);
    vspeed = -speed * std::sin(rad);
    if (std::fabs(hspeed) < 1e-12) hspeed = 0.0;
    if (std::fabs(vspeed) < 1e-12) vspeed = 0.0;
}

void Instance::sync_polar_from_hv() noexcept {
    speed = std::hypot(hspeed, vspeed);
    if (speed > 0.0) {
        f64 deg = rad_to_deg(std::atan2(-vspeed, hspeed));
        if (deg < 0.0) deg += 360.0;
        direction = deg;
    }
}

const SpriteData* instance_mask_sprite(const Instance& inst, const GameData& game) noexcept {
    const i32 idx = (inst.mask_index >= 0) ? inst.mask_index : inst.sprite_index;
    if (idx < 0 || static_cast<usize>(idx) >= game.sprites.size()) return nullptr;
    const auto& sp = game.sprites[static_cast<usize>(idx)];
    return (sp.exists && !sp.masks.empty()) ? &sp : nullptr;
}

const CollisionMask* instance_mask(const Instance& inst, const GameData& game) noexcept {
    const SpriteData* sp = instance_mask_sprite(inst, game);
    if (!sp || sp->masks.empty()) return nullptr;
    if (!sp->separate_masks || sp->masks.size() == 1) {
        return &sp->masks[0];
    }
    i32 sub = static_cast<i32>(std::floor(inst.image_index)) % static_cast<i32>(sp->masks.size());
    if (sub < 0) sub += static_cast<i32>(sp->masks.size());
    return &sp->masks[static_cast<usize>(sub)];
}

void ensure_instance_bbox(const Instance& inst, const GameData& game) noexcept {
    if (!inst.bbox_dirty) return;
    compute_bbox_at(inst, inst.x, inst.y, game, inst.bbox_left, inst.bbox_top, inst.bbox_right, inst.bbox_bottom);
    inst.bbox_dirty = false;
}

bool check_instance_collision(
    const Instance& a, f64 ax, f64 ay,
    const Instance& b,
    const GameData& game,
    VersionMode ver_mode) noexcept {
    if (a.destroyed || b.destroyed || a.deactivated || b.deactivated) {
        return false;
    }
    if (ver_mode == VersionMode::GM82 && (is_zero_scale(a) || is_zero_scale(b))) {
        return false;
    }

    const SpriteData* spa = instance_mask_sprite(a, game);
    const SpriteData* spb = instance_mask_sprite(b, game);
    const CollisionMask* ma = instance_mask(a, game);
    const CollisionMask* mb = instance_mask(b, game);
    if (!spa || !spb || !ma || !mb) {
        return false;
    }

    i32 al, at, ar, ab;
    compute_bbox_at(a, ax, ay, game, al, at, ar, ab);
    ensure_instance_bbox(b, game);
    const i32 bl = b.bbox_left;
    const i32 bt = b.bbox_top;
    const i32 br = b.bbox_right;
    const i32 bb = b.bbox_bottom;

    if (al > br || ar < bl || at > bb || ab < bt) {
        return false;
    }

    const i32 ol = std::max(al, bl);
    const i32 ot = std::max(at, bt);
    const i32 or_ = std::min(ar, br);
    const i32 ob = std::min(ab, bb);

    // Fast path: both untransformed (scale == 1, angle == 0) — exact integer grid overlap.
    if (is_untransformed(a) && is_untransformed(b)) {
        const i32 a_ox = static_cast<i32>(delphi_round(ax)) - spa->origin_x;
        const i32 a_oy = static_cast<i32>(delphi_round(ay)) - spa->origin_y;
        const i32 b_ox = static_cast<i32>(delphi_round(b.x)) - spb->origin_x;
        const i32 b_oy = static_cast<i32>(delphi_round(b.y)) - spb->origin_y;

        for (i32 wy = ot; wy <= ob; ++wy) {
            const i32 lay = wy - a_oy;
            const i32 lby = wy - b_oy;
            if (lay < 0 || lay >= ma->height || lby < 0 || lby >= mb->height) continue;
            const u8* row_a = ma->pixels.data() + static_cast<usize>(lay) * static_cast<usize>(ma->width);
            const u8* row_b = mb->pixels.data() + static_cast<usize>(lby) * static_cast<usize>(mb->width);
            for (i32 wx = ol; wx <= or_; ++wx) {
                const i32 lax = wx - a_ox;
                const i32 lbx = wx - b_ox;
                if (lax >= 0 && lax < ma->width && lbx >= 0 && lbx < mb->width) {
                    if (row_a[lax] != 0u && row_b[lbx] != 0u) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // General scaled/rotated narrowphase pixel overlap.
    if (is_zero_scale(a) || is_zero_scale(b)) {
        return false;
    }
    const f64 rad_a = deg_to_rad(a.image_angle);
    const f64 cos_a = std::cos(rad_a);
    const f64 sin_a = std::sin(rad_a);
    const f64 inv_sxa = 1.0 / a.image_xscale;
    const f64 inv_sya = 1.0 / a.image_yscale;

    const f64 rad_b = deg_to_rad(b.image_angle);
    const f64 cos_b = std::cos(rad_b);
    const f64 sin_b = std::sin(rad_b);
    const f64 inv_sxb = 1.0 / b.image_xscale;
    const f64 inv_syb = 1.0 / b.image_yscale;

    // TMask.CollisionMask samples integer world pixels relative to rounded instance positions.
    const f64 a_x = delphi_round_f64(ax), a_y = delphi_round_f64(ay);
    const f64 b_x = delphi_round_f64(b.x), b_y = delphi_round_f64(b.y);
    for (i32 wy = ot; wy <= ob; ++wy) {
        for (i32 wx = ol; wx <= or_; ++wx) {
            if (sample_instance_mask_at(a, a_x, a_y, *spa, *ma, wx, wy, cos_a, sin_a, inv_sxa, inv_sya) &&
                sample_instance_mask_at(b, b_x, b_y, *spb, *mb, wx, wy, cos_b, sin_b, inv_sxb, inv_syb)) {
                return true;
            }
        }
    }
    return false;
}

bool check_point_collision(
    const Instance& inst, f64 px, f64 py, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept {
    if (inst.destroyed || inst.deactivated) return false;
    if (ver_mode == VersionMode::GM82 && is_zero_scale(inst)) return false;
    const SpriteData* sp = instance_mask_sprite(inst, game);
    const CollisionMask* mk = instance_mask(inst, game);
    if (!sp || !mk) return false;

    ensure_instance_bbox(inst, game);
    if (!bbox_overlaps(inst, px, py, px, py)) {
        return false;
    }
    if (!prec) {
        return true;
    }
    if (is_zero_scale(inst)) return false;
    const f64 rad = deg_to_rad(inst.image_angle);
    return sample_instance_mask_at(
        inst, delphi_round_f64(inst.x), delphi_round_f64(inst.y), *sp, *mk,
        delphi_round_f64(px), delphi_round_f64(py),
        std::cos(rad), std::sin(rad), 1.0 / inst.image_xscale, 1.0 / inst.image_yscale);
}

bool check_rect_collision(
    const Instance& inst, f64 x1, f64 y1, f64 x2, f64 y2, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept {
    if (inst.destroyed || inst.deactivated) return false;
    if (ver_mode == VersionMode::GM82 && is_zero_scale(inst)) return false;
    const SpriteData* sp = instance_mask_sprite(inst, game);
    const CollisionMask* mk = instance_mask(inst, game);
    if (!sp || !mk) return false;

    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);

    ensure_instance_bbox(inst, game);
    if (!bbox_overlaps(inst, x1, y1, x2, y2)) {
        return false;
    }
    if (!prec) {
        return true;
    }
    if (is_zero_scale(inst)) return false;

    const i32 ol = std::max(inst.bbox_left, static_cast<i32>(std::floor(x1)));
    const i32 ot = std::max(inst.bbox_top, static_cast<i32>(std::floor(y1)));
    const i32 or_ = std::min(inst.bbox_right, static_cast<i32>(std::ceil(x2)));
    const i32 ob = std::min(inst.bbox_bottom, static_cast<i32>(std::ceil(y2)));

    const f64 rad = deg_to_rad(inst.image_angle);
    const f64 c = std::cos(rad);
    const f64 s = std::sin(rad);
    const f64 isx = 1.0 / inst.image_xscale;
    const f64 isy = 1.0 / inst.image_yscale;

    for (i32 wy = ot; wy <= ob; ++wy) {
        for (i32 wx = ol; wx <= or_; ++wx) {
            if (sample_instance_mask_at(inst, inst.x, inst.y, *sp, *mk,
                                        static_cast<f64>(wx) + 0.5, static_cast<f64>(wy) + 0.5, c, s, isx, isy)) {
                return true;
            }
        }
    }
    return false;
}

bool check_circle_collision(
    const Instance& inst, f64 cx, f64 cy, f64 radius, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept {
    if (radius < 0.0) return false;
    return check_ellipse_collision(inst, cx - radius, cy - radius, cx + radius, cy + radius, prec, game, ver_mode);
}

bool check_ellipse_collision(
    const Instance& inst, f64 x1, f64 y1, f64 x2, f64 y2, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept {
    if (inst.destroyed || inst.deactivated) return false;
    if (ver_mode == VersionMode::GM82 && is_zero_scale(inst)) return false;
    const SpriteData* sp = instance_mask_sprite(inst, game);
    const CollisionMask* mk = instance_mask(inst, game);
    if (!sp || !mk) return false;

    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    ensure_instance_bbox(inst, game);
    if (!bbox_overlaps(inst, x1, y1, x2, y2)) {
        return false;
    }

    const f64 cx = (x1 + x2) * 0.5;
    const f64 cy = (y1 + y2) * 0.5;
    const f64 rx = (x2 - x1) * 0.5;
    const f64 ry = (y2 - y1) * 0.5;
    const f64 left = delphi_round_f64(x1), right = delphi_round_f64(x2);
    const f64 top = delphi_round_f64(y1), bottom = delphi_round_f64(y2);
    if (left == right || top == bottom) {
        return check_rect_collision(inst, x1, y1, x2, y2, prec, game, ver_mode);
    }

    if (!prec) {
        if ((cx >= inst.bbox_left && cx <= inst.bbox_right) ||
            (cy >= inst.bbox_top && cy <= inst.bbox_bottom)) return true;
        // GM8 checks bbox corners against the ellipse with integer-rounded endpoints.
        const f64 ecx = (left + right) * 0.5, ecy = (top + bottom) * 0.5;
        const f64 closest_x = std::clamp(ecx, static_cast<f64>(inst.bbox_left), static_cast<f64>(inst.bbox_right));
        const f64 closest_y = std::clamp(ecy, static_cast<f64>(inst.bbox_top), static_cast<f64>(inst.bbox_bottom));
        const f64 nx = (closest_x - ecx) / ((right - left) * 0.5);
        const f64 ny = (closest_y - ecy) / ((bottom - top) * 0.5);
        return (nx * nx + ny * ny) <= 1.0;
    }

    if (is_zero_scale(inst)) return false;
    const i32 ol = std::max(inst.bbox_left, static_cast<i32>(std::floor(x1)));
    const i32 ot = std::max(inst.bbox_top, static_cast<i32>(std::floor(y1)));
    const i32 or_ = std::min(inst.bbox_right, static_cast<i32>(std::ceil(x2)));
    const i32 ob = std::min(inst.bbox_bottom, static_cast<i32>(std::ceil(y2)));

    const f64 rad = deg_to_rad(inst.image_angle);
    const f64 c = std::cos(rad);
    const f64 s = std::sin(rad);
    const f64 isx = 1.0 / inst.image_xscale;
    const f64 isy = 1.0 / inst.image_yscale;

    for (i32 wy = ot; wy <= ob; ++wy) {
        const f64 ny = (static_cast<f64>(wy) + 0.5 - cy) / ry;
        const f64 ny2 = ny * ny;
        if (ny2 > 1.0) continue;
        for (i32 wx = ol; wx <= or_; ++wx) {
            const f64 nx = (static_cast<f64>(wx) + 0.5 - cx) / rx;
            if (nx * nx + ny2 <= 1.0) {
                if (sample_instance_mask_at(inst, inst.x, inst.y, *sp, *mk,
                                            static_cast<f64>(wx) + 0.5, static_cast<f64>(wy) + 0.5, c, s, isx, isy)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool check_line_collision(
    const Instance& inst, f64 x1, f64 y1, f64 x2, f64 y2, bool prec,
    const GameData& game, VersionMode ver_mode) noexcept {
    if (inst.destroyed || inst.deactivated) return false;
    if (ver_mode == VersionMode::GM82 && is_zero_scale(inst)) return false;
    const SpriteData* sp = instance_mask_sprite(inst, game);
    const CollisionMask* mk = instance_mask(inst, game);
    if (!sp || !mk) return false;

    ensure_instance_bbox(inst, game);
    const f64 lx = std::min(x1, x2);
    const f64 rx = std::max(x1, x2);
    const f64 ty = std::min(y1, y2);
    const f64 by = std::max(y1, y2);
    if (!bbox_overlaps(inst, lx, ty, rx, by)) {
        return false;
    }

    // Liang-Barsky clip against instance AABB.
    f64 t0 = 0.0;
    f64 t1 = 1.0;
    const f64 dx = x2 - x1;
    const f64 dy = y2 - y1;
    auto clip = [&](f64 p, f64 q) noexcept -> bool {
        if (p == 0.0) return q >= 0.0;
        const f64 r = q / p;
        if (p < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };

    const f64 right = static_cast<f64>(inst.bbox_right) + 1.0;
    const f64 bottom = static_cast<f64>(inst.bbox_bottom) + 1.0;
    if (!clip(-dx, x1 - inst.bbox_left) || !clip(dx, right - x1)) {
        return false;
    }

    if (!prec) {
        const f64 ya = y1 + t0 * dy, yb = y1 + t1 * dy;
        return std::max(ya, yb) >= inst.bbox_top && std::min(ya, yb) < bottom;
    }

    if (is_zero_scale(inst)) return false;
    if (!clip(-dy, y1 - inst.bbox_top) || !clip(dy, bottom - y1)) return false;
    const f64 sx = x1 + t0 * dx;
    const f64 sy = y1 + t0 * dy;
    const f64 ex = x1 + t1 * dx;
    const f64 ey = y1 + t1 * dy;
    const i32 steps = std::max(1, static_cast<i32>(std::ceil(std::max(std::fabs(ex - sx), std::fabs(ey - sy)))));

    const f64 rad = deg_to_rad(inst.image_angle);
    const f64 c = std::cos(rad);
    const f64 s = std::sin(rad);
    const f64 isx = 1.0 / inst.image_xscale;
    const f64 isy = 1.0 / inst.image_yscale;

    for (i32 i = 0; i <= steps; ++i) {
        const f64 f = static_cast<f64>(i) / static_cast<f64>(steps);
        const f64 px = sx + (ex - sx) * f;
        const f64 py = sy + (ey - sy) * f;
        if (sample_instance_mask_at(inst, inst.x, inst.y, *sp, *mk, px, py, c, s, isx, isy)) {
            return true;
        }
    }
    return false;
}

// ---- Motion Planning Grid ----

i32 MpGridManager::create(f64 left, f64 top, i32 hcells, i32 vcells, f64 cell_w, f64 cell_h) {
    const i32 id = next_id_++;
    MpGrid g{};
    g.left = left;
    g.top = top;
    g.hcells = std::max(1, hcells);
    g.vcells = std::max(1, vcells);
    g.cell_w = std::max(1e-4, cell_w);
    g.cell_h = std::max(1e-4, cell_h);
    g.cells.assign(static_cast<usize>(g.hcells) * static_cast<usize>(g.vcells), 0u);
    grids_.emplace(id, std::move(g));
    return id;
}

void MpGridManager::destroy(i32 id) { grids_.erase(id); }
void MpGridManager::destroy_all() { grids_.clear(); }

MpGrid* MpGridManager::get(i32 id) {
    const auto it = grids_.find(id);
    return it != grids_.end() ? &it->second : nullptr;
}

void MpGridManager::clear_all(i32 id) {
    if (auto* g = get(id)) std::fill(g->cells.begin(), g->cells.end(), 0u);
}

void MpGridManager::clear_cell(i32 id, i32 h, i32 v) {
    if (auto* g = get(id); g && h >= 0 && v >= 0 && h < g->hcells && v < g->vcells) {
        g->cells[static_cast<usize>(v) * static_cast<usize>(g->hcells) + static_cast<usize>(h)] = 0u;
    }
}

void MpGridManager::add_cell(i32 id, i32 h, i32 v) {
    if (auto* g = get(id); g && h >= 0 && v >= 0 && h < g->hcells && v < g->vcells) {
        g->cells[static_cast<usize>(v) * static_cast<usize>(g->hcells) + static_cast<usize>(h)] = 1u;
    }
}

void MpGridManager::clear_rectangle(i32 id, f64 left, f64 top, f64 right, f64 bottom) {
    auto* g = get(id);
    if (!g) return;
    const i32 x0 = std::max(0, static_cast<i32>(std::floor((left - g->left) / g->cell_w)));
    const i32 y0 = std::max(0, static_cast<i32>(std::floor((top - g->top) / g->cell_h)));
    const i32 x1 = std::min(g->hcells - 1, static_cast<i32>(std::floor((right - g->left) / g->cell_w)));
    const i32 y1 = std::min(g->vcells - 1, static_cast<i32>(std::floor((bottom - g->top) / g->cell_h)));
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            g->cells[static_cast<usize>(y) * static_cast<usize>(g->hcells) + static_cast<usize>(x)] = 0u;
        }
    }
}

void MpGridManager::add_rectangle(i32 id, f64 left, f64 top, f64 right, f64 bottom) {
    auto* g = get(id);
    if (!g) return;
    const i32 x0 = std::max(0, static_cast<i32>(std::floor((left - g->left) / g->cell_w)));
    const i32 y0 = std::max(0, static_cast<i32>(std::floor((top - g->top) / g->cell_h)));
    const i32 x1 = std::min(g->hcells - 1, static_cast<i32>(std::floor((right - g->left) / g->cell_w)));
    const i32 y1 = std::min(g->vcells - 1, static_cast<i32>(std::floor((bottom - g->top) / g->cell_h)));
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            g->cells[static_cast<usize>(y) * static_cast<usize>(g->hcells) + static_cast<usize>(x)] = 1u;
        }
    }
}

bool MpGridManager::find_path(
    i32 id, PathData& out_path,
    f64 xstart, f64 ystart, f64 xgoal, f64 ygoal,
    bool allow_diag) {
    auto* g = get(id);
    if (!g) return false;

    const i32 sx = static_cast<i32>(std::floor((xstart - g->left) / g->cell_w));
    const i32 sy = static_cast<i32>(std::floor((ystart - g->top) / g->cell_h));
    const i32 gx = static_cast<i32>(std::floor((xgoal - g->left) / g->cell_w));
    const i32 gy = static_cast<i32>(std::floor((ygoal - g->top) / g->cell_h));

    auto in_bounds = [&](i32 x, i32 y) { return x >= 0 && y >= 0 && x < g->hcells && y < g->vcells; };
    auto cell_at = [&](i32 x, i32 y) {
        return g->cells[static_cast<usize>(y) * static_cast<usize>(g->hcells) + static_cast<usize>(x)];
    };

    if (!in_bounds(sx, sy) || !in_bounds(gx, gy) || cell_at(gx, gy) != 0u) {
        return false;
    }

    const usize total = static_cast<usize>(g->hcells) * static_cast<usize>(g->vcells);
    std::vector<f64> dist(total, std::numeric_limits<f64>::infinity());
    std::vector<i32> parent(total, -1);

    using Node = std::pair<f64, i32>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;

    const i32 start_idx = sy * g->hcells + sx;
    const i32 goal_idx = gy * g->hcells + gx;
    dist[static_cast<usize>(start_idx)] = 0.0;
    pq.emplace(0.0, start_idx);

    static constexpr i32 kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr i32 kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    static constexpr f64 kCost[8] = {1.0, 1.0, 1.0, 1.0, 1.41421356237, 1.41421356237, 1.41421356237, 1.41421356237};
    const int dirs = allow_diag ? 8 : 4;

    while (!pq.empty()) {
        const auto [d, cur] = pq.top();
        pq.pop();
        if (d > dist[static_cast<usize>(cur)]) continue;
        if (cur == goal_idx) break;

        const i32 cx = cur % g->hcells;
        const i32 cy = cur / g->hcells;

        for (int dir = 0; dir < dirs; ++dir) {
            const i32 nx = cx + kDx[dir];
            const i32 ny = cy + kDy[dir];
            if (!in_bounds(nx, ny) || cell_at(nx, ny) != 0u) continue;
            // Diagonal corner-cutting check matching CGrid.FindPath
            if (dir >= 4 && (cell_at(cx + kDx[dir], cy) != 0u || cell_at(cx, cy + kDy[dir]) != 0u)) {
                continue;
            }
            const i32 nidx = ny * g->hcells + nx;
            const f64 nd = d + kCost[dir];
            if (nd < dist[static_cast<usize>(nidx)]) {
                dist[static_cast<usize>(nidx)] = nd;
                parent[static_cast<usize>(nidx)] = cur;
                pq.emplace(nd, nidx);
            }
        }
    }

    if (!std::isfinite(dist[static_cast<usize>(goal_idx)])) {
        return false;
    }

    std::vector<i32> chain;
    for (i32 cur = goal_idx; cur != -1; cur = parent[static_cast<usize>(cur)]) {
        chain.push_back(cur);
    }
    std::reverse(chain.begin(), chain.end());

    auto& geometry = out_path.edit_geometry();
    geometry.points.clear();
    geometry.closed = false;
    geometry.points.push_back(PathPoint{xstart, ystart, 100.0});
    for (usize i = 1; i + 1 < chain.size(); ++i) {
        const i32 cx = chain[i] % g->hcells;
        const i32 cy = chain[i] / g->hcells;
        geometry.points.push_back(PathPoint{
            g->left + (static_cast<f64>(cx) + 0.5) * g->cell_w,
            g->top + (static_cast<f64>(cy) + 0.5) * g->cell_h,
            100.0,
        });
    }
    geometry.points.push_back(PathPoint{xgoal, ygoal, 100.0});
    return true;
}

} // namespace gm
