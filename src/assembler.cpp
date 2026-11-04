// SPDX-License-Identifier: MIT
#include "assembler.hpp"

#include "bundled.hpp"
#include "encode.hpp"
#include "expr.hpp"
#include "lexer.hpp"
#include "operand.hpp"
#include "predef.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace oc166 {
namespace {

constexpr int kMaxPasses = 32;
constexpr std::size_t kMaxIncludeDepth = 32;
// Guards against LITs whose texts multiply ('A LIT 'B B'', 'B LIT 'C C'', ...).
constexpr std::size_t kMaxExpandedLine = 65536;
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
    if (upper_dir == "DW" || upper_dir == "DSW" || upper_dir == "DPPTR" || upper_dir == "DSPTR" || upper_dir == "DBPTR")
        return LType::Word;
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
    "DPPTR", "DSPTR", "DBPTR",
};

// Directives that only matter to a linker or debugger; a raw image ignores them.
constexpr std::string_view kIgnoredDirectives[] = {
    "NAME", "REGDEF", "REGBANK", "SSKDEF", "PECDEF", "TYPEDEC",
};

constexpr std::string_view kDataDirectives[] = {"DB", "DW", "DD", "DS", "DSB", "DSW", "DSD", "DPPTR", "DSPTR", "DBPTR"};

bool is_pointer_directive(std::string_view upper_dir) {
    return upper_dir == "DPPTR" || upper_dir == "DSPTR" || upper_dir == "DBPTR";
}

// Keil places word data on even and double-word data on 4-byte boundaries.
int data_alignment(std::string_view upper_dir) {
    if (upper_dir == "DW" || upper_dir == "DSW" || is_pointer_directive(upper_dir)) return 2;
    if (upper_dir == "DD" || upper_dir == "DSD") return 4;
    return 1;
}

struct AlignType {
    std::string_view name;
    int align;
};
// BITADDRESSABLE sections lie in FD00H..FDFFH; PECADDRESSABLE only tells
// the linker that PEC pointers may address the section.
constexpr AlignType kAlignTypes[] = {
    {"BYTE", 1}, {"WORD", 2}, {"DWORD", 4}, {"PAGE", 0x4000}, {"SEGMENT", 0x10000},
    {"BITADDRESSABLE", 2}, {"PECADDRESSABLE", 2},
};
constexpr std::string_view kCombineTypes[] = {"PUBLIC", "PRIVATE", "COMMON", "GLOBAL", "SYSSTACK", "USRSTACK"};
constexpr std::int64_t kBitAreaStart = 0xFD00, kBitAreaEnd = 0xFE00;

bool align_allowed(std::string_view align, std::string_view type) {
    if (align == "BYTE") return type != "CODE";
    if (align == "BITADDRESSABLE") return type == "CODE" || type == "DATA" || type == "HDATA";
    if (align == "PECADDRESSABLE") return type == "CODE" || type == "DATA";
    return true;
}

// L166 keeps a DATA or CONST section within one 16 KB data page, and a CODE,
// HDATA or HCONST section within one 64 KB segment.
std::int64_t section_boundary(std::string_view type) {
    if (type == "DATA" || type == "CONST") return 0x4000;
    if (type == "CODE" || type == "HDATA" || type == "HCONST") return 0x10000;
    return 0;
}

std::int64_t align_up(std::int64_t v, std::int64_t n) { return (v + n - 1) / n * n; }

constexpr std::string_view kSectionTypes[] = {"CODE", "DATA", "CONST", "HDATA", "HCONST", "XDATA", "XCONST", "BIT"};

constexpr std::string_view kKeywords[] = {
    "EQU", "SET", "BIT", "LABEL", "SECTION", "ENDS", "PROC", "ENDP", "DB", "DW", "DD", "DS", "DSB", "DSW",
    "DSD", "ORG", "EVEN", "END", "NAME", "PUBLIC", "GLOBAL", "EXTERN", "EXTRN", "ASSUME", "REGDEF",
    "REGBANK", "SSKDEF", "PECDEF", "DEFR", "DEFB", "DEFA", "LIT", "DBIT", "DGROUP", "CGROUP", "MACRO",
    "TYPEDEC", "NEAR", "FAR", "INTERRUPT", "TASK", "AT", "SHORT", "PTR", "BYTE", "WORD", "DWORD",
    "DPPTR", "DSPTR", "DBPTR",
};

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    for (std::size_t pos = 0; pos <= text.size();) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (nl < text.size() || !line.empty()) lines.push_back(line);
        pos = nl + 1;
    }
    return lines;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Keil sources are written on Windows; '/' works there and everywhere else.
std::string portable_path(std::string_view path) {
    std::string p(trim(path));
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

bool is_absolute_path(std::string_view p) {
    return (!p.empty() && p[0] == '/') ||
           (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':');
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty() || is_absolute_path(name)) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string directory_of(std::string_view path) {
    std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string() : portable_path(path.substr(0, slash + 1));
}

std::optional<std::string> read_whole_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Reads a file, ignoring the case of each path component if the file
// system does not (Keil sources often spell include names differently from
// the files on disk).
std::optional<std::string> read_file_any_case(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path wanted(path);
    if (fs::is_regular_file(wanted, ec)) return read_whole_file(wanted);
    fs::path found = wanted.has_root_path() ? wanted.root_path() : fs::path(".");
    for (const auto& part : wanted.relative_path()) {
        fs::path next = found / part;
        if (!fs::exists(next, ec)) {
            std::string name = upper(part.string());
            bool matched = false;
            for (fs::directory_iterator it(found, ec), end; !ec && it != end; it.increment(ec)) {
                if (upper(it->path().filename().string()) == name) {
                    next = it->path();
                    matched = true;
                    break;
                }
            }
            if (!matched) return std::nullopt;
        }
        found = next;
    }
    if (!fs::is_regular_file(found, ec)) return std::nullopt;
    return read_whole_file(found);
}

template <std::size_t N>
bool contains(const std::string_view (&list)[N], std::string_view upper_name) {
    return std::find(std::begin(list), std::end(list), upper_name) != std::end(list);
}

// IF, ELSEIF, ELSE and ENDIF, as statements and as $ controls.
constexpr std::string_view kCondKeywords[] = {"IF", "ELSEIF", "ELSE", "ENDIF"};

// Keil opens a $IF only while fewer than this many IF blocks (of either
// kind) are open.
constexpr std::size_t kMaxControlIfDepth = 9;

bool word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '?' || c == '@';
}

// Tokens of the part of a line from index 'from' on, with columns of the
// whole line.
std::vector<Token> tokenize_from(std::string_view line, std::size_t from) {
    const int shift = static_cast<int>(from);
    try {
        auto toks = tokenize(line.substr(from));
        for (auto& t : toks) t.column += shift;
        return toks;
    } catch (const AsmError& e) {
        throw AsmError(e.code(), e.message(), e.column() ? e.column() + shift : 0);
    }
}

// Tokens of an IF condition. Keil reads 1.0 as a bit address here.
std::vector<Token> condition_tokens(std::string_view line, std::size_t from) {
    try {
        return tokenize_from(line, from);
    } catch (const AsmError& e) {
        if (e.code() != "A70") throw;
        throw AsmError("A6", "syntax error: a bit cannot be a condition", e.column());
    }
}

// Value of an IF condition. Keil reports a bit address (x.n) as a syntax error.
Value condition_value(TokenCursor& cur, SymbolContext& ctx, int column) {
    if (cur.at_end()) throw AsmError("A6", "syntax error: condition expected", column);
    for (std::size_t k = 0; cur.peek(k).kind != TokKind::End; ++k)
        if (cur.peek(k).is_op("."))
            throw AsmError("A6", "syntax error: a bit cannot be a condition", cur.peek(k).column);
    return evaluate(cur, ctx);
}

// Keil tests only bit 0 of a condition, so 2 is false. That keeps NOT
// working on 0/1 flags (NOT 1 is 0FFFEH, false), but surprises anyone who
// expects "non-zero is true", so other such values get a warning.
bool condition_truth(const Value& v, int column, Reporter& rep) {
    bool bit0 = (v.num & 1) != 0;
    if (!bit0 && v.num != 0 && (v.num & 0xFFFF) != 0xFFFE)
        rep.warning("OC10", "the condition is " + hex(v.num & 0xFFFFFFFF, 4) +
                                "H, which counts as false: Keil tests only bit 0 of a condition", column);
    return bit0;
}

// Statement IF and ELSEIF see EQU and SET symbols. Keil treats a condition
// that uses a label, a register, an SFR or a bit as false without a word,
// so these are noted for an error.
class ConditionContext final : public SymbolContext {
public:
    explicit ConditionContext(SymbolContext& outer) : outer_(outer) {}

    std::optional<Value> lookup(std::string_view name) override {
        unsigned before = outer_.forward_references();
        auto v = outer_.lookup(name);
        if (outer_.forward_references() != before) forward = true;
        if (v && v->type == VType::Cc)
            condition_code = true;
        else if (v && (v->rel != Rel::None || v->type != VType::Number))
            unusable = true;
        return v;
    }
    std::int64_t location() const override {
        dollar = true;
        return outer_.location();
    }
    bool location_fixed() const override { return outer_.location_fixed(); }
    std::uint16_t section_index() const override { return outer_.section_index(); }
    bool has_esfr() const override { return outer_.has_esfr(); }
    unsigned forward_references() const override { return outer_.forward_references(); }
    void warning(const AsmError& w) override { outer_.warning(w); }
    bool segmented() const override { return outer_.segmented(); }
    bool final_pass() const override { return true; }

    bool forward = false;
    bool condition_code = false;
    bool unusable = false;
    mutable bool dollar = false;

private:
    SymbolContext& outer_;
};

// $IF and $ELSEIF see only $SET names; any other name counts as 0.
class FlagContext final : public SymbolContext {
public:
    explicit FlagContext(const std::map<std::string, std::int64_t>& flags) : flags_(flags) {}

    std::optional<Value> lookup(std::string_view name) override {
        auto it = flags_.find(upper(name));
        return Value::number(it == flags_.end() ? 0 : it->second);
    }
    std::int64_t location() const override {
        dollar = true;
        return 0;
    }
    bool location_fixed() const override { return true; }
    bool final_pass() const override { return true; }

    mutable bool dollar = false;

private:
    const std::map<std::string, std::int64_t>& flags_;
};

// Controls allowed only before the first statement (Keil's "primary"
// controls). Those not handled in controls() only affect the listing or
// object file and are ignored.
constexpr std::string_view kPrimaryControls[] = {
    "MOD167", "M167", "MOD166", "M166", "NOMOD166", "NOM166", "MODV2", "CASE", "NOCASE", "INCDIR", "ID",
    "PAGELENGTH", "PL", "PAGEWIDTH", "PW", "SYMBOLS", "SB", "NOSYMBOLS", "NOSB", "XREF", "XR", "NOXREF",
    "NOXR", "DEBUG", "DB", "NODEBUG", "NODB", "PRINT", "PR", "NOPRINT", "NOPR", "OBJECT", "OJ", "NOOBJECT",
    "NOOJ", "ERRORPRINT", "EP", "NOERRORPRINT", "NOEP", "PAGING", "NOPAGING", "TABS", "DATE", "LINES",
    "NOLINES", "TYPE", "NOTYPE", "ABSOLUTE", "NOABSOLUTE", "AMAKE", "NOAMAKE", "SEGMENTED", "SG", "NONSEGMENTED", "NOSG",
    "USEDEXTONLY", "EXTMAC", "MACRO", "NOMACRO", "MPL", "NOMPL", "EXPDECNUM",
};

// Controls allowed anywhere that only affect the listing.
constexpr std::string_view kIgnoredControls[] = {
    "LIST", "NOLIST", "TITLE", "TT", "EJECT", "EJ", "GEN", "NOGEN", "GENONLY", "GO",
    "SYMLIST", "NOSYMLIST", "SAVE", "RESTORE", "REGUSE",
};

enum class ProcKind : std::uint8_t { None, Near, Far, Interrupt };

struct Section {
    std::string name;
    std::int64_t base = 0;
    std::int64_t loc = 0;
    std::int64_t size = 0;  // highest offset reached
    bool absolute = false;
    bool code = true;  // CODE section: plain labels are NEAR and must be even
    std::string type;  // CODE, DATA, ...
    std::string align_type = "WORD";
    int align = 2;
    std::string combine = "PRIVATE";  // or "AT" for a section with a fixed address
    std::string class_name;
    bool placed = false;  // AssembleOptions::section_addresses gives the address
    // Final pass: the listing line and column of the first SECTION line.
    std::size_t listing = SIZE_MAX;
    int column = 0;
};

// Keil's A86 check, made by DS and ORG only (data never triggers it): has
// the location counter outgrown what the section type allows? Returns why,
// or an empty string. A DATA section at a fixed address must stay in the
// 16 KB data page it starts in, which one DPP register selects. A CONST
// section must leave the last byte of its 16 KB page free (a relocatable
// one counts from its start). A CODE, HDATA or HCONST section may be 64 KB
// long. Other sections are not checked.
std::string length_overflow(const Section& s, std::int64_t loc) {
    const std::int64_t page_end = (s.absolute ? s.base & ~std::int64_t{0x3FFF} : s.base) + 0x4000;
    if (s.type == "DATA" && s.absolute && loc > page_end) return "a DATA section must fit in one 16 KB page";
    if (s.type == "CONST" && loc >= page_end)
        return "a CONST section must leave the last byte of its 16 KB page free";
    if (section_boundary(s.type) == 0x10000 && loc - s.base > 0x10000)
        return "a " + s.type + " section must not be longer than 64 KB";
    return {};
}

// name DGROUP sections or name CGROUP sections: the linker keeps the DATA
// and CONST sections of a DGROUP in one 16 KB data page, and the CODE
// sections of a CGROUP in one 64 KB segment.
struct Group {
    std::string name;
    bool code = false;  // CGROUP
    std::vector<std::string> members;  // section keys, in the order listed
    // Final pass: the listing line and column of the group line.
    std::size_t listing = SIZE_MAX;
    int column = 0;
};
struct PublicDef {
    std::string name;  // as written
    Value value;
    SymbolKind kind = SymbolKind::Label;
    std::size_t module = 0;
};

// What the modules of a program know about each other: the PUBLIC and
// GLOBAL symbols defined in the previous pass, by the name the linker
// matches (upper case unless the module uses $CASE).
struct LinkState {
    std::map<std::string, PublicDef> publics;
    std::vector<std::string> modules;  // file names
};

// Types of EXTERN symbols.
constexpr std::string_view kExternTypes[] = {
    "NEAR", "FAR", "BYTE", "WORD", "DWORD", "BIT", "BITWORD", "DATA3", "DATA4", "DATA8", "DATA16", "INTNO",
};

class Assembler final : public SymbolContext {
public:
    Assembler(const AssembleOptions& opt, std::string file_name, std::size_t module, const LinkState& link)
        : opt_(opt), main_name_(std::move(file_name)), module_(module), link_(link) {}

