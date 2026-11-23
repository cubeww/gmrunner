#pragma once

#include "base/types.h"
#include "graphics/gl_graphics_device.h"
#include "loader/game_data.h"
#include "platform/window.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

struct MessageFontSpec {
    std::string name = "MS Sans Serif";
    i32 size = 8;
    u32 color_bgr = 0xFFFFFFu;
    bool bold = false;
    bool italic = false;
    u8 charset = 1; // DEFAULT_CHARSET
};

struct MessageDialogStyle {
    f32 alpha = 1.0f;
    MessageFontSpec text_font{"MS Sans Serif", 8, 0xFFFFFFu, false, false, 1};
    MessageFontSpec button_font{"MS Sans Serif", 8, 0xFFFFFFu, true, false, 1};
    MessageFontSpec input_font{"MS Sans Serif", 8, 0xFFFFFFu, false, false, 1};
    u32 mouse_color_bgr = 0xFFFFFFu;
    u32 input_color_bgr = 0x3C3C3Cu; // 3947580 in TMessageForm DFM
    bool show_caption = false;       // bsNone by default
    std::string caption;
    i32 pos_x = -1;
    i32 pos_y = -1;
    i32 size_w = -1;
    i32 size_h = -1;
};

// Cross-platform self-drawn GameMaker 8 message & input dialog (TMessageForm).
// Supports show_message, show_message_ext, show_question, get_string, get_integer,
// and all message_* customization functions across Desktop, WebAssembly, and Android.
class MessageDialog : public NonCopyable {
public:
    MessageDialog();
    ~MessageDialog();

    [[nodiscard]] Result<void> init(GlGraphicsDevice& device, const GameData& game);
    void shutdown() noexcept;
    [[nodiscard]] Result<void> reload_gpu_resources();

    // GM8 message_* style setters
    void set_background(const Bitmap32& bmp);
    void set_alpha(f64 alpha) noexcept;
    void set_button(const SpriteData& spr);
    void set_text_font(std::string_view name, f64 size, f64 color, i32 style);
    void set_button_font(std::string_view name, f64 size, f64 color, i32 style);
    void set_input_font(std::string_view name, f64 size, f64 color, i32 style);
    void set_charset(i32 type, u8 charset) noexcept;
    void set_mouse_color(u32 color_bgr) noexcept { style_.mouse_color_bgr = color_bgr & 0xFFFFFFu; }
    void set_input_color(u32 color_bgr) noexcept { style_.input_color_bgr = color_bgr & 0xFFFFFFu; }
    void set_caption(bool show, std::string_view caption);
    void set_position(i32 x, i32 y) noexcept {
        style_.pos_x = x;
        style_.pos_y = y;
    }
    void set_size(i32 w, i32 h) noexcept {
        style_.size_w = w;
        style_.size_h = h;
    }

    [[nodiscard]] const MessageDialogStyle& style() const noexcept { return style_; }

    // Begin a modal dialog session (all strings are UTF-8).
    void begin_message(
        std::string_view text_utf8,
        std::string_view but1_utf8,
        std::string_view but2_utf8,
        std::string_view but3_utf8);

    void begin_input(
        std::string_view text_utf8,
        std::string_view default_utf8);

#if defined(__EMSCRIPTEN__)
    void begin_web_editor();
    void poll_web_editor(ModalInputFrame& input);
#endif

    // Advances dialog state for one modal frame. Returns true while the dialog
    // remains open, false once dismissed (button clicked, Enter/Escape, close X).
    [[nodiscard]] bool update(
        const ModalInputFrame& input,
        i32 win_w, i32 win_h,
        i32 drawable_w, i32 drawable_h,
        i32 win_x, i32 win_y,
        f32 display_scale,
        bool mobile_touch,
        u64 now_us);

    // Renders the dialog directly onto the window backbuffer (default FBO),
    // preserving the device's cached GL state.
    void render(
        i32 win_w, i32 win_h,
        i32 drawable_w, i32 drawable_h,
        i32 win_x, i32 win_y,
        f32 display_scale,
        bool mobile_touch,
        u64 now_us);

    [[nodiscard]] i32 result_button() const noexcept { return result_button_; }
    [[nodiscard]] std::string result_text_utf8() const;
    [[nodiscard]] bool consume_text_input_focus_request() noexcept {
        const bool v = request_text_input_;
        request_text_input_ = false;
        return v;
    }

