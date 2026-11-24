#include "loader/antidecompiler.h"

#include "loader/crypto.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace gm {

namespace {

constexpr u32 kMode2StubXor = 0x2E2E2E2Eu;

inline u32 rd_u32(std::span<const u8> b, usize off) noexcept {
    return static_cast<u32>(b[off]) |
           (static_cast<u32>(b[off + 1u]) << 8u) |
           (static_cast<u32>(b[off + 2u]) << 16u) |
           (static_cast<u32>(b[off + 3u]) << 24u);
}

inline void wr_u32(std::span<u8> b, usize off, u32 v) noexcept {
    b[off + 0u] = static_cast<u8>(v & 0xFFu);
    b[off + 1u] = static_cast<u8>((v >> 8u) & 0xFFu);
    b[off + 2u] = static_cast<u8>((v >> 16u) & 0xFFu);
    b[off + 3u] = static_cast<u8>((v >> 24u) & 0xFFu);
}

inline constexpr u32 bswap32(u32 v) noexcept {
    return ((v & 0x000000FFu) << 24u) |
           ((v & 0x0000FF00u) << 8u) |
           ((v & 0x00FF0000u) >> 8u) |
           ((v & 0xFF000000u) >> 24u);
}

// gm8.dat injected decryption stub (canonical file offset 0x32266, 226 code bytes).
// Variable 4-byte immediates (XOR 0x2E2E2E2E): PAD={0x43, 0x54, 0xBD}, KEY_B=0x6D, KEY_A=0x72, KEY_D=0x7E.
constexpr std::array<u8, 226> kGm80StubTemplate = {
    0x8Bu, 0x0Du, 0x78u, 0x34u, 0x59u, 0x00u, 0x33u, 0xC0u, 0x85u, 0xC9u, 0x0Fu, 0x84u, 0x67u, 0x01u, 0x00u, 0x00u,
    0x39u, 0x05u, 0x7Cu, 0x34u, 0x59u, 0x00u, 0x0Fu, 0x85u, 0x5Bu, 0x01u, 0x00u, 0x00u, 0x51u, 0xE8u, 0xE0u, 0x50u,
    0xFDu, 0xFFu, 0x33u, 0xC0u, 0xBAu, 0xB8u, 0x33u, 0x59u, 0x00u, 0xB1u, 0x2Fu, 0x89u, 0x02u, 0x83u, 0xC2u, 0x04u,
    0xFEu, 0xC9u, 0x75u, 0xF7u, 0x83u, 0xC2u, 0x04u, 0x89u, 0x02u, 0xE9u, 0x39u, 0x01u, 0x00u, 0x00u, 0x1Du, 0xEEu,
    0x7Eu, 0x7Eu, 0x46u, 0x00u, 0x00u, 0x00u, 0x00u, 0xD1u, 0x5Au, 0x0Au, 0x22u, 0xC6u, 0x30u, 0xCAu, 0xD2u, 0xD1u,
    0xAFu, 0x42u, 0x0Au, 0x26u, 0x00u, 0x00u, 0x00u, 0x00u, 0xC6u, 0x5Bu, 0x7Cu, 0xD3u, 0xD1u, 0x7Eu, 0xA5u, 0x7Au,
    0x0Au, 0xDAu, 0xA5u, 0x62u, 0x0Au, 0xD6u, 0xADu, 0xC4u, 0x2Au, 0xA5u, 0x2Au, 0x3Fu, 0x1Bu, 0x00u, 0x00u, 0x00u,
    0x00u, 0x2Bu, 0x00u, 0x00u, 0x00u, 0x00u, 0x21u, 0xE6u, 0xAFu, 0x03u, 0xFDu, 0x00u, 0x6Du, 0x2Eu, 0x00u, 0x00u,
    0x00u, 0x00u, 0xA7u, 0x2Au, 0x3Fu, 0x8Fu, 0xF6u, 0x00u, 0x6Du, 0x2Eu, 0x21u, 0xE6u, 0x6Eu, 0x8Du, 0xF6u, 0x00u,
    0x6Du, 0x2Eu, 0xADu, 0xC7u, 0x2Au, 0x5Bu, 0xFCu, 0xE8u, 0x2Bu, 0x02u, 0x83u, 0x6Eu, 0x2Eu, 0xC6u, 0xE9u, 0x2Bu,
    0x03u, 0x83u, 0x6Eu, 0x2Eu, 0x29u, 0xFAu, 0xD1u, 0xD1u, 0x76u, 0x79u, 0x97u, 0x32u, 0x2Eu, 0x2Eu, 0x2Eu, 0x91u,
    0x8Au, 0x00u, 0x6Du, 0x2Eu, 0xDDu, 0x85u, 0x71u, 0xA5u, 0x62u, 0x0Au, 0xD6u, 0xAFu, 0x2Fu, 0x00u, 0x00u, 0x00u,
    0x00u, 0xC7u, 0x2Bu, 0x50u, 0xD3u, 0xD1u, 0x33u, 0xC9u, 0xB1u, 0x88u, 0x80u, 0xB1u, 0xA3u, 0x2Eu, 0x43u, 0x00u,
    0x2Eu, 0xE2u, 0xF7u, 0xC7u, 0x05u, 0x2Eu, 0x2Fu, 0x43u, 0x00u, 0x6Au, 0x90u, 0x90u, 0x68u, 0xE9u, 0x5Cu, 0xFFu,
    0xFFu, 0xFFu,
};

// gm8_1_91.dat injected decryption stub (canonical file offset 0x46211, 214 bytes).
// Variable 4-byte immediates (XOR 0x2E2E2E2E): PAD={0x2E, 0x44, 0x5D}, KEY_A=0x63, KEY_B=0x72, KEY_D=0x82.
constexpr std::array<u8, 214> kGm81StubTemplate = {
    0xB9u, 0x2Fu, 0x00u, 0x00u, 0x00u, 0x89u, 0x81u, 0xC8u, 0x1Fu, 0x65u, 0x00u, 0x49u, 0x75u, 0xF7u, 0xE9u, 0x38u,
    0x01u, 0x00u, 0x00u, 0xE9u, 0x81u, 0x00u, 0x00u, 0x00u, 0xE9u, 0x2Bu, 0x26u, 0x73u, 0x6Fu, 0x2Eu, 0xC6u, 0x4Du,
    0x5Au, 0xD1u, 0xE8u, 0x2Bu, 0x22u, 0x73u, 0x6Fu, 0x2Eu, 0xD1u, 0x44u, 0x2Fu, 0x44u, 0x2Eu, 0x46u, 0x00u, 0x00u,
    0x00u, 0x00u, 0xD1u, 0x5Au, 0x0Au, 0x22u, 0x95u, 0x1Au, 0x0Bu, 0x6Eu, 0x2Eu, 0xD1u, 0xFDu, 0xADu, 0xC5u, 0x36u,
    0xAFu, 0x42u, 0x0Au, 0x26u, 0x00u, 0x00u, 0x00u, 0x00u, 0xA5u, 0x52u, 0x0Au, 0x26u, 0xA5u, 0x5Au, 0x0Au, 0x2Au,
    0x2Fu, 0xD9u, 0xADu, 0xC1u, 0x2Au, 0xD1u, 0xFDu, 0xA5u, 0x62u, 0x0Au, 0xD6u, 0xAFu, 0x2Fu, 0x00u, 0x00u, 0x00u,
    0x00u, 0x7Eu, 0x97u, 0x00u, 0x00u, 0x00u, 0x00u, 0xBEu, 0xBEu, 0xBEu, 0xBEu, 0xBEu, 0xBEu, 0xBEu, 0xBEu, 0xA5u,
    0x29u, 0x1Bu, 0x00u, 0x00u, 0x00u, 0x00u, 0x2Fu, 0xE6u, 0x21u, 0xE6u, 0xA7u, 0x29u, 0xAFu, 0x03u, 0xADu, 0x40u,
    0x6Au, 0x2Eu, 0x00u, 0x00u, 0x00u, 0x00u, 0x21u, 0xE7u, 0x6Fu, 0xADu, 0xC1u, 0x2Au, 0x17u, 0xD9u, 0x5Du, 0xF1u,
    0x9Eu, 0xE2u, 0x97u, 0x94u, 0x2Eu, 0x2Eu, 0x2Eu, 0xC5u, 0x03u, 0xE8u, 0x03u, 0x00u, 0x00u, 0x00u, 0x89u, 0xEBu,
    0x10u, 0x48u, 0xEBu, 0x01u, 0x3Du, 0x01u, 0x04u, 0x24u, 0x48u, 0xEBu, 0x01u, 0x8Bu, 0x29u, 0x04u, 0x24u, 0xC3u,
    0x89u, 0xB8u, 0x28u, 0x6Eu, 0x44u, 0x00u, 0xB9u, 0x81u, 0x00u, 0x00u, 0x00u, 0x80u, 0x34u, 0x08u, 0x2Eu, 0xE2u,
    0xFAu, 0xE9u, 0x52u, 0xFFu, 0xFFu, 0xFFu, 0xFDu, 0xE8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x5Fu, 0xF3u, 0xAAu, 0xFCu,
    0x58u, 0xE9u, 0x26u, 0xEEu, 0xFCu, 0xFFu,
};

struct StubDescriptor {
    AntiDecompilerProfile profile;
    usize canonical_offset;
    std::span<const u8> templ;
    std::array<usize, 3> pad_offsets;
    usize key_a_offset;
    usize key_b_offset;
    usize key_d_offset;
};

constexpr std::array<StubDescriptor, 2> kStubDescriptors = {{
    {
        AntiDecompilerProfile::GM80,
        0x32266u,
        kGm80StubTemplate,
        {0x43u, 0x54u, 0xBDu},
        0x72u,
        0x6Du,
        0x7Eu,
    },
    {
        AntiDecompilerProfile::GM81,
        0x46211u,
        kGm81StubTemplate,
        {0x2Eu, 0x44u, 0x5Du},
        0x63u,
        0x72u,
        0x82u,
    },
}};

bool is_wildcard_byte(const StubDescriptor& desc, usize rel) noexcept {
    auto in_dword = [rel](usize base) noexcept {
        return rel >= base && rel < base + 4u;
    };
    return in_dword(desc.pad_offsets[0]) ||
           in_dword(desc.pad_offsets[1]) ||
           in_dword(desc.pad_offsets[2]) ||
           in_dword(desc.key_a_offset) ||
           in_dword(desc.key_b_offset) ||
           in_dword(desc.key_d_offset);
}

std::optional<AntiDecompilerStubInfo> match_stub_at(
    std::span<const u8> exe,
    usize offset,
    const StubDescriptor& desc) noexcept {
    if (offset > exe.size() || desc.templ.size() > exe.size() - offset) {
        return std::nullopt;
    }
    for (usize i = 0; i < desc.templ.size(); ++i) {
        if (is_wildcard_byte(desc, i)) {
            continue;
        }
        if (exe[offset + i] != desc.templ[i]) {
            return std::nullopt;
        }
    }

    const u32 pad0 = rd_u32(exe, offset + desc.pad_offsets[0]) ^ kMode2StubXor;
    const u32 pad1 = rd_u32(exe, offset + desc.pad_offsets[1]) ^ kMode2StubXor;
    const u32 pad2 = rd_u32(exe, offset + desc.pad_offsets[2]) ^ kMode2StubXor;
    if (pad0 != pad1 || pad1 != pad2 || pad0 > 0xFF8u || (pad0 % 8u) != 0u) {
        return std::nullopt;
    }

    return AntiDecompilerStubInfo{
        desc.profile,
        offset,
        rd_u32(exe, offset + desc.key_a_offset) ^ kMode2StubXor,
        rd_u32(exe, offset + desc.key_b_offset) ^ kMode2StubXor,
        rd_u32(exe, offset + desc.key_d_offset) ^ kMode2StubXor,
        pad0,
    };
}

} // namespace

