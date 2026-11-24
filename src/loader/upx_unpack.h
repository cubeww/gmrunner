#pragma once

#include "base/types.h"

#include <span>
#include <vector>

namespace gm {

inline constexpr usize kMaxUpxImageSize = 512u * 1024u * 1024u;

// Decompresses a UPX NRV2B (method=2), NRV2D (method=5), or NRV2E (method=8) LE32 stream.
[[nodiscard]] Result<std::vector<u8>> decompress_upx_nrv(
    std::span<const u8> compressed,
    usize expected_size,
    u8 method);

// Fast check for a UPX-packed x86 PE32 executable (UPX0/UPX1/rsrc + "UPX!" pack header).
[[nodiscard]] bool is_upx_packed_pe(std::span<const u8> data) noexcept;

// Statically unpacks a supported UPX PE32 executable (header v12/13, format 9,
// NRV2B/2D/2E LE32, x86 CTO filters 0/0x24/0x25/0x26), reconstructing imports,
// resources, and original PE sections while preserving the entire overlay.
[[nodiscard]] Result<std::vector<u8>> unpack_upx_pe(std::span<const u8> data);

} // namespace gm
