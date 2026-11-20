#include "platform/vfs.h"

#include "base/encoding.h"
#include "base/log.h"
#include "platform/clock.h"

#include <algorithm>
#include <fstream>
#include <random>

namespace gm {

namespace {

bool is_windows_drive_path(std::string_view p) noexcept {
    return p.size() >= 2 &&
           ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
           p[1] == ':';
}

bool is_absolute_gm_path(std::string_view norm) noexcept {
    return (!norm.empty() && norm[0] == '/') || is_windows_drive_path(norm);
}

std::string canonical_pipe_key(std::string_view path) {
    return ascii_to_lower(Vfs::normalize_separators(path));
}

} // namespace

Vfs::Vfs() = default;

Vfs::~Vfs() {
    cleanup_temp_dir();
}

std::filesystem::path Vfs::path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string Vfs::path_to_utf8(const std::filesystem::path& path) {
    const std::u8string u8s = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8s.data()), u8s.size());
}

std::string Vfs::normalize_separators(std::string_view path) {
    std::string out(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

bool Vfs::is_virtual_pipe(std::string_view path) noexcept {
    if (path.size() < 9) {
        return false;
    }
    const std::string lower = ascii_to_lower(normalize_separators(path));
    return lower.starts_with("//./pipe/");
}

void Vfs::register_virtual_pipe(std::string_view pipe_path, std::vector<u8> data, bool one_shot) {
    std::scoped_lock lock(pipe_mutex_);
    const std::string key = canonical_pipe_key(pipe_path);
    const usize sz = data.size();
    virtual_pipes_[key] = VirtualPipeEntry{std::move(data), one_shot};
    if (Logger::trace_flags().vfs) {
        GM_LOG_TRACE("vfs", "Registered virtual pipe '%s' (%zu bytes)", key.c_str(), sz);
    }
}

void Vfs::unregister_virtual_pipe(std::string_view pipe_path) {
    std::scoped_lock lock(pipe_mutex_);
    virtual_pipes_.erase(canonical_pipe_key(pipe_path));
}

Result<void> Vfs::mount_game_root(const std::filesystem::path& game_root) {
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(game_root, ec);
    if (ec) {
        resolved = game_root;
    }
    game_root_ = resolved;
    if (save_root_.empty()) {
        save_root_ = game_root_;
    }
    GM_LOG_INFO("vfs", "Mounted game_root: %s", path_to_utf8(game_root_).c_str());
    return Result<void>::success();
}

Result<void> Vfs::mount_save_root(const std::filesystem::path& save_root) {
    std::error_code ec;
    std::filesystem::create_directories(save_root, ec);
    if (ec) {
        return Error{"Failed to create save_root: " + ec.message()};
    }
    save_root_ = std::filesystem::weakly_canonical(save_root, ec);
    if (ec) {
        save_root_ = save_root;
    }
    GM_LOG_INFO("vfs", "Mounted save_root: %s", path_to_utf8(save_root_).c_str());
    return Result<void>::success();
}

Result<void> Vfs::init_temp_dir(std::string_view prefix) {
    cleanup_temp_dir();

    std::error_code ec;
    const auto sys_tmp = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return Error{"Failed to locate system temp directory: " + ec.message()};
    }

    std::mt19937 rng(static_cast<u32>(Clock::now_us() & 0xFFFFFFFFu));
    std::uniform_int_distribution<u32> dist(10000u, 99999u);

    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::string name = std::string(prefix) + std::to_string(dist(rng));
        const auto candidate = sys_tmp / path_from_utf8(name);
        if (!std::filesystem::exists(candidate, ec)) {
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                temp_dir_ = candidate;
                owns_temp_dir_ = true;
                GM_LOG_INFO("vfs", "Initialized temp_dir: %s", path_to_utf8(temp_dir_).c_str());
                return Result<void>::success();
            }
        }
    }

    return Error{"Failed to allocate unique temp directory"};
}

void Vfs::cleanup_temp_dir() noexcept {
    if (owns_temp_dir_ && !temp_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
        owns_temp_dir_ = false;
        temp_dir_.clear();
    }
}

std::string Vfs::working_directory_gm() const {
    std::string s = path_to_utf8(game_root_);
    if (!s.empty() && s.back() != '/' && s.back() != '\\') {
        s.push_back('/');
    }
    return s;
}

