#include "graphics/touch_overlay.h"

#include "graphics/gl_headers.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace gm {

namespace {

constexpr f32 kTwoPi = 6.28318530717958647692f;

bool hit_button(const f32 x, const f32 y, const f32 cx, const f32 cy, const f32 rx, const f32 ry, const bool circle) noexcept {
    if (rx <= 0.0f || ry <= 0.0f) return false;
    const f32 dx = (x - cx) / rx;
    const f32 dy = (y - cy) / ry;
    if (circle) {
        return (dx * dx + dy * dy) <= 1.15f; // Slight touch slop for thumb comfort
    }
    return std::fabs(dx) <= 1.1f && std::fabs(dy) <= 1.1f;
}

} // namespace

TouchOverlay::~TouchOverlay() {
    shutdown();
}

Result<void> TouchOverlay::init(GlGraphicsDevice& device) {
    shutdown();
    device_ = &device;

    const bool is_es = device.caps().is_gles;
    const std::string header = is_es
        ? "#version 300 es\nprecision mediump float;\n"
        : "#version 330 core\n";

    const std::string vs_src = header + R"(
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_col;
out vec4 v_col;
void main() {
    v_col = a_col;
    gl_Position = vec4(a_pos.x * 2.0 - 1.0, 1.0 - a_pos.y * 2.0, 0.0, 1.0);
}
)";

    const std::string fs_src = header + R"(
in vec4 v_col;
out vec4 o_color;
void main() {
    o_color = v_col;
}
)";

    auto sh_res = device_->create_shader(vs_src, fs_src);
    if (!sh_res) return sh_res.error();
    shader_ = sh_res.value();

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, r)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    rebuild_layout(last_aspect_);
    initialized_ = true;
    return Result<void>::success();
}

void TouchOverlay::shutdown() noexcept {
    if (!initialized_) return;
    if (device_ && shader_.valid()) {
        device_->destroy_shader(shader_);
        shader_ = {};
    }
    if (vbo_ != 0) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_ != 0) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    verts_.clear();
    initialized_ = false;
}

Result<void> TouchOverlay::reload_gpu_resources() {
    if (!device_) return Error{"TouchOverlay::reload_gpu_resources called without device"};
    shader_ = {};
    vao_ = 0;
    vbo_ = 0;
    initialized_ = false;
    return init(*device_);
}

void TouchOverlay::set_enabled(bool enabled) noexcept {
    enabled_ = enabled;
}

void TouchOverlay::rebuild_layout(f32 aspect) noexcept {
    aspect = std::clamp(aspect, 0.45f, 3.0f);
    last_aspect_ = aspect;

    // Maintain circular geometry regardless of screen aspect ratio (rx = ry / aspect).
    const f32 unit_y = (aspect < 1.0f) ? 0.085f : 0.145f;
    const f32 unit_x = unit_y / aspect;

    // Bottom-left 8-way D-Pad.
    dpad_ry_ = unit_y * 1.15f;
    dpad_rx_ = dpad_ry_ / aspect;
    dpad_cx_ = std::max(dpad_rx_ + 0.035f, 0.13f);
    dpad_cy_ = 1.0f - std::max(dpad_ry_ + 0.055f, 0.22f);

    // Bottom-right 4-button action diamond (GM8 standard fangame/action layout).
    const f32 btn_ry = unit_y * 0.46f;
    const f32 btn_rx = btn_ry / aspect;
    const f32 cluster_cx = 1.0f - std::max(btn_rx * 2.5f + 0.04f, 0.15f);
    const f32 cluster_cy = dpad_cy_;
    const f32 spread_x = btn_rx * 1.38f;
    const f32 spread_y = btn_ry * 1.38f;

    // 0: Jump (Shift - vk_lshift=160 + vk_shift=16)
    buttons_[0] = ButtonDef{cluster_cx - spread_x, cluster_cy + spread_y * 0.30f, btn_rx, btn_ry, 160, 16, '^', true, false};
    // 1: Shoot ('Z' = 90)
    buttons_[1] = ButtonDef{cluster_cx + spread_x, cluster_cy + spread_y * 0.15f, btn_rx, btn_ry, 'Z', 0, 'Z', true, false};
    // 2: Action ('X' = 88)
    buttons_[2] = ButtonDef{cluster_cx - spread_x * 0.15f, cluster_cy - spread_y, btn_rx, btn_ry, 'X', 0, 'X', true, false};
    // 3: Space / Alt (vk_space = 32)
    buttons_[3] = ButtonDef{cluster_cx + spread_x * 0.15f, cluster_cy + spread_y, btn_rx * 0.88f, btn_ry * 0.88f, 32, 0, '_', true, false};

    // Top-left utility pills: R (restart), Enter, Esc.
    const f32 util_ry = unit_y * 0.27f;
    const f32 util_rx = util_ry * 1.55f / aspect;
    const f32 top_y = 0.055f + util_ry * 0.35f;
    buttons_[4] = ButtonDef{0.03f + util_rx, top_y, util_rx, util_ry, 'R', 0, 'R', false, false};
    buttons_[5] = ButtonDef{0.03f + util_rx * 3.4f, top_y, util_rx, util_ry, 13, 0, 'E', false, false};
    buttons_[6] = ButtonDef{0.03f + util_rx * 5.8f, top_y, util_rx, util_ry, 27, 0, 'Q', false, false};

    // Top-right toggle pill to hide/show controls.
    toggle_btn_ = ButtonDef{1.0f - 0.03f - util_rx, top_y, util_rx, util_ry, 0, 0, 'T', false, false};
    (void)unit_x;
}

