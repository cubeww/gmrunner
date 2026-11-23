#pragma once

#include "base/types.h"
#include "platform/vfs_source.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

#if defined(__EMSCRIPTEN__)

// Creates a lazy, zero-preload VFS source backed by the browser's selected/dropped
// folder handle map (Module.gmWebFs). Only individual requested files are read into WASM memory.
[[nodiscard]] Result<std::unique_ptr<IVfsReadSource>> create_web_lazy_folder_source();

// Creates a streaming ZIP VFS source backed by File.slice() on the browser's selected/dropped
// ZIP archive (Module.gmWebZip). Only the Central Directory is indexed on mount.
[[nodiscard]] Result<std::unique_ptr<IVfsReadSource>> create_web_lazy_zip_source();

// Mounts persistent save storage at /persist/<game_key> using OPFS when available,
// falling back automatically to Emscripten IDBFS (IndexedDB).
[[nodiscard]] Result<std::string> web_mount_persistent_storage(const std::string& game_key);

// Schedules a debounced asynchronous flush of /persist back to OPFS / IndexedDB.
void web_request_persist_sync();

// Immediately flushes /persist back to OPFS / IndexedDB (blocking via Asyncify/JSPI).
void web_flush_persist_sync();

// Browser dialog bridges (used in place of native OS dialogs on WebAssembly).
void web_show_message(std::string_view title, std::string_view message);
[[nodiscard]] bool web_show_question(std::string_view title, std::string_view message);
[[nodiscard]] std::string web_prompt_string(std::string_view prompt, std::string_view default_val);

void web_begin_dialog_editor(const std::string& value, const std::string& label);
void web_position_dialog_editor(int x, int y, int w, int h, int win_w, int win_h);
bool web_poll_dialog_editor(std::string& value, int& anchor, int& caret, int& action);
void web_end_dialog_editor();

// Attempts to resume any suspended WebAudio AudioContexts after a user gesture.
void web_unlock_audio();

// Reports runtime status/FPS back to the web shell HUD.
void web_report_status(std::string_view status_text);

// Browser 2D canvas glyph rasterizer used by the self-drawn message dialog on WebAssembly.
struct WebGlyphBitmap {
    i32 width = 0;
    i32 height = 0;
    i32 xoff = 0;
    i32 yoff = 0;
    f32 advance = 0.0f;
    std::vector<u8> alpha;
};

[[nodiscard]] bool web_measure_font_metrics(
    std::string_view family_name,
    f32 em_px,
    bool bold,
    bool italic,
    i32& out_ascent,
    i32& out_descent,
    i32& out_line_height);

[[nodiscard]] bool web_rasterize_glyph(
    std::string_view family_name,
    f32 em_px,
    bool bold,
    bool italic,
    u32 codepoint,
    WebGlyphBitmap& out);

#endif

} // namespace gm
