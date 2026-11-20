#include "runtime/runner.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "gml/builtins_catalog.h"
#include "platform/clock.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <sstream>

namespace gm {

namespace {

std::string trim_ascii(std::string_view s) {
    usize b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    usize e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return std::string(s.substr(b, e - b));
}

std::tm local_time_now() {
    const std::time_t t = std::time(nullptr);
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Runner lifecycle
// ---------------------------------------------------------------------------

Runner::Runner(Vfs& vfs, PlatformWindow& window, GlGraphicsDevice& gfx)
    : vfs_(vfs), window_(window), gfx_(gfx) {}

Runner::~Runner() {
    shutdown();
}

Result<void> Runner::init(GameData game, CompiledProgram program, std::vector<std::string> cmd_args) {
    game_ = std::move(game);
    program_ = std::move(program);
    cmd_args_ = std::move(cmd_args);

    next_instance_id_ = std::max(100001, game_.room_max_id + 1);
    next_tile_id_ = std::max(10000001, game_.room_max_tile_id + 1);
    next_create_counter_ = 1;
    window_color_ = game_.options.window_color;
    sync_vsync_ = (game_.options.sync_vertex != 0);

    // Initialize renderer and upload all embedded sprites/backgrounds/fonts.
    if (auto res = renderer_.init(gfx_); !res) {
        return res;
    }
    renderer_.set_interpolation(game_.options.interpolate_pixels);
    renderer_.upload_game_assets(game_);

    // Initialize audio engine (non-fatal if no audio device is available, e.g., headless).
    if (auto res = audio_.init(); !res) {
        GM_LOG_WARN("audio", "Audio initialization failed (%s); continuing muted", res.error_message().c_str());
    }
    audio_.load_game_sounds(game_);

    // Export included files marked export_action == 1 (temp_dir) or 2 (working_dir).
    for (const auto& inc : game_.included_files) {
        if (!inc.exists || !inc.store || inc.data.empty()) continue;
        if (inc.export_action == 1) {
            const std::string p = vfs_.temp_directory_gm() + inc.filename;
            if (inc.overwrite || !vfs_.exists(p)) {
                (void)vfs_.write_all_bytes(p, inc.data);
            }
        } else if (inc.export_action == 2) {
            if (inc.overwrite || !vfs_.exists(inc.filename)) {
                (void)vfs_.write_all_bytes(inc.filename, inc.data);
            }
        }
    }

    // Extract GEX DLL payloads into temp_dir for external DLL shims / lookups.
    for (const auto& ext : game_.extensions) {
        for (const auto& inc : ext.includes) {
            if (inc.kind == 1 && !inc.payload.empty() && !inc.filename.empty()) {
                const std::string p = vfs_.temp_directory_gm() + inc.filename;
                (void)vfs_.write_all_bytes(p, inc.payload);
            }
        }
    }

    // Bind built-in function dispatch table and create VM.
    register_all_builtins();

    // When verbose logging is enabled, audit all built-ins referenced by this game's bytecode.
    if (Logger::level() <= LogLevel::Debug) {
        std::array<bool, 1282> missing_seen{};
        auto scan_chunk = [&](const BytecodeChunk& ch) {
            for (const auto& ins : ch.code) {
                if (ins.op == OpCode::CallBuiltin && ins.operand < builtins_.size()) {
                    if (builtins_[ins.operand] == nullptr && !missing_seen[ins.operand]) {
                        missing_seen[ins.operand] = true;
                        const auto* info = BuiltinsCatalog::function_by_id(ins.operand);
                        GM_LOG_DEBUG("runtime", "Referenced built-in not yet implemented: %s (id=%u)",
                                     info ? std::string(info->name).c_str() : "?", ins.operand);
                    }
                }
            }
        };
        for (const auto& c : program_.const_init_chunks) scan_chunk(c);
        for (const auto& c : program_.script_chunks) if (c) scan_chunk(*c);
        for (const auto& c : program_.ext_gml_chunks) scan_chunk(c);
        for (const auto& c : program_.trigger_chunks) if (c) scan_chunk(*c);
        for (const auto& arr : program_.object_event_chunks)
            for (const auto& mp : arr)
                for (const auto& [_, c] : mp) scan_chunk(c);
        for (const auto& mp : program_.timeline_chunks)
            for (const auto& [_, c] : mp) scan_chunk(c);
        for (const auto& c : program_.room_creation_chunks) if (c) scan_chunk(*c);
        for (const auto& [_, c] : program_.instance_creation_chunks) scan_chunk(c);
        for (const auto& c : program_.library_init_chunks) scan_chunk(c);
    }

    vm_ = std::make_unique<GmlVm>(program_, this);

    start_time_us_ = Clock::now_us();
    fps_last_sample_us_ = start_time_us_;
    fps_frame_accum_ = 0;

    // Seed PRNG with current time (GM8 randomizes on startup unless fixed by game).
    set_random_seed(static_cast<u32>(start_time_us_ & 0xFFFFFFFFu));

    // Evaluate expression constants once before any room starts.
    if (auto res = vm_->init_constants(-1); !res) {
        GM_LOG_WARN("runtime", "Constant initialization warning: %s", res.error_message().c_str());
    }

    // Run extension GML init functions (if any).
    for (const auto& ext : game_.extensions) {
        for (const auto& inc : ext.includes) {
            if (inc.kind == 2 && !inc.init_func.empty()) {
                if (const auto* target = program_.symbols.find_callable(inc.init_func)) {
                    if (target->kind == CallableKind::ExtensionGml) {
                        auto r = vm_->execute_ext_gml(target->id, -1, -1);
                        if (!r) {
                            GM_LOG_WARN("runtime", "Extension init '%s' error: %s",
                                        inc.init_func.c_str(), r.error_message().c_str());
                        }
                    }
                }
            }
        }
    }

    // Run D&D library initialization code chunks.
    for (const auto& chunk : program_.library_init_chunks) {
        if (!chunk.code.empty()) {
            auto r = vm_->execute(chunk, -1, -1);
            if (!r) {
                GM_LOG_WARN("runtime", "Library init error: %s", r.error_message().c_str());
            }
        }
    }

    running_ = true;
    start_game();
    return Result<void>::success();
}

void Runner::shutdown() noexcept {
    if (!running_ && !vm_) {
        return;
    }
    running_ = false;

    if (vm_ && current_room_ >= 0) {
        // Fire Game End event on remaining active instances.
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            perform_event_on_instance(id, -1, 7, 3, id); // ev_other, ev_game_end
        }
    }

    // Flush open files & INI.
    if (ini_.open) {
        (void)ini_close();
    }
    for (auto& [id, tf] : text_files_) {
        if (tf.open && tf.mode != 0) {
            (void)vfs_.write_all_bytes(tf.gm_path,
                                       std::span<const u8>(reinterpret_cast<const u8*>(tf.write_buf.data()),
                                                           tf.write_buf.size()));
        }
    }
    text_files_.clear();
    for (auto& [id, bf] : bin_files_) {
        if (bf.open && bf.dirty) {
            (void)vfs_.write_all_bytes(bf.gm_path, bf.data);
        }
    }
    bin_files_.clear();

    instances_.clear();
    inst_by_id_.clear();
    tiles_.clear();
    saved_rooms_.clear();
    ds_.clear_all();
    particles_.clear_all();
    mp_grids_.destroy_all();

    audio_.shutdown();
    renderer_.shutdown();
    vm_.reset();
}

void Runner::register_builtin(std::string_view name, BuiltinFn fn) {
    if (const auto* info = BuiltinsCatalog::find_function(name)) {
        if (info->id < builtins_.size()) {
            builtins_[info->id] = fn;
        }
    }
}

void Runner::register_all_builtins() {
    builtins_.fill(nullptr);
    warned_unimpl_.fill(false);
    register_builtins_core();
    register_builtins_gameplay();
    register_builtins_graphics();
    register_builtins_actions();
}

// ---------------------------------------------------------------------------
// Game & Room lifecycle
// ---------------------------------------------------------------------------

void Runner::start_game() {
    instances_.clear();
    inst_by_id_.clear();
    tiles_.clear();
    saved_rooms_.clear();
    particles_.clear_all();
    audio_.sound_stop_all();

    score_ = 0.0;
    lives_ = -1.0;
    health_ = 100.0;
    no_more_lives_armed_ = false;
    no_more_health_armed_ = false;
    current_room_ = -1;
    pending_action_ = PendingRoomAction::None;
    pending_room_id_ = -1;

    if (game_.room_order.empty()) {
        GM_LOG_WARN("runtime", "Game has no rooms in room_order");
        running_ = false;
        return;
    }

    start_room(game_.room_order[0], true);
    process_pending_room_action();
}

void Runner::start_room(i32 room_id, bool is_game_start) {
    if (!room_exists(room_id)) {
        GM_LOG_ERROR("runtime", "Cannot enter invalid room id %d", room_id);
        running_ = false;
        return;
    }

    // 1. Leave previous room (if any)
    if (!is_game_start && current_room_ >= 0) {
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            perform_event_on_instance(id, -1, 7, 5, id); // ev_other, ev_room_end
        }
        remove_marked_instances();

        if (room_persistent_) {
            SavedRoomState state{};
            state.tiles = tiles_;
            state.backgrounds = backgrounds_;
            state.views = views_;
            state.view_enabled = view_enabled_;
            state.background_color = bg_color_;
            state.background_showcolor = bg_showcolor_;
            state.width = room_width_;
            state.height = room_height_;
            state.speed = room_speed_;
            state.caption = room_caption_;

            std::vector<std::unique_ptr<Instance>> persistent_kept;
            inst_by_id_.clear();
            for (auto& up : instances_) {
                if (up->destroyed) continue;
                if (up->persistent) {
                    inst_by_id_[up->id] = up.get();
                    persistent_kept.push_back(std::move(up));
                } else {
                    state.instances.push_back(*up);
                }
            }
            instances_ = std::move(persistent_kept);
            saved_rooms_[current_room_] = std::move(state);
        } else {
            std::vector<std::unique_ptr<Instance>> persistent_kept;
            inst_by_id_.clear();
            for (auto& up : instances_) {
                if (!up->destroyed && up->persistent) {
                    inst_by_id_[up->id] = up.get();
                    persistent_kept.push_back(std::move(up));
                }
            }
            instances_ = std::move(persistent_kept);
        }
        tiles_.clear();
    }

    current_room_ = room_id;
    const auto& rm = game_.rooms[static_cast<usize>(room_id)];
    bool restored_from_save = false;

    if (auto it = saved_rooms_.find(room_id); it != saved_rooms_.end()) {
        restored_from_save = true;
        SavedRoomState state = std::move(it->second);
        saved_rooms_.erase(it);

        room_width_ = state.width;
        room_height_ = state.height;
        room_speed_ = state.speed;
        room_persistent_ = rm.persistent;
        room_caption_ = state.caption;
        bg_color_ = state.background_color;
        bg_showcolor_ = state.background_showcolor;
        backgrounds_ = state.backgrounds;
        views_ = state.views;
        view_enabled_ = state.view_enabled;
        tiles_ = std::move(state.tiles);

        for (auto& saved_inst : state.instances) {
            auto up = std::make_unique<Instance>(std::move(saved_inst));
            up->mark_bbox_dirty();
            inst_by_id_[up->id] = up.get();
            instances_.push_back(std::move(up));
        }
    } else {
        room_width_ = rm.width;
        room_height_ = rm.height;
        room_speed_ = rm.speed > 0 ? rm.speed : 30;
        room_persistent_ = rm.persistent;
        room_caption_ = rm.caption;
        bg_color_ = rm.color & 0x00FFFFFFu;
        bg_showcolor_ = rm.show_color;
        view_enabled_ = rm.enable_views;

        for (usize i = 0; i < 8; ++i) {
            ActiveBackground ab{};
            if (i < rm.backgrounds.size()) {
                const auto& rb = rm.backgrounds[i];
                ab.visible = rb.visible;
                ab.foreground = rb.foreground;
                ab.index = rb.background_index;
                ab.x = static_cast<f64>(rb.x);
                ab.y = static_cast<f64>(rb.y);
                ab.htiled = rb.htiled;
                ab.vtiled = rb.vtiled;
                ab.hspeed = static_cast<f64>(rb.hspeed);
                ab.vspeed = static_cast<f64>(rb.vspeed);
                ab.stretch = rb.stretch;
            }
            backgrounds_[i] = ab;
        }

        for (usize i = 0; i < 8; ++i) {
            ActiveView av{};
            if (i < rm.views.size()) {
                const auto& rv = rm.views[i];
                av.visible = rv.visible;
                av.xview = static_cast<f64>(rv.xview);
                av.yview = static_cast<f64>(rv.yview);
                av.wview = static_cast<f64>(rv.wview);
                av.hview = static_cast<f64>(rv.hview);
                av.xport = rv.xport;
                av.yport = rv.yport;
                av.wport = rv.wport;
                av.hport = rv.hport;
                av.hborder = rv.hborder;
                av.vborder = rv.vborder;
                av.hspeed = rv.hspeed;
                av.vspeed = rv.vspeed;
                av.follow_object = rv.follow_object;
            }
            views_[i] = av;
        }

        tiles_.clear();
        tiles_.reserve(rm.tiles.size());
        for (const auto& rt : rm.tiles) {
            ActiveTile t{};
            t.id = rt.id;
            t.background_index = rt.background_index;
            t.xo = rt.xo;
            t.yo = rt.yo;
            t.w = rt.w;
            t.h = rt.h;
            t.x = static_cast<f64>(rt.x);
            t.y = static_cast<f64>(rt.y);
            t.depth = static_cast<f64>(rt.depth);
            t.xscale = rt.scale_x;
            t.yscale = rt.scale_y;
            t.blend = rt.blend & 0x00FFFFFFu;
            t.alpha = rt.alpha;
            t.visible = true;
            tiles_.push_back(t);
        }

        // First allocate all room instances so creation codes can reference each other by ID.
        std::vector<i32> created_ids;
        created_ids.reserve(rm.instances.size());
        for (const auto& ri : rm.instances) {
            const i32 id = create_instance(
                static_cast<f64>(ri.x), static_cast<f64>(ri.y),
                ri.object_index, /*run_create=*/false, ri.id);
            if (auto* inst = find_instance(id)) {
                inst->image_xscale = ri.scale_x;
                inst->image_yscale = ri.scale_y;
                inst->image_angle = ri.rotation;
                inst->image_blend = ri.color & 0x00FFFFFFu;
                inst->image_alpha = static_cast<f64>((ri.color >> 24u) & 0xFFu) / 255.0;
                inst->mark_bbox_dirty();
                created_ids.push_back(id);
            }
        }

        // Execute Create events and instance creation codes in GM8 order.
        for (i32 id : created_ids) {
            if (pending_action_ != PendingRoomAction::None) break;
            auto* inst = find_instance(id);
            if (!inst || inst->destroyed) continue;

            if (game_.options.creation_event_order) {
                if (auto cc_it = program_.instance_creation_chunks.find(id);
                    cc_it != program_.instance_creation_chunks.end() && !cc_it->second.code.empty()) {
                    auto r = vm_->execute(cc_it->second, id, id);
                    if (!r) GM_LOG_WARN("runtime", "Instance %d creation code error: %s", id, r.error_message().c_str());
                }
                if (find_instance(id) && !find_instance(id)->destroyed) {
                    perform_event_on_instance(id, -1, 0, 0, id); // ev_create
                }
            } else {
                perform_event_on_instance(id, -1, 0, 0, id); // ev_create
                if (pending_action_ != PendingRoomAction::None) break;
                if (find_instance(id) && !find_instance(id)->destroyed) {
                    if (auto cc_it = program_.instance_creation_chunks.find(id);
                        cc_it != program_.instance_creation_chunks.end() && !cc_it->second.code.empty()) {
                        auto r = vm_->execute(cc_it->second, id, id);
                        if (!r) GM_LOG_WARN("runtime", "Instance %d creation code error: %s", id, r.error_message().c_str());
                    }
                }
            }
        }
    }

