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
    Dup,                // duplicate top
    DupN,               // aux = n: duplicate top n values (for compound array/field assignments)
    Pop,                // discard top

    // Local (var) variables
    LoadLocal,          // operand = local slot
    StoreLocal,         // operand = local slot
    LoadLocalArr,       // operand = local slot, aux = dims (1 or 2)
    StoreLocalArr,      // operand = local slot, aux = dims (1 or 2)

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
    WithPop,            // pops active with iterator (used by break/return inside with)

    // Calls & returns
    CallBuiltin,        // operand = builtin_func_id, aux = argc
    CallScript,         // operand = script_id, aux = argc
    CallExtGml,         // operand = ext_gml_id, aux = argc
    CallExtDll,         // operand = ext_dll_id, aux = argc
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
    u32 local_count = 0;

    u32 add_real(f64 v);
    u32 add_string(std::string s);
    u32 emit(OpCode op, u32 operand = 0, u8 aux = 0, u16 line = 0);
    void patch_operand(u32 instr_idx, u32 operand);

    [[nodiscard]] std::string disassemble() const;
};

} // namespace gm
