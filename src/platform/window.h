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
    void swap_buffers();

    void set_title(std::string_view title);
    void set_size(i32 width, i32 height);
    void set_position(i32 x, i32 y);
    void set_fullscreen(bool fullscreen);
    void set_vsync(bool enabled);
    void set_cursor_visible(bool visible);

    [[nodiscard]] i32 width() const noexcept;
    [[nodiscard]] i32 height() const noexcept;
    [[nodiscard]] i32 drawable_width() const noexcept;
    [[nodiscard]] i32 drawable_height() const noexcept;
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
    static u8 sdl_scancode_to_gm_vk(i32 scancode) noexcept;
    static u8 sdl_mouse_button_to_gm(u8 button) noexcept;

    SDL_Window* window_ = nullptr;
    SDL_GLContextState* gl_context_ = nullptr;
    bool fullscreen_ = false;
    bool has_focus_ = true;
    InputState input_{};
};

} // namespace gm