    update_window_caption();

    // Update room creation code (only on first visit to room)
    if (!restored_from_save && pending_action_ == PendingRoomAction::None) {
        if (static_cast<usize>(room_id) < program_.room_creation_chunks.size() &&
            program_.room_creation_chunks[static_cast<usize>(room_id)].has_value()) {
            const auto& chunk = *program_.room_creation_chunks[static_cast<usize>(room_id)];
            if (!chunk.code.empty()) {
                auto r = vm_->execute(chunk, -1, -1);
                if (!r) {
                    GM_LOG_WARN("runtime", "Room '%s' creation code error: %s",
                                rm.name.c_str(), r.error_message().c_str());
                }
            }
        }
    }

    // Fire Game Start (first room only)
    if (is_game_start && pending_action_ == PendingRoomAction::None) {
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            if (pending_action_ != PendingRoomAction::None) break;
            perform_event_on_instance(id, -1, 7, 2, id); // ev_other, ev_game_start
        }
    }

    // Fire Room Start
    if (pending_action_ == PendingRoomAction::None) {
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            if (pending_action_ != PendingRoomAction::None) break;
            perform_event_on_instance(id, -1, 7, 4, id); // ev_other, ev_room_start
        }
    }

    remove_marked_instances();
    update_view_following();
}

void Runner::process_pending_room_action() {
    for (int guard = 0; guard < 32 && pending_action_ != PendingRoomAction::None; ++guard) {
        const PendingRoomAction act = pending_action_;
        const i32 target = pending_room_id_;
        pending_action_ = PendingRoomAction::None;
        pending_room_id_ = -1;

        switch (act) {
            case PendingRoomAction::None:
                return;
            case PendingRoomAction::Goto:
                start_room(target, false);
                break;
            case PendingRoomAction::RestartRoom:
                saved_rooms_.erase(current_room_);
                start_room(current_room_, false);
                break;
            case PendingRoomAction::RestartGame:
                start_game();
                return;
            case PendingRoomAction::EndGame:
                running_ = false;
                return;
        }
    }
}

void Runner::request_room_goto(i32 room_id) {
    if (room_exists(room_id)) {
        pending_action_ = PendingRoomAction::Goto;
        pending_room_id_ = room_id;
    }
}

void Runner::request_room_restart() {
    pending_action_ = PendingRoomAction::RestartRoom;
    pending_room_id_ = current_room_;
}

void Runner::request_game_restart() {
    pending_action_ = PendingRoomAction::RestartGame;
}

void Runner::request_game_end() {
    pending_action_ = PendingRoomAction::EndGame;
}

bool Runner::room_exists(i32 room_id) const noexcept {
    return room_id >= 0 && static_cast<usize>(room_id) < game_.rooms.size() &&
           game_.rooms[static_cast<usize>(room_id)].exists;
}

i32 Runner::room_order_index(i32 room_id) const noexcept {
    for (usize i = 0; i < game_.room_order.size(); ++i) {
        if (game_.room_order[i] == room_id) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

i32 Runner::room_next_id(i32 room_id) const noexcept {
    const i32 idx = room_order_index(room_id);
    if (idx < 0 || static_cast<usize>(idx + 1) >= game_.room_order.size()) return -1;
    return game_.room_order[static_cast<usize>(idx + 1)];
}

i32 Runner::room_previous_id(i32 room_id) const noexcept {
    const i32 idx = room_order_index(room_id);
    if (idx <= 0) return -1;
    return game_.room_order[static_cast<usize>(idx - 1)];
}

// ---------------------------------------------------------------------------
// Exact GM8 17-Stage Frame Step (gm81.exe.c:sub_60756C)
// ---------------------------------------------------------------------------

void Runner::tick_frame() {
    if (!running_) return;

    // Stage 1: Poll OS/SDL events & map mouse coordinates to active view.
    const WindowEvents ev = window_.poll_events();
    const InputState& inp = window_.input();

    if (ev.quit_requested) {
        // Fire ev_close_button (ev_other=7, sub=30) if any object handles it, then quit if close_sec allows.
        bool handled_close = false;
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            if (auto* inst = find_instance(id); inst && object_has_event(inst->object_index, 7, 30)) {
                handled_close = true;
                perform_event_on_instance(id, -1, 7, 30, id);
            }
        }
        if (!handled_close || pending_action_ == PendingRoomAction::EndGame) {
            running_ = false;
            return;
        }
    }

    // Standard GM8 default hotkeys (respecting GameOptions).
    if (game_.options.quit_key && inp.key_pressed[27]) { // ESC
        running_ = false;
        return;
    }
    if (game_.options.screen_key && inp.key_pressed[115]) { // F4 fullscreen toggle
        window_.set_fullscreen(!window_.is_fullscreen());
    }
    if (game_.options.screenshot_key && inp.key_pressed[120]) { // F9 screenshot
        (void)renderer_.screen_save("screenshot.png");
    }

    // Update mouse_button / mouse_lastbutton globals
    mouse_button_ = 0;
    if (inp.mouse_down[1]) mouse_button_ = 1;
    else if (inp.mouse_down[2]) mouse_button_ = 2;
    else if (inp.mouse_down[3]) mouse_button_ = 3;
    if (inp.mouse_pressed[1]) mouse_lastbutton_ = 1;
    else if (inp.mouse_pressed[2]) mouse_lastbutton_ = 2;
    else if (inp.mouse_pressed[3]) mouse_lastbutton_ = 3;

    update_mouse_room_coords();

    // Stage 2: Snapshot xprevious/yprevious & advance backgrounds + paths
    for (auto& bg : backgrounds_) {
        bg.x += bg.hspeed;
        bg.y += bg.vspeed;
    }
    for (auto& up : instances_) {
        if (up->destroyed || up->deactivated) continue;
        up->xprevious = up->x;
        up->yprevious = up->y;
        up->path_positionprevious = up->path_position;
    }

    // Stage 3: Begin Step triggers (moment=1) + Begin Step (ev 3, sub 1)
    step_triggers(1);
    if (pending_action_ == PendingRoomAction::None) {
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            if (pending_action_ != PendingRoomAction::None) break;
            perform_event_on_instance(id, -1, 3, 1, id);
        }
    }
    remove_marked_instances();

    // Stage 4: Timelines
    if (pending_action_ == PendingRoomAction::None) {
        step_timelines();
    }

    // Stage 5: Alarms 0..11
    if (pending_action_ == PendingRoomAction::None) {
        step_alarms();
    }

    // Stage 6: Keyboard events (held=5, pressed=9, released=10)
    if (pending_action_ == PendingRoomAction::None) {
        step_keyboard_events();
    }

    // Stage 7: Mouse events (ev 6)
    if (pending_action_ == PendingRoomAction::None) {
        step_mouse_events();
    }

    // Stage 8: Normal Step triggers (moment=0) + Normal Step (ev 3, sub 0)
    if (pending_action_ == PendingRoomAction::None) {
        step_triggers(0);
    }
    if (pending_action_ == PendingRoomAction::None) {
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            if (pending_action_ != PendingRoomAction::None) break;
            perform_event_on_instance(id, -1, 3, 0, id);
        }
    }
    remove_marked_instances();

    // Stage 9: Friction, gravity, paths, motion integration (x += hspeed; y += vspeed)
    if (pending_action_ == PendingRoomAction::None) {
        step_physics_and_paths();
    }

    // Stage 10: Outside Room / Intersect Boundary / Outside & Boundary View 0..7
    if (pending_action_ == PendingRoomAction::None) {
        step_boundary_events();
    }

    // Stage 11: Collisions (ev 4) with solid rollback & retry
    if (pending_action_ == PendingRoomAction::None) {
        step_collisions();
    }

    // Stage 12: End Step triggers (moment=2) + End Step (ev 3, sub 2)
    if (pending_action_ == PendingRoomAction::None) {
        step_triggers(2);
    }
    if (pending_action_ == PendingRoomAction::None) {
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            if (pending_action_ != PendingRoomAction::None) break;
            perform_event_on_instance(id, -1, 3, 2, id);
        }
    }

    // Stage 13: Particle update & deferred destroy cleanup
    particles_.update_all();
    remove_marked_instances();

    // Process room transitions requested during step BEFORE drawing.
    if (pending_action_ != PendingRoomAction::None) {
        process_pending_room_action();
        if (!running_) return;
    }

    // Stage 14: Draw pass (when automatic draw is enabled)
    if (auto_draw_) {
        render_current_frame();
    }

    // Stage 15: Post-draw cleanup + advance animations & Animation End (ev 7, sub 7)
    remove_marked_instances();
    if (pending_action_ == PendingRoomAction::None) {
        step_animations();
    }
    if (pending_action_ != PendingRoomAction::None) {
        process_pending_room_action();
    }

    // Stage 16: Present + FPS measurement
    if (auto_draw_) {
        window_.swap_buffers();
    }

    ++fps_frame_accum_;
    const u64 now_us = Clock::now_us();
    if (now_us - fps_last_sample_us_ >= 1000000u) {
        fps_ = static_cast<i32>(fps_frame_accum_);
        fps_frame_accum_ = 0;
        fps_last_sample_us_ = now_us;
    }
}

void Runner::step_triggers(i32 moment) {
    for (usize tid = 0; tid < game_.triggers.size(); ++tid) {
        if (pending_action_ != PendingRoomAction::None) return;
        const auto& tr = game_.triggers[tid];
        if (!tr.exists || tr.moment != moment) continue;
        if (tid >= program_.trigger_chunks.size() || !program_.trigger_chunks[tid].has_value()) continue;
        const auto& cond_chunk = *program_.trigger_chunks[tid];
        if (cond_chunk.code.empty()) continue;

        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            if (pending_action_ != PendingRoomAction::None) return;
            auto* inst = find_instance(id);
            if (!inst || inst->destroyed || inst->deactivated) continue;
            if (!object_has_event(inst->object_index, 11, static_cast<i32>(tid))) continue;

            auto res = vm_->execute(cond_chunk, id, id);
            if (res && res.value().is_truthy()) {
                perform_event_on_instance(id, -1, 11, static_cast<i32>(tid), id);
            }
        }
    }
}

void Runner::step_timelines() {
    std::vector<i32> snap;
    snapshot_active_instances(snap, next_create_counter_);
    for (i32 id : snap) {
        if (pending_action_ != PendingRoomAction::None) return;
        auto* inst = find_instance(id);
        if (!inst || inst->destroyed || inst->deactivated) continue;
        if (!inst->timeline_running || inst->timeline_index < 0) continue;
        if (static_cast<usize>(inst->timeline_index) >= game_.timelines.size()) continue;
        const auto& tl = game_.timelines[static_cast<usize>(inst->timeline_index)];
        if (!tl.exists) continue;
        const auto& chunks = program_.timeline_chunks[static_cast<usize>(inst->timeline_index)];

        const f64 prev_pos = inst->timeline_position;
        const f64 next_pos = prev_pos + inst->timeline_speed;
        const f64 lo = std::min(prev_pos, next_pos);
        const f64 hi = std::max(prev_pos, next_pos);
        const bool forward = inst->timeline_speed >= 0.0;

        for (const auto& m : tl.moments) {
            const f64 ms = static_cast<f64>(m.step);
            const bool hit = forward ? (ms >= lo && ms < hi) : (ms > lo && ms <= hi);
            if (!hit) continue;
            if (auto it = chunks.find(m.step); it != chunks.end() && !it->second.code.empty()) {
                auto r = vm_->execute(it->second, id, id);
                if (!r) GM_LOG_WARN("runtime", "Timeline %d step %d error: %s", inst->timeline_index, m.step, r.error_message().c_str());
                if (pending_action_ != PendingRoomAction::None) return;
                inst = find_instance(id);
                if (!inst || inst->destroyed) break;
            }
        }
        if (inst && !inst->destroyed) {
            inst->timeline_position = next_pos;
            if (inst->timeline_loop && !tl.moments.empty()) {
                const i32 max_step = tl.moments.back().step;
                if (max_step > 0 && inst->timeline_position > static_cast<f64>(max_step)) {
                    inst->timeline_position = 0.0;
                }
            }
        }
    }
}

