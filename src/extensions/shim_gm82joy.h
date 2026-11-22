#pragma once

#include "extensions/dll_registry.h"

#include <memory>

namespace gm {

[[nodiscard]] std::unique_ptr<IDllShim> create_gm82joy_shim();

} // namespace gm
