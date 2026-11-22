#pragma once

#include "base/types.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
struct SDL_GLContextState;

namespace gm {

struct WindowConfig {
    std::string title = "gmrunner";
    i32 width = 800;
    i32 height = 608;
    bool resizable = true;
    bool fullscreen = false;
    bool borderless = false;
    bool vsync = true;
    bool show_cursor = true;
};

struct InputState {
    std::array<bool, 256> key_down{};
    std::array<bool, 256> key_pressed{};
    std::array<bool, 256> key_released{};
    u8 last_key = 0;
    u8 current_key = 0;
    std::string last_char;
    std::string keyboard_string;

    f32 mouse_x = 0.0f;
    f32 mouse_y = 0.0f;
    std::array<bool, 8> mouse_down{};     // 1=left, 2=right, 3=middle, 4=x1, 5=x2
    std::array<bool, 8> mouse_pressed{};
    std::array<bool, 8> mouse_released{};
    bool mouse_wheel_up = false;
    bool mouse_wheel_down = false;

    std::vector<std::string> dropped_files;

    void begin_frame();
    void clear_all();
};

struct WindowEvents {
    bool quit_requested = false;
    bool resized = false;
    bool focus_gained = false;
    bool focus_lost = false;
    bool entered_background = false;
    bool entered_foreground = false;
    i32 new_width = 0;
    i32 new_height = 0;
};

class PlatformWindow : public NonCopyable {
public:
    PlatformWindow() = default;
    ~PlatformWindow();

    [[nodiscard]] Result<void> create(const WindowConfig& config);
    void destroy() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return window_ != nullptr; }

    WindowEvents poll_events();
    WindowEvents poll_events_accumulate();
    void swap_buffers();

    void set_title(std::string_view title);
    [[nodiscard]] std::string_view title() const noexcept;
    void set_size(i32 width, i32 height);
    void set_position(i32 x, i32 y);
    void center();
    void set_fullscreen(bool fullscreen);
    void set_vsync(bool enabled);
    void set_cursor_visible(bool visible);
    void set_borderless(bool borderless);
    [[nodiscard]] bool is_borderless() const noexcept { return borderless_; }
    void set_stay_on_top(bool on_top);
    [[nodiscard]] bool is_stay_on_top() const noexcept { return stay_on_top_; }
    void set_resizable(bool resizable);
    [[nodiscard]] bool is_resizable() const noexcept { return resizable_; }
    void set_visible(bool visible);
    [[nodiscard]] bool is_visible() const noexcept { return visible_; }
    [[nodiscard]] bool is_maximized() const noexcept;
    [[nodiscard]] bool is_minimized() const noexcept;
    void maximize();
    void minimize();
    void restore();
    void raise_focus();
    void set_mouse_position(i32 x, i32 y);
    [[nodiscard]] static i32 display_mouse_x() noexcept;
    [[nodiscard]] static i32 display_mouse_y() noexcept;
    static void set_display_mouse_position(i32 x, i32 y) noexcept;
    [[nodiscard]] u64 native_window_handle() const noexcept;

    // Legacy GM8 Joystick API (1-based id: 1 or 2) backed by SDL3 joysticks.
    [[nodiscard]] static bool joystick_exists(i32 id) noexcept;
    [[nodiscard]] static std::string joystick_name(i32 id);
    [[nodiscard]] static i32 joystick_axes(i32 id) noexcept;
    [[nodiscard]] static i32 joystick_buttons(i32 id) noexcept;
    [[nodiscard]] static bool joystick_has_pov(i32 id) noexcept;
    [[nodiscard]] static i32 joystick_direction(i32 id) noexcept;
    [[nodiscard]] static bool joystick_check_button(i32 id, i32 button) noexcept;
    [[nodiscard]] static f64 joystick_axis(i32 id, i32 axis_index) noexcept;
    [[nodiscard]] static f64 joystick_pov(i32 id) noexcept;

    [[nodiscard]] i32 x() const noexcept;
    [[nodiscard]] i32 y() const noexcept;
    [[nodiscard]] i32 width() const noexcept;
    [[nodiscard]] i32 height() const noexcept;
    [[nodiscard]] i32 drawable_width() const noexcept;
    [[nodiscard]] i32 drawable_height() const noexcept;
    [[nodiscard]] i32 display_width() const noexcept;
    [[nodiscard]] i32 display_height() const noexcept;
    [[nodiscard]] static bool keyboard_check_direct(i32 key) noexcept;
    [[nodiscard]] static bool numlock_on() noexcept;
    [[nodiscard]] static bool capslock_on() noexcept;
    [[nodiscard]] static bool scrolllock_on() noexcept;
    [[nodiscard]] bool is_fullscreen() const noexcept { return fullscreen_; }
    [[nodiscard]] bool has_focus() const noexcept { return has_focus_; }

    [[nodiscard]] const InputState& input() const noexcept { return input_; }
    [[nodiscard]] InputState& input_mut() noexcept { return input_; }

    [[nodiscard]] static void* gl_get_proc_address(const char* proc_name) noexcept;

    // Platform dialogs, URL & clipboard utilities.
    void show_message(std::string_view title, std::string_view message) const;
    [[nodiscard]] bool show_question(std::string_view title, std::string_view message) const;
    [[nodiscard]] static bool open_url(std::string_view url);
    [[nodiscard]] static std::string clipboard_get_text();
    static void clipboard_set_text(std::string_view text);

private:
    WindowEvents poll_events_impl(bool clear_edges);
    static u8 sdl_scancode_to_gm_vk(i32 scancode) noexcept;
    static u8 sdl_mouse_button_to_gm(u8 button) noexcept;

    SDL_Window* window_ = nullptr;
    SDL_GLContextState* gl_context_ = nullptr;
    bool fullscreen_ = false;
    bool borderless_ = false;
    bool stay_on_top_ = false;
    bool resizable_ = true;
    bool visible_ = true;
    bool has_focus_ = true;
    InputState input_{};
};

} // namespace gm
