#include "gml/compiler.h"

#include "base/log.h"
#include "gml/dnd_lowerer.h"
#include "gml/lexer.h"

#include <cstdio>
#include <unordered_map>

namespace gm {

namespace {

bool is_assign_op(TokenKind k) noexcept {
    switch (k) {
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::StarAssign:
        case TokenKind::SlashAssign:
        case TokenKind::BitOrAssign:
        case TokenKind::BitAndAssign:
        case TokenKind::BitXorAssign:
            return true;
        default:
            return false;
    }
}

OpCode compound_assign_op(TokenKind k) noexcept {
    switch (k) {
        case TokenKind::PlusAssign:   return OpCode::Add;
        case TokenKind::MinusAssign:  return OpCode::Sub;
        case TokenKind::StarAssign:   return OpCode::Mul;
        case TokenKind::SlashAssign:  return OpCode::Div;
        case TokenKind::BitOrAssign:  return OpCode::BitOr;
        case TokenKind::BitAndAssign: return OpCode::BitAnd;
        case TokenKind::BitXorAssign: return OpCode::BitXor;
        default:                      return OpCode::Nop;
    }
}

bool is_statement_terminator(TokenKind k) noexcept {
    return k == TokenKind::Eof ||
           k == TokenKind::Semicolon ||
           k == TokenKind::RBrace ||
           k == TokenKind::KwEnd ||
           k == TokenKind::KwElse ||
           k == TokenKind::KwUntil ||
           k == TokenKind::KwCase ||
           k == TokenKind::KwDefault;
}

class ParserCompiler {
public:
    ParserCompiler(std::string_view debug_name, std::vector<Token> tokens, SymbolTable& symbols)
        : tokens_(std::move(tokens)), symbols_(symbols) {
        chunk_.debug_name = std::string(debug_name);
    }

    Result<BytecodeChunk> compile_program(bool appendable = false) {
        while (!at(TokenKind::Eof)) {
            if (auto e = parse_statement(); !e) return e.error();
        }
        if (!appendable && (chunk_.code.empty() ||
            (chunk_.code.back().op != OpCode::Return && chunk_.code.back().op != OpCode::Exit))) {
            chunk_.emit(OpCode::Exit, 0, 0, cur_line());
        }
        chunk_.local_count = static_cast<u32>(locals_.size());
        chunk_.temp_count = next_temp_slot_;
        return std::move(chunk_);
    }

    Result<BytecodeChunk> compile_single_expression() {
        if (at(TokenKind::Eof)) {
            chunk_.emit(OpCode::PushZero, 0, 0, cur_line());
        } else {
            if (auto e = parse_expression(); !e) return e.error();
            match(TokenKind::Semicolon);
            if (!at(TokenKind::Eof)) {
                return error_here("Unexpected tokens after expression");
            }
        }
        chunk_.emit(OpCode::Return, 0, 0, cur_line());
        chunk_.local_count = static_cast<u32>(locals_.size());
        chunk_.temp_count = next_temp_slot_;
        return std::move(chunk_);
    }

private:
    struct LoopContext {
        bool is_with = false;
        bool is_switch = false;
        std::vector<u32> break_jumps;
        std::vector<u32> continue_jumps;
    };

    enum class LValueKind : u8 {
        None = 0,
        Local,
        UnqualifiedUser,
        SelfUser,
        GlobalUser,
        BuiltinGlobal,
        BuiltinInst,
        Field, // target expression is already on stack
    };

    struct PendingLValue {
        LValueKind kind = LValueKind::None;
        u32 id = 0;      // user var_id or builtin_var_id
        u8 arr_dims = 0; // 0 = scalar, 1 = [i], 2 = [i, j]
        bool is_array_builtin = false;
        u16 line = 0;
    };

    const Token& peek(usize offset = 0) const noexcept {
        const usize idx = pos_ + offset;
        return (idx < tokens_.size()) ? tokens_[idx] : tokens_.back();
    }

    bool at(TokenKind k) const noexcept { return peek().kind == k; }
    bool at_call() const noexcept {
        return (at(TokenKind::Identifier) || at(TokenKind::DndCodeAction)) && peek(1).kind == TokenKind::LParen;
    }
    u16 cur_line() const noexcept { return static_cast<u16>(peek().line); }

    const Token& advance() noexcept {
        const Token& t = peek();
        if (pos_ + 1 < tokens_.size()) ++pos_;
        return t;
    }

    bool match(TokenKind k) noexcept {
        if (at(k)) {
            advance();
            return true;
        }
        return false;
    }

    Result<void> expect(TokenKind k, std::string_view what) {
        if (at(k)) {
            advance();
            return Result<void>::success();
        }
        return error_here("Expected " + std::string(what));
    }

    Error error_here(std::string msg) const {
        const Token& t = peek();
        return Error{chunk_.debug_name + ":" + std::to_string(t.line) + ":" +
                     std::to_string(t.col) + ": " + std::move(msg)};
    }

    u32 alloc_temp_local() noexcept {
        return next_temp_slot_++;
    }

    u32 declare_local(std::string_view name) {
        const std::string key(name);
        if (const auto it = locals_.find(key); it != locals_.end()) {
            return it->second;
        }
        const u32 var_id = symbols_.intern_user_var(name);
        locals_.emplace(key, var_id);
        return var_id;
    }

    std::optional<u32> find_local(std::string_view name) const {
        const auto it = locals_.find(std::string(name));
        if (it == locals_.end()) return std::nullopt;
        return it->second;
    }

    void emit_push_real(f64 v, u16 line) {
        if (v == 0.0) {
            chunk_.emit(OpCode::PushZero, 0, 0, line);
        } else {
            const u32 idx = chunk_.add_real(v);
            chunk_.emit(OpCode::PushReal, idx, 0, line);
        }
    }

    void emit_lvalue_load(const PendingLValue& lv) {
        switch (lv.kind) {
            case LValueKind::Local:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::LoadLocal, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::LoadLocalArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::UnqualifiedUser:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::LoadVar, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::LoadArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::SelfUser:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::LoadSelfVar, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::LoadSelfArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::GlobalUser:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::LoadGlobalVar, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::LoadGlobalArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::BuiltinGlobal:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::LoadBuiltinGlobal, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::LoadBuiltinGlobalArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::BuiltinInst:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::LoadBuiltinInst, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::LoadBuiltinInstArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::Field:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::LoadField, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::LoadFieldArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::None:
                break;
        }
    }

