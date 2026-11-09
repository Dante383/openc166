// SPDX-License-Identifier: MIT
#include "disasm.hpp"

#include "bundled.hpp"
#include "isa/isa.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace oc166::dis {

namespace {

using isa::Cls;
using isa::Form;
using isa::Sub;

// Forms whose first byte can match each byte value, in table order.
const std::array<std::vector<const Form*>, 256>& first_byte_index() {
    static const auto table = [] {
        std::array<std::vector<const Form*>, 256> t;
        for (const Form& f : isa::all_forms()) {
            const auto& tb = f.bytes[0];
            for (int b = 0; b < 256; ++b) {
                int shift = 8;
                bool ok = true;
                for (int i = 0; i < tb.count && ok; ++i) {
                    const auto& p = tb.parts[i];
                    shift -= p.width;
                    if (p.operand == isa::kLiteral) {
                        unsigned mask = (1u << p.width) - 1;
                        ok = ((static_cast<unsigned>(b) >> shift) & mask) == p.literal;
                    }
                }
                if (ok) t[static_cast<std::size_t>(b)].push_back(&f);
            }
        }
        return t;
    }();
    return table;
}

int literal_bits(const Form& f) {
    int n = 0;
    for (int i = 0; i < f.size; ++i)
        for (int j = 0; j < f.bytes[i].count; ++j)
            if (f.bytes[i].parts[j].operand == isa::kLiteral) n += f.bytes[i].parts[j].width;
    return n;
}

// Matches 'f' against the bytes; fills 'out' on success.
bool match(const Form& f, const std::uint8_t* bytes, std::size_t avail, Insn& out) {
    if (avail < f.size) return false;
    // Per operand and sub-field: the bits seen so far, to check fields that
    // appear twice (DIV's n:n).
    struct Field {
        std::uint32_t full = 0, lo = 0, hi = 0, off = 0, bit = 0, reg = 0;
        std::uint8_t seen = 0;
    } fields[3];
    for (int i = 0; i < f.size; ++i) {
        const auto& tb = f.bytes[i];
        int shift = 8;
        for (int j = 0; j < tb.count; ++j) {
            const auto& p = tb.parts[j];
            shift -= p.width;
            const std::uint32_t mask = (1u << p.width) - 1;
            const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) >> shift) & mask;
            if (p.operand == isa::kLiteral) {
                if (v != p.literal) return false;
                continue;
            }
            Field& fl = fields[p.operand];
            std::uint32_t* slot = nullptr;
            std::uint8_t flag = 0;
            switch (p.sub) {
            case Sub::Full: slot = &fl.full; flag = 1; break;
            case Sub::Lo: slot = &fl.lo; flag = 2; break;
            case Sub::Hi: slot = &fl.hi; flag = 4; break;
            case Sub::Off: slot = &fl.off; flag = 8; break;
            case Sub::Bit: slot = &fl.bit; flag = 16; break;
            case Sub::Reg: slot = &fl.reg; flag = 32; break;
            }
            if (fl.seen & flag) {
                if (*slot != v) return false;
            } else {
                *slot = v;
                fl.seen |= flag;
            }
        }
    }
    out = Insn{};
    out.form = &f;
    out.size = f.size;
    out.nops = f.nops;
    for (int i = 0; i < f.size; ++i) out.bytes[i] = bytes[i];
    for (int i = 0; i < f.nops; ++i) {
        const Field& fl = fields[i];
        Operand& op = out.ops[i];
        op.cls = f.ops[i];
        const std::uint32_t word = fl.lo | (fl.hi << 8);
        switch (op.cls) {
        case Cls::IndDisp:
            op.value = static_cast<std::int32_t>(word);
            op.aux = static_cast<std::uint8_t>(fl.reg);
            break;
        case Cls::BitAddr:
            op.value = static_cast<std::int32_t>(fl.off);
            op.aux = static_cast<std::uint8_t>(fl.bit);
            break;
        case Cls::Mem:
        case Cls::D16:
        case Cls::Caddr:
            op.value = static_cast<std::int32_t>(word);
            break;
        case Cls::Pag10:
            op.value = static_cast<std::int32_t>(word & 0x3FF);
            if (word > 0x3FF) return false;
            break;
        case Cls::Irang2:
            op.value = static_cast<std::int32_t>(fl.full) + 1;
            break;
        case Cls::Rel:
            op.value = static_cast<std::int8_t>(static_cast<std::uint8_t>(fl.full));
            break;
        default:
            op.value = static_cast<std::int32_t>(fl.full);
            break;
        }
    }
    return true;
}

