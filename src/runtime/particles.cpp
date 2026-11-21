#include "runtime/particles.h"

#include "base/math.h"

#include <algorithm>
#include <cmath>

namespace gm {

namespace {

u32 lerp_bgr(u32 c1, u32 c2, f64 t) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    const ColorRGBA a = gm_bgr_to_rgba(c1);
    const ColorRGBA b = gm_bgr_to_rgba(c2);
    const u8 r = static_cast<u8>(a.r + (b.r - a.r) * t);
    const u8 g = static_cast<u8>(a.g + (b.g - a.g) * t);
    const u8 bl = static_cast<u8>(a.b + (b.b - a.b) * t);
    return rgba_to_gm_bgr(r, g, bl);
}

u32 hsv_to_gm_bgr(f64 h255, f64 s255, f64 v255) noexcept {
    const f64 h = std::fmod(std::max(0.0, h255), 256.0) / 256.0 * 6.0;
    const f64 s = std::clamp(s255 / 255.0, 0.0, 1.0);
    const f64 v = std::clamp(v255 / 255.0, 0.0, 1.0);
    const i32 sector = static_cast<i32>(std::floor(h)) % 6;
    const f64 f = h - std::floor(h);
    const f64 p = v * (1.0 - s);
    const f64 q = v * (1.0 - s * f);
    const f64 t = v * (1.0 - s * (1.0 - f));
    f64 r = 0, g = 0, b = 0;
    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return rgba_to_gm_bgr(static_cast<u8>(r * 255.0 + 0.5),
                          static_cast<u8>(g * 255.0 + 0.5),
                          static_cast<u8>(b * 255.0 + 0.5));
}

bool point_in_region(f64 x, f64 y, f64 xmin, f64 xmax, f64 ymin, f64 ymax, i32 shape) noexcept {
    if (x < xmin || x > xmax || y < ymin || y > ymax) return false;
    if (shape == 0) return true; // rectangle
    const f64 cx = (xmin + xmax) * 0.5;
    const f64 cy = (ymin + ymax) * 0.5;
    const f64 rx = std::max(1e-5, (xmax - xmin) * 0.5);
    const f64 ry = std::max(1e-5, (ymax - ymin) * 0.5);
    const f64 nx = (x - cx) / rx;
    const f64 ny = (y - cy) / ry;
    if (shape == 1) return (nx * nx + ny * ny) <= 1.0; // ellipse
    if (shape == 2) return (std::fabs(nx) + std::fabs(ny)) <= 1.0; // diamond
    return true;
}

} // namespace

f64 ParticleManager::rand_range(f64 a, f64 b) {
    if (a >= b) return a;
    return a + (static_cast<f64>(rng_.next()) * 0x1p-32) * (b - a);
}

i32 ParticleManager::spawn_count(i32 requested) {
    if (requested >= 0) return requested;
    return rng_.below(0u - static_cast<u32>(requested)) == 0 ? 1 : 0;
}

void ParticleManager::clear_all() {
    systems_.clear();
    types_.clear();
    next_sys_id_ = 0;
    next_type_id_ = 0;
    effect_sys_below_ = -1;
    effect_sys_above_ = -1;
    effect_type_ = -1;
}

i32 ParticleManager::system_create() {
    const i32 id = next_sys_id_++;
    systems_.emplace(id, ParticleSystem{});
    return id;
}

void ParticleManager::system_destroy(i32 ps) { systems_.erase(ps); }
void ParticleManager::system_destroy_all() { systems_.clear(); }
bool ParticleManager::system_exists(i32 ps) const noexcept { return systems_.contains(ps); }
void ParticleManager::system_clear(i32 ps) {
    if (auto it = systems_.find(ps); it != systems_.end()) {
        it->second.particles.clear();
        it->second.emitters.clear();
        it->second.attractors.clear();
        it->second.destroyers.clear();
        it->second.deflectors.clear();
        it->second.changers.clear();
    }
}
void ParticleManager::system_depth(i32 ps, f64 depth) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.depth = depth;
}
void ParticleManager::system_position(i32 ps, f64 x, f64 y) {
    if (auto it = systems_.find(ps); it != systems_.end()) { it->second.x = x; it->second.y = y; }
}
void ParticleManager::system_automatic_update(i32 ps, bool auto_upd) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.auto_update = auto_upd;
}
void ParticleManager::system_automatic_draw(i32 ps, bool auto_drw) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.auto_draw = auto_drw;
}
void ParticleManager::system_draw_order(i32 ps, bool oldtonew) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.old_to_new = oldtonew;
}
i32 ParticleManager::particles_count(i32 ps) const {
    const auto it = systems_.find(ps);
    return it != systems_.end() ? static_cast<i32>(it->second.particles.size()) : 0;
}
void ParticleManager::particles_clear(i32 ps) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.particles.clear();
}

