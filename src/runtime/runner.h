#pragma once

#include "audio/audio_engine.h"
#include "base/types.h"
#include "gml/compiler.h"
#include "gml/vm.h"
#include "graphics/gl_graphics_device.h"
#include "graphics/renderer2d.h"
#include "loader/game_data.h"
#include "platform/vfs.h"
#include "platform/window.h"
#include "runtime/collision.h"
#include "runtime/data_structures.h"
#include "runtime/instance.h"
#include "runtime/particles.h"

#include <array>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gm {

struct TextFileHandle {
    bool open = false;
    i32 mode = 0; // 0=read, 1=write, 2=append
    std::string gm_path;
    std::vector<std::string> lines;
    usize line_idx = 0;
    usize col_idx = 0;
    std::string write_buf;
};

struct BinFileHandle {
    bool open = false;
    i32 mode = 0; // 0=read, 1=write, 2=rw
    std::string gm_path;
    std::vector<u8> data;
    usize pos = 0;
    bool dirty = false;
};

struct IniState {
    bool open = false;
    std::string gm_path;
    bool dirty = false;
    // section_lower -> {orig_section, vector<{key_lower, orig_key, value}>}
    struct Entry {
        std::string key_lower;
        std::string key_orig;
        std::string value;
    };
    struct Section {
        std::string name_orig;
        std::vector<Entry> entries;
    };
    std::vector<std::pair<std::string, Section>> sections;
};

struct FileFindState {
    std::vector<std::string> results;
    usize index = 0;
    bool active = false;
};

struct HighscoreEntry {
    std::string name = "<nobody>";
    f64 score = 0.0;
};

struct EventFrame {
    i32 cat = 0;
    i32 sub = 0;
    i32 owner_obj = -1;
    i32 self_id = -1;
    i32 other_id = -1;
    i32 action_id = 0;
};

enum class PendingRoomAction : u8 {
    None = 0,
    Goto,
    RestartRoom,
    RestartGame,
    EndGame,
};

class Runner : public IVmHost, public NonCopyable {
public:
    using BuiltinFn = RValue (*)(Runner&, i32 self_id, i32 other_id, std::span<const RValue> args);

    Runner(Vfs& vfs, PlatformWindow& window, GlGraphicsDevice& gfx);
    ~Runner() override;

