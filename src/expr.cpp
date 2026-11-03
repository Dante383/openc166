// SPDX-License-Identifier: MIT
#include "expr.hpp"

#include "predef.hpp"

#include <string>

namespace oc166 {
namespace {

using U = std::uint64_t;

constexpr std::string_view kOperatorKeywords[] = {
    "AND", "OR",  "XOR", "NOT", "MOD", "SHL", "SHR", "HIGH", "LOW", "EQ",    "NE",    "LT",    "LE",     "GT",
    "GE",  "ULT", "ULE", "UGT", "UGE", "SEG", "PAG", "SOF",  "POF", "BOF",   "DATA3", "DATA4", "DATA8", "DATA16",
};

int byte_word_index(std::string_view t, std::string_view prefix) {
    if (t.size() != prefix.size() + 1 || !ieq(t.substr(0, prefix.size()), prefix)) return -1;
    char d = t.back();
    return (d >= '0' && d <= '9') ? d - '0' : -1;
}

// Width of a DATAn operator (DATA3, DATA4, DATA8, DATA16), otherwise 0.
// Returns -1 for DATA32 and DATA64, which Keil rejects.
int data_width(const Token& t) {
    if (t.kind != TokKind::Ident) return 0;
    for (int w : {3, 4, 8, 16, 32, 64})
        if (ieq(t.text, "DATA" + std::to_string(w))) return w > 16 ? -1 : w;
    return 0;
}

// Operators other than + and - see a DPPn: prefixed value as the address it
// stands for. Keil does not mask the offset here: DPP2:4000H OR 0 is C000H.
Value resolved(Value v) {
    if (v.dpp >= 0) {
        v.num |= std::int64_t{v.dpp} << 14;
        v.dpp = -1;
    }
    return v;
}

class Evaluator {
public:
    Evaluator(TokenCursor& cur, SymbolContext& ctx, bool immediate = false)
        : cur_(cur), ctx_(ctx), immediate_(immediate) {}

    Value expression() {
        // SHORT and "type PTR" apply to everything after them.
        const Token& t = cur_.peek();
        if (t.kind == TokKind::Ident && ieq(t.text, "SHORT")) {
            cur_.next();
            Value v = expression();
            v.short_jump = true;
            return v;
        }
        if (t.kind == TokKind::Ident && cur_.peek(1).kind == TokKind::Ident && ieq(cur_.peek(1).text, "PTR")) {
            LType type = ptr_type(t.text);
            if (type == LType::None) throw AsmError("A6", "syntax error: unknown type before PTR", t.column);
            cur_.next();
            cur_.next();
            Value v = expression();
            // Keil ignores the type of plain numbers.
            if (v.rel == Rel::Addr) v.ltype = type;
            return v;
        }
        return comparison();
    }

    static LType ptr_type(std::string_view s) {
        if (ieq(s, "NEAR")) return LType::Near;
        if (ieq(s, "FAR")) return LType::Far;
        if (ieq(s, "BYTE")) return LType::Byte;
        if (ieq(s, "WORD")) return LType::Word;
        if (ieq(s, "DWORD")) return LType::Dword;
        return LType::None;
    }

    Value primary() {
        const Token& t = cur_.peek();
        switch (t.kind) {
        case TokKind::Number:
            cur_.next();
            return Value::number(static_cast<std::int64_t>(t.num));
        case TokKind::String: {
            auto bytes = string_bytes(t);
            if (bytes.empty() || bytes.size() > 2)
                throw AsmError("A41", "illegal string in expression (one or two characters allowed)", t.column);
            cur_.next();
            std::int64_t v = 0;
            for (auto b : bytes) v = (v << 8) | b;
            return Value::number(v);
        }
        case TokKind::Ident: {
            cur_.next();
            if (is_operator_keyword(t.text)) throw AsmError("A6", "syntax error: unexpected operator", t.column);
            std::optional<Value> v;
            try {
                v = ctx_.lookup(t.text);
            } catch (const AsmError& e) {
                if (e.column()) throw;
                throw AsmError(e.code(), e.message(), t.column);
            }
            if (v) {
                // A GPR's address is that of its short register number F0H + n.
                if (immediate_ && (v->type == VType::Gpr || v->type == VType::GprB)) {
                    v->num = 0xFE00 + 2 * (0xF0 + v->num);
                    v->type = VType::Sfr;
                }
                return *v;
            }
            if (ctx_.final_pass())
                throw AsmError("A42", "undefined identifier '" + std::string(t.text) + "'", t.column);
            return Value::unknown();
        }
        case TokKind::Op:
            if (t.text == "$") {
                cur_.next();
                Value v = Value::number(ctx_.location());
                v.rel = Rel::Addr;
                v.unsig = true;
                v.fixed = ctx_.location_fixed();
                v.section = ctx_.section_index();
                return v;
            }
            if (t.text == "(") {
                cur_.next();
                Value v = expression();
                cur_.expect_op(")");
                return v;
            }
            break;
        default:
            break;
        }
        throw AsmError("A6", "syntax error: expected an operand", t.column);
    }