void decrypt_antidecompiler_mode2(std::span<u8> ciphertext, u32 key_a, u32 key_b, u32 key_d) noexcept {
    u32 a = key_a;
    u32 b = key_b;
    for (usize end = ciphertext.size(); end >= 4u; end -= 4u) {
        const usize pos = end - 4u;
        const u32 c = rd_u32(ciphertext, pos);
        const u32 p = bswap32((c ^ b) + a);
        wr_u32(ciphertext, pos, p);
        b -= key_d;
        a = bswap32(a) + 1u;
    }
}

std::optional<AntiDecompilerStubInfo> detect_antidecompiler_stub(
    std::span<const u8> exe,
    usize pe_overlay_offset) noexcept {
    const usize search_limit = (pe_overlay_offset > 0u && pe_overlay_offset <= exe.size())
                                   ? pe_overlay_offset
                                   : exe.size();

    for (const auto& desc : kStubDescriptors) {
        // Fast check at canonical file offset.
        if (auto info = match_stub_at(exe, desc.canonical_offset, desc)) {
            return info;
        }
        // Fallback scan within the PE image using the invariant 16-byte prefix.
        if (search_limit < desc.templ.size()) {
            continue;
        }
        const auto anchor = desc.templ.first(16u);
        const auto begin_it = exe.begin();
        const auto end_it = exe.begin() + static_cast<std::ptrdiff_t>(search_limit - desc.templ.size() + 16u);
        auto it = begin_it;
        while (it < end_it) {
            it = std::search(it, end_it, anchor.begin(), anchor.end());
            if (it == end_it) {
                break;
            }
            const usize pos = static_cast<usize>(it - begin_it);
            if (auto info = match_stub_at(exe, pos, desc)) {
                return info;
            }
            ++it;
        }
    }

    return std::nullopt;
}