bool is_ext(std::string_view m) {
    return m == "EXTR" || m == "EXTP" || m == "EXTPR" || m == "EXTS" || m == "EXTSR" || m == "ATOMIC";
}

const char* const kCc[16] = {"cc_UC", "cc_NET", "cc_Z",   "cc_NZ",  "cc_V",   "cc_NV",  "cc_N",   "cc_NN",
                             "cc_C",  "cc_NC",  "cc_SGT", "cc_SLE", "cc_SLT", "cc_SGE", "cc_UGT", "cc_ULE"};

std::string gpr(int n) { return "R" + std::to_string(n); }
std::string gpr_byte(int n) { return (n & 1 ? "RH" : "RL") + std::to_string(n >> 1); }

// Name tables built from REG167.INC.
struct NameTables {
    std::map<std::uint32_t, std::string> sfrs;
    std::map<std::pair<std::uint32_t, int>, std::string> bits;
};

const NameTables& c167_tables() {
    static const NameTables tables = [] {
        NameTables t;
        // Registers of the C167CS that REG167.INC does not name.
        const std::pair<std::uint32_t, const char*> extra[] = {
            {0xF024, "XPERCON"}, {0xFE0C, "MDH"}, {0xFF0E, "MDC"}, {0xFF1E, "ONES"},
        };
        for (const auto& [a, n] : extra) t.sfrs.emplace(a, n);
        auto file = bundled_file("REG167.INC");
        if (!file) return t;
        std::string_view text = file->text;
        std::map<std::string, std::uint32_t> by_name;
        for (auto& [a, n] : t.sfrs) by_name[n] = a;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t eol = text.find('\n', pos);
            if (eol == std::string_view::npos) eol = text.size();
            std::string line(text.substr(pos, eol - pos));
            pos = eol + 1;
            if (auto c = line.find(';'); c != std::string::npos) line.resize(c);
            char name[64], kind[16], arg[64];
            if (std::sscanf(line.c_str(), "%63s %15s %63s", name, kind, arg) != 3) continue;
            std::string k = kind;
            if (k == "DEFR") {
                unsigned v = 0;
                if (std::sscanf(arg, "%xH", &v) != 1 && std::sscanf(arg, "%x", &v) != 1) continue;
                t.sfrs.emplace(v, name);  // the first name of an address wins
                by_name[name] = v;
            } else if (k == "BIT") {
                std::string a = arg;
                auto dot = a.find('.');
                if (dot == std::string::npos) continue;
                auto it = by_name.find(a.substr(0, dot));
                if (it == by_name.end()) continue;
                t.bits.emplace(std::make_pair(it->second, std::atoi(a.c_str() + dot + 1)), name);
            }
        }
        return t;
    }();
    return tables;
}

}  // namespace

std::optional<Insn> decode(const std::uint8_t* bytes, std::size_t avail, isa::Cpu cpu) {
    if (avail < 2) return std::nullopt;
    std::optional<Insn> best;
    int best_bits = -1;
    for (const Form* f : first_byte_index()[bytes[0]]) {
        if (f->cpu == isa::Cpu::C167 && cpu != isa::Cpu::C167) continue;
        Insn insn;
        if (!match(*f, bytes, avail, insn)) continue;
        // Where encodings overlap (JMPR cc_UC and JMPR without a
        // condition), the form with more fixed bits is the more specific.
        const int bits = literal_bits(*f);
        if (bits > best_bits) {
            best = insn;
            best_bits = bits;
        }
    }
    return best;
}

