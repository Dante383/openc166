// SPDX-License-Identifier: MIT
#include "mpl.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

namespace oc166 {
namespace {

constexpr std::size_t kMaxDepth = 256;            // MPL calls being evaluated inside each other
constexpr std::uint64_t kMaxSteps = 20'000'000;   // macro calls and loop rounds in all
constexpr std::size_t kMaxLines = 1'000'000;      // output lines, as the assembler's limit (OC25)
constexpr const char* kOff = "; MPL is off for the rest of the source (as in Keil)";

bool blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool id_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '?'; }
bool id_char(char c) { return id_start(c) || std::isdigit(static_cast<unsigned char>(c)); }
bool digit(char c) { return c >= '0' && c <= '9'; }

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && blank(s.front())) s.remove_prefix(1);
    while (!s.empty() && blank(s.back())) s.remove_suffix(1);
    return s;
}

// 'word' (upper case) at s[i], as a whole word.
bool word_at(std::string_view s, std::size_t i, std::string_view word) {
    if (s.size() - std::min(i, s.size()) < word.size()) return false;
    for (std::size_t k = 0; k < word.size(); ++k)
        if (std::toupper(static_cast<unsigned char>(s[i + k])) != word[k]) return false;
    return i + word.size() == s.size() || !id_char(s[i + word.size()]);
}

std::size_t ident_end(std::string_view s, std::size_t i) {
    while (i < s.size() && id_char(s[i])) ++i;
    return i;
}

// A source file as MPL reads it: CR LF turned into LF.
struct FileText {
    std::size_t index = 0;
    std::string name;
    std::string text;
    std::vector<std::size_t> starts;  // offset of each line
    int line_of(std::size_t pos) const {
        return static_cast<int>(std::upper_bound(starts.begin(), starts.end(), pos) - starts.begin());
    }
};

struct Input {
    std::string_view s;
    std::size_t i = 0;
    const FileText* file = nullptr;  // set when reading a source file directly
    int level = 0;
    bool end() const { return i >= s.size(); }
};

// An element of a macro's define pattern.
struct Elem {
    enum Kind : unsigned char { Param, Literal, Blank, Word } kind;
    std::string text;  // parameter name or identifier delimiter (upper case), or literal delimiter
};

struct Macro {
    std::vector<Elem> pattern;
    std::vector<std::string> locals;  // upper case
    std::string body;
};

using Scope = std::map<std::string, std::string>;  // parameters and LOCAL names of a macro call

const std::vector<Elem> kPair{{Elem::Literal, "("}, {Elem::Param, "A"}, {Elem::Literal, ","},
                              {Elem::Param, "B"},   {Elem::Literal, ")"}};
const std::vector<Elem> kTriple{{Elem::Literal, "("}, {Elem::Param, "A"}, {Elem::Literal, ","}, {Elem::Param, "B"},
                                {Elem::Literal, ","}, {Elem::Param, "C"}, {Elem::Literal, ")"}};

struct Stop {};

class Processor {
public:
    explicit Processor(const MplHost& host) : host_(host) {}
    std::vector<MplLine> run(const std::vector<std::pair<std::size_t, int>>& files);

private:
    struct Pos {
        std::size_t file = 0;
        int line = 0;
        int level = 0;
    };

    const MplHost& host_;
    std::map<std::size_t, std::unique_ptr<FileText>> texts_;
    std::vector<MplLine> lines_;
    MplLine cur_;  // the output line being built
    bool started_ = false;
    std::size_t join_at_ = std::string::npos;  // where an include file without a final newline ended in cur_
    std::string join_name_;
    Pos pos_;  // where the source is being read
    char meta_ = '%';
    bool disabled_ = false;  // after the first error
    bool expr_ = false;  // reading the text of an expression
    bool exit_ = false;  // %EXIT: leave the innermost WHILE, REPEAT or macro
    int loops_ = 0;  // WHILE, REPEAT and macro bodies being expanded
    std::size_t depth_ = 0;
    std::uint64_t steps_ = 0;
    unsigned locals_ = 0;
    std::unordered_map<std::string, std::shared_ptr<const Macro>> macros_;

    // Output: to a string, or (out == nullptr) to the output lines.
    void put(std::string* out, std::string_view s);
    void start();
    void finish();
    void control(std::size_t k);
    void blank_lines(std::size_t n);
    void error(std::string code, std::string message);
    void warning(std::string code, std::string message);
    [[noreturn]] void fatal(std::string code, std::string message);
    void step();

    void scan_file(std::size_t index, int level);
    void scan(Input& in, std::string* out, const Scope* scope);
    std::string evaluate(std::string_view text, const Scope* scope);
    void call(Input& in, std::string* out, const Scope* scope);
    bool builtin(const std::string& name, Input& in, std::string* out, const Scope* scope);
    void user_call(const std::string& name, const Macro& m, bool literal, Input& in, std::string* out,
                   const Scope* scope);

    void skip_blanks(Input& in) const {
        while (!in.end() && blank(in.s[in.i])) ++in.i;
    }
    std::size_t special_end(std::string_view s, std::size_t i) const;
    std::string escapes(std::string_view s) const;
    bool open_paren(Input& in, std::string_view fn);
    bool balanced(Input& in, std::string_view fn, std::string_view& text);
    bool match(Input& in, const std::vector<Elem>& pattern, const std::string& name,
               std::vector<std::string>& args);
    bool collect(Input& in, const Elem* next, std::string& arg);
    std::vector<Elem> parse_pattern(std::string_view p) const;
    void define(Input& in, bool literal, const Scope* scope);
    void set_macro(const std::string& name, std::string body);
    void if_(Input& in, std::string* out, const Scope* scope);
    void loop(Input& in, std::string* out, const Scope* scope, bool repeat);
    void match_(Input& in, const Scope* scope);
    std::optional<std::int32_t> expression(std::string_view raw, const Scope* scope);
    std::optional<std::int32_t> parse_expression(std::string_view text);
};