i32 ParticleManager::type_create() {
    const i32 id = next_type_id_++;
    types_.emplace(id, ParticleType{});
    return id;
}
void ParticleManager::type_destroy(i32 pt) { types_.erase(pt); }
bool ParticleManager::type_exists(i32 pt) const noexcept { return types_.contains(pt); }
void ParticleManager::type_clear(i32 pt) {
    if (auto it = types_.find(pt); it != types_.end()) it->second = ParticleType{};
}
ParticleType* ParticleManager::get_type(i32 pt) {
    const auto it = types_.find(pt);
    return it != types_.end() ? &it->second : nullptr;
}

void ParticleManager::particles_create(i32 ps, f64 x, f64 y, i32 pt, i32 count, bool override_col, u32 col) {
    const auto sit = systems_.find(ps);
    const auto tit = types_.find(pt);
    if (sit == systems_.end() || tit == types_.end() || count <= 0) return;

    const auto& t = tit->second;
    for (i32 i = 0; i < count; ++i) {
        LiveParticle p{};
        p.type_id = pt;
        p.x = x;
        p.y = y;
        p.speed = rand_range(t.speed_min, t.speed_max);
        p.dir = rand_range(t.dir_min, t.dir_max);
        p.angle = rand_range(t.ang_min, t.ang_max) + (t.ang_relative ? p.dir : 0.0);
        p.life = std::max(1, static_cast<i32>(delphi_round(rand_range(t.life_min, t.life_max))));
        if (t.col_mode == 3) {
            p.color = lerp_bgr(t.col1, t.col2, rand_range(0.0, 1.0));
        } else if (t.col_mode == 4) {
            const u8 r = static_cast<u8>(std::clamp(rand_range(t.rmin, t.rmax), 0.0, 255.0));
            const u8 g = static_cast<u8>(std::clamp(rand_range(t.gmin, t.gmax), 0.0, 255.0));
            const u8 b = static_cast<u8>(std::clamp(rand_range(t.bmin, t.bmax), 0.0, 255.0));
            p.color = rgba_to_gm_bgr(r, g, b);
        } else if (t.col_mode == 5) {
            const f64 h = rand_range(t.hmin, t.hmax);
            const f64 s = rand_range(t.smin, t.smax);
            const f64 v = rand_range(t.vmin, t.vmax);
            p.color = hsv_to_gm_bgr(h, s, v);
        } else {
            p.color = t.col1;
        }
        if (override_col) p.color = col;
        p.alpha = t.alpha1;
        p.size = rand_range(t.size_min, t.size_max);
        if (t.sprite_random) p.subimg = rng_.below(10000);
        (void)rng_.below(100000); // GM assigns a random wiggle phase even when every wiggle is zero.
        sit->second.particles.push_back(p);
    }
}

