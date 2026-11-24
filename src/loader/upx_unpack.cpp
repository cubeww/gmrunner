#include "loader/upx_unpack.h"

#include "base/hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

namespace gm {

namespace {

inline bool has_range(usize total, u64 offset, u64 size) noexcept {
    return offset <= static_cast<u64>(total) && size <= static_cast<u64>(total) - offset;
}

inline Result<std::span<const u8>> checked_span(std::span<const u8> data, u64 offset, u64 size) {
    if (!has_range(data.size(), offset, size)) {
        return Error{"UPX: byte range outside buffer"};
    }
    return data.subspan(static_cast<usize>(offset), static_cast<usize>(size));
}

inline u16 rd_u16_unchecked(std::span<const u8> data, usize off) noexcept {
    return static_cast<u16>(static_cast<u16>(data[off]) | (static_cast<u16>(data[off + 1]) << 8u));
}

inline u32 rd_u32_unchecked(std::span<const u8> data, usize off) noexcept {
    return static_cast<u32>(data[off]) |
           (static_cast<u32>(data[off + 1]) << 8u) |
           (static_cast<u32>(data[off + 2]) << 16u) |
           (static_cast<u32>(data[off + 3]) << 24u);
}

inline Result<u16> rd_u16(std::span<const u8> data, u64 off) {
    if (!has_range(data.size(), off, 2u)) {
        return Error{"UPX: WORD read outside buffer"};
    }
    return rd_u16_unchecked(data, static_cast<usize>(off));
}

inline Result<u32> rd_u32(std::span<const u8> data, u64 off) {
    if (!has_range(data.size(), off, 4u)) {
        return Error{"UPX: DWORD read outside buffer"};
    }
    return rd_u32_unchecked(data, static_cast<usize>(off));
}

inline Result<void> wr_u16(std::span<u8> data, u64 off, u16 value) {
    if (!has_range(data.size(), off, 2u)) {
        return Error{"UPX: WORD write outside buffer"};
    }
    const usize idx = static_cast<usize>(off);
    data[idx + 0] = static_cast<u8>(value & 0xFFu);
    data[idx + 1] = static_cast<u8>((value >> 8u) & 0xFFu);
    return Result<void>::success();
}

inline Result<void> wr_u32(std::span<u8> data, u64 off, u32 value) {
    if (!has_range(data.size(), off, 4u)) {
        return Error{"UPX: DWORD write outside buffer"};
    }
    const usize idx = static_cast<usize>(off);
    data[idx + 0] = static_cast<u8>(value & 0xFFu);
    data[idx + 1] = static_cast<u8>((value >> 8u) & 0xFFu);
    data[idx + 2] = static_cast<u8>((value >> 16u) & 0xFFu);
    data[idx + 3] = static_cast<u8>((value >> 24u) & 0xFFu);
    return Result<void>::success();
}

inline Result<void> write_bytes(std::span<u8> dst, u64 off, std::span<const u8> src) {
    if (!has_range(dst.size(), off, src.size())) {
        return Error{"UPX: write outside buffer"};
    }
    if (!src.empty()) {
        std::memcpy(dst.data() + static_cast<usize>(off), src.data(), src.size());
    }
    return Result<void>::success();
}

inline Result<std::span<const u8>> read_cstring(std::span<const u8> data, u64 off) {
    if (!has_range(data.size(), off, 1u)) {
        return Error{"UPX: import name offset outside buffer"};
    }
    const usize start = static_cast<usize>(off);
    for (usize i = start; i < data.size(); ++i) {
        if (data[i] == 0u) {
            return data.subspan(start, i - start + 1u);
        }
    }
    return Error{"UPX: unterminated import name"};
}

inline Result<u64> align_up(u64 value, u64 boundary) {
    if (boundary == 0u || (boundary & (boundary - 1u)) != 0u) {
        return Error{"UPX: invalid PE alignment"};
    }
    const u64 mask = boundary - 1u;
    if (value > std::numeric_limits<u64>::max() - mask) {
        return Error{"UPX: alignment overflow"};
    }
    return (value + mask) & ~mask;
}

struct NrvBits {
    std::span<const u8> data;
    usize pos = 0;
    u32 word = 0;
    u32 remaining = 0;
    bool failed = false;

    explicit NrvBits(std::span<const u8> input) noexcept : data(input) {}

    u32 next_bit() noexcept {
        if (remaining == 0u) {
            if (pos > data.size() || data.size() - pos < 4u) {
                failed = true;
                return 0u;
            }
            word = rd_u32_unchecked(data, pos);
            pos += 4u;
            remaining = 32u;
        }
        --remaining;
        return (word >> remaining) & 1u;
    }