// ---------------------------------------------------------------- output

void Processor::start() {
    if (started_) return;
    started_ = true;
    cur_.file = pos_.file;
    cur_.line = pos_.line;
    cur_.level = pos_.level;
}

void Processor::put(std::string* out, std::string_view s) {
    if (out) {
        out->append(s);
        return;
    }
    while (!s.empty()) {
        start();
        const std::size_t nl = s.find('\n');
        if (nl == std::string_view::npos) {
            cur_.text.append(s);
            return;
        }
        cur_.text.append(s.substr(0, nl));
        s.remove_prefix(nl + 1);
        if (join_at_ != std::string::npos) {
            // Keil lists a line joined to the end of an include file where
            // the line ends.
            cur_.file = pos_.file;
            cur_.line = pos_.line;
            cur_.level = pos_.level;
        }
        finish();
    }
}

void Processor::finish() {
    start();
    if (join_at_ != std::string::npos) {
        const std::string_view t = cur_.text;
        if (!trim(t.substr(0, join_at_)).empty() && !trim(t.substr(join_at_)).empty()) cur_.joined = join_name_;
        join_at_ = std::string::npos;
    }
    lines_.push_back(std::move(cur_));
    cur_ = MplLine{};
    started_ = false;
    if (lines_.size() > kMaxLines) {
        MplLine& l = lines_.back();
        l.diagnostics.push_back({Severity::Error, "OC25",
                                 "more than " + std::to_string(kMaxLines) +
                                     " lines of MPL output: does a %WHILE or a macro expand without end?",
                                 {}, 0, 0});
        l.fatal = true;
        throw Stop{};
    }
    control(lines_.size() - 1);
}

// Keil's MPL reads the file of an output line that starts with
// '$INCLUDE (file)' right after that line; only the first control on the
// line counts. It also takes note of the directories of an $INCDIR that is
// the first control on its line (it ignores one after another control).
void Processor::control(std::size_t k) {
    const std::string_view t = lines_[k].text;
    std::size_t i = t.find_first_not_of(" \t");
    if (i == std::string_view::npos || t[i] != '$') return;
    ++i;
    for (bool first = true;; first = false) {
        while (i < t.size() && blank(t[i])) ++i;
        const std::size_t name_at = i;
        i = ident_end(t, i);
        if (i == name_at) return;
        const std::string name = upper(t.substr(name_at, i - name_at));
        while (i < t.size() && blank(t[i])) ++i;
        std::optional<std::string_view> arg;
        if (i < t.size() && t[i] == '(') {
            const std::size_t close = t.find(')', i);
            if (close == std::string_view::npos) return;
            arg = t.substr(i + 1, close - i - 1);
            i = close + 1;
        }
        if (name == "INCLUDE" || name == "IC") {
            // Keil reads the file only when nothing but a comment follows.
            if (!first || !arg || trim(*arg).empty()) return;
            const std::string_view rest = trim(t.substr(i));
            if (!rest.empty() && rest[0] != ';') return;
            std::size_t index = 0;
            try {
                index = host_.include(trim(*arg), lines_[k].file, lines_[k].level, static_cast<int>(name_at) + 1);
            } catch (const AsmError& e) {
                lines_[k].diagnostics.push_back({Severity::Error, e.code(), e.message(), {}, 0, e.column()});
                lines_[k].fatal = true;
                throw Stop{};
            }
            scan_file(index, lines_[k].level + 1);
            return;
        }
        if (!first) return;
        if ((name == "INCDIR" || name == "ID") && arg) host_.incdirs(*arg);
    }
}

// A %DEFINE read from the source file leaves an empty line for each line
// it took, ahead of the line it is on.
void Processor::blank_lines(std::size_t n) {
    for (std::size_t k = 0; k < n; ++k) {
        MplLine l;
        l.file = pos_.file;
        l.line = pos_.line;
        l.level = pos_.level;
        lines_.push_back(std::move(l));
    }
}

void Processor::error(std::string code, std::string message) {
    if (disabled_) return;
    disabled_ = true;
    start();
    cur_.diagnostics.push_back({Severity::Error, std::move(code), std::move(message) + kOff, {}, 0,
                                static_cast<int>(cur_.text.size()) + 1});
}

void Processor::warning(std::string code, std::string message) {
    start();
    cur_.diagnostics.push_back(
        {Severity::Warning, std::move(code), std::move(message), {}, 0, static_cast<int>(cur_.text.size()) + 1});
}

void Processor::fatal(std::string code, std::string message) {
    start();
    cur_.diagnostics.push_back({Severity::Error, std::move(code), std::move(message), {}, 0, 0});
    cur_.fatal = true;
    lines_.push_back(std::move(cur_));
    cur_ = MplLine{};
    started_ = false;
    throw Stop{};
}

void Processor::step() {
    if (++steps_ > kMaxSteps)
        fatal("OC33", "more than " + std::to_string(kMaxSteps) +
                          " MPL macro calls and loop rounds: does a %WHILE or a macro run without end?");
}

