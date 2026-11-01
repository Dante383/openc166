// SPDX-License-Identifier: MIT
// Generic JMP and CALL: form selection, label types and jump shortening.
// Expected values were checked against Keil A166.
#include "doctest.h"
#include "helpers.hpp"

using namespace oc166;
using test::code;
using test::diags;
using test::one;
using test::run;

namespace {

std::string program(std::string_view body) {
    std::string s = "$MOD167\nS SECTION CODE AT 0C000H\n";
    s += body;
    s += "S ENDS\n END\n";
    return s;
}

}  // namespace

TEST_CASE("near targets in range use the relative form") {
    auto r = run(program("P PROC NEAR\n"
                         "B0: NOP\n"            // 4  C000
                         " JMP B0\n"            // 5
                         " CALL B0\n"           // 6
                         " JMP cc_Z,B0\n"       // 7
                         " JMP cc_UC,B0\n"      // 8
                         " CALL cc_Z,B0\n"      // 9  CALLR has no condition
                         " CALL cc_UC,B0\n"     // 10
                         "P ENDP\n"));
    REQUIRE(r.ok());
    CHECK(code(r, 5) == "0DFE");
    CHECK(code(r, 6) == "BBFD");
    CHECK(code(r, 7) == "2DFC");
    CHECK(code(r, 8) == "0DFB");
    CHECK(code(r, 9) == "CA2000C0");
    CHECK(code(r, 10) == "BBF8");
}

TEST_CASE("numbers, indirect targets and invalid targets") {
    CHECK(one("JMP 0D000H") == "EA0000D0");
    CHECK(one("CALL 0D000H") == "CA0000D0");
    CHECK(one("JMP 12345H") == "EA004523 A134W");
    CHECK(one("JMP FAR PTR 1234H") == "EA003412");
    CHECK(one("JMP [R1]") == "9C01");
    CHECK(one("JMP cc_Z,[R1]") == "9C21");
    CHECK(one("CALL [R1]") == "AB01");
    CHECK(one("CALL cc_NZ,[R1]") == "AB31");
    CHECK(one("JMP R1") == "A74E");
    CHECK(one("JMP #1") == "A74E");
    CHECK(one("JMP PSW") == "A74E");
}

TEST_CASE("far targets") {
    auto r = run(program("P PROC NEAR\n"
                         " JMP Q\n"             // 4  C000
                         " CALL Q\n"            // 5
                         " JMP cc_Z,Q\n"        // 6
                         " CALL cc_UC,Q\n"      // 7
                         " JMP SHORT Q\n"       // 8
                         " JMP FAR PTR L\n"     // 9
                         " JMP NEAR PTR Q\n"    // 10
                         " CALLS SEG L,L\n"     // 11
                         "L: RET\n"             // 12 C01C
                         "P ENDP\n"
                         "Q PROC FAR\n"         // 14 C01E
                         " RET\n"
                         "Q ENDP\n"));
    REQUIRE(r.ok());
    CHECK(code(r, 4) == "FA001EC0");
    CHECK(code(r, 5) == "DA001EC0");
    CHECK(code(r, 6) == "EA201EC0");
    CHECK(diags(r, 6) == "A84W");
    CHECK(code(r, 7) == "CA001EC0");
    CHECK(diags(r, 7) == "A84W");
    CHECK(code(r, 8) == "0D06");
    CHECK(diags(r, 8).empty());
    CHECK(code(r, 9) == "FA001CC0");
    CHECK(code(r, 10) == "0D03");
    CHECK(code(r, 11) == "DA001CC0");
    CHECK(diags(r, 11).empty());
}

