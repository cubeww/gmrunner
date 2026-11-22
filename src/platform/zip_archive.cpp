#include "platform/zip_archive.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/zlib_util.h"
#include "platform/vfs.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

namespace gm {

namespace {

constexpr u32 kSigLocalFileHeader = 0x04034B50u;
constexpr u32 kSigCentralDirHeader = 0x02014B50u;
constexpr u32 kSigEocd = 0x06054B50u;
constexpr u32 kSigZip64EocdLocator = 0x07064B50u;
constexpr u32 kSigZip64Eocd = 0x06064B50u;

inline u16 read_u16_le(const u8* p) noexcept {
    return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8u);
}

inline u32 read_u32_le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8u) |
           (static_cast<u32>(p[2]) << 16u) |
           (static_cast<u32>(p[3]) << 24u);
}

inline u64 read_u64_le(const u8* p) noexcept {
    return static_cast<u64>(read_u32_le(p)) | (static_cast<u64>(read_u32_le(p + 4)) << 32u);
}

i64 dos_datetime_to_unix(u16 dos_time, u16 dos_date) noexcept {
    if (dos_date == 0 && dos_time == 0) return 0;
    std::tm tm_val{};
    tm_val.tm_sec = (dos_time & 0x1F) * 2;
    tm_val.tm_min = (dos_time >> 5) & 0x3F;
    tm_val.tm_hour = (dos_time >> 11) & 0x1F;
    tm_val.tm_mday = std::max(1, static_cast<int>(dos_date & 0x1F));
    tm_val.tm_mon = std::max(0, static_cast<int>((dos_date >> 5) & 0x0F) - 1);
    tm_val.tm_year = static_cast<int>((dos_date >> 9) & 0x7F) + 80; // DOS epoch 1980 -> tm_year 1900
    tm_val.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm_val);
    return t > 0 ? static_cast<i64>(t) : 0;
}

bool is_ignorable_exe_name(std::string_view lower_name) noexcept {
    return lower_name.starts_with("unins") ||
           lower_name.starts_with("vcredist") ||
           lower_name == "dxsetup.exe" ||
           lower_name == "crashreporter.exe";
}

} // namespace

std::string ZipVfsSource::normalize_rel(std::string_view path) {
    std::string s(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    while (!s.empty() && s.back() == '/') s.pop_back();
    // Collapse redundant slashes and "." segments.
    std::vector<std::string_view> parts;
    usize pos = 0;
    while (pos < s.size()) {
        usize next = s.find('/', pos);
        if (next == std::string::npos) next = s.size();
        const std::string_view part(s.data() + pos, next - pos);
        pos = next + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out;
    for (usize i = 0; i < parts.size(); ++i) {
        if (i > 0) out.push_back('/');
        out.append(parts[i]);
    }
    return out;
}

Result<std::unique_ptr<ZipVfsSource>> ZipVfsSource::open_file(const std::filesystem::path& zip_path) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(zip_path, ec);
    if (ec) {
        return Error{"Failed to stat ZIP archive '" + Vfs::path_to_utf8(zip_path) + "': " + ec.message()};
    }

    auto stream = std::make_shared<std::ifstream>(zip_path, std::ios::binary);
    if (!stream->is_open()) {
        return Error{"Failed to open ZIP archive: " + Vfs::path_to_utf8(zip_path)};
    }

    ReadRangeFn reader = [stream](u64 offset, u8* dst, usize len) -> bool {
        if (len == 0) return true;
        stream->clear();
        stream->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream->good()) return false;
        stream->read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(len));
        return static_cast<usize>(stream->gcount()) == len;
    };

    return open_reader(static_cast<u64>(sz), std::move(reader), Vfs::path_to_utf8(zip_path));
}

Result<std::unique_ptr<ZipVfsSource>> ZipVfsSource::open_reader(
    u64 archive_size,
    ReadRangeFn reader,
    std::string display_name) {
    if (!reader || archive_size < 22u) {
        return Error{"Invalid or truncated ZIP archive: " + display_name};
    }

    auto zip = std::make_unique<ZipVfsSource>();
    zip->archive_size_ = archive_size;
    zip->reader_ = std::move(reader);
    zip->display_name_ = std::move(display_name);

    if (auto res = zip->parse_central_directory(); !res) {
        return res.error();
    }

    GM_LOG_INFO("vfs", "Mounted streaming ZIP '%s' (%zu entries, default_exe='%s')",
                zip->display_name_.c_str(),
                zip->entries_.size(),
                zip->default_exe_.c_str());
    return zip;
}

