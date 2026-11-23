#include "platform/web_bridge.h"

#if defined(__EMSCRIPTEN__)

#include "base/encoding.h"
#include "base/log.h"
#include "platform/zip_archive.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdio>
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

// A real hit target lets the browser focus its editor during the user's tap.
EM_JS(void, js_begin_dialog_editor, (const char* value, const char* label), {
    const el = document.createElement('input');
    el.type = 'text';
    el.enterKeyHint = 'done';
    el.autocomplete = 'off';
    el.setAttribute('aria-label', UTF8ToString(label));
    el.value = UTF8ToString(value);
    el.style.cssText = 'position:fixed;z-index:2147483647;opacity:0;margin:0;padding:0;border:0;box-sizing:border-box;font-size:16px';
    const state = { el, dirty: false, action: 0, composing: false };
    Module.gmDialogEditor = state;
    const dirty = () => { state.dirty = true; };
    for (const type of ['input', 'select', 'click', 'keyup', 'selectionchange']) {
        el.addEventListener(type, dirty);
    }
    el.addEventListener('compositionstart', () => { state.composing = true; });
    el.addEventListener('compositionend', () => { state.composing = false; dirty(); });
    for (const type of ['keydown', 'keyup', 'keypress']) {
        el.addEventListener(type, e => {
            e.stopPropagation();
            if (type !== 'keydown' || e.isComposing || state.composing || e.keyCode === 229) return;
            if (e.key === 'Enter' || e.key === 'Escape') {
                e.preventDefault();
                state.action = e.key === 'Enter' ? 13 : 27;
                dirty();
            }
        });
    }
    Module.canvas.parentElement.appendChild(el);
    el.select();
});

EM_JS(void, js_position_dialog_editor, (int x, int y, int w, int h, int win_w, int win_h), {
    const canvas = Module.canvas;
    const rect = canvas.getBoundingClientRect();
    let width = rect.width, height = rect.height;
    if (getComputedStyle(canvas).objectFit === 'contain' && canvas.width && canvas.height) {
        const scale = Math.min(width / canvas.width, height / canvas.height);
        width = canvas.width * scale;
        height = canvas.height * scale;
    }
    const style = Module.gmDialogEditor.el.style;
    style.left = (rect.left + (rect.width - width) / 2 + x * width / win_w) + 'px';
    style.top = (rect.top + (rect.height - height) / 2 + y * height / win_h) + 'px';
    style.width = (w * width / win_w) + 'px';
    style.height = (h * height / win_h) + 'px';
});

EM_JS(char*, js_poll_dialog_editor, (int* meta), {
    const state = Module.gmDialogEditor;
    if (!state.dirty) return 0;
    state.dirty = false;
    const el = state.el;
    const start = Array.from(el.value.slice(0, el.selectionStart)).length;
    const end = Array.from(el.value.slice(0, el.selectionEnd)).length;
    const backward = el.selectionDirection === 'backward';
    HEAP32[meta >> 2] = backward ? end : start;
    HEAP32[(meta >> 2) + 1] = backward ? start : end;
    HEAP32[(meta >> 2) + 2] = state.action;
    state.action = 0;
    return stringToNewUTF8(el.value);
});

