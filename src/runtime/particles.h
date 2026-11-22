#pragma once

#include "base/math.h"
#include "base/types.h"
#include "graphics/renderer2d.h"
#include "loader/game_data.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace gm {

struct ParticleType {
    i32 shape = 0; // pt_shape_*
    i32 sprite_index = -1;
    bool sprite_anim = false;
    bool sprite_stretch = false;
    bool sprite_random = false;

    f64 size_min = 1.0, size_max = 1.0, size_incr = 0.0, size_wiggle = 0.0;
    f64 xscale = 1.0, yscale = 1.0;
    f64 ang_min = 0.0, ang_max = 0.0, ang_incr = 0.0, ang_wiggle = 0.0;
    bool ang_relative = false;

    u32 col1 = 0xFFFFFFu, col2 = 0xFFFFFFu, col3 = 0xFFFFFFu;
    i32 col_mode = 0; // 0=1col, 1=2col, 2=3col, 3=mix, 4=rgb, 5=hsv
    i32 rmin = 255, rmax = 255, gmin = 255, gmax = 255, bmin = 255, bmax = 255;
    i32 hmin = 0, hmax = 255, smin = 0, smax = 255, vmin = 255, vmax = 255;
    f64 alpha1 = 1.0, alpha2 = 1.0, alpha3 = 1.0;
    bool additive = false;

    f64 speed_min = 0.0, speed_max = 0.0, speed_incr = 0.0, speed_wiggle = 0.0;
    f64 dir_min = 0.0, dir_max = 0.0, dir_incr = 0.0, dir_wiggle = 0.0;
    f64 grav_amount = 0.0, grav_dir = 270.0;
    i32 life_min = 100, life_max = 100;
    i32 step_number = 0, step_type = -1;
    i32 death_number = 0, death_type = -1;
};

struct LiveParticle {
    i32 type_id = -1;
    f64 x = 0.0, y = 0.0;
    f64 speed = 0.0, dir = 0.0;
    f64 size = 1.0, angle = 0.0;
    u32 color = 0xFFFFFFu;
    f64 alpha = 1.0;
    i32 age = 0, life = 60;
    i32 phase = 0;
    f64 subimg = 0.0;
    bool alive = true;
};

struct ParticleEmitter {
    f64 xmin = 0.0, xmax = 0.0, ymin = 0.0, ymax = 0.0;
    i32 shape = 0; // ps_shape_rectangle..diamond
    i32 distr = 0; // ps_distr_linear..invgaussian
    i32 stream_type = -1;
    i32 stream_count = 0;
};

struct ParticleAttractor {
    f64 x = 0.0, y = 0.0;
    f64 force = 0.0, dist = 100.0;
    i32 kind = 0; // ps_force_constant..quadratic
    bool additive = false;
};

struct ParticleDestroyer {
    f64 xmin = 0.0, xmax = 0.0, ymin = 0.0, ymax = 0.0;
    i32 shape = 0;
};

struct ParticleDeflector {
    f64 xmin = 0.0, xmax = 0.0, ymin = 0.0, ymax = 0.0;
    i32 kind = 0; // 0=vertical, 1=horizontal
    f64 friction = 0.0;
};

struct ParticleChanger {
    f64 xmin = 0.0, xmax = 0.0, ymin = 0.0, ymax = 0.0;
    i32 shape = 0;
    i32 kind = 0; // 0=all, 1=shape, 2=motion
    i32 type1 = -1, type2 = -1;
};

struct ParticleSystem {
    f64 depth = 0.0;
    f64 x = 0.0, y = 0.0;
    bool auto_update = true;
    bool auto_draw = true;
    bool old_to_new = true;
    std::vector<LiveParticle> particles;
    std::unordered_map<i32, ParticleEmitter> emitters;
    std::unordered_map<i32, ParticleAttractor> attractors;
    std::unordered_map<i32, ParticleDestroyer> destroyers;
    std::unordered_map<i32, ParticleDeflector> deflectors;
    std::unordered_map<i32, ParticleChanger> changers;
    i32 next_emitter_id = 0;
    i32 next_attractor_id = 0;
    i32 next_destroyer_id = 0;
    i32 next_deflector_id = 0;
    i32 next_changer_id = 0;
};

class ParticleManager {
public:
    explicit ParticleManager(GmlRandom& rng) : rng_(rng) {}
    void clear_all();

