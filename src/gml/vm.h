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
    RValue scalar;
    bool initialized = false;
    std::unordered_map<u32, RValue> array; // linear key = row * 32000 + col (0 aliases scalar)
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
class IVmHost {
public:
    virtual ~IVmHost() = default;

    [[nodiscard]] virtual bool treat_uninitialized_as_zero() const noexcept { return true; }

    [[nodiscard]] virtual Result<RValue> get_builtin_global(u32 var_id, i32 idx, bool has_idx) = 0;
    [[nodiscard]] virtual Result<void> set_builtin_global(u32 var_id, i32 idx, bool has_idx, RValue value) = 0;

    [[nodiscard]] virtual Result<RValue> get_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx) = 0;
    [[nodiscard]] virtual Result<void> set_builtin_inst(i32 inst_id, u32 var_id, i32 idx, bool has_idx, RValue value) = 0;

    [[nodiscard]] virtual VariableTable* get_instance_vars(i32 inst_id) = 0;

    // Resolves target_id (-1=self, -2=other, -3=all, -4=noone, 0..99999=object, >=100000=instance)
    // into a snapshot list of active instance IDs.
    virtual void resolve_target_instances(i32 target_id, i32 self_id, i32 other_id, std::vector<i32>& out) = 0;

    [[nodiscard]] virtual Result<RValue> call_builtin(u32 func_id, i32 self_id, i32 other_id, std::span<const RValue> args) = 0;
    [[nodiscard]] virtual Result<RValue> call_extension_dll(u32 ext_dll_id, std::span<const RValue> args) = 0;
};

class GmlVm : public NonCopyable {
public:
    GmlVm(CompiledProgram& program, IVmHost* host = nullptr);

    void set_host(IVmHost* host) noexcept { host_ = host; }
    [[nodiscard]] VariableTable& globals() noexcept { return globals_; }
    [[nodiscard]] const VariableTable& globals() const noexcept { return globals_; }

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
    struct WithFrame {
        std::vector<i32> targets;
        usize index = 0;
        i32 saved_self = -1;
        i32 saved_other = -1;
    };

    static Result<u32> compute_array_key(i32 i0, i32 i1, u8 dims) noexcept;

    CompiledProgram& program_;
    IVmHost* host_ = nullptr;
    VariableTable globals_;
    std::vector<RValue> const_slots_;

    std::array<RValue, 16> current_args_{};
    i32 current_argc_ = 0;
    u32 call_depth_ = 0;
};

} // namespace gm
