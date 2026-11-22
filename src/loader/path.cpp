#include "loader/game_data.h"

#include <algorithm>
#include <cmath>

namespace gm {

namespace {

PathPoint midpoint(const PathPoint& a, const PathPoint& b) noexcept {
    return {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.speed + b.speed) * 0.5};
}

f64 distance_squared(const PathPoint& a, const PathPoint& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dy = a.y - b.y;
    return dx * dx + dy * dy;
}

} // namespace

void PathData::add_sample(PathPoint point) const {
    const f64 distance = samples_.empty() ? 0.0 :
        samples_.back().distance + std::sqrt(distance_squared(samples_.back().point, point));
    samples_.push_back({point, distance});
}

void PathData::subdivide(const PathPoint& a, const PathPoint& b, const PathPoint& c, i32 depth) const {
    if (depth == 0) return;
    const PathPoint ab = midpoint(a, b);
    const PathPoint bc = midpoint(b, c);
    const PathPoint mid = midpoint(ab, bc);
    // GM subdivides each side independently while its control points are over four pixels apart.
    if (distance_squared(a, b) > 16.0) subdivide(a, ab, mid, depth - 1);
    add_sample(mid);
    if (distance_squared(b, c) > 16.0) subdivide(mid, bc, c, depth - 1);
}

void PathData::build_samples() const {
    const auto& points = geometry_.points;
    if (!samples_.empty() || points.empty()) return;

    if (geometry_.kind == 1) {
        if (!geometry_.closed) add_sample(points.front());
        const usize count = geometry_.closed ? points.size() : points.size() - std::min<usize>(2, points.size());
        const i32 depth = std::clamp(geometry_.precision, 0, 8);
        for (usize i = 0; i < count; ++i) {
            const auto& a = points[i];
            const auto& b = points[(i + 1) % points.size()];
            const auto& c = points[(i + 2) % points.size()];
            subdivide(midpoint(a, b), b, midpoint(b, c), depth);
        }
        if (!geometry_.closed) add_sample(points.back());
    } else {
        for (const auto& point : points) add_sample(point);
    }
    if (geometry_.closed && !samples_.empty()) add_sample(samples_.front().point);
}

f64 PathData::length() const {
    build_samples();
    return samples_.empty() ? 0.0 : samples_.back().distance;
}

PathPoint PathData::sample(f64 position) const {
    const f64 total = length();
    if (samples_.empty()) return {};
    if (total == 0.0 || !(position > 0.0)) return samples_.front().point;
    if (position >= 1.0) return samples_.back().point;

    const f64 distance = position * total;
    // At coincident points GM takes the last speed at that distance.
    const auto next = std::upper_bound(samples_.begin() + 1, samples_.end() - 1, distance,
        [](f64 d, const Sample& s) { return d < s.distance; });
    const auto& prev = *(next - 1);
    const f64 span = next->distance - prev.distance;
    if (span == 0.0) return prev.point;
    const f64 t = (distance - prev.distance) / span;
    return {prev.point.x + (next->point.x - prev.point.x) * t,
            prev.point.y + (next->point.y - prev.point.y) * t,
            prev.point.speed + (next->point.speed - prev.point.speed) * t};
}

} // namespace gm