    void load(std::string text) { add_file(main_name_, std::move(text)); }
    void begin_pass(int pass, bool final);
    void run_lines();
    bool finish_pass();
    std::vector<std::pair<std::string, PublicDef>> exported() const;

    std::optional<Value> lookup(std::string_view name) override;
    std::int64_t location() const override { return sections_[cur_].loc + item_offset_; }
    bool location_fixed() const override { return sections_[cur_].absolute; }
    std::uint16_t section_index() const override { return static_cast<std::uint16_t>(cur_); }
    bool has_esfr() const override { return cpu_ == isa::Cpu::C167; }
    unsigned forward_references() const override { return forward_count_; }
    void soft_error(const AsmError& e) override { rep_->error(e.code(), e.message(), e.column()); }
    void warning(const AsmError& w) override { rep_->warning(w.code(), w.message(), w.column()); }
    bool segmented() const override { return segmented_; }
    bool final_pass() const override { return final_; }
    void external_used(const Value& v, int column) override;

private:
    struct Entry {
        Symbol sym;
        int pass = 0;  // pass of the latest definition
    };

    struct SourceFile {
        std::string name;
        std::string text;
        std::vector<std::string_view> lines;
        bool newline_at_end = true;
    };
    // A file being read: index into files_ and the number of lines read so far.
    struct Frame {
        std::size_t file = 0;
        std::size_t next = 0;
    };

    friend class Program;

    const AssembleOptions& opt_;
    std::string main_name_;  // file name of the module's source
    std::size_t module_;  // index of the module in the program
    const LinkState& link_;
    struct Export {
        std::string name;  // as written
        bool global = false;  // GLOBAL rather than PUBLIC
    };
    std::map<std::string, Export> exports_;  // PUBLIC and GLOBAL names of this pass, by key()
    std::uint16_t externs_ = 0;  // EXTERN symbols declared in this pass
    std::map<std::uint16_t, std::string> extern_bad_;  // EXTERN id -> why its definition does not suit it
    std::vector<std::unique_ptr<SourceFile>> files_;  // [0] is the main source
    std::map<std::string, int> includes_;  // include request -> files_ index, or -1 if not found
    std::vector<std::string> incdirs_;  // directories from $INCDIR
    std::vector<Frame> stack_;  // files being read, innermost last
    std::optional<std::size_t> include_;  // file to read after the current line
    bool fatal_ = false;  // stops the pass, like Keil's fatal errors
    bool primary_done_ = false;  // a statement was seen: primary controls are no longer allowed
    std::size_t file_ = 0;  // files_ index of the current line
    int level_ = 0;  // include nesting depth of the current line
    std::string joined_;  // include file whose last line was joined to the current line
    std::unordered_map<std::string, Entry> symbols_;
    bool lits_ = false;  // some pass defined a LIT, so lines need expand_lits()
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
    std::vector<Section> prev_sections_;  // sections_ of the previous pass, for forward section names
    std::map<std::string, std::int64_t> layout_;  // section key -> address chosen after the previous pass
    std::vector<Group> groups_;  // of this pass, in order of definition
    std::vector<Group> prev_groups_;  // of the previous pass, for group names used before their group line
    std::map<std::string, std::int64_t> group_layout_;  // group key -> its page or segment after the previous pass
    std::vector<std::pair<std::int64_t, std::int64_t>> outside_;  // [from, to) used outside any section
    std::size_t cur_ = 0;
    bool segmented_ = false;  // $SEGMENTED: DPPn: prefixes and ASSUME take effect
    std::array<int, 4> assume_{};  // per DPP: a section index, kAssumeSystem or -1 (ASSUME)
    std::vector<std::size_t> open_;
    std::int64_t free_ = 0;  // first address after everything placed so far
    ProcKind proc_ = ProcKind::None;
    std::string proc_name_;
    std::int64_t item_offset_ = 0;  // offset of the data item being evaluated: '$' is its address
    int ext_left_ = 0;
    bool ext_esfr_ = false;
    bool ext_page_ = false;  // the EXT sequence is EXTP or EXTPR: no DPP needed
    bool ended_ = false;
    std::size_t end_line_ = 0;  // index of the END line in res_.listing
    std::size_t insn_ = 0;
    int line_no_ = 0;  // counts lines across all files, as in Keil's listing
    int file_line_ = 0;  // line number within the current file

    // An open IF block. $IF and IF blocks share one stack, as in Keil: a
    // $ENDIF can close an IF statement.
    struct Cond {
        bool parent_active = true;  // the lines around the block are assembled
        bool active = true;  // the lines of the current branch are assembled
        bool taken = false;  // the IF or an ELSEIF was true
        bool in_else = false;  // an ELSE was seen
    };
    std::vector<Cond> conds_;
    std::map<std::string, std::int64_t> flags_;  // $SET names in upper case
    bool cond_listing_ = true;  // $COND: list the lines of false IF blocks

    AssemblyResult res_;

    bool active() const { return conds_.empty() || conds_.back().active; }
    void process_line(std::string_view text);
    bool expand_lits(std::string_view text, std::string& out, std::vector<std::string>& open,
                     bool keep_file_names = false);
    std::size_t add_file(std::string name, std::string text);
    void statement(std::string_view text, Reporter& rep, ListingLine& lst);
    void controls(std::string_view text, std::size_t from, Reporter& rep);
    void cond_statement(const std::string& kw, std::string_view text, std::size_t after, Reporter& rep);
    void cond_control(const std::string& kw, std::string_view text, std::size_t after, int column, bool misplaced,
                      Reporter& rep);
    bool statement_condition(std::string_view text, std::size_t after, int column, Reporter& rep);
    bool control_condition(std::string_view text, std::size_t after, int column, Reporter& rep);
    void set_flags(std::string_view text, std::size_t from, std::size_t to, bool set);
    void include(std::string_view name, int column);
    void named_directive(const Token& name, const std::string& dir, TokenCursor& cur, Reporter& rep,
                         ListingLine& lst);
    void directive(const std::string& kw, const Token& tok, TokenCursor& cur, Reporter& rep, ListingLine& lst);
    void instruction(const std::string& mnemonic, TokenCursor& cur, Reporter& rep, ListingLine& lst);
    void branch(std::string& mnemonic, std::vector<Operand>& ops, std::int64_t pc, Reporter& rep);
    void check_branch_target(const std::string& mnemonic, const std::vector<Operand>& ops, Reporter& rep) const;
    void data(const std::string& dir, TokenCursor& cur, Reporter& rep, ListingLine& lst);
    void pointer_data(const std::string& dir, TokenCursor& cur, Reporter& rep, ListingLine& lst);
    void assume(TokenCursor& cur, Reporter& rep);
    void extern_directive(TokenCursor& cur, Reporter& rep);
    void declare_extern(const Token& name, const std::string& type, int dpp, Reporter& rep);
    void public_directive(bool global, TokenCursor& cur, Reporter& rep);
    void check_public(const Token& name, Reporter& rep);
    bool section_name(const std::string& name_key) const;
    void report(Severity sev, const char* code, std::string msg);
    void report_at(std::size_t listing, int column, Severity sev, const char* code, std::string msg);
    void report_at_section(std::size_t index, Severity sev, const char* code, std::string msg) {
        report_at(sections_[index].listing, sections_[index].column, sev, code, std::move(msg));
    }
    std::optional<Value> section_value(std::string_view name);
    void group(const Token& name, bool code, TokenCursor& cur, Reporter& rep);
    std::optional<std::size_t> find_group(const std::string& name_key, bool forward) const;
    std::optional<std::size_t> group_of(const std::string& section_key) const;
    int assume_key(int id) const;
    std::optional<Value> group_value(std::string_view name);
    void assumed_section_page(Value& v) const;
    void check_section_name(const Token& name) const;
    void section_start(const Token& name, TokenCursor& cur, Reporter& rep);
    void section_end(const Token& name, Reporter& rep);
    std::optional<std::int64_t> option_address(const std::string& name_key) const;

    std::string key(std::string_view name) const { return case_ ? std::string(name) : upper(name); }
    void define(const Token& name, SymbolKind kind, const Value& v, bool same_ok = false);
    void undefine(const Token& name);
    Value here(LType type) const;
    Value expression(TokenCursor& cur);
    std::int64_t absolute(TokenCursor& cur, const char* what, bool offset = false);
    void expect_end(TokenCursor& cur);
    void access_attribute(TokenCursor& cur);
    void emit(const std::uint8_t* bytes, std::size_t n, ListingLine& lst);
    void advance(std::int64_t n);
    void align(int n);
    void check_alignment(int need, const Token& name, Reporter& rep);
    void check_byte_section(int need, int column);
    Section& section() { return sections_[cur_]; }
    // Alignment counts from the start of a relocatable section, as Keil
    // cannot know where the linker will put it.
    std::int64_t align_offset() const {
        const Section& s = sections_[cur_];
        return location() - (s.absolute ? 0 : s.base);
    }
};

std::optional<Value> Assembler::lookup(std::string_view name) {
    if (auto p = predefined(name, cpu_, sys166_)) return p;
    auto it = symbols_.find(key(name));
    if (it == symbols_.end()) {
        if (auto v = section_value(name)) return v;
        return group_value(name);
    }
    // expand_lits() leaves a LIT name in place when the LIT line comes later
    // or the text leads back to the name. Keil reports both as A60.
    if (it->second.sym.kind == SymbolKind::Lit) {
        if (!final_) return Value::unknown();
        throw AsmError("A60", "illegal identifier: LIT '" + std::string(name) +
                                  "' is used before its definition or its text refers back to it");
    }
    if (it->second.pass != pass_) {
        forward_ref_ = true;
        ++forward_count_;
    }
    return it->second.sym.value;
}

// same_ok: DEFR and BIT may repeat a register or bit definition (predefined or
// not) with the same address, as Keil's own register files do.
void Assembler::define(const Token& name, SymbolKind kind, const Value& v, bool same_ok) {
    std::string u = upper(name.text);
    if (isa::is_mnemonic(u) || is_operator_keyword(u) || contains(kKeywords, u))
        throw AsmError("A4", "illegal use of a reserved word", name.column);
    auto repeat = [&](const Value& old) { return same_ok && v.known && old.known && same_value(old, v); };
    if (auto p = predefined(name.text, cpu_, sys166_)) {
        if (repeat(*p)) return;
        throw AsmError("A25", "symbol redefinition", name.column);
    }
    for (std::size_t i = 1; i < sections_.size(); ++i)
        if (key(sections_[i].name) == key(name.text))
            throw AsmError("A25", "symbol redefinition: '" + sections_[i].name + "' is a section", name.column);
    if (auto g = find_group(key(name.text), false))
        throw AsmError("A25", "symbol redefinition: '" + groups_[*g].name + "' is a group", name.column);
    auto [it, inserted] = symbols_.try_emplace(key(name.text));
    Entry& e = it->second;
    if (!inserted && e.pass == pass_ && !(kind == SymbolKind::Set && e.sym.kind == SymbolKind::Set)) {
        if (repeat(e.sym.value)) return;
        throw AsmError("A25", "symbol redefinition", name.column);
    }
    if (kind != SymbolKind::Set && (inserted || !same_value(e.sym.value, v))) changed_ = true;
    e.sym = Symbol{std::string(name.text), kind, v, line_no_, {}};
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
    if (!v.fixed) v.movable = section_index();
    v.ltype = type;
    return v;
}

Value Assembler::expression(TokenCursor& cur) { return evaluate(cur, *this); }

// Evaluates an expression that must be a number known at this point. With
// 'offset' (ORG), a label or '$' in the current section without AT stands
// for its offset in the section, as in Keil.
std::int64_t Assembler::absolute(TokenCursor& cur, const char* what, bool offset) {
    int column = cur.peek().column;
    forward_ref_ = false;
    Value v = expression(cur);
    if (forward_ref_) throw AsmError("A71", std::string("expression with forward reference not permitted in ") + what, column);
    if (offset && v.external()) throw AsmError("A63", "illegal relocatable expression", column);
    if (v.type != VType::Number) throw AsmError("A72", "absolute expression required", column);
    if (v.movable) {
        if (!offset || v.movable != cur_ || v.rel != Rel::Addr)
            throw AsmError("A72", "absolute expression required", column);
        v.num -= section().base;
    }
    return v.dpp >= 0 ? v.num | (std::int64_t{v.dpp} << 14) : v.num;
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
    s.size = std::max(s.size, s.loc - s.base);
    if (cur_ == 0 && n > 0) {
        if (!outside_.empty() && outside_.back().second == s.loc - n)
            outside_.back().second = s.loc;
        else
            outside_.emplace_back(s.loc - n, s.loc);
    }
}

void Assembler::align(int n) {
    if (std::int64_t r = align_offset() % n) advance(n - r);
}

// Keil warns when a label was misaligned in any pass, even if shortening
// jumps in a later pass moved it to a good address.
void Assembler::check_alignment(int need, const Token& name, Reporter& rep) {
    if (align_offset() % need) misaligned_.insert(line_no_);
    if (final_ && misaligned_.count(line_no_)) rep.warning("A48", "insufficient alignment", name.column);
}