    // base.bit with nothing else; nullopt if there is no '.'.
    std::optional<Value> explicit_bit() {
        Value base = primary();
        if (!cur_.peek().is_op(".")) return std::nullopt;
        return select_bit(base);
    }

private:
    TokenCursor& cur_;
    SymbolContext& ctx_;
    bool immediate_;

    static Value num(const Value& a, const Value& b, U r) {
        Value v = Value::number(static_cast<std::int64_t>(r));
        v.known = a.known && b.known;
        return v;
    }

    // Keil silently miscomputes "-label", "NOT label" and the same applied to
    // label differences, even in the linked program, so refuse them.
    static void reject_negated(const Value& a, int column) {
        if (a.rel == Rel::Addr || a.rel == Rel::Diff)
            throw AsmError("A63", "illegal relocatable expression: Keil A166 miscomputes a negated address", column);
    }

    // Operands of arithmetic must be plain numbers; SFRs act as their address.
    static void require_number(const Value& v, int column) {
        if (v.type != VType::Number && v.type != VType::Sfr)
            throw AsmError("A61", "illegal operand for this operator", column);
    }

    // Level 1: base.bit
    Value bit_select() {
        Value base = primary();
        if (!cur_.peek().is_op(".")) return base;
        return select_bit(base);
    }

    Value select_bit(const Value& base_in) {
        const Value base = resolved(base_in);
        const Token& dot = cur_.next();
        int pos_column = cur_.peek().column;
        // Keil accepts a signed position such as 0FE00H.-1.
        bool negative = cur_.peek().is_op("-");
        if (negative || cur_.peek().is_op("+")) cur_.next();
        const unsigned forward = ctx_.forward_references();
        Value pos = primary();
        if (negative) pos.num = -pos.num;
        if (pos.type != VType::Number) throw AsmError("A67", "absolute value (0-15) expected for bit position", pos_column);
        if (ctx_.forward_references() != forward)
            ctx_.soft_error(AsmError("A67", "absolute value (0-15) expected for bit position: forward reference", pos_column));
        Value r;
        r.type = VType::Bit;
        r.known = base.known && pos.known;
        if (!r.known) return r;
        BitAddr b = oc166::bit_select(base, pos.num, dot.column, pos_column);
        if (b.ext && !ctx_.has_esfr())
            throw AsmError("A68", "invalid base in bit address: extended SFRs need $MOD167", dot.column);
        return bit_value(b);
    }

    static Value bit_value(const BitAddr& b) {
        Value r;
        r.type = VType::Bit;
        r.num = b.off;
        r.bit = b.bit;
        r.ext = b.ext;
        return r;
    }

    // Level 2: SEG PAG SOF POF BOF DATAn DPPn:
    Value segment_ops() {
        const Token& t = cur_.peek();
        if (t.kind == TokKind::Ident) {
            if (int n = dpp_prefix_at(cur_); n >= 0) return dpp_prefix(n, t);
            if (int w = data_width(t); w > 0) return data_op(w, t);
            if (data_width(t) < 0) throw AsmError("A6", "syntax error: Keil A166 has DATA3, DATA4, DATA8 and DATA16 only", t.column);
            if (ieq(t.text, "BOF")) return bit_offset_op(t);
            int which = ieq(t.text, "SEG") ? 0 : ieq(t.text, "PAG") ? 1 : ieq(t.text, "SOF") ? 2 : ieq(t.text, "POF") ? 3 : -1;
            if (which >= 0) {
                cur_.next();
                Value a = resolved(segment_ops());
                require_number(a, t.column);
                U x = static_cast<U>(a.num);
                U r = which == 0 ? x >> 16 : which == 1 ? x >> 14 : which == 2 ? x & 0xFFFF : x & 0x3FFF;
                Value v = num(a, a, r);
                if (which == 0) v.rel = Rel::Seg;  // Keil treats every SEG value as relocatable
                return v;
            }
        }
        return bit_select();
    }

