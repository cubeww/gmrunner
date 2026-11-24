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
    bool used_sudalv = false;
};

struct SudalvRunnerInfo {
    usize scan_start = 0;
    usize pe_overlay = 0;
    usize local_random_offset = 0;
    usize helper_offset = 0;
    bool crc_failure_patched = false;
};

// Validates decrypted GM 8.0 / 8.1 / 8.2 payload structure without executing GML:
// checks optional GM 8.1 prelude, exact zlib decompression of options and embedded
// D3DX8.dll (MZ header), and StreamE swap-table bijection & payload bounds.
[[nodiscard]] bool validate_gm8_payload_structure(
    std::span<const u8> payload,
    bool check_gm81_prelude) noexcept;

// Detects a GM 8.1 PE32 runner patched with the SUDALV dynamic-multiplier helper.
[[nodiscard]] std::optional<SudalvRunnerInfo> detect_sudalv_runner(std::span<const u8> exe) noexcept;

// Extracts the SUDALV WORD multiplier table backwards from `magic_offset - 4` down to `lower_bound`
// until the DWORD zero sentinel (inclusive).
[[nodiscard]] Result<std::vector<u16>> read_sudalv_multipliers(
    std::span<const u8> data,
    usize magic_offset,
    usize lower_bound = 0);

// Applies the SUDALV dynamic-multiplier MWC XOR stream in-place to a GM 8.1 WAD body.
[[nodiscard]] Result<void> xor_sudalv_wad(
    std::span<u8> payload,
    u32 seed_w,
    u32 seed_z,
    std::span<const u16> multipliers);

// Scans for GM 8.1 / 8.2 interleaved magic 0xF7140067, unmasks header word,
// decrypts the outer WAD via UTF-16LE CRC-32 + Marsaglia MWC PRNG (or SUDALV dynamic MWC
// when the known runner patches are detected), and verifies CRC-32 / payload structure.
[[nodiscard]] Result<Gm81DecryptResult> try_decrypt_gm81_wad(std::vector<u8>& exe, usize start_hint);

// Scans for GM 8.0 plain magic 1234321 followed by version 800.
// Returns byte offset of the magic integer (1234321) if found.
[[nodiscard]] std::optional<usize> find_gm80_payload(std::span<const u8> exe, usize pe_overlay_offset) noexcept;

// Decrypts GM 8.0 / 8.1 / 8.2 inner StreamE payload (CStream.ReadStreamE).
[[nodiscard]] Result<std::vector<u8>> decrypt_stream_e(BinaryReader& reader);

// Decrypts GM7 / Extension swap-cipher payload in-place (CStream.Decrypt).
void decrypt_gm7_swap(i32 key, std::span<u8> buf, usize start_pos) noexcept;

} // namespace gm