void TouchOverlay::release_all_virtual_keys(InputState& input) {
    for (usize vk = 0; vk < 256; ++vk) {
        if (active_vkeys_[vk]) {
            active_vkeys_[vk] = false;
            if (input.key_down[vk]) {
                input.key_down[vk] = false;
                input.key_released[vk] = true;
            }
        }
    }
}

void TouchOverlay::update(PlatformWindow& window) {
    InputState& input = window.input_mut();
    if (!enabled_) {
        // Auto-enable when touch contacts are detected on mobile browsers or Android.
        if (PlatformWindow::is_mobile_touch_platform() || input.touch_seen) {
            enabled_ = true;
        } else {
            return;
        }
    }

    const i32 dw = std::max(1, window.drawable_width());
    const i32 dh = std::max(1, window.drawable_height());
    rebuild_layout(static_cast<f32>(dw) / static_cast<f32>(dh));

    std::array<bool, 256> next_vkeys{};
    dpad_dir_.fill(false);
    for (auto& b : buttons_) b.pressed = false;
    toggle_btn_.pressed = false;

    const TouchFinger* mouse_finger = nullptr;
    std::unordered_set<i64> cur_finger_ids;
    cur_finger_ids.reserve(input.touches.size());

    for (const auto& f : input.touches) {
        cur_finger_ids.insert(f.finger_id);
        const bool is_new_touch = !prev_finger_ids_.contains(f.finger_id);

        // Top-right toggle button is always interactive.
        if (hit_button(f.x, f.y, toggle_btn_.cx, toggle_btn_.cy, toggle_btn_.rx, toggle_btn_.ry, false)) {
            toggle_btn_.pressed = true;
            if (is_new_touch) {
                controls_visible_ = !controls_visible_;
            }
            continue;
        }

        if (!controls_visible_) {
            if (!mouse_finger) mouse_finger = &f;
            continue;
        }

        // Check D-Pad zone (generous 1.35x capture radius around center).
        const f32 dx = (f.x - dpad_cx_) / std::max(0.001f, dpad_rx_);
        const f32 dy = (f.y - dpad_cy_) / std::max(0.001f, dpad_ry_);
        const f32 dist_sq = dx * dx + dy * dy;
        if (dist_sq <= 1.35f * 1.35f) {
            if (dist_sq >= 0.18f * 0.18f) {
                if (dx < -0.32f) { dpad_dir_[0] = true; next_vkeys[37] = true; } // vk_left
                if (dy < -0.32f) { dpad_dir_[1] = true; next_vkeys[38] = true; } // vk_up
                if (dx >  0.32f) { dpad_dir_[2] = true; next_vkeys[39] = true; } // vk_right
                if (dy >  0.32f) { dpad_dir_[3] = true; next_vkeys[40] = true; } // vk_down
            }
            continue;
        }

        // Check action & utility buttons.
        bool claimed = false;
        for (auto& b : buttons_) {
            if (hit_button(f.x, f.y, b.cx, b.cy, b.rx, b.ry, b.is_circle)) {
                b.pressed = true;
                if (b.vk_primary != 0) next_vkeys[b.vk_primary] = true;
                if (b.vk_secondary != 0) next_vkeys[b.vk_secondary] = true;
                claimed = true;
            }
        }
        if (!claimed && !mouse_finger) {
            mouse_finger = &f;
        }
    }

    prev_finger_ids_ = std::move(cur_finger_ids);

    // Emit clean synthetic keyboard press/hold/release edges without clobbering physical keys.
    for (usize vk = 1; vk < 256; ++vk) {
        const bool was = active_vkeys_[vk];
        const bool now = next_vkeys[vk];
        if (now && !was) {
            if (!input.key_down[vk]) input.key_pressed[vk] = true;
            input.key_down[vk] = true;
            input.last_key = static_cast<u8>(vk);
            input.current_key = static_cast<u8>(vk);
        } else if (now && was) {
            input.key_down[vk] = true;
        } else if (!now && was) {
            if (input.key_down[vk]) {
                input.key_down[vk] = false;
                input.key_released[vk] = true;
            }
            if (input.current_key == vk) input.current_key = 0;
        }
        active_vkeys_[vk] = now;
    }

    // Forward unclaimed screen touches (or all touches when overlay is hidden) to left mouse button.
    if (mouse_finger) {
        input.mouse_x = mouse_finger->x * static_cast<f32>(window.width());
        input.mouse_y = mouse_finger->y * static_cast<f32>(window.height());
        if (!touch_mouse_active_) {
            if (!input.mouse_down[1]) input.mouse_pressed[1] = true;
            input.mouse_down[1] = true;
            touch_mouse_active_ = true;
        }
    } else if (touch_mouse_active_) {
        if (input.mouse_down[1]) {
            input.mouse_down[1] = false;
            input.mouse_released[1] = true;
        }
        touch_mouse_active_ = false;
    }
}

