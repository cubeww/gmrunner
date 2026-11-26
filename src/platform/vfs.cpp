#include "platform/vfs.h"

#include "base/encoding.h"
#include "base/log.h"
#include "platform/clock.h"
#include "platform/zip_archive.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <unordered_set>

namespace gm {

namespace {

std::string canonical_pipe_key(std::string_view path) {
    return ascii_to_lower(Vfs::normalize_separators(path));
}

std::string canonical_rel_key(std::string_view rel) {
    std::string s = Vfs::normalize_separators(rel);
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    while (!s.empty() && s.back() == '/') s.pop_back();
    return ascii_to_lower(s);
}

std::optional<i64> fs_last_write_unix(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return std::nullopt;
    const auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;
    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<i64>(std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count());
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

bool Vfs::is_directory_writable(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    const u64 suffix = Clock::now_us();
    for (u64 attempt = 0; attempt < 32; ++attempt) {
        const auto probe = dir / path_from_utf8(".gmrunner_write_probe_" + std::to_string(suffix + attempt));
#if defined(_WIN32)
        std::FILE* file = nullptr;
        const int open_error = _wfopen_s(&file, probe.c_str(), L"wbx");
#else
        std::FILE* file = std::fopen(probe.c_str(), "wbx");
        const int open_error = errno;
#endif
        if (file) {
            std::fclose(file);
            return std::filesystem::remove(probe, ec);
        }
        if (open_error != EEXIST) return false;
    }
    return false;
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

void Vfs::invalidate_dir_cache() const {
    std::scoped_lock lock(dir_cache_mutex_);
    dir_cache_.clear();
}

void Vfs::invalidate_dir_cache_for(const std::filesystem::path& dir) const {
    if (dir.empty()) return;
    std::scoped_lock lock(dir_cache_mutex_);
    dir_cache_.erase(path_to_utf8(dir));
}

void Vfs::notify_persist_write() const {
    if (persist_callback_) {
        persist_callback_();
    }
}

bool Vfs::is_tombstoned(std::string_view rel_in_root) const {
    if (deleted_rel_paths_.empty()) return false;
    return deleted_rel_paths_.contains(canonical_rel_key(rel_in_root));
}

Result<void> Vfs::mount_game_root(const std::filesystem::path& game_root) {
    virtual_root_.reset();
    deleted_rel_paths_.clear();
    invalidate_dir_cache();

    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(game_root, ec);
    if (ec) {
        resolved = game_root;
    }
    game_root_ = resolved.make_preferred();
    working_directory_ = game_root_;
    if (save_root_.empty()) {
        save_root_ = game_root_;
    }
    GM_LOG_INFO("vfs", "Mounted game_root: %s", path_to_utf8(game_root_).c_str());
    return Result<void>::success();
}

Result<void> Vfs::mount_zip(const std::filesystem::path& zip_path) {
    auto zip_res = ZipVfsSource::open_file(zip_path);
    if (!zip_res) return zip_res.error();
    return mount_read_source(std::move(zip_res.value()), std::filesystem::path("/game"));
}

Result<void> Vfs::mount_read_source(
    std::unique_ptr<IVfsReadSource> source,
    const std::filesystem::path& virtual_root) {
    if (!source) {
        return Error{"Cannot mount null VFS read source"};
    }
    virtual_root_ = std::move(source);
    deleted_rel_paths_.clear();
    invalidate_dir_cache();
    game_root_ = virtual_root;
    working_directory_ = virtual_root;
    GM_LOG_INFO("vfs", "Mounted virtual game_root '%s' -> %s",
                path_to_utf8(game_root_).c_str(),
                virtual_root_->display_name().c_str());
    return Result<void>::success();
}

std::string Vfs::default_exe_path() const {
    return virtual_root_ ? virtual_root_->default_exe_path() : std::string{};
}

Result<void> Vfs::mount_save_root(const std::filesystem::path& save_root) {
    invalidate_dir_cache();
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
    std::filesystem::path sys_tmp;
#if defined(__ANDROID__)
    if (const char* cache_path = SDL_GetAndroidCachePath()) {
        sys_tmp = path_from_utf8(cache_path);
    }
    if (sys_tmp.empty()) {
        if (char* pref = SDL_GetPrefPath("gmrunner", "temp")) {
            sys_tmp = path_from_utf8(pref);
            SDL_free(pref);
        }
    }
#elif defined(__EMSCRIPTEN__)
    sys_tmp = std::filesystem::path("/tmp");
    std::filesystem::create_directories(sys_tmp, ec);
    ec.clear();
#else
    sys_tmp = std::filesystem::temp_directory_path(ec);
    if (ec || sys_tmp.empty()) {
        ec.clear();
        if (char* pref = SDL_GetPrefPath("gmrunner", "temp")) {
            sys_tmp = path_from_utf8(pref);
            SDL_free(pref);
        }
    }
#endif
    if (sys_tmp.empty()) {
        return Error{"Failed to locate writable temp directory"};
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

std::string Vfs::directory_name_gm(const std::filesystem::path& dir) const {
    const std::string native = path_to_utf8(dir);
    if (game_path_codepage_ == 65001 || std::all_of(native.begin(), native.end(), [](unsigned char c) { return c < 128; })) {
        return native;
    }
    std::string name = utf8_to_codepage(native, game_path_codepage_);
    const std::string decoded = codepage_to_utf8(name, game_path_codepage_);
    if (native == decoded) return name;

    // A legacy script may append a filename to working_directory. Remember
    // the native prefix even when its visible spelling contains replacements.
    const auto key = ascii_to_lower(normalize_separators(decoded));
    auto [it, inserted] = directory_aliases_.try_emplace(key, dir);
    if (inserted || it->second == dir) return name;
    for (unsigned i = 2; ; ++i) {
        const auto suffix = "~" + std::to_string(i);
        auto [alias, added] = directory_aliases_.try_emplace(key + suffix, dir);
        if (added || alias->second == dir) return name + suffix;
    }
}

std::string Vfs::working_directory_gm() const {
    return directory_name_gm(working_directory_);
}

std::string Vfs::program_directory_gm() const {
    return directory_name_gm(game_root_);
}

std::filesystem::path Vfs::capture_file_path(std::string_view gm_path) const {
    if (gm_path.empty()) return {};
    return resolve_paths(gm_path).game;
}

void Vfs::set_working_directory(std::string_view gm_path) {
    if (gm_path.empty()) return;
    auto paths = resolve_paths(gm_path);
    if (directory_exists(gm_path)) {
        working_directory_ = std::move(paths.game);
    }
}

std::string Vfs::temp_directory_gm() const {
    return directory_name_gm(temp_dir_);
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

        // Case-insensitive directory lookup with per-directory cache (critical for
        // Linux / Android / case-sensitive APFS with thousands of external assets).
        bool matched = false;
        const std::string target_lower = ascii_to_lower(part);
        const std::string dir_key = path_to_utf8(current);
        {
            std::scoped_lock lock(dir_cache_mutex_);
            auto cache_it = dir_cache_.find(dir_key);
            if (cache_it == dir_cache_.end()) {
                std::unordered_map<std::string, std::string> entries;
                if (std::filesystem::is_directory(current, ec) && !ec) {
                    for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
                        if (ec) break;
                        const std::string entry_name = path_to_utf8(entry.path().filename());
                        entries.emplace(ascii_to_lower(entry_name), entry_name);
                    }
                }
                cache_it = dir_cache_.emplace(dir_key, std::move(entries)).first;
            }
            if (const auto hit = cache_it->second.find(target_lower); hit != cache_it->second.end()) {
                current = current / path_from_utf8(hit->second);
                matched = true;
            }
        }
        if (!matched) {
            current = direct;
        }
    }

    return current;
}

bool Vfs::FilePaths::overlay_exists() const {
    std::error_code ec;
    return !overlay.empty() && std::filesystem::exists(overlay, ec) && !ec;
}

const std::filesystem::path& Vfs::FilePaths::for_read() const {
    return overlay_exists() ? overlay : game;
}

Vfs::FilePaths Vfs::resolve_paths(std::string_view gm_path) const {
    const std::string norm = normalize_separators(codepage_to_utf8(gm_path, game_path_codepage_));
    auto input = path_from_utf8(norm);
    if (!directory_aliases_.empty() && input.has_root_directory()) {
        // Walk upwards to match complete components and prefer the longest prefix.
        std::filesystem::path suffix;
        for (auto prefix = input; prefix != prefix.root_path(); prefix = prefix.parent_path()) {
            const auto key = ascii_to_lower(normalize_separators(path_to_utf8(prefix)));
            if (auto it = directory_aliases_.find(key); it != directory_aliases_.end()) {
                input = suffix.empty() ? it->second : it->second / suffix;
                break;
            }
            if (prefix.has_filename()) suffix = suffix.empty() ? prefix.filename() : prefix.filename() / suffix;
        }
    }
    return resolve_native_paths(input);
}

Vfs::FilePaths Vfs::resolve_native_paths(const std::filesystem::path& input) const {
    auto path = (working_directory_ / input).lexically_normal();
    if (!path.has_filename() && path.has_relative_path()) path = path.parent_path();
    auto part = path.begin();
    auto root = game_root_.begin();
    while (root != game_root_.end() && part != path.end() &&
           (*root == *part || ascii_to_lower(path_to_utf8(*root)) == ascii_to_lower(path_to_utf8(*part)))) {
        ++part;
        ++root;
    }
    if (!game_root_.empty() && root == game_root_.end()) {
        std::filesystem::path relative;
        for (; part != path.end(); ++part) relative /= *part;
        const auto suffix = normalize_separators(path_to_utf8(relative));
        FilePaths paths{};
        paths.rel_in_root = suffix;
        paths.under_game_root = true;
        if (virtual_root_) {
            paths.game = game_root_ / relative;
            if (!save_root_.empty()) {
                paths.overlay = resolve_case_insensitive(save_root_, suffix);
            }
        } else {
            paths.game = resolve_case_insensitive(game_root_, suffix);
            if (!save_root_.empty() && save_root_ != game_root_) {
                paths.overlay = resolve_case_insensitive(save_root_, suffix);
            }
        }
        return paths;
    }
    FilePaths external{};
    external.game = resolve_case_insensitive(path.root_path(), normalize_separators(path_to_utf8(path.relative_path())));
    return external;
}

std::filesystem::path Vfs::resolve_for_read(std::string_view gm_path) const {
    return resolve_paths(gm_path).for_read();
}

std::filesystem::path Vfs::resolve_for_write(std::string_view gm_path) const {
    auto paths = resolve_paths(gm_path);
    auto out = paths.overlay.empty() ? std::move(paths.game) : std::move(paths.overlay);
    if (out.has_parent_path()) {
        invalidate_dir_cache_for(out.parent_path());
    }
    return out;
}

bool Vfs::exists(std::string_view gm_path) const {
    if (is_virtual_pipe(gm_path)) {
        std::scoped_lock lock(pipe_mutex_);
        return virtual_pipes_.contains(canonical_pipe_key(gm_path));
    }
    const auto paths = resolve_paths(gm_path);
    if (paths.overlay_exists()) return true;
    if (paths.under_game_root && is_tombstoned(paths.rel_in_root)) return false;
    if (paths.under_game_root && virtual_root_) {
        return virtual_root_->exists(paths.rel_in_root);
    }
    std::error_code ec;
    return std::filesystem::exists(paths.game, ec) && !ec;
}

bool Vfs::directory_exists(std::string_view gm_path) const {
    const auto paths = resolve_paths(gm_path);
    std::error_code ec;
    if (!paths.overlay.empty() && std::filesystem::is_directory(paths.overlay, ec) && !ec) {
        return true;
    }
    if (paths.under_game_root && virtual_root_) {
        return virtual_root_->directory_exists(paths.rel_in_root);
    }
    return std::filesystem::is_directory(paths.game, ec) && !ec;
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
    const auto paths = resolve_paths(gm_path);
    if (paths.overlay_exists()) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(paths.overlay, ec);
        if (!ec) return static_cast<u64>(sz);
    }
    if (paths.under_game_root && is_tombstoned(paths.rel_in_root)) {
        return Error{"File not found: " + std::string(gm_path)};
    }
    if (paths.under_game_root && virtual_root_) {
        return virtual_root_->file_size(paths.rel_in_root);
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(paths.game, ec);
    if (ec) {
        return Error{"Failed to query file_size for '" + std::string(gm_path) + "': " + ec.message()};
    }
    return static_cast<u64>(sz);
}

std::optional<i64> Vfs::last_write_time_unix(std::string_view gm_path) const {
    const auto paths = resolve_paths(gm_path);
    if (paths.overlay_exists()) {
        return fs_last_write_unix(paths.overlay);
    }
    if (paths.under_game_root && is_tombstoned(paths.rel_in_root)) {
        return std::nullopt;
    }
    if (paths.under_game_root && virtual_root_) {
        return virtual_root_->last_write_time_unix(paths.rel_in_root);
    }
    return fs_last_write_unix(paths.game);
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

    const auto paths = resolve_paths(gm_path);
    if (Logger::trace_flags().vfs) {
        GM_LOG_TRACE("vfs", "read_all_bytes('%.*s') -> rel='%s' (virtual=%d)",
                     static_cast<int>(gm_path.size()), gm_path.data(),
                     paths.rel_in_root.c_str(), virtual_root_ ? 1 : 0);
    }

    if (!paths.overlay_exists() && paths.under_game_root) {
        if (is_tombstoned(paths.rel_in_root)) {
            return Error{"File has been deleted: " + std::string(gm_path)};
        }
        if (virtual_root_) {
            return virtual_root_->read_all_bytes(paths.rel_in_root);
        }
    }

    const auto& path = paths.for_read();
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
    return write_resolved_bytes(resolve_paths(gm_path), data);
}

Result<void> Vfs::write_file_at(const std::filesystem::path& path, std::span<const u8> data) {
    return write_resolved_bytes(resolve_native_paths(path), data);
}

Result<void> Vfs::write_resolved_bytes(const FilePaths& paths, std::span<const u8> data) {
    const auto& path = paths.overlay.empty() ? paths.game : paths.overlay;
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        invalidate_dir_cache_for(path.parent_path());
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

    if (paths.under_game_root && !paths.rel_in_root.empty()) {
        deleted_rel_paths_.erase(canonical_rel_key(paths.rel_in_root));
    }
    if (!paths.overlay.empty() || paths.under_game_root) {
        notify_persist_write();
    }
    return Result<void>::success();
}

Result<void> Vfs::delete_file(std::string_view gm_path) {
    const auto paths = resolve_paths(gm_path);
    const auto& path = paths.overlay.empty() ? paths.game : paths.overlay;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::filesystem::remove(path, ec);
        if (ec) {
            return Error{"Failed to delete file '" + std::string(gm_path) + "': " + ec.message()};
        }
        if (path.has_parent_path()) {
            invalidate_dir_cache_for(path.parent_path());
        }
    }
    if (paths.under_game_root && (!paths.overlay.empty() || virtual_root_)) {
        deleted_rel_paths_.insert(canonical_rel_key(paths.rel_in_root));
    }
    notify_persist_write();
    return Result<void>::success();
}

Result<void> Vfs::create_directory(std::string_view gm_path) {
    const auto paths = resolve_paths(gm_path);
    const auto& path = paths.overlay.empty() ? paths.game : paths.overlay;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return Error{"Failed to create directory '" + std::string(gm_path) + "': " + ec.message()};
    }
    if (path.has_parent_path()) {
        invalidate_dir_cache_for(path.parent_path());
    }
    notify_persist_write();
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
        if (p < pattern.size() && pattern[p] == '?') {
            ++p;
            (void)utf8_decode_next(text, t);
        } else if (p < pattern.size() && lower(pattern[p]) == lower(text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            (void)utf8_decode_next(text, star_t);
            t = star_t;
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
    const std::string norm = normalize_separators(codepage_to_utf8(gm_pattern, game_path_codepage_));
    const usize last_slash = norm.rfind('/');
    const std::string_view dir_part = (last_slash == std::string_view::npos) ? std::string_view{} : std::string_view(norm).substr(0, last_slash + 1);
    const std::string_view mask_part = (last_slash == std::string_view::npos) ? std::string_view(norm) : std::string_view(norm).substr(last_slash + 1);

    const auto dirs = resolve_paths(utf8_to_codepage(dir_part, game_path_codepage_));
    std::vector<std::string> results;
    std::error_code ec;
    const bool want_dirs = (attr_mask & kFaDirectory) != 0;
    std::unordered_set<std::string> seen;

    // 1. Scan writable overlay first (if present).
    if (!dirs.overlay.empty() && std::filesystem::is_directory(dirs.overlay, ec) && !ec) {
        for (const auto& entry : std::filesystem::directory_iterator(dirs.overlay, ec)) {
            if (ec) break;
            const std::string name = path_to_utf8(entry.path().filename());
            if (!seen.insert(ascii_to_lower(name)).second) continue;
            if ((want_dirs || !entry.is_directory(ec)) && wildcard_match_ci(mask_part, name)) {
                results.push_back(utf8_to_codepage(name, game_path_codepage_));
            }
        }
    }

    // 2. Scan virtual read-only game root OR host filesystem game root.
    if (dirs.under_game_root && virtual_root_) {
        const auto children = virtual_root_->list_directory(dirs.rel_in_root);
        for (const auto& ch : children) {
            const std::string rel_child = dirs.rel_in_root.empty() ? ch.name : (dirs.rel_in_root + "/" + ch.name);
            if (is_tombstoned(rel_child)) continue;
            if (!seen.insert(ascii_to_lower(ch.name)).second) continue;
            if ((want_dirs || !ch.is_directory) && wildcard_match_ci(mask_part, ch.name)) {
                results.push_back(utf8_to_codepage(ch.name, game_path_codepage_));
            }
        }
    } else if (std::filesystem::is_directory(dirs.game, ec) && !ec) {
        for (const auto& entry : std::filesystem::directory_iterator(dirs.game, ec)) {
            if (ec) break;
            const std::string name = path_to_utf8(entry.path().filename());
            const std::string rel_child = dirs.rel_in_root.empty() ? name : (dirs.rel_in_root + "/" + name);
            if (dirs.under_game_root && is_tombstoned(rel_child)) continue;
            if (!seen.insert(ascii_to_lower(name)).second) continue;
            if ((want_dirs || !entry.is_directory(ec)) && wildcard_match_ci(mask_part, name)) {
                results.push_back(utf8_to_codepage(name, game_path_codepage_));
            }
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}

} // namespace gm
