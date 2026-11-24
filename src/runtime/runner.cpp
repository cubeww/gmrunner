#include "runtime/runner.h"

#include "base/encoding.h"
#include "base/log.h"
#include "base/math.h"
#include "gml/builtins_catalog.h"
#include "platform/clock.h"
#include "platform/web_bridge.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <sstream>

namespace gm {

namespace {

i32 scaled_extent(i32 extent, f64 scale) noexcept {
    return static_cast<i32>(std::clamp<i64>(delphi_round(extent * scale), 1, std::numeric_limits<i32>::max()));
}

const SpriteData* instance_sprite(const Instance& inst, const GameData& game) noexcept {
    if (inst.sprite_index < 0 || static_cast<usize>(inst.sprite_index) >= game.sprites.size()) return nullptr;
    const auto& sprite = game.sprites[static_cast<usize>(inst.sprite_index)];
    return sprite.exists ? &sprite : nullptr;
}

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
    ds_.set_string_encoding(game_.meta.encoding);
    program_ = std::move(program);
    // Timeline wrappers can gain actions during play. Keep a copied program independent.
    for (auto& moments : program_.timeline_chunks) {
        for (auto& [step, event] : moments) {
            if (event.use_count() > 1) event = std::make_shared<BytecodeChunk>(*event);
        }
    }
    use_precomputed_events_ = true;
    generated_event_tables_.reset();
    cmd_args_ = std::move(cmd_args);
    const u32 game_codepage = game_.meta.string_codepage();
    vfs_.set_game_path_codepage(game_codepage);
    vfs_.registry_init(game_.meta.game_id);
    if (game_codepage != 65001u) {
        for (auto& arg : cmd_args_) arg = utf8_to_codepage(arg, game_codepage);
    }

    next_instance_id_ = std::max(100001, game_.room_max_id + 1);
    next_tile_id_ = std::max(10000001, game_.room_max_tile_id + 1);
    window_color_ = game_.options.window_color;
    window_cursor_ = game_.options.show_cursor ? 0 : -1;
    window_.set_cursor(window_cursor_);
    window_.set_show_icons(!game_.options.no_buttons);
    window_region_scale_ = game_.options.scale / 100.0;
    sync_vsync_ = (game_.options.sync_vertex != 0);

    // Initialize renderer and upload all embedded sprites/backgrounds/fonts.
    if (auto res = renderer_.init(gfx_); !res) {
        return res;
    }
    renderer_.set_interpolation(game_.options.interpolate_pixels);
    renderer_.upload_game_assets(game_);
    if (auto res = touch_overlay_.init(gfx_); !res) {
        GM_LOG_WARN("gfx", "Touch overlay init failed: %s", res.error_message().c_str());
    }
    if (auto res = message_dialog_.init(gfx_, game_); !res) {
        GM_LOG_WARN("gfx", "Message dialog init failed: %s", res.error_message().c_str());
    }
    if (auto res = modal_dialogs_.init(gfx_); !res) {
        GM_LOG_WARN("gfx", "Modal dialogs init failed: %s", res.error_message().c_str());
    }
    if (PlatformWindow::is_mobile_touch_platform()) {
        touch_overlay_.set_enabled(true);
    }

    // Initialize audio engine (non-fatal if no audio device is available, e.g., headless).
    if (auto res = audio_.init(); !res) {
        GM_LOG_WARN("audio", "Audio initialization failed (%s); continuing muted", res.error_message().c_str());
    }
    audio_.load_game_sounds(game_);

    // Register compiled native cross-platform extensions and pre-bind GEX DLL exports.
    extensions_.reset(this);
    extensions_.register_compiled_extensions();
    extensions_.bind_gex_functions(program_.symbols.extension_dll_functions());

    // Export included files marked export_action == 1 (temp_dir) or 2 (working_dir).
    for (const auto& inc : game_.included_files) {
        if (!inc.exists || !inc.store || inc.data.empty()) continue;
        if (inc.export_action == 1) {
            const std::string p = vfs_.temp_directory_gm() + "/" + inc.filename;
            if (inc.overwrite || !vfs_.exists(p)) {
                (void)vfs_.write_all_bytes(p, inc.data);
            }
        } else if (inc.export_action == 2) {
            if (inc.overwrite || !vfs_.exists(inc.filename)) {
                (void)vfs_.write_all_bytes(inc.filename, inc.data);
            }
        }
    }

