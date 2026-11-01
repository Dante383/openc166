// SPDX-License-Identifier: MIT
// Instruction selection and encoding.
#pragma once

#include "diag.hpp"
#include "isa/isa.hpp"
#include "operand.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace oc166 {

struct EncodeContext {
    isa::Cpu cpu = isa::Cpu::C166;
    std::int64_t pc = 0;   // address of the instruction
    bool ext_sfr = false;  // inside an EXTR/EXTPR/EXTSR range
};

struct Encoding {
    std::array<std::uint8_t, 4> bytes{};
    std::uint8_t size = 0;
    const isa::Form* form = nullptr;
};

// Encodes an instruction. The mnemonic must be upper case; Keil software
// instructions (MOV selecting MOV/MOVB/BMOV, ...) and aliases are resolved
// here. Warnings and recoverable errors go to the reporter; throws AsmError
// if no instruction form fits the operands.
Encoding encode(std::string_view mnemonic, const std::vector<Operand>& ops, const EncodeContext& ctx,
                Reporter& rep);

// Range of an ATOMIC or EXT* instruction: it covers the next 'count'
// instructions; 'esfr' is set for EXTR, EXTPR and EXTSR, which switch short
// register and bit addresses to the extended SFR space.
struct ExtSequence {
    int count = 0;  // 0 if the instruction does not start a sequence
    bool esfr = false;
};
ExtSequence ext_sequence(const Encoding& e);

}  // namespace oc166
