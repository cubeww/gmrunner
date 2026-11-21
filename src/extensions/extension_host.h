#pragma once

#include "audio/audio_engine.h"
#include "base/types.h"
#include "gml/value.h"
#include "loader/game_data.h"
#include "platform/vfs.h"
#include "platform/window.h"

#include <span>

namespace gm {

// Clean host interface implemented by gm_runtime::Runner so gm_extensions stays
// cleanly decoupled from gm_runtime with zero circular CMake dependencies.
class IExtensionHost {
public:
    virtual ~IExtensionHost() = default;

    [[nodiscard]] virtual Vfs& ext_vfs() noexcept = 0;
    [[nodiscard]] virtual AudioEngine& ext_audio() noexcept = 0;
    [[nodiscard]] virtual const GameData& ext_game() const noexcept = 0;
    [[nodiscard]] virtual PlatformWindow& ext_window() noexcept = 0;

    // Helper for FMODSnapShotToDsList / extensions writing into GM ds_list handles.
    virtual void ext_ds_list_add_real(i32 list_id, f64 value) = 0;

    // A shim can invoke a GML built-in obtained with get_function_address.
    // The caller supplies the self/other context, as in the runner callback ABI.
    virtual Result<RValue> ext_call_function(i32 address, i32 self, i32 other,
                                             std::span<const RValue> args) = 0;
};

} // namespace gm
