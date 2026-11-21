#include "gml/bytecode.h"

#include <cstdio>

namespace gm {

namespace {

const char* opcode_name(OpCode op) noexcept {
    switch (op) {
        case OpCode::Nop:                  return "NOP";
        case OpCode::PushReal:             return "PUSH_REAL";
        case OpCode::PushString:           return "PUSH_STR";
        case OpCode::PushZero:             return "PUSH_ZERO";
        case OpCode::LoadConstSlot:        return "LOAD_CONST_SLOT";
        case OpCode::Swap:                 return "SWAP";
        case OpCode::DupN:                 return "DUP_N";
        case OpCode::Pop:                  return "POP";
        case OpCode::LoadTemp:             return "LOAD_TEMP";
        case OpCode::StoreTemp:            return "STORE_TEMP";
        case OpCode::DeclareGlobal:        return "DECLARE_GLOBAL";
        case OpCode::LoadVar:              return "LOAD_VAR";
        case OpCode::StoreVar:             return "STORE_VAR";
        case OpCode::LoadArr:              return "LOAD_ARR";
        case OpCode::StoreArr:             return "STORE_ARR";
        case OpCode::LoadLocal:            return "LOAD_LOCAL";
        case OpCode::StoreLocal:           return "STORE_LOCAL";
        case OpCode::LoadLocalArr:         return "LOAD_LOCAL_ARR";
        case OpCode::StoreLocalArr:        return "STORE_LOCAL_ARR";
        case OpCode::LoadSelfVar:          return "LOAD_SELF";
        case OpCode::StoreSelfVar:         return "STORE_SELF";
        case OpCode::LoadSelfArr:          return "LOAD_SELF_ARR";
        case OpCode::StoreSelfArr:         return "STORE_SELF_ARR";
        case OpCode::LoadGlobalVar:        return "LOAD_GLOBAL";
        case OpCode::StoreGlobalVar:       return "STORE_GLOBAL";
        case OpCode::LoadGlobalArr:        return "LOAD_GLOBAL_ARR";
        case OpCode::StoreGlobalArr:       return "STORE_GLOBAL_ARR";
        case OpCode::LoadField:            return "LOAD_FIELD";
        case OpCode::StoreField:           return "STORE_FIELD";
        case OpCode::LoadFieldArr:         return "LOAD_FIELD_ARR";
        case OpCode::StoreFieldArr:        return "STORE_FIELD_ARR";
        case OpCode::LoadBuiltinGlobal:    return "LOAD_BG";
        case OpCode::StoreBuiltinGlobal:   return "STORE_BG";
        case OpCode::LoadBuiltinGlobalArr: return "LOAD_BG_ARR";
        case OpCode::StoreBuiltinGlobalArr:return "STORE_BG_ARR";
        case OpCode::LoadBuiltinInst:      return "LOAD_BI";
        case OpCode::StoreBuiltinInst:     return "STORE_BI";
        case OpCode::LoadBuiltinInstArr:   return "LOAD_BI_ARR";
        case OpCode::StoreBuiltinInstArr:  return "STORE_BI_ARR";
        case OpCode::Add:                  return "ADD";
        case OpCode::Sub:                  return "SUB";
        case OpCode::Mul:                  return "MUL";
        case OpCode::Div:                  return "DIV";
        case OpCode::IntDiv:               return "INT_DIV";
        case OpCode::Mod:                  return "MOD";
        case OpCode::Neg:                  return "NEG";
        case OpCode::Pos:                  return "POS";
        case OpCode::BitNot:               return "BIT_NOT";
        case OpCode::LogNot:               return "LOG_NOT";
        case OpCode::BitAnd:               return "BIT_AND";
        case OpCode::BitOr:                return "BIT_OR";
        case OpCode::BitXor:               return "BIT_XOR";
        case OpCode::Shl:                  return "SHL";
        case OpCode::Shr:                  return "SHR";
        case OpCode::LogAnd:               return "LOG_AND";
        case OpCode::LogOr:                return "LOG_OR";
        case OpCode::LogXor:               return "LOG_XOR";
        case OpCode::CmpEq:                return "CMP_EQ";
        case OpCode::CmpNeq:               return "CMP_NEQ";
        case OpCode::CmpLt:                return "CMP_LT";
        case OpCode::CmpLe:                return "CMP_LE";
        case OpCode::CmpGt:                return "CMP_GT";
        case OpCode::CmpGe:                return "CMP_GE";
        case OpCode::Jump:                 return "JMP";
        case OpCode::JumpIfFalse:          return "JMP_F";
        case OpCode::JumpIfTrue:           return "JMP_T";
        case OpCode::RepeatInit:           return "REPEAT_INIT";
        case OpCode::RepeatNext:           return "REPEAT_NEXT";
        case OpCode::WithPush:             return "WITH_PUSH";
        case OpCode::WithNext:             return "WITH_NEXT";
        case OpCode::WithBreak:            return "WITH_BREAK";
        case OpCode::CallBuiltin:          return "CALL_BUILTIN";
        case OpCode::CallScript:           return "CALL_SCRIPT";
        case OpCode::CallExtGml:           return "CALL_EXT_GML";
        case OpCode::CallExtDll:           return "CALL_EXT_DLL";
        case OpCode::RunCodeAction:        return "RUN_CODE_ACTION";
        case OpCode::JumpIfRoomChange:     return "JMP_ROOM_CHANGE";
        case OpCode::Return:               return "RETURN";
        case OpCode::Exit:                 return "EXIT";
    }
    return "UNKNOWN";
}

} // namespace

u32 BytecodeChunk::add_real(f64 v) {
    reals.push_back(v);
    return static_cast<u32>(reals.size() - 1);
}

u32 BytecodeChunk::add_string(std::string s) {
    strings.push_back(std::move(s));
    return static_cast<u32>(strings.size() - 1);
}

u32 BytecodeChunk::emit(OpCode op, u32 operand, u8 aux, u16 line) {
    code.push_back(Instruction{op, aux, line, operand});
    return static_cast<u32>(code.size() - 1);
}

void BytecodeChunk::patch_operand(u32 instr_idx, u32 operand) {
    if (instr_idx < code.size()) {
        code[instr_idx].operand = operand;
    }
}

std::string BytecodeChunk::disassemble() const {
    std::string out;
    out += "=== " + debug_name + " (locals=" + std::to_string(local_count) +
           ", insns=" + std::to_string(code.size()) + ") ===\n";
    char line_buf[160];
    for (usize i = 0; i < code.size(); ++i) {
        const auto& ins = code[i];
        std::snprintf(line_buf, sizeof(line_buf), "%04zu  L%-4u %-16s op=%-8u aux=%u",
                      i, static_cast<unsigned>(ins.line), opcode_name(ins.op),
                      ins.operand, static_cast<unsigned>(ins.aux));
        out += line_buf;
        if (ins.op == OpCode::PushReal && ins.operand < reals.size()) {
            std::snprintf(line_buf, sizeof(line_buf), "  ; %.15g", reals[ins.operand]);
            out += line_buf;
        } else if (ins.op == OpCode::PushString && ins.operand < strings.size()) {
            out += "  ; \"" + strings[ins.operand] + "\"";
        }
        out.push_back('\n');
    }
    for (const auto& action : code_actions) out += action.disassemble();
    return out;
}

usize BytecodeChunk::instruction_count() const noexcept {
    usize count = code.size();
    for (const auto& action : code_actions) count += action.code.size();
    return count;
}

} // namespace gm
