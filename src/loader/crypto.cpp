#include "loader/crypto.h"

#include "base/hash.h"

#include <algorithm>
#include <array>
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

inline void wr_u32(std::span<u8> b, usize off, u32 v) noexcept {
    b[off + 0] = static_cast<u8>(v & 0xFFu);
    b[off + 1] = static_cast<u8>((v >> 8u) & 0xFFu);
    b[off + 2] = static_cast<u8>((v >> 16u) & 0xFFu);
    b[off + 3] = static_cast<u8>((v >> 24u) & 0xFFu);
}

} // namespace

usize find_pe_overlay_offset(std::span<const u8> exe) noexcept {
    if (exe.size() < 0x40 || exe[0] != 'M' || exe[1] != 'Z') {
        return 0;
    }
    const u32 pe_off = rd_u32(exe, 0x3C);
    if (static_cast<usize>(pe_off) + 24 > exe.size()) {
        return 0;
    }
    if (exe[pe_off] != 'P' || exe[pe_off + 1] != 'E' || exe[pe_off + 2] != 0 || exe[pe_off + 3] != 0) {
        return 0;
    }

    const u16 num_sections = rd_u16(exe, pe_off + 6);
    const u16 opt_hdr_size = rd_u16(exe, pe_off + 20);
    const usize sec_table_off = static_cast<usize>(pe_off) + 24u + static_cast<usize>(opt_hdr_size);

    usize end_pe = 0;
    for (u16 i = 0; i < num_sections; ++i) {
        const usize s = sec_table_off + static_cast<usize>(i) * 40u;
        if (s + 40u > exe.size()) {
            break;
        }
        const u32 raw_size = rd_u32(exe, s + 16u);
        const u32 raw_ptr = rd_u32(exe, s + 20u);
        if (raw_size > 0) {
            const usize sec_end = static_cast<usize>(raw_ptr) + static_cast<usize>(raw_size);
            if (sec_end > end_pe && sec_end <= exe.size()) {
                end_pe = sec_end;
            }
        }
    }
    return end_pe;
}

Result<Gm81DecryptResult> try_decrypt_gm81_wad(std::vector<u8>& exe, usize start_hint) {
    if (exe.size() < 32) {
        return Error{"Executable too small for GM 8.1/8.2 WAD"};
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
            return Gm81DecryptResult{version_off, found};
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
