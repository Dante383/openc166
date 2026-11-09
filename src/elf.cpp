// SPDX-License-Identifier: MIT
// ELF output with DWARF 4 debug information, for tools that read the
// industry formats (Ghidra, readelf, objdump, debuggers).
//
// Layout of the file:
//   ELF header, program headers (one PT_LOAD per run of bytes),
//   the bytes of each run, .debug_abbrev, .debug_info, .debug_line,
//   .debug_aranges, .debug_ranges, .symtab, .strtab, .shstrtab, and the
//   section header table.
#include "assembler.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace oc166 {

namespace {

constexpr std::uint16_t kEmC166 = 116;  // EM_C166: Infineon C16x/XC16x

// ELF constants.
constexpr std::uint32_t SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3;
constexpr std::uint32_t SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4;
constexpr std::uint32_t PT_LOAD = 1, PF_X = 1, PF_W = 2, PF_R = 4;
constexpr std::uint8_t STB_LOCAL = 0, STB_GLOBAL = 1, STT_NOTYPE = 0, STT_OBJECT = 1, STT_FILE = 4;
constexpr std::uint16_t SHN_ABS = 0xFFF1;

// DWARF constants.
constexpr std::uint8_t DW_TAG_compile_unit = 0x11, DW_TAG_label = 0x0a;
constexpr std::uint8_t DW_CHILDREN_no = 0, DW_CHILDREN_yes = 1;
constexpr std::uint8_t DW_AT_name = 0x03, DW_AT_stmt_list = 0x10, DW_AT_low_pc = 0x11, DW_AT_language = 0x13,
                       DW_AT_comp_dir = 0x1b, DW_AT_producer = 0x25, DW_AT_decl_file = 0x3a,
                       DW_AT_decl_line = 0x3b, DW_AT_external = 0x3f, DW_AT_ranges = 0x55;
constexpr std::uint8_t DW_FORM_addr = 0x01, DW_FORM_data2 = 0x05, DW_FORM_string = 0x08, DW_FORM_udata = 0x0f,
                       DW_FORM_sec_offset = 0x17, DW_FORM_flag_present = 0x19;
constexpr std::uint16_t DW_LANG_Mips_Assembler = 0x8001;
constexpr std::uint8_t DW_LNS_copy = 1, DW_LNS_advance_pc = 2, DW_LNS_advance_line = 3, DW_LNS_set_file = 4;
constexpr std::uint8_t DW_LNE_end_sequence = 1, DW_LNE_set_address = 2;

class Buf {
public:
    std::vector<std::uint8_t> b;
    void u8(std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v)); }
    void u16(std::uint32_t v) {
        u8(v);
        u8(v >> 8);
    }
    void u32(std::uint64_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint32_t>(v >> (8 * i)));
    }
    void uleb(std::uint64_t v) {
        do {
            std::uint8_t byte = v & 0x7F;
            v >>= 7;
            if (v) byte |= 0x80;
            b.push_back(byte);
        } while (v);
    }
    void sleb(std::int64_t v) {
        bool more = true;
        while (more) {
            std::uint8_t byte = v & 0x7F;
            v >>= 7;
            if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
                more = false;
            else
                byte |= 0x80;
            b.push_back(byte);
        }
    }
    void str(const std::string& s) {
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }
    void bytes(const std::vector<std::uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
    std::size_t size() const { return b.size(); }
    void patch32(std::size_t at, std::uint64_t v) {
        for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void align(std::size_t n) {
        while (b.size() % n) b.push_back(0);
    }
};

class StrTab {
public:
    StrTab() { buf.u8(0); }
    std::uint32_t add(const std::string& s) {
        if (s.empty()) return 0;
        auto it = index.find(s);
        if (it != index.end()) return it->second;
        const auto at = static_cast<std::uint32_t>(buf.size());
        buf.str(s);
        index.emplace(s, at);
        return at;
    }
    Buf buf;

private:
    std::map<std::string, std::uint32_t> index;
};

std::string hex_name(std::int64_t a) {
    char s[16];
    std::snprintf(s, sizeof s, "%06llX", static_cast<long long>(a));
    return s;
}

// A loadable piece: part of a run of bytes that lies in one assembler
// section (or in none).
struct Piece {
    std::int64_t address;
    std::vector<std::uint8_t> bytes;
    std::string name;
    std::uint32_t flags;
    std::uint16_t index = 0;  // ELF section index
};

// One row of the line table.
struct Row {
    std::int64_t address;
    std::int64_t size;
    int file;  // index in the module's file list (1-based)
    int line;
};

}  // namespace

