#pragma once

#include "base/types.h"

#include <optional>
#include <span>
#include <vector>

namespace gm {

enum class AntiDecompilerProfile : u8 {
    GM80 = 0, // gm8.dat (mode 2, destructive 16-byte header)
    GM81 = 1, // gm8_1_91.dat (mode 2, preserves GM 8.1 WAD header)
};

struct AntiDecompilerStubInfo {
    AntiDecompilerProfile profile = AntiDecompilerProfile::GM80;
    usize stub_offset = 0;
    u32 key_a = 0;
    u32 key_b = 0;
    u32 key_d = 0;
    u32 padding = 0;
};

struct AntiDecompilerResult {
    AntiDecompilerProfile profile = AntiDecompilerProfile::GM80;
    bool is_gm81_family = false;
    usize header_offset = 0;
    usize payload_offset = 0;
};

// Decrypts an AntiDecompiler mode-2 ciphertext span in-place from its end backwards
// in 4-byte steps (aligned to the end of the span).
void decrypt_antidecompiler_mode2(std::span<u8> ciphertext, u32 key_a, u32 key_b, u32 key_d) noexcept;

// Scans the PE runner for a known AntiDecompiler decryption stub (GM 8.0 or GM 8.1)
// and extracts the XOR-masked key/padding immediates.
[[nodiscard]] std::optional<AntiDecompilerStubInfo> detect_antidecompiler_stub(
    std::span<const u8> exe,
    usize pe_overlay_offset) noexcept;

// Attempts to recognize and decrypt an AntiDecompiler-protected GM 8.0 or GM 8.1 executable.
// Leaves `exe` unmodified if no recognized stub is present or validation fails.
[[nodiscard]] Result<AntiDecompilerResult> try_decrypt_antidecompiler(
    std::vector<u8>& exe,
    usize pe_overlay_offset);

} // namespace gm