i32 ParticleManager::emitter_create(i32 ps) {
    const auto it = systems_.find(ps);
    if (it == systems_.end()) return -1;
    const i32 id = it->second.next_emitter_id++;
    it->second.emitters.emplace(id, ParticleEmitter{});
    return id;
}
void ParticleManager::emitter_destroy(i32 ps, i32 em) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.emitters.erase(em);
}
void ParticleManager::emitter_destroy_all(i32 ps) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.emitters.clear();
}
bool ParticleManager::emitter_exists(i32 ps, i32 em) const noexcept {
    const auto it = systems_.find(ps);
    return it != systems_.end() && it->second.emitters.contains(em);
}
void ParticleManager::emitter_clear(i32 ps, i32 em) {
    if (auto it = systems_.find(ps); it != systems_.end()) {
        if (auto eit = it->second.emitters.find(em); eit != it->second.emitters.end()) eit->second = ParticleEmitter{};
    }
}
void ParticleManager::emitter_region(i32 ps, i32 em, f64 xmin, f64 xmax, f64 ymin, f64 ymax, i32 shape, i32 distr) {
    const auto sit = systems_.find(ps);
    if (sit == systems_.end()) return;
    const auto eit = sit->second.emitters.find(em);
    if (eit == sit->second.emitters.end()) return;
    eit->second.xmin = std::min(xmin, xmax);
    eit->second.xmax = std::max(xmin, xmax);
    eit->second.ymin = std::min(ymin, ymax);
    eit->second.ymax = std::max(ymin, ymax);
    eit->second.shape = shape;
    eit->second.distr = distr;
}
void ParticleManager::emitter_burst(i32 ps, i32 em, i32 pt, i32 number) {
    const auto sit = systems_.find(ps);
    if (sit == systems_.end()) return;
    const auto eit = sit->second.emitters.find(em);
    if (eit == sit->second.emitters.end()) return;

    const i32 spawn_cnt = spawn_count(number);
    const auto& e = eit->second;
    for (i32 i = 0; i < spawn_cnt; ++i) {
        const f64 px = rand_range(e.xmin, e.xmax);
        const f64 py = rand_range(e.ymin, e.ymax);
        particles_create(ps, px, py, pt, 1);
    }
}
void ParticleManager::emitter_stream(i32 ps, i32 em, i32 pt, i32 number) {
    const auto sit = systems_.find(ps);
    if (sit == systems_.end()) return;
    const auto eit = sit->second.emitters.find(em);
    if (eit == sit->second.emitters.end()) return;
    eit->second.stream_type = pt;
    eit->second.stream_count = number;
}

i32 ParticleManager::attractor_create(i32 ps) {
    const auto it = systems_.find(ps);
    if (it == systems_.end()) return -1;
    const i32 id = it->second.next_attractor_id++;
    it->second.attractors.emplace(id, ParticleAttractor{});
    return id;
}
void ParticleManager::attractor_destroy(i32 ps, i32 at) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.attractors.erase(at);
}
void ParticleManager::attractor_destroy_all(i32 ps) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.attractors.clear();
}
bool ParticleManager::attractor_exists(i32 ps, i32 at) const noexcept {
    const auto it = systems_.find(ps);
    return it != systems_.end() && it->second.attractors.contains(at);
}
void ParticleManager::attractor_clear(i32 ps, i32 at) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto ait = it->second.attractors.find(at); ait != it->second.attractors.end()) ait->second = ParticleAttractor{};
}
void ParticleManager::attractor_position(i32 ps, i32 at, f64 x, f64 y) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto ait = it->second.attractors.find(at); ait != it->second.attractors.end()) { ait->second.x = x; ait->second.y = y; }
}
void ParticleManager::attractor_force(i32 ps, i32 at, f64 force, f64 dist, i32 kind, bool additive) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto ait = it->second.attractors.find(at); ait != it->second.attractors.end()) {
            ait->second.force = force; ait->second.dist = dist; ait->second.kind = kind; ait->second.additive = additive;
        }
}

i32 ParticleManager::destroyer_create(i32 ps) {
    const auto it = systems_.find(ps);
    if (it == systems_.end()) return -1;
    const i32 id = it->second.next_destroyer_id++;
    it->second.destroyers.emplace(id, ParticleDestroyer{});
    return id;
}
void ParticleManager::destroyer_destroy(i32 ps, i32 de) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.destroyers.erase(de);
}
void ParticleManager::destroyer_destroy_all(i32 ps) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.destroyers.clear();
}
bool ParticleManager::destroyer_exists(i32 ps, i32 de) const noexcept {
    const auto it = systems_.find(ps);
    return it != systems_.end() && it->second.destroyers.contains(de);
}
void ParticleManager::destroyer_clear(i32 ps, i32 de) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto dit = it->second.destroyers.find(de); dit != it->second.destroyers.end()) dit->second = ParticleDestroyer{};
}
void ParticleManager::destroyer_region(i32 ps, i32 de, f64 xmin, f64 xmax, f64 ymin, f64 ymax, i32 shape) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto dit = it->second.destroyers.find(de); dit != it->second.destroyers.end()) {
            dit->second.xmin = std::min(xmin, xmax); dit->second.xmax = std::max(xmin, xmax);
            dit->second.ymin = std::min(ymin, ymax); dit->second.ymax = std::max(ymin, ymax);
            dit->second.shape = shape;
        }
}