// A relocatable BYTE section may start at any address, so Keil refuses data
// that needs an even address there and drops the line.
void Assembler::check_byte_section(int need, int column) {
    const Section& s = section();
    if (need > 1 && !s.absolute && s.align == 1)
        throw AsmError("A48", "insufficient alignment: a BYTE section cannot hold word-aligned data", column);
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

std::size_t Assembler::add_file(std::string name, std::string text) {
    auto f = std::make_unique<SourceFile>();
    f->name = std::move(name);
    f->text = std::move(text);
    f->lines = split_lines(f->text);
    f->newline_at_end = f->text.empty() || f->text.back() == '\n';
    files_.push_back(std::move(f));
    return files_.size() - 1;
}

void Assembler::begin_pass(int pass, bool final) {
    pass_ = pass;
    final_ = final;
    changed_ = false;
    if (final) res_ = AssemblyResult{};
}

void Assembler::run_lines() {
    cpu_ = opt_.cpu;
    sys166_ = cpu_ == isa::Cpu::C166;
    case_ = false;
    sections_.assign(1, Section{});
    sections_[0].loc = opt_.origin;
    sections_[0].absolute = true;
    groups_.clear();
    outside_.clear();
    cur_ = 0;
    open_.clear();
    free_ = opt_.origin;
    proc_ = ProcKind::None;
    proc_name_.clear();
    ext_left_ = 0;
    ext_esfr_ = false;
    ext_page_ = false;
    segmented_ = false;
    assume_.fill(-1);
    ended_ = false;
    insn_ = 0;
    incdirs_.clear();
    fatal_ = false;
    primary_done_ = false;
    conds_.clear();
    flags_.clear();
    cond_listing_ = true;

    exports_.clear();
    externs_ = 0;
    extern_bad_.clear();

    stack_.assign(1, Frame{});
    line_no_ = 0;
    // Keil joins the last line of an include file that does not end with a
    // newline to the line read after it.
    std::string carry;
    std::size_t carry_file = 0;
    int carry_line = 0, carry_level = 0;
    while ((!stack_.empty() || !carry.empty()) && !fatal_) {
        std::string text;
        if (stack_.empty()) {
            text = std::move(carry);
            file_ = carry_file;
            file_line_ = carry_line;
            level_ = carry_level;
            joined_.clear();
        } else {
            Frame& f = stack_.back();
            const SourceFile& file = *files_[f.file];
            if (f.next == file.lines.size()) {
                stack_.pop_back();
                continue;
            }
            std::string_view line = file.lines[f.next++];
            file_ = f.file;
            file_line_ = static_cast<int>(f.next);
            level_ = static_cast<int>(stack_.size()) - 1;
            if (f.next == file.lines.size() && !file.newline_at_end && level_ > 0) {
                carry += line;
                carry_file = file_;
                carry_line = file_line_;
                carry_level = level_;
                continue;
            }
            joined_.clear();
            if (!trim(carry).empty() && !trim(line).empty()) joined_ = files_[carry_file]->name;
            text = std::move(carry) + std::string(line);
        }
        carry.clear();
        ++line_no_;
        include_.reset();
        process_line(text);
        if (include_) stack_.push_back(Frame{*include_, 0});
    }
}

// After Program::locate() has placed the sections: notes whether anything
// moved, and in the final pass reports what is missing at the end. Returns
// true if another pass is needed.
bool Assembler::finish_pass() {
    // ASSUME and expressions may name a section before it starts; they see
    // where the previous pass put it. Passes repeat until every section is
    // where the layout of the previous pass put it.
    std::vector<Section> located = sections_;
    for (std::size_t i = 1; i < located.size(); ++i)
        if (!located[i].absolute) located[i].base = layout_.at(key(located[i].name));
    auto same = [](const Section& a, const Section& b) { return a.name == b.name && a.base == b.base; };
    if (sections_.size() > 1 &&
        (sections_.size() != prev_sections_.size() ||
         !std::equal(sections_.begin(), sections_.end(), prev_sections_.begin(), same) ||
         !std::equal(sections_.begin(), sections_.end(), located.begin(), same)))
        changed_ = true;
    prev_sections_ = std::move(located);
    // ASSUME and group names may come before the group line.
    auto same_group = [](const Group& a, const Group& b) {
        return a.name == b.name && a.code == b.code && a.members == b.members;
    };
    if (groups_.size() != prev_groups_.size() ||
        !std::equal(groups_.begin(), groups_.end(), prev_groups_.begin(), same_group))
        changed_ = true;
    prev_groups_ = groups_;

    if (!final_ || fatal_) return changed_;
    // Like Keil, report problems found at the end on the last line of the
    // file, or on the END line for unterminated sections and procedures.
    auto add = [&](Severity sev, const char* code, std::string msg, bool at_end = false) {
        std::string file = main_name_;
        int line = 0;
        if (!res_.listing.empty()) {
            ListingLine& last = at_end && ended_ ? res_.listing[end_line_] : res_.listing.back();
            last.diagnostics.push_back(res_.diagnostics.size());
            file = last.file;
            line = last.file_line;
        }
        res_.diagnostics.push_back({sev, code, std::move(msg), file, line, 0});
    };
    if (!ended_) add(Severity::Warning, "A89", "premature end of file (no END statement)");
    if (!conds_.empty())
        add(Severity::Error, "A12", "unbalanced IF-ENDIF: " + std::to_string(conds_.size()) +
                                        " IF block(s) not closed by ENDIF");
    std::string open;
    if (proc_ != ProcKind::None) open = "procedure '" + proc_name_ + "'";
    for (auto i : open_) open += (open.empty() ? "section '" : ", section '") + sections_[i].name + "'";
    if (!open.empty()) add(Severity::Error, "A106", "unterminated " + open, true);
    return changed_;
}

// The PUBLIC and GLOBAL symbols of this pass that other modules can use, by
// the name the linker matches.
std::vector<std::pair<std::string, PublicDef>> Assembler::exported() const {
    std::vector<std::pair<std::string, PublicDef>> out;
    for (const auto& [k, e] : exports_) {
        PublicDef def{e.name, {}, SymbolKind::Equ, module_};
        if (auto it = symbols_.find(k); it != symbols_.end() && it->second.pass == pass_) {
            const Symbol& sym = it->second.sym;
            if ((sym.kind != SymbolKind::Label && sym.kind != SymbolKind::Equ && sym.kind != SymbolKind::Bit) ||
                sym.value.external())
                continue;
            def.value = sym.value;
            def.kind = sym.kind;
        } else if (auto p = predefined(e.name, cpu_, sys166_)) {
            def.value = *p;
        } else {
            continue;
        }
        out.emplace_back(k, std::move(def));
    }
    return out;
}

void Assembler::report(Severity sev, const char* code, std::string msg) {
    if (final_) res_.diagnostics.push_back({sev, code, std::move(msg), main_name_, 0, 0});
}

// Reports a problem on a line of the final pass, such as the first SECTION
// line of a section.
void Assembler::report_at(std::size_t listing, int column, Severity sev, const char* code, std::string msg) {
    if (!final_) return;
    if (listing >= res_.listing.size()) return report(sev, code, std::move(msg));
    ListingLine& l = res_.listing[listing];
    l.diagnostics.push_back(res_.diagnostics.size());
    res_.diagnostics.push_back({sev, code, std::move(msg), l.file, l.file_line, column});
}

void Assembler::process_line(std::string_view text) {
    const std::string& file = files_[file_]->name;
    ListingLine lst;
    lst.line = line_no_;
    lst.file = file;
    lst.file_line = file_line_;
    lst.level = level_;
    lst.source = std::string(text);
    // Keil lists the lines after END but does nothing else with them.
    if (ended_) {
        if (final_) res_.listing.push_back(std::move(lst));
        return;
    }
    Reporter rep;
    rep_ = &rep;
    if (!joined_.empty())
        rep.warning("OC8", "the last line of '" + joined_ +
                               "' has no newline at the end, so it was joined to this line (as in Keil)");
    std::size_t lead = text.find_first_not_of(" \t");
    const bool control_line = lead != std::string_view::npos && text[lead] == '$';
    const bool was_active = active();
    try {
        statement(text, rep, lst);
    } catch (const AsmError& e) {
        rep.error(e.code(), e.message(), e.column());
    }
    // Like Keil, a $IF line is numbered in the listing when the lines after
    // it are assembled, an IF statement when the lines before it are.
    // $NOCOND leaves out every line after which assembly is off.
    const bool now_active = active();
    lst.skipped = !(control_line ? now_active : was_active);
    lst.hidden = !now_active && !cond_listing_;
    rep_ = nullptr;
    if (!final_) return;
    for (auto& d : rep.items()) {
        d.file = file;
        d.line = file_line_;
        lst.diagnostics.push_back(res_.diagnostics.size());
        res_.diagnostics.push_back(std::move(d));
    }
    res_.listing.push_back(std::move(lst));
}

// Copies 'text' to 'out', replacing the name of each LIT defined earlier in
// this pass by the LIT's text, which is expanded the same way. Strings and
// comments are copied as they are. 'open' holds the LITs being expanded: a
// name whose text leads back to it stays, and lookup() reports it.
// keep_file_names leaves the parentheses after INCLUDE, IC, INCDIR and ID
// alone. Returns true if a name was replaced.
bool Assembler::expand_lits(std::string_view text, std::string& out, std::vector<std::string>& open,
                            bool keep_file_names) {
    bool replaced = false;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t j = i + 1;
        if (c == ';') {
            j = text.size();
        } else if (c == '\'' || c == '"') {
            while (j < text.size()) {
                if (static_cast<unsigned char>(text[j++]) != c) continue;
                if (j < text.size() && static_cast<unsigned char>(text[j]) == c) {
                    ++j;
                    continue;
                }
                break;
            }
        } else if (std::isdigit(c)) {
            while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '$')) ++j;
        } else if (ident_start(text[i])) {
            while (j < text.size() && ident_char(text[j])) ++j;
            if (keep_file_names) {
                const std::string word = upper(text.substr(i, j - i));
                std::size_t p = text.find_first_not_of(" \t", j);
                if ((word == "INCLUDE" || word == "IC" || word == "INCDIR" || word == "ID") &&
                    p != std::string_view::npos && text[p] == '(') {
                    std::size_t close = text.find(')', p);
                    j = close == std::string_view::npos ? text.size() : close + 1;
                    out.append(text, i, j - i);
                    i = j;
                    continue;
                }
            }
            std::string k = key(text.substr(i, j - i));
            auto it = symbols_.find(k);
            if (it != symbols_.end() && it->second.sym.kind == SymbolKind::Lit && it->second.pass == pass_ &&
                std::find(open.begin(), open.end(), k) == open.end()) {
                open.push_back(std::move(k));
                expand_lits(it->second.sym.text, out, open);
                open.pop_back();
                if (out.size() > kMaxExpandedLine)
                    throw AsmError("OC11", "line longer than " + std::to_string(kMaxExpandedLine) +
                                               " characters after LIT replacement");
                replaced = true;
                i = j;
                continue;
            }
        }
        out.append(text, i, j - i);
        i = j;
    }
    return replaced;
}

