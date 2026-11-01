// SPDX-License-Identifier: MIT
// isagen: compiles c166.tbl into a constexpr C++ table and a JSON description.
//
// Usage: isagen <c166.tbl> <out.inc> <out.json>

#include "isa/isa_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace oc166::isa;

namespace {

struct Operand {
    Cls cls = Cls::None;
    std::string field;
};

struct FormDef {
    std::string mnemonic;
    std::vector<Operand> ops;
    std::vector<ByteTemplate> bytes;
    std::string encoding;
    Cpu cpu = Cpu::C166;
    int line = 0;
};

std::string g_path;
int g_errors = 0;

void error(int line, const std::string& msg) {
    std::fprintf(stderr, "%s:%d: error: %s\n", g_path.c_str(), line, msg.c_str());
    ++g_errors;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

bool is_hex(char c) { return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'A' && c <= 'F'); }

bool is_field_name(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

bool parse_class(const std::string& name, Cls& out) {
    for (const auto& ci : kClassInfo) {
        if (ci.cls != Cls::None && ci.name == name) {
            out = ci.cls;
            return true;
        }
    }
    return false;
}

bool parse_part(const std::string& tok, const FormDef& f, int line, Part& out) {
    // Hex nibble literal.
    if (tok.size() == 1 && is_hex(tok[0])) {
        out = {4, kLiteral, Sub::Full, static_cast<std::uint8_t>(std::stoi(tok, nullptr, 16))};
        return true;
    }
    // Binary literal.
    if (tok[0] == '%') {
        if (tok.size() < 2 || tok.size() > 9 ||
            tok.find_first_not_of("01", 1) != std::string::npos) {
            error(line, "bad binary literal '" + tok + "'");
            return false;
        }
        out = {static_cast<std::uint8_t>(tok.size() - 1), kLiteral, Sub::Full,
               static_cast<std::uint8_t>(std::stoi(tok.substr(1), nullptr, 2))};
        return true;
    }
    // Field reference.
    std::string name = tok, sub;
    if (auto dot = tok.find('.'); dot != std::string::npos) {
        name = tok.substr(0, dot);
        sub = tok.substr(dot + 1);
    }
    if (!is_field_name(name)) {
        error(line, "bad encoding part '" + tok + "'");
        return false;
    }
    int idx = -1;
    for (size_t i = 0; i < f.ops.size(); ++i)
        if (f.ops[i].field == name) idx = static_cast<int>(i);
    if (idx < 0) {
        error(line, "encoding refers to unknown field '" + name + "'");
        return false;
    }
    const ClassInfo& ci = class_info(f.ops[idx].cls);
    Sub s;
    if (sub.empty()) {
        s = Sub::Full;
        if (ci.width == 0) {
            error(line, "field '" + name + "' (" + std::string(ci.name) + ") needs a sub-field");
            return false;
        }
    } else if ((sub == "lo" || sub == "hi") && ci.has_lohi) {
        s = sub == "lo" ? Sub::Lo : Sub::Hi;
    } else if ((sub == "off" || sub == "bit") && ci.has_offbit) {
        s = sub == "off" ? Sub::Off : Sub::Bit;
    } else if (sub == "reg" && ci.has_reg) {
        s = Sub::Reg;
    } else {
        error(line, "sub-field '." + sub + "' is not valid for class " + std::string(ci.name));
        return false;
    }
    out = {sub_width(f.ops[idx].cls, s), static_cast<std::uint8_t>(idx), s, 0};
    return true;
}

bool parse_byte(const std::string& tok, const FormDef& f, int line, ByteTemplate& out) {
    out = {};
    if (tok.size() == 2 && is_hex(tok[0]) && is_hex(tok[1])) {
        out.count = 1;
        out.parts[0] = {8, kLiteral, Sub::Full, static_cast<std::uint8_t>(std::stoi(tok, nullptr, 16))};
        return true;
    }
    auto parts = split(tok, ':');
    if (parts.size() > 4) {
        error(line, "too many parts in '" + tok + "'");
        return false;
    }
    int bits = 0;
    for (const auto& p : parts) {
        if (p.empty()) {
            error(line, "empty part in '" + tok + "'");
            return false;
        }
        Part part{};
        if (!parse_part(p, f, line, part)) return false;
        out.parts[out.count++] = part;
        bits += part.width;
    }
    if (bits != 8) {
        error(line, "byte '" + tok + "' has " + std::to_string(bits) + " bits, expected 8");
        return false;
    }
    return true;
}

std::string json_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

const char* sub_enum(Sub s) {
    switch (s) {
    case Sub::Full: return "Sub::Full";
    case Sub::Lo: return "Sub::Lo";
    case Sub::Hi: return "Sub::Hi";
    case Sub::Off: return "Sub::Off";
    case Sub::Bit: return "Sub::Bit";
    case Sub::Reg: return "Sub::Reg";
    }
    return "Sub::Full";
}

std::string cls_enum(Cls c) {
    static const char* names[] = {"None", "Rw", "Rb", "Ind", "PostInc", "PreDec", "IndI",
                                  "PostIncI", "IndDisp", "Reg", "RegB", "Mem", "BitOff",
                                  "BitAddr", "D3", "D4", "D8", "D16", "M8", "Trap7",
                                  "Irang2", "Pag10", "Seg8", "Cc", "Rel", "Caddr", "Seg"};
    return std::string("Cls::") + names[static_cast<int>(c)];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: isagen <c166.tbl> <out.inc> <out.json>\n");
        return 2;
    }
    g_path = argv[1];
    std::ifstream in(g_path);
    if (!in) {
        std::fprintf(stderr, "isagen: cannot open %s\n", argv[1]);
        return 1;
    }

    std::vector<FormDef> forms;
    std::vector<std::pair<std::string, std::string>> aliases;
    std::vector<std::vector<std::string>> generics;
    std::vector<int> alias_lines, generic_lines;

    std::string raw;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        if (auto hash = raw.find('#'); hash != std::string::npos && raw.find('|') == std::string::npos)
            raw = raw.substr(0, hash);
        std::string text = trim(raw);
        if (text.empty() || text[0] == '#') continue;

        if (text[0] == '@') {
            auto w = words(text);
            if (w[0] == "@alias" && w.size() == 3) {
                aliases.emplace_back(w[1], w[2]);
                alias_lines.push_back(line);
            } else if (w[0] == "@generic" && (w.size() == 4 || w.size() == 5)) {
                generics.emplace_back(w.begin() + 1, w.end());
                generic_lines.push_back(line);
            } else {
                error(line, "bad directive '" + text + "'");
            }
            continue;
        }

        auto cols = split(text, '|');
        if (cols.size() != 4) {
            error(line, "expected 4 columns separated by '|'");
            continue;
        }
        FormDef f;
        f.line = line;
        f.mnemonic = cols[0];
        if (f.mnemonic.empty() ||
            !std::all_of(f.mnemonic.begin(), f.mnemonic.end(),
                         [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); })) {
            error(line, "bad mnemonic '" + f.mnemonic + "'");
            continue;
        }

