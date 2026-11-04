// SPDX-License-Identifier: MIT
// $INCLUDE, $INCDIR and the placement of primary controls.
#include "helpers.hpp"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <map>

using oc166::AssembleOptions;
using oc166::AssemblyResult;

namespace {

struct Files {
    std::map<std::string, std::string> files;
    std::vector<std::string> requests;  // paths asked for, in order

    AssembleOptions options(std::string main_name = "main.a66") {
        AssembleOptions o;
        o.file_name = std::move(main_name);
        o.cpu = oc166::isa::Cpu::C167;
        o.read_file = [this](const std::string& path) -> std::optional<std::string> {
            requests.push_back(path);
            auto it = files.find(path);
            if (it == files.end()) return std::nullopt;
            return it->second;
        };
        return o;
    }
};

const oc166::ListingLine* listing_line(const AssemblyResult& r, int line) {
    for (const auto& l : r.listing)
        if (l.line == line) return &l;
    return nullptr;
}

// Code bytes and diagnostic codes of a listing line, like test::one().
std::string at(const AssemblyResult& r, int line) {
    const auto* l = listing_line(r, line);
    if (!l) return "?";
    std::string s = test::hex(l->bytes);
    for (auto i : l->diagnostics) {
        const auto& d = r.diagnostics[i];
        s += (s.empty() ? "" : " ") + d.code + (d.severity == oc166::Severity::Error ? "E" : "W");
    }
    return s;
}

std::optional<std::int64_t> symbol(const AssemblyResult& r, const std::string& name) {
    for (const auto& s : r.symbols)
        if (s.name == name) return s.value.num;
    return std::nullopt;
}

constexpr const char* kHead = "S SECTION CODE AT 0C000H\nP PROC NEAR\n";
constexpr const char* kTail = "P ENDP\nS ENDS\n END\n";

}  // namespace

TEST_CASE("included lines are numbered across files like Keil's listing") {
    Files f;
    f.files["a.inc"] = "; a\nVAL EQU 1234H\n MOV R2,#1\n";
    std::string src = std::string(kHead) + "$INCLUDE (a.inc)\n MOV R1,#VAL\n" + kTail;
    auto r = oc166::assemble(src, f.options());
    REQUIRE(r.ok());
    CHECK(at(r, 6) == "E012");
    CHECK(at(r, 7) == "E6F13412");
    const auto* inc = listing_line(r, 6);
    REQUIRE(inc);
    CHECK(inc->file == "a.inc");
    CHECK(inc->file_line == 3);
    CHECK(inc->level == 1);
    const auto* back = listing_line(r, 7);
    REQUIRE(back);
    CHECK(back->file == "main.a66");
    CHECK(back->file_line == 4);
    CHECK(back->level == 0);
    CHECK(r.listing.size() == 10);
}

TEST_CASE("diagnostics in include files name the file and its own line number") {
    Files f;
    f.files["a.inc"] = "\n BOGUS\n";
    auto r = oc166::assemble(std::string(kHead) + "$INCLUDE (a.inc)\n" + kTail, f.options());
    REQUIRE(r.error_count() == 1);
    const auto& d = r.diagnostics[0];
    CHECK(d.code == "A6");
    CHECK(d.file == "a.inc");
    CHECK(d.line == 2);
    CHECK(at(r, 5) == "A6E");
    std::string lst = oc166::format_listing(r);
    CHECK(lst.find("+1     5     BOGUS") != std::string::npos);
    CHECK(lst.find("(a.inc, line 2)") != std::string::npos);
}

TEST_CASE("include files nest and may be included repeatedly") {
    Files f;
    f.files["outer.inc"] = "CNT SET CNT+1\n$INCLUDE (inner.inc)\n";
    f.files["inner.inc"] = "CNT SET CNT+10\n";
    std::string src = std::string("CNT SET 0\n") + kHead + "$IC (outer.inc)\n DW CNT\n$IC (outer.inc)\n DW CNT\n" + kTail;
    auto r = oc166::assemble(src, f.options());
    REQUIRE(r.ok());
    CHECK(at(r, 8) == "0B00");
    CHECK(at(r, 13) == "1600");
    CHECK(listing_line(r, 7)->level == 2);
    // Each file is read only once, however often it is included.
    CHECK(f.requests.size() == 2);
}

