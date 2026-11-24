#include "runtime/runner.h"

#include "base/binary_reader.h"
#include "base/log.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace gm {

namespace {

inline constexpr u32 kSaveMagic = 0x56415347u; // "GSAV"
inline constexpr i32 kGmSaveVersion = 540;     // Original GM 8.0 / 8.1 / 8.2 save version
inline constexpr u32 kSaveFormatRev = 2u;      // Runner serialization revision (rev 2 adds cursor_subimg)
inline constexpr i32 kGlobalsMarker = 700;     // Original GM8 globals stream marker
inline constexpr i32 kRoomMarker = 540;        // Original GM8 CRoom stream marker

inline constexpr u32 kMaxSerializedVars = 1u << 20;
inline constexpr u32 kMaxSerializedEntities = 1u << 20;

class SaveWriter {
public:
    explicit SaveWriter(usize reserve_bytes = 16384) {
        buf_.reserve(reserve_bytes);
    }

    void write_u8(u8 v) { buf_.push_back(v); }
    void write_bool(bool v) { buf_.push_back(v ? 1u : 0u); }

    void write_u32(u32 v) {
        const u8 b[4] = {
            static_cast<u8>(v & 0xFFu),
            static_cast<u8>((v >> 8u) & 0xFFu),
            static_cast<u8>((v >> 16u) & 0xFFu),
            static_cast<u8>((v >> 24u) & 0xFFu),
        };
        buf_.insert(buf_.end(), b, b + 4);
    }

    void write_i32(i32 v) { write_u32(static_cast<u32>(v)); }

    void write_f64(f64 v) {
        u64 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        for (int i = 0; i < 8; ++i) {
            buf_.push_back(static_cast<u8>((bits >> (i * 8)) & 0xFFu));
        }
    }

    void write_string(std::string_view s) {
        write_u32(static_cast<u32>(s.size()));
        if (!s.empty()) {
            const auto* p = reinterpret_cast<const u8*>(s.data());
            buf_.insert(buf_.end(), p, p + s.size());
        }
    }