std::string Vfs::temp_directory_gm() const {
    std::string s = path_to_utf8(temp_dir_);
    if (!s.empty() && s.back() != '/' && s.back() != '\\') {
        s.push_back('/');
    }
    return s;
}

std::filesystem::path Vfs::resolve_case_insensitive(
    const std::filesystem::path& base,
    std::string_view relative_norm) const {
    std::filesystem::path current = base;
    std::error_code ec;

    usize start = 0;
    while (start < relative_norm.size()) {
        usize slash = relative_norm.find('/', start);
        if (slash == std::string_view::npos) {
            slash = relative_norm.size();
        }
        const std::string_view part = relative_norm.substr(start, slash - start);
        start = slash + 1;

        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            current = current.parent_path();
            continue;
        }

        const auto direct = current / path_from_utf8(part);
        if (std::filesystem::exists(direct, ec) && !ec) {
            current = direct;
            continue;
        }

        // Case-insensitive directory scan fallback (critical for Linux / Android / case-sensitive APFS).
        bool matched = false;
        const std::string target_lower = ascii_to_lower(part);
        if (std::filesystem::is_directory(current, ec) && !ec) {
            for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
                if (ec) break;
                const std::string entry_name = path_to_utf8(entry.path().filename());
                if (ascii_to_lower(entry_name) == target_lower) {
                    current = entry.path();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            current = direct;
        }
    }

    return current;
}

std::filesystem::path Vfs::resolve_for_read(std::string_view gm_path) const {
    const std::string norm = normalize_separators(gm_path);
    std::error_code ec;

    if (is_absolute_gm_path(norm)) {
        const auto abs_path = path_from_utf8(norm);
        if (std::filesystem::exists(abs_path, ec) && !ec) {
            return abs_path;
        }
        return abs_path;
    }

    // Check writable save overlay first if distinct from game_root.
    if (!save_root_.empty() && save_root_ != game_root_) {
        const auto save_candidate = resolve_case_insensitive(save_root_, norm);
        if (std::filesystem::exists(save_candidate, ec) && !ec) {
            return save_candidate;
        }
    }

    if (!game_root_.empty()) {
        return resolve_case_insensitive(game_root_, norm);
    }
    return path_from_utf8(norm);
}

std::filesystem::path Vfs::resolve_for_write(std::string_view gm_path) const {
    const std::string norm = normalize_separators(gm_path);
    if (is_absolute_gm_path(norm)) {
        return path_from_utf8(norm);
    }
    const auto& root = !save_root_.empty() ? save_root_ : game_root_;
    if (root.empty()) {
        return path_from_utf8(norm);
    }
    return resolve_case_insensitive(root, norm);
}

bool Vfs::exists(std::string_view gm_path) const {
    if (is_virtual_pipe(gm_path)) {
        std::scoped_lock lock(pipe_mutex_);
        return virtual_pipes_.contains(canonical_pipe_key(gm_path));
    }
    std::error_code ec;
    return std::filesystem::exists(resolve_for_read(gm_path), ec) && !ec;
}

bool Vfs::directory_exists(std::string_view gm_path) const {
    std::error_code ec;
    return std::filesystem::is_directory(resolve_for_read(gm_path), ec) && !ec;
}

Result<u64> Vfs::file_size(std::string_view gm_path) const {
    if (is_virtual_pipe(gm_path)) {
        std::scoped_lock lock(pipe_mutex_);
        const auto it = virtual_pipes_.find(canonical_pipe_key(gm_path));
        if (it == virtual_pipes_.end()) {
            return Error{"Virtual pipe not found: " + std::string(gm_path)};
        }
        return static_cast<u64>(it->second.data.size());
    }
    std::error_code ec;
    const auto path = resolve_for_read(gm_path);
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        return Error{"Failed to query file_size for '" + std::string(gm_path) + "': " + ec.message()};
    }
    return static_cast<u64>(sz);
}