void Assembler::statement(std::string_view text, Reporter& rep, ListingLine& lst) {
    std::size_t lead = text.find_first_not_of(" \t");
    if (lead == std::string_view::npos) return;
    // Like Keil, replace LIT names before reading the line, in $ controls too,
    // and list the result. Lines that a false IF block skips stay as they are.
    std::string expanded;
    if (lits_) {
        std::size_t end = lead;
        while (end < text.size() && word_char(text[end])) ++end;
        const bool control = text[lead] == '$';
        const bool cond_line = control || contains(kCondKeywords, upper(text.substr(lead, end - lead)));
        const bool parent_active = conds_.empty() || conds_.back().parent_active;
        std::vector<std::string> open;
        if ((active() || (cond_line && parent_active)) && expand_lits(text, expanded, open)) {
            lst.source = expanded;
            // Keil lists the file names of $INCLUDE and $INCDIR replaced, but
            // uses them as written.
            if (control) {
                expanded.clear();
                expand_lits(text, expanded, open, true);
            }
            text = expanded;
            lead = text.find_first_not_of(" \t");
            if (lead == std::string_view::npos) return;
        }
    }
    if (text[lead] == '$') {
        controls(text, lead + 1, rep);
        return;
    }
    // Lines in false IF blocks are not assembled, so they may hold anything;
    // only a first word IF, ELSEIF, ELSE or ENDIF counts, as in Keil.
    std::size_t end = lead;
    while (end < text.size() && word_char(text[end])) ++end;
    std::string word = upper(text.substr(lead, end - lead));
    if (contains(kCondKeywords, word)) {
        cond_statement(word, text, end, rep);
        return;
    }
    if (!active()) return;

    auto toks = tokenize(text);
    TokenCursor cur(toks);
    if (cur.at_end()) return;
    primary_done_ = true;

    if (cur.peek().kind == TokKind::Ident && cur.peek(1).is_op(":")) {
        const Token& label = cur.next();
        cur.next();
        // Keil rejects 'label: name EQU ...' and the like. Inside a section the
        // label is still defined, and both the name and the directive are
        // reported; outside one, the name is reported and nothing is defined.
        const Token& name = cur.peek();
        const Token& dir = cur.peek(1);
        const bool named = name.kind == TokKind::Ident && dir.kind == TokKind::Ident &&
                           contains(kNamedDirectives, upper(dir.text));
        const std::string misplaced = "syntax error: a label cannot come before '" + std::string(name.text) + " " +
                                      upper(dir.text) + "'";
        if (named && cur_ == 0) throw AsmError("A6", misplaced, name.column);
        // A label in front of data takes the type and alignment of the data;
        // otherwise it is a NEAR label in code sections and needs an even address.
        std::string next = cur.peek().kind == TokKind::Ident ? upper(cur.peek().text) : std::string();
        bool data_follows = contains(kDataDirectives, next);
        if (data_follows) {
            // Keil reports the data and the line, and leaves the label undefined.
            const int column = cur.peek().column;
            try {
                check_byte_section(data_alignment(next), column);
            } catch (const AsmError& e) {
                rep.error(e.code(), e.message(), e.column());
                throw AsmError("A6", "syntax error: the label is ignored", column);
            }
            align(data_alignment(next));
        }
        lst.has_address = true;
        lst.address = location();
        // Keil gives plain labels outside CODE sections the type BYTE.
        LType type = data_follows ? data_type(next) : section().code ? LType::Near : LType::Byte;
        define(label, SymbolKind::Label, here(type));
        if (!data_follows && section().code) check_alignment(2, label, rep);
        if (named) {
            rep.error("A6", misplaced, name.column);
            throw AsmError("A6", "syntax error: " + upper(dir.text) + " is ignored after a label", dir.column);
        }
        if (cur.at_end()) return;
        // Keil defines the label, reports the line and still obeys the IF.
        if (const Token& t = cur.peek(); t.kind == TokKind::Ident && contains(kCondKeywords, upper(t.text))) {
            rep.error("A6", "syntax error: " + upper(t.text) + " cannot have a label", t.column);
            cond_statement(upper(t.text), text, static_cast<std::size_t>(t.column - 1) + t.text.size(), rep);
            return;
        }
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

// A $ line: 'from' is the index after the '$'.
void Assembler::controls(std::string_view text, std::size_t from, Reporter& rep) {
    struct Control {
        std::string name;
        std::size_t arg = 0;  // index of the text inside the parentheses
        std::size_t arg_end = 0;
        bool paren = false;
        int column = 0;
    };
    std::size_t i = from;
    auto skip_space = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    };
    auto read_name = [&] {
        std::size_t start = i;
        while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
        return upper(text.substr(start, i - start));
    };
    // In a false IF block only the first control counts, and only if it is
    // one of $IF, $ELSEIF, $ELSE and $ENDIF.
    skip_space();
    int first_column = static_cast<int>(i) + 1;
    if (std::string first = read_name(); contains(kCondKeywords, first)) {
        cond_control(first, text, i, first_column, false, rep);
        return;
    }
    if (!active()) return;

    std::vector<Control> list;
    std::optional<Control> cond;  // $IF and the like after another control
    for (i = from;;) {
        skip_space();
        if (i >= text.size() || text[i] == ';') break;
        Control c;
        c.column = static_cast<int>(i) + 1;
        c.name = read_name();
        if (c.name.empty()) throw AsmError("A16", "unknown control or bad argument(s)", c.column);
        if (contains(kCondKeywords, c.name)) {
            c.arg = i;
            cond = std::move(c);
            break;
        }
        skip_space();
        c.paren = i < text.size() && text[i] == '(';
        if (c.name == "INCLUDE" || c.name == "IC") {
            // The file name runs to the first ')'; Keil takes quotes and
            // spaces inside it literally.
            std::size_t close = c.paren ? text.find(')', i) : std::string_view::npos;
            if (close == std::string_view::npos) {
                const char* msg = c.paren ? "')' expected" : "'(' expected";
                if (!list.empty()) throw AsmError("A22", msg, c.column);
                rep.error("A22", msg, c.column);
                throw AsmError("A171", "error in include directive", c.column);
            }
            c.arg = i + 1;
            c.arg_end = close;
            i = close + 1;
        } else if (c.paren) {
            std::size_t open = i;
            int depth = 0;
            for (; i < text.size(); ++i) {
                if (text[i] == '(') ++depth;
                if (text[i] == ')' && --depth == 0) break;
            }
            if (i >= text.size()) throw AsmError("A22", "')' expected", c.column);
            c.arg = open + 1;
            c.arg_end = i;
            ++i;
        }
        list.push_back(std::move(c));
    }
    auto arg = [&](const Control& c) { return text.substr(c.arg, c.arg_end - c.arg); };

    auto add_incdirs = [&](std::string_view dirs) {
        for (std::size_t pos = 0; pos <= dirs.size();) {
            std::size_t semi = std::min(dirs.find(';', pos), dirs.size());
            if (std::string dir = portable_path(dirs.substr(pos, semi - pos)); !dir.empty())
                incdirs_.push_back(std::move(dir));
            pos = semi + 1;
        }
    };
    // Primary controls must come before the first statement; otherwise Keil
    // ignores the whole line (though it still uses INCDIR directories).
    if (primary_done_) {
        auto primary = std::find_if(list.begin(), list.end(), [](const Control& c) {
            return contains(kPrimaryControls, c.name);
        });
        if (primary != list.end()) {
            for (const auto& c : list)
                if (c.name == "INCDIR" || c.name == "ID") add_incdirs(arg(c));
            throw AsmError("A8", "misplaced primary control, line ignored: $" + primary->name +
                                     " must come before the first statement",
                           primary->column);
        }
    }

    for (std::size_t k = 0; k < list.size(); ++k) {
        const std::string& name = list[k].name;
        int column = list[k].column;
        if (name == "INCLUDE" || name == "IC") {
            if (k > 0) {
                // Keil silently skips an INCLUDE that follows another control.
                rep.warning("OC7", "$" + name + " ignored: it must be the first control on the line (as in Keil)",
                            column);
                continue;
            }
            std::string_view file = trim(arg(list[k]));
            if (file.empty()) {
                rep.error("A20", "non-null argument expected", column);
                throw AsmError("A171", "error in include directive", column);
            }
            if (list.size() > 1 || cond)
                throw AsmError("A171", "error in include directive: no other control may follow it", column);
            include(file, column);
        } else if (name == "INCDIR" || name == "ID") {
            add_incdirs(arg(list[k]));
        } else if (name == "MOD167" || name == "M167") {
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
        } else if (name == "SEGMENTED" || name == "SG") {
            segmented_ = true;
        } else if (name == "NONSEGMENTED" || name == "NOSG") {
            segmented_ = false;
        } else if (name == "CASE") {
            case_ = true;
        } else if (name == "NOCASE") {
            case_ = false;
        } else if (name == "SET" || name == "RESET") {
            if (!list[k].paren) throw AsmError("A22", "'(' expected", column);
            set_flags(text, list[k].arg, list[k].arg_end, name == "SET");
        } else if (name == "COND") {
            cond_listing_ = true;
        } else if (name == "NOCOND") {
            cond_listing_ = false;
        } else if (!contains(kIgnoredControls, name) && !contains(kPrimaryControls, name)) {
            throw AsmError("A16", "unknown control '" + name + "'", column);
        }
    }
    if (cond) cond_control(cond->name, text, cond->arg, cond->column, true, rep);
}

// $SET (name [= number], ...) and $RESET (name, ...). Keil takes only a plain
// number as the value, not an expression.
void Assembler::set_flags(std::string_view text, std::size_t from, std::size_t to, bool set) {
    auto toks = tokenize_from(text.substr(0, to), from);
    TokenCursor cur(toks);
    do {
        const Token& name = cur.next();
        if (name.kind != TokKind::Ident) throw AsmError("A23", "identifier expected", name.column);
        std::int64_t value = set ? 1 : 0;
        if (set && cur.accept_op("=")) {
            const Token& t = cur.next();
            if (t.kind == TokKind::String)
                throw AsmError("A1", "illegal character in numeric constant: $SET takes a number", t.column);
            if (t.kind != TokKind::Number) throw AsmError("A22", "')' expected: $SET takes a plain number", t.column);
            value = static_cast<std::int64_t>(t.num);
        }
        if (!cur.at_end() && !cur.peek().is_op(",")) throw AsmError("A22", "')' expected", cur.peek().column);
        flags_[upper(name.text)] = value;
    } while (cur.accept_op(","));
}

// $IF, $ELSEIF, $ELSE and $ENDIF; 'after' is the index after the name. Keil
// reports a misplaced one (after another control on the line) but still
// obeys it, taking the condition as false.
void Assembler::cond_control(const std::string& kw, std::string_view text, std::size_t after, int column,
                             bool misplaced, Reporter& rep) {
    if (misplaced) rep.error("A6", "syntax error: $" + kw + " must be the first control on its line", column);
    // Keil reports text after $ELSE and $ENDIF but still obeys them.
    auto check_end = [&] {
        std::size_t p = text.find_first_not_of(" \t", after);
        if (!misplaced && p != std::string_view::npos && text[p] != ';')
            rep.error("A6", "syntax error: unexpected text after $" + kw, static_cast<int>(p) + 1);
    };
    if (kw == "IF") {
        if (conds_.size() >= kMaxControlIfDepth) {
            if (active())
                rep.error("A6", "syntax error: $IF nested too deeply (Keil allows " +
                                    std::to_string(kMaxControlIfDepth) + " levels)",
                          column);
            return;
        }
        if (!active()) {
            conds_.push_back({false, false, true, false});
            return;
        }
        bool c = !misplaced && control_condition(text, after, column, rep);
        conds_.push_back({true, c, c, false});
        return;
    }
    if (kw == "ELSEIF") {
        if (conds_.empty()) {
            rep.error("A6", "syntax error: $ELSEIF without $IF", column);
            return;
        }
        Cond& top = conds_.back();
        if (!top.parent_active) return;
        if (top.in_else) {
            rep.error("A6", "syntax error: $ELSEIF after $ELSE", column);
            return;
        }
        bool c = !misplaced && control_condition(text, after, column, rep);
        top.active = !top.taken && c;
        top.taken = top.taken || c;
        return;
    }
    if (conds_.empty()) throw AsmError("A17", "misplaced $" + kw + ": no IF block is open", column);
    if (kw == "ELSE") {
        Cond& top = conds_.back();
        if (!top.parent_active) return;
        if (top.in_else) throw AsmError("A17", "misplaced $ELSE: this IF block already has an ELSE", column);
        top.in_else = true;
        top.active = !top.taken;
        check_end();
        return;
    }
    bool live = conds_.back().parent_active;
    conds_.pop_back();
    if (live) check_end();
}

// The condition of $IF and $ELSEIF: the rest of the line. Keil reports text
// after it but still uses the value.
bool Assembler::control_condition(std::string_view text, std::size_t after, int column, Reporter& rep) {
    try {
        auto toks = condition_tokens(text, after);
        TokenCursor cur(toks);
        FlagContext ctx(flags_);
        Value v = condition_value(cur, ctx, column);
        if (ctx.dollar) throw AsmError("A6", "syntax error: '$' cannot be used in a condition", column);
        if (!cur.at_end()) rep.error("A6", "syntax error: unexpected text after the condition", cur.peek().column);
        return condition_truth(v, column, rep);
    } catch (const AsmError& e) {
        rep.error(e.code(), e.message(), e.column());
        return false;
    }
}

// The condition of an IF or ELSEIF statement; false if it has an error.
bool Assembler::statement_condition(std::string_view text, std::size_t after, int column, Reporter& rep) {
    static constexpr const char* kForward = "IF condition uses a symbol that is not defined before this line";
    try {
        auto toks = condition_tokens(text, after);
        TokenCursor cur(toks);
        int at = cur.at_end() ? column : cur.peek().column;
        ConditionContext ctx(*this);
        Value v;
        try {
            v = condition_value(cur, ctx, column);
        } catch (const AsmError& e) {
            if (e.code() != "A42") throw;
            throw AsmError("A71", kForward, e.column());
        }
        if (ctx.dollar || ctx.condition_code)
            throw AsmError("A6", "syntax error: '$' and condition codes cannot be used in a condition", at);
        if (ctx.forward || !v.known) throw AsmError("A71", kForward, at);
        if (ctx.unusable || v.rel != Rel::None || v.type != VType::Number || v.external())
            throw AsmError("A11", "bad conditional expression: labels, registers, SFRs and bits cannot be used "
                                  "(Keil silently takes such a condition as false)",
                           at);
        if (!cur.at_end()) rep.warning("OC9", "text after the condition is ignored (as in Keil)", cur.peek().column);
        return condition_truth(v, at, rep);
    } catch (const AsmError& e) {
        rep.error(e.code(), e.message(), e.column());
        return false;
    }
}

// IF, ELSEIF, ELSE and ENDIF statements; 'after' is the index after the
// keyword. Keil accepts odd structures such as a stray ENDIF or a second
// ELSE without a word; openc166 does the same but warns.
void Assembler::cond_statement(const std::string& kw, std::string_view text, std::size_t after, Reporter& rep) {
    const int column = static_cast<int>(after - kw.size()) + 1;
    auto check_end = [&] {
        std::vector<Token> toks;
        try {
            toks = tokenize_from(text, after);
        } catch (const AsmError&) {
            rep.warning("OC9", "text after " + kw + " is ignored (as in Keil)", static_cast<int>(after) + 1);
            return;
        }
        const Token& t = toks.front();
        if (t.kind == TokKind::End) return;
        if (t.kind == TokKind::Ident && contains(kCondKeywords, upper(t.text)))
            throw AsmError("A6", "syntax error: " + upper(t.text) + " cannot follow " + kw + " on the same line",
                           t.column);
        rep.warning("OC9", "text after " + kw + " is ignored (as in Keil)", t.column);
    };
    if (kw == "IF") {
        if (!active()) {
            conds_.push_back({false, false, true, false});
            return;
        }
        bool c = statement_condition(text, after, column, rep);
        conds_.push_back({true, c, c, false});
        return;
    }
    if (conds_.empty()) {
        rep.warning("OC9", kw + " without IF is ignored (as in Keil)", column);
        return;
    }
    // Keil handles a second conditional keyword on the line erratically, so
    // openc166 reports it and ignores the line.
    if (kw == "ENDIF") {
        if (conds_.back().parent_active) check_end();
        conds_.pop_back();
        return;
    }
    Cond& top = conds_.back();
    if (!top.parent_active) return;
    if (kw == "ELSEIF") {
        if (top.in_else)
            rep.warning("OC9", "ELSEIF after ELSE: Keil still assembles its lines if no IF or ELSEIF before was true",
                        column);
        bool c = statement_condition(text, after, column, rep);
        top.active = !top.taken && c;
        top.taken = top.taken || c;
        return;
    }
    check_end();
    if (top.in_else) rep.warning("OC9", "second ELSE: Keil assembles it too if no IF or ELSEIF was true", column);
    top.in_else = true;
    top.active = !top.taken;
}

// Finds an include file and arranges for it to be read after the current
// line. Like Keil, a missing file ends the assembly.
void Assembler::include(std::string_view name, int column) {
    if (stack_.size() > kMaxIncludeDepth) {
        fatal_ = true;
        throw AsmError("OC6", "include files nested too deeply (does a file include itself?)", column);
    }
    std::string wanted = portable_path(name);
    std::string request = wanted;
    for (const auto& d : incdirs_) request += "\n" + d;
    auto [it, inserted] = includes_.try_emplace(request, -1);
    if (inserted) {
        std::string main_dir = directory_of(main_name_);
        std::vector<std::string> dirs{"", main_dir};
        for (const auto& d : opt_.include_dirs) dirs.push_back(portable_path(d));
        for (const auto& d : incdirs_) {
            dirs.push_back(d);
            // Keil takes a relative $INCDIR from the current directory only;
            // also trying the source directory lets the source be assembled
            // from anywhere.
            if (!is_absolute_path(d) && !main_dir.empty()) dirs.push_back(join_path(main_dir, d));
        }
        for (const auto& dir : dirs) {
            std::string path = join_path(dir, wanted);
            auto text = opt_.read_file ? opt_.read_file(path) : read_file_any_case(path);
            if (!text) continue;
            it->second = static_cast<int>(add_file(path, std::move(*text)));
            break;
        }
        // Last resort for a bare file name: the copy built into openc166.
        if (it->second < 0 && wanted.find('/') == std::string::npos) {
            if (auto f = bundled_file(wanted))
                it->second = static_cast<int>(add_file("<bundled>/" + std::string(f->name), std::string(f->text)));
        }
    }
    if (it->second < 0) {
        fatal_ = true;
        throw AsmError("OC5", "cannot open include file '" + std::string(name) + "'", column);
    }
    include_ = static_cast<std::size_t>(it->second);
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
        if (kind != SymbolKind::Bit && v.type == VType::Bit && !v.external())
            throw AsmError("A70", "invalid type of expression: use BIT for bit addresses", column);
        // A DPPn: prefix stays with a label; a number takes the DPP number
        // into bits 14 and up (without clearing them first, as Keil does).
        if (v.dpp >= 0 && v.rel != Rel::Addr) {
            v.num |= std::int64_t{v.dpp} << 14;
            v.dpp = -1;
        }
        // Only a plain label keeps its section for ASSUME; a section name or
        // "label + n" becomes a plain number.
        if (v.rel != Rel::Addr) v.section = 0;
        v.data_bits = 0;
        define(name, kind, v, kind == SymbolKind::Bit);
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
        section_start(name, cur, rep);
    } else if (dir == "ENDS") {
        expect_end(cur);
        section_end(name, rep);
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
            define(name, SymbolKind::Equ, d, sfr);
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
        const Token& t = cur.peek();
        if (t.kind != TokKind::String) throw AsmError("A6", "syntax error: LIT expects a quoted string", t.column);
        cur.next();
        auto bytes = string_bytes(t);
        std::string text(bytes.begin(), bytes.end());
        auto old = symbols_.find(key(name.text));
        std::string old_text = old != symbols_.end() ? old->second.sym.text : std::string();
        define(name, SymbolKind::Lit, Value::number(0));
        if (old_text != text) changed_ = true;
        symbols_.at(key(name.text)).sym.text = std::move(text);
        lits_ = true;
        // Keil keeps the first string of 'name LIT 'a', 'b''.
        expect_end(cur);
    } else if (contains(kDataDirectives, dir)) {
        check_byte_section(data_alignment(dir), name.column);
        align(data_alignment(dir));
        lst.has_address = true;
        lst.address = location();
        define(name, SymbolKind::Label, here(data_type(dir)));
        data(dir, cur, rep, lst);
    } else if (dir == "DGROUP" || dir == "CGROUP") {
        group(name, dir == "CGROUP", cur, rep);
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
        std::int64_t v = absolute(cur, "ORG", true);
        expect_end(cur);
        Section& s = section();
        std::int64_t target = s.absolute ? v : s.base + v;
        if (target < s.base) throw AsmError("A132", "location counter may not point below section base", column);
        // Like Keil, the location counter stays where it was.
        if (std::string why = length_overflow(s, target); !why.empty())
            throw AsmError("A86", "section length overflow: " + why, column);
        s.loc = target;
        free_ = std::max(free_, target);
    } else if (kw == "EVEN") {
        expect_end(cur);
        check_byte_section(2, tok.column);
        if (align_offset() & 1) advance(1);
    } else if (kw == "END") {
        expect_end(cur);
        ended_ = true;
        end_line_ = res_.listing.size();
    } else if (contains(kDataDirectives, kw)) {
        check_byte_section(data_alignment(kw), tok.column);
        align(data_alignment(kw));
        lst.has_address = true;
        lst.address = location();
        data(kw, cur, rep, lst);
    } else if (kw == "ASSUME") {
        assume(cur, rep);
    } else if (kw == "EXTERN" || kw == "EXTRN") {
        extern_directive(cur, rep);
    } else if (kw == "PUBLIC" || kw == "GLOBAL") {
        public_directive(kw == "GLOBAL", cur, rep);
    } else if (contains(kIgnoredDirectives, kw)) {
        // No effect on a raw image.
    } else if (kw == "ENDS" || kw == "ENDP") {
        throw AsmError("A6", "syntax error: " + kw + " needs the name of the " +
                                 (kw == "ENDS" ? "section" : "procedure"),
                       tok.column);
    } else if (kw == "DGROUP" || kw == "CGROUP") {
        throw AsmError("A6", "syntax error: " + kw + " needs the name of the group", tok.column);
    } else if (contains(kKeywords, kw)) {
        throw AsmError("OC3", "directive " + kw + " is not supported yet", tok.column);
    } else {
        throw AsmError("A6", "syntax error: unknown instruction or directive '" + std::string(tok.text) + "'",
                       tok.column);
    }
}

