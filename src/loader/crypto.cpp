#include "loader/crypto.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/zlib_util.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace gm {

namespace {

inline u16 rd_u16(std::span<const u8> b, usize off) noexcept {
    return static_cast<u16>(static_cast<u16>(b[off]) | (static_cast<u16>(b[off + 1]) << 8u));
}

inline u32 rd_u32(std::span<const u8> b, usize off) noexcept {
    return static_cast<u32>(b[off]) |
           (static_cast<u32>(b[off + 1]) << 8u) |
           (static_cast<u32>(b[off + 2]) << 16u) |
           (static_cast<u32>(b[off + 3]) << 24u);
}

inline i32 rd_i32(std::span<const u8> b, usize off) noexcept {
    return static_cast<i32>(rd_u32(b, off));
}

inline void wr_u32(std::span<u8> b, usize off, u32 v) noexcept {
    b[off + 0] = static_cast<u8>(v & 0xFFu);
    b[off + 1] = static_cast<u8>((v >> 8u) & 0xFFu);
    b[off + 2] = static_cast<u8>((v >> 16u) & 0xFFu);
    b[off + 3] = static_cast<u8>((v >> 24u) & 0xFFu);
}

inline bool has_bytes(usize total, usize off, usize len) noexcept {
    return off <= total && len <= total - off;
}

constexpr std::array<u8, 18> kSudalvLocalRandomTail = {
    0x5Au, 0x8Bu, 0x02u, 0xC1u, 0xE0u, 0x10u, 0x8Bu, 0x11u, 0x81u,
    0xE2u, 0xFFu, 0xFFu, 0x00u, 0x00u, 0x01u, 0xD0u, 0x5Bu, 0xC3u,
};

constexpr std::array<u8, 83> kSudalvHelperCode = {
    0x57u, 0x8Bu, 0x58u, 0x04u, 0x89u, 0xDFu, 0x83u, 0xC7u, 0x20u, 0x8Bu, 0x1Fu, 0x83u, 0xFBu, 0x00u, 0x75u, 0x0Eu,
    0x8Bu, 0x58u, 0x04u, 0x03u, 0x58u, 0x0Cu, 0x83u, 0xEBu, 0x0Cu, 0x89u, 0x1Fu, 0x89u, 0x5Fu, 0x08u, 0x50u,
    0x8Bu, 0x02u, 0x25u, 0xFFu, 0xFFu, 0x00u, 0x00u, 0x8Bu, 0x1Fu, 0x51u, 0x8Bu, 0x0Bu, 0x83u, 0xF9u, 0x00u, 0x75u, 0x07u,
    0x8Bu, 0x5Fu, 0x08u, 0x89u, 0x1Fu, 0xEBu, 0x05u, 0x83u, 0xC3u, 0xFEu, 0x89u, 0x1Fu, 0x81u, 0xE1u, 0xFFu, 0xFFu, 0x00u, 0x00u,
    0x52u, 0xF7u, 0xE9u, 0x5Au, 0x59u, 0x8Bu, 0x1Au, 0xC1u, 0xEBu, 0x10u, 0x01u, 0xD8u, 0x89u, 0x02u, 0x58u, 0x5Fu, 0xC3u,
};

constexpr std::array<u8, 8> kGm81MagicCompare = {
    0x81u, 0x7Du, 0xECu, 0x67u, 0x00u, 0x14u, 0xF7u, 0x74u,
};

constexpr std::array<u8, 10> kSudalvCrcBypass = {
    0x3Bu, 0x45u, 0xF8u, 0x74u, 0x05u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u,
};

std::optional<usize> find_pattern(
    std::span<const u8> haystack,
    usize start,
    usize end,
    std::span<const u8> needle) noexcept {
    if (needle.empty() || start >= end || end > haystack.size() || end - start < needle.size()) {
        return std::nullopt;
    }
    const auto begin_it = haystack.begin() + static_cast<std::ptrdiff_t>(start);
    const auto end_it = haystack.begin() + static_cast<std::ptrdiff_t>(end);
    const auto it = std::search(begin_it, end_it, needle.begin(), needle.end());
    if (it == end_it) {
        return std::nullopt;
    }
    return static_cast<usize>(it - haystack.begin());
}

bool match_sudalv_local_random(
    std::span<const u8> exe,
    usize local,
    usize sec_start,
    usize sec_end,
    usize& out_helper) noexcept {
    if (local < sec_start || sec_end < local || sec_end - local < 32u) {
        return false;
    }
    if (exe[local] != 0x53u || exe[local + 1u] != 0xE8u) {
        return false;
    }
    if (exe[local + 6u] != 0x52u || exe[local + 7u] != 0x89u ||
        exe[local + 8u] != 0xCAu || exe[local + 9u] != 0xE8u) {
        return false;
    }
    if (std::memcmp(exe.data() + local + 14u, kSudalvLocalRandomTail.data(), kSudalvLocalRandomTail.size()) != 0) {
        return false;
    }

    const i64 rel1 = rd_i32(exe, local + 2u);
    const i64 rel2 = rd_i32(exe, local + 10u);
    const i64 target1 = static_cast<i64>(local) + 6 + rel1;
    const i64 target2 = static_cast<i64>(local) + 14 + rel2;
    if (target1 != target2 || target1 < static_cast<i64>(sec_start)) {
        return false;
    }
    const usize helper = static_cast<usize>(target1);
    if (helper > sec_end || sec_end - helper < kSudalvHelperCode.size()) {
        return false;
    }
    if (std::memcmp(exe.data() + helper, kSudalvHelperCode.data(), kSudalvHelperCode.size()) != 0) {
        return false;
    }
    out_helper = helper;
    return true;
}

std::optional<Gm81DecryptResult> try_decrypt_gm81_sudalv(
    std::vector<u8>& exe,
    const SudalvRunnerInfo& runner) {
    constexpr u32 kInterleavedMagic = 0xF7140067u;
    const usize scan_start = std::max(runner.scan_start, runner.pe_overlay);
    if (scan_start >= exe.size() || exe.size() - scan_start < 20u) {
        return std::nullopt;
    }

    for (usize i = scan_start; i + 20u <= exe.size(); ++i) {
        const u32 a = rd_u32(exe, i);
        const u32 b = rd_u32(exe, i + 4u);
        const u32 check = (a & 0xFF00FF00u) | (b & 0x00FF00FFu);
        if (check != kInterleavedMagic) {
            continue;
        }

        const u32 mask = (b & 0xFF00FF00u) | (a & 0x00FF00FFu);
        const usize wad_base = i + 8u;
        const usize version_off = i + 16u;
        const usize payload_off = i + 20u;
        if (payload_off >= exe.size()) {
            continue;
        }

        const u32 unmasked_ver = rd_u32(exe, version_off) ^ mask;
        if (unmasked_ver != 810u) {
            continue;
        }

        auto mult_res = read_sudalv_multipliers(exe, i, runner.pe_overlay);
        if (!mult_res) {
            continue;
        }

        const u32 seed1 = rd_u32(exe, wad_base);
        const u32 stored_crc = rd_u32(exe, wad_base + 4u);
        const std::string passphrase = "_MJD" + std::to_string(seed1) + "#RWK";
        const u32 seed_w = crc32_gm81_utf16le(passphrase);

        std::vector<u8> payload(exe.begin() + static_cast<std::ptrdiff_t>(payload_off), exe.end());
        if (!xor_sudalv_wad(payload, seed_w, stored_crc, mult_res.value())) {
            continue;
        }

        const u32 actual_crc = crc32_gm81(payload);
        if (actual_crc != stored_crc && !runner.crc_failure_patched) {
            continue;
        }

        if (!validate_gm8_payload_structure(payload, /*check_gm81_prelude=*/true)) {
            continue;
        }

        wr_u32(exe, version_off, unmasked_ver);
        std::copy(payload.begin(), payload.end(), exe.begin() + static_cast<std::ptrdiff_t>(payload_off));
        return Gm81DecryptResult{version_off, i, /*used_sudalv=*/true};
    }

    return std::nullopt;
}

} // namespace

