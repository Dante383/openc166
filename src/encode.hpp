// SPDX-License-Identifier: MIT
// Instruction selection and encoding.
#pragma once

#include "diag.hpp"
#include "isa/isa.hpp"
#include "operand.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace oc166 {

// ASSUME DPPn:SYSTEM; other ASSUME targets are section indices or group
// numbers (Value::kGroupBase on).
constexpr int kAssumeSystem = -2;

struct EncodeContext {
    isa::Cpu cpu = isa::Cpu::C166;
    std::int64_t pc = 0;   // address of the instruction
    bool ext_sfr = false;  // inside an EXTR/EXTPR/EXTSR range
    // Segmented mode: memory operands, immediates and displacements of an
    // address get the page of the DPP that ASSUME ties to its section (or to
    // SYSTEM for SFRs). assume[n] is a section index, a group number,
    // kAssumeSystem or -1. assume_key maps the section of a value to what
    // ASSUME ties: the group of a section in one, else the section itself.
    bool segmented = false;
    std::array<int, 4> assume{-1, -1, -1, -1};
    std::function<int(int)> assume_key;
    bool ext_page = false;  // inside an EXTP/EXTPR range: the page is explicit
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
// register and bit addresses to the extended SFR space, and 'page' for EXTP
// and EXTPR, which give the data page of long addresses.
struct ExtSequence {
    int count = 0;  // 0 if the instruction does not start a sequence
    bool esfr = false;
    bool page = false;
};
ExtSequence ext_sequence(const Encoding& e);

}  // namespace oc166