i32 ParticleManager::deflector_create(i32 ps) {
    const auto it = systems_.find(ps);
    if (it == systems_.end()) return -1;
    const i32 id = it->second.next_deflector_id++;
    it->second.deflectors.emplace(id, ParticleDeflector{});
    return id;
}
void ParticleManager::deflector_destroy(i32 ps, i32 df) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.deflectors.erase(df);
}
void ParticleManager::deflector_destroy_all(i32 ps) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.deflectors.clear();
}
bool ParticleManager::deflector_exists(i32 ps, i32 df) const noexcept {
    const auto it = systems_.find(ps);
    return it != systems_.end() && it->second.deflectors.contains(df);
}
void ParticleManager::deflector_clear(i32 ps, i32 df) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto dit = it->second.deflectors.find(df); dit != it->second.deflectors.end()) dit->second = ParticleDeflector{};
}
void ParticleManager::deflector_region(i32 ps, i32 df, f64 xmin, f64 xmax, f64 ymin, f64 ymax) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto dit = it->second.deflectors.find(df); dit != it->second.deflectors.end()) {
            dit->second.xmin = std::min(xmin, xmax); dit->second.xmax = std::max(xmin, xmax);
            dit->second.ymin = std::min(ymin, ymax); dit->second.ymax = std::max(ymin, ymax);
        }
}
void ParticleManager::deflector_kind(i32 ps, i32 df, i32 kind) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto dit = it->second.deflectors.find(df); dit != it->second.deflectors.end()) dit->second.kind = kind;
}
void ParticleManager::deflector_friction(i32 ps, i32 df, f64 friction) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto dit = it->second.deflectors.find(df); dit != it->second.deflectors.end()) dit->second.friction = friction;
}

i32 ParticleManager::changer_create(i32 ps) {
    const auto it = systems_.find(ps);
    if (it == systems_.end()) return -1;
    const i32 id = it->second.next_changer_id++;
    it->second.changers.emplace(id, ParticleChanger{});
    return id;
}
void ParticleManager::changer_destroy(i32 ps, i32 ch) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.changers.erase(ch);
}
void ParticleManager::changer_destroy_all(i32 ps) {
    if (auto it = systems_.find(ps); it != systems_.end()) it->second.changers.clear();
}
bool ParticleManager::changer_exists(i32 ps, i32 ch) const noexcept {
    const auto it = systems_.find(ps);
    return it != systems_.end() && it->second.changers.contains(ch);
}
void ParticleManager::changer_clear(i32 ps, i32 ch) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto cit = it->second.changers.find(ch); cit != it->second.changers.end()) cit->second = ParticleChanger{};
}
void ParticleManager::changer_region(i32 ps, i32 ch, f64 xmin, f64 xmax, f64 ymin, f64 ymax, i32 shape) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto cit = it->second.changers.find(ch); cit != it->second.changers.end()) {
            cit->second.xmin = std::min(xmin, xmax); cit->second.xmax = std::max(xmin, xmax);
            cit->second.ymin = std::min(ymin, ymax); cit->second.ymax = std::max(ymin, ymax);
            cit->second.shape = shape;
        }
}
void ParticleManager::changer_kind(i32 ps, i32 ch, i32 kind) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto cit = it->second.changers.find(ch); cit != it->second.changers.end()) cit->second.kind = kind;
}
void ParticleManager::changer_types(i32 ps, i32 ch, i32 pt1, i32 pt2) {
    if (auto it = systems_.find(ps); it != systems_.end())
        if (auto cit = it->second.changers.find(ch); cit != it->second.changers.end()) {
            cit->second.type1 = pt1; cit->second.type2 = pt2;
        }
}

