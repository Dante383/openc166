// SPDX-License-Identifier: MIT
// Written from the behaviour described in TASKING's m166 manual; no TASKING
// code is used.
#include "m166.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace oc166 {
namespace {

// Built-in functions of m166 that openc166 does not support.
constexpr std::string_view kUnsupported[] = {
    "DEFINE", "ENDD", "LOCAL", "WHILE", "ENDW", "REPEAT", "ENDR", "BREAK", "EXIT", "ABORT", "LEN",
    "SUBSTR", "MATCH", "EQS", "NES", "LTS", "LES", "GTS", "GES", "IN", "OUT",
};

bool name_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string upper(std::string_view s) {
    std::string u(s);
    for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

bool same_name(std::string_view a, std::string_view b, bool case_sensitive) {
    return case_sensitive ? a == b : upper(a) == upper(b);
}

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

bool valid_name(std::string_view s) {
    if (s.empty() || !name_start(s[0])) return false;
    for (char c : s)
        if (!name_char(c)) return false;
    return true;
}

// The text between the parentheses after index 'i' (spaces may come
// first), as [first, last); moves 'i' past the ')'.
std::optional<std::pair<std::size_t, std::size_t>> paren_arg(std::string_view text, std::size_t& i) {
    std::size_t p = i;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (p >= text.size() || text[p] != '(') return std::nullopt;
    int depth = 0;
    for (std::size_t q = p; q < text.size(); ++q) {
        if (text[q] == '(') ++depth;
        if (text[q] == ')' && --depth == 0) {
            i = q + 1;
            return std::pair{p + 1, q};
        }
    }
    return std::nullopt;
}

// 32-bit wrap-around, as m166 computes.
std::int64_t wrap(std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v)); }
std::uint32_t bits(std::int64_t v) { return static_cast<std::uint32_t>(v); }

// m166 expressions over numbers only: macro calls are expanded before.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) { next(); }

    std::int64_t parse() {
        std::int64_t v = parse_or();
        if (kind_ != Kind::End) fail("unexpected '" + tok_ + "' in the expression");
        return v;
    }

    struct Error {
        std::string message;
    };