// ---------------------------------------------------------------- reading

std::vector<MplLine> Processor::run(const std::vector<std::pair<std::size_t, int>>& files) {
    try {
        for (const auto& [index, level] : files) scan_file(index, level);
        if (started_) finish();
    } catch (const Stop&) {
    }
    return std::move(lines_);
}

void Processor::scan_file(std::size_t index, int level) {
    auto& slot = texts_[index];
    if (!slot) {
        slot = std::make_unique<FileText>();
        slot->index = index;
        slot->name = host_.name(index);
        const std::string& raw = host_.text(index);
        slot->text.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i)
            if (raw[i] != '\r' || (i + 1 < raw.size() && raw[i + 1] != '\n')) slot->text += raw[i];
        slot->starts.push_back(0);
        for (std::size_t i = 0; i < slot->text.size(); ++i)
            if (slot->text[i] == '\n') slot->starts.push_back(i + 1);
    }
    const FileText& f = *slot;
    const Pos saved = pos_;
    const bool saved_exit = exit_;
    const int saved_loops = loops_;
    exit_ = false;
    loops_ = 0;
    Input in{f.text, 0, &f, level};
    scan(in, nullptr, nullptr);
    pos_ = saved;
    exit_ = saved_exit;
    loops_ = saved_loops;
    if (!f.text.empty() && f.text.back() != '\n' && started_ && level > 0) {
        join_at_ = cur_.text.size();
        join_name_ = f.name;
    }
}

void Processor::scan(Input& in, std::string* out, const Scope* scope) {
    while (!in.end() && !exit_) {
        if (in.file) pos_ = {in.file->index, in.file->line_of(in.i), in.level};
        if (disabled_ || in.s[in.i] != meta_) {
            // Plain text up to the metacharacter (or, in a file, the end of
            // the line, to keep track of line numbers).
            std::size_t j = disabled_ ? in.s.size() : in.s.find(meta_, in.i);
            if (j == std::string_view::npos) j = in.s.size();
            if (in.file) j = std::min(j, std::min(in.s.find('\n', in.i), in.s.size() - 1) + 1);
            put(out, in.s.substr(in.i, j - in.i));
            in.i = j;
            continue;
        }
        ++in.i;
        if (++depth_ > kMaxDepth)
            fatal("OC33", "MPL calls nested more than " + std::to_string(kMaxDepth) +
                              " deep: does a macro call itself without end?");
        call(in, out, scope);
        --depth_;
    }
}

std::string Processor::evaluate(std::string_view text, const Scope* scope) {
    std::string r;
    Input in{text};
    scan(in, &r, scope);
    return r;
}

// After the metacharacter.
void Processor::call(Input& in, std::string* out, const Scope* scope) {
    // Only the text of the expression itself, not that of calls in it.
    const bool expr = expr_;
    expr_ = false;
    struct Restore {
        bool& flag;
        bool value;
        ~Restore() { flag = value; }
    } restore{expr_, expr};
    const std::size_t after_meta = in.i;
    skip_blanks(in);
    if (in.end()) {
        error("A150", "premature end of the source after '" + std::string(1, meta_) + "'");
        return;
    }
    char c = in.s[in.i];
    if (in.i != after_meta && (digit(c) || c == '(' || c == '\'')) {
        // Not an escape, bracket or comment after blanks: see below.
    } else if (digit(c)) {
        // %n: the next n characters are text.
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(c - '0'), in.s.size() - in.i - 1);
        put(out, in.s.substr(in.i + 1, n));
        in.i += 1 + n;
        return;
    } else if (c == '(') {
        std::string_view text;
        if (balanced(in, "BRACKET", text)) put(out, escapes(text));
        return;
    } else if (c == '\'') {
        in.i = special_end(in.s, in.i - 1);
        return;
    }
    bool literal = false;
    if (c == '*') {
        literal = true;
        ++in.i;
        skip_blanks(in);
        if (in.end()) {
            error("A150", "premature end of the source after '" + std::string(1, meta_) + "*'");
            return;
        }
        c = in.s[in.i];
    }
    if (!id_start(c)) {
        // Keil keeps the metacharacter and drops the character after it. In
        // an expression, the '%' is then an invalid token (A166).
        if (!expr)
            error("A151", std::string("identifier expected after '") + meta_ + (literal ? "*" : "") + "', not '" +
                          std::string(1, c) + "'");
        put(out, std::string(1, meta_));
        ++in.i;
        return;
    }
    const std::size_t from = in.i;
    in.i = ident_end(in.s, in.i);
    const std::string name = upper(in.s.substr(from, in.i - from));
    if (scope) {
        if (auto it = scope->find(name); it != scope->end()) {
            put(out, it->second);
            return;
        }
    }
    if (name == "DEFINE") {
        define(in, literal, scope);
        return;
    }
    if (builtin(name, in, out, scope)) return;
    auto m = macros_.find(name);
    if (m == macros_.end()) {
        error("A160", "call of undefined MPL macro '" + name + "'");
        return;
    }
    const std::shared_ptr<const Macro> macro = m->second;  // the call may redefine it
    user_call(name, *macro, literal, in, out, scope);
}

// The end of an escape (%n) or a comment (%'...' or %'... to the end of the
// line) whose metacharacter is at s[i], or i if there is none.
std::size_t Processor::special_end(std::string_view s, std::size_t i) const {
    // Keil allows blanks between '%' and a name, but not before an escape's
    // digit or a comment's quote.
    const std::size_t j = i + 1;
    if (j >= s.size()) return i;
    if (digit(s[j])) return std::min(s.size(), j + 1 + static_cast<std::size_t>(s[j] - '0'));
    if (s[j] != '\'') return i;
    const std::size_t e = s.find_first_of("'\n", j + 1);
    return e == std::string_view::npos ? s.size() : e + 1;
}

