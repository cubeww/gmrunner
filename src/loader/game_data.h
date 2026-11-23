#pragma once

#include "base/types.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace gm {

struct GameMetadata {
    VersionMode detected_version = VersionMode::GM80;
    EncodingMode encoding = EncodingMode::LegacyAnsi;
    i32 format_version = 800; // 800 (GM 8.0) or 810 (GM 8.1 / 8.2)
    bool debug_mode = false;
    bool pro_edition = true;
    u32 edition_flags = 0;
    i32 game_id = 0;
    std::array<u8, 16> guid{};
    usize exe_size = 0;
    usize pe_overlay_offset = 0;
    usize payload_offset = 0;
    bool has_gm82_event_tables = false;
    u32 inferred_codepage = 1252;

    [[nodiscard]] u32 string_codepage() const noexcept {
        return encoding == EncodingMode::Utf8 ? 65001u : inferred_codepage;
    }
};

struct GameOptions {
    i32 version = 800;
    bool fullscreen = false;
    bool interpolate_pixels = false;
    bool no_border = false;
    bool show_cursor = true;
    i32 scale = 0;
    bool sizeable = false;
    bool stay_on_top = false;
    u32 window_color = 0;
    bool change_resolution = false;
    i32 color_depth = 0;
    i32 resolution = 0;
    i32 frequency = 0;
    bool no_buttons = false;
    i32 sync_vertex = 0;
    bool no_screensaver = true;
    bool screen_key = true;
    bool help_key = true;
    bool quit_key = true;
    bool save_key = true;
    bool screenshot_key = true;
    bool close_sec = true;
    i32 priority = 0;
    bool freeze = false;
    bool show_progress = false;
    std::vector<u8> progress_back_image;
    std::vector<u8> progress_front_image;
    std::vector<u8> load_image;
    bool load_transparent = false;
    i32 load_alpha = 255;
    bool scale_progress = true;
    bool display_errors = true;
    bool write_errors = false;
    bool abort_errors = false;
    bool treat_uninitialized_as_zero = false;
    bool error_on_arg_count = false;
    i32 webgl_mode = 0;
    bool creation_event_order = false;
};

struct ExtensionFunction {
    std::string name;
    std::string external_name;
    i32 call_type = 0; // 2=GML, 11=stdcall, 12=cdecl
    i32 id = 0;
    i32 arg_count = 0;
    std::vector<i32> arg_types; // 1=string, 2=real (in GEX)
    i32 return_type = 2;
};

struct ExtensionConstant {
    std::string name;
    std::string value;
};

struct ExtensionInclude {
    std::string filename;
    i32 kind = 0; // 1=DLL, 2=GML, 3=Lib, 4=Other
    std::string init_func;
    std::string final_func;
    std::vector<ExtensionFunction> functions;
    std::vector<ExtensionConstant> constants;
    std::vector<u8> payload; // Decrypted & decompressed file bytes (for kind != 3)
};

struct ExtensionData {
    std::string name;
    std::string folder;
    std::vector<ExtensionInclude> includes;
};

struct TriggerData {
    bool exists = false;
    std::string name;
    std::string condition;
    i32 moment = 0; // 0=Step, 1=Begin Step, 2=End Step
    std::string const_name;
};

struct ConstantData {
    std::string name;
    std::string value;
};

struct SoundData {
    bool exists = false;
    std::string name;
    i32 kind = 0; // 0=normal, 1=bg/midi, 2=3D, 3=multimedia
    std::string extension;
    std::string orig_name;
    std::vector<u8> data;
    i32 effects = 0;
    f64 volume = 1.0;
    f64 pan = 0.0;
    bool preload = true;
};

struct Bitmap32 {
    i32 width = 0;
    i32 height = 0;
    std::vector<u8> bgra; // width * height * 4 bytes (BGRA8, top-down)
};