    u8 next_byte() noexcept {
        if (pos >= data.size()) {
            failed = true;
            return 0u;
        }
        return data[pos++];
    }
};

struct PeSection {
    std::array<u8, 40> header{};
    std::string name;
    u32 virtual_size = 0;
    u32 rva = 0;
    u32 raw_size = 0;
    u32 raw_offset = 0;
};

Result<std::vector<PeSection>> parse_sections(std::span<const u8> data, u64 pe_offset) {
    auto count_res = rd_u16(data, pe_offset + 6u);
    auto opt_res = rd_u16(data, pe_offset + 20u);
    if (!count_res || !opt_res) {
        return Error{"UPX: truncated PE COFF header"};
    }
    const u16 count = count_res.value();
    if (count < 1u || count > 96u || opt_res.value() != 224u) {
        return Error{"UPX: unsupported PE section count or optional header"};
    }

    std::vector<PeSection> result;
    result.reserve(count);
    for (u16 i = 0; i < count; ++i) {
        auto raw_res = checked_span(data, pe_offset + 248u + static_cast<u64>(i) * 40u, 40u);
        if (!raw_res) {
            return raw_res.error();
        }
        const auto raw = raw_res.value();
        PeSection sec{};
        std::memcpy(sec.header.data(), raw.data(), 40u);
        usize name_len = 8u;
        while (name_len > 0u && raw[name_len - 1u] == 0u) {
            --name_len;
        }
        sec.name.assign(reinterpret_cast<const char*>(raw.data()), name_len);
        sec.virtual_size = rd_u32_unchecked(raw, 8u);
        sec.rva = rd_u32_unchecked(raw, 12u);
        sec.raw_size = rd_u32_unchecked(raw, 16u);
        sec.raw_offset = rd_u32_unchecked(raw, 20u);
        result.push_back(std::move(sec));
    }
    return result;
}

std::pair<u32, u32> read_directory(std::span<const u8> pe_header, usize index) noexcept {
    const usize off = 120u + index * 8u;
    return {rd_u32_unchecked(pe_header, off), rd_u32_unchecked(pe_header, off + 4u)};
}

Result<void> unfilter_x86(std::span<u8> image, u64 start, u64 size, u8 kind, u8 cto) {
    if (kind == 0u) {
        return Result<void>::success();
    }
    if (kind != 0x24u && kind != 0x25u && kind != 0x26u) {
        return Error{"UPX: unsupported x86 filter"};
    }
    if (!has_range(image.size(), start, size)) {
        return Error{"UPX: filter range outside image"};
    }
    if (size <= 5u) {
        return Result<void>::success();
    }

    const usize end = static_cast<usize>(start + size - 5u);
    usize pos = static_cast<usize>(start);
    while (pos < end) {
        const u8 op = image[pos];
        const bool match_op = (kind == 0x24u && op == 0xE8u) ||
                              (kind == 0x25u && op == 0xE9u) ||
                              (kind == 0x26u && (op == 0xE8u || op == 0xE9u));
        if (match_op && image[pos + 1u] == cto) {
            const u32 absolute = (static_cast<u32>(image[pos + 2u]) << 16u) |
                                 (static_cast<u32>(image[pos + 3u]) << 8u) |
                                 static_cast<u32>(image[pos + 4u]);
            const u32 rel = absolute - static_cast<u32>(pos) - 1u;
            if (auto e = wr_u32(image, pos + 1u, rel); !e) {
                return e.error();
            }
            pos += 5u;
        } else {
            ++pos;
        }
    }
    return Result<void>::success();
}

struct ImportFunc {
    bool is_name = false;
    std::vector<u8> name;
    u32 raw_value = 0;
};

struct ImportRecord {
    std::vector<u8> dll_name;
    u32 iat_offset = 0;
    std::vector<ImportFunc> functions;
};

Result<u64> rebuild_imports(
    std::span<u8> image,
    std::span<const u8> pe_header,
    u64 extra,
    u32 rva_min,
    std::span<const u8> packed_imports) {
    const auto [import_rva, import_size] = read_directory(pe_header, 1u);
    if (import_rva == 0u || import_size <= 20u) {
        return extra;
    }
    if (import_rva < rva_min) {
        return Error{"UPX: import directory RVA below image base section"};
    }

    auto cursor_res = rd_u32(image, extra);
    auto names_rva_res = rd_u32(image, extra + 4u);
    if (!cursor_res || !names_rva_res) {
        return Error{"UPX: truncated import extra header"};
    }
    u64 cursor = cursor_res.value();
    const u32 names_rva = names_rva_res.value();

    std::vector<ImportRecord> records;
    while (true) {
        auto dll_off_res = rd_u32(image, cursor);
        if (!dll_off_res) return dll_off_res.error();
        if (dll_off_res.value() == 0u) {
            break;
        }
        if (records.size() >= import_size / 20u) {
            return Error{"UPX: too many import descriptors"};
        }
        auto dll_res = read_cstring(packed_imports, dll_off_res.value());
        if (!dll_res) return dll_res.error();
        auto iat_res = rd_u32(image, cursor + 4u);
        if (!iat_res) return iat_res.error();
        cursor += 8u;

        ImportRecord rec{};
        rec.dll_name.assign(dll_res.value().begin(), dll_res.value().end());
        rec.iat_offset = iat_res.value();

        while (true) {
            if (!has_range(image.size(), cursor, 1u)) {
                return Error{"UPX: truncated import function list"};
            }
            const u8 tag = image[static_cast<usize>(cursor)];
            if (tag == 0u) {
                ++cursor;
                break;
            }
            ++cursor;
            ImportFunc fn{};
            if (tag == 1u) {
                auto name_res = read_cstring(image, cursor);
                if (!name_res) return name_res.error();
                fn.is_name = true;
                fn.name.assign(name_res.value().begin(), name_res.value().end());
                cursor += fn.name.size();
            } else if (tag == 255u) {
                auto ord_res = rd_u16(image, cursor);
                if (!ord_res) return ord_res.error();
                fn.is_name = false;
                fn.raw_value = 0x80000000u | static_cast<u32>(ord_res.value());
                cursor += 2u;
            } else {
                auto ptr_res = rd_u32(image, cursor);
                if (!ptr_res) return ptr_res.error();
                auto val_res = rd_u32(packed_imports, ptr_res.value());
                if (!val_res) return val_res.error();
                fn.is_name = false;
                fn.raw_value = val_res.value();
                cursor += 4u;
            }
            rec.functions.push_back(std::move(fn));
        }

        records.push_back(std::move(rec));
    }

    u64 names = 0;
    u64 functions_start = 0;
    u64 function_name = 0;
    if (names_rva != 0u) {
        if (names_rva < rva_min) {
            return Error{"UPX: import names RVA below image base section"};
        }
        names = static_cast<u64>(names_rva - rva_min);
        u64 dll_sum = 0;
        for (const auto& rec : records) {
            dll_sum += rec.dll_name.size();
        }
        auto aligned_sum = align_up(dll_sum, 2u);
        if (!aligned_sum) return aligned_sum.error();
        functions_start = names + aligned_sum.value();
        function_name = functions_start;
    }

    for (usize i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
        const u64 descriptor = static_cast<u64>(import_rva - rva_min) + static_cast<u64>(i) * 20u;
        if (names_rva != 0u) {
            if (auto e = write_bytes(image, names, rec.dll_name); !e) return e.error();
            if (auto e = wr_u32(image, descriptor + 12u, static_cast<u32>(names + rva_min)); !e) return e.error();
            names += rec.dll_name.size();
        } else {
            auto dst_rva_res = rd_u32(image, descriptor + 12u);
            if (!dst_rva_res) return dst_rva_res.error();
            if (dst_rva_res.value() < rva_min) {
                return Error{"UPX: import descriptor DLL name RVA out of range"};
            }
            if (auto e = write_bytes(image, dst_rva_res.value() - rva_min, rec.dll_name); !e) return e.error();
        }
        if (auto e = wr_u32(image, descriptor + 16u, rec.iat_offset + rva_min); !e) return e.error();

        for (usize f = 0; f < rec.functions.size(); ++f) {
            const auto& fn = rec.functions[f];
            const u64 entry = static_cast<u64>(rec.iat_offset) + static_cast<u64>(f) * 4u;
            if (fn.is_name) {
                if (names_rva != 0u) {
                    // UPX overlaps odd-length hint/name padding by 1 zero byte.
                    function_name -= (function_name - functions_start) & 1u;
                    if (auto e = write_bytes(image, function_name + 2u, fn.name); !e) return e.error();
                    if (auto e = wr_u32(image, entry, static_cast<u32>(function_name + rva_min)); !e) return e.error();
                    function_name += fn.name.size() + 2u;
                } else {
                    auto fn_rva_res = rd_u32(image, entry);
                    if (!fn_rva_res) return fn_rva_res.error();
                    if (fn_rva_res.value() < rva_min) {
                        return Error{"UPX: import thunk RVA out of range"};
                    }
                    if (auto e = write_bytes(image, static_cast<u64>(fn_rva_res.value() - rva_min) + 2u, fn.name); !e) {
                        return e.error();
                    }
                }
            } else {
                if (auto e = wr_u32(image, entry, fn.raw_value); !e) return e.error();
            }
        }
        if (auto e = wr_u32(image, static_cast<u64>(rec.iat_offset) + static_cast<u64>(rec.functions.size()) * 4u, 0u);
            !e) {
            return e.error();
        }
    }

    return extra + 8u;
}

struct ResourceRebuilder {
    std::span<u8> image;
    u32 packed_rva = 0;
    std::span<const u8> packed_data;
    std::vector<u8> tree;
    u32 rva_min = 0;
    u16 icon_count = 0;
    u64 extent = 0;
    std::unordered_set<u32> visited;

