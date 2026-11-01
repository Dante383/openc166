// SPDX-License-Identifier: MIT
#include "assembler.hpp"

#include "encode.hpp"
#include "expr.hpp"
#include "lexer.hpp"
#include "operand.hpp"
#include "predef.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace oc166 {
namespace {

constexpr int kMaxPasses = 32;
constexpr std::int64_t kAddressLimit = 0x1000000;  // 16 MB C167 address space

std::string upper(std::string_view s) {
    std::string u(s);
    for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

std::string hex(std::int64_t v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*llX", digits, static_cast<unsigned long long>(v));
    return buf;
}

// Listing text of an EQU/SET/BIT value; bits show as word.bit like Keil.
std::string value_text(const Value& v) {
    if (v.type != VType::Bit) return hex(v.num, 7);
    std::int64_t word = bit_word_address(v);
    if (word < 0) return "R" + std::to_string(v.num - 0xF0) + "." + std::to_string(v.bit);
    return hex(word, 4) + "H." + std::to_string(v.bit);
}

// Type of a label in front of data, or of a data directive's name.
LType data_type(std::string_view upper_dir) {
    if (upper_dir == "DW" || upper_dir == "DSW") return LType::Word;
    if (upper_dir == "DD" || upper_dir == "DSD") return LType::Dword;
    return LType::Byte;
}

const char* type_name(LType t) {
    static constexpr const char* kNames[] = {"", "NEAR", "FAR", "BYTE", "WORD", "DWORD"};
    return kNames[static_cast<int>(t)];
}

bool same_value(const Value& a, const Value& b) {
    return a.type == b.type && a.known == b.known && a.ext == b.ext && a.bit == b.bit && a.num == b.num;
}

// Directives written as "name DIRECTIVE ...".
constexpr std::string_view kNamedDirectives[] = {
    "EQU", "SET", "BIT", "LABEL", "SECTION", "ENDS", "PROC", "ENDP", "DB", "DW", "DD", "DS", "DSB", "DSW", "DSD",
    "DEFR", "DEFB", "DEFA", "LIT", "REGDEF", "REGBANK", "COMREG", "DBIT", "DGROUP", "CGROUP", "MACRO", "TYPEDEC",
};

// Directives that only matter to a linker or debugger; a raw image ignores them.
constexpr std::string_view kIgnoredDirectives[] = {
    "NAME", "PUBLIC", "GLOBAL", "ASSUME", "REGDEF", "REGBANK", "SSKDEF", "PECDEF", "TYPEDEC",
};

constexpr std::string_view kDataDirectives[] = {"DB", "DW", "DD", "DS", "DSB", "DSW", "DSD"};

// Keil places word data on even and double-word data on 4-byte boundaries.
int data_alignment(std::string_view upper_dir) {
    if (upper_dir == "DW" || upper_dir == "DSW") return 2;
    if (upper_dir == "DD" || upper_dir == "DSD") return 4;
    return 1;
}

constexpr std::string_view kKeywords[] = {
    "EQU", "SET", "BIT", "LABEL", "SECTION", "ENDS", "PROC", "ENDP", "DB", "DW", "DD", "DS", "DSB", "DSW",
    "DSD", "ORG", "EVEN", "END", "NAME", "PUBLIC", "GLOBAL", "EXTERN", "EXTRN", "ASSUME", "REGDEF",
    "REGBANK", "SSKDEF", "PECDEF", "DEFR", "DEFB", "DEFA", "LIT", "DBIT", "DGROUP", "CGROUP", "MACRO",
    "TYPEDEC", "NEAR", "FAR", "INTERRUPT", "TASK", "AT", "SHORT", "PTR", "BYTE", "WORD", "DWORD",
};

template <std::size_t N>
bool contains(const std::string_view (&list)[N], std::string_view upper_name) {
    return std::find(std::begin(list), std::end(list), upper_name) != std::end(list);
}

// Controls that only affect listing or object file details.
constexpr std::string_view kIgnoredControls[] = {
    "LIST", "NOLIST", "TITLE", "TT", "PAGELENGTH", "PL", "PAGEWIDTH", "PW", "EJECT", "EJ",
    "SYMBOLS", "SB", "NOSYMBOLS", "NOSB", "XREF", "XR", "NOXREF", "NOXR", "DEBUG", "DB",
    "NODEBUG", "NODB", "GEN", "NOGEN", "GENONLY", "GO", "PRINT", "PR", "NOPRINT", "NOPR",
    "OBJECT", "OJ", "NOOBJECT", "NOOJ", "ERRORPRINT", "EP", "NOERRORPRINT", "NOEP", "PAGING",
    "NOPAGING", "TABS", "DATE", "COND", "NOCOND", "LINES", "NOLINES", "SYMLIST", "NOSYMLIST",
    "TYPE", "NOTYPE", "ABSOLUTE", "NOABSOLUTE", "AMAKE", "NOAMAKE", "SEGMENTED", "NONSEGMENTED",
    "USEDEXTONLY", "EXTMAC", "MACRO", "NOMACRO", "MPL", "NOMPL", "SAVE", "RESTORE",
};

enum class ProcKind : std::uint8_t { None, Near, Far, Interrupt };

struct Section {
    std::string name;
    std::int64_t base = 0;
    std::int64_t loc = 0;
    bool absolute = false;
    bool code = true;  // CODE section: plain labels are NEAR and must be even
};

class Assembler final : public SymbolContext {
public:
    explicit Assembler(const AssembleOptions& opt) : opt_(opt) {}

    AssemblyResult run(std::string_view source);

    std::optional<Value> lookup(std::string_view name) override;
    std::int64_t location() const override { return sections_[cur_].loc; }
    bool location_fixed() const override { return sections_[cur_].absolute; }
    std::uint16_t section_index() const override { return static_cast<std::uint16_t>(cur_); }
    bool has_esfr() const override { return cpu_ == isa::Cpu::C167; }
    unsigned forward_references() const override { return forward_count_; }
    void soft_error(const AsmError& e) override { rep_->error(e.code(), e.message(), e.column()); }
    bool final_pass() const override { return final_; }

private:
    struct Entry {
        Symbol sym;
        int pass = 0;  // pass of the latest definition
    };

    const AssembleOptions& opt_;
    std::vector<std::string_view> lines_;
    std::unordered_map<std::string, Entry> symbols_;
    std::vector<int> sizes_;  // instruction sizes of the previous pass
    std::set<int> misaligned_;  // lines that were misaligned in some pass

    int pass_ = 0;
    bool final_ = false;
    bool changed_ = false;
    bool forward_ref_ = false;
    unsigned forward_count_ = 0;
    Reporter* rep_ = nullptr;  // diagnostics of the current line
    isa::Cpu cpu_ = isa::Cpu::C166;
    bool sys166_ = true;
    bool case_ = false;
    std::vector<Section> sections_;  // [0] holds code outside any section
    std::size_t cur_ = 0;
    std::vector<std::size_t> open_;
    std::int64_t free_ = 0;  // first address after everything placed so far
    ProcKind proc_ = ProcKind::None;
    std::string proc_name_;
    int ext_left_ = 0;
    bool ext_esfr_ = false;
    bool ended_ = false;
    std::size_t insn_ = 0;
    int line_no_ = 0;

    AssemblyResult res_;

    void run_pass();
    void process_line(std::string_view text);
    void statement(std::string_view text, Reporter& rep, ListingLine& lst);
    void controls(std::string_view text, Reporter& rep);
    void named_directive(const Token& name, const std::string& dir, TokenCursor& cur, Reporter& rep,
                         ListingLine& lst);
    void directive(const std::string& kw, const Token& tok, TokenCursor& cur, Reporter& rep, ListingLine& lst);
    void instruction(const std::string& mnemonic, TokenCursor& cur, Reporter& rep, ListingLine& lst);
    void branch(std::string& mnemonic, std::vector<Operand>& ops, std::int64_t pc, Reporter& rep);
    void check_branch_target(const std::string& mnemonic, const std::vector<Operand>& ops, Reporter& rep) const;
    void data(const std::string& dir, TokenCursor& cur, Reporter& rep, ListingLine& lst);
    void section_start(const Token& name, TokenCursor& cur);
    void section_end(const Token& name);

    std::string key(std::string_view name) const { return case_ ? std::string(name) : upper(name); }
    void define(const Token& name, SymbolKind kind, const Value& v);
    void undefine(const Token& name);
    Value here(LType type) const;
    Value expression(TokenCursor& cur);
    std::int64_t absolute(TokenCursor& cur, const char* what);
    void expect_end(TokenCursor& cur);
    void access_attribute(TokenCursor& cur);
    void emit(const std::uint8_t* bytes, std::size_t n, ListingLine& lst);
    void advance(std::int64_t n);
    void align(int n);
    void check_alignment(int need, const Token& name, Reporter& rep);
    Section& section() { return sections_[cur_]; }
};

std::optional<Value> Assembler::lookup(std::string_view name) {
    if (auto p = predefined(name, cpu_, sys166_)) return p;
    auto it = symbols_.find(key(name));
    if (it == symbols_.end()) return std::nullopt;
    if (it->second.pass != pass_) {
        forward_ref_ = true;
        ++forward_count_;
    }
    return it->second.sym.value;
}

void Assembler::define(const Token& name, SymbolKind kind, const Value& v) {
    std::string u = upper(name.text);
    if (isa::is_mnemonic(u) || is_operator_keyword(u) || contains(kKeywords, u))
        throw AsmError("A4", "illegal use of a reserved word", name.column);
    if (predefined(name.text, cpu_, sys166_)) throw AsmError("A25", "symbol redefinition", name.column);
    auto [it, inserted] = symbols_.try_emplace(key(name.text));
    Entry& e = it->second;
    if (!inserted && e.pass == pass_ && !(kind == SymbolKind::Set && e.sym.kind == SymbolKind::Set))
        throw AsmError("A25", "symbol redefinition", name.column);
    if (kind != SymbolKind::Set && (inserted || !same_value(e.sym.value, v))) changed_ = true;
    e.sym = Symbol{std::string(name.text), kind, v, line_no_};
    e.pass = pass_;
}

// Drops a symbol that an earlier pass defined on this line, for definitions
// that Keil leaves undefined when they fail.
void Assembler::undefine(const Token& name) {
    auto it = symbols_.find(key(name.text));
    if (it == symbols_.end() || it->second.sym.line != line_no_) return;
    symbols_.erase(it);
    changed_ = true;
}

// Value of a label at the current location: an address, which Keil treats as
// relocatable.
Value Assembler::here(LType type) const {
    Value v = Value::number(location());
    v.rel = Rel::Addr;
    v.unsig = true;
    v.fixed = location_fixed();
    v.section = section_index();
    v.ltype = type;
    return v;
}

Value Assembler::expression(TokenCursor& cur) { return evaluate(cur, *this); }

// Evaluates an expression that must be a number known at this point.
std::int64_t Assembler::absolute(TokenCursor& cur, const char* what) {
    int column = cur.peek().column;
    forward_ref_ = false;
    Value v = expression(cur);
    if (forward_ref_) throw AsmError("A71", std::string("expression with forward reference not permitted in ") + what, column);
    if (v.type != VType::Number) throw AsmError("A72", "absolute expression required", column);
    return v.num;
}

// DEFR, DEFB and DEFA take an optional access attribute (R, RW or W). Only a
// debugger uses it.
void Assembler::access_attribute(TokenCursor& cur) {
    if (!cur.accept_op(",")) return;
    const Token& t = cur.peek();
    if (t.kind == TokKind::Ident && (ieq(t.text, "R") || ieq(t.text, "RW") || ieq(t.text, "W"))) {
        cur.next();
        return;
    }
    throw AsmError("A6", "syntax error: access attribute R, RW or W expected", t.column);
}

void Assembler::expect_end(TokenCursor& cur) {
    if (!cur.at_end()) throw AsmError("A6", "syntax error: unexpected '" + std::string(cur.peek().text) + "'", cur.peek().column);
}

void Assembler::advance(std::int64_t n) {
    Section& s = section();
    s.loc += n;
    if (s.loc < 0 || s.loc > kAddressLimit) throw AsmError("A66", "location counter out of the 16 MB address space");
    free_ = std::max(free_, s.loc);
}

void Assembler::align(int n) {
    if (std::int64_t r = location() % n) advance(n - r);
}

// Keil warns when a label was misaligned in any pass, even if shortening
// jumps in a later pass moved it to a good address.
void Assembler::check_alignment(int need, const Token& name, Reporter& rep) {
    if (location() % need) misaligned_.insert(line_no_);
    if (final_ && misaligned_.count(line_no_)) rep.warning("A48", "insufficient alignment", name.column);
}

void Assembler::emit(const std::uint8_t* bytes, std::size_t n, ListingLine& lst) {
    if (n == 0) return;
    std::int64_t at = location();
    if (at + static_cast<std::int64_t>(n) > kAddressLimit)
        throw AsmError("A66", "location counter out of the 16 MB address space");
    if (final_) {
        lst.bytes.insert(lst.bytes.end(), bytes, bytes + n);
        auto& chunks = res_.chunks;
        if (chunks.empty() || chunks.back().address + static_cast<std::int64_t>(chunks.back().bytes.size()) != at)
            chunks.push_back(Chunk{at, {}});
        chunks.back().bytes.insert(chunks.back().bytes.end(), bytes, bytes + n);
    }
    advance(static_cast<std::int64_t>(n));
}

AssemblyResult Assembler::run(std::string_view source) {
    for (std::size_t pos = 0; pos <= source.size();) {
        std::size_t nl = source.find('\n', pos);
        if (nl == std::string_view::npos) nl = source.size();
        std::string_view line = source.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (nl < source.size() || !line.empty()) lines_.push_back(line);
        pos = nl + 1;
    }

    bool converged = false;
    for (pass_ = 1; pass_ <= kMaxPasses; ++pass_) {
        changed_ = false;
        run_pass();
        if (!changed_) {
            converged = true;
            break;
        }
    }
    if (!converged) pass_ = kMaxPasses;
    ++pass_;
    final_ = true;
    res_ = AssemblyResult{};
    run_pass();
    res_.passes = pass_;
    res_.cpu = cpu_;
    if (!converged)
        res_.diagnostics.push_back({Severity::Error, "A133", "addresses do not settle; code size keeps changing between passes",
                                    opt_.file_name, 0, 0});

    std::vector<const Chunk*> sorted;
    for (const auto& c : res_.chunks) sorted.push_back(&c);
    std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->address < b->address; });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        std::int64_t prev_end = sorted[i - 1]->address + static_cast<std::int64_t>(sorted[i - 1]->bytes.size());
        if (sorted[i]->address < prev_end)
            res_.diagnostics.push_back({Severity::Warning, "OC1",
                                        "code or data overlaps at address " + hex(sorted[i]->address, 6) + "H",
                                        opt_.file_name, 0, 0});
    }

    for (auto& [k, e] : symbols_) res_.symbols.push_back(e.sym);
    std::sort(res_.symbols.begin(), res_.symbols.end(), [](const Symbol& a, const Symbol& b) { return a.name < b.name; });
    return std::move(res_);
}

