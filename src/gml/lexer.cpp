#include "gml/lexer.h"

#include <cctype>
#include <cstdlib>

namespace gm {

namespace {

bool is_ident_start(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_';
}

bool is_ident_cont(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return is_ident_start(c) || (u >= '0' && u <= '9');
}

bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

TokenKind classify_keyword(std::string_view s) noexcept {
    if (s == "var")       return TokenKind::KwVar;
    if (s == "globalvar") return TokenKind::KwGlobalVar;
    if (s == "if")        return TokenKind::KwIf;
    if (s == "then")      return TokenKind::KwThen;
    if (s == "else")      return TokenKind::KwElse;
    if (s == "begin")     return TokenKind::KwBegin;
    if (s == "end")       return TokenKind::KwEnd;
    if (s == "while")     return TokenKind::KwWhile;
    if (s == "do")        return TokenKind::KwDo;
    if (s == "until")     return TokenKind::KwUntil;
    if (s == "for")       return TokenKind::KwFor;
    if (s == "repeat")    return TokenKind::KwRepeat;
    if (s == "with")      return TokenKind::KwWith;
    if (s == "switch")    return TokenKind::KwSwitch;
    if (s == "case")      return TokenKind::KwCase;
    if (s == "default")   return TokenKind::KwDefault;
    if (s == "exit")      return TokenKind::KwExit;
    if (s == "return")    return TokenKind::KwReturn;
    if (s == "break")     return TokenKind::KwBreak;
    if (s == "continue")  return TokenKind::KwContinue;
    if (s == "and")       return TokenKind::LogAnd;
    if (s == "or")        return TokenKind::LogOr;
    if (s == "not")       return TokenKind::LogNot;
    if (s == "div")       return TokenKind::KwDiv;
    if (s == "mod")       return TokenKind::KwMod;
    if (s == "xor")       return TokenKind::LogXor; // Note: verify xor precedence in GM8!
    return TokenKind::Identifier;
}

} // namespace

Result<LexResult> lex_gml(std::string_view source) {
    LexResult out{};
    out.tokens.reserve(source.size() / 4 + 8);

    usize i = 0;
    u32 line = 1;
    u32 col = 1;

    auto advance = [&]() noexcept -> char {
        const char c = source[i++];
        if (c == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
        return c;
    };

    auto peek = [&](usize offset = 0) noexcept -> char {
        return (i + offset < source.size()) ? source[i + offset] : '\0';
    };

    while (i < source.size()) {
        const char c = peek();

        // Skip whitespace (including UTF-8 BOM at start of file)
        if (i == 0 && source.size() >= 3 &&
            static_cast<u8>(source[0]) == 0xEFu &&
            static_cast<u8>(source[1]) == 0xBBu &&
            static_cast<u8>(source[2]) == 0xBFu) {
            i += 3;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            advance();
            continue;
        }

        // Comments: // line comment, /* block comment */
        if (c == '/' && peek(1) == '/') {
            while (i < source.size() && peek() != '\n') {
                advance();
            }
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            advance();
            advance();
            bool closed = false;
            while (i < source.size()) {
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) {
                out.had_unclosed_comment = true;
            }
            continue;
        }

        const u32 tok_line = line;
        const u32 tok_col = col;

        // String literal ("..." or '...', verbatim with no backslash escaping).
        // Matching Delphi GM8 (sub_611BE4): an unclosed string at EOF closes silently.
        if (c == '"' || c == '\'') {
            const char quote = advance();
            std::string val;
            while (i < source.size()) {
                const char sc = advance();
                if (sc == quote) {
                    break;
                }
                val.push_back(sc);
            }
            out.tokens.push_back(Token{TokenKind::String, tok_line, tok_col, 0.0, std::move(val)});
            continue;
        }

        // Hex literal: $FF00FF
        if (c == '$') {
            advance();
            u64 hex_val = 0;
            usize digits = 0;
            while (i < source.size() && is_hex_digit(peek())) {
                const char h = advance();
                u64 d = 0;
                if (h >= '0' && h <= '9') d = static_cast<u64>(h - '0');
                else if (h >= 'a' && h <= 'f') d = static_cast<u64>(h - 'a' + 10);
                else d = static_cast<u64>(h - 'A' + 10);
                hex_val = (hex_val << 4u) | d;
                ++digits;
            }
            if (digits == 0) {
                return Error{"Expected hexadecimal digits after '$' at line " + std::to_string(tok_line)};
            }
            // GM8 treats hex literals as signed/unsigned 64-bit -> double.
            out.tokens.push_back(Token{TokenKind::Number, tok_line, tok_col, static_cast<f64>(hex_val), {}});
            continue;
        }

        // Numeric literal (123, 12.34, .5, 12.).
        // Matching Delphi GM8 (sub_611A94): consumes digits and '.' greedily (e.g. `1.5.-0.5` -> `1.5` `-` `0.5`).
        if ((c >= '0' && c <= '9') || (c == '.' && peek(1) >= '0' && peek(1) <= '9')) {
            const usize start = i;
            while (i < source.size()) {
                const char nc = peek();
                if ((nc >= '0' && nc <= '9') || nc == '.') {
                    advance();
                } else {
                    break;
                }
            }
            std::string num_str(source.substr(start, i - start));
            const f64 val = std::strtod(num_str.c_str(), nullptr);
            out.tokens.push_back(Token{TokenKind::Number, tok_line, tok_col, val, {}});
            continue;
        }

        // Identifier or keyword
        if (is_ident_start(c)) {
            const usize start = i;
            while (i < source.size() && is_ident_cont(peek())) {
                advance();
            }
            const std::string_view ident = source.substr(start, i - start);
            const TokenKind kw = classify_keyword(ident);
            if (kw == TokenKind::Identifier) {
                out.tokens.push_back(Token{TokenKind::Identifier, tok_line, tok_col, 0.0, std::string(ident)});
            } else {
                out.tokens.push_back(Token{kw, tok_line, tok_col, 0.0, {}});
            }
            continue;
        }

        // Operators and punctuation
        advance();
        auto emit = [&](TokenKind k) {
            out.tokens.push_back(Token{k, tok_line, tok_col, 0.0, {}});
        };

        switch (c) {
            case '(': emit(TokenKind::LParen); break;
            case ')': emit(TokenKind::RParen); break;
            case '[': emit(TokenKind::LBracket); break;
            case ']': emit(TokenKind::RBracket); break;
            case '{': emit(TokenKind::LBrace); break;
            case '}': emit(TokenKind::RBrace); break;
            case '.': emit(TokenKind::Dot); break;
            case ',': emit(TokenKind::Comma); break;
            case ';': emit(TokenKind::Semicolon); break;
            case '~': emit(TokenKind::BitNot); break;

            case ':':
                if (peek() == '=') { advance(); emit(TokenKind::Assign); }
                else emit(TokenKind::Colon);
                break;

            case '=':
                if (peek() == '=') { advance(); emit(TokenKind::Equal); }
                else emit(TokenKind::Assign);
                break;

            case '!':
                if (peek() == '=') { advance(); emit(TokenKind::NotEqual); }
                else emit(TokenKind::LogNot);
                break;

            case '<':
                if (peek() == '=') { advance(); emit(TokenKind::LessEqual); }
                else if (peek() == '>') { advance(); emit(TokenKind::NotEqual); }
                else if (peek() == '<') { advance(); emit(TokenKind::Shl); }
                else emit(TokenKind::Less);
                break;

            case '>':
                if (peek() == '=') { advance(); emit(TokenKind::GreaterEqual); }
                else if (peek() == '>') { advance(); emit(TokenKind::Shr); }
                else emit(TokenKind::Greater);
                break;

            case '+':
                if (peek() == '=') { advance(); emit(TokenKind::PlusAssign); }
                else emit(TokenKind::Plus);
                break;

            case '-':
                if (peek() == '=') { advance(); emit(TokenKind::MinusAssign); }
                else emit(TokenKind::Minus);
                break;

            case '*':
                if (peek() == '=') { advance(); emit(TokenKind::StarAssign); }
                else emit(TokenKind::Star);
                break;

            case '/':
                if (peek() == '=') { advance(); emit(TokenKind::SlashAssign); }
                else emit(TokenKind::Slash);
                break;

            case '|':
                if (peek() == '|') { advance(); emit(TokenKind::LogOr); }
                else if (peek() == '=') { advance(); emit(TokenKind::BitOrAssign); }
                else emit(TokenKind::BitOr);
                break;

            case '&':
                if (peek() == '&') { advance(); emit(TokenKind::LogAnd); }
                else if (peek() == '=') { advance(); emit(TokenKind::BitAndAssign); }
                else emit(TokenKind::BitAnd);
                break;

            case '^':
                if (peek() == '^') { advance(); emit(TokenKind::LogXor); }
                else if (peek() == '=') { advance(); emit(TokenKind::BitXorAssign); }
                else emit(TokenKind::BitXor);
                break;

            default:
                return Error{"Unexpected character '" + std::string(1, c) + "' (0x" +
                             std::to_string(static_cast<unsigned int>(static_cast<u8>(c))) +
                             ") at line " + std::to_string(tok_line) + ", col " + std::to_string(tok_col)};
        }
    }

    out.tokens.push_back(Token{TokenKind::Eof, line, col, 0.0, {}});
    return out;
}

} // namespace gm