private:
    enum class Kind { Num, Word, Op, End };
    std::string_view text_;
    std::size_t pos_ = 0;
    Kind kind_ = Kind::End;
    std::string tok_;  // upper case for words
    std::int64_t num_ = 0;

    [[noreturn]] static void fail(std::string message) { throw Error{std::move(message)}; }

    void next() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ >= text_.size()) {
            kind_ = Kind::End;
            tok_ = "end";
            return;
        }
        const char c = text_[pos_];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t start = pos_;
            while (pos_ < text_.size() && std::isalnum(static_cast<unsigned char>(text_[pos_]))) ++pos_;
            tok_ = upper(text_.substr(start, pos_ - start));
            kind_ = Kind::Num;
            num_ = number(tok_);
            return;
        }
        if (name_start(c)) {
            std::size_t start = pos_;
            while (pos_ < text_.size() && name_char(text_[pos_])) ++pos_;
            tok_ = upper(text_.substr(start, pos_ - start));
            kind_ = Kind::Word;
            return;
        }
        static constexpr std::string_view kTwo[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||"};
        for (std::string_view op : kTwo)
            if (text_.substr(pos_, 2) == op) {
                tok_ = std::string(op);
                pos_ += 2;
                kind_ = Kind::Op;
                return;
            }
        if (std::string_view("()+-~*/%<>!&^|").find(c) == std::string_view::npos)
            fail(std::string("unexpected '") + c + "' in the expression");
        tok_ = std::string(1, c);
        ++pos_;
        kind_ = Kind::Op;
    }

    static std::int64_t number(const std::string& t) {
        std::string digits = t;
        int base = 10;
        if (digits.size() > 2 && digits[0] == '0' && digits[1] == 'X') {
            base = 16;
            digits = digits.substr(2);
        } else {
            switch (digits.back()) {
            case 'H': base = 16; break;
            case 'B': case 'Y': base = 2; break;
            case 'O': case 'Q': base = 8; break;
            case 'D': case 'T': base = 10; break;
            default: break;
            }
            if (!std::isdigit(static_cast<unsigned char>(digits.back()))) digits.pop_back();
        }
        if (digits.empty()) fail("bad number '" + t + "'");
        std::uint64_t v = 0;
        for (char d : digits) {
            const int n = std::isdigit(static_cast<unsigned char>(d)) ? d - '0' : d >= 'A' && d <= 'F' ? d - 'A' + 10 : 99;
            if (n >= base) fail("bad number '" + t + "'");
            v = (v * static_cast<unsigned>(base) + static_cast<unsigned>(n)) & 0xFFFFFFFFu;
        }
        return wrap(static_cast<std::int64_t>(v));
    }

    bool is(std::string_view s) const { return (kind_ == Kind::Op || kind_ == Kind::Word) && tok_ == s; }

    std::int64_t parse_or() {
        std::int64_t l = parse_and();
        for (;;) {
            if (is("|") || is("OR")) {
                next();
                l = wrap(l | parse_and());
            } else if (is("^") || is("XOR")) {
                next();
                l = wrap(l ^ parse_and());
            } else if (is("||")) {
                next();
                const std::int64_t r = parse_and();
                l = (l != 0 || r != 0) ? 1 : 0;
            } else {
                return l;
            }
        }
    }

    std::int64_t parse_and() {
        std::int64_t l = parse_not();
        for (;;) {
            if (is("&") || is("AND")) {
                next();
                l = wrap(l & parse_not());
            } else if (is("&&")) {
                next();
                const std::int64_t r = parse_not();
                l = (l != 0 && r != 0) ? 1 : 0;
            } else {
                return l;
            }
        }
    }

    // NOT and '!' give 1 for 0 and 0 for anything else, so that they turn a
    // condition around.
    std::int64_t parse_not() {
        if (is("!") || is("NOT")) {
            next();
            return parse_not() == 0 ? 1 : 0;
        }
        return parse_rel();
    }

    std::int64_t parse_rel() {
        std::int64_t l = parse_add();
        for (;;) {
            const std::string op = kind_ == Kind::Op || kind_ == Kind::Word ? tok_ : "";
            bool r;
            if (op == "<" || op == "LT" || op == "<=" || op == "LE" || op == ">" || op == "GT" || op == ">=" ||
                op == "GE" || op == "==" || op == "EQ" || op == "!=" || op == "NE" || op == "ULT" || op == "ULE" ||
                op == "UGT" || op == "UGE") {
                next();
                const std::int64_t v = parse_add();
                const std::uint32_t a = bits(l), b = bits(v);
                if (op == "<" || op == "LT") r = l < v;
                else if (op == "<=" || op == "LE") r = l <= v;
                else if (op == ">" || op == "GT") r = l > v;
                else if (op == ">=" || op == "GE") r = l >= v;
                else if (op == "==" || op == "EQ") r = l == v;
                else if (op == "!=" || op == "NE") r = l != v;
                else if (op == "ULT") r = a < b;
                else if (op == "ULE") r = a <= b;
                else if (op == "UGT") r = a > b;
                else r = a >= b;
                l = r ? 1 : 0;
            } else {
                return l;
            }
        }
    }

    std::int64_t parse_add() {
        std::int64_t l = parse_mul();
        for (;;) {
            if (is("+")) {
                next();
                l = wrap(l + parse_mul());
            } else if (is("-")) {
                next();
                l = wrap(l - parse_mul());
            } else {
                return l;
            }
        }
    }

    std::int64_t parse_mul() {
        std::int64_t l = parse_unary();
        for (;;) {
            if (is("*")) {
                next();
                l = wrap(l * parse_unary());
            } else if (is("/") || is("%") || is("MOD")) {
                const bool div = is("/");
                next();
                const std::int64_t r = parse_unary();
                if (r == 0) fail("division by zero");
                l = wrap(div ? l / r : l % r);
            } else if (is("<<") || is("SHL")) {
                next();
                const std::uint32_t n = bits(parse_unary());
                l = n >= 32 ? 0 : wrap(static_cast<std::int64_t>(bits(l) << n));
            } else if (is(">>") || is("SHR")) {
                next();
                const std::uint32_t n = bits(parse_unary());
                l = n >= 32 ? 0 : wrap(static_cast<std::int64_t>(bits(l) >> n));
            } else {
                return l;
            }
        }
    }

    std::int64_t parse_unary() {
        if (is("+")) {
            next();
            return parse_unary();
        }
        if (is("-")) {
            next();
            return wrap(-parse_unary());
        }
        if (is("~")) {
            next();
            return wrap(~parse_unary());
        }
        // HIGH shifts right by 8 keeping the sign; LOW keeps the low 8 bits.
        if (is("HIGH")) {
            next();
            return parse_unary() >> 8;
        }
        if (is("LOW")) {
            next();
            return parse_unary() & 0xFF;
        }
        return primary();
    }

    std::int64_t primary() {
        if (kind_ == Kind::Num) {
            const std::int64_t v = num_;
            next();
            return v;
        }
        if (is("(")) {
            next();
            const std::int64_t v = parse_or();
            if (!is(")")) fail("')' expected");
            next();
            return v;
        }
        if (kind_ == Kind::End) fail("value expected");
        if (kind_ == Kind::Word)
            fail("'" + tok_ + "' is not known when m166 runs: only numbers and m166 macros (@name) can be used");
        fail("unexpected '" + tok_ + "' in the expression");
    }
};

}  // namespace

