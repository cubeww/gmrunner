#include "graphics/model3d.h"

#include "graphics/gl_headers.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>

namespace gm {

namespace {

inline ColorRGBA make_rgba_u32(u32 gm_bgr, f64 alpha) noexcept {
    const f32 clamped = static_cast<f32>(std::clamp(alpha, 0.0, 1.0));
    return gm_bgr_to_rgba(gm_bgr, static_cast<u8>(clamped * 255.0f + 0.5f));
}

inline void push_fan_as_tris(std::vector<Vertex3D>& out, const Vertex3D* fan, usize count, bool recorded) {
    if (count < 3) return;
    for (usize i = 1; i + 1 < count; ++i) {
        // GM8.1+ records fans as center-first triangle lists; D3D fans shade from the spoke.
        if (recorded) out.push_back(fan[0]);
        out.push_back(fan[i]);
        out.push_back(fan[i + 1]);
        if (!recorded) out.push_back(fan[0]);
    }
}

inline void push_strip_as_tris(std::vector<Vertex3D>& out, const std::vector<Vertex3D>& strip, bool recorded) {
    if (strip.size() < 3) return;
    for (usize i = 0; i + 2 < strip.size(); ++i) {
        if ((i & 1u) == 0u) {
            out.push_back(strip[i]);
            out.push_back(strip[i + 1]);
            out.push_back(strip[i + 2]);
        } else {
            if (!recorded) out.push_back(strip[i]);
            out.push_back(strip[i + 2]);
            out.push_back(strip[i + 1]);
            if (recorded) out.push_back(strip[i]);
        }
    }
}

std::string_view read_model_token(std::string_view& text, usize limit) {
    while (!text.empty() && static_cast<u8>(text.front()) <= ' ') text.remove_prefix(1);
    usize n = 0;
    while (n < text.size() && n < limit && static_cast<u8>(text[n]) > ' ') ++n;
    const auto token = text.substr(0, n);
    text.remove_prefix(n);
    return token;
}

void end_model_line(std::string_view& text) {
    const usize pos = text.find_first_of("\r\n");
    if (pos == std::string_view::npos) {
        text = {};
    } else {
        const bool crlf = text.substr(pos, 2) == "\r\n";
        text.remove_prefix(pos + (crlf ? 2u : 1u));
    }
}

bool model_integer(std::string_view token, i32& n) {
    n = 0;
    if (token.empty()) return true; // ReadLong returns zero at EOF.
    const bool negative = token.front() == '-';
    if (negative || token.front() == '+') token.remove_prefix(1);
    int base = 10;
    if (token.starts_with('$')) { base = 16; token.remove_prefix(1); }
    else if (token.starts_with("0x") || token.starts_with("0X")) { base = 16; token.remove_prefix(2); }
    u32 value = 0;
    const char* end = token.data() + token.size();
    const auto result = std::from_chars(token.data(), end, value, base);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    n = static_cast<i32>(negative ? 0u - value : value);
    return true;
}

// The Delphi RTL scales first by 10^(n mod 32), then by the remaining power.
f64 model_scale10(f64 value, int exponent) {
    const int n = std::abs(exponent);
    const f64 lo = std::pow(10.0, n % 32);
    const f64 hi = std::pow(10.0, n - n % 32);
    return exponent < 0 ? (value / lo) / hi : (value * lo) * hi;
}

bool model_real(std::string_view token, f64& value) {
    value = 0.0;
    if (token.empty()) return true; // ReadExt returns zero at EOF.
    const bool negative = token.front() == '-';
    if (negative || token.front() == '+') token.remove_prefix(1);
    if (token.empty()) return false;
    int scale = 0;
    bool fraction = false;
    while (!token.empty()) {
        const char c = token.front();
        if (c == '.' && !fraction) fraction = true;
        else if (c >= '0' && c <= '9') {
            value = value * 10.0 + (c - '0');
            if (fraction) --scale;
        } else break;
        token.remove_prefix(1);
    }
    if (!token.empty()) {
        // On an invalid field Val leaves the unsigned, unscaled mantissa in the entry.
        if (token.front() != 'e' && token.front() != 'E') return false;
        token.remove_prefix(1);
        const bool exp_negative = token.starts_with('-');
        if (exp_negative || token.starts_with('+')) token.remove_prefix(1);
        if (token.empty()) return false;
        int exponent = 0;
        for (char c : token) {
            if (c < '0' || c > '9') return false;
            exponent = std::min(1000, exponent * 10 + c - '0');
        }
        scale += exp_negative ? -exponent : exponent;
    }
    value = model_scale10(negative ? -value : value, scale);
    return true;
}

void append_model_real(std::string& out, f64 value, char decimal) {
    if (!std::isfinite(value)) {
        out += std::isnan(value) ? "NAN" : (value < 0 ? "-INF" : "INF");
        return;
    }
    const f64 magnitude = std::abs(value);
    u64 digits = 0;
    int point = 0;
    int keep = 0;
    if (magnitude >= 0.00001) {
        // FloatToStrF(ffFixed, 10, 4) first converts to 18 decimal digits, then
        // rounds the first discarded decimal digit up at five.
        point = static_cast<int>(std::floor(std::ilogb(magnitude) * (19728.0 / 65536.0))) + 1;
        f64 scaled = std::nearbyint(model_scale10(magnitude, 18 - point));
        if (scaled >= 1e18) {
            scaled = std::nearbyint(scaled / 10.0);
            ++point;
        }
        keep = std::clamp(point + 4, 0, 10);
        const u64 divisor = static_cast<u64>(std::pow(10.0, 18 - keep));
        digits = (static_cast<u64>(scaled) + divisor / 2) / divisor;
    }
    if (digits == 0) {
        out += '0';
        out += decimal;
        out += "0000";
        return;
    }
    if (value < 0.0) out += '-';
    std::string s = std::to_string(digits);
    point += static_cast<int>(s.size()) - keep;
    if (point > 10) {
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        out += s.front();
        if (s.size() > 1) { out += decimal; out.append(s, 1); }
        out += 'E';
        const auto exponent = std::to_string(point - 1);
        out.append(4 - exponent.size(), '0');
        out += exponent;
    } else {
        if (point <= 0) {
            s.insert(0, static_cast<usize>(1 - point), '0');
            point = 1;
        }
        s.resize(static_cast<usize>(point) + 4, '0');
        s.insert(static_cast<usize>(point), 1, decimal);
        out += s;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Exact GM8 Fixed-Function Basic 3D Shape Builders (gm82.exe.c sub_565054..sub_566024)
// ---------------------------------------------------------------------------

void append_d3d_block(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hr, f32 vr,
    ColorRGBA col, bool recorded) {
    auto emit_face = [&](f32 ax, f32 ay, f32 az, f32 au, f32 av,
                         f32 bx, f32 by, f32 bz, f32 bu, f32 bv,
                         f32 cx, f32 cy, f32 cz, f32 cu, f32 cv,
                         f32 dx, f32 dy, f32 dz, f32 du, f32 dv,
                         f32 nx, f32 ny, f32 nz) {
        const Vertex3D fan[4] = {
            {ax, ay, az, nx, ny, nz, au, av, col.r, col.g, col.b, col.a},
            {bx, by, bz, nx, ny, nz, bu, bv, col.r, col.g, col.b, col.a},
            {cx, cy, cz, nx, ny, nz, cu, cv, col.r, col.g, col.b, col.a},
            {dx, dy, dz, nx, ny, nz, du, dv, col.r, col.g, col.b, col.a},
        };
        push_fan_as_tris(out_tris, fan, 4, recorded);
    };

    // z = z1, n = (0, 0, -1)
    emit_face(x1, y1, z1, 0.0f, 0.0f,
              x1, y2, z1, 0.0f, vr,
              x2, y2, z1, hr,   vr,
              x2, y1, z1, hr,   0.0f,
              0.0f, 0.0f, -1.0f);
    // z = z2, n = (0, 0, +1)
    emit_face(x1, y1, z2, 0.0f, 0.0f,
              x2, y1, z2, hr,   0.0f,
              x2, y2, z2, hr,   vr,
              x1, y2, z2, 0.0f, vr,
              0.0f, 0.0f, 1.0f);
    // y = y2, n = (0, +1, 0)
    emit_face(x1, y2, z1, 0.0f, 0.0f,
              x1, y2, z2, 0.0f, vr,
              x2, y2, z2, hr,   vr,
              x2, y2, z1, hr,   0.0f,
              0.0f, 1.0f, 0.0f);
    // x = x2, n = (+1, 0, 0)
    emit_face(x2, y2, z1, 0.0f, 0.0f,
              x2, y2, z2, 0.0f, vr,
              x2, y1, z2, hr,   vr,
              x2, y1, z1, hr,   0.0f,
              1.0f, 0.0f, 0.0f);
    // y = y1, n = (0, -1, 0)
    emit_face(x2, y1, z1, 0.0f, 0.0f,
              x2, y1, z2, 0.0f, vr,
              x1, y1, z2, hr,   vr,
              x1, y1, z1, hr,   0.0f,
              0.0f, -1.0f, 0.0f);
    // x = x1, n = (-1, 0, 0)
    emit_face(x1, y1, z1, 0.0f, 0.0f,
              x1, y1, z2, 0.0f, vr,
              x1, y2, z2, hr,   vr,
              x1, y2, z1, hr,   0.0f,
              -1.0f, 0.0f, 0.0f);
}

void append_d3d_wall(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hr, f32 vr,
    ColorRGBA col, bool recorded) {
    const f32 dx = x2 - x1;
    const f32 dy = y2 - y1;
    const f32 len = std::hypot(dx, dy);
    if (len == 0.0f) return;

    const f32 nx = dy / len;
    const f32 ny = -dx / len;
    const Vertex3D fan[4] = {
        {x1, y1, z1, nx, ny, 0.0f, 0.0f, 0.0f, col.r, col.g, col.b, col.a},
        {x2, y2, z1, nx, ny, 0.0f, hr,   0.0f, col.r, col.g, col.b, col.a},
        {x2, y2, z2, nx, ny, 0.0f, hr,   vr,   col.r, col.g, col.b, col.a},
        {x1, y1, z2, nx, ny, 0.0f, 0.0f, vr,   col.r, col.g, col.b, col.a},
    };
    push_fan_as_tris(out_tris, fan, 4, recorded);
}

void append_d3d_floor(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hr, f32 vr,
    ColorRGBA col, bool recorded) {
    // Exact GM8 sub_566024 vertex order and normal (0, 0, +1).
    const Vertex3D fan[4] = {
        {x1, y1, z1, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, col.r, col.g, col.b, col.a},
        {x1, y2, z1, 0.0f, 0.0f, 1.0f, 0.0f, vr,   col.r, col.g, col.b, col.a},
        {x2, y2, z2, 0.0f, 0.0f, 1.0f, hr,   vr,   col.r, col.g, col.b, col.a},
        {x2, y1, z2, 0.0f, 0.0f, 1.0f, hr,   0.0f, col.r, col.g, col.b, col.a},
    };
    push_fan_as_tris(out_tris, fan, 4, recorded);
}

void append_d3d_cylinder(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hr, f32 vr,
    bool closed, i32 steps,
    ColorRGBA col, bool recorded) {
    const i32 n = std::clamp(steps, 3, 128);
    const f32 cx = (x1 + x2) * 0.5f;
    const f32 cy = (y1 + y2) * 0.5f;
    const f32 rx = (x2 - x1) * 0.5f;
    const f32 ry = (y2 - y1) * 0.5f;

    f32 cc[129];
    f32 ss[129];
    for (i32 i = 0; i <= n; ++i) {
        const f64 ang = (2.0 * kPi * static_cast<f64>(i)) / static_cast<f64>(n);
        cc[i] = static_cast<f32>(std::cos(ang));
        ss[i] = static_cast<f32>(std::sin(ang));
    }

    if (closed) {
        std::vector<Vertex3D> top_fan;
        top_fan.reserve(static_cast<usize>(n + 2));
        top_fan.push_back(Vertex3D{cx, cy, z2, 0.0f, 0.0f, 1.0f, 0.0f, vr, col.r, col.g, col.b, col.a});
        for (i32 i = 0; i <= n; ++i) {
            top_fan.push_back(Vertex3D{cx + cc[i] * rx, cy + ss[i] * ry, z2,
                                       0.0f, 0.0f, 1.0f, 0.0f, vr, col.r, col.g, col.b, col.a});
        }
        push_fan_as_tris(out_tris, top_fan.data(), top_fan.size(), recorded);
    }

    std::vector<Vertex3D> side;
    side.reserve(static_cast<usize>((n + 1) * 2));
    for (i32 i = 0; i <= n; ++i) {
        const f32 u = (static_cast<f32>(i) * hr) / static_cast<f32>(n);
        const f32 px = cx + cc[i] * rx;
        const f32 py = cy + ss[i] * ry;
        side.push_back(Vertex3D{px, py, z2, cc[i], ss[i], 0.0f, u, vr,   col.r, col.g, col.b, col.a});
        side.push_back(Vertex3D{px, py, z1, cc[i], ss[i], 0.0f, u, 0.0f, col.r, col.g, col.b, col.a});
    }
    push_strip_as_tris(out_tris, side, recorded);

    if (closed) {
        std::vector<Vertex3D> bot_fan;
        bot_fan.reserve(static_cast<usize>(n + 2));
        bot_fan.push_back(Vertex3D{cx, cy, z1, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
        for (i32 i = n; i >= 0; --i) {
            bot_fan.push_back(Vertex3D{cx + cc[i] * rx, cy + ss[i] * ry, z1,
                                       0.0f, 0.0f, -1.0f, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
        }
        push_fan_as_tris(out_tris, bot_fan.data(), bot_fan.size(), recorded);
    }
}

void append_d3d_cone(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hr, f32 vr,
    bool closed, i32 steps,
    ColorRGBA col, bool recorded) {
    const i32 n = std::clamp(steps, 3, 128);
    const f32 cx = (x1 + x2) * 0.5f;
    const f32 cy = (y1 + y2) * 0.5f;
    const f32 rx = (x2 - x1) * 0.5f;
    const f32 ry = (y2 - y1) * 0.5f;

    f32 cc[129];
    f32 ss[129];
    for (i32 i = 0; i <= n; ++i) {
        const f64 ang = (2.0 * kPi * static_cast<f64>(i)) / static_cast<f64>(n);
        cc[i] = static_cast<f32>(std::cos(ang));
        ss[i] = static_cast<f32>(std::sin(ang));
    }

    std::vector<Vertex3D> side;
    side.reserve(static_cast<usize>((n + 1) * 2));
    for (i32 i = 0; i <= n; ++i) {
        const f32 u = (static_cast<f32>(i) * hr) / static_cast<f32>(n);
        side.push_back(Vertex3D{cx, cy, z2, 0.0f, 0.0f, 1.0f, u, vr, col.r, col.g, col.b, col.a});
        side.push_back(Vertex3D{cx + cc[i] * rx, cy + ss[i] * ry, z1,
                                cc[i], ss[i], 0.0f, u, 0.0f, col.r, col.g, col.b, col.a});
    }
    push_strip_as_tris(out_tris, side, recorded);

    if (closed) {
        std::vector<Vertex3D> bot_fan;
        bot_fan.reserve(static_cast<usize>(n + 2));
        bot_fan.push_back(Vertex3D{cx, cy, z1, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
        for (i32 i = n; i >= 0; --i) {
            bot_fan.push_back(Vertex3D{cx + cc[i] * rx, cy + ss[i] * ry, z1,
                                       0.0f, 0.0f, -1.0f, 0.0f, 0.0f, col.r, col.g, col.b, col.a});
        }
        push_fan_as_tris(out_tris, bot_fan.data(), bot_fan.size(), recorded);
    }
}

void append_d3d_ellipsoid(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hr, f32 vr,
    i32 steps,
    ColorRGBA col, bool recorded) {
    const i32 n = std::clamp(steps, 3, 128);
    const i32 stacks = (n + 1) / 2;
    const f32 cx = (x1 + x2) * 0.5f;
    const f32 cy = (y1 + y2) * 0.5f;
    const f32 cz = (z1 + z2) * 0.5f;
    const f32 rx = (x2 - x1) * 0.5f;
    const f32 ry = (y2 - y1) * 0.5f;
    const f32 rz = (z2 - z1) * 0.5f;

    f32 cc[129];
    f32 ss[129];
    for (i32 j = 0; j <= n; ++j) {
        const f64 ang = (2.0 * kPi * static_cast<f64>(j)) / static_cast<f64>(n);
        cc[j] = static_cast<f32>(std::cos(ang));
        ss[j] = static_cast<f32>(std::sin(ang));
    }

    std::vector<Vertex3D> strip;
    strip.reserve(static_cast<usize>((n + 1) * 2));
    for (i32 i = 0; i < stacks; ++i) {
        const f64 t0 = (kPi * static_cast<f64>(i)) / static_cast<f64>(stacks);
        const f64 t1 = (kPi * static_cast<f64>(i + 1)) / static_cast<f64>(stacks);
        const f32 ct0 = static_cast<f32>(std::cos(t0));
        const f32 st0 = static_cast<f32>(std::sin(t0));
        const f32 ct1 = static_cast<f32>(std::cos(t1));
        const f32 st1 = static_cast<f32>(std::sin(t1));
        const f32 v0 = (static_cast<f32>(i) * vr) / static_cast<f32>(stacks);
        const f32 v1 = (static_cast<f32>(i + 1) * vr) / static_cast<f32>(stacks);

        strip.clear();
        for (i32 j = 0; j <= n; ++j) {
            const f32 u = (static_cast<f32>(j) * hr) / static_cast<f32>(n);
            const f32 nx0 = st0 * cc[j], ny0 = st0 * ss[j], nz0 = ct0;
            const f32 nx1 = st1 * cc[j], ny1 = st1 * ss[j], nz1 = ct1;
            strip.push_back(Vertex3D{cx + rx * nx0, cy + ry * ny0, cz + rz * nz0,
                                     nx0, ny0, nz0, u, v0, col.r, col.g, col.b, col.a});
            strip.push_back(Vertex3D{cx + rx * nx1, cy + ry * ny1, cz + rz * nz1,
                                     nx1, ny1, nz1, u, v1, col.r, col.g, col.b, col.a});
        }
        push_strip_as_tris(out_tris, strip, recorded);
    }
}

// ---------------------------------------------------------------------------
// Model3DManager (C3D_Model + static VBO caching)
// ---------------------------------------------------------------------------

Model3DManager::~Model3DManager() {
    shutdown();
}

void Model3DManager::release_bucket_gpu(Model3DMeshBucket& bucket) noexcept {
    if (bucket.vbo != 0) {
        const GLuint vbo = bucket.vbo;
        glDeleteBuffers(1, &vbo);
        bucket.vbo = 0;
    }
    if (bucket.vao != 0) {
        const GLuint vao = bucket.vao;
        glDeleteVertexArrays(1, &vao);
        bucket.vao = 0;
    }
    bucket.gpu_uploaded = false;
}

void Model3DManager::shutdown() noexcept {
    for (auto& m : models_) {
        release_bucket_gpu(m.triangles);
        release_bucket_gpu(m.lines);
        release_bucket_gpu(m.points);
    }
    models_.clear();
}

i32 Model3DManager::create() {
    for (usize i = 0; i < models_.size(); ++i) {
        if (!models_[i].exists) {
            models_[i].exists = true;
            models_[i].command_count = 0;
            models_[i].commands.clear();
            models_[i].dirty = true;
            return static_cast<i32>(i);
        }
    }
    Model3DData m{};
    m.exists = true;
    models_.push_back(std::move(m));
    return static_cast<i32>(models_.size() - 1);
}

void Model3DManager::destroy(i32 id) {
    if (!exists(id)) return;
    auto& m = models_[static_cast<usize>(id)];
    release_bucket_gpu(m.triangles);
    release_bucket_gpu(m.lines);
    release_bucket_gpu(m.points);
    m.commands.clear();
    m.triangles = {};
    m.lines = {};
    m.points = {};
    m.exists = false;
    m.dirty = true;
}

void Model3DManager::clear(i32 id) {
    if (!exists(id)) return;
    auto& m = models_[static_cast<usize>(id)];
    m.command_count = 0;
    release_bucket_gpu(m.triangles);
    release_bucket_gpu(m.lines);
    release_bucket_gpu(m.points);
    m.commands.clear();
    m.triangles = {};
    m.lines = {};
    m.points = {};
    m.dirty = true;
}

bool Model3DManager::exists(i32 id) const noexcept {
    return id >= 0 && static_cast<usize>(id) < models_.size() && models_[static_cast<usize>(id)].exists;
}

void Model3DManager::add_command(i32 id, i32 op, const std::array<f64, 10>& params) {
    if (!exists(id)) return;
    auto& m = models_[static_cast<usize>(id)];
    if (m.command_count == std::numeric_limits<i32>::max()) return;
    m.commands.push_back(Model3DCommand{op, 1, params});
    m.command_count = std::max(m.command_count, 0) + 1;
    m.dirty = true;
}

bool Model3DManager::load_from_text(i32 id, std::string_view text) {
    if (!exists(id)) return false;
    clear(id);

    text = text.substr(0, text.find('\x1a')); // Delphi TextFile recognizes DOS EOF.
    i32 version = 0;
    if (!model_integer(read_model_token(text, 32), version) || version != 100) return false;
    end_model_line(text);
    auto& m = models_[static_cast<usize>(id)];
    if (!model_integer(read_model_token(text, 32), m.command_count) || m.command_count < 0) return false;
    end_model_line(text);
    bool ok = true;
    i32 i = 0;
    for (; i < m.command_count && ok; ++i) {
        const auto op = read_model_token(text, 32);
        if (op.empty()) break;
        Model3DCommand cmd{};
        ok = model_integer(op, cmd.op);
        for (auto& p : cmd.p) {
            if (!model_real(read_model_token(text, 64), p)) ok = false;
        }
        end_model_line(text); // I/O errors take effect after all ten fields and ReadLn.
        m.commands.push_back(cmd);
    }
    if (i < m.command_count) {
        // The original preallocates cleared entries. One primitive-begin with kind
        // zero has the same drawing effect as any number of consecutive empty begins.
        Model3DCommand missing{};
        missing.repetitions = m.command_count - i;
        m.commands.push_back(missing);
    }
    return ok;
}

std::string Model3DManager::save_to_text(i32 id) const {
    if (!exists(id)) return {};
    const auto& m = models_[static_cast<usize>(id)];

    std::string out;
    out.reserve(32 + m.commands.size() * 64);
    out += "100\r\n";
    out += std::to_string(m.command_count);
    out += "\r\n";

    static const char decimal = [] {
        try {
            return std::use_facet<std::numpunct<char>>(std::locale("")).decimal_point();
        } catch (const std::runtime_error&) {
            return '.';
        }
    }();
    std::string line;
    line.reserve(160);
    for (const auto& cmd : m.commands) {
        line = std::to_string(cmd.op);
        for (f64 value : cmd.p) {
            line += ' ';
            append_model_real(line, value, decimal);
        }
        line += "\r\n";
        for (i32 i = 0; i < cmd.repetitions; ++i) out += line;
    }
    return out;
}

void Model3DManager::compile_model(Model3DData& model) {
    release_bucket_gpu(model.triangles);
    release_bucket_gpu(model.lines);
    release_bucket_gpu(model.points);
    model.triangles = {};
    model.lines = {};
    model.points = {};

    const ColorRGBA white{255, 255, 255, 255};
    bool in_prim = false;
    i32 prim_kind = 4; // pr_trianglelist
    std::vector<Vertex3D> prim_verts;
    std::vector<u8> prim_exp;

    auto push_bucket_vert = [&](Model3DMeshBucket& b, Vertex3D v, u8 is_explicit) {
        if (model.recorded && is_explicit) v.b |= 1u;
        b.verts.push_back(v);
        b.explicit_color.push_back(is_explicit);
        if (is_explicit) b.has_explicit = true;
        else b.has_uncolored = true;
    };

    auto flush_prim = [&]() {
        if (!in_prim) return;
        in_prim = false;
        const usize n = prim_verts.size();
        if (n == 0) return;

        switch (prim_kind) {
            case 1: // pr_pointlist
                for (usize i = 0; i < n; ++i) {
                    push_bucket_vert(model.points, prim_verts[i], prim_exp[i]);
                }
                break;
            case 2: // pr_linelist
                for (usize i = 0; i + 1 < n; i += 2) {
                    push_bucket_vert(model.lines, prim_verts[i], prim_exp[i]);
                    push_bucket_vert(model.lines, prim_verts[i + 1], prim_exp[i + 1]);
                }
                break;
            case 3: // pr_linestrip
                for (usize i = 0; i + 1 < n; ++i) {
                    push_bucket_vert(model.lines, prim_verts[i], prim_exp[i]);
                    push_bucket_vert(model.lines, prim_verts[i + 1], prim_exp[i + 1]);
                }
                break;
            case 4: // pr_trianglelist
                for (usize i = 0; i + 2 < n; i += 3) {
                    push_bucket_vert(model.triangles, prim_verts[i], prim_exp[i]);
                    push_bucket_vert(model.triangles, prim_verts[i + 1], prim_exp[i + 1]);
                    push_bucket_vert(model.triangles, prim_verts[i + 2], prim_exp[i + 2]);
                }
                break;
            case 5: // pr_trianglestrip
                for (usize i = 0; i + 2 < n; ++i) {
                    if ((i & 1u) == 0u) {
                        push_bucket_vert(model.triangles, prim_verts[i], prim_exp[i]);
                        push_bucket_vert(model.triangles, prim_verts[i + 1], prim_exp[i + 1]);
                        push_bucket_vert(model.triangles, prim_verts[i + 2], prim_exp[i + 2]);
                    } else {
                        if (!model.recorded) push_bucket_vert(model.triangles, prim_verts[i], prim_exp[i]);
                        push_bucket_vert(model.triangles, prim_verts[i + 2], prim_exp[i + 2]);
                        push_bucket_vert(model.triangles, prim_verts[i + 1], prim_exp[i + 1]);
                        if (model.recorded) push_bucket_vert(model.triangles, prim_verts[i], prim_exp[i]);
                    }
                }
                break;
            case 6: // pr_trianglefan
                for (usize i = 1; i + 1 < n; ++i) {
                    if (model.recorded) push_bucket_vert(model.triangles, prim_verts[0], prim_exp[0]);
                    push_bucket_vert(model.triangles, prim_verts[i], prim_exp[i]);
                    push_bucket_vert(model.triangles, prim_verts[i + 1], prim_exp[i + 1]);
                    if (!model.recorded) push_bucket_vert(model.triangles, prim_verts[0], prim_exp[0]);
                }
                break;
            default:
                break;
        }
        prim_verts.clear();
        prim_exp.clear();
    };

    auto append_shape_tris = [&](const std::vector<Vertex3D>& shape_tris) {
        if (shape_tris.empty()) return;
        model.triangles.verts.insert(model.triangles.verts.end(), shape_tris.begin(), shape_tris.end());
        model.triangles.explicit_color.insert(model.triangles.explicit_color.end(), shape_tris.size(), 0u);
        model.triangles.has_uncolored = true;
    };

    std::vector<Vertex3D> scratch;
    for (const auto& cmd : model.commands) {
        const auto& p = cmd.p;
        switch (cmd.op) {
            case 0: // primitive_begin(kind)
                if (in_prim) flush_prim();
                in_prim = true;
                prim_kind = static_cast<i32>(delphi_round(p[0]));
                prim_verts.clear();
                prim_exp.clear();
                break;
            case 1: // primitive_end()
                flush_prim();
                break;
            case 2: // vertex(x, y, z)
                if (in_prim) {
                    prim_verts.push_back(Vertex3D{
                        static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255});
                    prim_exp.push_back(0u);
                }
                break;
            case 3: // vertex_color(x, y, z, col, alpha)
                if (in_prim) {
                    const ColorRGBA c = make_rgba_u32(static_cast<u32>(delphi_round(p[3])), p[4]);
                    prim_verts.push_back(Vertex3D{
                        static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, c.r, c.g, c.b, c.a});
                    prim_exp.push_back(1u);
                }
                break;
            case 4: // vertex_texture(x, y, z, u, v)
                if (in_prim) {
                    prim_verts.push_back(Vertex3D{
                        static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                        0.0f, 0.0f, 0.0f, static_cast<f32>(p[3]), static_cast<f32>(p[4]), 255, 255, 255, 255});
                    prim_exp.push_back(0u);
                }
                break;
            case 5: // vertex_texture_color(x, y, z, u, v, col, alpha)
                if (in_prim) {
                    const ColorRGBA c = make_rgba_u32(static_cast<u32>(delphi_round(p[5])), p[6]);
                    prim_verts.push_back(Vertex3D{
                        static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                        0.0f, 0.0f, 0.0f, static_cast<f32>(p[3]), static_cast<f32>(p[4]), c.r, c.g, c.b, c.a});
                    prim_exp.push_back(1u);
                }
                break;
            case 6: // vertex_normal(x, y, z, nx, ny, nz)
                if (in_prim) {
                    prim_verts.push_back(Vertex3D{
                        static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                        static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                        0.0f, 0.0f, 255, 255, 255, 255});
                    prim_exp.push_back(0u);
                }
                break;
            case 7: // vertex_normal_color(x, y, z, nx, ny, nz, col, alpha)
                if (in_prim) {
                    const ColorRGBA c = make_rgba_u32(static_cast<u32>(delphi_round(p[6])), p[7]);
                    prim_verts.push_back(Vertex3D{
                        static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                        static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                        0.0f, 0.0f, c.r, c.g, c.b, c.a});
                    prim_exp.push_back(1u);
                }
                break;
            case 8: // vertex_normal_texture(x, y, z, nx, ny, nz, u, v)
                if (in_prim) {
                    prim_verts.push_back(Vertex3D{
                        static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                        static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                        static_cast<f32>(p[6]), static_cast<f32>(p[7]), 255, 255, 255, 255});
                    prim_exp.push_back(0u);
                }
                break;
            case 9: // vertex_normal_texture_color(x, y, z, nx, ny, nz, u, v, col, alpha)
                if (in_prim) {
                    const ColorRGBA c = make_rgba_u32(static_cast<u32>(delphi_round(p[8])), p[9]);
                    prim_verts.push_back(Vertex3D{
                        static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                        static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                        static_cast<f32>(p[6]), static_cast<f32>(p[7]), c.r, c.g, c.b, c.a});
                    prim_exp.push_back(1u);
                }
                break;
            case 10: // block(x1,y1,z1, x2,y2,z2, hr, vr)
                scratch.clear();
                append_d3d_block(scratch,
                                 static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                                 static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                                 static_cast<f32>(p[6]), static_cast<f32>(p[7]), white, model.recorded);
                append_shape_tris(scratch);
                break;
            case 11: // cylinder(x1,y1,z1, x2,y2,z2, hr, vr, closed, steps)
                scratch.clear();
                append_d3d_cylinder(scratch,
                                    static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                                    static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                                    static_cast<f32>(p[6]), static_cast<f32>(p[7]),
                                    p[8] >= 0.5, static_cast<i32>(delphi_round(p[9])), white, model.recorded);
                append_shape_tris(scratch);
                break;
            case 12: // cone(x1,y1,z1, x2,y2,z2, hr, vr, closed, steps)
                scratch.clear();
                append_d3d_cone(scratch,
                                static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                                static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                                static_cast<f32>(p[6]), static_cast<f32>(p[7]),
                                p[8] >= 0.5, static_cast<i32>(delphi_round(p[9])), white, model.recorded);
                append_shape_tris(scratch);
                break;
            case 13: // ellipsoid(x1,y1,z1, x2,y2,z2, hr, vr, steps)
                scratch.clear();
                append_d3d_ellipsoid(scratch,
                                     static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                                     static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                                     static_cast<f32>(p[6]), static_cast<f32>(p[7]),
                                     static_cast<i32>(delphi_round(p[8])), white, model.recorded);
                append_shape_tris(scratch);
                break;
            case 14: // wall(x1,y1,z1, x2,y2,z2, hr, vr)
                scratch.clear();
                append_d3d_wall(scratch,
                                static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                                static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                                static_cast<f32>(p[6]), static_cast<f32>(p[7]), white, model.recorded);
                append_shape_tris(scratch);
                break;
            case 15: // floor(x1,y1,z1, x2,y2,z2, hr, vr)
                scratch.clear();
                append_d3d_floor(scratch,
                                 static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2]),
                                 static_cast<f32>(p[3]), static_cast<f32>(p[4]), static_cast<f32>(p[5]),
                                 static_cast<f32>(p[6]), static_cast<f32>(p[7]), white, model.recorded);
                append_shape_tris(scratch);
                break;
            default:
                break;
        }
    }
    if (in_prim) {
        flush_prim();
    }

    upload_bucket_if_static(model.triangles);
    upload_bucket_if_static(model.lines);
    upload_bucket_if_static(model.points);
    model.dirty = false;
}

void Model3DManager::upload_bucket_if_static(Model3DMeshBucket& bucket) {
    if (bucket.verts.empty()) return;
    // Mixed explicit + uncolored vertices require per-draw color substitution; keep CPU-side for that rare case.
    if (bucket.has_explicit && bucket.has_uncolored) {
        return;
    }

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(bucket.verts.size() * sizeof(Vertex3D)),
                 bucket.verts.data(),
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex3D),
                          reinterpret_cast<const void*>(offsetof(Vertex3D, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex3D),
                          reinterpret_cast<const void*>(offsetof(Vertex3D, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex3D),
                          reinterpret_cast<const void*>(offsetof(Vertex3D, u)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex3D),
                          reinterpret_cast<const void*>(offsetof(Vertex3D, r)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    bucket.vao = vao;
    bucket.vbo = vbo;
    bucket.gpu_uploaded = true;
}

Model3DData* Model3DManager::prepare_for_draw(i32 id, bool recorded) {
    if (!exists(id)) return nullptr;
    auto& m = models_[static_cast<usize>(id)];
    if (m.dirty || m.recorded != recorded) {
        m.recorded = recorded;
        compile_model(m);
    }
    return &m;
}

} // namespace gm
