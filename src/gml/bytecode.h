#pragma once

#include "base/types.h"
#include "gml/symbol_table.h"

#include <string>
#include <vector>

namespace gm {

enum class OpCode : u8 {
    Nop = 0,

    // Constants & stack manipulation
    PushReal,           // operand = real_pool index
    PushString,         // operand = string_pool index
    PushZero,           // fast push 0.0
    LoadConstSlot,      // operand = runtime-evaluated constant slot index
    Swap,               // exchange top two values
    DupN,               // duplicate aux values, skipping operand values at the top
    Pop,                // discard top

    // Temporary slots used by control flow
    LoadTemp,
    StoreTemp,

    // Local (var) variables
    LoadLocal,          // operand = user var_id in the current call frame
    StoreLocal,         // operand = user var_id
    LoadLocalArr,       // operand = user var_id, aux = dims (1 or 2)
    StoreLocalArr,      // operand = user var_id, aux = dims (1 or 2)

    // Bare user variables resolve to self until a globalvar declaration executes.
    DeclareGlobal,
    LoadVar,
    StoreVar,
    LoadArr,
    StoreArr,

    // Self-scoped user variables
    LoadSelfVar,        // operand = user var_id (>= 100000)
    StoreSelfVar,       // operand = user var_id
    LoadSelfArr,        // operand = user var_id, aux = dims (1 or 2)
    StoreSelfArr,       // operand = user var_id, aux = dims (1 or 2)

    // Global-scoped user variables
    LoadGlobalVar,      // operand = user var_id
    StoreGlobalVar,     // operand = user var_id
    LoadGlobalArr,      // operand = user var_id, aux = dims (1 or 2)
    StoreGlobalArr,     // operand = user var_id, aux = dims (1 or 2)

    // Dynamic target field access (target scope/object/instance ID on stack)
    LoadField,          // operand = var_id (builtin or user)
    StoreField,         // operand = var_id (builtin or user)
    LoadFieldArr,       // operand = var_id, aux = dims (1 or 2)
    StoreFieldArr,      // operand = var_id, aux = dims (1 or 2)

    // Built-in variables
    LoadBuiltinGlobal,    // operand = builtin_var_id
    StoreBuiltinGlobal,   // operand = builtin_var_id
    LoadBuiltinGlobalArr, // operand = builtin_var_id, aux = dims (1 or 2)
    StoreBuiltinGlobalArr,// operand = builtin_var_id, aux = dims (1 or 2)
    LoadBuiltinInst,      // operand = builtin_var_id (on self)
    StoreBuiltinInst,     // operand = builtin_var_id (on self)
    LoadBuiltinInstArr,   // operand = builtin_var_id, aux = dims (1 or 2)
    StoreBuiltinInstArr,  // operand = builtin_var_id, aux = dims (1 or 2)

    // Operators (exact GM8 semantics; non-short-circuiting LogAnd/LogOr)
    Add,
    Sub,
    Mul,
    Div,
    IntDiv,
    Mod,
    Neg,
    Pos,
    BitNot,
    LogNot,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    LogAnd,
    LogOr,
    LogXor,
    CmpEq,
    CmpNeq,
    CmpLt,
    CmpLe,
    CmpGt,
    CmpGe,

    // Control flow
    Jump,               // operand = target ip
    JumpIfFalse,        // operand = target ip (pops condition)
    JumpIfTrue,         // operand = target ip (pops condition)
    RepeatInit,         // operand = end_ip, aux = temp local slot (pops count, jumps to end_ip if <= 0)
    RepeatNext,         // operand = loop_start_ip, aux = temp local slot (decrements & loops if > 0)
    WithPush,           // operand = end_ip (pops target expr; pushes with iterator; jumps to end_ip if empty)
    WithNext,           // operand = loop_start_ip (advances with iterator; jumps to loop_start_ip or pops)
    WithBreak,          // pop scope; object/all selectors jump to the end of this with

    // Calls & returns
    CallBuiltin,        // operand = builtin_func_id, aux = argc
    CallScript,         // operand = script_id, aux = argc
    CallExtGml,         // operand = ext_gml_id, aux = argc
    CallExtDll,         // operand = ext_dll_id, aux = argc
    RunCodeAction,      // operand = index into this event's code_actions
    JumpIfRoomChange,   // at a D&D action boundary, jump to the event epilogue
    Return,             // pops return value and returns
    Exit,               // returns 0.0 immediately
};

struct Instruction {
    OpCode op = OpCode::Nop;
    u8 aux = 0;
    u16 line = 0;
    u32 operand = 0;
};

struct BytecodeChunk {
    std::string debug_name;
    std::vector<Instruction> code;
    std::vector<f64> reals;
    std::vector<std::string> strings;
    std::vector<BytecodeChunk> code_actions;
    u32 local_count = 0;
    u32 temp_count = 0;

    u32 add_real(f64 v);
    u32 add_string(std::string s);
    u32 emit(OpCode op, u32 operand = 0, u8 aux = 0, u16 line = 0);
    void patch_operand(u32 instr_idx, u32 operand);

    [[nodiscard]] std::string disassemble() const;
    [[nodiscard]] usize instruction_count() const noexcept;
};

} // namespace gm