usize find_pe_overlay_offset(std::span<const u8> exe) noexcept {
    if (exe.size() < 0x40 || exe[0] != 'M' || exe[1] != 'Z') {
        return 0;
    }
    const u32 pe_off = rd_u32(exe, 0x3C);
    if (!has_bytes(exe.size(), pe_off, 24u)) {
        return 0;
    }
    if (exe[pe_off] != 'P' || exe[pe_off + 1] != 'E' || exe[pe_off + 2] != 0 || exe[pe_off + 3] != 0) {
        return 0;
    }

    const u16 num_sections = rd_u16(exe, pe_off + 6);
    const u16 opt_hdr_size = rd_u16(exe, pe_off + 20);
    const u64 sec_table_off = static_cast<u64>(pe_off) + 24u + static_cast<u64>(opt_hdr_size);

    usize end_pe = 0;
    for (u16 i = 0; i < num_sections; ++i) {
        const u64 s = sec_table_off + static_cast<u64>(i) * 40u;
        if (s > exe.size() || exe.size() - static_cast<usize>(s) < 40u) {
            break;
        }
        const usize s_off = static_cast<usize>(s);
        const u32 raw_size = rd_u32(exe, s_off + 16u);
        const u32 raw_ptr = rd_u32(exe, s_off + 20u);
        if (raw_size > 0) {
            const u64 sec_end = static_cast<u64>(raw_ptr) + static_cast<u64>(raw_size);
            if (sec_end > end_pe && sec_end <= exe.size()) {
                end_pe = static_cast<usize>(sec_end);
            }
        }
    }
    return end_pe;
}