// What a literal call and the first step of a normal call make of a macro
// body: escapes and comments are processed; everything else, including
// %(...), is kept.
std::string Processor::escapes(std::string_view s) const {
    if (disabled_) return std::string(s);
    std::string r;
    r.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != meta_) {
            r += s[i++];
            continue;
        }
        const std::size_t j = i + 1;
        if (j < s.size() && digit(s[j])) {
            const std::size_t e = special_end(s, i);
            r.append(s.substr(j + 1, e - j - 1));
            i = e;
        } else if (j < s.size() && s[j] == '\'') {
            i = special_end(s, i);
        } else if (j < s.size() && s[j] == '(') {
            int depth = 0;
            std::size_t k = j;
            for (; k < s.size(); ++k) {
                if (s[k] == meta_) {
                    if (std::size_t e = special_end(s, k); e != k) {
                        k = e - 1;
                        continue;
                    }
                }
                if (s[k] == '(') ++depth;
                if (s[k] == ')' && --depth == 0) break;
            }
            k = std::min(k + 1, s.size());
            r.append(s.substr(i, k - i));
            i = k;
        } else {
            r += s[i++];
        }
    }
    return r;
}

// The '(' of a built-in function, after blanks.
bool Processor::open_paren(Input& in, std::string_view fn) {
    skip_blanks(in);
    if (!in.end() && in.s[in.i] == '(') return true;
    error("A152", "MPL function '" + std::string(fn) + "': '(' expected");
    // Keil drops the character it found instead.
    if (!in.end()) ++in.i;
    return false;
}

// '(balanced text)', after blanks. Escapes and comments inside do not count
// in the balance.
bool Processor::balanced(Input& in, std::string_view fn, std::string_view& text) {
    if (!open_paren(in, fn)) return false;
    const std::size_t from = ++in.i;
    int depth = 1;
    while (!in.end()) {
        const char c = in.s[in.i];
        if (c == meta_ && !disabled_) {
            if (std::size_t e = special_end(in.s, in.i); e != in.i) {
                in.i = e;
                continue;
            }
        }
        if (c == '(') ++depth;
        if (c == ')' && --depth == 0) {
            text = in.s.substr(from, in.i - from);
            ++in.i;
            return true;
        }
        ++in.i;
    }
    error("A153", "MPL function '" + std::string(fn) + "': unbalanced parentheses");
    return false;
}

// ---------------------------------------------------------------- macros

// The define pattern of a macro, after its name: parameters, literal
// delimiters (other characters, or any characters in %n and %(...)),
// identifier delimiters (@word) and blank delimiters. Blanks next to a
// literal or identifier delimiter, or at either end, do not count.
std::vector<Elem> Processor::parse_pattern(std::string_view p) const {
    std::vector<Elem> raw;
    bool join = false;  // the next literal character extends the previous literal
    auto literal = [&](std::string_view s) {
        if (join && !raw.empty() && raw.back().kind == Elem::Literal)
            raw.back().text += s;
        else
            raw.push_back({Elem::Literal, std::string(s)});
        join = true;
    };
    for (std::size_t i = 0; i < p.size();) {
        const char c = p[i];
        if (blank(c)) {
            while (i < p.size() && blank(p[i])) ++i;
            raw.push_back({Elem::Blank, {}});
            join = false;
        } else if (id_start(c)) {
            const std::size_t e = ident_end(p, i);
            raw.push_back({Elem::Param, upper(p.substr(i, e - i))});
            i = e;
            join = false;
        } else if (c == '@' && i + 1 < p.size() && id_start(p[i + 1])) {
            const std::size_t e = ident_end(p, i + 1);
            raw.push_back({Elem::Word, upper(p.substr(i + 1, e - i - 1))});
            i = e;
            join = false;
        } else if (c == meta_) {
            std::size_t j = i + 1;
            while (j < p.size() && blank(p[j])) ++j;
            if (j < p.size() && digit(p[j])) {
                const std::size_t e = special_end(p, i);
                literal(p.substr(j + 1, e - j - 1));
                i = e;
            } else if (j < p.size() && p[j] == '\'') {
                i = special_end(p, i);
            } else if (j < p.size() && p[j] == '(') {
                int depth = 0;
                std::size_t k = j;
                for (; k < p.size(); ++k) {
                    if (p[k] == '(') ++depth;
                    if (p[k] == ')' && --depth == 0) break;
                }
                literal(p.substr(j + 1, std::min(k, p.size()) - j - 1));
                i = std::min(k + 1, p.size());
            } else {
                literal(p.substr(i, 1));
                ++i;
            }
        } else {
            literal(p.substr(i, 1));
            ++i;
        }
    }
    std::vector<Elem> out;
    for (std::size_t k = 0; k < raw.size(); ++k) {
        if (raw[k].kind == Elem::Blank) {
            const bool edge = k == 0 || k + 1 == raw.size();
            auto delimiter = [&](std::size_t n) {
                return raw[n].kind == Elem::Literal || raw[n].kind == Elem::Word;
            };
            if (edge || delimiter(k - 1) || delimiter(k + 1)) continue;
        }
        out.push_back(std::move(raw[k]));
    }
    return out;
}

