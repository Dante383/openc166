// SPDX-License-Identifier: MIT
// Tokenizer for one A166 source line.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace oc166 {

enum class TokKind : std::uint8_t {
    End,     // end of line (always the last token)
    Ident,   // name, keyword or mnemonic
    Number,  // numeric constant; num holds the value
    String,  // quoted string; text holds the raw contents with doubled quotes
    Op,      // operator or punctuation; text holds its spelling
};

struct Token {
    TokKind kind = TokKind::End;
    std::string_view text;
    std::uint64_t num = 0;
    int column = 0;  // 1-based

    bool is(TokKind k, std::string_view t) const { return kind == k && text == t; }
    bool is_op(std::string_view t) const { return kind == TokKind::Op && text == t; }
};

// Splits a line into tokens, stopping at a ';' comment. Throws AsmError.
std::vector<Token> tokenize(std::string_view line);

// Decodes a quoted string's contents (doubled quotes become one quote).
std::vector<std::uint8_t> string_bytes(const Token& t);

// Characters that start and continue a name.
bool ident_start(char c);
bool ident_char(char c);

// Case-insensitive comparison with an upper-case keyword.
bool ieq(std::string_view s, std::string_view upper);

}  // namespace oc166