std::string M166::format(std::int64_t value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::uint32_t v = bits(value);
    std::string digits;
    do {
        digits.insert(digits.begin(), kHex[v & 0xF]);
        v >>= 4;
    } while (v != 0);
    return "0" + digits + "h";
}

void M166::reset() {
    ifs_.clear();
    macros_.clear();
    pending_.clear();
}

const std::string* M166::find(std::string_view name, bool case_sensitive) const {
    if (auto it = macros_.find(std::string(name)); it != macros_.end()) return &it->second;
    if (case_sensitive) return nullptr;
    for (const auto& [n, text] : macros_)
        if (same_name(n, name, false)) return &text;
    return nullptr;
}

void M166::define(std::string_view name, std::string text, bool case_sensitive) {
    if (!case_sensitive)
        for (auto it = macros_.begin(); it != macros_.end(); ++it)
            if (it->first != name && same_name(it->first, name, false)) {
                macros_.erase(it);
                break;
            }
    macros_[std::string(name)] = std::move(text);
}

M166::Line M166::process(std::string_view text, bool case_sensitive, Reporter& rep) {
    Line out;
    out.active_before = active();
    out.text = std::move(pending_);
    pending_.clear();
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '@') {
            if (active()) out.text += text[i];
            ++i;
            continue;
        }
        out.calls = true;
        const int column = static_cast<int>(i) + 1;
        // @"comment" or @"comment to the end of the line, newline included.
        if (i + 1 < text.size() && text[i + 1] == '"') {
            const std::size_t q = text.find('"', i + 2);
            if (q == std::string_view::npos) {
                if (active()) pending_ = std::exchange(out.text, {});
                break;
            }
            i = q + 1;
            continue;
        }
        std::size_t j = i + 1;
        if (j < text.size() && name_start(text[j]))
            while (j < text.size() && name_char(text[j])) ++j;
        const std::string word = upper(text.substr(i + 1, j - i - 1));
        if (word == "IF") {
            i = j;
            const auto arg = paren_arg(text, i);
            if (!active()) {
                ifs_.push_back({false, false, true, false});
                continue;
            }
            bool ok = false;
            std::int64_t v = 0;
            if (arg)
                v = evaluate(text.substr(arg->first, arg->second - arg->first), static_cast<int>(arg->first) + 1, true,
                             case_sensitive, rep, ok);
            else
                rep.error("OC30", "m166: @IF needs a condition in parentheses", column);
            // m166 takes a value above 0 as true.
            const bool c = ok && v > 0;
            ifs_.push_back({true, c, c, false});
            continue;
        }
        if (word == "ELSE") {
            i = j;
            if (ifs_.empty()) {
                rep.error("OC30", "m166: @ELSE without @IF", column);
            } else if (If& top = ifs_.back(); top.parent_active) {
                if (top.in_else) rep.error("OC30", "m166: second @ELSE for the same @IF", column);
                top.in_else = true;
                top.active = !top.taken;
                top.taken = true;
            }
            continue;
        }
        if (word == "ENDI") {
            i = j;
            if (ifs_.empty())
                rep.error("OC30", "m166: @ENDI without @IF", column);
            else
                ifs_.pop_back();
            continue;
        }
        if (!active()) {
            i = j;
            continue;
        }
        call(text, i, 0, false, case_sensitive, rep, out.text);
    }
    out.active_after = active();
    return out;
}

