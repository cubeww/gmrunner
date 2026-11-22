#include "platform/web_bridge.h"

#if defined(__EMSCRIPTEN__)

#include "base/encoding.h"
#include "base/log.h"
#include "platform/zip_archive.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace gm {

namespace {

// ---------------------------------------------------------------------------
// Synchronous JS metadata queries for Module.gmWebFs (dropped/picked folder)
// ---------------------------------------------------------------------------

EM_JS(int, js_webfs_is_ready, (), {
    return (Module.gmWebFs && Module.gmWebFs.files) ? 1 : 0;
});

EM_JS(char*, js_webfs_display_name, (), {
    const name = (Module.gmWebFs && Module.gmWebFs.displayName) ? String(Module.gmWebFs.displayName) : 'web-folder';
    return stringToNewUTF8(name);
});

EM_JS(char*, js_webfs_default_exe, (), {
    const exe = (Module.gmWebFs && Module.gmWebFs.defaultExe) ? String(Module.gmWebFs.defaultExe) : '';
    return stringToNewUTF8(exe);
});

EM_JS(int, js_webfs_exists, (const char* path_ptr), {
    if (!Module.gmWebFs) return 0;
    const key = UTF8ToString(path_ptr);
    if (Module.gmWebFs.files && Module.gmWebFs.files.has(key)) return 1;
    if (Module.gmWebFs.dirs && Module.gmWebFs.dirs.has(key)) return 1;
    return 0;
});

EM_JS(int, js_webfs_dir_exists, (const char* path_ptr), {
    if (!Module.gmWebFs || !Module.gmWebFs.dirs) return 0;
    return Module.gmWebFs.dirs.has(UTF8ToString(path_ptr)) ? 1 : 0;
});

EM_JS(double, js_webfs_file_size, (const char* path_ptr), {
    if (!Module.gmWebFs || !Module.gmWebFs.files) return -1;
    const f = Module.gmWebFs.files.get(UTF8ToString(path_ptr));
    return f ? f.size : -1;
});

EM_JS(double, js_webfs_mtime_unix, (const char* path_ptr), {
    if (!Module.gmWebFs || !Module.gmWebFs.files) return -1;
    const f = Module.gmWebFs.files.get(UTF8ToString(path_ptr));
    return f ? Math.floor((f.lastModified || 0) / 1000) : -1;
});

// Returns a newline-delimited list: "D<name>\nF<name>\n..."
EM_JS(char*, js_webfs_list_dir, (const char* dir_ptr), {
    if (!Module.gmWebFs || !Module.gmWebFs.children) return stringToNewUTF8('');
    const list = Module.gmWebFs.children.get(UTF8ToString(dir_ptr));
    if (!list || list.length === 0) return stringToNewUTF8('');
    let out = '';
    for (let i = 0; i < list.length; ++i) {
        const item = list[i];
        out += (item.isDir ? 'D' : 'F') + item.name + '\n';
    }
    return stringToNewUTF8(out);
});

// Async on-demand single-file read via Asyncify / JSPI (zero upfront RAM copy).
EM_ASYNC_JS(int, js_webfs_read_file, (const char* path_ptr, uint8_t* dst, int max_len), {
    try {
        if (!Module.gmWebFs || !Module.gmWebFs.files) return -1;
        const f = Module.gmWebFs.files.get(UTF8ToString(path_ptr));
        if (!f) return -1;
        const buf = await f.arrayBuffer();
        const bytes = new Uint8Array(buf);
        const n = Math.min(bytes.length, max_len);
        HEAPU8.set(bytes.subarray(0, n), dst);
        return n;
    } catch (err) {
        console.error('[gmrunner] js_webfs_read_file error:', err);
        return -1;
    }
});

// ---------------------------------------------------------------------------
// Streaming File.slice() bridge for Module.gmWebZip (dropped/picked/URL .zip)
// ---------------------------------------------------------------------------

EM_JS(double, js_webzip_size, (), {
    return (Module.gmWebZip && Module.gmWebZip.file) ? Module.gmWebZip.file.size : -1;
});

EM_JS(char*, js_webzip_name, (), {
    const name = (Module.gmWebZip && Module.gmWebZip.file && Module.gmWebZip.file.name)
        ? String(Module.gmWebZip.file.name) : 'game.zip';
    return stringToNewUTF8(name);
});

EM_ASYNC_JS(int, js_webzip_read_range, (double offset, uint8_t* dst, int len), {
    try {
        if (!Module.gmWebZip || !Module.gmWebZip.file) return 0;
        const blob = Module.gmWebZip.file.slice(offset, offset + len);
        const buf = await blob.arrayBuffer();
        const bytes = new Uint8Array(buf);
        if (bytes.length < len) return 0;
        HEAPU8.set(bytes.subarray(0, len), dst);
        return 1;
    } catch (err) {
        console.error('[gmrunner] js_webzip_read_range error:', err);
        return 0;
    }
});

// ---------------------------------------------------------------------------
// Persistent storage (OPFS primary + IDBFS fallback)
// ---------------------------------------------------------------------------

EM_ASYNC_JS(int, js_web_mount_persist, (const char* game_key_ptr), {
    const gameKey = UTF8ToString(game_key_ptr);
    if (typeof Module.gmMountPersist === 'function') {
        try {
            return (await Module.gmMountPersist(gameKey)) ? 1 : 0;
        } catch (err) {
            console.warn('[gmrunner] gmMountPersist failed:', err);
            return 0;
        }
    }
    try {
        const root = '/persist';
        try { FS.mkdir(root); } catch (_) {}
        try { FS.mount(IDBFS, {}, root); } catch (_) {}
        await new Promise((resolve, reject) => {
            FS.syncfs(true, (err) => err ? reject(err) : resolve());
        });
        const sub = root + '/' + gameKey;
        try { FS.mkdir(sub); } catch (_) {}
        return 1;
    } catch (err) {
        console.warn('[gmrunner] fallback IDBFS mount failed:', err);
        return 0;
    }
});

EM_JS(void, js_web_request_persist_sync, (), {
    if (typeof Module.gmSchedulePersistSync === 'function') {
        Module.gmSchedulePersistSync();
    } else if (typeof FS !== 'undefined' && typeof FS.syncfs === 'function') {
        FS.syncfs(false, () => {});
    }
});

EM_ASYNC_JS(void, js_web_flush_persist_sync, (), {
    if (typeof Module.gmFlushPersistSync === 'function') {
        try { await Module.gmFlushPersistSync(); } catch (_) {}
    } else if (typeof FS !== 'undefined' && typeof FS.syncfs === 'function') {
        await new Promise((resolve) => FS.syncfs(false, () => resolve()));
    }
});

// ---------------------------------------------------------------------------
// Dialogs, WebAudio unlock, and status HUD
// ---------------------------------------------------------------------------

EM_JS(void, js_web_show_message, (const char* title_ptr, const char* msg_ptr), {
    const title = UTF8ToString(title_ptr);
    const msg = UTF8ToString(msg_ptr);
    window.alert(title ? (title + '\n\n' + msg) : msg);
});

EM_JS(int, js_web_show_question, (const char* title_ptr, const char* msg_ptr), {
    const title = UTF8ToString(title_ptr);
    const msg = UTF8ToString(msg_ptr);
    return window.confirm(title ? (title + '\n\n' + msg) : msg) ? 1 : 0;
});

EM_JS(char*, js_web_prompt_string, (const char* prompt_ptr, const char* def_ptr), {
    const p = UTF8ToString(prompt_ptr);
    const d = UTF8ToString(def_ptr);
    const res = window.prompt(p, d);
    return stringToNewUTF8(res === null ? d : String(res));
});

EM_JS(void, js_web_unlock_audio, (), {
    if (typeof Module.gmUnlockAudio === 'function') {
        Module.gmUnlockAudio();
    }
});

EM_JS(void, js_web_report_status, (const char* status_ptr), {
    if (typeof Module.gmSetStatus === 'function') {
        Module.gmSetStatus(UTF8ToString(status_ptr));
    }
});

std::string take_js_string(char* ptr) {
    if (!ptr) return {};
    std::string out(ptr);
    std::free(ptr);
    return out;
}

std::string normalize_web_rel(std::string_view rel_path) {
    std::string s(rel_path);
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return ascii_to_lower(s);
}

class WebLazyFolderSource final : public IVfsReadSource {
public:
    WebLazyFolderSource()
        : display_name_(take_js_string(js_webfs_display_name())),
          default_exe_(take_js_string(js_webfs_default_exe())) {}