void Assembler::run_pass() {
    cpu_ = opt_.cpu;
    sys166_ = cpu_ == isa::Cpu::C166;
    case_ = false;
    sections_.assign(1, Section{"", 0, opt_.origin, true});
    cur_ = 0;
    open_.clear();
    free_ = opt_.origin;
    proc_ = ProcKind::None;
    proc_name_.clear();
    ext_left_ = 0;
    ext_esfr_ = false;
    ended_ = false;
    insn_ = 0;

    for (line_no_ = 1; line_no_ <= static_cast<int>(lines_.size()) && !ended_; ++line_no_)
        process_line(lines_[line_no_ - 1]);

    if (!final_) return;
    auto add = [&](Severity sev, const char* code, std::string msg) {
        res_.diagnostics.push_back({sev, code, std::move(msg), opt_.file_name, 0, 0});
    };
    if (!ended_) add(Severity::Warning, "A89", "premature end of file (no END statement)");
    if (proc_ != ProcKind::None) add(Severity::Error, "A106", "unterminated procedure '" + proc_name_ + "'");
    for (auto i : open_) add(Severity::Error, "A106", "unterminated section '" + sections_[i].name + "'");
}

void Assembler::process_line(std::string_view text) {
    ListingLine lst;
    lst.line = line_no_;
    lst.source = std::string(text);
    Reporter rep;
    rep_ = &rep;
    try {
        statement(text, rep, lst);
    } catch (const AsmError& e) {
        rep.error(e.code(), e.message(), e.column());
    }
    rep_ = nullptr;
    if (!final_) return;
    for (auto& d : rep.items()) {
        d.file = opt_.file_name;
        d.line = line_no_;
        res_.diagnostics.push_back(std::move(d));
    }
    res_.listing.push_back(std::move(lst));
}

