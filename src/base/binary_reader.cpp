#include "base/binary_reader.h"
#include "base/zlib_util.h"

#include <cstring>

namespace gm {

Result<void> BinaryReader::seek(usize offset) noexcept {
    if (offset > data_.size()) {
        return Error{"BinaryReader::seek out of bounds"};
    }
    pos_ = offset;
    return Result<void>::success();
}

Result<void> BinaryReader::skip(usize count) noexcept {
    if (!has(count)) {
        return Error{"BinaryReader::skip out of bounds"};
    }
    pos_ += count;
    return Result<void>::success();
}

Result<u8> BinaryReader::read_u8() noexcept {
    if (!has(1)) {
        return Error{"BinaryReader::read_u8 unexpected EOF"};
    }
    return data_[pos_++];
}

Result<u16> BinaryReader::read_u16() noexcept {
    if (!has(2)) {
        return Error{"BinaryReader::read_u16 unexpected EOF"};
    }
    const u8* p = data_.data() + pos_;
    pos_ += 2;
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8u));
}

Result<i16> BinaryReader::read_i16() noexcept {
    auto r = read_u16();
    if (!r) return r.error();
    return static_cast<i16>(r.value());
}

Result<u32> BinaryReader::read_u32() noexcept {
    if (!has(4)) {
        return Error{"BinaryReader::read_u32 unexpected EOF"};
    }
    const u8* p = data_.data() + pos_;
    pos_ += 4;
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8u) |
           (static_cast<u32>(p[2]) << 16u) |
           (static_cast<u32>(p[3]) << 24u);
}

Result<i32> BinaryReader::read_i32() noexcept {
    auto r = read_u32();
    if (!r) return r.error();
    return static_cast<i32>(r.value());
}

Result<u64> BinaryReader::read_u64() noexcept {
    if (!has(8)) {
        return Error{"BinaryReader::read_u64 unexpected EOF"};
    }
    const u8* p = data_.data() + pos_;
    pos_ += 8;
    return static_cast<u64>(p[0]) |
           (static_cast<u64>(p[1]) << 8u) |
           (static_cast<u64>(p[2]) << 16u) |
           (static_cast<u64>(p[3]) << 24u) |
           (static_cast<u64>(p[4]) << 32u) |
           (static_cast<u64>(p[5]) << 40u) |
           (static_cast<u64>(p[6]) << 48u) |
           (static_cast<u64>(p[7]) << 56u);
}

Result<i64> BinaryReader::read_i64() noexcept {
    auto r = read_u64();
    if (!r) return r.error();
    return static_cast<i64>(r.value());
}

Result<f32> BinaryReader::read_f32() noexcept {
    auto r = read_u32();
    if (!r) return r.error();
    f32 v = 0.0f;
    const u32 bits = r.value();
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

Result<f64> BinaryReader::read_f64() noexcept {
    auto r = read_u64();
    if (!r) return r.error();
    f64 v = 0.0;
    const u64 bits = r.value();
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

Result<bool> BinaryReader::read_bool32() noexcept {
    auto r = read_i32();
    if (!r) return r.error();
    return r.value() != 0;
}

Result<std::span<const u8>> BinaryReader::read_span(usize count) noexcept {
    if (!has(count)) {
        return Error{"BinaryReader::read_span unexpected EOF"};
    }
    auto slice = data_.subspan(pos_, count);
    pos_ += count;
    return slice;
}

Result<std::vector<u8>> BinaryReader::read_bytes(usize count) {
    auto s = read_span(count);
    if (!s) return s.error();
    return std::vector<u8>(s.value().begin(), s.value().end());
}

Result<std::string> BinaryReader::read_gm_string() {
    auto len_res = read_u32();
    if (!len_res) return len_res.error();
    auto span_res = read_span(len_res.value());
    if (!span_res) return span_res.error();
    return std::string(reinterpret_cast<const char*>(span_res.value().data()), span_res.value().size());
}

Result<std::span<const u8>> BinaryReader::read_length_prefixed_span() noexcept {
    auto len_res = read_u32();
    if (!len_res) return len_res.error();
    return read_span(len_res.value());
}

Result<std::vector<u8>> BinaryReader::read_compressed_stream() {
    auto span_res = read_length_prefixed_span();
    if (!span_res) return span_res.error();
    return zlib_decompress(span_res.value());
}

} // namespace gm