void ParticleManager::effect_create(bool above, i32, f64 x, f64 y, i32 size, u32 color) {
    if (effect_sys_below_ < 0 || !systems_.contains(effect_sys_below_)) {
        effect_sys_below_ = system_create();
        system_depth(effect_sys_below_, 100000.0);
    }
    if (effect_sys_above_ < 0 || !systems_.contains(effect_sys_above_)) {
        effect_sys_above_ = system_create();
        system_depth(effect_sys_above_, -100000.0);
    }
    if (effect_type_ < 0 || !types_.contains(effect_type_)) {
        effect_type_ = type_create();
        auto* t = get_type(effect_type_);
        t->speed_min = 1.0; t->speed_max = 4.0;
        t->dir_min = 0.0; t->dir_max = 360.0;
        t->life_min = 15; t->life_max = 35;
        t->alpha1 = 1.0; t->alpha2 = 0.0; t->alpha3 = 0.0;
    }
    const i32 ps = above ? effect_sys_above_ : effect_sys_below_;
    const i32 count = (size == 0) ? 10 : (size == 1 ? 20 : 32);
    particles_create(ps, x, y, effect_type_, count, true, color);
}

void ParticleManager::effect_clear() {
    if (effect_sys_below_ >= 0) particles_clear(effect_sys_below_);
    if (effect_sys_above_ >= 0) particles_clear(effect_sys_above_);
}

void ParticleManager::update_system(i32 ps) {
    const auto sit = systems_.find(ps);
    if (sit == systems_.end()) return;
    auto& sys = sit->second;

    // Age the particles present on entry. Children draw their birth randoms immediately,
    // before the next parent's chance roll, and join the motion pass at age zero.
    const usize previous_count = sys.particles.size();
    for (usize i = 0; i < previous_count; ++i) {
        auto& p = sys.particles[i];
        p.alive = ++p.age < p.life;
        const auto tit = types_.find(p.type_id);
        if (tit == types_.end()) continue;
        const auto& t = tit->second;
        const i32 count = spawn_count(p.alive ? t.step_number : t.death_number);
        if (count > 0) {
            particles_create(ps, p.x, p.y, p.alive ? t.step_type : t.death_type, count);
        }
    }

    for (auto& p : sys.particles) {
        if (!p.alive) continue;
        const auto tit = types_.find(p.type_id);
        if (tit == types_.end()) continue;
        const auto& t = tit->second;
        p.speed += t.speed_incr;
        p.dir += t.dir_incr;
        p.size = std::max(0.0, p.size + t.size_incr);
        p.angle += t.ang_incr;

        const f64 rad = deg_to_rad(p.dir);
        f64 vx = p.speed * std::cos(rad);
        f64 vy = -p.speed * std::sin(rad);
        if (t.grav_amount != 0.0) {
            const f64 grad = deg_to_rad(t.grav_dir);
            vx += t.grav_amount * std::cos(grad);
            vy += -t.grav_amount * std::sin(grad);
        }

        // Attractors
        for (const auto& [aid, att] : sys.attractors) {
            const f64 dx = att.x - p.x;
            const f64 dy = att.y - p.y;
            const f64 d = std::hypot(dx, dy);
            if (d > 1e-4 && d <= att.dist) {
                f64 f = att.force;
                if (att.kind == 1) f *= (1.0 - d / att.dist);
                else if (att.kind == 2) { const f64 q = 1.0 - d / att.dist; f *= q * q; }
                const f64 fx = (dx / d) * f;
                const f64 fy = (dy / d) * f;
                if (att.additive) { vx += fx; vy += fy; }
                else { p.x += fx; p.y += fy; }
            }
        }

        // Deflectors
        for (const auto& [dfid, def] : sys.deflectors) {
            if (p.x >= def.xmin && p.x <= def.xmax && p.y >= def.ymin && p.y <= def.ymax) {
                if (def.kind == 1) vy = -vy; else vx = -vx;
                const f64 spd = std::max(0.0, std::hypot(vx, vy) - def.friction);
                const f64 cur = std::hypot(vx, vy);
                if (cur > 1e-5) { vx = vx / cur * spd; vy = vy / cur * spd; }
            }
        }

        p.speed = std::hypot(vx, vy);
        p.dir = rad_to_deg(std::atan2(-vy, vx));
        p.x += vx;
        p.y += vy;

        // Destroyers
        for (const auto& [deid, des] : sys.destroyers) {
            if (point_in_region(p.x, p.y, des.xmin, des.xmax, des.ymin, des.ymax, des.shape)) {
                p.alive = false;
                break;
            }
        }
        if (!p.alive) continue;

        // Changers
        for (const auto& [chid, chg] : sys.changers) {
            if (p.type_id == chg.type1 && chg.type2 >= 0 &&
                point_in_region(p.x, p.y, chg.xmin, chg.xmax, chg.ymin, chg.ymax, chg.shape)) {
                p.type_id = chg.type2;
                break;
            }
        }

        const f64 frac = static_cast<f64>(p.age) / static_cast<f64>(std::max(1, p.life));
        if (frac < 0.5) {
            p.alpha = t.alpha1 + (t.alpha2 - t.alpha1) * (frac * 2.0);
            if (t.col_mode == 1) p.color = lerp_bgr(t.col1, t.col2, frac);
            else if (t.col_mode == 2) p.color = lerp_bgr(t.col1, t.col2, frac * 2.0);
        } else {
            p.alpha = t.alpha2 + (t.alpha3 - t.alpha2) * ((frac - 0.5) * 2.0);
            if (t.col_mode == 1) p.color = lerp_bgr(t.col1, t.col2, frac);
            else if (t.col_mode == 2) p.color = lerp_bgr(t.col2, t.col3, (frac - 0.5) * 2.0);
        }
    }

    sys.particles.erase(
        std::remove_if(sys.particles.begin(), sys.particles.end(), [](const LiveParticle& p) { return !p.alive; }),
        sys.particles.end());

    for (const auto& [eid, em] : sys.emitters) {
        if (em.stream_type >= 0 && em.stream_count != 0) {
            emitter_burst(ps, eid, em.stream_type, em.stream_count);
        }
    }
}