void Assembler::statement(std::string_view text, Reporter& rep, ListingLine& lst) {
    if (!text.empty() && text[0] == '$') {
        controls(text.substr(1), rep);
        return;
    }
    auto toks = tokenize(text);
    TokenCursor cur(toks);
    if (cur.at_end()) return;

    if (cur.peek().kind == TokKind::Ident && cur.peek(1).is_op(":")) {
        const Token& label = cur.next();
        cur.next();
        // A label in front of data takes the type and alignment of the data;
        // otherwise it is a NEAR label in code sections and needs an even address.
        std::string next = cur.peek().kind == TokKind::Ident ? upper(cur.peek().text) : std::string();
        bool data_follows = contains(kDataDirectives, next);
        if (data_follows) align(data_alignment(next));
        lst.has_address = true;
        lst.address = location();
        // Keil gives plain labels outside CODE sections the type BYTE.
        LType type = data_follows ? data_type(next) : section().code ? LType::Near : LType::Byte;
        define(label, SymbolKind::Label, here(type));
        if (!data_follows && section().code) check_alignment(2, label, rep);
        if (cur.at_end()) return;
    }

    const Token& first = cur.peek();
    if (first.kind != TokKind::Ident) throw AsmError("A6", "syntax error", first.column);
    if (cur.peek(1).kind == TokKind::Ident) {
        std::string dir = upper(cur.peek(1).text);
        if (contains(kNamedDirectives, dir)) {
            cur.next();
            cur.next();
            named_directive(first, dir, cur, rep, lst);
            return;
        }
    }
    cur.next();
    std::string kw = upper(first.text);
    if (isa::is_mnemonic(kw))
        instruction(kw, cur, rep, lst);
    else
        directive(kw, first, cur, rep, lst);
}

