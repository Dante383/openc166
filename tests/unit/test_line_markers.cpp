// SPDX-License-Identifier: MIT
// Line markers (# 42 "main.c"): which lines of a C program the assembly
// lines belong to, for debug information.
#include "doctest.h"
#include "helpers.hpp"

using namespace oc166;

namespace {

// "file:line" of the C source of each line with code, in order, or "-" for
// a line without one.
std::string sources(const AssemblyResult& r) {
    std::string s;
    for (const auto& l : r.listing) {
        if (l.bytes.empty()) continue;
        if (!s.empty()) s += ' ';
        s += l.source_line ? l.source_file + ":" + std::to_string(l.source_line) : "-";
    }
    return s;
}

std::string codes(const AssemblyResult& r) {
    std::string s;
    for (const auto& d : r.diagnostics) s += (s.empty() ? "" : " ") + d.code;
    return s;
}

std::string program(const std::string& body) {
    return "$MOD167\nP SECTION CODE AT 0C000H\n" + body + "P ENDS\n END\n";
}

}  // namespace

TEST_CASE("code after a line marker belongs to its line") {
    auto r = test::run(program(" NOP\n"
                               "# 10 \"main.c\"\n"
                               " NOP\n"
                               " NOP\n"
                               "# 12\n"  // the file named last
                               " NOP\n"
                               "#line 3 \"lib/x\\\\y.c\" 1 3\n"  // #line, a backslash, flags
                               " NOP\n"
                               "# 4 ; comment\n"
                               " NOP\n"
                               "# 0\n"  // back to the assembly lines
                               " NOP\n"
                               "#\n"));  // ignored
    CHECK(codes(r) == "");
    CHECK(sources(r) == "- main.c:10 main.c:10 main.c:12 lib/x\\y.c:3 lib/x\\y.c:4 -");
    // The marker lines themselves emit nothing.
    CHECK(test::hex(flat_image(r).bytes) == "CC00CC00CC00CC00CC00CC00CC00");
}

TEST_CASE("macro expansions take the line marker of the call") {
    const std::string src = "$MOD167\nTWO MACRO\n NOP\n NOP\n ENDM\nP SECTION CODE AT 0C000H\n# 7 \"m.c\"\n TWO\nP ENDS\n END\n";
    auto r = test::run(src);
    CHECK(codes(r) == "");
    CHECK(sources(r) == "m.c:7 m.c:7");
    // The same without MPL.
    r = test::run("$NOMPL\n" + src);
    CHECK(sources(r) == "m.c:7 m.c:7");
}

TEST_CASE("a line marker holds for its own file only") {
    AssembleOptions o;
    o.cpu = isa::Cpu::C167;
    o.read_file = [](const std::string& path) -> std::optional<std::string> {
        if (path.find("in.inc") != std::string::npos) return std::string(" NOP\n# 50 \"inc.c\"\n NOP\n");
        return std::nullopt;
    };
    for (const char* mpl : {"", "$NOMPL\n"}) {
        CAPTURE(mpl);
        auto r = assemble(mpl + program("# 1 \"main.c\"\n NOP\n$INCLUDE(in.inc)\n NOP\n$INCLUDE(in.inc)\n"), o);
        CHECK(codes(r) == "");
        CHECK(sources(r) == "main.c:1 - inc.c:50 main.c:1 - inc.c:50");
    }
}

TEST_CASE("line markers in lines that are not assembled") {
    auto r = test::run(program("# 5 \"a.c\"\nIF 0\n# 9 \"b.c\"\nENDIF\n NOP\n"));
    CHECK(codes(r) == "");
    CHECK(sources(r) == "a.c:5");
}

TEST_CASE("bad line markers") {
    for (const char* bad : {"# x", "# 12x", "# 12 main.c", "# 12 \"main.c", "#line", "# 99999999999"}) {
        CAPTURE(bad);
        auto r = test::run(program(std::string(bad) + "\n NOP\n"));
        CHECK(codes(r) == "OC36");
    }
    // TASKING a166 ignores every line that starts with '#'.
    AssembleOptions o;
    o.dialect = Dialect::Tasking;
    auto r = assemble("S SECTION CODE AT 0C000H\n# x\n# 3 \"t.c\"\n NOP\nS ENDS\nEND\n", o);
    CHECK(codes(r) == "");
    CHECK(sources(r) == "t.c:3");
}

TEST_CASE("line markers in debug information") {
    auto r = test::run(program("# 10 \"main.c\"\n NOP\n# 0\n NOP\n"));
    const std::string info = format_debug_info(r);
    CHECK(info.find("\"source\": {\"file\": 1, \"line\": 10}") != std::string::npos);
    CHECK(info.find("\"main.c\"") != std::string::npos);
}
