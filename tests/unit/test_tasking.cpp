// SPDX-License-Identifier: MIT
// The TASKING dialect: the m166 subset, TASKING controls and syntax.
#include "helpers.hpp"

#include <doctest.h>

#include <map>

using oc166::AssembleOptions;
using oc166::AssemblyResult;

namespace {

struct Tasking {
    std::map<std::string, std::string> files;
    std::vector<std::string> controls;
    bool keil = false;

    AssemblyResult run(std::string_view source) {
        AssembleOptions o;
        o.file_name = "main.src";
        o.cpu = oc166::isa::Cpu::C167;
        o.dialect = keil ? oc166::Dialect::Keil : oc166::Dialect::Tasking;
        o.controls = controls;
        o.read_file = [this](const std::string& path) -> std::optional<std::string> {
            auto it = files.find(path);
            if (it == files.end()) return std::nullopt;
            return it->second;
        };
        return oc166::assemble(source, o);
    }
};

// Code bytes and diagnostic codes of a listing line, like test::one().
std::string at(const AssemblyResult& r, int line) {
    for (const auto& l : r.listing) {
        if (l.line != line) continue;
        std::string s = test::hex(l.bytes);
        for (auto i : l.diagnostics) {
            const auto& d = r.diagnostics[i];
            s += (s.empty() ? "" : " ") + d.code + (d.severity == oc166::Severity::Error ? "E" : "W");
        }
        return s;
    }
    return "?";
}

// All diagnostic codes, e.g. "OC32W A106E".
std::string codes(const AssemblyResult& r) {
    std::string s;
    for (const auto& d : r.diagnostics)
        s += (s.empty() ? "" : " ") + d.code + (d.severity == oc166::Severity::Error ? "E" : "W");
    return s;
}

// Code bytes and diagnostic codes of the line with this source text, for
// sources whose line numbers include those of STDNAMES files.
std::string of(const AssemblyResult& r, std::string_view source) {
    for (const auto& l : r.listing)
        if (l.source == source) return at(r, l.line);
    return "?";
}

std::string all_bytes(const AssemblyResult& r) {
    std::string s;
    for (const auto& l : r.listing) s += test::hex(l.bytes);
    return s;
}

// A source with 'body' inside a NEAR procedure of a code section at 0C000H;
// the body starts on line 3.
std::string near(std::string_view body) {
    return "S SECTION CODE AT 0C000H\nP PROC NEAR\n" + std::string(body) + "\nP ENDP\nS ENDS\nEND\n";
}

}  // namespace

TEST_CASE("m166 @SET, macro variables and @EVAL") {
    Tasking t;
    auto r = t.run("@SET(N, 3)\n" + near(" MOV R1,#@N\n MOV R2,#@EVAL(@N*2)\n MOV R3,#@EVAL(@N SHL 2)"));
    CHECK(codes(r) == "");
    CHECK(at(r, 4) == "E031");
    CHECK(at(r, 5) == "E062");
    CHECK(at(r, 6) == "E0C3");
}

TEST_CASE("m166 @IF, @ELSE and @ENDI keep or drop text") {
    Tasking t;
    const std::string body = " @IF(@K)\n MOV R1,#1\n @ELSE\n MOV R1,#2\n @ENDI\n NOP";
    auto on = t.run("@SET(K, 1)\n" + near(body));
    auto off = t.run("@SET(K, 0)\n" + near(body));
    CHECK(codes(on) == "");
    CHECK(codes(off) == "");
    CHECK(all_bytes(on) == "E011CC00");
    CHECK(all_bytes(off) == "E021CC00");
    // Inside a line too: m166 works on characters.
    auto inline_if = t.run("@SET(K, 0)\n" + near(" MOV R1,#@IF(@K)1@ELSE 2@ENDI"));
    CHECK(at(inline_if, 4) == "E021");
}

TEST_CASE("m166 @DEFINED and the DEFINE control") {
    Tasking t;
    const std::string body = " @IF(@DEFINED(V))\n MOV R1,#@V\n @ELSE\n NOP\n @ENDI";
    CHECK(all_bytes(t.run(near(body))) == "CC00");
    t.controls = {"DEFINE(V,5)"};
    auto r = t.run(near(body));
    CHECK(codes(r) == "");
    CHECK(all_bytes(r) == "E051");
    // Without text the macro is 1.
    t.controls = {"DEF(V)"};
    CHECK(all_bytes(t.run(near(body))) == "E011");
}

