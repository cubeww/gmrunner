#include "extensions/core/extension.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <string>
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

class Gm82JoyExtension final : public IExtension {
public:
    ~Gm82JoyExtension() override {
        close_all();
    }

    std::string_view canonical_name() const noexcept override { return "gm82joy"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_aliases({"gm82joy", "sdl2"});

        reg.bind({"SDL_GetError", "sdl_get_error"}, []() {
            const char* err = SDL_GetError();
            return std::string(err ? err : "");
        });

        reg.bind("joy_init", [this]() {
            initialize();
            return 0.0;
        });
        reg.bind<&Gm82JoyExtension::joy_update>(this, "joy_update");
        reg.bind("joy_count", [this]() {
            return static_cast<f64>(slots_.size());
        });

        reg.bind("joy_name", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            if (!h) return RValue(std::string{});
            const char* name = SDL_GetJoystickName(h);
            return RValue(std::string(name ? name : ""));
        });
        reg.bind("joy_axes", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            return RValue(h ? static_cast<f64>(SDL_GetNumJoystickAxes(h)) : -1.0);
        });
        reg.bind("joy_buttons", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            return RValue(h ? static_cast<f64>(SDL_GetNumJoystickButtons(h)) : -1.0);
        });
        reg.bind("joy_hats", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            return RValue(h ? static_cast<f64>(SDL_GetNumJoystickHats(h)) : -1.0);
        });
        reg.bind("joy_balls", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            return RValue(h ? static_cast<f64>(SDL_GetNumJoystickBalls(h)) : -1.0);
        });
        reg.bind("joy_axis", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            if (!h) return RValue(0.0);
            const i32 axis = ctx.i32_arg(1, 0);
            const f32 val = static_cast<f32>(SDL_GetJoystickAxis(h, axis)) / 32768.0f;
            return RValue(static_cast<f64>(val));
        });
        reg.bind("joy_button", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            if (!h) return RValue(0.0);
            return RValue(SDL_GetJoystickButton(h, ctx.i32_arg(1, 0)) ? 1.0 : 0.0);
        });
        reg.bind("joy_button_pressed", [this](const CallContext& ctx) {
            return check_button_edge(ctx, kPressed);
        });
        reg.bind("joy_button_released", [this](const CallContext& ctx) {
            return check_button_edge(ctx, kReleased);
        });
        reg.bind<&Gm82JoyExtension::joy_hat>(this, "joy_hat");
        reg.bind("joy_hat_x", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            if (!h) return RValue(0.0);
            const Uint8 hat = SDL_GetJoystickHat(h, ctx.i32_arg(1, 0));
            if (hat & SDL_HAT_LEFT) return RValue(-1.0);
            if (hat & SDL_HAT_RIGHT) return RValue(1.0);
            return RValue(0.0);
        });
        reg.bind("joy_hat_y", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            if (!h) return RValue(0.0);
            const Uint8 hat = SDL_GetJoystickHat(h, ctx.i32_arg(1, 0));
            if (hat & SDL_HAT_UP) return RValue(-1.0);
            if (hat & SDL_HAT_DOWN) return RValue(1.0);
            return RValue(0.0);
        });
        reg.bind({"joy_ball_x", "joy_ball_y"}, [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            if (!h) return RValue(0.0);
            int dx = 0;
            int dy = 0;
            SDL_GetJoystickBall(h, ctx.i32_arg(1, 0), &dx, &dy);
            return RValue(static_cast<f64>(ctx.func_name == "joy_ball_x" ? dx : dy));
        });
        reg.bind("joy_rumble", [this](const CallContext& ctx) {
            SDL_Joystick* h = joy_handle(ctx);
            if (!h) return RValue(-1.0);
            const auto strength = static_cast<Uint16>(std::clamp(ctx.i32_arg(1, 0), 0, 65535));
            const auto len_ms = static_cast<Uint32>(std::max(0, ctx.i32_arg(2, 0)));
            const bool ok = SDL_RumbleJoystick(h, strength, strength, len_ms);
            return RValue(ok ? 0.0 : -1.0);
        });
    }

    void on_unload(IExtensionHost&) override {
        close_all();
    }

private:
    [[nodiscard]] JoySlot* joy_slot(const CallContext& ctx) noexcept {
        const i32 id = ctx.i32_arg(0, -1);
        if (id < 0 || static_cast<usize>(id) >= slots_.size()) return nullptr;
        auto& slot = slots_[static_cast<usize>(id)];
        return slot.handle ? &slot : nullptr;
    }

    [[nodiscard]] SDL_Joystick* joy_handle(const CallContext& ctx) noexcept {
        auto* slot = joy_slot(ctx);
        return slot ? slot->handle : nullptr;
    }

    [[nodiscard]] RValue check_button_edge(const CallContext& ctx, u8 mask) noexcept {
        auto* slot = joy_slot(ctx);
        const i32 btn = ctx.i32_arg(1, -1);
        if (!slot || btn < 0 || static_cast<usize>(btn) >= kMaxButtons) return RValue(0.0);
        return RValue((slot->edges[static_cast<usize>(btn)] & mask) != 0);
    }

    f64 joy_update() {
        initialize();
        if (!initialized_) return 0.0;
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

        return changed ? 1.0 : 0.0;
    }

    RValue joy_hat(const CallContext& ctx) {
        SDL_Joystick* h = joy_handle(ctx);
        if (!h) return RValue(-1.0);
        const Uint8 hat = SDL_GetJoystickHat(h, ctx.i32_arg(1, 0));
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

    // Watch joystick events without consuming the window's SDL event queue.
    // SDL may deliver these on a device thread, independently of joy_update.
    static bool SDLCALL watch_button(void* userdata, SDL_Event* ev) {
        if (ev->type != SDL_EVENT_JOYSTICK_BUTTON_DOWN && ev->type != SDL_EVENT_JOYSTICK_BUTTON_UP) return true;
        auto& self = *static_cast<Gm82JoyExtension*>(userdata);
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

std::unique_ptr<IExtension> create_gm82joy_extension() {
    return std::make_unique<Gm82JoyExtension>();
}

} // namespace gm
