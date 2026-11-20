#include "base/hash.h"

#include <cstring>

namespace gm {

namespace {

constexpr std::array<u32, 256> make_crc32_table() noexcept {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<u32, 256> kCrc32Table = make_crc32_table();

inline constexpr u32 rotl32(u32 x, u32 n) noexcept {
    return (x << n) | (x >> (32u - n));
}

inline u32 load_le32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8u) |
           (static_cast<u32>(p[2]) << 16u) |
           (static_cast<u32>(p[3]) << 24u);
}

inline u32 load_be32(const u8* p) noexcept {
    return (static_cast<u32>(p[0]) << 24u) |
           (static_cast<u32>(p[1]) << 16u) |
           (static_cast<u32>(p[2]) << 8u) |
           static_cast<u32>(p[3]);
}

inline void store_le32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>(v & 0xFFu);
    p[1] = static_cast<u8>((v >> 8u) & 0xFFu);
    p[2] = static_cast<u8>((v >> 16u) & 0xFFu);
    p[3] = static_cast<u8>((v >> 24u) & 0xFFu);
}

inline void store_be32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>((v >> 24u) & 0xFFu);
    p[1] = static_cast<u8>((v >> 16u) & 0xFFu);
    p[2] = static_cast<u8>((v >> 8u) & 0xFFu);
    p[3] = static_cast<u8>(v & 0xFFu);
}

void md5_compress(u32 state[4], const u8 block[64]) noexcept {
    static constexpr u32 kS[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    static constexpr u32 kK[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
        0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
        0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
        0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };

    u32 m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = load_le32(block + i * 4);
    }

    u32 a = state[0];
    u32 b = state[1];
    u32 c = state[2];
    u32 d = state[3];

    for (u32 i = 0; i < 64; ++i) {
        u32 f = 0;
        u32 g = 0;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5u * i + 1u) & 15u;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) & 15u;
        } else {
            f = c ^ (b | (~d));
            g = (7u * i) & 15u;
        }
        const u32 tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + kK[i] + m[g], kS[i]);
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void sha1_compress(u32 state[5], const u8 block[64]) noexcept {
    u32 w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = load_be32(block + i * 4);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1u);
    }

    u32 a = state[0];
    u32 b = state[1];
    u32 c = state[2];
    u32 d = state[3];
    u32 e = state[4];

    for (int i = 0; i < 80; ++i) {
        u32 f = 0;
        u32 k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const u32 tmp = rotl32(a, 5u) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30u);
        b = a;
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

} // namespace

u32 crc32_gm81(std::span<const u8> data) noexcept {
    u32 crc = 0xFFFFFFFFu;
    for (const u8 b : data) {
        crc = kCrc32Table[(crc ^ b) & 0xFFu] ^ (crc >> 8u);
    }
    return crc;
}

u32 crc32_gm81_utf16le(std::string_view ascii) noexcept {
    u32 crc = 0xFFFFFFFFu;
    for (const char ch : ascii) {
        const u8 lo = static_cast<u8>(ch);
        crc = kCrc32Table[(crc ^ lo) & 0xFFu] ^ (crc >> 8u);
        crc = kCrc32Table[crc & 0xFFu] ^ (crc >> 8u); // high byte 0x00
    }
    return crc;
}

u32 crc32_ieee(std::span<const u8> data) noexcept {
    return crc32_gm81(data) ^ 0xFFFFFFFFu;
}

Md5Digest md5_bytes(std::span<const u8> data) noexcept {
    u32 state[4] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};
    usize offset = 0;
    while (data.size() - offset >= 64) {
        md5_compress(state, data.data() + offset);
        offset += 64;
    }

    u8 tail[128]{};
    const usize rem = data.size() - offset;
    if (rem > 0) {
        std::memcpy(tail, data.data() + offset, rem);
    }
    tail[rem] = 0x80u;
    const usize pad_len = (rem < 56) ? 64 : 128;
    const u64 bit_len = static_cast<u64>(data.size()) * 8u;
    store_le32(tail + pad_len - 8, static_cast<u32>(bit_len & 0xFFFFFFFFu));
    store_le32(tail + pad_len - 4, static_cast<u32>(bit_len >> 32u));

    md5_compress(state, tail);
    if (pad_len == 128) {
        md5_compress(state, tail + 64);
    }

    Md5Digest digest{};
    for (int i = 0; i < 4; ++i) {
        store_le32(digest.data() + i * 4, state[i]);
    }
    return digest;
}

Sha1Digest sha1_bytes(std::span<const u8> data) noexcept {
    u32 state[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    usize offset = 0;
    while (data.size() - offset >= 64) {
        sha1_compress(state, data.data() + offset);
        offset += 64;
    }

    u8 tail[128]{};
    const usize rem = data.size() - offset;
    if (rem > 0) {
        std::memcpy(tail, data.data() + offset, rem);
    }
    tail[rem] = 0x80u;
    const usize pad_len = (rem < 56) ? 64 : 128;
    const u64 bit_len = static_cast<u64>(data.size()) * 8u;
    store_be32(tail + pad_len - 8, static_cast<u32>(bit_len >> 32u));
    store_be32(tail + pad_len - 4, static_cast<u32>(bit_len & 0xFFFFFFFFu));

    sha1_compress(state, tail);
    if (pad_len == 128) {
        sha1_compress(state, tail + 64);
    }

    Sha1Digest digest{};
    for (int i = 0; i < 5; ++i) {
        store_be32(digest.data() + i * 4, state[i]);
    }
    return digest;
}

std::string hex_encode(std::span<const u8> bytes, bool uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (usize i = 0; i < bytes.size(); ++i) {
        out[i * 2] = digits[(bytes[i] >> 4u) & 0x0Fu];
        out[i * 2 + 1] = digits[bytes[i] & 0x0Fu];
    }
    return out;
}

} // namespace gm
