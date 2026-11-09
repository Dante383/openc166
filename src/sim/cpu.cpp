// SPDX-License-Identifier: MIT
// Instruction semantics of the C166/C167 core, following the Infineon
// "C166 Family Instruction Set Manual". Decoding uses the shared instruction
// table (disasm.cpp); this file gives each mnemonic its effect.
//
// Cycle counts are approximate: 2 states per instruction word, 2 more for a
// taken branch, call or return, 10 for a multiply and 20 for a divide. Bus
// wait states are not modelled.
#include "sim/machine.hpp"

#include "isa/isa.hpp"

#include <string_view>
#include <unordered_map>

namespace oc166::sim {

namespace {

using isa::Cls;

enum class Op : std::uint8_t {
    Add, Addc, Sub, Subc, Cmp, Xor, And, Or,
    Cmpi1, Cmpi2, Cmpd1, Cmpd2,
    Mul, Mulu, Prior, Div, Divu, Divl, Divlu, Neg, Cpl,
    Rol, Ror, Shl, Shr, Ashr,
    Bclr, Bset, Bmov, Bmovn, Band, Bor, Bxor, Bcmp, Bfldl, Bfldh,
    Mov, Movbz, Movbs,
    Jmpa, Jmpi, Jmpr, Jmps, Jb, Jnb, Jbc, Jnbs, Calla, Calli, Callr, Calls, Pcall, Trap,
    Ret, Rets, Reti, Retp, Push, Pop, Scxt,
    Nop, Srst, Idle, Pwrdn, Srvwdt, Diswdt, Einit,
    Atomic, Extr, Extp, Extpr, Exts, Extsr,
    Invalid,
};

struct OpInfo {
    Op op = Op::Invalid;
    bool byte = false;  // operates on bytes
};

const std::vector<OpInfo>& op_table() {
    static const std::vector<OpInfo> table = [] {
        static const std::unordered_map<std::string_view, OpInfo> by_name = {
            {"ADD", {Op::Add}},     {"ADDC", {Op::Addc}},   {"SUB", {Op::Sub}},      {"SUBC", {Op::Subc}},
            {"CMP", {Op::Cmp}},     {"XOR", {Op::Xor}},     {"AND", {Op::And}},      {"OR", {Op::Or}},
            {"ADDB", {Op::Add, true}}, {"ADDCB", {Op::Addc, true}}, {"SUBB", {Op::Sub, true}},
            {"SUBCB", {Op::Subc, true}}, {"CMPB", {Op::Cmp, true}}, {"XORB", {Op::Xor, true}},
            {"ANDB", {Op::And, true}}, {"ORB", {Op::Or, true}},
            {"CMPI1", {Op::Cmpi1}}, {"CMPI2", {Op::Cmpi2}}, {"CMPD1", {Op::Cmpd1}},  {"CMPD2", {Op::Cmpd2}},
            {"MUL", {Op::Mul}},     {"MULU", {Op::Mulu}},   {"PRIOR", {Op::Prior}},  {"DIV", {Op::Div}},
            {"DIVU", {Op::Divu}},   {"DIVL", {Op::Divl}},   {"DIVLU", {Op::Divlu}},  {"NEG", {Op::Neg}},
            {"CPL", {Op::Cpl}},     {"NEGB", {Op::Neg, true}}, {"CPLB", {Op::Cpl, true}},
            {"ROL", {Op::Rol}},     {"ROR", {Op::Ror}},     {"SHL", {Op::Shl}},      {"SHR", {Op::Shr}},
            {"ASHR", {Op::Ashr}},
            {"BCLR", {Op::Bclr}},   {"BSET", {Op::Bset}},   {"BMOV", {Op::Bmov}},    {"BMOVN", {Op::Bmovn}},
            {"BAND", {Op::Band}},   {"BOR", {Op::Bor}},     {"BXOR", {Op::Bxor}},    {"BCMP", {Op::Bcmp}},
            {"BFLDL", {Op::Bfldl}}, {"BFLDH", {Op::Bfldh}},
            {"MOV", {Op::Mov}},     {"MOVB", {Op::Mov, true}}, {"MOVBZ", {Op::Movbz}}, {"MOVBS", {Op::Movbs}},
            {"JMPA", {Op::Jmpa}},   {"JMPI", {Op::Jmpi}},   {"JMPR", {Op::Jmpr}},    {"JMPS", {Op::Jmps}},
            {"JB", {Op::Jb}},       {"JNB", {Op::Jnb}},     {"JBC", {Op::Jbc}},      {"JNBS", {Op::Jnbs}},
            {"CALLA", {Op::Calla}}, {"CALLI", {Op::Calli}}, {"CALLR", {Op::Callr}},  {"CALLS", {Op::Calls}},
            {"PCALL", {Op::Pcall}}, {"TRAP", {Op::Trap}},
            {"RET", {Op::Ret}},     {"RETS", {Op::Rets}},   {"RETI", {Op::Reti}},    {"RETP", {Op::Retp}},
            {"PUSH", {Op::Push}},   {"POP", {Op::Pop}},     {"SCXT", {Op::Scxt}},
            {"NOP", {Op::Nop}},     {"SRST", {Op::Srst}},   {"IDLE", {Op::Idle}},    {"PWRDN", {Op::Pwrdn}},
            {"SRVWDT", {Op::Srvwdt}}, {"DISWDT", {Op::Diswdt}}, {"EINIT", {Op::Einit}},
            {"ATOMIC", {Op::Atomic}}, {"EXTR", {Op::Extr}}, {"EXTP", {Op::Extp}},    {"EXTPR", {Op::Extpr}},
            {"EXTS", {Op::Exts}},   {"EXTSR", {Op::Extsr}},
        };
        std::vector<OpInfo> t;
        for (const auto& f : isa::all_forms()) {
            auto it = by_name.find(f.mnemonic);
            t.push_back(it == by_name.end() ? OpInfo{} : it->second);
        }
        return t;
    }();
    return table;
}

constexpr std::uint16_t N = 1, C = 2, V = 4, Z = 8, E = 16;
constexpr std::uint16_t kFlags = N | C | V | Z | E;

// TFR bits raised by instructions.
constexpr std::uint16_t kUndopc = 1u << 7, kPrtflt = 1u << 3, kIllopa = 1u << 2, kIllina = 1u << 1;

// Aborts the current instruction with a class B hardware trap.
struct TrapAbort {
    std::uint16_t bit;
};

}  // namespace

// The execution of one instruction.
struct Exec {
    Machine& m;
    const dis::Insn& in;
    bool byte;
    std::uint16_t next_ip;
    int extra_cycles = 0;
    // Post-increments to apply after the operands are accessed.
    struct PostInc {
        int reg;
        int by;
    };
    PostInc post[2] = {};
    int npost = 0;