TEST_CASE("m166 @\" comments are removed") {
    Tasking t;
    // A comment ends at the next quote, or takes the end of the line with
    // it, so the next line joins this one.
    auto r = t.run(near(" MOV R1,#@\"a comment\"4\nL1: @\" to the end of the line\n NOP"));
    CHECK(codes(r) == "");
    CHECK(at(r, 3) == "E041");
    CHECK(all_bytes(r) == "E041CC00");
    bool found = false;
    for (const auto& sym : r.symbols) found |= sym.name == "L1" && sym.value.num == 0xC002;
    CHECK(found);
}

TEST_CASE("m166 errors") {
    Tasking t;
    // Functions outside the subset.
    CHECK(at(t.run(near(" MOV R1,#@LEN(abc)")), 3).rfind("OC29E", 0) == 0);
    // An undefined macro, a stray @ENDI and an @IF that is never closed.
    CHECK(at(t.run(near(" MOV R1,#@NOSUCH")), 3).find("OC30E") != std::string::npos);
    CHECK(at(t.run(near(" @ENDI")), 3) == "OC30E");
    CHECK(codes(t.run("@IF(1)\n" + near(" NOP"))) == "OC30E");
}

TEST_CASE("TASKING controls on the command line and in the source") {
    Tasking t;
    // Abbreviations and controls with no effect on the code.
    t.controls = {"EX", "CASE", "DEBUG", "SYMB", "NOMOD166", "CHECKC166SV1DIV", "SN(regs.def)"};
    t.files["regs.def"] = "MYREG DEFR 0FE40H\n";
    // TASKING gives E 341 and Keil A77 for an SFR as a memory operand in
    // segmented mode without a DPP for the system page.
    auto r = t.run("$SEGMENTED\nASSUME DPP3:SYSTEM\n" + near(" MOV MYREG,R1"));
    CHECK(codes(r) == "");
    CHECK(of(r, " MOV MYREG,R1") == "F6F140FE");
    // An unknown control is reported against the command line.
    t.controls = {"NOSUCHCONTROL"};
    r = t.run(near(" NOP"));
    REQUIRE(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == "A16");
    CHECK(r.diagnostics[0].file == "<command line>");
    // EXTEND1 and the like fall back to the C167.
    t.controls = {"EXTEND2"};
    CHECK(codes(t.run(near(" NOP"))) == "OC2W");
    t.controls = {"NOOPTIMIZE"};
    CHECK(codes(t.run(near(" NOP"))) == "OC31W");
}

TEST_CASE("STDNAMES") {
    Tasking t;
    SUBCASE("a missing file falls back to the built-in C167 names") {
        t.controls = {"STDNAMES(reg167.def)"};
        auto r = t.run(near(" MOV ADCON,R1"));
        CHECK(codes(r) == "OC32W");
        CHECK(of(r, " MOV ADCON,R1") == "F6F1A0FF");
    }
    SUBCASE("the same file twice is read once") {
        t.files["r.def"] = "MYREG DEFR 0FE40H\n";
        t.controls = {"STDNAMES(r.def)"};
        auto r = t.run("$STDNAMES(r.def)\n" + near(" MOV MYREG,R1"));
        CHECK(codes(r) == "");
    }
    SUBCASE("files included from it are part of it") {
        // So primary controls may still follow.
        t.files["r.def"] = "$INCLUDE(more.def)\nRSTCON DEFA 0F1E0H\n";
        t.files["more.def"] = "MYREG DEFR 0FE40H\n";
        t.controls = {"STDNAMES(r.def)"};
        auto r = t.run("$SEGMENTED\nASSUME DPP3:SYSTEM\n" + near(" MOV MYREG,R1\n MOV RSTCON,R8"));
        CHECK(codes(r) == "");
        CHECK(of(r, " MOV MYREG,R1") == "F6F140FE");
        CHECK(of(r, " MOV RSTCON,R8") == "F6F8E0F1");
    }
}