    void emit_lvalue_store(const PendingLValue& lv) {
        chunk_.emit(OpCode::SaveResult, 0, 0, lv.line);
        switch (lv.kind) {
            case LValueKind::Local:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::StoreLocal, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::StoreLocalArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::UnqualifiedUser:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::StoreVar, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::StoreArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::SelfUser:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::StoreSelfVar, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::StoreSelfArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::GlobalUser:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::StoreGlobalVar, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::StoreGlobalArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::BuiltinGlobal:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::StoreBuiltinGlobal, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::StoreBuiltinGlobalArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::BuiltinInst:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::StoreBuiltinInst, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::StoreBuiltinInstArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::Field:
                if (lv.arr_dims == 0) chunk_.emit(OpCode::StoreField, lv.id, 0, lv.line);
                else chunk_.emit(OpCode::StoreFieldArr, lv.id, lv.arr_dims, lv.line);
                break;
            case LValueKind::None:
                break;
        }
    }

    Result<u8> parse_optional_array_indices() {
        if (!match(TokenKind::LBracket)) {
            return static_cast<u8>(0);
        }
        if (auto e = parse_expression(); !e) return e.error();
        u8 dims = 1;
        if (match(TokenKind::Comma)) {
            if (auto e = parse_expression(); !e) return e.error();
            dims = 2;
        }
        if (auto e = expect(TokenKind::RBracket, "']'"); !e) return e.error();
        return dims;
    }

    // Statements
    Result<void> parse_statement() {
        while (match(TokenKind::Semicolon)) {}
        while (match(TokenKind::DndActionStart)) {
            action_end_jumps_.push_back(chunk_.emit(OpCode::JumpIfRoomChange, 0, 0, cur_line()));
        }
        if (match(TokenKind::DndEventEnd)) {
            for (u32 jump : action_end_jumps_) chunk_.patch_operand(jump, static_cast<u32>(chunk_.code.size()));
            return Result<void>::success();
        }
        if (at(TokenKind::Eof) || at(TokenKind::RBrace) || at(TokenKind::KwEnd)) {
            return Result<void>::success();
        }

        switch (peek().kind) {
            case TokenKind::LBrace:
            case TokenKind::KwBegin:
                return parse_block();
            case TokenKind::KwVar:
                return parse_var_decl();
            case TokenKind::KwGlobalVar:
                return parse_globalvar_decl();
            case TokenKind::KwIf:
                return parse_if();
            case TokenKind::KwWhile:
                return parse_while();
            case TokenKind::KwDo:
                return parse_do_until();
            case TokenKind::KwRepeat:
                return parse_repeat();
            case TokenKind::KwFor:
                return parse_for();
            case TokenKind::KwWith:
                return parse_with();
            case TokenKind::KwSwitch:
                return parse_switch();
            case TokenKind::KwBreak:
                return parse_break();
            case TokenKind::KwContinue:
                return parse_continue();
            case TokenKind::KwExit:
                advance();
                chunk_.emit(OpCode::Exit, 0, 0, cur_line());
                match(TokenKind::Semicolon);
                return Result<void>::success();
            case TokenKind::KwReturn:
                return parse_return();
            default:
                return parse_assign_or_call_stmt();
        }
    }

    Result<void> parse_block() {
        const bool is_begin = at(TokenKind::KwBegin);
        advance(); // consume '{' or 'begin'
        while (!at(TokenKind::Eof) && !at(TokenKind::RBrace) && !at(TokenKind::KwEnd)) {
            if (auto e = parse_statement(); !e) return e.error();
        }
        if (is_begin) {
            if (!match(TokenKind::KwEnd) && !match(TokenKind::RBrace)) {
                return error_here("Expected 'end' to close 'begin' block");
            }
        } else {
            if (!match(TokenKind::RBrace) && !match(TokenKind::KwEnd)) {
                return error_here("Expected '}' to close block");
            }
        }
        return Result<void>::success();
    }

    bool has_next_declared_name() {
        if (match(TokenKind::Comma)) return true;
        // GM accepts adjacent names. With no semicolon, an identifier can also
        // start the next statement, e.g. "var tmp obj.x = 1".
        if (!at(TokenKind::Identifier)) return false;
        const TokenKind next = peek(1).kind;
        return !is_assign_op(next) && next != TokenKind::LParen &&
               next != TokenKind::LBracket && next != TokenKind::Dot;
    }

    Result<void> parse_var_decl() {
        advance(); // 'var'
        if (!at(TokenKind::Identifier)) {
            return error_here("Expected identifier after 'var'");
        }
        for (;;) {
            if (!at(TokenKind::Identifier)) {
                break;
            }
            const Token tok = advance();
            const u32 var_id = declare_local(tok.text);

            // Tolerate optional initializer `var x = expr` if encountered.
            if (match(TokenKind::Assign)) {
                if (auto e = parse_expression(); !e) return e.error();
                chunk_.emit(OpCode::SaveResult, 0, 0, static_cast<u16>(tok.line));
                chunk_.emit(OpCode::StoreLocal, var_id, 0, static_cast<u16>(tok.line));
            }
            if (!has_next_declared_name()) {
                break;
            }
        }
        match(TokenKind::Semicolon);
        return Result<void>::success();
    }

    Result<void> parse_globalvar_decl() {
        advance(); // 'globalvar'
        if (!at(TokenKind::Identifier)) {
            return error_here("Expected identifier after 'globalvar'");
        }
        for (;;) {
            if (!at(TokenKind::Identifier)) {
                break;
            }
            const Token tok = advance();
            const u32 vid = symbols_.intern_user_var(tok.text);
            chunk_.emit(OpCode::DeclareGlobal, vid, 0, static_cast<u16>(tok.line));
            if (!has_next_declared_name()) {
                break;
            }
        }
        match(TokenKind::Semicolon);
        return Result<void>::success();
    }

    Result<void> parse_if() {
        const u16 line = cur_line();
        advance(); // 'if'
        if (auto e = parse_expression(); !e) return e.error();
        match(TokenKind::KwThen);

        const u32 jfalse = chunk_.emit(OpCode::JumpIfFalse, 0, 0, line);
        if (auto e = parse_statement(); !e) return e.error();

        // GM8 tolerates optional ';' before 'else' (e.g. `if (x) { ... }; else { ... }`).
        while (at(TokenKind::Semicolon) && peek(1).kind == TokenKind::KwElse) {
            advance();
        }

        if (match(TokenKind::KwElse)) {
            const u32 jend = chunk_.emit(OpCode::Jump, 0, 0, cur_line());
            chunk_.patch_operand(jfalse, static_cast<u32>(chunk_.code.size()));
            if (auto e = parse_statement(); !e) return e.error();
            chunk_.patch_operand(jend, static_cast<u32>(chunk_.code.size()));
        } else {
            chunk_.patch_operand(jfalse, static_cast<u32>(chunk_.code.size()));
        }
        return Result<void>::success();
    }