bool validate_gm8_payload_structure(
    std::span<const u8> payload,
    bool check_gm81_prelude) noexcept {
    constexpr usize kMaxZlibBytes = 128u * 1024u * 1024u;
    if (payload.size() < 24u) {
        return false;
    }

    if (check_gm81_prelude) {
        const u32 dbg = rd_u32(payload, 4u);
        const u32 opt_ver = rd_u32(payload, 12u);
        if (dbg > 1u || opt_ver < 800u || opt_ver > 830u) {
            return false;
        }
    }

    const u32 options_len = rd_u32(payload, 16u);
    if (options_len < 2u || !has_bytes(payload.size(), 20u, options_len)) {
        return false;
    }
    auto opt_res = zlib_decompress_exact(payload.subspan(20u, options_len), kMaxZlibBytes);
    if (!opt_res || opt_res.value().size() < 64u) {
        return false;
    }

    usize pos = 20u + static_cast<usize>(options_len);
    if (!has_bytes(payload.size(), pos, 4u)) {
        return false;
    }
    const u32 name_len = rd_u32(payload, pos);
    pos += 4u;
    if (name_len > 260u || !has_bytes(payload.size(), pos, name_len)) {
        return false;
    }
    const std::string_view dll_name(reinterpret_cast<const char*>(payload.data() + pos), name_len);
    pos += name_len;
    if (name_len > 0u && ascii_to_lower(dll_name) != "d3dx8.dll") {
        return false;
    }

    if (!has_bytes(payload.size(), pos, 4u)) {
        return false;
    }
    const u32 dll_len = rd_u32(payload, pos);
    pos += 4u;
    if (!has_bytes(payload.size(), pos, dll_len)) {
        return false;
    }
    if (name_len == 0u && dll_len != 0u) {
        return false;
    }
    if (dll_len > 0u) {
        auto dll_res = zlib_decompress_exact(payload.subspan(pos, dll_len), kMaxZlibBytes);
        if (!dll_res || dll_res.value().size() < 2u ||
            dll_res.value()[0] != 'M' || dll_res.value()[1] != 'Z') {
            return false;
        }
        pos += dll_len;
    }

    if (!has_bytes(payload.size(), pos, 8u)) {
        return false;
    }
    const u32 n1 = rd_u32(payload, pos);
    const u32 n2 = rd_u32(payload, pos + 4u);
    pos += 8u;
    if (n1 > 10000000u || n2 > 10000000u) {
        return false;
    }
    const u64 junk1_bytes = static_cast<u64>(n1) * 4u;
    if (junk1_bytes > payload.size() - pos) {
        return false;
    }
    pos += static_cast<usize>(junk1_bytes);

    if (!has_bytes(payload.size(), pos, 256u)) {
        return false;
    }
    std::array<u8, 256> seen{};
    for (usize i = 0; i < 256u; ++i) {
        const u8 b = payload[pos + i];
        if (seen[b] != 0u) {
            return false;
        }
        seen[b] = 1u;
    }
    pos += 256u;

    const u64 junk2_bytes = static_cast<u64>(n2) * 4u;
    if (junk2_bytes > payload.size() - pos) {
        return false;
    }
    pos += static_cast<usize>(junk2_bytes);

    if (!has_bytes(payload.size(), pos, 4u)) {
        return false;
    }
    const u32 stream_e_len = rd_u32(payload, pos);
    pos += 4u;
    if (stream_e_len == 0u || !has_bytes(payload.size(), pos, stream_e_len)) {
        return false;
    }

    return true;
}

