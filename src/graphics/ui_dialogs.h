#pragma once

#include "base/types.h"
#include "graphics/gl_graphics_device.h"
#include "graphics/rtf_document.h"
#include "loader/game_data.h"
#include "platform/window.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

struct HighscoreDialogRow {
    std::string name_utf8;
    i64 score = 0;
};

struct HighscoreDialogParams {
    std::array<HighscoreDialogRow, 10> rows{};
    i32 edit_rank = -1; // 0..9 when player qualified for inline name entry, -1 for display-only
    std::optional<Bitmap32> bg_bitmap;
    bool show_border = true;
    u32 col_back_bgr = 0xFFFFFFu;
    u32 col_new_bgr = 0x0000FFu;
    u32 col_other_bgr = 0x000000u;
    std::string font_name = "Arial";
    i32 font_size = 12;
    i32 font_style = 0; // bit0=bold, bit1=italic, bit2=underline, bit3=strikeout
    u8 charset = 1;
    std::string caption_utf8 = "Top Ten Players";
    std::string escape_text_utf8 = "press <Escape> to close";
};

struct MenuDialogParams {
    std::vector<std::string> items_utf8; // split by '|', "-" denotes a separator
    i32 anchor_win_x = 0;
    i32 anchor_win_y = 0;
    u8 charset = 1;
};

struct ColorPickerParams {
    u32 initial_color_bgr = 0;
};

struct SplashDialogParams {
    bool is_image = true;
    Bitmap32 image_bitmap;
    RtfDocument text_doc;
    i32 delay_ms = 0;
    u32 bg_color_bgr = 0x000000u;
    f64 scale = -1.0;
    bool in_main_window = true;
    bool fullscreen = false;
    bool show_border = true;
    std::string caption_utf8;
    i32 size_w = 640;
    i32 size_h = 480;
    i32 pos_x = -1;
    i32 pos_y = -1;
    bool adapt = true;
    bool stop_key = true;
    bool stop_mouse = true;
    bool close_button = true;
};

struct GameInfoDialogParams {
    RtfDocument doc;
    u32 bg_color_bgr = 0xFFFFFFu;
    std::string caption_utf8 = "Game Information";
    i32 left = -1;
    i32 top = -1;
    i32 width = 600;
    i32 height = 400;
    bool border = true;
    bool sizable = true;
    bool mimic = false;
};

// Cross-platform self-drawn GM8 modal dialog suite:
// Highscore table (TScoreForm), Popup menu (TPopupMenu), Color picker (TColorDialog),
// Splash viewer (TSplashForm), and Game Information RTF viewer (TInfoForm/THelpForm).
class GmModalDialogs : public NonCopyable {
public:
    GmModalDialogs();
    ~GmModalDialogs();

    [[nodiscard]] Result<void> init(GlGraphicsDevice& device);
    void shutdown() noexcept;
    [[nodiscard]] Result<void> reload_gpu_resources();

    void begin_highscore(const HighscoreDialogParams& params);
    void begin_menu(const MenuDialogParams& params);
    void begin_color_picker(const ColorPickerParams& params);
    void begin_splash(const SplashDialogParams& params, u64 now_us);
    void begin_game_info(const GameInfoDialogParams& params);

    [[nodiscard]] bool update(
        const ModalInputFrame& input,
        i32 win_w, i32 win_h,
        i32 drawable_w, i32 drawable_h,
        i32 win_x, i32 win_y,
        f32 display_scale,
        bool mobile_touch,
        u64 now_us);

    void render(
        i32 win_w, i32 win_h,
        i32 drawable_w, i32 drawable_h,
        i32 win_x, i32 win_y,
        f32 display_scale,
        bool mobile_touch,
        u64 now_us);

    [[nodiscard]] bool wants_text_input() const noexcept;
    [[nodiscard]] std::string result_highscore_name_utf8() const;
    [[nodiscard]] i32 result_menu_index() const noexcept;
    [[nodiscard]] bool result_color_accepted() const noexcept;
    [[nodiscard]] u32 result_color_bgr() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gm