void ExtState::advance(const Insn& insn) {
    const std::string_view m = insn.mnemonic();
    if (is_ext(m)) {
        *this = ExtState{};
        // The count is the last operand.
        left = insn.ops[insn.nops - 1].value;
        atomic = m == "ATOMIC";
        esfr = m == "EXTR" || m == "EXTPR" || m == "EXTSR";
        if (m.starts_with("EXTP")) kind = Kind::Page;
        if (m.starts_with("EXTS")) kind = Kind::Seg;
        if (kind != Kind::None) {
            if (insn.ops[0].cls == Cls::Rw)
                reg = insn.ops[0].value;
            else
                value = static_cast<std::uint32_t>(insn.ops[0].value);
        }
        return;
    }
    if (left > 0 && --left == 0) *this = ExtState{};
}

std::uint32_t reg_address(std::uint8_t r, bool esfr) {
    if (r >= 0xF0) return r;
    return (esfr ? 0xF000u : 0xFE00u) + 2u * r;
}

std::uint32_t bitoff_address(std::uint8_t q, bool esfr) {
    if (q >= 0xF0) return q;
    if (q < 0x80) return 0xFD00u + 2u * q;
    return (esfr ? 0xF100u : 0xFF00u) + 2u * (q - 0x80u);
}

const Names& c167_names() {
    static const Names names = [] {
        Names n;
        n.sfr = [](std::uint32_t a, bool) -> std::string {
            const auto& t = c167_tables().sfrs;
            auto it = t.find(a);
            return it == t.end() ? std::string{} : it->second;
        };
        n.bit = [](std::uint32_t a, int b) -> std::string {
            const auto& t = c167_tables().bits;
            auto it = t.find({a, b});
            return it == t.end() ? std::string{} : it->second;
        };
        return n;
    }();
    return names;
}

std::string hex(std::uint32_t v) {
    if (v < 10) return std::to_string(v);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%XH", v);
    std::string s = buf;
    if (!std::isdigit(static_cast<unsigned char>(s[0]))) s.insert(s.begin(), '0');
    return s;
}

std::optional<std::uint32_t> branch_target(const Insn& insn, std::uint32_t address) {
    for (int i = 0; i < insn.nops; ++i) {
        const Operand& op = insn.ops[i];
        if (op.cls == Cls::Rel) {
            const std::uint32_t ip = (address + insn.size + 2 * static_cast<std::uint32_t>(op.value)) & 0xFFFF;
            return (address & 0xFF0000) | ip;
        }
        if (op.cls == Cls::Caddr) {
            if (i > 0 && insn.ops[0].cls == Cls::Seg)
                return (static_cast<std::uint32_t>(insn.ops[0].value) << 16) | static_cast<std::uint32_t>(op.value);
            return (address & 0xFF0000) | static_cast<std::uint32_t>(op.value);
        }
    }
    return std::nullopt;
}

