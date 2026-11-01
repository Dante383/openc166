// SPDX-License-Identifier: MIT
// Small helpers shared by the unit tests.
#pragma once

#include "assembler.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace test {

inline std::string hex(const std::vector<std::uint8_t>& bytes) {
    std::string s;
    char buf[3];
    for (auto b : bytes) {
        std::snprintf(buf, sizeof buf, "%02X", b);
        s += buf;
    }
    return s;
}

inline oc166::AssemblyResult run(std::string_view source, oc166::isa::Cpu cpu = oc166::isa::Cpu::C167) {
    oc166::AssembleOptions o;
    o.cpu = cpu;
    return oc166::assemble(source, o);
}

// Code bytes of a source line.
inline std::string code(const oc166::AssemblyResult& r, int line) {
    for (const auto& l : r.listing)
        if (l.line == line) return hex(l.bytes);
    return "?";
}

// Diagnostic codes of a source line, e.g. "A62W" or "A62W A110W".
inline std::string diags(const oc166::AssemblyResult& r, int line) {
    std::string s;
    for (const auto& d : r.diagnostics) {
        if (d.line != line) continue;
        if (!s.empty()) s += ' ';
        s += d.code + (d.severity == oc166::Severity::Error ? "E" : "W");
    }
    return s;
}

// Result of one statement inside a C167 code section at 0C000H:
// its code bytes followed by any diagnostic codes, e.g. "E6F1FFFF",
// "5C01 A62W" or "A57E".
inline std::string one(std::string_view statement, oc166::isa::Cpu cpu = oc166::isa::Cpu::C167) {
    std::string src = cpu == oc166::isa::Cpu::C167 ? "$MOD167\n" : "\n";
    src += "S SECTION CODE AT 0C000H\nP PROC NEAR\n";
    src += statement;
    src += "\nP ENDP\nS ENDS\n END\n";
    auto r = run(src, cpu);
    std::string c = code(r, 4), d = diags(r, 4);
    return c + (!c.empty() && !d.empty() ? " " : "") + d;
}

}  // namespace test
