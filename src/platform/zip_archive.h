#pragma once

#include "base/types.h"
#include "platform/vfs_source.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gm {

// Streaming, zero-extraction read-only ZIP32/ZIP64 archive backend for Vfs.
// Parses only the Central Directory on mount and reads/inflates individual
// entries on demand via random-access range reads.
class ZipVfsSource final : public IVfsReadSource {
public:
    using ReadRangeFn = std::function<bool(u64 offset, u8* dst, usize len)>;

    [[nodiscard]] static Result<std::unique_ptr<ZipVfsSource>> open_file(const std::filesystem::path& zip_path);
    [[nodiscard]] static Result<std::unique_ptr<ZipVfsSource>> open_reader(
        u64 archive_size,
        ReadRangeFn reader,
        std::string display_name);

    [[nodiscard]] bool exists(std::string_view rel_path) const override;
    [[nodiscard]] bool directory_exists(std::string_view rel_path) const override;
    [[nodiscard]] Result<u64> file_size(std::string_view rel_path) const override;
    [[nodiscard]] Result<std::vector<u8>> read_all_bytes(std::string_view rel_path) override;
    [[nodiscard]] std::vector<VfsDirEntry> list_directory(std::string_view rel_dir) const override;
    [[nodiscard]] std::optional<i64> last_write_time_unix(std::string_view rel_path) const override;
    [[nodiscard]] std::string default_exe_path() const override { return default_exe_; }
    [[nodiscard]] std::string display_name() const override { return display_name_; }

    [[nodiscard]] usize entry_count() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::string rel_path; // Original-cased path relative to stripped root prefix
        u64 local_header_offset = 0;
        u64 compressed_size = 0;
        u64 uncompressed_size = 0;
        u16 method = 0;
        i64 mtime_unix = 0;
        bool is_dir = false;
    };

    Result<void> parse_central_directory();
    [[nodiscard]] static std::string normalize_rel(std::string_view path);

    u64 archive_size_ = 0;
    ReadRangeFn reader_;
    mutable std::mutex reader_mutex_;
    std::string display_name_;
    std::string default_exe_;

    std::vector<Entry> entries_;
    std::unordered_map<std::string, usize> file_index_; // lowercase rel_path -> entries_ index
    std::unordered_set<std::string> dir_set_;           // lowercase rel_dir ("" for root)
    std::unordered_map<std::string, std::vector<VfsDirEntry>> dir_children_;
};

} // namespace gm