std::string format(const Insn& insn, std::uint32_t address, const ExtState& ext, const Names& names,
                   const FormatOptions& options) {
    if (!insn.form) return {};
    auto lookup = [](const std::function<std::string(std::uint32_t, bool)>& f, std::uint32_t a, bool b) {
        return f ? f(a, b) : std::string{};
    };
    // A word register at a short address: a GPR, a named SFR, or a number
    // (which only names a 'mem' operand, so 'reg' needs a name).
    auto reg_text = [&](std::uint8_t r, bool byte) -> std::string {
        if (r >= 0xF0) return byte ? gpr_byte(r & 0xF) : gpr(r & 0xF);
        const std::uint32_t a = reg_address(r, ext.esfr);
        std::string n = lookup(names.sfr, a, byte);
        return n.empty() ? hex(a) : n;
    };
    auto word_text = [&](std::uint8_t q) -> std::string {
        if (q >= 0xF0) return gpr(q & 0xF);
        const std::uint32_t a = bitoff_address(q, ext.esfr);
        std::string n = q >= 0x80 ? lookup(names.sfr, a, false) : std::string{};
        return n.empty() ? hex(a) : n;
    };
    auto bit_text = [&](std::uint8_t q, int bit) -> std::string {
        if (q < 0xF0 && names.bit && !options.reassemble) {
            std::string n = names.bit(bitoff_address(q, ext.esfr), bit);
            if (!n.empty()) return n;
        }
        return word_text(q) + "." + std::to_string(bit);
    };
    auto code_text = [&](std::uint32_t target) -> std::string {
        if (names.code && !options.reassemble) {
            std::string n = names.code(target);
            if (!n.empty()) return n;
        }
        return hex(target);
    };

    const std::string_view m = insn.mnemonic();
    std::string out(m);
    std::string ops;
    for (int i = 0; i < insn.nops; ++i) {
        const Operand& op = insn.ops[i];
        const std::uint32_t v = static_cast<std::uint32_t>(op.value);
        std::string t;
        switch (op.cls) {
        case Cls::None: break;
        case Cls::Rw: t = gpr(op.value); break;
        case Cls::Rb: t = gpr_byte(op.value); break;
        case Cls::Ind:
        case Cls::IndI: t = "[" + gpr(op.value) + "]"; break;
        case Cls::PostInc:
        case Cls::PostIncI: t = "[" + gpr(op.value) + "+]"; break;
        case Cls::PreDec: t = "[-" + gpr(op.value) + "]"; break;
        case Cls::IndDisp: t = "[" + gpr(op.aux) + "+#" + hex(v) + "]"; break;
        case Cls::Reg: t = reg_text(static_cast<std::uint8_t>(v), false); break;
        case Cls::RegB: t = reg_text(static_cast<std::uint8_t>(v), true); break;
        case Cls::Mem: {
            std::string n;
            if (names.data && !options.reassemble) n = names.data(v);
            if (n.empty() && !options.reassemble && names.sfr && ext.kind == ExtState::Kind::None) {
                // Direct accesses to SFRs (through DPP3 = 3, the reset value).
                if ((v >= 0xFE00 && v <= 0xFFFF) || (v >= 0xF000 && v <= 0xF1FF)) n = names.sfr(v & ~1u, false);
                if (!n.empty() && (v & 1)) n += "+1";
            }
            t = n.empty() ? hex(v) : n;
            break;
        }
        case Cls::BitOff: t = word_text(static_cast<std::uint8_t>(v)); break;
        case Cls::BitAddr: t = bit_text(static_cast<std::uint8_t>(v), op.aux); break;
        case Cls::D16:
            t = std::string("#") + (options.reassemble ? "DATA16 " : "") + hex(v);
            break;
        case Cls::D8:
            // Byte immediates that a #d4 form could also hold.
            t = std::string("#") + (options.reassemble && insn.nops == 2 ? "DATA8 " : "") + hex(v);
            break;
        case Cls::D3:
        case Cls::D4:
        case Cls::M8:
        case Cls::Trap7:
        case Cls::Pag10:
        case Cls::Seg8:
        case Cls::Seg: t = (op.cls == Cls::Seg ? "" : "#") + hex(v); break;
        case Cls::Irang2: t = "#" + std::to_string(op.value); break;
        case Cls::Cc: t = kCc[v & 15]; break;
        case Cls::Rel:
        case Cls::Caddr: t = code_text(*branch_target(insn, address)); break;
        }
        if (i) ops += ", ";
        ops += t;
    }
    if (!ops.empty()) {
        out.resize(std::max<std::size_t>(out.size() + 1, 8), ' ');
        out += ops;
    }
    if (!options.upper) {
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace oc166::dis
