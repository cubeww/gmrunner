#include "extensions/shim_gm82joy.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gm {

namespace {

constexpr usize kMaxButtons = 256;
using ButtonEdges = std::array<u8, kMaxButtons>;
constexpr u8 kPressed = 1;
constexpr u8 kReleased = 2;

struct JoySlot {
    SDL_Joystick* handle = nullptr;
    SDL_JoystickID id = 0;
    ButtonEdges edges{};
};

class Gm82JoyShim final : public IDllShim {
public:
    ~Gm82JoyShim() override {
        close_all();
    }

    std::string_view canonical_name() const noexcept override { return "gm82joy"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "gm82joy" || normalized_dll == "sdl2";
    }

    bool has_function(std::string_view fn) const noexcept override {
        return fn == "SDL_GetError" || fn == "sdl_get_error" || fn.starts_with("joy_");
    }

    void on_unload(IExtensionHost&) override {
        close_all();
    }

    RValue invoke(
        IExtensionHost&,
        std::string_view fn,
        std::span<const RValue> args,
        ExternalValueType) override {
        if (fn == "SDL_GetError" || fn == "sdl_get_error") {
            const char* err = SDL_GetError();
            return RValue(std::string(err ? err : ""));
        }

        if (fn == "joy_init") {
            initialize();
            return RValue(0.0);
        }

        if (fn == "joy_update") {
            initialize();
            if (!initialized_) return RValue(0.0);
            bool changed = false;
            SDL_UpdateJoysticks();

            // Remove detached joysticks and compact slots to match gm82joy indexing.
            usize write_idx = 0;
            for (usize i = 0; i < slots_.size(); ++i) {
                if (!slots_[i].handle || !SDL_JoystickConnected(slots_[i].handle)) {
                    if (slots_[i].handle) {
                        SDL_CloseJoystick(slots_[i].handle);
                    }
                    changed = true;
                } else {
                    if (write_idx != i) {
                        slots_[write_idx] = std::move(slots_[i]);
                    }
                    ++write_idx;
                }
            }
            slots_.resize(write_idx);

            // Discover newly attached joysticks without polling SDL_PollEvent
            // (which would steal window and keyboard events from PlatformWindow).
            int avail_count = 0;
            SDL_JoystickID* avail_ids = SDL_GetJoysticks(&avail_count);
            if (avail_ids) {
                for (int i = 0; i < avail_count; ++i) {
                    const SDL_JoystickID jid = avail_ids[i];
                    bool already_open = false;
                    for (const auto& s : slots_) {
                        if (s.id == jid) {
                            already_open = true;
                            break;
                        }
                    }
                    if (already_open) continue;

                    SDL_Joystick* joy = SDL_OpenJoystick(jid);
                    if (!joy) continue;

                    JoySlot slot{};
                    slot.handle = joy;
                    slot.id = jid;
                    slots_.push_back(std::move(slot));
                    changed = true;
                }
                SDL_free(avail_ids);
            }

            SDL_PumpEvents();
            {
                std::scoped_lock lock(edges_mutex_);
                for (auto& slot : slots_) {
                    const auto it = pending_edges_.find(slot.id);
                    if (it == pending_edges_.end()) slot.edges.fill(0);
                    else slot.edges = it->second;
                }
                pending_edges_.clear();
            }

            return RValue(changed ? 1.0 : 0.0);
        }

        if (fn == "joy_count") {
            return RValue(static_cast<f64>(slots_.size()));
        }

        const i32 id = shim_i32(args, 0, -1);
        const bool valid_id = (id >= 0 && static_cast<usize>(id) < slots_.size() && slots_[static_cast<usize>(id)].handle != nullptr);

        if (fn == "joy_name") {
            if (!valid_id) return RValue(std::string{});
            const char* name = SDL_GetJoystickName(slots_[static_cast<usize>(id)].handle);
            return RValue(std::string(name ? name : ""));
        }
        if (fn == "joy_axes") {
            return RValue(valid_id ? static_cast<f64>(SDL_GetNumJoystickAxes(slots_[static_cast<usize>(id)].handle)) : -1.0);
        }
        if (fn == "joy_buttons") {
            return RValue(valid_id ? static_cast<f64>(SDL_GetNumJoystickButtons(slots_[static_cast<usize>(id)].handle)) : -1.0);
        }
        if (fn == "joy_hats") {
            return RValue(valid_id ? static_cast<f64>(SDL_GetNumJoystickHats(slots_[static_cast<usize>(id)].handle)) : -1.0);
        }
        if (fn == "joy_balls") {
            return RValue(valid_id ? static_cast<f64>(SDL_GetNumJoystickBalls(slots_[static_cast<usize>(id)].handle)) : -1.0);
        }
        if (fn == "joy_axis") {
            if (!valid_id) return RValue(0.0);
            const i32 axis = shim_i32(args, 1, 0);
            const f32 val = static_cast<f32>(SDL_GetJoystickAxis(slots_[static_cast<usize>(id)].handle, axis)) / 32768.0f;
            return RValue(static_cast<f64>(val));
        }
        if (fn == "joy_button") {
            if (!valid_id) return RValue(0.0);
            const i32 btn = shim_i32(args, 1, 0);
            return RValue(SDL_GetJoystickButton(slots_[static_cast<usize>(id)].handle, btn) ? 1.0 : 0.0);
        }
        if (fn == "joy_button_pressed") {
            const i32 btn = shim_i32(args, 1, -1);
            if (!valid_id || btn < 0 || static_cast<usize>(btn) >= kMaxButtons) return RValue(0.0);
            return RValue((slots_[static_cast<usize>(id)].edges[static_cast<usize>(btn)] & kPressed) != 0);
        }
        if (fn == "joy_button_released") {
            const i32 btn = shim_i32(args, 1, -1);
            if (!valid_id || btn < 0 || static_cast<usize>(btn) >= kMaxButtons) return RValue(0.0);
            return RValue((slots_[static_cast<usize>(id)].edges[static_cast<usize>(btn)] & kReleased) != 0);
        }
        if (fn == "joy_hat") {
            if (!valid_id) return RValue(-1.0);
            const Uint8 hat = SDL_GetJoystickHat(slots_[static_cast<usize>(id)].handle, shim_i32(args, 1, 0));
            switch (hat) {
                case SDL_HAT_UP:        return RValue(0.0);
                case SDL_HAT_RIGHTUP:   return RValue(45.0);
                case SDL_HAT_RIGHT:     return RValue(90.0);
                case SDL_HAT_RIGHTDOWN: return RValue(135.0);
                case SDL_HAT_DOWN:      return RValue(180.0);
                case SDL_HAT_LEFTDOWN:  return RValue(225.0);
                case SDL_HAT_LEFT:      return RValue(270.0);
                case SDL_HAT_LEFTUP:    return RValue(315.0);
                default:                return RValue(-1.0);
            }
        }
        if (fn == "joy_hat_x") {
            if (!valid_id) return RValue(0.0);
            const Uint8 hat = SDL_GetJoystickHat(slots_[static_cast<usize>(id)].handle, shim_i32(args, 1, 0));
            if (hat & SDL_HAT_LEFT) return RValue(-1.0);
            if (hat & SDL_HAT_RIGHT) return RValue(1.0);
            return RValue(0.0);
        }
        if (fn == "joy_hat_y") {
            if (!valid_id) return RValue(0.0);
            const Uint8 hat = SDL_GetJoystickHat(slots_[static_cast<usize>(id)].handle, shim_i32(args, 1, 0));
            if (hat & SDL_HAT_UP) return RValue(-1.0);
            if (hat & SDL_HAT_DOWN) return RValue(1.0);
            return RValue(0.0);
        }
        if (fn == "joy_ball_x" || fn == "joy_ball_y") {
            if (!valid_id) return RValue(0.0);
            int dx = 0;
            int dy = 0;
            SDL_GetJoystickBall(slots_[static_cast<usize>(id)].handle, shim_i32(args, 1, 0), &dx, &dy);
            return RValue(static_cast<f64>(fn == "joy_ball_x" ? dx : dy));
        }
        if (fn == "joy_rumble") {
            if (!valid_id) return RValue(-1.0);
            const auto strength = static_cast<Uint16>(std::clamp(shim_i32(args, 1, 0), 0, 65535));
            const auto len_ms = static_cast<Uint32>(std::max(0, shim_i32(args, 2, 0)));
            const bool ok = SDL_RumbleJoystick(slots_[static_cast<usize>(id)].handle, strength, strength, len_ms);
            return RValue(ok ? 0.0 : -1.0);
        }

        return RValue(0.0);
    }

private:
    // Watch joystick events without consuming the window's SDL event queue.
    // SDL may deliver these on a device thread, independently of joy_update.
    static bool SDLCALL watch_button(void* userdata, SDL_Event* ev) {
        if (ev->type != SDL_EVENT_JOYSTICK_BUTTON_DOWN && ev->type != SDL_EVENT_JOYSTICK_BUTTON_UP) return true;
        auto& self = *static_cast<Gm82JoyShim*>(userdata);
        std::scoped_lock lock(self.edges_mutex_);
        self.pending_edges_[ev->jbutton.which][ev->jbutton.button] |=
            ev->type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? kPressed : kReleased;
        return true;
    }

    void initialize() {
        if (initialized_ || !SDL_InitSubSystem(SDL_INIT_JOYSTICK)) return;
        SDL_SetJoystickEventsEnabled(true);
        initialized_ = SDL_AddEventWatch(watch_button, this);
        if (!initialized_) SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    }

    void close_all() {
        if (!initialized_) return;
        SDL_RemoveEventWatch(watch_button, this);
        for (auto& s : slots_) {
            if (s.handle) {
                SDL_CloseJoystick(s.handle);
                s.handle = nullptr;
            }
        }
        slots_.clear();
        pending_edges_.clear();
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        initialized_ = false;
    }

    bool initialized_ = false;
    std::vector<JoySlot> slots_;
    std::mutex edges_mutex_;
    std::unordered_map<SDL_JoystickID, ButtonEdges> pending_edges_;
};

} // namespace

std::unique_ptr<IDllShim> create_gm82joy_shim() {
    return std::make_unique<Gm82JoyShim>();
}

} // namespace gm
