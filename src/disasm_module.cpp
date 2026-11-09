// SPDX-License-Identifier: MIT
// disassemble_module(): a memory image as source that reassembles to the
// same bytes, checked with the assembler itself.
#include "assembler.hpp"
#include "disasm.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace oc166::dis {

namespace {

std::string db_line(const std::uint8_t* p, std::size_t n) {
    std::string s = "        DB      ";
    for (std::size_t i = 0; i < n; ++i) {
        if (i) s += ", ";
        char buf[8];
        std::snprintf(buf, sizeof buf, "0%02XH", p[i]);
        s += buf;
    }
    return s;
}

struct Generated {
    std::string text;
    // For each source line (1-based) that holds an instruction: its address.
    std::map<int, std::uint32_t> insn_lines;
    std::size_t instructions = 0, data_words = 0;
};

Generated generate(const std::uint8_t* bytes, std::size_t size, std::uint32_t base, isa::Cpu cpu,
                   const std::set<std::uint32_t>& as_data) {
    std::set<std::uint32_t> sfrs;
    Names names;
    names.sfr = [&](std::uint32_t a, bool) {
        sfrs.insert(a);
        return "S_" + hex(a).substr(0, hex(a).size() - 1);
    };
    FormatOptions fo;
    fo.reassemble = true;

    std::vector<std::string> body;
    std::map<std::size_t, std::uint32_t> body_insns;  // index in body -> address
    Generated g;
    ExtState ext;
    std::uint32_t segment = 0xFFFFFFFF;
    std::size_t off = 0;
    auto data = [&](std::size_t n) {
        body.push_back(db_line(bytes + off, n));
        off += n;
        ++g.data_words;
        ext = ExtState{};
    };
    while (off < size) {
        const std::uint32_t a = base + static_cast<std::uint32_t>(off);
        if ((a >> 16) != segment) {
            if (segment != 0xFFFFFFFF) body.push_back("SEG" + hex(segment) + " ENDS");
            segment = a >> 16;
            body.push_back("SEG" + hex(segment) + " SECTION CODE AT " + hex(a));
            ext = ExtState{};
        }
        const std::size_t seg_left = 0x10000 - (a & 0xFFFF);
        const std::size_t avail = std::min(size - off, seg_left);
        if (a & 1) {
            data(1);
            continue;
        }
        std::optional<Insn> insn;
        if (!as_data.count(a)) insn = decode(bytes + off, avail, cpu);
        if (!insn) {
            data(std::min<std::size_t>(2, avail));
            continue;
        }
        body_insns[body.size()] = a;
        body.push_back("        " + format(*insn, a, ext, names, fo));
        ext.advance(*insn);
        off += insn->size;
        ++g.instructions;
    }
    if (segment != 0xFFFFFFFF) body.push_back("SEG" + hex(segment) + " ENDS");

    std::string head = "$MOD167\n$SEGMENTED\n$NOMACRO\n";
    for (std::uint32_t a : sfrs) {
        std::string h = hex(a);
        head += "S_" + h.substr(0, h.size() - 1) + " DEFR " + h + "\n";
    }
    int line = 1;
    for (char c : head)
        if (c == '\n') ++line;
    g.text = head;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (auto it = body_insns.find(i); it != body_insns.end()) g.insn_lines[line] = it->second;
        g.text += body[i];
        g.text += '\n';
        ++line;
    }
    g.text += "        END\n";
    return g;
}

}  // namespace

ModuleText disassemble_module(const std::uint8_t* bytes, std::size_t size, std::uint32_t base, isa::Cpu cpu) {
    std::set<std::uint32_t> as_data;
    std::vector<std::uint32_t> fallbacks;
    for (int round = 0; round < 8; ++round) {
        Generated g = generate(bytes, size, base, cpu, as_data);
        AssembleOptions o;
        o.cpu = cpu;
        o.file_name = "<disassembly>";
        AssemblyResult r = assemble(g.text, o);
        std::size_t added = 0;
        for (const ListingLine& l : r.listing) {
            auto it = g.insn_lines.find(l.line);
            if (it == g.insn_lines.end()) continue;
            const std::uint32_t a = it->second;
            bool bad = false;
            for (std::size_t d : l.diagnostics)
                if (r.diagnostics[d].severity == Severity::Error) bad = true;
            if (!l.has_address || l.address != static_cast<std::int64_t>(a)) bad = true;
            for (std::size_t i = 0; !bad && i < l.bytes.size(); ++i)
                if (a - base + i >= size || l.bytes[i] != bytes[a - base + i]) bad = true;
            if (l.bytes.empty()) bad = true;
            if (bad && as_data.insert(a).second) {
                fallbacks.push_back(a);
                ++added;
            }
        }
        if (added == 0) {
            ModuleText m;
            m.text = std::move(g.text);
            m.instructions = g.instructions;
            m.data_words = g.data_words;
            m.fallbacks = fallbacks.size();
            m.fallback_addresses = std::move(fallbacks);
            return m;
        }
    }
    ModuleText m;
    Generated g = generate(bytes, size, base, cpu, as_data);
    m.text = std::move(g.text);
    m.instructions = g.instructions;
    m.data_words = g.data_words;
    m.fallbacks = fallbacks.size();
    m.fallback_addresses = std::move(fallbacks);
    return m;
}

}  // namespace oc166::dis
