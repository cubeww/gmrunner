#pragma once

#include "extensions/buffer_store.h"
#include "extensions/dll_registry.h"

#include <memory>

namespace gm {

[[nodiscard]] std::unique_ptr<IDllShim> create_gm82dx_shim(std::shared_ptr<GmBufferStore> buffers);

} // namespace gm