        bool ok = true;
        if (!cols[1].empty()) {
            for (const auto& opText : split(cols[1], ',')) {
                auto sp = opText.find_last_of(" \t");
                if (sp == std::string::npos) {
                    error(line, "operand '" + opText + "' needs '<class> <field>'");
                    ok = false;
                    break;
                }
                Operand op;
                std::string cname = trim(opText.substr(0, sp));
                op.field = trim(opText.substr(sp + 1));
                if (!parse_class(cname, op.cls)) {
                    error(line, "unknown operand class '" + cname + "'");
                    ok = false;
                    break;
                }
                if (!is_field_name(op.field)) {
                    error(line, "bad field name '" + op.field + "'");
                    ok = false;
                    break;
                }
                for (const auto& other : f.ops) {
                    if (other.field == op.field) {
                        error(line, "duplicate field name '" + op.field + "'");
                        ok = false;
                    }
                }
                f.ops.push_back(op);
            }
        }
        if (!ok) continue;
        if (f.ops.size() > 3) {
            error(line, "more than 3 operands");
            continue;
        }

        f.encoding = cols[2];
        auto toks = words(cols[2]);
        if (toks.size() != 2 && toks.size() != 4) {
            error(line, "encoding must be 2 or 4 bytes");
            continue;
        }
        std::vector<bool> used(f.ops.size(), false);
        for (const auto& t : toks) {
            ByteTemplate bt;
            if (!parse_byte(t, f, line, bt)) {
                ok = false;
                break;
            }
            for (int i = 0; i < bt.count; ++i)
                if (bt.parts[i].operand != kLiteral) used[bt.parts[i].operand] = true;
            f.bytes.push_back(bt);
        }
        if (!ok) continue;
        for (size_t i = 0; i < f.ops.size(); ++i)
            if (!used[i]) error(line, "operand field '" + f.ops[i].field + "' is never encoded");

