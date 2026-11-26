#include "platform/window.h"

#include "base/encoding.h"
#include "base/log.h"
#include "platform/web_bridge.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>

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
#include <commctrl.h>
#include <dwmapi.h>
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
    mouse_is_touch = false;
    physical_left_down = false;
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
#if defined(_WIN32)
    if (!config.show_icons) flags |= SDL_WINDOW_HIDDEN;
#endif

    window_ = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (!window_) {
        return Error{std::string("SDL_CreateWindow failed: ") + SDL_GetError()};
    }

    show_icons_ = true;
    set_show_icons(config.show_icons);
#if defined(_WIN32)
    if (!config.show_icons) SDL_ShowWindow(window_);
#endif
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
    composite_alpha_ = false;
    chromakey_enabled_ = false;
    chromakey_color_ = 0;
    set_vsync(config.vsync);
    set_cursor(config.show_cursor ? 0 : -1);
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
    for (SDL_Cursor*& cur : system_cursors_) {
        if (cur) {
            SDL_DestroyCursor(cur);
            cur = nullptr;
        }
    }
    current_cursor_ = 0;
    if (gl_context_) {
        SDL_GL_DestroyContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    composite_alpha_ = false;
    chromakey_enabled_ = false;
    chromakey_color_ = 0;
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

    auto update_aggregate_modifiers = [this](u8 vk, bool pressed) {
        if (vk == 160 || vk == 161) {
            if (pressed) {
                if (!input_.key_down[16]) input_.key_pressed[16] = true;
                input_.key_down[16] = true;
            } else if (!input_.key_down[160] && !input_.key_down[161] && input_.key_down[16]) {
                input_.key_down[16] = false;
                input_.key_released[16] = true;
            }
        } else if (vk == 162 || vk == 163) {
            if (pressed) {
                if (!input_.key_down[17]) input_.key_pressed[17] = true;
                input_.key_down[17] = true;
            } else if (!input_.key_down[162] && !input_.key_down[163] && input_.key_down[17]) {
                input_.key_down[17] = false;
                input_.key_released[17] = true;
            }
        } else if (vk == 164 || vk == 165) {
            if (pressed) {
                if (!input_.key_down[18]) input_.key_pressed[18] = true;
                input_.key_down[18] = true;
            } else if (!input_.key_down[164] && !input_.key_down[165] && input_.key_down[18]) {
                input_.key_down[18] = false;
                input_.key_released[18] = true;
            }
        }
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
                const u8 raw_vk = sdl_scancode_to_gm_vk(static_cast<i32>(ev.key.scancode));
                if (raw_vk != 0 && !ev.key.repeat) {
                    const i32 mapped = input_.get_key_map(raw_vk);
                    if (mapped >= 0 && mapped < 256) {
                        const u8 vk = static_cast<u8>(mapped);
                        if (!input_.key_down[vk]) {
                            input_.key_pressed[vk] = true;
                        }
                        input_.key_down[vk] = true;
                        input_.last_key = vk;
                        input_.current_key = vk;

                        // Mirror aggregate modifier press edges (vk_shift=16, vk_control=17, vk_alt=18).
                        update_aggregate_modifiers(vk, true);
                    }
                }
                break;
            }

            case SDL_EVENT_KEY_UP: {
                const u8 raw_vk = sdl_scancode_to_gm_vk(static_cast<i32>(ev.key.scancode));
                if (raw_vk != 0) {
                    const i32 mapped = input_.get_key_map(raw_vk);
                    if (mapped >= 0 && mapped < 256) {
                        const u8 vk = static_cast<u8>(mapped);
                        if (input_.key_down[vk]) {
                            input_.key_released[vk] = true;
                        }
                        input_.key_down[vk] = false;
                        if (input_.current_key == vk) {
                            input_.current_key = 0;
                        }
                        update_aggregate_modifiers(vk, false);
                    }
                }
                break;
            }

            case SDL_EVENT_TEXT_INPUT:
                if (ev.text.text && ev.text.text[0] != '\0') {
                    input_.last_char = utf8_to_codepage(ev.text.text, input_codepage_);
                    input_.keyboard_string += input_.last_char;
                    if (input_.keyboard_string.size() > 1024) {
                        input_.keyboard_string.erase(0, input_.keyboard_string.size() - 1024);
                    }
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                input_.mouse_is_touch = (ev.motion.which == SDL_TOUCH_MOUSEID);
                input_.mouse_x = ev.motion.x;
                input_.mouse_y = ev.motion.y;
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                out.any_user_gesture = true;
                input_.mouse_is_touch = (ev.button.which == SDL_TOUCH_MOUSEID);
                const u8 btn = sdl_mouse_button_to_gm(ev.button.button);
                if (btn == 1 && !input_.mouse_is_touch) input_.physical_left_down = true;
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
                input_.mouse_is_touch = (ev.button.which == SDL_TOUCH_MOUSEID);
                const u8 btn = sdl_mouse_button_to_gm(ev.button.button);
                if (btn == 1 && !input_.mouse_is_touch) input_.physical_left_down = false;
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
                    *it = TouchFinger{fid, tx, ty};
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

void PlatformWindow::reset_modal_input_state() noexcept {
    modal_left_down_ = false;
    modal_pointer_is_touch_ = false;
    modal_touch_finger_.reset();
    modal_mouse_x_ = input_.mouse_x;
    modal_mouse_y_ = input_.mouse_y;
}

void PlatformWindow::poll_modal_events(ModalInputFrame& out) {
    out = ModalInputFrame{};
    out.mouse_x = modal_mouse_x_;
    out.mouse_y = modal_mouse_y_;
    out.left_down = modal_left_down_;
    out.pointer_is_touch = modal_pointer_is_touch_;

#if defined(__EMSCRIPTEN__)
    std::array<f32, 2> pointer_scale{};
    auto map_pointer = [&](float& x, float& y, float w, float h) {
        if (pointer_scale[0] == 0.0f) web_canvas_pointer_scale(pointer_scale.data());
        x = (x - w * 0.5f) * pointer_scale[0] + w * 0.5f;
        y = (y - h * 0.5f) * pointer_scale[1] + h * 0.5f;
    };
#endif

    const f32 win_w = static_cast<f32>(std::max(1, width()));
    const f32 win_h = static_cast<f32>(std::max(1, height()));

    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
#if defined(__EMSCRIPTEN__)
        if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            map_pointer(ev.motion.x, ev.motion.y, win_w, win_h);
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            map_pointer(ev.button.x, ev.button.y, win_w, win_h);
        } else if (ev.type == SDL_EVENT_FINGER_DOWN || ev.type == SDL_EVENT_FINGER_MOTION ||
                   ev.type == SDL_EVENT_FINGER_UP || ev.type == SDL_EVENT_FINGER_CANCELED) {
            map_pointer(ev.tfinger.x, ev.tfinger.y, 1.0f, 1.0f);
        }
#endif
        switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                out.window_events.quit_requested = true;
                break;

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
                out.window_events.resized = true;
                out.window_events.new_width = drawable_width();
                out.window_events.new_height = drawable_height();
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                has_focus_ = true;
                out.window_events.focus_gained = true;
                break;

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                has_focus_ = false;
                out.window_events.focus_lost = true;
                break;

            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                fullscreen_ = true;
                out.window_events.resized = true;
                out.window_events.new_width = drawable_width();
                out.window_events.new_height = drawable_height();
                break;

            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                fullscreen_ = false;
                apply_pending_windowed_size();
                out.window_events.resized = true;
                out.window_events.new_width = drawable_width();
                out.window_events.new_height = drawable_height();
                break;

            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                out.window_events.entered_background = true;
                break;

            case SDL_EVENT_DID_ENTER_FOREGROUND:
                out.window_events.entered_foreground = true;
                break;

            case SDL_EVENT_RENDER_DEVICE_RESET:
            case SDL_EVENT_RENDER_TARGETS_RESET:
                out.window_events.context_lost = true;
                break;

            case SDL_EVENT_KEY_DOWN: {
                out.window_events.any_user_gesture = true;
                ModalKeyEvent ke{};
                ke.vk = sdl_scancode_to_gm_vk(static_cast<i32>(ev.key.scancode));
                ke.keycode = static_cast<u32>(ev.key.key);
                ke.repeat = ev.key.repeat;
                ke.ctrl = (ev.key.mod & SDL_KMOD_CTRL) != 0;
                ke.shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
                ke.alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
                ke.gui = (ev.key.mod & SDL_KMOD_GUI) != 0;
                out.keyboard_events.emplace_back(ke);
                break;
            }

            case SDL_EVENT_TEXT_INPUT:
                if (ev.text.text && ev.text.text[0] != '\0') {
                    out.keyboard_events.emplace_back(std::string(ev.text.text));
                }
                break;

            case SDL_EVENT_TEXT_EDITING:
                out.keyboard_events.emplace_back(ModalComposition{
                    ev.edit.text ? ev.edit.text : "", ev.edit.start, ev.edit.length});
                break;

            case SDL_EVENT_MOUSE_MOTION:
                modal_pointer_is_touch_ = (ev.motion.which == SDL_TOUCH_MOUSEID);
                modal_mouse_x_ = ev.motion.x;
                modal_mouse_y_ = ev.motion.y;
                out.mouse_x = modal_mouse_x_;
                out.mouse_y = modal_mouse_y_;
                out.mouse_moved = true;
                out.pointer_is_touch = modal_pointer_is_touch_;
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                out.window_events.any_user_gesture = true;
                modal_pointer_is_touch_ = (ev.button.which == SDL_TOUCH_MOUSEID);
                modal_mouse_x_ = ev.button.x;
                modal_mouse_y_ = ev.button.y;
                out.mouse_x = modal_mouse_x_;
                out.mouse_y = modal_mouse_y_;
                out.pointer_is_touch = modal_pointer_is_touch_;
                out.any_mouse_pressed = true;
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    modal_left_down_ = true;
                    out.left_down = true;
                    out.left_pressed = true;
                    if (ev.button.clicks >= 2) {
                        out.left_double_click = true;
                    }
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    out.right_pressed = true;
                } else if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    out.middle_pressed = true;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                modal_pointer_is_touch_ = (ev.button.which == SDL_TOUCH_MOUSEID);
                modal_mouse_x_ = ev.button.x;
                modal_mouse_y_ = ev.button.y;
                out.mouse_x = modal_mouse_x_;
                out.mouse_y = modal_mouse_y_;
                out.pointer_is_touch = modal_pointer_is_touch_;
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    modal_left_down_ = false;
                    out.left_down = false;
                    out.left_released = true;
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                out.wheel_y += ev.wheel.y;
                break;

            case SDL_EVENT_FINGER_DOWN: {
                out.window_events.any_user_gesture = true;
                const i64 fid = static_cast<i64>(ev.tfinger.fingerID);
                if (!modal_touch_finger_.has_value()) {
                    modal_touch_finger_ = fid;
                    modal_pointer_is_touch_ = true;
                    modal_mouse_x_ = std::clamp(ev.tfinger.x, 0.0f, 1.0f) * win_w;
                    modal_mouse_y_ = std::clamp(ev.tfinger.y, 0.0f, 1.0f) * win_h;
                    modal_left_down_ = true;
                    out.mouse_x = modal_mouse_x_;
                    out.mouse_y = modal_mouse_y_;
                    out.mouse_moved = true;
                    out.left_down = true;
                    out.left_pressed = true;
                    out.any_mouse_pressed = true;
                    out.pointer_is_touch = true;
                }
                break;
            }

            case SDL_EVENT_FINGER_MOTION: {
                const i64 fid = static_cast<i64>(ev.tfinger.fingerID);
                if (modal_touch_finger_.has_value() && *modal_touch_finger_ == fid) {
                    modal_pointer_is_touch_ = true;
                    modal_mouse_x_ = std::clamp(ev.tfinger.x, 0.0f, 1.0f) * win_w;
                    modal_mouse_y_ = std::clamp(ev.tfinger.y, 0.0f, 1.0f) * win_h;
                    out.mouse_x = modal_mouse_x_;
                    out.mouse_y = modal_mouse_y_;
                    out.mouse_moved = true;
                    out.pointer_is_touch = true;
                }
                break;
            }

            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: {
                const i64 fid = static_cast<i64>(ev.tfinger.fingerID);
                if (modal_touch_finger_.has_value() && *modal_touch_finger_ == fid) {
                    modal_touch_finger_.reset();
                    modal_pointer_is_touch_ = true;
                    modal_mouse_x_ = std::clamp(ev.tfinger.x, 0.0f, 1.0f) * win_w;
                    modal_mouse_y_ = std::clamp(ev.tfinger.y, 0.0f, 1.0f) * win_h;
                    modal_left_down_ = false;
                    out.mouse_x = modal_mouse_x_;
                    out.mouse_y = modal_mouse_y_;
                    out.left_down = false;
                    out.left_released = true;
                    out.pointer_is_touch = true;
                }
                break;
            }

            default:
                break;
        }
    }
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
    cursor_visible_ = visible;
    if (visible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

void PlatformWindow::set_cursor(i32 gm_cursor) {
    current_cursor_ = gm_cursor;
    if (gm_cursor == -1) { // cr_none
        set_cursor_visible(false);
        return;
    }

    SDL_SystemCursor sys_id = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (gm_cursor) {
        case 0:   // cr_default
        case -2:  // cr_arrow / cr_arrrow
        case -12: // cr_drag
        case -16: // cr_multidrag
        case -20: // cr_help
            sys_id = SDL_SYSTEM_CURSOR_DEFAULT;
            break;
        case -3:  // cr_cross
            sys_id = SDL_SYSTEM_CURSOR_CROSSHAIR;
            break;
        case -4:  // cr_beam
            sys_id = SDL_SYSTEM_CURSOR_TEXT;
            break;
        case -6:  // cr_size_nesw
            sys_id = SDL_SYSTEM_CURSOR_NESW_RESIZE;
            break;
        case -7:  // cr_size_ns
        case -14: // cr_hsplit
            sys_id = SDL_SYSTEM_CURSOR_NS_RESIZE;
            break;
        case -8:  // cr_size_nwse
            sys_id = SDL_SYSTEM_CURSOR_NWSE_RESIZE;
            break;
        case -9:  // cr_size_we
        case -15: // cr_vsplit
            sys_id = SDL_SYSTEM_CURSOR_EW_RESIZE;
            break;
        case -10: // cr_uparrow
            sys_id = SDL_SYSTEM_CURSOR_N_RESIZE;
            break;
        case -11: // cr_hourglass
        case -17: // cr_sqlwait
            sys_id = SDL_SYSTEM_CURSOR_WAIT;
            break;
        case -13: // cr_nodrop
        case -18: // cr_no
            sys_id = SDL_SYSTEM_CURSOR_NOT_ALLOWED;
            break;
        case -19: // cr_appstart
            sys_id = SDL_SYSTEM_CURSOR_PROGRESS;
            break;
        case -21: // cr_handpoint
            sys_id = SDL_SYSTEM_CURSOR_POINTER;
            break;
        case -22: // cr_size_all
            sys_id = SDL_SYSTEM_CURSOR_MOVE;
            break;
        default:
            sys_id = SDL_SYSTEM_CURSOR_DEFAULT;
            break;
    }

    const usize slot = static_cast<usize>(sys_id);
    if (slot < system_cursors_.size()) {
        if (!system_cursors_[slot]) {
            system_cursors_[slot] = SDL_CreateSystemCursor(sys_id);
        }
        if (system_cursors_[slot]) {
            SDL_SetCursor(system_cursors_[slot]);
        }
    }
    set_cursor_visible(true);
}

f32 PlatformWindow::display_scale() const noexcept {
    if (!window_) return 1.0f;
    const f32 s = SDL_GetWindowDisplayScale(window_);
    return (s > 0.1f && s < 16.0f) ? s : 1.0f;
}

void PlatformWindow::set_borderless(bool borderless) {
    borderless_ = borderless;
    if (window_) {
        SDL_SetWindowBordered(window_, !borderless);
    }
}

#if defined(_WIN32)
namespace {
constexpr DWORD caption_buttons = WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

LRESULT CALLBACK filter_caption_buttons(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR data) {
    if (msg == WM_STYLECHANGING && static_cast<int>(wp) == GWL_STYLE) {
        auto& style = *reinterpret_cast<STYLESTRUCT*>(lp);
        // SDL rebuilds the window style on fullscreen, border and resize changes.
        // Remember its latest windowed buttons for window_set_showicons(true).
        if (style.styleNew & WS_SYSMENU) {
            *reinterpret_cast<u32*>(data) = style.styleNew & caption_buttons;
            style.styleNew &= ~caption_buttons;
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, filter_caption_buttons, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
} // namespace
#endif

void PlatformWindow::set_show_icons(bool show) {
    if (show_icons_ == show) return;
#if defined(_WIN32)
    if (window_) {
        const auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                                                  SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd) {
            const DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
            DWORD updated = style;
            if (show) {
                RemoveWindowSubclass(hwnd, filter_caption_buttons, 0);
                if (style & WS_CAPTION) updated |= caption_button_style_;
            } else {
                if (!SetWindowSubclass(hwnd, filter_caption_buttons, 0,
                                       reinterpret_cast<DWORD_PTR>(&caption_button_style_))) {
                    GM_LOG_WARN("platform", "Could not hide window caption buttons");
                    return;
                }
                caption_button_style_ = style & caption_buttons;
                if (style & WS_SYSMENU) updated &= ~caption_buttons;
            }
            if (updated != style) {
                SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(updated));
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOACTIVATE |
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            }
        }
    }
#endif
    show_icons_ = show;
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

void PlatformWindow::set_composite_alpha(bool enabled) {
    if (composite_alpha_ == enabled) return;
    composite_alpha_ = enabled;
    update_window_transparency();
}

void PlatformWindow::set_chromakey(bool enabled, u32 color_bgr) {
    chromakey_color_ = color_bgr & 0x00FFFFFFu;
    if (chromakey_enabled_ == enabled) return;
    chromakey_enabled_ = enabled;
    update_window_transparency();
}

void PlatformWindow::update_window_transparency() {
#if defined(_WIN32)
    if (!window_) return;
    const auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return;
    if (composite_alpha_ || chromakey_enabled_) {
        // Use SDL3/GLFW's Win32 OpenGL DWM transparency technique: empty blur region
        // tells DWM to composite the framebuffer's alpha channel without glass blur.
        HRGN rgn = CreateRectRgn(-1, -1, 0, 0);
        DWM_BLURBEHIND bb{};
        bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        bb.fEnable = TRUE;
        bb.hRgnBlur = rgn;
        bb.fTransitionOnMaximized = FALSE;
        DwmEnableBlurBehindWindow(hwnd, &bb);
        if (rgn) DeleteObject(rgn);
    } else {
        DWM_BLURBEHIND bb{};
        bb.dwFlags = DWM_BB_ENABLE;
        bb.fEnable = FALSE;
        DwmEnableBlurBehindWindow(hwnd, &bb);
    }
#endif
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
#if defined(__EMSCRIPTEN__)
    // GM display dimensions are pixels. SDL caches screen.width in CSS pixels at startup.
    if (is_mobile_touch_platform()) {
        return EM_ASM_INT({ return Math.max(1, Math.round(screen.width * devicePixelRatio)); });
    }
#endif
    const SDL_DisplayID did = window_ ? SDL_GetDisplayForWindow(window_) : SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(did)) {
        return mode->w;
    }
    return 1920;
}

i32 PlatformWindow::display_height() const noexcept {
#if defined(__EMSCRIPTEN__)
    if (is_mobile_touch_platform()) {
        return EM_ASM_INT({ return Math.max(1, Math.round(screen.height * devicePixelRatio)); });
    }
#endif
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

namespace {

#if !defined(__EMSCRIPTEN__)
struct ParsedFileFilters {
    std::vector<std::pair<std::string, std::string>> owned;
    std::vector<SDL_DialogFileFilter> sdl;
};

std::string sanitize_gm_mask_to_sdl_pattern(std::string_view mask) {
    std::string out;
    usize pos = 0;
    while (pos <= mask.size()) {
        const usize sep = mask.find(';', pos);
        std::string_view tok = (sep == std::string_view::npos) ? mask.substr(pos) : mask.substr(pos, sep - pos);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.remove_suffix(1);

        if (tok == "*" || tok == "*.*" || tok.ends_with(".*")) {
            return "*";
        }
        if (tok.starts_with("*.")) {
            tok.remove_prefix(2);
        } else if (tok.starts_with('.')) {
            tok.remove_prefix(1);
        } else if (const usize dot = tok.rfind('.'); dot != std::string_view::npos) {
            tok.remove_prefix(dot + 1);
        }

        std::string ext;
        for (char c : tok) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            if (ok) ext.push_back(c);
        }
        while (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
        if (!ext.empty()) {
            if (!out.empty()) out.push_back(';');
            out += ext;
        }

        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }
    return out.empty() ? std::string("*") : out;
}

ParsedFileFilters parse_gm_file_filters(std::string_view gm_filter) {
    ParsedFileFilters res{};
    if (gm_filter.empty()) return res;

    std::vector<std::string_view> parts;
    usize pos = 0;
    while (pos <= gm_filter.size()) {
        const usize bar = gm_filter.find('|', pos);
        if (bar == std::string_view::npos) {
            parts.push_back(gm_filter.substr(pos));
            break;
        }
        parts.push_back(gm_filter.substr(pos, bar - pos));
        pos = bar + 1;
    }

    for (usize i = 0; i + 1 < parts.size(); i += 2) {
        std::string name(parts[i]);
        std::string pattern = sanitize_gm_mask_to_sdl_pattern(parts[i + 1]);
        if (name.empty()) name = pattern;
        res.owned.emplace_back(std::move(name), std::move(pattern));
    }
    res.sdl.reserve(res.owned.size());
    for (const auto& [name, pattern] : res.owned) {
        res.sdl.push_back(SDL_DialogFileFilter{name.c_str(), pattern.c_str()});
    }
    return res;
}

struct SdlFileDialogResult {
    std::mutex mutex;
    std::atomic<bool> done{false};
    std::string selected_path;
};

void SDLCALL sdl_file_dialog_callback(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* st = static_cast<SdlFileDialogResult*>(userdata);
    if (!st) return;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        if (filelist && filelist[0] && filelist[0][0] != '\0') {
            st->selected_path = filelist[0];
        } else {
            st->selected_path.clear();
        }
    }
    st->done.store(true, std::memory_order_release);
}
#endif // !defined(__EMSCRIPTEN__)

std::string run_sdl_file_dialog(
    SDL_Window* window,
    SDL_FileDialogType type,
    std::string_view gm_filter,
    std::string_view default_location,
    std::string_view title,
    const std::function<void()>& on_idle) {
#if defined(__EMSCRIPTEN__)
    (void)window;
    (void)type;
    (void)gm_filter;
    (void)default_location;
    (void)title;
    (void)on_idle;
    return {};
#else
    std::error_code cwd_ec;
    const auto saved_cwd = std::filesystem::current_path(cwd_ec);

    const ParsedFileFilters filters = (type == SDL_FILEDIALOG_OPENFOLDER)
        ? ParsedFileFilters{}
        : parse_gm_file_filters(gm_filter);
    const std::string default_loc(default_location);
    const std::string title_str(title);

    SDL_PropertiesID props = SDL_CreateProperties();
    if (props == 0) return {};

    if (window) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    }
    if (!filters.sdl.empty()) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                               const_cast<SDL_DialogFileFilter*>(filters.sdl.data()));
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                              static_cast<Sint64>(filters.sdl.size()));
    }
    if (!default_loc.empty()) {
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, default_loc.c_str());
    }
    if (!title_str.empty()) {
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title_str.c_str());
    }
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);

    SdlFileDialogResult state{};
    SDL_ShowFileDialogWithProperties(type, sdl_file_dialog_callback, &state, props);
    SDL_DestroyProperties(props);

    while (!state.done.load(std::memory_order_acquire)) {
        SDL_Event ev{};
        while (SDL_PollEvent(&ev)) {
            // Drain SDL queue so platform portals/window messages stay responsive.
        }
        if (on_idle) {
            on_idle();
        }
        SDL_Delay(10);
    }

    if (!cwd_ec && !saved_cwd.empty()) {
        std::error_code restore_ec;
        std::filesystem::current_path(saved_cwd, restore_ec);
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    return state.selected_path;
#endif
}

} // namespace