TEST_CASE("include search order") {
    SUBCASE("current directory before the source directory") {
        Files f;
        f.files["a.inc"] = "A EQU 1\n";
        f.files["src/a.inc"] = "A EQU 2\n";
        auto r = oc166::assemble("$INCLUDE (a.inc)\n END\n", f.options("src/main.a66"));
        CHECK(symbol(r, "A") == 1);
    }
    SUBCASE("the directory of the main source, not of the including file") {
        Files f;
        f.files["src/sub/b.inc"] = "$INCLUDE (c.inc)\n";
        f.files["src/c.inc"] = "CV EQU 3\n";
        f.files["src/sub/c.inc"] = "CV EQU 4\n";
        auto r = oc166::assemble("$INCLUDE (sub/b.inc)\n END\n", f.options("src/main.a66"));
        CHECK(symbol(r, "CV") == 3);
    }
    SUBCASE("include_dirs before $INCDIR") {
        Files f;
        f.files["lib/a.inc"] = "A EQU 5\n";
        f.files["x/a.inc"] = "A EQU 6\n";
        auto o = f.options();
        o.include_dirs = {"lib"};
        auto r = oc166::assemble("$INCDIR (x)\n$INCLUDE (a.inc)\n END\n", o);
        CHECK(symbol(r, "A") == 5);
    }
    SUBCASE("$INCDIR takes a list and backslashes work everywhere") {
        Files f;
        f.files["y/z/a.inc"] = "A EQU 7\n";
        auto r = oc166::assemble("$INCDIR (x; y\\z)\n$INCLUDE (a.inc)\n END\n", f.options());
        CHECK(symbol(r, "A") == 7);
    }
    SUBCASE("a relative $INCDIR is also tried below the source directory") {
        Files f;
        f.files["src/x/a.inc"] = "A EQU 8\n";
        auto r = oc166::assemble("$ID (x)\n$INCLUDE (a.inc)\n END\n", f.options("src/main.a66"));
        CHECK(symbol(r, "A") == 8);
        CHECK(f.requests == std::vector<std::string>{"a.inc", "src/a.inc", "x/a.inc", "src/x/a.inc"});
    }
}

TEST_CASE("a missing include file ends the assembly") {
    Files f;
    auto r = oc166::assemble("$INCLUDE (none.inc)\nX EQU 1\n", f.options());
    REQUIRE(r.error_count() == 1);
    CHECK(r.diagnostics[0].code == "OC5");
    CHECK(r.warning_count() == 0);  // no "missing END"
    CHECK(r.listing.size() == 1);
}

TEST_CASE("REG167.INC is built in") {
    // C167 sources include a register file; Keil's own definitions agree with
    // these values.
    std::string src = std::string("$MOD167\n$INCLUDE (reg167.inc)\n") + kHead +
                      " MOV SYSCON3, R1\n BSET T3IR\n BCLR SSCEN\n JB T3EUD, P\n BSET S0PEN\n BSET IEN\n" + kTail;
    SUBCASE("used when no file of that name is found") {
        Files f;
        auto r = oc166::assemble(src, f.options());
        REQUIRE(r.ok());
        CHECK(r.warning_count() == 0);
        CHECK(listing_line(r, 3)->file == "<bundled>/REG167.INC");
        int n = static_cast<int>(r.listing.size());
        CHECK(at(r, n - 8) == "F6F1D4F1");
        CHECK(at(r, n - 7) == "7FB1");
        CHECK(at(r, n - 6) == "FED9");
        CHECK(at(r, n - 5) == "8AE2FA40");
        CHECK(at(r, n - 4) == "5FD8");
        CHECK(at(r, n - 3) == "BF88");
        CHECK(symbol(r, "ADDAT") == 0xFEA0);
        CHECK(symbol(r, "XP3IC") == 0xF19E);
    }
    SUBCASE("a file found on the search path comes first") {
        Files f;
        f.files["reg167.inc"] = "SYSCON3 DEFR 0F000H\nT3IR BIT 0FD00H.0\nSSCEN BIT 0FD00H.1\n"
                                "T3EUD BIT 0FD00H.2\nS0PEN BIT 0FD00H.3\n";
        auto r = oc166::assemble(src, f.options());
        REQUIRE(r.ok());
        CHECK(at(r, 10) == "F6F100F0");
        CHECK(!symbol(r, "ADDAT"));
    }
    SUBCASE("only for a bare file name") {
        Files f;
        auto r = oc166::assemble("$MOD167\n$INCLUDE (inc\\reg167.inc)\n END\n", f.options());
        REQUIRE(r.error_count() == 1);
        CHECK(r.diagnostics[0].code == "OC5");
    }
}

TEST_CASE("recursive includes are stopped") {
    Files f;
    f.files["self.inc"] = "$INCLUDE (self.inc)\n";
    auto r = oc166::assemble("$INCLUDE (self.inc)\n END\n", f.options());
    REQUIRE(r.error_count() == 1);
    CHECK(r.diagnostics[0].code == "OC6");
}