    bool exists(std::string_view rel_path) const override {
        const std::string key = normalize_web_rel(rel_path);
        return js_webfs_exists(key.c_str()) != 0;
    }

    bool directory_exists(std::string_view rel_path) const override {
        const std::string key = normalize_web_rel(rel_path);
        return js_webfs_dir_exists(key.c_str()) != 0;
    }

    Result<u64> file_size(std::string_view rel_path) const override {
        const std::string key = normalize_web_rel(rel_path);
        const double sz = js_webfs_file_size(key.c_str());
        if (sz < 0.0) {
            return Error{"Web folder file not found: " + std::string(rel_path)};
        }
        return static_cast<u64>(sz);
    }

    Result<std::vector<u8>> read_all_bytes(std::string_view rel_path) override {
        const std::string key = normalize_web_rel(rel_path);
        const double sz = js_webfs_file_size(key.c_str());
        if (sz < 0.0) {
            return Error{"Web folder file not found: " + std::string(rel_path)};
        }
        const usize byte_len = static_cast<usize>(sz);
        std::vector<u8> out(byte_len);
        if (byte_len > 0) {
            const int got = js_webfs_read_file(key.c_str(), out.data(), static_cast<int>(byte_len));
            if (got < 0) {
                return Error{"Failed to read browser file: " + std::string(rel_path)};
            }
            out.resize(static_cast<usize>(got));
        }
        return out;
    }