    // An operand location: a memory address or an immediate.
    struct Loc {
        bool imm = false;
        Addr addr = 0;
        std::uint16_t value = 0;
        bool gpr = false;  // a GPR, so word alignment is not checked
    };

    std::uint16_t cp() const { return m.sfr_get(reg::CP); }
    Addr gpr_addr(int n, bool b) const { return static_cast<Addr>(cp() + (b ? n : 2 * n)) & 0xFFFF; }
    std::uint16_t rw(int n) { return m.read16(gpr_addr(n, false)); }
    void set_rw(int n, std::uint16_t v) { m.write16(gpr_addr(n, false), v); }

    Loc reg_loc(std::uint8_t r, bool b) const {
        Loc l;
        if (r >= 0xF0) {
            l.addr = gpr_addr(r & 0xF, b);
            l.gpr = true;
        } else {
            l.addr = dis::reg_address(r, m.ext_.esfr);
        }
        return l;
    }
    Loc bitoff_loc(std::uint8_t q) const {
        Loc l;
        if (q >= 0xF0) {
            l.addr = gpr_addr(q & 0xF, false);
            l.gpr = true;
        } else {
            l.addr = dis::bitoff_address(q, m.ext_.esfr);
        }
        return l;
    }
    Loc indirect(std::uint16_t a) const {
        Loc l;
        l.addr = m.data_address(a);
        return l;
    }

