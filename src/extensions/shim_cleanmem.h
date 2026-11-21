#pragma once

#include "extensions/dll_registry.h"

#include <memory>

namespace gm {

[[nodiscard]] std::unique_ptr<IDllShim> create_cleanmem_shim();

} // namespace gm