EM_JS(void, js_end_dialog_editor, (), {
    const el = Module.gmDialogEditor.el;
    const focused = document.activeElement === el;
    el.remove();
    delete Module.gmDialogEditor;
    if (focused) Module.canvas.focus({ preventScroll: true });
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

EM_JS(int, js_web_font_metrics, (const char* css_font_ptr, float em_px, int* out_vals), {
    try {
        if (!Module._gmFontCanvas) {
            Module._gmFontCanvas = document.createElement('canvas');
            Module._gmFontCanvas.width = 128;
            Module._gmFontCanvas.height = 128;
            Module._gmFontCtx = Module._gmFontCanvas.getContext('2d', { willReadFrequently: true });
        }
        const ctx = Module._gmFontCtx;
        if (!ctx) return 0;
        ctx.font = UTF8ToString(css_font_ptr);
        const m = ctx.measureText('Agjy|');
        const asc = Math.ceil(m.fontBoundingBoxAscent || m.actualBoundingBoxAscent || (em_px * 0.85));
        const desc = Math.ceil(m.fontBoundingBoxDescent || m.actualBoundingBoxDescent || (em_px * 0.25));
        const lh = Math.max(asc + desc, Math.ceil(em_px * 1.18));
        HEAP32[out_vals >> 2] = Math.max(1, asc);
        HEAP32[(out_vals >> 2) + 1] = Math.max(0, desc);
        HEAP32[(out_vals >> 2) + 2] = Math.max(1, lh);
        return 1;
    } catch (_) {
        return 0;
    }
});

EM_JS(uint8_t*, js_web_rasterize_glyph, (const char* css_font_ptr, float em_px, uint32_t codepoint, int* out_meta, float* out_adv), {
    try {
        if (!Module._gmFontCanvas) {
            Module._gmFontCanvas = document.createElement('canvas');
            Module._gmFontCanvas.width = 128;
            Module._gmFontCanvas.height = 128;
            Module._gmFontCtx = Module._gmFontCanvas.getContext('2d', { willReadFrequently: true });
        }
        const canvas = Module._gmFontCanvas;
        const ctx = Module._gmFontCtx;
        if (!ctx) return 0;

        const fontStr = UTF8ToString(css_font_ptr);
        const ch = String.fromCodePoint(codepoint);
        ctx.font = fontStr;
        const m = ctx.measureText(ch);
        const adv = m.width || 0;
        HEAPF32[out_adv >> 2] = adv;

        const pad = Math.max(8, Math.ceil(em_px * 0.6));
        const cw = Math.max(32, Math.min(256, Math.ceil(adv + pad * 2)));
        const chh = Math.max(32, Math.min(256, Math.ceil(em_px * 2.4 + pad * 2)));
        if (canvas.width < cw || canvas.height < chh) {
            canvas.width = Math.max(canvas.width, cw);
            canvas.height = Math.max(canvas.height, chh);
        }

        const ox = pad;
        const oy = Math.ceil(em_px * 1.4) + pad;
        ctx.clearRect(0, 0, cw, chh);
        ctx.font = fontStr;
        ctx.textBaseline = 'alphabetic';
        ctx.textAlign = 'left';
        ctx.fillStyle = '#ffffff';
        ctx.fillText(ch, ox, oy);

        const img = ctx.getImageData(0, 0, cw, chh).data;
        let minX = cw, minY = chh, maxX = -1, maxY = -1;
        for (let y = 0; y < chh; ++y) {
            const row = y * cw * 4;
            for (let x = 0; x < cw; ++x) {
                if (img[row + x * 4 + 3] > 0) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }

        if (maxX < minX || maxY < minY) {
            HEAP32[out_meta >> 2] = 0;
            HEAP32[(out_meta >> 2) + 1] = 0;
            HEAP32[(out_meta >> 2) + 2] = 0;
            HEAP32[(out_meta >> 2) + 3] = 0;
            return 0;
        }

        const gw = maxX - minX + 1;
        const gh = maxY - minY + 1;
        const ptr = _malloc(gw * gh);
        if (!ptr) return 0;
        let di = 0;
        for (let y = minY; y <= maxY; ++y) {
            const row = y * cw * 4;
            for (let x = minX; x <= maxX; ++x) {
                HEAPU8[ptr + di] = img[row + x * 4 + 3];
                ++di;
            }
        }
        HEAP32[out_meta >> 2] = gw;
        HEAP32[(out_meta >> 2) + 1] = gh;
        HEAP32[(out_meta >> 2) + 2] = minX - ox;
        HEAP32[(out_meta >> 2) + 3] = minY - oy;
        return ptr;
    } catch (_) {
        return 0;
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

void web_begin_dialog_editor(const std::string& value, const std::string& label) {
    js_begin_dialog_editor(value.c_str(), label.c_str());
}

void web_position_dialog_editor(int x, int y, int w, int h, int win_w, int win_h) {
    js_position_dialog_editor(x, y, w, h, win_w, win_h);
}

bool web_poll_dialog_editor(std::string& value, int& anchor, int& caret, int& action) {
    int meta[3]{};
    char* text = js_poll_dialog_editor(meta);
    if (!text) return false;
    value = take_js_string(text);
    anchor = meta[0];
    caret = meta[1];
    action = meta[2];
    return true;
}

void web_end_dialog_editor() {
    js_end_dialog_editor();
}

void web_unlock_audio() {
    js_web_unlock_audio();
}

void web_report_status(std::string_view status_text) {
    std::string s(status_text);
    js_web_report_status(s.c_str());
}

namespace {

std::string build_web_css_font(std::string_view family_name, f32 em_px, bool bold, bool italic) {
    std::string css;
    if (italic) css += "italic ";
    if (bold) css += "bold ";
    char sz_buf[32];
    std::snprintf(sz_buf, sizeof(sz_buf), "%.2fpx ", static_cast<double>(std::max(1.0f, em_px)));
    css += sz_buf;

    const std::string lower = ascii_to_lower(family_name);
    if (lower.empty() || lower == "ms sans serif" || lower == "microsoft sans serif") {
        css += "Tahoma, \"Microsoft Sans Serif\", Arial, sans-serif";
    } else {
        std::string safe;
        safe.reserve(family_name.size());
        for (char c : family_name) {
            if (c != '"' && c != '\'' && c != ';' && c != '\r' && c != '\n') safe.push_back(c);
        }
        if (!safe.empty()) {
            css += "\"";
            css += safe;
            css += "\", ";
        }
        css += "Tahoma, Arial, sans-serif";
    }
    return css;
}

} // namespace

bool web_measure_font_metrics(
    std::string_view family_name,
    f32 em_px,
    bool bold,
    bool italic,
    i32& out_ascent,
    i32& out_descent,
    i32& out_line_height) {
    const std::string css = build_web_css_font(family_name, em_px, bold, italic);
    int vals[3]{};
    if (js_web_font_metrics(css.c_str(), em_px, vals) == 0) {
        return false;
    }
    out_ascent = vals[0];
    out_descent = vals[1];
    out_line_height = vals[2];
    return true;
}

bool web_rasterize_glyph(
    std::string_view family_name,
    f32 em_px,
    bool bold,
    bool italic,
    u32 codepoint,
    WebGlyphBitmap& out) {
    const std::string css = build_web_css_font(family_name, em_px, bold, italic);
    int meta[4]{};
    float adv = 0.0f;
    uint8_t* pixels = js_web_rasterize_glyph(css.c_str(), em_px, codepoint, meta, &adv);
    out.width = meta[0];
    out.height = meta[1];
    out.xoff = meta[2];
    out.yoff = meta[3];
    out.advance = adv;
    out.alpha.clear();
    if (pixels && out.width > 0 && out.height > 0) {
        const usize count = static_cast<usize>(out.width) * static_cast<usize>(out.height);
        out.alpha.assign(pixels, pixels + count);
        std::free(pixels);
    }
    return true;
}

} // namespace gm

#endif // __EMSCRIPTEN__