    [[nodiscard]] std::span<const u8> bytes() const noexcept { return buf_; }

private:
    std::vector<u8> buf_;
};

void write_rvalue(SaveWriter& w, const RValue& v) {
    w.write_u8(static_cast<u8>(v.kind()));
    w.write_f64(v.as_real());
    w.write_string(v.as_string());
}

Result<RValue> read_rvalue(BinaryReader& r) {
    auto kind_res = r.read_u8();
    if (!kind_res) return kind_res.error();
    auto real_res = r.read_f64();
    if (!real_res) return real_res.error();
    auto str_res = r.read_gm_string();
    if (!str_res) return str_res.error();
    const RValue::Kind kind = (kind_res.value() == static_cast<u8>(RValue::Kind::String))
                                  ? RValue::Kind::String
                                  : RValue::Kind::Real;
    return RValue(kind, real_res.value(), std::move(str_res.value()));
}

void write_variable_table(SaveWriter& w, const VariableTable& table, const SymbolTable& symbols) {
    auto ids = table.all_var_ids();
    std::sort(ids.begin(), ids.end());

    struct NamedSlot {
        std::string_view name;
        const VarSlot* slot = nullptr;
    };
    std::vector<NamedSlot> entries;
    entries.reserve(ids.size());
    for (u32 id : ids) {
        const std::string_view name = symbols.user_var_name(id);
        const VarSlot* slot = table.find_slot(id);
        if (!name.empty() && slot != nullptr) {
            entries.push_back(NamedSlot{name, slot});
        }
    }

    w.write_u32(static_cast<u32>(entries.size()));
    for (const auto& entry : entries) {
        w.write_string(entry.name);
        write_rvalue(w, entry.slot->scalar);
        const bool has_array = entry.slot->array_data && !entry.slot->array_data->rows.empty();
        w.write_bool(has_array);
        if (has_array) {
            const auto& rows = entry.slot->array_data->rows;
            std::vector<u32> row_keys;
            row_keys.reserve(rows.size());
            for (const auto& [row_idx, _] : rows) row_keys.push_back(row_idx);
            std::sort(row_keys.begin(), row_keys.end());

            w.write_u32(static_cast<u32>(row_keys.size()));
            for (u32 row_idx : row_keys) {
                const auto& row = rows.at(row_idx);
                w.write_u32(row_idx);
                w.write_u32(row.length);

                std::vector<u32> col_keys;
                col_keys.reserve(row.values.size());
                for (const auto& [col_idx, _] : row.values) col_keys.push_back(col_idx);
                std::sort(col_keys.begin(), col_keys.end());

                w.write_u32(static_cast<u32>(col_keys.size()));
                for (u32 col_idx : col_keys) {
                    w.write_u32(col_idx);
                    write_rvalue(w, row.values.at(col_idx));
                }
            }
        }
    }
}

Result<VariableTable> read_variable_table(BinaryReader& r, SymbolTable& symbols) {
    auto count_res = r.read_u32();
    if (!count_res) return count_res.error();
    const u32 count = count_res.value();
    if (count > kMaxSerializedVars || count > r.remaining()) {
        return Error{"Corrupt variable table count"};
    }

    VariableTable out;
    for (u32 i = 0; i < count; ++i) {
        auto name_res = r.read_gm_string();
        if (!name_res) return name_res.error();
        auto scalar_res = read_rvalue(r);
        if (!scalar_res) return scalar_res.error();
        auto has_arr_res = r.read_u8();
        if (!has_arr_res) return has_arr_res.error();

        VarSlot slot;
        slot.scalar = std::move(scalar_res.value());

        if (has_arr_res.value() != 0u) {
            auto row_count_res = r.read_u32();
            if (!row_count_res) return row_count_res.error();
            const u32 row_count = row_count_res.value();
            if (row_count > static_cast<u32>(kGmArrayMaxDim) || row_count > r.remaining()) {
                return Error{"Corrupt array row count"};
            }
            slot.array_data = std::make_unique<VarSlot::ArrayData>();
            slot.array_data->rows.reserve(row_count);

            for (u32 ri = 0; ri < row_count; ++ri) {
                auto row_idx_res = r.read_u32();
                auto row_len_res = r.read_u32();
                auto col_count_res = r.read_u32();
                if (!row_idx_res || !row_len_res || !col_count_res) {
                    return Error{"Truncated array row header"};
                }
                const u32 row_idx = row_idx_res.value();
                const u32 row_len = row_len_res.value();
                const u32 col_count = col_count_res.value();
                if (row_idx >= static_cast<u32>(kGmArrayMaxDim) ||
                    row_len > static_cast<u32>(kGmArrayMaxDim) ||
                    col_count > static_cast<u32>(kGmArrayMaxDim) ||
                    col_count > r.remaining()) {
                    return Error{"Corrupt array row dimensions"};
                }

                VarSlot::ArrayRow row;
                row.length = row_len;
                row.values.reserve(col_count);
                for (u32 ci = 0; ci < col_count; ++ci) {
                    auto col_idx_res = r.read_u32();
                    if (!col_idx_res) return col_idx_res.error();
                    const u32 col_idx = col_idx_res.value();
                    if (col_idx >= static_cast<u32>(kGmArrayMaxDim)) {
                        return Error{"Corrupt array column index"};
                    }
                    auto val_res = read_rvalue(r);
                    if (!val_res) return val_res.error();
                    row.values[col_idx] = std::move(val_res.value());
                }
                slot.array_data->rows[row_idx] = std::move(row);
            }
        }

        if (!name_res.value().empty()) {
            const u32 var_id = symbols.intern_user_var(name_res.value());
            out.set_slot(var_id, std::move(slot));
        }
    }
    return out;
}

void write_instance(SaveWriter& w, const Instance& inst, const SymbolTable& symbols) {
    w.write_i32(inst.id);
    w.write_i32(inst.object_index);

    w.write_f64(inst.x);
    w.write_f64(inst.y);
    w.write_f64(inst.xprevious);
    w.write_f64(inst.yprevious);
    w.write_f64(inst.xstart);
    w.write_f64(inst.ystart);

    w.write_f64(inst.hspeed);
    w.write_f64(inst.vspeed);
    w.write_f64(inst.direction);
    w.write_f64(inst.speed);
    w.write_f64(inst.friction);
    w.write_f64(inst.gravity);
    w.write_f64(inst.gravity_direction);

    for (i32 a : inst.alarm) w.write_i32(a);

    w.write_bool(inst.solid);
    w.write_bool(inst.visible);
    w.write_bool(inst.persistent);
    w.write_f64(inst.depth);

    w.write_i32(inst.sprite_index);
    w.write_f64(inst.image_index);
    w.write_f64(inst.image_single);
    w.write_f64(inst.image_xscale);
    w.write_f64(inst.image_yscale);
    w.write_f64(inst.image_angle);
    w.write_f64(inst.image_alpha);
    w.write_u32(inst.image_blend);
    w.write_f64(inst.image_speed);
    w.write_i32(inst.mask_index);

    w.write_i32(inst.path_index);
    w.write_f64(inst.path_position);
    w.write_f64(inst.path_positionprevious);
    w.write_f64(inst.path_speed);
    w.write_f64(inst.path_scale);
    w.write_f64(inst.path_orientation);
    w.write_i32(inst.path_endaction);
    w.write_f64(inst.path_xstart);
    w.write_f64(inst.path_ystart);

    w.write_i32(inst.timeline_index);
    w.write_f64(inst.timeline_position);
    w.write_f64(inst.timeline_speed);
    w.write_bool(inst.timeline_running);
    w.write_bool(inst.timeline_loop);

    w.write_bool(inst.destroyed);
    w.write_bool(inst.deactivated);

    write_variable_table(w, inst.vars, symbols);
}

Result<Instance> read_instance(BinaryReader& r, SymbolTable& symbols) {
    auto rd_i32 = [&](i32& out) -> bool {
        auto v = r.read_i32();
        if (!v) return false;
        out = v.value();
        return true;
    };
    auto rd_u32 = [&](u32& out) -> bool {
        auto v = r.read_u32();
        if (!v) return false;
        out = v.value();
        return true;
    };
    auto rd_f64 = [&](f64& out) -> bool {
        auto v = r.read_f64();
        if (!v) return false;
        out = v.value();
        return true;
    };
    auto rd_bool = [&](bool& out) -> bool {
        auto v = r.read_u8();
        if (!v) return false;
        out = v.value() != 0u;
        return true;
    };

    Instance inst{};
    if (!rd_i32(inst.id) || !rd_i32(inst.object_index)) return Error{"Truncated instance header"};

    if (!rd_f64(inst.x) || !rd_f64(inst.y) ||
        !rd_f64(inst.xprevious) || !rd_f64(inst.yprevious) ||
        !rd_f64(inst.xstart) || !rd_f64(inst.ystart)) {
        return Error{"Truncated instance position"};
    }

    if (!rd_f64(inst.hspeed) || !rd_f64(inst.vspeed) ||
        !rd_f64(inst.direction) || !rd_f64(inst.speed) ||
        !rd_f64(inst.friction) || !rd_f64(inst.gravity) ||
        !rd_f64(inst.gravity_direction)) {
        return Error{"Truncated instance motion"};
    }

    for (i32& a : inst.alarm) {
        if (!rd_i32(a)) return Error{"Truncated instance alarm"};
    }

    if (!rd_bool(inst.solid) || !rd_bool(inst.visible) ||
        !rd_bool(inst.persistent) || !rd_f64(inst.depth)) {
        return Error{"Truncated instance flags"};
    }

    if (!rd_i32(inst.sprite_index) || !rd_f64(inst.image_index) ||
        !rd_f64(inst.image_single) || !rd_f64(inst.image_xscale) ||
        !rd_f64(inst.image_yscale) || !rd_f64(inst.image_angle) ||
        !rd_f64(inst.image_alpha) || !rd_u32(inst.image_blend) ||
        !rd_f64(inst.image_speed) || !rd_i32(inst.mask_index)) {
        return Error{"Truncated instance sprite state"};
    }

    if (!rd_i32(inst.path_index) || !rd_f64(inst.path_position) ||
        !rd_f64(inst.path_positionprevious) || !rd_f64(inst.path_speed) ||
        !rd_f64(inst.path_scale) || !rd_f64(inst.path_orientation) ||
        !rd_i32(inst.path_endaction) || !rd_f64(inst.path_xstart) ||
        !rd_f64(inst.path_ystart)) {
        return Error{"Truncated instance path state"};
    }

    if (!rd_i32(inst.timeline_index) || !rd_f64(inst.timeline_position) ||
        !rd_f64(inst.timeline_speed) || !rd_bool(inst.timeline_running) ||
        !rd_bool(inst.timeline_loop)) {
        return Error{"Truncated instance timeline state"};
    }

    if (!rd_bool(inst.destroyed) || !rd_bool(inst.deactivated)) {
        return Error{"Truncated instance lifecycle flags"};
    }

    auto vars_res = read_variable_table(r, symbols);
    if (!vars_res) return vars_res.error();
    inst.vars = std::move(vars_res.value());
    inst.mark_bbox_dirty();
    return inst;
}

void write_background(SaveWriter& w, const ActiveBackground& bg) {
    w.write_bool(bg.visible);
    w.write_bool(bg.foreground);
    w.write_i32(bg.index);
    w.write_f64(bg.x);
    w.write_f64(bg.y);
    w.write_bool(bg.htiled);
    w.write_bool(bg.vtiled);
    w.write_f64(bg.xscale);
    w.write_f64(bg.yscale);
    w.write_f64(bg.hspeed);
    w.write_f64(bg.vspeed);
    w.write_u32(bg.blend);
    w.write_f64(bg.alpha);
    w.write_bool(bg.stretch);
}

Result<ActiveBackground> read_background(BinaryReader& r) {
    ActiveBackground bg{};
    auto rd_bool = [&](bool& out) -> bool { auto v = r.read_u8(); if (!v) return false; out = v.value() != 0u; return true; };
    auto rd_i32  = [&](i32& out)  -> bool { auto v = r.read_i32(); if (!v) return false; out = v.value(); return true; };
    auto rd_u32  = [&](u32& out)  -> bool { auto v = r.read_u32(); if (!v) return false; out = v.value(); return true; };
    auto rd_f64  = [&](f64& out)  -> bool { auto v = r.read_f64(); if (!v) return false; out = v.value(); return true; };

    if (!rd_bool(bg.visible) || !rd_bool(bg.foreground) || !rd_i32(bg.index) ||
        !rd_f64(bg.x) || !rd_f64(bg.y) || !rd_bool(bg.htiled) || !rd_bool(bg.vtiled) ||
        !rd_f64(bg.xscale) || !rd_f64(bg.yscale) || !rd_f64(bg.hspeed) || !rd_f64(bg.vspeed) ||
        !rd_u32(bg.blend) || !rd_f64(bg.alpha) || !rd_bool(bg.stretch)) {
        return Error{"Truncated background state"};
    }
    return bg;
}

void write_view(SaveWriter& w, const ActiveView& v) {
    w.write_bool(v.visible);
    w.write_f64(v.xview);
    w.write_f64(v.yview);
    w.write_f64(v.wview);
    w.write_f64(v.hview);
    w.write_i32(v.xport);
    w.write_i32(v.yport);
    w.write_i32(v.wport);
    w.write_i32(v.hport);
    w.write_f64(v.angle);
    w.write_i32(v.hborder);
    w.write_i32(v.vborder);
    w.write_i32(v.hspeed);
    w.write_i32(v.vspeed);
    w.write_i32(v.follow_object);
}

Result<ActiveView> read_view(BinaryReader& r) {
    ActiveView av{};
    auto rd_bool = [&](bool& out) -> bool { auto v = r.read_u8(); if (!v) return false; out = v.value() != 0u; return true; };
    auto rd_i32  = [&](i32& out)  -> bool { auto v = r.read_i32(); if (!v) return false; out = v.value(); return true; };
    auto rd_f64  = [&](f64& out)  -> bool { auto v = r.read_f64(); if (!v) return false; out = v.value(); return true; };

    if (!rd_bool(av.visible) ||
        !rd_f64(av.xview) || !rd_f64(av.yview) || !rd_f64(av.wview) || !rd_f64(av.hview) ||
        !rd_i32(av.xport) || !rd_i32(av.yport) || !rd_i32(av.wport) || !rd_i32(av.hport) ||
        !rd_f64(av.angle) || !rd_i32(av.hborder) || !rd_i32(av.vborder) ||
        !rd_i32(av.hspeed) || !rd_i32(av.vspeed) || !rd_i32(av.follow_object)) {
        return Error{"Truncated view state"};
    }
    return av;
}

void write_tile(SaveWriter& w, const ActiveTile& t) {
    w.write_i32(t.id);
    w.write_i32(t.background_index);
    w.write_i32(t.xo);
    w.write_i32(t.yo);
    w.write_i32(t.w);
    w.write_i32(t.h);
    w.write_f64(t.x);
    w.write_f64(t.y);
    w.write_f64(t.depth);
    w.write_f64(t.xscale);
    w.write_f64(t.yscale);
    w.write_u32(t.blend);
    w.write_f64(t.alpha);
    w.write_bool(t.visible);
}

Result<ActiveTile> read_tile(BinaryReader& r) {
    ActiveTile t{};
    auto rd_bool = [&](bool& out) -> bool { auto v = r.read_u8(); if (!v) return false; out = v.value() != 0u; return true; };
    auto rd_i32  = [&](i32& out)  -> bool { auto v = r.read_i32(); if (!v) return false; out = v.value(); return true; };
    auto rd_u32  = [&](u32& out)  -> bool { auto v = r.read_u32(); if (!v) return false; out = v.value(); return true; };
    auto rd_f64  = [&](f64& out)  -> bool { auto v = r.read_f64(); if (!v) return false; out = v.value(); return true; };

    if (!rd_i32(t.id) || !rd_i32(t.background_index) ||
        !rd_i32(t.xo) || !rd_i32(t.yo) || !rd_i32(t.w) || !rd_i32(t.h) ||
        !rd_f64(t.x) || !rd_f64(t.y) || !rd_f64(t.depth) ||
        !rd_f64(t.xscale) || !rd_f64(t.yscale) || !rd_u32(t.blend) ||
        !rd_f64(t.alpha) || !rd_bool(t.visible)) {
        return Error{"Truncated tile state"};
    }
    return t;
}

struct LoadedRoomData {
    std::string caption;
    i32 width = 640;
    i32 height = 480;
    i32 speed = 30;
    bool persistent = false;
    u32 bg_color = 0xC0C0C0u;
    bool bg_showcolor = true;
    std::array<ActiveBackground, 8> backgrounds{};
    bool view_enabled = false;
    std::array<ActiveView, 8> views{};
    std::vector<Instance> active_instances;
    std::vector<Instance> inactive_instances;
    std::vector<ActiveTile> tiles;
};

void write_room_data(
    SaveWriter& w,
    std::string_view caption,
    i32 width,
    i32 height,
    i32 speed,
    bool persistent,
    u32 bg_color,
    bool bg_showcolor,
    const std::array<ActiveBackground, 8>& backgrounds,
    bool view_enabled,
    const std::array<ActiveView, 8>& views,
    std::span<const Instance* const> active_instances,
    std::span<const Instance* const> inactive_instances,
    std::span<const ActiveTile> tiles,
    const SymbolTable& symbols) {
    w.write_i32(kRoomMarker);
    w.write_string(caption);
    w.write_i32(width);
    w.write_i32(height);
    w.write_i32(speed);
    w.write_bool(persistent);
    w.write_u32(bg_color);
    w.write_bool(bg_showcolor);

    w.write_u32(8u);
    for (const auto& bg : backgrounds) write_background(w, bg);

    w.write_bool(view_enabled);
    w.write_u32(8u);
    for (const auto& av : views) write_view(w, av);

    w.write_u32(static_cast<u32>(active_instances.size()));
    for (const Instance* inst : active_instances) {
        write_instance(w, *inst, symbols);
    }

    w.write_u32(static_cast<u32>(inactive_instances.size()));
    for (const Instance* inst : inactive_instances) {
        write_instance(w, *inst, symbols);
    }

    w.write_u32(static_cast<u32>(tiles.size()));
    for (const auto& t : tiles) {
        write_tile(w, t);
    }
}

Result<LoadedRoomData> read_room_data(BinaryReader& r, SymbolTable& symbols) {
    auto marker_res = r.read_i32();
    if (!marker_res || marker_res.value() != kRoomMarker) {
        return Error{"Invalid room stream marker"};
    }

    LoadedRoomData rm{};
    auto cap_res = r.read_gm_string();
    auto w_res = r.read_i32();
    auto h_res = r.read_i32();
    auto spd_res = r.read_i32();
    auto pers_res = r.read_u8();
    auto bgc_res = r.read_u32();
    auto bgs_res = r.read_u8();
    if (!cap_res || !w_res || !h_res || !spd_res || !pers_res || !bgc_res || !bgs_res) {
        return Error{"Truncated room header"};
    }
    rm.caption = std::move(cap_res.value());
    rm.width = std::max(1, w_res.value());
    rm.height = std::max(1, h_res.value());
    rm.speed = std::max(1, spd_res.value());
    rm.persistent = pers_res.value() != 0u;
    rm.bg_color = bgc_res.value() & 0x00FFFFFFu;
    rm.bg_showcolor = bgs_res.value() != 0u;

    auto bg_count_res = r.read_u32();
    if (!bg_count_res || bg_count_res.value() != 8u) {
        return Error{"Invalid room background count"};
    }
    for (usize i = 0; i < 8; ++i) {
        auto bg_res = read_background(r);
        if (!bg_res) return bg_res.error();
        rm.backgrounds[i] = bg_res.value();
    }

    auto ve_res = r.read_u8();
    auto view_count_res = r.read_u32();
    if (!ve_res || !view_count_res || view_count_res.value() != 8u) {
        return Error{"Invalid room view header"};
    }
    rm.view_enabled = ve_res.value() != 0u;
    for (usize i = 0; i < 8; ++i) {
        auto av_res = read_view(r);
        if (!av_res) return av_res.error();
        rm.views[i] = av_res.value();
    }

    auto active_cnt_res = r.read_u32();
    if (!active_cnt_res || active_cnt_res.value() > kMaxSerializedEntities ||
        active_cnt_res.value() > r.remaining()) {
        return Error{"Corrupt active instance count"};
    }
    rm.active_instances.reserve(active_cnt_res.value());
    for (u32 i = 0; i < active_cnt_res.value(); ++i) {
        auto inst_res = read_instance(r, symbols);
        if (!inst_res) return inst_res.error();
        inst_res.value().deactivated = false;
        rm.active_instances.push_back(std::move(inst_res.value()));
    }

    auto inactive_cnt_res = r.read_u32();
    if (!inactive_cnt_res || inactive_cnt_res.value() > kMaxSerializedEntities ||
        inactive_cnt_res.value() > r.remaining()) {
        return Error{"Corrupt inactive instance count"};
    }
    rm.inactive_instances.reserve(inactive_cnt_res.value());
    for (u32 i = 0; i < inactive_cnt_res.value(); ++i) {
        auto inst_res = read_instance(r, symbols);
        if (!inst_res) return inst_res.error();
        inst_res.value().deactivated = true;
        rm.inactive_instances.push_back(std::move(inst_res.value()));
    }

    auto tile_cnt_res = r.read_u32();
    if (!tile_cnt_res || tile_cnt_res.value() > kMaxSerializedEntities ||
        tile_cnt_res.value() > r.remaining()) {
        return Error{"Corrupt tile count"};
    }
    rm.tiles.reserve(tile_cnt_res.value());
    for (u32 i = 0; i < tile_cnt_res.value(); ++i) {
        auto tile_res = read_tile(r);
        if (!tile_res) return tile_res.error();
        rm.tiles.push_back(tile_res.value());
    }

    return rm;
}

} // namespace