    // Returns the input field bounds in window coordinates (for SDL_SetTextInputArea).
    [[nodiscard]] bool get_input_window_rect(
        i32 win_w, i32 win_h,
        i32 drawable_w, i32 drawable_h,
        i32 win_x, i32 win_y,
        f32 display_scale,
        bool mobile_touch,
        i32& out_x, i32& out_y, i32& out_w, i32& out_h, i32& out_caret_px);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    GlGraphicsDevice* device_ = nullptr;
    bool initialized_ = false;
    MessageDialogStyle style_{};

    // CPU-side image copies (persisted for GL context loss recovery).
    Bitmap32 bg_bitmap_{};
    std::array<Bitmap32, 3> button_bitmaps_{};
    TextureHandle bg_tex_{};
    std::array<TextureHandle, 3> button_tex_{};
    TextureHandle white_tex_{};
    ShaderHandle shader_{};
    u32 vao_ = 0;
    u32 vbo_ = 0;
    i32 loc_proj_ = -1;
    i32 loc_tex_ = -1;

    // Active modal session state
    struct ParsedButton {
        bool visible = false;
        std::string raw_text;
        std::vector<u32> display_cps;
        i32 underline_cp_idx = -1;
        u32 shortcut_upper = 0;
        bool hovered = false;
        bool pressed = false;
    };

    bool active_ = false;
    bool with_input_ = false;
    std::string message_text_;
    std::array<ParsedButton, 3> buttons_{};
    i32 pressed_button_idx_ = -1; // 0..2, or -1 if none
    i32 result_button_ = 0;

    // Caption bar close button & dragging
    bool close_hovered_ = false;
    bool close_pressed_ = false;
    bool dragging_ = false;
    f32 drag_offset_x_ = 0.0f;
    f32 drag_offset_y_ = 0.0f;
    bool has_custom_drag_pos_ = false;
    f32 dragged_x_ = 0.0f;
    f32 dragged_y_ = 0.0f;

    // Input box state (UTF-32 codepoints for clean UTF-8 editing)
    std::vector<u32> input_cps_;
    usize caret_ = 0;
    usize sel_anchor_ = 0;
    f32 input_scroll_x_ = 0.0f;
    bool input_selecting_ = false;
    bool request_text_input_ = false;
    std::string ime_text_;
    i32 ime_start_ = 0;
    i32 ime_length_ = 0;
    u64 caret_reset_us_ = 0;

    void create_default_assets(const GameData& game);
    Result<void> create_gpu_pipeline();
    void destroy_gpu_resources(bool delete_gl_objects) noexcept;
    void upload_bg_texture();
    void upload_button_textures();

    struct RectF {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 w = 0.0f;
        f32 h = 0.0f;
        [[nodiscard]] bool contains(f32 px, f32 py) const noexcept {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    struct Layout {
        f32 scale = 1.0f;        // Physical pixels per logical GM8 dialog pixel
        f32 raster_scale = 1.0f; // Quantized font raster scale
        f32 win_to_phys_x = 1.0f;
        f32 win_to_phys_y = 1.0f;
        // All rects below are in logical dialog-scaled coordinate space rooted at (0, 0) of the drawable.
        RectF outer{};
        RectF title_bar{};
        RectF close_btn{};
        RectF client{};
        RectF text_rect{};
        RectF input_rect{};
        std::array<RectF, 3> btn_rects{};
        std::vector<std::vector<u32>> wrapped_lines;
        f32 text_line_h = 13.0f;
        f32 button_line_h = 13.0f;
        f32 input_line_h = 13.0f;
    };

    Layout compute_layout(
        i32 win_w, i32 win_h,
        i32 drawable_w, i32 drawable_h,
        i32 win_x, i32 win_y,
        f32 display_scale,
        bool mobile_touch);

    // Input editing helpers
    [[nodiscard]] bool has_selection() const noexcept { return caret_ != sel_anchor_; }
    void delete_selection();
    void insert_utf8_at_caret(std::string_view utf8);
    void ensure_caret_visible(const Layout& layout, f32 font_scale);
    [[nodiscard]] usize hit_test_input_caret(const Layout& layout, f32 local_x);
    void trigger_first_visible_button() noexcept;
};

} // namespace gm