    [[nodiscard]] Result<void> init(
        GameData game,
        CompiledProgram program,
        std::vector<std::string> cmd_args = {});
    void shutdown() noexcept;

    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] i32 room_speed() const noexcept { return room_speed_ > 0 ? room_speed_ : 30; }
    [[nodiscard]] u64 target_frame_us() const noexcept {
        return static_cast<u64>(1000000.0 / static_cast<f64>(room_speed()));
    }

    // Executes one full GM8 frame (events, physics, collisions, render, swap).
    void tick_frame();

    // IVmHost implementation
    [[nodiscard]] bool treat_uninitialized_as_zero() const noexcept override {
        return game_.options.treat_uninitialized_as_zero;
    }
    [[nodiscard]] Result<RValue> get_builtin_global(u32 var_id, i32 idx, bool has_idx) override;
    [[nodiscard]] Result<void> set_builtin_global(u32 var_id, i32 idx, bool has_idx, RValue value) override;
    [[nodiscard]] Result<RValue> get_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx) override;
    [[nodiscard]] Result<void> set_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx, RValue value) override;
    [[nodiscard]] VariableTable* get_instance_vars(i32 inst_id) override;
    void resolve_target_instances(i32 target_id, i32 self_id, i32 other_id, std::vector<i32>& out) override;
    [[nodiscard]] Result<RValue> call_builtin(u32 func_id, i32 self_id, i32 other_id, std::span<const RValue> args) override;
    [[nodiscard]] Result<RValue> call_extension_dll(u32 ext_dll_id, std::span<const RValue> args) override;

    // Subsystems accessors for built-in implementation units
    [[nodiscard]] GameData& game() noexcept { return game_; }
    [[nodiscard]] const GameData& game() const noexcept { return game_; }
    [[nodiscard]] CompiledProgram& program() noexcept { return program_; }
    [[nodiscard]] GmlVm& vm() noexcept { return *vm_; }
    [[nodiscard]] Vfs& vfs() noexcept { return vfs_; }
    [[nodiscard]] PlatformWindow& window() noexcept { return window_; }
    [[nodiscard]] GlGraphicsDevice& gfx() noexcept { return gfx_; }
    [[nodiscard]] Renderer2D& renderer() noexcept { return renderer_; }
    [[nodiscard]] AudioEngine& audio() noexcept { return audio_; }
    [[nodiscard]] DsManager& ds() noexcept { return ds_; }
    [[nodiscard]] ParticleManager& particles() noexcept { return particles_; }
    [[nodiscard]] MpGridManager& mp_grids() noexcept { return mp_grids_; }
    [[nodiscard]] VersionMode version_mode() const noexcept { return game_.meta.detected_version; }

    // Instance & object hierarchy helpers
    [[nodiscard]] Instance* find_instance(i32 id) noexcept;
    [[nodiscard]] const Instance* find_instance(i32 id) const noexcept;
    [[nodiscard]] const std::vector<std::unique_ptr<Instance>>& instances() const noexcept { return instances_; }
    [[nodiscard]] std::vector<std::unique_ptr<Instance>>& instances_mut() noexcept { return instances_; }

    [[nodiscard]] bool object_exists(i32 obj_idx) const noexcept;
    [[nodiscard]] bool object_is_ancestor(i32 child_obj, i32 parent_obj) const noexcept;
    [[nodiscard]] bool object_matches(i32 inst_obj, i32 target) const noexcept;

    i32 create_instance(f64 x, f64 y, i32 obj_idx, bool run_create = true, i32 forced_id = -1);
    void destroy_instance(i32 inst_id, bool perform_event = true);
    void change_instance(i32 inst_id, i32 new_obj_idx, bool perform_events);
    i32 copy_instance(i32 src_id, bool perform_event);
    void remove_marked_instances();

    void deactivate_all(bool notme, i32 self_id);
    void deactivate_object(i32 target, i32 self_id);
    void deactivate_region(f64 left, f64 top, f64 w, f64 h, bool inside, bool notme, i32 self_id);
    void activate_all();
    void activate_object(i32 target);
    void activate_region(f64 left, f64 top, f64 w, f64 h, bool inside);

    // Event dispatch helpers
    [[nodiscard]] const BytecodeChunk* resolve_event_chunk(i32 obj_idx, i32 cat, i32 sub, i32& out_owner_obj) const noexcept;
    [[nodiscard]] bool object_has_event(i32 obj_idx, i32 cat, i32 sub) const noexcept;
    void perform_event_on_instance(i32 inst_id, i32 obj_override, i32 cat, i32 sub, i32 other_id = -1);
    void perform_event_inherited(i32 self_id);

    // Collision & movement helpers
    [[nodiscard]] i32 find_collision_instance(
        const Instance& self, f64 test_x, f64 test_y, i32 target, bool solid_only) const noexcept;
    [[nodiscard]] bool check_place_free(const Instance& self, f64 test_x, f64 test_y) const noexcept;
    [[nodiscard]] bool check_place_empty(const Instance& self, f64 test_x, f64 test_y) const noexcept;
    [[nodiscard]] bool check_place_meeting(const Instance& self, f64 test_x, f64 test_y, i32 target) const noexcept;
    void move_contact(Instance& inst, f64 dir_deg, f64 max_dist, bool solid_only);
    void move_outside(Instance& inst, f64 dir_deg, f64 max_dist, bool solid_only);
    void move_bounce(Instance& inst, bool advanced, bool solid_only);
    void start_path(Instance& inst, i32 path_id, f64 speed, i32 endaction, bool absolute);
    void end_path(Instance& inst);

    // Room & game control
    void request_room_goto(i32 room_id);
    void request_room_restart();
    void request_game_restart();
    void request_game_end();
    [[nodiscard]] i32 room_order_index(i32 room_id) const noexcept;
    [[nodiscard]] i32 room_next_id(i32 room_id) const noexcept;
    [[nodiscard]] i32 room_previous_id(i32 room_id) const noexcept;
    [[nodiscard]] bool room_exists(i32 room_id) const noexcept;

    // RNG helpers
    void set_random_seed(u32 seed) noexcept;
    [[nodiscard]] u32 random_seed() const noexcept { return rng_seed_; }
    [[nodiscard]] f64 random_uniform(f64 upper) noexcept;
    [[nodiscard]] i64 random_int_inclusive(i64 upper) noexcept;

    // Text/Binary/INI/Find/Highscore helpers (used by builtins_core.cpp)
    i32 text_file_open(std::string_view gm_path, i32 mode);
    TextFileHandle* get_text_file(i32 handle);
    void text_file_close(i32 handle);

    i32 bin_file_open(std::string_view gm_path, i32 mode);
    BinFileHandle* get_bin_file(i32 handle);
    void bin_file_close(i32 handle);

    void ini_open(std::string_view gm_path);
    void ini_open_from_string(std::string_view content);
    std::string ini_close();
    std::string ini_read_string(std::string_view sec, std::string_view key, std::string_view def) const;
    f64 ini_read_real(std::string_view sec, std::string_view key, f64 def) const;
    void ini_write_string(std::string_view sec, std::string_view key, std::string_view val);
    void ini_write_real(std::string_view sec, std::string_view key, f64 val);
    bool ini_key_exists(std::string_view sec, std::string_view key) const;
    bool ini_section_exists(std::string_view sec) const;
    void ini_key_delete(std::string_view sec, std::string_view key);
    void ini_section_delete(std::string_view sec);

    std::string file_find_first(std::string_view mask, u32 attr);
    std::string file_find_next();
    void file_find_close();

    // Tiles accessors
    [[nodiscard]] std::vector<ActiveTile>& tiles() noexcept { return tiles_; }
    [[nodiscard]] i32 alloc_tile_id() noexcept { return next_tile_id_++; }

    // Screen/draw/window helpers
    void render_current_frame();
    void draw_instance_default(const Instance& inst);
    void update_window_caption();

    // Public state fields accessed by built-in implementation units
    i32 current_room_ = -1;
    i32 room_width_ = 640;
    i32 room_height_ = 480;
    i32 room_speed_ = 30;
    bool room_persistent_ = false;
    std::string room_caption_;

    u32 bg_color_ = 0xC0C0C0u;
    bool bg_showcolor_ = true;
    std::array<ActiveBackground, 8> backgrounds_{};
    std::array<ActiveView, 8> views_{};
    bool view_enabled_ = false;
    i32 view_current_ = 0;

    f64 score_ = 0.0;
    f64 lives_ = -1.0;
    f64 health_ = 100.0;
    bool show_score_ = true;
    bool show_lives_ = false;
    bool show_health_ = false;
    std::string caption_score_ = "Score: ";
    std::string caption_lives_ = "Lives: ";
    std::string caption_health_ = "Health: ";
    i32 transition_kind_ = 0;
    i32 transition_steps_ = 80;
    i32 cursor_sprite_ = -1;
    i32 window_cursor_ = 0;
    u32 window_color_ = 0;
    bool auto_draw_ = true;
    bool sync_vsync_ = false;
    bool error_occurred_ = false;
    std::string error_last_;
    bool action_relative_ = false;

    f64 mouse_x_ = 0.0;
    f64 mouse_y_ = 0.0;
    i32 mouse_button_ = 0;
    i32 mouse_lastbutton_ = 0;

    i32 fps_ = 60;
    u64 start_time_us_ = 0;
    std::vector<std::string> cmd_args_;
    std::array<HighscoreEntry, 10> highscores_{};

    // Registration helper called by the 4 builtins_*.cpp files
    void register_builtin(std::string_view name, BuiltinFn fn);