    Result<void> parse_while() {
        const u16 line = cur_line();
        advance(); // 'while'

        const u32 loop_start = static_cast<u32>(chunk_.code.size());
        if (auto e = parse_expression(); !e) return e.error();
        match(TokenKind::KwDo);

        const u32 jend = chunk_.emit(OpCode::JumpIfFalse, 0, 0, line);

        loops_.push_back(LoopContext{false, false, {}, {}});
        if (auto e = parse_statement(); !e) return e.error();
        LoopContext ctx = std::move(loops_.back());
        loops_.pop_back();

        chunk_.emit(OpCode::Jump, loop_start, 0, line);
        const u32 end_ip = static_cast<u32>(chunk_.code.size());
        chunk_.patch_operand(jend, end_ip);
        for (u32 j : ctx.break_jumps) chunk_.patch_operand(j, end_ip);
        for (u32 j : ctx.continue_jumps) chunk_.patch_operand(j, loop_start);
        return Result<void>::success();
    }

    Result<void> parse_do_until() {
        const u16 line = cur_line();
        advance(); // 'do'

        const u32 loop_start = static_cast<u32>(chunk_.code.size());
        loops_.push_back(LoopContext{false, false, {}, {}});
        if (auto e = parse_statement(); !e) return e.error();
        LoopContext ctx = std::move(loops_.back());
        loops_.pop_back();

        while (match(TokenKind::Semicolon)) {}
        if (auto e = expect(TokenKind::KwUntil, "'until'"); !e) return e.error();

        const u32 cond_ip = static_cast<u32>(chunk_.code.size());
        if (auto e = parse_expression(); !e) return e.error();
        chunk_.emit(OpCode::JumpIfFalse, loop_start, 0, line);

        const u32 end_ip = static_cast<u32>(chunk_.code.size());
        for (u32 j : ctx.break_jumps) chunk_.patch_operand(j, end_ip);
        for (u32 j : ctx.continue_jumps) chunk_.patch_operand(j, cond_ip);
        match(TokenKind::Semicolon);
        return Result<void>::success();
    }

    Result<void> parse_repeat() {
        const u16 line = cur_line();
        advance(); // 'repeat'

        if (auto e = parse_expression(); !e) return e.error();
        match(TokenKind::KwDo);

        const u32 slot = alloc_temp_local();
        if (slot > 255u) {
            return error_here("Exceeded maximum local/temp slots (255) for repeat");
        }
        const u32 init_ip = chunk_.emit(OpCode::RepeatInit, 0, static_cast<u8>(slot), line);
        const u32 body_start = static_cast<u32>(chunk_.code.size());

        loops_.push_back(LoopContext{false, false, {}, {}});
        if (auto e = parse_statement(); !e) return e.error();
        LoopContext ctx = std::move(loops_.back());
        loops_.pop_back();

        const u32 next_ip = static_cast<u32>(chunk_.code.size());
        chunk_.emit(OpCode::RepeatNext, body_start, static_cast<u8>(slot), line);
        const u32 end_ip = static_cast<u32>(chunk_.code.size());

        chunk_.patch_operand(init_ip, end_ip);
        for (u32 j : ctx.break_jumps) chunk_.patch_operand(j, end_ip);
        for (u32 j : ctx.continue_jumps) chunk_.patch_operand(j, next_ip);
        return Result<void>::success();
    }

    Result<void> parse_for() {
        const u16 line = cur_line();
        advance(); // 'for'
        if (auto e = expect(TokenKind::LParen, "'(' after 'for'"); !e) return e.error();

        // Init statement (optional)
        if (!at(TokenKind::Semicolon)) {
            if (auto e = parse_statement(); !e) return e.error();
        }
        match(TokenKind::Semicolon);

        // Condition expression (optional; defaults to true)
        const u32 cond_ip = static_cast<u32>(chunk_.code.size());
        u32 jend = 0;
        bool has_cond = false;
        if (!at(TokenKind::Semicolon)) {
            if (auto e = parse_expression(); !e) return e.error();
            jend = chunk_.emit(OpCode::JumpIfFalse, 0, 0, line);
            has_cond = true;
        }
        match(TokenKind::Semicolon);

        // Step statement (optional) — jump over step to body, then body jumps back to step.
        const u32 jbody = chunk_.emit(OpCode::Jump, 0, 0, line);
        const u32 step_ip = static_cast<u32>(chunk_.code.size());
        if (!at(TokenKind::RParen)) {
            if (auto e = parse_statement(); !e) return e.error();
            while (match(TokenKind::Semicolon)) {}
        }
        chunk_.emit(OpCode::Jump, cond_ip, 0, line);

        if (auto e = expect(TokenKind::RParen, "')' after 'for' clauses"); !e) return e.error();

        chunk_.patch_operand(jbody, static_cast<u32>(chunk_.code.size()));

        loops_.push_back(LoopContext{false, false, {}, {}});
        if (auto e = parse_statement(); !e) return e.error();
        LoopContext ctx = std::move(loops_.back());
        loops_.pop_back();

        chunk_.emit(OpCode::Jump, step_ip, 0, line);
        const u32 end_ip = static_cast<u32>(chunk_.code.size());
        if (has_cond) chunk_.patch_operand(jend, end_ip);
        for (u32 j : ctx.break_jumps) chunk_.patch_operand(j, end_ip);
        for (u32 j : ctx.continue_jumps) chunk_.patch_operand(j, step_ip);
        return Result<void>::success();
    }

    Result<void> parse_with() {
        const u16 line = cur_line();
        advance(); // 'with'

        if (auto e = parse_expression(); !e) return e.error();
        match(TokenKind::KwDo);

        const u32 push_ip = chunk_.emit(OpCode::WithPush, 0, 0, line);
        const u32 body_start = static_cast<u32>(chunk_.code.size());

        loops_.push_back(LoopContext{true, false, {}, {}});
        if (auto e = parse_statement(); !e) return e.error();
        LoopContext ctx = std::move(loops_.back());
        loops_.pop_back();

        const u32 next_ip = static_cast<u32>(chunk_.code.size());
        chunk_.emit(OpCode::WithNext, body_start, 0, line);
        const u32 end_ip = static_cast<u32>(chunk_.code.size());

        chunk_.patch_operand(push_ip, end_ip);
        for (u32 j : ctx.break_jumps) chunk_.patch_operand(j, end_ip);
        for (u32 j : ctx.continue_jumps) chunk_.patch_operand(j, next_ip);
        return Result<void>::success();
    }

