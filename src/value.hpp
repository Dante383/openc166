// SPDX-License-Identifier: MIT
// Typed values produced by expression evaluation.
#pragma once

#include <cstdint>

namespace oc166 {

enum class VType : std::uint8_t {
    Number,  // plain number or address
    Gpr,     // R0..R15; num = register number
    GprB,    // RL0..RH7; num = byte register number (RL0=0, RH0=1, ...)
    Sfr,     // special function register; num = 16-bit address
    Bit,     // bit address; num = encoded bitoff byte, bit = position
    Cc,      // condition code; num = 4-bit encoding
};

// How a number relates to the location of code or data. Keil keeps room for a
// linker fixup in relocatable immediates, so they never use the short 3/4-bit
// forms, and it treats label differences as unsigned 61-bit numbers.
enum class Rel : std::uint8_t {
    None,  // plain number
    Addr,  // label or '$', possibly plus or minus a number
    Seg,   // SEG of anything
    Diff,  // label - label, or a fixed address plus or minus a number
};

// Type of a label as Keil records it. JMP and CALL use it to choose between
// near and segmented (far) branches and to warn about jumps to data.
// "label + n" keeps the type while it is still an address; other arithmetic
// drops it. "type label" and "type PTR label" change it.
enum class LType : std::uint8_t { None, Near, Far, Byte, Word, Dword };

constexpr bool is_data_type(LType t) { return t == LType::Byte || t == LType::Word || t == LType::Dword; }

struct Value {
    VType type = VType::Number;
    bool known = true;     // false while a forward reference is unresolved
    bool ext = false;      // Bit: lies in the extended SFR space
    std::uint8_t bit = 0;  // Bit: bit position 0..15
    std::int64_t num = 0;
    Rel rel = Rel::None;
    bool unsig = false;  // range checks treat num as unsigned
    bool fixed = false;  // Addr: in a section with a fixed (AT) address
    LType ltype = LType::None;
    bool short_jump = false;  // SHORT operator: JMP/CALL must use the relative form
    // A type operator (SHORT, NEAR, FAR, ...) was applied to a forward
    // reference. Keil refuses to use such a value in arithmetic.
    bool typed_forward = false;
    std::uint16_t section = 0;  // Addr: index of the section that holds the address
    // Segmented mode: the DPPn: prefix (0..3) that selects the data page,
    // with num holding the page offset; -1 if none.
    std::int8_t dpp = -1;
    // Size class (3, 4, 8 or 16 bits) that a DATAn or BOF operator gave an
    // immediate; 0 if none.
    std::uint8_t data_bits = 0;
    // Derived from an SFR address, so ASSUME DPPn:SYSTEM applies to it.
    bool system = false;
    // Index of the section without AT whose start address the value depends
    // on (a label in it, or PAG, POF, SOF, SEG, HIGH or LOW of one, plus or
    // minus a number); 0 if none. Keil leaves such values to the linker.
    // External symbols (EXTERN) use the numbers from kExternBase on, one per
    // symbol, as their movable value and, for addresses, as their section.
    // Groups (DGROUP, CGROUP) likewise use the numbers from kGroupBase on.
    std::uint16_t movable = 0;

    static constexpr std::uint16_t kGroupBase = 0x4000;
    static constexpr std::uint16_t kExternBase = 0x8000;

    bool relocatable() const { return rel == Rel::Addr || rel == Rel::Seg || movable != 0; }
    // The name of a group, plus or minus a number.
    bool group() const { return movable >= kGroupBase && movable < kExternBase; }
    // Derived from an external symbol: the linker fills it in.
    bool external() const { return movable >= kExternBase; }

    // The 16-bit address a DPPn: prefixed value stands for: the DPP number in
    // bits 15..14 above the page offset.
    std::int64_t dpp_address() const { return (std::int64_t{dpp} << 14) | (num & 0x3FFF); }

    static Value number(std::int64_t n) { return Value{VType::Number, true, false, 0, n}; }
    static Value unknown() { return Value{VType::Number, false, false, 0, 0}; }
};

// True if v can be stored in 'width' bits without losing information: as an
// unsigned number for unsigned values, otherwise zero- or sign-extended.
constexpr bool fits_width(const Value& v, int width) {
    if (width >= 64) return true;
    if (v.unsig) return (static_cast<std::uint64_t>(v.num) >> width) == 0;
    std::int64_t hi = v.num >> width;
    return hi == 0 || hi == -1;
}

// True if L166 accepts n, taken as a signed 32-bit number, in a fixup field
// of 'width' bits: the bits above the field must be all zeros or all ones.
constexpr bool link_fits(std::int64_t n, int width) {
    const std::int64_t n32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(n & 0xFFFFFFFF));
    const std::int64_t hi = n32 >> width;
    return hi == 0 || hi == -1;
}

// Extended SFRs (C167) live at F000h..F1FEh, standard SFRs at FE00h..FFFEh.
constexpr bool is_esfr_address(std::int64_t a) { return a >= 0xF000 && a <= 0xF1FF; }

}  // namespace oc166