void Runner::step_alarms() {
    std::vector<i32> snap;
    snapshot_active_instances(snap, next_create_counter_);
    for (i32 id : snap) {
        if (pending_action_ != PendingRoomAction::None) return;
        auto* inst = find_instance(id);
        if (!inst || inst->destroyed || inst->deactivated) continue;

        for (i32 a = 0; a < 12; ++a) {
            if (!object_has_event(inst->object_index, 2, a)) continue;
            if (inst->alarm[static_cast<usize>(a)] >= 0) {
                --inst->alarm[static_cast<usize>(a)];
                if (inst->alarm[static_cast<usize>(a)] == 0) {
                    perform_event_on_instance(id, -1, 2, a, id);
                    if (pending_action_ != PendingRoomAction::None) return;
                    inst = find_instance(id);
                    if (!inst || inst->destroyed || inst->deactivated) break;
                }
            }
        }
    }
}

void Runner::step_keyboard_events() {
    const InputState& inp = window_.input();
    bool any_down = false;
    bool any_pressed = false;
    bool any_released = false;
    for (usize k = 2; k < 256; ++k) {
        if (inp.key_down[k]) any_down = true;
        if (inp.key_pressed[k]) any_pressed = true;
        if (inp.key_released[k]) any_released = true;
    }

    auto dispatch_key_cat = [&](i32 cat, const std::array<bool, 256>& arr, bool any_flag) {
        std::vector<i32> snap;
        snapshot_active_instances(snap, next_create_counter_);
        for (i32 id : snap) {
            if (pending_action_ != PendingRoomAction::None) return;
            auto* inst = find_instance(id);
            if (!inst || inst->destroyed || inst->deactivated) continue;

            // Walk object + ancestors to collect distinct key sub-IDs handled by this object.
            i32 cur_obj = inst->object_index;
            std::vector<i32> keys;
            for (int depth = 0; depth < 64 && object_exists(cur_obj); ++depth) {
                const auto& ev_map = program_.object_event_chunks[static_cast<usize>(cur_obj)][static_cast<usize>(cat)];
                for (const auto& [sub, _] : ev_map) {
                    if (std::find(keys.begin(), keys.end(), sub) == keys.end()) {
                        keys.push_back(sub);
                    }
                }
                cur_obj = game_.objects[static_cast<usize>(cur_obj)].parent_index;
            }
            for (i32 key : keys) {
                bool match = false;
                if (key == 0) { // vk_nokey
                    match = !any_flag;
                } else if (key == 1) { // vk_anykey
                    match = any_flag;
                } else if (key >= 2 && key < 256) {
                    match = arr[static_cast<usize>(key)];
                }
                if (match) {
                    perform_event_on_instance(id, -1, cat, key, id);
                    if (pending_action_ != PendingRoomAction::None) return;
                    inst = find_instance(id);
                    if (!inst || inst->destroyed || inst->deactivated) break;
                }
            }
        }
    };

    dispatch_key_cat(5, inp.key_down, any_down);         // ev_keyboard
    if (pending_action_ != PendingRoomAction::None) return;
    dispatch_key_cat(9, inp.key_pressed, any_pressed);   // ev_keypress
    if (pending_action_ != PendingRoomAction::None) return;
    dispatch_key_cat(10, inp.key_released, any_released); // ev_keyrelease
}

void Runner::step_mouse_events() {
    const InputState& inp = window_.input();
    std::vector<i32> snap;
    snapshot_active_instances(snap, next_create_counter_);

    for (i32 id : snap) {
        if (pending_action_ != PendingRoomAction::None) return;
        auto* inst = find_instance(id);
        if (!inst || inst->destroyed || inst->deactivated) continue;

        const bool over = check_point_collision(*inst, mouse_x_, mouse_y_, true, game_, version_mode());
        const bool entered = over && !inst->mouse_inside_prev;
        const bool left = !over && inst->mouse_inside_prev;
        inst->mouse_inside_prev = over;

        auto fire_if = [&](i32 sub, bool cond) {
            if (!cond || pending_action_ != PendingRoomAction::None) return;
            if (inst && !inst->destroyed && !inst->deactivated && object_has_event(inst->object_index, 6, sub)) {
                perform_event_on_instance(id, -1, 6, sub, id);
                inst = find_instance(id);
            }
        };

        // Local mouse events (only when cursor is over instance)
        if (over) {
            fire_if(0, inp.mouse_down[1]);
            fire_if(1, inp.mouse_down[2]);
            fire_if(2, inp.mouse_down[3]);
            fire_if(3, !inp.mouse_down[1] && !inp.mouse_down[2] && !inp.mouse_down[3]);
            fire_if(4, inp.mouse_pressed[1]);
            fire_if(5, inp.mouse_pressed[2]);
            fire_if(6, inp.mouse_pressed[3]);
            fire_if(7, inp.mouse_released[1]);
            fire_if(8, inp.mouse_released[2]);
            fire_if(9, inp.mouse_released[3]);
        }
        fire_if(10, entered); // ev_mouse_enter
        fire_if(11, left);    // ev_mouse_leave

        // Global mouse events
        fire_if(50, inp.mouse_down[1]);
        fire_if(51, inp.mouse_down[2]);
        fire_if(52, inp.mouse_down[3]);
        fire_if(53, inp.mouse_pressed[1]);
        fire_if(54, inp.mouse_pressed[2]);
        fire_if(55, inp.mouse_pressed[3]);
        fire_if(56, inp.mouse_released[1]);
        fire_if(57, inp.mouse_released[2]);
        fire_if(58, inp.mouse_released[3]);
        fire_if(60, inp.mouse_wheel_up);
        fire_if(61, inp.mouse_wheel_down);
    }
}

void Runner::step_physics_and_paths() {
    std::vector<i32> end_of_path_ids;

    for (auto& up : instances_) {
        if (up->destroyed || up->deactivated) continue;
        Instance& inst = *up;

        // 1. Friction
        if (inst.friction != 0.0 && inst.speed != 0.0) {
            if (inst.speed > 0.0) {
                inst.speed = std::max(0.0, inst.speed - inst.friction);
            } else {
                inst.speed = std::min(0.0, inst.speed + inst.friction);
            }
            inst.sync_hv_from_polar();
        }

        // 2. Gravity
        if (inst.gravity != 0.0) {
            const f64 grad = deg_to_rad(inst.gravity_direction);
            inst.hspeed += inst.gravity * std::cos(grad);
            inst.vspeed -= inst.gravity * std::sin(grad);
            inst.sync_polar_from_hv();
        }

        // 3. Motion integration
        if (inst.hspeed != 0.0 || inst.vspeed != 0.0) {
            inst.x += inst.hspeed;
            inst.y += inst.vspeed;
            inst.mark_bbox_dirty();
        }

        // 4. Path following
        if (inst.path_index >= 0 && static_cast<usize>(inst.path_index) < game_.paths.size()) {
            const auto& path = game_.paths[static_cast<usize>(inst.path_index)];
            if (path.exists && !path.points.empty() && inst.path_speed != 0.0) {
                const f64 len = compute_path_length(path);
                if (len > 1e-6) {
                    const f64 sp_step = inst.path_speed / (len * std::max(1e-6, std::fabs(inst.path_scale)));
                    inst.path_position += sp_step;
                    bool reached_end = false;
                    if (inst.path_position >= 1.0) {
                        reached_end = true;
                        switch (inst.path_endaction) {
                            case 0: // stop
                                inst.path_position = 1.0;
                                inst.path_index = -1;
                                inst.path_speed = 0.0;
                                break;
                            case 1: // restart
                                inst.path_position -= 1.0;
                                break;
                            case 2: // continue from current
                                inst.path_position -= 1.0;
                                inst.path_xstart = inst.x;
                                inst.path_ystart = inst.y;
                                break;
                            case 3: // reverse
                                inst.path_position = 2.0 - inst.path_position;
                                inst.path_speed = -inst.path_speed;
                                break;
                            default:
                                inst.path_position = 1.0;
                                inst.path_index = -1;
                                break;
                        }
                    } else if (inst.path_position <= 0.0) {
                        reached_end = true;
                        switch (inst.path_endaction) {
                            case 0:
                                inst.path_position = 0.0;
                                inst.path_index = -1;
                                inst.path_speed = 0.0;
                                break;
                            case 1:
                                inst.path_position += 1.0;
                                break;
                            case 2:
                                inst.path_position += 1.0;
                                inst.path_xstart = inst.x;
                                inst.path_ystart = inst.y;
                                break;
                            case 3:
                                inst.path_position = -inst.path_position;
                                inst.path_speed = -inst.path_speed;
                                break;
                            default:
                                inst.path_position = 0.0;
                                inst.path_index = -1;
                                break;
                        }
                    }

                    f64 px = 0.0, py = 0.0;
                    evaluate_path_point(path, std::clamp(inst.path_position, 0.0, 1.0), px, py);
                    if (inst.path_absolute) {
                        inst.x = px;
                        inst.y = py;
                    } else {
                        const f64 dx = (px - path.points[0].x) * inst.path_scale;
                        const f64 dy = (py - path.points[0].y) * inst.path_scale;
                        const f64 rad = deg_to_rad(inst.path_orientation);
                        const f64 c = std::cos(rad);
                        const f64 s = std::sin(rad);
                        inst.x = inst.path_xstart + dx * c + dy * s;
                        inst.y = inst.path_ystart - dx * s + dy * c;
                    }
                    inst.mark_bbox_dirty();
                    if (reached_end) {
                        end_of_path_ids.push_back(inst.id);
                    }
                }
            }
        }
    }

    for (i32 id : end_of_path_ids) {
        if (pending_action_ != PendingRoomAction::None) return;
        perform_event_on_instance(id, -1, 7, 8, id); // ev_other, ev_end_of_path
    }
}

void Runner::step_boundary_events() {
    std::vector<i32> snap;
    snapshot_active_instances(snap, next_create_counter_);

    for (i32 id : snap) {
        if (pending_action_ != PendingRoomAction::None) return;
        auto* inst = find_instance(id);
        if (!inst || inst->destroyed || inst->deactivated) continue;

        ensure_instance_bbox(*inst, game_);
        const bool outside = (inst->bbox_right < 0 || inst->bbox_left > room_width_ ||
                              inst->bbox_bottom < 0 || inst->bbox_top > room_height_);
        const bool intersect = !outside &&
                               (inst->bbox_left < 0 || inst->bbox_right > room_width_ ||
                                inst->bbox_top < 0 || inst->bbox_bottom > room_height_);

        if (outside) {
            perform_event_on_instance(id, -1, 7, 0, id); // ev_outside
            if (pending_action_ != PendingRoomAction::None) return;
            inst = find_instance(id);
            if (!inst || inst->destroyed || inst->deactivated) continue;
        }
        if (intersect) {
            perform_event_on_instance(id, -1, 7, 1, id); // ev_boundary
            if (pending_action_ != PendingRoomAction::None) return;
            inst = find_instance(id);
            if (!inst || inst->destroyed || inst->deactivated) continue;
        }

        // View boundary/outside events (40..47 outside view, 50..57 boundary view)
        if (view_enabled_) {
            for (i32 v = 0; v < 8; ++v) {
                const auto& vw = views_[static_cast<usize>(v)];
                if (!vw.visible) continue;
                const bool has_out = object_has_event(inst->object_index, 7, 40 + v);
                const bool has_bnd = object_has_event(inst->object_index, 7, 50 + v);
                if (!has_out && !has_bnd) continue;

                ensure_instance_bbox(*inst, game_);
                const bool v_out = (static_cast<f64>(inst->bbox_right) < vw.xview ||
                                    static_cast<f64>(inst->bbox_left) > vw.xview + vw.wview ||
                                    static_cast<f64>(inst->bbox_bottom) < vw.yview ||
                                    static_cast<f64>(inst->bbox_top) > vw.yview + vw.hview);
                const bool v_bnd = !v_out &&
                                   (static_cast<f64>(inst->bbox_left) < vw.xview ||
                                    static_cast<f64>(inst->bbox_right) > vw.xview + vw.wview ||
                                    static_cast<f64>(inst->bbox_top) < vw.yview ||
                                    static_cast<f64>(inst->bbox_bottom) > vw.yview + vw.hview);
                if (has_out && v_out) {
                    perform_event_on_instance(id, -1, 7, 40 + v, id);
                    if (pending_action_ != PendingRoomAction::None) return;
                    inst = find_instance(id);
                    if (!inst || inst->destroyed || inst->deactivated) break;
                }
                if (has_bnd && v_bnd) {
                    perform_event_on_instance(id, -1, 7, 50 + v, id);
                    if (pending_action_ != PendingRoomAction::None) return;
                    inst = find_instance(id);
                    if (!inst || inst->destroyed || inst->deactivated) break;
                }
            }
        }
    }
}

