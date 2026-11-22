#pragma once

#include "base/types.h"
#include "graphics/gl_graphics_device.h"
#include "platform/window.h"

#include <array>
#include <unordered_set>
#include <vector>

namespace gm {

// Resolution-independent OpenGL 3.3 / GLES 3.0 / WebGL 2.0 on-screen virtual
// touch controller for Android, mobile WebAssembly, and desktop testing (--touch-overlay).
// Rendered directly onto the backbuffer after present_screen() so it never interferes
// with game surfaces or custom shaders.
class TouchOverlay : public NonCopyable {
public:
    TouchOverlay() = default;
    ~TouchOverlay();

    [[nodiscard]] Result<void> init(GlGraphicsDevice& device);
    void shutdown() noexcept;
    [[nodiscard]] Result<void> reload_gpu_resources();

    void set_enabled(bool enabled) noexcept;
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }
    void set_controls_visible(bool visible) noexcept { controls_visible_ = visible; }
    [[nodiscard]] bool controls_visible() const noexcept { return controls_visible_; }

    // Processes active touch contacts, updates synthetic keyboard & touch-mouse input edges.
    void update(PlatformWindow& window);

    // Draws the translucent touch overlay onto the default backbuffer.
    void render(i32 drawable_w, i32 drawable_h);

private:
    struct Vertex {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 r = 1.0f;
        f32 g = 1.0f;
        f32 b = 1.0f;
        f32 a = 1.0f;
    };

    struct ButtonDef {
        f32 cx = 0.0f; // Normalized [0, 1] screen center
        f32 cy = 0.0f;
        f32 rx = 0.0f; // Normalized half-width (or radius in X)
        f32 ry = 0.0f; // Normalized half-height (or radius in Y)
        u8 vk_primary = 0;
        u8 vk_secondary = 0;
        char glyph = 0;
        bool is_circle = true;
        bool pressed = false;
    };

    void rebuild_layout(f32 aspect) noexcept;
    void push_rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 r, f32 g, f32 b, f32 a);
    void push_circle(f32 cx, f32 cy, f32 rx, f32 ry, f32 r, f32 g, f32 b, f32 a, i32 segments = 24);
    void push_ring(f32 cx, f32 cy, f32 rx_in, f32 ry_in, f32 rx_out, f32 ry_out,
                   f32 r, f32 g, f32 b, f32 a, i32 segments = 24);
    void push_tri(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, f32 r, f32 g, f32 b, f32 a);
    void push_glyph(char ch, f32 cx, f32 cy, f32 sx, f32 sy, f32 r, f32 g, f32 b, f32 a);
    void release_all_virtual_keys(InputState& input);

    GlGraphicsDevice* device_ = nullptr;
    bool initialized_ = false;
    bool enabled_ = false;
    bool controls_visible_ = true;

    ShaderHandle shader_{};
    u32 vao_ = 0;
    u32 vbo_ = 0;
    std::vector<Vertex> verts_;

    f32 last_aspect_ = 16.0f / 9.0f;
    f32 dpad_cx_ = 0.14f;
    f32 dpad_cy_ = 0.76f;
    f32 dpad_rx_ = 0.09f;
    f32 dpad_ry_ = 0.16f;
    std::array<bool, 4> dpad_dir_{}; // 0=left, 1=up, 2=right, 3=down

    std::array<ButtonDef, 7> buttons_{}; // Jump, Z, X, Space, R, Enter, Esc
    ButtonDef toggle_btn_{};

    std::array<bool, 256> active_vkeys_{};
    std::unordered_set<i64> prev_finger_ids_;
    bool touch_mouse_active_ = false;
};

} // namespace gm