void TouchOverlay::push_tri(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                            f32 r, f32 g, f32 b, f32 a) {
    verts_.push_back(Vertex{x0, y0, r, g, b, a});
    verts_.push_back(Vertex{x1, y1, r, g, b, a});
    verts_.push_back(Vertex{x2, y2, r, g, b, a});
}

void TouchOverlay::push_rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 r, f32 g, f32 b, f32 a) {
    push_tri(x0, y0, x1, y0, x1, y1, r, g, b, a);
    push_tri(x0, y0, x1, y1, x0, y1, r, g, b, a);
}

void TouchOverlay::push_circle(f32 cx, f32 cy, f32 rx, f32 ry, f32 r, f32 g, f32 b, f32 a, i32 segments) {
    for (i32 i = 0; i < segments; ++i) {
        const f32 a0 = kTwoPi * static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(i + 1) / static_cast<f32>(segments);
        push_tri(cx, cy,
                 cx + std::cos(a0) * rx, cy + std::sin(a0) * ry,
                 cx + std::cos(a1) * rx, cy + std::sin(a1) * ry,
                 r, g, b, a);
    }
}

void TouchOverlay::push_ring(f32 cx, f32 cy, f32 rx_in, f32 ry_in, f32 rx_out, f32 ry_out,
                             f32 r, f32 g, f32 b, f32 a, i32 segments) {
    for (i32 i = 0; i < segments; ++i) {
        const f32 a0 = kTwoPi * static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(i + 1) / static_cast<f32>(segments);
        const f32 c0 = std::cos(a0), s0 = std::sin(a0);
        const f32 c1 = std::cos(a1), s1 = std::sin(a1);
        const f32 ix0 = cx + c0 * rx_in, iy0 = cy + s0 * ry_in;
        const f32 ox0 = cx + c0 * rx_out, oy0 = cy + s0 * ry_out;
        const f32 ix1 = cx + c1 * rx_in, iy1 = cy + s1 * ry_in;
        const f32 ox1 = cx + c1 * rx_out, oy1 = cy + s1 * ry_out;
        push_tri(ix0, iy0, ox0, oy0, ox1, oy1, r, g, b, a);
        push_tri(ix0, iy0, ox1, oy1, ix1, iy1, r, g, b, a);
    }
}