std::string PlatformWindow::open_file_dialog(
    std::string_view gm_filter,
    std::string_view default_location,
    const std::function<void()>& on_idle) {
    return run_sdl_file_dialog(window_, SDL_FILEDIALOG_OPENFILE, gm_filter, default_location, {}, on_idle);
}

std::string PlatformWindow::save_file_dialog(
    std::string_view gm_filter,
    std::string_view default_location,
    const std::function<void()>& on_idle) {
    return run_sdl_file_dialog(window_, SDL_FILEDIALOG_SAVEFILE, gm_filter, default_location, {}, on_idle);
}

std::string PlatformWindow::open_folder_dialog(
    std::string_view title,
    std::string_view default_location,
    const std::function<void()>& on_idle) {
    return run_sdl_file_dialog(window_, SDL_FILEDIALOG_OPENFOLDER, {}, default_location, title, on_idle);
}

void PlatformWindow::set_text_input(bool enabled) {
    if (!window_) return;
    if (enabled) {
        SDL_StartTextInput(window_);
    } else {
        SDL_StopTextInput(window_);
    }
}

bool PlatformWindow::is_text_input_active() const noexcept {
    return window_ && SDL_TextInputActive(window_);
}

void PlatformWindow::set_text_input_area(i32 x, i32 y, i32 w, i32 h, i32 cursor_offset) {
    if (!window_) return;
    const SDL_Rect rect{x, y, std::max(1, w), std::max(1, h)};
    SDL_SetTextInputArea(window_, &rect, cursor_offset);
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