std::optional<SudalvRunnerInfo> detect_sudalv_runner(std::span<const u8> exe) noexcept {
    if (exe.size() < 0x40 || exe[0] != 'M' || exe[1] != 'Z') {
        return std::nullopt;
    }
    const u32 pe = rd_u32(exe, 0x3Cu);
    if (!has_bytes(exe.size(), pe, 26u)) {
        return std::nullopt;
    }
    if (exe[pe] != 'P' || exe[pe + 1u] != 'E' || exe[pe + 2u] != 0u || exe[pe + 3u] != 0u) {
        return std::nullopt;
    }
    if (rd_u16(exe, pe + 4u) != 0x14Cu || rd_u16(exe, pe + 24u) != 0x10Bu) {
        return std::nullopt;
    }

    const u16 section_count = rd_u16(exe, pe + 6u);
    const u16 opt_hdr_size = rd_u16(exe, pe + 20u);
    const u64 table_off = static_cast<u64>(pe) + 24u + static_cast<u64>(opt_hdr_size);
    if (section_count == 0u || section_count > 96u ||
        table_off > exe.size() ||
        static_cast<u64>(section_count) * 40u > exe.size() - static_cast<usize>(table_off)) {
        return std::nullopt;
    }

    struct SecRange {
        usize start = 0;
        usize end = 0;
    };
    std::vector<SecRange> sections;
    sections.reserve(section_count);
    usize pe_overlay = 0;
    for (u16 i = 0; i < section_count; ++i) {
        const usize off = static_cast<usize>(table_off) + static_cast<usize>(i) * 40u;
        const u32 raw_size = rd_u32(exe, off + 16u);
        const u32 raw_ptr = rd_u32(exe, off + 20u);
        const u64 sec_end = static_cast<u64>(raw_ptr) + static_cast<u64>(raw_size);
        if (sec_end > exe.size()) {
            return std::nullopt;
        }
        if (raw_size > 0u) {
            sections.push_back(SecRange{raw_ptr, static_cast<usize>(sec_end)});
            if (sec_end > pe_overlay) {
                pe_overlay = static_cast<usize>(sec_end);
            }
        }
    }
    if (pe_overlay == 0u || pe_overlay >= exe.size()) {
        return std::nullopt;
    }

    // 1. Locate patched CStream.LocalRandom and verify its 83-byte multiplier helper.
    usize local_offset = 0;
    usize helper_offset = 0;
    SecRange text_sec{};
    bool found_local = false;

    // Fast path: canonical GM 8.1.141 offset 0x10BB74.
    constexpr usize kCanonicalLocal = 0x10BB74u;
    for (const auto& sec : sections) {
        if (sec.start <= kCanonicalLocal && kCanonicalLocal < sec.end) {
            if (match_sudalv_local_random(exe, kCanonicalLocal, sec.start, sec.end, helper_offset)) {
                local_offset = kCanonicalLocal;
                text_sec = sec;
                found_local = true;
            }
            break;
        }
    }

    // General path: scan executable PE sections for the exact patched LocalRandom signature.
    if (!found_local) {
        for (const auto& sec : sections) {
            usize cursor = sec.start;
            while (auto hit = find_pattern(exe, cursor, sec.end, kSudalvLocalRandomTail)) {
                if (hit.value() >= sec.start + 14u) {
                    const usize cand = hit.value() - 14u;
                    if (match_sudalv_local_random(exe, cand, sec.start, sec.end, helper_offset)) {
                        local_offset = cand;
                        text_sec = sec;
                        found_local = true;
                        break;
                    }
                }
                cursor = hit.value() + 1u;
            }
            if (found_local) {
                break;
            }
        }
    }

    if (!found_local) {
        return std::nullopt;
    }

    // 2. Locate GM 8.1 interleaved magic compare and extract the runner's actual scan start.
    usize scan_start = 0;
    bool found_scan = false;

    constexpr usize kCanonicalMovScan = 0x226CF3u;
    constexpr usize kCanonicalMagicCmp = 0x226D7Cu;
    constexpr std::array<u8, 8> kCanonicalMovPrefix = {
        0xE8u, 0x80u, 0xF2u, 0xDDu, 0xFFu, 0xC7u, 0x45u, 0xF0u,
    };
    if (text_sec.start <= kCanonicalMovScan &&
        kCanonicalMagicCmp + kGm81MagicCompare.size() <= text_sec.end &&
        std::memcmp(exe.data() + kCanonicalMovScan, kCanonicalMovPrefix.data(), kCanonicalMovPrefix.size()) == 0 &&
        std::memcmp(exe.data() + kCanonicalMagicCmp, kGm81MagicCompare.data(), kGm81MagicCompare.size()) == 0) {
        scan_start = rd_u32(exe, kCanonicalMovScan + 8u);
        found_scan = true;
    } else if (auto cmp_pos = find_pattern(exe, text_sec.start, text_sec.end, kGm81MagicCompare)) {
        const usize search_from = (cmp_pos.value() > text_sec.start + 0xB0u)
                                      ? (cmp_pos.value() - 0xB0u)
                                      : text_sec.start;
        constexpr std::array<u8, 3> kMovEbpF0 = {0xC7u, 0x45u, 0xF0u};
        usize cursor = search_from;
        while (auto mov_pos = find_pattern(exe, cursor, cmp_pos.value(), kMovEbpF0)) {
            if (mov_pos.value() >= text_sec.start + 5u &&
                exe[mov_pos.value() - 5u] == 0xE8u &&
                mov_pos.value() + 7u <= cmp_pos.value()) {
                scan_start = rd_u32(exe, mov_pos.value() + 3u);
                found_scan = true;
                break;
            }
            cursor = mov_pos.value() + 1u;
        }
    }

    if (!found_scan || scan_start < 0x100000u || !has_bytes(exe.size(), scan_start, 20u)) {
        return std::nullopt;
    }

    // 3. Check whether CStream.DecryptWad's CRC-failure Halt0 call was NOPed out.
    bool crc_patched = false;
    constexpr usize kCanonicalCrcCheck = 0x10BD46u;
    if (local_offset == kCanonicalLocal &&
        kCanonicalCrcCheck + kSudalvCrcBypass.size() <= text_sec.end) {
        crc_patched = std::memcmp(exe.data() + kCanonicalCrcCheck,
                                  kSudalvCrcBypass.data(),
                                  kSudalvCrcBypass.size()) == 0;
    } else {
        const usize search_end = std::min(text_sec.end, local_offset + 0x400u);
        crc_patched = find_pattern(exe, local_offset, search_end, kSudalvCrcBypass).has_value();
    }

    return SudalvRunnerInfo{
        scan_start,
        pe_overlay,
        local_offset,
        helper_offset,
        crc_patched,
    };
}

