// SPDX-License-Identifier: MIT
// Lookup into the generated C166 instruction table.
#pragma once

#include "isa/isa_types.hpp"

#include <span>
#include <string_view>

namespace oc166::isa {

// Every hardware instruction form, grouped by mnemonic.
std::span<const Form> all_forms();

// Hardware forms of an upper-case mnemonic, in order of preference.
// Empty if the name is not a hardware mnemonic.
std::span<const Form> forms(std::string_view mnemonic);

// Hardware mnemonic that an alias (e.g. ADDW) stands for, or empty.
std::string_view alias_target(std::string_view mnemonic);

// Keil software instruction (e.g. MOV selecting MOV/MOVB/BMOV), or nullptr.
const GenericEntry* generic(std::string_view mnemonic);

// Keil's JMP and CALL software instructions, which the assembler resolves to
// a relative, absolute, indirect or segmented branch.
bool is_branch(std::string_view mnemonic);

// True for hardware mnemonics, aliases and software instructions.
bool is_mnemonic(std::string_view mnemonic);

}  // namespace oc166::isa