    // Extract GEX DLL (kind == 1) and File/Other (kind == 4) payloads into temp_dir[/ext.folder]/filename.
    for (const auto& ext : game_.extensions) {
        std::string base_dir = vfs_.temp_directory_gm();
        if (!base_dir.empty() && base_dir.back() != '\\' && base_dir.back() != '/') {
            base_dir.push_back('/');
        }
        if (!ext.folder.empty()) {
            base_dir += ext.folder;
            if (!base_dir.empty() && base_dir.back() != '\\' && base_dir.back() != '/') {
                base_dir.push_back('/');
            }
        }
        for (const auto& inc : ext.includes) {
            if ((inc.kind == 1 || inc.kind == 4) && !inc.payload.empty() && !inc.filename.empty()) {
                const std::string p = base_dir + inc.filename;
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
        auto scan_event = [&](const BytecodeChunk& event) {
            scan_chunk(event);
            for (const auto& action : event.code_actions) scan_chunk(*action);
        };
        for (const auto& arr : program_.object_event_chunks)
            for (const auto& mp : arr)
                for (const auto& [_, c] : mp) scan_event(c);
        for (const auto& mp : program_.timeline_chunks)
            for (const auto& [_, c] : mp) scan_event(*c);
        for (const auto& c : program_.room_creation_chunks) if (c) scan_chunk(*c);
        for (const auto& room : program_.instance_creation_chunks)
            for (const auto& [_, c] : room) scan_chunk(c);
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

    // Run extension init functions for both kind == 1 (DLL) and kind == 2 (GML) in include order.
    // Provide a temporary context instance (matching GM8's startup context) so non-var scratch
    // variables inside extension init scripts (e.g. __gm82core_init's __s) resolve properly.
    {
        Instance ext_ctx{};
        set_instance_object(ext_ctx, 0);
        ext_ctx.timeline_running = false;
        register_instance_handle(ext_ctx);

        for (const auto& ext : game_.extensions) {
            for (const auto& inc : ext.includes) {
                if (inc.init_func.empty()) continue;
                if (inc.kind == 2) {
                    if (const auto* target = program_.symbols.find_callable(inc.init_func)) {
                        if (target->kind == CallableKind::ExtensionGml) {
                            auto r = vm_->execute_ext_gml(target->id, ext_ctx.handle, ext_ctx.handle);
                            if (!r) {
                                GM_LOG_WARN("runtime", "Extension init '%s' error: %s",
                                            inc.init_func.c_str(), r.error_message().c_str());
                            }
                        }
                    }
                } else if (inc.kind == 1) {
                    std::string ext_sym = inc.init_func;
                    for (const auto& fn : inc.functions) {
                        if (fn.name == inc.init_func && !fn.external_name.empty()) {
                            ext_sym = fn.external_name;
                            break;
                        }
                    }
                    (void)extensions_.call_gex_dll(*this, inc.filename, ext_sym, {}, ExternalValueType::Real);
                }
            }
        }

        // Run D&D library initialization code chunks.
        for (const auto& chunk : program_.library_init_chunks) {
            if (!chunk.code.empty()) {
                auto r = vm_->execute(chunk, ext_ctx.handle, ext_ctx.handle);
                if (!r) {
                    GM_LOG_WARN("runtime", "Library init error: %s", r.error_message().c_str());
                }
            }
        }

        inst_by_id_.erase(ext_ctx.handle);
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
        // A host stop interrupted the active event. Game End handlers still run.
        if (host_stop_requested_) pending_action_ = PendingRoomAction::None;
        // Fire Game End event on remaining active instances.
        broadcast_event(7, 3); // ev_other, ev_game_end
    }

    // Run extension finalization functions (both kind == 1 DLL and kind == 2 GML) before teardown.
    if (vm_) {
        Instance ext_ctx{};
        set_instance_object(ext_ctx, 0);
        ext_ctx.timeline_running = false;
        register_instance_handle(ext_ctx);
        for (const auto& ext : game_.extensions) {
            for (const auto& inc : ext.includes) {
                if (inc.final_func.empty()) continue;
                if (inc.kind == 2) {
                    if (const auto* target = program_.symbols.find_callable(inc.final_func)) {
                        if (target->kind == CallableKind::ExtensionGml) {
                            (void)vm_->execute_ext_gml(target->id, ext_ctx.handle, ext_ctx.handle);
                        }
                    }
                } else if (inc.kind == 1) {
                    std::string ext_sym = inc.final_func;
                    for (const auto& fn : inc.functions) {
                        if (fn.name == inc.final_func && !fn.external_name.empty()) {
                            ext_sym = fn.external_name;
                            break;
                        }
                    }
                    (void)extensions_.call_gex_dll(*this, inc.filename, ext_sym, {}, ExternalValueType::Real);
                }
            }
        }
        inst_by_id_.erase(ext_ctx.handle);
    }

    // Flush open files & INI.
    (void)close_legacy_text_file();
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
    inactive_instances_.clear();
    object_instances_.clear();
    inst_by_id_.clear();
    tiles_.clear();
    saved_rooms_.clear();
    ds_.clear_all();
    particles_.clear_all();
    mp_grids_.destroy_all();

    extensions_.reset(this);
    cancel_room_transition();
    audio_.shutdown();
    modal_dialogs_.shutdown();
    message_dialog_.shutdown();
    touch_overlay_.shutdown();
    renderer_.shutdown();
    vm_.reset();
}

void Runner::register_builtin(std::string_view name, BuiltinFn fn) {
    if (const auto* info = BuiltinsCatalog::find_function(name)) {
        if (info->id < builtins_.size()) {
            builtins_[info->id] = std::move(fn);
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
    register_builtins_misc();
}

// ---------------------------------------------------------------------------
// Game & Room lifecycle
// ---------------------------------------------------------------------------

void Runner::start_game() {
    instances_.clear();
    inactive_instances_.clear();
    object_instances_.clear();
    inst_by_id_.clear();
    tiles_.clear();
    saved_rooms_.clear();
    particles_.clear_all();
    audio_.sound_stop_all();

    score_ = 0.0;
    lives_ = -1.0;
    health_ = 100.0;
    cursor_sprite_ = -1;
    cursor_subimg_ = 0;
    transition_kind_ = 0;
    no_more_lives_armed_ = false;
    no_more_health_armed_ = false;
    current_room_ = -1;
    pending_action_ = PendingRoomAction::None;
    pending_room_id_ = -1;
    pending_load_path_.clear();
    cancel_room_transition();

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
        // GM8 prepares the room transition before Room End (gm81.exe.c:sub_606E94 -> sub_5F96B4).
        prepare_room_transition(transition_kind_);

        broadcast_event(7, 5); // ev_other, ev_room_end
        remove_marked_instances();
        if (io_roomend_clear_) {
            window_.input_mut().clear_all();
        }

        SavedRoomState state{};
        if (room_persistent_) {
            state.tiles = tiles_;
            state.backgrounds = backgrounds_;
            state.views = views_;
            state.view_enabled = view_enabled_;
            state.background_color = bg_color_;
            state.background_showcolor = bg_showcolor_;
            state.width = room_width_;
            state.height = room_height_;
            state.speed = room_speed_;
            state.persistent = room_persistent_;
            state.caption = room_caption_;

        }
        std::vector<std::unique_ptr<Instance>> travelling;
        inst_by_id_.clear();
        for (auto& inst : instances_) {
            if (inst->persistent) {
                inst_by_id_[inst->id] = inst.get();
                inst_by_id_[inst->handle] = inst.get();
                travelling.push_back(std::move(inst));
            } else if (room_persistent_) {
                state.instances.push_back(std::move(*inst));
            }
        }
        if (room_persistent_) {
            for (auto& inst : inactive_instances_) state.instances.push_back(std::move(*inst));
            saved_rooms_[current_room_] = std::move(state);
        }
        instances_ = std::move(travelling);
        inactive_instances_.clear();
        rebuild_object_instances();
        tiles_.clear();
    }

    current_room_ = room_id;
    const auto& rm = game_.rooms[static_cast<usize>(room_id)];
    const auto travelling_count = instances_.size();
    std::vector<i32> created_ids;
    bool restored_from_save = false;

    if (auto it = saved_rooms_.find(room_id); it != saved_rooms_.end()) {
        restored_from_save = true;
        SavedRoomState state = std::move(it->second);
        saved_rooms_.erase(it);

        room_width_ = state.width;
        room_height_ = state.height;
        room_speed_ = state.speed;
        room_persistent_ = state.persistent;
        room_caption_ = state.caption;
        bg_color_ = state.background_color;
        bg_showcolor_ = state.background_showcolor;
        backgrounds_ = state.backgrounds;
        views_ = state.views;
        view_enabled_ = state.view_enabled;
        tiles_ = std::move(state.tiles);

        for (auto& saved_inst : state.instances) {
            if (find_instance(saved_inst.id)) continue; // A travelling instance owns this ID.
            auto up = std::make_unique<Instance>(std::move(saved_inst));
            up->mark_bbox_dirty();
            add_room_instance(std::move(up));
        }
    } else {
        room_width_ = rm.width;
        room_height_ = rm.height;
        room_speed_ = rm.speed > 0 ? rm.speed : (version_mode() == VersionMode::GM82 ? 1 : 30);
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
    }

    // GM8 sets the room's screen region before instance creation code, including on re-entry.
    sync_room_screen_region();

    if (!restored_from_save) {
        // First allocate all room instances so creation codes can reference each other by ID.
        created_ids.reserve(rm.instances.size());
        for (const auto& ri : rm.instances) {
            if (find_instance(ri.id)) continue; // Keep the travelling instance, including its variables.
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
                created_ids.push_back(inst->handle);
            }
        }
    }

    // GM appends travelling instances after the new room's own instances.
    if (travelling_count > 0) {
        std::rotate(instances_.begin(), instances_.begin() + travelling_count, instances_.end());
        rebuild_object_instances();
    }
    for (i32 id : created_ids) {
        if (pending_action_ != PendingRoomAction::None) break;
        auto* inst = find_instance(id);
        if (!inst || inst->destroyed) continue;

        if (game_.options.creation_event_order) {
            perform_event_on_instance(id, -1, 0, 0, id);
            if (pending_action_ != PendingRoomAction::None) break;
        }
        if (static_cast<usize>(room_id) < program_.instance_creation_chunks.size()) {
            const auto& chunks = program_.instance_creation_chunks[static_cast<usize>(room_id)];
            if (auto it = chunks.find(inst->id); it != chunks.end()) {
                auto result = vm_->execute(it->second, id, id);
                if (!result) GM_LOG_WARN("runtime", "Instance %d creation code error: %s", inst->id, result.error_message().c_str());
            }
        }
        if (!game_.options.creation_event_order && pending_action_ == PendingRoomAction::None) {
            execute_instance_event(id, inst->object_index, 0, 0, id);
        }
    }

    // The first room's creation code sees globals initialized by Game Start.
    if (is_game_start && pending_action_ == PendingRoomAction::None) {
        broadcast_event(7, 2);
    }

    // Run creation code only when the room is freshly initialized.
    if (!restored_from_save && pending_action_ == PendingRoomAction::None) {
        if (static_cast<usize>(room_id) < program_.room_creation_chunks.size() &&
            program_.room_creation_chunks[static_cast<usize>(room_id)].has_value()) {
            const auto& chunk = *program_.room_creation_chunks[static_cast<usize>(room_id)];
            if (!chunk.code.empty()) {
                // GM uses a temporary instance of object 0 as both self and other.
                // It has its own variables but never joins the room's instance lists.
                Instance context{};
                set_instance_object(context, 0);
                context.timeline_running = false;
                register_instance_handle(context);
                auto r = vm_->execute(chunk, context.handle, context.handle);
                inst_by_id_.erase(context.handle);
                if (!r) {
                    GM_LOG_WARN("runtime", "Room '%s' creation code error: %s",
                                game_.rooms[static_cast<usize>(room_id)].name.c_str(), r.error_message().c_str());
                }
            }
        }
    }

    // Fire Room Start
    if (pending_action_ == PendingRoomAction::None) {
        broadcast_event(7, 4); // ev_other, ev_room_start
    }

    remove_marked_instances();
    if (pending_action_ == PendingRoomAction::None && auto_draw_) {
        if (active_transition_kind_ > 0 && transition_surf_old_ >= 0) {
            finish_room_transition();
        } else {
            cancel_room_transition();
            // Room Start draws the initial image before the new room's first step.
            render_current_frame();
            present_current_frame();
        }
        transition_kind_ = 0;
    } else {
        cancel_room_transition();
        update_view_following();
    }
}

void Runner::sync_room_screen_region() {
    i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
    room_canvas_size(cw, ch);
    // On phones the logical game region can be larger than the display in either axis.
    // Keep its dimensions intact and fit it when presenting to the drawable.
    if (!PlatformWindow::is_mobile_touch_platform()) {
        cw = std::min(cw, window_.display_width());
        ch = std::min(ch, window_.display_height());
    }
    if (!window_fixed_ && (cw != window_region_w_ || ch != window_region_h_)) {
        window_region_w_ = cw;
        window_region_h_ = ch;
        window_default();
        requested_backbuffer_w_ = cw;
        requested_backbuffer_h_ = ch;
    }
    compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
    renderer_.set_screen_region(window_.drawable_width(), window_.drawable_height(),
                                requested_backbuffer_w_ > 0 ? requested_backbuffer_w_ : cw,
                                requested_backbuffer_h_ > 0 ? requested_backbuffer_h_ : ch,
                                ViewportRect{vx, vy, vw, vh});
}

bool Runner::transition_define(i32 kind, std::string_view script_name) {
    if (kind <= 0 || script_name.empty()) {
        return false;
    }
    const auto* target = program_.symbols.find_callable(script_name);
    if (!target || (target->kind != CallableKind::Script && target->kind != CallableKind::ExtensionGml)) {
        return false;
    }
    transition_scripts_[kind] = TransitionScript{target->kind, target->id, std::string(script_name)};
    return true;
}

bool Runner::transition_exists(i32 kind) const noexcept {
    if (kind <= 0) return false;
    return (kind >= 1 && kind <= 21) || transition_scripts_.contains(kind);
}

void Runner::cancel_room_transition() noexcept {
    if (transition_surf_new_ >= 0) {
        if (renderer_.current_surface_target() == transition_surf_new_) {
            renderer_.surface_reset_target();
        }
        renderer_.surface_free(transition_surf_new_);
        transition_surf_new_ = -1;
    }
    if (transition_surf_old_ >= 0) {
        if (renderer_.current_surface_target() == transition_surf_old_) {
            renderer_.surface_reset_target();
        }
        renderer_.surface_free(transition_surf_old_);
        transition_surf_old_ = -1;
    }
    active_transition_kind_ = -1;
}

void Runner::prepare_room_transition(i32 kind) {
    cancel_room_transition();
    if (!transition_exists(kind) || !window_.is_open()) {
        return;
    }

    i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
    compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
    const i32 bw = std::max(1, requested_backbuffer_w_ > 0 ? requested_backbuffer_w_ : cw);
    const i32 bh = std::max(1, requested_backbuffer_h_ > 0 ? requested_backbuffer_h_ : ch);

    if (renderer_.current_surface_target() >= 0) {
        renderer_.surface_reset_target();
    }

    const i32 surf_old = renderer_.surface_create(bw, bh);
    if (surf_old < 0) {
        return;
    }

    active_transition_kind_ = kind;
    transition_surf_old_ = surf_old;

    renderer_.surface_set_target(surf_old);
    renderer_.clear_color(window_color_, 1.0f);

    // Match gm81.exe.c:sub_5F96B4: temporarily clear pending state so Draw events run while capturing the old room.
    const PendingRoomAction saved_action = pending_action_;
    const i32 saved_room_id = pending_room_id_;
    const std::string saved_load_path = pending_load_path_;
    pending_action_ = PendingRoomAction::None;
    pending_room_id_ = -1;
    pending_load_path_.clear();

    render_current_frame();

    pending_action_ = saved_action;
    pending_room_id_ = saved_room_id;
    pending_load_path_ = saved_load_path;

    renderer_.surface_fill_alpha_opaque(surf_old);
    if (renderer_.current_surface_target() == surf_old) {
        renderer_.surface_reset_target();
    }
}

void Runner::execute_custom_transition_script(
    const TransitionScript& script,
    i32 surf_old, i32 surf_new,
    i32 width, i32 height, f64 fraction) {
    if (!vm_) return;
    // Match gm81.exe.c:sub_5F93B0: run with a temporary object-0 context instance and 5 arguments.
    Instance ctx{};
    set_instance_object(ctx, 0);
    ctx.timeline_running = false;
    register_instance_handle(ctx);

    const std::array<RValue, 5> args{
        RValue(surf_old),
        RValue(surf_new),
        RValue(width),
        RValue(height),
        RValue(fraction),
    };

    Result<RValue> res = (script.kind == CallableKind::ExtensionGml)
        ? vm_->execute_ext_gml(script.id, ctx.handle, ctx.handle, args)
        : vm_->execute_script(script.id, ctx.handle, ctx.handle, args);

    inst_by_id_.erase(ctx.handle);
    if (!res) {
        GM_LOG_WARN("runtime", "Transition script '%s' error: %s",
                    script.name.c_str(), res.error_message().c_str());
    }
}

void Runner::finish_room_transition() {
    const i32 kind = active_transition_kind_;
    if (kind <= 0 || transition_surf_old_ < 0 || !renderer_.surface_exists(transition_surf_old_) ||
        !transition_exists(kind) || !window_.is_open()) {
        cancel_room_transition();
        render_current_frame();
        present_current_frame();
        return;
    }

    i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
    compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
    i32 bw = std::max(1, requested_backbuffer_w_ > 0 ? requested_backbuffer_w_ : cw);
    i32 bh = std::max(1, requested_backbuffer_h_ > 0 ? requested_backbuffer_h_ : ch);

    if (renderer_.current_surface_target() >= 0) {
        renderer_.surface_reset_target();
    }

    transition_surf_new_ = renderer_.surface_create(bw, bh);
    if (transition_surf_new_ < 0) {
        cancel_room_transition();
        render_current_frame();
        present_current_frame();
        return;
    }

    renderer_.surface_set_target(transition_surf_new_);
    renderer_.clear_color(window_color_, 1.0f);
    render_current_frame();
    renderer_.surface_fill_alpha_opaque(transition_surf_new_);
    if (renderer_.current_surface_target() == transition_surf_new_) {
        renderer_.surface_reset_target();
    }

    std::optional<TransitionScript> custom_script;
    if (const auto it = transition_scripts_.find(kind); it != transition_scripts_.end()) {
        custom_script = it->second;
    }

    const i32 steps = std::max(1, transition_steps_);
    ModalInputFrame minput{};
    bool context_lost_aborted = false;

    for (i32 step = 0; step <= steps && running_ && !host_stop_requested_; ++step) {
        const u64 frame_start = Clock::now_us();

        bool aborted = false;
        while (true) {
            window_.poll_modal_events(minput);
            const auto& wev = minput.window_events;

            if (wev.entered_background) {
                paused_in_background_ = true;
                audio_.suspend();
                vfs_.notify_persist_write();
            }
            if (wev.entered_foreground) {
                paused_in_background_ = false;
                audio_.resume();
            }
            if (wev.context_lost) {
                if (auto res = gfx_.reinit_after_context_loss(&PlatformWindow::gl_get_proc_address); res) {
                    (void)renderer_.reload_gpu_resources(game_);
                    (void)touch_overlay_.reload_gpu_resources();
                    (void)message_dialog_.reload_gpu_resources();
                    (void)modal_dialogs_.reload_gpu_resources();
                }
                context_lost_aborted = true;
                aborted = true;
                break;
            }
#if defined(__EMSCRIPTEN__)
            if (wev.any_user_gesture && !web_audio_unlocked_) {
                web_audio_unlocked_ = true;
                web_unlock_audio();
                audio_.resume();
            }
#endif
            if (host_stop_requested_ || wev.quit_requested) {
                running_ = false;
                pending_action_ = PendingRoomAction::EndGame;
                aborted = true;
                break;
            }
            if (!paused_in_background_) {
                break;
            }
#if defined(__EMSCRIPTEN__)
            Clock::yield_ms(16);
#else
            Clock::sleep_until_us(Clock::now_us() + 16000u);
#endif
        }
        if (aborted) break;

        const i32 dw = std::max(1, window_.drawable_width());
        const i32 dh = std::max(1, window_.drawable_height());
        compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
        bw = std::max(1, requested_backbuffer_w_ > 0 ? requested_backbuffer_w_ : cw);
        bh = std::max(1, requested_backbuffer_h_ > 0 ? requested_backbuffer_h_ : ch);
        renderer_.set_screen_region(dw, dh, bw, bh, ViewportRect{vx, vy, vw, vh});

        renderer_.begin_frame(dw, dh);
        renderer_.begin_screen_pass(bw, bh, dw, dh, ViewportRect{vx, vy, vw, vh});

        const f64 fraction = static_cast<f64>(step) / static_cast<f64>(steps);
        if (custom_script.has_value()) {
            const bool to_screen_fbo = renderer_.rendering_to_fbo();
            const ViewportRect port = to_screen_fbo ? ViewportRect{0, 0, bw, bh} : ViewportRect{vx, vy, vw, vh};
            gfx_.set_scissor(false);
            renderer_.set_projection_ortho(0.0f, 0.0f, static_cast<f32>(bw), static_cast<f32>(bh),
                                           0.0f, port.x, port.y, port.width, port.height);
            execute_custom_transition_script(*custom_script, transition_surf_old_, transition_surf_new_,
                                             bw, bh, fraction);
            if (renderer_.current_surface_target() >= 0) {
                renderer_.surface_reset_target();
            }
        } else {
            renderer_.draw_builtin_transition(kind, transition_surf_old_, transition_surf_new_,
                                              bw, bh, steps, fraction);
        }

        present_current_frame();

        if (step < steps && running_ && !host_stop_requested_) {
#if defined(__EMSCRIPTEN__)
            const u64 elapsed_ms = (Clock::now_us() - frame_start) / 1000u;
            Clock::yield_ms(elapsed_ms < 16u ? static_cast<u32>(16u - elapsed_ms) : 1u);
#else
            Clock::sleep_until_us(frame_start + 16667u);
#endif
        }
    }

    cancel_room_transition();
    if (context_lost_aborted && running_ && !host_stop_requested_) {
        render_current_frame();
        present_current_frame();
    }
}

void Runner::process_pending_room_action() {
    for (int guard = 0; guard < 32 && pending_action_ != PendingRoomAction::None; ++guard) {
        const PendingRoomAction act = pending_action_;
        const i32 target = pending_room_id_;
        const std::string load_path = std::move(pending_load_path_);
        pending_action_ = PendingRoomAction::None;
        pending_room_id_ = -1;
        pending_load_path_.clear();

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
                vm_->reset_globals();
                start_game();
                return;
            case PendingRoomAction::EndGame:
                running_ = false;
                return;
            case PendingRoomAction::LoadGame:
                // Match gm81.exe.c:sub_60714C: clear IO before executing deferred game_load.
                window_.input_mut().clear_all();
                mouse_button_ = 0;
                mouse_lastbutton_ = 0;
                (void)load_game(load_path);
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

void Runner::request_game_load(std::string_view path) {
    pending_load_path_ = std::string(path);
    pending_action_ = PendingRoomAction::LoadGame;
    pending_room_id_ = -1;
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

    if (ev.entered_background) {
        paused_in_background_ = true;
        audio_.suspend();
        vfs_.notify_persist_write();
        GM_LOG_INFO("runtime", "Entered background; suspended audio and frame simulation");
    }
    if (ev.entered_foreground) {
        paused_in_background_ = false;
        audio_.resume();
        GM_LOG_INFO("runtime", "Returned to foreground; resumed audio and frame simulation");
    }
    if (ev.context_lost) {
        GM_LOG_WARN("gfx", "Graphics context lost; reloading GPU resources");
        if (auto res = gfx_.reinit_after_context_loss(&PlatformWindow::gl_get_proc_address); res) {
            (void)renderer_.reload_gpu_resources(game_);
            (void)touch_overlay_.reload_gpu_resources();
            (void)message_dialog_.reload_gpu_resources();
            (void)modal_dialogs_.reload_gpu_resources();
        }
    }
#if defined(__EMSCRIPTEN__)
    if (ev.any_user_gesture && !web_audio_unlocked_) {
        web_audio_unlocked_ = true;
        web_unlock_audio();
        audio_.resume();
    }
#endif

    if (ev.quit_requested) {
        // Fire ev_close_button (ev_other=7, sub=30) if any object handles it, then quit if close_sec allows.
        const bool handled_close = broadcast_event(7, 30);
        if (!handled_close || pending_action_ == PendingRoomAction::EndGame) {
            running_ = false;
            return;
        }
    }

    if (paused_in_background_) {
        return;
    }

    if (touch_overlay_.update(window_)) {
        if (ev.resized) {
            i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
            compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
            const i32 bw = requested_backbuffer_w_ > 0 ? requested_backbuffer_w_ : cw;
            const i32 bh = requested_backbuffer_h_ > 0 ? requested_backbuffer_h_ : ch;
            renderer_.set_screen_region(window_.drawable_width(), window_.drawable_height(),
                                        bw, bh, ViewportRect{vx, vy, vw, vh});
        }
        present_current_frame();
        return;
    }
    const InputState& inp = window_.input();

    // Standard GM8 default hotkeys (respecting GameOptions).
    if (game_.options.help_key && inp.key_pressed[112]) { // F1 Game Information
        show_info_dialog();
        return;
    }
    if (game_.options.quit_key && inp.key_pressed[27]) { // ESC
        running_ = false;
        return;
    }
    if (game_.options.screen_key && inp.key_pressed[115]) { // F4 fullscreen toggle
        window_.set_fullscreen(!window_.is_fullscreen());
    }
    if (game_.options.save_key && inp.key_pressed[116]) { // F5 quick-save (gm81.exe.c:sub_6065A0)
        (void)save_game("_save" + std::to_string(game_.meta.game_id) + ".sav");
    }
    if (game_.options.save_key && inp.key_pressed[117]) { // F6 quick-load (gm81.exe.c:sub_6065A0)
        const std::string quick_path = "_save" + std::to_string(game_.meta.game_id) + ".sav";
        window_.input_mut().clear_all();
        mouse_button_ = 0;
        mouse_lastbutton_ = 0;
        if (load_game(quick_path)) {
            return;
        }
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
    if (pending_action_ == PendingRoomAction::None) step_event(1);
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
    if (pending_action_ == PendingRoomAction::None) step_event(0);
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
    if (pending_action_ == PendingRoomAction::None) step_event(2);

    // Stage 13: Particle update & deferred destroy cleanup
    particles_.update_all();
    remove_marked_instances();

    // The new room draws on entry and starts advancing animations on its own first step.
    if (pending_action_ != PendingRoomAction::None) {
        process_pending_room_action();
        return;
    }

    // Stage 14: Draw pass (when automatic draw is enabled).
    // Defer presenting screen_fbo_ until after Stage 15 so extensions like gm82dx9
    // can compose application_surface onto the screen in ev_other / ev_animation_end.
    if (auto_draw_) {
        render_current_frame();
    }

    // Stage 15: Post-draw cleanup + advance animations & Animation End (ev 7, sub 7)
    remove_marked_instances();
    if (pending_action_ == PendingRoomAction::None) {
        step_animations();
        ++cursor_subimg_; // gm82.exe.c:sub_60756C (++*off_688A0C)
    }
    if (pending_action_ != PendingRoomAction::None) {
        process_pending_room_action();
        return;
    }

    // Stage 16: Present + FPS measurement
    if (auto_draw_) {
        present_current_frame();
    }

    ++fps_frame_accum_;
    const u64 now_us = Clock::now_us();
    if (now_us - fps_last_sample_us_ >= 1000000u) {
        fps_ = static_cast<i32>(fps_frame_accum_);
        fps_frame_accum_ = 0;
        fps_last_sample_us_ = now_us;
    }
}

void Runner::invalidate_event_tables() {
    // A change to object inheritance also invalidates the tables in a GM8.2 EXE.
    use_precomputed_events_ = false;
    generated_event_tables_.reset();
}

const Gm82EventTables& Runner::event_tables() {
    if (use_precomputed_events_ && game_.meta.has_gm82_event_tables && version_mode() == VersionMode::GM82) {
        return game_.gm82_event_tables;
    }
    if (generated_event_tables_) return *generated_event_tables_;

    auto& tables = generated_event_tables_.emplace();
    // GM8's sub_5BD6BC allocates these sub-event buckets before populating
    // them in ascending object-resource order. Collision pairs live separately.
    const std::array<usize, 12> sizes{1, 1, 13, 17, 0, 129, 129, 129, 1, 129, 129, game_.triggers.size()};
    for (usize cat = 0; cat < sizes.size(); ++cat) tables.event_objects[cat].resize(sizes[cat]);

    std::vector<std::vector<i32>> descendants(game_.objects.size());
    for (i32 object = 0; static_cast<usize>(object) < game_.objects.size(); ++object) {
        i32 owner = object;
        while (object_exists(owner)) {
            auto& children = descendants[owner];
            if (!children.empty() && children.back() == object) break;
            children.push_back(object);
            if (static_cast<usize>(owner) < program_.object_event_chunks.size()) {
                const auto& events = program_.object_event_chunks[owner];
                for (usize cat = 0; cat < events.size(); ++cat) {
                    for (const auto& [sub, chunk] : events[cat]) {
                        if (sub >= 0 && static_cast<usize>(sub) < sizes[cat]) {
                            auto& objects = tables.event_objects[cat][sub];
                            if (objects.empty() || objects.back() != object) objects.push_back(object);
                        }
                    }
                }
            }
            owner = game_.objects[owner].parent_index;
        }
    }
    auto& pairs = tables.collision_pairs;
    for (usize owner = 0; owner < descendants.size() && owner < program_.object_event_chunks.size(); ++owner) {
        for (const auto& [sub, chunk] : program_.object_event_chunks[owner][4]) {
            if (!object_exists(sub)) continue;
            for (i32 a : descendants[owner]) {
                for (i32 b : descendants[sub]) pairs.emplace_back(std::min(a, b), std::max(a, b));
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return tables;
}

const std::vector<i32>& Runner::event_objects(i32 category, i32 sub) {
    static const std::vector<i32> none;
    if (category < 0 || category >= 12 || sub < 0) return none;
    const auto& buckets = event_tables().event_objects[category];
    return static_cast<usize>(sub) < buckets.size() ? buckets[sub] : none;
}

void Runner::step_event(i32 moment) {
    for_event_instances(3, moment, [&](Instance& inst) {
        execute_instance_event(inst.handle, inst.object_index, 3, moment, inst.handle);
    });
}

void Runner::step_triggers(i32 moment) {
    for (usize tid = 0; tid < game_.triggers.size(); ++tid) {
        const auto& tr = game_.triggers[tid];
        if (!tr.exists || tr.moment != moment) continue;
        if (tid >= program_.trigger_chunks.size() || !program_.trigger_chunks[tid].has_value()) continue;
        const auto& cond_chunk = *program_.trigger_chunks[tid];
        if (cond_chunk.code.empty()) continue;

        const i32 sub = static_cast<i32>(tid);
        for_event_instances(11, sub, [&](Instance& inst) {
            const i32 handle = inst.handle;
            auto res = vm_->execute(cond_chunk, handle, handle);
            if (res && res.value().is_truthy() && pending_action_ == PendingRoomAction::None) {
                execute_instance_event(handle, inst.object_index, 11, sub, handle);
            }
        }, /*continue_after_transition=*/true); // GM still evaluates the remaining conditions.
    }
}

void Runner::step_timelines() {
    const usize count = instances_.size();
    for (usize i = 0; i < count && i < instances_.size(); ++i) {
        auto* inst = instances_[i].get();
        if (inst->destroyed || inst->deactivated || !inst->timeline_running) continue;
        const i32 index = inst->timeline_index;
        if (index < 0 || static_cast<usize>(index) >= game_.timelines.size()) continue;
        const auto& tl = game_.timelines[index];
        if (!tl.exists || tl.moments.empty() || inst->timeline_speed == 0.0) continue;
        const f64 prev = inst->timeline_position;
        const f64 next = prev + inst->timeline_speed;
        const bool forward = inst->timeline_speed > 0.0;
        inst->timeline_position = next;
        // The native loop fixes its index range, then looks up each moment in the live timeline.
        const auto bound = [&](f64 pos) {
            const auto it = forward ? std::ranges::lower_bound(tl.moments, pos, {}, &TimelineMoment::step)
                                    : std::ranges::upper_bound(tl.moments, pos, {}, &TimelineMoment::step);
            return static_cast<usize>(it - tl.moments.begin());
        };
        usize first = bound(forward ? prev : next);
        usize end = bound(forward ? next : prev);
        while (first < end && i < instances_.size() && pending_action_ == PendingRoomAction::None) {
            const usize slot = forward ? first++ : --end;
            const auto& moments = game_.timelines[index].moments;
            if (slot >= moments.size()) continue;
            const i32 step = moments[slot].step;
            const auto& chunks = program_.timeline_chunks[index];
            const auto it = chunks.find(step);
            if (it == chunks.end()) continue;
            const auto event = it->second; // A callback can delete its own moment.
            // GM fetches the receiver from the current room slot for every moment.
            const i32 handle = instances_[i]->handle;
            const bool saved_rel = action_relative_;
            action_relative_ = false;
            event_stack_.push_back(EventFrame{100000, step, index, handle, handle, 0});
            auto result = vm_->execute(*event, handle, handle);
            event_stack_.pop_back();
            action_relative_ = saved_rel;
            if (!result) GM_LOG_WARN("runtime", "Timeline %d step %d error: %s", index, step, result.error_message().c_str());
        }
        if (inst->timeline_loop && !game_.timelines[index].moments.empty()) {
            const i32 last_step = game_.timelines[index].moments.back().step;
            if (forward && inst->timeline_position > last_step) inst->timeline_position = 0;
            if (!forward && inst->timeline_position < 0.0) inst->timeline_position = last_step;
        }
    }
}

void Runner::step_alarms() {
    for (i32 alarm = 0; alarm < 12; ++alarm) {
        for_event_instances(2, alarm, [&](Instance& inst) {
            i32& ticks = inst.alarm[alarm];
            if (ticks >= 0) --ticks;
            if (ticks == 0 && pending_action_ == PendingRoomAction::None) {
                execute_instance_event(inst.handle, inst.object_index, 2, alarm, inst.handle);
            }
        }, /*continue_after_transition=*/true);
    }
}

void Runner::step_keyboard_events() {
    const InputState& input = window_.input();
    for (const i32 cat : {5, 9, 10}) {
        bool any = false;
        auto dispatch = [&](i32 key) {
            for_event_instances(cat, key, [&](Instance& inst) {
                perform_event_on_instance(inst.handle, -1, cat, key, inst.handle);
            });
        };
        for (i32 key = 8; key < 256; ++key) {
            if (pending_action_ != PendingRoomAction::None) return;
            const bool pressed = input.key_pressed[key];
            const bool active = cat == 5 ? (input.key_down[key] || pressed)
                              : cat == 9 ? pressed : input.key_released[key];
            if (active) {
                dispatch(key);
                any = true;
            }
        }
        dispatch(any ? 1 : 0); // vk_anykey / vk_nokey follow the individual keys.
    }
}

void Runner::step_mouse_events() {
    const InputState& input = window_.input();
    const f64 previous_x = event_mouse_x_, previous_y = event_mouse_y_;
    event_mouse_x_ = mouse_x_;
    event_mouse_y_ = mouse_y_;

    auto dispatch = [&](i32 sub, bool global = false) {
        for_event_instances(6, sub, [&](Instance& inst) {
            if (!global) {
                const bool over = check_point_collision(inst, event_mouse_x_, event_mouse_y_, true, game_, version_mode());
                if (sub == 10 || sub == 11) {
                    const bool was_over = check_point_collision(inst, previous_x, previous_y, true, game_, version_mode());
                    if (was_over == over || over != (sub == 10)) return;
                } else if (!over) {
                    return;
                }
            }
            perform_event_on_instance(inst.handle, -1, 6, sub, inst.handle);
        });
    };

    bool any = false;
    for (i32 button = 1; button <= 3; ++button) {
        if (input.mouse_down[button] || input.mouse_pressed[button]) {
            dispatch(button - 1);
            dispatch(button + 49, true);
            any = true;
        }
    }
    if (!any) dispatch(3);
    for (i32 button = 1; button <= 3; ++button) {
        if (input.mouse_pressed[button]) {
            dispatch(button + 3);
            dispatch(12, true);
            dispatch(button + 52, true);
        }
    }
    for (i32 button = 1; button <= 3; ++button) {
        if (input.mouse_released[button]) {
            dispatch(button + 6);
            dispatch(13, true);
            dispatch(button + 55, true);
        }
    }
    dispatch(10);
    dispatch(11);
    if (input.mouse_wheel_up) dispatch(60, true);
    if (input.mouse_wheel_down) dispatch(61, true);
}

static bool advance_instance_path(Instance& inst, const GameData& game) {
    if (inst.path_index < 0 || static_cast<usize>(inst.path_index) >= game.paths.size() || inst.path_speed == 0.0) return false;
    const auto& path = game.paths[static_cast<usize>(inst.path_index)];
    if (!path.exists || path.length() <= 0.0) return false;

    const f64 speed = path.sample(inst.path_position).speed / (100.0 * inst.path_scale);
    inst.path_position += inst.path_speed * speed / path.length();

    const PathPoint start = path.sample(0.0);
    const f64 angle = deg_to_rad(inst.path_orientation);
    const f64 c = std::cos(angle), s = std::sin(angle);
    const auto offset = [&](const PathPoint& p) {
        const f64 dx = p.x - start.x, dy = p.y - start.y;
        return std::pair{inst.path_scale * (dx * c + dy * s), inst.path_scale * (dy * c - dx * s)};
    };

    const bool reached_end = inst.path_position >= 1.0 || inst.path_position <= 0.0;
    if (reached_end) {
        const bool before_start = inst.path_position < 0.0;
        switch (inst.path_endaction) {
            case 2: { // Continue from the opposite endpoint.
                const auto [dx, dy] = offset(path.sample(1.0));
                inst.path_xstart += before_start ? -dx : dx;
                inst.path_ystart += before_start ? -dy : dy;
                [[fallthrough]];
            }
            case 1: // Restart.
                inst.path_position += before_start ? 1.0 : -1.0;
                break;
            case 3: // Reverse.
                inst.path_position = before_start ? -inst.path_position : 2.0 - inst.path_position;
                inst.path_speed = before_start ? std::fabs(inst.path_speed) : -std::fabs(inst.path_speed);
                break;
            default: // Stop.
                inst.path_position = 1.0;
                inst.path_index = -1;
                break;
        }
    }

    const auto [dx, dy] = offset(path.sample(inst.path_position));
    const f64 x = inst.path_xstart + dx, y = inst.path_ystart + dy;
    // GM keeps the direction of the path displacement and clears ordinary motion.
    if (x != inst.x || y != inst.y) inst.direction = gm_point_direction(inst.x, inst.y, x, y);
    inst.speed = inst.hspeed = inst.vspeed = 0.0;
    inst.x = x;
    inst.y = y;
    inst.mark_bbox_dirty();
    return reached_end;
}

static void apply_instance_velocity(Instance& inst) {
    if (inst.hspeed != 0.0 || inst.vspeed != 0.0) {
        inst.x += inst.hspeed;
        inst.y += inst.vspeed;
        inst.mark_bbox_dirty();
    }
}

void Runner::step_physics_and_paths() {
    const usize count = instances_.size();
    for (usize i = 0; i < count && i < instances_.size(); ++i) {
        Instance& inst = *instances_[i];
        if (inst.destroyed) continue;

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

        if (advance_instance_path(inst, game_)) {
            perform_event_on_instance(inst.handle, -1, 7, 8, inst.handle);
            if (pending_action_ != PendingRoomAction::None) return;
        }
        apply_instance_velocity(inst);
    }
}

void Runner::step_boundary_events() {
    // Outside Room, Boundary, all Outside View events, then all Boundary View events.
    // Recheck both the bounds and the instance for each event as handlers can move either.
    auto dispatch = [&](i32 sub, bool outside, const ActiveView* view) {
        for_event_instances(7, sub, [&](Instance& inst) {
            const f64 left = view ? view->xview : 0.0;
            const f64 top = view ? view->yview : 0.0;
            const f64 right = left + (view ? view->wview : room_width_);
            const f64 bottom = top + (view ? view->hview : room_height_);
            bool hit;
            if (instance_mask_sprite(inst, game_)) {
                ensure_instance_bbox(inst, game_);
                hit = outside ? (inst.bbox_right < left || inst.bbox_left > right ||
                                 inst.bbox_bottom < top || inst.bbox_top > bottom)
                              : (inst.bbox_left < left || inst.bbox_right > right ||
                                 inst.bbox_top < top || inst.bbox_bottom > bottom);
            } else {
                hit = inst.x < left || inst.x > right || inst.y < top || inst.y > bottom;
            }
            if (hit) perform_event_on_instance(inst.handle, -1, 7, sub, inst.handle);
        });
    };

    dispatch(0, true, nullptr);
    dispatch(1, false, nullptr);
    for (const i32 base : {40, 50}) {
        for (i32 v = 0; v < 8; ++v) {
            if (pending_action_ != PendingRoomAction::None) return;
            if (view_enabled_ && views_[v].visible) dispatch(base + v, base == 40, &views_[v]);
        }
    }
}

void Runner::step_collisions() {
    auto rollback = [](Instance& inst) {
        inst.x = inst.xprevious;
        inst.y = inst.yprevious;
        inst.path_position = inst.path_positionprevious;
        inst.mark_bbox_dirty();
    };
    const usize pair_count = event_tables().collision_pairs.size();
    for (usize p = 0; p < pair_count; ++p) {
        const auto& pairs = event_tables().collision_pairs;
        if (p >= pairs.size()) break;
        const auto [left, right] = pairs[p];
        // A game can ship millions of pairs. An empty left group cannot run
        // a callback that populates it before the next distinct left object.
        if (!object_exists(left) || object_instances(left, false).empty()) {
            const usize end = std::min(pair_count, pairs.size());
            while (p + 1 < end && pairs[p + 1].first == left) ++p;
            continue;
        }
        if (!object_exists(right) || object_instances(right, false).empty()) continue;
        for_object_instances(left, 0, [&](Instance& a, usize i) {
            // The right-hand count is sampled for each A. Its callback can
            // create an instance that will collide with the next A this frame.
            for_object_instances(right, left == right ? i + 1 : 0, [&](Instance& b, usize) {
                if (!check_instance_collision(a, a.x, a.y, b, game_, version_mode())) return;

                if (a.solid || b.solid) {
                    rollback(a);
                    rollback(b);
                }
                if (pending_action_ == PendingRoomAction::None) {
                    execute_instance_event(a.handle, a.object_index, 4, b.object_index, b.handle);
                }
                // Both events belong to this contact, even when the first
                // handler moves, destroys, or changes one of the participants.
                if (pending_action_ == PendingRoomAction::None) {
                    execute_instance_event(b.handle, b.object_index, 4, a.object_index, a.handle);
                }
                if (a.solid || b.solid) {
                    advance_instance_path(a, game_);
                    advance_instance_path(b, game_);
                    apply_instance_velocity(a);
                    apply_instance_velocity(b);
                    if (check_instance_collision(a, a.x, a.y, b, game_, version_mode())) {
                        rollback(a);
                        rollback(b);
                    }
                }
            });
        });
    }
}

void Runner::step_animations() {
    const usize instance_count = instances_.size();
    for (usize i = 0; i < instance_count && i < instances_.size(); ++i) {
        if (pending_action_ != PendingRoomAction::None) return;
        auto* inst = instances_[i].get();
        if (inst->destroyed) continue;
        const auto* sprite = instance_sprite(*inst, game_);
        const f64 count = sprite ? static_cast<f64>(sprite->images.size()) : 0.0;
        const f64 prev_idx = inst->image_index;
        inst->image_index += inst->image_speed;

        // GM subtracts image_number once, even when a sprite-less instance has zero frames.
        const bool ended = inst->image_index >= count;
        if (ended) inst->image_index -= count;

        if (std::floor(prev_idx) != std::floor(inst->image_index)) {
            const auto* mask = instance_mask_sprite(*inst, game_);
            if (mask && mask->separate_masks) inst->mark_bbox_dirty();
        }
        if (ended) {
            perform_event_on_instance(inst->handle, -1, 7, 7, inst->handle); // ev_other, ev_animation_end
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering & View / Mouse Projection
// ---------------------------------------------------------------------------

void Runner::room_canvas_size(i32& width, i32& height) const noexcept {
    width = std::max(1, room_width_);
    height = std::max(1, room_height_);
    if (view_enabled_) {
        width = 8;
        height = 8;
        for (const auto& v : views_) {
            if (!v.visible) continue;
            width = std::max(width, v.xport + v.wport);
            height = std::max(height, v.yport + v.hport);
        }
    }
}

void Runner::set_window_size(i32 width, i32 height) {
    if (window_region_scale_ > 0.0) {
        width = std::max(width, scaled_extent(window_region_w_, window_region_scale_));
        height = std::max(height, scaled_extent(window_region_h_, window_region_scale_));
    }
    if (window_.is_fullscreen() || width != window_.width() || height != window_.height()) {
        window_.set_size(width, height);
    }
}

void Runner::adapt_window_to_region(bool adapt) {
    if (window_region_scale_ > 0.0) {
        set_window_size(adapt ? 1 : window_.width(), adapt ? 1 : window_.height());
    }
}

void Runner::window_default() {
    const f64 scale = window_region_scale_ > 0.0 ? window_region_scale_ : 1.0;
    window_.set_size(scaled_extent(window_region_w_, scale), scaled_extent(window_region_h_, scale));
    window_.center();
}

void Runner::compute_canvas_viewport(i32& out_vx, i32& out_vy, i32& out_vw, i32& out_vh, i32& out_cw, i32& out_ch) const noexcept {
    room_canvas_size(out_cw, out_ch);
    const i32 cw = window_region_w_ > 0 ? window_region_w_ : out_cw;
    const i32 ch = window_region_h_ > 0 ? window_region_h_ : out_ch;
    const i32 dw = std::max(1, window_.drawable_width());
    const i32 dh = std::max(1, window_.drawable_height());

    i32 vw = dw, vh = dh;
    if (PlatformWindow::is_mobile_touch_platform() || window_region_scale_ < 0.0) {
        const f64 scale = std::min(static_cast<f64>(dw) / cw, static_cast<f64>(dh) / ch);
        vw = scaled_extent(cw, scale);
        vh = scaled_extent(ch, scale);
    } else if (window_region_scale_ > 0.0) {
        vw = scaled_extent(cw, window_region_scale_ * dw / std::max(1, window_.width()));
        vh = scaled_extent(ch, window_region_scale_ * dh / std::max(1, window_.height()));
    }
    out_vx = (dw - vw) / 2;
    out_vy = (dh - vh) / 2;
    out_vw = vw;
    out_vh = vh;
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

        const Instance* target = find_instance(find_matching_instance(v.follow_object, [](const Instance&) { return true; }));
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

    i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
    compute_canvas_viewport(vx, vy, vw, vh, cw, ch);

    // Always synchronize the logical screen region before rendering so surface_reset_target()
    // during post-draw composition (e.g. gm82dx9 application_surface) binds the right screen FBO.
    const i32 bw = requested_backbuffer_w_ > 0 ? requested_backbuffer_w_ : cw;
    const i32 bh = requested_backbuffer_h_ > 0 ? requested_backbuffer_h_ : ch;
    renderer_.set_screen_region(dw, dh, bw, bh, ViewportRect{vx, vy, vw, vh});

    const i32 user_surf = renderer_.current_surface_target();
    if (user_surf >= 0) {
        // Drawing into an active user surface target (e.g. gm82dx9 application_surface or screen_redraw thumbnails).
        const i32 sw = std::max(1, renderer_.surface_width(user_surf));
        const i32 sh = std::max(1, renderer_.surface_height(user_surf));
        if (!view_enabled_) {
            view_current_ = 0;
            render_view(0,
                        0.0f, 0.0f, static_cast<f32>(room_width_), static_cast<f32>(room_height_), 0.0f,
                        0, 0, sw, sh);
        } else {
            const f64 sx = static_cast<f64>(sw) / static_cast<f64>(cw);
            const f64 sy = static_cast<f64>(sh) / static_cast<f64>(ch);
            for (i32 vi = 0; vi < 8; ++vi) {
                const auto& v = views_[static_cast<usize>(vi)];
                if (!v.visible) continue;
                view_current_ = vi;
                const i32 px = static_cast<i32>(std::floor(static_cast<f64>(v.xport) * sx + 0.5));
                const i32 py = static_cast<i32>(std::floor(static_cast<f64>(v.yport) * sy + 0.5));
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
        renderer_.set_projection_ortho(0.0f, 0.0f, static_cast<f32>(sw), static_cast<f32>(sh),
                                       0.0f, 0, 0, sw, sh);
        update_window_caption();
        return;
    }

    renderer_.begin_frame(dw, dh);
    renderer_.begin_screen_pass(bw, bh, dw, dh, ViewportRect{vx, vy, vw, vh});

    const bool to_screen_fbo = renderer_.rendering_to_fbo();
    if (!to_screen_fbo) {
        // Fallback when screen FBO is unavailable: clear entire backbuffer with border color.
        gfx_.set_scissor(false);
        gfx_.set_viewport(ViewportRect{0, 0, dw, dh});
        renderer_.clear_color(window_color_, 1.0f);
    }

    if (!view_enabled_) {
        view_current_ = 0;
        if (to_screen_fbo) {
            render_view(0,
                        0.0f, 0.0f, static_cast<f32>(room_width_), static_cast<f32>(room_height_), 0.0f,
                        0, 0, cw, ch);
        } else {
            render_view(0,
                        0.0f, 0.0f, static_cast<f32>(room_width_), static_cast<f32>(room_height_), 0.0f,
                        vx, vy, vw, vh);
        }
    } else {
        const f64 sx = to_screen_fbo ? 1.0 : (static_cast<f64>(vw) / static_cast<f64>(cw));
        const f64 sy = to_screen_fbo ? 1.0 : (static_cast<f64>(vh) / static_cast<f64>(ch));
        const i32 ox = to_screen_fbo ? 0 : vx;
        const i32 oy = to_screen_fbo ? 0 : vy;
        for (i32 vi = 0; vi < 8; ++vi) {
            const auto& v = views_[static_cast<usize>(vi)];
            if (!v.visible) continue;
            view_current_ = vi;
            const i32 px = ox + static_cast<i32>(std::floor(static_cast<f64>(v.xport) * sx + 0.5));
            const i32 py = oy + static_cast<i32>(std::floor(static_cast<f64>(v.yport) * sy + 0.5));
            const i32 pw = std::max(1, static_cast<i32>(std::floor(static_cast<f64>(v.wport) * sx + 0.5)));
            const i32 ph = std::max(1, static_cast<i32>(std::floor(static_cast<f64>(v.hport) * sy + 0.5)));
            render_view(vi,
                        static_cast<f32>(v.xview), static_cast<f32>(v.yview),
                        static_cast<f32>(v.wview), static_cast<f32>(v.hview),
                        static_cast<f32>(v.angle),
                        px, py, pw, ph);
        }
        view_current_ = 0;
        const i32 rw = window_region_w_ > 0 ? window_region_w_ : cw;
        const i32 rh = window_region_h_ > 0 ? window_region_h_ : ch;
        const ViewportRect region = to_screen_fbo ? ViewportRect{0, 0, rw, rh} : ViewportRect{vx, vy, vw, vh};
        renderer_.set_view_projection(0.0f, 0.0f, static_cast<f32>(rw), static_cast<f32>(rh), 0.0f, region);
    }

    gfx_.set_scissor(false);
    update_window_caption();
}

void Runner::present_current_frame() {
    renderer_.present_screen(window_color_);
    touch_overlay_.render(window_.drawable_width(), window_.drawable_height());
    window_.swap_buffers();
    renderer_.end_frame();
}

void Runner::render_view(i32 /*view_idx*/, f32 vx, f32 vy, f32 vw, f32 vh, f32 angle, i32 px, i32 py, i32 pw, i32 ph) {
    const i32 dh = std::max(1, window_.drawable_height());
    const i32 gl_y = renderer_.rendering_to_fbo() ? py : (dh - (py + ph));
    gfx_.set_scissor(true, ViewportRect{px, gl_y, pw, ph});

    renderer_.set_view_projection(vx, vy, vw, vh, angle, {px, py, pw, ph});
    if (bg_showcolor_) {
        renderer_.clear_color(bg_color_, 1.0f);
    } else if (renderer_.d3d_active()) {
        renderer_.clear_depth(1.0f);
    }

    // 1. Non-foreground backgrounds (0..7)
    if (renderer_.d3d_active()) renderer_.d3d_set_depth(12000.0f);
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
    // A Draw event can call screen_redraw() before this traversal finishes.
    std::vector<RenderItem> items;
    items.swap(render_items_scratch_);
    items.reserve(instances_.size() + tiles_.size() + 8);

    for (usize i = 0; i < tiles_.size(); ++i) {
        if (!tiles_[i].visible) continue;
        items.push_back(RenderItem{tiles_[i].depth, 0, i, static_cast<i32>(i)});
    }
    for (usize i = 0; i < instances_.size(); ++i) {
        const auto& up = instances_[i];
        if (up->destroyed || up->deactivated || !up->visible) continue;
        items.push_back(RenderItem{up->depth, 1, i, up->handle});
    }
    const auto ps_list = particles_.auto_draw_systems();
    for (usize i = 0; i < ps_list.size(); ++i) {
        items.push_back(RenderItem{ps_list[i].second, 2, i, ps_list[i].first});
    }

    std::stable_sort(items.begin(), items.end(), [](const RenderItem& a, const RenderItem& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        // At equal depth GM draws particle systems, then instances, then tiles.
        if (a.kind != b.kind) return a.kind > b.kind;
        return a.order < b.order;
    });

    for (const auto& item : items) {
        if (renderer_.d3d_active()) {
            renderer_.d3d_set_depth(static_cast<f32>(std::min(item.depth, 11000.0)));
        }
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
                perform_event_on_instance(inst->handle, -1, 8, 0, inst->handle);
            } else {
                draw_instance_default(*inst);
            }
        } else {
            particles_.draw_system(item.ref_id, renderer_, game_);
        }
    }

    items.clear();
    items.swap(render_items_scratch_);

    // 3. Foreground backgrounds (0..7)
    if (renderer_.d3d_active()) renderer_.d3d_set_depth(-12000.0f);
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
        if (renderer_.d3d_active()) renderer_.d3d_set_depth(-13000.0f);
        renderer_.draw_sprite_ext(
            game_, cursor_sprite_, cursor_subimg_,
            static_cast<f32>(mouse_x_), static_cast<f32>(mouse_y_),
            1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
    }

    renderer_.flush();
    if (renderer_.d3d_active()) renderer_.d3d_set_depth(0.0f);
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
    const i32 score = static_cast<i32>(delphi_round(score_));
    if (show_score_ && (show_score_explicit_ || score > 0)) {
        cap += " " + caption_score_ + std::to_string(score);
    }
    if (show_lives_) {
        cap += " " + caption_lives_ + std::to_string(static_cast<i32>(delphi_round(lives_)));
    }
    if (show_health_) {
        cap += " " + caption_health_ + std::to_string(delphi_round(health_));
    }
    window_.set_title(codepage_to_utf8(cap, game_.meta.string_codepage()));
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

i32 Runner::instance_handle(i32 id) const noexcept {
    if (id < 0) return id;
    if (const auto* inst = find_instance(id)) return inst->handle;
    return id;
}

const std::vector<Instance*>& Runner::object_instances(i32 object, bool include_descendants) const noexcept {
    static const std::vector<Instance*> empty;
    if (object < 0 || static_cast<usize>(object) >= object_instances_.size()) return empty;
    const auto& group = object_instances_[static_cast<usize>(object)];
    return include_descendants ? group.recursive : group.exact;
}

void Runner::index_instance(Instance& inst) {
    if (object_instances_.size() < game_.objects.size()) object_instances_.resize(game_.objects.size());
    i32 object = inst.object_index;
    if (!object_exists(object)) return;
    object_instances_[static_cast<usize>(object)].exact.push_back(&inst);
    while (object_exists(object)) {
        auto& group = object_instances_[static_cast<usize>(object)].recursive;
        if (!group.empty() && group.back() == &inst) break; // Invalid cyclic parent chain.
        group.push_back(&inst);
        object = game_.objects[static_cast<usize>(object)].parent_index;
    }
}

void Runner::rebuild_object_instances() {
    for (auto& group : object_instances_) {
        group.exact.clear();
        group.recursive.clear();
    }
    for (const auto& inst : instances_) index_instance(*inst);
}

void Runner::register_instance_handle(Instance& inst) {
    do {
        inst.handle = next_handle_;
        next_handle_ = next_handle_ == std::numeric_limits<i32>::min() ? -8 : next_handle_ - 1;
    } while (inst_by_id_.contains(inst.handle));
    inst_by_id_[inst.handle] = &inst;
}

void Runner::add_room_instance(std::unique_ptr<Instance> inst) {
    register_instance_handle(*inst);
    inst_by_id_[inst->id] = inst.get();
    if (inst->deactivated) {
        inactive_instances_.push_back(std::move(inst));
    } else {
        index_instance(*inst);
        instances_.push_back(std::move(inst));
    }
}

void Runner::set_object_parent(i32 object, i32 parent) {
    if (!object_exists(object)) return;
    if (parent >= 0 && (!object_exists(parent) || parent == object || object_is_ancestor(parent, object))) return;
    game_.objects[static_cast<usize>(object)].parent_index = parent < 0 ? -1 : parent;
    // GM rebuilds the event tables immediately. Existing instances keep their
    // recursive memberships until the next room-list rebuild.
    invalidate_event_tables();
}

bool Runner::object_exists(i32 obj_idx) const noexcept {
    return obj_idx >= 0 && static_cast<usize>(obj_idx) < game_.objects.size() &&
           game_.objects[static_cast<usize>(obj_idx)].exists;
}

bool Runner::object_is_ancestor(i32 child_obj, i32 parent_obj) const noexcept {
    if (!object_exists(child_obj) || !object_exists(parent_obj)) return false;
    i32 cur = game_.objects[static_cast<usize>(child_obj)].parent_index;
    for (usize depth = 0; depth < game_.objects.size() && object_exists(cur); ++depth) {
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

void Runner::set_instance_object(Instance& inst, i32 object) const {
    inst.object_index = object;
    const auto* obj = object_exists(object) ? &game_.objects[static_cast<usize>(object)] : nullptr;
    // With a missing object slot, GM leaves the instance's object properties zeroed.
    inst.sprite_index = obj ? obj->sprite_index : 0;
    inst.mask_index = obj ? obj->mask_index : 0;
    inst.depth = obj ? static_cast<f64>(obj->depth) : 0.0;
    inst.solid = obj && obj->solid;
    inst.visible = obj && obj->visible;
    inst.persistent = obj && obj->persistent;
    inst.mark_bbox_dirty();
}

i32 Runner::create_instance(f64 x, f64 y, i32 obj_idx, bool run_create, i32 forced_id) {
    if (!object_exists(obj_idx)) {
        return -4; // noone
    }
    auto inst = std::make_unique<Instance>();
    inst->id = (forced_id >= 100000) ? forced_id : next_instance_id_++;
    if (inst->id >= next_instance_id_) {
        next_instance_id_ = inst->id + 1;
    }
    set_instance_object(*inst, obj_idx);
    inst->x = x;
    inst->y = y;
    inst->xprevious = x;
    inst->yprevious = y;
    inst->xstart = x;
    inst->ystart = y;

    const i32 id = inst->id;
    add_room_instance(std::move(inst));

    if (run_create) {
        perform_event_on_instance(id, -1, 0, 0, id); // ev_create
    }
    return id;
}

void Runner::destroy_instance(i32 inst_id, bool perform_event) {
    auto* inst = find_instance(inst_id);
    if (!inst || inst->destroyed) return;
    if (perform_event) {
        perform_event_on_instance(inst->handle, -1, 1, 0, inst->handle); // ev_destroy
    }
    inst->destroyed = true;
}

void Runner::change_instance(i32 inst_id, i32 new_obj_idx, bool perform_events) {
    auto* inst = find_instance(inst_id);
    if (!inst || inst->destroyed || !object_exists(new_obj_idx)) return;
    if (perform_events) {
        perform_event_on_instance(inst->handle, -1, 1, 0, inst->handle);
    }
    // Native AddInstance appends a new slot. Assign copies the public ID; the
    // current GML frame and its other still refer to the old, marked slot.
    auto changed = std::make_unique<Instance>(*inst);
    set_instance_object(*changed, new_obj_idx);
    ++next_instance_id_;
    const i32 id = inst->id;
    inst->destroyed = true;
    add_room_instance(std::move(changed));
    if (perform_events) perform_event_on_instance(id, -1, 0, 0, id);
}

i32 Runner::copy_instance(i32 src_id, bool perform_event) {
    const auto* src = find_instance(src_id);
    if (!src || src->destroyed) return -4;
    auto copy = std::make_unique<Instance>(*src);
    copy->id = next_instance_id_++;
    copy->mark_bbox_dirty();

    const i32 new_id = copy->id;
    add_room_instance(std::move(copy));

    if (perform_event) {
        perform_event_on_instance(new_id, -1, 0, 0, new_id);
    }
    return new_id;
}

void Runner::remove_marked_instances() {
    auto marked = [this](const auto& inst) {
        if (!inst->destroyed) return false;
        inst_by_id_.erase(inst->handle);
        if (find_instance(inst->id) == inst.get()) inst_by_id_.erase(inst->id);
        return true;
    };
    if (std::erase_if(instances_, marked) != 0) rebuild_object_instances();
}

void Runner::move_deactivated(bool deactivate) {
    auto& from = deactivate ? instances_ : inactive_instances_;
    auto& to = deactivate ? inactive_instances_ : instances_;
    const usize previous_size = to.size();
    for (auto& inst : from) {
        if (inst->deactivated == deactivate) to.push_back(std::move(inst));
    }
    if (to.size() != previous_size) {
        std::erase(from, nullptr);
        rebuild_object_instances();
    }
}

void Runner::deactivate_all(bool notme, i32 self_id) {
    const auto* self = find_instance(self_id);
    for (auto& inst : instances_) {
        if (!notme || inst.get() != self) inst->deactivated = true;
    }
    move_deactivated(true);
}

void Runner::deactivate_object(i32 target, i32 self_id, i32 other_id) {
    if (target == -3) {
        deactivate_all(false, self_id);
        return;
    }
    if (target == -1 || target == -2) {
        if (auto* inst = find_instance(target == -1 ? self_id : other_id)) inst->deactivated = true;
    } else if (target >= 100000) {
        // During instance_change several room slots can carry the same public ID.
        for (auto& inst : instances_) {
            if (inst->id == target) inst->deactivated = true;
        }
    } else {
        for (auto* inst : object_instances(target, true)) inst->deactivated = true;
    }
    move_deactivated(true);
}

void Runner::deactivate_region(f64 left, f64 top, f64 w, f64 h, bool inside, bool notme, i32 self_id) {
    const auto* self = find_instance(self_id);
    for (auto& inst : instances_) {
        if (notme && inst.get() == self) continue;
        ensure_instance_bbox(*inst, game_);
        const bool overlaps = inst->bbox_right >= left && inst->bbox_left < left + w &&
                              inst->bbox_bottom >= top && inst->bbox_top < top + h;
        if (overlaps == inside) inst->deactivated = true;
    }
    move_deactivated(true);
}

void Runner::activate_all() {
    for (auto& inst : inactive_instances_) inst->deactivated = false;
    move_deactivated(false);
}

void Runner::activate_object(i32 target, i32 self_id, i32 other_id) {
    if (target == -3) {
        activate_all();
        return;
    }
    if (target == -1 || target == -2) {
        if (auto* inst = find_instance(target == -1 ? self_id : other_id)) inst->deactivated = false;
    } else if (target >= 100000) {
        for (auto& inst : inactive_instances_) {
            if (inst->id == target) inst->deactivated = false;
        }
    } else {
        for (auto& inst : inactive_instances_) {
            if (object_matches(inst->object_index, target)) inst->deactivated = false;
        }
    }
    move_deactivated(false);
}

void Runner::activate_region(f64 left, f64 top, f64 w, f64 h, bool inside) {
    for (auto& inst : inactive_instances_) {
        ensure_instance_bbox(*inst, game_);
        const bool overlaps = inst->bbox_right >= left && inst->bbox_left < left + w &&
                              inst->bbox_bottom >= top && inst->bbox_top < top + h;
        if (overlaps == inside) inst->deactivated = false;
    }
    move_deactivated(false);
}

bool Runner::broadcast_event(i32 category, i32 sub) {
    bool handled = false;
    const usize count = instances_.size();
    for (usize i = 0; i < count && i < instances_.size(); ++i) {
        if (pending_action_ != PendingRoomAction::None) break;
        const auto& inst = *instances_[i];
        if (!inst.destroyed && execute_instance_event(inst.handle, inst.object_index, category, sub, inst.handle)) {
            handled = true;
        }
    }
    return handled;
}

const BytecodeChunk* Runner::resolve_event_chunk(i32 obj_idx, i32 cat, i32 sub, i32& out_owner_obj) const noexcept {
    if (cat < 0 || cat >= 12) return nullptr;
    // For collisions GM first searches the owner's hierarchy for the exact
    // target, then repeats that search for each parent of the target.
    for (usize target_depth = 0; target_depth < game_.objects.size(); ++target_depth) {
        i32 cur = obj_idx;
        for (usize depth = 0; depth < game_.objects.size() && object_exists(cur); ++depth) {
            if (static_cast<usize>(cur) < program_.object_event_chunks.size()) {
                const auto& ev_map = program_.object_event_chunks[static_cast<usize>(cur)][static_cast<usize>(cat)];
                if (auto it = ev_map.find(sub); it != ev_map.end()) {
                    out_owner_obj = cur;
                    return &it->second;
                }
            }
            cur = game_.objects[static_cast<usize>(cur)].parent_index;
        }
        if (cat != 4 || !object_exists(sub)) break;
        sub = game_.objects[static_cast<usize>(sub)].parent_index;
        if (!object_exists(sub)) break;
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
    execute_instance_event(inst_id, start_obj, cat, sub, other_id);
}

bool Runner::execute_instance_event(i32 inst_id, i32 obj_idx, i32 cat, i32 sub, i32 other_id) {
    i32 owner_obj = -1;
    const BytecodeChunk* chunk = resolve_event_chunk(obj_idx, cat, sub, owner_obj);
    if (!chunk || chunk->code.empty()) return false;

    const bool saved_rel = action_relative_;
    action_relative_ = false;
    inst_id = instance_handle(inst_id);
    other_id = instance_handle(other_id);
    event_stack_.push_back(EventFrame{cat, sub, owner_obj, inst_id, other_id, 0});

    auto res = vm_->execute(*chunk, inst_id, other_id);
    if (!res && running_) {
        error_occurred_ = true;
        error_last_ = res.error_message();
        GM_LOG_WARN("runtime", "Event error (%s): %s", chunk->debug_name.c_str(), error_last_.c_str());
    }

    event_stack_.pop_back();
    action_relative_ = saved_rel;
    return true;
}

void Runner::perform_event_inherited(i32 self_id) {
    if (event_stack_.empty()) return;
    const EventFrame cur = event_stack_.back();
    if (!object_exists(cur.owner_obj)) return;
    const i32 parent = game_.objects[static_cast<usize>(cur.owner_obj)].parent_index;
    if (!object_exists(parent)) return;

    execute_instance_event(self_id, parent, cur.cat, cur.sub, cur.other_id);
}

// ---------------------------------------------------------------------------
// Collision & Movement Helpers
// ---------------------------------------------------------------------------

i32 Runner::find_collision_instance(
    const Instance& self, f64 test_x, f64 test_y, i32 target, bool solid_only,
    const Instance* context_other) const noexcept {
    return find_matching_instance(target, [&](const Instance& inst) {
        return &inst != &self && (!solid_only || inst.solid) &&
               check_instance_collision(self, test_x, test_y, inst, game_, version_mode());
    }, nullptr, context_other);
}

bool Runner::check_place_free(const Instance& self, f64 test_x, f64 test_y) const noexcept {
    return find_collision_instance(self, test_x, test_y, -3, /*solid_only=*/true) == -4;
}

bool Runner::check_place_empty(const Instance& self, f64 test_x, f64 test_y) const noexcept {
    return find_collision_instance(self, test_x, test_y, -3, /*solid_only=*/false) == -4;
}

bool Runner::check_place_meeting(const Instance& self, f64 test_x, f64 test_y, i32 target,
                                 const Instance* context_other) const noexcept {
    return find_collision_instance(self, test_x, test_y, target, /*solid_only=*/false, context_other) != -4;
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
    if (p.length() <= 0.0) {
        inst.path_index = -1;
        return;
    }
    inst.path_index = path_id;
    inst.path_speed = speed;
    inst.path_endaction = endaction;
    inst.path_position = (speed >= 0.0) ? 0.0 : 1.0;
    inst.path_positionprevious = inst.path_position;
    inst.path_scale = 1.0;
    inst.path_orientation = 0.0;
    if (absolute) {
        const auto start = p.sample(inst.path_position);
        inst.x = start.x;
        inst.y = start.y;
        inst.mark_bbox_dirty();
    }
    inst.path_xstart = inst.x;
    inst.path_ystart = inst.y;
}

void Runner::end_path(Instance& inst) {
    inst.path_index = -1;
}

// ---------------------------------------------------------------------------
// IVmHost Implementation (All 166 Built-in Variables & Calls)
// ---------------------------------------------------------------------------

VariableTable* Runner::get_instance_vars(i32 inst_id) {
    auto* inst = find_instance(inst_id);
    return inst ? &inst->vars : nullptr;
}

usize Runner::target_instance_count(i32 target) const {
    if (target == -3) return instances_.size();
    if (target < 0) return 0;
    if (target >= 100000) return 1;
    return object_instances(target, true).size();
}

const Instance* Runner::field_target_by_id(i32 id, TargetUse use) const {
    // GM8 shares these two room slots between field reads and writes. A write can
    // hit the old, destroyed slot immediately after instance_change reused its ID.
    for (usize index : cached_field_indices_) {
        if (index >= instances_.size()) continue;
        const auto* inst = instances_[index].get();
        if (inst->id == id && (use == TargetUse::WriteField || !inst->destroyed)) return inst;
    }
    cached_field_indices_[1] = cached_field_indices_[0];
    for (usize i = instances_.size(); i > 0; --i) {
        if (instances_[i - 1]->id == id) {
            cached_field_indices_[0] = i - 1;
            return instances_[i - 1].get();
        }
    }
    return nullptr;
}

i32 Runner::target_instance_at(i32 target, usize index, TargetUse use) const {
    const Instance* instance = nullptr;
    if (target == -3) {
        if (index < instances_.size()) instance = instances_[index].get();
    } else if (target >= 100000) {
        if (index != 0) return -4;
        if (use == TargetUse::With) {
            for (const auto& inst : instances_) {
                if (inst->id == target) {
                    instance = inst.get();
                    break;
                }
            }
        } else {
            instance = field_target_by_id(target, use);
        }
    } else if (target >= 0) {
        const auto& members = object_instances(target, true);
        if (index < members.size()) instance = members[index];
    }
    return instance && !instance->destroyed && !instance->deactivated ? instance->handle : -4;
}

Result<RValue> Runner::get_builtin_global(u32 var_id, i32 idx, bool has_idx) {
    const i32 i8 = has_idx && idx >= 0 && idx < 8 ? idx : 0;
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
        case 30u: return RValue(vfs_.program_directory_gm());
        case 31u: return RValue(static_cast<i32>(instances_.size()));
        case 32u: {
            const i32 target_i = has_idx ? idx : 0;
            return RValue(target_i >= 0 && static_cast<usize>(target_i) < instances_.size()
                              ? instances_[static_cast<usize>(target_i)]->id : -4);
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
    const i32 i8 = has_idx && idx >= 0 && idx < 8 ? idx : 0;
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
        case 23u: transition_steps_ = std::max(1, value.as_i32()); return Result<void>::success();
        case 24u:
            score_ = value.as_real();
            return Result<void>::success();
        case 25u: {
            const f64 prev = lives_;
            lives_ = value.as_real();
            if (lives_ > 0.0) no_more_lives_armed_ = true;
            if (no_more_lives_armed_ && prev > 0.0 && lives_ <= 0.0) {
                no_more_lives_armed_ = false;
                broadcast_event(7, 6); // ev_no_more_lives
            }
            return Result<void>::success();
        }
        case 26u: {
            const f64 prev = health_;
            health_ = value.as_real();
            if (health_ > 0.0) no_more_health_armed_ = true;
            if (no_more_health_armed_ && prev > 0.0 && health_ <= 0.0) {
                no_more_health_armed_ = false;
                broadcast_event(7, 9); // ev_no_more_health
            }
            return Result<void>::success();
        }
        case 35u:
            room_caption_ = value.is_string() ? value.as_string() : "";
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
        // GM stores the view rectangle in integer pixels on each GML assignment.
        case 58u: views_[static_cast<usize>(i8)].xview = value.as_i32(); return Result<void>::success();
        case 59u: views_[static_cast<usize>(i8)].yview = value.as_i32(); return Result<void>::success();
        case 60u: views_[static_cast<usize>(i8)].wview = value.as_i32(); return Result<void>::success();
        case 61u: views_[static_cast<usize>(i8)].hview = value.as_i32(); return Result<void>::success();
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
        case 81u: show_score_ = value.as_i64() > 0; show_score_explicit_ = true; return Result<void>::success();
        case 82u: show_lives_ = value.as_i64() > 0; return Result<void>::success();
        case 83u: show_health_ = value.as_i64() > 0; return Result<void>::success();
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

    const SpriteData* sp = instance_sprite(*inst, game_);

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

Result<RValue> Runner::ext_call_function(i32 address, i32 self, i32 other, std::span<const RValue> args) {
    const auto* func = ExtensionRegistry::callback_by_address(address);
    if (!func) return Error{"Invalid GM function callback"};
    return call_builtin(func->id, self, other, args);
}

void Runner::ext_resize_backbuffer(i32 width, i32 height) {
    width = std::min(width, window_.display_width());
    height = std::min(height, window_.display_height());
    if (width > 0 && height > 0 && renderer_.resize_backbuffer(width, height)) {
        requested_backbuffer_w_ = width;
        requested_backbuffer_h_ = height;
    }
}

i32 Runner::ext_game_get_state() const noexcept {
    switch (pending_action_) {
        case PendingRoomAction::None:        return -1; // gs_running
        case PendingRoomAction::EndGame:     return -100;
        case PendingRoomAction::RestartGame: return -200;
        case PendingRoomAction::LoadGame:    return -300;
        case PendingRoomAction::RestartRoom: return current_room_;
        case PendingRoomAction::Goto:        return pending_room_id_;
    }
    return -1;
}

std::string Runner::ext_var_name_get(i32 index) const {
    if (index < 0 || static_cast<usize>(index) >= program_.symbols.user_var_count()) {
        return {};
    }
    return std::string(program_.symbols.user_var_name(kUserVarIdBase + static_cast<u32>(index)));
}

std::vector<std::string> Runner::ext_instance_var_names(i32 inst_id) const {
    std::vector<std::string> out;
    const auto* inst = find_instance(inst_id);
    if (!inst) return out;
    const auto ids = inst->vars.all_var_ids();
    out.reserve(ids.size());
    for (u32 vid : ids) {
        if (vid >= kUserVarIdBase) {
            const std::string_view name = program_.symbols.user_var_name(vid);
            if (!name.empty()) out.emplace_back(name);
        }
    }
    return out;
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

Result<RValue> Runner::call_extension_dll(u32 ext_dll_id, std::span<const RValue> args) {
    return extensions_.call_gex_bound(*this, ext_dll_id, args);
}

// ---------------------------------------------------------------------------
// RNG Helpers
// ---------------------------------------------------------------------------

void Runner::set_random_seed(u32 seed) noexcept {
    rng_.seed = seed;
}

f64 Runner::random_uniform(f64 upper) noexcept {
    return static_cast<f64>(rng_.next()) * 0x1p-32 * upper;
}

i64 Runner::random_int_inclusive(i64 upper) noexcept {
    return static_cast<i32>(rng_.below(static_cast<u32>(upper) + 1u));
}

// ---------------------------------------------------------------------------
// Text File, Binary File, INI & File Find Managers
// ---------------------------------------------------------------------------

i32 Runner::text_file_open(std::string_view gm_path, i32 mode) {
    TextFileHandle tf{};
    tf.open = true;
    tf.mode = mode;
    tf.gm_path = vfs_.absolute_path_gm(gm_path);

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
    bf.gm_path = vfs_.absolute_path_gm(gm_path);

    if (auto bytes = vfs_.read_all_bytes(gm_path); bytes) {
        bf.data = std::move(bytes.value());
    } else {
        if (vfs_.exists(gm_path) || !vfs_.write_all_bytes(gm_path, {})) return -1;
        // GM opens a newly created binary file for both reading and writing.
        bf.mode = 2;
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
    // GM8.1/8.2 pass the supplied name to TMemIniFile and resolve it again at close.
    ini_.gm_path = version_mode() == VersionMode::GM80 ? vfs_.absolute_path_gm(gm_path) : std::string(gm_path);
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

// ---------------------------------------------------------------------------
// Event-Pumping Sleep & Input Wait Helpers
// ---------------------------------------------------------------------------

bool Runner::sleep_ms(f64 ms) {
    if (ms <= 0.0) return true;
    window_.input_mut().begin_frame();

    const u64 dur_us = static_cast<u64>(ms * 1000.0);
    const u64 end_us = Clock::now_us() + dur_us;
    while (true) {
        const WindowEvents ev = window_.poll_events_accumulate();
        update_mouse_room_coords();
        if (host_stop_requested_ || ev.quit_requested) {
            running_ = false;
            pending_action_ = PendingRoomAction::EndGame;
            return false;
        }
        const u64 now = Clock::now_us();
        if (now >= end_us) break;
        Clock::sleep_until_us(std::min(end_us, now + 4000u));
    }
    return true;
}

bool Runner::wait_for_key() {
    auto& inp = window_.input_mut();
    inp.key_pressed.fill(false);
    inp.key_released.fill(false);
    while (true) {
        const WindowEvents ev = window_.poll_events_accumulate();
        if (host_stop_requested_ || ev.quit_requested) {
            running_ = false;
            pending_action_ = PendingRoomAction::EndGame;
            return false;
        }
        if (touch_overlay_.update(window_)) {
            present_current_frame();
            Clock::sleep_until_us(Clock::now_us() + 16000u);
            continue;
        }
        update_mouse_room_coords();
        for (usize k = 1; k < 256; ++k) {
            if (window_.input().key_pressed[k]) return true;
        }
        Clock::sleep_until_us(Clock::now_us() + 4000u);
    }
}

bool Runner::wait_for_mouse() {
    auto& inp = window_.input_mut();
    inp.mouse_pressed.fill(false);
    inp.mouse_released.fill(false);
    while (true) {
        const WindowEvents ev = window_.poll_events_accumulate();
        if (host_stop_requested_ || ev.quit_requested) {
            running_ = false;
            pending_action_ = PendingRoomAction::EndGame;
            return false;
        }
        if (touch_overlay_.update(window_)) {
            present_current_frame();
            Clock::sleep_until_us(Clock::now_us() + 16000u);
            continue;
        }
        update_mouse_room_coords();
        if (window_.input().mouse_pressed[1] ||
            window_.input().mouse_pressed[2] ||
            window_.input().mouse_pressed[3]) {
            return true;
        }
        Clock::sleep_until_us(Clock::now_us() + 4000u);
    }
}

// ---------------------------------------------------------------------------
// Cross-Platform Self-Drawn GM8 Modal Message & Input Dialogs
// ---------------------------------------------------------------------------

std::string Runner::decode_dialog_string(std::string_view raw, u8 charset) const {
    if (game_.meta.encoding == EncodingMode::Utf8) {
        return std::string(raw);
    }
    const u32 cp = gdi_charset_to_codepage(charset, game_.meta.string_codepage());
    return codepage_to_utf8(raw, cp);
}

std::string Runner::encode_dialog_string(std::string_view utf8, u8 charset) const {
    if (game_.meta.encoding == EncodingMode::Utf8) {
        return std::string(utf8);
    }
    const u32 cp = gdi_charset_to_codepage(charset, game_.meta.string_codepage());
    return utf8_to_codepage(utf8, cp);
}

void Runner::run_message_dialog_modal(bool with_input) {
    if (!window_.is_open()) {
        return;
    }

    const i32 prev_cursor = window_.cursor();
    const bool prev_text_input = window_.is_text_input_active();
    window_.set_cursor(0);
    if (with_input) {
        window_.set_text_input(true);
    }
    window_.reset_modal_input_state();
    renderer_.flush();

    const bool mobile_touch = PlatformWindow::is_mobile_touch_platform();
#if defined(__EMSCRIPTEN__)
    const bool web_editor = with_input && mobile_touch;
    if (web_editor) message_dialog_.begin_web_editor();
#endif
    ModalInputFrame minput{};

    while (!host_stop_requested_) {
        const u64 frame_start = Clock::now_us();
        window_.poll_modal_events(minput);
        const auto& wev = minput.window_events;

        if (wev.entered_background) {
            paused_in_background_ = true;
            audio_.suspend();
            vfs_.notify_persist_write();
        }
        if (wev.entered_foreground) {
            paused_in_background_ = false;
            audio_.resume();
        }
        if (wev.context_lost) {
            if (auto res = gfx_.reinit_after_context_loss(&PlatformWindow::gl_get_proc_address); res) {
                (void)renderer_.reload_gpu_resources(game_);
                (void)touch_overlay_.reload_gpu_resources();
                (void)message_dialog_.reload_gpu_resources();
                (void)modal_dialogs_.reload_gpu_resources();
            }
        }
#if defined(__EMSCRIPTEN__)
        if (wev.any_user_gesture && !web_audio_unlocked_) {
            web_audio_unlocked_ = true;
            web_unlock_audio();
            audio_.resume();
        }
#endif
        if (wev.quit_requested) {
            running_ = false;
            pending_action_ = PendingRoomAction::EndGame;
            break;
        }

        const i32 ww = std::max(1, window_.width());
        const i32 wh = std::max(1, window_.height());
        const i32 dw = std::max(1, window_.drawable_width());
        const i32 dh = std::max(1, window_.drawable_height());

        if (wev.resized) {
            i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
            compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
            const i32 bw = requested_backbuffer_w_ > 0 ? requested_backbuffer_w_ : cw;
            const i32 bh = requested_backbuffer_h_ > 0 ? requested_backbuffer_h_ : ch;
            renderer_.set_screen_region(dw, dh, bw, bh, ViewportRect{vx, vy, vw, vh});
        }

        const i32 wx = window_.x();
        const i32 wy = window_.y();
        const f32 dscale = window_.display_scale();

#if defined(__EMSCRIPTEN__)
        if (web_editor) message_dialog_.poll_web_editor(minput);
#endif
        const bool keep_open = message_dialog_.update(
            minput, ww, wh, dw, dh, wx, wy, dscale, mobile_touch, frame_start);

        if (with_input && message_dialog_.consume_text_input_focus_request()) {
            window_.set_text_input(true);
        }
        if (with_input && keep_open) {
            i32 ix = 0, iy = 0, iw = 0, ih = 0, icaret = 0;
            if (message_dialog_.get_input_window_rect(ww, wh, dw, dh, wx, wy, dscale, mobile_touch,
                                                      ix, iy, iw, ih, icaret)) {
#if defined(__EMSCRIPTEN__)
                if (web_editor) web_position_dialog_editor(ix, iy, iw, ih, ww, wh);
#endif
                window_.set_text_input_area(ix, iy, iw, ih, icaret);
            }
        }

        if (!paused_in_background_) {
            if (renderer_.has_screen_frame()) {
                renderer_.present_screen(window_color_);
            } else {
                const auto saved = gfx_.save_render_state();
                gfx_.bind_framebuffer(FramebufferHandle{});
                gfx_.set_scissor(false);
                gfx_.set_viewport(ViewportRect{0, 0, dw, dh});
                const ColorRGBA c = gm_bgr_to_rgba(window_color_, 255);
                gfx_.clear(true, false, false,
                           static_cast<f32>(c.r) / 255.0f,
                           static_cast<f32>(c.g) / 255.0f,
                           static_cast<f32>(c.b) / 255.0f,
                           1.0f);
                gfx_.restore_render_state(saved);
            }
            if (keep_open) {
                message_dialog_.render(ww, wh, dw, dh, wx, wy, dscale, mobile_touch, frame_start);
            }
            window_.swap_buffers();
            renderer_.end_frame();
        }

        if (!keep_open) {
            break;
        }
#if defined(__EMSCRIPTEN__)
        // Even an over-budget frame must return control to the browser for input.
        const u64 elapsed_ms = (Clock::now_us() - frame_start) / 1000;
        Clock::yield_ms(elapsed_ms < 16 ? static_cast<u32>(16 - elapsed_ms) : 1);
#else
        Clock::sleep_until_us(frame_start + 16000u);
#endif
    }

#if defined(__EMSCRIPTEN__)
    if (web_editor) web_end_dialog_editor();
#endif
    if (with_input && !prev_text_input) {
        window_.set_text_input(false);
    }
    window_.set_cursor(prev_cursor);

    // Match GM8 sub_5F6964: clear all keyboard/mouse state so dismissing the dialog
    // never leaks clicks or key presses into the next game step.
    window_.input_mut().clear_all();
    mouse_button_ = 0;
    mouse_lastbutton_ = 0;
}

void Runner::show_message_dialog(std::string_view text) {
    (void)show_message_ext_dialog(text, "", "OK", "");
}

i32 Runner::show_message_ext_dialog(
    std::string_view text,
    std::string_view but1,
    std::string_view but2,
    std::string_view but3) {
    const auto& st = message_dialog_.style();
    message_dialog_.begin_message(
        decode_dialog_string(text, st.text_font.charset),
        decode_dialog_string(but1, st.button_font.charset),
        decode_dialog_string(but2, st.button_font.charset),
        decode_dialog_string(but3, st.button_font.charset));
    run_message_dialog_modal(false);
    return message_dialog_.result_button();
}

bool Runner::show_question_dialog(std::string_view text) {
    return show_message_ext_dialog(text, "Yes", "", "No") == 1;
}

std::string Runner::get_string_dialog(std::string_view text, std::string_view def) {
    const auto& st = message_dialog_.style();
    message_dialog_.begin_input(
        decode_dialog_string(text, st.text_font.charset),
        decode_dialog_string(def, st.input_font.charset));
    run_message_dialog_modal(true);
    if (message_dialog_.result_button() == 0) return std::string(def);
    return encode_dialog_string(message_dialog_.result_text_utf8(), st.input_font.charset);
}

f64 Runner::get_integer_dialog(std::string_view text, f64 def) {
    const i64 def_int = static_cast<i64>(delphi_round(def));
    const std::string def_str = std::to_string(def_int);
    const std::string raw = get_string_dialog(text, def_str);

    if (message_dialog_.result_button() == 0) return def;

    // Match Delphi SysUtils.StrToInt semantics: optional leading/trailing whitespace,
    // optional +/-, decimal or $/0x hex integer; invalid input falls back to round(def).
    usize start = 0;
    usize end = raw.size();
    while (start < end && (raw[start] == ' ' || raw[start] == '\t' || raw[start] == '\r' || raw[start] == '\n')) {
        ++start;
    }
    while (end > start && (raw[end - 1] == ' ' || raw[end - 1] == '\t' || raw[end - 1] == '\r' || raw[end - 1] == '\n')) {
        --end;
    }
    if (start >= end) {
        return static_cast<f64>(def_int);
    }

    std::string_view sv(raw.data() + start, end - start);
    bool negative = false;
    if (sv.front() == '+' || sv.front() == '-') {
        negative = (sv.front() == '-');
        sv.remove_prefix(1);
    }
    if (sv.empty()) {
        return static_cast<f64>(def_int);
    }

    int base = 10;
    if (sv.front() == '$') {
        base = 16;
        sv.remove_prefix(1);
    } else if (sv.size() >= 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) {
        base = 16;
        sv.remove_prefix(2);
    }
    if (sv.empty()) {
        return static_cast<f64>(def_int);
    }

    i64 value = 0;
    for (char c : sv) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
        else return static_cast<f64>(def_int);
        if (value > (INT64_MAX - digit) / base) {
            return static_cast<f64>(def_int);
        }
        value = value * base + digit;
    }
    return static_cast<f64>(negative ? -value : value);
}

// ---------------------------------------------------------------------------
// Native File & Directory Picker Dialogs (SDL3)
// ---------------------------------------------------------------------------

namespace {

std::string resolve_dialog_default_location(const Vfs& vfs, std::string_view raw_path, u32 codepage, bool is_dir) {
    if (raw_path.empty()) {
        std::string dir = Vfs::path_to_utf8(vfs.resolve_for_read("."));
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
#if defined(_WIN32)
            dir.push_back('\\');
#else
            dir.push_back('/');
#endif
        }
        return dir;
    }

    const std::string utf8_in = codepage_to_utf8(raw_path, codepage);
    const std::filesystem::path p = Vfs::path_from_utf8(utf8_in);
    if (p.is_absolute()) {
        return utf8_in;
    }

    std::string resolved = Vfs::path_to_utf8(is_dir ? vfs.resolve_for_read(raw_path) : vfs.resolve_for_write(raw_path));
    if (is_dir && !resolved.empty() && resolved.back() != '/' && resolved.back() != '\\') {
#if defined(_WIN32)
        resolved.push_back('\\');
#else
        resolved.push_back('/');
#endif
    }
    return resolved;
}

} // namespace

std::string Runner::get_open_filename_dialog(std::string_view filter, std::string_view fname) {
    const u32 cp = game_.meta.string_codepage();
    const std::string filter_utf8 = codepage_to_utf8(filter, cp);
    const std::string def_loc = resolve_dialog_default_location(vfs_, fname, cp, false);

    const i32 prev_cursor = window_.cursor();
    window_.set_cursor(0);
    renderer_.flush();
    std::string selected = window_.open_file_dialog(filter_utf8, def_loc);
    window_.set_cursor(prev_cursor);
    window_.input_mut().clear_all();
    mouse_button_ = 0;
    mouse_lastbutton_ = 0;

    if (selected.empty()) return {};
    return (game_.meta.encoding == EncodingMode::Utf8) ? selected : utf8_to_codepage(selected, cp);
}

std::string Runner::get_save_filename_dialog(std::string_view filter, std::string_view fname) {
    const u32 cp = game_.meta.string_codepage();
    const std::string filter_utf8 = codepage_to_utf8(filter, cp);
    const std::string def_loc = resolve_dialog_default_location(vfs_, fname, cp, false);

    const i32 prev_cursor = window_.cursor();
    window_.set_cursor(0);
    renderer_.flush();
    std::string selected = window_.save_file_dialog(filter_utf8, def_loc);
    window_.set_cursor(prev_cursor);
    window_.input_mut().clear_all();
    mouse_button_ = 0;
    mouse_lastbutton_ = 0;

    if (selected.empty()) return {};
    return (game_.meta.encoding == EncodingMode::Utf8) ? selected : utf8_to_codepage(selected, cp);
}

std::string Runner::get_directory_dialog(std::string_view dname) {
    const u32 cp = game_.meta.string_codepage();
    const std::string def_loc = resolve_dialog_default_location(vfs_, dname, cp, true);

    const i32 prev_cursor = window_.cursor();
    window_.set_cursor(0);
    renderer_.flush();
    std::string selected = window_.open_folder_dialog({}, def_loc);
    window_.set_cursor(prev_cursor);
    window_.input_mut().clear_all();
    mouse_button_ = 0;
    mouse_lastbutton_ = 0;

    if (selected.empty()) return {};
    return (game_.meta.encoding == EncodingMode::Utf8) ? selected : utf8_to_codepage(selected, cp);
}

std::string Runner::get_directory_alt_dialog(std::string_view capt, std::string_view root) {
    const u32 cp = game_.meta.string_codepage();
    const std::string capt_utf8 = codepage_to_utf8(capt, cp);
    const std::string def_loc = resolve_dialog_default_location(vfs_, root, cp, true);

    const i32 prev_cursor = window_.cursor();
    window_.set_cursor(0);
    renderer_.flush();
    std::string selected = window_.open_folder_dialog(capt_utf8, def_loc);
    window_.set_cursor(prev_cursor);
    window_.input_mut().clear_all();
    mouse_button_ = 0;
    mouse_lastbutton_ = 0;

    if (selected.empty()) return {};
    return (game_.meta.encoding == EncodingMode::Utf8) ? selected : utf8_to_codepage(selected, cp);
}

// ---------------------------------------------------------------------------
// Self-Drawn GM8 Modal Dialogs (Highscore, Menu, ColorPicker, Splash, Info)
// ---------------------------------------------------------------------------

void Runner::run_ui_dialog_modal(bool show_cursor) {
    if (!window_.is_open()) {
        return;
    }

    const i32 prev_cursor = window_.cursor();
    const bool prev_text_input = window_.is_text_input_active();
    const bool wants_text = modal_dialogs_.wants_text_input();
    window_.set_cursor(show_cursor ? 0 : -1);
    if (wants_text) {
        window_.set_text_input(true);
    }
    window_.reset_modal_input_state();
    renderer_.flush();

    const bool mobile_touch = PlatformWindow::is_mobile_touch_platform();
    ModalInputFrame minput{};

    while (!host_stop_requested_) {
        const u64 frame_start = Clock::now_us();
        window_.poll_modal_events(minput);
        const auto& wev = minput.window_events;

        if (wev.entered_background) {
            paused_in_background_ = true;
            audio_.suspend();
            vfs_.notify_persist_write();
        }
        if (wev.entered_foreground) {
            paused_in_background_ = false;
            audio_.resume();
        }
        if (wev.context_lost) {
            if (auto res = gfx_.reinit_after_context_loss(&PlatformWindow::gl_get_proc_address); res) {
                (void)renderer_.reload_gpu_resources(game_);
                (void)touch_overlay_.reload_gpu_resources();
                (void)message_dialog_.reload_gpu_resources();
                (void)modal_dialogs_.reload_gpu_resources();
            }
        }
#if defined(__EMSCRIPTEN__)
        if (wev.any_user_gesture && !web_audio_unlocked_) {
            web_audio_unlocked_ = true;
            web_unlock_audio();
            audio_.resume();
        }
#endif
        if (wev.quit_requested) {
            running_ = false;
            pending_action_ = PendingRoomAction::EndGame;
            break;
        }

        const i32 ww = std::max(1, window_.width());
        const i32 wh = std::max(1, window_.height());
        const i32 dw = std::max(1, window_.drawable_width());
        const i32 dh = std::max(1, window_.drawable_height());

        if (wev.resized) {
            i32 vx = 0, vy = 0, vw = 1, vh = 1, cw = 1, ch = 1;
            compute_canvas_viewport(vx, vy, vw, vh, cw, ch);
            const i32 bw = requested_backbuffer_w_ > 0 ? requested_backbuffer_w_ : cw;
            const i32 bh = requested_backbuffer_h_ > 0 ? requested_backbuffer_h_ : ch;
            renderer_.set_screen_region(dw, dh, bw, bh, ViewportRect{vx, vy, vw, vh});
        }

        const i32 wx = window_.x();
        const i32 wy = window_.y();
        const f32 dscale = window_.display_scale();

        const bool keep_open = modal_dialogs_.update(
            minput, ww, wh, dw, dh, wx, wy, dscale, mobile_touch, frame_start);

        if (!paused_in_background_) {
            if (renderer_.has_screen_frame()) {
                renderer_.present_screen(window_color_);
            } else {
                const auto saved = gfx_.save_render_state();
                gfx_.bind_framebuffer(FramebufferHandle{});
                gfx_.set_scissor(false);
                gfx_.set_viewport(ViewportRect{0, 0, dw, dh});
                const ColorRGBA c = gm_bgr_to_rgba(window_color_, 255);
                gfx_.clear(true, false, false,
                           static_cast<f32>(c.r) / 255.0f,
                           static_cast<f32>(c.g) / 255.0f,
                           static_cast<f32>(c.b) / 255.0f,
                           1.0f);
                gfx_.restore_render_state(saved);
            }
            if (keep_open) {
                modal_dialogs_.render(ww, wh, dw, dh, wx, wy, dscale, mobile_touch, frame_start);
            }
            window_.swap_buffers();
            renderer_.end_frame();
        }

        if (!keep_open) {
            break;
        }
#if defined(__EMSCRIPTEN__)
        const u64 elapsed_ms = (Clock::now_us() - frame_start) / 1000;
        Clock::yield_ms(elapsed_ms < 16 ? static_cast<u32>(16 - elapsed_ms) : 1);
#else
        Clock::sleep_until_us(frame_start + 16000u);
#endif
    }

    if (wants_text && !prev_text_input) {
        window_.set_text_input(false);
    }
    window_.set_cursor(prev_cursor);

    // Match GM8 sub_5F6964: clear all keyboard/mouse state after dismissing any modal dialog.
    window_.input_mut().clear_all();
    mouse_button_ = 0;
    mouse_lastbutton_ = 0;
}

void Runner::highscore_clear() {
    for (auto& h : highscores_) {
        h.name = highscore_nobody_;
        h.score = 0.0;
    }
}

i32 Runner::highscore_add_entry(std::string_view name, f64 score) {
    const f64 sc = static_cast<f64>(delphi_round(score));
    for (usize i = 0; i < highscores_.size(); ++i) {
        if (sc > highscores_[i].score) {
            for (usize j = highscores_.size() - 1; j > i; --j) {
                highscores_[j] = highscores_[j - 1];
            }
            highscores_[i] = HighscoreEntry{std::string(name), sc};
            return static_cast<i32>(i);
        }
    }
    return -1;
}

void Runner::highscore_show_dialog(f64 score_val) {
    const u32 cp = game_.meta.string_codepage();
    const i64 rounded = delphi_round(score_val);
    i32 edit_rank = -1;
    if (rounded >= 0 && static_cast<f64>(rounded) > highscores_.back().score) {
        edit_rank = highscore_add_entry("", static_cast<f64>(rounded));
    }

    HighscoreDialogParams params{};
    params.edit_rank = edit_rank;
    params.show_border = highscore_border_;
    params.col_back_bgr = highscore_col_back_ & 0xFFFFFFu;
    params.col_new_bgr = highscore_col_new_ & 0xFFFFFFu;
    params.col_other_bgr = highscore_col_other_ & 0xFFFFFFu;
    params.font_name = codepage_to_utf8(highscore_font_name_, cp);
    params.font_size = highscore_font_size_;
    params.font_style = highscore_font_style_;
    params.caption_utf8 = codepage_to_utf8(highscore_caption_, cp);
    params.escape_text_utf8 = codepage_to_utf8(highscore_escape_, cp);

    if (highscore_bg_ >= 0 && static_cast<usize>(highscore_bg_) < game_.backgrounds.size() &&
        game_.backgrounds[static_cast<usize>(highscore_bg_)].exists) {
        params.bg_bitmap = game_.backgrounds[static_cast<usize>(highscore_bg_)].bitmap;
    }
    for (usize i = 0; i < highscores_.size(); ++i) {
        const std::string& raw_name = highscores_[i].name.empty() ? highscore_nobody_ : highscores_[i].name;
        params.rows[i].name_utf8 = codepage_to_utf8(raw_name, cp);
        params.rows[i].score = delphi_round(highscores_[i].score);
    }

    modal_dialogs_.begin_highscore(params);
    run_ui_dialog_modal(true);

    if (edit_rank >= 0 && static_cast<usize>(edit_rank) < highscores_.size()) {
        const std::string entered_utf8 = modal_dialogs_.result_highscore_name_utf8();
        if (entered_utf8.empty()) {
            highscores_[static_cast<usize>(edit_rank)].name = highscore_nobody_;
        } else {
            highscores_[static_cast<usize>(edit_rank)].name =
                (game_.meta.encoding == EncodingMode::Utf8) ? entered_utf8 : utf8_to_codepage(entered_utf8, cp);
        }
    }
}

void Runner::highscore_add_current_dialog() {
    const i64 sc = delphi_round(score_);
    if (static_cast<f64>(sc) > highscores_.back().score) {
        const std::string name = get_string_dialog("Enter your name:", "player");
        (void)highscore_add_entry(name.empty() ? highscore_nobody_ : name, static_cast<f64>(sc));
    }
}

RValue Runner::show_menu_dialog(
    std::string_view items_str,
    const RValue& default_val,
    std::optional<std::pair<i32, i32>> screen_pos) {
    const u32 cp = game_.meta.string_codepage();
    MenuDialogParams params{};

    // Match GM8 sub_628BB0: split by '|' (char 124); a trailing '|' does not create an extra item.
    usize pos = 0;
    while (pos < items_str.size()) {
        const usize bar = items_str.find('|', pos);
        const std::string_view seg = (bar == std::string_view::npos)
            ? items_str.substr(pos)
            : items_str.substr(pos, bar - pos);
        params.items_utf8.push_back(codepage_to_utf8(seg, cp));
        if (bar == std::string_view::npos) break;
        pos = bar + 1;
    }
    if (params.items_utf8.empty()) {
        return default_val;
    }

    if (screen_pos.has_value()) {
        params.anchor_win_x = screen_pos->first - window_.x();
        params.anchor_win_y = screen_pos->second - window_.y();
    } else {
        params.anchor_win_x = static_cast<i32>(std::round(window_.input().mouse_x));
        params.anchor_win_y = static_cast<i32>(std::round(window_.input().mouse_y));
    }

    modal_dialogs_.begin_menu(params);
    run_ui_dialog_modal(true);
    const i32 chosen = modal_dialogs_.result_menu_index();
    return (chosen >= 0) ? RValue(chosen) : default_val;
}

i32 Runner::get_color_dialog(i32 def_col) {
    ColorPickerParams params{};
    params.initial_color_bgr = static_cast<u32>(def_col) & 0xFFFFFFu;
    modal_dialogs_.begin_color_picker(params);
    run_ui_dialog_modal(true);
    // Exact GM8 sub_644820 behavior: return -1 when cancelled.
    if (!modal_dialogs_.result_color_accepted()) {
        return -1;
    }
    return static_cast<i32>(modal_dialogs_.result_color_bgr());
}

bool Runner::splash_show_image_dialog(std::string_view fname, i32 delay_ms) {
    auto bytes = vfs_.read_all_bytes(fname);
    if (!bytes) {
        GM_LOG_WARN("runtime", "splash_show_image: Image file not found: %.*s",
                    static_cast<int>(fname.size()), fname.data());
        return false;
    }
    auto bmp = Renderer2D::decode_image_to_bgra(bytes.value());
    if (!bmp) {
        GM_LOG_WARN("runtime", "splash_show_image: Failed to decode image: %.*s",
                    static_cast<int>(fname.size()), fname.data());
        return false;
    }

    const u32 cp = game_.meta.string_codepage();
    SplashDialogParams params{};
    params.is_image = true;
    params.image_bitmap = std::move(bmp.value());
    params.delay_ms = delay_ms;
    params.bg_color_bgr = splash_color_ & 0xFFFFFFu;
    params.scale = splash_scale_;
    params.in_main_window = splash_main_;
    params.fullscreen = splash_fullscreen_;
    params.show_border = splash_border_;
    params.caption_utf8 = codepage_to_utf8(splash_caption_, cp);
    params.size_w = splash_size_w_ > 0 ? splash_size_w_ : 640;
    params.size_h = splash_size_h_ > 0 ? splash_size_h_ : 480;
    params.pos_x = splash_pos_x_;
    params.pos_y = splash_pos_y_;
    params.adapt = splash_adapt_;
    params.stop_key = splash_stop_key_;
    params.stop_mouse = splash_stop_mouse_;
    params.close_button = splash_close_button_;

    modal_dialogs_.begin_splash(params, Clock::now_us());
    run_ui_dialog_modal(splash_cursor_);
    return true;
}

bool Runner::splash_show_text_dialog(std::string_view fname, i32 delay_ms) {
    auto bytes = vfs_.read_all_bytes(fname);
    if (!bytes) {
        GM_LOG_WARN("runtime", "splash_show_text: Text file not found: %.*s",
                    static_cast<int>(fname.size()), fname.data());
        return false;
    }

    const u32 cp = game_.meta.string_codepage();
    const u32 bg_bgr = splash_color_ & 0xFFFFFFu;
    const u32 r = bg_bgr & 0xFFu;
    const u32 g = (bg_bgr >> 8u) & 0xFFu;
    const u32 b = (bg_bgr >> 16u) & 0xFFu;
    const u32 luma = (r * 77u + g * 150u + b * 29u) >> 8u;
    const u32 default_fg_bgr = (luma < 128u) ? 0xFFFFFFu : 0x000000u;

    const std::string_view raw(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    SplashDialogParams params{};
    params.is_image = false;
    params.text_doc = parse_rtf_or_plain_text(raw, cp, default_fg_bgr);
    params.delay_ms = delay_ms;
    params.bg_color_bgr = bg_bgr;
    params.scale = splash_scale_;
    params.in_main_window = splash_main_;
    params.fullscreen = splash_fullscreen_;
    params.show_border = splash_border_;
    params.caption_utf8 = codepage_to_utf8(splash_caption_, cp);
    params.size_w = splash_size_w_ > 0 ? splash_size_w_ : 640;
    params.size_h = splash_size_h_ > 0 ? splash_size_h_ : 480;
    params.pos_x = splash_pos_x_;
    params.pos_y = splash_pos_y_;
    params.adapt = splash_adapt_;
    params.stop_key = splash_stop_key_;
    params.stop_mouse = splash_stop_mouse_;
    params.close_button = splash_close_button_;

    modal_dialogs_.begin_splash(params, Clock::now_us());
    run_ui_dialog_modal(splash_cursor_);
    return true;
}

bool Runner::load_info_file(std::string_view fname) {
    auto bytes = vfs_.read_all_bytes(fname);
    if (!bytes) {
        GM_LOG_WARN("runtime", "load_info: File not found: %.*s",
                    static_cast<int>(fname.size()), fname.data());
        return false;
    }
    game_.help.rtf_text.assign(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    return true;
}

void Runner::show_info_dialog() {
    const u32 cp = game_.meta.string_codepage();
    const u32 bg_bgr = game_.help.bg_color & 0xFFFFFFu;
    const u32 r = bg_bgr & 0xFFu;
    const u32 g = (bg_bgr >> 8u) & 0xFFu;
    const u32 b = (bg_bgr >> 16u) & 0xFFu;
    const u32 luma = (r * 77u + g * 150u + b * 29u) >> 8u;
    const u32 default_fg_bgr = (luma < 128u) ? 0xFFFFFFu : 0x000000u;

    GameInfoDialogParams params{};
    params.doc = parse_rtf_or_plain_text(game_.help.rtf_text, cp, default_fg_bgr);
    params.bg_color_bgr = bg_bgr;
    params.caption_utf8 = game_.help.caption.empty()
        ? std::string("Game Information")
        : codepage_to_utf8(game_.help.caption, cp);
    params.left = game_.help.left;
    params.top = game_.help.top;
    params.width = game_.help.width > 0 ? game_.help.width : 600;
    params.height = game_.help.height > 0 ? game_.help.height : 400;
    params.border = game_.help.border;
    params.sizable = game_.help.sizable;
    params.mimic = game_.help.mimic;

    modal_dialogs_.begin_game_info(params);
    run_ui_dialog_modal(true);
}

// ---------------------------------------------------------------------------
// Motion Planning Helpers (mp_linear_* & mp_potential_*)
// ---------------------------------------------------------------------------

bool Runner::mp_linear_step(Instance& inst, f64 xgoal, f64 ygoal, f64 stepsize, bool checkall, i32 target_obj) {
    const f64 dx = xgoal - inst.x;
    const f64 dy = ygoal - inst.y;
    const f64 dist = std::hypot(dx, dy);
    if (dist <= kGmlEpsilon) return true;

    const bool solid_only = (target_obj == -3) && !checkall;
    const f64 dir = gm_point_direction(inst.x, inst.y, xgoal, ygoal);

    if (dist <= std::fabs(stepsize)) {
        if (find_collision_instance(inst, xgoal, ygoal, target_obj, solid_only) == -4) {
            inst.x = xgoal;
            inst.y = ygoal;
            inst.direction = dir;
            inst.mark_bbox_dirty();
            return true;
        }
        return false;
    }

    const f64 nx = inst.x + (dx / dist) * stepsize;
    const f64 ny = inst.y + (dy / dist) * stepsize;
    if (find_collision_instance(inst, nx, ny, target_obj, solid_only) == -4) {
        inst.x = nx;
        inst.y = ny;
        inst.direction = dir;
        inst.mark_bbox_dirty();
    }
    return false;
}

bool Runner::mp_linear_path(Instance& inst, i32 path_id, f64 xgoal, f64 ygoal, f64 stepsize, bool checkall, i32 target_obj) {
    if (path_id < 0 || static_cast<usize>(path_id) >= game_.paths.size() ||
        !game_.paths[static_cast<usize>(path_id)].exists) {
        return false;
    }
    auto& path = game_.paths[static_cast<usize>(path_id)].edit_geometry();
    path.points.clear();
    path.closed = false;
    path.points.push_back(PathPoint{inst.x, inst.y, 100.0});
    if (stepsize <= 1e-6) return false;

    Instance sim = inst;
    const f64 dist = std::hypot(xgoal - sim.x, ygoal - sim.y);
    const i32 max_steps = std::min(10000, static_cast<i32>(std::ceil(dist / stepsize)) + 4);
    for (i32 s = 0; s < max_steps; ++s) {
        const f64 px = sim.x;
        const f64 py = sim.y;
        const bool reached = mp_linear_step(sim, xgoal, ygoal, stepsize, checkall, target_obj);
        if (sim.x != px || sim.y != py) {
            path.points.push_back(PathPoint{sim.x, sim.y, 100.0});
        } else {
            return false;
        }
        if (reached) return true;
    }
    return false;
}

bool Runner::mp_potential_step(Instance& inst, f64 xgoal, f64 ygoal, f64 stepsize, bool checkall, i32 target_obj) {
    if (inst.x == xgoal && inst.y == ygoal) return true;
    const f64 goal_dir = gm_point_direction(inst.x, inst.y, xgoal, ygoal);
    const auto set_direction = [&](f64 angle) {
        inst.direction = wrap_angle_deg(angle);
        inst.sync_hv_from_polar();
    };
    const auto clear_at = [&](f64 x, f64 y) {
        return find_collision_instance(inst, x, y, target_obj, target_obj == -3 && !checkall) == -4;
    };
    if (std::hypot(xgoal - inst.x, ygoal - inst.y) <= stepsize) {
        if (clear_at(xgoal, ygoal)) {
            set_direction(goal_dir);
            inst.x = xgoal;
            inst.y = ygoal;
            inst.mark_bbox_dirty();
        }
        return true;
    }

    const auto try_direction = [&](f64 direction) {
        if (std::abs(std::remainder(direction - inst.direction, 360.0)) > mp_potential_maxrot_) return false;
        const f64 dx = stepsize * std::cos(deg_to_rad(direction));
        const f64 dy = -stepsize * std::sin(deg_to_rad(direction));
        if (!clear_at(inst.x + dx * mp_potential_ahead_, inst.y + dy * mp_potential_ahead_) ||
            !clear_at(inst.x + dx, inst.y + dy)) return false;
        set_direction(direction);
        inst.x += dx;
        inst.y += dy;
        inst.mark_bbox_dirty();
        return true;
    };
    for (f64 offset = 0; offset < 180.0; offset += mp_potential_rotstep_) {
        if (try_direction(goal_dir - offset) || (offset != 0 && try_direction(goal_dir + offset))) return false;
    }
    if (mp_potential_onspot_) set_direction(inst.direction + mp_potential_maxrot_);
    return false;
}

bool Runner::mp_potential_path(Instance& inst, i32 path_id, f64 xgoal, f64 ygoal, f64 stepsize, f64 factor, bool checkall, i32 target_obj) {
    if (path_id < 0 || static_cast<usize>(path_id) >= game_.paths.size() ||
        !game_.paths[static_cast<usize>(path_id)].exists || !(factor >= 1.0) || !(stepsize > 0.0)) return false;

    auto& path = game_.paths[static_cast<usize>(path_id)].edit_geometry();
    path.points.clear();
    path.closed = false;
    path.kind = 0;
    path.points.push_back({inst.x, inst.y, 100.0});

    // Use the live instance so collision checks exclude the correct object. Restore
    // its complete motion state after the search, including on allocation failure.
    struct RestoreMotion {
        Instance& inst;
        f64 x, y, direction, hspeed, vspeed;
        ~RestoreMotion() {
            inst.x = x;
            inst.y = y;
            inst.direction = direction;
            inst.hspeed = hspeed;
            inst.vspeed = vspeed;
            inst.mark_bbox_dirty();
        }
    } restore{inst, inst.x, inst.y, inst.direction, inst.hspeed, inst.vspeed};

    const f64 max_length = factor * std::hypot(xgoal - inst.x, ygoal - inst.y);
    f64 length = 0.0;
    f64 stationary_turn = 0.0;
    while (!mp_potential_step(inst, xgoal, ygoal, stepsize, checkall, target_obj)) {
        const auto& previous = path.points.back();
        const f64 advance = std::hypot(inst.x - previous.x, inst.y - previous.y);
        path.points.push_back({inst.x, inst.y, 100.0});
        length += advance;
        if (length > max_length) return false;
        // At one position all collision candidates remain fixed. A full turn tests
        // every heading window, even when maxrot is smaller than rotstep.
        if (advance == 0.0) {
            stationary_turn += mp_potential_maxrot_;
            if (!mp_potential_onspot_ || stationary_turn >= 360.0) return false;
        } else {
            stationary_turn = 0.0;
        }
    }
    path.points.push_back({xgoal, ygoal, 100.0});
    return true;
}

// ---------------------------------------------------------------------------
// Dynamic Asset & Event Mutation Helpers
// ---------------------------------------------------------------------------

void Runner::apply_removeback_and_smooth(Bitmap32& bmp, bool removeback, bool smooth) {
    if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgra.empty()) return;
    if (removeback) {
        const usize bl = static_cast<usize>(bmp.height - 1) * static_cast<usize>(bmp.width) * 4u;
        const u8 kb = bmp.bgra[bl + 0];
        const u8 kg = bmp.bgra[bl + 1];
        const u8 kr = bmp.bgra[bl + 2];
        for (usize p = 0; p + 3 < bmp.bgra.size(); p += 4) {
            if (bmp.bgra[p + 0] == kb && bmp.bgra[p + 1] == kg && bmp.bgra[p + 2] == kr) {
                bmp.bgra[p + 3] = 0;
            }
        }
    }
    if (smooth && bmp.width > 2 && bmp.height > 2) {
        std::vector<u8> orig = bmp.bgra;
        const i32 w = bmp.width;
        const i32 h = bmp.height;
        for (i32 y = 1; y < h - 1; ++y) {
            for (i32 x = 1; x < w - 1; ++x) {
                const usize i = (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4u;
                if (orig[i + 3] == 0) continue;
                i32 transparent_neighbors = 0;
                const i32 offs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& o : offs) {
                    const usize ni = (static_cast<usize>(y + o[1]) * static_cast<usize>(w) + static_cast<usize>(x + o[0])) * 4u;
                    if (orig[ni + 3] == 0) ++transparent_neighbors;
                }
                if (transparent_neighbors > 0) {
                    bmp.bgra[i + 3] = static_cast<u8>(std::max(0, static_cast<i32>(orig[i + 3]) - transparent_neighbors * 48));
                }
            }
        }
    }
}

void Runner::rebuild_sprite_masks(SpriteData& sp, i32 alpha_tolerance, i32 bbox_mode,
                                  i32 bleft, i32 bright, i32 btop, i32 bbottom) {
    sp.masks.clear();
    if (sp.images.empty()) {
        sp.bbox_left = sp.bbox_right = sp.bbox_top = sp.bbox_bottom = 0;
        return;
    }

    auto build_mask_for_range = [&](usize first_img, usize last_img) -> CollisionMask {
        CollisionMask m{};
        const i32 w = sp.images[first_img].width;
        const i32 h = sp.images[first_img].height;
        m.width = w;
        m.height = h;
        m.pixels.assign(static_cast<usize>(w) * static_cast<usize>(h), 0);

        // A merged mask keeps the first bitmap's pixel storage and the union of every frame's bounds.
        i32 min_x = w - 1, min_y = h - 1, last_x = w - 1, last_y = h - 1;
        for (usize idx = first_img + 1; idx <= last_img; ++idx) {
            min_x = std::min(min_x, sp.images[idx].width - 1);
            min_y = std::min(min_y, sp.images[idx].height - 1);
            last_x = std::max(last_x, sp.images[idx].width - 1);
            last_y = std::max(last_y, sp.images[idx].height - 1);
        }
        i32 max_x = 0, max_y = 0; // Transparent frames keep inverted automatic bounds.
        if (bbox_mode == 1) { // full image
            min_x = 0; max_x = last_x; min_y = 0; max_y = last_y;
        } else if (bbox_mode != 0) { // manual
            min_x = std::max(bleft, 0);
            max_x = std::min(bright, last_x);
            min_y = std::max(btop, 0);
            max_y = std::min(bbottom, last_y);
        } else { // automatic from alpha
            for (usize idx = first_img; idx <= last_img && idx < sp.images.size(); ++idx) {
                const auto& img = sp.images[idx];
                for (i32 y = 0; y < img.height; ++y) {
                    for (i32 x = 0; x < img.width; ++x) {
                        const usize p = (static_cast<usize>(y) * static_cast<usize>(img.width) + static_cast<usize>(x)) * 4u;
                        if (p + 3 < img.bgra.size() && img.bgra[p + 3] > alpha_tolerance) {
                            min_x = std::min(min_x, x);
                            max_x = std::max(max_x, x);
                            min_y = std::min(min_y, y);
                            max_y = std::max(max_y, y);
                        }
                    }
                }
            }
        }
        m.bbox_left = min_x;
        m.bbox_right = max_x;
        m.bbox_top = min_y;
        m.bbox_bottom = max_y;

        const f64 cx = (static_cast<f64>(min_x) + static_cast<f64>(max_x)) * 0.5;
        const f64 cy = (static_cast<f64>(min_y) + static_cast<f64>(max_y)) * 0.5;
        const f64 rx = cx - min_x + 0.5;
        const f64 ry = cy - min_y + 0.5;

        for (i32 y = std::max(0, min_y); y <= std::min(h - 1, max_y); ++y) {
            for (i32 x = std::max(0, min_x); x <= std::min(w - 1, max_x); ++x) {
                bool solid = false;
                if (sp.bbox_shape == 0) { // precise
                    for (usize idx = first_img; idx <= last_img && idx < sp.images.size(); ++idx) {
                        const auto& img = sp.images[idx];
                        if (x < img.width && y < img.height) {
                            const usize p = (static_cast<usize>(y) * static_cast<usize>(img.width) + static_cast<usize>(x)) * 4u;
                            if (p + 3 < img.bgra.size() && img.bgra[p + 3] > alpha_tolerance) {
                                solid = true;
                                break;
                            }
                        }
                    }
                } else if (sp.bbox_shape == 1) { // rectangle
                    solid = true;
                } else if (sp.bbox_shape == 2) { // disk/ellipse
                    const f64 nx = (static_cast<f64>(x) - cx) / rx;
                    const f64 ny = (static_cast<f64>(y) - cy) / ry;
                    solid = (nx * nx + ny * ny) < 1.0;
                } else if (sp.bbox_shape == 3) { // diamond
                    const f64 nx = (static_cast<f64>(x) - cx) / rx;
                    const f64 ny = (static_cast<f64>(y) - cy) / ry;
                    solid = (std::fabs(nx) + std::fabs(ny)) < 1.0;
                }
                m.pixels[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)] = solid ? 1u : 0u;
            }
        }
        return m;
    };

    if (sp.separate_masks) {
        sp.masks.reserve(sp.images.size());
        i32 gl = sp.width - 1, gr = 0, gt = sp.height - 1, gb = 0;
        for (usize i = 0; i < sp.images.size(); ++i) {
            CollisionMask m = build_mask_for_range(i, i);
            gl = std::min(gl, m.bbox_left);
            gr = std::max(gr, m.bbox_right);
            gt = std::min(gt, m.bbox_top);
            gb = std::max(gb, m.bbox_bottom);
            sp.masks.push_back(std::move(m));
        }
        sp.bbox_left = gl;
        sp.bbox_right = gr;
        sp.bbox_top = gt;
        sp.bbox_bottom = gb;
    } else {
        CollisionMask m = build_mask_for_range(0, sp.images.size() - 1);
        sp.bbox_left = m.bbox_left;
        sp.bbox_right = m.bbox_right;
        sp.bbox_top = m.bbox_top;
        sp.bbox_bottom = m.bbox_bottom;
        sp.masks.push_back(std::move(m));
    }
}

i32 Runner::create_sprite_from_bitmap(Bitmap32 bmp, bool removeback, bool smooth, i32 xorig, i32 yorig) {
    if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgra.empty()) return -1;
    apply_removeback_and_smooth(bmp, removeback, smooth);

    SpriteData sp{};
    sp.exists = true;
    sp.width = bmp.width;
    sp.height = bmp.height;
    sp.origin_x = xorig;
    sp.origin_y = yorig;
    sp.bbox_shape = 1; // rectangle by default for dynamic sprites
    sp.images.push_back(std::move(bmp));
    rebuild_sprite_masks(sp, 0, 1);

    const i32 new_id = static_cast<i32>(game_.sprites.size());
    game_.sprites.push_back(std::move(sp));
    renderer_.upload_sprite(static_cast<usize>(new_id), game_.sprites[static_cast<usize>(new_id)]);
    return new_id;
}

i32 Runner::create_background_from_bitmap(Bitmap32 bmp, bool removeback, bool smooth) {
    if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgra.empty()) return -1;
    apply_removeback_and_smooth(bmp, removeback, smooth);

    BackgroundData bg{};
    bg.exists = true;
    bg.width = bmp.width;
    bg.height = bmp.height;
    bg.bitmap = std::move(bmp);

    const i32 new_id = static_cast<i32>(game_.backgrounds.size());
    game_.backgrounds.push_back(std::move(bg));
    renderer_.upload_background(static_cast<usize>(new_id), game_.backgrounds[static_cast<usize>(new_id)]);
    return new_id;
}

bool Runner::object_event_add(i32 obj_idx, i32 ev_type, i32 ev_numb, std::string_view code) {
    if (!object_exists(obj_idx) || ev_type < 0 || ev_type >= 12) return false;
    if (static_cast<usize>(obj_idx) >= program_.object_event_chunks.size()) {
        program_.object_event_chunks.resize(static_cast<usize>(obj_idx) + 1);
    }
    auto compiled = GmlCompiler::compile_chunk("object_event_add", code, program_.symbols);
    if (!compiled) {
        GM_LOG_WARN("runtime", "object_event_add compile error: %s", compiled.error_message().c_str());
        return false;
    }
    auto& ev_map = program_.object_event_chunks[static_cast<usize>(obj_idx)][static_cast<usize>(ev_type)];
    auto it = ev_map.find(ev_numb);
    if (it == ev_map.end()) {
        ev_map[ev_numb] = std::move(compiled.value());
    } else {
        // Append newly compiled code as a chained code action.
        auto& existing = it->second;
        const u32 action_idx = static_cast<u32>(existing.code_actions.size());
        existing.code_actions.push_back(std::make_shared<BytecodeChunk>(std::move(compiled.value())));
        // Remove the trailing Exit/Return before appending the action.
        if (!existing.code.empty() && existing.code.back().op == OpCode::Exit) {
            existing.code.pop_back();
        } else if (!existing.code.empty() && existing.code.back().op == OpCode::Return) {
            existing.code.pop_back();
            if (!existing.code.empty() && existing.code.back().op == OpCode::PushReal) {
                existing.code.pop_back();
            }
        }
        existing.emit(OpCode::CallCodeAction, action_idx);
        existing.emit(OpCode::Pop);
        existing.emit(OpCode::Exit);
    }
    invalidate_event_tables();
    return true;
}

bool Runner::object_event_clear(i32 obj_idx, i32 ev_type, i32 ev_numb) {
    if (!object_exists(obj_idx) || ev_type < 0 || ev_type >= 12) return false;
    if (static_cast<usize>(obj_idx) < program_.object_event_chunks.size()) {
        program_.object_event_chunks[static_cast<usize>(obj_idx)][static_cast<usize>(ev_type)].erase(ev_numb);
    }
    invalidate_event_tables();
    return true;
}

bool Runner::timeline_moment_add(i32 tl_idx, i32 step, std::string_view code) {
    if (tl_idx < 0 || static_cast<usize>(tl_idx) >= game_.timelines.size() ||
        !game_.timelines[static_cast<usize>(tl_idx)].exists) {
        return false;
    }
    if (static_cast<usize>(tl_idx) >= program_.timeline_chunks.size()) {
        program_.timeline_chunks.resize(static_cast<usize>(tl_idx) + 1);
    }
    auto compiled = GmlCompiler::compile_chunk("timeline_moment_add", code, program_.symbols);
    if (!compiled) {
        GM_LOG_WARN("runtime", "timeline_moment_add compile error: %s", compiled.error_message().c_str());
        return false;
    }
    auto& moments = game_.timelines[tl_idx].moments;
    auto moment = std::ranges::lower_bound(moments, step, {}, &TimelineMoment::step);
    if (moment == moments.end() || moment->step != step) {
        moment = moments.insert(moment, TimelineMoment{step, {}});
    }
    ActionData action;
    action.kind = 7; // Execute Code
    action.exe_type = 2;
    action.args.emplace_back(code);
    moment->event.actions.push_back(std::move(action));

    auto& event = program_.timeline_chunks[tl_idx][step];
    if (!event) event = std::make_shared<BytecodeChunk>();
    const auto action_index = static_cast<u32>(event->code_actions.size());
    event->code_actions.push_back(std::make_shared<BytecodeChunk>(std::move(compiled.value())));
    const u32 skip = event->emit(OpCode::JumpIfRoomChange);
    event->emit(OpCode::CallCodeAction, action_index);
    event->emit(OpCode::Pop);
    event->patch_operand(skip, static_cast<u32>(event->code.size()));
    return true;
}

bool Runner::timeline_moment_clear(i32 tl_idx, i32 step) {
    if (tl_idx < 0 || static_cast<usize>(tl_idx) >= game_.timelines.size() ||
        !game_.timelines[static_cast<usize>(tl_idx)].exists) {
        return false;
    }
    auto& moments = game_.timelines[tl_idx].moments;
    auto moment = std::ranges::lower_bound(moments, step, {}, &TimelineMoment::step);
    if (moment != moments.end() && moment->step == step) moments.erase(moment);
    if (static_cast<usize>(tl_idx) < program_.timeline_chunks.size()) {
        auto& chunks = program_.timeline_chunks[tl_idx];
        if (auto it = chunks.find(step); it != chunks.end()) {
            it->second->code.clear(); // Stop the remaining actions if this moment is executing.
            chunks.erase(it);
        }
    }
    return true;
}

void Runner::timeline_clear(i32 tl_idx) {
    if (tl_idx < 0 || static_cast<usize>(tl_idx) >= game_.timelines.size()) return;
    game_.timelines[tl_idx].moments.clear();
    if (static_cast<usize>(tl_idx) >= program_.timeline_chunks.size()) return;
    auto& chunks = program_.timeline_chunks[tl_idx];
    for (auto& [step, event] : chunks) event->code.clear();
    chunks.clear();
}

bool Runner::room_set_code(i32 room_idx, std::string_view code) {
    if (!room_exists(room_idx)) return false;
    game_.rooms[static_cast<usize>(room_idx)].creation_code = std::string(code);
    if (static_cast<usize>(room_idx) >= program_.room_creation_chunks.size()) {
        program_.room_creation_chunks.resize(static_cast<usize>(room_idx) + 1);
    }
    if (code.empty()) {
        program_.room_creation_chunks[static_cast<usize>(room_idx)].reset();
        return true;
    }
    auto compiled = GmlCompiler::compile_chunk("room_set_code", code, program_.symbols);
    if (!compiled) {
        GM_LOG_WARN("runtime", "room_set_code compile error: %s", compiled.error_message().c_str());
        return false;
    }
    program_.room_creation_chunks[static_cast<usize>(room_idx)] = std::move(compiled.value());
    return true;
}

} // namespace gm