Result<std::vector<u16>> read_sudalv_multipliers(
    std::span<const u8> data,
    usize magic_offset,
    usize lower_bound) {
    if (magic_offset < 4u || magic_offset > data.size() || magic_offset - 4u < lower_bound) {
        return Error{"SUDALV: multiplier cursor starts outside bounds"};
    }
    constexpr usize kMaxMultipliers = 1u * 1024u * 1024u;
    usize cursor = magic_offset - 4u; // FPosition (= magic + 8) - 12
    std::vector<u16> values;
    while (cursor >= lower_bound && has_bytes(data.size(), cursor, 4u)) {
        if (values.size() >= kMaxMultipliers) {
            return Error{"SUDALV: multiplier table exceeds safety limit"};
        }
        const u32 value = rd_u32(data, cursor);
        values.push_back(static_cast<u16>(value & 0xFFFFu));
        if (value == 0u) {
            return values;
        }
        if (cursor < lower_bound + 2u) {
            break;
        }
        cursor -= 2u;
    }
    return Error{"SUDALV: multiplier table has no DWORD zero sentinel within bounds"};
}

Result<void> xor_sudalv_wad(
    std::span<u8> payload,
    u32 seed_w,
    u32 seed_z,
    std::span<const u16> multipliers) {
    if (multipliers.empty()) {
        return Error{"SUDALV: empty multiplier table"};
    }
    u32 z = seed_z;
    u32 w = seed_w;
    usize idx = 0;
    const usize count = multipliers.size();
    const usize xor_start = static_cast<usize>(seed_w & 0xFFu) + 6u;
    for (usize pos = xor_start; pos + 5u < payload.size(); pos += 4u) {
        const u32 mz = multipliers[idx];
        if (++idx == count) idx = 0;
        const u32 mw = multipliers[idx];
        if (++idx == count) idx = 0;
        z = mz * (z & 0xFFFFu) + (z >> 16u);
        w = mw * (w & 0xFFFFu) + (w >> 16u);
        const u32 mask32 = (z << 16u) + (w & 0xFFFFu);
        wr_u32(payload, pos, rd_u32(payload, pos) ^ mask32);
    }
    return Result<void>::success();
}