TEST_CASE("include syntax as accepted by Keil") {
    Files f;
    f.files["a.inc"] = " NOP\n";
    f.files["\"a.inc\""] = "";
    auto line = [&](const std::string& control) {
        auto r = oc166::assemble(std::string(kHead) + control + "\n" + kTail, f.options());
        return at(r, 3) + (at(r, 4) == "CC00" ? " included" : "");
    };
    CHECK(line("$INCLUDE (a.inc)") == " included");
    CHECK(line("$include(a.inc)") == " included");
    CHECK(line("$IC ( a.inc ) ; comment") == " included");
    CHECK(line("$INCLUDE a.inc") == "A22E A171E");
    CHECK(line("$INCLUDE (a.inc") == "A22E A171E");
    CHECK(line("$INCLUDE ()") == "A20E A171E");
    CHECK(line("$INCLUDE (a.inc) LIST") == "A171E");
    CHECK(line("$LIST INCLUDE (a.inc)") == "OC7W");
    // Quotes are part of the name.
    CHECK(line("$INCLUDE (\"a.inc\")").empty());
}

TEST_CASE("the last line of an include file without a newline joins the next line") {
    Files f;
    f.files["code.inc"] = " MOV R1,#1\n MOV R2,#2";
    f.files["comment.inc"] = "; comment without newline";
    std::string src = std::string(kHead) + "$INCLUDE (code.inc)\n\n$INCLUDE (comment.inc)\n RET\n RET\n" + kTail;
    auto r = oc166::assemble(src, f.options());
    REQUIRE(r.ok());
    CHECK(at(r, 4) == "E011");
    CHECK(at(r, 5) == "E022");  // joined to the empty line
    CHECK(listing_line(r, 5)->level == 0);
    CHECK(at(r, 7) == "OC8W");  // the RET became part of the comment
    CHECK(listing_line(r, 7)->source == "; comment without newline RET");
    CHECK(at(r, 8) == "CB00");
    CHECK(r.listing.size() == 11);
}

TEST_CASE("primary controls must come before the first statement") {
    auto run = [](const std::string& src) { return test::run(src, oc166::isa::Cpu::C166); };
    SUBCASE("a late $MOD167 is ignored") {
        auto r = run(std::string(kHead) + " NOP\n$MOD167\n EXTR #1\n" + kTail);
        CHECK(test::diags(r, 4) == "A8E");
        CHECK(test::diags(r, 5) == "A108E");
    }
    SUBCASE("the whole line is ignored") {
        auto r = run("X EQU 1\n$NOLIST MOD167\n$CASE\nabc EQU 1\nABC EQU 2\n END\n");
        CHECK(test::diags(r, 2) == "A8E");
        CHECK(test::diags(r, 3) == "A8E");
        CHECK(test::diags(r, 5) == "A25E");
    }
    SUBCASE("comments, blank lines, general controls and empty include files are not statements") {
        Files f;
        f.files["c.inc"] = "; only a comment\n\n";
        auto o = f.options();
        o.cpu = oc166::isa::Cpu::C166;
        auto r = oc166::assemble(std::string("; c\n\n$NOLIST\n$LIST\n$INCLUDE (c.inc)\n$MOD167\n") + kHead + " EXTR #1\n" + kTail, o);
        CHECK(r.ok());
        CHECK(r.cpu == oc166::isa::Cpu::C167);
    }
    SUBCASE("statements in include files are") {
        Files f;
        f.files["e.inc"] = "E EQU 1\n";
        auto r = oc166::assemble("$INCLUDE (e.inc)\n$MOD167\n END\n", f.options());
        CHECK(at(r, 3) == "A8E");
    }
    SUBCASE("a late $INCDIR is reported but still searched, as in Keil") {
        Files f;
        f.files["x/a.inc"] = "A EQU 1\n";
        auto r = oc166::assemble("X EQU 0\n$INCDIR (x)\n$INCLUDE (a.inc)\n END\n", f.options());
        CHECK(at(r, 2) == "A8E");
        CHECK(symbol(r, "A") == 1);
    }
}

TEST_CASE("include file names ignore case on the file system") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "openc166_test_include";
    fs::remove_all(dir);
    fs::create_directories(dir / "Inc");
    std::ofstream(dir / "Inc" / "Regs.INC") << "R EQU 42\n";
    AssembleOptions o;
    o.file_name = (dir / "main.a66").string();
    auto r = oc166::assemble("$INCLUDE (inc\\regs.inc)\n END\n", o);
    CHECK(r.ok());
    CHECK(symbol(r, "R") == 42);
    fs::remove_all(dir);
}