void Assembler::data(const std::string& dir, TokenCursor& cur, Reporter& rep, ListingLine& lst) {
    if (is_pointer_directive(dir)) return pointer_data(dir, cur, rep, lst);
    if (dir == "DS" || dir == "DSB" || dir == "DSW" || dir == "DSD") {
        int column = cur.peek().column;
        std::int64_t n = absolute(cur, dir.c_str());
        expect_end(cur);
        if (n < 0) throw AsmError("A19", "numeric value out of range", column);
        advance(n * (dir == "DSW" ? 2 : dir == "DSD" ? 4 : 1));
        // Keil still reserves the space after this error.
        if (std::string why = length_overflow(section(), section().loc); !why.empty())
            rep.error("A86", "section length overflow: " + why, column);
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
            item_offset_ = static_cast<std::int64_t>(out.size());
            try {
                // Keil skips the range check of an item that starts with an
                // (ignored) DPPn: prefix in nonsegmented mode.
                const bool unchecked = !segmented_ && dpp_prefix_at(cur) >= 0;
                Value v = expression(cur);
                if (v.type != VType::Number && v.type != VType::Sfr)
                    throw AsmError("A70", "invalid type of expression", t.column);
                // Data keeps the address of a label even with a DPPn: prefix,
                // but the linker applies the DPP of an EXTERN.
                if (v.dpp >= 0 && (v.rel != Rel::Addr || v.external())) v.num = v.dpp_address();
                assumed_section_page(v);
                if (v.known && v.external() && width == 1 && !link_fits(v.num, 8))
                    rep.error("OC18", "the external value " + hex(v.num, 4) +
                                          "H does not fit in 8 bits (L166 reports L121: improper fixup)",
                              t.column);
                else if (v.known && !unchecked && !v.external() && !fits_width(v, 8 * width))
                    rep.warning("A62", "value truncation", t.column);
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
            item_offset_ = 0;
            if (keep)
                for (int b = 0; b < width; ++b) out.push_back(static_cast<std::uint8_t>(num >> (8 * b)));
        }
        if (cur.at_end()) break;
        cur.expect_op(",");
    }
    emit(out.data(), out.size(), lst);
}

// DPPTR, DSPTR and DBPTR store an address in two or three words, for code
// that loads a DPP or segment register from a table:
//   DPPTR addr   page offset (addr & 3FFFH), page number (addr >> 14)
//   DSPTR addr   segment offset (addr & 0FFFFH), segment number (addr >> 16)
//   DBPTR bit    bit number, then the DPPTR words of the bit's word address
void Assembler::pointer_data(const std::string& dir, TokenCursor& cur, Reporter& rep, ListingLine& lst) {
    if (cur.at_end()) throw AsmError("A6", "syntax error: " + dir + " expects an expression", cur.peek().column);
    const bool bit = dir == "DBPTR";
    const int words = bit ? 3 : 2;
    std::vector<std::uint8_t> out;
    auto word = [&](std::int64_t w) {
        out.push_back(static_cast<std::uint8_t>(w));
        out.push_back(static_cast<std::uint8_t>(w >> 8));
    };
    for (;;) {
        const Token& t = cur.peek();
        item_offset_ = static_cast<std::int64_t>(out.size());
        try {
            Value v = expression(cur);
            item_offset_ = 0;
            if (v.known && (bit ? v.type != VType::Bit || bit_word_address(v) < 0
                                : v.type != VType::Number && v.type != VType::Sfr))
                throw AsmError("A70", std::string("invalid type of expression: ") +
                                          (bit ? "DBPTR needs a bit address" : dir + " needs an address"),
                               t.column);
            if (v.dpp >= 0 && v.rel != Rel::Addr) v.num = v.dpp_address();
            if (!bit) assumed_section_page(v);
            std::int64_t a = bit ? (v.known ? bit_word_address(v) : 0) : v.num;
            if (bit) word(v.bit);
            if (dir == "DSPTR") {
                word(a & 0xFFFF);
                word(a >> 16);
            } else {
                word(a & 0x3FFF);
                word(a >> 14);
            }
        } catch (const AsmError& e) {
            item_offset_ = 0;
            rep.error(e.code(), e.message(), e.column());
            if (e.code() == "A42") out.resize(out.size() + 2 * words);
            for (int depth = 0; !cur.at_end() && !(depth == 0 && cur.peek().is_op(","));) {
                const Token& skipped = cur.next();
                if (skipped.is_op("(")) ++depth;
                else if (skipped.is_op(")") && depth > 0) --depth;
            }
        }
        if (cur.at_end()) break;
        cur.expect_op(",");
    }
    emit(out.data(), out.size(), lst);
}

// EXTERN [DPPn:]name:type, ... declares symbols that another module defines
// and makes PUBLIC or GLOBAL. Like Keil, an error stops the list, except
// for a REGBANK type.
void Assembler::extern_directive(TokenCursor& cur, Reporter& rep) {
    for (;;) {
        int dpp = -1;
        bool stop = false;
        // A register in front of ':' selects the data page, so it must be DPP0..DPP3.
        if (const Token& t = cur.peek(); t.kind == TokKind::Ident && cur.peek(1).is_op(":")) {
            const int n = dpp_number(t.text);
            const std::optional<Value> pre = predefined(t.text, cpu_, sys166_);
            if (n >= 0 || (pre && (pre->type == VType::Gpr || pre->type == VType::GprB))) {
                if (n < 0) {
                    rep.error("A35", "'DPPn' expected", t.column);
                    stop = true;
                } else if (!segmented_) {
                    rep.warning("A55", "'DPP' in nonsegmented mode ignored", t.column);
                } else {
                    dpp = n;
                }
                cur.next();
                cur.next();
            }
        }
        const Token& name = cur.peek();
        if (name.kind != TokKind::Ident || contains(kExternTypes, upper(name.text)) || ieq(name.text, "REGBANK"))
            throw AsmError("A23", "identifier expected", name.column);
        const std::string u = upper(name.text);
        if (isa::is_mnemonic(u) || is_operator_keyword(u) || contains(kKeywords, u))
            throw AsmError("A4", "illegal use of a reserved word", name.column);
        cur.next();
        if (!cur.accept_op(":")) {
            rep.error("A6", "syntax error", name.column);
            throw AsmError("A36", "':' expected", cur.peek().column);
        }
        const Token& type = cur.peek();
        const std::string t = type.kind == TokKind::Ident ? upper(type.text) : std::string();
        if (t == "REGBANK") {
            cur.next();
            rep.error("A113", "PUBLIC/EXTERN on REGBANK not allowed", name.column);
        } else if (!contains(kExternTypes, t)) {
            rep.error("A6", "syntax error", name.column);
            throw AsmError("A37", "type identifier expected: NEAR, FAR, BYTE, WORD, DWORD, BIT, BITWORD, DATA3, "
                                  "DATA4, DATA8, DATA16 or INTNO",
                           type.column);
        } else {
            cur.next();
            declare_extern(name, t, dpp, rep);
        }
        if (stop || cur.at_end()) return;
        const Token& comma = cur.peek();
        if (!cur.accept_op(",")) throw AsmError("A6", "syntax error: ',' expected", comma.column);
        if (cur.at_end()) throw AsmError("A6", "syntax error: name expected after ','", comma.column);
    }
}

// Defines an EXTERN symbol with the value of the PUBLIC or GLOBAL symbol of
// that name in another module, as found in the previous pass. The value
// keeps the linker's view: an address of the declared type, a DATAn number
// for the short immediate forms, a bit, and so on. Like L166, the final
// pass reports a missing definition and a code type for something that is
// not code here, but a value that does not suit the type only where it is
// used (external_used()).
void Assembler::declare_extern(const Token& name, const std::string& type, int dpp, Reporter& rep) {
    const std::string k = key(name.text);
    if (auto it = exports_.find(k); it != exports_.end())
        throw AsmError("A25", "symbol redefinition: '" + std::string(name.text) + "' is " +
                                  (it->second.global ? "GLOBAL" : "PUBLIC") + " in this module",
                       name.column);
    const std::uint16_t id = static_cast<std::uint16_t>(Value::kExternBase + externs_++);
    auto pub_it = link_.publics.find(k);
    const PublicDef* pub = pub_it != link_.publics.end() && pub_it->second.module != module_ ? &pub_it->second : nullptr;
    const std::string who = "'" + std::string(name.text) + "'";
    if (final_ && !pub)
        rep.error("OC15", "unresolved external " + who + ": no other module makes it PUBLIC or GLOBAL", name.column);

    std::string bad;
    Value v = Value::number(type == "BITWORD" ? 0xFD00 : 0);
    v.unsig = true;
    v.movable = id;
    if (pub) {
        const Value& p = pub->value;
        const std::string where = " in " + link_.modules[pub->module];
        if (type == "BIT") {
            if (p.type == VType::Bit && bit_word_address(p) >= 0) {
                v.type = VType::Bit;
                v.num = p.num;
                v.bit = p.bit;
                v.ext = p.ext;
            } else {
                bad = "it is not a bit" + where;
            }
        } else if (p.type == VType::Bit) {
            if (bit_word_address(p) >= 0) v.num = bit_word_address(p);
        } else if (p.type == VType::Number || p.type == VType::Sfr) {
            v.num = p.num;
        } else {
            bad = "it is a register or condition code" + where;
        }
        const std::int64_t w = v.num & ~std::int64_t{1};
        if (type == "BITWORD" && bad.empty() &&
            !((w >= 0xFD00 && w <= 0xFDFE) || (w >= 0xFF00 && w <= 0xFFDE) || (w >= 0xF100 && w <= 0xF1DE))) {
            bad = hex(v.num, 4) + "H is not a bit-addressable word" + where;
            v.num = 0xFD00;
        }
        if ((type == "NEAR" || type == "FAR") && p.ltype != LType::Near && p.ltype != LType::Far && final_)
            rep.warning("OC17", "external " + who + " is " + type + ", but it is not a code label" + where +
                                    " (L166 warns L21)",
                        name.column);
    }
    if (type == "BIT") {
        v.type = VType::Bit;
        v.unsig = false;
    } else if (type == "NEAR" || type == "FAR" || type == "BYTE" || type == "WORD" || type == "DWORD") {
        v.rel = Rel::Addr;
        v.section = id;
        v.ltype = type == "NEAR"   ? LType::Near
                : type == "FAR"    ? LType::Far
                : type == "BYTE"   ? LType::Byte
                : type == "WORD"   ? LType::Word
                                   : LType::Dword;
        v.dpp = static_cast<std::int8_t>(dpp);
    } else if (type.rfind("DATA", 0) == 0) {
        v.data_bits = static_cast<std::uint8_t>(std::stoi(type.substr(4)));
    }
    if (!bad.empty()) extern_bad_[id] = "external " + who + " cannot be " + type + ": " + bad;
    define(name, SymbolKind::Extern, v);
    symbols_.at(k).sym.text = type;
}

// Like L166, which checks each place the linker fills in, reports a use of
// an EXTERN symbol whose PUBLIC definition does not suit its type.
void Assembler::external_used(const Value& v, int column) {
    if (!final_ || !rep_) return;
    if (auto it = extern_bad_.find(v.movable); it != extern_bad_.end())
        rep_->error("OC18", it->second + " (L166 reports L121: improper fixup)", column);
}

// PUBLIC name, ... and GLOBAL name, ... let other modules use symbols of
// this one. Like Keil, the final pass checks each name on its line once the
// whole module is known.
void Assembler::public_directive(bool global, TokenCursor& cur, Reporter& rep) {
    for (;;) {
        const Token& t = cur.peek();
        if (t.kind != TokKind::Ident || is_operator_keyword(upper(t.text)))
            throw AsmError("A23", "identifier expected", t.column);
        cur.next();
        auto [it, inserted] = exports_.try_emplace(key(t.text), Export{std::string(t.text), global});
        if (!inserted && it->second.global != global)
            throw AsmError("A25", "symbol redefinition: '" + std::string(t.text) + "' is already " +
                                      (global ? "PUBLIC" : "GLOBAL"),
                           t.column);
        if (final_) check_public(t, rep);
        if (cur.at_end()) return;
        if (!cur.accept_op(",")) throw AsmError("A6", "syntax error: ',' expected", cur.peek().column);
    }
}

bool Assembler::section_name(const std::string& name_key) const {
    for (const auto* list : {&sections_, &prev_sections_})
        for (std::size_t i = 1; i < list->size(); ++i)
            if (key((*list)[i].name) == name_key) return true;
    return false;
}