void ParticleManager::update_all() {
    std::vector<i32> ids;
    ids.reserve(systems_.size());
    for (const auto& [id, sys] : systems_) {
        if (sys.auto_update) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    for (i32 id : ids) update_system(id);
}

void ParticleManager::draw_system(i32 ps, Renderer2D& renderer, const GameData& game) const {
    const auto sit = systems_.find(ps);
    if (sit == systems_.end()) return;
    const auto& sys = sit->second;

    const usize n = sys.particles.size();
    for (usize idx = 0; idx < n; ++idx) {
        const auto& p = sys.particles[sys.old_to_new ? idx : (n - 1u - idx)];
        if (!p.alive) continue;
        const auto tit = types_.find(p.type_id);
        if (tit == types_.end()) continue;
        const auto& t = tit->second;
        if (t.additive) renderer.set_blend_mode(1);

        const f32 px = static_cast<f32>(sys.x + p.x);
        const f32 py = static_cast<f32>(sys.y + p.y);
        if (t.sprite_index >= 0) {
            renderer.draw_sprite_ext(
                game, t.sprite_index, static_cast<i32>(p.subimg),
                px, py,
                static_cast<f32>(p.size * t.xscale), static_cast<f32>(p.size * t.yscale),
                static_cast<f32>(p.angle), p.color, static_cast<f32>(p.alpha));
        } else {
            const f32 r = std::max(1.0f, static_cast<f32>(p.size * 4.0));
            renderer.draw_circle_color(px, py, r, p.color, p.color, false, static_cast<f32>(p.alpha));
        }
        if (t.additive) renderer.set_blend_mode(0);
    }
}

std::vector<std::pair<i32, f64>> ParticleManager::auto_draw_systems() const {
    std::vector<std::pair<i32, f64>> out;
    for (const auto& [id, sys] : systems_) {
        if (sys.auto_draw && !sys.particles.empty()) {
            out.emplace_back(id, sys.depth);
        }
    }
    return out;
}

} // namespace gm