    Result<void> parse_switch() {
        const u16 line = cur_line();
        advance(); // 'switch'

        if (auto e = parse_expression(); !e) return e.error();
        const u32 sw_slot = alloc_temp_local();
        chunk_.emit(OpCode::StoreTemp, sw_slot, 0, line);

        const bool is_begin = at(TokenKind::KwBegin);
        if (!match(TokenKind::LBrace) && !match(TokenKind::KwBegin)) {
            return error_here("Expected '{' or 'begin' after 'switch' expression");
        }

        // Jump to dispatch table emitted at the bottom of the switch so case bodies stay
        // contiguous for fallthrough while case expressions evaluate in source order!
        const u32 jump_to_dispatch = chunk_.emit(OpCode::Jump, 0, 0, line);

        struct CaseEntry {
            bool is_default = false;
            u32 body_ip = 0;
            std::vector<Token> expr_tokens;
            u16 line = 0;
        };
        std::vector<CaseEntry> cases;

        loops_.push_back(LoopContext{false, true, {}, {}});

        while (!at(TokenKind::Eof) && !at(TokenKind::RBrace) && !at(TokenKind::KwEnd)) {
            if (match(TokenKind::KwCase)) {
                CaseEntry entry{};
                entry.is_default = false;
                entry.line = cur_line();
                // Capture case expression tokens until ':' at paren/bracket depth 0.
                int depth = 0;
                while (!at(TokenKind::Eof)) {
                    if (depth == 0 && at(TokenKind::Colon)) break;
                    if (at(TokenKind::LParen) || at(TokenKind::LBracket)) ++depth;
                    else if ((at(TokenKind::RParen) || at(TokenKind::RBracket)) && depth > 0) --depth;
                    entry.expr_tokens.push_back(advance());
                }
                if (auto e = expect(TokenKind::Colon, "':' after case expression"); !e) return e.error();
                entry.body_ip = static_cast<u32>(chunk_.code.size());
                cases.push_back(std::move(entry));
            } else if (match(TokenKind::KwDefault)) {
                CaseEntry entry{};
                entry.is_default = true;
                entry.line = cur_line();
                if (auto e = expect(TokenKind::Colon, "':' after 'default'"); !e) return e.error();
                entry.body_ip = static_cast<u32>(chunk_.code.size());
                cases.push_back(std::move(entry));
            } else {
                if (auto e = parse_statement(); !e) return e.error();
            }
        }

        if (is_begin) {
            if (!match(TokenKind::KwEnd) && !match(TokenKind::RBrace)) {
                return error_here("Expected 'end' to close switch");
            }
        } else {
            if (!match(TokenKind::RBrace) && !match(TokenKind::KwEnd)) {
                return error_here("Expected '}' to close switch");
            }
        }

        LoopContext ctx = std::move(loops_.back());
        loops_.pop_back();

        // Normal fall-off from the last case body skips past the dispatch table to end_ip.
        const u32 jump_over_dispatch = chunk_.emit(OpCode::Jump, 0, 0, cur_line());

        // Emit dispatch table at dispatch_ip: evaluate each case expression in source order.
        const u32 dispatch_ip = static_cast<u32>(chunk_.code.size());
        chunk_.patch_operand(jump_to_dispatch, dispatch_ip);

        bool has_default = false;
        u32 default_ip = 0;

        for (auto& c : cases) {
            if (c.is_default) {
                has_default = true;
                default_ip = c.body_ip;
                continue;
            }
            chunk_.emit(OpCode::LoadTemp, sw_slot, 0, c.line);
            // Parse the captured case expression tokens using a temporary sub-view.
            c.expr_tokens.push_back(Token{TokenKind::Eof, c.line, 1, 0.0, {}});
            auto saved_tokens = std::move(tokens_);
            const usize saved_pos = pos_;
            tokens_ = std::move(c.expr_tokens);
            pos_ = 0;
            auto expr_res = parse_expression();
            tokens_ = std::move(saved_tokens);
            pos_ = saved_pos;
            if (!expr_res) return expr_res.error();

            chunk_.emit(OpCode::CmpEq, 0, 0, c.line);
            chunk_.emit(OpCode::JumpIfTrue, c.body_ip, 0, c.line);
        }

        if (has_default) {
            chunk_.emit(OpCode::Jump, default_ip, 0, line);
        }

        const u32 end_ip = static_cast<u32>(chunk_.code.size());
        chunk_.patch_operand(jump_over_dispatch, end_ip);
        for (u32 j : ctx.break_jumps) chunk_.patch_operand(j, end_ip);
        return Result<void>::success();
    }

    Result<void> parse_break() {
        const u16 line = cur_line();
        advance(); // 'break'
        // Object/all iterations consume break. A single-instance with restores its scope
        // and passes break through to the next enclosing loop or switch.
        for (auto it = loops_.rbegin(); it != loops_.rend(); ++it) {
            if (it->is_with) {
                it->break_jumps.push_back(chunk_.emit(OpCode::WithBreak, 0, 0, line));
            } else {
                it->break_jumps.push_back(chunk_.emit(OpCode::Jump, 0, 0, line));
                match(TokenKind::Semicolon);
                return Result<void>::success();
            }
        }
        chunk_.emit(OpCode::Exit, 0, 0, line);
        match(TokenKind::Semicolon);
        return Result<void>::success();
    }

    Result<void> parse_continue() {
        const u16 line = cur_line();
        advance(); // 'continue'
        // Find innermost non-switch loop (continue inside a switch targets the enclosing loop!).
        LoopContext* target_loop = nullptr;
        for (auto it = loops_.rbegin(); it != loops_.rend(); ++it) {
            if (!it->is_switch) {
                target_loop = &(*it);
                break;
            }
        }
        if (!target_loop) {
            chunk_.emit(OpCode::Exit, 0, 0, line);
        } else {
            const u32 j = chunk_.emit(OpCode::Jump, 0, 0, line);
            target_loop->continue_jumps.push_back(j);
        }
        match(TokenKind::Semicolon);
        return Result<void>::success();
    }

    Result<void> parse_return() {
        const u16 line = cur_line();
        advance(); // 'return'
        if (is_statement_terminator(peek().kind)) {
            chunk_.emit(OpCode::PushZero, 0, 0, line);
        } else {
            if (auto e = parse_expression(); !e) return e.error();
        }
        chunk_.emit(OpCode::Return, 0, 0, line);
        match(TokenKind::Semicolon);
        return Result<void>::success();
    }

    // Resolves a standalone identifier as a PendingLValue (when not a function call).
    PendingLValue resolve_ident_lvalue(std::string_view name, u16 line) {
        PendingLValue lv{};
        lv.line = line;

        if (auto slot = find_local(name); slot.has_value()) {
            lv.kind = LValueKind::Local;
            lv.id = slot.value();
            return lv;
        }
        if (const auto* bv = symbols_.find_builtin_var(name)) {
            lv.kind = bv->is_global ? LValueKind::BuiltinGlobal : LValueKind::BuiltinInst;
            lv.id = bv->id;
            lv.is_array_builtin = bv->is_array;
            return lv;
        }
        lv.kind = LValueKind::UnqualifiedUser;
        lv.id = symbols_.intern_user_var(name);
        return lv;
    }