Result<AntiDecompilerResult> try_decrypt_antidecompiler(
    std::vector<u8>& exe,
    usize pe_overlay_offset) {
    auto stub_opt = detect_antidecompiler_stub(exe, pe_overlay_offset);
    if (!stub_opt) {
        return Error{"AntiDecompiler stub not detected"};
    }
    const AntiDecompilerStubInfo stub = stub_opt.value();

    if (stub.profile == AntiDecompilerProfile::GM80) {
        std::vector<usize> candidates;
        auto add_candidate = [&](u64 raw_off) {
            if (raw_off >= exe.size()) {
                return;
            }
            const usize off = static_cast<usize>(raw_off);
            if (exe.size() - off < 32u || ((exe.size() - off) % 4u) != 0u) {
                return;
            }
            if (std::find(candidates.begin(), candidates.end(), off) == candidates.end()) {
                candidates.push_back(off);
            }
        };

        // 1. Canonical gm8.dat prefix (0x192200 + 0x1E5800 - 0x1A8A00 = 0x1CF000) + padding.
        add_candidate(0x1CF000ull + stub.padding);
        // 2. Actual PE overlay offset + padding (covers resized .rsrc sections).
        if (pe_overlay_offset > 0u) {
            add_candidate(static_cast<u64>(pe_overlay_offset) + stub.padding);
        }
        // 3. Runner read-base immediate at 0x144AC0 (uncompressed custom offsets).
        if (exe.size() >= 0x144AC4u) {
            add_candidate(static_cast<u64>(rd_u32(exe, 0x144AC0u)) + stub.padding);
        }
        // 4. Scan-shifted GM 8.0 layouts (original payload at 2,000,000 + k*10,000 -> 0x1D1C80 + k*10,000).
        for (u64 k = 1; k <= 64u; ++k) {
            const u64 cand = 0x1D1C80ull + k * 10000ull + stub.padding;
            if (cand >= exe.size()) {
                break;
            }
            add_candidate(cand);
        }

        for (const usize off : candidates) {
            std::vector<u8> payload(exe.begin() + static_cast<std::ptrdiff_t>(off), exe.end());
            decrypt_antidecompiler_mode2(payload, stub.key_a, stub.key_b, stub.key_d);
            if (!validate_gm8_payload_structure(payload, /*check_gm81_prelude=*/false)) {
                continue;
            }

            // Reconstruct the 16-byte GM 8.0 header randomized before encryption (debug=0).
            wr_u32(payload, 0u, 1234321u);
            wr_u32(payload, 4u, 800u);
            wr_u32(payload, 8u, 0u);
            wr_u32(payload, 12u, 800u);

            std::copy(payload.begin(), payload.end(), exe.begin() + static_cast<std::ptrdiff_t>(off));
            return AntiDecompilerResult{
                AntiDecompilerProfile::GM80,
                /*is_gm81_family=*/false,
                off,
                off,
            };
        }

        return Error{"AntiDecompiler GM 8.0 stub detected, but no candidate decrypted to a valid payload"};
    }

    // GM 8.1 (gm8_1_91.dat): reverse-decrypt from EOF down to PE overlay boundary so any
    // pre-magic scan shift (even non-multiple of 4) keeps exact DWORD alignment with EOF.
    const usize decrypt_floor = (pe_overlay_offset > 0u && pe_overlay_offset < exe.size())
                                    ? pe_overlay_offset
                                    : 0u;
    if (exe.size() - decrypt_floor < 32u) {
        return Error{"AntiDecompiler GM 8.1 payload too small"};
    }

    std::vector<u8> outer_decrypted = exe;
    decrypt_antidecompiler_mode2(
        std::span<u8>(outer_decrypted).subspan(decrypt_floor),
        stub.key_a,
        stub.key_b,
        stub.key_d);

    // AntiDecompiler aligns ciphertext length up to a multiple of 4 by appending 0..3 bytes.
    // Try trimming 0..3 trailing bytes so native GM 8.1 WAD CRC-32 matches the original length.
    for (usize trim = 0; trim <= 3u; ++trim) {
        if (trim > 0u) {
            if (outer_decrypted.size() <= decrypt_floor + 32u) {
                break;
            }
            outer_decrypted.pop_back();
        }
        auto gm81_res = try_decrypt_gm81_wad(outer_decrypted, decrypt_floor);
        if (gm81_res) {
            // Preserve the original PE + pre-magic region, committing only the decrypted WAD.
            const usize magic_off = gm81_res.value().magic_offset;
            exe.resize(outer_decrypted.size());
            std::copy(outer_decrypted.begin() + static_cast<std::ptrdiff_t>(magic_off),
                      outer_decrypted.end(),
                      exe.begin() + static_cast<std::ptrdiff_t>(magic_off));
            return AntiDecompilerResult{
                AntiDecompilerProfile::GM81,
                /*is_gm81_family=*/true,
                gm81_res.value().header_offset,
                magic_off,
            };
        }
    }

    return Error{"AntiDecompiler GM 8.1 stub detected, but inner GM 8.1 WAD verification failed"};
}

} // namespace gm
