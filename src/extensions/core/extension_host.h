#pragma once

#include "audio/audio_engine.h"
#include "base/types.h"
#include "gml/value.h"
#include "graphics/gl_graphics_device.h"
#include "graphics/renderer2d.h"
#include "loader/game_data.h"
#include "platform/vfs.h"
#include "platform/window.h"

#include <span>
#include <string>
#include <vector>

namespace gm {

// Clean host interface implemented by gm_runtime::Runner so gm_extensions stays
// decoupled from gm_runtime with zero circular CMake dependencies.
class IExtensionHost {
public:
    virtual ~IExtensionHost() = default;

    [[nodiscard]] virtual Vfs& ext_vfs() noexcept = 0;
    [[nodiscard]] virtual AudioEngine& ext_audio() noexcept = 0;
    [[nodiscard]] virtual const GameData& ext_game() const noexcept = 0;
    [[nodiscard]] virtual PlatformWindow& ext_window() noexcept = 0;
    [[nodiscard]] virtual GlGraphicsDevice& ext_gfx() noexcept = 0;
    [[nodiscard]] virtual Renderer2D& ext_renderer() noexcept = 0;

    // Helper for FMODSnapShotToDsList / extensions writing into GM ds_list handles.
    virtual void ext_ds_list_add_real(i32 list_id, f64 value) = 0;

    // A module can invoke a GML built-in obtained with get_function_address.
    // The caller supplies the self/other context, as in the runner callback ABI.
    virtual Result<RValue> ext_call_function(i32 address, i32 self, i32 other,
                                             std::span<const RValue> args) = 0;

    // GM 8.2 runner internal hooks (gm82core terrible_gm8_hacking.c).
    [[nodiscard]] virtual bool ext_error_is_enabled() const noexcept = 0;
    virtual void ext_error_set_enabled(bool enabled) noexcept = 0;
    virtual void ext_io_set_roomend_clear(bool enabled) noexcept = 0;
    virtual void ext_window_set_fixed(bool enabled) noexcept = 0;
    virtual void ext_resize_backbuffer(i32 width, i32 height) = 0;
    [[nodiscard]] virtual i32 ext_game_get_state() const noexcept = 0;
    virtual void ext_room_goto_cancel() noexcept = 0;
    virtual void ext_game_end() noexcept = 0;

    // GM 8.2 variable reflection hooks (gm82test inst_meta.c).
    [[nodiscard]] virtual i32 ext_var_name_count() const noexcept = 0;
    [[nodiscard]] virtual std::string ext_var_name_get(i32 index) const = 0;
    [[nodiscard]] virtual std::vector<std::string> ext_instance_var_names(i32 inst_id) const = 0;
};

} // namespace gm
