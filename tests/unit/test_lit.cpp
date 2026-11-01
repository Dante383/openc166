// SPDX-License-Identifier: MIT
// LIT text replacement. Expected values were checked against Keil A166.
#include "doctest.h"
#include "helpers.hpp"

#include <map>

using namespace oc166;
using test::code;
using test::diags;
using test::run;

namespace {

// Definitions on lines 2..; the code section starts after them at 0C000H.
std::string program(std::string_view defs, std::string_view code, std::string_view mod = "$MOD167\n") {
    std::string s(mod);
    s += defs;
    s += "S SECTION CODE AT 0C000H\nP PROC NEAR\n";
    s += code;
    s += "P ENDP\nS ENDS\n END\n";
    return s;
}

std::string source(const AssemblyResult& r, int line) {
    for (const auto& l : r.listing)
        if (l.line == line) return l.source;
    return "?";
}

const Symbol* symbol(const AssemblyResult& r, std::string_view name) {
    for (const auto& s : r.symbols)
        if (s.name == name) return &s;
    return nullptr;
}

}  // namespace

TEST_CASE("LIT replaces a name by its text") {
    auto r = run(program("U1 LIT 'R5'\n"          // 2
                         "U2 LIT \"R6\"\n"        // 3
                         "EX LIT '1+2'\n"         // 4
                         "MYMOV LIT 'MOV'\n"      // 5
                         "NESTED LIT 'U1'\n"      // 6
                         "PAIR LIT 'R1, R2'\n"    // 7
                         "EMPTY LIT ''\n"         // 8
                         "VAL EQU EX*2\n"         // 9
                         "PORT LIT 'R2'\n"        // 10
                         "ADR LIT '0FA00H'\n"     // 11
                         "QS LIT \"'AB'\"\n"      // 12
                         "SP1 LIT '  R3  '\n",    // 13
                         " MOV U1,#1\n"           // 16
                         " MOV U2,u1\n"           // 17 names ignore case
                         " MOV R1,#EX*2\n"        // 18 plain text: 1+2*2
                         " MYMOV R1,R2\n"         // 19
                         " MOV NESTED,#3\n"       // 20
                         " ADD PAIR\n"            // 21
                         " MOV R1,#VAL\n"         // 22
                         " DB 'U1'EMPTY\n"        // 23 not inside strings
                         " BSET PORT.3\n"         // 24
                         " MOV R1,ADR\n"          // 25
                         " DB QS\n"               // 26
                         " MOV SP1,#1\n"));       // 27
    REQUIRE(r.ok());
    CHECK(code(r, 16) == "E015");
    CHECK(code(r, 17) == "F065");
    CHECK(code(r, 18) == "E051");
    CHECK(code(r, 19) == "F012");
    CHECK(code(r, 20) == "E035");
    CHECK(code(r, 21) == "0012");
    CHECK(code(r, 22) == "E051");
    CHECK(code(r, 23) == "5531");
    CHECK(code(r, 24) == "3FF2");
    CHECK(code(r, 25) == "F2F100FA");
    CHECK(code(r, 26) == "4142");
    CHECK(code(r, 27) == "E013");
    // The listing shows the line after replacement, spacing included.
    CHECK(source(r, 9) == "VAL EQU 1+2*2");
    CHECK(source(r, 23) == " DB 'U1'");
    CHECK(source(r, 27) == " MOV   R3  ,#1");
    CHECK(source(r, 2) == "U1 LIT 'R5'");

    const Symbol* s = symbol(r, "NESTED");
    REQUIRE(s);
    CHECK(s->kind == SymbolKind::Lit);
    CHECK(s->text == "U1");
    REQUIRE(symbol(r, "EMPTY"));
    CHECK(symbol(r, "EMPTY")->text.empty());
    CHECK(format_listing(r).find("QS                               LIT    \"'AB'\"") != std::string::npos);
}