void Assembler::controls(std::string_view text, Reporter& rep) {
    std::size_t i = 0;
    auto skip_space = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    };
    for (;;) {
        skip_space();
        if (i >= text.size() || text[i] == ';') return;
        std::size_t start = i;
        while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
        int column = static_cast<int>(start) + 2;
        if (i == start) throw AsmError("A16", "unknown control or bad argument(s)", column);
        std::string name = upper(text.substr(start, i - start));
        skip_space();
        if (i < text.size() && text[i] == '(') {
            int depth = 0;
            for (; i < text.size(); ++i) {
                if (text[i] == '(') ++depth;
                if (text[i] == ')' && --depth == 0) break;
            }
            if (i >= text.size()) throw AsmError("A22", "')' expected", column);
            ++i;
        }
        if (name == "MOD167" || name == "M167") {
            cpu_ = isa::Cpu::C167;
            sys166_ = false;
        } else if (name == "MOD166" || name == "M166") {
            sys166_ = true;
        } else if (name == "NOMOD166" || name == "NOM166") {
            sys166_ = false;
        } else if (name == "MODV2") {
            cpu_ = isa::Cpu::C167;
            sys166_ = false;
            rep.warning("OC2", "MODV2: the extended XC16x/Super10 instructions are not supported; using the C167 set",
                        column);
        } else if (name == "CASE") {
            case_ = true;
        } else if (name == "NOCASE") {
            case_ = false;
        } else if (name == "INCLUDE" || name == "IC" || name == "IF" || name == "ELSEIF" || name == "ELSE" ||
                   name == "ENDIF" || name == "SET" || name == "RESET") {
            throw AsmError("OC3", "control $" + name + " is not supported yet", column);
        } else if (!contains(kIgnoredControls, name)) {
            throw AsmError("A16", "unknown control '" + name + "'", column);
        }
    }
}