struct CollisionMask {
    i32 width = 0;
    i32 height = 0;
    i32 bbox_left = 0;
    i32 bbox_right = 0;
    i32 bbox_bottom = 0;
    i32 bbox_top = 0;
    std::vector<u8> pixels; // width * height (row-major y * width + x, 0 or 1)
};

struct SpriteData {
    bool exists = false;
    std::string name;
    i32 origin_x = 0;
    i32 origin_y = 0;
    i32 width = 0;
    i32 height = 0;
    i32 bbox_shape = 0; // v810: 0=precise, 1=rect, 2=disk, 3=diamond
    bool separate_masks = false;
    i32 bbox_left = 0;
    i32 bbox_right = 0;
    i32 bbox_bottom = 0;
    i32 bbox_top = 0;
    std::vector<Bitmap32> images;
    std::vector<CollisionMask> masks;
};

struct BackgroundData {
    bool exists = false;
    std::string name;
    i32 width = 0;
    i32 height = 0;
    bool is_tileset = false;
    Bitmap32 bitmap;
};

struct PathPoint {
    f64 x = 0.0;
    f64 y = 0.0;
    f64 speed = 100.0;
};

struct PathGeometry {
    i32 kind = 0; // 0=straight, 1=smooth
    bool closed = false;
    i32 precision = 4;
    std::vector<PathPoint> points;
};

struct PathData {
    bool exists = false;
    std::string name;

    [[nodiscard]] const PathGeometry& geometry() const noexcept { return geometry_; }
    PathGeometry& edit_geometry() noexcept { samples_.clear(); return geometry_; }
    [[nodiscard]] f64 length() const;
    [[nodiscard]] PathPoint sample(f64 position) const;

private:
    struct Sample {
        PathPoint point;
        f64 distance;
    };
    PathGeometry geometry_;
    mutable std::vector<Sample> samples_;

    void build_samples() const;
    void add_sample(PathPoint point) const;
    void subdivide(const PathPoint& a, const PathPoint& b, const PathPoint& c, i32 depth) const;
};

struct ScriptData {
    bool exists = false;
    std::string name;
    std::string source;
};

struct GlyphData {
    i32 x = 0;
    i32 y = 0;
    i32 w = 0;
    i32 h = 0;
    i32 shift = 0;
    i32 offset = 0;
};

struct FontData {
    bool exists = false;
    std::string name;
    std::string font_name;
    i32 size = 12;
    bool bold = false;
    bool italic = false;
    i32 first = 32;
    i32 last = 127;
    u8 charset = 1;
    u8 antialias = 3;
    std::array<GlyphData, 256> glyphs{};
    i32 tex_width = 0;
    i32 tex_height = 0;
    std::vector<u8> alpha_map; // tex_width * tex_height (1-byte alpha per pixel)
};

struct ActionData {
    i32 lib_id = 0;
    i32 id = 0;
    i32 kind = 0; // eAction (0=normal, 1=begin, 2=end, 3=else, 4=exit, 5=repeat, 6=var, 7=code)
    bool use_relative = false;
    bool is_question = false;
    bool use_apply_to = false;
    i32 exe_type = 0; // 0=none, 1=func, 2=code
    std::string name;
    std::string code;
    i32 arg_count = 0;
    std::vector<i32> arg_types;
    i32 who = -1;
    bool relative = false;
    std::vector<std::string> args;
    bool is_not = false;
};

struct EventData {
    std::vector<ActionData> actions;
};

struct TimelineMoment {
    i32 step = 0;
    EventData event;
};

struct TimelineData {
    bool exists = false;
    std::string name;
    std::vector<TimelineMoment> moments;
};

struct ObjectEventEntry {
    i32 sub_id = 0;
    EventData event;
};

struct ObjectData {
    bool exists = false;
    std::string name;
    i32 sprite_index = -1;
    bool solid = false;
    bool visible = true;
    i32 depth = 0;
    bool persistent = false;
    i32 parent_index = -100;
    i32 mask_index = -1;
    std::array<std::vector<ObjectEventEntry>, 12> events;
};

