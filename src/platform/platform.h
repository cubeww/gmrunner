#pragma once

#include "base/types.h"

namespace gm {

class Platform {
public:
    [[nodiscard]] static Result<void> init();
    static void shutdown() noexcept;
    [[nodiscard]] static bool is_initialized() noexcept;
    [[nodiscard]] static const char* platform_name() noexcept;
};

} // namespace gm