void Assembler::named_directive(const Token& name, const std::string& dir, TokenCursor& cur, Reporter& rep,
                                ListingLine& lst) {
    if (dir == "EQU" || dir == "SET" || dir == "BIT") {
        int column = cur.peek().column;
        forward_ref_ = false;
        Value v = expression(cur);
        expect_end(cur);
        SymbolKind kind = dir == "EQU" ? SymbolKind::Equ : dir == "SET" ? SymbolKind::Set : SymbolKind::Bit;
        // Keil reports the error but still defines the symbol.
        if (forward_ref_)
            rep.error("A71", "expression with forward reference not permitted in " + dir, column);
        if (kind == SymbolKind::Bit && v.known && v.type == VType::Bit && bit_word_address(v) < 0)
            throw AsmError("A70", "invalid type of expression: BIT cannot name a GPR bit", column);
        if (kind == SymbolKind::Bit && v.known && v.type != VType::Bit)
            throw AsmError("A70", "invalid type of expression: bit address expected", column);
        if (kind != SymbolKind::Bit && v.type == VType::Bit)
            throw AsmError("A70", "invalid type of expression: use BIT for bit addresses", column);
        define(name, kind, v);
        if (v.known) lst.value = v;
    } else if (dir == "LABEL") {
        int need = 1;
        LType type = section().code ? LType::Near : LType::Byte;
        if (!cur.at_end()) {
            std::string t = upper(cur.next().text);
            need = t == "DWORD" ? 4 : t == "WORD" || t == "NEAR" || t == "FAR" ? 2 : 1;
            type = t == "NEAR" ? LType::Near : t == "FAR" ? LType::Far : t == "BYTE" ? LType::Byte
                 : t == "WORD" ? LType::Word : t == "DWORD" ? LType::Dword : type;
        }
        expect_end(cur);
        lst.has_address = true;
        lst.address = location();
        define(name, SymbolKind::Label, here(type));
        check_alignment(need, name, rep);
    } else if (dir == "SECTION") {
        section_start(name, cur);
    } else if (dir == "ENDS") {
        expect_end(cur);
        section_end(name);
    } else if (dir == "PROC") {
        if (proc_ != ProcKind::None) throw AsmError("A75", "can't nest procedures", name.column);
        ProcKind kind = ProcKind::Near;
        if (cur.accept_keyword("FAR"))
            kind = ProcKind::Far;
        else if (cur.accept_keyword("INTERRUPT") || cur.accept_keyword("TASK"))
            kind = ProcKind::Interrupt;
        else
            cur.accept_keyword("NEAR");
        // Interrupt names, numbers and USING clauses only matter to the linker.
        lst.has_address = true;
        lst.address = location();
        define(name, SymbolKind::Label, here(kind == ProcKind::Far ? LType::Far : LType::Near));
        check_alignment(2, name, rep);
        proc_ = kind;
        proc_name_ = std::string(name.text);
    } else if (dir == "ENDP") {
        expect_end(cur);
        if (proc_ == ProcKind::None || key(proc_name_) != key(name.text))
            throw AsmError("A29", "unmatched ENDS/ENDP", name.column);
        proc_ = ProcKind::None;
        proc_name_.clear();
    } else if (dir == "DEFR" || dir == "DEFA") {
        // DEFR names an SFR (FE00H..FFDEH, or an extended SFR at F000H..F1DEH
        // on the C167); DEFA names a word of internal RAM (FA00H..FDFEH).
        const bool sfr = dir == "DEFR";
        const char* code = sfr ? "A101" : "A102";
        const std::string what = sfr ? "invalid system address" : "invalid internal RAM address";
        int column = cur.peek().column;
        forward_ref_ = false;
        Value v;
        try {
            v = expression(cur);
        } catch (const AsmError&) {
            undefine(name);
            throw;
        }
        const std::int64_t a = v.num;
        if (v.known && (v.type != VType::Number || (a & 1) ||
                        !(sfr ? (a >= 0xFE00 && a <= 0xFFDE) || (cpu_ == isa::Cpu::C167 && a >= 0xF000 && a <= 0xF1DE)
                              : a >= 0xFA00 && a <= 0xFDFE))) {
            undefine(name);
            throw AsmError(code, what + (sfr ? ": expected an even SFR address" : ": expected an even address in FA00H..FDFEH"),
                           column);
        }
        Value d = Value::number(a);
        d.known = v.known;
        if (sfr)
            d.type = VType::Sfr;
        else
            d.ltype = LType::Word;
        if (sfr && d.known) lst.value = d;
        // Keil reports a forward reference; it still defines a DEFA symbol but
        // not a DEFR one.
        if (forward_ref_) rep.error(code, what + ": forward reference not permitted", column);
        if (sfr && forward_ref_)
            undefine(name);
        else
            define(name, SymbolKind::Equ, d);
        access_attribute(cur);
        expect_end(cur);
    } else if (dir == "DEFB") {
        int column = cur.peek().column;
        forward_ref_ = false;
        std::optional<Value> v;
        try {
            v = evaluate_bit_select(cur, *this);
            if (!v || !(cur.at_end() || cur.peek().is_op(",")))
                throw AsmError("A103", "invalid bit address: DEFB expects word.bit", column);
            if (v->known && bit_word_address(*v) < 0)
                throw AsmError("A70", "invalid type of expression: DEFB cannot name a GPR bit", column);
        } catch (const AsmError&) {
            undefine(name);
            throw;
        }
        define(name, SymbolKind::Bit, *v);
        if (v->known) lst.value = *v;
        if (forward_ref_) rep.error("A103", "invalid bit address: forward reference not permitted", column);
        access_attribute(cur);
        expect_end(cur);
    } else if (dir == "LIT") {
        throw AsmError("A6", "syntax error: LIT is only supported by Keil AX51, not A166", name.column);
    } else if (contains(kDataDirectives, dir)) {
        align(data_alignment(dir));
        lst.has_address = true;
        lst.address = location();
        define(name, SymbolKind::Label, here(data_type(dir)));
        data(dir, cur, rep, lst);
    } else if (contains(kIgnoredDirectives, dir)) {
        // REGDEF and REGBANK name register banks for the linker.
    } else {
        throw AsmError("OC3", "directive " + dir + " is not supported yet", cur.peek().column);
    }
}