// One argument of a call, ending where the next element of the pattern
// matches: a literal delimiter at parenthesis depth 0; a blank followed by an
// identifier delimiter; or (for a blank delimiter or the end of the pattern)
// a blank, which is taken too. %(...) in an argument gives its contents;
// escapes stay for the evaluation of the argument.
bool Processor::collect(Input& in, const Elem* next, std::string& arg) {
    const bool to_blank = !next || next->kind == Elem::Blank || next->kind == Elem::Param;
    if (to_blank) skip_blanks(in);
    int depth = 0;
    while (!in.end()) {
        const char c = in.s[in.i];
        if (depth == 0) {
            if (to_blank && blank(c)) {
                ++in.i;
                return true;
            }
            if (next && next->kind == Elem::Literal && in.s.substr(in.i).substr(0, next->text.size()) == next->text)
                return true;
            if (next && next->kind == Elem::Word && in.i > 0 && blank(in.s[in.i - 1]) &&
                word_at(in.s, in.i, next->text))
                return true;
        }
        if (c == meta_ && !disabled_) {
            std::size_t j = in.i + 1;
            while (j < in.s.size() && blank(in.s[j])) ++j;
            if (j < in.s.size() && in.s[j] == '(') {
                Input b{in.s, j};
                std::string_view text;
                if (!balanced(b, "BRACKET", text)) {
                    in.i = b.i;
                    return false;
                }
                arg += text;
                in.i = b.i;
                continue;
            }
            if (std::size_t e = special_end(in.s, in.i); e != in.i) {
                arg.append(in.s.substr(in.i, e - in.i));
                in.i = e;
                continue;
            }
        }
        if (c == '(') ++depth;
        if (c == ')') {
            if (depth == 0 && !to_blank) return false;
            --depth;
        }
        arg += c;
        ++in.i;
    }
    return to_blank;
}

// Reads the arguments of a call by the macro's define pattern.
bool Processor::match(Input& in, const std::vector<Elem>& pattern, const std::string& name,
                      std::vector<std::string>& args) {
    bool after_param = false;
    for (std::size_t k = 0; k < pattern.size(); ++k) {
        const Elem& e = pattern[k];
        switch (e.kind) {
            case Elem::Param: {
                std::string arg;
                const Elem* next = k + 1 < pattern.size() ? &pattern[k + 1] : nullptr;
                if (!collect(in, next, arg)) {
                    error("A159", "MPL call of '" + name + "' does not match its pattern: delimiter '" +
                                      (next ? next->text : std::string()) + "' not found");
                    return false;
                }
                args.push_back(std::move(arg));
                after_param = true;
                continue;
            }
            case Elem::Literal:
                if (!after_param) skip_blanks(in);
                if (in.s.substr(in.i).substr(0, e.text.size()) != e.text) {
                    error("A159",
                          "MPL call of '" + name + "' does not match its pattern: delimiter '" + e.text + "' not found");
                    return false;
                }
                in.i += e.text.size();
                break;
            case Elem::Word:
                skip_blanks(in);
                if (!word_at(in.s, in.i, e.text)) {
                    error("A159",
                          "MPL call of '" + name + "' does not match its pattern: delimiter '" + e.text + "' not found");
                    return false;
                }
                in.i += e.text.size();
                skip_blanks(in);
                break;
            case Elem::Blank:
                skip_blanks(in);
                break;
        }
        after_param = false;
    }
    return true;
}

void Processor::user_call(const std::string& name, const Macro& m, bool literal, Input& in, std::string* out,
                          const Scope* scope) {
    step();
    std::vector<std::string> args;
    if (!match(in, m.pattern, name, args)) return;
    Scope inner;
    std::size_t n = 0;
    for (const Elem& e : m.pattern)
        if (e.kind == Elem::Param) inner.emplace(e.text, evaluate(args[n++], scope));
    // LOCAL names: ??NAME?n. Keil numbers them from its counter plus the
    // number of names, then raises the counter by twice that number.
    const auto k = static_cast<unsigned>(m.locals.size());
    for (unsigned i = 0; i < k; ++i)
        inner.emplace(m.locals[i], "??" + m.locals[i] + "?" + std::to_string(locals_ + k + i));
    locals_ += 2 * k;
    std::string body = escapes(m.body);
    if (literal) {
        put(out, body);
        return;
    }
    ++loops_;
    Input b{body};
    scan(b, out, &inner);
    --loops_;
    exit_ = false;
}

void Processor::set_macro(const std::string& name, std::string body) {
    auto m = std::make_shared<Macro>();
    m->body = std::move(body);
    macros_[name] = std::move(m);
}

