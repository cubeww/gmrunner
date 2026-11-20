#pragma once

#include "base/types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace gm {

// Reflected CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, NO final XOR),
// matching GM 8.1 / 8.2 CStream.CalcCRC used in DecryptWad.
[[nodiscard]] u32 crc32_gm81(std::span<const u8> data) noexcept;

// Hashes an ASCII string as UTF-16LE (lo, 0x00 per char) via crc32_gm81,
// matching GM 8.1 / 8.2 passphrase hashing ("_MJD" + seed + "#RWK").
[[nodiscard]] u32 crc32_gm81_utf16le(std::string_view ascii) noexcept;

// Standard IEEE CRC-32 (with final XOR 0xFFFFFFFF), used by gm82buf.
[[nodiscard]] u32 crc32_ieee(std::span<const u8> data) noexcept;

// Marsaglia Multiply-With-Carry PRNG matching GM 8.1 / 8.2 CStream.LocalRandom.
struct MarsagliaMwc {
    u32 w = 0; // s1 (passphrase CRC)
    u32 z = 0; // s2 (expected CRC)

    constexpr MarsagliaMwc(u32 seed_w, u32 seed_z) noexcept : w(seed_w), z(seed_z) {}

    [[nodiscard]] constexpr u32 next() noexcept {
        z = 36969u * (z & 0xFFFFu) + (z >> 16u);
        w = 18000u * (w & 0xFFFFu) + (w >> 16u);
        return (z << 16u) + (w & 0xFFFFu);
    }
};

using Md5Digest = std::array<u8, 16>;
using Sha1Digest = std::array<u8, 20>;

[[nodiscard]] Md5Digest md5_bytes(std::span<const u8> data) noexcept;
[[nodiscard]] Sha1Digest sha1_bytes(std::span<const u8> data) noexcept;

[[nodiscard]] std::string hex_encode(std::span<const u8> bytes, bool uppercase = false);

} // namespace gm
