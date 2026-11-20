#include "base/zlib_util.h"

#include <zlib.h>

#include <array>

namespace gm {

namespace {

Result<std::vector<u8>> inflate_impl(std::span<const u8> compressed, int window_bits) {
    if (compressed.empty()) {
        return std::vector<u8>{};
    }

    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
    strm.avail_in = static_cast<uInt>(compressed.size());

    if (inflateInit2(&strm, window_bits) != Z_OK) {
        return Error{"zlib inflateInit2 failed"};
    }

    std::vector<u8> out;
    out.reserve(compressed.size() * 2);
    std::array<u8, 32768> chunk{};

    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        strm.next_out = chunk.data();
        strm.avail_out = static_cast<uInt>(chunk.size());
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            const char* msg = strm.msg ? strm.msg : "zlib inflate error";
            inflateEnd(&strm);
            return Error{std::string("zlib inflate failed: ") + msg};
        }
        const usize produced = chunk.size() - strm.avail_out;
        if (produced > 0) {
            out.insert(out.end(), chunk.data(), chunk.data() + produced);
        }
        if (ret == Z_OK && strm.avail_in == 0 && produced == 0) {
            inflateEnd(&strm);
            return Error{"zlib inflate truncated stream"};
        }
    }

    inflateEnd(&strm);
    return out;
}

Result<std::vector<u8>> deflate_impl(std::span<const u8> raw, int level, int window_bits) {
    z_stream strm{};
    if (deflateInit2(&strm, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return Error{"zlib deflateInit2 failed"};
    }

    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(raw.data()));
    strm.avail_in = static_cast<uInt>(raw.size());

    std::vector<u8> out;
    out.reserve(raw.size() / 2 + 64);
    std::array<u8, 32768> chunk{};

    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        strm.next_out = chunk.data();
        strm.avail_out = static_cast<uInt>(chunk.size());
        ret = deflate(&strm, Z_FINISH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            const char* msg = strm.msg ? strm.msg : "zlib deflate error";
            deflateEnd(&strm);
            return Error{std::string("zlib deflate failed: ") + msg};
        }
        const usize produced = chunk.size() - strm.avail_out;
        if (produced > 0) {
            out.insert(out.end(), chunk.data(), chunk.data() + produced);
        }
    }

    deflateEnd(&strm);
    return out;
}

} // namespace

Result<std::vector<u8>> zlib_decompress(std::span<const u8> compressed) {
    return inflate_impl(compressed, MAX_WBITS);
}

Result<std::vector<u8>> zlib_compress(std::span<const u8> raw, int level) {
    return deflate_impl(raw, level, MAX_WBITS);
}

Result<std::vector<u8>> raw_inflate(std::span<const u8> compressed) {
    return inflate_impl(compressed, -MAX_WBITS);
}

Result<std::vector<u8>> raw_deflate(std::span<const u8> raw, int level) {
    return deflate_impl(raw, level, -MAX_WBITS);
}

} // namespace gm