TEST_CASE("TASKING DEFR, DEFA, DEFX, DEFB, DEFBF and DEFVAL") {
    Tasking t;
    // Fields for TASKING's other tools follow the address and are ignored.
    auto r = t.run(
        "R1X DEFR 0FF40H, RW, 0, 0000H, \"a register\"\n"
        "A1 DEFA 0C000H\n"
        "A2 DEFA 0F1E0H,R\n"
        "X1 DEFX 012346H\n"
        "B1 DEFB R1X.3, RW, \"a bit\"\n"
        "F1 DEFBF R1X,0,3\n"
        "DEFVAL R1X,0,3,5,\"five\"\n" +
        near(" MOV R1X,R1\n MOV R1,A2\n BSET B1"));
    CHECK(codes(r) == "");
    CHECK(at(r, 10) == "F6F140FF");
    CHECK(at(r, 11) == "F2F1E0F1");
    CHECK(at(r, 12) == "3FA0");
    // DEFA: the system page outside the PEC pointers.
    CHECK(codes(t.run("A DEFA 0BFFEH\n" + near(" NOP"))) == "A102E");
    CHECK(codes(t.run("A DEFA 0FCE0H\n" + near(" NOP"))) == "A102E");
    CHECK(codes(t.run("A DEFA 0C001H\n" + near(" NOP"))) == "A102E");
    // Keil's DEFA is limited to FA00H..FDFEH and has no extra fields.
    t.keil = true;
    CHECK(codes(t.run("$MOD167\nA DEFA 0F1E0H\n" + near(" NOP"))) == "A102E");
    CHECK(codes(t.run("$MOD167\nR1X DEFR 0FE40H,RW,0\n" + near(" NOP"))) == "A6E");
    CHECK(codes(t.run("$MOD167\nX1 DEFX 012346H\n" + near(" NOP"))).find("A6E") != std::string::npos);
}

TEST_CASE("TASKING procedures and returns") {
    Tasking t;
    // RETN is always the near return, RETV makes no code.
    auto r = t.run("$SEGMENTED\nS SECTION CODE AT 10000H\nP PROC FAR\n RETN\n RETV\n RET\nP ENDP\nS ENDS\nEND\n");
    CHECK(codes(r) == "");
    CHECK(at(r, 4) == "CB00");
    CHECK(at(r, 5) == "");
    CHECK(at(r, 6) == "DB00");
    CHECK(at(t.run(near(" RETV R1")), 3) == "A73E");
    // PROC without a type is FAR in segmented mode, NEAR otherwise.
    r = t.run("$SEGMENTED\nS SECTION CODE AT 10000H\nP PROC\n RET\nP ENDP\nS ENDS\nEND\n");
    CHECK(at(r, 4) == "DB00");
    r = t.run("S SECTION CODE AT 0C000H\nP PROC\n RET\nP ENDP\nS ENDS\nEND\n");
    CHECK(at(r, 3) == "CB00");
    // The end of the section ends a procedure without ENDP; Keil gives A106.
    const char* open = "$SEGMENTED\nS SECTION CODE AT 10000H\nP PROC FAR\n RETS\nS ENDS\nEND\n";
    CHECK(codes(t.run(open)) == "");
    t.keil = true;
    CHECK(codes(t.run(std::string("$MOD167\n") + open)) == "A106E");
}

TEST_CASE("TASKING label types and generic jumps") {
    Tasking t;
    // In a FAR procedure, labels are FAR. A generic JMP to one in the same
    // section is JMPR when close and JMPA when not, never JMPS; a CALL is
    // CALLS.
    auto r = t.run(
        "$SEGMENTED\n"
        "S SECTION CODE AT 10000H\n"
        "P PROC FAR\n"
        "L1: NOP\n"
        " JMP L1\n"
        " DS 400\n"
        " JMP L1\n"
        " CALL L1\n"
        " RETS\n"
        "P ENDP\n"
        "S ENDS\n"
        "END\n");
    CHECK(codes(r) == "");
    CHECK(at(r, 5) == "0DFE");
    CHECK(at(r, 7) == "EA000000");
    CHECK(at(r, 8) == "DA010000");
    // A typed label on a line of its own.
    r = t.run(
        "$SEGMENTED\n"
        "S SECTION CODE AT 10000H\n"
        "P PROC FAR\n"
        "N1: NEAR\n"
        " NOP\n"
        " CALL N1\n"
        " RETS\n"
        "P ENDP\n"
        "S ENDS\n"
        "END\n");
    CHECK(codes(r) == "");
    CHECK(at(r, 6) == "BBFE");
}

TEST_CASE("TASKING register banks and # lines") {
    Tasking t;
    auto r = t.run(
        "# 1 \"main.c\"\n"
        "B1 REGDEF R0-R7 PRIVATE\n"
        "B2 REGDEF R0-R3 COMMON = SHARED\n"
        "B3 REGDEF\n"
        "SHARED COMREG R0-R3\n" +
        near(" NOP"));
    CHECK(codes(r) == "");
    // Keil has no COMREG. It rejects '#' lines too, with an error code that
    // depends on the rest of the line (A1, A64); openc166 gives A6.
    t.keil = true;
    CHECK(codes(t.run("$MOD167\nSHARED COMREG R0-R3\n" + near(" NOP"))) == "A6E");
    CHECK(codes(t.run("$MOD167\n# 1 \"main.c\"\n" + near(" NOP"))) == "A6E");
}
