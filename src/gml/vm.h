#pragma once

#include "base/types.h"
#include "gml/bytecode.h"
#include "gml/compiler.h"
#include "gml/value.h"

#include <array>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gm {

inline constexpr i32 kGmArrayMaxDim = 32000;

struct VarSlot {
    struct ArrayRow {
        u32 length = 0;
        std::unordered_map<u32, RValue> values;
    };
    RValue scalar;
    std::unordered_map<u32, ArrayRow> rows; // unwritten columns within a row's length read as zero
};

class VariableTable {
public:
    [[nodiscard]] bool exists(u32 var_id) const noexcept;
    [[nodiscard]] const RValue* get_scalar(u32 var_id) const noexcept;
    void set_scalar(u32 var_id, RValue value);

    [[nodiscard]] Result<const RValue*> get_array(u32 var_id, i32 i0, i32 i1, u8 dims) const noexcept;
    [[nodiscard]] Result<void> set_array(u32 var_id, i32 i0, i32 i1, u8 dims, RValue value);

    void clear();
    [[nodiscard]] std::vector<u32> all_var_ids() const;

private:
    std::unordered_map<u32, VarSlot> slots_;
};

// Runtime host interface implemented by gm_runtime (Phases 3-6).
// Allows gm_gml VM to run completely decoupled from graphics/audio/rooms.
enum class TargetUse { With, ReadField, WriteField };

class IVmHost {
public:
    virtual ~IVmHost() = default;

    [[nodiscard]] virtual bool treat_uninitialized_as_zero() const noexcept { return true; }
    [[nodiscard]] virtual bool room_change_pending() const noexcept { return false; }

    [[nodiscard]] virtual Result<RValue> get_builtin_global(u32 var_id, i32 idx, bool has_idx) = 0;
    [[nodiscard]] virtual Result<void> set_builtin_global(u32 var_id, i32 idx, bool has_idx, RValue value) = 0;

    [[nodiscard]] virtual Result<RValue> get_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx) = 0;
    [[nodiscard]] virtual Result<void> set_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx, RValue value) = 0;

    [[nodiscard]] virtual VariableTable* get_instance_vars(i32 inst_id) = 0;
    [[nodiscard]] virtual i32 instance_handle(i32 id) const { return id; }

    // GM8 captures the slot count, then looks up each slot in the current room/object list.
    // Returns an instance handle, or noone for a destroyed instance or a removed slot.
    [[nodiscard]] virtual usize target_instance_count(i32 target) const = 0;
    [[nodiscard]] virtual i32 target_instance_at(i32 target, usize index, TargetUse use) const = 0;

    [[nodiscard]] virtual Result<RValue> call_builtin(u32 func_id, i32 self_id, i32 other_id, std::span<const RValue> args) = 0;
    [[nodiscard]] virtual Result<RValue> call_extension_dll(u32 ext_dll_id, std::span<const RValue> args) = 0;
};

class GmlVm : public NonCopyable {
public:
    GmlVm(CompiledProgram& program, IVmHost* host = nullptr);

    void set_host(IVmHost* host) noexcept { host_ = host; }
    [[nodiscard]] VariableTable& globals() noexcept { return globals_; }
    [[nodiscard]] const VariableTable& globals() const noexcept { return globals_; }
    void reset_globals();

    // Evaluates all expression constants once at game startup.
    [[nodiscard]] Result<void> init_constants(i32 dummy_self_id = -1);

    [[nodiscard]] Result<RValue> execute(
        const BytecodeChunk& chunk,
        i32 self_id,
        i32 other_id,
        std::span<const RValue> args = {});

    [[nodiscard]] Result<RValue> execute_script(
        u32 script_id,
        i32 self_id,
        i32 other_id,
        std::span<const RValue> args = {});

    [[nodiscard]] Result<RValue> execute_ext_gml(
        u32 ext_gml_id,
        i32 self_id,
        i32 other_id,
        std::span<const RValue> args = {});

    [[nodiscard]] Result<RValue> execute_string(
        std::string_view code,
        i32 self_id,
        i32 other_id,
        std::span<const RValue> args = {});

    // Accessors for built-in argument0..15 / argument[] / argument_count in current frame.
    [[nodiscard]] RValue get_argument(i32 idx) const noexcept;
    void set_argument(i32 idx, RValue value);
    [[nodiscard]] i32 argument_count() const noexcept { return current_argc_; }

private:
    struct TargetIterator {
        i32 target;
        TargetUse use;
        usize count;
        usize next_index = 0;
    };

    struct WithFrame {
        TargetIterator targets;
        i32 saved_self;
        i32 saved_other;
    };

    [[nodiscard]] TargetIterator begin_targets(i32 target, TargetUse use) const;
    [[nodiscard]] i32 next_target(TargetIterator& it, i32 self_id, i32 other_id) const;
    [[nodiscard]] bool is_declared_global(u32 var_id) const noexcept;

    CompiledProgram& program_;
    IVmHost* host_ = nullptr;
    VariableTable globals_;
    std::unordered_map<i32, VariableTable> standalone_instances_;
    std::vector<bool> declared_globals_;
    std::vector<RValue> const_slots_;

    std::array<RValue, 16> current_args_{};
    i32 current_argc_ = 0;
    u32 call_depth_ = 0;
};

} // namespace gm
