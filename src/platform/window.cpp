#include "platform/window.h"

#include "base/log.h"
#include "platform/web_bridge.h"

#include <SDL3/SDL.h>

#include <algorithm>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gm {

#if defined(__EMSCRIPTEN__)
// SDL normalizes pointer positions against the whole element. With object-fit: contain,
// the canvas bitmap occupies only the centered rectangle inside that element.
EM_JS(void, web_canvas_pointer_scale, (float* factors), {
    const canvas = Module.canvas;
    const rect = canvas.getBoundingClientRect();
    let x = 1, y = 1;
    if (canvas.width && canvas.height && rect.width && rect.height &&
        getComputedStyle(canvas).objectFit === 'contain') {
        const scale = Math.min(rect.width / canvas.width, rect.height / canvas.height);
        x = rect.width / (canvas.width * scale);
        y = rect.height / (canvas.height * scale);
    }
    HEAPF32[factors >> 2] = x;
    HEAPF32[(factors >> 2) + 1] = y;
});
#endif

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
    touches.clear();
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
    borderless_ = config.borderless;
    resizable_ = config.resizable;
    stay_on_top_ = false;
    visible_ = true;
    set_vsync(config.vsync);
    set_cursor_visible(config.show_cursor);
#if !defined(__ANDROID__)
    // Enable UTF-8 text input events for keyboard_string / keyboard_lastchar on desktop & web.
    SDL_StartTextInput(window_);
#endif

    GM_LOG_INFO("platform", "Created window %dx%d (drawable %dx%d)",
                width(), height(), drawable_width(), drawable_height());
    return Result<void>::success();
}

void PlatformWindow::destroy() noexcept {
    pending_windowed_size_.reset();
    pending_windowed_position_.reset();
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
    return poll_events_impl(true);
}

WindowEvents PlatformWindow::poll_events_accumulate() {
    return poll_events_impl(false);
}

WindowEvents PlatformWindow::poll_events_impl(bool clear_edges) {
    WindowEvents out{};
    if (clear_edges) {
        input_.begin_frame();
    }

    auto update_aggregate_modifiers = [this]() {
        input_.key_down[16] = input_.key_down[160] || input_.key_down[161]; // vk_shift
        input_.key_down[17] = input_.key_down[162] || input_.key_down[163]; // vk_control
        input_.key_down[18] = input_.key_down[164] || input_.key_down[165]; // vk_alt
    };

#if defined(__EMSCRIPTEN__)
    std::array<f32, 2> pointer_scale{};
    auto map_pointer = [&](float& x, float& y, float w, float h) {
        if (pointer_scale[0] == 0.0f) web_canvas_pointer_scale(pointer_scale.data());
        x = (x - w * 0.5f) * pointer_scale[0] + w * 0.5f;
        y = (y - h * 0.5f) * pointer_scale[1] + h * 0.5f;
    };
#endif
    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
#if defined(__EMSCRIPTEN__)
        if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            map_pointer(ev.motion.x, ev.motion.y, width(), height());
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            map_pointer(ev.button.x, ev.button.y, width(), height());
        } else if (ev.type == SDL_EVENT_FINGER_DOWN || ev.type == SDL_EVENT_FINGER_MOTION) {
            map_pointer(ev.tfinger.x, ev.tfinger.y, 1.0f, 1.0f);
        }
