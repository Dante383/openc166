// SPDX-License-Identifier: MIT
#include "isa/isa.hpp"

#include <algorithm>

namespace oc166::isa {

#include "openc166/isa_table.inc"

std::span<const Form> all_forms() { return kForms; }

std::span<const Form> forms(std::string_view mnemonic) {
    const auto* end = std::end(kMnemonics);
    const auto* it = std::lower_bound(std::begin(kMnemonics), end, mnemonic,
                                      [](const MnemonicEntry& e, std::string_view n) { return e.name < n; });
    if (it == end || it->name != mnemonic) return {};
    return std::span<const Form>(kForms).subspan(it->first, it->count);
}

std::string_view alias_target(std::string_view mnemonic) {
    for (const auto& a : kAliases)
        if (a.alias == mnemonic) return a.target;
    return {};
}

const GenericEntry* generic(std::string_view mnemonic) {
    for (const auto& g : kGenerics)
        if (g.name == mnemonic) return &g;
    return nullptr;
}

bool is_branch(std::string_view mnemonic) { return mnemonic == "JMP" || mnemonic == "CALL"; }

bool is_mnemonic(std::string_view mnemonic) {
    return !forms(mnemonic).empty() || !alias_target(mnemonic).empty() || generic(mnemonic) != nullptr ||
           is_branch(mnemonic);
}

}  // namespace oc166::isa