TEST_CASE("LIT replaces names in the name field, labels and controls") {
    auto r = run(program("NM LIT 'ABC'\n"        // 2
                         "NM EQU 5\n"            // 3 defines ABC
                         "DIR LIT 'EQU'\n"       // 4
                         "XYZ DIR 9\n"           // 5
                         "LBL LIT 'START'\n"     // 6
                         "FLAG LIT '1'\n"        // 7
                         "QT LIT 'IT''S'\n"      // 8
                         "$IF FLAG\n"            // 9
                         "IN1 EQU 1\n"           // 10
                         "$ENDIF\n"              // 11
                         " IF FLAG\n"            // 12
                         "IN2 EQU 2\n"           // 13
                         " ENDIF\n",             // 14
                         "LBL: NOP\n"            // 17
                         " MOV R2,#ABC\n"        // 18
                         " MOV R2,#XYZ\n"        // 19
                         " JMP START\n"          // 20
                         " MOV R1,#IN1+IN2\n"    // 21
                         " DB 'QT'\n"));         // 22
    REQUIRE(r.ok());
    CHECK(source(r, 3) == "ABC EQU 5");
    CHECK(source(r, 9) == "$IF 1");
    CHECK(source(r, 12) == " IF 1");
    CHECK(code(r, 17) == "CC00");
    CHECK(code(r, 18) == "E052");
    CHECK(code(r, 19) == "E092");
    CHECK(code(r, 20) == "0DFC");
    CHECK(code(r, 21) == "E031");
    CHECK(code(r, 22) == "5154");
    REQUIRE(symbol(r, "QT"));
    CHECK(symbol(r, "QT")->text == "IT'S");
    CHECK(!symbol(r, "NM")->text.empty());
}

TEST_CASE("$INCLUDE uses its file name as written") {
    std::map<std::string, std::string> files{{"INCN", "VAL EQU 7\nIR LIT 'R4'\n"}};
    AssembleOptions o;
    o.file_name = "main.a66";
    o.cpu = isa::Cpu::C167;
    o.read_file = [&](const std::string& path) -> std::optional<std::string> {
        auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return it->second;
    };
    auto r = assemble(program("INC LIT 'other.inc'\n"  // 2
                              "$INCLUDE (INCN)\n",     // 3, then 4 and 5
                              " MOV IR,#VAL\n"),       // 8
                      o);
    REQUIRE(r.ok());
    CHECK(source(r, 3) == "$INCLUDE (INCN)");
    CHECK(code(r, 8) == "E074");

    r = assemble(program("INCN LIT 'other.inc'\n"  // 2
                         "$INCLUDE (INCN)\n",      // 3, then 4 and 5
                         " MOV IR,#VAL\n"),        // 8
                 o);
    REQUIRE(r.ok());
    CHECK(source(r, 3) == "$INCLUDE (other.inc)");
    CHECK(code(r, 8) == "E074");
}

TEST_CASE("LIT names are case-sensitive under $CASE") {
    auto r = run(program("Lo LIT 'R3'\nLO EQU 4\n", " MOV Lo,#1\n MOV R1,#LO\n", "$CASE\n$MOD167\n"));
    REQUIRE(r.ok());
    CHECK(code(r, 7) == "E013");
    CHECK(code(r, 8) == "E041");
}

TEST_CASE("a ';' in LIT text starts a comment") {
    auto r = run(program("Q LIT 'R1 ; tail'\n", " MOV Q,#2\n"));
    CHECK(source(r, 5) == " MOV R1 ; tail,#2");
    CHECK(diags(r, 5) == "A73E");
}

TEST_CASE("LIT lines that are not assembled") {
    auto r = run(program(" IF 0\n"              // 2
                         "SKIP LIT 'R4'\n"      // 3
                         " ENDIF\n",            // 4
                         " MOV R1,#SKIP\n"));   // 7
    CHECK(diags(r, 7) == "A42E");
    CHECK(!symbol(r, "SKIP"));
}