    // Parses an assignment statement (`lhs op= rhs`) or standalone function call statement (`fn(...)`).
    Result<void> parse_assign_or_call_stmt() {
        const u16 line = cur_line();
        PendingLValue lv{};
        bool produced_value = false;

        // Handle leading prefix:
        // Case A: global.field | self.field | other.field
        if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Dot &&
            (peek().text == "global" || peek().text == "self")) {
            const bool is_global = (peek().text == "global");
            advance(); // 'global' or 'self'
            advance(); // '.'
            if (!at(TokenKind::Identifier)) return error_here("Expected field name after '.'");
            const Token field = advance();
            auto dims_res = parse_optional_array_indices();
            if (!dims_res) return dims_res.error();

            if (is_global) {
                lv.kind = LValueKind::GlobalUser;
                lv.id = symbols_.intern_user_var(field.text);
                lv.arr_dims = dims_res.value();
                lv.line = static_cast<u16>(field.line);
            } else {
                if (const auto* bv = symbols_.find_builtin_var(field.text); bv && !bv->is_global) {
                    lv.kind = LValueKind::BuiltinInst;
                    lv.id = bv->id;
                    lv.is_array_builtin = bv->is_array;
                } else {
                    lv.kind = LValueKind::SelfUser;
                    lv.id = symbols_.intern_user_var(field.text);
                }
                lv.arr_dims = dims_res.value();
                lv.line = static_cast<u16>(field.line);
            }
        }
        // Case B: Function call (possibly followed by `.field = ...`, e.g. `instance_create(...).speed = 5`)
        else if (at_call()) {
            if (auto e = parse_call_expr(); !e) return e.error();
            produced_value = true;
        }
        // Case C: Parenthesized expression target `(expr).field = ...`
        else if (at(TokenKind::LParen)) {
            advance();
            if (auto e = parse_expression(); !e) return e.error();
            if (auto e = expect(TokenKind::RParen, "')'"); !e) return e.error();
            produced_value = true;
        }
        // Case D: Numeric instance ID target `100001.x = 5`
        else if (at(TokenKind::Number)) {
            const Token num = advance();
            emit_push_real(num.number_val, static_cast<u16>(num.line));
            produced_value = true;
        }
        // Case E: Identifier (could be constant/asset/other/all followed by `.field`, or bare variable)
        else if (at(TokenKind::Identifier)) {
            const Token ident = advance();
            // If followed by `.`, check if it's a constant/object/scope (`other`, `all`, `obj_player`)
            if (at(TokenKind::Dot)) {
                if (!find_local(ident.text).has_value()) {
                    if (const auto* cb = symbols_.find_constant(ident.text)) {
                        if (cb->is_expression) {
                            chunk_.emit(OpCode::LoadConstSlot, cb->expr_slot, 0, static_cast<u16>(ident.line));
                        } else {
                            emit_push_real(cb->numeric_value, static_cast<u16>(ident.line));
                        }
                        produced_value = true;
                    }
                }
            }
            if (!produced_value) {
                lv = resolve_ident_lvalue(ident.text, static_cast<u16>(ident.line));
                auto dims_res = parse_optional_array_indices();
                if (!dims_res) return dims_res.error();
                lv.arr_dims = dims_res.value();
            }
        } else {
            return error_here("Unexpected token at start of statement");
        }

        // Consume any trailing `.field[...]` chain; earlier segments materialize onto stack as target.
        while (match(TokenKind::Dot)) {
            if (!produced_value) {
                emit_lvalue_load(lv);
                produced_value = true;
            }
            if (!at(TokenKind::Identifier)) {
                return error_here("Expected member name after '.'");
            }
            const Token field = advance();
            auto dims_res = parse_optional_array_indices();
            if (!dims_res) return dims_res.error();

            lv = PendingLValue{};
            lv.kind = LValueKind::Field;
            lv.line = static_cast<u16>(field.line);
            lv.arr_dims = dims_res.value();
            if (const auto* bv = symbols_.find_builtin_var(field.text); bv && !bv->is_global) {
                lv.id = bv->id;
                lv.is_array_builtin = bv->is_array;
            } else {
                lv.id = symbols_.intern_user_var(field.text);
            }
            produced_value = false;
        }

        // If no assignment operator follows, it must be a standalone call statement.
        if (!is_assign_op(peek().kind)) {
            if (produced_value) {
                chunk_.emit(OpCode::SaveResult, 0, 0, line);
                chunk_.emit(OpCode::Pop, 0, 0, line);
                match(TokenKind::Semicolon);
                return Result<void>::success();
            }
            return error_here("Expected assignment operator or function call");
        }

        if (lv.kind == LValueKind::None) {
            return error_here("Invalid assignment target");
        }

        // Implicit [0] for array built-ins if no brackets were provided (e.g. `view_xview = 0` -> `view_xview[0] = 0`).
        if (lv.is_array_builtin && lv.arr_dims == 0) {
            chunk_.emit(OpCode::PushZero, 0, 0, lv.line);
            lv.arr_dims = 1;
        }

        const Token op_tok = advance();
        if (auto e = parse_expression(); !e) return e.error();
        if (op_tok.kind == TokenKind::Assign) {
            emit_lvalue_store(lv);
        } else {
            // GM8 resolves the target and indices, evaluates the RHS, then reads the old value.
            // A call on the RHS can change the first instance in the object's list.
            const u8 stack_inputs = static_cast<u8>((lv.kind == LValueKind::Field ? 1u : 0u) + lv.arr_dims);
            if (stack_inputs) chunk_.emit(OpCode::DupN, 1, stack_inputs, line);
            emit_lvalue_load(lv);
            chunk_.emit(OpCode::Swap, 0, 0, line);
            chunk_.emit(compound_assign_op(op_tok.kind), 0, 1, static_cast<u16>(op_tok.line));
            emit_lvalue_store(lv);
        }

        match(TokenKind::Semicolon);
        return Result<void>::success();
    }

    // Expressions (exact GM8 precedence, low -> high)
    Result<void> parse_expression() {
        return parse_logical();
    }

    // Level 1: && / and, || / or, ^^ / xor (all same precedence, non-short-circuiting)
    Result<void> parse_logical() {
        if (auto e = parse_comparison(); !e) return e.error();
        while (at(TokenKind::LogAnd) || at(TokenKind::LogOr) || at(TokenKind::LogXor)) {
            const Token op = advance();
            if (auto e = parse_comparison(); !e) return e.error();
            const OpCode oc = (op.kind == TokenKind::LogAnd) ? OpCode::LogAnd
                            : (op.kind == TokenKind::LogOr)  ? OpCode::LogOr
                                                             : OpCode::LogXor;
            chunk_.emit(oc, 0, 0, static_cast<u16>(op.line));
        }
        return Result<void>::success();
    }

    // Level 2: <, <=, ==, =, !=, <>, >, >=
    Result<void> parse_comparison() {
        if (auto e = parse_bitwise(); !e) return e.error();
        while (at(TokenKind::Less) || at(TokenKind::LessEqual) ||
               at(TokenKind::Equal) || at(TokenKind::Assign) ||
               at(TokenKind::NotEqual) || at(TokenKind::Greater) ||
               at(TokenKind::GreaterEqual)) {
            const Token op = advance();
            if (auto e = parse_bitwise(); !e) return e.error();
            OpCode oc = OpCode::CmpEq;
            switch (op.kind) {
                case TokenKind::Less:         oc = OpCode::CmpLt; break;
                case TokenKind::LessEqual:    oc = OpCode::CmpLe; break;
                case TokenKind::Equal:
                case TokenKind::Assign:       oc = OpCode::CmpEq; break;
                case TokenKind::NotEqual:     oc = OpCode::CmpNeq; break;
                case TokenKind::Greater:      oc = OpCode::CmpGt; break;
                case TokenKind::GreaterEqual: oc = OpCode::CmpGe; break;
                default: break;
            }
            chunk_.emit(oc, 0, 0, static_cast<u16>(op.line));
        }
        return Result<void>::success();
    }

