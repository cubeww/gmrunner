#pragma once

#include "base/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace gm {

enum class TokenKind : u8 {
    Eof = 0,
    Number,
    String,
    Identifier,

    // Keywords
    KwVar,
    KwGlobalVar,
    KwIf,
    KwThen,
    KwElse,
    KwBegin,
    KwEnd,
    KwWhile,
    KwDo,
    KwUntil,
    KwFor,
    KwRepeat,
    KwWith,
    KwSwitch,
    KwCase,
    KwDefault,
    KwExit,
    KwReturn,
    KwBreak,
    KwContinue,
    KwDiv,
    KwMod,

    // Assignment & compound assignment
    Assign,       // = or :=
    PlusAssign,   // +=
    MinusAssign,  // -=
    StarAssign,   // *=
    SlashAssign,  // /=
    BitOrAssign,  // |=
    BitAndAssign, // &=
    BitXorAssign, // ^=

    // Comparison
    Equal,        // ==
    NotEqual,     // != or <>
    Less,         // <
    LessEqual,    // <=
    Greater,      // >
    GreaterEqual, // >=

    // Arithmetic / bitwise / logical
    Plus,         // +
    Minus,        // -
    Star,         // *
    Slash,        // /
    BitOr,        // |
    BitAnd,       // &
    BitXor,       // ^ or xor
    BitNot,       // ~
    Shl,          // <<
    Shr,          // >>
    LogAnd,       // && or and
    LogOr,        // || or or
    LogXor,       // ^^
    LogNot,       // ! or not

    // Punctuation
    LParen,       // (
    RParen,       // )
    LBracket,     // [
    RBracket,     // ]
    LBrace,       // {
    RBrace,       // }
    Dot,          // .
    Comma,        // ,
    Semicolon,    // ;
    Colon,        // :
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    u32 line = 1;
    u32 col = 1;
    f64 number_val = 0.0;
    std::string text;
};

struct LexResult {
    std::vector<Token> tokens;
    bool had_unclosed_comment = false;
};

[[nodiscard]] Result<LexResult> lex_gml(std::string_view source);

} // namespace gm