    // Resolves an operand; 'b' is its width (byte or word). Pre-decrements
    // happen now; post-increments are queued.
    Loc loc(const dis::Operand& op, bool b) {
        const int step = b ? 1 : 2;
        switch (op.cls) {
        case Cls::Rw: return reg_loc(static_cast<std::uint8_t>(0xF0 | op.value), false);
        case Cls::Rb: {
            Loc l;
            l.addr = gpr_addr(op.value, true);
            l.gpr = true;
            return l;
        }
        case Cls::Ind:
        case Cls::IndI: return indirect(rw(op.value));
        case Cls::PostInc:
        case Cls::PostIncI: {
            Loc l = indirect(rw(op.value));
            post[npost++] = {op.value, step};
            return l;
        }
        case Cls::PreDec: {
            const std::uint16_t a = static_cast<std::uint16_t>(rw(op.value) - step);
            set_rw(op.value, a);
            return indirect(a);
        }
        case Cls::IndDisp:
            return indirect(static_cast<std::uint16_t>(rw(op.aux) + static_cast<std::uint16_t>(op.value)));
        case Cls::Reg: return reg_loc(static_cast<std::uint8_t>(op.value), false);
        case Cls::RegB: return reg_loc(static_cast<std::uint8_t>(op.value), true);
        case Cls::Mem: return indirect(static_cast<std::uint16_t>(op.value));
        case Cls::BitOff:
        case Cls::BitAddr: return bitoff_loc(static_cast<std::uint8_t>(op.value));
        default: {
            Loc l;
            l.imm = true;
            l.value = static_cast<std::uint16_t>(op.value);
            return l;
        }
        }
    }

    std::uint16_t read(const Loc& l, bool b) {
        if (l.imm) return b ? static_cast<std::uint16_t>(l.value & 0xFF) : l.value;
        if (b) return m.read8(l.addr);
        if ((l.addr & 1) && !l.gpr) throw TrapAbort{kIllopa};
        return m.read16(l.addr);
    }
    void write(const Loc& l, bool b, std::uint16_t v) {
        if (l.imm) return;
        if (b) return m.write8(l.addr, static_cast<std::uint8_t>(v));
        if ((l.addr & 1) && !l.gpr) throw TrapAbort{kIllopa};
        m.write16(l.addr, v);
    }
    void finish_post() {
        for (int i = 0; i < npost; ++i)
            set_rw(post[i].reg, static_cast<std::uint16_t>(rw(post[i].reg) + post[i].by));
        npost = 0;
    }

    void flags(std::uint16_t set, std::uint16_t mask) {
        const std::uint16_t p = m.psw();
        const std::uint16_t v = static_cast<std::uint16_t>((p & ~mask) | (set & mask));
        m.sfr_set(reg::PSW, v);
        // Flag updates are not bus accesses, but watchpoints on PSW bits
        // must see them.
        if (v != p && m.on_access) m.on_access({AccessKind::Write, reg::PSW, 2, v, p});
    }
    bool flag(std::uint16_t f) const { return m.psw() & f; }

    bool cond(int cc) const {
        const std::uint16_t p = m.psw();
        const bool n = p & N, c = p & C, v = p & V, z = p & Z, e = p & E;
        switch (cc & 15) {
        case 0x0: return true;
        case 0x1: return !z && !e;
        case 0x2: return z;
        case 0x3: return !z;
        case 0x4: return v;
        case 0x5: return !v;
        case 0x6: return n;
        case 0x7: return !n;
        case 0x8: return c;
        case 0x9: return !c;
        case 0xA: return !(z || (n != v));
        case 0xB: return z || (n != v);
        case 0xC: return n != v;
        case 0xD: return n == v;
        case 0xE: return !(z || c);
        case 0xF: return z || c;
        }
        return false;
    }

    // Flag results for a value of the operation width.
    std::uint16_t nz(std::uint32_t r, bool b) const {
        const std::uint32_t mask = b ? 0xFF : 0xFFFF, sign = b ? 0x80 : 0x8000;
        std::uint16_t f = 0;
        if ((r & mask) == 0) f |= Z;
        if (r & sign) f |= N;
        return f;
    }
    std::uint16_t e_of(std::uint32_t v, bool b) const { return (v & (b ? 0xFF : 0xFFFF)) == (b ? 0x80u : 0x8000u) ? E : 0; }