// %DEFINE (name pattern) [LOCAL names] (body). %*DEFINE keeps the body as
// written; %DEFINE expands it first.
void Processor::define(Input& in, bool literal, const Scope* scope) {
    const std::size_t from = in.i;
    struct Lines {
        Processor& p;
        Input& in;
        std::size_t from;
        ~Lines() {
            if (in.file && !std::uncaught_exceptions())
                p.blank_lines(static_cast<std::size_t>(
                    std::count(in.s.begin() + static_cast<std::ptrdiff_t>(from),
                               in.s.begin() + static_cast<std::ptrdiff_t>(std::min(in.i, in.s.size())), '\n')));
        }
    } lines{*this, in, from};
    std::string_view head;
    if (!balanced(in, "DEFINE", head)) return;
    std::size_t i = 0;
    while (i < head.size() && blank(head[i])) ++i;
    if (i >= head.size() || !id_start(head[i])) {
        error("A151", "MPL function 'DEFINE': macro name expected");
        return;
    }
    const std::size_t name_end = ident_end(head, i);
    const std::string name = upper(head.substr(i, name_end - i));
    auto m = std::make_shared<Macro>();
    m->pattern = parse_pattern(head.substr(name_end));
    skip_blanks(in);
    if (word_at(in.s, in.i, "LOCAL")) {
        in.i += 5;
        for (;;) {
            skip_blanks(in);
            if (in.end() || in.s[in.i] == '(') break;
            if (in.s[in.i] == ',') {
                ++in.i;
                continue;
            }
            if (!id_start(in.s[in.i])) {
                error("A152", "MPL function 'DEFINE': '(' expected");
                return;
            }
            const std::size_t e = ident_end(in.s, in.i);
            m->locals.push_back(upper(in.s.substr(in.i, e - in.i)));
            in.i = e;
        }
    }
    std::string_view body;
    if (!balanced(in, "DEFINE", body)) return;
    m->body = literal ? std::string(body) : evaluate(body, scope);
    macros_[name] = std::move(m);
}

// %IF (expression) THEN (text) [ELSE (text)] FI
void Processor::if_(Input& in, std::string* out, const Scope* scope) {
    std::string_view cond, yes, no;
    if (!balanced(in, "IF", cond)) return;
    const auto value = expression(cond, scope);
    skip_blanks(in);
    if (!word_at(in.s, in.i, "THEN")) {
        error("A154", "'THEN' expected in %IF");
        return;
    }
    in.i += 4;
    if (!balanced(in, "IF", yes)) return;
    skip_blanks(in);
    if (word_at(in.s, in.i, "ELSE")) {
        in.i += 4;
        if (!balanced(in, "IF", no)) return;
        skip_blanks(in);
    }
    const bool fi = word_at(in.s, in.i, "FI");
    Input text{value && *value != 0 ? yes : no};
    scan(text, out, scope);
    if (fi) {
        in.i += 2;
    } else {
        // Keil drops the character in place of FI.
        error("A154", "'FI' expected at the end of %IF");
        if (!in.end()) ++in.i;
    }
}

// %WHILE (expression) (text) and %REPEAT (expression) (text)
void Processor::loop(Input& in, std::string* out, const Scope* scope, bool repeat) {
    const char* fn = repeat ? "REPEAT" : "WHILE";
    std::string_view cond, body;
    if (!balanced(in, fn, cond)) return;
    std::optional<std::int32_t> count;
    if (repeat) count = expression(cond, scope);
    if (!balanced(in, fn, body)) return;
    ++loops_;
    for (std::int64_t round = 0; !disabled_; ++round) {
        step();
        if (repeat) {
            if (!count || round >= *count) break;
        } else if (auto v = expression(cond, scope); !v || *v == 0) {
            break;
        }
        Input b{body};
        scan(b, out, scope);
        if (exit_) break;
    }
    --loops_;
    exit_ = false;
}

// %MATCH (name1 delimiter name2) (text)
void Processor::match_(Input& in, const Scope* scope) {
    std::string_view head, text;
    if (!balanced(in, "MATCH", head)) return;
    std::size_t i = 0;
    while (i < head.size() && blank(head[i])) ++i;
    std::size_t e = ident_end(head, i);
    const std::string first = upper(head.substr(i, e - i));
    std::optional<char> delimiter;  // none: a blank
    i = e;
    if (i < head.size() && !blank(head[i]) && !id_char(head[i])) delimiter = head[i++];
    while (i < head.size() && blank(head[i])) ++i;
    e = ident_end(head, i);
    const std::string second = upper(head.substr(i, e - i));
    i = e;
    while (i < head.size() && blank(head[i])) ++i;
    if (first.empty() || second.empty() || i != head.size() || !id_start(first[0]) || !id_start(second[0])) {
        error("A156", "MPL function 'MATCH': the pattern must be a name, a delimiter and a name");
        return;
    }
    if (!balanced(in, "MATCH", text)) return;
    const std::string value = evaluate(text, scope);
    std::string left = value, right;
    if (delimiter) {
        if (std::size_t p = value.find(*delimiter); p != std::string::npos) {
            left = value.substr(0, p);
            right = value.substr(p + 1);
        }
    } else {
        const std::string_view v = value;
        std::size_t a = 0;
        while (a < v.size() && blank(v[a])) ++a;
        std::size_t b = a;
        while (b < v.size() && !blank(v[b])) ++b;
        left = std::string(v.substr(a, b - a));
        if (b < v.size()) right = std::string(v.substr(b + 1));
    }
    set_macro(first, std::move(left));
    set_macro(second, std::move(right));
}