// A PUBLIC or GLOBAL name must be a label, an EQU, BIT, DEFR, DEFA or DEFB
// symbol, or a predefined register, that no other module also exports.
void Assembler::check_public(const Token& name, Reporter& rep) {
    const std::string k = key(name.text), u = upper(name.text);
    auto illegal = [&](const std::string& why) {
        rep.error("A39", "illegal PUBLIC/GLOBAL symbol: '" + std::string(name.text) + "' " + why, name.column);
    };
    if (isa::is_mnemonic(u) || contains(kKeywords, u)) return illegal("is a reserved word");
    if (section_name(k)) return illegal("is a section");
    if (find_group(k, true)) return illegal("is a group");
    if (auto it = symbols_.find(k); it != symbols_.end() && it->second.pass >= pass_ - 1) {
        const Symbol& sym = it->second.sym;
        if (sym.kind == SymbolKind::Set) return illegal("is a SET symbol");
        if (sym.kind == SymbolKind::Lit) return illegal("is a LIT");
        if (sym.kind == SymbolKind::Extern) return illegal("is EXTERN");
        if (sym.value.external()) return illegal("depends on an EXTERN symbol");
    } else if (!predefined(name.text, cpu_, sys166_)) {
        return rep.error("A42", "undefined identifier '" + std::string(name.text) + "'", name.column);
    }
    if (auto p = link_.publics.find(k); p != link_.publics.end() && p->second.module != module_)
        rep.error("OC16", "'" + std::string(name.text) + "' is also PUBLIC or GLOBAL in " +
                              link_.modules[p->second.module],
                  name.column);
}

// A section name stands for the section's start address. Keil applies the
// DPP that ASSUME ties to a data section to it, but not to a code section.
std::optional<Value> Assembler::section_value(std::string_view name) {
    const std::string k = key(name);
    for (const auto* list : {&sections_, &prev_sections_}) {
        for (std::size_t i = 1; i < list->size(); ++i) {
            const Section& s = (*list)[i];
            if (key(s.name) != k) continue;
            if (list == &prev_sections_) {
                forward_ref_ = true;
                ++forward_count_;
            }
            Value v = Value::number(s.base);
            v.rel = s.absolute ? Rel::None : Rel::Addr;
            v.unsig = true;
            v.fixed = s.absolute;
            v.section = s.code ? 0 : static_cast<std::uint16_t>(i);
            if (!s.absolute) v.movable = static_cast<std::uint16_t>(i);
            return v;
        }
    }
    return std::nullopt;
}

// Section types a group takes: DATA and CONST for a DGROUP, CODE for a CGROUP.
bool group_type_ok(bool code, const std::string& type) {
    return code ? type == "CODE" : type == "DATA" || type == "CONST";
}

// The group with this name: defined in this pass, or, with 'forward', in
// the previous pass (named before its group line). Groups are numbered in
// order of definition, which is the same in every pass.
std::optional<std::size_t> Assembler::find_group(const std::string& name_key, bool forward) const {
    for (std::size_t i = 0; i < groups_.size(); ++i)
        if (key(groups_[i].name) == name_key) return i;
    if (forward)
        for (std::size_t i = groups_.size(); i < prev_groups_.size(); ++i)
            if (key(prev_groups_[i].name) == name_key) return i;
    return std::nullopt;
}

// The group a section belongs to, even if its group line comes later.
std::optional<std::size_t> Assembler::group_of(const std::string& section_key) const {
    for (const auto* list : {&groups_, &prev_groups_})
        for (std::size_t i = 0; i < list->size(); ++i) {
            const auto& m = (*list)[i].members;
            if (std::find(m.begin(), m.end(), section_key) != m.end()) return i;
        }
    return std::nullopt;
}

// ASSUME ties a DPP to a whole group: assuming one section of a group
// assumes all of them. Maps a section index to its group's number, if it
// is in one.
int Assembler::assume_key(int id) const {
    if (id <= 0 || id >= Value::kGroupBase) return id;
    const auto i = static_cast<std::size_t>(id);
    const std::vector<Section>& list = i < sections_.size() ? sections_ : prev_sections_;
    if (i >= list.size()) return id;
    if (auto g = group_of(key(list[i].name))) return Value::kGroupBase + static_cast<int>(*g);
    return id;
}

// A group name stands for the start of the 16 KB page (DGROUP) or 64 KB
// segment (CGROUP) that the linker puts the group in. Like the name of a
// data section, a DGROUP name takes the DPP that ASSUME ties to the group.
std::optional<Value> Assembler::group_value(std::string_view name) {
    const std::string k = key(name);
    std::optional<std::size_t> g = find_group(k, false);
    if (!g) {
        g = find_group(k, true);
        if (!g) return std::nullopt;
        forward_ref_ = true;
        ++forward_count_;
    }
    const bool code = *g < groups_.size() ? groups_[*g].code : prev_groups_[*g].code;
    auto it = group_layout_.find(k);
    Value v = Value::number(it != group_layout_.end() ? it->second : 0);
    v.rel = Rel::Addr;
    v.unsig = true;
    const auto id = static_cast<std::uint16_t>(Value::kGroupBase + *g);
    v.section = code ? 0 : id;
    v.movable = id;
    return v;
}

// name DGROUP section, ... or name CGROUP section, ...: see Group. As in
// Keil, sections may be defined after the group line, a group cannot be
// extended by another line with its name, and a section belongs to one
// group only. A section of the wrong type is reported but still joins.
void Assembler::group(const Token& name, bool code, TokenCursor& cur, Reporter& rep) {
    const char* kw = code ? "CGROUP" : "DGROUP";
    check_section_name(name);
    const std::string k = key(name.text);
    if (auto it = symbols_.find(k); predefined(name.text, cpu_, sys166_) ||
                                    (it != symbols_.end() && it->second.pass == pass_) || find_group(k, false))
        throw AsmError("A25", "symbol redefinition: '" + std::string(name.text) + "' is already defined", name.column);
    for (std::size_t i = 1; i < sections_.size(); ++i)
        if (key(sections_[i].name) == k)
            throw AsmError("A25", "symbol redefinition: '" + sections_[i].name + "' is a section", name.column);
    if (cur.at_end())
        throw AsmError("A6", std::string("syntax error: ") + kw + " needs section names", cur.peek().column);
    Group g;
    g.name = std::string(name.text);
    g.code = code;
    if (final_) {
        g.listing = res_.listing.size();
        g.column = name.column;
    }
    groups_.push_back(std::move(g));
    const std::size_t index = groups_.size() - 1;
    do {
        const Token& t = cur.peek();
        if (t.kind == TokKind::Number) throw AsmError("A23", "identifier expected", t.column);
        if (t.kind != TokKind::Ident) throw AsmError("A6", "syntax error: section name expected", t.column);
        check_section_name(t);
        cur.next();
        const std::string mk = key(t.text);
        const Section* s = nullptr;
        for (const auto* list : {&sections_, &prev_sections_})
            for (std::size_t i = 1; i < list->size() && !s; ++i)
                if (key((*list)[i].name) == mk) s = &(*list)[i];
        std::optional<std::size_t> other;
        for (std::size_t i = 0; i < index && !other; ++i)
            if (std::find(groups_[i].members.begin(), groups_[i].members.end(), mk) != groups_[i].members.end())
                other = i;
        if (!s && (predefined(t.text, cpu_, sys166_) || symbols_.count(mk) || find_group(mk, true))) {
            rep.error("A25", "symbol redefinition: '" + std::string(t.text) + "' is not a section", t.column);
        } else if (other) {
            rep.error("A53", "section is already a member of group '" + groups_[*other].name + "'", t.column);
        } else {
            if (!s)
                rep.error("A93", "name of a section expected: '" + std::string(t.text) + "' is not defined", t.column);
            else if (!group_type_ok(code, s->type))
                rep.error("A34", std::string("illegal section type: a ") + kw + " takes " +
                                     (code ? "CODE sections" : "DATA and CONST sections") + ", not " + s->type,
                          t.column);
            auto& members = groups_[index].members;
            if (std::find(members.begin(), members.end(), mk) == members.end()) members.push_back(mk);
        }
        if (!cur.at_end() && !cur.peek().is_op(","))
            throw AsmError("A6", "syntax error: unexpected '" + std::string(cur.peek().text) + "'", cur.peek().column);
    } while (cur.accept_op(","));
}

// In segmented mode, data directives store the name of a data section with a
// fixed address (alone, in parentheses, or plus or minus a number) as the DPP
// address that ASSUME gives the section. Labels keep their plain address.
void Assembler::assumed_section_page(Value& v) const {
    if (!segmented_ || !v.known || v.rel != Rel::None || v.section == 0 || v.dpp >= 0) return;
    const int k = assume_key(v.section);
    for (int d = 0; d < 4; ++d) {
        if (assume_[d] != k) continue;
        v.num = (std::int64_t{d} << 14) | (v.num & 0x3FFF);
        return;
    }
}

// ASSUME DPPn:target, ... tells the assembler which data page a DPP register
// will select at run time, so that it can turn an address into the DPP
// number and the offset in that page. The target is a section, a label in
// one, SYSTEM (the page of the SFRs) or NOTHING. Each section, and SYSTEM,
// can have only one DPP. "ASSUME NOTHING" forgets all of them.
void Assembler::assume(TokenCursor& cur, Reporter& rep) {
    if (!segmented_) {
        rep.warning("A54", "'ASSUME' in nonsegmented mode ignored", cur.peek().column);
        while (!cur.at_end()) cur.next();
        return;
    }
    if (cur.accept_keyword("NOTHING")) {
        expect_end(cur);
        assume_.fill(-1);
        return;
    }
    do {
        const Token& reg = cur.next();
        const int n = reg.kind == TokKind::Ident ? dpp_number(reg.text) : -1;
        if (n < 0) throw AsmError("A35", "'DPPn' expected", reg.column);
        if (!cur.accept_op(":")) throw AsmError("A36", "':' expected", cur.peek().column);
        const Token& t = cur.next();
        if (t.kind != TokKind::Ident)
            throw AsmError("A6", "syntax error: section, label, SYSTEM or NOTHING expected", t.column);
        std::optional<int> target;
        if (ieq(t.text, "NOTHING")) {
            target = -1;
        } else if (ieq(t.text, "SYSTEM")) {
            target = kAssumeSystem;
        } else {
            const std::string k = key(t.text);
            for (std::size_t i = 1; i < sections_.size() && !target; ++i)
                if (key(sections_[i].name) == k) target = static_cast<int>(i);
            if (auto it = symbols_.find(k); !target && it != symbols_.end()) {
                const Value& v = it->second.sym.value;
                if (it->second.sym.kind == SymbolKind::Label && v.section) target = v.section;
            }
            if (auto g = find_group(k, true); !target && g) target = Value::kGroupBase + static_cast<int>(*g);
            for (std::size_t i = 1; i < prev_sections_.size() && !target; ++i)
                if (key(prev_sections_[i].name) == k) target = static_cast<int>(i);
            // Keil quietly ignores a name it does not know.
            if (!target && pass_ == 1) changed_ = true;
            if (target) target = assume_key(*target);
        }
        if (target) {
            bool taken = false;
            for (int d = 0; d < 4; ++d) taken |= d != n && *target != -1 && assume_[d] == *target;
            if (taken)
                rep.error("A94", "multiple DPP assume for one section or group", t.column);
            else
                assume_[n] = *target;
        }
        if (!cur.at_end() && !cur.peek().is_op(","))
            throw AsmError("A6", "syntax error: unexpected '" + std::string(cur.peek().text) + "'", cur.peek().column);
    } while (cur.accept_op(","));
}

// name SECTION type [align] [AT address | combine] ['class']
// The alignment, combine type and class only matter to the linker, except
// that the alignment guides where openc166 puts the section. Keil opens the
// section even when it reports a problem with these, except a different
// type for a section seen before.
// Keil does not take a mnemonic, directive, operator, section type or
// condition code as a section name: SECTION and ENDS lines with one are
// syntax errors.
void Assembler::check_section_name(const Token& name) const {
    const std::string u = upper(name.text);
    const std::optional<Value> pre = predefined(name.text, cpu_, sys166_);
    if (isa::is_mnemonic(u) || is_operator_keyword(u) || contains(kKeywords, u) || contains(kSectionTypes, u) ||
        (pre && pre->type == VType::Cc))
        throw AsmError("A6", "syntax error: '" + std::string(name.text) + "' is a reserved word", name.column);
}

void Assembler::section_start(const Token& name, TokenCursor& cur, Reporter& rep) {
    // As in Keil, a name already taken by a symbol, register, flag or other
    // predefined name or group is a redefinition, and the section is not opened.
    check_section_name(name);
    if (auto it = symbols_.find(key(name.text)); predefined(name.text, cpu_, sys166_) ||
                                                 (it != symbols_.end() && it->second.pass == pass_) ||
                                                 find_group(key(name.text), false))
        throw AsmError("A25", "symbol redefinition: '" + std::string(name.text) + "' is already defined", name.column);
    const Token& type_tok = cur.peek();
    if (type_tok.kind != TokKind::Ident || !contains(kSectionTypes, upper(type_tok.text)))
        throw AsmError("A34", "illegal section type", type_tok.column);
    if (ieq(type_tok.text, "BIT")) throw AsmError("OC3", "BIT sections are not supported yet", type_tok.column);
    const std::string type = upper(cur.next().text);

    const std::string k = key(name.text);
    std::size_t old = 0;
    for (std::size_t i = 1; i < sections_.size() && !old; ++i)
        if (key(sections_[i].name) == k) old = i;
    if (old && sections_[old].type != type) {
        AsmError e("A30", "redefinition: section type: '" + sections_[old].name + "' is a " +
                              sections_[old].type + " section", cur.peek().column);
        if (cur.at_end()) throw e;
        rep.error(e.code(), e.message(), e.column());
        throw AsmError("A6", "syntax error", cur.peek().column);
    }

    const AlignType* align = nullptr;
    int align_column = 0;
    if (const Token& t = cur.peek(); t.kind == TokKind::Ident) {
        for (const auto& a : kAlignTypes)
            if (ieq(t.text, a.name)) align = &a;
    }
    if (align) {
        align_column = cur.next().column;
        if (!align_allowed(align->name, type)) {
            rep.error("A43", "illegal align type: " + std::string(align->name) + " for a " + type + " section",
                      align_column);
            align = nullptr;
        }
    }
    std::optional<std::string> combine;
    std::optional<std::int64_t> at;
    int combine_column = cur.peek().column;
    if (cur.accept_keyword("AT")) {
        int column = cur.peek().column;
        try {
            at = absolute(cur, "AT");
        } catch (const AsmError& e) {
            // Keil opens the section anyway.
            if (e.code() != "A72") throw;
            rep.error(e.code(), e.message(), e.column());
            at = 0;
        }
        // Without $SEGMENTED, everything lives in the first 64 KB.
        if (!segmented_ && *at > 0xFFFF) {
            rep.error("A19", "numeric value out of range: AT above 0FFFFH needs $SEGMENTED", column);
            at = 0;
        }
        combine = "AT";
    } else if (const Token& t = cur.peek(); t.kind == TokKind::Ident && contains(kCombineTypes, upper(t.text))) {
        combine = upper(cur.next().text);
    }
    std::optional<std::string> class_name;
    int class_column = cur.peek().column;
    if (cur.peek().kind == TokKind::String) {
        auto bytes = string_bytes(cur.next());
        class_name = std::string(bytes.begin(), bytes.end());
    }
    if (!cur.at_end())
        rep.error("A6", "syntax error: unexpected '" + std::string(cur.peek().text) + "'", cur.peek().column);

    if (old) {
        const Section& s = sections_[old];
        if (align && align->name != s.align_type)
            rep.error("A31", "redefinition: align type: '" + s.name + "' is " + s.align_type, align_column);
        if (combine && (*combine != s.combine || (at && *at != s.base)))
            rep.error("A32", "redefinition: combine type: '" + s.name + "' is " +
                                 (s.absolute ? "AT " + hex(s.base, 4) + "H" : s.combine), combine_column);
        if (class_name && *class_name != s.class_name)
            rep.error("A33", "redefinition: class name of '" + s.name + "'", class_column);
        open_.push_back(old);
        cur_ = old;
        return;
    }
    if (at && (*at < 0 || *at >= kAddressLimit)) throw AsmError("A66", "value out of range");
    Section s;
    s.name = std::string(name.text);
    s.type = type;
    s.code = type == "CODE";
    if (align) {
        s.align_type = std::string(align->name);
        s.align = align->align;
    }
    if (combine) s.combine = *combine;
    if (class_name) s.class_name = *class_name;
    s.absolute = at.has_value();
    const std::optional<std::int64_t> given = option_address(k);
    s.placed = !at && given;
    if (at)
        s.base = *at;
    else if (auto it = layout_.find(k); it != layout_.end())
        s.base = it->second;
    else if (given)
        s.base = *given;
    else  // the first pass: a guess that the following passes correct
        s.base = align_up(std::max(free_, s.align_type == "BITADDRESSABLE" ? kBitAreaStart : std::int64_t{0}), s.align);
    s.loc = s.base;
    if (final_) {
        s.listing = res_.listing.size();
        s.column = name.column;
    }
    sections_.push_back(s);
    cur_ = sections_.size() - 1;
    open_.push_back(cur_);
    free_ = std::max(free_, s.loc);
}