    // Arithmetic with flags; returns the result.
    std::uint16_t arith(Op op, std::uint32_t a, std::uint32_t s, bool b) {
        const std::uint32_t mask = b ? 0xFF : 0xFFFF, sign = b ? 0x80 : 0x8000;
        std::uint32_t r = 0;
        std::uint16_t f = e_of(s, b);
        switch (op) {
        case Op::Add:
        case Op::Addc: {
            const std::uint32_t cin = op == Op::Addc && flag(C) ? 1 : 0;
            r = a + s + cin;
            if (r > mask) f |= C;
            if ((~(a ^ s) & (a ^ r)) & sign) f |= V;
            break;
        }
        case Op::Sub:
        case Op::Subc:
        case Op::Cmp: {
            const std::uint32_t bin = op == Op::Subc && flag(C) ? 1 : 0;
            r = a - s - bin;
            if (a < s + bin) f |= C;
            if (((a ^ s) & (a ^ r)) & sign) f |= V;
            break;
        }
        case Op::And: r = a & s; break;
        case Op::Or: r = a | s; break;
        case Op::Xor: r = a ^ s; break;
        default: break;
        }
        f |= nz(r, b);
        if ((op == Op::Addc || op == Op::Subc) && !flag(Z)) f &= static_cast<std::uint16_t>(~Z);
        flags(f, kFlags);
        return static_cast<std::uint16_t>(r & mask);
    }

    void jump(std::uint16_t ip) {
        next_ip = ip;
        extra_cycles += 2;
    }
    std::uint16_t rel_target(const dis::Operand& op) const {
        return static_cast<std::uint16_t>(next_ip + 2 * op.value);
    }

    bool get_bit(const dis::Operand& op, Loc& l) {
        l = loc(op, false);
        return (read(l, false) >> op.aux) & 1;
    }
    void put_bit(const Loc& l, int bit, bool v) {
        std::uint16_t w = read(l, false);
        w = static_cast<std::uint16_t>(v ? (w | (1u << bit)) : (w & ~(1u << bit)));
        write(l, false, w);
    }

