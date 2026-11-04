// SPDX-License-Identifier: MIT
// A166 expression evaluation with Keil operator precedence:
//   ()  >  .  >  SEG PAG SOF POF BOF DATAn DPPn:  >  HIGH LOW BYTEn WORDn NOT unary+-
//   >  * / MOD  >  + -  >  SHL SHR << >>  >  AND OR XOR & | ^
//   >  EQ NE LT LE GT GE ULT ULE UGT UGE == != < <= > >=
//   >  SHORT, type PTR (prefixes that apply to the whole rest)
// Arithmetic is 64-bit; comparisons yield 1 (true) or 0 (false).
#pragma once

#include "diag.hpp"
#include "lexer.hpp"
#include "value.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace oc166 {

// Symbol environment seen by expressions.
class SymbolContext {
public:
    virtual ~SymbolContext() = default;
    // Value of a symbol, or nullopt if it is (not yet) defined.
    virtual std::optional<Value> lookup(std::string_view name) = 0;
    // Value of '$', the current location counter.
    virtual std::int64_t location() const = 0;
    // True if the current section has a fixed (AT) address.
    virtual bool location_fixed() const = 0;
    // Index of the current section; addresses remember where they lie.
    virtual std::uint16_t section_index() const { return 0; }
    // True if extended SFRs exist (C167); the C166 has none.
    virtual bool has_esfr() const { return true; }
    // Number of lookups so far that found a symbol defined later in the source.
    virtual unsigned forward_references() const { return 0; }
    // Reports an error after which evaluation goes on, as Keil does; contexts
    // without a diagnostics sink make it fatal.
    virtual void soft_error(const AsmError& e) { throw e; }
    // Reports a warning; contexts without a diagnostics sink drop it.
    virtual void warning(const AsmError& w) { (void)w; }
    // True in Keil's segmented mode ($SEGMENTED), where DPPn: prefixes and
    // ASSUME select data pages. Nonsegmented mode ignores both.
    virtual bool segmented() const { return false; }
    // In the final pass undefined symbols are errors; before it they are
    // unknown values that later passes resolve.
    virtual bool final_pass() const = 0;
    // An expression uses an EXTERN symbol (Value::external()) at 'column'.
    virtual void external_used(const Value& v, int column) {
        (void)v;
        (void)column;
    }
};

// Cursor over the tokens of one line.
class TokenCursor {
public:
    explicit TokenCursor(const std::vector<Token>& tokens, std::size_t pos = 0) : toks_(tokens), pos_(pos) {}
    const Token& peek(std::size_t ahead = 0) const {
        std::size_t i = pos_ + ahead;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    const Token& next() {
        const Token& t = peek();
        if (pos_ < toks_.size() - 1) ++pos_;
        return t;
    }
    bool at_end() const { return peek().kind == TokKind::End; }
    bool accept_op(std::string_view op) {
        if (!peek().is_op(op)) return false;
        next();
        return true;
    }
    bool accept_keyword(std::string_view upper) {
        if (peek().kind != TokKind::Ident || !ieq(peek().text, upper)) return false;
        next();
        return true;
    }
    void expect_op(std::string_view op) {
        if (accept_op(op)) return;
        if (op == ")") throw AsmError("A22", "')' expected", peek().column);
        throw AsmError("A6", "syntax error: expected '" + std::string(op) + "'", peek().column);
    }
    std::size_t position() const { return pos_; }

private:
    const std::vector<Token>& toks_;
    std::size_t pos_;
};

// Parses and evaluates one expression. Throws AsmError on errors.
Value evaluate(TokenCursor& cur, SymbolContext& ctx);

// Evaluates the expression of an immediate (#expr) or a displacement
// ([Rw+#expr]). Like Keil, register names stand for their 16-bit address here.
Value evaluate_immediate(TokenCursor& cur, SymbolContext& ctx);

// Evaluates a single register name or symbol (no operators), as used inside
// indirect operands such as [R1+].
Value evaluate_primary(TokenCursor& cur, SymbolContext& ctx);

// Evaluates a bit address written as base.bit, without further operators,
// as DEFB requires. Returns nullopt (consuming only the base) if there is no
// '.'.
std::optional<Value> evaluate_bit_select(TokenCursor& cur, SymbolContext& ctx);

// True if the identifier is an expression operator keyword (AND, SHL, ...).
bool is_operator_keyword(std::string_view name);

// DPP register number (0..3) if the name is DPP0..DPP3, otherwise -1.
int dpp_number(std::string_view name);

// DPP register number if the cursor is at a "DPPn :" prefix, otherwise -1.
int dpp_prefix_at(const TokenCursor& cur);

}  // namespace oc166
