// SPDX-License-Identifier: MIT
// Instruction operand syntax.
#pragma once

#include "expr.hpp"

#include <vector>

namespace oc166 {

enum class OpMode : std::uint8_t {
    Direct,   // expression: register, SFR, address, bit, condition code
    Imm,      // #expression
    Ind,      // [Rw]
    PostInc,  // [Rw+]
    PreDec,   // [-Rw]
    IndDisp,  // [Rw+#expression]
};

struct Operand {
    OpMode mode = OpMode::Direct;
    Value value;            // Direct/Imm: the expression; IndDisp: the displacement
    std::uint8_t reg = 0;   // indirect modes: pointer register number
    int column = 0;
};

// Parses comma-separated operands up to the end of the line.
std::vector<Operand> parse_operands(TokenCursor& cur, SymbolContext& ctx);

}  // namespace oc166