bool Processor::builtin(const std::string& name, Input& in, std::string* out, const Scope* scope) {
    std::string_view text;
    std::vector<std::string> args;
    if (name == "SET") {
        if (!match(in, kPair, name, args)) return true;
        const std::string target = evaluate(args[0], scope);
        const std::string_view id = trim(target);
        if (id.empty() || !id_start(id[0]) || ident_end(id, 0) != id.size()) {
            error("A151", "MPL function 'SET': name expected, not '" + std::string(id) + "'");
            return true;
        }
        const auto value = expression(args[1], scope);
        set_macro(upper(id), mpl_number(value.value_or(0)));
    } else if (name == "EVAL") {
        if (!balanced(in, name, text)) return true;
        put(out, mpl_number(expression(text, scope).value_or(0)));
    } else if (name == "LEN") {
        if (balanced(in, name, text)) put(out, std::to_string(evaluate(text, scope).size()));
    } else if (name == "EQS" || name == "NES" || name == "LTS" || name == "LES" || name == "GTS" || name == "GES") {
        if (!open_paren(in, name) || !match(in, kPair, name, args)) return true;
        const int c = evaluate(args[0], scope).compare(evaluate(args[1], scope));
        const bool r = name == "EQS"   ? c == 0
                       : name == "NES" ? c != 0
                       : name == "LTS" ? c < 0
                       : name == "LES" ? c <= 0
                       : name == "GTS" ? c > 0
                                       : c >= 0;
        put(out, r ? "1" : "0");
    } else if (name == "SUBSTR") {
        if (!open_paren(in, name) || !match(in, kTriple, name, args)) return true;
        const std::string s = evaluate(args[0], scope);
        const auto from = expression(args[1], scope);
        const auto count = expression(args[2], scope);
        if (!from || !count) return true;
        const auto f = static_cast<std::uint32_t>(*from), n = static_cast<std::uint32_t>(*count);
        if (f >= 1 && f <= s.size()) put(out, std::string_view(s).substr(f - 1, n));
    } else if (name == "MATCH") {
        match_(in, scope);
    } else if (name == "IF") {
        if_(in, out, scope);
    } else if (name == "WHILE" || name == "REPEAT") {
        loop(in, out, scope, name == "REPEAT");
    } else if (name == "EXIT") {
        if (loops_ == 0)
            error("A157", "%EXIT is allowed only in %WHILE, %REPEAT and macros");
        else
            exit_ = true;
    } else if (name == "METACHAR") {
        if (!balanced(in, name, text)) return true;
        const std::string v = evaluate(text, scope);
        const char c = v.empty() ? ' ' : v[0];
        if (c == '@' || c == '(' || c == ')' || c == '*' || blank(c) || id_char(c))
            error("A158", "illegal MPL metacharacter '" + std::string(1, c) + "'");
        else
            meta_ = c;
    } else if (name == "IN") {
        warning("OC34", "%IN reads a line from the console in Keil; openc166 has no console, so it gives nothing");
    } else if (name == "OUT") {
        if (balanced(in, name, text)) warning("OC34", "%OUT (Keil prints this on the console): " + evaluate(text, scope));
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- expressions

std::optional<std::int32_t> Processor::expression(std::string_view raw, const Scope* scope) {
    const bool was_disabled = disabled_;
    expr_ = true;
    const std::string text = evaluate(raw, scope);
    expr_ = false;
    if (disabled_ && !was_disabled) return std::nullopt;
    return parse_expression(text);
}

std::int32_t wrap(std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v)); }