    Result<void> visit(u32 offset, int level, u32 resource_type) {
        if (level > 2 || !visited.insert(offset).second) {
            return Error{"UPX: invalid or cyclic resource directory"};
        }
        auto named_res = rd_u16(tree, static_cast<u64>(offset) + 12u);
        auto id_res = rd_u16(tree, static_cast<u64>(offset) + 14u);
        if (!named_res || !id_res) {
            return Error{"UPX: truncated resource directory"};
        }
        const u64 entries = static_cast<u64>(named_res.value()) + static_cast<u64>(id_res.value());
        const u64 dir_end = static_cast<u64>(offset) + 16u + entries * 8u;
        if (!has_range(tree.size(), offset, 16u + entries * 8u)) {
            return Error{"UPX: resource directory extends outside section"};
        }
        extent = std::max(extent, dir_end);

        for (u64 i = 0; i < entries; ++i) {
            const u64 entry = static_cast<u64>(offset) + 16u + i * 8u;
            const u32 identifier = rd_u32_unchecked(tree, static_cast<usize>(entry));
            const u32 child = rd_u32_unchecked(tree, static_cast<usize>(entry + 4u));
            if ((identifier & 0x80000000u) != 0u) {
                const u64 name_off = identifier & 0x7FFFFFFFu;
                auto len_res = rd_u16(tree, name_off);
                if (!len_res) return len_res.error();
                const u64 name_size = 2u + 2u * static_cast<u64>(len_res.value());
                if (!has_range(tree.size(), name_off, name_size)) {
                    return Error{"UPX: resource name outside section"};
                }
                extent = std::max(extent, name_off + name_size);
            }
            const u32 kind = (level == 0) ? identifier : resource_type;
            if (level < 2) {
                if ((child & 0x80000000u) == 0u) {
                    return Error{"UPX: unsupported resource tree depth"};
                }
                if (auto e = visit(child & 0x7FFFFFFFu, level + 1, kind); !e) {
                    return e.error();
                }
            } else {
                if ((child & 0x80000000u) != 0u) {
                    return Error{"UPX: unsupported resource leaf"};
                }
                if (!has_range(tree.size(), child, 16u)) {
                    return Error{"UPX: resource data entry outside section"};
                }
                extent = std::max(extent, static_cast<u64>(child) + 16u);
                const u32 data_rva = rd_u32_unchecked(tree, child);
                const u32 data_size = rd_u32_unchecked(tree, child + 4u);
                if (data_rva > packed_rva) {
                    const u64 source = static_cast<u64>(data_rva - packed_rva);
                    if (source < 4u) {
                        return Error{"UPX: invalid kept resource RVA prefix"};
                    }
                    auto target_res = rd_u32(packed_data, source - 4u);
                    if (!target_res) return target_res.error();
                    const u32 target = target_res.value();
                    if (target < rva_min) {
                        return Error{"UPX: restored resource target RVA below base"};
                    }
                    auto aligned_sz = align_up(data_size, 4u);
                    if (!aligned_sz) return aligned_sz.error();
                    auto blob_res = checked_span(packed_data, source, aligned_sz.value());
                    if (!blob_res) return blob_res.error();
                    if (auto e = write_bytes(image, static_cast<u64>(target - rva_min), blob_res.value()); !e) {
                        return e.error();
                    }
                    if (auto e = wr_u32(tree, child, target); !e) {
                        return e.error();
                    }
                    if (icon_count != 0u && kind == 14u) {
                        if (auto e = wr_u16(image, static_cast<u64>(target - rva_min) + 4u, icon_count); !e) {
                            return e.error();
                        }
                        icon_count = 0u;
                    }
                }
            }
        }
        return Result<void>::success();
    }
};

Result<void> rebuild_resources(
    std::span<u8> image,
    u32 original_rva,
    u32 packed_rva,
    std::span<const u8> packed_data,
    u32 rva_min,
    u16 icon_count) {
    if (original_rva < rva_min) {
        return Error{"UPX: original resource RVA below image base section"};
    }
    ResourceRebuilder rebuilder{};
    rebuilder.image = image;
    rebuilder.packed_rva = packed_rva;
    rebuilder.packed_data = packed_data;
    rebuilder.tree.assign(packed_data.begin(), packed_data.end());
    rebuilder.rva_min = rva_min;
    rebuilder.icon_count = icon_count;

    if (auto e = rebuilder.visit(0u, 0, 0u); !e) {
        return e.error();
    }
    auto existing_res = rd_u32(image, static_cast<u64>(original_rva - rva_min) + 12u);
    if (!existing_res) return existing_res.error();
    if (existing_res.value() == 0u) {
        auto aligned_ext = align_up(rebuilder.extent, 4u);
        if (!aligned_ext) return aligned_ext.error();
        auto tree_span = checked_span(rebuilder.tree, 0u, aligned_ext.value());
        if (!tree_span) return tree_span.error();
        if (auto e = write_bytes(image, static_cast<u64>(original_rva - rva_min), tree_span.value()); !e) {
            return e.error();
        }
    }
    return Result<void>::success();
}

std::optional<usize> find_upx_pack_header(std::span<const u8> data, const PeSection& upx1) noexcept {
    const usize start = (upx1.raw_offset > 64u) ? (upx1.raw_offset - 64u) : 0u;
    if (start >= data.size()) {
        return std::nullopt;
    }
    const usize limit = std::min<usize>(data.size(), start + 1024u);
    if (limit < start + 4u) {
        return std::nullopt;
    }
    for (usize i = start; i + 4u <= limit; ++i) {
        if (data[i] == 'U' && data[i + 1u] == 'P' && data[i + 2u] == 'X' && data[i + 3u] == '!') {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

Result<std::vector<u8>> decompress_upx_nrv(
    std::span<const u8> compressed,
    usize expected_size,
    u8 method) {
    if (method != 2u && method != 5u && method != 8u) {
        return Error{"UPX: unsupported NRV compression method"};
    }
    if (expected_size == 0u || expected_size > kMaxUpxImageSize) {
        return Error{"UPX: decompressed image exceeds the configured limit"};
    }

    NrvBits bits(compressed);
    std::vector<u8> output;
    output.reserve(expected_size);
    u64 previous_distance = 1u;

    while (true) {
        while (bits.next_bit() != 0u) {
            if (bits.failed) return Error{"UPX: truncated NRV stream"};
            if (output.size() == expected_size) {
                return Error{"UPX: NRV literal exceeds output length"};
            }
            const u8 lit = bits.next_byte();
            if (bits.failed) return Error{"UPX: truncated NRV stream"};
            output.push_back(lit);
        }
        if (bits.failed) return Error{"UPX: truncated NRV stream"};

        u64 prefix = 1u;
        while (true) {
            prefix = prefix * 2u + bits.next_bit();
            if (bits.failed) return Error{"UPX: truncated NRV stream"};
            if (prefix > 0x1000002ull) {
                return Error{"UPX: invalid NRV distance prefix"};
            }
            if (bits.next_bit() != 0u) {
                break;
            }
            if (bits.failed) return Error{"UPX: truncated NRV stream"};
            if (method != 2u) {
                prefix = (prefix - 1u) * 2u + bits.next_bit();
                if (bits.failed) return Error{"UPX: truncated NRV stream"};
            }
        }
        if (bits.failed) return Error{"UPX: truncated NRV stream"};

        u32 length_bit = 0u;
        u64 distance = 0u;
        if (prefix == 2u) {
            distance = previous_distance;
            if (method != 2u) {
                length_bit = bits.next_bit();
                if (bits.failed) return Error{"UPX: truncated NRV stream"};
            }
        } else {
            const u8 low_byte = bits.next_byte();
            if (bits.failed) return Error{"UPX: truncated NRV stream"};
            const u64 encoded = (prefix - 3u) * 256u + static_cast<u64>(low_byte);
            if (encoded == 0xFFFFFFFFull) {
                break;
            }
            if (method == 2u) {
                distance = encoded + 1u;
            } else {
                length_bit = static_cast<u32>((encoded ^ 1ull) & 1ull);
                distance = (encoded >> 1u) + 1u;
            }
            previous_distance = distance;
        }

        u64 length = 0u;
        if (method == 8u) {
            if (length_bit != 0u) {
                length = 1u + bits.next_bit();
            } else if (bits.next_bit() != 0u) {
                length = 3u + bits.next_bit();
            } else {
                length = 0u;
            }
        } else {
            const u32 first = (method == 2u) ? bits.next_bit() : length_bit;
            length = static_cast<u64>(first) * 2u + bits.next_bit();
        }
        if (bits.failed) return Error{"UPX: truncated NRV stream"};

        if (length == 0u) {
            length = 1u;
            while (true) {
                length = length * 2u + bits.next_bit();
                if (bits.failed) return Error{"UPX: truncated NRV stream"};
                if (length >= expected_size) {
                    return Error{"UPX: invalid NRV match length"};
                }
                if (bits.next_bit() != 0u) {
                    break;
                }
                if (bits.failed) return Error{"UPX: truncated NRV stream"};
            }
            if (bits.failed) return Error{"UPX: truncated NRV stream"};
            length += (method == 8u) ? 3u : 2u;
        }

        const u64 threshold = (method == 2u) ? 0xD00ull : 0x500ull;
        length += 1u + (distance > threshold ? 1u : 0u);

        if (distance == 0u || distance > output.size() || length > expected_size - output.size()) {
            return Error{"UPX: NRV match outside output window"};
        }
        const usize dst_start = output.size();
        const usize dist_sz = static_cast<usize>(distance);
        const usize len_sz = static_cast<usize>(length);
        output.resize(dst_start + len_sz);
        const usize src_start = dst_start - dist_sz;
        for (usize i = 0; i < len_sz; ++i) {
            output[dst_start + i] = output[src_start + i];
        }
    }

    if (output.size() != expected_size || bits.pos != compressed.size()) {
        return Error{"UPX: NRV output length or end marker does not match header"};
    }
    return output;
}

bool is_upx_packed_pe(std::span<const u8> data) noexcept {
    if (data.size() < 0x40 || data[0] != 'M' || data[1] != 'Z') {
        return false;
    }
    const u32 pe = rd_u32_unchecked(data, 0x3Cu);
    if (!has_range(data.size(), pe, 248u)) {
        return false;
    }
    if (data[pe] != 'P' || data[pe + 1u] != 'E' || data[pe + 2u] != 0u || data[pe + 3u] != 0u) {
        return false;
    }
    if (rd_u16_unchecked(data, pe + 4u) != 0x14Cu || rd_u16_unchecked(data, pe + 24u) != 0x10Bu) {
        return false;
    }
    if (rd_u16_unchecked(data, pe + 6u) != 3u || rd_u16_unchecked(data, pe + 20u) != 224u) {
        return false;
    }
    const u64 sec_off = static_cast<u64>(pe) + 248u;
    if (!has_range(data.size(), sec_off, 120u)) {
        return false;
    }
    const auto s0 = data.subspan(static_cast<usize>(sec_off), 8u);
    const auto s1 = data.subspan(static_cast<usize>(sec_off + 40u), 8u);
    if (std::memcmp(s0.data(), "UPX0\0\0\0\0", 8u) != 0 || std::memcmp(s1.data(), "UPX1\0\0\0\0", 8u) != 0) {
        return false;
    }
    PeSection upx1{};
    upx1.raw_offset = rd_u32_unchecked(data, static_cast<usize>(sec_off + 40u + 20u));
    return find_upx_pack_header(data, upx1).has_value();
}

Result<std::vector<u8>> unpack_upx_pe(std::span<const u8> data) {
    if (data.size() < 0x40 || data[0] != 'M' || data[1] != 'Z') {
        return Error{"UPX: expected a DOS/PE executable"};
    }
    auto pe_res = rd_u32(data, 0x3Cu);
    if (!pe_res) return pe_res.error();
    const u32 pe = pe_res.value();

    auto packed_hdr_res = checked_span(data, pe, 248u);
    if (!packed_hdr_res) {
        return Error{"UPX: truncated PE32 header"};
    }
    const auto packed_header = packed_hdr_res.value();
    if (std::memcmp(packed_header.data(), "PE\0\0", 4u) != 0 ||
        rd_u16_unchecked(packed_header, 4u) != 0x14Cu ||
        rd_u16_unchecked(packed_header, 24u) != 0x10Bu) {
        return Error{"UPX: only x86 PE32 is supported"};
    }

    auto packed_secs_res = parse_sections(data, pe);
    if (!packed_secs_res) return packed_secs_res.error();
    const auto packed_sections = std::move(packed_secs_res.value());
    if (packed_sections.size() != 3u || packed_sections[0].name != "UPX0" || packed_sections[1].name != "UPX1") {
        return Error{"UPX: expected the UPX0/UPX1/rsrc PE32 layout"};
    }

    auto pack_pos = find_upx_pack_header(data, packed_sections[1]);
    if (!pack_pos.has_value()) {
        return Error{"UPX: pack header not found"};
    }
    auto pack_res = checked_span(data, pack_pos.value(), 32u);
    if (!pack_res) return pack_res.error();
    const auto pack = pack_res.value();

    const u8 version = pack[4];
    const u8 format_id = pack[5];
    const u8 method = pack[6];
    if ((version != 12u && version != 13u) || format_id != 9u) {
        return Error{"UPX: unsupported header version or format"};
    }
    u32 header_sum = 0u;
    for (usize i = 4u; i < 31u; ++i) {
        header_sum += pack[i];
    }
    if ((header_sum % 251u) != pack[31]) {
        return Error{"UPX: pack header checksum mismatch"};
    }
    if ((method != 2u && method != 5u && method != 8u) || pack[30] != 0u) {
        return Error{"UPX: unsupported compression method or filter MRU"};
    }

    const u32 unpacked_adler = rd_u32_unchecked(pack, 8u);
    const u32 packed_adler = rd_u32_unchecked(pack, 12u);
    const u32 image_size = rd_u32_unchecked(pack, 16u);
    const u32 packed_size = rd_u32_unchecked(pack, 20u);
    const u32 file_size = rd_u32_unchecked(pack, 24u);
    const u8 filter_kind = pack[28];
    const u8 filter_cto = pack[29];

    auto compressed_res = checked_span(data, static_cast<u64>(pack_pos.value()) + 32u, packed_size);
    if (!compressed_res) return compressed_res.error();
    if (adler32_bytes(compressed_res.value()) != packed_adler) {
        return Error{"UPX: compressed Adler-32 mismatch"};
    }

    auto image_res = decompress_upx_nrv(compressed_res.value(), image_size, method);
    if (!image_res) return image_res.error();
    std::vector<u8> image = std::move(image_res.value());
    if (adler32_bytes(image) != unpacked_adler) {
        return Error{"UPX: decompressed Adler-32 mismatch"};
    }

    if (image.size() < 4u) {
        return Error{"UPX: decompressed image too small"};
    }
    u64 extra = rd_u32_unchecked(image, image.size() - 4u);
    auto orig_hdr_res = checked_span(image, extra, 248u);
    if (!orig_hdr_res) return orig_hdr_res.error();
    std::array<u8, 248> header{};
    std::memcpy(header.data(), orig_hdr_res.value().data(), 248u);
    if (std::memcmp(header.data(), "PE\0\0", 4u) != 0 ||
        rd_u16_unchecked(header, 4u) != 0x14Cu ||
        rd_u16_unchecked(header, 24u) != 0x10Bu) {
        return Error{"UPX: invalid saved PE header"};
    }

    auto orig_secs_res = parse_sections(image, extra);
    if (!orig_secs_res) return orig_secs_res.error();
    const auto original_sections = std::move(orig_secs_res.value());
    extra += 248u + static_cast<u64>(original_sections.size()) * 40u;

    const u32 rva_min = original_sections[0].rva;
    const u32 base_of_code = rd_u32_unchecked(header, 44u);
    const u32 size_of_code = rd_u32_unchecked(header, 28u);
    if (base_of_code < rva_min) {
        return Error{"UPX: BaseOfCode below first section RVA"};
    }
    if (auto e = unfilter_x86(image, base_of_code - rva_min, size_of_code, filter_kind, filter_cto); !e) {
        return e.error();
    }

    auto packed_rva = [&](u32 rva, u32 size) -> Result<std::span<const u8>> {
        for (const auto& sec : packed_sections) {
            if (rva >= sec.rva) {
                const u64 offset = static_cast<u64>(rva - sec.rva);
                if (offset <= sec.raw_size && size <= sec.raw_size - offset) {
                    return checked_span(data, static_cast<u64>(sec.raw_offset) + offset, size);
                }
            }
        }
        return Error{"UPX: RVA outside packed sections"};
    };

    const auto [packed_import_rva, packed_import_size] = read_directory(packed_header, 1u);
    (void)packed_import_size;
    std::span<const u8> import_data{};
    for (const auto& sec : packed_sections) {
        if (packed_import_rva >= sec.rva &&
            static_cast<u64>(packed_import_rva - sec.rva) < static_cast<u64>(sec.raw_size)) {
            const u32 avail = sec.raw_size - (packed_import_rva - sec.rva);
            auto imp_res = packed_rva(packed_import_rva, avail);
            if (!imp_res) return imp_res.error();
            import_data = imp_res.value();
            break;
        }
    }

    auto next_extra = rebuild_imports(image, header, extra, rva_min, import_data);
    if (!next_extra) return next_extra.error();
    extra = next_extra.value();

    const auto [reloc_rva, reloc_size] = read_directory(header, 5u);
    if (reloc_rva != 0u && reloc_size != 0u && (rd_u16_unchecked(header, 22u) & 1u) == 0u) {
        return Error{"UPX: relocation reconstruction is not supported"};
    }
    if (read_directory(header, 0u).second != 0u) {
        return Error{"UPX: export reconstruction is not supported"};
    }

    const auto [resource_rva, resource_size] = read_directory(header, 2u);
    const auto [packed_resource_rva, packed_resource_size] = read_directory(packed_header, 2u);
    if (resource_size != 0u && packed_resource_size != 0u) {
        auto icon_res = rd_u16(image, extra);
        if (!icon_res) return icon_res.error();
        extra += 2u;
        const auto& rsrc_sec = packed_sections.back();
        if (packed_resource_rva < rsrc_sec.rva ||
            static_cast<u64>(packed_resource_rva - rsrc_sec.rva) > static_cast<u64>(rsrc_sec.raw_size)) {
            return Error{"UPX: packed resource RVA outside last section"};
        }
        const u32 avail = rsrc_sec.raw_size - (packed_resource_rva - rsrc_sec.rva);
        auto src_res = packed_rva(packed_resource_rva, avail);
        if (!src_res) return src_res.error();
        if (auto e = rebuild_resources(image, resource_rva, packed_resource_rva, src_res.value(),
                                       rva_min, icon_res.value());
            !e) {
            return e.error();
        }
    }

    if (extra != image.size() - 4u) {
        return Error{"UPX: unexpected extra information layout"};
    }

    for (const usize dir_idx : {6u, 11u, 12u}) {
        std::memset(header.data() + 120u + dir_idx * 8u, 0, 8u);
    }
    (void)wr_u32(header, 84u, rva_min);
    (void)wr_u32(header, 88u, 0u);
    if ((rd_u16_unchecked(packed_header, 22u) & 1u) != 0u) {
        (void)wr_u16(header, 22u, static_cast<u16>(rd_u16_unchecked(header, 22u) | 1u));
        std::memset(header.data() + 160u, 0, 8u);
    }

    const u32 packed_file_align = rd_u32_unchecked(packed_header, 60u);
    u64 packed_end = 0u;
    for (const auto& sec : packed_sections) {
        if (sec.raw_size > 0u) {
            auto end_res = align_up(static_cast<u64>(sec.raw_offset) + sec.raw_size, packed_file_align);
            if (!end_res) return end_res.error();
            packed_end = std::max(packed_end, end_res.value());
        }
    }
    if (packed_end > data.size()) {
        return Error{"UPX: packed sections extend outside input file"};
    }
    const auto overlay = data.subspan(static_cast<usize>(packed_end));

    const u32 file_alignment = rd_u32_unchecked(header, 60u);
    u64 raw_end = 0u;
    for (const auto& sec : original_sections) {
        if (sec.raw_size > 0u) {
            auto aligned_sz = align_up(sec.raw_size, file_alignment);
            if (!aligned_sz) return aligned_sz.error();
            raw_end = std::max(raw_end, static_cast<u64>(sec.raw_offset) + aligned_sz.value());
        }
    }
    if (raw_end > file_size || raw_end > kMaxUpxImageSize) {
        return Error{"UPX: restored PE exceeds the original file size"};
    }
    if (overlay.size() > std::numeric_limits<usize>::max() - static_cast<usize>(raw_end)) {
        return Error{"UPX: restored executable size overflow"};
    }

    std::vector<u8> output(static_cast<usize>(raw_end) + overlay.size(), 0u);
    if (auto e = write_bytes(output, 0u, data.first(pe)); !e) return e.error();
    if (auto e = write_bytes(output, pe, header); !e) return e.error();

    for (usize i = 0; i < original_sections.size(); ++i) {
        const auto& sec = original_sections[i];
        if (auto e = write_bytes(output, static_cast<u64>(pe) + 248u + static_cast<u64>(i) * 40u, sec.header); !e) {
            return e.error();
        }
        if (sec.raw_size > 0u) {
            if (sec.rva < rva_min) {
                return Error{"UPX: original section RVA below first section"};
            }
            auto aligned_sz = align_up(sec.raw_size, file_alignment);
            if (!aligned_sz) return aligned_sz.error();
            auto sec_bytes = checked_span(image, sec.rva - rva_min, aligned_sz.value());
            if (!sec_bytes) return sec_bytes.error();
            if (auto e = write_bytes(output, sec.raw_offset, sec_bytes.value()); !e) return e.error();
        }
    }

    if (!overlay.empty()) {
        std::memcpy(output.data() + static_cast<usize>(raw_end), overlay.data(), overlay.size());
    }
    return output;
}

} // namespace gm