void Runner::step_collisions() {
    std::vector<i32> snap;
    snapshot_active_instances(snap, next_create_counter_);

    for (i32 id_a : snap) {
        if (pending_action_ != PendingRoomAction::None) return;
        auto* a = find_instance(id_a);
        if (!a || a->destroyed || a->deactivated) continue;

        // Collect all collision target object IDs defined on a's object or any ancestor.
        std::vector<i32> targets;
        i32 cur_obj = a->object_index;
        for (int d = 0; d < 64 && object_exists(cur_obj); ++d) {
            const auto& col_map = program_.object_event_chunks[static_cast<usize>(cur_obj)][4];
            for (const auto& [sub, _] : col_map) {
                if (std::find(targets.begin(), targets.end(), sub) == targets.end()) {
                    targets.push_back(sub);
                }
            }
            cur_obj = game_.objects[static_cast<usize>(cur_obj)].parent_index;
        }
        if (targets.empty()) continue;

        for (i32 target_obj : targets) {
            if (pending_action_ != PendingRoomAction::None) return;
            a = find_instance(id_a);
            if (!a || a->destroyed || a->deactivated) break;

            for (i32 id_b : snap) {
                if (id_b == id_a) continue;
                auto* b = find_instance(id_b);
                if (!b || b->destroyed || b->deactivated) continue;
                if (!object_matches(b->object_index, target_obj)) continue;

                a = find_instance(id_a);
                if (!a || a->destroyed || a->deactivated) break;

                if (!check_instance_collision(*a, a->x, a->y, *b, game_, version_mode())) {
                    continue;
                }

                const bool solid_pair = a->solid || b->solid;
                if (solid_pair) {
                    // Exact GM8 solid collision rollback -> event -> retry (hspeed, vspeed) -> rollback if still colliding.
                    a->x = a->xprevious;
                    a->y = a->yprevious;
                    a->mark_bbox_dirty();
                    const f64 bx_saved = b->x;
                    const f64 by_saved = b->y;
                    b->x = b->xprevious;
                    b->y = b->yprevious;
                    b->mark_bbox_dirty();

                    perform_event_on_instance(id_a, -1, 4, target_obj, id_b);
                    if (pending_action_ != PendingRoomAction::None) return;

                    a = find_instance(id_a);
                    b = find_instance(id_b);
                    if (b && !b->destroyed) {
                        // Restore b's position if b didn't get moved by the event.
                        if (b->x == b->xprevious && b->y == b->yprevious) {
                            b->x = bx_saved;
                            b->y = by_saved;
                            b->mark_bbox_dirty();
                        }
                    }
                    if (!a || a->destroyed || a->deactivated) break;

                    // Post-event slide retry along updated (hspeed, vspeed)
                    if (a->hspeed != 0.0 || a->vspeed != 0.0) {
                        const f64 pre_x = a->x;
                        const f64 pre_y = a->y;
                        const f64 try_x = pre_x + a->hspeed;
                        const f64 try_y = pre_y + a->vspeed;
                        const bool still_collides =
                            (b && !b->destroyed && !b->deactivated &&
                             check_instance_collision(*a, try_x, try_y, *b, game_, version_mode()));
                        if (!still_collides) {
                            a->x = try_x;
                            a->y = try_y;
                            a->mark_bbox_dirty();
                        }
                    }
                } else {
                    perform_event_on_instance(id_a, -1, 4, target_obj, id_b);
                    if (pending_action_ != PendingRoomAction::None) return;
                }
            }
        }
    }
}