#endif
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

            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                fullscreen_ = true;
                out.resized = true;
                out.new_width = drawable_width();
                out.new_height = drawable_height();
                break;

            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                fullscreen_ = false;
                apply_pending_windowed_size();
                out.resized = true;
                out.new_width = drawable_width();
                out.new_height = drawable_height();
                break;

            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                out.entered_background = true;
                break;

            case SDL_EVENT_DID_ENTER_FOREGROUND:
                out.entered_foreground = true;
                break;

            case SDL_EVENT_RENDER_DEVICE_RESET:
            case SDL_EVENT_RENDER_TARGETS_RESET:
                out.context_lost = true;
                break;

            case SDL_EVENT_KEY_DOWN: {
                out.any_user_gesture = true;
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
                out.any_user_gesture = true;
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

            case SDL_EVENT_FINGER_DOWN: {
                out.any_user_gesture = true;
                input_.touch_seen = true;
                const i64 fid = static_cast<i64>(ev.tfinger.fingerID);
                const f32 tx = std::clamp(ev.tfinger.x, 0.0f, 1.0f);
                const f32 ty = std::clamp(ev.tfinger.y, 0.0f, 1.0f);
                auto it = std::find_if(input_.touches.begin(), input_.touches.end(),
                                       [fid](const TouchFinger& f) { return f.finger_id == fid; });
                if (it != input_.touches.end()) {
                    it->x = tx;
                    it->y = ty;
                } else {
                    input_.touches.push_back(TouchFinger{fid, tx, ty});
                }
                break;
            }

            case SDL_EVENT_FINGER_MOTION: {
                input_.touch_seen = true;
                const i64 fid = static_cast<i64>(ev.tfinger.fingerID);
                const f32 tx = std::clamp(ev.tfinger.x, 0.0f, 1.0f);
                const f32 ty = std::clamp(ev.tfinger.y, 0.0f, 1.0f);
                auto it = std::find_if(input_.touches.begin(), input_.touches.end(),
                                       [fid](const TouchFinger& f) { return f.finger_id == fid; });
                if (it != input_.touches.end()) {
                    it->x = tx;
                    it->y = ty;
                } else {
                    input_.touches.push_back(TouchFinger{fid, tx, ty});
                }
                break;
            }

            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: {
                const i64 fid = static_cast<i64>(ev.tfinger.fingerID);
                input_.touches.erase(
                    std::remove_if(input_.touches.begin(), input_.touches.end(),
                                   [fid](const TouchFinger& f) { return f.finger_id == fid; }),
                    input_.touches.end());
                break;
            }

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
    if (!window_) return;
#if defined(__EMSCRIPTEN__)
    // CSS owns the mobile drawable size. Game window sizes apply to its logical region.
    if (is_mobile_touch_platform()) return;
#endif
    if (fullscreen_ || pending_windowed_size_) {
        pending_windowed_size_.emplace(width, height);
        return;
    }
    SDL_SetWindowSize(window_, width, height);
}

void PlatformWindow::set_position(i32 x, i32 y) {
    if (!window_) return;
    if (!fullscreen_ && pending_windowed_size_) {
        pending_windowed_position_.emplace(x, y);
        return;
    }
    SDL_SetWindowPosition(window_, x, y);
}