TEST_CASE("LIT errors") {
    auto r = run(program("U1 LIT 'QQ'\n"         // 2
                         "U1 LIT 'ZZ'\n"         // 3 the name is replaced too: defines QQ
                         "R1X LIT 'R5'\n"        // 4
                         "R1X LIT 'R5'\n"        // 5 R5 LIT 'R5': R5 is a register
                         "E1 EQU 3\n"            // 6
                         "E1 LIT 'R1'\n"         // 7
                         "NOQ LIT R5\n"          // 8
                         "NOARG LIT\n"           // 9
                         "TWO LIT 'A', 'B'\n"    // 10 keeps 'A'
                         "A1 LIT 'B1'\n"         // 11
                         "B1 LIT 'A1'\n"         // 12
                         "SELF LIT 'SELF'\n"     // 13
                         "X1: LT2 LIT 'R2'\n",   // 14
                         " MOV R1,#FWD\n"        // 17 LIT defined later
                         " MOV R1,#A1\n"         // 18 A1 -> B1 -> A1
                         " MOV R1,#SELF\n"       // 19
                         " MOV R1,LT2\n"         // 20
                         " MOV R1,#QQ\n"         // 21
                         " MOV R1,#E1\n"         // 22
                         "FWD LIT '4'\n"));      // 23
    for (int line : {2, 3, 4, 6, 11, 12, 13, 23}) CHECK(diags(r, line).empty());
    for (int line : {5, 7}) CHECK(diags(r, line) == "A25E");
    for (int line : {8, 9, 10, 14}) CHECK(diags(r, line) == "A6E");
    for (int line : {17, 18, 19}) CHECK(diags(r, line) == "A60E");
    CHECK(diags(r, 20) == "A42E");
    CHECK(source(r, 18) == " MOV R1,#A1");
    CHECK(source(r, 21) == " MOV R1,#ZZ");
    CHECK(diags(r, 21) == "A42E");
    CHECK(code(r, 22) == "E031");
    REQUIRE(symbol(r, "QQ"));
    CHECK(symbol(r, "QQ")->text == "ZZ");
    CHECK(symbol(r, "U1")->text == "QQ");
    CHECK(symbol(r, "TWO")->text == "A");
    CHECK(!symbol(r, "X1"));
    CHECK(!symbol(r, "LT2"));
    // A failed instruction keeps its size from the earlier passes.
    CHECK(r.passes <= 3);
}

TEST_CASE("a label cannot come before 'name directive'") {
    auto r = run(program("X1: ABC EQU 5\n"       // 2
                         "X2: SR1 DEFR 0FE00H\n" // 3
                         "X3: ST1 SET 3\n"       // 4
                         "X5: TB DB 1\n",        // 5
                         "X4: P2 PROC\n"         // 8
                         "Z1: K1 EQU 1\n"        // 9
                         "Z2: N2 DW 1\n"         // 10
                         " NOP\n"));             // 11
    // Outside a section: one error, and nothing is defined.
    for (int line : {2, 3, 4, 5}) CHECK(diags(r, line) == "A6E");
    for (const char* name : {"X1", "X2", "X3", "X5", "ABC", "SR1", "ST1", "TB"}) CHECK(!symbol(r, name));
    // Inside one: the label is defined, the rest of the line is ignored.
    for (int line : {8, 9, 10}) CHECK(diags(r, line) == "A6E A6E");
    for (const char* name : {"P2", "K1", "N2"}) CHECK(!symbol(r, name));
    for (const char* name : {"X4", "Z1", "Z2"}) {
        REQUIRE(symbol(r, name));
        CHECK(symbol(r, name)->value.num == 0xC000);
        CHECK(symbol(r, name)->value.ltype == LType::Near);
    }
    CHECK(code(r, 11) == "CC00");

    r = run("D1 SECTION DATA AT 0F000H\nY1: M1 EQU 1\nY2: M2 DW 1\nD1 ENDS\n END\n");
    for (int line : {2, 3}) CHECK(diags(r, line) == "A6E A6E");
    for (const char* name : {"Y1", "Y2"}) {
        REQUIRE(symbol(r, name));
        CHECK(symbol(r, name)->value.num == 0xF000);
        CHECK(symbol(r, name)->value.ltype == LType::Byte);
    }
}

TEST_CASE("LIT text that grows without end is stopped") {
    std::string defs;
    for (int i = 0; i < 20; ++i) defs += "L" + std::to_string(i) + " LIT 'L" + std::to_string(i + 1) + " L" +
                                         std::to_string(i + 1) + "'\n";
    auto r = run(program(defs, " DW L0\n"));
    CHECK(diags(r, 24) == "OC11E");
}
