// SPDX-License-Identifier: MIT
// Debug information: a JSON map from addresses to source lines, symbols and
// sections, read by the simulator's debug core. See docs/debugger-design.md
// (3.1) and format_debug_info() in assembler.hpp for the format.
#include "assembler.hpp"
#include "predef.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace oc166 {

namespace {

void json_string(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            // Control characters, and bytes above ASCII: sources are often
            // Windows-1252, so they are written as Latin-1 code points and
            // the JSON stays valid UTF-8.
            if (c < 0x20 || c >= 0x80) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

const char* kind_name(SymbolKind k) {
    switch (k) {
    case SymbolKind::Label: return "label";
    case SymbolKind::Equ: return "equ";
    case SymbolKind::Set: return "set";
    case SymbolKind::Bit: return "bit";
    case SymbolKind::Lit: return "lit";
    case SymbolKind::Extern: return "extern";
    }
    return "";
}

const char* ltype_name(LType t) {
    switch (t) {
    case LType::None: return "";
    case LType::Near: return "near";
    case LType::Far: return "far";
    case LType::Byte: return "byte";
    case LType::Word: return "word";
    case LType::Dword: return "dword";
    }
    return "";
}

const char* vtype_name(VType t) {
    switch (t) {
    case VType::Number: return "number";
    case VType::Gpr: return "gpr";
    case VType::GprB: return "gpr_byte";
    case VType::Sfr: return "sfr";
    case VType::Bit: return "bit";
    case VType::Cc: return "cc";
    }
    return "";
}

class FileTable {
public:
    int index(const std::string& name) {
        auto [it, added] = ids_.try_emplace(name, static_cast<int>(names_.size()));
        if (added) names_.push_back(name);
        return it->second;
    }
    const std::vector<std::string>& names() const { return names_; }

private:
    std::map<std::string, int> ids_;
    std::vector<std::string> names_;
};

}  // namespace

std::string format_debug_info(const AssemblyResult& result) {
    // Sections other than BIT sections, by address, to tag lines and labels.
    auto section_of = [&](std::int64_t address) -> int {
        for (std::size_t i = 0; i < result.sections.size(); ++i) {
            const SectionInfo& s = result.sections[i];
            if (s.type == "BIT") continue;
            if (address >= s.address && address < s.address + std::max<std::int64_t>(s.size, 1)) return static_cast<int>(i);
        }
        return -1;
    };

    FileTable files;
    for (const auto& m : result.modules) files.index(m);
    std::string lines;
    // The last line seen at each include level of the current module: the
    // $INCLUDE line of the next level up.
    std::vector<std::pair<int, int>> last;
    int module = -1;
    bool first = true;
    for (const ListingLine& l : result.listing) {
        if (l.module != module) {
            module = l.module;
            last.clear();
        }
        const int file = files.index(l.file);
        if (static_cast<int>(last.size()) <= l.level) last.resize(static_cast<std::size_t>(l.level) + 1, {-1, 0});
        last[static_cast<std::size_t>(l.level)] = {file, l.file_line};
        if (!l.has_address || l.bit_address || l.bytes.empty() || l.skipped) continue;
        if (!first) lines += ",\n";
        first = false;
        lines += "    {\"address\": " + std::to_string(l.address) + ", \"size\": " + std::to_string(l.bytes.size()) +
                 ", \"module\": " + std::to_string(l.module) + ", \"file\": " + std::to_string(file) +
                 ", \"line\": " + std::to_string(l.file_line) + ", \"section\": " + std::to_string(section_of(l.address));
        if (l.level > 0) {
            lines += ", \"include\": [";
            for (int i = 0; i < l.level; ++i) {
                if (i) lines += ", ";
                const auto& [f, n] = last[static_cast<std::size_t>(i)];
                lines += "[" + std::to_string(f) + ", " + std::to_string(n) + "]";
            }
            lines += "]";
        }
        if (l.macro_level > 0 && !l.macro_file.empty()) {
            lines += ", \"macro\": {\"file\": " + std::to_string(files.index(l.macro_file)) +
                     ", \"line\": " + std::to_string(l.macro_line) + ", \"depth\": " + std::to_string(l.macro_level) +
                     "}";
        }
        lines += "}";
    }

    std::string out = "{\n  \"format\": \"openc166-debug\",\n  \"version\": 1,\n  \"cpu\": ";
    json_string(out, result.cpu == isa::Cpu::C167 ? "c167" : "c166");
    out += ",\n  \"modules\": [";
    for (std::size_t i = 0; i < result.modules.size(); ++i) {
        if (i) out += ", ";
        json_string(out, result.modules[i]);
    }
    out += "],\n  \"files\": [";
    const auto& names = files.names();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        json_string(out, names[i]);
    }
    out += "],\n  \"sections\": [\n";
    for (std::size_t i = 0; i < result.sections.size(); ++i) {
        const SectionInfo& s = result.sections[i];
        out += "    {\"name\": ";
        json_string(out, s.name);
        out += ", \"type\": ";
        json_string(out, s.type);
        out += ", \"align\": ";
        json_string(out, s.align);
        out += ", \"address\": " + std::to_string(s.address) + ", \"size\": " + std::to_string(s.size);
        if (s.type == "BIT") out += ", \"bit\": " + std::to_string(s.bit) + ", \"bit_size\": " + std::to_string(s.bit_size);
        out += ", \"class\": ";
        json_string(out, s.class_name);
        out += ", \"group\": ";
        json_string(out, s.group);
        out += i + 1 < result.sections.size() ? "},\n" : "}\n";
    }
    out += "  ],\n  \"symbols\": [\n";
    first = true;
    for (const Symbol& s : result.symbols) {
        if (s.kind == SymbolKind::Lit) continue;
        if (!first) out += ",\n";
        first = false;
        const Value& v = s.value;
        out += "    {\"name\": ";
        json_string(out, s.name);
        out += ", \"kind\": \"";
        out += kind_name(s.kind);
        out += "\", \"type\": \"";
        out += vtype_name(v.type);
        out += "\"";
        if (!v.known) {
            out += ", \"value\": null";
        } else if (v.type == VType::Bit) {
            out += ", \"value\": " + std::to_string(bit_word_address(v)) + ", \"bit\": " + std::to_string(v.bit);
        } else {
            out += ", \"value\": " + std::to_string(v.num);
        }
        if (v.ltype != LType::None) {
            out += ", \"label_type\": \"";
            out += ltype_name(v.ltype);
            out += "\"";
        }
        if (s.kind == SymbolKind::Extern) {
            out += ", \"extern_type\": ";
            json_string(out, s.text);
        }
        out += ", \"module\": " + std::to_string(s.module);
        if (s.exported) out += ", \"exported\": true";
        if (s.kind == SymbolKind::Label && v.known && v.type == VType::Number)
            out += ", \"section\": " + std::to_string(section_of(v.num));
        out += "}";
    }
    out += "\n  ],\n  \"lines\": [\n" + lines + "\n  ]\n}\n";
    return out;
}

}  // namespace oc166