struct RoomBackground {
    bool visible = false;
    bool foreground = false;
    i32 background_index = -1;
    i32 x = 0;
    i32 y = 0;
    bool htiled = true;
    bool vtiled = true;
    i32 hspeed = 0;
    i32 vspeed = 0;
    bool stretch = false;
};

struct RoomView {
    bool visible = false;
    i32 xview = 0;
    i32 yview = 0;
    i32 wview = 640;
    i32 hview = 480;
    i32 xport = 0;
    i32 yport = 0;
    i32 wport = 640;
    i32 hport = 480;
    i32 hborder = 32;
    i32 vborder = 32;
    i32 hspeed = -1;
    i32 vspeed = -1;
    i32 follow_object = -1;
};

struct RoomInstance {
    i32 x = 0;
    i32 y = 0;
    i32 object_index = -1;
    i32 id = 0;
    std::string creation_code;
    f64 scale_x = 1.0;
    f64 scale_y = 1.0;
    u32 color = 0xFFFFFFFFu;
    f64 rotation = 0.0;
};

struct RoomTile {
    i32 x = 0;
    i32 y = 0;
    i32 background_index = -1;
    i32 xo = 0;
    i32 yo = 0;
    i32 w = 0;
    i32 h = 0;
    i32 depth = 0;
    i32 id = 0;
    f64 scale_x = 1.0;
    f64 scale_y = 1.0;
    u32 blend = 0xFFFFFFu;
    f64 alpha = 1.0;
};

struct RoomData {
    bool exists = false;
    std::string name;
    std::string caption;
    i32 width = 640;
    i32 height = 480;
    i32 speed = 30;
    bool persistent = false;
    u32 color = 0xC0C0C0u;
    bool show_color = true;
    bool view_clear_screen = true;
    std::string creation_code;
    std::vector<RoomBackground> backgrounds;
    bool enable_views = false;
    std::vector<RoomView> views;
    std::vector<RoomInstance> instances;
    std::vector<RoomTile> tiles;
};

struct IncludedFileData {
    std::string filename;
    std::string orig_name;
    bool exists = false;
    i32 size = 0;
    bool store = false;
    std::vector<u8> data;
    i32 export_action = 0;
    std::string export_dir;
    bool overwrite = false;
    bool free_data = false;
    bool remove_end = false;
};

struct GameHelpData {
    u32 bg_color = 0;
    bool mimic = false;
    std::string caption;
    i32 left = -1;
    i32 top = -1;
    i32 width = 600;
    i32 height = 400;
    bool border = true;
    bool sizable = true;
    bool on_top = false;
    bool modal = true;
    std::string rtf_text;
};

struct Gm82EventTables {
    std::array<std::vector<std::vector<i32>>, 12> event_objects;
    std::vector<std::pair<i32, i32>> collision_pairs;
};

struct GameData {
    GameMetadata meta;
    GameOptions options;
    std::array<Bitmap32, 14> particle_shapes;
    std::vector<u8> message_background_image; // Encoded JPEG/BMP from TMessageForm.BackgroundImage
    std::array<Bitmap32, 3> message_buttons;  // 3-state button strip from TMessageForm.TheImages

    std::vector<ExtensionData> extensions;
    std::vector<TriggerData> triggers;
    std::vector<ConstantData> constants;
    std::vector<SoundData> sounds;
    std::vector<SpriteData> sprites;
    std::vector<BackgroundData> backgrounds;
    std::vector<PathData> paths;
    std::vector<ScriptData> scripts;
    std::vector<FontData> fonts;
    std::vector<TimelineData> timelines;
    std::vector<ObjectData> objects;
    std::vector<RoomData> rooms;
    i32 room_max_id = 100000;
    i32 room_max_tile_id = 10000000;
    std::vector<IncludedFileData> included_files;
    GameHelpData help;
    std::vector<std::string> libraries;
    std::vector<i32> room_order;
    Gm82EventTables gm82_event_tables;
};

} // namespace gm