void Assembler::directive(const std::string& kw, const Token& tok, TokenCursor& cur, Reporter& rep,
                          ListingLine& lst) {
    if (kw == "ORG") {
        int column = cur.peek().column;
        std::int64_t v = absolute(cur, "ORG");
        expect_end(cur);
        Section& s = section();
        std::int64_t target = s.absolute ? v : s.base + v;
        if (target < s.base) throw AsmError("A132", "location counter may not point below section base", column);
        s.loc = target;
        free_ = std::max(free_, target);
    } else if (kw == "EVEN") {
        expect_end(cur);
        if (location() & 1) advance(1);
    } else if (kw == "END") {
        expect_end(cur);
        ended_ = true;
    } else if (contains(kDataDirectives, kw)) {
        align(data_alignment(kw));
        lst.has_address = true;
        lst.address = location();
        data(kw, cur, rep, lst);
    } else if (kw == "EXTERN" || kw == "EXTRN") {
        throw AsmError("OC4", "external symbols need a linker, which openc166 does not use", tok.column);
    } else if (contains(kIgnoredDirectives, kw)) {
        // No effect on a raw image.
    } else if (contains(kKeywords, kw)) {
        throw AsmError("OC3", "directive " + kw + " is not supported yet", tok.column);
    } else {
        throw AsmError("A6", "syntax error: unknown instruction or directive '" + std::string(tok.text) + "'",
                       tok.column);
    }
}

void Assembler::data(const std::string& dir, TokenCursor& cur, Reporter& rep, ListingLine& lst) {
    if (dir == "DS" || dir == "DSB" || dir == "DSW" || dir == "DSD") {
        int column = cur.peek().column;
        std::int64_t n = absolute(cur, dir.c_str());
        expect_end(cur);
        if (n < 0) throw AsmError("A19", "numeric value out of range", column);
        advance(n * (dir == "DSW" ? 2 : dir == "DSD" ? 4 : 1));
        return;
    }
    const int width = dir == "DB" ? 1 : dir == "DW" ? 2 : 4;
    std::vector<std::uint8_t> out;
    for (;;) {
        const Token& t = cur.peek();
        if (width == 1 && t.kind == TokKind::String && (cur.peek(1).is_op(",") || cur.peek(1).kind == TokKind::End)) {
            cur.next();
            auto bytes = string_bytes(t);
            out.insert(out.end(), bytes.begin(), bytes.end());
        } else {
            // Like Keil, report a bad element and carry on with the next one. An
            // undefined name keeps its place so later addresses stay put.
            std::int64_t num = 0;
            bool keep = true;
            try {
                Value v = expression(cur);
                if (v.type != VType::Number && v.type != VType::Sfr)
                    throw AsmError("A70", "invalid type of expression", t.column);
                if (v.known && !fits_width(v, 8 * width)) rep.warning("A62", "value truncation", t.column);
                num = v.num;
            } catch (const AsmError& e) {
                rep.error(e.code(), e.message(), e.column());
                keep = e.code() == "A42";
                for (int depth = 0; !cur.at_end() && !(depth == 0 && cur.peek().is_op(","));) {
                    const Token& skipped = cur.next();
                    if (skipped.is_op("(")) ++depth;
                    else if (skipped.is_op(")") && depth > 0) --depth;
                }
            }
            if (keep)
                for (int b = 0; b < width; ++b) out.push_back(static_cast<std::uint8_t>(num >> (8 * b)));
        }
        if (cur.at_end()) break;
        cur.expect_op(",");
    }
    emit(out.data(), out.size(), lst);
}

void Assembler::section_start(const Token& name, TokenCursor& cur) {
    static constexpr std::string_view kTypes[] = {"CODE", "DATA", "CONST", "HDATA", "HCONST", "XDATA", "XCONST", "BIT"};
    const Token& type = cur.peek();
    if (type.kind != TokKind::Ident || !contains(kTypes, upper(type.text)))
        throw AsmError("A34", "illegal section type", type.column);
    if (ieq(type.text, "BIT")) throw AsmError("OC3", "BIT sections are not supported yet", type.column);
    const bool code = ieq(type.text, "CODE");
    cur.next();
    std::optional<std::int64_t> at;
    while (!cur.at_end()) {
        const Token& t = cur.peek();
        if (t.kind == TokKind::Ident && ieq(t.text, "AT")) {
            cur.next();
            at = absolute(cur, "AT");
        } else if (t.kind == TokKind::Ident || t.kind == TokKind::String) {
            cur.next();  // alignment, combine type or class name: linker information
        } else {
            throw AsmError("A6", "syntax error", t.column);
        }
    }
    std::string k = key(name.text);
    for (std::size_t i = 1; i < sections_.size(); ++i) {
        if (key(sections_[i].name) != k) continue;
        if (std::find(open_.begin(), open_.end(), i) != open_.end())
            throw AsmError("A46", "section '" + sections_[i].name + "' is already open", name.column);
        if (at && (!sections_[i].absolute || *at != sections_[i].base))
            throw AsmError("A30", "section redefinition with a different AT address", name.column);
        open_.push_back(i);
        cur_ = i;
        return;
    }
    if (at && (*at < 0 || *at >= kAddressLimit)) throw AsmError("A66", "value out of range");
    Section s;
    s.name = std::string(name.text);
    s.absolute = at.has_value();
    s.code = code;
    s.base = s.loc = at ? *at : (free_ + 1) & ~std::int64_t{1};
    sections_.push_back(s);
    cur_ = sections_.size() - 1;
    open_.push_back(cur_);
    free_ = std::max(free_, s.loc);
}

