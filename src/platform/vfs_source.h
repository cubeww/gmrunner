#pragma once

#include "base/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

struct VfsDirEntry {
    std::string name;
    bool is_directory = false;
};

// Pluggable read-only game root backend (e.g. streaming ZIP archive or
// browser lazy-file directory handle) mounted beneath the writable save overlay.
class IVfsReadSource {
public:
    virtual ~IVfsReadSource() = default;

    // Relative paths use forward slashes ('/'), without leading slash.
    [[nodiscard]] virtual bool exists(std::string_view rel_path) const = 0;
    [[nodiscard]] virtual bool directory_exists(std::string_view rel_path) const = 0;
    [[nodiscard]] virtual Result<u64> file_size(std::string_view rel_path) const = 0;
    [[nodiscard]] virtual Result<std::vector<u8>> read_all_bytes(std::string_view rel_path) = 0;
    [[nodiscard]] virtual std::vector<VfsDirEntry> list_directory(std::string_view rel_dir) const = 0;
    [[nodiscard]] virtual std::optional<i64> last_write_time_unix(std::string_view rel_path) const = 0;

    // Auto-detected game executable relative path inside this source (if any).
    [[nodiscard]] virtual std::string default_exe_path() const = 0;
    [[nodiscard]] virtual std::string display_name() const = 0;
};

} // namespace gm
