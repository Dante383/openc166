// SPDX-License-Identifier: MIT
// SFR and bit addresses are hardware facts taken from the Infineon 80C166
// and C167 data sheets and user manuals.
#include "predef.hpp"

#include "diag.hpp"

#include <cctype>
#include <string>
#include <unordered_map>

namespace oc166 {
namespace {

struct Sfr {
    const char* name;
    std::uint16_t addr;
};

struct BitDef {
    const char* name;
    std::uint16_t word;  // SFR address
    std::uint8_t pos;
};

// Keil A166 predefines CPU core registers in every mode. In 80C166 mode (no
// $MOD167) it also predefines the 80C166 peripherals; C167 sources include a
// register file instead, so those names must stay free for it.
constexpr Sfr kCoreSfrs[] = {
    {"DPP0", 0xFE00}, {"DPP1", 0xFE02}, {"DPP2", 0xFE04}, {"DPP3", 0xFE06},  {"MDH", 0xFE0C},
    {"MDL", 0xFE0E},  {"CP", 0xFE10},   {"PECC0", 0xFEC0}, {"PECC1", 0xFEC2}, {"PECC2", 0xFEC4},
    {"PECC3", 0xFEC6}, {"PECC4", 0xFEC8}, {"PECC5", 0xFECA}, {"PECC6", 0xFECC}, {"PECC7", 0xFECE},
    {"MDC", 0xFF0E},  {"PSW", 0xFF10},  {"ZEROS", 0xFF1C}, {"ONES", 0xFF1E},
};

constexpr BitDef kCoreBits[] = {
    {"N", 0xFF10, 0},     {"C", 0xFF10, 1},    {"V", 0xFF10, 2},     {"Z", 0xFF10, 3},
    {"E", 0xFF10, 4},     {"MULIP", 0xFF10, 5}, {"USR0", 0xFF10, 6}, {"HLDEN", 0xFF10, 10},
    {"IEN", 0xFF10, 11},  {"MDRIU", 0xFF0E, 4},
};

constexpr Sfr k80C166Sfrs[] = {
    {"CSP", 0xFE08},    {"SP", 0xFE12},     {"STKOV", 0xFE14},  {"STKUN", 0xFE16},  {"ADDRSEL1", 0xFE18},
    {"T2", 0xFE40},     {"T3", 0xFE42},     {"T4", 0xFE44},     {"T5", 0xFE46},     {"T6", 0xFE48},
    {"CAPREL", 0xFE4A}, {"T0", 0xFE50},     {"T1", 0xFE52},     {"T0REL", 0xFE54},  {"T1REL", 0xFE56},
    {"CC0", 0xFE80},    {"CC1", 0xFE82},    {"CC2", 0xFE84},    {"CC3", 0xFE86},    {"CC4", 0xFE88},
    {"CC5", 0xFE8A},    {"CC6", 0xFE8C},    {"CC7", 0xFE8E},    {"CC8", 0xFE90},    {"CC9", 0xFE92},
    {"CC10", 0xFE94},   {"CC11", 0xFE96},   {"CC12", 0xFE98},   {"CC13", 0xFE9A},   {"CC14", 0xFE9C},
    {"CC15", 0xFE9E},   {"ADDAT", 0xFEA0},  {"WDT", 0xFEAE},    {"S0TBUF", 0xFEB0}, {"S0RBUF", 0xFEB2},
    {"S0BG", 0xFEB4},   {"S1TBUF", 0xFEB8}, {"S1RBUF", 0xFEBA}, {"S1BG", 0xFEBC},   {"P0", 0xFF00},
    {"DP0", 0xFF02},    {"P1", 0xFF04},     {"DP1", 0xFF06},    {"P4", 0xFF08},     {"DP4", 0xFF0A},
    {"SYSCON", 0xFF0C}, {"T2CON", 0xFF40},  {"T3CON", 0xFF42},  {"T4CON", 0xFF44},  {"T5CON", 0xFF46},
    {"T6CON", 0xFF48},  {"T01CON", 0xFF50}, {"CCM0", 0xFF52},   {"CCM1", 0xFF54},   {"CCM2", 0xFF56},
    {"CCM3", 0xFF58},   {"T2IC", 0xFF60},   {"T3IC", 0xFF62},   {"T4IC", 0xFF64},   {"T5IC", 0xFF66},
    {"T6IC", 0xFF68},   {"CRIC", 0xFF6A},   {"S0TIC", 0xFF6C},  {"S0RIC", 0xFF6E},  {"S0EIC", 0xFF70},
    {"S1TIC", 0xFF72},  {"S1RIC", 0xFF74},  {"S1EIC", 0xFF76},  {"CC0IC", 0xFF78},  {"CC1IC", 0xFF7A},
    {"CC2IC", 0xFF7C},  {"CC3IC", 0xFF7E},  {"CC4IC", 0xFF80},  {"CC5IC", 0xFF82},  {"CC6IC", 0xFF84},
    {"CC7IC", 0xFF86},  {"CC8IC", 0xFF88},  {"CC9IC", 0xFF8A},  {"CC10IC", 0xFF8C}, {"CC11IC", 0xFF8E},
    {"CC12IC", 0xFF90}, {"CC13IC", 0xFF92}, {"CC14IC", 0xFF94}, {"CC15IC", 0xFF96}, {"ADCIC", 0xFF98},
    {"ADEIC", 0xFF9A},  {"T0IC", 0xFF9C},   {"T1IC", 0xFF9E},   {"ADCON", 0xFFA0},  {"P5", 0xFFA2},
    {"TFR", 0xFFAC},    {"WDTCON", 0xFFAE}, {"S0CON", 0xFFB0},  {"S1CON", 0xFFB8},  {"P2", 0xFFC0},
    {"DP2", 0xFFC2},    {"P3", 0xFFC4},     {"DP3", 0xFFC6},
};

// Multi-bit fields (e.g. BTYP, T2I) name their lowest bit, as in Keil.
constexpr BitDef k80C166Bits[] = {
    {"ILLBUS", 0xFFAC, 0},  {"ILLINA", 0xFFAC, 1},  {"ILLOPA", 0xFFAC, 2},  {"PRTFLT", 0xFFAC, 3},
    {"UNDOPC", 0xFFAC, 7},  {"STKUF", 0xFFAC, 13},  {"STKOF", 0xFFAC, 14},  {"NMI", 0xFFAC, 15},
    {"MCTC", 0xFF0C, 0},    {"RWDC", 0xFF0C, 4},    {"MTTC", 0xFF0C, 5},    {"BTYP", 0xFF0C, 6},
    {"CLKEN", 0xFF0C, 8},   {"BYTDIS", 0xFF0C, 9},  {"SGTDIS", 0xFF0C, 11}, {"RDYEN", 0xFF0C, 12},
    {"WDTIN", 0xFFAE, 0},   {"WDTR", 0xFFAE, 1},
    {"ADCH", 0xFFA0, 0},    {"ADM", 0xFFA0, 4},     {"ADST", 0xFFA0, 7},    {"ADBSY", 0xFFA0, 8},
    {"S0M", 0xFFB0, 0},     {"S0STP", 0xFFB0, 3},   {"S0REN", 0xFFB0, 4},   {"S0PEN", 0xFFB0, 5},
    {"S0FEN", 0xFFB0, 6},   {"S0OEN", 0xFFB0, 7},   {"S0PE", 0xFFB0, 8},    {"S0FE", 0xFFB0, 9},
    {"S0OE", 0xFFB0, 10},   {"S0R", 0xFFB0, 15},
    {"S1M", 0xFFB8, 0},     {"S1STP", 0xFFB8, 3},   {"S1REN", 0xFFB8, 4},   {"S1PEN", 0xFFB8, 5},
    {"S1FEN", 0xFFB8, 6},   {"S1OEN", 0xFFB8, 7},   {"S1PE", 0xFFB8, 8},    {"S1FE", 0xFFB8, 9},
    {"S1OE", 0xFFB8, 10},   {"S1R", 0xFFB8, 15},
    {"T0M", 0xFF50, 3},     {"T0R", 0xFF50, 6},     {"T1M", 0xFF50, 11},    {"T1R", 0xFF50, 14},
    {"T2I", 0xFF40, 0},     {"T2M", 0xFF40, 3},     {"T2R", 0xFF40, 6},     {"T2UD", 0xFF40, 7},
    {"T3I", 0xFF42, 0},     {"T3M", 0xFF42, 3},     {"T3R", 0xFF42, 6},     {"T3UD", 0xFF42, 7},
    {"T3UDE", 0xFF42, 8},   {"T3OE", 0xFF42, 9},    {"T3OTL", 0xFF42, 10},
    {"T4I", 0xFF44, 0},     {"T4M", 0xFF44, 3},     {"T4R", 0xFF44, 6},     {"T4UD", 0xFF44, 7},
    {"T5I", 0xFF46, 0},     {"T5M", 0xFF46, 3},     {"T5R", 0xFF46, 6},     {"T5UD", 0xFF46, 7},
    {"CI", 0xFF46, 12},     {"T5CLR", 0xFF46, 14},  {"T5SC", 0xFF46, 15},
    {"T6I", 0xFF48, 0},     {"T6R", 0xFF48, 6},     {"T6UD", 0xFF48, 7},    {"T6OE", 0xFF48, 9},
    {"T6OTL", 0xFF48, 10},  {"T6SR", 0xFF48, 15},
};

// 80C166 interrupt control registers: xxIR is bit 7, xxIE bit 6.
constexpr Sfr k80C166IntCtl[] = {
    {"T2", 0xFF60},  {"T3", 0xFF62},  {"T4", 0xFF64},  {"T5", 0xFF66},   {"T6", 0xFF68},
    {"CR", 0xFF6A},  {"S0T", 0xFF6C}, {"S0R", 0xFF6E}, {"S0E", 0xFF70},  {"S1T", 0xFF72},
    {"S1R", 0xFF74}, {"S1E", 0xFF76}, {"CC0", 0xFF78}, {"CC1", 0xFF7A},  {"CC2", 0xFF7C},
    {"CC3", 0xFF7E}, {"CC4", 0xFF80}, {"CC5", 0xFF82}, {"CC6", 0xFF84},  {"CC7", 0xFF86},
    {"CC8", 0xFF88}, {"CC9", 0xFF8A}, {"CC10", 0xFF8C}, {"CC11", 0xFF8E}, {"CC12", 0xFF90},
    {"CC13", 0xFF92}, {"CC14", 0xFF94}, {"CC15", 0xFF96}, {"ADC", 0xFF98}, {"ADE", 0xFF9A},
    {"T0", 0xFF9C},  {"T1", 0xFF9E},
};

struct CcDef {
    const char* name;
    std::uint8_t code;
};

constexpr CcDef kConditions[] = {
    {"CC_UC", 0x0},  {"CC_NET", 0x1}, {"CC_Z", 0x2},   {"CC_EQ", 0x2},  {"CC_NZ", 0x3},
    {"CC_NE", 0x3},  {"CC_V", 0x4},   {"CC_NV", 0x5},  {"CC_N", 0x6},   {"CC_NN", 0x7},
    {"CC_C", 0x8},   {"CC_ULT", 0x8}, {"CC_NC", 0x9},  {"CC_UGE", 0x9}, {"CC_SGT", 0xA},
    {"CC_SLE", 0xB}, {"CC_SLT", 0xC}, {"CC_SGE", 0xD}, {"CC_UGT", 0xE}, {"CC_ULE", 0xF},
};

Value make_bit(std::uint16_t word, std::uint8_t pos) {
    Value v;
    v.type = VType::Bit;
    v.num = 0x80 + (word - 0xFF00) / 2;
    v.bit = pos;
    return v;
}

using Table = std::unordered_map<std::string, Value>;

Value make_sfr(std::uint16_t addr) {
    Value v;
    v.type = VType::Sfr;
    v.num = addr;
    return v;
}

Table build(isa::Cpu cpu, bool sys166) {
    Table m;
    for (int i = 0; i < 16; ++i) {
        Value r;
        r.type = VType::Gpr;
        r.num = i;
        m["R" + std::to_string(i)] = r;
    }
    for (int i = 0; i < 8; ++i) {
        Value r;
        r.type = VType::GprB;
        r.num = 2 * i;
        m["RL" + std::to_string(i)] = r;
        r.num = 2 * i + 1;
        m["RH" + std::to_string(i)] = r;
    }
    for (const auto& c : kConditions) {
        Value v;
        v.type = VType::Cc;
        v.num = c.code;
        m[c.name] = v;
    }
    for (const auto& s : kCoreSfrs) m[s.name] = make_sfr(s.addr);
    for (const auto& b : kCoreBits) m[b.name] = make_bit(b.word, b.pos);
    // PEC source/destination pointers are internal RAM words, not SFRs.
    const int pec_base = cpu == isa::Cpu::C167 ? 0xFCE0 : 0xFDE0;
    for (int i = 0; i < 8; ++i) {
        m["SRCP" + std::to_string(i)] = Value::number(pec_base + 4 * i);
        m["DSTP" + std::to_string(i)] = Value::number(pec_base + 4 * i + 2);
    }
    if (sys166) {
        for (const auto& s : k80C166Sfrs) m[s.name] = make_sfr(s.addr);
        for (const auto& b : k80C166Bits) m[b.name] = make_bit(b.word, b.pos);
        for (const auto& ic : k80C166IntCtl) {
            m[std::string(ic.name) + "IR"] = make_bit(ic.addr, 7);
            m[std::string(ic.name) + "IE"] = make_bit(ic.addr, 6);
        }
    }
    return m;
}

// TASKING a166 predefines fewer names than Keil: the core SFRs and PSW bits
// below; the rest come from its STDNAMES file.
constexpr Sfr kTaskingSfrs[] = {
    {"DPP0", 0xFE00}, {"DPP1", 0xFE02}, {"DPP2", 0xFE04}, {"DPP3", 0xFE06}, {"CSP", 0xFE08},
    {"MDH", 0xFE0C},  {"MDL", 0xFE0E},  {"CP", 0xFE10},   {"SP", 0xFE12},   {"STKOV", 0xFE14},
    {"STKUN", 0xFE16}, {"MDC", 0xFF0E}, {"PSW", 0xFF10},  {"ZEROS", 0xFF1C}, {"ONES", 0xFF1E},
};

constexpr BitDef kTaskingBits[] = {
    {"N", 0xFF10, 0}, {"C", 0xFF10, 1},     {"V", 0xFF10, 2},    {"Z", 0xFF10, 3},     {"E", 0xFF10, 4},
    {"MULIP", 0xFF10, 5}, {"USR0", 0xFF10, 6}, {"HLDEN", 0xFF10, 10}, {"IEN", 0xFF10, 11},
};

Table build_tasking() {
    Table m;
    for (auto& [name, v] : build(isa::Cpu::C167, false))
        if (v.type == VType::Gpr || v.type == VType::GprB || v.type == VType::Cc) m[name] = v;
    for (const auto& s : kTaskingSfrs) m[s.name] = make_sfr(s.addr);
    for (const auto& b : kTaskingBits) m[b.name] = make_bit(b.word, b.pos);
    return m;
}

const Table& table(isa::Cpu cpu, bool sys166) {
    static const Table tables[2][2] = {
        {build(isa::Cpu::C166, false), build(isa::Cpu::C166, true)},
        {build(isa::Cpu::C167, false), build(isa::Cpu::C167, true)},
    };
    return tables[cpu == isa::Cpu::C167][sys166];
}

std::string upper(std::string_view s) {
    std::string u(s);
    for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

}  // namespace

std::optional<Value> predefined(std::string_view name, isa::Cpu cpu, bool sys166) {
    const auto& t = table(cpu, sys166);
    auto it = t.find(upper(name));
    if (it == t.end()) return std::nullopt;
    return it->second;
}

std::optional<Value> predefined_tasking(std::string_view name) {
    static const Table t = build_tasking();
    auto it = t.find(upper(name));
    if (it == t.end()) return std::nullopt;
    return it->second;
}

bool is_reserved_word(std::string_view name) {
    auto v = predefined(name, isa::Cpu::C167, false);
    return v && (v->type == VType::Gpr || v->type == VType::GprB || v->type == VType::Cc);
}

namespace {

[[noreturn]] void invalid_bit(int column) { throw AsmError("A103", "invalid bit address", column); }

// Bit address of linear bit number 'n' = byte address * 8 + bit.
std::optional<BitAddr> from_linear(std::int64_t n) {
    if (n < 0) return std::nullopt;
    std::int64_t word = (n >> 4) << 1;
    BitAddr r;
    r.bit = static_cast<std::uint8_t>(n & 15);
    if (word >= 0xFD00 && word <= 0xFDFE) {
        r.off = static_cast<std::uint8_t>((word - 0xFD00) / 2);
    } else if (word >= 0xFF00 && word <= 0xFFDE) {
        r.off = static_cast<std::uint8_t>(0x80 + (word - 0xFF00) / 2);
    } else if (word >= 0xF100 && word <= 0xF1DE) {
        r.off = static_cast<std::uint8_t>(0x80 + (word - 0xF100) / 2);
        r.ext = true;
    } else {
        return std::nullopt;
    }
    return r;
}

bool in_sfr_area(std::int64_t a) { return a >= 0xF000 && a <= 0xFFFF; }

}  // namespace

BitAddr bit_select(const Value& base, std::int64_t pos, int base_column, int pos_column) {
    const std::int64_t a = base.num;
    switch (base.type) {
    case VType::Gpr:
    case VType::Sfr: {
        if (pos < 0 || pos > 15) throw AsmError("A67", "absolute value (0-15) expected for bit position", pos_column);
        if (base.type == VType::Gpr) return BitAddr{static_cast<std::uint8_t>(0xF0 + a), static_cast<std::uint8_t>(pos)};
        auto r = from_linear(a * 8 + pos);
        if (!r) throw AsmError("A69", "not a bit-addressable base", base_column);
        return *r;
    }
    case VType::Number:
        if (a >= 0 && a < 0xF0) {
            // Raw bitoff number; Keil silently encodes bitoff 0 for 00h..7Fh.
            std::int64_t n = a * 16 + pos;
            if (n < 0x80 * 16 || n >= 0xF0 * 16) invalid_bit(base_column);
            return BitAddr{static_cast<std::uint8_t>(n >> 4), static_cast<std::uint8_t>(n & 15)};
        }
        if (in_sfr_area(a)) {
            // Keil silently encodes bitoff 0 when the result is outside bit-addressable memory.
            auto r = from_linear(a * 8 + pos);
            if (!r) invalid_bit(base_column);
            return *r;
        }
        throw AsmError("A69", "not a bit-addressable base", base_column);
    case VType::Bit: throw AsmError("A69", "not a bit-addressable base: a bit has no bits", base_column);
    default: throw AsmError("A70", "invalid type of expression", base_column);
    }
}

std::int64_t bit_word_address(const Value& bit) {
    if (bit.num >= 0xF0) return -1;
    return bit.num < 0x80 ? 0xFD00 + 2 * bit.num : (bit.ext ? 0xF100 : 0xFF00) + 2 * (bit.num - 0x80);
}

BitAddr bit_offset(const Value& bit, std::int64_t n, int column) {
    std::int64_t word = bit_word_address(bit);
    if (word < 0) throw AsmError("A61", "illegal operand for this operator", column);
    auto r = from_linear(word * 8 + bit.bit + n);
    if (!r) invalid_bit(column);
    return *r;
}

BitAddr word_bitoff(const Value& v, int column) {
    if (v.type == VType::Gpr) return BitAddr{static_cast<std::uint8_t>(0xF0 + v.num)};
    if ((v.type != VType::Number && v.type != VType::Sfr) || !in_sfr_area(v.num)) invalid_bit(column);
    auto r = from_linear((v.num & ~std::int64_t{1}) * 8);
    if (!r) invalid_bit(column);
    return *r;
}

}  // namespace oc166
