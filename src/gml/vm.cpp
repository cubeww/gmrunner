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

const VarSlot* VariableTable::find_slot(u32 var_id) const noexcept {
    if (map_) {
        const auto it = map_->find(var_id);
        return it == map_->end() ? nullptr : &it->second;
    }
    for (const auto& [id, slot] : flat_) {
        if (id == var_id) return &slot;
    }
    return nullptr;
}

void VariableTable::promote_to_map() {
    map_ = std::make_unique<std::unordered_map<u32, VarSlot>>();
    map_->reserve(flat_.size() * 2 + 8);
    for (auto& [id, slot] : flat_) {
        map_->emplace(id, std::move(slot));
    }
    flat_.clear();
    flat_.shrink_to_fit();
}

VarSlot& VariableTable::find_or_create_slot(u32 var_id) {
    if (map_) {
        return (*map_)[var_id];
    }
    for (auto& [id, slot] : flat_) {
        if (id == var_id) return slot;
    }
    if (flat_.size() >= kFlatLimit) {
        promote_to_map();
        return (*map_)[var_id];
    }
    flat_.emplace_back(var_id, VarSlot{});
    return flat_.back().second;
}

bool VariableTable::exists(u32 var_id) const noexcept {
    return find_slot(var_id) != nullptr;
}

const RValue* VariableTable::get_scalar(u32 var_id) const noexcept {
    const VarSlot* slot = find_slot(var_id);
    return slot ? &slot->scalar : nullptr;
}

void VariableTable::set_scalar(u32 var_id, RValue value) {
    find_or_create_slot(var_id).scalar = std::move(value);
}

Result<const RValue*> VariableTable::get_array(u32 var_id, i32 i0, i32 i1, u8 dims) const noexcept {
    auto key_res = pack_array_index(i0, i1, dims);
    if (!key_res) return key_res.error();
    const u32 key = key_res.value();

    const VarSlot* slot = find_slot(var_id);
    if (!slot) {
        return static_cast<const RValue*>(nullptr);
    }
    if (key == 0u) {
        return &slot->scalar;
    }
    if (!slot->array_data) {
        return static_cast<const RValue*>(nullptr);
    }
    const u32 column = key % kGmArrayMaxDim;
    const auto row = slot->array_data->rows.find(key / kGmArrayMaxDim);
    if (row == slot->array_data->rows.end() || column >= row->second.length) {
        return static_cast<const RValue*>(nullptr);
    }
    const auto value = row->second.values.find(column);
    static const RValue zero(0.0);
    return value != row->second.values.end() ? &value->second : &zero;
}

Result<void> VariableTable::set_array(u32 var_id, i32 i0, i32 i1, u8 dims, RValue value) {
    auto key_res = pack_array_index(i0, i1, dims);
    if (!key_res) return key_res.error();
    const u32 key = key_res.value();

    VarSlot& slot = find_or_create_slot(var_id);
    if (key == 0u) {
        slot.scalar = std::move(value);
    } else {
        if (!slot.array_data) {
            slot.array_data = std::make_unique<VarSlot::ArrayData>();
        }
        auto& row = slot.array_data->rows[key / kGmArrayMaxDim];
        const u32 column = key % kGmArrayMaxDim;
        row.values[column] = std::move(value);
        row.length = std::max(row.length, column + 1);
    }
    return Result<void>::success();
}

void VariableTable::clear() {
    flat_.clear();
    if (map_) map_->clear();
}

std::vector<u32> VariableTable::all_var_ids() const {
    std::vector<u32> ids;
    if (map_) {
        ids.reserve(map_->size());
        for (const auto& [id, slot] : *map_) {
            (void)slot;
            ids.push_back(id);
        }
    } else {
        ids.reserve(flat_.size());
        for (const auto& [id, slot] : flat_) {
            (void)slot;
            ids.push_back(id);
        }
    }
    return ids;
}

GmlVm::GmlVm(CompiledProgram& program, IVmHost* host)
    : program_(program), host_(host) {
    const_slots_.resize(program_.const_init_chunks.size());
    value_stack_.reserve(512);
    temp_stack_.reserve(128);
    with_stack_.reserve(32);
}

bool GmlVm::is_declared_global(u32 var_id) const noexcept {
    const u32 index = var_id - kUserVarIdBase;
    return index < declared_globals_.size() && declared_globals_[index];
}

void GmlVm::reset_globals() {
    globals_.clear();
    declared_globals_.clear();
    standalone_instances_.clear();
}

GmlVm::TargetIterator GmlVm::begin_targets(i32 target, TargetUse use) const {
    if (target == -1 || target == -2 || target == -6) return {target, use, 1};
    return {target, use, host_ ? host_->target_instance_count(target) : 0};
}