Result<void> ZipVfsSource::parse_central_directory() {
    // 1. Locate End of Central Directory (EOCD) within the last 64 KiB + 22 bytes.
    const usize tail_len = static_cast<usize>(std::min<u64>(archive_size_, 65557u));
    const u64 tail_offset = archive_size_ - tail_len;
    std::vector<u8> tail(tail_len);
    {
        std::scoped_lock lock(reader_mutex_);
        if (!reader_(tail_offset, tail.data(), tail.size())) {
            return Error{"Failed to read ZIP trailer from " + display_name_};
        }
    }

    usize eocd_rel = std::string::npos;
    for (usize i = tail_len - 22u;; --i) {
        if (read_u32_le(tail.data() + i) == kSigEocd) {
            const u16 comment_len = read_u16_le(tail.data() + i + 20u);
            if (i + 22u + comment_len == tail_len) {
                eocd_rel = i;
                break;
            }
        }
        if (i == 0) break;
    }
    if (eocd_rel == std::string::npos) {
        return Error{"ZIP End-of-Central-Directory record not found in " + display_name_};
    }

    const u8* eocd = tail.data() + eocd_rel;
    u64 cd_entries = read_u16_le(eocd + 10u);
    u64 cd_size = read_u32_le(eocd + 12u);
    u64 cd_offset = read_u32_le(eocd + 16u);

    // 2. Handle ZIP64 EOCD locator if present immediately before EOCD.
    if (eocd_rel >= 20u && read_u32_le(tail.data() + eocd_rel - 20u) == kSigZip64EocdLocator) {
        const u64 z64_offset = read_u64_le(tail.data() + eocd_rel - 12u);
        if (z64_offset + 56u <= archive_size_) {
            u8 z64[56]{};
            bool ok = false;
            {
                std::scoped_lock lock(reader_mutex_);
                ok = reader_(z64_offset, z64, sizeof(z64));
            }
            if (ok && read_u32_le(z64) == kSigZip64Eocd) {
                cd_entries = read_u64_le(z64 + 32u);
                cd_size = read_u64_le(z64 + 40u);
                cd_offset = read_u64_le(z64 + 48u);
            }
        }
    }

    if (cd_offset + cd_size > archive_size_) {
        return Error{"ZIP Central Directory exceeds archive bounds in " + display_name_};
    }

    // 3. Read entire Central Directory in one range read.
    std::vector<u8> cd(static_cast<usize>(cd_size));
    if (cd_size > 0) {
        std::scoped_lock lock(reader_mutex_);
        if (!reader_(cd_offset, cd.data(), cd.size())) {
            return Error{"Failed to read ZIP Central Directory from " + display_name_};
        }
    }

    struct RawEntry {
        std::string norm_path;
        u64 local_offset = 0;
        u64 comp_size = 0;
        u64 uncomp_size = 0;
        u16 method = 0;
        i64 mtime_unix = 0;
        bool is_dir = false;
    };

    std::vector<RawEntry> raw_entries;
    raw_entries.reserve(static_cast<usize>(std::min<u64>(cd_entries, 262144u)));

    usize pos = 0;
    while (pos + 46u <= cd.size()) {
        const u8* h = cd.data() + pos;
        if (read_u32_le(h) != kSigCentralDirHeader) break;

        const u16 flags = read_u16_le(h + 8u);
        const u16 method = read_u16_le(h + 10u);
        const u16 mod_time = read_u16_le(h + 12u);
        const u16 mod_date = read_u16_le(h + 14u);
        u64 comp_size = read_u32_le(h + 20u);
        u64 uncomp_size = read_u32_le(h + 24u);
        const u16 name_len = read_u16_le(h + 28u);
        const u16 extra_len = read_u16_le(h + 30u);
        const u16 comment_len = read_u16_le(h + 32u);
        u64 local_off = read_u32_le(h + 42u);

        const usize next_pos = pos + 46u + name_len + extra_len + comment_len;
        if (next_pos > cd.size()) break;

        std::string_view raw_name(reinterpret_cast<const char*>(h + 46u), name_len);
        const bool is_utf8 = (flags & 0x0800u) != 0u;
        std::string name_utf8 = is_utf8 ? std::string(raw_name) : codepage_to_utf8(raw_name, 1252u);
        const bool trailing_slash = !name_utf8.empty() && (name_utf8.back() == '/' || name_utf8.back() == '\\');

        // Parse ZIP64 extended information extra field if needed.
        if (comp_size == 0xFFFFFFFFu || uncomp_size == 0xFFFFFFFFu || local_off == 0xFFFFFFFFu) {
            const u8* ep = h + 46u + name_len;
            usize epos = 0;
            while (epos + 4u <= extra_len) {
                const u16 eid = read_u16_le(ep + epos);
                const u16 esz = read_u16_le(ep + epos + 2u);
                epos += 4u;
                if (epos + esz > extra_len) break;
                if (eid == 0x0001u) {
                    usize fpos = 0;
                    if (uncomp_size == 0xFFFFFFFFu && fpos + 8u <= esz) {
                        uncomp_size = read_u64_le(ep + epos + fpos);
                        fpos += 8u;
                    }
                    if (comp_size == 0xFFFFFFFFu && fpos + 8u <= esz) {
                        comp_size = read_u64_le(ep + epos + fpos);
                        fpos += 8u;
                    }
                    if (local_off == 0xFFFFFFFFu && fpos + 8u <= esz) {
                        local_off = read_u64_le(ep + epos + fpos);
                        fpos += 8u;
                    }
                    break;
                }
                epos += esz;
            }
        }

        std::string norm = normalize_rel(name_utf8);
        if (!norm.empty()) {
            RawEntry re{};
            re.norm_path = std::move(norm);
            re.local_offset = local_off;
            re.comp_size = comp_size;
            re.uncomp_size = uncomp_size;
            re.method = method;
            re.mtime_unix = dos_datetime_to_unix(mod_time, mod_date);
            re.is_dir = trailing_slash;
            raw_entries.push_back(std::move(re));
        }

        pos = next_pos;
    }

    // 4. Detect the game executable and optional wrapper root folder (e.g. "MyGame/MyGame.exe").
    std::string best_exe;
    usize best_depth = std::string::npos;
    u64 best_exe_size = 0;
    for (const auto& re : raw_entries) {
        if (re.is_dir) continue;
        const std::string lower = ascii_to_lower(re.norm_path);
        if (!lower.ends_with(".exe")) continue;
        const usize slash = lower.rfind('/');
        const std::string_view fname = (slash == std::string::npos)
                                           ? std::string_view(lower)
                                           : std::string_view(lower).substr(slash + 1);
        if (is_ignorable_exe_name(fname)) continue;
        const usize depth = static_cast<usize>(std::count(lower.begin(), lower.end(), '/'));
        if (depth < best_depth || (depth == best_depth && re.uncomp_size > best_exe_size)) {
            best_depth = depth;
            best_exe_size = re.uncomp_size;
            best_exe = re.norm_path;
        }
    }

    std::string strip_prefix;
    if (!best_exe.empty()) {
        const usize slash = best_exe.rfind('/');
        if (slash != std::string::npos) {
            strip_prefix = best_exe.substr(0, slash + 1);
            default_exe_ = best_exe.substr(slash + 1);
        } else {
            default_exe_ = best_exe;
        }
    }
    const std::string strip_prefix_lo = ascii_to_lower(strip_prefix);

    // 5. Build case-insensitive file & directory indexes relative to strip_prefix.
    dir_set_.insert("");
    std::unordered_map<std::string, std::unordered_set<std::string>> seen_children;

    auto ensure_parent_dirs = [&](std::string_view rel_path) {
        usize p = 0;
        std::string cur_dir;
        while (true) {
            const usize slash = rel_path.find('/', p);
            if (slash == std::string_view::npos) break;
            const std::string_view seg = rel_path.substr(p, slash - p);
            const std::string parent_lo = ascii_to_lower(cur_dir);
            if (!cur_dir.empty()) cur_dir.push_back('/');
            cur_dir.append(seg);
            const std::string dir_lo = ascii_to_lower(cur_dir);
            if (dir_set_.insert(dir_lo).second) {
                const std::string seg_lo = ascii_to_lower(seg);
                if (seen_children[parent_lo].insert(seg_lo).second) {
                    dir_children_[parent_lo].push_back(VfsDirEntry{std::string(seg), true});
                }
            }
            p = slash + 1;
        }
    };

    entries_.reserve(raw_entries.size());
    for (auto& re : raw_entries) {
        std::string rel = re.norm_path;
        if (!strip_prefix_lo.empty()) {
            const std::string rel_lo = ascii_to_lower(rel);
            if (!rel_lo.starts_with(strip_prefix_lo)) {
                continue;
            }
            rel.erase(0, strip_prefix.size());
        }
        if (rel.empty()) continue;

        ensure_parent_dirs(rel);
        const std::string rel_lo = ascii_to_lower(rel);
        const usize slash = rel.rfind('/');
        const std::string parent_lo = (slash == std::string::npos) ? std::string{} : ascii_to_lower(std::string_view(rel).substr(0, slash));
        const std::string leaf = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
        const std::string leaf_lo = ascii_to_lower(leaf);

        if (re.is_dir) {
            if (dir_set_.insert(rel_lo).second) {
                if (seen_children[parent_lo].insert(leaf_lo).second) {
                    dir_children_[parent_lo].push_back(VfsDirEntry{leaf, true});
                }
            }
            continue;
        }

        Entry e{};
        e.rel_path = std::move(rel);
        e.local_header_offset = re.local_offset;
        e.compressed_size = re.comp_size;
        e.uncompressed_size = re.uncomp_size;
        e.method = re.method;
        e.mtime_unix = re.mtime_unix;
        e.is_dir = false;

        const usize idx = entries_.size();
        entries_.push_back(std::move(e));
        file_index_[rel_lo] = idx;

        if (seen_children[parent_lo].insert(leaf_lo).second) {
            dir_children_[parent_lo].push_back(VfsDirEntry{leaf, false});
        }
    }

    return Result<void>::success();
}

