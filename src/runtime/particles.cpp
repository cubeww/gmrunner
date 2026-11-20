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

} // namespace

f64 ParticleManager::rand_range(f64 a, f64 b) {
    if (a >= b) return a;
    std::uniform_real_distribution<f64> dist(a, b);
    return dist(rng_);
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
        p.size = rand_range(t.size_min, t.size_max);
        p.angle = rand_range(t.ang_min, t.ang_max) + (t.ang_relative ? p.dir : 0.0);
        p.color = override_col ? col : t.col1;
        p.alpha = t.alpha1;
        p.age = 0;
        p.life = std::max(1, static_cast<i32>(delphi_round(rand_range(static_cast<f64>(t.life_min), static_cast<f64>(t.life_max)))));
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

    i32 spawn_cnt = number;
    if (number < 0) {
        if (rand_range(0.0, static_cast<f64>(-number)) < 1.0) spawn_cnt = 1;
        else return;
    }
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

    for (const auto& [eid, em] : sys.emitters) {
        if (em.stream_type >= 0 && em.stream_count != 0) {
            emitter_burst(ps, eid, em.stream_type, em.stream_count);
        }
    }

    for (auto& p : sys.particles) {
        if (!p.alive) continue;
        ++p.age;
        if (p.age >= p.life) {
            p.alive = false;
            continue;
        }
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
            p.speed = std::hypot(vx, vy);
            p.dir = rad_to_deg(std::atan2(-vy, vx));
        }
        p.x += vx;
        p.y += vy;

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
}

void ParticleManager::update_all() {
    std::vector<i32> ids;
    ids.reserve(systems_.size());
    for (const auto& [id, sys] : systems_) {
        if (sys.auto_update) ids.push_back(id);
    }
    for (i32 id : ids) update_system(id);
}

void ParticleManager::draw_system(i32 ps, Renderer2D& renderer, const GameData& game) const {
    const auto sit = systems_.find(ps);
    if (sit == systems_.end()) return;
    const auto& sys = sit->second;

    for (const auto& p : sys.particles) {
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