void Runner::step_animations() {
    std::vector<i32> snap;
    snapshot_active_instances(snap, next_create_counter_);

    for (i32 id : snap) {
        if (pending_action_ != PendingRoomAction::None) return;
        auto* inst = find_instance(id);
        if (!inst || inst->destroyed || inst->deactivated) continue;
        if (inst->sprite_index < 0 || static_cast<usize>(inst->sprite_index) >= game_.sprites.size()) continue;
        const auto& sp = game_.sprites[static_cast<usize>(inst->sprite_index)];
        if (!sp.exists || sp.images.empty()) continue;

        const f64 count = static_cast<f64>(sp.images.size());
        const f64 prev_idx = inst->image_index;
        inst->image_index += inst->image_speed;

        bool ended = false;
        if (inst->image_index >= count) {
            inst->image_index = std::fmod(inst->image_index, count);
            if (inst->image_index < 0.0) inst->image_index += count;
            ended = true;
        } else if (inst->image_index < 0.0) {
            inst->image_index = std::fmod(inst->image_index, count);
            if (inst->image_index < 0.0) inst->image_index += count;
            ended = true;
        }

        if (sp.separate_masks && static_cast<i32>(std::floor(prev_idx)) != static_cast<i32>(std::floor(inst->image_index))) {
            inst->mark_bbox_dirty();
        }
        if (ended) {
            perform_event_on_instance(id, -1, 7, 7, id); // ev_other, ev_animation_end
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering & View / Mouse Projection
// ---------------------------------------------------------------------------

void Runner::compute_canvas_viewport(i32& out_vx, i32& out_vy, i32& out_vw, i32& out_vh, i32& out_cw, i32& out_ch) const noexcept {
    i32 cw = std::max(1, room_width_);
    i32 ch = std::max(1, room_height_);
    if (view_enabled_) {
        i32 max_r = 0;
        i32 max_b = 0;
        for (const auto& v : views_) {
            if (!v.visible) continue;
            max_r = std::max(max_r, v.xport + v.wport);
            max_b = std::max(max_b, v.yport + v.hport);
        }
        if (max_r > 0 && max_b > 0) {
            cw = max_r;
            ch = max_b;
        }
    }

    const i32 dw = std::max(1, window_.drawable_width());
    const i32 dh = std::max(1, window_.drawable_height());

    const f64 scale = std::min(static_cast<f64>(dw) / static_cast<f64>(cw),
                               static_cast<f64>(dh) / static_cast<f64>(ch));
    const i32 vw = std::max(1, static_cast<i32>(std::floor(static_cast<f64>(cw) * scale + 0.5)));
    const i32 vh = std::max(1, static_cast<i32>(std::floor(static_cast<f64>(ch) * scale + 0.5)));
    out_vx = (dw - vw) / 2;
    out_vy = (dh - vh) / 2;
    out_vw = vw;
    out_vh = vh;
    out_cw = cw;
    out_ch = ch;
}

void Runner::update_mouse_room_coords() {
    i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
    compute_canvas_viewport(vx, vy, vw, vh, cw, ch);

    const f64 dpi_x = static_cast<f64>(window_.drawable_width()) / static_cast<f64>(std::max(1, window_.width()));
    const f64 dpi_y = static_cast<f64>(window_.drawable_height()) / static_cast<f64>(std::max(1, window_.height()));
    const f64 mx_px = static_cast<f64>(window_.input().mouse_x) * dpi_x;
    const f64 my_px = static_cast<f64>(window_.input().mouse_y) * dpi_y;

    const f64 canvas_x = (mx_px - static_cast<f64>(vx)) * static_cast<f64>(cw) / static_cast<f64>(vw);
    const f64 canvas_y = (my_px - static_cast<f64>(vy)) * static_cast<f64>(ch) / static_cast<f64>(vh);

    if (!view_enabled_) {
        mouse_x_ = canvas_x;
        mouse_y_ = canvas_y;
        return;
    }

    // Find active view containing cursor (fallback to first visible view).
    i32 chosen = -1;
    i32 first_vis = -1;
    for (i32 i = 0; i < 8; ++i) {
        const auto& v = views_[static_cast<usize>(i)];
        if (!v.visible) continue;
        if (first_vis < 0) first_vis = i;
        if (canvas_x >= v.xport && canvas_x < v.xport + v.wport &&
            canvas_y >= v.yport && canvas_y < v.yport + v.hport) {
            chosen = i;
        }
    }
    if (chosen < 0) chosen = first_vis;
    if (chosen < 0) {
        mouse_x_ = canvas_x;
        mouse_y_ = canvas_y;
        return;
    }

    const auto& v = views_[static_cast<usize>(chosen)];
    const f64 pw = std::max(1.0, static_cast<f64>(v.wport));
    const f64 ph = std::max(1.0, static_cast<f64>(v.hport));
    mouse_x_ = v.xview + (canvas_x - static_cast<f64>(v.xport)) * v.wview / pw;
    mouse_y_ = v.yview + (canvas_y - static_cast<f64>(v.yport)) * v.hview / ph;
}

void Runner::update_view_following() {
    if (!view_enabled_) return;
    for (auto& v : views_) {
        if (!v.visible || v.follow_object < 0) continue;

        const Instance* target = nullptr;
        for (const auto& up : instances_) {
            if (!up->destroyed && !up->deactivated && object_matches(up->object_index, v.follow_object)) {
                target = up.get();
                break;
            }
        }
        if (!target) continue;

        const f64 hb = std::min(static_cast<f64>(v.hborder), v.wview * 0.5);
        const f64 vb = std::min(static_cast<f64>(v.vborder), v.hview * 0.5);
        f64 tx = v.xview;
        f64 ty = v.yview;
        if (target->x < v.xview + hb) tx = target->x - hb;
        else if (target->x > v.xview + v.wview - hb) tx = target->x + hb - v.wview;
        if (target->y < v.yview + vb) ty = target->y - vb;
        else if (target->y > v.yview + v.hview - vb) ty = target->y + vb - v.hview;

        tx = std::clamp(tx, 0.0, std::max(0.0, static_cast<f64>(room_width_) - v.wview));
        ty = std::clamp(ty, 0.0, std::max(0.0, static_cast<f64>(room_height_) - v.hview));

        if (v.hspeed >= 0) {
            const f64 dx = tx - v.xview;
            if (std::fabs(dx) > v.hspeed) tx = v.xview + (dx > 0.0 ? v.hspeed : -v.hspeed);
        }
        if (v.vspeed >= 0) {
            const f64 dy = ty - v.yview;
            if (std::fabs(dy) > v.vspeed) ty = v.yview + (dy > 0.0 ? v.vspeed : -v.vspeed);
        }
        v.xview = tx;
        v.yview = ty;
    }
}

void Runner::render_current_frame() {
    update_view_following();

    const i32 dw = std::max(1, window_.drawable_width());
    const i32 dh = std::max(1, window_.drawable_height());
    renderer_.begin_frame(dw, dh);

    // Clear entire backbuffer with window border color (letterbox bars).
    gfx_.set_scissor(false);
    gfx_.set_viewport(ViewportRect{0, 0, dw, dh});
    renderer_.clear_color(window_color_, 1.0f);

    i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
    compute_canvas_viewport(vx, vy, vw, vh, cw, ch);

    if (!view_enabled_) {
        view_current_ = 0;
        render_view(0,
                    0.0f, 0.0f, static_cast<f32>(room_width_), static_cast<f32>(room_height_), 0.0f,
                    vx, vy, vw, vh);
    } else {
        const f64 sx = static_cast<f64>(vw) / static_cast<f64>(cw);
        const f64 sy = static_cast<f64>(vh) / static_cast<f64>(ch);
        for (i32 vi = 0; vi < 8; ++vi) {
            const auto& v = views_[static_cast<usize>(vi)];
            if (!v.visible) continue;
            view_current_ = vi;
            const i32 px = vx + static_cast<i32>(std::floor(static_cast<f64>(v.xport) * sx + 0.5));
            const i32 py = vy + static_cast<i32>(std::floor(static_cast<f64>(v.yport) * sy + 0.5));
            const i32 pw = std::max(1, static_cast<i32>(std::floor(static_cast<f64>(v.wport) * sx + 0.5)));
            const i32 ph = std::max(1, static_cast<i32>(std::floor(static_cast<f64>(v.hport) * sy + 0.5)));
            render_view(vi,
                        static_cast<f32>(v.xview), static_cast<f32>(v.yview),
                        static_cast<f32>(v.wview), static_cast<f32>(v.hview),
                        static_cast<f32>(v.angle),
                        px, py, pw, ph);
        }
        view_current_ = 0;
    }

    gfx_.set_scissor(false);
    renderer_.end_frame();
}

void Runner::render_view(i32 /*view_idx*/, f32 vx, f32 vy, f32 vw, f32 vh, f32 angle, i32 px, i32 py, i32 pw, i32 ph) {
    const i32 dh = std::max(1, window_.drawable_height());
    const i32 gl_y = dh - (py + ph);
    gfx_.set_scissor(true, ViewportRect{px, gl_y, pw, ph});

    renderer_.set_projection_ortho(vx, vy, vw, vh, angle, px, py, pw, ph);
    if (bg_showcolor_) {
        renderer_.clear_color(bg_color_, 1.0f);
    }

    // 1. Non-foreground backgrounds (0..7)
    for (const auto& bg : backgrounds_) {
        if (!bg.visible || bg.foreground || bg.index < 0) continue;
        if (bg.stretch) {
            renderer_.draw_background_stretched_ext(
                game_, bg.index, 0.0f, 0.0f,
                static_cast<f32>(room_width_), static_cast<f32>(room_height_),
                bg.blend, static_cast<f32>(bg.alpha));
        } else if (bg.htiled || bg.vtiled) {
            renderer_.draw_background_tiled_ext(
                game_, bg.index,
                static_cast<f32>(bg.x), static_cast<f32>(bg.y),
                static_cast<f32>(bg.xscale), static_cast<f32>(bg.yscale),
                bg.blend, static_cast<f32>(bg.alpha),
                vx, vy, vw, vh, bg.htiled, bg.vtiled);
        } else {
            renderer_.draw_background_ext(
                game_, bg.index,
                static_cast<f32>(bg.x), static_cast<f32>(bg.y),
                static_cast<f32>(bg.xscale), static_cast<f32>(bg.yscale),
                0.0f, bg.blend, static_cast<f32>(bg.alpha));
        }
    }

    // 2. 3-way depth-descending render list: instances, tiles, auto-draw particle systems.
    // Higher depth draws FIRST (behind); lower depth draws LATER (in front).
    struct RenderItem {
        f64 depth = 0.0;
        u8 kind = 0; // 0 = tile, 1 = instance, 2 = particles
        usize order = 0;
        i32 ref_id = 0;
    };
    std::vector<RenderItem> items;
    items.reserve(instances_.size() + tiles_.size() + 8);

    for (usize i = 0; i < tiles_.size(); ++i) {
        if (!tiles_[i].visible) continue;
        items.push_back(RenderItem{tiles_[i].depth, 0, i, static_cast<i32>(i)});
    }
    for (usize i = 0; i < instances_.size(); ++i) {
        const auto& up = instances_[i];
        if (up->destroyed || up->deactivated || !up->visible) continue;
        items.push_back(RenderItem{up->depth, 1, i, up->id});
    }
    const auto ps_list = particles_.auto_draw_systems();
    for (usize i = 0; i < ps_list.size(); ++i) {
        items.push_back(RenderItem{ps_list[i].second, 2, i, ps_list[i].first});
    }

    std::stable_sort(items.begin(), items.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.order < b.order;
    });

    for (const auto& item : items) {
        if (item.kind == 0) {
            const auto& t = tiles_[static_cast<usize>(item.ref_id)];
            renderer_.draw_tile(
                game_, t.background_index,
                static_cast<f32>(t.xo), static_cast<f32>(t.yo),
                static_cast<f32>(t.w), static_cast<f32>(t.h),
                static_cast<f32>(t.x), static_cast<f32>(t.y),
                static_cast<f32>(t.xscale), static_cast<f32>(t.yscale),
                t.blend, static_cast<f32>(t.alpha));
        } else if (item.kind == 1) {
            const auto* inst = find_instance(item.ref_id);
            if (!inst || inst->destroyed || inst->deactivated || !inst->visible) continue;
            if (object_has_event(inst->object_index, 8, 0)) {
                perform_event_on_instance(inst->id, -1, 8, 0, inst->id);
                // Re-bind view projection in case a Draw event temporarily switched surface target.
                if (renderer_.current_surface_target() < 0) {
                    renderer_.set_projection_ortho(vx, vy, vw, vh, angle, px, py, pw, ph);
                    gfx_.set_scissor(true, ViewportRect{px, gl_y, pw, ph});
                }
            } else {
                draw_instance_default(*inst);
            }
        } else {
            particles_.draw_system(item.ref_id, renderer_, game_);
        }
    }

    // 3. Foreground backgrounds (0..7)
    for (const auto& bg : backgrounds_) {
        if (!bg.visible || !bg.foreground || bg.index < 0) continue;
        if (bg.stretch) {
            renderer_.draw_background_stretched_ext(
                game_, bg.index, 0.0f, 0.0f,
                static_cast<f32>(room_width_), static_cast<f32>(room_height_),
                bg.blend, static_cast<f32>(bg.alpha));
        } else if (bg.htiled || bg.vtiled) {
            renderer_.draw_background_tiled_ext(
                game_, bg.index,
                static_cast<f32>(bg.x), static_cast<f32>(bg.y),
                static_cast<f32>(bg.xscale), static_cast<f32>(bg.yscale),
                bg.blend, static_cast<f32>(bg.alpha),
                vx, vy, vw, vh, bg.htiled, bg.vtiled);
        } else {
            renderer_.draw_background_ext(
                game_, bg.index,
                static_cast<f32>(bg.x), static_cast<f32>(bg.y),
                static_cast<f32>(bg.xscale), static_cast<f32>(bg.yscale),
                0.0f, bg.blend, static_cast<f32>(bg.alpha));
        }
    }

    // 4. Custom cursor_sprite (if set)
    if (cursor_sprite_ >= 0) {
        renderer_.draw_sprite_ext(
            game_, cursor_sprite_, 0,
            static_cast<f32>(mouse_x_), static_cast<f32>(mouse_y_),
            1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
    }

    renderer_.flush();
}

void Runner::draw_instance_default(const Instance& inst) {
    if (inst.sprite_index < 0) return;
    const i32 subimg = static_cast<i32>(std::floor(inst.image_index));
    renderer_.draw_sprite_ext(
        game_, inst.sprite_index, subimg,
        static_cast<f32>(inst.x), static_cast<f32>(inst.y),
        static_cast<f32>(inst.image_xscale), static_cast<f32>(inst.image_yscale),
        static_cast<f32>(inst.image_angle),
        inst.image_blend, static_cast<f32>(inst.image_alpha));
}

void Runner::update_window_caption() {
    std::string cap = room_caption_;
    if (cap.empty() && room_exists(current_room_)) {
        cap = game_.rooms[static_cast<usize>(current_room_)].name;
    }
    if (show_score_ && score_ != 0.0) {
        cap += "  " + caption_score_ + std::to_string(delphi_round(score_));
    }
    if (show_lives_ && lives_ >= 0.0) {
        cap += "  " + caption_lives_ + std::to_string(delphi_round(lives_));
    }
    if (show_health_) {
        cap += "  " + caption_health_ + std::to_string(delphi_round(health_));
    }
    window_.set_title(cap);
}

// ---------------------------------------------------------------------------
// Instance, Object Hierarchy & Event Dispatch
// ---------------------------------------------------------------------------

Instance* Runner::find_instance(i32 id) noexcept {
    auto it = inst_by_id_.find(id);
    return it != inst_by_id_.end() ? it->second : nullptr;
}

const Instance* Runner::find_instance(i32 id) const noexcept {
    auto it = inst_by_id_.find(id);
    return it != inst_by_id_.end() ? it->second : nullptr;
}

bool Runner::object_exists(i32 obj_idx) const noexcept {
    return obj_idx >= 0 && static_cast<usize>(obj_idx) < game_.objects.size() &&
           game_.objects[static_cast<usize>(obj_idx)].exists;
}

bool Runner::object_is_ancestor(i32 child_obj, i32 parent_obj) const noexcept {
    if (!object_exists(child_obj) || !object_exists(parent_obj)) return false;
    i32 cur = game_.objects[static_cast<usize>(child_obj)].parent_index;
    for (int guard = 0; guard < 64 && object_exists(cur); ++guard) {
        if (cur == parent_obj) return true;
        cur = game_.objects[static_cast<usize>(cur)].parent_index;
    }
    return false;
}

bool Runner::object_matches(i32 inst_obj, i32 target) const noexcept {
    if (target == -3) return true; // all
    if (inst_obj == target) return true;
    return object_is_ancestor(inst_obj, target);
}

i32 Runner::create_instance(f64 x, f64 y, i32 obj_idx, bool run_create, i32 forced_id) {
    if (!object_exists(obj_idx)) {
        return -4; // noone
    }
    const auto& obj = game_.objects[static_cast<usize>(obj_idx)];
    auto inst = std::make_unique<Instance>();
    inst->id = (forced_id >= 100000) ? forced_id : next_instance_id_++;
    if (inst->id >= next_instance_id_) {
        next_instance_id_ = inst->id + 1;
    }
    inst->object_index = obj_idx;
    inst->x = x;
    inst->y = y;
    inst->xprevious = x;
    inst->yprevious = y;
    inst->xstart = x;
    inst->ystart = y;
    inst->solid = obj.solid;
    inst->visible = obj.visible;
    inst->persistent = obj.persistent;
    inst->depth = static_cast<f64>(obj.depth);
    inst->sprite_index = obj.sprite_index;
    inst->mask_index = obj.mask_index;
    inst->create_counter = next_create_counter_++;
    inst->mark_bbox_dirty();

    const i32 id = inst->id;
    inst_by_id_[id] = inst.get();
    instances_.push_back(std::move(inst));

    if (run_create) {
        perform_event_on_instance(id, -1, 0, 0, id); // ev_create
    }
    return id;
}

void Runner::destroy_instance(i32 inst_id, bool perform_event) {
    auto* inst = find_instance(inst_id);
    if (!inst || inst->destroyed) return;
    if (perform_event) {
        perform_event_on_instance(inst_id, -1, 1, 0, inst_id); // ev_destroy
    }
    inst = find_instance(inst_id);
    if (inst) {
        inst->destroyed = true;
    }
}

void Runner::change_instance(i32 inst_id, i32 new_obj_idx, bool perform_events) {
    auto* inst = find_instance(inst_id);
    if (!inst || inst->destroyed || !object_exists(new_obj_idx)) return;
    if (perform_events) {
        perform_event_on_instance(inst_id, -1, 1, 0, inst_id); // ev_destroy
        inst = find_instance(inst_id);
        if (!inst || inst->destroyed) return;
    }
    const auto& obj = game_.objects[static_cast<usize>(new_obj_idx)];
    inst->object_index = new_obj_idx;
    inst->sprite_index = obj.sprite_index;
    inst->solid = obj.solid;
    inst->visible = obj.visible;
    inst->persistent = obj.persistent;
    inst->depth = static_cast<f64>(obj.depth);
    inst->mask_index = obj.mask_index;
    inst->mark_bbox_dirty();

    if (perform_events) {
        perform_event_on_instance(inst_id, -1, 0, 0, inst_id); // ev_create
    }
}

i32 Runner::copy_instance(i32 src_id, bool perform_event) {
    const auto* src = find_instance(src_id);
    if (!src || src->destroyed) return -4;
    auto copy = std::make_unique<Instance>(*src);
    copy->id = next_instance_id_++;
    copy->create_counter = next_create_counter_++;
    copy->mark_bbox_dirty();

    const i32 new_id = copy->id;
    inst_by_id_[new_id] = copy.get();
    instances_.push_back(std::move(copy));

    if (perform_event) {
        perform_event_on_instance(new_id, -1, 0, 0, new_id);
    }
    return new_id;
}

void Runner::remove_marked_instances() {
    bool any = false;
    for (const auto& up : instances_) {
        if (up->destroyed) {
            any = true;
            break;
        }
    }
    if (!any) return;

    std::vector<std::unique_ptr<Instance>> kept;
    kept.reserve(instances_.size());
    for (auto& up : instances_) {
        if (up->destroyed) {
            inst_by_id_.erase(up->id);
        } else {
            kept.push_back(std::move(up));
        }
    }
    instances_ = std::move(kept);
}

void Runner::deactivate_all(bool notme, i32 self_id) {
    for (auto& up : instances_) {
        if (up->destroyed) continue;
        if (notme && up->id == self_id) continue;
        up->deactivated = true;
    }
}

void Runner::deactivate_object(i32 target, i32 /*self_id*/) {
    if (target >= 100000) {
        if (auto* inst = find_instance(target)) inst->deactivated = true;
        return;
    }
    for (auto& up : instances_) {
        if (up->destroyed) continue;
        if (object_matches(up->object_index, target)) up->deactivated = true;
    }
}

void Runner::deactivate_region(f64 left, f64 top, f64 w, f64 h, bool inside, bool notme, i32 self_id) {
    const f64 right = left + w;
    const f64 bottom = top + h;
    for (auto& up : instances_) {
        if (up->destroyed) continue;
        if (notme && up->id == self_id) continue;
        ensure_instance_bbox(*up, game_);
        const bool in_box = !(static_cast<f64>(up->bbox_right) < left ||
                              static_cast<f64>(up->bbox_left) > right ||
                              static_cast<f64>(up->bbox_bottom) < top ||
                              static_cast<f64>(up->bbox_top) > bottom);
        if (in_box == inside) {
            up->deactivated = true;
        }
    }
}

void Runner::activate_all() {
    for (auto& up : instances_) {
        if (!up->destroyed) up->deactivated = false;
    }
}

void Runner::activate_object(i32 target) {
    if (target >= 100000) {
        if (auto* inst = find_instance(target); inst && !inst->destroyed) inst->deactivated = false;
        return;
    }
    for (auto& up : instances_) {
        if (up->destroyed) continue;
        if (object_matches(up->object_index, target)) up->deactivated = false;
    }
}

void Runner::activate_region(f64 left, f64 top, f64 w, f64 h, bool inside) {
    const f64 right = left + w;
    const f64 bottom = top + h;
    for (auto& up : instances_) {
        if (up->destroyed || !up->deactivated) continue;
        ensure_instance_bbox(*up, game_);
        const bool in_box = !(static_cast<f64>(up->bbox_right) < left ||
                              static_cast<f64>(up->bbox_left) > right ||
                              static_cast<f64>(up->bbox_bottom) < top ||
                              static_cast<f64>(up->bbox_top) > bottom);
        if (in_box == inside) {
            up->deactivated = false;
        }
    }
}

void Runner::snapshot_active_instances(std::vector<i32>& out, u64 max_counter) const {
    out.clear();
    out.reserve(instances_.size());
    for (const auto& up : instances_) {
        if (!up->destroyed && !up->deactivated && up->create_counter < max_counter) {
            out.push_back(up->id);
        }
    }
}

const BytecodeChunk* Runner::resolve_event_chunk(i32 obj_idx, i32 cat, i32 sub, i32& out_owner_obj) const noexcept {
    if (cat < 0 || cat >= 12) return nullptr;
    i32 cur = obj_idx;
    for (int guard = 0; guard < 64 && object_exists(cur); ++guard) {
        if (static_cast<usize>(cur) < program_.object_event_chunks.size()) {
            const auto& ev_map = program_.object_event_chunks[static_cast<usize>(cur)][static_cast<usize>(cat)];
            if (auto it = ev_map.find(sub); it != ev_map.end()) {
                out_owner_obj = cur;
                return &it->second;
            }
        }
        cur = game_.objects[static_cast<usize>(cur)].parent_index;
    }
    return nullptr;
}

bool Runner::object_has_event(i32 obj_idx, i32 cat, i32 sub) const noexcept {
    i32 owner = -1;
    return resolve_event_chunk(obj_idx, cat, sub, owner) != nullptr;
}

void Runner::perform_event_on_instance(i32 inst_id, i32 obj_override, i32 cat, i32 sub, i32 other_id) {
    auto* inst = find_instance(inst_id);
    if (!inst || inst->destroyed) return;

    const i32 start_obj = (obj_override >= 0) ? obj_override : inst->object_index;
    i32 owner_obj = -1;
    const BytecodeChunk* chunk = resolve_event_chunk(start_obj, cat, sub, owner_obj);
    if (!chunk || chunk->code.empty()) return;

    const bool saved_rel = action_relative_;
    action_relative_ = false;
    event_stack_.push_back(EventFrame{cat, sub, owner_obj, inst_id, other_id, 0});

    auto res = vm_->execute(*chunk, inst_id, other_id);
    if (!res) {
        error_occurred_ = true;
        error_last_ = res.error_message();
        GM_LOG_WARN("runtime", "Event error (%s): %s", chunk->debug_name.c_str(), error_last_.c_str());
    }

    event_stack_.pop_back();
    action_relative_ = saved_rel;
}

void Runner::perform_event_inherited(i32 self_id) {
    if (event_stack_.empty()) return;
    const EventFrame cur = event_stack_.back();
    if (!object_exists(cur.owner_obj)) return;
    const i32 parent = game_.objects[static_cast<usize>(cur.owner_obj)].parent_index;
    if (!object_exists(parent)) return;

    perform_event_on_instance(self_id, parent, cur.cat, cur.sub, cur.other_id);
}

// ---------------------------------------------------------------------------
// Collision & Movement Helpers
// ---------------------------------------------------------------------------

i32 Runner::find_collision_instance(
    const Instance& self, f64 test_x, f64 test_y, i32 target, bool solid_only) const noexcept {
    if (target == -4) return -4; // noone
    if (target >= 100000) {
        const auto* other = find_instance(target);
        if (!other || other->destroyed || other->deactivated || other->id == self.id) return -4;
        if (solid_only && !other->solid) return -4;
        return check_instance_collision(self, test_x, test_y, *other, game_, version_mode()) ? other->id : -4;
    }
    for (const auto& up : instances_) {
        if (up->destroyed || up->deactivated || up->id == self.id) continue;
        if (solid_only && !up->solid) continue;
        if (!object_matches(up->object_index, target)) continue;
        if (check_instance_collision(self, test_x, test_y, *up, game_, version_mode())) {
            return up->id;
        }
    }
    return -4;
}

bool Runner::check_place_free(const Instance& self, f64 test_x, f64 test_y) const noexcept {
    return find_collision_instance(self, test_x, test_y, -3, /*solid_only=*/true) == -4;
}

bool Runner::check_place_empty(const Instance& self, f64 test_x, f64 test_y) const noexcept {
    return find_collision_instance(self, test_x, test_y, -3, /*solid_only=*/false) == -4;
}

bool Runner::check_place_meeting(const Instance& self, f64 test_x, f64 test_y, i32 target) const noexcept {
    return find_collision_instance(self, test_x, test_y, target, /*solid_only=*/false) != -4;
}

void Runner::move_contact(Instance& inst, f64 dir_deg, f64 max_dist, bool solid_only) {
    const f64 max_steps = (max_dist <= 0.0) ? 1000.0 : std::floor(max_dist);
    const f64 rad = deg_to_rad(dir_deg);
    const f64 dx = std::cos(rad);
    const f64 dy = -std::sin(rad);

    // If already colliding at current position, do not move.
    if (find_collision_instance(inst, inst.x, inst.y, -3, solid_only) != -4) {
        return;
    }

    const i32 steps = static_cast<i32>(max_steps);
    for (i32 s = 1; s <= steps; ++s) {
        const f64 nx = inst.x + dx;
        const f64 ny = inst.y + dy;
        if (find_collision_instance(inst, nx, ny, -3, solid_only) != -4) {
            return;
        }
        inst.x = nx;
        inst.y = ny;
        inst.mark_bbox_dirty();
    }
}

void Runner::move_outside(Instance& inst, f64 dir_deg, f64 max_dist, bool solid_only) {
    const f64 max_steps = (max_dist <= 0.0) ? 1000.0 : std::floor(max_dist);
    const f64 rad = deg_to_rad(dir_deg);
    const f64 dx = std::cos(rad);
    const f64 dy = -std::sin(rad);

    const i32 steps = static_cast<i32>(max_steps);
    for (i32 s = 0; s < steps; ++s) {
        if (find_collision_instance(inst, inst.x, inst.y, -3, solid_only) == -4) {
            return;
        }
        inst.x += dx;
        inst.y += dy;
        inst.mark_bbox_dirty();
    }
}

void Runner::move_bounce(Instance& inst, bool /*advanced*/, bool solid_only) {
    if (find_collision_instance(inst, inst.x + inst.hspeed, inst.y + inst.vspeed, -3, solid_only) == -4) {
        return;
    }
    const bool hit_h = find_collision_instance(inst, inst.x + inst.hspeed, inst.y, -3, solid_only) != -4;
    const bool hit_v = find_collision_instance(inst, inst.x, inst.y + inst.vspeed, -3, solid_only) != -4;
    if (hit_h) inst.hspeed = -inst.hspeed;
    if (hit_v) inst.vspeed = -inst.vspeed;
    if (!hit_h && !hit_v) {
        inst.hspeed = -inst.hspeed;
        inst.vspeed = -inst.vspeed;
    }
    inst.sync_polar_from_hv();
}

void Runner::start_path(Instance& inst, i32 path_id, f64 speed, i32 endaction, bool absolute) {
    if (path_id < 0 || static_cast<usize>(path_id) >= game_.paths.size() || !game_.paths[static_cast<usize>(path_id)].exists) {
        inst.path_index = -1;
        return;
    }
    const auto& p = game_.paths[static_cast<usize>(path_id)];
    inst.path_index = path_id;
    inst.path_speed = speed;
    inst.path_endaction = endaction;
    inst.path_absolute = absolute;
    inst.path_position = (speed >= 0.0) ? 0.0 : 1.0;
    inst.path_positionprevious = inst.path_position;
    inst.path_scale = 1.0;
    inst.path_orientation = 0.0;
    inst.path_xstart = inst.x;
    inst.path_ystart = inst.y;
    if (absolute && !p.points.empty()) {
        f64 px = 0.0, py = 0.0;
        evaluate_path_point(p, inst.path_position, px, py);
        inst.x = px;
        inst.y = py;
        inst.mark_bbox_dirty();
    }
}

void Runner::end_path(Instance& inst) {
    inst.path_index = -1;
    inst.path_speed = 0.0;
}

// ---------------------------------------------------------------------------
// IVmHost Implementation (All 166 Built-in Variables & Calls)
// ---------------------------------------------------------------------------

VariableTable* Runner::get_instance_vars(i32 inst_id) {
    auto* inst = find_instance(inst_id);
    return inst ? &inst->vars : nullptr;
}

void Runner::resolve_target_instances(i32 target_id, i32 self_id, i32 other_id, std::vector<i32>& out) {
    out.clear();
    if (target_id == -1) { // self
        if (auto* inst = find_instance(self_id); inst && !inst->destroyed) out.push_back(self_id);
        return;
    }
    if (target_id == -2) { // other
        if (auto* inst = find_instance(other_id); inst && !inst->destroyed) out.push_back(other_id);
        return;
    }
    if (target_id == -4) { // noone
        return;
    }
    if (target_id >= 100000) { // specific instance ID
        if (auto* inst = find_instance(target_id); inst && !inst->destroyed && !inst->deactivated) {
            out.push_back(target_id);
        }
        return;
    }
    // all (-3) or object_index (0..99999)
    for (const auto& up : instances_) {
        if (up->destroyed || up->deactivated) continue;
        if (target_id == -3 || object_matches(up->object_index, target_id)) {
            out.push_back(up->id);
        }
    }
}

Result<RValue> Runner::get_builtin_global(u32 var_id, i32 idx, bool has_idx) {
    const i32 i8 = std::clamp(has_idx ? idx : 0, 0, 7);
    switch (var_id) {
        case 0u: return RValue(action_relative_);
        case 1u: case 2u: case 3u: case 4u: case 5u: case 6u: case 7u: case 8u:
        case 9u: case 10u: case 11u: case 12u: case 13u: case 14u: case 15u: case 16u:
            return vm_->get_argument(static_cast<i32>(var_id - 1u));
        case 17u: return vm_->get_argument(has_idx ? idx : 0);
        case 18u: return RValue(vm_->argument_count());
        case 19u: return RValue(current_room_);
        case 20u: return RValue(game_.room_order.empty() ? -1 : game_.room_order.front());
        case 21u: return RValue(game_.room_order.empty() ? -1 : game_.room_order.back());
        case 22u: return RValue(transition_kind_);
        case 23u: return RValue(transition_steps_);
        case 24u: return RValue(score_);
        case 25u: return RValue(lives_);
        case 26u: return RValue(health_);
        case 27u: return RValue(game_.meta.game_id);
        case 28u: return RValue(vfs_.working_directory_gm());
        case 29u: return RValue(vfs_.temp_directory_gm());
        case 30u: return RValue(vfs_.working_directory_gm());
        case 31u: {
            i32 cnt = 0;
            for (const auto& up : instances_) {
                if (!up->destroyed && !up->deactivated) ++cnt;
            }
            return RValue(cnt);
        }
        case 32u: {
            const i32 target_i = has_idx ? idx : 0;
            i32 cur = 0;
            for (const auto& up : instances_) {
                if (up->destroyed || up->deactivated) continue;
                if (cur == target_i) return RValue(up->id);
                ++cur;
            }
            return RValue(-4);
        }
        case 33u: return RValue(room_width_);
        case 34u: return RValue(room_height_);
        case 35u: return RValue(room_caption_);
        case 36u: return RValue(room_speed_);
        case 37u: return RValue(room_persistent_);
        case 38u: return RValue(bg_color_);
        case 39u: return RValue(bg_showcolor_);
        case 40u: return RValue(backgrounds_[static_cast<usize>(i8)].visible);
        case 41u: return RValue(backgrounds_[static_cast<usize>(i8)].foreground);
        case 42u: return RValue(backgrounds_[static_cast<usize>(i8)].index);
        case 43u: return RValue(backgrounds_[static_cast<usize>(i8)].x);
        case 44u: return RValue(backgrounds_[static_cast<usize>(i8)].y);
        case 45u: {
            const i32 bid = backgrounds_[static_cast<usize>(i8)].index;
            if (bid >= 0 && static_cast<usize>(bid) < game_.backgrounds.size() && game_.backgrounds[static_cast<usize>(bid)].exists) {
                return RValue(game_.backgrounds[static_cast<usize>(bid)].width);
            }
            return RValue(0);
        }
        case 46u: {
            const i32 bid = backgrounds_[static_cast<usize>(i8)].index;
            if (bid >= 0 && static_cast<usize>(bid) < game_.backgrounds.size() && game_.backgrounds[static_cast<usize>(bid)].exists) {
                return RValue(game_.backgrounds[static_cast<usize>(bid)].height);
            }
            return RValue(0);
        }
        case 47u: return RValue(backgrounds_[static_cast<usize>(i8)].htiled);
        case 48u: return RValue(backgrounds_[static_cast<usize>(i8)].vtiled);
        case 49u: return RValue(backgrounds_[static_cast<usize>(i8)].xscale);
        case 50u: return RValue(backgrounds_[static_cast<usize>(i8)].yscale);
        case 51u: return RValue(backgrounds_[static_cast<usize>(i8)].hspeed);
        case 52u: return RValue(backgrounds_[static_cast<usize>(i8)].vspeed);
        case 53u: return RValue(backgrounds_[static_cast<usize>(i8)].blend);
        case 54u: return RValue(backgrounds_[static_cast<usize>(i8)].alpha);
        case 55u: return RValue(view_enabled_);
        case 56u: return RValue(view_current_);
        case 57u: return RValue(views_[static_cast<usize>(i8)].visible);
        case 58u: return RValue(views_[static_cast<usize>(i8)].xview);
        case 59u: return RValue(views_[static_cast<usize>(i8)].yview);
        case 60u: return RValue(views_[static_cast<usize>(i8)].wview);
        case 61u: return RValue(views_[static_cast<usize>(i8)].hview);
        case 62u: return RValue(views_[static_cast<usize>(i8)].xport);
        case 63u: return RValue(views_[static_cast<usize>(i8)].yport);
        case 64u: return RValue(views_[static_cast<usize>(i8)].wport);
        case 65u: return RValue(views_[static_cast<usize>(i8)].hport);
        case 66u: return RValue(views_[static_cast<usize>(i8)].angle);
        case 67u: return RValue(views_[static_cast<usize>(i8)].hborder);
        case 68u: return RValue(views_[static_cast<usize>(i8)].vborder);
        case 69u: return RValue(views_[static_cast<usize>(i8)].hspeed);
        case 70u: return RValue(views_[static_cast<usize>(i8)].vspeed);
        case 71u: return RValue(views_[static_cast<usize>(i8)].follow_object);
        case 72u: return RValue(mouse_x_);
        case 73u: return RValue(mouse_y_);
        case 74u: return RValue(mouse_button_);
        case 75u: return RValue(mouse_lastbutton_);
        case 76u: return RValue(static_cast<i32>(window_.input().current_key));
        case 77u: return RValue(static_cast<i32>(window_.input().last_key));
        case 78u: return RValue(window_.input().last_char);
        case 79u: return RValue(window_.input().keyboard_string);
        case 80u: return RValue(cursor_sprite_);
        case 81u: return RValue(show_score_);
        case 82u: return RValue(show_lives_);
        case 83u: return RValue(show_health_);
        case 84u: return RValue(caption_score_);
        case 85u: return RValue(caption_lives_);
        case 86u: return RValue(caption_health_);
        case 87u: return RValue(fps_);
        case 88u: return RValue(static_cast<f64>((Clock::now_us() - start_time_us_) / 1000u));
        case 89u: return RValue(local_time_now().tm_year + 1900);
        case 90u: return RValue(local_time_now().tm_mon + 1);
        case 91u: return RValue(local_time_now().tm_mday);
        case 92u: return RValue(local_time_now().tm_wday + 1); // GM8: 1=Sun..7=Sat
        case 93u: return RValue(local_time_now().tm_hour);
        case 94u: return RValue(local_time_now().tm_min);
        case 95u: return RValue(local_time_now().tm_sec);
        case 96u: return RValue(event_stack_.empty() ? 0 : event_stack_.back().cat);
        case 97u: return RValue(event_stack_.empty() ? 0 : event_stack_.back().sub);
        case 98u: return RValue(event_stack_.empty() ? -1 : event_stack_.back().owner_obj);
        case 99u: return RValue(event_stack_.empty() ? 0 : event_stack_.back().action_id);
        case 100u: return RValue(0);
        case 101u: return RValue(game_.meta.debug_mode);
        case 102u: return RValue(error_occurred_);
        case 103u: return RValue(error_last_);
        case 104u: return RValue(1);
        case 105u: return RValue(1);
        case 106u: return RValue(game_.meta.format_version);
        case 107u: return RValue(0); // os_windows
        case 108u: return RValue(0);
        case 109u: return RValue(-1); // os_browser = browser_not_a_browser
        case 110u: return RValue(0);
        case 111u: return RValue(window_.width());
        case 112u: return RValue(window_.height());
        case 113u: return RValue(0);
        case 114u: return RValue(-1);
        default:
            return RValue(0.0);
    }
}

Result<void> Runner::set_builtin_global(u32 var_id, i32 idx, bool has_idx, RValue value) {
    const i32 i8 = std::clamp(has_idx ? idx : 0, 0, 7);
    switch (var_id) {
        case 1u: case 2u: case 3u: case 4u: case 5u: case 6u: case 7u: case 8u:
        case 9u: case 10u: case 11u: case 12u: case 13u: case 14u: case 15u: case 16u:
            vm_->set_argument(static_cast<i32>(var_id - 1u), std::move(value));
            return Result<void>::success();
        case 17u:
            vm_->set_argument(has_idx ? idx : 0, std::move(value));
            return Result<void>::success();
        case 19u: request_room_goto(value.as_i32()); return Result<void>::success();
        case 22u: transition_kind_ = value.as_i32(); return Result<void>::success();
        case 23u: transition_steps_ = value.as_i32(); return Result<void>::success();
        case 24u:
            score_ = value.as_real();
            if (show_score_) update_window_caption();
            return Result<void>::success();
        case 25u: {
            const f64 prev = lives_;
            lives_ = value.as_real();
            if (lives_ > 0.0) no_more_lives_armed_ = true;
            if (show_lives_) update_window_caption();
            if (no_more_lives_armed_ && prev > 0.0 && lives_ <= 0.0) {
                no_more_lives_armed_ = false;
                std::vector<i32> snap;
                snapshot_active_instances(snap, next_create_counter_);
                for (i32 id : snap) {
                    if (pending_action_ != PendingRoomAction::None) break;
                    perform_event_on_instance(id, -1, 7, 6, id); // ev_no_more_lives
                }
            }
            return Result<void>::success();
        }
        case 26u: {
            const f64 prev = health_;
            health_ = value.as_real();
            if (health_ > 0.0) no_more_health_armed_ = true;
            if (show_health_) update_window_caption();
            if (no_more_health_armed_ && prev > 0.0 && health_ <= 0.0) {
                no_more_health_armed_ = false;
                std::vector<i32> snap;
                snapshot_active_instances(snap, next_create_counter_);
                for (i32 id : snap) {
                    if (pending_action_ != PendingRoomAction::None) break;
                    perform_event_on_instance(id, -1, 7, 9, id); // ev_no_more_health
                }
            }
            return Result<void>::success();
        }
        case 35u:
            room_caption_ = value.is_string() ? value.as_string() : "";
            update_window_caption();
            return Result<void>::success();
        case 36u: room_speed_ = std::max(1, value.as_i32()); return Result<void>::success();
        case 37u: room_persistent_ = value.is_truthy(); return Result<void>::success();
        case 38u: bg_color_ = static_cast<u32>(value.as_i64()) & 0x00FFFFFFu; return Result<void>::success();
        case 39u: bg_showcolor_ = value.is_truthy(); return Result<void>::success();
        case 40u: backgrounds_[static_cast<usize>(i8)].visible = value.is_truthy(); return Result<void>::success();
        case 41u: backgrounds_[static_cast<usize>(i8)].foreground = value.is_truthy(); return Result<void>::success();
        case 42u: backgrounds_[static_cast<usize>(i8)].index = value.as_i32(); return Result<void>::success();
        case 43u: backgrounds_[static_cast<usize>(i8)].x = value.as_real(); return Result<void>::success();
        case 44u: backgrounds_[static_cast<usize>(i8)].y = value.as_real(); return Result<void>::success();
        case 47u: backgrounds_[static_cast<usize>(i8)].htiled = value.is_truthy(); return Result<void>::success();
        case 48u: backgrounds_[static_cast<usize>(i8)].vtiled = value.is_truthy(); return Result<void>::success();
        case 49u: backgrounds_[static_cast<usize>(i8)].xscale = value.as_real(); return Result<void>::success();
        case 50u: backgrounds_[static_cast<usize>(i8)].yscale = value.as_real(); return Result<void>::success();
        case 51u: backgrounds_[static_cast<usize>(i8)].hspeed = value.as_real(); return Result<void>::success();
        case 52u: backgrounds_[static_cast<usize>(i8)].vspeed = value.as_real(); return Result<void>::success();
        case 53u: backgrounds_[static_cast<usize>(i8)].blend = static_cast<u32>(value.as_i64()) & 0x00FFFFFFu; return Result<void>::success();
        case 54u: backgrounds_[static_cast<usize>(i8)].alpha = value.as_real(); return Result<void>::success();
        case 55u: view_enabled_ = value.is_truthy(); return Result<void>::success();
        case 57u: views_[static_cast<usize>(i8)].visible = value.is_truthy(); return Result<void>::success();
        case 58u: views_[static_cast<usize>(i8)].xview = value.as_real(); return Result<void>::success();
        case 59u: views_[static_cast<usize>(i8)].yview = value.as_real(); return Result<void>::success();
        case 60u: views_[static_cast<usize>(i8)].wview = value.as_real(); return Result<void>::success();
        case 61u: views_[static_cast<usize>(i8)].hview = value.as_real(); return Result<void>::success();
        case 62u: views_[static_cast<usize>(i8)].xport = value.as_i32(); return Result<void>::success();
        case 63u: views_[static_cast<usize>(i8)].yport = value.as_i32(); return Result<void>::success();
        case 64u: views_[static_cast<usize>(i8)].wport = value.as_i32(); return Result<void>::success();
        case 65u: views_[static_cast<usize>(i8)].hport = value.as_i32(); return Result<void>::success();
        case 66u: views_[static_cast<usize>(i8)].angle = value.as_real(); return Result<void>::success();
        case 67u: views_[static_cast<usize>(i8)].hborder = value.as_i32(); return Result<void>::success();
        case 68u: views_[static_cast<usize>(i8)].vborder = value.as_i32(); return Result<void>::success();
        case 69u: views_[static_cast<usize>(i8)].hspeed = value.as_i32(); return Result<void>::success();
        case 70u: views_[static_cast<usize>(i8)].vspeed = value.as_i32(); return Result<void>::success();
        case 71u: views_[static_cast<usize>(i8)].follow_object = value.as_i32(); return Result<void>::success();
        case 74u: mouse_button_ = value.as_i32(); return Result<void>::success();
        case 75u: mouse_lastbutton_ = value.as_i32(); return Result<void>::success();
        case 76u: window_.input_mut().current_key = static_cast<u8>(value.as_i32() & 0xFF); return Result<void>::success();
        case 77u: window_.input_mut().last_key = static_cast<u8>(value.as_i32() & 0xFF); return Result<void>::success();
        case 78u: window_.input_mut().last_char = value.is_string() ? value.as_string() : ""; return Result<void>::success();
        case 79u: window_.input_mut().keyboard_string = value.is_string() ? value.as_string() : ""; return Result<void>::success();
        case 80u: cursor_sprite_ = value.as_i32(); return Result<void>::success();
        case 81u: show_score_ = value.is_truthy(); update_window_caption(); return Result<void>::success();
        case 82u: show_lives_ = value.is_truthy(); update_window_caption(); return Result<void>::success();
        case 83u: show_health_ = value.is_truthy(); update_window_caption(); return Result<void>::success();
        case 84u: caption_score_ = value.is_string() ? value.as_string() : ""; return Result<void>::success();
        case 85u: caption_lives_ = value.is_string() ? value.as_string() : ""; return Result<void>::success();
        case 86u: caption_health_ = value.is_string() ? value.as_string() : ""; return Result<void>::success();
        case 102u: error_occurred_ = value.is_truthy(); return Result<void>::success();
        case 103u: error_last_ = value.is_string() ? value.as_string() : ""; return Result<void>::success();
        default:
            return Result<void>::success();
    }
}

Result<RValue> Runner::get_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx) {
    const auto* inst = find_instance(inst_id);
    if (!inst) return RValue(0.0);

    const SpriteData* sp = (inst->sprite_index >= 0 && static_cast<usize>(inst->sprite_index) < game_.sprites.size() &&
                            game_.sprites[static_cast<usize>(inst->sprite_index)].exists)
                               ? &game_.sprites[static_cast<usize>(inst->sprite_index)]
                               : nullptr;

    switch (var_id) {
        case 115u: return RValue(inst->x);
        case 116u: return RValue(inst->y);
        case 117u: return RValue(inst->xprevious);
        case 118u: return RValue(inst->yprevious);
        case 119u: return RValue(inst->xstart);
        case 120u: return RValue(inst->ystart);
        case 121u: return RValue(inst->hspeed);
        case 122u: return RValue(inst->vspeed);
        case 123u: return RValue(inst->direction);
        case 124u: return RValue(inst->speed);
        case 125u: return RValue(inst->friction);
        case 126u: return RValue(inst->gravity);
        case 127u: return RValue(inst->gravity_direction);
        case 128u: return RValue(inst->object_index);
        case 129u: return RValue(inst->id);
        case 130u: {
            const i32 a = std::clamp(has_idx ? idx : 0, 0, 11);
            return RValue(inst->alarm[static_cast<usize>(a)]);
        }
        case 131u: return RValue(inst->solid);
        case 132u: return RValue(inst->visible);
        case 133u: return RValue(inst->persistent);
        case 134u: return RValue(inst->depth);
        case 135u: ensure_instance_bbox(*inst, game_); return RValue(inst->bbox_left);
        case 136u: ensure_instance_bbox(*inst, game_); return RValue(inst->bbox_right);
        case 137u: ensure_instance_bbox(*inst, game_); return RValue(inst->bbox_top);
        case 138u: ensure_instance_bbox(*inst, game_); return RValue(inst->bbox_bottom);
        case 139u: return RValue(inst->sprite_index);
        case 140u: return RValue(inst->image_index);
        case 141u: return RValue(inst->image_speed == 0.0 ? inst->image_index : -1.0);
        case 142u: return RValue(sp ? static_cast<i32>(sp->images.size()) : 0);
        case 143u: return RValue(sp ? static_cast<f64>(sp->width) * inst->image_xscale : 0.0);
        case 144u: return RValue(sp ? static_cast<f64>(sp->height) * inst->image_yscale : 0.0);
        case 145u: return RValue(sp ? static_cast<f64>(sp->origin_x) * inst->image_xscale : 0.0);
        case 146u: return RValue(sp ? static_cast<f64>(sp->origin_y) * inst->image_yscale : 0.0);
        case 147u: return RValue(inst->image_xscale);
        case 148u: return RValue(inst->image_yscale);
        case 149u: return RValue(inst->image_angle);
        case 150u: return RValue(inst->image_alpha);
        case 151u: return RValue(inst->image_blend);
        case 152u: return RValue(inst->image_speed);
        case 153u: return RValue(inst->mask_index);
        case 154u: return RValue(inst->path_index);
        case 155u: return RValue(inst->path_position);
        case 156u: return RValue(inst->path_positionprevious);
        case 157u: return RValue(inst->path_speed);
        case 158u: return RValue(inst->path_scale);
        case 159u: return RValue(inst->path_orientation);
        case 160u: return RValue(inst->path_endaction);
        case 161u: return RValue(inst->timeline_index);
        case 162u: return RValue(inst->timeline_position);
        case 163u: return RValue(inst->timeline_speed);
        case 164u: return RValue(inst->timeline_running);
        case 165u: return RValue(inst->timeline_loop);
        default:
            return RValue(0.0);
    }
}

Result<void> Runner::set_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx, RValue value) {
    auto* inst = find_instance(inst_id);
    if (!inst) return Result<void>::success();

    switch (var_id) {
        case 115u: inst->x = value.as_real(); inst->mark_bbox_dirty(); break;
        case 116u: inst->y = value.as_real(); inst->mark_bbox_dirty(); break;
        case 117u: inst->xprevious = value.as_real(); break;
        case 118u: inst->yprevious = value.as_real(); break;
        case 119u: inst->xstart = value.as_real(); break;
        case 120u: inst->ystart = value.as_real(); break;
        case 121u: inst->hspeed = value.as_real(); inst->sync_polar_from_hv(); break;
        case 122u: inst->vspeed = value.as_real(); inst->sync_polar_from_hv(); break;
        case 123u: {
            f64 d = std::fmod(value.as_real(), 360.0);
            if (d < 0.0) d += 360.0;
            inst->direction = d;
            inst->sync_hv_from_polar();
            break;
        }
        case 124u: inst->speed = value.as_real(); inst->sync_hv_from_polar(); break;
        case 125u: inst->friction = value.as_real(); break;
        case 126u: inst->gravity = value.as_real(); break;
        case 127u: inst->gravity_direction = value.as_real(); break;
        case 130u: {
            const i32 a = std::clamp(has_idx ? idx : 0, 0, 11);
            inst->alarm[static_cast<usize>(a)] = value.as_i32();
            break;
        }
        case 131u: inst->solid = value.is_truthy(); break;
        case 132u: inst->visible = value.is_truthy(); break;
        case 133u: inst->persistent = value.is_truthy(); break;
        case 134u: inst->depth = value.as_real(); break;
        case 139u: inst->sprite_index = value.as_i32(); inst->mark_bbox_dirty(); break;
        case 140u: inst->image_index = value.as_real(); inst->mark_bbox_dirty(); break;
        case 141u:
            if (value.as_real() >= 0.0) {
                inst->image_index = value.as_real();
                inst->image_speed = 0.0;
            } else {
                inst->image_speed = 1.0;
            }
            inst->mark_bbox_dirty();
            break;
        case 147u: inst->image_xscale = value.as_real(); inst->mark_bbox_dirty(); break;
        case 148u: inst->image_yscale = value.as_real(); inst->mark_bbox_dirty(); break;
        case 149u: inst->image_angle = value.as_real(); inst->mark_bbox_dirty(); break;
        case 150u: inst->image_alpha = value.as_real(); break;
        case 151u: inst->image_blend = static_cast<u32>(value.as_i64()) & 0x00FFFFFFu; break;
        case 152u: inst->image_speed = value.as_real(); break;
        case 153u: inst->mask_index = value.as_i32(); inst->mark_bbox_dirty(); break;
        case 155u: inst->path_position = value.as_real(); break;
        case 156u: inst->path_positionprevious = value.as_real(); break;
        case 157u: inst->path_speed = value.as_real(); break;
        case 158u: inst->path_scale = value.as_real(); break;
        case 159u: inst->path_orientation = value.as_real(); break;
        case 160u: inst->path_endaction = value.as_i32(); break;
        case 161u: inst->timeline_index = value.as_i32(); break;
        case 162u: inst->timeline_position = value.as_real(); break;
        case 163u: inst->timeline_speed = value.as_real(); break;
        case 164u: inst->timeline_running = value.is_truthy(); break;
        case 165u: inst->timeline_loop = value.is_truthy(); break;
        default:
            break;
    }
    return Result<void>::success();
}

Result<RValue> Runner::call_builtin(u32 func_id, i32 self_id, i32 other_id, std::span<const RValue> args) {
    if (func_id < builtins_.size() && builtins_[func_id] != nullptr) {
        return builtins_[func_id](*this, self_id, other_id, args);
    }
    if (func_id < warned_unimpl_.size() && !warned_unimpl_[func_id]) {
        warned_unimpl_[func_id] = true;
        const auto* info = BuiltinsCatalog::function_by_id(func_id);
        GM_LOG_WARN("runtime", "Unimplemented built-in function: %s (id=%u)",
                    info ? std::string(info->name).c_str() : "?", func_id);
    }
    return RValue(0.0);
}

Result<RValue> Runner::call_extension_dll(u32 ext_dll_id, std::span<const RValue> /*args*/) {
    if (ext_dll_id < program_.symbols.extension_dll_functions().size()) {
        const auto& fn = program_.symbols.extension_dll_functions()[ext_dll_id];
        GM_LOG_WARN("ext", "Unimplemented extension DLL call: %s -> %s (Phase 4/5)",
                    fn.dll_filename.c_str(), fn.external_name.c_str());
    }
    return RValue(0.0);
}

// ---------------------------------------------------------------------------
// RNG Helpers
// ---------------------------------------------------------------------------

void Runner::set_random_seed(u32 seed) noexcept {
    rng_seed_ = seed;
    rng_.seed(seed);
}

f64 Runner::random_uniform(f64 upper) noexcept {
    std::uniform_real_distribution<f64> dist(0.0, 1.0);
    return dist(rng_) * upper;
}

i64 Runner::random_int_inclusive(i64 upper) noexcept {
    if (upper < 0) {
        std::uniform_int_distribution<i64> dist(upper, 0);
        return dist(rng_);
    }
    std::uniform_int_distribution<i64> dist(0, upper);
    return dist(rng_);
}

// ---------------------------------------------------------------------------
// Text File, Binary File, INI & File Find Managers
// ---------------------------------------------------------------------------

i32 Runner::text_file_open(std::string_view gm_path, i32 mode) {
    TextFileHandle tf{};
    tf.open = true;
    tf.mode = mode;
    tf.gm_path = std::string(gm_path);

    if (mode == 0) { // read
        auto bytes = vfs_.read_all_bytes(gm_path);
        if (!bytes) return -1;
        std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        usize pos = 0;
        while (pos <= text.size()) {
            usize nl = text.find('\n', pos);
            if (nl == std::string::npos) {
                std::string line = text.substr(pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                tf.lines.push_back(std::move(line));
                break;
            }
            std::string line = text.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            tf.lines.push_back(std::move(line));
            pos = nl + 1;
            if (pos == text.size()) break;
        }
    } else if (mode == 2) { // append
        if (auto bytes = vfs_.read_all_bytes(gm_path); bytes) {
            tf.write_buf.assign(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        }
    }

    const i32 id = next_text_file_id_++;
    text_files_[id] = std::move(tf);
    return id;
}

TextFileHandle* Runner::get_text_file(i32 handle) {
    auto it = text_files_.find(handle);
    return (it != text_files_.end() && it->second.open) ? &it->second : nullptr;
}

void Runner::text_file_close(i32 handle) {
    auto it = text_files_.find(handle);
    if (it == text_files_.end()) return;
    if (it->second.open && it->second.mode != 0) {
        (void)vfs_.write_all_bytes(
            it->second.gm_path,
            std::span<const u8>(reinterpret_cast<const u8*>(it->second.write_buf.data()),
                                it->second.write_buf.size()));
    }
    text_files_.erase(it);
}

i32 Runner::bin_file_open(std::string_view gm_path, i32 mode) {
    BinFileHandle bf{};
    bf.open = true;
    bf.mode = mode;
    bf.gm_path = std::string(gm_path);

    if (mode == 0 || mode == 2) {
        if (auto bytes = vfs_.read_all_bytes(gm_path); bytes) {
            bf.data = std::move(bytes.value());
        } else if (mode == 0) {
            return -1;
        }
    } else {
        bf.dirty = true;
    }

    const i32 id = next_bin_file_id_++;
    bin_files_[id] = std::move(bf);
    return id;
}

BinFileHandle* Runner::get_bin_file(i32 handle) {
    auto it = bin_files_.find(handle);
    return (it != bin_files_.end() && it->second.open) ? &it->second : nullptr;
}

void Runner::bin_file_close(i32 handle) {
    auto it = bin_files_.find(handle);
    if (it == bin_files_.end()) return;
    if (it->second.open && it->second.dirty) {
        (void)vfs_.write_all_bytes(it->second.gm_path, it->second.data);
    }
    bin_files_.erase(it);
}

void Runner::parse_ini_text(std::string_view text) {
    ini_.sections.clear();
    std::string cur_sec_lower;
    usize pos = 0;
    while (pos < text.size()) {
        usize nl = text.find('\n', pos);
        const std::string_view raw_line =
            (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;

        const std::string line = trim_ascii(raw_line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']' && line.size() >= 2) {
            std::string sec_name = trim_ascii(std::string_view(line).substr(1, line.size() - 2));
            cur_sec_lower = ascii_to_lower(sec_name);
            auto it = std::find_if(ini_.sections.begin(), ini_.sections.end(),
                                   [&](const auto& p) { return p.first == cur_sec_lower; });
            if (it == ini_.sections.end()) {
                ini_.sections.push_back({cur_sec_lower, IniState::Section{sec_name, {}}});
            }
            continue;
        }
        const usize eq = line.find('=');
        if (eq == std::string::npos || cur_sec_lower.empty()) continue;
        std::string key = trim_ascii(std::string_view(line).substr(0, eq));
        std::string val = trim_ascii(std::string_view(line).substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        ini_write_string(cur_sec_lower, key, val);
    }
    ini_.dirty = false;
}

std::string Runner::serialize_ini() const {
    std::string out;
    for (const auto& [sec_l, sec] : ini_.sections) {
        out += "[" + sec.name_orig + "]\r\n";
        for (const auto& e : sec.entries) {
            out += e.key_orig + "=" + e.value + "\r\n";
        }
    }
    return out;
}

void Runner::ini_open(std::string_view gm_path) {
    if (ini_.open) (void)ini_close();
    ini_ = IniState{};
    ini_.open = true;
    ini_.gm_path = std::string(gm_path);
    if (auto bytes = vfs_.read_all_bytes(gm_path); bytes) {
        parse_ini_text(std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
    }
}

void Runner::ini_open_from_string(std::string_view content) {
    if (ini_.open) (void)ini_close();
    ini_ = IniState{};
    ini_.open = true;
    parse_ini_text(content);
}

std::string Runner::ini_close() {
    if (!ini_.open) return {};
    std::string out = serialize_ini();
    if (ini_.dirty && !ini_.gm_path.empty()) {
        (void)vfs_.write_all_bytes(
            ini_.gm_path,
            std::span<const u8>(reinterpret_cast<const u8*>(out.data()), out.size()));
    }
    ini_ = IniState{};
    return out;
}

std::string Runner::ini_read_string(std::string_view sec, std::string_view key, std::string_view def) const {
    const std::string sl = ascii_to_lower(sec);
    const std::string kl = ascii_to_lower(key);
    for (const auto& [sn, s] : ini_.sections) {
        if (sn == sl) {
            for (const auto& e : s.entries) {
                if (e.key_lower == kl) return e.value;
            }
        }
    }
    return std::string(def);
}

f64 Runner::ini_read_real(std::string_view sec, std::string_view key, f64 def) const {
    const std::string s = ini_read_string(sec, key, "");
    if (s.empty()) return def;
    try {
        return std::stod(s);
    } catch (...) {
        return def;
    }
}

void Runner::ini_write_string(std::string_view sec, std::string_view key, std::string_view val) {
    const std::string sl = ascii_to_lower(sec);
    const std::string kl = ascii_to_lower(key);
    auto it = std::find_if(ini_.sections.begin(), ini_.sections.end(),
                           [&](const auto& p) { return p.first == sl; });
    if (it == ini_.sections.end()) {
        ini_.sections.push_back({sl, IniState::Section{std::string(sec), {}}});
        it = std::prev(ini_.sections.end());
    }
    for (auto& e : it->second.entries) {
        if (e.key_lower == kl) {
            e.value = std::string(val);
            ini_.dirty = true;
            return;
        }
    }
    it->second.entries.push_back(IniState::Entry{kl, std::string(key), std::string(val)});
    ini_.dirty = true;
}

void Runner::ini_write_real(std::string_view sec, std::string_view key, f64 val) {
    std::ostringstream oss;
    oss << val;
    ini_write_string(sec, key, oss.str());
}

bool Runner::ini_key_exists(std::string_view sec, std::string_view key) const {
    const std::string sl = ascii_to_lower(sec);
    const std::string kl = ascii_to_lower(key);
    for (const auto& [sn, s] : ini_.sections) {
        if (sn == sl) {
            for (const auto& e : s.entries) {
                if (e.key_lower == kl) return true;
            }
        }
    }
    return false;
}

bool Runner::ini_section_exists(std::string_view sec) const {
    const std::string sl = ascii_to_lower(sec);
    for (const auto& [sn, _] : ini_.sections) {
        if (sn == sl) return true;
    }
    return false;
}

void Runner::ini_key_delete(std::string_view sec, std::string_view key) {
    const std::string sl = ascii_to_lower(sec);
    const std::string kl = ascii_to_lower(key);
    for (auto& [sn, s] : ini_.sections) {
        if (sn == sl) {
            auto& vec = s.entries;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&](const auto& e) { return e.key_lower == kl; }),
                      vec.end());
            ini_.dirty = true;
            return;
        }
    }
}

void Runner::ini_section_delete(std::string_view sec) {
    const std::string sl = ascii_to_lower(sec);
    ini_.sections.erase(std::remove_if(ini_.sections.begin(), ini_.sections.end(),
                                       [&](const auto& p) { return p.first == sl; }),
                        ini_.sections.end());
    ini_.dirty = true;
}

std::string Runner::file_find_first(std::string_view mask, u32 attr) {
    file_find_.results = vfs_.find_files(mask, attr);
    file_find_.index = 0;
    file_find_.active = true;
    if (file_find_.results.empty()) return {};
    return file_find_.results[file_find_.index++];
}

std::string Runner::file_find_next() {
    if (!file_find_.active || file_find_.index >= file_find_.results.size()) return {};
    return file_find_.results[file_find_.index++];
}

void Runner::file_find_close() {
    file_find_.results.clear();
    file_find_.index = 0;
    file_find_.active = false;
}

} // namespace gm