TEST_CASE("branches to data labels warn") {
    auto r = run(program("P PROC NEAR\n"
                         " JMP LW\n"                // 4  C000
                         " JMPR cc_UC,LB\n"         // 5
                         " CALL LD\n"               // 6
                         " JMP WORD PTR P\n"        // 7
                         " JMP LW+0\n"              // 8  "label + 0" keeps the type
                         " CALL cc_Z,SHORT P\n"     // 9
                         " JMP SHORT cc_Z,P\n"      // 10
                         " RET\n"
                         "P ENDP\n"
                         "LW LABEL WORD\n"
                         "LB LABEL BYTE\n"
                         "LD LABEL DWORD\n"));      // 15
    CHECK(code(r, 4) == "0D07");
    CHECK(diags(r, 4) == "A74W");
    CHECK(code(r, 5) == "0D06");
    CHECK(diags(r, 5) == "A74W");
    CHECK(code(r, 6) == "BB05");
    CHECK(diags(r, 6) == "A74W");
    CHECK(code(r, 7) == "0DFC");
    CHECK(diags(r, 7) == "A74W");
    CHECK(code(r, 8) == "0D03");
    CHECK(diags(r, 8) == "A74W");
    CHECK(code(r, 9) == "CA2000C0");
    CHECK(diags(r, 10) == "A73E A6E");
    // Misaligned in the first pass, before the forward jumps were shortened.
    CHECK(diags(r, 15) == "A48W");
}

TEST_CASE("out of range relative jumps") {
    auto r = run(program("P PROC NEAR\n"
                         " JMP SHORT F\n"   // 4
                         " JMPR cc_UC,F\n"  // 5
                         " DS 300\n"
                         "F: RET\n"
                         "P ENDP\n"));
    CHECK(diags(r, 4) == "A82E");
    CHECK(diags(r, 5) == "A82E");
}

TEST_CASE("forward jumps are shortened when they fit") {
    // Each of the four jumps to X only fits once the jumps after it are short.
    auto r = run(program("P PROC NEAR\n"
                         " JMP T\n"          // 4  C000
                         " JMP X\n"          // 5
                         " JMP X\n"
                         " JMP X\n"
                         " JMP X\n"          // 8
                         "X: DS 244\n"
                         "T: NOP\n"          // 10 C0FE
                         "P ENDP\n"));
    REQUIRE(r.ok());
    CHECK(code(r, 4) == "0D7E");
    CHECK(code(r, 5) == "0D03");
    CHECK(code(r, 8) == "0D00");
    CHECK(diags(r, 5) == "A74W");  // X labels data, so it is a BYTE label
}

TEST_CASE("jumps that could be short or long stay long") {
    auto r = run(program("P PROC NEAR\n"
                         "A: JMP B\n"     // 4  C000
                         " DS 252\n"
                         " JMP A\n"       // 6  C100
                         "B: NOP\n"       // 7  C104
                         "P ENDP\n"));
    REQUIRE(r.ok());
    CHECK(code(r, 4) == "EA0004C1");
    CHECK(code(r, 6) == "EA0000C0");
}

TEST_CASE("relative range limits") {
    // A forward jump is measured with the target address of the previous
    // pass, so one that would reach exactly +254 bytes once short stays long.
    auto r = run(program("P PROC NEAR\n"
                         " JMP T1\n"      // 4  C000
                         " DS 254\n"
                         "T1: JMP T2\n"   // 6  C102
                         " DS 256\n"
                         "T2: NOP\n"      // 8  C206
                         " NOP\n"
                         "N1: NOP\n"      // 10 C20A
                         " DS 252\n"
                         " JMP N1\n"      // 12 C308  -256 bytes: fits
                         "N2: NOP\n"      // 13 C30A
                         " DS 254\n"
                         " JMP N2\n"      // 15 C40A  -258 bytes: too far
                         "P ENDP\n"));
    REQUIRE(r.ok());
    CHECK(code(r, 4) == "EA0002C1");
    CHECK(code(r, 6) == "EA0006C2");
    CHECK(code(r, 12) == "0D80");
    CHECK(code(r, 15) == "EA000AC3");
}

TEST_CASE("label plus zero is still the label") {
    auto r = run(program("P PROC NEAR\n"
                         " JMP LW-0\n"     // 4
                         " JMP 0+LW\n"     // 5
                         " JMP Q+0\n"      // 6
                         " JMP LW+(2-2)\n" // 7
                         " RET\n"
                         "P ENDP\n"
                         "Q PROC FAR\n"    // 10 C00C
                         "LW LABEL WORD\n"
                         " RET\n"
                         "Q ENDP\n"));
    REQUIRE(r.ok());
    CHECK(diags(r, 4) == "A74W");
    CHECK(diags(r, 5) == "A74W");
    CHECK(code(r, 6) == "FA000CC0");
    CHECK(diags(r, 7) == "A74W");
}