std::vector<std::uint8_t> format_elf(const AssemblyResult& result) {
    // ---- Loadable pieces
    std::vector<const SectionInfo*> secs;
    for (const auto& s : result.sections)
        if (s.type != "BIT" && s.size > 0) secs.push_back(&s);
    std::sort(secs.begin(), secs.end(), [](auto* a, auto* b) { return a->address < b->address; });
    auto section_at = [&](std::int64_t a) -> const SectionInfo* {
        for (auto* s : secs)
            if (a >= s->address && a < s->address + s->size) return s;
        return nullptr;
    };
    std::vector<Chunk> runs = memory_runs(result);
    std::vector<Piece> pieces;
    for (const Chunk& run : runs) {
        if (run.address < 0 || run.address + static_cast<std::int64_t>(run.bytes.size()) > 0x1000000)
            throw std::invalid_argument("address " + hex_name(run.address) + " does not fit in 24 bits");
        std::size_t off = 0;
        while (off < run.bytes.size()) {
            const std::int64_t a = run.address + static_cast<std::int64_t>(off);
            const SectionInfo* s = section_at(a);
            std::int64_t end = run.address + static_cast<std::int64_t>(run.bytes.size());
            if (s) end = std::min(end, s->address + s->size);
            else
                for (auto* t : secs)
                    if (t->address > a) end = std::min(end, t->address);
            Piece p;
            p.address = a;
            p.bytes.assign(run.bytes.begin() + static_cast<std::ptrdiff_t>(off),
                           run.bytes.begin() + static_cast<std::ptrdiff_t>(end - run.address));
            if (s) {
                p.name = s->name;
                const bool code = s->type == "CODE" || s->type == "CONST" || s->type == "HCONST" || s->type == "XCONST";
                p.flags = SHF_ALLOC | (s->type == "CODE" ? SHF_EXECINSTR : 0) | (code ? 0 : SHF_WRITE);
            } else {
                p.name = ".abs_" + hex_name(a);
                p.flags = SHF_ALLOC | SHF_EXECINSTR;
            }
            off += static_cast<std::size_t>(end - a);
            pieces.push_back(std::move(p));
        }
    }
    auto piece_of = [&](std::int64_t a) -> int {
        for (std::size_t i = 0; i < pieces.size(); ++i)
            if (a >= pieces[i].address && a < pieces[i].address + static_cast<std::int64_t>(pieces[i].bytes.size()))
                return static_cast<int>(i);
        return -1;
    };

    // ---- DWARF, one compile unit per module
    Buf abbrev, info, line, aranges, ranges;
    abbrev.uleb(1);
    abbrev.uleb(DW_TAG_compile_unit);
    abbrev.u8(DW_CHILDREN_yes);
    for (auto [at, form] : {std::pair{DW_AT_producer, DW_FORM_string}, {DW_AT_language, DW_FORM_data2},
                            {DW_AT_name, DW_FORM_string}, {DW_AT_comp_dir, DW_FORM_string},
                            {DW_AT_stmt_list, DW_FORM_sec_offset}, {DW_AT_low_pc, DW_FORM_addr},
                            {DW_AT_ranges, DW_FORM_sec_offset}}) {
        abbrev.uleb(at);
        abbrev.uleb(form);
    }
    abbrev.u16(0);
    for (int external = 0; external < 2; ++external) {
        abbrev.uleb(2 + static_cast<std::uint64_t>(external));
        abbrev.uleb(DW_TAG_label);
        abbrev.u8(DW_CHILDREN_no);
        for (auto [at, form] : {std::pair{DW_AT_name, DW_FORM_string}, {DW_AT_decl_file, DW_FORM_udata},
                                {DW_AT_decl_line, DW_FORM_udata}, {DW_AT_low_pc, DW_FORM_addr}}) {
            abbrev.uleb(at);
            abbrev.uleb(form);
        }
        if (external) {
            abbrev.uleb(DW_AT_external);
            abbrev.uleb(DW_FORM_flag_present);
        }
        abbrev.u16(0);
    }
    abbrev.u8(0);

    // Listing lines by module, and the line of each symbol's definition.
    std::vector<std::map<int, const ListingLine*>> by_line(result.modules.size());
    for (const ListingLine& l : result.listing)
        if (l.module >= 0 && static_cast<std::size_t>(l.module) < by_line.size()) by_line[static_cast<std::size_t>(l.module)][l.line] = &l;

    for (std::size_t m = 0; m < result.modules.size(); ++m) {
        // Files and line rows of this module.
        std::vector<std::string> files;
        std::map<std::string, int> file_index;
        auto file_of = [&](const std::string& name) {
            auto [it, added] = file_index.try_emplace(name, static_cast<int>(files.size()) + 1);
            if (added) files.push_back(name);
            return it->second;
        };
        file_of(result.modules[m]);
        std::vector<Row> rows;
        for (const ListingLine& l : result.listing) {
            if (l.module != static_cast<int>(m) || !l.has_address || l.bit_address || l.bytes.empty() || l.skipped)
                continue;
            const bool macro = l.macro_level > 0 && !l.macro_file.empty() && l.macro_line > 0;
            rows.push_back({l.address, static_cast<std::int64_t>(l.bytes.size()),
                            file_of(macro ? l.macro_file : l.file), macro ? l.macro_line : l.file_line});
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.address < b.address; });
        // Where each label of the module was defined (files a label names
        // must be in the line table's file list too).
        struct Label {
            const Symbol* sym;
            int file;
            int line;
        };
        std::vector<Label> labels;
        for (const Symbol& s : result.symbols) {
            if (s.module != static_cast<int>(m) || s.kind != SymbolKind::Label || !s.value.known ||
                s.value.type != VType::Number)
                continue;
            int decl_file = 1, decl_line = 0;
            auto it = by_line[m].find(s.line);
            if (it != by_line[m].end()) {
                const ListingLine& l = *it->second;
                const bool macro = l.macro_level > 0 && !l.macro_file.empty();
                decl_file = file_of(macro ? l.macro_file : l.file);
                decl_line = macro ? l.macro_line : l.file_line;
            }
            labels.push_back({&s, decl_file, decl_line});
        }

        // .debug_line program.
        const std::size_t line_start = line.size();
        line.u32(0);  // unit_length
        line.u16(4);  // version
        const std::size_t header_length_at = line.size();
        line.u32(0);
        const std::size_t header_start = line.size();
        line.u8(1);    // minimum_instruction_length
        line.u8(1);    // maximum_operations_per_instruction
        line.u8(1);    // default_is_stmt
        line.u8(251);  // line_base -5
        line.u8(14);   // line_range
        line.u8(13);   // opcode_base
        for (int n : {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1}) line.u8(static_cast<std::uint32_t>(n));
        line.u8(0);  // no include_directories: file names are as the assembler saw them
        for (const auto& f : files) {
            line.str(f);
            line.uleb(0);
            line.uleb(0);
            line.uleb(0);
        }
        line.u8(0);
        line.patch32(header_length_at, line.size() - header_start);
        std::int64_t addr = 0, end = -1;
        int file = 1, ln = 1;
        std::vector<std::pair<std::int64_t, std::int64_t>> spans;
        auto end_sequence = [&] {
            if (end < 0) return;
            if (end > addr) {
                line.u8(DW_LNS_advance_pc);
                line.uleb(static_cast<std::uint64_t>(end - addr));
            }
            line.u8(0);
            line.uleb(1);
            line.u8(DW_LNE_end_sequence);
            file = 1;
            ln = 1;
        };
        for (const Row& r : rows) {
            if (r.address != end) {
                end_sequence();
                line.u8(0);
                line.uleb(5);
                line.u8(DW_LNE_set_address);
                line.u32(static_cast<std::uint64_t>(r.address));
                addr = r.address;
                spans.push_back({r.address, r.address});
            } else if (r.address > addr) {
                line.u8(DW_LNS_advance_pc);
                line.uleb(static_cast<std::uint64_t>(r.address - addr));
                addr = r.address;
            }
            if (r.file != file) {
                line.u8(DW_LNS_set_file);
                line.uleb(static_cast<std::uint64_t>(r.file));
                file = r.file;
            }
            if (r.line != ln) {
                line.u8(DW_LNS_advance_line);
                line.sleb(r.line - ln);
                ln = r.line;
            }
            line.u8(DW_LNS_copy);
            end = r.address + r.size;
            spans.back().second = end;
        }
        end_sequence();
        line.patch32(line_start, line.size() - line_start - 4);

        // .debug_ranges for the unit.
        const std::size_t ranges_at = ranges.size();
        for (auto [lo, hi] : spans) {
            ranges.u32(static_cast<std::uint64_t>(lo));
            ranges.u32(static_cast<std::uint64_t>(hi));
        }
        ranges.u32(0);
        ranges.u32(0);

        // .debug_info: the unit and its labels.
        const std::size_t info_start = info.size();
        info.u32(0);
        info.u16(4);
        info.u32(0);  // abbrev offset
        info.u8(4);   // address size
        info.uleb(1);
        info.str("openc166 " + std::string(OPENC166_VERSION_STRING));
        info.u16(DW_LANG_Mips_Assembler);
        info.str(result.modules[m]);
        info.str(".");
        info.u32(line_start);
        info.u32(0);  // low_pc: the ranges are absolute
        info.u32(ranges_at);
        for (const Label& lb : labels) {
            info.uleb(lb.sym->exported ? 3 : 2);
            info.str(lb.sym->name);
            info.uleb(static_cast<std::uint64_t>(lb.file));
            info.uleb(static_cast<std::uint64_t>(lb.line));
            info.u32(static_cast<std::uint64_t>(lb.sym->value.num) & 0xFFFFFF);
        }
        info.u8(0);  // end of children
        info.patch32(info_start, info.size() - info_start - 4);

        const std::size_t ar_start = aranges.size();
        aranges.u32(0);
        aranges.u16(2);
        aranges.u32(info_start);
        aranges.u8(4);
        aranges.u8(0);
        aranges.u32(0);  // pad the header to 16 bytes
        for (auto [lo, hi] : spans) {
            aranges.u32(static_cast<std::uint64_t>(lo));
            aranges.u32(static_cast<std::uint64_t>(hi - lo));
        }
        aranges.u32(0);
        aranges.u32(0);
        aranges.patch32(ar_start, aranges.size() - ar_start - 4);
    }

    // ---- Symbols
    StrTab strtab;
    struct Sym {
        std::uint32_t name;
        std::uint32_t value;
        std::uint32_t size;
        std::uint8_t info;
        std::uint16_t shndx;
    };
    std::vector<Sym> locals{{0, 0, 0, 0, 0}}, globals;
    for (const auto& mod : result.modules) locals.push_back({strtab.add(mod), 0, 0, STT_FILE, SHN_ABS});
    // Section indices: 1.. for the pieces, then the debug sections etc.
    for (std::size_t i = 0; i < pieces.size(); ++i) pieces[i].index = static_cast<std::uint16_t>(1 + i);
    for (const Symbol& s : result.symbols) {
        if (s.kind != SymbolKind::Label || !s.value.known || s.value.type != VType::Number) continue;
        const std::int64_t a = s.value.num & 0xFFFFFF;
        const int p = piece_of(a);
        std::uint8_t type = STT_NOTYPE;
        std::uint32_t size = 0;
        switch (s.value.ltype) {
        case LType::Byte: type = STT_OBJECT; size = 1; break;
        case LType::Word: type = STT_OBJECT; size = 2; break;
        case LType::Dword: type = STT_OBJECT; size = 4; break;
        default: break;
        }
        Sym sym{strtab.add(s.name), static_cast<std::uint32_t>(a), size, 0,
                p >= 0 ? pieces[static_cast<std::size_t>(p)].index : SHN_ABS};
        sym.info = static_cast<std::uint8_t>(((s.exported ? STB_GLOBAL : STB_LOCAL) << 4) | type);
        (s.exported ? globals : locals).push_back(sym);
    }
    Buf symtab;
    for (const auto* list : {&locals, &globals})
        for (const Sym& s : *list) {
            symtab.u32(s.name);
            symtab.u32(s.value);
            symtab.u32(s.size);
            symtab.u8(s.info);
            symtab.u8(0);
            symtab.u16(s.shndx);
        }

    // ---- Assemble the file
    StrTab shstr;
    struct Shdr {
        std::uint32_t name, type, flags, addr, offset, size, link, info, align, entsize;
    };
    std::vector<Shdr> shdrs{{}};
    const std::uint32_t ehsize = 52, phentsize = 32;
    Buf out;
    out.b.resize(ehsize + phentsize * runs.size(), 0);
    // Program headers: one per run; the file offset is that of its first piece.
    std::vector<std::uint32_t> run_offset(runs.size());
    std::size_t next_piece = 0;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        run_offset[r] = static_cast<std::uint32_t>(out.size());
        const std::int64_t run_end = runs[r].address + static_cast<std::int64_t>(runs[r].bytes.size());
        while (next_piece < pieces.size() && pieces[next_piece].address < run_end) {
            Piece& p = pieces[next_piece++];
            shdrs.push_back({shstr.add(p.name), SHT_PROGBITS, p.flags, static_cast<std::uint32_t>(p.address),
                             static_cast<std::uint32_t>(out.size()), static_cast<std::uint32_t>(p.bytes.size()), 0, 0,
                             1, 0});
            out.bytes(p.bytes);
        }
    }
    auto add_section = [&](const char* name, std::uint32_t type, const Buf& data, std::uint32_t link = 0,
                           std::uint32_t inf = 0, std::uint32_t align = 1, std::uint32_t entsize = 0) {
        if (align > 1) out.align(align);
        shdrs.push_back({shstr.add(name), type, 0, 0, static_cast<std::uint32_t>(out.size()),
                         static_cast<std::uint32_t>(data.size()), link, inf, align, entsize});
        out.bytes(data.b);
        return static_cast<std::uint32_t>(shdrs.size() - 1);
    };
    add_section(".debug_abbrev", SHT_PROGBITS, abbrev);
    add_section(".debug_info", SHT_PROGBITS, info);
    add_section(".debug_line", SHT_PROGBITS, line);
    add_section(".debug_aranges", SHT_PROGBITS, aranges);
    add_section(".debug_ranges", SHT_PROGBITS, ranges);
    const std::uint32_t strtab_index = static_cast<std::uint32_t>(shdrs.size() + 1);
    add_section(".symtab", SHT_SYMTAB, symtab, strtab_index, static_cast<std::uint32_t>(locals.size()), 4, 16);
    add_section(".strtab", SHT_STRTAB, strtab.buf);
    const std::uint32_t shstrndx = static_cast<std::uint32_t>(shdrs.size());
    shstr.add(".shstrtab");
    shdrs.push_back({shstr.add(".shstrtab"), SHT_STRTAB, 0, 0, static_cast<std::uint32_t>(out.size()),
                     static_cast<std::uint32_t>(shstr.buf.size()), 0, 0, 1, 0});
    out.bytes(shstr.buf.b);
    out.align(4);
    const auto shoff = static_cast<std::uint32_t>(out.size());
    for (const Shdr& h : shdrs)
        for (std::uint32_t v : {h.name, h.type, h.flags, h.addr, h.offset, h.size, h.link, h.info, h.align, h.entsize})
            out.u32(v);

    // ELF header.
    Buf eh;
    for (std::uint32_t c : {0x7Fu, 0x45u, 0x4Cu, 0x46u, 1u /* 32-bit */, 1u /* little endian */, 1u /* version */})
        eh.u8(c);
    while (eh.size() < 16) eh.u8(0);
    eh.u16(2);  // ET_EXEC
    eh.u16(kEmC166);
    eh.u32(1);
    eh.u32(0);  // entry: the reset vector at 0
    eh.u32(runs.empty() ? 0 : ehsize);
    eh.u32(shoff);
    eh.u32(0);  // flags
    eh.u16(ehsize);
    eh.u16(runs.empty() ? 0 : phentsize);
    eh.u16(static_cast<std::uint32_t>(runs.size()));
    eh.u16(40);
    eh.u16(static_cast<std::uint32_t>(shdrs.size()));
    eh.u16(shstrndx);
    std::copy(eh.b.begin(), eh.b.end(), out.b.begin());
    for (std::size_t r = 0; r < runs.size(); ++r) {
        Buf ph;
        // Executable if it holds code, writable if it holds data.
        std::uint32_t flags = PF_R;
        for (const Piece& p : pieces)
            if (p.address >= runs[r].address && p.address < runs[r].address + static_cast<std::int64_t>(runs[r].bytes.size()))
                flags |= ((p.flags & SHF_EXECINSTR) ? PF_X : 0) | ((p.flags & SHF_WRITE) ? PF_W : 0);
        for (std::uint32_t v : {PT_LOAD, run_offset[r], static_cast<std::uint32_t>(runs[r].address),
                                static_cast<std::uint32_t>(runs[r].address), static_cast<std::uint32_t>(runs[r].bytes.size()),
                                static_cast<std::uint32_t>(runs[r].bytes.size()), flags, 1u})
            ph.u32(v);
        std::copy(ph.b.begin(), ph.b.end(), out.b.begin() + ehsize + static_cast<std::ptrdiff_t>(r * phentsize));
    }
    return out.b;
}

}  // namespace oc166