bool ZipVfsSource::exists(std::string_view rel_path) const {
    const std::string key = ascii_to_lower(normalize_rel(rel_path));
    return file_index_.contains(key) || dir_set_.contains(key);
}

bool ZipVfsSource::directory_exists(std::string_view rel_path) const {
    const std::string key = ascii_to_lower(normalize_rel(rel_path));
    return dir_set_.contains(key);
}

Result<u64> ZipVfsSource::file_size(std::string_view rel_path) const {
    const std::string key = ascii_to_lower(normalize_rel(rel_path));
    const auto it = file_index_.find(key);
    if (it == file_index_.end()) {
        return Error{"File not found in ZIP archive: " + std::string(rel_path)};
    }
    return entries_[it->second].uncompressed_size;
}

std::optional<i64> ZipVfsSource::last_write_time_unix(std::string_view rel_path) const {
    const std::string key = ascii_to_lower(normalize_rel(rel_path));
    const auto it = file_index_.find(key);
    if (it == file_index_.end()) {
        return std::nullopt;
    }
    return entries_[it->second].mtime_unix;
}

std::vector<VfsDirEntry> ZipVfsSource::list_directory(std::string_view rel_dir) const {
    const std::string key = ascii_to_lower(normalize_rel(rel_dir));
    const auto it = dir_children_.find(key);
    if (it == dir_children_.end()) {
        return {};
    }
    return it->second;
}

