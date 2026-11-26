#pragma once

#include "audio/audio_engine.h"
#include "base/types.h"
#include "extensions/core/extension_registry.h"
#include "gml/compiler.h"
#include "gml/vm.h"
#include "graphics/gl_graphics_device.h"
#include "graphics/message_dialog.h"
#include "graphics/renderer.h"
#include "graphics/touch_overlay.h"
#include "graphics/ui_dialogs.h"
#include "loader/game_data.h"
#include "platform/vfs.h"
#include "platform/window.h"
#include "runtime/collision.h"
#include "runtime/data_structures.h"
#include "runtime/instance.h"
#include "runtime/particles.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gm {

struct TextFileHandle {
    bool open = false;
    i32 mode = 0; // 0=read, 1=write, 2=append
    std::filesystem::path path;
    std::vector<std::string> lines;
    usize line_idx = 0;
    usize col_idx = 0;
    std::string write_buf;
};

struct BinFileHandle {
    bool open = false;
    i32 mode = 0; // 0=read, 1=write, 2=rw
    std::filesystem::path path;
    std::vector<u8> data;
    usize pos = 0;
    bool dirty = false;
};

struct LegacyTextFile {
    std::filesystem::path path;
    std::string contents;
    usize pos = 0;
    bool writable = false;

    [[nodiscard]] bool eof() const noexcept { return pos >= contents.size(); }
    [[nodiscard]] bool eoln() const noexcept { return eof() || contents[pos] == '\r'; }
    std::string read_string();
    f64 read_real();
    void skip_line();
};

struct IniState {
    bool open = false;
    std::filesystem::path path;
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
    LoadGame,
};

struct TransitionScript {
    CallableKind kind = CallableKind::Script;
    u32 id = 0;
    std::string name;
};

class Runner : public IVmHost, public IExtensionHost, public NonCopyable {
public:
    using BuiltinFn = std::function<Result<RValue>(Runner&, i32 self_id, i32 other_id, std::span<const RValue> args)>;

    Runner(Vfs& vfs, PlatformWindow& window, GlGraphicsDevice& gfx);
    ~Runner() override;