std::optional<std::int64_t> Assembler::option_address(const std::string& name_key) const {
    const auto& given = opt_.section_addresses;
    for (auto it = given.rbegin(); it != given.rend(); ++it)
        if (key(it->first) == name_key) return it->second;
    return std::nullopt;
}

// Sections nest: ENDS closes the innermost open one, which may be the same
// section opened again inside itself. Like Keil, an ENDS with the name of
// another section still closes it, but one with an unknown name does not.
void Assembler::section_end(const Token& name, Reporter& rep) {
    check_section_name(name);
    if (open_.empty()) {
        rep.error("A6", "syntax error: no section is open", name.column);
        throw AsmError("A27", "misplaced directive: ENDS outside a section", name.column);
    }
    const std::string k = key(name.text);
    if (key(sections_[open_.back()].name) != k) {
        bool known = false;
        for (std::size_t i = 1; i < sections_.size(); ++i) known = known || key(sections_[i].name) == k;
        if (!known) throw AsmError("A28", "invalid or unmatched section name", name.column);
        const std::string closed = sections_[open_.back()].name;
        open_.pop_back();
        cur_ = open_.empty() ? 0 : open_.back();
        throw AsmError("A29", "unmatched ENDS/ENDP: closed section '" + closed + "' instead", name.column);
    }
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
        ctx.segmented = segmented_;
        ctx.assume = assume_;
        ctx.assume_key = [this](int id) { return assume_key(id); };
        ctx.ext_page = ext_left_ > 0 && ext_page_;
        Encoding e = encode(m, ops, ctx, rep);
        if (align_offset() & 1) rep.error("A134", "improper address alignment: instruction at an odd address");

        if (ext_left_ > 0) --ext_left_;
        if (ExtSequence seq = ext_sequence(e); seq.count) {
            ext_left_ = seq.count;
            ext_esfr_ = seq.esfr;
            ext_page_ = seq.page;
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
            throw AsmError("A6", "syntax error: SHORT or a type belongs in front of the target", cc.column);
        }
        if (cc.mode != OpMode::Direct || cc.value.type != VType::Cc)
            throw AsmError("A74", "operand type mismatch: condition code expected", cc.column);
    }
    if (Value& t = ops.back().value; t.dpp >= 0) {
        t.num = t.dpp_address();
        t.dpp = -1;
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
        off.value.movable = v.movable;  // for check_branch_target()
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

// Keil warns when a near branch goes to a FAR label, when any branch goes
// to a data label, and when an absolute target that does not depend on the
// linker is odd.
void Assembler::check_branch_target(const std::string& m, const std::vector<Operand>& ops, Reporter& rep) const {
    const bool near_branch = m == "JMPA" || m == "JMPR" || m == "CALLA" || m == "CALLR";
    if (!near_branch && m != "JMPS" && m != "CALLS") return;
    if (ops.empty() || ops.back().mode != OpMode::Direct) return;
    const Operand& t = ops.back();
    if (near_branch && t.value.ltype == LType::Far && !t.value.short_jump)
        rep.warning("A84", "near reference to far location", t.column);
    if (near_branch && is_data_type(t.value.ltype))
        rep.warning("A74", "operand type mismatch: branch to a data label", t.column);
    if (m != "JMPR" && m != "CALLR" && t.value.known && !t.value.movable && (t.value.num & 1))
        rep.warning("A134", "address alignment not sufficient: branch target is odd", t.column);
}

// Assembles the modules of a program together, as A166 would one after the
// other and L166 would then link them. Each pass assembles every module
// with the PUBLIC and GLOBAL symbols and the section addresses that the
// previous pass found, then places the sections, until nothing moves.
class Program {
public:
    explicit Program(const AssembleOptions& opt) : opt_(opt) {}
    AssemblyResult run(const std::vector<Source>& sources);

private:
    // A section of one module.
    struct Part {
        Assembler* m = nullptr;
        std::size_t index = 0;  // into m->sections_
        std::int64_t offset = 0;  // from the start of the unit
        const Section& section() const { return m->sections_[index]; }
    };
    // A section as L166 places it: the parts with the same name and class
    // that the modules combine.
    struct Unit {
        std::vector<Part> parts;
        std::int64_t base = 0;
        std::int64_t size = 0;
        int align = 1;
        bool bit = false;  // BITADDRESSABLE
    };

    const AssembleOptions& opt_;
    LinkState link_;
    std::vector<std::unique_ptr<Assembler>> mods_;
    std::vector<SectionInfo> infos_;  // of the final pass
    std::vector<std::pair<std::int64_t, std::int64_t>> common_;  // [from, to) of combined COMMON sections

    bool run_pass(int pass, bool final);
    std::vector<Unit> units();
    void locate(bool final);
    bool update_publics();
};

AssemblyResult Program::run(const std::vector<Source>& sources) {
    if (sources.empty()) return {};
    for (const auto& src : sources) link_.modules.push_back(src.file_name);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        mods_.push_back(std::make_unique<Assembler>(opt_, sources[i].file_name, i, link_));
        mods_.back()->load(sources[i].text);
    }
    bool converged = false;
    int pass = 1;
    for (; pass <= kMaxPasses; ++pass) {
        if (!run_pass(pass, false)) {
            converged = true;
            break;
        }
    }
    if (!converged) pass = kMaxPasses;
    ++pass;
    run_pass(pass, true);

    AssemblyResult out;
    for (std::size_t i = 0; i < mods_.size(); ++i) {
        Assembler& m = *mods_[i];
        AssemblyResult& r = m.res_;
        const std::size_t first_diag = out.diagnostics.size();
        for (auto& d : r.diagnostics) out.diagnostics.push_back(std::move(d));
        for (auto& l : r.listing) {
            for (auto& d : l.diagnostics) d += first_diag;
            l.module = static_cast<int>(i);
            out.listing.push_back(std::move(l));
        }
        for (auto& c : r.chunks) out.chunks.push_back(std::move(c));
        if (m.cpu_ == isa::Cpu::C167) out.cpu = isa::Cpu::C167;
        std::set<std::string> exported;
        for (const auto& [k, def] : m.exported()) exported.insert(k);
        for (const auto& [k, e] : m.symbols_) {
            out.symbols.push_back(e.sym);
            out.symbols.back().module = static_cast<int>(i);
            out.symbols.back().exported = exported.count(k) > 0;
        }
    }
    out.passes = pass;
    const std::string& main_name = link_.modules.front();
    if (!converged)
        out.diagnostics.push_back({Severity::Error, "A133",
                                   "addresses do not settle; code size keeps changing between passes", main_name, 0, 0});
    // Like L166, the first module sets the CPU mode of the program.
    auto mode = [](const Assembler& m) { return m.segmented_ ? "SEGMENTED" : "NONSEGMENTED"; };
    for (std::size_t i = 1; i < mods_.size(); ++i)
        if (mods_[i]->segmented_ != mods_.front()->segmented_)
            out.diagnostics.push_back({Severity::Warning, "OC20",
                                       "this module is " + std::string(mode(*mods_[i])) + ", but " + main_name +
                                           " is " + mode(*mods_.front()) + " (L166 warns L15: incompatible CPU mode)",
                                       link_.modules[i], 0, 0});

    // Parts of a COMMON section overlap on purpose: the last module's bytes
    // win, as in L166.
    auto in_common = [&](std::int64_t a) {
        return std::any_of(common_.begin(), common_.end(), [&](const auto& c) { return c.first <= a && a < c.second; });
    };
    std::vector<const Chunk*> sorted;
    for (const auto& c : out.chunks) sorted.push_back(&c);
    std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->address < b->address; });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        std::int64_t prev_end = sorted[i - 1]->address + static_cast<std::int64_t>(sorted[i - 1]->bytes.size());
        if (sorted[i]->address < prev_end && !in_common(sorted[i]->address))
            out.diagnostics.push_back({Severity::Warning, "OC1",
                                       "code or data overlaps at address " + hex(sorted[i]->address, 6) + "H",
                                       main_name, 0, 0});
    }

    std::sort(out.symbols.begin(), out.symbols.end(), [](const Symbol& a, const Symbol& b) {
        return a.name != b.name ? a.name < b.name : a.module < b.module;
    });
    out.sections = std::move(infos_);
    out.modules = link_.modules;
    return out;
}

bool Program::run_pass(int pass, bool final) {
    for (auto& m : mods_) m->begin_pass(pass, final);
    for (auto& m : mods_) m->run_lines();
    locate(final);
    bool changed = false;
    for (auto& m : mods_) changed = m->finish_pass() || changed;
    return update_publics() || changed;
}

// Takes the PUBLIC and GLOBAL symbols of this pass for the next one. If a
// name is exported twice, the first module wins (check_public() reports
// the others). Returns true if another module may now see other values.
bool Program::update_publics() {
    std::map<std::string, PublicDef> publics;
    for (const auto& m : mods_)
        for (auto& [k, def] : m->exported()) publics.try_emplace(k, std::move(def));
    auto same = [](const auto& a, const auto& b) {
        const Value &x = a.second.value, &y = b.second.value;
        return a.first == b.first && a.second.module == b.second.module && a.second.kind == b.second.kind &&
               x.type == y.type && x.known == y.known && x.ext == y.ext && x.bit == y.bit && x.num == y.num &&
               x.ltype == y.ltype;
    };
    const bool changed = publics.size() != link_.publics.size() ||
                         !std::equal(publics.begin(), publics.end(), link_.publics.begin(), same);
    link_.publics = std::move(publics);
    // A single module has no one to tell.
    return changed && mods_.size() > 1;
}

// The sections of all modules in order of first appearance. Like L166,
// parts with the same name and class are combined into one section if they
// all have the same type and combine type PUBLIC, GLOBAL, COMMON, SYSSTACK
// or USRSTACK: COMMON parts overlay each other, the others follow one
// another, each aligned as its module asks.
std::vector<Program::Unit> Program::units() {
    std::vector<Unit> found;
    std::map<std::pair<std::string, std::string>, std::size_t> index;
    for (const auto& mp : mods_) {
        Assembler& m = *mp;
        for (std::size_t i = 1; i < m.sections_.size(); ++i) {
            const Section& s = m.sections_[i];
            auto [it, inserted] = index.try_emplace({m.key(s.name), m.key(s.class_name)}, found.size());
            if (inserted) found.emplace_back();
            found[it->second].parts.push_back(Part{&m, i, 0});
        }
    }
    auto combinable = [](const std::string& c) {
        return c == "PUBLIC" || c == "GLOBAL" || c == "COMMON" || c == "SYSSTACK" || c == "USRSTACK";
    };
    // As in L166, each later part is checked against the first one only; a
    // part that does not match stands alone (and is reported), the others
    // still combine.
    std::vector<Unit> out;
    for (auto& u : found) {
        const Section& first = u.parts[0].section();
        Unit kept{{u.parts[0]}};
        std::vector<Unit> rejected;
        for (std::size_t j = 1; j < u.parts.size(); ++j) {
            const Part& p = u.parts[j];
            const Section& s = p.section();
            if (!first.absolute && !s.absolute && s.type == first.type && s.combine == first.combine &&
                combinable(first.combine)) {
                kept.parts.push_back(p);
                continue;
            }
            p.m->report_at_section(
                p.index, Severity::Error, "OC19",
                "section '" + s.name + "' (" + s.type + ", " + s.combine + ") cannot be combined with '" +
                    first.name + "' (" + first.type + ", " + first.combine + ") of " +
                    link_.modules[u.parts[0].m->module_] +
                    ": parts of a section must have the same type and combine type PUBLIC, GLOBAL, COMMON, "
                    "SYSSTACK or USRSTACK");
            rejected.push_back(Unit{{p}});
        }
        out.push_back(std::move(kept));
        for (auto& r : rejected) out.push_back(std::move(r));
    }
    for (auto& u : out) {
        const bool common = u.parts[0].section().combine == "COMMON";
        for (Part& p : u.parts) {
            const Section& s = p.section();
            p.offset = common ? 0 : align_up(u.size, s.align);
            u.size = common ? std::max(u.size, s.size) : p.offset + s.size;
            u.align = std::max(u.align, s.align);
            u.bit = u.bit || s.align_type == "BITADDRESSABLE";
        }
    }
    return out;
}