    // Level 3: |, &, ^ (all same precedence in GM8)
    Result<void> parse_bitwise() {
        if (auto e = parse_shift(); !e) return e.error();
        while (at(TokenKind::BitOr) || at(TokenKind::BitAnd) || at(TokenKind::BitXor)) {
            const Token op = advance();
            if (auto e = parse_shift(); !e) return e.error();
            const OpCode oc = (op.kind == TokenKind::BitOr)  ? OpCode::BitOr
                            : (op.kind == TokenKind::BitAnd) ? OpCode::BitAnd
                                                             : OpCode::BitXor;
            chunk_.emit(oc, 0, 0, static_cast<u16>(op.line));
        }
        return Result<void>::success();
    }

    // Level 4: <<, >>
    Result<void> parse_shift() {
        if (auto e = parse_additive(); !e) return e.error();
        while (at(TokenKind::Shl) || at(TokenKind::Shr)) {
            const Token op = advance();
            if (auto e = parse_additive(); !e) return e.error();
            chunk_.emit(op.kind == TokenKind::Shl ? OpCode::Shl : OpCode::Shr, 0, 0, static_cast<u16>(op.line));
        }
        return Result<void>::success();
    }

    // Level 5: +, -
    Result<void> parse_additive() {
        if (auto e = parse_multiplicative(); !e) return e.error();
        while (at(TokenKind::Plus) || at(TokenKind::Minus)) {
            const Token op = advance();
            if (auto e = parse_multiplicative(); !e) return e.error();
            chunk_.emit(op.kind == TokenKind::Plus ? OpCode::Add : OpCode::Sub, 0, 0, static_cast<u16>(op.line));
        }
        return Result<void>::success();
    }

    // Level 6: *, /, div, mod
    Result<void> parse_multiplicative() {
        if (auto e = parse_unary(); !e) return e.error();
        while (at(TokenKind::Star) || at(TokenKind::Slash) || at(TokenKind::KwDiv) || at(TokenKind::KwMod)) {
            const Token op = advance();
            if (auto e = parse_unary(); !e) return e.error();
            OpCode oc = OpCode::Mul;
            switch (op.kind) {
                case TokenKind::Star:  oc = OpCode::Mul; break;
                case TokenKind::Slash: oc = OpCode::Div; break;
                case TokenKind::KwDiv: oc = OpCode::IntDiv; break;
                case TokenKind::KwMod: oc = OpCode::Mod; break;
                default: break;
            }
            chunk_.emit(oc, 0, 0, static_cast<u16>(op.line));
        }
        return Result<void>::success();
    }

    // Level 7: unary !, not, +, -, ~
    Result<void> parse_unary() {
        if (at(TokenKind::LogNot) || at(TokenKind::Plus) || at(TokenKind::Minus) || at(TokenKind::BitNot)) {
            const Token op = advance();
            if (auto e = parse_unary(); !e) return e.error();
            OpCode oc = OpCode::Pos;
            switch (op.kind) {
                case TokenKind::LogNot: oc = OpCode::LogNot; break;
                case TokenKind::Plus:   oc = OpCode::Pos; break;
                case TokenKind::Minus:  oc = OpCode::Neg; break;
                case TokenKind::BitNot: oc = OpCode::BitNot; break;
                default: break;
            }
            chunk_.emit(oc, 0, 0, static_cast<u16>(op.line));
            return Result<void>::success();
        }
        return parse_postfix();
    }

    Result<void> parse_call_expr() {
        const Token& fn_tok = advance();
        if (auto e = expect(TokenKind::LParen, "'(' in function call"); !e) return e.error();

        u8 argc = 0;
        if (!at(TokenKind::RParen)) {
            for (;;) {
                if (auto e = parse_expression(); !e) return e.error();
                ++argc;
                if (!match(TokenKind::Comma)) break;
                // GM8 permits trailing commas before ')' (e.g. choose(1, 2,)).
                if (at(TokenKind::RParen)) break;
            }
        }
        if (auto e = expect(TokenKind::RParen, "')' after function arguments"); !e) return e.error();

        const u16 line = static_cast<u16>(fn_tok.line);
        if (fn_tok.kind == TokenKind::DndCodeAction) {
            const auto name = chunk_.debug_name + ":action" + std::to_string(fn_tok.line);
            auto action = GmlCompiler::compile_chunk(name, fn_tok.text, symbols_);
            if (!action) return action.error();
            chunk_.emit(OpCode::CallCodeAction, static_cast<u32>(chunk_.code_actions.size()), argc, line);
            chunk_.code_actions.push_back(std::make_shared<BytecodeChunk>(std::move(action.value())));
            return Result<void>::success();
        }
        const CallableTarget* target = symbols_.find_callable(fn_tok.text);
        if (!target) {
            return error_here("Unknown function or script '" + fn_tok.text + "'");
        }

        switch (target->kind) {
            case CallableKind::Builtin:
                chunk_.emit(OpCode::CallBuiltin, target->id, argc, line);
                break;
            case CallableKind::Script:
                chunk_.emit(OpCode::CallScript, target->id, argc, line);
                break;
            case CallableKind::ExtensionGml:
                chunk_.emit(OpCode::CallExtGml, target->id, argc, line);
                break;
            case CallableKind::ExtensionDll:
                chunk_.emit(OpCode::CallExtDll, target->id, argc, line);
                break;
        }
        return Result<void>::success();
    }

