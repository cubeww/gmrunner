#include "gml/vm.h"

#include "base/log.h"
#include "base/math.h"

#include <algorithm>

namespace gm {

namespace {

inline constexpr u32 kMaxCallDepth = 512;

Result<u32> pack_array_index(i32 i0, i32 i1, u8 dims) noexcept {
    if (dims == 1) {
        if (i0 < 0 || i0 >= kGmArrayMaxDim) {
            return Error{"Array index out of bounds: " + std::to_string(i0)};
        }
        return static_cast<u32>(i0);
    }
    if (i0 < 0 || i0 >= kGmArrayMaxDim || i1 < 0 || i1 >= kGmArrayMaxDim) {
        return Error{"2D array index out of bounds: [" + std::to_string(i0) + ", " + std::to_string(i1) + "]"};
    }
    return static_cast<u32>(i0) * static_cast<u32>(kGmArrayMaxDim) + static_cast<u32>(i1);
}

} // namespace

bool VariableTable::exists(u32 var_id) const noexcept {
    const auto it = slots_.find(var_id);
    return it != slots_.end() && (it->second.initialized || !it->second.array.empty());
}

const RValue* VariableTable::get_scalar(u32 var_id) const noexcept {
    const auto it = slots_.find(var_id);
    if (it == slots_.end() || !it->second.initialized) {
        return nullptr;
    }
    return &it->second.scalar;
}

void VariableTable::set_scalar(u32 var_id, RValue value) {
    auto& slot = slots_[var_id];
    slot.scalar = std::move(value);
    slot.initialized = true;
}

Result<const RValue*> VariableTable::get_array(u32 var_id, i32 i0, i32 i1, u8 dims) const noexcept {
    auto key_res = pack_array_index(i0, i1, dims);
    if (!key_res) return key_res.error();
    const u32 key = key_res.value();

    const auto it = slots_.find(var_id);
    if (it == slots_.end()) {
        return static_cast<const RValue*>(nullptr);
    }
    if (key == 0u) {
        return it->second.initialized ? &it->second.scalar : static_cast<const RValue*>(nullptr);
    }
    const auto ait = it->second.array.find(key);
    return (ait != it->second.array.end()) ? &ait->second : static_cast<const RValue*>(nullptr);
}

Result<void> VariableTable::set_array(u32 var_id, i32 i0, i32 i1, u8 dims, RValue value) {
    auto key_res = pack_array_index(i0, i1, dims);
    if (!key_res) return key_res.error();
    const u32 key = key_res.value();

    auto& slot = slots_[var_id];
    if (key == 0u) {
        slot.scalar = std::move(value);
        slot.initialized = true;
    } else {
        slot.array[key] = std::move(value);
    }
    return Result<void>::success();
}

void VariableTable::clear() {
    slots_.clear();
}

std::vector<u32> VariableTable::all_var_ids() const {
    std::vector<u32> ids;
    ids.reserve(slots_.size());
    for (const auto& [id, slot] : slots_) {
        if (slot.initialized || !slot.array.empty()) {
            ids.push_back(id);
        }
    }
    return ids;
}

GmlVm::GmlVm(CompiledProgram& program, IVmHost* host)
    : program_(program), host_(host) {
    const_slots_.resize(program_.const_init_chunks.size());
}

Result<u32> GmlVm::compute_array_key(i32 i0, i32 i1, u8 dims) noexcept {
    return pack_array_index(i0, i1, dims);
}

RValue GmlVm::get_argument(i32 idx) const noexcept {
    if (idx < 0 || idx >= 16) return RValue(0.0);
    return current_args_[static_cast<usize>(idx)];
}

void GmlVm::set_argument(i32 idx, RValue value) {
    if (idx >= 0 && idx < 16) {
        current_args_[static_cast<usize>(idx)] = std::move(value);
    }
}

Result<void> GmlVm::init_constants(i32 dummy_self_id) {
    const_slots_.resize(program_.const_init_chunks.size());
    for (usize i = 0; i < program_.const_init_chunks.size(); ++i) {
        auto res = execute(program_.const_init_chunks[i], dummy_self_id, dummy_self_id);
        if (!res) {
            GM_LOG_WARN("gml", "Failed initializing constant slot %zu (%s): %s",
                        i, program_.const_init_chunks[i].debug_name.c_str(), res.error_message().c_str());
            const_slots_[i] = RValue(0.0);
        } else {
            const_slots_[i] = std::move(res.value());
        }
    }
    return Result<void>::success();
}

Result<RValue> GmlVm::execute_script(u32 script_id, i32 self_id, i32 other_id, std::span<const RValue> args) {
    if (script_id >= program_.script_chunks.size() || !program_.script_chunks[script_id].has_value()) {
        return RValue(0.0);
    }
    return execute(*program_.script_chunks[script_id], self_id, other_id, args);
}

Result<RValue> GmlVm::execute_ext_gml(u32 ext_gml_id, i32 self_id, i32 other_id, std::span<const RValue> args) {
    if (ext_gml_id >= program_.ext_gml_chunks.size()) {
        return RValue(0.0);
    }
    return execute(program_.ext_gml_chunks[ext_gml_id], self_id, other_id, args);
}

Result<RValue> GmlVm::execute_string(std::string_view code, i32 self_id, i32 other_id, std::span<const RValue> args) {
    GmlCompiler::prescan_globalvars(code, program_.symbols);
    auto chunk_res = GmlCompiler::compile_chunk("<execute_string>", code, program_.symbols);
    if (!chunk_res) return chunk_res.error();
    return execute(chunk_res.value(), self_id, other_id, args);
}

Result<RValue> GmlVm::execute(
    const BytecodeChunk& chunk,
    i32 self_id,
    i32 other_id,
    std::span<const RValue> args) {
    if (call_depth_ >= kMaxCallDepth) {
        return Error{"GML call stack overflow in " + chunk.debug_name};
    }

    // Save & populate GM8 argument0..15 / argument_count registers.
    const auto saved_args = current_args_;
    const i32 saved_argc = current_argc_;
    current_args_.fill(RValue(0.0));
    const usize copy_cnt = std::min<usize>(args.size(), 16);
    for (usize i = 0; i < copy_cnt; ++i) {
        current_args_[i] = args[i];
    }
    current_argc_ = static_cast<i32>(copy_cnt);
    ++call_depth_;

    struct ScopeRestore {
        GmlVm* vm;
        const std::array<RValue, 16>& saved_args;
        i32 saved_argc;
        ~ScopeRestore() {
            vm->current_args_ = saved_args;
            vm->current_argc_ = saved_argc;
            --vm->call_depth_;
        }
    } guard{this, saved_args, saved_argc};

    std::vector<RValue> stack;
    stack.reserve(64);

    std::vector<VarSlot> locals(chunk.local_count);
    std::vector<WithFrame> with_stack;

    i32 cur_self = self_id;
    i32 cur_other = other_id;
    const bool uninit_zero = host_ ? host_->treat_uninitialized_as_zero() : true;

    auto pop = [&]() -> RValue {
        RValue v = std::move(stack.back());
        stack.pop_back();
        return v;
    };

    auto pop_indices = [&](u8 dims, i32& i0, i32& i1) {
        if (dims == 2) {
            i1 = pop().as_i32();
            i0 = pop().as_i32();
        } else {
            i0 = pop().as_i32();
            i1 = 0;
        }
    };

    auto handle_uninit = [&](u32 var_id) -> Result<RValue> {
        if (uninit_zero) {
            return RValue(0.0);
        }
        const std::string_view vname = program_.symbols.user_var_name(var_id);
        return Error{"Unknown variable " + std::string(vname) + " in " + chunk.debug_name};
    };

    const auto& code = chunk.code;
    usize ip = 0;

    while (ip < code.size()) {
        const Instruction& ins = code[ip++];

        switch (ins.op) {
            case OpCode::Nop:
                break;

            case OpCode::PushReal:
                stack.emplace_back(chunk.reals[ins.operand]);
                break;

            case OpCode::PushString:
                stack.emplace_back(chunk.strings[ins.operand]);
                break;

            case OpCode::PushZero:
                stack.emplace_back(0.0);
                break;

            case OpCode::LoadConstSlot:
                if (ins.operand < const_slots_.size()) {
                    stack.push_back(const_slots_[ins.operand]);
                } else {
                    stack.emplace_back(0.0);
                }
                break;

            case OpCode::Dup:
                stack.push_back(stack.back());
                break;

            case OpCode::DupN: {
                const usize n = ins.aux;
                const usize base = stack.size() - n;
                for (usize k = 0; k < n; ++k) {
                    stack.push_back(stack[base + k]);
                }
                break;
            }

            case OpCode::Pop:
                stack.pop_back();
                break;

            // ---- Local variables ----
            case OpCode::LoadLocal: {
                const auto& slot = locals[ins.operand];
                if (slot.initialized) {
                    stack.push_back(slot.scalar);
                } else if (uninit_zero) {
                    stack.emplace_back(0.0);
                } else {
                    return Error{"Uninitialized local variable in " + chunk.debug_name};
                }
                break;
            }

            case OpCode::StoreLocal: {
                locals[ins.operand].scalar = pop();
                locals[ins.operand].initialized = true;
                break;
            }

            case OpCode::LoadLocalArr: {
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                auto key_res = compute_array_key(i0, i1, ins.aux);
                if (!key_res) return key_res.error();
                const auto& slot = locals[ins.operand];
                if (key_res.value() == 0u) {
                    stack.push_back(slot.initialized ? slot.scalar : RValue(0.0));
                } else {
                    const auto it = slot.array.find(key_res.value());
                    stack.push_back(it != slot.array.end() ? it->second : RValue(0.0));
                }
                break;
            }

            case OpCode::StoreLocalArr: {
                RValue val = pop();
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                auto key_res = compute_array_key(i0, i1, ins.aux);
                if (!key_res) return key_res.error();
                auto& slot = locals[ins.operand];
                if (key_res.value() == 0u) {
                    slot.scalar = std::move(val);
                    slot.initialized = true;
                } else {
                    slot.array[key_res.value()] = std::move(val);
                }
                break;
            }

            // ---- Self user variables ----
            case OpCode::LoadSelfVar: {
                VariableTable* vt = host_ ? host_->get_instance_vars(cur_self) : &globals_;
                const RValue* v = vt ? vt->get_scalar(ins.operand) : nullptr;
                if (v) {
                    stack.push_back(*v);
                } else {
                    auto u = handle_uninit(ins.operand);
                    if (!u) return u.error();
                    stack.push_back(std::move(u.value()));
                }
                break;
            }

            case OpCode::StoreSelfVar: {
                RValue val = pop();
                if (VariableTable* vt = host_ ? host_->get_instance_vars(cur_self) : &globals_) {
                    vt->set_scalar(ins.operand, std::move(val));
                }
                break;
            }

            case OpCode::LoadSelfArr: {
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                VariableTable* vt = host_ ? host_->get_instance_vars(cur_self) : &globals_;
                if (!vt) {
                    stack.emplace_back(0.0);
                } else {
                    auto r = vt->get_array(ins.operand, i0, i1, ins.aux);
                    if (!r) return r.error();
                    if (r.value()) {
                        stack.push_back(*r.value());
                    } else {
                        auto u = handle_uninit(ins.operand);
                        if (!u) return u.error();
                        stack.push_back(std::move(u.value()));
                    }
                }
                break;
            }

            case OpCode::StoreSelfArr: {
                RValue val = pop();
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                if (VariableTable* vt = host_ ? host_->get_instance_vars(cur_self) : &globals_) {
                    if (auto e = vt->set_array(ins.operand, i0, i1, ins.aux, std::move(val)); !e) {
                        return e.error();
                    }
                }
                break;
            }

            // ---- Global user variables ----
            case OpCode::LoadGlobalVar: {
                if (const RValue* v = globals_.get_scalar(ins.operand)) {
                    stack.push_back(*v);
                } else {
                    auto u = handle_uninit(ins.operand);
                    if (!u) return u.error();
                    stack.push_back(std::move(u.value()));
                }
                break;
            }

            case OpCode::StoreGlobalVar:
                globals_.set_scalar(ins.operand, pop());
                break;

            case OpCode::LoadGlobalArr: {
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                auto r = globals_.get_array(ins.operand, i0, i1, ins.aux);
                if (!r) return r.error();
                if (r.value()) {
                    stack.push_back(*r.value());
                } else {
                    auto u = handle_uninit(ins.operand);
                    if (!u) return u.error();
                    stack.push_back(std::move(u.value()));
                }
                break;
            }

            case OpCode::StoreGlobalArr: {
                RValue val = pop();
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                if (auto e = globals_.set_array(ins.operand, i0, i1, ins.aux, std::move(val)); !e) {
                    return e.error();
                }
                break;
            }

            // ---- Dynamic target fields ----
            case OpCode::LoadField: {
                const i32 target = pop().as_i32();
                if (target == -5) { // global
                    const RValue* v = globals_.get_scalar(ins.operand);
                    stack.push_back(v ? *v : RValue(0.0));
                    break;
                }
                std::vector<i32> targets;
                if (host_) {
                    host_->resolve_target_instances(target, cur_self, cur_other, targets);
                }
                if (targets.empty()) {
                    auto u = handle_uninit(ins.operand);
                    if (!u) return u.error();
                    stack.push_back(std::move(u.value()));
                } else if (ins.operand < kUserVarIdBase) {
                    auto r = host_->get_builtin_inst(targets[0], ins.operand, 0, false);
                    if (!r) return r.error();
                    stack.push_back(std::move(r.value()));
                } else {
                    VariableTable* vt = host_->get_instance_vars(targets[0]);
                    const RValue* v = vt ? vt->get_scalar(ins.operand) : nullptr;
                    if (v) {
                        stack.push_back(*v);
                    } else {
                        auto u = handle_uninit(ins.operand);
                        if (!u) return u.error();
                        stack.push_back(std::move(u.value()));
                    }
                }
                break;
            }

            case OpCode::StoreField: {
                RValue val = pop();
                const i32 target = pop().as_i32();
                if (target == -5) {
                    globals_.set_scalar(ins.operand, std::move(val));
                    break;
                }
                std::vector<i32> targets;
                if (host_) {
                    host_->resolve_target_instances(target, cur_self, cur_other, targets);
                }
                for (i32 tid : targets) {
                    if (ins.operand < kUserVarIdBase) {
                        if (auto e = host_->set_builtin_inst(tid, ins.operand, 0, false, val); !e) {
                            return e.error();
                        }
                    } else if (VariableTable* vt = host_->get_instance_vars(tid)) {
                        vt->set_scalar(ins.operand, val);
                    }
                }
                break;
            }

            case OpCode::LoadFieldArr: {
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                const i32 target = pop().as_i32();
                if (target == -5) {
                    auto r = globals_.get_array(ins.operand, i0, i1, ins.aux);
                    if (!r) return r.error();
                    stack.push_back(r.value() ? *r.value() : RValue(0.0));
                    break;
                }
                std::vector<i32> targets;
                if (host_) {
                    host_->resolve_target_instances(target, cur_self, cur_other, targets);
                }
                if (targets.empty()) {
                    stack.emplace_back(0.0);
                } else if (ins.operand < kUserVarIdBase) {
                    auto r = host_->get_builtin_inst(targets[0], ins.operand, i0, true);
                    if (!r) return r.error();
                    stack.push_back(std::move(r.value()));
                } else {
                    VariableTable* vt = host_->get_instance_vars(targets[0]);
                    if (!vt) {
                        stack.emplace_back(0.0);
                    } else {
                        auto r = vt->get_array(ins.operand, i0, i1, ins.aux);
                        if (!r) return r.error();
                        stack.push_back(r.value() ? *r.value() : RValue(0.0));
                    }
                }
                break;
            }

            case OpCode::StoreFieldArr: {
                RValue val = pop();
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                const i32 target = pop().as_i32();
                if (target == -5) {
                    if (auto e = globals_.set_array(ins.operand, i0, i1, ins.aux, std::move(val)); !e) {
                        return e.error();
                    }
                    break;
                }
                std::vector<i32> targets;
                if (host_) {
                    host_->resolve_target_instances(target, cur_self, cur_other, targets);
                }
                for (i32 tid : targets) {
                    if (ins.operand < kUserVarIdBase) {
                        if (auto e = host_->set_builtin_inst(tid, ins.operand, i0, true, val); !e) {
                            return e.error();
                        }
                    } else if (VariableTable* vt = host_->get_instance_vars(tid)) {
                        if (auto e = vt->set_array(ins.operand, i0, i1, ins.aux, val); !e) {
                            return e.error();
                        }
                    }
                }
                break;
            }

            // ---- Built-in variables ----
            case OpCode::LoadBuiltinGlobal: {
                // Fast-path argument0..15 (ids 1..16) & argument_count (id 18)
                if (ins.operand >= 1u && ins.operand <= 16u) {
                    stack.push_back(get_argument(static_cast<i32>(ins.operand - 1u)));
                } else if (ins.operand == 18u) {
                    stack.emplace_back(static_cast<f64>(current_argc_));
                } else if (host_) {
                    auto r = host_->get_builtin_global(ins.operand, 0, false);
                    if (!r) return r.error();
                    stack.push_back(std::move(r.value()));
                } else {
                    stack.emplace_back(0.0);
                }
                break;
            }

            case OpCode::StoreBuiltinGlobal: {
                RValue val = pop();
                if (ins.operand >= 1u && ins.operand <= 16u) {
                    set_argument(static_cast<i32>(ins.operand - 1u), std::move(val));
                } else if (host_) {
                    if (auto e = host_->set_builtin_global(ins.operand, 0, false, std::move(val)); !e) {
                        return e.error();
                    }
                }
                break;
            }

            case OpCode::LoadBuiltinGlobalArr: {
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                if (ins.operand == 17u) { // argument[i]
                    stack.push_back(get_argument(i0));
                } else if (host_) {
                    auto r = host_->get_builtin_global(ins.operand, i0, true);
                    if (!r) return r.error();
                    stack.push_back(std::move(r.value()));
                } else {
                    stack.emplace_back(0.0);
                }
                break;
            }

            case OpCode::StoreBuiltinGlobalArr: {
                RValue val = pop();
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                if (ins.operand == 17u) { // argument[i]
                    set_argument(i0, std::move(val));
                } else if (host_) {
                    if (auto e = host_->set_builtin_global(ins.operand, i0, true, std::move(val)); !e) {
                        return e.error();
                    }
                }
                break;
            }

            case OpCode::LoadBuiltinInst: {
                if (host_) {
                    auto r = host_->get_builtin_inst(cur_self, ins.operand, 0, false);
                    if (!r) return r.error();
                    stack.push_back(std::move(r.value()));
                } else {
                    stack.emplace_back(0.0);
                }
                break;
            }

            case OpCode::StoreBuiltinInst: {
                RValue val = pop();
                if (host_) {
                    if (auto e = host_->set_builtin_inst(cur_self, ins.operand, 0, false, std::move(val)); !e) {
                        return e.error();
                    }
                }
                break;
            }

            case OpCode::LoadBuiltinInstArr: {
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                if (host_) {
                    auto r = host_->get_builtin_inst(cur_self, ins.operand, i0, true);
                    if (!r) return r.error();
                    stack.push_back(std::move(r.value()));
                } else {
                    stack.emplace_back(0.0);
                }
                break;
            }

            case OpCode::StoreBuiltinInstArr: {
                RValue val = pop();
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                if (host_) {
                    if (auto e = host_->set_builtin_inst(cur_self, ins.operand, i0, true, std::move(val)); !e) {
                        return e.error();
                    }
                }
                break;
            }

            // ---- Binary & Unary Operators ----
#define GM_BINOP(opcode, fn)                       \
    case OpCode::opcode: {                         \
        RValue rhs = pop();                        \
        RValue lhs = pop();                        \
        auto res = RValue::fn(lhs, rhs);           \
        if (!res) return res.error();              \
        stack.push_back(std::move(res.value()));   \
        break;                                     \
    }
            GM_BINOP(Add, add)
            GM_BINOP(Sub, sub)
            GM_BINOP(Mul, mul)
            GM_BINOP(Div, div_float)
            GM_BINOP(IntDiv, div_int)
            GM_BINOP(Mod, mod_int)
            GM_BINOP(BitAnd, bit_and)
            GM_BINOP(BitOr, bit_or)
            GM_BINOP(BitXor, bit_xor)
            GM_BINOP(Shl, shl)
            GM_BINOP(Shr, shr)
            GM_BINOP(LogAnd, log_and)
            GM_BINOP(LogOr, log_or)
            GM_BINOP(LogXor, log_xor)
            GM_BINOP(CmpEq, cmp_eq)
            GM_BINOP(CmpNeq, cmp_neq)
            GM_BINOP(CmpLt, cmp_lt)
            GM_BINOP(CmpLe, cmp_le)
            GM_BINOP(CmpGt, cmp_gt)
            GM_BINOP(CmpGe, cmp_ge)
#undef GM_BINOP

#define GM_UNOP(opcode, fn)                        \
    case OpCode::opcode: {                         \
        RValue v = pop();                          \
        auto res = RValue::fn(v);                  \
        if (!res) return res.error();              \
        stack.push_back(std::move(res.value()));   \
        break;                                     \
    }
            GM_UNOP(Neg, neg)
            GM_UNOP(Pos, pos)
            GM_UNOP(BitNot, bit_not)
            GM_UNOP(LogNot, log_not)
#undef GM_UNOP

            // ---- Control flow ----
            case OpCode::Jump:
                ip = ins.operand;
                break;

            case OpCode::JumpIfFalse: {
                RValue cond = pop();
                if (!cond.is_truthy()) {
                    ip = ins.operand;
                }
                break;
            }

            case OpCode::JumpIfTrue: {
                RValue cond = pop();
                if (cond.is_truthy()) {
                    ip = ins.operand;
                }
                break;
            }

            case OpCode::RepeatInit: {
                const i64 cnt = delphi_round(pop().as_real());
                if (cnt <= 0) {
                    ip = ins.operand;
                } else {
                    locals[ins.aux].scalar = RValue(cnt);
                    locals[ins.aux].initialized = true;
                }
                break;
            }

            case OpCode::RepeatNext: {
                const i64 rem = static_cast<i64>(locals[ins.aux].scalar.as_real()) - 1;
                if (rem > 0) {
                    locals[ins.aux].scalar = RValue(rem);
                    ip = ins.operand;
                }
                break;
            }

            case OpCode::WithPush: {
                const i32 target = pop().as_i32();
                WithFrame wf{};
                wf.saved_self = cur_self;
                wf.saved_other = cur_other;
                if (host_) {
                    host_->resolve_target_instances(target, cur_self, cur_other, wf.targets);
                } else if (target == -1) {
                    wf.targets.push_back(cur_self);
                } else if (target == -2) {
                    wf.targets.push_back(cur_other);
                }
                if (wf.targets.empty()) {
                    ip = ins.operand;
                } else {
                    cur_other = cur_self;
                    cur_self = wf.targets[0];
                    with_stack.push_back(std::move(wf));
                }
                break;
            }

            case OpCode::WithNext: {
                auto& wf = with_stack.back();
                ++wf.index;
                if (wf.index < wf.targets.size()) {
                    cur_self = wf.targets[wf.index];
                    ip = ins.operand;
                } else {
                    cur_self = wf.saved_self;
                    cur_other = wf.saved_other;
                    with_stack.pop_back();
                }
                break;
            }

            case OpCode::WithPop: {
                if (!with_stack.empty()) {
                    const auto& wf = with_stack.back();
                    cur_self = wf.saved_self;
                    cur_other = wf.saved_other;
                    with_stack.pop_back();
                }
                break;
            }

            // ---- Calls & returns ----
            case OpCode::CallBuiltin: {
                const usize argc = ins.aux;
                const usize base = stack.size() - argc;
                std::span<const RValue> call_args(stack.data() + base, argc);
                RValue ret(0.0);
                if (host_) {
                    auto r = host_->call_builtin(ins.operand, cur_self, cur_other, call_args);
                    if (!r) return r.error();
                    ret = std::move(r.value());
                }
                stack.resize(base);
                stack.push_back(std::move(ret));
                break;
            }

            case OpCode::CallScript: {
                const usize argc = ins.aux;
                const usize base = stack.size() - argc;
                std::vector<RValue> call_args(stack.begin() + static_cast<std::ptrdiff_t>(base), stack.end());
                stack.resize(base);
                auto r = execute_script(ins.operand, cur_self, cur_other, call_args);
                if (!r) return r.error();
                stack.push_back(std::move(r.value()));
                break;
            }

            case OpCode::CallExtGml: {
                const usize argc = ins.aux;
                const usize base = stack.size() - argc;
                std::vector<RValue> call_args(stack.begin() + static_cast<std::ptrdiff_t>(base), stack.end());
                stack.resize(base);
                auto r = execute_ext_gml(ins.operand, cur_self, cur_other, call_args);
                if (!r) return r.error();
                stack.push_back(std::move(r.value()));
                break;
            }

            case OpCode::CallExtDll: {
                const usize argc = ins.aux;
                const usize base = stack.size() - argc;
                std::span<const RValue> call_args(stack.data() + base, argc);
                RValue ret(0.0);
                if (host_) {
                    auto r = host_->call_extension_dll(ins.operand, call_args);
                    if (!r) return r.error();
                    ret = std::move(r.value());
                }
                stack.resize(base);
                stack.push_back(std::move(ret));
                break;
            }

            case OpCode::Return:
                return pop();

            case OpCode::Exit:
                return RValue(0.0);
        }
    }

    return RValue(0.0);
}

} // namespace gm