bool Runner::save_game(std::string_view path) {
    if (path.empty() || !vm_ || current_room_ < 0 || !room_exists(current_room_)) {
        return false;
    }

    SaveWriter w;
    // Header (mirrors GM8 gm81.exe.c:sub_5B8AAC version 540 + game_id).
    w.write_u32(kSaveMagic);
    w.write_i32(kGmSaveVersion);
    w.write_u32(kSaveFormatRev);
    w.write_i32(game_.meta.game_id);

    // Global runner state.
    w.write_i32(current_room_);
    w.write_i32(transition_kind_);
    w.write_f64(score_);
    w.write_f64(lives_);
    w.write_f64(health_);
    w.write_bool(no_more_lives_armed_);
    w.write_bool(no_more_health_armed_);
    w.write_i32(cursor_sprite_);
    w.write_i32(cursor_subimg_);
    w.write_bool(auto_draw_);

    // Global variables & globalvar declarations (gm81.exe.c:sub_5725CC).
    w.write_i32(kGlobalsMarker);
    write_variable_table(w, vm_->globals(), program_.symbols);

    const auto& decls = vm_->declared_globals();
    std::vector<std::string_view> decl_names;
    for (usize i = 0; i < decls.size(); ++i) {
        if (!decls[i]) continue;
        const std::string_view name = program_.symbols.user_var_name(kUserVarIdBase + static_cast<u32>(i));
        if (!name.empty()) decl_names.push_back(name);
    }
    w.write_u32(static_cast<u32>(decl_names.size()));
    for (std::string_view name : decl_names) {
        w.write_string(name);
    }

    // Active room + saved persistent rooms (gm81.exe.c:sub_5B8AAC rooms loop).
    const u32 room_count = static_cast<u32>(game_.rooms.size());
    w.write_u32(room_count);
    for (u32 ridx = 0; ridx < room_count; ++ridx) {
        const i32 rid = static_cast<i32>(ridx);
        if (rid == current_room_) {
            w.write_bool(true);
            std::vector<const Instance*> active_ptrs;
            active_ptrs.reserve(instances_.size());
            for (const auto& up : instances_) active_ptrs.push_back(up.get());

            std::vector<const Instance*> inactive_ptrs;
            inactive_ptrs.reserve(inactive_instances_.size());
            for (const auto& up : inactive_instances_) inactive_ptrs.push_back(up.get());

            write_room_data(w,
                            room_caption_, room_width_, room_height_, room_speed_, room_persistent_,
                            bg_color_, bg_showcolor_, backgrounds_, view_enabled_, views_,
                            active_ptrs, inactive_ptrs, tiles_, program_.symbols);
        } else if (const auto it = saved_rooms_.find(rid); it != saved_rooms_.end()) {
            w.write_bool(true);
            const SavedRoomState& st = it->second;
            std::vector<const Instance*> active_ptrs;
            std::vector<const Instance*> inactive_ptrs;
            for (const auto& inst : st.instances) {
                if (inst.deactivated) inactive_ptrs.push_back(&inst);
                else active_ptrs.push_back(&inst);
            }
            write_room_data(w,
                            st.caption, st.width, st.height, st.speed, st.persistent,
                            st.background_color, st.background_showcolor, st.backgrounds,
                            st.view_enabled, st.views,
                            active_ptrs, inactive_ptrs, st.tiles, program_.symbols);
        } else {
            w.write_bool(false);
        }
    }

    // Footer: instance & tile ID allocators.
    w.write_i32(next_instance_id_);
    w.write_i32(next_tile_id_);

    if (auto res = vfs_.write_all_bytes(path, w.bytes()); !res) {
        GM_LOG_WARN("runtime", "game_save('%.*s') failed: %s",
                    static_cast<int>(path.size()), path.data(), res.error_message().c_str());
        return false;
    }
    return true;
}

