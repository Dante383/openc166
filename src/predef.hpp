// SPDX-License-Identifier: MIT
// Names the assembler knows without any include file: general purpose
// registers, condition codes and, matching Keil A166, the CPU core SFRs.
// By default the 80C166 peripheral SFRs and bits are predefined too.
#pragma once

#include "isa/isa_types.hpp"
#include "value.hpp"

#include <optional>
#include <string_view>

namespace oc166 {

// Case-insensitive lookup of a predefined name. 'sys166' adds the 80C166
// peripheral SFRs and bits, which Keil predefines unless $MOD167 or
// $NOMOD166 is given.
std::optional<Value> predefined(std::string_view name, isa::Cpu cpu, bool sys166);

// GPRs and condition codes are reserved words; SFR and bit names are
// predefined symbols that the source may not redefine.
bool is_reserved_word(std::string_view name);

// A bit operand: the short bitoff byte and bit number 0..15.
struct BitAddr {
    std::uint8_t off = 0;
    std::uint8_t bit = 0;
    bool ext = false;  // extended SFR space (C167), needs an EXTR sequence
};

// The '.' operator. GPR and SFR bases take bit numbers 0..15. Like Keil, a
// numeric base counts bits linearly, so 0FD00H.17 is 0FD02H.1 and 0FD01H.3
// is 0FD00H.11; a raw bitoff number (80h..EFh) counts in words. Throws
// AsmError (A67, A69, A70, A103) if the result is not bit-addressable.
BitAddr bit_select(const Value& base, std::int64_t pos, int base_column, int pos_column);

// Keil allows "bit + n" and "bit - n" on a bit address of memory or an SFR.
BitAddr bit_offset(const Value& bit, std::int64_t n, int column);

// Word address of a bit address, or -1 for a GPR bit.
std::int64_t bit_word_address(const Value& bit);

// The bitoff byte of a word operand (BFLDL, BFLDH). Odd addresses name their
// word, as in Keil. Throws AsmError (A103) if the word is not bit-addressable.
BitAddr word_bitoff(const Value& v, int column);

}  // namespace oc166
