#include "platform/window.h"

#include "base/log.h"

#include <SDL3/SDL.h>

namespace gm {

void InputState::begin_frame() {
    key_pressed.fill(false);
    key_released.fill(false);
    mouse_pressed.fill(false);
    mouse_released.fill(false);
    mouse_wheel_up = false;
    mouse_wheel_down = false;
}

void InputState::clear_all() {
    key_down.fill(false);
    key_pressed.fill(false);
    key_released.fill(false);
    mouse_down.fill(false);
    mouse_pressed.fill(false);
    mouse_released.fill(false);
    mouse_wheel_up = false;
    mouse_wheel_down = false;
    last_key = 0;
    current_key = 0;
    last_char.clear();
}

PlatformWindow::~PlatformWindow() {
    destroy();
}

Result<void> PlatformWindow::create(const WindowConfig& config) {
    destroy();

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
#endif

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (config.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    if (config.borderless) flags |= SDL_WINDOW_BORDERLESS;

    window_ = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (!window_) {
        return Error{std::string("SDL_CreateWindow failed: ") + SDL_GetError()};
    }

    gl_context_ = SDL_GL_CreateContext(window_);
    if (!gl_context_) {
        std::string err = SDL_GetError();
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return Error{"SDL_GL_CreateContext failed: " + err};
    }

    if (!SDL_GL_MakeCurrent(window_, gl_context_)) {
        std::string err = SDL_GetError();
        destroy();
        return Error{"SDL_GL_MakeCurrent failed: " + err};
    }

    fullscreen_ = config.fullscreen;
    set_vsync(config.vsync);
    set_cursor_visible(config.show_cursor);

    GM_LOG_INFO("platform", "Created window %dx%d (drawable %dx%d)",
                width(), height(), drawable_width(), drawable_height());
    return Result<void>::success();
}

void PlatformWindow::destroy() noexcept {
    if (gl_context_) {
        SDL_GL_DestroyContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

u8 PlatformWindow::sdl_scancode_to_gm_vk(i32 scancode) noexcept {
    const auto sc = static_cast<SDL_Scancode>(scancode);
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return static_cast<u8>('A' + (sc - SDL_SCANCODE_A));
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        return static_cast<u8>('1' + (sc - SDL_SCANCODE_1));
    }
    if (sc == SDL_SCANCODE_0) return static_cast<u8>('0');
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        return static_cast<u8>(112 + (sc - SDL_SCANCODE_F1));
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {
        return static_cast<u8>(97 + (sc - SDL_SCANCODE_KP_1));
    }

    switch (sc) {
        case SDL_SCANCODE_BACKSPACE:    return 8;
        case SDL_SCANCODE_TAB:          return 9;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:     return 13;
        case SDL_SCANCODE_PAUSE:        return 19;
        case SDL_SCANCODE_CAPSLOCK:     return 20;
        case SDL_SCANCODE_ESCAPE:       return 27;
        case SDL_SCANCODE_SPACE:        return 32;
        case SDL_SCANCODE_PAGEUP:       return 33;
        case SDL_SCANCODE_PAGEDOWN:     return 34;
        case SDL_SCANCODE_END:          return 35;
        case SDL_SCANCODE_HOME:         return 36;
        case SDL_SCANCODE_LEFT:         return 37;
        case SDL_SCANCODE_UP:           return 38;
        case SDL_SCANCODE_RIGHT:        return 39;
        case SDL_SCANCODE_DOWN:         return 40;
        case SDL_SCANCODE_PRINTSCREEN:  return 44;
        case SDL_SCANCODE_INSERT:       return 45;
        case SDL_SCANCODE_DELETE:       return 46;
        case SDL_SCANCODE_KP_0:         return 96;
        case SDL_SCANCODE_KP_MULTIPLY:  return 106;
        case SDL_SCANCODE_KP_PLUS:      return 107;
        case SDL_SCANCODE_KP_MINUS:     return 109;
        case SDL_SCANCODE_KP_PERIOD:    return 110;
        case SDL_SCANCODE_KP_DIVIDE:    return 111;
        case SDL_SCANCODE_LSHIFT:       return 160;
        case SDL_SCANCODE_RSHIFT:       return 161;
        case SDL_SCANCODE_LCTRL:        return 162;
        case SDL_SCANCODE_RCTRL:        return 163;
        case SDL_SCANCODE_LALT:         return 164;
        case SDL_SCANCODE_RALT:         return 165;
        case SDL_SCANCODE_SEMICOLON:    return 186;
        case SDL_SCANCODE_EQUALS:       return 187;
        case SDL_SCANCODE_COMMA:        return 188;
        case SDL_SCANCODE_MINUS:        return 189;
        case SDL_SCANCODE_PERIOD:       return 190;
        case SDL_SCANCODE_SLASH:        return 191;
        case SDL_SCANCODE_GRAVE:        return 192;
        case SDL_SCANCODE_LEFTBRACKET:  return 219;
        case SDL_SCANCODE_BACKSLASH:    return 220;
        case SDL_SCANCODE_RIGHTBRACKET: return 221;
        case SDL_SCANCODE_APOSTROPHE:   return 222;
        default:                        return 0;
    }
}

u8 PlatformWindow::sdl_mouse_button_to_gm(u8 button) noexcept {
    switch (button) {
        case SDL_BUTTON_LEFT:   return 1; // mb_left
        case SDL_BUTTON_RIGHT:  return 2; // mb_right
        case SDL_BUTTON_MIDDLE: return 3; // mb_middle
        case SDL_BUTTON_X1:     return 4;
        case SDL_BUTTON_X2:     return 5;
        default:                return 0;
    }
}

WindowEvents PlatformWindow::poll_events() {
    WindowEvents out{};
    input_.begin_frame();

    auto update_aggregate_modifiers = [this]() {
        input_.key_down[16] = input_.key_down[160] || input_.key_down[161]; // vk_shift
        input_.key_down[17] = input_.key_down[162] || input_.key_down[163]; // vk_control
        input_.key_down[18] = input_.key_down[164] || input_.key_down[165]; // vk_alt
    };

    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                out.quit_requested = true;
                break;

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
                out.resized = true;
                out.new_width = drawable_width();
                out.new_height = drawable_height();
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                has_focus_ = true;
                out.focus_gained = true;
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                has_focus_ = false;
                out.focus_lost = true;
                break;

            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                out.entered_background = true;
                break;

            case SDL_EVENT_DID_ENTER_FOREGROUND:
                out.entered_foreground = true;
                break;

            case SDL_EVENT_KEY_DOWN: {
                const u8 vk = sdl_scancode_to_gm_vk(static_cast<i32>(ev.key.scancode));
                if (vk != 0 && !ev.key.repeat) {
                    if (!input_.key_down[vk]) {
                        input_.key_pressed[vk] = true;
                    }
                    input_.key_down[vk] = true;
                    input_.last_key = vk;
                    input_.current_key = vk;

                    // Mirror aggregate modifier press edges (vk_shift=16, vk_control=17, vk_alt=18).
                    if (vk == 160 || vk == 161) input_.key_pressed[16] = true;
                    if (vk == 162 || vk == 163) input_.key_pressed[17] = true;
                    if (vk == 164 || vk == 165) input_.key_pressed[18] = true;
                    update_aggregate_modifiers();
                }
                break;
            }

            case SDL_EVENT_KEY_UP: {
                const u8 vk = sdl_scancode_to_gm_vk(static_cast<i32>(ev.key.scancode));
                if (vk != 0) {
                    if (input_.key_down[vk]) {
                        input_.key_released[vk] = true;
                    }
                    input_.key_down[vk] = false;
                    if (input_.current_key == vk) {
                        input_.current_key = 0;
                    }

                    const bool prev_shift = input_.key_down[16];
                    const bool prev_ctrl = input_.key_down[17];
                    const bool prev_alt = input_.key_down[18];
                    update_aggregate_modifiers();
                    if (prev_shift && !input_.key_down[16]) input_.key_released[16] = true;
                    if (prev_ctrl && !input_.key_down[17]) input_.key_released[17] = true;
                    if (prev_alt && !input_.key_down[18]) input_.key_released[18] = true;
                }
                break;
            }

            case SDL_EVENT_TEXT_INPUT:
                if (ev.text.text && ev.text.text[0] != '\0') {
                    input_.last_char = ev.text.text;
                    input_.keyboard_string += ev.text.text;
                    if (input_.keyboard_string.size() > 1024) {
                        input_.keyboard_string.erase(0, input_.keyboard_string.size() - 1024);
                    }
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                input_.mouse_x = ev.motion.x;
                input_.mouse_y = ev.motion.y;
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                const u8 btn = sdl_mouse_button_to_gm(ev.button.button);
                if (btn > 0 && btn < input_.mouse_down.size()) {
                    if (!input_.mouse_down[btn]) {
                        input_.mouse_pressed[btn] = true;
                    }
                    input_.mouse_down[btn] = true;
                }
                input_.mouse_x = ev.button.x;
                input_.mouse_y = ev.button.y;
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const u8 btn = sdl_mouse_button_to_gm(ev.button.button);
                if (btn > 0 && btn < input_.mouse_down.size()) {
                    if (input_.mouse_down[btn]) {
                        input_.mouse_released[btn] = true;
                    }
                    input_.mouse_down[btn] = false;
                }
                input_.mouse_x = ev.button.x;
                input_.mouse_y = ev.button.y;
                break;
            }

            case SDL_EVENT_MOUSE_WHEEL:
                if (ev.wheel.y > 0.0f) input_.mouse_wheel_up = true;
                if (ev.wheel.y < 0.0f) input_.mouse_wheel_down = true;
                break;

            case SDL_EVENT_DROP_FILE:
                if (ev.drop.data && ev.drop.data[0] != '\0') {
                    input_.dropped_files.emplace_back(ev.drop.data);
                    GM_LOG_INFO("platform", "File dropped: %s", ev.drop.data);
                }
                break;

            default:
                break;
        }
    }

    return out;
}

void PlatformWindow::swap_buffers() {
    if (window_) {
        SDL_GL_SwapWindow(window_);
    }
}

void PlatformWindow::set_title(std::string_view title) {
    if (window_) {
        std::string copy(title);
        SDL_SetWindowTitle(window_, copy.c_str());
    }
}

std::string_view PlatformWindow::title() const noexcept {
    return window_ ? SDL_GetWindowTitle(window_) : "";
}

void PlatformWindow::set_size(i32 width, i32 height) {
    if (window_) {
        SDL_SetWindowSize(window_, width, height);
    }
}

void PlatformWindow::set_position(i32 x, i32 y) {
    if (window_) {
        SDL_SetWindowPosition(window_, x, y);
    }
}

void PlatformWindow::center() {
    if (window_) {
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void PlatformWindow::set_fullscreen(bool fullscreen) {
    if (window_ && fullscreen_ != fullscreen) {
        if (SDL_SetWindowFullscreen(window_, fullscreen)) {
            fullscreen_ = fullscreen;
        }
    }
}

void PlatformWindow::set_vsync(bool enabled) {
    if (gl_context_) {
        if (!SDL_GL_SetSwapInterval(enabled ? 1 : 0)) {
            GM_LOG_WARN("platform", "SDL_GL_SetSwapInterval(%d) failed: %s",
                        enabled ? 1 : 0, SDL_GetError());
        }
    }
}

void PlatformWindow::set_cursor_visible(bool visible) {
    if (visible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

i32 PlatformWindow::x() const noexcept {
    int px = 0, py = 0;
    if (window_) SDL_GetWindowPosition(window_, &px, &py);
    return px;
}

i32 PlatformWindow::y() const noexcept {
    int px = 0, py = 0;
    if (window_) SDL_GetWindowPosition(window_, &px, &py);
    return py;
}

i32 PlatformWindow::width() const noexcept {
    int w = 0;
    int h = 0;
    if (window_) SDL_GetWindowSize(window_, &w, &h);
    return w;
}

i32 PlatformWindow::height() const noexcept {
    int w = 0;
    int h = 0;
    if (window_) SDL_GetWindowSize(window_, &w, &h);
    return h;
}

i32 PlatformWindow::drawable_width() const noexcept {
    int w = 0;
    int h = 0;
    if (window_) SDL_GetWindowSizeInPixels(window_, &w, &h);
    return w;
}

i32 PlatformWindow::drawable_height() const noexcept {
    int w = 0;
    int h = 0;
    if (window_) SDL_GetWindowSizeInPixels(window_, &w, &h);
    return h;
}

i32 PlatformWindow::display_width() const noexcept {
    const SDL_DisplayID did = window_ ? SDL_GetDisplayForWindow(window_) : SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(did)) {
        return mode->w;
    }
    return 1920;
}

i32 PlatformWindow::display_height() const noexcept {
    const SDL_DisplayID did = window_ ? SDL_GetDisplayForWindow(window_) : SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(did)) {
        return mode->h;
    }
    return 1080;
}

bool PlatformWindow::numlock_on() noexcept {
    return (SDL_GetModState() & SDL_KMOD_NUM) != 0;
}

bool PlatformWindow::capslock_on() noexcept {
    return (SDL_GetModState() & SDL_KMOD_CAPS) != 0;
}

bool PlatformWindow::scrolllock_on() noexcept {
    return (SDL_GetModState() & SDL_KMOD_SCROLL) != 0;
}

void* PlatformWindow::gl_get_proc_address(const char* proc_name) noexcept {
    return reinterpret_cast<void*>(SDL_GL_GetProcAddress(proc_name));
}

void PlatformWindow::show_message(std::string_view title, std::string_view message) const {
    const std::string t(title);
    const std::string m(message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, t.c_str(), m.c_str(), window_);
}

bool PlatformWindow::show_question(std::string_view title, std::string_view message) const {
    const std::string t(title);
    const std::string m(message);

    const SDL_MessageBoxButtonData buttons[2] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"},
    };
    const SDL_MessageBoxData data{
        SDL_MESSAGEBOX_INFORMATION,
        window_,
        t.c_str(),
        m.c_str(),
        2,
        buttons,
        nullptr,
    };
    int button_id = 0;
    if (!SDL_ShowMessageBox(&data, &button_id)) {
        return false;
    }
    return button_id == 1;
}

bool PlatformWindow::open_url(std::string_view url) {
    const std::string u(url);
    return SDL_OpenURL(u.c_str());
}

std::string PlatformWindow::clipboard_get_text() {
    char* text = SDL_GetClipboardText();
    if (!text) {
        return {};
    }
    std::string out(text);
    SDL_free(text);
    return out;
}

void PlatformWindow::clipboard_set_text(std::string_view text) {
    const std::string copy(text);
    SDL_SetClipboardText(copy.c_str());
}

} // namespace gm