    // DPPn: selects the data page register for an address in segmented mode;
    // the operand binds like a unary operator (DPP2:HIGH x is DPP2:(HIGH x)).
    // Only + and - keep the prefix; other operators see the address.
    // Nonsegmented mode ignores the prefix.
    Value dpp_prefix(int n, const Token& t) {
        cur_.next();
        cur_.next();
        Value a = unary();
        if (a.type == VType::Gpr) {
            a.num = 0xFE00 + 2 * (0xF0 + a.num);
            a.type = VType::Sfr;
        }
        require_number(a, t.column);
        if (!ctx_.segmented()) {
            ctx_.warning(AsmError("A55", "'DPP' in nonsegmented mode ignored", t.column));
            return a;
        }
        if (a.ltype == LType::Near || a.ltype == LType::Far) {
            ctx_.soft_error(AsmError("A100", "illegal 'DPP' operand: a code label has no data page", t.column));
            throw AsmError("A74", "operand type mismatch", t.column);
        }
        a.type = VType::Number;
        a.dpp = static_cast<std::int8_t>(n);
        return a;
    }

    // DATAn masks a number to n bits and makes an immediate use an n-bit
    // form: the shortest one that holds n bits, else the longest one there
    // is. Its operand binds like a unary operator (DATA4 -1+20 is 0FH+20).
    // Arithmetic on the result drops the size; parentheses keep it.
    Value data_op(int width, const Token& t) {
        cur_.next();
        Value a = resolved(unary());
        if (a.type == VType::Sfr) {
            // Keil reports a register here but goes on with its address.
            ctx_.soft_error(AsmError("A104", "DATAn operator: incompatible operand", t.column));
            a.type = VType::Number;
        } else if (a.type != VType::Number) {
            throw AsmError("A104", "DATAn operator: incompatible operand", t.column);
        }
        // Relocatable addresses keep their value; the linker would truncate them.
        if (!a.relocatable()) {
            a.num &= (std::int64_t{1} << width) - 1;
            a.unsig = true;
        }
        a.data_bits = static_cast<std::uint8_t>(width);
        return a;
    }

    // BOF: the bit position (0..15) of a bit address, a 4-bit immediate.
    // It binds tighter than '.', so it takes a bit name, not word.bit.
    Value bit_offset_op(const Token& t) {
        cur_.next();
        Value a = primary();
        Value r = Value::number(0);
        r.data_bits = 4;
        r.unsig = true;
        if (!a.known) {
            r.known = false;  // a bit defined later
            return r;
        }
        if (a.type == VType::Bit && bit_word_address(a) >= 0) {
            r.num = a.bit;
            return r;
        }
        if (a.type == VType::Number && a.rel == Rel::None) {
            ctx_.soft_error(AsmError("A66", "value out of range: BOF needs a bit address", t.column));
            return r;
        }
        throw AsmError("A61", "illegal operand for this operator: BOF needs a bit address", t.column);
    }

    // Level 3: HIGH LOW BYTEn WORDn NOT unary + -
    Value unary() {
        const Token& t = cur_.peek();
        if (t.is_op("-") || t.is_op("+")) {
            cur_.next();
            Value a = unary();
            require_number(a, t.column);
            if (t.text == "+") return a;
            a = resolved(a);
            reject_negated(a, t.column);
            return num(a, a, U{0} - static_cast<U>(a.num));
        }
        if (t.kind == TokKind::Ident) {
            int byte = byte_word_index(t.text, "BYTE");
            int word = byte_word_index(t.text, "WORD");
            bool high = ieq(t.text, "HIGH"), low = ieq(t.text, "LOW"), bnot = ieq(t.text, "NOT");
            if (byte > 7 || (word >= 0 && (word > 6 || word % 2))) throw AsmError("A6", "syntax error", t.column);
            if (high || low || bnot || byte >= 0 || word >= 0) {
                cur_.next();
                Value a = resolved(unary());
                require_number(a, t.column);
                if (bnot) reject_negated(a, t.column);
                U x = static_cast<U>(a.num);
                U r = high ? (x >> 8) & 0xFF
                    : low  ? x & 0xFF
                    : bnot ? ~x
                    : byte >= 0 ? (x >> (8 * byte)) & 0xFF
                                : (x >> (8 * word)) & 0xFFFF;
                return num(a, a, r);
            }
        }
        return segment_ops();
    }

    template <typename Next, typename Match>
    Value binary_level(Next next, Match match) {
        Value a = (this->*next)();
        for (;;) {
            const Token& t = cur_.peek();
            int op = match(t);
            if (op < 0) return a;
            cur_.next();
            Value b = (this->*next)();
            if ((op == Add || op == Sub) && (a.type == VType::Bit || b.type == VType::Bit)) {
                a = bit_arithmetic(op, a, b, t.column);
                continue;
            }
            require_number(a, t.column);
            require_number(b, t.column);
            a = (op == Add || op == Sub) ? add_sub(op, a, b, t.column) : apply(op, resolved(a), resolved(b), t.column);
        }
    }

