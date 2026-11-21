#include "audio/audio_crypto.h"

#include "base/binary_reader.h"
#include "base/hash.h"
#include "base/zlib_util.h"

#include <algorithm>
#include <cstring>

namespace gm {

namespace {

constexpr std::string_view kFmodMagic = "FMODSIMPLEPW";
constexpr usize kFmodHashLen = 40;
constexpr usize kFmodTrailerLen = kFmodHashLen + kFmodMagic.size(); // 52 bytes
constexpr usize kFmodTotalOverhead = kFmodTrailerLen + 1;            // +1 trailing EOF byte written by FMODEncryptFile

std::string sha1_upper_hex(std::string_view text) {
    const Sha1Digest d = sha1_bytes(std::span<const u8>(
        reinterpret_cast<const u8*>(text.data()), text.size()));
    return hex_encode(d, true);
}

} // namespace

bool is_fmodsimple_encrypted(std::span<const u8> data) noexcept {
    if (data.size() < kFmodTotalOverhead) {
        return false;
    }
    const u8* tail = data.data() + (data.size() - kFmodMagic.size());
    return std::memcmp(tail, kFmodMagic.data(), kFmodMagic.size()) == 0;
}

Result<std::vector<u8>> decrypt_fmodsimple(
    std::span<const u8> data,
    std::string_view password) {
    if (!is_fmodsimple_encrypted(data)) {
        return std::vector<u8>(data.begin(), data.end());
    }

    const std::string hash1 = sha1_upper_hex(password);
    const usize hash_offset = data.size() - kFmodTrailerLen;
    if (std::memcmp(data.data() + hash_offset, hash1.data(), kFmodHashLen) != 0) {
        return Error{"FMODSIMPLEPW password hash mismatch"};
    }

    std::string rev_pw(password.rbegin(), password.rend());
    const std::string hash2 = sha1_upper_hex(rev_pw);

    const usize payload_size = data.size() - kFmodTotalOverhead;
    std::vector<u8> out(payload_size);
    for (usize i = 0; i < payload_size; ++i) {
        const usize pat = i % kFmodHashLen;
        out[i] = static_cast<u8>(
            data[i] ^ static_cast<u8>(hash1[pat]) ^ static_cast<u8>(hash2[pat]));
    }
    return out;
}

Result<std::vector<u8>> encrypt_fmodsimple(
    std::span<const u8> raw,
    std::string_view password) {
    if (is_fmodsimple_encrypted(raw)) {
        return Error{"Source buffer is already FMODSIMPLEPW encrypted"};
    }

    const std::string hash1 = sha1_upper_hex(password);
    std::string rev_pw(password.rbegin(), password.rend());
    const std::string hash2 = sha1_upper_hex(rev_pw);

    std::vector<u8> out;
    out.reserve(raw.size() + kFmodTotalOverhead);
    for (usize i = 0; i < raw.size(); ++i) {
        const usize pat = i % kFmodHashLen;
        out.push_back(static_cast<u8>(
            raw[i] ^ static_cast<u8>(hash2[pat]) ^ static_cast<u8>(hash1[pat])));
    }
    // Match original FMODEncryptFile trailing EOF byte before the 52-byte trailer.
    const usize eof_pat = raw.size() % kFmodHashLen;
    out.push_back(static_cast<u8>(
        0xFFu ^ static_cast<u8>(hash2[eof_pat]) ^ static_cast<u8>(hash1[eof_pat])));

    out.insert(out.end(), hash1.begin(), hash1.end());
    out.insert(out.end(), kFmodMagic.begin(), kFmodMagic.end());
    return out;
}

Result<std::vector<WasdEntry>> unpack_wasd_archive(std::span<const u8> archive_bytes) {
    BinaryReader rd(archive_bytes);
    // Null-terminated magic "WASD1.0"
    std::string magic;
    while (!rd.eof()) {
        auto b = rd.read_u8();
        if (!b) return Error{"Unexpected EOF while reading WASD header"};
        if (b.value() == 0) break;
        magic.push_back(static_cast<char>(b.value()));
        if (magic.size() > 16) return Error{"Invalid WASD header"};
    }
    if (magic != "WASD1.0") {
        return Error{"Not a valid WASD1.0 archive"};
    }

    auto count_res = rd.read_u32();
    if (!count_res) return Error{"Missing WASD entry count"};
    const u32 count = count_res.value();

    std::vector<WasdEntry> entries;
    entries.reserve(std::min<u32>(count, 4096u));

    for (u32 i = 0; i < count; ++i) {
        std::string filename;
        while (!rd.eof()) {
            auto b = rd.read_u8();
            if (!b) return Error{"Unexpected EOF reading WASD filename"};
            if (b.value() == 0) break;
            filename.push_back(static_cast<char>(b.value()));
        }
        auto len_res = rd.read_u32();
        if (!len_res) return Error{"Unexpected EOF reading WASD payload length"};
        const u32 comp_len = len_res.value();
        auto bytes_res = rd.read_bytes(comp_len);
        if (!bytes_res) return Error{"Unexpected EOF reading WASD payload data"};

        auto decomp = zlib_decompress(bytes_res.value());
        if (!decomp) {
            decomp = raw_inflate(bytes_res.value());
        }
        if (!decomp) {
            return Error{"Failed to inflate WASD entry: " + filename};
        }

        WasdEntry entry;
        entry.filename = std::move(filename);
        entry.data = std::move(decomp.value());
        entries.push_back(std::move(entry));
    }

    return entries;
}

} // namespace gm