i32 GmlVm::next_target(TargetIterator& it, i32 self_id, i32 other_id) const {
    while (it.next_index < it.count) {
        const usize index = it.next_index++;
        if (it.target == -1 || it.target == -6) return self_id;
        if (it.target == -2) return other_id;
        const i32 id = host_->target_instance_at(it.target, index, it.use);
        if (id != -4) return id;
    }
    return -4;
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
    std::string key(code);
    std::shared_ptr<BytecodeChunk> cached;
    if (const auto it = exec_string_cache_.find(key); it != exec_string_cache_.end()) {
        cached = it->second;
    } else {
        auto chunk_res = GmlCompiler::compile_chunk("<execute_string>", code, program_.symbols);
        if (!chunk_res) return chunk_res.error();
        cached = std::make_shared<BytecodeChunk>(std::move(chunk_res.value()));
        if (exec_string_cache_.size() >= 512) {
            exec_string_cache_.clear();
        }
        exec_string_cache_.emplace(std::move(key), cached);
    }
    return execute(*cached, self_id, other_id, args);
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

    const usize stack_base = value_stack_.size();
    const usize temp_base = temp_stack_.size();
    const usize with_base = with_stack_.size();
    if (chunk.temp_count > 0) {
        temp_stack_.resize(temp_base + chunk.temp_count, RValue(0.0));
    }

    struct ScopeRestore {
        GmlVm* vm;
        const std::array<RValue, 16>& saved_args;
        i32 saved_argc;
        usize stack_base;
        usize temp_base;
        usize with_base;
        ~ScopeRestore() {
            vm->value_stack_.resize(stack_base);
            vm->temp_stack_.resize(temp_base);
            vm->with_stack_.resize(with_base);
            vm->current_args_ = saved_args;
            vm->current_argc_ = saved_argc;
            --vm->call_depth_;
        }
    } guard{this, saved_args, saved_argc, stack_base, temp_base, with_base};

    auto& stack = value_stack_;
    auto& with_stack = with_stack_;
    VariableTable locals;

    i32 cur_self = host_ ? host_->instance_handle(self_id) : self_id;
    i32 cur_other = host_ ? host_->instance_handle(other_id) : other_id;
    const bool uninit_zero = host_ ? host_->treat_uninitialized_as_zero() : true;

    auto pop = [&]() -> RValue {
        RValue v = std::move(stack.back());
        stack.pop_back();
        return v;
    };

    auto pop_call_args = [&](usize argc, std::array<RValue, 16>& small_buf, std::vector<RValue>& big_buf) -> std::span<const RValue> {
        const usize base = stack.size() - argc;
        if (argc <= small_buf.size()) {
            for (usize k = 0; k < argc; ++k) {
                small_buf[k] = std::move(stack[base + k]);
            }
            stack.resize(base);
            return std::span<const RValue>(small_buf.data(), argc);
        }
        big_buf.resize(argc);
        for (usize k = 0; k < argc; ++k) {
            big_buf[k] = std::move(stack[base + k]);
        }
        stack.resize(base);
        return std::span<const RValue>(big_buf.data(), argc);
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

    auto push_variable = [&](const RValue* value, u32 var_id) -> Result<void> {
        if (value) {
            stack.push_back(*value);
        } else if (uninit_zero) {
            stack.emplace_back(0.0);
        } else {
            const auto name = program_.symbols.user_var_name(var_id);
            return Error{"Unknown variable " + std::string(name) + " in " + chunk.debug_name};
        }
        return Result<void>::success();
    };

    auto push_array = [&](VariableTable* vars, const Instruction& ins, i32 i0, i32 i1) -> Result<void> {
        if (!vars) return push_variable(nullptr, ins.operand);
        auto value = vars->get_array(ins.operand, i0, i1, ins.aux);
        if (!value) return value.error();
        return push_variable(value.value(), ins.operand);
    };

    auto user_vars = [&](const Instruction& ins) -> VariableTable* {
        switch (ins.op) {
            case OpCode::LoadLocal: case OpCode::StoreLocal:
            case OpCode::LoadLocalArr: case OpCode::StoreLocalArr:
                return &locals;
            case OpCode::LoadGlobalVar: case OpCode::StoreGlobalVar:
            case OpCode::LoadGlobalArr: case OpCode::StoreGlobalArr:
                return &globals_;
            case OpCode::LoadVar: case OpCode::StoreVar:
            case OpCode::LoadArr: case OpCode::StoreArr:
                if (is_declared_global(ins.operand)) return &globals_;
                break;
            default:
                break;
        }
        return host_ ? host_->get_instance_vars(cur_self) : &standalone_instances_[cur_self];
    };

    // GM8 uses -6 for an unqualified target and -7 for the current code's locals.
    auto field_vars = [&](i32& target, u32 var_id) -> VariableTable* {
        if (target == -6) target = is_declared_global(var_id) ? -5 : -1;
        if (target == -7) return &locals;
        if (target == -5) return &globals_;
        if (!host_ && var_id >= kUserVarIdBase) {
            if (target == -1 || target == cur_self) return &standalone_instances_[cur_self];
            if (target == -2 || target == cur_other) return &standalone_instances_[cur_other];
        }
        return nullptr;
    };

    const auto& code = chunk.code;
    RValue statement_result(0.0);
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

            case OpCode::Swap:
                std::iter_swap(stack.end() - 1, stack.end() - 2);
                break;

            case OpCode::DupN: {
                const usize n = ins.aux;
                const usize base = stack.size() - n - ins.operand;
                for (usize k = 0; k < n; ++k) {
                    stack.push_back(stack[base + k]);
                }
                break;
            }

            case OpCode::Pop:
                stack.pop_back();
                break;

            case OpCode::SaveResult:
                statement_result = stack.back();
                break;

            case OpCode::LoadTemp:
                stack.push_back(temp_stack_[temp_base + ins.operand]);
                break;

            case OpCode::StoreTemp:
                temp_stack_[temp_base + ins.operand] = pop();
                break;

            case OpCode::DeclareGlobal: {
                const u32 index = ins.operand - kUserVarIdBase;
                if (index >= declared_globals_.size()) declared_globals_.resize(index + 1);
                declared_globals_[index] = true;
                if (!globals_.exists(ins.operand)) globals_.set_scalar(ins.operand, RValue(0.0));
                break;
            }

            case OpCode::LoadLocal:
            case OpCode::LoadVar:
            case OpCode::LoadSelfVar:
            case OpCode::LoadGlobalVar: {
                auto* vars = user_vars(ins);
                auto r = push_variable(vars ? vars->get_scalar(ins.operand) : nullptr, ins.operand);
                if (!r) return r.error();
                break;
            }

            case OpCode::StoreLocal:
            case OpCode::StoreVar:
            case OpCode::StoreSelfVar:
            case OpCode::StoreGlobalVar: {
                RValue value = pop();
                if (auto* vars = user_vars(ins)) vars->set_scalar(ins.operand, std::move(value));
                break;
            }

            case OpCode::LoadLocalArr:
            case OpCode::LoadArr:
            case OpCode::LoadSelfArr:
            case OpCode::LoadGlobalArr: {
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                if (auto r = push_array(user_vars(ins), ins, i0, i1); !r) return r.error();
                break;
            }

            case OpCode::StoreLocalArr:
            case OpCode::StoreArr:
            case OpCode::StoreSelfArr:
            case OpCode::StoreGlobalArr: {
                RValue value = pop();
                i32 i0 = 0, i1 = 0;
                pop_indices(ins.aux, i0, i1);
                if (auto* vars = user_vars(ins)) {
                    auto r = vars->set_array(ins.operand, i0, i1, ins.aux, std::move(value));
                    if (!r) return r.error();
                }
                break;
            }

            // ---- Dynamic target fields ----
            case OpCode::LoadField:
            case OpCode::LoadFieldArr: {
                i32 i0 = 0, i1 = 0;
                if (ins.aux) pop_indices(ins.aux, i0, i1);
                i32 target = pop().as_i32();
                VariableTable* vars = field_vars(target, ins.operand);
                if (!vars) {
                    auto it = begin_targets(target, TargetUse::ReadField);
                    const i32 id = next_target(it, cur_self, cur_other);
                    if (id != -4 && host_) {
                        if (ins.operand < kUserVarIdBase) {
                            auto value = host_->get_builtin_inst(id, ins.operand, i0, ins.aux != 0);
                            if (!value) return value.error();
                            stack.push_back(std::move(value.value()));
                            break;
                        }
                        vars = host_->get_instance_vars(id);
                    }
                }
                auto r = ins.aux ? push_array(vars, ins, i0, i1)
                                 : push_variable(vars ? vars->get_scalar(ins.operand) : nullptr, ins.operand);
                if (!r) return r.error();
                break;
            }

            case OpCode::StoreField:
            case OpCode::StoreFieldArr: {
                RValue val = pop();
                i32 i0 = 0, i1 = 0;
                if (ins.aux) pop_indices(ins.aux, i0, i1);
                i32 target = pop().as_i32();
                auto store = [&](VariableTable& vars) -> Result<void> {
                    if (ins.aux) return vars.set_array(ins.operand, i0, i1, ins.aux, val);
                    vars.set_scalar(ins.operand, val);
                    return Result<void>::success();
                };
                if (auto* vars = field_vars(target, ins.operand)) {
                    if (auto r = store(*vars); !r) return r.error();
                    break;
                }
                if (target == -4 || target < -7) return Error{"Cannot assign to the variable"};
                if (!host_) break;
                auto it = begin_targets(target, TargetUse::WriteField);
                for (i32 id; (id = next_target(it, cur_self, cur_other)) != -4;) {
                    if (ins.operand < kUserVarIdBase) {
                        auto r = host_->set_builtin_inst(id, ins.operand, i0, ins.aux != 0, val);
                        if (!r) return r.error();
                    } else if (auto* vars = host_->get_instance_vars(id)) {
                        if (auto r = store(*vars); !r) return r.error();
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
        if (!res && ins.aux) res = std::move(lhs); \
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
                    temp_stack_[temp_base + ins.aux] = RValue(cnt);
                }
                break;
            }

            case OpCode::RepeatNext: {
                const i64 rem = static_cast<i64>(temp_stack_[temp_base + ins.aux].as_real()) - 1;
                if (rem > 0) {
                    temp_stack_[temp_base + ins.aux] = RValue(rem);
                    ip = ins.operand;
                }
                break;
            }

            case OpCode::WithPush: {
                const RValue value = pop();
                if (!value.is_real()) return Error{"Object id expected"};
                const i32 target = value.as_i32();
                if (target == -5) return Error{"Cannot use global in with statement."};
                if (target == -7) return Error{"Cannot use local in with statement."};
                WithFrame wf{begin_targets(target, TargetUse::With), cur_self, cur_other};
                const i32 next = next_target(wf.targets, cur_self, cur_other);
                if (next == -4) {
                    ip = ins.operand;
                } else {
                    cur_other = cur_self;
                    cur_self = next;
                    with_stack.push_back(wf);
                }
                break;
            }

            case OpCode::WithNext: {
                auto& wf = with_stack.back();
                const i32 next = next_target(wf.targets, wf.saved_self, wf.saved_other);
                if (next != -4) {
                    cur_self = next;
                    ip = ins.operand;
                } else {
                    cur_self = wf.saved_self;
                    cur_other = wf.saved_other;
                    with_stack.pop_back();
                }
                break;
            }

            case OpCode::WithBreak: {
                const auto& wf = with_stack.back();
                const i32 target = wf.targets.target;
                if (target == -3 || (target >= 0 && target < 100000)) ip = ins.operand;
                cur_self = wf.saved_self;
                cur_other = wf.saved_other;
                with_stack.pop_back();
                break;
            }

            // ---- Calls & returns ----
            case OpCode::CallBuiltin: {
                std::array<RValue, 16> small_args;
                std::vector<RValue> big_args;
                const auto call_args = pop_call_args(ins.aux, small_args, big_args);
                RValue ret(0.0);
                if (host_) {
                    auto r = host_->call_builtin(ins.operand, cur_self, cur_other, call_args);
                    if (!r) return r.error();
                    ret = std::move(r.value());
                }
                stack.push_back(std::move(ret));
                break;
            }

            case OpCode::CallScript: {
                std::array<RValue, 16> small_args;
                std::vector<RValue> big_args;
                const auto call_args = pop_call_args(ins.aux, small_args, big_args);
                auto r = execute_script(ins.operand, cur_self, cur_other, call_args);
                if (!r) return r.error();
                stack.push_back(std::move(r.value()));
                break;
            }

            case OpCode::CallExtGml: {
                std::array<RValue, 16> small_args;
                std::vector<RValue> big_args;
                const auto call_args = pop_call_args(ins.aux, small_args, big_args);
                auto r = execute_ext_gml(ins.operand, cur_self, cur_other, call_args);
                if (!r) return r.error();
                stack.push_back(std::move(r.value()));
                break;
            }

            case OpCode::CallExtDll: {
                std::array<RValue, 16> small_args;
                std::vector<RValue> big_args;
                const auto call_args = pop_call_args(ins.aux, small_args, big_args);
                RValue ret(0.0);
                if (host_) {
                    auto r = host_->call_extension_dll(ins.operand, call_args);
                    if (!r) return r.error();
                    ret = std::move(r.value());
                }
                stack.push_back(std::move(ret));
                break;
            }

            case OpCode::CallCodeAction: {
                const auto action = chunk.code_actions[ins.operand];
                std::array<RValue, 16> small_args;
                std::vector<RValue> big_args;
                const auto call_args = pop_call_args(ins.aux, small_args, big_args);
                auto result = execute(*action, cur_self, cur_other, call_args);
                if (!result) return result.error();
                stack.push_back(std::move(result.value()));
                break;
            }
            case OpCode::JumpIfRoomChange:
                if (host_ && host_->room_change_pending()) ip = ins.operand;
                break;

            case OpCode::Return:
                return pop();

            case OpCode::Exit:
                return statement_result;
        }
    }

    return statement_result;
}

} // namespace gm