    // Keil accepts "bit + n" and "bit - n". It also accepts "n + bit" but
    // yields an unrelated bit, so that is an error here.
    static Value bit_arithmetic(int op, const Value& a, const Value& b, int column) {
        const Value& bit = a;
        const Value& n = b;
        if (a.type != VType::Bit || n.type != VType::Number)
            throw AsmError("A61", "illegal operand for this operator", column);
        if (!bit.known || !n.known) {
            Value r = bit;
            r.known = false;
            return r;
        }
        return bit_value(bit_offset(bit, op == Sub ? -n.num : n.num, column));
    }

    enum Op { Mul, Div, Mod, Add, Sub, Shl, Shr, And, Or, Xor, Eq, Ne, Lt, Le, Gt, Ge, Ult, Ule, Ugt, Uge };

    static Value with(Value v, Rel rel, bool unsig) {
        v.rel = rel;
        v.unsig = unsig;
        return v;
    }

    // Keil computes addresses and label differences modulo 2^61.
    static constexpr U kRelMask = (U{1} << 61) - 1;

    // Addition and subtraction follow the left operand: "L - L" is an
    // unsigned difference, "Diff - 3" is still one, but "1 - Diff" is signed.
    // In a section with a fixed address, "label + n" with n not zero (a
    // number or a label difference) is no longer relocatable; Keil treats it
    // like a label difference.
    // A DPPn: prefix, an SFR origin (for ASSUME DPPn:SYSTEM) and the section
    // of a section name carry over to the sum.
    static Value add_sub(int op, const Value& a, const Value& b, int column) {
        Value v = add_sub_value(op, a, b, column);
        v.dpp = a.dpp >= 0 ? a.dpp : b.dpp;
        if (a.rel == Rel::None && b.rel == Rel::None) {
            v.system = a.system || b.system || a.type == VType::Sfr || b.type == VType::Sfr;
            if (v.section == 0) v.section = a.section ? a.section : b.section;
        }
        return v;
    }

    static Value add_sub_value(int op, const Value& a, const Value& b, int column) {
        U x = static_cast<U>(a.num), y = static_cast<U>(b.num);
        const bool rel_left = a.rel == Rel::Addr || a.rel == Rel::Diff;
        if (b.rel == Rel::Diff && !rel_left && (y >> 60)) y |= ~kRelMask;  // sign-extend from 61 bits
        U r = op == Add ? x + y : x - y;
        Value v = num(a, b, r);
        auto rel = [&](Rel k) {
            v.num = static_cast<std::int64_t>(r & kRelMask);
            return with(v, k, true);
        };
        const bool zero = b.known && b.num == 0;
        auto addr = [&](const Value& l, const Value& n) {
            v.fixed = l.fixed;
            v.section = l.section;
            const bool offset = (n.rel == Rel::None || n.rel == Rel::Diff) && !(n.known && n.num == 0);
            if (n.known && n.num == 0 && (n.rel == Rel::None || n.rel == Rel::Diff))
                v.ltype = l.ltype;  // "label + 0" is still the label
            return rel(l.fixed && offset ? Rel::Diff : Rel::Addr);
        };
        if (a.rel == Rel::Addr) return op == Sub && b.rel == Rel::Addr ? rel(Rel::Diff) : addr(a, b);
        if (b.rel == Rel::Addr) {
            if (op == Add) return addr(b, a);
            // Keil gets "n - label" wrong but handles "(L1 - L0) - label".
            if (a.known && !a.unsig)
                throw AsmError("A63", "illegal relocatable expression: Keil A166 miscomputes 'number - address'", column);
            return with(v, Rel::None, true);
        }
        if (a.rel == Rel::Seg) return with(v, zero ? Rel::Seg : Rel::None, false);
        if (a.rel == Rel::Diff && b.rel != Rel::Seg) return rel(Rel::Diff);
        return with(v, Rel::None, a.unsig);
    }

