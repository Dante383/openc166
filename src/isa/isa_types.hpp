// SPDX-License-Identifier: MIT
// Instruction-table types shared by the isagen generator and the assembler.
#pragma once

#include <cstdint>
#include <string_view>

namespace oc166::isa {

// Operand classes of the C166 instruction set (see c166.tbl).
enum class Cls : std::uint8_t {
    None,
    Rw,        // word GPR R0..R15
    Rb,        // byte GPR RL0..RH7
    Ind,       // [Rw]
    PostInc,   // [Rw+]
    PreDec,    // [-Rw]
    IndI,      // [Ri], R0..R3
    PostIncI,  // [Ri+], R0..R3
    IndDisp,   // [Rw+#d16]
    Reg,       // word register, 8-bit short address
    RegB,      // byte register, 8-bit short address
    Mem,       // 16-bit direct address
    BitOff,    // bit-addressable word
    BitAddr,   // bitoff.bit
    D3, D4, D8, D16, M8, Trap7, Irang2, Pag10, Seg8,
    Cc,        // condition code
    Rel,       // signed 8-bit word displacement
    Caddr,     // 16-bit code address
    Seg,       // code segment number
};

// Sub-field selectors used in encoding templates.
enum class Sub : std::uint8_t { Full, Lo, Hi, Off, Bit, Reg };

struct ClassInfo {
    Cls cls;
    std::string_view name;   // spelling in c166.tbl
    std::uint8_t width;      // natural width of a bare reference, 0 = sub-fields only
    bool has_lohi;           // .lo/.hi allowed
    bool has_offbit;         // .off/.bit allowed
    bool has_reg;            // .reg allowed
};

inline constexpr ClassInfo kClassInfo[] = {
    {Cls::None, "", 0, false, false, false},
    {Cls::Rw, "Rw", 4, false, false, false},
    {Cls::Rb, "Rb", 4, false, false, false},
    {Cls::Ind, "[Rw]", 4, false, false, false},
    {Cls::PostInc, "[Rw+]", 4, false, false, false},
    {Cls::PreDec, "[-Rw]", 4, false, false, false},
    {Cls::IndI, "[Ri]", 2, false, false, false},
    {Cls::PostIncI, "[Ri+]", 2, false, false, false},
    {Cls::IndDisp, "[Rw+#d16]", 0, true, false, true},
    {Cls::Reg, "reg", 8, false, false, false},
    {Cls::RegB, "regb", 8, false, false, false},
    {Cls::Mem, "mem", 0, true, false, false},
    {Cls::BitOff, "bitoff", 8, false, false, false},
    {Cls::BitAddr, "bitaddr", 0, false, true, false},
    {Cls::D3, "#d3", 3, false, false, false},
    {Cls::D4, "#d4", 4, false, false, false},
    {Cls::D8, "#d8", 8, false, false, false},
    {Cls::D16, "#d16", 0, true, false, false},
    {Cls::M8, "#m8", 8, false, false, false},
    {Cls::Trap7, "#trap7", 7, false, false, false},
    {Cls::Irang2, "#irang2", 2, false, false, false},
    {Cls::Pag10, "#pag10", 0, true, false, false},
    {Cls::Seg8, "#seg8", 8, false, false, false},
    {Cls::Cc, "cc", 4, false, false, false},
    {Cls::Rel, "rel", 8, false, false, false},
    {Cls::Caddr, "caddr", 0, true, false, false},
    {Cls::Seg, "seg", 8, false, false, false},
};

constexpr const ClassInfo& class_info(Cls c) { return kClassInfo[static_cast<int>(c)]; }

// Width in bits of a sub-field reference.
constexpr std::uint8_t sub_width(Cls c, Sub s) {
    switch (s) {
    case Sub::Full: return class_info(c).width;
    case Sub::Lo:
    case Sub::Hi:
    case Sub::Off: return 8;
    case Sub::Bit:
    case Sub::Reg: return 4;
    }
    return 0;
}

inline constexpr std::uint8_t kLiteral = 0xFF;

// One bit-group of a template byte: either a literal or (part of) an operand.
struct Part {
    std::uint8_t width;
    std::uint8_t operand;   // operand index, or kLiteral
    Sub sub;
    std::uint8_t literal;
};

struct ByteTemplate {
    std::uint8_t count;
    Part parts[4];
};

enum class Cpu : std::uint8_t { C166 = 0, C167 = 1 };

struct Form {
    std::string_view mnemonic;
    Cpu cpu;
    std::uint8_t size;
    std::uint8_t nops;
    Cls ops[3];
    ByteTemplate bytes[4];
    std::uint16_t line;      // line in c166.tbl, for diagnostics and tests
};

struct MnemonicEntry {
    std::string_view name;
    std::uint16_t first;
    std::uint16_t count;
};

struct AliasEntry {
    std::string_view alias;
    std::string_view target;
};

struct GenericEntry {
    std::string_view name;
    std::string_view word;
    std::string_view byte;
    std::string_view bit;    // empty if there is no bit variant
};

}  // namespace oc166::isa
