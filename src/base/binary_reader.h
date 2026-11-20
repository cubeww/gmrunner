#pragma once

#include "base/types.h"

#include <span>
#include <string>
#include <vector>

namespace gm {

class BinaryReader {
public:
    explicit BinaryReader(std::span<const u8> data) noexcept : data_(data), pos_(0) {}

    [[nodiscard]] usize position() const noexcept { return pos_; }
    [[nodiscard]] usize size() const noexcept { return data_.size(); }
    [[nodiscard]] usize remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] bool eof() const noexcept { return pos_ >= data_.size(); }
    [[nodiscard]] std::span<const u8> full_span() const noexcept { return data_; }

    [[nodiscard]] Result<void> seek(usize offset) noexcept;
    [[nodiscard]] Result<void> skip(usize count) noexcept;

    [[nodiscard]] Result<u8> read_u8() noexcept;
    [[nodiscard]] Result<i16> read_i16() noexcept;
    [[nodiscard]] Result<u16> read_u16() noexcept;
    [[nodiscard]] Result<i32> read_i32() noexcept;
    [[nodiscard]] Result<u32> read_u32() noexcept;
    [[nodiscard]] Result<i64> read_i64() noexcept;
    [[nodiscard]] Result<u64> read_u64() noexcept;
    [[nodiscard]] Result<f32> read_f32() noexcept;
    [[nodiscard]] Result<f64> read_f64() noexcept;
    [[nodiscard]] Result<bool> read_bool32() noexcept;

    [[nodiscard]] Result<std::span<const u8>> read_span(usize count) noexcept;
    [[nodiscard]] Result<std::vector<u8>> read_bytes(usize count);

    // GM8 length-prefixed string (u32 length + raw bytes).
    [[nodiscard]] Result<std::string> read_gm_string();

    // GM8 length-prefixed raw stream (u32 length + raw bytes).
    [[nodiscard]] Result<std::span<const u8>> read_length_prefixed_span() noexcept;

    // GM8 CStream.ReadStreamC (u32 compressed length + zlib stream).
    [[nodiscard]] Result<std::vector<u8>> read_compressed_stream();

private:
    [[nodiscard]] bool has(usize count) const noexcept { return count <= remaining(); }

    std::span<const u8> data_;
    usize pos_ = 0;
};

} // namespace gm