Result<Gm81DecryptResult> try_decrypt_gm81_wad(std::vector<u8>& exe, usize start_hint) {
    if (exe.size() < 32) {
        return Error{"Executable too small for GM 8.1/8.2 WAD"};
    }

    // Check for explicitly recognized SUDALV GM 8.1 runner patches first so we avoid
    // scanning obsolete headers and wasting work on standard MWC when SUDALV is present.
    // If SUDALV validation fails (e.g. a normalized analysis file), fall through safely.
    if (auto sudalv = detect_sudalv_runner(exe)) {
        if (auto res = try_decrypt_gm81_sudalv(exe, sudalv.value())) {
            return res.value();
        }
    }

    // GM 8.1 scans from 3,800,004; GM 8.2 scans from 3,174,000; PE overlay start may be slightly earlier.
    usize scan_start = start_hint;
    if (scan_start > exe.size()) {
        scan_start = 0;
    }

    constexpr u32 kInterleavedMagic = 0xF7140067u;
    usize found = std::string::npos;

    for (usize i = scan_start; i + 20 <= exe.size(); ++i) {
        const u32 a = rd_u32(exe, i);
        const u32 b = rd_u32(exe, i + 4);
        const u32 check = (a & 0xFF00FF00u) | (b & 0x00FF00FFu);
        if (check != kInterleavedMagic) {
            continue;
        }

        // Verify candidate by computing mask, unmasking +16, and checking CRC after Marsaglia MWC.
        const u32 mask = (b & 0xFF00FF00u) | (a & 0x00FF00FFu);
        const usize wad_base = i + 8;       // seed1 at +0, expected_crc at +4, version at +8, payload at +12
        const usize version_off = i + 16;   // wad_base + 8
        const usize payload_off = wad_base + 12;

        if (payload_off >= exe.size()) {
            continue;
        }

        const u32 unmasked_ver = rd_u32(exe, version_off) ^ mask;
        // GM 8.1 / 8.2 version DWORD is 810 (0x32A).
        if (unmasked_ver != 810u) {
            continue;
        }

        const u32 seed1 = rd_u32(exe, wad_base);
        const u32 expected_crc = rd_u32(exe, wad_base + 4);
        const std::string passphrase = "_MJD" + std::to_string(seed1) + "#RWK";
        const u32 s1_init = crc32_gm81_utf16le(passphrase);

        // Decrypt payload into a copy first so a false positive magic never corrupts the buffer.
        const usize payload_len = exe.size() - payload_off;
        std::vector<u8> payload(exe.begin() + static_cast<std::ptrdiff_t>(payload_off), exe.end());

        MarsagliaMwc prng(s1_init, expected_crc);
        const usize xor_start = static_cast<usize>(s1_init & 0xFFu) + 6u;
        for (usize pos = xor_start; pos + 5 < payload_len; pos += 4) {
            const u32 word = rd_u32(payload, pos) ^ prng.next();
            wr_u32(payload, pos, word);
        }

        const u32 actual_crc = crc32_gm81(payload);
        if (actual_crc == expected_crc) {
            // Commit unmasked version + decrypted payload in place.
            wr_u32(exe, version_off, unmasked_ver);
            std::copy(payload.begin(), payload.end(), exe.begin() + static_cast<std::ptrdiff_t>(payload_off));
            found = i;
            return Gm81DecryptResult{version_off, found, /*used_sudalv=*/false};
        }
    }

    return Error{"GM 8.1/8.2 interleaved magic 0xF7140067 with valid CRC not found"};
}

