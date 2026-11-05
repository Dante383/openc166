// SPDX-License-Identifier: MIT
#include "lexer.hpp"

#include "diag.hpp"

#include <cctype>
#include <string>

namespace oc166 {
namespace {

int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

// Number syntax: a leading digit, optional '$' separators, and a radix
// suffix H, D, O, Q or B; C-style 0x prefixes are accepted as well.
std::uint64_t parse_number(std::string_view text, int column) {
    std::string s;
    for (char c : text)
        if (c != '$') s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (text.back() == '$') throw AsmError("A1", "illegal character in numeric constant", column);
    unsigned radix = 10;
    std::string_view body = s;
    if (body.size() > 2 && body[0] == '0' && body[1] == 'X') {
        radix = 16;
        body.remove_prefix(2);
    } else {
        switch (body.back()) {
        case 'H': radix = 16; body.remove_suffix(1); break;
        case 'D': radix = 10; body.remove_suffix(1); break;
        case 'O':
        case 'Q': radix = 8; body.remove_suffix(1); break;
        case 'B': radix = 2; body.remove_suffix(1); break;
        default: break;
        }
    }
    if (body.empty()) throw AsmError("A1", "illegal character in numeric constant", column);
    std::uint64_t v = 0;
    for (char c : body) {
        unsigned d = static_cast<unsigned>(digit_value(c));
        if (d >= radix) throw AsmError("A1", "illegal digit in number '" + std::string(text) + "'", column);
        if (v > (UINT64_MAX - d) / radix) throw AsmError("A1", "number too large", column);
        v = v * radix + d;
    }
    return v;
}

}  // namespace

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '?'; }
bool ident_char(char c) { return ident_start(c) || std::isdigit(static_cast<unsigned char>(c)); }

bool ieq(std::string_view s, std::string_view upper) {
    if (s.size() != upper.size()) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(s[i])) != upper[i]) return false;
    return true;
}

std::vector<Token> tokenize(std::string_view line) {
    static constexpr std::string_view kOps2[] = {"<<", ">>", "==", "!=", "<=", ">=", "<>"};
    static constexpr std::string_view kOps1 = "+-*/()[]#,.:$<>&|^=";
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        int column = static_cast<int>(i) + 1;
        if (c == ';') break;
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        Token t;
        t.column = column;
        if (ident_start(c)) {
            std::size_t j = i + 1;
            while (j < line.size() && ident_char(line[j])) ++j;
            t.kind = TokKind::Ident;
            t.text = line.substr(i, j - i);
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i + 1;
            while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '$')) ++j;
            t.kind = TokKind::Number;
            t.text = line.substr(i, j - i);
            // Keil reads "1.5" as a floating-point number, which no instruction accepts.
            bool decimal = t.text.find_first_not_of("0123456789") == std::string_view::npos;
            if (decimal && j + 1 < line.size() && line[j] == '.' && std::isdigit(static_cast<unsigned char>(line[j + 1])))
                throw AsmError("A70", "invalid type of expression: floating-point numbers are not supported", column);
            t.num = parse_number(t.text, column);
            i = j;
        } else if (c == '\'' || c == '"') {
            std::size_t j = i + 1;
            for (;;) {
                if (j >= line.size()) throw AsmError("A2", "missing string terminator", column);
                if (line[j] == c) {
                    if (j + 1 < line.size() && line[j + 1] == c) {
                        j += 2;
                        continue;
                    }
                    break;
                }
                ++j;
            }
            t.kind = TokKind::String;
            t.text = line.substr(i + 1, j - i - 1);
            i = j + 1;
        } else {
            t.kind = TokKind::Op;
            t.text = line.substr(i, 1);
            for (auto op : kOps2) {
                if (line.substr(i, 2) == op) {
                    t.text = line.substr(i, 2);
                    break;
                }
            }
            if (t.text.size() == 1 && kOps1.find(c) == std::string_view::npos)
                throw AsmError("A6", std::string("syntax error: illegal character '") + c + "'", column);
            i += t.text.size();
        }
        out.push_back(t);
    }
    Token end;
    end.column = static_cast<int>(line.size()) + 1;
    out.push_back(end);
    return out;
}

std::vector<std::uint8_t> string_bytes(const Token& t) {
    // The token text is a view into the line, so the opening quote precedes it.
    const char quote = t.text.data()[-1];
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < t.text.size(); ++i) {
        out.push_back(static_cast<std::uint8_t>(t.text[i]));
        if (t.text[i] == quote && i + 1 < t.text.size() && t.text[i + 1] == quote) ++i;
    }
    return out;
}

}  // namespace oc166