private:
    void register_all_builtins();
    void register_builtins_core();
    void register_builtins_gameplay();
    void register_builtins_graphics();
    void register_builtins_actions();

    void start_game();
    void start_room(i32 room_id, bool is_game_start);
    void process_pending_room_action();

    void update_mouse_room_coords();
    void compute_canvas_viewport(i32& out_vx, i32& out_vy, i32& out_vw, i32& out_vh, i32& out_cw, i32& out_ch) const noexcept;
    void update_view_following();
    void step_triggers(i32 moment);
    void step_timelines();
    void step_alarms();
    void step_keyboard_events();
    void step_mouse_events();
    void step_physics_and_paths();
    void step_boundary_events();
    void step_collisions();
    void step_animations();
    void render_view(i32 view_idx, f32 vx, f32 vy, f32 vw, f32 vh, f32 angle, i32 px, i32 py, i32 pw, i32 ph);

    void snapshot_active_instances(std::vector<i32>& out, u64 max_counter) const;
    void parse_ini_text(std::string_view text);
    std::string serialize_ini() const;

    Vfs& vfs_;
    PlatformWindow& window_;
    GlGraphicsDevice& gfx_;

    GameData game_;
    CompiledProgram program_;
    std::unique_ptr<GmlVm> vm_;
    Renderer2D renderer_;
    AudioEngine audio_;
    DsManager ds_;
    ParticleManager particles_;
    MpGridManager mp_grids_;

    std::vector<std::unique_ptr<Instance>> instances_;
    std::unordered_map<i32, Instance*> inst_by_id_;
    std::vector<ActiveTile> tiles_;
    std::unordered_map<i32, SavedRoomState> saved_rooms_;

    i32 next_instance_id_ = 100001;
    i32 next_tile_id_ = 10000001;
    u64 next_create_counter_ = 1;

    std::vector<EventFrame> event_stack_;
    PendingRoomAction pending_action_ = PendingRoomAction::None;
    i32 pending_room_id_ = -1;
    bool running_ = false;
    bool no_more_lives_armed_ = false;
    bool no_more_health_armed_ = false;

    u32 rng_seed_ = 0x12345678u;
    std::mt19937 rng_{0x12345678u};

    u32 fps_frame_accum_ = 0;
    u64 fps_last_sample_us_ = 0;

    std::array<BuiltinFn, 1282> builtins_{};
    std::array<bool, 1282> warned_unimpl_{};

    std::unordered_map<i32, TextFileHandle> text_files_;
    i32 next_text_file_id_ = 1;
    std::unordered_map<i32, BinFileHandle> bin_files_;
    i32 next_bin_file_id_ = 1;
    IniState ini_;
    FileFindState file_find_;
};

// Shared helper functions for built-in implementation units
[[nodiscard]] inline f64 arg_real(std::span<const RValue> args, usize i, f64 def = 0.0) noexcept {
    return (i < args.size()) ? args[i].as_real() : def;
}
[[nodiscard]] inline i32 arg_i32(std::span<const RValue> args, usize i, i32 def = 0) noexcept {
    return (i < args.size()) ? args[i].as_i32() : def;
}
[[nodiscard]] inline bool arg_bool(std::span<const RValue> args, usize i, bool def = false) noexcept {
    return (i < args.size()) ? args[i].is_truthy() : def;
}
[[nodiscard]] inline std::string_view arg_str(std::span<const RValue> args, usize i, std::string_view def = {}) noexcept {
    return (i < args.size() && args[i].is_string()) ? std::string_view(args[i].as_string()) : def;
}

} // namespace gm