Result<std::vector<u8>> Vfs::read_all_bytes(std::string_view gm_path) {
    if (is_virtual_pipe(gm_path)) {
        std::scoped_lock lock(pipe_mutex_);
        const std::string key = canonical_pipe_key(gm_path);
        const auto it = virtual_pipes_.find(key);
        if (it == virtual_pipes_.end()) {
            return Error{"Virtual pipe not found: " + std::string(gm_path)};
        }
        std::vector<u8> bytes = it->second.data;
        if (it->second.one_shot) {
            virtual_pipes_.erase(it);
        }
        return bytes;
    }

    const auto path = resolve_for_read(gm_path);
    if (Logger::trace_flags().vfs) {
        GM_LOG_TRACE("vfs", "read_all_bytes('%.*s') -> '%s'",
                     static_cast<int>(gm_path.size()), gm_path.data(),
                     path_to_utf8(path).c_str());
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return Error{"Failed to open file for reading: " + path_to_utf8(path)};
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        return Error{"Failed to determine file size: " + path_to_utf8(path)};
    }
    file.seekg(0, std::ios::beg);

    std::vector<u8> buffer(static_cast<usize>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return Error{"Failed to read file contents: " + path_to_utf8(path)};
    }
    return buffer;
}

Result<void> Vfs::write_all_bytes(std::string_view gm_path, std::span<const u8> data) {
    const auto path = resolve_for_write(gm_path);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return Error{"Failed to open file for writing: " + path_to_utf8(path)};
    }
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!file.good()) {
        return Error{"Failed while writing file: " + path_to_utf8(path)};
    }
    return Result<void>::success();
}

Result<void> Vfs::delete_file(std::string_view gm_path) {
    std::error_code ec;
    std::filesystem::remove(resolve_for_write(gm_path), ec);
    if (ec) {
        return Error{"Failed to delete file '" + std::string(gm_path) + "': " + ec.message()};
    }
    return Result<void>::success();
}

Result<void> Vfs::create_directory(std::string_view gm_path) {
    std::error_code ec;
    std::filesystem::create_directories(resolve_for_write(gm_path), ec);
    if (ec) {
        return Error{"Failed to create directory '" + std::string(gm_path) + "': " + ec.message()};
    }
    return Result<void>::success();
}

bool Vfs::wildcard_match_ci(std::string_view pattern, std::string_view text) noexcept {
    usize p = 0;
    usize t = 0;
    usize star_p = std::string_view::npos;
    usize star_t = 0;

    auto lower = [](char c) noexcept -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

std::vector<std::string> Vfs::find_files(std::string_view gm_pattern, u32 attr_mask) const {
    const std::string norm = normalize_separators(gm_pattern);
    const usize last_slash = norm.rfind('/');
    const std::string_view dir_part = (last_slash == std::string_view::npos) ? std::string_view{} : std::string_view(norm).substr(0, last_slash);
    const std::string_view mask_part = (last_slash == std::string_view::npos) ? std::string_view(norm) : std::string_view(norm).substr(last_slash + 1);

    const std::filesystem::path dir = dir_part.empty() ? game_root_ : resolve_for_read(dir_part);
    std::vector<std::string> results;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return results;
    }

    const bool want_dirs = (attr_mask & kFaDirectory) != 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        const bool is_dir = entry.is_directory(ec);
        if (is_dir && !want_dirs) {
            continue;
        }
        const std::string name = path_to_utf8(entry.path().filename());
        if (wildcard_match_ci(mask_part, name)) {
            results.push_back(name);
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}

std::string Vfs::make_registry_key(std::string_view key, std::string_view name) const {
    return std::to_string(registry_root_) + "|" + ascii_to_lower(key) + "|" + ascii_to_lower(name);
}

void Vfs::registry_set_root(i32 root_id) {
    registry_root_ = root_id;
}

void Vfs::registry_write_string(std::string_view key, std::string_view name, std::string_view value) {
    registry_strings_[make_registry_key(key, name)] = std::string(value);
}

void Vfs::registry_write_real(std::string_view key, std::string_view name, u32 value) {
    registry_reals_[make_registry_key(key, name)] = value;
}

std::string Vfs::registry_read_string(std::string_view key, std::string_view name) const {
    const auto it = registry_strings_.find(make_registry_key(key, name));
    return it != registry_strings_.end() ? it->second : std::string{};
}

u32 Vfs::registry_read_real(std::string_view key, std::string_view name) const {
    const auto it = registry_reals_.find(make_registry_key(key, name));
    return it != registry_reals_.end() ? it->second : 0u;
}

bool Vfs::registry_exists(std::string_view key, std::string_view name) const {
    const std::string k = make_registry_key(key, name);
    return registry_strings_.contains(k) || registry_reals_.contains(k);
}

} // namespace gm