    // Level 8: primary + `.member[...]` chain
    Result<void> parse_postfix() {
        if (at(TokenKind::Number)) {
            const Token t = advance();
            emit_push_real(t.number_val, static_cast<u16>(t.line));
        } else if (at(TokenKind::String)) {
            const Token t = advance();
            const u32 idx = chunk_.add_string(t.text);
            chunk_.emit(OpCode::PushString, idx, 0, static_cast<u16>(t.line));
        } else if (match(TokenKind::LParen)) {
            if (auto e = parse_expression(); !e) return e.error();
            if (auto e = expect(TokenKind::RParen, "')'"); !e) return e.error();
        } else if (at_call()) {
            if (auto e = parse_call_expr(); !e) return e.error();
        } else if (at(TokenKind::Identifier)) {
            if (peek().text == "global" && peek(1).kind == TokenKind::Dot) {
                advance(); // 'global'
                advance(); // '.'
                if (!at(TokenKind::Identifier)) return error_here("Expected member name after 'global.'");
                const Token field = advance();
                auto dims_res = parse_optional_array_indices();
                if (!dims_res) return dims_res.error();
                PendingLValue lv{LValueKind::GlobalUser, symbols_.intern_user_var(field.text),
                                 dims_res.value(), false, static_cast<u16>(field.line)};
                emit_lvalue_load(lv);
            } else if (peek().text == "self" && peek(1).kind == TokenKind::Dot) {
                advance(); // 'self'
                advance(); // '.'
                if (!at(TokenKind::Identifier)) return error_here("Expected member name after 'self.'");
                const Token field = advance();
                auto dims_res = parse_optional_array_indices();
                if (!dims_res) return dims_res.error();
                PendingLValue lv{};
                lv.line = static_cast<u16>(field.line);
                lv.arr_dims = dims_res.value();
                if (const auto* bv = symbols_.find_builtin_var(field.text); bv && !bv->is_global) {
                    lv.kind = LValueKind::BuiltinInst;
                    lv.id = bv->id;
                    lv.is_array_builtin = bv->is_array;
                } else {
                    lv.kind = LValueKind::SelfUser;
                    lv.id = symbols_.intern_user_var(field.text);
                }
                if (lv.is_array_builtin && lv.arr_dims == 0) {
                    chunk_.emit(OpCode::PushZero, 0, 0, lv.line);
                    lv.arr_dims = 1;
                }
                emit_lvalue_load(lv);
            } else {
                const Token ident = advance();
                const u16 line = static_cast<u16>(ident.line);
                // Precedence for non-call identifier: local var -> constant -> builtin/globalvar/self var
                if (auto slot = find_local(ident.text); slot.has_value()) {
                    auto dims_res = parse_optional_array_indices();
                    if (!dims_res) return dims_res.error();
                    PendingLValue lv{LValueKind::Local, slot.value(), dims_res.value(), false, line};
                    emit_lvalue_load(lv);
                } else if (const auto* cb = symbols_.find_constant(ident.text); cb && !at(TokenKind::LBracket)) {
                    if (cb->is_expression) {
                        chunk_.emit(OpCode::LoadConstSlot, cb->expr_slot, 0, line);
                    } else {
                        emit_push_real(cb->numeric_value, line);
                    }
                } else {
                    PendingLValue lv = resolve_ident_lvalue(ident.text, line);
                    auto dims_res = parse_optional_array_indices();
                    if (!dims_res) return dims_res.error();
                    lv.arr_dims = dims_res.value();
                    if (lv.is_array_builtin && lv.arr_dims == 0) {
                        chunk_.emit(OpCode::PushZero, 0, 0, line);
                        lv.arr_dims = 1;
                    }
                    emit_lvalue_load(lv);
                }
            }
        } else {
            return error_here("Expected expression");
        }

        // Trailing `.member[...]` chain
        while (match(TokenKind::Dot)) {
            if (!at(TokenKind::Identifier)) {
                return error_here("Expected member name after '.'");
            }
            const Token field = advance();
            auto dims_res = parse_optional_array_indices();
            if (!dims_res) return dims_res.error();

            PendingLValue lv{};
            lv.kind = LValueKind::Field;
            lv.line = static_cast<u16>(field.line);
            lv.arr_dims = dims_res.value();
            if (const auto* bv = symbols_.find_builtin_var(field.text); bv && !bv->is_global) {
                lv.id = bv->id;
                lv.is_array_builtin = bv->is_array;
            } else {
                lv.id = symbols_.intern_user_var(field.text);
            }
            if (lv.is_array_builtin && lv.arr_dims == 0) {
                chunk_.emit(OpCode::PushZero, 0, 0, lv.line);
                lv.arr_dims = 1;
            }
            emit_lvalue_load(lv);
        }

        return Result<void>::success();
    }

    std::vector<Token> tokens_;
    usize pos_ = 0;
    SymbolTable& symbols_;
    BytecodeChunk chunk_;
    std::unordered_map<std::string, u32> locals_;
    u32 next_temp_slot_ = 0;
    std::vector<LoopContext> loops_;
    std::vector<u32> action_end_jumps_;
};

} // namespace

Result<BytecodeChunk> GmlCompiler::compile_chunk(
    std::string_view debug_name,
    std::string_view source,
    SymbolTable& symbols) {
    auto lex_res = lex_gml(source);
    if (!lex_res) {
        return Error{std::string(debug_name) + ": " + lex_res.error_message()};
    }
    ParserCompiler pc(debug_name, std::move(lex_res.value().tokens), symbols);
    return pc.compile_program();
}

Result<BytecodeChunk> GmlCompiler::compile_expression(
    std::string_view debug_name,
    std::string_view expr_source,
    SymbolTable& symbols) {
    auto lex_res = lex_gml(expr_source);
    if (!lex_res) {
        return Error{std::string(debug_name) + ": " + lex_res.error_message()};
    }
    ParserCompiler pc(debug_name, std::move(lex_res.value().tokens), symbols);
    return pc.compile_single_expression();
}