void TouchOverlay::push_glyph(char ch, f32 cx, f32 cy, f32 sx, f32 sy, f32 r, f32 g, f32 b, f32 a) {
    const f32 t_x = sx * 0.24f;
    const f32 t_y = sy * 0.24f;
    switch (ch) {
        case '^': // Jump upward chevron
            push_tri(cx, cy - sy * 0.72f, cx + sx * 0.72f, cy + sy * 0.35f, cx - sx * 0.72f, cy + sy * 0.35f, r, g, b, a);
            push_rect(cx - sx * 0.55f, cy + sy * 0.48f, cx + sx * 0.55f, cy + sy * 0.74f, r, g, b, a);
            break;
        case 'Z':
            push_rect(cx - sx * 0.55f, cy - sy * 0.65f, cx + sx * 0.55f, cy - sy * 0.65f + t_y, r, g, b, a);
            push_rect(cx - sx * 0.55f, cy + sy * 0.65f - t_y, cx + sx * 0.55f, cy + sy * 0.65f, r, g, b, a);
            push_tri(cx + sx * 0.55f, cy - sy * 0.55f, cx + sx * 0.55f - t_x, cy - sy * 0.55f,
                     cx - sx * 0.55f, cy + sy * 0.55f, r, g, b, a);
            push_tri(cx + sx * 0.55f, cy - sy * 0.55f, cx - sx * 0.55f, cy + sy * 0.55f,
                     cx - sx * 0.55f + t_x, cy + sy * 0.55f, r, g, b, a);
            break;
        case 'X':
            push_tri(cx - sx * 0.55f, cy - sy * 0.60f, cx - sx * 0.55f + t_x * 1.1f, cy - sy * 0.60f,
                     cx + sx * 0.55f, cy + sy * 0.60f, r, g, b, a);
            push_tri(cx - sx * 0.55f, cy - sy * 0.60f, cx + sx * 0.55f, cy + sy * 0.60f,
                     cx + sx * 0.55f - t_x * 1.1f, cy + sy * 0.60f, r, g, b, a);
            push_tri(cx + sx * 0.55f, cy - sy * 0.60f, cx + sx * 0.55f - t_x * 1.1f, cy - sy * 0.60f,
                     cx - sx * 0.55f, cy + sy * 0.60f, r, g, b, a);
            push_tri(cx + sx * 0.55f, cy - sy * 0.60f, cx - sx * 0.55f, cy + sy * 0.60f,
                     cx - sx * 0.55f + t_x * 1.1f, cy + sy * 0.60f, r, g, b, a);
            break;
        case '_': // Spacebar bracket
            push_rect(cx - sx * 0.65f, cy + sy * 0.15f, cx + sx * 0.65f, cy + sy * 0.42f, r, g, b, a);
            push_rect(cx - sx * 0.65f, cy - sy * 0.25f, cx - sx * 0.65f + t_x, cy + sy * 0.42f, r, g, b, a);
            push_rect(cx + sx * 0.65f - t_x, cy - sy * 0.25f, cx + sx * 0.65f, cy + sy * 0.42f, r, g, b, a);
            break;
        case 'R':
            push_rect(cx - sx * 0.45f, cy - sy * 0.60f, cx - sx * 0.45f + t_x, cy + sy * 0.60f, r, g, b, a);
            push_rect(cx - sx * 0.45f, cy - sy * 0.60f, cx + sx * 0.35f, cy - sy * 0.60f + t_y, r, g, b, a);
            push_rect(cx - sx * 0.45f, cy - t_y * 0.5f, cx + sx * 0.35f, cy + t_y * 0.5f, r, g, b, a);
            push_rect(cx + sx * 0.35f - t_x, cy - sy * 0.60f, cx + sx * 0.35f, cy + t_y * 0.5f, r, g, b, a);
            push_tri(cx - sx * 0.05f, cy, cx + sx * 0.15f, cy, cx + sx * 0.45f, cy + sy * 0.60f, r, g, b, a);
            push_tri(cx - sx * 0.05f, cy, cx + sx * 0.45f, cy + sy * 0.60f, cx + sx * 0.22f, cy + sy * 0.60f, r, g, b, a);
            break;
        case 'E': // Enter / Confirm right-arrow
            push_rect(cx - sx * 0.55f, cy - t_y * 0.5f, cx + sx * 0.25f, cy + t_y * 0.5f, r, g, b, a);
            push_tri(cx + sx * 0.15f, cy - sy * 0.50f, cx + sx * 0.65f, cy, cx + sx * 0.15f, cy + sy * 0.50f, r, g, b, a);
            break;
        case 'Q': // Esc / Pause dual bars
            push_rect(cx - sx * 0.45f, cy - sy * 0.55f, cx - sx * 0.12f, cy + sy * 0.55f, r, g, b, a);
            push_rect(cx + sx * 0.12f, cy - sy * 0.55f, cx + sx * 0.45f, cy + sy * 0.55f, r, g, b, a);
            break;
        case 'T': // Toggle icon (3 horizontal menu bars)
            push_rect(cx - sx * 0.55f, cy - sy * 0.52f, cx + sx * 0.55f, cy - sy * 0.52f + t_y * 0.9f, r, g, b, a);
            push_rect(cx - sx * 0.55f, cy - t_y * 0.45f, cx + sx * 0.55f, cy + t_y * 0.45f, r, g, b, a);
            push_rect(cx - sx * 0.55f, cy + sy * 0.52f - t_y * 0.9f, cx + sx * 0.55f, cy + sy * 0.52f, r, g, b, a);
            break;
        default:
            break;
    }
}

