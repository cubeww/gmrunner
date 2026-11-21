#include "platform/vfs.h"

#include "base/encoding.h"
#include "base/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <share.h>
#else
#include <cerrno>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace gm {

namespace {

struct RegistryEntry {
    std::string key;
    std::string name;
    std::variant<f64, std::string> value;
};

bool equal_registry_name(const std::string& lhs, const std::string& rhs) {
#if defined(_WIN32)
    const auto a = utf8_to_utf16(lhs);
    const auto b = utf8_to_utf16(rhs);
    return CompareStringOrdinal(reinterpret_cast<LPCWCH>(a.c_str()), -1,
                                reinterpret_cast<LPCWCH>(b.c_str()), -1, TRUE) == CSTR_EQUAL;
#else
    return SDL_strcasecmp(lhs.c_str(), rhs.c_str()) == 0;
#endif
}

const std::filesystem::path& registry_directory() {
    static const auto directory = [] {
        std::unique_ptr<char, decltype(&SDL_free)> path(SDL_GetPrefPath("gmrunner", "registry"), SDL_free);
        return path ? Vfs::path_from_utf8(path.get()) : std::filesystem::path{};
    }();
    return directory;
}

using FileLock = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

// Lock a separate file because committing a write replaces the hive itself.
FileLock lock_hive(const std::filesystem::path& path) {
#if defined(_WIN32)
    FileLock file(_wfsopen(path.c_str(), L"a+b", _SH_DENYNO), std::fclose);
    OVERLAPPED overlapped{};
    if (file && !LockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file.get()))),
                            LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped)) {
        file.reset();
    }
#else
    FileLock file(std::fopen(path.c_str(), "a+b"), std::fclose);
    if (file) {
        int result;
        do {
            result = flock(fileno(file.get()), LOCK_EX);
        } while (result == -1 && errno == EINTR);
        if (result == -1) file.reset();
    }
#endif
    return file;
}

bool read_hive(const std::filesystem::path& path, std::vector<RegistryEntry>& entries) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return !ec;
    std::ifstream in(path, std::ios::binary);
    in.imbue(std::locale::classic());
    std::string header;
    if (!std::getline(in, header) || header != "GMREG1") return false;

    while ((in >> std::ws).peek() != std::char_traits<char>::eof()) {
        RegistryEntry entry;
        char type;
        if (!(in >> std::quoted(entry.key) >> std::quoted(entry.name) >> type)) return false;
        if (type == 'S') {
            if (!(in >> std::quoted(entry.value.emplace<std::string>()))) return false;
        } else if (type == 'R') {
            u64 bits;
            if (!(in >> std::hex >> bits)) return false;
            entry.value = std::bit_cast<f64>(bits);
        } else {
            return false;
        }
        entries.push_back(std::move(entry));
    }
    return !in.bad();
}

bool write_hive(const std::filesystem::path& path, const std::vector<RegistryEntry>& entries) {
    auto tmp = path;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.imbue(std::locale::classic());
    out << "GMREG1\n";
    for (const auto& entry : entries) {
        out << std::quoted(entry.key) << ' ' << std::quoted(entry.name);
        if (const auto* text = std::get_if<std::string>(&entry.value)) {
            out << " S " << std::quoted(*text);
        } else {
            out << " R " << std::hex << std::bit_cast<u64>(std::get<f64>(entry.value));
        }
        out << '\n';
    }
    out.close();
    return out && SDL_RenamePath(Vfs::path_to_utf8(tmp).c_str(), Vfs::path_to_utf8(path).c_str());
}

} // namespace

void Vfs::registry_init(i32 game_id) {
    registry_game_key_ = "Software\\Game Maker\\" + std::to_string(game_id);
    registry_root_ = 0;
}

void Vfs::registry_set_root(i32 root_id) {
    registry_root_ = root_id >= 0 && root_id <= 3 ? root_id : 0;
}

std::optional<Vfs::RegistryValue> Vfs::access_registry_value(
    std::optional<std::string_view> key, std::string_view name, const RegistryValue* replacement) const {
    static constexpr const char* hives[] = {"HKCU", "HKLM", "HKCR", "HKU"};
    const auto& dir = registry_directory();
    if (dir.empty()) {
        GM_LOG_WARN("vfs", "Cannot locate virtual registry directory");
        return std::nullopt;
    }
    const auto path = dir / hives[key ? registry_root_ : 0];
    auto lock_path = path;
    lock_path += ".lock";
    const auto lock = lock_hive(lock_path);
    std::vector<RegistryEntry> entries;
    if (!lock || !read_hive(path, entries)) {
        GM_LOG_WARN("vfs", "Cannot read virtual registry hive '%s'", path_to_utf8(path).c_str());
        return std::nullopt;
    }

    std::string subkey = registry_game_key_;
    if (key) {
        const auto input = key->substr(0, key->find('\0'));
        subkey = codepage_to_utf8(input, game_path_codepage_);
        subkey.erase(0, subkey.find_first_not_of('\\'));
        if (!subkey.empty()) subkey.erase(subkey.find_last_not_of('\\') + 1);
    }
    const auto value_name = codepage_to_utf8(name.substr(0, name.find('\0')), game_path_codepage_);
    const auto found = std::ranges::find_if(entries, [&](const RegistryEntry& e) {
        return equal_registry_name(e.key, subkey) && equal_registry_name(e.name, value_name);
    });
    if (!replacement) return found == entries.end() ? std::nullopt : std::optional(found->value);

    if (found == entries.end()) entries.push_back({std::move(subkey), value_name, *replacement});
    else found->value = *replacement;
    if (!write_hive(path, entries)) {
        GM_LOG_WARN("vfs", "Cannot write virtual registry hive '%s'", path_to_utf8(path).c_str());
    }
    return std::nullopt;
}

void Vfs::registry_write_string(std::optional<std::string_view> key, std::string_view name, std::string_view value) {
    const RegistryValue data = codepage_to_utf8(value, game_path_codepage_);
    (void)access_registry_value(key, name, &data);
}

void Vfs::registry_write_real(std::optional<std::string_view> key, std::string_view name, f64 value) {
    const RegistryValue data = value;
    (void)access_registry_value(key, name, &data);
}

std::string Vfs::registry_read_string(std::optional<std::string_view> key, std::string_view name) const {
    const auto value = access_registry_value(key, name);
    if (value && std::holds_alternative<std::string>(*value)) {
        return utf8_to_codepage(std::get<std::string>(*value), game_path_codepage_);
    }
    return {};
}

f64 Vfs::registry_read_real(std::optional<std::string_view> key, std::string_view name) const {
    const auto value = access_registry_value(key, name);
    return value && std::holds_alternative<f64>(*value) ? std::get<f64>(*value) : 0.0;
}

bool Vfs::registry_exists(std::optional<std::string_view> key, std::string_view name) const {
    return access_registry_value(key, name).has_value();
}

} // namespace gm