        if (cols[3] == "c167") {
            f.cpu = Cpu::C167;
        } else if (!cols[3].empty()) {
            error(line, "unknown flag '" + cols[3] + "'");
            continue;
        }
        forms.push_back(std::move(f));
    }

    // Cross-checks.
    std::set<std::string> mnemonics;
    std::set<std::string> signatures;
    for (const auto& f : forms) {
        mnemonics.insert(f.mnemonic);
        std::string sig = f.mnemonic;
        for (const auto& op : f.ops) sig += " " + std::string(class_info(op.cls).name);
        if (!signatures.insert(sig).second) error(f.line, "duplicate form '" + sig + "'");
    }
    for (size_t i = 0; i < aliases.size(); ++i) {
        if (!mnemonics.count(aliases[i].second))
            error(alias_lines[i], "alias target '" + aliases[i].second + "' is not a mnemonic");
        if (mnemonics.count(aliases[i].first))
            error(alias_lines[i], "alias '" + aliases[i].first + "' shadows a mnemonic");
    }
    for (size_t i = 0; i < generics.size(); ++i)
        for (size_t j = 1; j < generics[i].size(); ++j)
            if (!mnemonics.count(generics[i][j]))
                error(generic_lines[i], "generic variant '" + generics[i][j] + "' is not a mnemonic");

    if (g_errors) return 1;

    std::stable_sort(forms.begin(), forms.end(),
                     [](const FormDef& a, const FormDef& b) { return a.mnemonic < b.mnemonic; });
    std::sort(aliases.begin(), aliases.end());
    std::sort(generics.begin(), generics.end());

    // C++ table.
    std::ostringstream cpp;
    cpp << "// Generated by isagen from c166.tbl. Do not edit.\n";
    cpp << "inline constexpr Form kForms[] = {\n";
    for (const auto& f : forms) {
        cpp << "    {\"" << f.mnemonic << "\", " << (f.cpu == Cpu::C167 ? "Cpu::C167" : "Cpu::C166")
            << ", " << f.bytes.size() << ", " << f.ops.size() << ", {";
        for (size_t i = 0; i < 3; ++i)
            cpp << (i ? ", " : "") << cls_enum(i < f.ops.size() ? f.ops[i].cls : Cls::None);
        cpp << "}, {";
        for (size_t b = 0; b < f.bytes.size(); ++b) {
            const auto& bt = f.bytes[b];
            cpp << (b ? ", " : "") << "{" << int(bt.count) << ", {";
            for (int p = 0; p < bt.count; ++p) {
                const auto& part = bt.parts[p];
                cpp << (p ? ", " : "") << "{" << int(part.width) << ", " << int(part.operand) << ", "
                    << sub_enum(part.sub) << ", " << int(part.literal) << "}";
            }
            cpp << "}}";
        }
        cpp << "}, " << f.line << "},\n";
    }
    cpp << "};\n\ninline constexpr MnemonicEntry kMnemonics[] = {\n";
    for (size_t i = 0; i < forms.size();) {
        size_t j = i;
        while (j < forms.size() && forms[j].mnemonic == forms[i].mnemonic) ++j;
        cpp << "    {\"" << forms[i].mnemonic << "\", " << i << ", " << (j - i) << "},\n";
        i = j;
    }
    cpp << "};\n\ninline constexpr AliasEntry kAliases[] = {\n";
    for (const auto& a : aliases) cpp << "    {\"" << a.first << "\", \"" << a.second << "\"},\n";
    cpp << "};\n\ninline constexpr GenericEntry kGenerics[] = {\n";
    for (const auto& g : generics)
        cpp << "    {\"" << g[0] << "\", \"" << g[1] << "\", \"" << g[2] << "\", \""
            << (g.size() > 3 ? g[3] : "") << "\"},\n";
    cpp << "};\n";

    // JSON description for third-party tools.
    std::ostringstream js;
    js << "{\n  \"format\": \"openc166-isa\",\n  \"version\": 1,\n  \"forms\": [\n";
    for (size_t i = 0; i < forms.size(); ++i) {
        const auto& f = forms[i];
        js << "    {\"mnemonic\": \"" << f.mnemonic << "\", \"operands\": [";
        for (size_t k = 0; k < f.ops.size(); ++k)
            js << (k ? ", " : "") << "{\"class\": \"" << json_escape(class_info(f.ops[k].cls).name)
               << "\", \"field\": \"" << f.ops[k].field << "\"}";
        js << "], \"size\": " << f.bytes.size() << ", \"encoding\": \"" << json_escape(f.encoding)
           << "\", \"cpu\": \"" << (f.cpu == Cpu::C167 ? "c167" : "c166") << "\"}"
           << (i + 1 < forms.size() ? "," : "") << "\n";
    }
    js << "  ],\n  \"aliases\": {";
    for (size_t i = 0; i < aliases.size(); ++i)
        js << (i ? ", " : "") << "\"" << aliases[i].first << "\": \"" << aliases[i].second << "\"";
    js << "},\n  \"generics\": {";
    for (size_t i = 0; i < generics.size(); ++i) {
        const auto& g = generics[i];
        js << (i ? ", " : "") << "\"" << g[0] << "\": {\"word\": \"" << g[1] << "\", \"byte\": \""
           << g[2] << "\"" << (g.size() > 3 ? ", \"bit\": \"" + g[3] + "\"" : "") << "}";
    }
    js << "}\n}\n";

    // Always rewrite: the build system reruns isagen only when its inputs change.
    auto write_file = [](const char* path, const std::string& content) {
        std::ofstream out(path, std::ios::binary);
        out << content;
        return static_cast<bool>(out);
    };
    if (!write_file(argv[2], cpp.str()) || !write_file(argv[3], js.str())) {
        std::fprintf(stderr, "isagen: cannot write output\n");
        return 1;
    }
    std::printf("isagen: %zu forms, %zu mnemonics\n", forms.size(), mnemonics.size());
    return 0;
}
