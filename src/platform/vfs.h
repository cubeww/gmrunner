#pragma once

#include "base/types.h"
#include "platform/vfs_source.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace gm {

// GM8 file_find attribute bitmask (matching Windows FILE_ATTRIBUTE_* flags).
enum FileFindAttr : u32 {
    kFaReadOnly = 0x01,
    kFaHidden = 0x02,
    kFaSysFile = 0x04,
    kFaVolumeId = 0x08,
    kFaDirectory = 0x10,
    kFaArchive = 0x20,
};

// Cross-platform Virtual Filesystem:
// - Normalizes Windows backslashes ('\') to '/'
// - Resolves paths case-insensitively on case-sensitive filesystems (Linux/Android/APFS)
//   using a per-directory lowercase lookup cache
// - Supports mounting local directories, streaming zero-extraction ZIP archives, and
//   pluggable read-only sources (e.g. WebAssembly lazy browser folder handles)
// - Maintains a writable save overlay and ephemeral temp_directory
// - Intercepts virtual named pipes (e.g. \\.\pipe\gm82netpipe.*) in memory for gm82buf/d3d_model_load
// - Emulates the Windows registry in the runner's per-user or persistent data directory
class Vfs : public NonCopyable {
public:
    Vfs();
    ~Vfs();

    [[nodiscard]] Result<void> mount_game_root(const std::filesystem::path& game_root);
    [[nodiscard]] Result<void> mount_zip(const std::filesystem::path& zip_path);
    [[nodiscard]] Result<void> mount_read_source(
        std::unique_ptr<IVfsReadSource> source,
        const std::filesystem::path& virtual_root = std::filesystem::path("/game"));
    [[nodiscard]] Result<void> mount_save_root(const std::filesystem::path& save_root);
    [[nodiscard]] Result<void> init_temp_dir(std::string_view prefix = "gm_ttt_");
    void cleanup_temp_dir() noexcept;

    [[nodiscard]] bool has_virtual_root() const noexcept { return virtual_root_ != nullptr; }
    [[nodiscard]] std::string default_exe_path() const;

    [[nodiscard]] const std::filesystem::path& game_root() const noexcept { return game_root_; }
    [[nodiscard]] const std::filesystem::path& save_root() const noexcept { return save_root_; }
    [[nodiscard]] const std::filesystem::path& temp_dir() const noexcept { return temp_dir_; }

    void set_registry_dir(const std::filesystem::path& dir) { registry_dir_ = dir; }
    [[nodiscard]] const std::filesystem::path& registry_dir_override() const noexcept { return registry_dir_; }
    void set_persist_callback(std::function<void()> cb) { persist_callback_ = std::move(cb); }
    void notify_persist_write() const;

    [[nodiscard]] std::string working_directory_gm() const;
    [[nodiscard]] std::string program_directory_gm() const;
    [[nodiscard]] std::string absolute_path_gm(std::string_view gm_path) const;
    void set_working_directory(std::string_view gm_path);
    [[nodiscard]] std::string temp_directory_gm() const;

    // Virtual in-memory named pipes (cross-platform replacement for \\.\pipe\gm82netpipe.*).
    [[nodiscard]] static bool is_virtual_pipe(std::string_view path) noexcept;
    void register_virtual_pipe(std::string_view pipe_path, std::vector<u8> data, bool one_shot = true);
    void unregister_virtual_pipe(std::string_view pipe_path);

    // GameMaker 8.0 paths use the game's Windows codepage. Host paths start in UTF-8.
    void set_game_path_codepage(u32 codepage) noexcept { game_path_codepage_ = codepage; }
    [[nodiscard]] static std::string normalize_separators(std::string_view path);
    [[nodiscard]] std::filesystem::path resolve_for_read(std::string_view gm_path) const;
    [[nodiscard]] std::filesystem::path resolve_for_write(std::string_view gm_path) const;
    void invalidate_dir_cache() const;

    // File operations.
    [[nodiscard]] bool exists(std::string_view gm_path) const;
    [[nodiscard]] bool directory_exists(std::string_view gm_path) const;
    [[nodiscard]] Result<u64> file_size(std::string_view gm_path) const;
    [[nodiscard]] std::optional<i64> last_write_time_unix(std::string_view gm_path) const;
    [[nodiscard]] Result<std::vector<u8>> read_all_bytes(std::string_view gm_path);
    [[nodiscard]] Result<void> write_all_bytes(std::string_view gm_path, std::span<const u8> data);
    [[nodiscard]] Result<void> delete_file(std::string_view gm_path);
    [[nodiscard]] Result<void> create_directory(std::string_view gm_path);

    // Wildcard directory search (* and ?) matching GM8 file_find_first / file_find_next.
    [[nodiscard]] std::vector<std::string> find_files(std::string_view gm_pattern, u32 attr_mask = 0) const;

    // An omitted key uses HKCU\Software\Game Maker\<game_id>, independent of the selected root.
    void registry_init(i32 game_id);
    void registry_set_root(i32 root_id);
    void registry_write_string(std::optional<std::string_view> key, std::string_view name, std::string_view value);
    void registry_write_real(std::optional<std::string_view> key, std::string_view name, f64 value);
    [[nodiscard]] std::string registry_read_string(std::optional<std::string_view> key, std::string_view name) const;
    [[nodiscard]] f64 registry_read_real(std::optional<std::string_view> key, std::string_view name) const;
    [[nodiscard]] bool registry_exists(std::optional<std::string_view> key, std::string_view name) const;

    [[nodiscard]] static std::filesystem::path path_from_utf8(std::string_view utf8);
    [[nodiscard]] static std::string path_to_utf8(const std::filesystem::path& path);
    [[nodiscard]] static bool is_directory_writable(const std::filesystem::path& dir);

private:
    struct VirtualPipeEntry {
        std::vector<u8> data;
        bool one_shot = true;
    };

    struct FilePaths {
        std::filesystem::path game;
        std::filesystem::path overlay;
        std::string rel_in_root;
        bool under_game_root = false;
        [[nodiscard]] bool overlay_exists() const;
        [[nodiscard]] const std::filesystem::path& for_read() const;
    };

    [[nodiscard]] std::filesystem::path resolve_case_insensitive(
        const std::filesystem::path& base,
        std::string_view relative_norm) const;
    [[nodiscard]] FilePaths resolve_paths(std::string_view gm_path) const;
    void invalidate_dir_cache_for(const std::filesystem::path& dir) const;
    [[nodiscard]] bool is_tombstoned(std::string_view rel_in_root) const;

    [[nodiscard]] static bool wildcard_match_ci(std::string_view pattern, std::string_view text) noexcept;
    using RegistryValue = std::variant<f64, std::string>;
    [[nodiscard]] std::optional<RegistryValue> access_registry_value(
        std::optional<std::string_view> key, std::string_view name, const RegistryValue* replacement = nullptr) const;

    std::filesystem::path game_root_;
    std::filesystem::path working_directory_;
    std::filesystem::path save_root_;
    std::filesystem::path temp_dir_;
    std::filesystem::path registry_dir_;
    bool owns_temp_dir_ = false;
    u32 game_path_codepage_ = 65001;

    std::unique_ptr<IVfsReadSource> virtual_root_;
    std::unordered_set<std::string> deleted_rel_paths_;
    std::function<void()> persist_callback_;

    mutable std::mutex dir_cache_mutex_;
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>> dir_cache_;

    mutable std::mutex pipe_mutex_;
    std::unordered_map<std::string, VirtualPipeEntry> virtual_pipes_;

    i32 registry_root_ = 0; // 0 = HKCU
    std::string registry_game_key_;
};

} // namespace gm