void PlatformWindow::center() {
    set_position(SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void PlatformWindow::set_fullscreen(bool fullscreen) {
    if (window_ && fullscreen_ != fullscreen) {
        if (SDL_SetWindowFullscreen(window_, fullscreen)) {
            fullscreen_ = fullscreen;
            apply_pending_windowed_size();
        }
    }
}

void PlatformWindow::apply_pending_windowed_size() {
    // SDL defers a hidden window's fullscreen transition until it is shown again.
    if (fullscreen_ || !visible_ || !pending_windowed_size_) return;
    SDL_SyncWindow(window_);
    SDL_SetWindowSize(window_, pending_windowed_size_->first, pending_windowed_size_->second);
    pending_windowed_size_.reset();
    if (pending_windowed_position_) {
        set_position(pending_windowed_position_->first, pending_windowed_position_->second);
        pending_windowed_position_.reset();
    }
}

void PlatformWindow::set_vsync(bool enabled) {
#if defined(__EMSCRIPTEN__)
    // WebGL swap timing is governed by the browser compositor / requestAnimationFrame.
    (void)enabled;
#else
    if (gl_context_) {
        if (!SDL_GL_SetSwapInterval(enabled ? 1 : 0)) {
            GM_LOG_WARN("platform", "SDL_GL_SetSwapInterval(%d) failed: %s",
                        enabled ? 1 : 0, SDL_GetError());
        }
    }
#endif
}

void PlatformWindow::set_cursor_visible(bool visible) {
    if (visible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

void PlatformWindow::set_borderless(bool borderless) {
    borderless_ = borderless;
    if (window_) {
        SDL_SetWindowBordered(window_, !borderless);
    }
}

void PlatformWindow::set_stay_on_top(bool on_top) {
    stay_on_top_ = on_top;
    if (window_) {
        SDL_SetWindowAlwaysOnTop(window_, on_top);
    }
}

void PlatformWindow::set_resizable(bool resizable) {
    resizable_ = resizable;
    if (window_) {
        SDL_SetWindowResizable(window_, resizable);
    }
}

void PlatformWindow::set_visible(bool visible) {
    visible_ = visible;
    if (window_) {
        if (visible) {
            SDL_ShowWindow(window_);
            apply_pending_windowed_size();
        } else {
            SDL_HideWindow(window_);
        }
    }
}

bool PlatformWindow::is_maximized() const noexcept {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
}

bool PlatformWindow::is_minimized() const noexcept {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

void PlatformWindow::maximize() {
    if (window_) SDL_MaximizeWindow(window_);
}

void PlatformWindow::minimize() {
    if (window_) SDL_MinimizeWindow(window_);
}

void PlatformWindow::restore() {
    if (window_) SDL_RestoreWindow(window_);
}

void PlatformWindow::raise_focus() {
    if (window_) SDL_RaiseWindow(window_);
}

void PlatformWindow::set_mouse_position(i32 x, i32 y) {
    if (window_) {
        SDL_WarpMouseInWindow(window_, static_cast<float>(x), static_cast<float>(y));
        input_.mouse_x = static_cast<f32>(x);
        input_.mouse_y = static_cast<f32>(y);
    }
}

i32 PlatformWindow::display_mouse_x() noexcept {
    float mx = 0.0f, my = 0.0f;
    SDL_GetGlobalMouseState(&mx, &my);
    return static_cast<i32>(mx);
}

i32 PlatformWindow::display_mouse_y() noexcept {
    float mx = 0.0f, my = 0.0f;
    SDL_GetGlobalMouseState(&mx, &my);
    return static_cast<i32>(my);
}

void PlatformWindow::set_display_mouse_position(i32 x, i32 y) noexcept {
    SDL_WarpMouseGlobal(static_cast<float>(x), static_cast<float>(y));
}

u64 PlatformWindow::native_window_handle() const noexcept {
    if (!window_) return 0;
#if defined(_WIN32)
    if (void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
        return reinterpret_cast<u64>(hwnd);
    }
#endif
    return reinterpret_cast<u64>(window_);
}

namespace {

SDL_Joystick* open_gm_joystick(i32 id) noexcept {
    if (id < 1) return nullptr;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) return nullptr;
    SDL_Joystick* joy = nullptr;
    if (id - 1 < count) {
        joy = SDL_GetJoystickFromID(ids[id - 1]);
        if (!joy) {
            joy = SDL_OpenJoystick(ids[id - 1]);
        }
    }
    SDL_free(ids);
    return joy;
}

} // namespace

bool PlatformWindow::joystick_exists(i32 id) noexcept {
    return open_gm_joystick(id) != nullptr;
}

std::string PlatformWindow::joystick_name(i32 id) {
    if (SDL_Joystick* joy = open_gm_joystick(id)) {
        const char* n = SDL_GetJoystickName(joy);
        return n ? std::string(n) : std::string{};
    }
    return {};
}

i32 PlatformWindow::joystick_axes(i32 id) noexcept {
    if (SDL_Joystick* joy = open_gm_joystick(id)) {
        return SDL_GetNumJoystickAxes(joy);
    }
    return 0;
}

i32 PlatformWindow::joystick_buttons(i32 id) noexcept {
    if (SDL_Joystick* joy = open_gm_joystick(id)) {
        return SDL_GetNumJoystickButtons(joy);
    }
    return 0;
}

bool PlatformWindow::joystick_has_pov(i32 id) noexcept {
    if (SDL_Joystick* joy = open_gm_joystick(id)) {
        return SDL_GetNumJoystickHats(joy) > 0;
    }
    return false;
}

bool PlatformWindow::joystick_check_button(i32 id, i32 button) noexcept {
    if (button < 1) return false;
    if (SDL_Joystick* joy = open_gm_joystick(id)) {
        return SDL_GetJoystickButton(joy, button - 1);
    }
    return false;
}

f64 PlatformWindow::joystick_axis(i32 id, i32 axis_index) noexcept {
    if (axis_index < 0) return 0.0;
    if (SDL_Joystick* joy = open_gm_joystick(id)) {
        if (axis_index < SDL_GetNumJoystickAxes(joy)) {
            const Sint16 v = SDL_GetJoystickAxis(joy, axis_index);
            return static_cast<f64>(v) / 32767.0;
        }
    }
    return 0.0;
}

f64 PlatformWindow::joystick_pov(i32 id) noexcept {
    if (SDL_Joystick* joy = open_gm_joystick(id)) {
        if (SDL_GetNumJoystickHats(joy) > 0) {
            switch (SDL_GetJoystickHat(joy, 0)) {
                case SDL_HAT_UP:        return 0.0;
                case SDL_HAT_RIGHTUP:   return 45.0;
                case SDL_HAT_RIGHT:     return 90.0;
                case SDL_HAT_RIGHTDOWN: return 135.0;
                case SDL_HAT_DOWN:      return 180.0;
                case SDL_HAT_LEFTDOWN:  return 225.0;
                case SDL_HAT_LEFT:      return 270.0;
                case SDL_HAT_LEFTUP:    return 315.0;
                default:                return -1.0;
            }
        }
    }
    return -1.0;
}

i32 PlatformWindow::joystick_direction(i32 id) noexcept {
    // GM8 returns numpad key code: 97=SW..105=NE, 101=center.
    const f64 x = joystick_axis(id, 0);
    const f64 y = joystick_axis(id, 1);
    const i32 dx = (x < -0.5) ? -1 : (x > 0.5 ? 1 : 0);
    const i32 dy = (y < -0.5) ? -1 : (y > 0.5 ? 1 : 0);
    return 101 + dx - dy * 3;
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

bool PlatformWindow::keyboard_check_direct(i32 key) noexcept {
#if defined(_WIN32)
    // GM tests the whole return value, including the press since the last query.
    return GetAsyncKeyState(key) != 0;
#else
    if (key <= 0 || key > 255) return false;
    SDL_PumpEvents();
    int count = 0;
    const bool* down = SDL_GetKeyboardState(&count);
    switch (key) {
        case 1: return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
        case 2: return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) != 0;
        case 4: return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MMASK) != 0;
        case 5: return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_X1MASK) != 0;
        case 6: return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_X2MASK) != 0;
        case 16: return down[SDL_SCANCODE_LSHIFT] || down[SDL_SCANCODE_RSHIFT];
        case 17: return down[SDL_SCANCODE_LCTRL] || down[SDL_SCANCODE_RCTRL];
        case 18: return down[SDL_SCANCODE_LALT] || down[SDL_SCANCODE_RALT];
    }
    for (int i = 0; i < count; ++i) {
        if (down[i] && sdl_scancode_to_gm_vk(i) == key) return true;
    }
    return false;
#endif
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
#if defined(__EMSCRIPTEN__)
    web_show_message(title, message);
#else
    const std::string t(title);
    const std::string m(message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, t.c_str(), m.c_str(), window_);
#endif
}

bool PlatformWindow::show_question(std::string_view title, std::string_view message) const {
#if defined(__EMSCRIPTEN__)
    return web_show_question(title, message);
#else
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
#endif
}

std::string PlatformWindow::prompt_string(std::string_view prompt, std::string_view default_val) const {
#if defined(__EMSCRIPTEN__)
    return web_prompt_string(prompt, default_val);
#else
    (void)prompt;
    return std::string(default_val);
#endif
}

void PlatformWindow::set_text_input(bool enabled) {
    if (!window_) return;
    if (enabled) {
        SDL_StartTextInput(window_);
    } else {
        SDL_StopTextInput(window_);
    }
}

bool PlatformWindow::is_mobile_touch_platform() noexcept {
#if defined(__ANDROID__)
    return true;
#elif defined(__EMSCRIPTEN__)
    static const bool mobile = EM_ASM_INT({
        return window.matchMedia("(pointer: coarse)").matches;
    }) != 0;
    return mobile;
#else
    return false;
#endif
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