    [[nodiscard]] Result<void> init(
        GameData game,
        CompiledProgram program,
        std::vector<std::string> cmd_args = {});
    void shutdown() noexcept;
    void set_touch_overlay_enabled(bool enabled) { touch_overlay_.set_enabled(enabled, &window_.input_mut()); }
    void set_touch_overlay_editing(bool editing) { touch_overlay_.set_editing(editing, &window_.input_mut()); }
    [[nodiscard]] bool is_touch_overlay_editing() const noexcept { return touch_overlay_.is_editing(); }
    void set_touch_layout_path(const std::filesystem::path& path) {
        touch_overlay_.set_config_path(path, [this]() { vfs_.notify_persist_write(); });
    }
    [[nodiscard]] std::string export_touch_layout() const { return touch_overlay_.export_layout_text(); }
    bool import_touch_layout(std::string_view text) { return touch_overlay_.import_layout_text(text, true); }
    void set_audio_muted(bool muted) noexcept { audio_.set_output_muted(muted); }
    [[nodiscard]] bool is_paused_in_background() const noexcept { return paused_in_background_; }
    [[nodiscard]] i32 current_fps() const noexcept { return fps_; }
    void record_frame_time(u64 frame_end_us);

    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] i32 room_speed() const noexcept {
        if (room_speed_ > 0) return room_speed_;
        return version_mode() == VersionMode::GM82 ? 1 : 30;
    }
    [[nodiscard]] u64 target_frame_us() const noexcept {
        return static_cast<u64>(1000000.0 / static_cast<f64>(room_speed()));
    }
    [[nodiscard]] f64 effect_speed_factor() const noexcept {
        const i32 speed = room_speed();
        if (speed <= 30 || fps_ <= 30) return 1.0;
        return 30.0 / (static_cast<f64>(speed) / fps_ < 1.2 ? speed : fps_);
    }

    // Executes one full GM8 frame (events, physics, collisions, render, swap).
    void tick_frame();

    // IVmHost implementation
    [[nodiscard]] bool treat_uninitialized_as_zero() const noexcept override {
        return game_.options.treat_uninitialized_as_zero;
    }
    [[nodiscard]] bool room_change_pending() const noexcept override { return pending_action_ != PendingRoomAction::None; }
    [[nodiscard]] Result<RValue> get_builtin_global(u32 var_id, i32 idx, bool has_idx) override;
    [[nodiscard]] Result<void> set_builtin_global(u32 var_id, i32 idx, bool has_idx, RValue value) override;
    [[nodiscard]] Result<RValue> get_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx) override;
    [[nodiscard]] Result<void> set_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx, RValue value) override;
    [[nodiscard]] VariableTable* get_instance_vars(i32 inst_id) override;
    [[nodiscard]] usize target_instance_count(i32 target) const override;
    [[nodiscard]] i32 target_instance_at(i32 target, usize index, TargetUse use) const override;
    [[nodiscard]] Result<RValue> call_builtin(u32 func_id, i32 self_id, i32 other_id, std::span<const RValue> args) override;
    [[nodiscard]] Result<RValue> call_extension_dll(u32 ext_dll_id, std::span<const RValue> args) override;

    // IExtensionHost implementation
    [[nodiscard]] Vfs& ext_vfs() noexcept override { return vfs_; }
    [[nodiscard]] AudioEngine& ext_audio() noexcept override { return audio_; }
    [[nodiscard]] const GameData& ext_game() const noexcept override { return game_; }
    [[nodiscard]] PlatformWindow& ext_window() noexcept override { return window_; }
    [[nodiscard]] GlGraphicsDevice& ext_gfx() noexcept override { return gfx_; }
    [[nodiscard]] Renderer& ext_renderer() noexcept override { return renderer_; }
    void ext_ds_list_add_real(i32 list_id, f64 value) override { ds_.list_add(list_id, RValue(value)); }
    Result<RValue> ext_call_function(i32 address, i32 self, i32 other, std::span<const RValue> args) override;
    [[nodiscard]] bool ext_error_is_enabled() const noexcept override { return game_.options.display_errors; }
    void ext_error_set_enabled(bool enabled) noexcept override { game_.options.display_errors = enabled; }
    void ext_io_set_roomend_clear(bool enabled) noexcept override { io_roomend_clear_ = enabled; }
    void ext_window_set_fixed(bool enabled) noexcept override { window_fixed_ = enabled; }
    void ext_resize_backbuffer(i32 width, i32 height) override;
    [[nodiscard]] i32 ext_game_get_state() const noexcept override;
    void ext_room_goto_cancel() noexcept override {
        pending_action_ = PendingRoomAction::None;
        pending_room_id_ = -1;
        pending_load_path_.clear();
    }
    void ext_game_end() noexcept override { request_game_end(); }
    [[nodiscard]] i32 ext_var_name_count() const noexcept override {
        return static_cast<i32>(program_.symbols.user_var_count());
    }
    [[nodiscard]] std::string ext_var_name_get(i32 index) const override;
    [[nodiscard]] std::vector<std::string> ext_instance_var_names(i32 inst_id) const override;

    // Subsystems accessors for built-in implementation units
    [[nodiscard]] GameData& game() noexcept { return game_; }
    [[nodiscard]] const GameData& game() const noexcept { return game_; }
    [[nodiscard]] CompiledProgram& program() noexcept { return program_; }
    [[nodiscard]] GmlVm& vm() noexcept { return *vm_; }
    [[nodiscard]] Vfs& vfs() noexcept { return vfs_; }
    [[nodiscard]] PlatformWindow& window() noexcept { return window_; }
    [[nodiscard]] GlGraphicsDevice& gfx() noexcept { return gfx_; }
    [[nodiscard]] Renderer& renderer() noexcept { return renderer_; }
    [[nodiscard]] MessageDialog& message_dialog() noexcept { return message_dialog_; }
    [[nodiscard]] AudioEngine& audio() noexcept { return audio_; }
    [[nodiscard]] ExtensionRegistry& extensions() noexcept { return extensions_; }
    [[nodiscard]] DsManager& ds() noexcept { return ds_; }
    [[nodiscard]] ParticleManager& particles() noexcept { return particles_; }
    [[nodiscard]] MpGridManager& mp_grids() noexcept { return mp_grids_; }
    [[nodiscard]] VersionMode version_mode() const noexcept { return game_.meta.detected_version; }

    // Instance & object hierarchy helpers
    [[nodiscard]] Instance* find_instance(i32 id) noexcept;
    [[nodiscard]] const Instance* find_instance(i32 id) const noexcept;
    [[nodiscard]] i32 instance_handle(i32 id) const noexcept override;
    [[nodiscard]] const std::vector<std::unique_ptr<Instance>>& instances() const noexcept { return instances_; }
    [[nodiscard]] const std::vector<Instance*>& object_instances(i32 object, bool include_descendants) const noexcept;
    void set_object_parent(i32 object, i32 parent);

    [[nodiscard]] bool object_exists(i32 obj_idx) const noexcept;
    [[nodiscard]] bool object_is_ancestor(i32 child_obj, i32 parent_obj) const noexcept;
    [[nodiscard]] bool object_matches(i32 inst_obj, i32 target) const noexcept;

    // Read-only queries use room order for all, inherited object order for an
    // object, and a single slot for an instance ID. The context pointers are
    // supplied only by built-ins that accept the special self/other values.
    template<class Visitor>
    auto with_instance_selection(i32 target, Visitor visit, const Instance* self = nullptr,
                                 const Instance* other = nullptr) const {
        auto scan = [&](const auto& group) {
            auto active = group | std::views::filter([](const auto& slot) {
                return slot && !slot->destroyed && !slot->deactivated;
            }) | std::views::transform([](const auto& slot) -> const Instance& { return *slot; });
            return visit(active);
        };
        if (target == -3) return scan(instances_);
        if (target >= 0 && target < 100000) return scan(object_instances(target, true));
        const Instance* single = nullptr;
        if (target >= 100000) single = find_instance(target);
        if (target == -1) single = self;
        if (target == -2) single = other;
        return scan(std::array{single});
    }

    template<class Predicate>
    i32 find_matching_instance(i32 target, Predicate matches, const Instance* self = nullptr,
                               const Instance* other = nullptr) const {
        return with_instance_selection(target, [&](auto& instances) {
            for (const Instance& inst : instances) {
                if (matches(inst)) return inst.id;
            }
            return -4;
        }, self, other);
    }

    i32 create_instance(f64 x, f64 y, i32 obj_idx, bool run_create = true, i32 forced_id = -1);
    void destroy_instance(i32 inst_id, bool perform_event = true);
    void change_instance(i32 inst_id, i32 new_obj_idx, bool perform_events);
    i32 copy_instance(i32 src_id, bool perform_event);
    void remove_marked_instances();

    void deactivate_all(bool notme, i32 self_id);
    void deactivate_object(i32 target, i32 self_id, i32 other_id = -1);
    void deactivate_region(f64 left, f64 top, f64 w, f64 h, bool inside, bool notme, i32 self_id);
    void activate_all();
    void activate_object(i32 target, i32 self_id = -1, i32 other_id = -1);
    void activate_region(f64 left, f64 top, f64 w, f64 h, bool inside);

    // Event dispatch helpers
    [[nodiscard]] const BytecodeChunk* resolve_event_chunk(i32 obj_idx, i32 cat, i32 sub, i32& out_owner_obj) const noexcept;
    [[nodiscard]] bool object_has_event(i32 obj_idx, i32 cat, i32 sub) const noexcept;
    void perform_event_on_instance(i32 inst_id, i32 obj_override, i32 cat, i32 sub, i32 other_id = -1);
    void perform_event_inherited(i32 self_id);
    void invalidate_event_tables();

    // Collision & movement helpers
    [[nodiscard]] i32 find_collision_instance(
        const Instance& self, f64 test_x, f64 test_y, i32 target, bool solid_only,
        const Instance* context_other = nullptr) const noexcept;
    [[nodiscard]] bool check_place_free(const Instance& self, f64 test_x, f64 test_y) const noexcept;
    [[nodiscard]] bool check_place_empty(const Instance& self, f64 test_x, f64 test_y) const noexcept;
    [[nodiscard]] bool check_place_meeting(const Instance& self, f64 test_x, f64 test_y, i32 target,
                                           const Instance* context_other = nullptr) const noexcept;
    void move_contact(Instance& inst, f64 dir_deg, f64 max_dist, bool solid_only);
    void move_outside(Instance& inst, f64 dir_deg, f64 max_dist, bool solid_only);
    void move_bounce(Instance& inst, bool advanced, bool solid_only);
    void start_path(Instance& inst, i32 path_id, f64 speed, i32 endaction, bool absolute);
    void end_path(Instance& inst);
    bool mp_linear_step(Instance& inst, f64 xgoal, f64 ygoal, f64 stepsize, bool checkall, i32 target_obj = -3);
    bool mp_linear_path(Instance& inst, i32 path_id, f64 xgoal, f64 ygoal, f64 stepsize, bool checkall, i32 target_obj = -3);
    bool mp_potential_step(Instance& inst, f64 xgoal, f64 ygoal, f64 stepsize, bool checkall, i32 target_obj = -3);
    bool mp_potential_path(Instance& inst, i32 path_id, f64 xgoal, f64 ygoal, f64 stepsize, f64 factor, bool checkall, i32 target_obj = -3);

    // Event-pumping wait / sleep helpers (returns false if window quit was requested)
    bool sleep_ms(f64 ms);
    bool wait_for_key();
    bool wait_for_mouse();

    // Cross-platform self-drawn GM8 modal message & input dialogs (TMessageForm)
    void show_message_dialog(std::string_view text);
    [[nodiscard]] i32 show_message_ext_dialog(
        std::string_view text,
        std::string_view but1,
        std::string_view but2,
        std::string_view but3);
    [[nodiscard]] bool show_question_dialog(std::string_view text);
    [[nodiscard]] std::string get_string_dialog(std::string_view text, std::string_view def);
    [[nodiscard]] f64 get_integer_dialog(std::string_view text, f64 def);

    // Native file & directory picker dialogs (SDL3)
    [[nodiscard]] std::string get_open_filename_dialog(std::string_view filter, std::string_view fname);
    [[nodiscard]] std::string get_save_filename_dialog(std::string_view filter, std::string_view fname);
    [[nodiscard]] std::string get_directory_dialog(std::string_view dname);
    [[nodiscard]] std::string get_directory_alt_dialog(std::string_view capt, std::string_view root);

    // Self-drawn GM8 modal dialogs (Highscore, Menu, ColorPicker, Splash, GameInfo)
    void highscore_clear();
    i32 highscore_add_entry(std::string_view name, f64 score);
    void highscore_show_dialog(f64 score_val);
    void highscore_add_current_dialog();
    [[nodiscard]] RValue show_menu_dialog(
        std::string_view items_str,
        const RValue& default_val,
        std::optional<std::pair<i32, i32>> screen_pos = std::nullopt);
    [[nodiscard]] i32 get_color_dialog(i32 def_col);
    bool splash_show_image_dialog(std::string_view fname, i32 delay_ms);
    bool splash_show_text_dialog(std::string_view fname, i32 delay_ms);
    void show_info_dialog();
    bool load_info_file(std::string_view fname);

    // Dynamic asset & event mutation helpers
    static void apply_removeback_and_smooth(Bitmap32& bmp, bool removeback, bool smooth);
    static void rebuild_sprite_masks(SpriteData& sp, i32 alpha_tolerance = 0, i32 bbox_mode = 0,
                                     i32 bleft = 0, i32 bright = 0, i32 btop = 0, i32 bbottom = 0);
    i32 create_sprite_from_bitmap(Bitmap32 bmp, bool removeback, bool smooth, i32 xorig, i32 yorig);
    i32 create_background_from_bitmap(Bitmap32 bmp, bool removeback, bool smooth);
    bool object_event_add(i32 obj_idx, i32 ev_type, i32 ev_numb, std::string_view code);
    bool object_event_clear(i32 obj_idx, i32 ev_type, i32 ev_numb);
    bool timeline_moment_add(i32 tl_idx, i32 step, std::string_view code);
    bool timeline_moment_clear(i32 tl_idx, i32 step);
    void timeline_clear(i32 tl_idx);
    bool room_set_code(i32 room_idx, std::string_view code);

    // Room & game control
    void request_room_goto(i32 room_id);
    void request_room_restart();
    void request_game_restart();
    void request_game_end();
    void request_game_load(std::string_view path);
    bool save_game(std::string_view path);
    bool load_game(std::string_view path);
    void request_host_stop() { host_stop_requested_ = true; request_game_end(); }
    [[nodiscard]] i32 room_order_index(i32 room_id) const noexcept;
    [[nodiscard]] i32 room_next_id(i32 room_id) const noexcept;
    [[nodiscard]] i32 room_previous_id(i32 room_id) const noexcept;
    [[nodiscard]] bool room_exists(i32 room_id) const noexcept;

    // Room transitions (GM 8.0 / 8.1 / 8.2)
    bool transition_define(i32 kind, std::string_view script_name);
    [[nodiscard]] bool transition_exists(i32 kind) const noexcept;

    // RNG helpers
    void set_random_seed(u32 seed) noexcept;
    [[nodiscard]] i32 random_seed() const noexcept { return static_cast<i32>(rng_.seed); }
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

    // Tiles & instance ID allocators
    [[nodiscard]] std::vector<ActiveTile>& tiles() noexcept { return tiles_; }
    [[nodiscard]] i32 alloc_tile_id() noexcept { return next_tile_id_++; }
    [[nodiscard]] i32 alloc_instance_id() noexcept { return next_instance_id_++; }

    // Screen/draw/window helpers
    void render_current_frame();
    void present_current_frame();
    void set_window_size(i32 width, i32 height);
    void adapt_window_to_region(bool adapt);
    void window_default();
    void draw_instance_default(const Instance& inst);
    void compute_canvas_viewport(i32& out_vx, i32& out_vy, i32& out_vw, i32& out_vh, i32& out_cw, i32& out_ch) const noexcept;
    void update_mouse_room_coords();

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
    bool show_score_explicit_ = false;
    bool show_lives_ = false;
    bool show_health_ = false;
    std::string caption_score_ = "Score: ";
    std::string caption_lives_ = "Lives: ";
    std::string caption_health_ = "Health: ";
    i32 transition_kind_ = 0;
    i32 transition_steps_ = 80;
    i32 cursor_sprite_ = -1;
    i32 cursor_subimg_ = 0;
    i32 window_cursor_ = 0;
    u32 window_color_ = 0;
    bool auto_draw_ = true;
    bool error_occurred_ = false;
    std::string error_last_;
    bool action_relative_ = false;

    f64 mouse_x_ = 0.0;
    f64 mouse_y_ = 0.0;
    f64 event_mouse_x_ = 0.0;
    f64 event_mouse_y_ = 0.0;
    i32 mouse_button_ = 0;
    i32 mouse_lastbutton_ = 0;

    i32 fps_ = 0;
    u64 start_time_us_ = 0;
    std::vector<std::string> cmd_args_;
    std::array<HighscoreEntry, 10> highscores_{};

    // Motion planning potential settings
    f64 mp_potential_maxrot_ = 30.0;
    f64 mp_potential_rotstep_ = 10.0;
    f64 mp_potential_ahead_ = 3.0;
    bool mp_potential_onspot_ = true;

    // Window & transition state
    bool window_showborder_ = true;
    f64 window_region_scale_ = -1.0;
    i32 window_region_w_ = 0;
    i32 window_region_h_ = 0;
    i32 requested_backbuffer_w_ = 0;
    i32 requested_backbuffer_h_ = 0;
    std::unordered_map<i32, TransitionScript> transition_scripts_;

    // Highscore appearance state
    i32 highscore_bg_ = -1;
    bool highscore_border_ = true;
    u32 highscore_col_back_ = 0xFFFFFFu;
    u32 highscore_col_new_ = 0x0000FFu;
    u32 highscore_col_other_ = 0x000000u;
    std::string highscore_font_name_ = "Arial";
    i32 highscore_font_size_ = 12;
    i32 highscore_font_style_ = 0;
    std::string highscore_caption_ = "Top Ten Players";
    std::string highscore_nobody_ = "<nobody>";
    std::string highscore_escape_ = "press <Escape> to close";

    // Splash screen settings
    std::string splash_caption_;
    bool splash_fullscreen_ = false;
    bool splash_border_ = true;
    i32 splash_size_w_ = 640;
    i32 splash_size_h_ = 480;
    i32 splash_pos_x_ = -1;
    i32 splash_pos_y_ = -1;
    bool splash_adapt_ = true;
    bool splash_top_ = true;
    u32 splash_color_ = 0;
    bool splash_main_ = true;
    f64 splash_scale_ = -1.0;
    bool splash_cursor_ = true;
    bool splash_interrupt_ = true;
    bool splash_stop_key_ = true;
    bool splash_stop_mouse_ = true;
    bool splash_close_button_ = true;

    // Legacy D&D action state
    u32 action_fill_color_ = 0xFFFFFFu;
    u32 action_line_color_ = 0x000000u;
    i32 action_part_system_ = -1;
    std::array<i32, 16> action_part_types_{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<i32, 16> action_part_emitters_{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    // Registration helper called by the builtins_*.cpp files
    void register_builtin(std::string_view name, BuiltinFn fn);

private:
    void register_all_builtins();
    void register_builtins_core();
    void register_builtins_gameplay();
    void register_builtins_graphics();
    void register_builtins_actions();
    void register_builtins_misc();

    template<class Fn>
    void for_instances_at_position(f64 x, f64 y, Fn visit) {
        // A Destroy or Create event can move later instances or change the room list.
        const usize count = instances_.size();
        for (usize i = 0; i < count && i < instances_.size(); ++i) {
            const Instance& inst = *instances_[i];
            if (!inst.destroyed && !inst.deactivated &&
                check_point_collision(inst, x, y, true, game_, version_mode())) {
                visit(inst.handle);
            }
        }
    }

    void start_game();
    void register_instance_handle(Instance& instance);
    void add_room_instance(std::unique_ptr<Instance> instance);
    void set_instance_object(Instance& instance, i32 object) const;
    void index_instance(Instance& instance);
    [[nodiscard]] const Instance* field_target_by_id(i32 id, TargetUse use) const;
    mutable std::array<usize, 2> cached_field_indices_{};
    void rebuild_object_instances();
    void move_deactivated(bool deactivate);
    bool broadcast_event(i32 category, i32 sub);
    bool execute_instance_event(i32 inst_id, i32 obj_idx, i32 cat, i32 sub, i32 other_id);
    void start_room(i32 room_id, bool is_game_start);
    void process_pending_room_action();
    void sync_room_screen_region();
    void prepare_room_transition(i32 kind);
    void finish_room_transition();
    void cancel_room_transition() noexcept;
    void execute_custom_transition_script(const TransitionScript& script,
                                          i32 surf_old, i32 surf_new,
                                          i32 width, i32 height, f64 fraction);

    void update_window_caption();
    void room_canvas_size(i32& width, i32& height) const noexcept;
    void update_view_following();
    void step_triggers(i32 moment);
    void step_event(i32 moment);
    void step_timelines();
    void step_alarms();
    void step_keyboard_events();
    void step_mouse_events();
    void step_physics_and_paths();
    void step_boundary_events();
    void step_collisions();
    void step_animations();
    const Gm82EventTables& event_tables();
    const std::vector<i32>& event_objects(i32 category, i32 sub);

    // Delphi's for loop captures the last index on entry, but fetches each
    // instance from the current list. A later object group takes its own bound.
    template<class Fn>
    void for_object_instances(i32 object, usize first, Fn&& visit) {
        const usize count = object_instances(object, false).size();
        for (usize i = first; i < count; ++i) {
            const auto& live = object_instances(object, false);
            if (i >= live.size()) break;
            Instance* inst = live[i];
            if (inst && !inst->destroyed && !inst->deactivated) visit(*inst, i);
        }
    }

    template<class Fn>
    void for_event_instances(i32 category, i32 sub, Fn&& visit, bool continue_after_transition = false) {
        const usize object_count = event_objects(category, sub).size();
        for (usize i = 0; i < object_count; ++i) {
            if (!continue_after_transition && pending_action_ != PendingRoomAction::None) return;
            const auto& objects = event_objects(category, sub);
            if (i >= objects.size()) break;
            const i32 object = objects[i];
            if (!object_exists(object)) continue;
            for_object_instances(object, 0, [&](Instance& inst, usize) {
                if (continue_after_transition || pending_action_ == PendingRoomAction::None) visit(inst);
            });
        }
    }
    void render_view(i32 view_idx, f32 vx, f32 vy, f32 vw, f32 vh, f32 angle, i32 px, i32 py, i32 pw, i32 ph);
    void run_message_dialog_modal(bool with_input);
    void run_ui_dialog_modal(bool show_cursor);

    void parse_ini_text(std::string_view text);
    std::string serialize_ini() const;

    Vfs& vfs_;
    PlatformWindow& window_;
    GlGraphicsDevice& gfx_;

    GameData game_;
    CompiledProgram program_;
    std::optional<Gm82EventTables> generated_event_tables_;
    bool use_precomputed_events_ = true;
    std::unique_ptr<GmlVm> vm_;
    Renderer renderer_;
    MessageDialog message_dialog_;
    GmModalDialogs modal_dialogs_;
    TouchOverlay touch_overlay_;
    AudioEngine audio_;
    ExtensionRegistry extensions_;
    DsManager ds_;
    GmlRandom rng_;
    ParticleManager particles_{rng_};
    MpGridManager mp_grids_;

    // CRoom maintains separate ordered active and inactive lists. Object lists
    // reference the active entries, including marked instances until cleanup.
    std::vector<std::unique_ptr<Instance>> instances_;
    std::vector<std::unique_ptr<Instance>> inactive_instances_;
    struct ObjectInstances {
        std::vector<Instance*> exact;
        std::vector<Instance*> recursive;
    };
    std::deque<ObjectInstances> object_instances_;
    std::unordered_map<i32, Instance*> inst_by_id_;
    i32 next_handle_ = -8; // Below all GML selector constants.
    std::vector<ActiveTile> tiles_;
    std::unordered_map<i32, SavedRoomState> saved_rooms_;

    i32 next_instance_id_ = 100001;
    i32 next_tile_id_ = 10000001;

    std::vector<EventFrame> event_stack_;
    bool host_stop_requested_ = false;
    PendingRoomAction pending_action_ = PendingRoomAction::None;
    i32 pending_room_id_ = -1;
    std::string pending_load_path_;
    i32 active_transition_kind_ = -1;
    i32 transition_surf_old_ = -1;
    i32 transition_surf_new_ = -1;
    struct RenderItem {
        f64 depth = 0.0;
        u8 kind = 0; // 0 = tile, 1 = instance, 2 = particles
        usize order = 0;
        i32 ref_id = 0;
    };
    std::vector<RenderItem> render_items_scratch_;

    bool running_ = false;
    bool paused_in_background_ = false;
    bool web_audio_unlocked_ = false;
    bool io_roomend_clear_ = true;
    bool window_fixed_ = false;
    bool no_more_lives_armed_ = false;
    bool no_more_health_armed_ = false;

    u32 fps_frame_accum_ = 0;
    u64 fps_next_second_us_ = 0;

    std::array<BuiltinFn, 1282> builtins_{};
    std::array<bool, 1282> warned_unimpl_{};

    std::unordered_map<i32, TextFileHandle> text_files_;
    i32 next_text_file_id_ = 1;
    std::optional<LegacyTextFile> legacy_text_file_;
    Result<RValue> open_legacy_text_file(std::string_view path, i32 mode);
    Result<RValue> close_legacy_text_file();
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
