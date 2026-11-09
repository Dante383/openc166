// SPDX-License-Identifier: MIT
// Disassembler for the C166/C167, driven by the same instruction table as
// the encoder (src/isa/c166.tbl), so the two cannot disagree.
#pragma once

#include "isa/isa_types.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace oc166::dis {

// One decoded operand. What 'value' and 'aux' hold depends on the class:
//   Rw Rb [Rw] [Rw+] [-Rw] [Ri] [Ri+]  value = register number
//   [Rw+#d16]                          value = displacement, aux = register
//   reg regb bitoff                    value = 8-bit short address
//   bitaddr                            value = bitoff byte, aux = bit 0..15
//   mem caddr #d16 #pag10 ...          value = the number
//   #irang2                            value = the count as written (1..4)
//   cc                                 value = 4-bit condition code
//   rel                                value = signed word displacement
struct Operand {
    isa::Cls cls = isa::Cls::None;
    std::int32_t value = 0;
    std::uint8_t aux = 0;
};

struct Insn {
    const isa::Form* form = nullptr;
    std::uint8_t size = 0;
    std::uint8_t nops = 0;
    Operand ops[3];
    std::uint8_t bytes[4] = {};

    std::string_view mnemonic() const { return form ? form->mnemonic : std::string_view{}; }
};

// Decodes the instruction at 'bytes' ('avail' bytes are readable). Returns
// std::nullopt if the bytes are not a valid instruction for 'cpu'.
std::optional<Insn> decode(const std::uint8_t* bytes, std::size_t avail, isa::Cpu cpu = isa::Cpu::C167);

// The state that an EXTR, EXTP, EXTS (or their R forms) or ATOMIC sets up
// for the instructions after it.
struct ExtState {
    int left = 0;           // instructions still covered
    bool esfr = false;      // short register addresses select the ESFR space
    enum class Kind : std::uint8_t { None, Page, Seg } kind = Kind::None;
    std::uint32_t value = 0;  // page or segment number, if not taken from a register
    int reg = -1;            // the GPR that holds the page or segment, or -1
    bool atomic = false;

    // Moves on past 'insn', which was decoded in this state.
    void advance(const Insn& insn);
};

// The address a short (8-bit) register address stands for: a GPR (as
// 0xF0..0xFF, unchanged), an SFR at 0FE00H + 2 * r, or with 'esfr' an ESFR
// at 0F000H + 2 * r.
std::uint32_t reg_address(std::uint8_t r, bool esfr);
// The word address of a bitoff byte: bit-addressable RAM at 0FD00H, SFRs at
// 0FF00H (ESFRs at 0F100H with 'esfr'), or 0xF0..0xFF for a GPR.
std::uint32_t bitoff_address(std::uint8_t q, bool esfr);

struct Names {
    // Name of the SFR or ESFR at 'address', or empty. 'byte' is set for
    // byte operands.
    std::function<std::string(std::uint32_t address, bool byte)> sfr;
    // Name of the bit 'bit' of the word at 'address' (an SFR or bit-
    // addressable RAM), or empty.
    std::function<std::string(std::uint32_t address, int bit)> bit;
    // Name of a code address (24-bit), or empty.
    std::function<std::string(std::uint32_t address)> code;
    // Name of a data address (16-bit, as in the instruction), or empty.
    std::function<std::string(std::uint32_t address)> data;
};

// SFR and bit names of the C167 (the core registers and REG167.INC, plus
// the C167CS registers that file lacks).
const Names& c167_names();

struct FormatOptions {
    // Text that openc166 assembles back to exactly the same bytes (in a
    // $SEGMENTED module at the same address): long immediate forms get a
    // DATAn operator, and every operand is a number or a name from 'names'.
    bool reassemble = false;
    bool upper = true;  // mnemonics and register names in upper case
};

// The instruction as assembly text, e.g. "MOV     R4, #0BEEFH". 'address'
// is where it lies (24-bit), for relative and absolute branch targets.
std::string format(const Insn& insn, std::uint32_t address, const ExtState& ext, const Names& names,
                   const FormatOptions& options = {});

// A number in A166 syntax: decimal below 10, else hex with a leading 0 if
// needed and an H suffix.
std::string hex(std::uint32_t v);

// The target of a relative branch (rel) or absolute branch (caddr) of
// 'insn' at 'address', or std::nullopt if it has none. Indirect branches
// (JMPI, CALLI) and returns have no static target.
std::optional<std::uint32_t> branch_target(const Insn& insn, std::uint32_t address);

// A whole memory image as an openc166 source that assembles back to the
// same bytes: one CODE section per 64 KB segment, $SEGMENTED, DEFR names
// for the SFRs used. Bytes that do not decode, or whose text would not
// assemble back to the same bytes (checked by assembling the result), are
// written as DB.
struct ModuleText {
    std::string text;
    std::size_t instructions = 0;  // written as instructions
    std::size_t data_words = 0;    // DB lines for bytes that do not decode
    std::size_t fallbacks = 0;     // decoded, but DB because the text does not reassemble exactly
    std::vector<std::uint32_t> fallback_addresses;
};
ModuleText disassemble_module(const std::uint8_t* bytes, std::size_t size, std::uint32_t base,
                              isa::Cpu cpu = isa::Cpu::C167);

}  // namespace oc166::dis
