#pragma once

#include "base/types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

struct WasdEntry {
    std::string filename;
    std::vector<u8> data;
};

// Detects whether a byte buffer carries the GMFMODSimple "FMODSIMPLEPW" trailer.
[[nodiscard]] bool is_fmodsimple_encrypted(std::span<const u8> data) noexcept;

// Decrypts a GMFMODSimple password-protected audio buffer using SHA-1(pw) / SHA-1(reverse(pw)).
// If the buffer is not encrypted, copies it unchanged. Returns Error if the password hash mismatches.
[[nodiscard]] Result<std::vector<u8>> decrypt_fmodsimple(
    std::span<const u8> data,
    std::string_view password);

// Encrypts raw audio bytes into the exact GMFMODSimple "FMODSIMPLEPW" format.
[[nodiscard]] Result<std::vector<u8>> encrypt_fmodsimple(
    std::span<const u8> raw,
    std::string_view password);

// Unpacks a gm82snd WASD1.0 zlib sound pack into individual named audio payloads.
[[nodiscard]] Result<std::vector<WasdEntry>> unpack_wasd_archive(std::span<const u8> archive_bytes);

} // namespace gm