std::optional<usize> find_gm80_payload(std::span<const u8> exe, usize pe_overlay_offset) noexcept {
    constexpr u32 kGmMagic = 1234321u;
    constexpr u32 kGm80Ver = 800u;

    auto matches_at = [&](usize off) noexcept -> bool {
        return off + 8 <= exe.size() && rd_u32(exe, off) == kGmMagic && rd_u32(exe, off + 4) == kGm80Ver;
    };

    // Fast canonical offsets first: exact PE overlay end, 1,980,000, and 2,000,000 + k*10,000.
    if (pe_overlay_offset > 0 && matches_at(pe_overlay_offset)) {
        return pe_overlay_offset;
    }
    if (matches_at(1980000u)) {
        return 1980000u;
    }
    for (usize off = 2000000u; off + 8 <= exe.size(); off += 10000u) {
        if (matches_at(off)) {
            return off;
        }
    }

    // Resilient fallback: byte scan from PE overlay start (handles custom icons / repacked stubs).
    const usize scan_start = pe_overlay_offset > 0 ? pe_overlay_offset : 0;
    for (usize off = scan_start; off + 8 <= exe.size(); ++off) {
        if (matches_at(off)) {
            return off;
        }
    }

    return std::nullopt;
}

Result<std::vector<u8>> decrypt_stream_e(BinaryReader& reader) {
    auto n1_res = reader.read_i32();
    if (!n1_res) return Error{"StreamE: failed reading n1: " + n1_res.error_message()};
    auto n2_res = reader.read_i32();
    if (!n2_res) return Error{"StreamE: failed reading n2: " + n2_res.error_message()};

    const i32 n1 = n1_res.value();
    const i32 n2 = n2_res.value();
    if (n1 < 0 || n2 < 0) {
        return Error{"StreamE: negative junk integer count"};
    }

    if (auto s = reader.skip(static_cast<usize>(n1) * 4u); !s) {
        return Error{"StreamE: failed skipping n1 junk ints"};
    }

    auto tbl_span = reader.read_span(256);
    if (!tbl_span) return Error{"StreamE: failed reading 256-byte swap table"};

    std::array<u8, 256> tbl{};
    std::array<u8, 256> inv{};
    std::copy(tbl_span.value().begin(), tbl_span.value().end(), tbl.begin());
    for (usize k = 0; k < 256; ++k) {
        inv[tbl[k]] = static_cast<u8>(k);
    }

    if (auto s = reader.skip(static_cast<usize>(n2) * 4u); !s) {
        return Error{"StreamE: failed skipping n2 junk ints"};
    }

    auto len_res = reader.read_i32();
    if (!len_res) return Error{"StreamE: failed reading payload length"};
    const i32 len = len_res.value();
    if (len < 0) {
        return Error{"StreamE: negative payload length"};
    }

    auto buf_res = reader.read_bytes(static_cast<usize>(len));
    if (!buf_res) return Error{"StreamE: failed reading encrypted payload bytes"};
    std::vector<u8> buf = std::move(buf_res.value());

    // Pass 1 (reverse, i = len-1 down to 1): buf[i] = (inv[buf[i]] - buf[i-1] - i) mod 256
    for (usize i = buf.size(); i > 1; --i) {
        const usize idx = i - 1;
        const i32 val = (static_cast<i32>(inv[buf[idx]]) - static_cast<i32>(buf[idx - 1]) - static_cast<i32>(idx)) & 0xFF;
        buf[idx] = static_cast<u8>(val);
    }

    // Pass 2 (reverse, i = len-1 down to 0): swap(buf[i], buf[max(0, i - tbl[i % 256])])
    for (usize i = buf.size(); i > 0; --i) {
        const usize idx = i - 1;
        const usize shift = tbl[idx & 0xFFu];
        const usize target = (idx > shift) ? (idx - shift) : 0u;
        std::swap(buf[idx], buf[target]);
    }

    return buf;
}

void decrypt_gm7_swap(i32 key, std::span<u8> buf, usize start_pos) noexcept {
    i32 a = 6 + (key % 250);
    if (a < 0) a += 256;
    i32 b = key / 250;
    if (b < 0) b += 256;

    std::array<u8, 256> map1{};
    std::array<u8, 256> map2{};
    for (u32 i = 0; i < 256; ++i) {
        map1[i] = static_cast<u8>(i);
        map2[i] = static_cast<u8>(i);
    }

    for (i32 k = 1; k <= 10000; ++k) {
        const usize j = static_cast<usize>(1 + ((k * a + b) % 254));
        std::swap(map1[j], map1[j + 1]);
    }
    for (u32 i = 1; i < 256; ++i) {
        map2[map1[i]] = static_cast<u8>(i);
    }

    for (usize pos = start_pos; pos < buf.size(); ++pos) {
        buf[pos] = map2[buf[pos]];
    }
}

} // namespace gm
