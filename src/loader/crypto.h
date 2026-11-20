#pragma once

#include "base/binary_reader.h"
#include "base/types.h"

#include <optional>
#include <span>
#include <vector>

namespace gm {

// Parses PE32 headers to find the byte offset immediately after the last PE section.
// Returns 0 if the buffer is not a valid PE image.
[[nodiscard]] usize find_pe_overlay_offset(std::span<const u8> exe) noexcept;

struct Gm81DecryptResult {
    usize header_offset = 0; // Offset of the unmasked version int32 (810)
    usize magic_offset = 0;  // Offset where 0xF7140067 interleaved magic was found
};

// Scans for GM 8.1 / 8.2 interleaved magic 0xF7140067, unmasks header word,
// decrypts the outer WAD via UTF-16LE CRC-32 + Marsaglia MWC PRNG, and verifies CRC-32.
[[nodiscard]] Result<Gm81DecryptResult> try_decrypt_gm81_wad(std::vector<u8>& exe, usize start_hint);

// Scans for GM 8.0 plain magic 1234321 followed by version 800.
// Returns byte offset of the magic integer (1234321) if found.
[[nodiscard]] std::optional<usize> find_gm80_payload(std::span<const u8> exe, usize pe_overlay_offset) noexcept;

// Decrypts GM 8.0 / 8.1 / 8.2 inner StreamE payload (CStream.ReadStreamE).
[[nodiscard]] Result<std::vector<u8>> decrypt_stream_e(BinaryReader& reader);

// Decrypts GM7 / Extension swap-cipher payload in-place (CStream.Decrypt).
void decrypt_gm7_swap(i32 key, std::span<u8> buf, usize start_pos) noexcept;

} // namespace gm