    i32 system_create();
    void system_destroy(i32 ps);
    void system_destroy_all();
    [[nodiscard]] bool system_exists(i32 ps) const noexcept;
    void system_clear(i32 ps);
    void system_depth(i32 ps, f64 depth);
    void system_position(i32 ps, f64 x, f64 y);
    void system_automatic_update(i32 ps, bool auto_upd);
    void system_automatic_draw(i32 ps, bool auto_drw);
    void system_draw_order(i32 ps, bool oldtonew);
    [[nodiscard]] i32 particles_count(i32 ps) const;
    void particles_clear(i32 ps);
    void particles_create(i32 ps, f64 x, f64 y, i32 pt, i32 count, bool override_col = false, u32 col = 0xFFFFFFu);

    i32 type_create();
    void type_destroy(i32 pt);
    [[nodiscard]] bool type_exists(i32 pt) const noexcept;
    void type_clear(i32 pt);
    [[nodiscard]] ParticleType* get_type(i32 pt);

    i32 emitter_create(i32 ps);
    void emitter_destroy(i32 ps, i32 em);
    void emitter_destroy_all(i32 ps);
    [[nodiscard]] bool emitter_exists(i32 ps, i32 em) const noexcept;
    void emitter_clear(i32 ps, i32 em);
    void emitter_region(i32 ps, i32 em, f64 xmin, f64 xmax, f64 ymin, f64 ymax, i32 shape, i32 distr);
    void emitter_burst(i32 ps, i32 em, i32 pt, i32 number);
    void emitter_stream(i32 ps, i32 em, i32 pt, i32 number);

    i32 attractor_create(i32 ps);
    void attractor_destroy(i32 ps, i32 at);
    void attractor_destroy_all(i32 ps);
    [[nodiscard]] bool attractor_exists(i32 ps, i32 at) const noexcept;
    void attractor_clear(i32 ps, i32 at);
    void attractor_position(i32 ps, i32 at, f64 x, f64 y);
    void attractor_force(i32 ps, i32 at, f64 force, f64 dist, i32 kind, bool additive);

    i32 destroyer_create(i32 ps);
    void destroyer_destroy(i32 ps, i32 de);
    void destroyer_destroy_all(i32 ps);
    [[nodiscard]] bool destroyer_exists(i32 ps, i32 de) const noexcept;
    void destroyer_clear(i32 ps, i32 de);
    void destroyer_region(i32 ps, i32 de, f64 xmin, f64 xmax, f64 ymin, f64 ymax, i32 shape);

    i32 deflector_create(i32 ps);
    void deflector_destroy(i32 ps, i32 df);
    void deflector_destroy_all(i32 ps);
    [[nodiscard]] bool deflector_exists(i32 ps, i32 df) const noexcept;
    void deflector_clear(i32 ps, i32 df);
    void deflector_region(i32 ps, i32 df, f64 xmin, f64 xmax, f64 ymin, f64 ymax);
    void deflector_kind(i32 ps, i32 df, i32 kind);
    void deflector_friction(i32 ps, i32 df, f64 friction);

    i32 changer_create(i32 ps);
    void changer_destroy(i32 ps, i32 ch);
    void changer_destroy_all(i32 ps);
    [[nodiscard]] bool changer_exists(i32 ps, i32 ch) const noexcept;
    void changer_clear(i32 ps, i32 ch);
    void changer_region(i32 ps, i32 ch, f64 xmin, f64 xmax, f64 ymin, f64 ymax, i32 shape);
    void changer_kind(i32 ps, i32 ch, i32 kind);
    void changer_types(i32 ps, i32 ch, i32 pt1, i32 pt2);

    // Simple GM8 effect_create_below / effect_create_above / effect_clear
    void effect_create(bool above, i32 kind, f64 x, f64 y, i32 size, u32 color,
                       f64 speed_factor, i32 room_width, i32 room_height);
    void effect_clear();

    void update_all();
    void update_system(i32 ps);
    void draw_system(i32 ps, Renderer2D& renderer, const GameData& game) const;

    // Returns (ps_id, depth) for all auto-draw systems (used in 3-way depth-sorted render pass).
    [[nodiscard]] std::vector<std::pair<i32, f64>> auto_draw_systems() const;

private:
    f64 rand_range(f64 a, f64 b);
    i32 spawn_count(i32 requested);

    std::unordered_map<i32, ParticleSystem> systems_;
    std::unordered_map<i32, ParticleType> types_;
    i32 next_sys_id_ = 0;
    i32 next_type_id_ = 0;
    i32 effect_sys_below_ = -1;
    i32 effect_sys_above_ = -1;
    i32 effect_type_base_ = -1;
    GmlRandom& rng_;
};

} // namespace gm