    std::vector<VfsDirEntry> list_directory(std::string_view rel_dir) const override {
        const std::string key = normalize_web_rel(rel_dir);
        const std::string listing = take_js_string(js_webfs_list_dir(key.c_str()));
        std::vector<VfsDirEntry> out;
        usize pos = 0;
        while (pos < listing.size()) {
            usize nl = listing.find('\n', pos);
            if (nl == std::string::npos) nl = listing.size();
            if (nl > pos + 1) {
                const bool is_dir = (listing[pos] == 'D');
                out.push_back(VfsDirEntry{listing.substr(pos + 1, nl - pos - 1), is_dir});
            }
            pos = nl + 1;
        }
        return out;
    }

    std::optional<i64> last_write_time_unix(std::string_view rel_path) const override {
        const std::string key = normalize_web_rel(rel_path);
        const double ts = js_webfs_mtime_unix(key.c_str());
        if (ts < 0.0) return std::nullopt;
        return static_cast<i64>(ts);
    }

    std::string default_exe_path() const override { return default_exe_; }
    std::string display_name() const override { return display_name_; }

private:
    std::string display_name_;
    std::string default_exe_;
};

} // namespace

Result<std::unique_ptr<IVfsReadSource>> create_web_lazy_folder_source() {
    if (js_webfs_is_ready() == 0) {
        return Error{"Browser folder handle map is not initialized"};
    }
    auto src = std::make_unique<WebLazyFolderSource>();
    GM_LOG_INFO("vfs", "Mounted browser lazy folder '%s' (default_exe='%s')",
                src->display_name().c_str(), src->default_exe_path().c_str());
    return std::unique_ptr<IVfsReadSource>(std::move(src));
}

Result<std::unique_ptr<IVfsReadSource>> create_web_lazy_zip_source() {
    const double sz = js_webzip_size();
    if (sz < 22.0) {
        return Error{"Browser ZIP file is not initialized or too small"};
    }
    std::string name = take_js_string(js_webzip_name());
    auto reader = [](u64 offset, u8* dst, usize len) -> bool {
        if (len == 0) return true;
        return js_webzip_read_range(static_cast<double>(offset), dst, static_cast<int>(len)) != 0;
    };
    auto zip_res = ZipVfsSource::open_reader(static_cast<u64>(sz), std::move(reader), std::move(name));
    if (!zip_res) return zip_res.error();
    return std::unique_ptr<IVfsReadSource>(std::move(zip_res.value()));
}

Result<std::string> web_mount_persistent_storage(const std::string& game_key) {
    if (js_web_mount_persist(game_key.c_str()) == 0) {
        return Error{"Failed to mount browser persistent storage"};
    }
    return "/persist/" + game_key;
}

void web_request_persist_sync() {
    js_web_request_persist_sync();
}

void web_flush_persist_sync() {
    js_web_flush_persist_sync();
}

void web_show_message(std::string_view title, std::string_view message) {
    std::string t(title);
    std::string m(message);
    js_web_show_message(t.c_str(), m.c_str());
}

bool web_show_question(std::string_view title, std::string_view message) {
    std::string t(title);
    std::string m(message);
    return js_web_show_question(t.c_str(), m.c_str()) != 0;
}

std::string web_prompt_string(std::string_view prompt, std::string_view default_val) {
    std::string p(prompt);
    std::string d(default_val);
    return take_js_string(js_web_prompt_string(p.c_str(), d.c_str()));
}

void web_unlock_audio() {
    js_web_unlock_audio();
}

void web_report_status(std::string_view status_text) {
    std::string s(status_text);
    js_web_report_status(s.c_str());
}

} // namespace gm

#endif // __EMSCRIPTEN__