    static Value apply(int op, const Value& a, const Value& b, int column) {
        U x = static_cast<U>(a.num), y = static_cast<U>(b.num);
        std::int64_t sx = a.num, sy = b.num;
        const bool known = a.known && b.known;
        const bool unsig = a.unsig || b.unsig;
        auto result = [&](U r) { return with(num(a, b, r), Rel::None, unsig); };
        auto truth = [&](bool r) { return num(a, b, r); };
        switch (op) {
        case Mul: return result(x * y);
        case Div:
        case Mod:
            if (!known) return result(0);
            if (y == 0) throw AsmError("A24", "division by zero", column);
            if (unsig) return result(op == Div ? x / y : x % y);
            if (sy == -1) return result(op == Div ? U{0} - x : 0);
            return result(static_cast<U>(op == Div ? sx / sy : sx % sy));
        case Shl: return result(y >= 64 ? 0 : x << y);
        case Shr: return result(y >= 64 ? 0 : x >> y);
        case And: return result(x & y);
        case Or: return result(x | y);
        case Xor: return result(x ^ y);
        case Eq: return truth(x == y);
        case Ne: return truth(x != y);
        case Lt: return truth(unsig ? x < y : sx < sy);
        case Le: return truth(unsig ? x <= y : sx <= sy);
        case Gt: return truth(unsig ? x > y : sx > sy);
        case Ge: return truth(unsig ? x >= y : sx >= sy);
        case Ult: return truth(x < y);
        case Ule: return truth(x <= y);
        case Ugt: return truth(x > y);
        case Uge: return truth(x >= y);
        }
        return truth(false);
    }

    static int match_any(const Token& t, std::initializer_list<std::pair<std::string_view, int>> table) {
        for (auto& [spelling, op] : table) {
            bool word = spelling[0] >= 'A' && spelling[0] <= 'Z';
            if (word ? (t.kind == TokKind::Ident && ieq(t.text, spelling)) : t.is_op(spelling)) return op;
        }
        return -1;
    }

    Value multiplicative() {
        return binary_level(&Evaluator::unary,
                            [](const Token& t) { return match_any(t, {{"*", Mul}, {"/", Div}, {"MOD", Mod}}); });
    }
    Value additive() {
        return binary_level(&Evaluator::multiplicative,
                            [](const Token& t) { return match_any(t, {{"+", Add}, {"-", Sub}}); });
    }
    Value shift() {
        return binary_level(&Evaluator::additive, [](const Token& t) {
            return match_any(t, {{"SHL", Shl}, {"<<", Shl}, {"SHR", Shr}, {">>", Shr}});
        });
    }
    Value bitwise() {
        return binary_level(&Evaluator::shift, [](const Token& t) {
            return match_any(t, {{"AND", And}, {"&", And}, {"OR", Or}, {"|", Or}, {"XOR", Xor}, {"^", Xor}});
        });
    }
    Value comparison() {
        return binary_level(&Evaluator::bitwise, [](const Token& t) {
            return match_any(t, {{"EQ", Eq}, {"==", Eq}, {"=", Eq}, {"NE", Ne}, {"!=", Ne}, {"<>", Ne}, {"LT", Lt},
                                 {"<", Lt},  {"LE", Le},  {"<=", Le}, {"GT", Gt}, {">", Gt},   {"GE", Ge},
                                 {">=", Ge}, {"ULT", Ult}, {"ULE", Ule}, {"UGT", Ugt}, {"UGE", Uge}});
        });
    }
};

}  // namespace

bool is_operator_keyword(std::string_view name) {
    for (auto k : kOperatorKeywords)
        if (ieq(name, k)) return true;
    return byte_word_index(name, "BYTE") >= 0 || byte_word_index(name, "WORD") >= 0;
}

int dpp_number(std::string_view name) {
    int n = byte_word_index(name, "DPP");
    return n >= 0 && n <= 3 ? n : -1;
}

int dpp_prefix_at(const TokenCursor& cur) {
    const Token& t = cur.peek();
    if (t.kind != TokKind::Ident || !cur.peek(1).is_op(":")) return -1;
    return dpp_number(t.text);
}

Value evaluate(TokenCursor& cur, SymbolContext& ctx) { return Evaluator(cur, ctx).expression(); }

Value evaluate_immediate(TokenCursor& cur, SymbolContext& ctx) {
    int column = cur.peek().column;
    Value v = Evaluator(cur, ctx, true).expression();
    if (v.type == VType::Sfr) {
        v.type = VType::Number;
        v.system = true;
    }
    if (v.type == VType::Cc) throw AsmError("A6", "syntax error: a condition code is not a value", column);
    if (v.type != VType::Number) throw AsmError("A70", "invalid type of expression", column);
    return v;
}

Value evaluate_primary(TokenCursor& cur, SymbolContext& ctx) { return Evaluator(cur, ctx).primary(); }

std::optional<Value> evaluate_bit_select(TokenCursor& cur, SymbolContext& ctx) {
    return Evaluator(cur, ctx).explicit_bit();
}

}  // namespace oc166