bool Runner::load_game(std::string_view path) {
    // Exact GM8 behavior (gm81.exe.c:sub_5B874C): silently do nothing if file does not exist.
    if (path.empty() || !vm_ || !vfs_.exists(path)) {
        return false;
    }

    auto bytes_res = vfs_.read_all_bytes(path);
    if (!bytes_res) {
        GM_LOG_WARN("runtime", "game_load('%.*s') read error: %s",
                    static_cast<int>(path.size()), path.data(), bytes_res.error_message().c_str());
        return false;
    }

    auto report_version_error = [&]() {
        error_occurred_ = true;
        error_last_ = "Cannot load game saved with other version.";
        GM_LOG_WARN("runtime", "game_load('%.*s'): %s",
                    static_cast<int>(path.size()), path.data(), error_last_.c_str());
    };

    BinaryReader r(bytes_res.value());
    auto magic_res = r.read_u32();
    auto ver_res = r.read_i32();
    auto rev_res = r.read_u32();
    if (!magic_res || !ver_res || !rev_res ||
        magic_res.value() != kSaveMagic ||
        ver_res.value() != kGmSaveVersion ||
        (rev_res.value() != 1u && rev_res.value() != kSaveFormatRev)) {
        report_version_error();
        return false;
    }
    const u32 file_rev = rev_res.value();

    auto gid_res = r.read_i32();
    if (!gid_res) {
        report_version_error();
        return false;
    }
    if (gid_res.value() != game_.meta.game_id) {
        error_occurred_ = true;
        error_last_ = "This is a save file for a different game!";
        GM_LOG_WARN("runtime", "game_load('%.*s'): %s",
                    static_cast<int>(path.size()), path.data(), error_last_.c_str());
        return false;
    }

    // Parse entire snapshot into temporary state first so corrupt files never corrupt live state.
    auto cur_room_res = r.read_i32();
    auto trans_kind_res = r.read_i32();
    auto score_res = r.read_f64();
    auto lives_res = r.read_f64();
    auto health_res = r.read_f64();
    auto armed_lives_res = r.read_u8();
    auto armed_health_res = r.read_u8();
    auto cursor_res = r.read_i32();
    i32 loaded_cursor_subimg = 0;
    if (file_rev >= 2u) {
        auto cursor_sub_res = r.read_i32();
        if (!cursor_sub_res) {
            report_version_error();
            return false;
        }
        loaded_cursor_subimg = cursor_sub_res.value();
    }
    auto autodraw_res = r.read_u8();
    if (!cur_room_res || !trans_kind_res || !score_res || !lives_res || !health_res ||
        !armed_lives_res || !armed_health_res || !cursor_res || !autodraw_res) {
        report_version_error();
        return false;
    }

    auto glob_marker_res = r.read_i32();
    if (!glob_marker_res || glob_marker_res.value() != kGlobalsMarker) {
        report_version_error();
        return false;
    }

    auto globals_res = read_variable_table(r, program_.symbols);
    if (!globals_res) {
        report_version_error();
        return false;
    }

    auto decl_count_res = r.read_u32();
    if (!decl_count_res || decl_count_res.value() > kMaxSerializedVars ||
        decl_count_res.value() > r.remaining()) {
        report_version_error();
        return false;
    }
    std::vector<std::string> loaded_decl_names;
    loaded_decl_names.reserve(decl_count_res.value());
    for (u32 i = 0; i < decl_count_res.value(); ++i) {
        auto name_res = r.read_gm_string();
        if (!name_res) {
            report_version_error();
            return false;
        }
        loaded_decl_names.push_back(std::move(name_res.value()));
    }

    auto room_count_res = r.read_u32();
    if (!room_count_res || room_count_res.value() != static_cast<u32>(game_.rooms.size())) {
        report_version_error();
        return false;
    }
    const u32 room_count = room_count_res.value();

    std::unordered_map<i32, LoadedRoomData> loaded_rooms;
    for (u32 ridx = 0; ridx < room_count; ++ridx) {
        auto exists_res = r.read_u8();
        if (!exists_res) {
            report_version_error();
            return false;
        }
        if (exists_res.value() != 0u) {
            auto rm_res = read_room_data(r, program_.symbols);
            if (!rm_res) {
                report_version_error();
                return false;
            }
            loaded_rooms.emplace(static_cast<i32>(ridx), std::move(rm_res.value()));
        }
    }

    auto next_inst_res = r.read_i32();
    auto next_tile_res = r.read_i32();
    if (!next_inst_res || !next_tile_res) {
        report_version_error();
        return false;
    }

    const i32 loaded_current_room = cur_room_res.value();
    auto active_rm_it = loaded_rooms.find(loaded_current_room);
    if (active_rm_it == loaded_rooms.end() || !room_exists(loaded_current_room)) {
        report_version_error();
        return false;
    }

    // Commit parsed state atomically.
    cancel_room_transition();

    vm_->globals() = std::move(globals_res.value());
    vm_->clear_declared_globals();
    for (const auto& name : loaded_decl_names) {
        if (!name.empty()) {
            vm_->declare_global(program_.symbols.intern_user_var(name));
        }
    }

    current_room_ = loaded_current_room;
    transition_kind_ = trans_kind_res.value();
    score_ = score_res.value();
    lives_ = lives_res.value();
    health_ = health_res.value();
    no_more_lives_armed_ = armed_lives_res.value() != 0u;
    no_more_health_armed_ = armed_health_res.value() != 0u;
    cursor_sprite_ = cursor_res.value();
    cursor_subimg_ = loaded_cursor_subimg;
    auto_draw_ = autodraw_res.value() != 0u;

    instances_.clear();
    inactive_instances_.clear();
    object_instances_.clear();
    inst_by_id_.clear();
    tiles_.clear();
    saved_rooms_.clear();
    cached_field_indices_ = {0, 0};
    next_handle_ = -8;

    i32 max_inst_id = 100000;
    i32 max_tile_id = 10000000;
    for (const auto& [_, rm] : loaded_rooms) {
        for (const auto& inst : rm.active_instances) max_inst_id = std::max(max_inst_id, inst.id);
        for (const auto& inst : rm.inactive_instances) max_inst_id = std::max(max_inst_id, inst.id);
        for (const auto& t : rm.tiles) max_tile_id = std::max(max_tile_id, t.id);
    }

    LoadedRoomData active_rm = std::move(active_rm_it->second);
    loaded_rooms.erase(active_rm_it);

    room_caption_ = std::move(active_rm.caption);
    room_width_ = active_rm.width;
    room_height_ = active_rm.height;
    room_speed_ = active_rm.speed;
    room_persistent_ = active_rm.persistent;
    bg_color_ = active_rm.bg_color;
    bg_showcolor_ = active_rm.bg_showcolor;
    backgrounds_ = active_rm.backgrounds;
    view_enabled_ = active_rm.view_enabled;
    views_ = active_rm.views;
    tiles_ = std::move(active_rm.tiles);

    // Restore inactive instances first, then active instances so live IDs take precedence in inst_by_id_.
    inactive_instances_.reserve(active_rm.inactive_instances.size());
    for (auto& inst_val : active_rm.inactive_instances) {
        auto up = std::make_unique<Instance>(std::move(inst_val));
        up->deactivated = true;
        up->mark_bbox_dirty();
        register_instance_handle(*up);
        inst_by_id_[up->id] = up.get();
        inactive_instances_.push_back(std::move(up));
    }

    instances_.reserve(active_rm.active_instances.size());
    for (auto& inst_val : active_rm.active_instances) {
        auto up = std::make_unique<Instance>(std::move(inst_val));
        up->deactivated = false;
        up->mark_bbox_dirty();
        register_instance_handle(*up);
        if (!up->destroyed || !inst_by_id_.contains(up->id)) {
            inst_by_id_[up->id] = up.get();
        }
        index_instance(*up);
        instances_.push_back(std::move(up));
    }

    for (auto& [rid, rm] : loaded_rooms) {
        SavedRoomState st{};
        st.caption = std::move(rm.caption);
        st.width = rm.width;
        st.height = rm.height;
        st.speed = rm.speed;
        st.persistent = rm.persistent;
        st.background_color = rm.bg_color;
        st.background_showcolor = rm.bg_showcolor;
        st.backgrounds = rm.backgrounds;
        st.view_enabled = rm.view_enabled;
        st.views = rm.views;
        st.tiles = std::move(rm.tiles);
        st.instances.reserve(rm.active_instances.size() + rm.inactive_instances.size());
        for (auto& inst : rm.active_instances) st.instances.push_back(std::move(inst));
        for (auto& inst : rm.inactive_instances) st.instances.push_back(std::move(inst));
        saved_rooms_.emplace(rid, std::move(st));
    }

    next_instance_id_ = std::max(next_inst_res.value(), max_inst_id + 1);
    next_tile_id_ = std::max(next_tile_res.value(), max_tile_id + 1);

    pending_action_ = PendingRoomAction::None;
    pending_room_id_ = -1;
    pending_load_path_.clear();

    // Update screen region, window size, caption and redraw (gm81.exe.c:sub_58F93C).
    sync_room_screen_region();
    update_window_caption();
    if (auto_draw_ && window_.is_open()) {
        render_current_frame();
        present_current_frame();
    } else {
        update_view_following();
    }
    return true;
}

} // namespace gm
