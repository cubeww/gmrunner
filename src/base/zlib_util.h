#pragma once

#include "base/types.h"

#include <span>
#include <vector>

namespace gm {

// Decompresses a standard zlib-wrapped stream (matching GM8 ReadStreamC, .g3z, .wasd).
[[nodiscard]] Result<std::vector<u8>> zlib_decompress(std::span<const u8> compressed);

// Decompresses a standard zlib-wrapped stream requiring exact consumption (no unused trailing bytes)
// and enforcing a maximum output size (used when validating candidate decrypted payloads).
[[nodiscard]] Result<std::vector<u8>> zlib_decompress_exact(
    std::span<const u8> compressed,
    usize max_output_bytes);

// Compresses raw bytes into a zlib-wrapped stream.
[[nodiscard]] Result<std::vector<u8>> zlib_compress(std::span<const u8> raw, int level = 6);

// Raw deflate/inflate without zlib header (used by gm82buf).
[[nodiscard]] Result<std::vector<u8>> raw_inflate(std::span<const u8> compressed);
[[nodiscard]] Result<std::vector<u8>> raw_deflate(std::span<const u8> raw, int level = 6);

} // namespace gm