void TouchOverlay::render(i32 drawable_w, i32 drawable_h) {
    if (!initialized_ || !enabled_ || !device_ || drawable_w <= 0 || drawable_h <= 0) {
        return;
    }

    rebuild_layout(static_cast<f32>(drawable_w) / static_cast<f32>(drawable_h));
    verts_.clear();

    // Always draw top-right toggle pill.
    {
        const auto& b = toggle_btn_;
        const f32 bg_a = b.pressed ? 0.55f : (controls_visible_ ? 0.28f : 0.22f);
        push_rect(b.cx - b.rx, b.cy - b.ry, b.cx + b.rx, b.cy + b.ry, 0.12f, 0.16f, 0.24f, bg_a);
        push_glyph(b.glyph, b.cx, b.cy, b.rx * 0.45f, b.ry * 0.58f, 0.92f, 0.96f, 1.0f, controls_visible_ ? 0.78f : 0.55f);
    }

    if (controls_visible_) {
        // 1. D-Pad outer base ring & subtle cross arms.
        push_circle(dpad_cx_, dpad_cy_, dpad_rx_, dpad_ry_, 0.08f, 0.10f, 0.15f, 0.24f, 28);
        push_ring(dpad_cx_, dpad_cy_, dpad_rx_ * 0.92f, dpad_ry_ * 0.92f, dpad_rx_, dpad_ry_,
                  0.85f, 0.92f, 1.0f, 0.34f, 28);

        const f32 arm_w = dpad_rx_ * 0.30f;
        const f32 arm_h = dpad_ry_ * 0.30f;
        push_rect(dpad_cx_ - dpad_rx_ * 0.78f, dpad_cy_ - arm_h, dpad_cx_ + dpad_rx_ * 0.78f, dpad_cy_ + arm_h,
                  0.85f, 0.92f, 1.0f, 0.14f);
        push_rect(dpad_cx_ - arm_w, dpad_cy_ - dpad_ry_ * 0.78f, dpad_cx_ + arm_w, dpad_cy_ + dpad_ry_ * 0.78f,
                  0.85f, 0.92f, 1.0f, 0.14f);

        // Directional highlight arrows (Left, Up, Right, Down).
        auto arrow_col = [](bool on, f32& r, f32& g, f32& b, f32& a) {
            if (on) { r = 0.38f; g = 0.85f; b = 1.0f; a = 0.85f; }
            else    { r = 0.92f; g = 0.96f; b = 1.0f; a = 0.42f; }
        };
        f32 ar, ag, ab, aa;
        arrow_col(dpad_dir_[0], ar, ag, ab, aa);
        push_tri(dpad_cx_ - dpad_rx_ * 0.82f, dpad_cy_,
                 dpad_cx_ - dpad_rx_ * 0.42f, dpad_cy_ - dpad_ry_ * 0.24f,
                 dpad_cx_ - dpad_rx_ * 0.42f, dpad_cy_ + dpad_ry_ * 0.24f, ar, ag, ab, aa);

        arrow_col(dpad_dir_[1], ar, ag, ab, aa);
        push_tri(dpad_cx_, dpad_cy_ - dpad_ry_ * 0.82f,
                 dpad_cx_ - dpad_rx_ * 0.24f, dpad_cy_ - dpad_ry_ * 0.42f,
                 dpad_cx_ + dpad_rx_ * 0.24f, dpad_cy_ - dpad_ry_ * 0.42f, ar, ag, ab, aa);

        arrow_col(dpad_dir_[2], ar, ag, ab, aa);
        push_tri(dpad_cx_ + dpad_rx_ * 0.82f, dpad_cy_,
                 dpad_cx_ + dpad_rx_ * 0.42f, dpad_cy_ - dpad_ry_ * 0.24f,
                 dpad_cx_ + dpad_rx_ * 0.42f, dpad_cy_ + dpad_ry_ * 0.24f, ar, ag, ab, aa);

        arrow_col(dpad_dir_[3], ar, ag, ab, aa);
        push_tri(dpad_cx_, dpad_cy_ + dpad_ry_ * 0.82f,
                 dpad_cx_ - dpad_rx_ * 0.24f, dpad_cy_ + dpad_ry_ * 0.42f,
                 dpad_cx_ + dpad_rx_ * 0.24f, dpad_cy_ + dpad_ry_ * 0.42f, ar, ag, ab, aa);

        // 2. Action & utility buttons.
        for (usize i = 0; i < buttons_.size(); ++i) {
            const auto& b = buttons_[i];
            f32 br = 0.14f, bg = 0.18f, bb = 0.26f, ba = b.pressed ? 0.62f : 0.28f;
            if (b.pressed) {
                if (i == 0) { br = 0.20f; bg = 0.68f; bb = 0.92f; }
                else if (i == 1) { br = 0.92f; bg = 0.58f; bb = 0.20f; }
                else { br = 0.35f; bg = 0.75f; bb = 0.55f; }
            }
            if (b.is_circle) {
                push_circle(b.cx, b.cy, b.rx, b.ry, br, bg, bb, ba, 22);
                push_ring(b.cx, b.cy, b.rx * 0.88f, b.ry * 0.88f, b.rx, b.ry,
                          0.92f, 0.96f, 1.0f, b.pressed ? 0.85f : 0.38f, 22);
            } else {
                push_rect(b.cx - b.rx, b.cy - b.ry, b.cx + b.rx, b.cy + b.ry, br, bg, bb, ba);
            }
            push_glyph(b.glyph, b.cx, b.cy, b.rx * 0.52f, b.ry * 0.52f,
                       0.96f, 0.98f, 1.0f, b.pressed ? 0.96f : 0.68f);
        }
    }

    if (verts_.empty()) return;

    device_->bind_framebuffer(FramebufferHandle{});
    device_->set_viewport(ViewportRect{0, 0, drawable_w, drawable_h});
    device_->set_scissor(false);
    device_->set_depth_state(DepthState{false, false, CompareFunc::Always});
    device_->set_cull_mode(CullMode::None);
    device_->set_polygon_mode(2);
    device_->set_color_mask(true, true, true, true);
    device_->set_blend_state(BlendState{true, BlendFactor::SrcAlpha, BlendFactor::InvSrcAlpha,
                                        BlendFactor::One, BlendFactor::InvSrcAlpha,
                                        BlendOp::Add, BlendOp::Add});
    device_->bind_shader(shader_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts_.size() * sizeof(Vertex)),
                 verts_.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size()));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

} // namespace gm