// Expands the call at text[i] ('@') into 'out' and moves 'i' past it.
// Returns false after reporting a problem.
bool M166::call(std::string_view text, std::size_t& i, int base_column, bool undefined_is_zero, bool case_sensitive,
                Reporter& rep, std::string& out) {
    const int column = base_column + static_cast<int>(i) + 1;
    std::size_t start = i + 1;
    const bool literal = start < text.size() && text[start] == '*';
    if (literal) ++start;
    std::size_t j = start;
    if (j < text.size() && name_start(text[j]))
        while (j < text.size() && name_char(text[j])) ++j;
    if (j == start) {
        rep.error("OC30", "m166: '@' must be followed by the name of a macro or function", column);
        i = start;
        return false;
    }
    const std::string_view name = text.substr(start, j - start);
    const std::string word = upper(name);
    i = j;
    auto argument = [&](std::string_view* arg, int* arg_column) {
        const auto a = paren_arg(text, i);
        if (!a) {
            rep.error("OC30", "m166: @" + word + " needs its arguments in parentheses", column);
            return false;
        }
        *arg = text.substr(a->first, a->second - a->first);
        *arg_column = base_column + static_cast<int>(a->first) + 1;
        return true;
    };
    std::string_view arg;
    int arg_column = 0;
    bool ok = false;
    if (literal || std::find(std::begin(kUnsupported), std::end(kUnsupported), word) != std::end(kUnsupported)) {
        rep.error("OC29", "m166 function @" + std::string(literal ? "*" : "") + word +
                              " is not supported by openc166 (only @SET, @EVAL, @IF, @ELSE, @ENDI, @DEFINED, "
                              "macro variables and @\" comments are)",
                  column);
        return false;
    }
    if (word == "SET") {
        if (!argument(&arg, &arg_column)) return false;
        int depth = 0;
        std::size_t comma = std::string_view::npos;
        for (std::size_t k = 0; k < arg.size() && comma == std::string_view::npos; ++k) {
            if (arg[k] == '(') ++depth;
            if (arg[k] == ')') --depth;
            if (arg[k] == ',' && depth == 0) comma = k;
        }
        const std::string_view var = trim(arg.substr(0, comma));
        if (comma == std::string_view::npos || !valid_name(var)) {
            rep.error("OC30", "m166: @SET takes a macro name and an expression: @SET(name, expression)", arg_column);
            return false;
        }
        const std::int64_t v =
            evaluate(arg.substr(comma + 1), arg_column + static_cast<int>(comma) + 1, false, case_sensitive, rep, ok);
        if (ok) define(var, format(v), case_sensitive);
        return ok;
    }
    if (word == "EVAL") {
        if (!argument(&arg, &arg_column)) return false;
        const std::int64_t v = evaluate(arg, arg_column, undefined_is_zero, case_sensitive, rep, ok);
        if (ok) out += format(v);
        return ok;
    }
    if (word == "DEFINED") {
        if (!argument(&arg, &arg_column)) return false;
        std::string_view n = trim(arg);
        if (!n.empty() && n[0] == '@') n.remove_prefix(1);
        if (!valid_name(n)) {
            rep.error("OC30", "m166: @DEFINED takes a macro name", arg_column);
            return false;
        }
        out += format(find(n, case_sensitive) ? 1 : 0);
        return true;
    }
    if (word == "IF" || word == "ELSE" || word == "ENDI") {
        rep.error("OC30", "m166: @" + word + " cannot be used inside the arguments of a function", column);
        return false;
    }
    // A call may give an empty argument list: @name().
    if (i + 1 < text.size() && text[i] == '(' && text[i + 1] == ')') i += 2;
    if (const std::string* body = find(name, case_sensitive)) {
        out += *body;
        return true;
    }
    // In @IF conditions, m166 takes an undefined macro as 0.
    if (undefined_is_zero) {
        out += '0';
        return true;
    }
    rep.error("OC30", "m166: undefined macro @" + std::string(name), column);
    return false;
}

// Expands the m166 calls in 'text', an argument starting at 'column'.
std::string M166::expand(std::string_view text, int column, bool undefined_is_zero, bool case_sensitive,
                         Reporter& rep) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '@') {
            out += text[i++];
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '"') {
            const std::size_t q = text.find('"', i + 2);
            i = q == std::string_view::npos ? text.size() : q + 1;
            continue;
        }
        if (!call(text, i, column - 1, undefined_is_zero, case_sensitive, rep, out)) out += '0';
    }
    return out;
}

std::int64_t M166::evaluate(std::string_view text, int column, bool undefined_is_zero, bool case_sensitive,
                            Reporter& rep, bool& ok) {
    const std::size_t errors = rep.items().size();
    const std::string expanded = expand(text, column, undefined_is_zero, case_sensitive, rep);
    ok = rep.items().size() == errors;
    if (!ok) return 0;
    try {
        return Parser(expanded).parse();
    } catch (const Parser::Error& e) {
        rep.error("OC30", "m166: " + e.message + " (in '" + std::string(trim(expanded)) + "')", column);
        ok = false;
        return 0;
    }
}

}  // namespace oc166