    void run();
};

void Exec::run() {
    const OpInfo info = op_table()[static_cast<std::size_t>(in.form - isa::all_forms().data())];
    const dis::Operand* o = in.ops;
    switch (info.op) {
    case Op::Add:
    case Op::Addc:
    case Op::Sub:
    case Op::Subc:
    case Op::And:
    case Op::Or:
    case Op::Xor:
    case Op::Cmp: {
        Loc src = loc(o[1], byte);
        const std::uint16_t s = read(src, byte);
        Loc dst = loc(o[0], byte);
        const std::uint16_t a = read(dst, byte);
        const std::uint16_t r = arith(info.op, a, s, byte);
        if (info.op != Op::Cmp) write(dst, byte, r);
        finish_post();
        break;
    }
    case Op::Cmpi1:
    case Op::Cmpi2:
    case Op::Cmpd1:
    case Op::Cmpd2: {
        Loc src = loc(o[1], false);
        const std::uint16_t s = read(src, false);
        const std::uint16_t a = rw(o[0].value);
        arith(Op::Cmp, a, s, false);
        const int d = info.op == Op::Cmpi1 ? 1 : info.op == Op::Cmpi2 ? 2 : info.op == Op::Cmpd1 ? -1 : -2;
        set_rw(o[0].value, static_cast<std::uint16_t>(a + d));
        break;
    }
    case Op::Mul:
    case Op::Mulu: {
        const std::uint16_t a = rw(o[0].value), b = rw(o[1].value);
        std::uint32_t r;
        bool overflow;
        if (info.op == Op::Mul) {
            const std::int32_t sr = static_cast<std::int16_t>(a) * static_cast<std::int32_t>(static_cast<std::int16_t>(b));
            r = static_cast<std::uint32_t>(sr);
            overflow = sr < -32768 || sr > 32767;
        } else {
            r = static_cast<std::uint32_t>(a) * b;
            overflow = r > 0xFFFF;
        }
        m.sfr_set(reg::MDH, static_cast<std::uint16_t>(r >> 16));
        m.sfr_set(reg::MDL, static_cast<std::uint16_t>(r));
        m.sfr_set(reg::MDC, static_cast<std::uint16_t>(m.sfr_get(reg::MDC) | 0x10));
        std::uint16_t f = 0;
        if (r == 0) f |= Z;
        if (r & 0x80000000u) f |= N;
        if (overflow) f |= V;
        flags(f, kFlags);
        extra_cycles += 8;
        break;
    }
    case Op::Div:
    case Op::Divu:
    case Op::Divl:
    case Op::Divlu: {
        const std::uint16_t d = rw(o[0].value);
        const std::uint16_t mdl = m.sfr_get(reg::MDL), mdh = m.sfr_get(reg::MDH);
        bool overflow = d == 0;
        std::uint32_t q = 0, rem = 0;
        if (!overflow) {
            switch (info.op) {
            case Op::Div: {
                const std::int32_t n = static_cast<std::int16_t>(mdl), dv = static_cast<std::int16_t>(d);
                const std::int32_t sq = n / dv;
                overflow = sq > 32767 || sq < -32768;
                q = static_cast<std::uint32_t>(sq);
                rem = static_cast<std::uint32_t>(n % dv);
                break;
            }
            case Op::Divu:
                q = mdl / d;
                rem = mdl % d;
                break;
            case Op::Divl: {
                const std::int64_t n = static_cast<std::int32_t>((static_cast<std::uint32_t>(mdh) << 16) | mdl);
                const std::int64_t dv = static_cast<std::int16_t>(d);
                const std::int64_t sq = n / dv;
                overflow = sq > 32767 || sq < -32768;
                q = static_cast<std::uint32_t>(sq);
                rem = static_cast<std::uint32_t>(n % dv);
                break;
            }
            default: {
                const std::uint32_t n = (static_cast<std::uint32_t>(mdh) << 16) | mdl;
                q = n / d;
                rem = n % d;
                overflow = q > 0xFFFF;
                break;
            }
            }
        }
        std::uint16_t f = 0;
        if (overflow) {
            f |= V;
        } else {
            m.sfr_set(reg::MDL, static_cast<std::uint16_t>(q));
            m.sfr_set(reg::MDH, static_cast<std::uint16_t>(rem));
            f |= nz(q, false);
        }
        m.sfr_set(reg::MDC, static_cast<std::uint16_t>(m.sfr_get(reg::MDC) | 0x10));
        flags(f, kFlags);
        extra_cycles += 18;
        break;
    }
    case Op::Prior: {
        std::uint16_t v = rw(o[1].value);
        std::uint16_t count = 0;
        if (v != 0)
            while (!(v & 0x8000)) {
                v = static_cast<std::uint16_t>(v << 1);
                ++count;
            }
        set_rw(o[0].value, count);
        flags(rw(o[1].value) == 0 ? Z : 0, kFlags);
        break;
    }
    case Op::Neg: {
        Loc l = loc(o[0], byte);
        const std::uint16_t a = read(l, byte);
        const std::uint16_t r = arith(Op::Sub, 0, a, byte);
        write(l, byte, r);
        break;
    }
    case Op::Cpl: {
        Loc l = loc(o[0], byte);
        const std::uint16_t a = read(l, byte);
        const std::uint16_t r = static_cast<std::uint16_t>(~a & (byte ? 0xFF : 0xFFFF));
        flags(static_cast<std::uint16_t>(nz(r, byte) | e_of(a, byte)), kFlags);
        write(l, byte, r);
        break;
    }
    case Op::Rol:
    case Op::Ror:
    case Op::Shl:
    case Op::Shr:
    case Op::Ashr: {
        const int n = (o[1].cls == Cls::Rw ? rw(o[1].value) : static_cast<std::uint16_t>(o[1].value)) & 15;
        const std::uint32_t a = rw(o[0].value);
        std::uint32_t r = a;
        std::uint16_t f = 0;
        if (n > 0) {
            switch (info.op) {
            case Op::Shl:
                r = a << n;
                if ((a >> (16 - n)) & 1) f |= C;
                break;
            case Op::Rol:
                r = (a << n) | (a >> (16 - n));
                if ((a >> (16 - n)) & 1) f |= C;
                break;
            case Op::Shr:
            case Op::Ashr:
            case Op::Ror:
                if (info.op == Op::Shr) r = a >> n;
                if (info.op == Op::Ashr) r = static_cast<std::uint32_t>(static_cast<std::int16_t>(a) >> n);
                if (info.op == Op::Ror) r = (a >> n) | (a << (16 - n));
                if ((a >> (n - 1)) & 1) f |= C;
                if (n > 1 && (a & ((1u << (n - 1)) - 1))) f |= V;
                break;
            default: break;
            }
        }
        f |= nz(r, false);
        flags(f, kFlags);
        set_rw(o[0].value, static_cast<std::uint16_t>(r));
        break;
    }
    case Op::Bclr:
    case Op::Bset: {
        Loc l;
        const bool old = get_bit(o[0], l);
        flags(static_cast<std::uint16_t>(old ? N : Z), kFlags);
        put_bit(l, o[0].aux, info.op == Op::Bset);
        break;
    }
    case Op::Bmov:
    case Op::Bmovn: {
        Loc sl, dl;
        const bool s = get_bit(o[1], sl);
        dl = loc(o[0], false);
        flags(static_cast<std::uint16_t>(s ? N : Z), kFlags);
        put_bit(dl, o[0].aux, info.op == Op::Bmov ? s : !s);
        break;
    }
    case Op::Band:
    case Op::Bor:
    case Op::Bxor:
    case Op::Bcmp: {
        Loc sl, dl;
        const bool s = get_bit(o[1], sl);
        const bool d = get_bit(o[0], dl);
        std::uint16_t f = 0;
        if (!(d || s)) f |= Z;
        if (d || s) f |= V;
        if (d && s) f |= C;
        if (d != s) f |= N;
        flags(f, kFlags);
        if (info.op == Op::Band) put_bit(dl, o[0].aux, d && s);
        if (info.op == Op::Bor) put_bit(dl, o[0].aux, d || s);
        if (info.op == Op::Bxor) put_bit(dl, o[0].aux, d != s);
        break;
    }
    case Op::Bfldl:
    case Op::Bfldh: {
        Loc l = loc(o[0], false);
        std::uint16_t w = read(l, false);
        const int shift = info.op == Op::Bfldh ? 8 : 0;
        const std::uint16_t mask = static_cast<std::uint16_t>((o[1].value & 0xFF) << shift);
        const std::uint16_t data = static_cast<std::uint16_t>((o[2].value & 0xFF) << shift);
        w = static_cast<std::uint16_t>((w & ~mask) | (data & mask));
        flags(nz(w, false), kFlags);
        write(l, false, w);
        break;
    }
    case Op::Mov: {
        Loc src = loc(o[1], byte);
        const std::uint16_t s = read(src, byte);
        Loc dst = loc(o[0], byte);
        flags(static_cast<std::uint16_t>(nz(s, byte) | e_of(s, byte)), N | Z | E);
        write(dst, byte, s);
        finish_post();
        break;
    }
    case Op::Movbz:
    case Op::Movbs: {
        Loc src = loc(o[1], true);
        const std::uint8_t s = static_cast<std::uint8_t>(read(src, true));
        const std::uint16_t r = info.op == Op::Movbz ? s : static_cast<std::uint16_t>(static_cast<std::int8_t>(s));
        Loc dst = loc(o[0], false);
        std::uint16_t f = s == 0 ? Z : 0;
        if (s == 0x80) f |= E;
        if (info.op == Op::Movbs && (s & 0x80)) f |= N;
        flags(f, N | Z | E);
        write(dst, false, r);
        break;
    }
    case Op::Jmpa:
        if (cond(o[0].value)) jump(static_cast<std::uint16_t>(o[1].value));
        break;
    case Op::Jmpi:
        if (cond(o[0].value)) jump(rw(o[1].value));
        break;
    case Op::Jmpr:
        if (in.nops == 1)
            jump(rel_target(o[0]));
        else if (cond(o[0].value))
            jump(rel_target(o[1]));
        break;
    case Op::Jmps:
        m.set_csp(static_cast<std::uint16_t>(o[0].value));
        jump(static_cast<std::uint16_t>(o[1].value));
        break;
    case Op::Jb:
    case Op::Jnb:
    case Op::Jbc:
    case Op::Jnbs: {
        Loc l;
        const bool bit = get_bit(o[0], l);
        if (info.op == Op::Jbc || info.op == Op::Jnbs) flags(static_cast<std::uint16_t>(bit ? N : Z), kFlags);
        const bool take = (info.op == Op::Jb || info.op == Op::Jbc) ? bit : !bit;
        if (take) {
            if (info.op == Op::Jbc) put_bit(l, o[0].aux, false);
            if (info.op == Op::Jnbs) put_bit(l, o[0].aux, true);
            jump(rel_target(o[1]));
        }
        break;
    }
    case Op::Calla:
        if (cond(o[0].value)) {
            m.push(next_ip);
            jump(static_cast<std::uint16_t>(o[1].value));
        }
        break;
    case Op::Calli:
        if (cond(o[0].value)) {
            const std::uint16_t t = rw(o[1].value);
            m.push(next_ip);
            jump(t);
        }
        break;
    case Op::Callr:
        m.push(next_ip);
        jump(rel_target(o[0]));
        break;
    case Op::Calls:
        m.push(m.csp());
        m.push(next_ip);
        m.set_csp(static_cast<std::uint16_t>(o[0].value));
        jump(static_cast<std::uint16_t>(o[1].value));
        break;
    case Op::Pcall: {
        Loc l = loc(o[0], false);
        const std::uint16_t v = read(l, false);
        m.push(v);
        flags(static_cast<std::uint16_t>(nz(v, false) | e_of(v, false)), N | Z | E);
        m.push(next_ip);
        jump(static_cast<std::uint16_t>(o[1].value));
        break;
    }
    case Op::Trap: {
        m.ip_ = next_ip;
        m.enter_trap(static_cast<std::uint8_t>(o[0].value), false);
        next_ip = m.ip_;
        break;
    }
    case Op::Ret: jump(m.pop()); break;
    case Op::Rets: {
        const std::uint16_t ip = m.pop();
        m.set_csp(m.pop());
        jump(ip);
        break;
    }
    case Op::Reti: {
        const std::uint16_t ip = m.pop();
        if (!(m.sfr_get(reg::SYSCON) & 0x0800)) m.set_csp(m.pop());
        m.set_psw(m.pop());
        jump(ip);
        break;
    }
    case Op::Retp: {
        const std::uint16_t ip = m.pop();
        const std::uint16_t v = m.pop();
        Loc l = loc(o[0], false);
        write(l, false, v);
        flags(static_cast<std::uint16_t>(nz(v, false) | e_of(v, false)), N | Z | E);
        jump(ip);
        break;
    }
    case Op::Push: {
        Loc l = loc(o[0], false);
        const std::uint16_t v = read(l, false);
        m.push(v);
        flags(static_cast<std::uint16_t>(nz(v, false) | e_of(v, false)), N | Z | E);
        break;
    }
    case Op::Pop: {
        const std::uint16_t v = m.pop();
        Loc l = loc(o[0], false);
        write(l, false, v);
        flags(static_cast<std::uint16_t>(nz(v, false) | e_of(v, false)), N | Z | E);
        break;
    }
    case Op::Scxt: {
        Loc src = loc(o[1], false);
        const std::uint16_t s = read(src, false);
        Loc dst = loc(o[0], false);
        m.push(read(dst, false));
        write(dst, false, s);
        break;
    }
    case Op::Nop: break;
    case Op::Srst: break;  // handled by the caller
    case Op::Idle: m.idle_ = true; break;
    case Op::Pwrdn: m.pwrdn_ = true; break;
    case Op::Srvwdt:
        m.sfr_set(reg::WDTCON, static_cast<std::uint16_t>(m.sfr_get(reg::WDTCON) & ~2u));
        if (m.wdt_enabled_) m.watchdog_reload();
        break;
    case Op::Diswdt:
        if (!m.einit_) {
            m.wdt_enabled_ = false;
            m.watchdog_schedule();
        }
        break;
    case Op::Einit:
        m.einit_ = true;
        if (SfrHook* h = m.hook(reg::SYSCON)) h->readonly_mask = 0x1FFF;
        m.sfr_set(reg::WDTCON, static_cast<std::uint16_t>(m.sfr_get(reg::WDTCON) & ~0x1Cu));
        break;
    case Op::Atomic:
    case Op::Extr:
    case Op::Extp:
    case Op::Extpr:
    case Op::Exts:
    case Op::Extsr:
        break;  // handled by the caller
    case Op::Invalid: throw TrapAbort{kUndopc};
    }
}

Stop Machine::step() {
    if (stop_requested_) {
        stop_requested_ = false;
        return Stop::Halted;
    }
    if (pwrdn_) return Stop::PowerDown;
    stop_pending_ = Stop::None;
    cur_pc_ = pc();

    // Hardware traps raised by the previous instruction, then interrupts.
    if (pending_traps_) {
        const std::uint16_t t = pending_traps_;
        pending_traps_ = 0;
        sfr_set(reg::TFR, static_cast<std::uint16_t>(sfr_get(reg::TFR) | t));
        std::uint8_t trap;
        if (t & (1u << 15))
            trap = 2;
        else if (t & (1u << 14))
            trap = 4;
        else if (t & (1u << 13))
            trap = 6;
        else
            trap = 0x0A;
        ext_ = {};
        enter_trap(trap, true);
        run_events();
        return stop_pending_;
    }
    if (take_interrupt()) {
        run_events();
        return stop_pending_;
    }
    if (idle_) {
        // Sleep until the next event, which may raise an interrupt.
        if (events_.empty()) return Stop::Idle;
        cycles_ = std::max(cycles_, events_.front().time);
        run_events();
        return stop_pending_;
    }
    if (on_fetch && on_fetch(cur_pc_)) return Stop::Breakpoint;

    if (ip_ & 1) {
        raise_trap(kIllina);
        return Stop::None;
    }
    auto insn = decode_at(cur_pc_);
    if (!insn) {
        const std::uint8_t b0 = peek8(cur_pc_);
        const bool protected_op = b0 == 0xA5 || b0 == 0xA7 || b0 == 0x87 || b0 == 0x97 || b0 == 0xB5 || b0 == 0xB7;
        if (stop_on_undefined && !protected_op) return Stop::Undefined;
        raise_trap(protected_op ? kPrtflt : kUndopc);
        ip_ = static_cast<std::uint16_t>(ip_ + 2);
        cycles_ += 2;
        ++instructions_;
        return stop_pending_;
    }
    const OpInfo info = op_table()[static_cast<std::size_t>(insn->form - isa::all_forms().data())];
    if (info.op == Op::Srst) {
        ++instructions_;
        cycles_ += 4;
        reset(ResetKind::Software);
        return stop_pending_;
    }
    Exec ex{*this, *insn, info.byte, static_cast<std::uint16_t>(ip_ + insn->size)};
    // EXT and ATOMIC: the sequence starts after this instruction.
    const bool starts_sequence = info.op >= Op::Atomic && info.op <= Op::Extsr;
    try {
        ex.run();
        ip_ = ex.next_ip;
    } catch (const TrapAbort& t) {
        ip_ = static_cast<std::uint16_t>(ip_ + insn->size);
        raise_trap(t.bit);
    }
    if (starts_sequence) {
        ext_.advance(*insn);
        if (ext_.reg >= 0) {
            ext_.value = gpr(ext_.reg) & (ext_.kind == dis::ExtState::Kind::Page ? 0x3FF : 0xFF);
            ext_.reg = -1;
        }
    } else {
        ext_.advance(*insn);
    }
    ++instructions_;
    cycles_ += static_cast<std::uint64_t>(insn->size + ex.extra_cycles);
    run_events();
    return stop_pending_;
}

Stop Machine::run(std::uint64_t max_instructions) {
    const std::uint64_t end = max_instructions ? instructions_ + max_instructions : ~std::uint64_t{0};
    // Interrupt entries and idle ticks are steps that run no instruction;
    // the step limit keeps a CPU that idles forever from looping here.
    const std::uint64_t max_steps = max_instructions ? 4 * max_instructions + 16 : ~std::uint64_t{0};
    for (std::uint64_t steps = 0; instructions_ < end && steps < max_steps; ++steps) {
        const Stop s = step();
        if (s != Stop::None) return s;
    }
    return Stop::None;
}

}  // namespace oc166::sim