void Assembler::section_end(const Token& name) {
    if (open_.empty() || key(sections_[open_.back()].name) != key(name.text))
        throw AsmError("A29", "unmatched ENDS/ENDP", name.column);
    open_.pop_back();
    cur_ = open_.empty() ? 0 : open_.back();
}

void Assembler::instruction(const std::string& mnemonic, TokenCursor& cur, Reporter& rep, ListingLine& lst) {
    const std::size_t index = insn_++;
    if (index >= sizes_.size()) sizes_.resize(index + 1, 0);
    const std::int64_t pc = location();
    lst.has_address = true;
    lst.address = pc;
    try {
        auto ops = parse_operands(cur, *this);

        // Keil translates RET according to the enclosing procedure.
        std::string m = mnemonic;
        if (isa::is_branch(m)) branch(m, ops, pc, rep);
        check_branch_target(m, ops, rep);
        if (m == "RET") {
            m = proc_ == ProcKind::Far ? "RETS" : proc_ == ProcKind::Interrupt ? "RETI" : "RET";
        } else if (m == "RETS" && proc_ != ProcKind::Far) {
            rep.warning("A78", "'far return' in non far procedure");
        } else if (m == "RETI" && proc_ != ProcKind::Interrupt) {
            rep.warning("A80", "'interrupt return' in non interrupt procedure");
        }

        EncodeContext ctx;
        ctx.cpu = cpu_;
        ctx.pc = pc;
        ctx.ext_sfr = ext_left_ > 0 && ext_esfr_;
        Encoding e = encode(m, ops, ctx, rep);
        if (pc & 1) rep.error("A134", "improper address alignment: instruction at an odd address");

        if (ext_left_ > 0) --ext_left_;
        if (ExtSequence seq = ext_sequence(e); seq.count) {
            ext_left_ = seq.count;
            ext_esfr_ = seq.esfr;
        }
        sizes_[index] = e.size;
        emit(e.bytes.data(), e.size, lst);
    } catch (const AsmError&) {
        // Keep later addresses where the previous pass put them.
        if (ext_left_ > 0) --ext_left_;
        advance(sizes_[index]);
        throw;
    }
}

// Resolves Keil's JMP and CALL to a hardware branch:
//   [Rw] target                          JMPI / CALLI
//   FAR label, no condition              JMPS / CALLS
//   SHORT, or a known target in range    JMPR / CALLR (CALLR has no condition)
//   anything else                        JMPA / CALLA
// A forward reference starts out absolute; later passes shrink it to the
// relative form once the target is known to be close. Like Keil, the range
// check uses the target address from the previous pass.
void Assembler::branch(std::string& m, std::vector<Operand>& ops, std::int64_t pc, Reporter& rep) {
    const bool call = m == "CALL";
    if (ops.size() != 1 && ops.size() != 2) throw AsmError("A73", "number of operands does not match instruction");
    Operand cc;
    cc.value.type = VType::Cc;  // cc_UC
    const bool has_cc = ops.size() == 2;
    if (has_cc) {
        cc = ops[0];
        if (cc.value.short_jump) {
            rep.error("A73", "number of operands does not match instruction", cc.column);
            throw AsmError("A6", "syntax error: SHORT belongs in front of the target", cc.column);
        }
        if (cc.mode != OpMode::Direct || cc.value.type != VType::Cc)
            throw AsmError("A74", "operand type mismatch: condition code expected", cc.column);
    }
    const Operand target = ops.back();
    if (target.mode == OpMode::Ind) {
        m = call ? "CALLI" : "JMPI";
        ops = {cc, target};
        return;
    }
    const Value& v = target.value;
    if (target.mode != OpMode::Direct) throw AsmError("A74", "operand type mismatch", target.column);
    if (v.type == VType::Bit) throw AsmError("A70", "invalid type of expression", target.column);
    if (v.type != VType::Number) throw AsmError("A74", "operand type mismatch", target.column);

    if (!has_cc && v.ltype == LType::Far && !v.short_jump) {
        m = call ? "CALLS" : "JMPS";
        Operand seg = target, off = target;
        seg.value = Value::number((v.num >> 16) & 0xFF);
        off.value = Value::number(v.num & 0xFFFF);
        seg.value.known = off.value.known = v.known;
        ops = {seg, off};
        return;
    }
    const bool unconditional = cc.value.num == 0;
    bool relative = v.short_jump;
    if (!relative && v.known && v.ltype != LType::Far) {
        // Keil only measures the distance when it does not depend on the
        // linker: numbers, the same section, or two sections with AT addresses.
        bool measurable = v.rel != Rel::Addr || v.section == cur_ || (v.fixed && location_fixed());
        std::int64_t disp = v.num - (pc + 2);
        relative = measurable && !(disp & 1) && disp >= -256 && disp <= 254;
    }
    if (call && !unconditional) relative = false;
    if (relative) {
        m = call ? "CALLR" : "JMPR";
        ops = call ? std::vector<Operand>{target} : std::vector<Operand>{cc, target};
    } else {
        m = call ? "CALLA" : "JMPA";
        ops = {cc, target};
        // Keil silently keeps the low 16 bits of a number.
        if (v.rel != Rel::Addr) ops.back().value.num &= 0xFFFF;
    }
}