std::optional<std::int32_t> Processor::parse_expression(std::string_view text) {
    struct Tok {
        enum Kind : unsigned char { Num, Op, Open, Close } kind;
        std::int32_t value = 0;
        std::string op;
    };
    std::vector<Tok> toks;
    static const char* const kWords[] = {"MOD", "SHL", "SHR", "EQ",  "NE", "LT",  "LE",   "GT",
                                         "GE",  "AND", "OR",  "XOR", "NOT", "HIGH", "LOW"};
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (blank(c)) {
            ++i;
        } else if (digit(c)) {
            // Like Keil: hex digits, then an optional H, O or Q; without one,
            // a last B or D gives the base. So 1G is 1 and G, 12Q3 is 12Q and 3.
            std::size_t e = i;
            while (e < text.size() && (std::isxdigit(static_cast<unsigned char>(text[e])) || text[e] == '$')) ++e;
            std::string digits;
            for (char d : text.substr(i, e - i))
                if (d != '$') digits += static_cast<char>(std::toupper(static_cast<unsigned char>(d)));
            int base = 10;
            const char suffix =
                e < text.size() ? static_cast<char>(std::toupper(static_cast<unsigned char>(text[e]))) : '\0';
            if (suffix == 'H' || suffix == 'O' || suffix == 'Q') {
                base = suffix == 'H' ? 16 : 8;
                ++e;
            } else if (digits.back() == 'B' || digits.back() == 'D') {
                base = digits.back() == 'B' ? 2 : 10;
                digits.pop_back();
            }
            std::uint64_t v = 0;
            for (char d : digits) {
                const int n = digit(d) ? d - '0' : d >= 'A' && d <= 'Z' ? d - 'A' + 10 : 99;
                if (n >= base) {
                    error("A162", "invalid digit '" + std::string(1, d) + "' in the number '" +
                                      std::string(text.substr(i, e - i)) + "'");
                    return std::nullopt;
                }
                v = (v * static_cast<unsigned>(base) + static_cast<unsigned>(n)) & 0xFFFFFFFFu;
            }
            toks.push_back({Tok::Num, wrap(static_cast<std::int64_t>(v)), {}});
            i = e;
        } else if (c == '\'' || c == '"') {
            // Keil accepts a string left open at the end, but not at a line end.
            std::size_t e = text.find_first_of(c == '\'' ? std::string_view("'\n") : std::string_view("\"\n"), i + 1);
            if (e != std::string_view::npos && text[e] == '\n') {
                error("A163", "unclosed string in an MPL expression");
                return std::nullopt;
            }
            if (e == std::string_view::npos) e = text.size();
            const std::string_view s = text.substr(i + 1, e - i - 1);
            if (s.empty() || s.size() > 2) {
                error("A164", "a string in an MPL expression must have 1 or 2 characters");
                return std::nullopt;
            }
            std::int32_t v = 0;
            for (char d : s) v = v * 256 + static_cast<unsigned char>(d);
            toks.push_back({Tok::Num, v, {}});
            i = std::min(e + 1, text.size());
        } else if (id_start(c)) {
            const std::size_t e = ident_end(text, i);
            const std::string w = upper(text.substr(i, e - i));
            if (std::find(std::begin(kWords), std::end(kWords), w) == std::end(kWords)) {
                error("A165", "unknown identifier '" + w +
                                  "' in an MPL expression (MPL knows its own macros, not assembler symbols)");
                return std::nullopt;
            }
            toks.push_back({Tok::Op, 0, w});
            i = e;
        } else if (c == '+' || c == '-' || c == '*' || c == '/') {
            toks.push_back({Tok::Op, 0, std::string(1, c)});
            ++i;
        } else if (c == '(' || c == ')') {
            toks.push_back({c == '(' ? Tok::Open : Tok::Close, 0, {}});
            ++i;
        } else {
            error("A166", "'" + std::string(1, c) + "' is not allowed in an MPL expression");
            return std::nullopt;
        }
    }
    if (toks.empty()) return 0;

    // Keil stops at the first problem: a missing operand silently makes the
    // whole expression 0 ('1 +', '()'), a division by zero is an error.
    std::size_t p = 0;
    bool bad = false;  // syntax error
    bool zero = false;  // division by zero
    bool missing = false;  // operand missing
    auto stop = [&] { return bad || zero || missing; };
    auto is = [&](std::initializer_list<const char*> ops) -> const char* {
        if (p >= toks.size() || toks[p].kind != Tok::Op) return nullptr;
        for (const char* op : ops)
            if (toks[p].op == op) return op;
        return nullptr;
    };
    std::function<std::int32_t(int)> level = [&](int lv) -> std::int32_t {
        static const std::initializer_list<const char*> kLevels[] = {
            {"AND", "OR", "XOR"}, {"EQ", "NE", "LT", "LE", "GT", "GE"}, {"SHL", "SHR"}, {"+", "-"}, {"*", "/", "MOD"}};
        if (lv == 5) {
            if (const char* op = is({"-", "+", "NOT", "HIGH", "LOW"})) {
                ++p;
                const std::int32_t v = level(5);
                if (stop()) return 0;
                const std::string_view o = op;
                if (o == "-") return wrap(-static_cast<std::int64_t>(v));
                if (o == "+") return v;
                if (o == "NOT") return v == 0 ? 1 : 0;
                if (o == "HIGH") return (v >> 8) & 0xFF;
                return v & 0xFF;
            }
            if (p < toks.size() && toks[p].kind == Tok::Num) return toks[p++].value;
            if (p < toks.size() && toks[p].kind == Tok::Open) {
                ++p;
                const std::int32_t v = level(0);
                if (stop()) return 0;
                if (p < toks.size() && toks[p].kind == Tok::Close)
                    ++p;
                else
                    bad = true;
                return v;
            }
            missing = true;
            return 0;
        }
        std::int32_t a = level(lv + 1);
        while (!stop()) {
            const char* op = is(kLevels[lv]);
            if (!op) break;
            ++p;
            const std::int32_t b = level(lv + 1);
            if (stop()) break;
            const std::string_view o = op;
            const std::int64_t x = a, y = b;
            if (o == "AND") a = a & b;
            else if (o == "OR") a = a | b;
            else if (o == "XOR") a = a ^ b;
            else if (o == "EQ") a = a == b;
            else if (o == "NE") a = a != b;
            else if (o == "LT") a = a < b;
            else if (o == "LE") a = a <= b;
            else if (o == "GT") a = a > b;
            else if (o == "GE") a = a >= b;
            // As on x86, only the low 5 bits of a shift count count.
            else if (o == "SHL") a = wrap(static_cast<std::int64_t>(static_cast<std::uint32_t>(a) << (y & 31)));
            else if (o == "SHR") a = a >> (y & 31);
            else if (o == "+") a = wrap(x + y);
            else if (o == "-") a = wrap(x - y);
            else if (o == "*") a = wrap(x * y);
            else if (b == 0) zero = true, a = 0;
            else if (o == "/") a = wrap(x / y);
            else a = wrap(x % y);
        }
        return a;
    };
    const std::int32_t v = level(0);
    if (zero) {
        error("A167", "division by zero in an MPL expression");
        return std::nullopt;
    }
    if (missing) return 0;
    if (!bad && p != toks.size()) bad = true;
    if (bad) {
        error("A168", "syntax error in the MPL expression '" + std::string(trim(text)) + "'");
        return std::nullopt;
    }
    return v;
}

}  // namespace

std::string mpl_number(std::int64_t value) {
    if (value < 0 || value > 0xFFFF) return std::to_string(static_cast<std::int32_t>(value));
    static const char kHex[] = "0123456789ABCDEF";
    // Keil writes two hex digits up to 0FFH and four above.
    std::string digits;
    const std::size_t width = value > 0xFF ? 4 : 2;
    for (auto v = static_cast<unsigned>(value); digits.size() < width; v >>= 4) digits.insert(digits.begin(), kHex[v & 15]);
    if (digits[0] >= 'A') digits.insert(digits.begin(), '0');
    return digits + "H";
}

std::vector<MplLine> run_mpl(const std::vector<std::pair<std::size_t, int>>& files, const MplHost& host) {
    return Processor(host).run(files);
}

}  // namespace oc166
