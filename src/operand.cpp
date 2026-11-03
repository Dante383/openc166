// SPDX-License-Identifier: MIT
#include "operand.hpp"

namespace oc166 {
namespace {

std::uint8_t pointer_register(TokenCursor& cur, SymbolContext& ctx) {
    int column = cur.peek().column;
    Value v = evaluate_primary(cur, ctx);
    if (v.type != VType::Gpr) throw AsmError("A6", "syntax error: word register expected", column);
    return static_cast<std::uint8_t>(v.num);
}

Operand parse_operand(TokenCursor& cur, SymbolContext& ctx) {
    Operand op;
    op.column = cur.peek().column;
    if (cur.accept_op("#")) {
        op.mode = OpMode::Imm;
        op.value = evaluate_immediate(cur, ctx);
        return op;
    }
    if (cur.accept_op("[")) {
        if (cur.accept_op("-")) {
            op.mode = OpMode::PreDec;
            op.reg = pointer_register(cur, ctx);
        } else {
            op.reg = pointer_register(cur, ctx);
            if (cur.accept_op("+")) {
                if (cur.accept_op("#")) {
                    op.mode = OpMode::IndDisp;
                    op.value = evaluate_immediate(cur, ctx);
                    if (!cur.accept_op("]")) throw AsmError("A58", "']' expected", cur.peek().column);
                    return op;
                } else {
                    op.mode = OpMode::PostInc;
                }
            } else {
                op.mode = OpMode::Ind;
            }
        }
        cur.expect_op("]");
        return op;
    }
    op.mode = OpMode::Direct;
    op.unchecked = !ctx.segmented() && dpp_prefix_at(cur) >= 0;
    op.value = evaluate(cur, ctx);
    return op;
}

}  // namespace

std::vector<Operand> parse_operands(TokenCursor& cur, SymbolContext& ctx) {
    std::vector<Operand> ops;
    if (cur.at_end()) return ops;
    for (;;) {
        ops.push_back(parse_operand(cur, ctx));
        if (cur.at_end()) return ops;
        if (!cur.accept_op(",")) throw AsmError("A6", "syntax error: expected ',' or end of line", cur.peek().column);
    }
}

}  // namespace oc166
