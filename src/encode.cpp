// SPDX-License-Identifier: MIT
#include "encode.hpp"

#include "predef.hpp"

#include <string>

namespace oc166 {
namespace {

using isa::Cls;

// How well an operand fits an operand class. Selection prefers the first
// form whose operands all fit Strict, then the first Loose one, then the last
// form that only needs truncation (forms are ordered short to long). Bad
// means the operand has the right kind but an illegal value; it explains
// the error when no other form fits.
// Fallback is a fit that a longer form should win over.
enum class Fit : std::uint8_t { No, Bad, Trunc, Fallback, Loose, Strict };

// Which SFR space a short (8-bit) register or bit address refers to.
enum class Space : std::uint8_t { None, Sfr, Esfr };

struct FieldMatch {
    Fit fit = Fit::No;
    std::uint32_t field = 0;
    Space space = Space::None;
    const char* code = nullptr;  // Bad: error code and message
    const char* message = nullptr;
};

FieldMatch fits(Fit f, std::int64_t field, Space space = Space::None) {
    return FieldMatch{f, static_cast<std::uint32_t>(field), space};
}

FieldMatch bad(const char* code, const char* message) { return FieldMatch{Fit::Bad, 0, Space::None, code, message}; }

// Short bit offsets 80h..EFh address SFR words; F0h..FFh are GPRs.
Space bitoff_space(std::int64_t off, bool ext) {
    if (off < 0x80 || off >= 0xF0) return Space::None;
    return ext ? Space::Esfr : Space::Sfr;
}

FieldMatch match_immediate(Cls cls, const Value& v) {
    if (!v.known) return fits(Fit::Strict, 0);
    std::int64_t n = v.num;
    switch (cls) {
    case Cls::D3:
    case Cls::D4: {
        int w = cls == Cls::D3 ? 3 : 4;
        Fit f = (n >= 0 && n < (std::int64_t{1} << w)) ? Fit::Strict : fits_width(v, w) ? Fit::Loose : Fit::Trunc;
        if (v.relocatable() && f != Fit::Trunc) f = Fit::Fallback;
        return fits(f, n & ((std::int64_t{1} << w) - 1));
    }
    case Cls::D8:
    case Cls::M8:
    case Cls::Seg8: return fits(fits_width(v, 8) ? Fit::Strict : Fit::Trunc, n & 0xFF);
    case Cls::D16: return fits(fits_width(v, 16) ? Fit::Strict : Fit::Trunc, n & 0xFFFF);
    case Cls::Trap7: return fits(n >= 0 && n <= 0x7F ? Fit::Strict : Fit::Trunc, n & 0x7F);
    case Cls::Irang2:
        if (v.relocatable()) return bad("A72", "absolute expression required");
        return n >= 1 && n <= 4 ? fits(Fit::Strict, n - 1) : fits(Fit::Trunc, 0);
    // Keil keeps 14 bits of the page number.
    case Cls::Pag10: return fits(fits_width(v, 14) ? Fit::Strict : Fit::Trunc, n & 0x3FFF);
    default: return {};
    }
}

FieldMatch match_register(Cls cls, const Value& v) {
    if (v.type == (cls == Cls::Reg ? VType::Gpr : VType::GprB)) return fits(Fit::Strict, 0xF0 + v.num);
    if (v.type != VType::Sfr || (v.num & 1)) return {};
    if (v.num >= 0xFE00 && v.num <= 0xFFDE) return fits(Fit::Strict, (v.num - 0xFE00) / 2, Space::Sfr);
    if (v.num >= 0xF000 && v.num <= 0xF1DE) return fits(Fit::Strict, (v.num - 0xF000) / 2, Space::Esfr);
    return {};
}

FieldMatch match(Cls cls, const Operand& op, std::int64_t next_pc) {
    const Value& v = op.value;
    const bool direct = op.mode == OpMode::Direct;
    switch (cls) {
    case Cls::None: return {};
    case Cls::Rw:
        if (direct && v.type == VType::Gpr) return fits(Fit::Strict, v.num);
        return {};
    case Cls::Rb:
        if (direct && v.type == VType::GprB) return fits(Fit::Strict, v.num);
        return {};
    case Cls::Ind:
        if (op.mode == OpMode::Ind) return fits(Fit::Strict, op.reg);
        return {};
    case Cls::PostInc:
        if (op.mode == OpMode::PostInc) return fits(Fit::Strict, op.reg);
        return {};
    case Cls::PreDec:
        if (op.mode == OpMode::PreDec) return fits(Fit::Strict, op.reg);
        return {};
    case Cls::IndI:
    case Cls::PostIncI:
        if (op.mode != (cls == Cls::IndI ? OpMode::Ind : OpMode::PostInc)) return {};
        if (op.reg > 3) return bad("A57", "illegal register: only R0..R3 are allowed here");
        return fits(Fit::Strict, op.reg);
    case Cls::IndDisp: {
        if (op.mode != OpMode::IndDisp || v.type != VType::Number) return {};
        Fit f = !v.known || fits_width(v, 16) ? Fit::Strict : Fit::Trunc;
        return fits(f, (std::int64_t{op.reg} << 16) | (v.num & 0xFFFF));
    }
    case Cls::Reg:
    case Cls::RegB:
        if (!direct) return {};
        return match_register(cls, v);
    case Cls::Mem:
        if (!direct) return {};
        if (v.type == VType::Sfr) return fits(Fit::Strict, v.num);
        if (v.type != VType::Number) return {};
        if (!v.known) return fits(Fit::Strict, 0);
        return fits(v.num >= 0 && v.num <= 0xFFFF ? Fit::Strict : Fit::Trunc, v.num & 0xFFFF);
    case Cls::BitOff: {
        if (!direct) return {};
        if (v.type == VType::Gpr) return fits(Fit::Strict, 0xF0 + v.num);
        if (v.type != VType::Sfr && v.type != VType::Number) return {};
        if (!v.known) return fits(Fit::Strict, 0);
        BitAddr b = word_bitoff(v, op.column);
        return fits(Fit::Strict, b.off, bitoff_space(b.off, b.ext));
    }
    case Cls::BitAddr: {
        if (!direct) return {};
        if (v.type == VType::Number && !v.known) return fits(Fit::Strict, 0);
        // Keil encodes some plain numbers here without complaint; a bit needs "word.bit".
        if (v.type == VType::Number) return bad("A103", "invalid bit address: use word.bit");
        if (v.type != VType::Bit) return {};
        if (!v.known) return fits(Fit::Strict, 0);
        return fits(Fit::Strict, v.num | (std::int64_t{v.bit} << 8), bitoff_space(v.num, v.ext));
    }
    case Cls::D3:
    case Cls::D4:
    case Cls::D8:
    case Cls::D16:
    case Cls::M8:
    case Cls::Trap7:
    case Cls::Irang2:
    case Cls::Pag10:
    case Cls::Seg8:
        if (op.mode != OpMode::Imm || v.type != VType::Number) return {};
        return match_immediate(cls, v);
    case Cls::Cc:
        if (direct && v.type == VType::Cc) return fits(Fit::Strict, v.num);
        return {};
    case Cls::Rel: {
        if (!direct || v.type != VType::Number) return {};
        if (!v.known) return fits(Fit::Strict, 0);
        std::int64_t disp = v.num - next_pc;
        if ((disp & 1) || disp < -256 || disp > 254) return fits(Fit::Trunc, 0);
        return fits(Fit::Strict, (disp / 2) & 0xFF);
    }
    case Cls::Caddr:
        if (!direct || v.type != VType::Number) return {};
        if (!v.known) return fits(Fit::Strict, 0);
        return fits(v.num >= 0 && v.num <= 0xFFFF ? Fit::Strict : Fit::Trunc, v.num & 0xFFFF);
    case Cls::Seg:
        if (!direct || v.type != VType::Number) return {};
        if (!v.known) return fits(Fit::Strict, 0);
        return fits(v.num >= 0 && v.num <= 0xFF ? Fit::Strict : Fit::Trunc, v.num & 0xFF);
    }
    return {};
}

std::uint32_t extract(std::uint32_t field, isa::Sub sub) {
    switch (sub) {
    case isa::Sub::Full:
    case isa::Sub::Lo:
    case isa::Sub::Off: return sub == isa::Sub::Full ? field : field & 0xFF;
    case isa::Sub::Hi: return (field >> 8) & 0xFF;
    case isa::Sub::Bit: return (field >> 8) & 0xF;
    case isa::Sub::Reg: return (field >> 16) & 0xF;
    }
    return 0;
}

// Keil software instructions pick the bit, byte or word instruction from the
// operand types.
std::string_view resolve_mnemonic(std::string_view mnemonic, const std::vector<Operand>& ops) {
    if (auto target = isa::alias_target(mnemonic); !target.empty()) return target;
    const isa::GenericEntry* g = isa::generic(mnemonic);
    if (!g) return mnemonic;
    bool bit = false, byte = false;
    for (const auto& op : ops) {
        if (op.mode != OpMode::Direct) continue;
        bit |= op.value.type == VType::Bit;
        byte |= op.value.type == VType::GprB;
    }
    if (bit && !g->bit.empty()) return g->bit;
    if (byte) return g->byte;
    return g->word;
}

}  // namespace

Encoding encode(std::string_view mnemonic, const std::vector<Operand>& ops, const EncodeContext& ctx,
                Reporter& rep) {
    std::string_view name = resolve_mnemonic(mnemonic, ops);
    auto forms = isa::forms(name);
    if (forms.empty()) throw AsmError("A6", "unknown instruction '" + std::string(mnemonic) + "'");

    const isa::Form* strict = nullptr;
    const isa::Form* loose = nullptr;
    const isa::Form* fallback = nullptr;
    const isa::Form* trunc = nullptr;
    FieldMatch bad_operand;
    int bad_column = 0;
    bool count_ok = false;
    for (const auto& f : forms) {
        if (f.nops != ops.size()) continue;
        count_ok = true;
        Fit worst = Fit::Strict;
        for (std::size_t i = 0; i < ops.size() && worst != Fit::No; ++i) {
            FieldMatch m = match(f.ops[i], ops[i], ctx.pc + f.size);
            if (m.fit < worst) worst = m.fit;
            if (m.fit == Fit::Bad && !bad_operand.code) {
                bad_operand = m;
                bad_column = ops[i].column;
            }
        }
        if (worst == Fit::Strict) {
            strict = &f;
            break;
        }
        if (worst == Fit::Loose && !loose) loose = &f;
        if (worst == Fit::Fallback && !fallback) fallback = &f;
        if (worst == Fit::Trunc) trunc = &f;
    }
    if (!count_ok) throw AsmError("A73", "number of operands does not match instruction");
    const isa::Form* form = strict ? strict : loose ? loose : fallback ? fallback : trunc;
    if (!form && bad_operand.code) throw AsmError(bad_operand.code, bad_operand.message, bad_column);
    if (!form) throw AsmError("A74", "operand type mismatch");

    if (form->cpu == isa::Cpu::C167 && ctx.cpu != isa::Cpu::C167)
        throw AsmError("A108", "80C167 instruction '" + std::string(form->mnemonic) + "' requires the MOD167 control");

    std::uint32_t fields[3] = {};
    for (std::size_t i = 0; i < ops.size(); ++i) {
        FieldMatch m = match(form->ops[i], ops[i], ctx.pc + form->size);
        fields[i] = m.field;
        int col = ops[i].column;
        if (m.space == Space::Esfr && !ctx.ext_sfr)
            rep.warning("A111", "access to extended SFR outside an 'extended' sequence", col);
        if (m.space == Space::Sfr && ctx.ext_sfr)
            rep.warning("A110", "access to normal SFR within an 'extended' sequence", col);
        if (m.fit != Fit::Trunc) continue;
        if (form->ops[i] == Cls::Irang2) {
            rep.error("A19", "numeric value out of range (1..4)", col);
        } else if (form->ops[i] == Cls::Rel) {
            std::int64_t disp = ops[i].value.num - (ctx.pc + form->size);
            if (disp & 1)
                rep.error("A134", "improper address alignment: branch target is odd", col);
            else
                rep.error("A82", "relative distance out of range (" + std::to_string(disp) + " bytes)", col);
        } else {
            rep.warning("A62", "value truncation", col);
        }
    }

    Encoding e;
    e.form = form;
    e.size = form->size;
    for (int b = 0; b < form->size; ++b) {
        const isa::ByteTemplate& t = form->bytes[b];
        unsigned byte = 0;
        int shift = 8;
        for (int p = 0; p < t.count; ++p) {
            const isa::Part& part = t.parts[p];
            shift -= part.width;
            std::uint32_t v = part.operand == isa::kLiteral ? part.literal : extract(fields[part.operand], part.sub);
            byte |= (v & ((1u << part.width) - 1)) << shift;
        }
        e.bytes[b] = static_cast<std::uint8_t>(byte);
    }
    return e;
}

ExtSequence ext_sequence(const Encoding& e) {
    if (!e.form) return {};
    auto m = e.form->mnemonic;
    if (m != "ATOMIC" && m.substr(0, 3) != "EXT") return {};
    // The irang2 field is bits 5..4 of the second byte and holds count-1.
    return {((e.bytes[1] >> 4) & 3) + 1, m == "EXTR" || m == "EXTPR" || m == "EXTSR"};
}

}  // namespace oc166