// Puts the sections without AT where L166 would by default: in the order in
// which they first appear, each at the lowest suitably aligned address from
// the origin on where it fits between the sections and code placed so far,
// without crossing the page or segment boundary its type forbids. Sections
// with an address in AssembleOptions::section_addresses go there first. The
// next pass assembles with these addresses.
void Program::locate(bool final) {
    std::vector<Unit> all = units();
    std::vector<std::pair<std::int64_t, std::int64_t>> used;
    for (const auto& m : mods_) used.insert(used.end(), m->outside_.begin(), m->outside_.end());
    auto top_of = [](const Assembler& m) { return m.segmented_ ? kAddressLimit : std::int64_t{0x10000}; };

    for (Unit& u : all) {
        Assembler& m = *u.parts[0].m;
        const Section& s = u.parts[0].section();
        const std::int64_t top = top_of(m);
        const std::optional<std::int64_t> given = m.option_address(m.key(s.name));
        if (s.absolute) {
            if (given)
                m.report(Severity::Error, "OC14", "section '" + s.name + "' has an AT address; it cannot also be placed at " +
                                                     hex(*given, 6) + "H");
            u.base = s.base;
        } else if (given) {
            u.base = *given;
            if (*given < 0 || *given + u.size > top)
                m.report(Severity::Error, "OC14", "section '" + s.name + "' cannot be placed at " + hex(*given, 6) +
                                                     "H: it would end beyond " + hex(top - 1, 6) + "H" +
                                                     (m.segmented_ ? "" : " (higher addresses need $SEGMENTED)"));
            else if (*given % u.align || (u.bit && (*given < kBitAreaStart || *given + u.size > kBitAreaEnd)))
                m.report(Severity::Warning, "OC13", "section '" + s.name + "' is placed at " + hex(*given, 6) +
                                                       "H, which does not suit its alignment " + s.align_type);
        } else {
            continue;
        }
        if (u.size > 0) used.emplace_back(u.base, u.base + u.size);
    }
    std::set<std::string> unknown;
    for (const auto& [name, address] : opt_.section_addresses) {
        bool found = false;
        for (const auto& m : mods_)
            for (std::size_t i = 1; i < m->sections_.size() && !found; ++i)
                found = m->key(m->sections_[i].name) == m->key(name);
        if (!found && unknown.insert(mods_[0]->key(name)).second)
            mods_[0]->report(Severity::Warning, "OC13",
                             "an address is given for section '" + name + "', which does not exist");
    }

    // The lowest address in [from, to) from which u fits between the ranges
    // in 'taken', suitably aligned and without crossing a multiple of
    // 'boundary' (0: none).
    auto first_fit = [](const Unit& u, std::int64_t from, std::int64_t to, std::int64_t boundary,
                        const std::vector<std::pair<std::int64_t, std::int64_t>>& taken) -> std::optional<std::int64_t> {
        std::int64_t a = align_up(from, u.align);
        while (a + u.size <= to) {
            if (boundary && u.size > 0 && u.size <= boundary && a / boundary != (a + u.size - 1) / boundary) {
                a = align_up((a / boundary + 1) * boundary, u.align);
                continue;
            }
            auto clash = std::find_if(taken.begin(), taken.end(), [&](const auto& r) {
                return r.first < a + std::max<std::int64_t>(u.size, 1) && a < r.second;
            });
            if (clash == taken.end()) return a;
            a = align_up(clash->second, u.align);
        }
        return std::nullopt;
    };
    std::vector<bool> done(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Assembler& m = *all[i].parts[0].m;
        const Section& s = all[i].parts[0].section();
        done[i] = s.absolute || m.option_address(m.key(s.name));
    }

    // Groups of the same name in different modules are one group. Like
    // L166, place all DGROUPs and then all CGROUPs, each in order of first
    // definition, before the other sections. A group goes to the first 16 KB
    // page (DGROUP) or 64 KB segment (CGROUP) from the origin on that has
    // room for all its members, or to the page or segment of its first
    // member with a fixed address. Its members go there one by one, largest
    // first, each at the lowest address where it fits.
    struct LinkGroup {
        std::vector<std::pair<Assembler*, std::size_t>> defs;  // module and index into its groups_
        bool code = false;
        std::vector<std::size_t> members;  // into all, in the order listed
        std::int64_t base = 0;
        bool placed = false;
    };
    std::map<std::pair<const Assembler*, std::size_t>, std::size_t> unit_of;
    for (std::size_t i = 0; i < all.size(); ++i)
        for (const Part& p : all[i].parts) unit_of[{p.m, p.index}] = i;
    std::vector<LinkGroup> groups;
    std::map<std::string, std::size_t> group_index;
    std::vector<bool> grouped(all.size());
    for (const auto& mp : mods_) {
        Assembler& m = *mp;
        for (std::size_t gi = 0; gi < m.groups_.size(); ++gi) {
            const Group& g = m.groups_[gi];
            auto [it, inserted] = group_index.try_emplace(m.key(g.name), groups.size());
            if (inserted) {
                groups.emplace_back();
                groups.back().code = g.code;
            }
            LinkGroup& lg = groups[it->second];
            lg.defs.emplace_back(&m, gi);
            for (const std::string& member : g.members)
                for (std::size_t i = 1; i < m.sections_.size(); ++i) {
                    if (m.key(m.sections_[i].name) != member || !group_type_ok(lg.code, m.sections_[i].type)) continue;
                    const std::size_t u = unit_of.at({&m, i});
                    if (grouped[u]) continue;
                    grouped[u] = true;
                    lg.members.push_back(u);
                }
        }
    }
    for (const bool code : {false, true}) {
        for (LinkGroup& g : groups) {
            if (g.code != code || g.members.empty()) continue;
            Assembler& m = *g.defs[0].first;
            const Group& def = m.groups_[g.defs[0].second];
            const std::int64_t region = code ? 0x10000 : 0x4000;
            const std::string what = std::string(code ? "CGROUP" : "DGROUP") + " '" + def.name + "'";
            std::int64_t total = 0;
            for (std::size_t u : g.members) total += all[u].size;
            if (total > region) {
                m.report_at(def.listing, def.column, Severity::Error, "OC22",
                            what + " (" + hex(total, 4) + "H bytes) does not fit in one " +
                                (code ? "64 KB segment (L166 reports L116)" : "16 KB data page (L166 reports L115)"));
                continue;
            }
            std::vector<std::size_t> order = g.members;
            std::stable_sort(order.begin(), order.end(),
                             [&](std::size_t a, std::size_t b) { return all[a].size > all[b].size; });
            // Like L166, the first member listed with a fixed address decides
            // the page or segment; other fixed members may lie elsewhere.
            std::optional<std::int64_t> pinned;
            for (std::size_t u : g.members)
                if (done[u]) {
                    pinned = all[u].base / region * region;
                    break;
                }
            const std::int64_t top = top_of(m);
            for (std::int64_t page = pinned ? *pinned : opt_.origin / region * region;
                 page < top && (!pinned || page == *pinned) && !g.placed; page += region) {
                auto trial = used;
                std::vector<std::pair<std::size_t, std::int64_t>> bases;
                std::vector<std::size_t> left_out;
                bool fits = true;
                for (std::size_t u : order) {
                    if (done[u]) continue;
                    const Unit& un = all[u];
                    const std::int64_t from = std::max(page, un.bit ? kBitAreaStart : opt_.origin);
                    const std::int64_t to = std::min(page + region, un.bit ? kBitAreaEnd : top);
                    std::optional<std::int64_t> a = first_fit(un, from, to, 0, trial);
                    if (!a && pinned) {
                        // Like L166 (L112), a member without room in the page
                        // or segment the group is tied to is left out.
                        const Part& p = un.parts[0];
                        p.m->report_at_section(p.index, Severity::Error, "OC22",
                                               "section '" + p.section().name + "' does not fit in the " +
                                                   (code ? "segment" : "data page") + " of " + what +
                                                   " (L166 reports L112)");
                        left_out.push_back(u);
                        continue;
                    }
                    if (!a) {
                        fits = false;
                        break;
                    }
                    bases.emplace_back(u, *a);
                    if (un.size > 0) trial.emplace_back(*a, *a + un.size);
                }
                if (!fits) continue;
                used = std::move(trial);
                for (const auto& [u, a] : bases) {
                    all[u].base = a;
                    done[u] = true;
                }
                for (std::size_t u : left_out) done[u] = true;
                g.base = page;
                g.placed = true;
            }
            if (!g.placed)
                m.report_at(def.listing, def.column, Severity::Error, "OC22",
                            "no room for " + what + " (" + hex(total, 4) + "H bytes) in one " +
                                (code ? "64 KB segment" : "16 KB data page"));
        }
    }

    for (std::size_t i = 0; i < all.size(); ++i) {
        if (done[i]) continue;
        Unit& u = all[i];
        Assembler& m = *u.parts[0].m;
        const Section& s = u.parts[0].section();
        const std::int64_t from = u.bit ? kBitAreaStart : opt_.origin, to = u.bit ? kBitAreaEnd : top_of(m);
        const std::int64_t boundary = section_boundary(s.type);
        if (boundary == 0x4000 && u.size > boundary && final)
            m.report_at_section(u.parts[0].index, Severity::Error, "OC21",
                                "section '" + s.name + "' (" + hex(u.size, 4) + "H bytes) does not fit in one 16 KB data page "
                                "(L166 reports L129)");
        std::optional<std::int64_t> a = first_fit(u, from, to, boundary, used);
        if (!a) {
            m.report(Severity::Error, "OC12", "no room for section '" + s.name + "' (" + hex(u.size, 4) + "H bytes" +
                                                 (u.bit ? " in FD00H..FDFFH)" : ")"));
            a = s.base;
        } else if (u.size > 0) {
            used.emplace_back(*a, *a + u.size);
        }
        u.base = *a;
    }

    for (const Unit& u : all)
        for (const Part& p : u.parts)
            if (!p.section().absolute) p.m->layout_[p.m->key(p.section().name)] = u.base + p.offset;
    for (LinkGroup& g : groups) {
        const std::int64_t region = g.code ? 0x10000 : 0x4000;
        if (!g.placed && !g.members.empty()) g.base = all[g.members[0]].base / region * region;
        for (const auto& [m, gi] : g.defs) {
            auto [it, inserted] = m->group_layout_.try_emplace(m->key(m->groups_[gi].name), g.base);
            if (inserted ? g.base != 0 : it->second != g.base) m->changed_ = true;
            it->second = g.base;
        }
    }

    if (!final) return;
    std::vector<std::string> group_names(all.size());
    for (const LinkGroup& g : groups)
        for (std::size_t u : g.members) group_names[u] = g.defs[0].first->groups_[g.defs[0].second].name;
    infos_.clear();
    common_.clear();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Unit& u = all[i];
        const Section& s = u.parts[0].section();
        infos_.push_back({s.name, s.type, s.align_type, s.base, u.size,
                          s.absolute ? SectionInfo::Placement::At
                          : s.placed ? SectionInfo::Placement::Given
                                     : SectionInfo::Placement::Located,
                          s.class_name, group_names[i]});
        if (u.parts.size() > 1 && s.combine == "COMMON") common_.emplace_back(s.base, s.base + u.size);
    }
}

}  // namespace

std::size_t AssemblyResult::error_count() const {
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                                  [](const Diagnostic& d) { return d.severity == Severity::Error; }));
}

std::size_t AssemblyResult::warning_count() const { return diagnostics.size() - error_count(); }

AssemblyResult assemble(std::string_view source, const AssembleOptions& options) {
    return Program(options).run({Source{options.file_name, std::string(source)}});
}

AssemblyResult assemble(const std::vector<Source>& sources, const AssembleOptions& options) {
    return Program(options).run(sources);
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
    std::vector<bool> shown_diag(result.diagnostics.size());
    const bool multi = result.modules.size() > 1;
    std::string main_file = result.listing.empty() ? std::string() : result.listing.front().file;
    static constexpr const char* kHeader = "LOC       OBJ                  LINE  SOURCE\n\n";
    std::string out = multi ? "" : kHeader;
    auto print = [&](std::size_t i) {
        const Diagnostic& d = result.diagnostics[i];
        shown_diag[i] = true;
        out += "*** ";
        out += d.severity == Severity::Error ? "ERROR " : "WARNING ";
        out += d.code + ": " + d.message;
        if (d.line && d.file != main_file)
            out += " (" + d.file + ", line " + std::to_string(d.line) + ")";
        else if (!d.line && multi && !d.file.empty())
            out += " (" + d.file + ")";
        out += "\n";
    };
    int module = -1;
    for (const auto& l : result.listing) {
        if (multi && l.module != module) {
            module = l.module;
            if (static_cast<std::size_t>(module) < result.modules.size()) main_file = result.modules[module];
            out += std::string(out.empty() ? "" : "\n") + "MODULE " + main_file + "\n\n" + kHeader;
        }
        if (l.hidden && l.diagnostics.empty()) continue;
        std::string loc = l.has_address ? hex(l.address, 8) : l.value ? "=" + value_text(*l.value) : "";
        std::string obj;
        std::size_t shown = 0;
        for (; shown < l.bytes.size() && shown < 8; ++shown) obj += hex(l.bytes[shown], 2);
        // Lines from include files are marked +1, +2, ... like in Keil's listing.
        std::string level = l.level ? "+" + std::to_string(std::min(l.level, 9)) : "";
        // Lines in false IF blocks have no number, as in Keil's listing.
        std::string number = l.skipped ? "" : std::to_string(l.line);
        char head[64];
        std::snprintf(head, sizeof head, "%-9s %-16s %-2s%6s  ", loc.c_str(), obj.c_str(), level.c_str(),
                      number.c_str());
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
        for (auto i : l.diagnostics) print(i);
    }
    if (multi) {
        main_file.clear();
        bool any = false;
        for (std::size_t i = 0; i < result.diagnostics.size(); ++i) any = any || !shown_diag[i];
        if (any) out += "\n";
    }
    for (std::size_t i = 0; i < result.diagnostics.size(); ++i)
        if (!shown_diag[i]) print(i);

    out += "\nSYMBOLS\n\n";
    for (const auto& s : result.symbols) {
        static constexpr const char* kKinds[] = {"LABEL", "EQU", "SET", "BIT", "LIT", "EXTERN"};
        std::string value;
        switch (s.value.type) {
        case VType::Number: value = s.value.known ? hex(s.value.num, 8) + "H" : "?"; break;
        case VType::Bit: value = value_text(s.value); break;
        case VType::Gpr: value = "R" + std::to_string(s.value.num); break;
        case VType::GprB: value = std::string(s.value.num & 1 ? "RH" : "RL") + std::to_string(s.value.num / 2); break;
        case VType::Sfr: value = "SFR " + hex(s.value.num, 4) + "H"; break;
        case VType::Cc: value = "cc " + std::to_string(s.value.num); break;
        }
        if (s.kind == SymbolKind::Extern)
            value += " " + s.text;
        else if (s.value.ltype != LType::None)
            value += std::string(" ") + type_name(s.value.ltype);
        if (s.kind == SymbolKind::Lit) value = "\"" + s.text + "\"";
        if (s.exported) value += " PUBLIC";
        if (multi && static_cast<std::size_t>(s.module) < result.modules.size())
            value += "  (" + result.modules[s.module] + ")";
        char row[160];
        std::snprintf(row, sizeof row, "%-32s %-6s %s\n", s.name.c_str(), kKinds[static_cast<int>(s.kind)], value.c_str());
        out += row;
    }
    if (!result.sections.empty()) {
        out += "\nSECTIONS\n\n";
        for (const auto& s : result.sections) {
            static constexpr const char* kPlacement[] = {"AT", "given", "located"};
            char row[160];
            std::snprintf(row, sizeof row, "%-32s %-6s %-14s %sH  size %sH  %s", s.name.c_str(), s.type.c_str(),
                          s.align.c_str(), hex(s.address, 6).c_str(), hex(s.size, 4).c_str(),
                          kPlacement[static_cast<int>(s.placement)]);
            out += row;
            if (!s.class_name.empty()) out += "  class '" + s.class_name + "'";
            if (!s.group.empty()) out += "  group " + s.group;
            out += "\n";
        }
    }
    out += "\n" + std::to_string(result.error_count()) + " error(s), " + std::to_string(result.warning_count()) +
           " warning(s)\n";
    return out;
}

}  // namespace oc166