Result<std::vector<u8>> ZipVfsSource::read_all_bytes(std::string_view rel_path) {
    const std::string key = ascii_to_lower(normalize_rel(rel_path));
    const auto it = file_index_.find(key);
    if (it == file_index_.end()) {
        return Error{"File not found in ZIP archive: " + std::string(rel_path)};
    }
    const Entry& e = entries_[it->second];

    // Read 30-byte Local File Header to locate exact compressed payload offset.
    u8 lfh[30]{};
    {
        std::scoped_lock lock(reader_mutex_);
        if (e.local_header_offset + sizeof(lfh) > archive_size_ ||
            !reader_(e.local_header_offset, lfh, sizeof(lfh))) {
            return Error{"Failed to read ZIP local header for: " + e.rel_path};
        }
    }
    if (read_u32_le(lfh) != kSigLocalFileHeader) {
        return Error{"Corrupt ZIP local header signature for: " + e.rel_path};
    }

    const u16 lfh_name_len = read_u16_le(lfh + 26u);
    const u16 lfh_extra_len = read_u16_le(lfh + 28u);
    const u64 data_offset = e.local_header_offset + 30u + lfh_name_len + lfh_extra_len;
    if (data_offset + e.compressed_size > archive_size_) {
        return Error{"ZIP entry data exceeds archive bounds: " + e.rel_path};
    }

    std::vector<u8> comp(static_cast<usize>(e.compressed_size));
    if (e.compressed_size > 0) {
        std::scoped_lock lock(reader_mutex_);
        if (!reader_(data_offset, comp.data(), comp.size())) {
            return Error{"Failed to read ZIP entry payload: " + e.rel_path};
        }
    }

    if (e.method == 0u) {
        return comp;
    }
    if (e.method == 8u) {
        return raw_inflate(comp);
    }
    return Error{"Unsupported ZIP compression method (" + std::to_string(e.method) + ") for: " + e.rel_path};
}

} // namespace gm
