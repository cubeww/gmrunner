#pragma once

#include "extensions/dll_registry.h"

#include <memory>

namespace gm {

[[nodiscard]] std::unique_ptr<IDllShim> create_gmfmod_shim();

} // namespace gm
