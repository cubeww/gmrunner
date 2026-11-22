#pragma once

#include "base/types.h"
#include "graphics/gl_graphics_device.h"
#include "platform/window.h"

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace gm {

enum class TouchControlKind : u8 {
    Button = 0, // Virtual keyboard button (single/combo key, hold or latch toggle)
    Dpad = 1,   // 8-way directional pad mapped to 4 directional virtual keys
};

enum class TouchControlShape : u8 {
    Circle = 0,
    Rect = 1,
};

struct TouchControl {
    TouchControlKind kind = TouchControlKind::Button;
    TouchControlShape shape = TouchControlShape::Circle;
    f32 cx = 0.5f;         // Normalized [0, 1] screen center X
    f32 cy = 0.5f;         // Normalized [0, 1] screen center Y
    f32 size = 0.067f;     // Radius normalized to min(drawable_w, drawable_h)
    f32 aspect = 1.0f;     // Width/height aspect multiplier for Rect buttons
    u8 vk = 0;             // Primary GM virtual key (Button)
    u8 vk_extra = 0;       // Optional secondary GM virtual key (Button)
    std::array<u8, 4> dpad_vk = {37, 38, 39, 40}; // Left, Up, Right, Down (Dpad)
    bool toggle_mode = false;
    u8 color_idx = 0;      // Accent palette index (0..5)
    std::string label;     // Optional custom label (empty = auto-derived from key)

    // Runtime-only transient state (not persisted)
    bool pressed = false;
    bool latched = false;
    i64 active_finger_id = -1;
    bool has_active_finger = false;
    std::array<bool, 4> dpad_dir{};
};

struct TouchLayout {
    f32 opacity = 1.0f;
    bool snap_to_grid = false;
    std::vector<TouchControl> controls;
};

// Resolution-independent OpenGL 3.3 / GLES 3.0 / WebGL 2.0 on-screen virtual
// touch controller with an integrated cross-platform visual layout editor.
// Rendered directly onto the backbuffer after present_screen() so it never
// interferes with game surfaces or custom shaders.
class TouchOverlay : public NonCopyable {
public:
    static constexpr usize kMaxControls = 48;

    TouchOverlay();
    ~TouchOverlay();

    [[nodiscard]] Result<void> init(GlGraphicsDevice& device);
    void shutdown() noexcept;
    [[nodiscard]] Result<void> reload_gpu_resources();

    void set_enabled(bool enabled, InputState* input = nullptr);
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool key_down(i32 key) const noexcept {
        return enabled_ && !editing_ && key > 0 && key < static_cast<i32>(active_vkeys_.size()) && active_vkeys_[key];
    }
    void set_controls_visible(bool visible) noexcept { controls_visible_ = visible; }
    [[nodiscard]] bool controls_visible() const noexcept { return controls_visible_; }

    void set_editing(bool editing, InputState* input = nullptr);
    [[nodiscard]] bool is_editing() const noexcept { return editing_; }

    // Configures where customized layouts are loaded from and saved to.
    void set_config_path(const std::filesystem::path& path, std::function<void()> on_saved = {});
    [[nodiscard]] const std::filesystem::path& config_path() const noexcept { return config_path_; }
    bool load_layout_from_disk();
    bool save_layout_to_disk();

    [[nodiscard]] std::string export_layout_text() const;
    bool import_layout_text(std::string_view text, bool persist = true);
    void reset_to_default(bool persist = true);
    [[nodiscard]] const TouchLayout& layout() const noexcept { return layout_; }

    // Processes active touch (and desktop mouse) input, updates synthetic keyboard
    // and touch-mouse edges, or drives the interactive layout editor.
    // Returns true when Edit Mode consumed this frame (game simulation should pause).
    bool update(PlatformWindow& window);

    // Draws the translucent touch overlay (or layout editor) onto the default backbuffer.
    void render(i32 drawable_w, i32 drawable_h);

    // Virtual key metadata and layout serialization helpers.
    [[nodiscard]] static std::string vk_short_label(u8 vk);
    [[nodiscard]] static std::string vk_config_name(u8 vk);
    [[nodiscard]] static u8 parse_vk_token(std::string_view token);
    [[nodiscard]] static TouchLayout make_default_layout(f32 aspect = 16.0f / 9.0f);
    [[nodiscard]] static std::string serialize_layout(const TouchLayout& layout);
    [[nodiscard]] static bool deserialize_layout(std::string_view text, TouchLayout& out);

private:
    struct Vertex {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 r = 1.0f;
        f32 g = 1.0f;
        f32 b = 1.0f;
        f32 a = 1.0f;
    };

    struct RectBox {
        f32 x0 = 0.0f;
        f32 y0 = 0.0f;
        f32 x1 = 0.0f;
        f32 y1 = 0.0f;

        [[nodiscard]] f32 cx() const noexcept { return (x0 + x1) * 0.5f; }
        [[nodiscard]] f32 cy() const noexcept { return (y0 + y1) * 0.5f; }
        [[nodiscard]] f32 w() const noexcept { return x1 - x0; }
        [[nodiscard]] f32 h() const noexcept { return y1 - y0; }
        [[nodiscard]] bool contains(f32 x, f32 y, f32 slop = 0.0f) const noexcept {
            return x >= (x0 - slop) && x <= (x1 + slop) && y >= (y0 - slop) && y <= (y1 + slop);
        }
    };

    struct ResolvedGeom {
        f32 cx = 0.5f;
        f32 cy = 0.5f;
        f32 rx = 0.05f;
        f32 ry = 0.05f;
    };

    enum class DragMode : u8 {
        None = 0,
        Move = 1,
        Resize = 2,
    };

    enum class EditorModal : u8 {
        None = 0,
        KeyPicker = 1,
        DpadBind = 2,
    };

    struct UiButton {
        RectBox rect;
        std::string text;
        i32 action_id = 0;
        bool active = false;
        bool accent = false;
        bool danger = false;
        u8 tint_color_idx = 255;
    };

    void update_system_pills(f32 aspect) noexcept;
    [[nodiscard]] ResolvedGeom resolve_control(const TouchControl& ctrl, f32 aspect) const noexcept;
    [[nodiscard]] RectBox resize_handle_box(const TouchControl& ctrl, const ResolvedGeom& g, f32 aspect) const noexcept;
    void build_editor_toolbar(f32 aspect, std::vector<UiButton>& out) const;
    void build_inspector_bar(f32 aspect, std::vector<UiButton>& out) const;
    void build_key_picker_ui(f32 aspect, RectBox& card, RectBox& close_btn,
                             std::vector<UiButton>& tabs, std::vector<UiButton>& keys) const;
    void build_dpad_modal_ui(f32 aspect, RectBox& card, RectBox& close_btn,
                             std::vector<UiButton>& presets, std::vector<UiButton>& dirs) const;

    void enter_edit_mode(InputState* input, bool from_pointer);
    void exit_edit_mode(InputState* input, bool save);
    bool update_play_mode(PlatformWindow& window, f32 aspect);
    void update_edit_mode(PlatformWindow& window, f32 aspect);
    void apply_picked_key(u8 vk);
    void add_new_dpad();

    void push_tri(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, f32 r, f32 g, f32 b, f32 a);
    void push_rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 r, f32 g, f32 b, f32 a);
    void push_rect_outline(f32 x0, f32 y0, f32 x1, f32 y1, f32 tx, f32 ty, f32 r, f32 g, f32 b, f32 a);
    void push_rounded_rect(f32 x0, f32 y0, f32 x1, f32 y1, f32 rad_x, f32 rad_y, f32 r, f32 g, f32 b, f32 a);
    void push_rounded_outline(f32 x0, f32 y0, f32 x1, f32 y1, f32 rad_x, f32 rad_y, f32 tx, f32 ty,
                              f32 r, f32 g, f32 b, f32 a);
    void push_circle(f32 cx, f32 cy, f32 rx, f32 ry, f32 r, f32 g, f32 b, f32 a, i32 segments = 24);
    void push_ring(f32 cx, f32 cy, f32 rx_in, f32 ry_in, f32 rx_out, f32 ry_out,
                   f32 r, f32 g, f32 b, f32 a, i32 segments = 24);
    void push_menu_icon(f32 cx, f32 cy, f32 sx, f32 sy, f32 r, f32 g, f32 b, f32 a);
    void push_text_centered(std::string_view text, f32 cx, f32 cy, f32 max_w, f32 max_h,
                            f32 aspect, f32 r, f32 g, f32 b, f32 a);
    void draw_ui_button(const UiButton& btn, f32 aspect);
    void draw_control(const TouchControl& ctrl, const ResolvedGeom& g, f32 aspect, f32 alpha_scale, bool selected);
    void release_all_virtual_keys(InputState& input);
    static void assert_vkey_with_modifiers(std::array<bool, 256>& keys, u8 vk) noexcept;

    GlGraphicsDevice* device_ = nullptr;
    bool initialized_ = false;
    bool enabled_ = false;
    bool controls_visible_ = true;
    bool editing_ = false;

    ShaderHandle shader_{};
    u32 vao_ = 0;
    u32 vbo_ = 0;
    std::vector<Vertex> verts_;

    f32 last_aspect_ = 16.0f / 9.0f;
    TouchLayout layout_{};
    bool using_default_layout_ = true;
    bool layout_dirty_ = false;

    std::filesystem::path config_path_;
    std::function<void()> on_saved_callback_;

    // Top-right play-mode system pills.
    RectBox toggle_pill_{};
    RectBox edit_pill_{};
    bool toggle_pill_pressed_ = false;
    bool edit_pill_pressed_ = false;

    // Play-mode input tracking.
    std::array<bool, 256> active_vkeys_{};
    std::unordered_set<i64> prev_finger_ids_;
    bool touch_mouse_active_ = false;
    bool play_mouse_captured_ = false;
    bool prev_play_mouse_down_ = false;

    // Edit-mode interactive state.
    i32 selected_idx_ = -1;
    DragMode drag_mode_ = DragMode::None;
    EditorModal modal_ = EditorModal::None;
    i32 picker_tab_ = 0;
    bool pending_add_button_ = false;
    i32 dpad_bind_dir_ = -1; // 0=Left, 1=Up, 2=Right, 3=Down when rebinding a single D-Pad arm
    bool reset_confirm_ = false;
    bool toolbar_at_bottom_ = false;
    bool inspector_at_bottom_locked_ = false;

    bool edit_pointer_down_ = false;
    bool edit_pointer_is_touch_ = false;
    i64 edit_pointer_id_ = -1;
    bool edit_ignore_until_release_ = false;
    f32 drag_offset_x_ = 0.0f;
    f32 drag_offset_y_ = 0.0f;
    f32 resize_start_dist_ = 0.05f;
    f32 resize_start_size_ = 0.067f;
};

} // namespace gm