CompiledProgram GmlCompiler::compile_game(const GameData& game) {
    CompiledProgram prog{};
    prog.symbols.populate_from_game(game);

    auto compile_event = [&](std::string_view name, const EventData& event, bool appendable = false) -> Result<BytecodeChunk> {
        auto lexed = lex_dnd_event(event, game, appendable);
        if (!lexed) return Error{std::string(name) + ": " + lexed.error_message()};
        ParserCompiler parser(name, std::move(lexed.value().tokens), prog.symbols);
        return parser.compile_program(appendable);
    };

    auto record_error = [&](std::string name, std::string msg, std::string_view src = {}) {
        ++prog.stats.total_errors;
        std::string snippet;
        if (!src.empty()) {
            snippet = std::string(src.substr(0, std::min<usize>(src.size(), 400)));
        }
        prog.errors.push_back(CompileErrorInfo{std::move(name), std::move(msg), std::move(snippet)});
    };

    // Pass 1: Compile expression constants.
    prog.const_init_chunks.reserve(prog.symbols.expression_constants().size());
    for (const auto& ec : prog.symbols.expression_constants()) {
        std::string uname = "const:" + ec.name;
        auto res = compile_expression(uname, ec.expr_source, prog.symbols);
        if (res) {
            prog.stats.total_instructions += res.value().code.size();
            ++prog.stats.expr_consts_compiled;
            prog.const_init_chunks.push_back(std::move(res.value()));
        } else {
            record_error(uname, res.error_message());
            BytecodeChunk fallback;
            fallback.debug_name = uname;
            fallback.emit(OpCode::PushZero);
            fallback.emit(OpCode::Return);
            prog.const_init_chunks.push_back(std::move(fallback));
        }
    }

    // Compile extension GML functions.
    prog.ext_gml_chunks.reserve(prog.symbols.extension_gml_functions().size());
    for (const auto& fn : prog.symbols.extension_gml_functions()) {
        std::string uname = "ext:" + fn.name;
        auto res = compile_chunk(uname, fn.source, prog.symbols);
        if (res) {
            prog.stats.total_instructions += res.value().code.size();
            ++prog.stats.ext_gml_compiled;
            prog.ext_gml_chunks.push_back(std::move(res.value()));
        } else {
            record_error(uname, res.error_message());
            prog.ext_gml_chunks.emplace_back();
        }
    }

    // Compile game scripts.
    prog.script_chunks.resize(game.scripts.size());
    for (usize i = 0; i < game.scripts.size(); ++i) {
        const auto& sc = game.scripts[i];
        if (!sc.exists) continue;
        std::string uname = "script:" + sc.name;
        auto res = compile_chunk(uname, sc.source, prog.symbols);
        if (res) {
            prog.stats.total_instructions += res.value().code.size();
            ++prog.stats.scripts_compiled;
            prog.script_chunks[i] = std::move(res.value());
        } else {
            record_error(uname, res.error_message(), sc.source);
        }
    }

    // Compile triggers (GM8 triggers can be either a bare expression or a GML block using `return`).
    prog.trigger_chunks.resize(game.triggers.size());
    for (usize i = 0; i < game.triggers.size(); ++i) {
        const auto& tr = game.triggers[i];
        if (!tr.exists) continue;
        std::string uname = "trigger:" + tr.name;
        auto res = compile_expression(uname, tr.condition, prog.symbols);
        if (!res) {
            res = compile_chunk(uname, tr.condition, prog.symbols);
        }
        if (res) {
            prog.stats.total_instructions += res.value().code.size();
            ++prog.stats.triggers_compiled;
            prog.trigger_chunks[i] = std::move(res.value());
        } else {
            record_error(uname, res.error_message(), tr.condition);
        }
    }

    // Compile object events.
    prog.object_event_chunks.resize(game.objects.size());
    for (usize oi = 0; oi < game.objects.size(); ++oi) {
        const auto& obj = game.objects[oi];
        if (!obj.exists) continue;
        for (usize cat = 0; cat < 12; ++cat) {
            for (const auto& [sub_id, event] : obj.events[cat]) {
                std::string uname = "obj:" + obj.name + ":ev" + std::to_string(cat) + "_" + std::to_string(sub_id);
                auto res = compile_event(uname, event);
                if (res) {
                    prog.stats.total_instructions += res.value().instruction_count();
                    ++prog.stats.object_events_compiled;
                    prog.object_event_chunks[oi][cat].emplace(sub_id, std::move(res.value()));
                } else {
                    record_error(uname, res.error_message(), lower_dnd_event_to_gml(event, game));
                }
            }
        }
    }

    // Compile timeline moments.
    prog.timeline_chunks.resize(game.timelines.size());
    for (usize ti = 0; ti < game.timelines.size(); ++ti) {
        const auto& tl = game.timelines[ti];
        if (!tl.exists) continue;
        for (const auto& [step, event] : tl.moments) {
            std::string uname = "tl:" + tl.name + ":step" + std::to_string(step);
            auto res = compile_event(uname, event, true);
            if (res) {
                prog.stats.total_instructions += res.value().instruction_count();
                ++prog.stats.timeline_moments_compiled;
                prog.timeline_chunks[ti].emplace(step, std::make_shared<BytecodeChunk>(std::move(res.value())));
            } else {
                record_error(uname, res.error_message());
            }
        }
    }

    // Compile room & instance creation codes.
    prog.room_creation_chunks.resize(game.rooms.size());
    prog.instance_creation_chunks.resize(game.rooms.size());
    for (usize ri = 0; ri < game.rooms.size(); ++ri) {
        const auto& rm = game.rooms[ri];
        if (!rm.exists) continue;
        if (!rm.creation_code.empty()) {
            std::string uname = "room:" + rm.name;
            auto res = compile_chunk(uname, rm.creation_code, prog.symbols);
            if (res) {
                prog.stats.total_instructions += res.value().code.size();
                ++prog.stats.room_codes_compiled;
                prog.room_creation_chunks[ri] = std::move(res.value());
            } else {
                record_error(uname, res.error_message());
            }
        }
        for (const auto& inst : rm.instances) {
            if (inst.creation_code.empty()) continue;
            std::string uname = "room:" + rm.name + ":inst" + std::to_string(inst.id);
            auto res = compile_chunk(uname, inst.creation_code, prog.symbols);
            if (res) {
                prog.stats.total_instructions += res.value().code.size();
                ++prog.stats.instance_codes_compiled;
                prog.instance_creation_chunks[ri].emplace(inst.id, std::move(res.value()));
            } else {
                record_error(uname, res.error_message(), inst.creation_code);
            }
        }
    }

    // Compile library init codes.
    for (usize li = 0; li < game.libraries.size(); ++li) {
        if (game.libraries[li].empty()) continue;
        std::string uname = "library:" + std::to_string(li);
        auto res = compile_chunk(uname, game.libraries[li], prog.symbols);
        if (res) {
            prog.stats.total_instructions += res.value().code.size();
            ++prog.stats.library_codes_compiled;
            prog.library_init_chunks.push_back(std::move(res.value()));
        } else {
            record_error(uname, res.error_message());
        }
    }

    return prog;
}

void print_compile_summary(const CompiledProgram& prog, bool verbose) {
    const auto& s = prog.stats;
    std::printf("=== GML Compilation Summary ===\n");
    std::printf("  Scripts:            %zu\n", s.scripts_compiled);
    std::printf("  Extension GML:      %zu\n", s.ext_gml_compiled);
    std::printf("  Expr Constants:     %zu\n", s.expr_consts_compiled);
    std::printf("  Triggers:           %zu\n", s.triggers_compiled);
    std::printf("  Object Events:      %zu\n", s.object_events_compiled);
    std::printf("  Timeline Moments:   %zu\n", s.timeline_moments_compiled);
    std::printf("  Room Creation:      %zu\n", s.room_codes_compiled);
    std::printf("  Instance Creation:  %zu\n", s.instance_codes_compiled);
    std::printf("  Library Init:       %zu\n", s.library_codes_compiled);
    std::printf("  Interned User Vars: %zu\n", prog.symbols.user_var_count());
    std::printf("  Total Instructions: %zu\n", s.total_instructions);
    std::printf("  Compile Errors:     %zu\n", s.total_errors);

    if (!prog.errors.empty()) {
        const usize limit = verbose ? prog.errors.size() : std::min<usize>(prog.errors.size(), 20);
        std::printf("\n--- Errors (showing %zu / %zu) ---\n", limit, prog.errors.size());
        for (usize i = 0; i < limit; ++i) {
            std::printf("  [%zu] %s\n", i + 1, prog.errors[i].message.c_str());
            if (verbose && !prog.errors[i].source_snippet.empty()) {
                std::printf("      --- source ---\n%s\n      --------------\n",
                            prog.errors[i].source_snippet.c_str());
            }
        }
    }
}

} // namespace gm