// Keil warns when a near branch goes to a FAR label and when any branch goes
// to a data label.
void Assembler::check_branch_target(const std::string& m, const std::vector<Operand>& ops, Reporter& rep) const {
    const bool near_branch = m == "JMPA" || m == "JMPR" || m == "CALLA" || m == "CALLR";
    if (!near_branch && m != "JMPS" && m != "CALLS") return;
    if (ops.empty() || ops.back().mode != OpMode::Direct) return;
    const Operand& t = ops.back();
    if (near_branch && t.value.ltype == LType::Far && !t.value.short_jump)
        rep.warning("A84", "near reference to far location", t.column);
    if (is_data_type(t.value.ltype)) rep.warning("A74", "operand type mismatch: branch to a data label", t.column);
    if (m != "JMPR" && m != "CALLR" && t.value.known && (t.value.num & 1))
        rep.warning("A134", "address alignment not sufficient: branch target is odd", t.column);
}

}  // namespace

std::size_t AssemblyResult::error_count() const {
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                                  [](const Diagnostic& d) { return d.severity == Severity::Error; }));
}

std::size_t AssemblyResult::warning_count() const { return diagnostics.size() - error_count(); }

AssemblyResult assemble(std::string_view source, const AssembleOptions& options) {
    return Assembler(options).run(source);
}

FlatImage flat_image(const AssemblyResult& result, std::optional<std::int64_t> base, std::uint8_t fill) {
    FlatImage img;
    if (result.chunks.empty()) {
        img.base = base.value_or(0);
        return img;
    }
    std::int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (const auto& c : result.chunks) {
        if (c.bytes.empty()) continue;
        lo = std::min(lo, c.address);
        hi = std::max(hi, c.address + static_cast<std::int64_t>(c.bytes.size()));
    }
    img.base = base.value_or(lo);
    if (lo < img.base)
        throw std::invalid_argument("code at address " + hex(lo, 6) + "H lies below the image base " +
                                    hex(img.base, 6) + "H");
    if (hi - img.base > kAddressLimit) throw std::invalid_argument("image would be larger than 16 MB");
    img.bytes.assign(static_cast<std::size_t>(hi - img.base), fill);
    for (const auto& c : result.chunks)
        std::copy(c.bytes.begin(), c.bytes.end(), img.bytes.begin() + (c.address - img.base));
    return img;
}

std::string format_diagnostic(const Diagnostic& d) {
    std::string s = d.file;
    if (d.line > 0) s += ":" + std::to_string(d.line);
    if (d.line > 0 && d.column > 0) s += ":" + std::to_string(d.column);
    s += d.severity == Severity::Error ? ": error " : ": warning ";
    s += d.code + ": " + d.message;
    return s;
}

std::string format_listing(const AssemblyResult& result) {
    std::multimap<int, const Diagnostic*> by_line;
    for (const auto& d : result.diagnostics) by_line.emplace(d.line, &d);

    std::string out = "LOC       OBJ                  LINE  SOURCE\n\n";
    auto diagnostics_for = [&](int line) {
        auto [b, e] = by_line.equal_range(line);
        for (auto it = b; it != e; ++it) {
            const Diagnostic& d = *it->second;
            out += "*** ";
            out += d.severity == Severity::Error ? "ERROR " : "WARNING ";
            out += d.code + ": " + d.message + "\n";
        }
    };
    for (const auto& l : result.listing) {
        std::string loc = l.has_address ? hex(l.address, 8) : l.value ? "=" + value_text(*l.value) : "";
        std::string obj;
        std::size_t shown = 0;
        for (; shown < l.bytes.size() && shown < 8; ++shown) obj += hex(l.bytes[shown], 2);
        char head[64];
        std::snprintf(head, sizeof head, "%-9s %-18s %6d  ", loc.c_str(), obj.c_str(), l.line);
        out += head + l.source + "\n";
        // Long DB/DW lines continue on extra rows.
        while (shown < l.bytes.size()) {
            std::string more;
            std::size_t row = shown;
            for (; shown < l.bytes.size() && shown < row + 8; ++shown) more += hex(l.bytes[shown], 2);
            std::snprintf(head, sizeof head, "%-9s %-18s\n", hex(l.address + static_cast<std::int64_t>(row), 8).c_str(),
                          more.c_str());
            out += head;
        }
        diagnostics_for(l.line);
    }
    diagnostics_for(0);

    out += "\nSYMBOLS\n\n";
    for (const auto& s : result.symbols) {
        static constexpr const char* kKinds[] = {"LABEL", "EQU", "SET", "BIT"};
        std::string value;
        switch (s.value.type) {
        case VType::Number: value = s.value.known ? hex(s.value.num, 8) + "H" : "?"; break;
        case VType::Bit: value = value_text(s.value); break;
        case VType::Gpr: value = "R" + std::to_string(s.value.num); break;
        case VType::GprB: value = std::string(s.value.num & 1 ? "RH" : "RL") + std::to_string(s.value.num / 2); break;
        case VType::Sfr: value = "SFR " + hex(s.value.num, 4) + "H"; break;
        case VType::Cc: value = "cc " + std::to_string(s.value.num); break;
        }
        if (s.value.ltype != LType::None) value += std::string(" ") + type_name(s.value.ltype);
        char row[160];
        std::snprintf(row, sizeof row, "%-32s %-6s %s\n", s.name.c_str(), kKinds[static_cast<int>(s.kind)], value.c_str());
        out += row;
    }
    out += "\n" + std::to_string(result.error_count()) + " error(s), " + std::to_string(result.warning_count()) +
           " warning(s)\n";
    return out;
}

}  // namespace oc166
