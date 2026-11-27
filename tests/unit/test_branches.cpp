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

TEST_CASE("type operators bind tightly and the outermost one wins") {
    auto r = run(program("P PROC NEAR\n"
                         "B0: NOP\n"                    // 4  C000
                         " JMP SHORT FAR PTR B0\n"      // 5  C002
                         " JMP FAR PTR SHORT B0\n"      // 6  C004
                         " JMP (FAR B0)\n"              // 7  C008
                         " DW SOF FAR B0, FAR HIGH B0\n" // 8  C00C
                         " MOV R1,FAR R2\n"             // 9  C010
                         " DS 300\n"
                         " JMP NEAR SHORT B0\n"         // 11 C140
                         " JMP SHORT NEAR B0\n"         // 12 C144
                         " JMP FAR B0 + 2\n"            // 13 C146 a number in an AT section
                         " JMP FAR cc_Z,B0\n"           // 14
                         " JMP SHORT [R1]\n"            // 15
                         " RET\n"
                         "P ENDP\n"));
    CHECK(code(r, 5) == "0DFE");
    CHECK(code(r, 6) == "FA0000C0");
    CHECK(code(r, 7) == "FA0000C0");
    CHECK(code(r, 8) == "00C0C000");
    CHECK(code(r, 9) == "F2F1E4FF");
    CHECK(code(r, 11) == "EA0000C0");
    CHECK(diags(r, 12) == "A82E");
    CHECK(code(r, 13) == "EA0002C0");
    CHECK(diags(r, 14) == "A73E A6E");
    CHECK(diags(r, 15) == "A74E A6E");
}

TEST_CASE("label plus a number keeps its type in a relocatable section") {
    auto r = run("$MOD167\n"
                 "S SECTION CODE\n"
                 "P PROC NEAR\n"
                 "T: NOP\n"                // 4  0000
                 " JMP Q + 2\n"            // 5  0002
                 " CALL Q - 2\n"           // 6  0006
                 " JMP 2 + Q\n"            // 7  000A
                 " JMP cc_Z,Q + 2\n"       // 8  000E
                 " JMP LW + 2\n"           // 9  0012
                 " JMP FAR T + 2\n"        // 10 0014
                 " JMP SHORT T + 2\n"      // 11 0018
                 " RET\n"
                 "P ENDP\n"
                 "LW LABEL WORD\n"
                 "Q PROC FAR\n"            // 15 001C
                 " RETS\n"
                 "Q ENDP\n"
                 "S ENDS\n"
                 " END\n");
    REQUIRE(r.ok());
    CHECK(code(r, 5) == "FA001E00");
    CHECK(code(r, 6) == "DA001A00");
    CHECK(code(r, 7) == "FA001E00");
    CHECK(code(r, 8) == "EA201E00");
    CHECK(diags(r, 8) == "A84W");
    CHECK(code(r, 9) == "0D05");
    CHECK(diags(r, 9) == "A74W");
    CHECK(code(r, 10) == "FA000200");
    CHECK(code(r, 11) == "0DF4");
}

TEST_CASE("a type operator on a forward reference stays out of arithmetic") {
    auto r = run("$MOD167\n"
                 "S SECTION CODE AT 0C000H\n"
                 "P PROC NEAR\n"
                 "B: NOP\n"                    // 4
                 " JMP FAR F + 2\n"            // 5
                 " DW (NEAR F) + 2\n"          // 6
                 " DW F - FAR F\n"             // 7
                 " DW WORD F SHR 1\n"          // 8
                 " DW FAR F LT F\n"            // 9
                 " DW FAR F EQ B\n"            // 10
                 " DW FAR (F + 2)\n"           // 11
                 " DW FAR B + 2\n"             // 12
                 "F: RET\n"                    // 13 C012
                 "P ENDP\n"
                 "S ENDS\n"
                 " END\n");
    CHECK(diags(r, 5) == "A61E");
    CHECK(code(r, 6) == "14C0");
    CHECK(diags(r, 6) == "A61E");
    CHECK(diags(r, 7) == "A61E");
    CHECK(code(r, 8) == "0960");
    CHECK(diags(r, 8) == "A63E");
    CHECK(diags(r, 9).empty());
    CHECK(diags(r, 10) == "A63E");
    CHECK(diags(r, 11).empty());
    CHECK(diags(r, 12).empty());
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

// Keil shortens a forward jump when its target, where the previous pass put
// it, is in reach of where the jump is now. openc166 measures from where
// the jump was in the previous pass, which ends in the same code in a few
// passes instead of one pass per few hundred bytes of shortened jumps...
TEST_CASE("forward jumps in a long section settle in a few passes") {
    std::string src = "$MOD167\nCS SECTION CODE\nP PROC NEAR\n";
    for (int i = 0; i < 3000; ++i)
        src += "L" + std::to_string(i) + ": JMP cc_NZ, L" + std::to_string(i + 3) + "\n NOP\n";
    for (int i = 3000; i < 3003; ++i) src += "L" + std::to_string(i) + ": NOP\n";
    src += " RET\nP ENDP\nCS ENDS\n END\n";
    auto r = test::run(src);
    REQUIRE(r.ok());
    CHECK(r.passes <= 4);
    // Every jump is JMPR.
    CHECK(flat_image(r).bytes.size() == 3000 * 4 + 3 * 2 + 2);
}

// ...except where shrinking code can push a target away, as the padding
// before a DD can. There the two can end in different code, and openc166
// measures as Keil does. Checked against Keil A166 V5.38.
TEST_CASE("forward jumps across DD padding are shortened as Keil does") {
    const char* src =
    "$MOD167\n"
    "CS SECTION CODE\n"
    "P PROC NEAR\n"
    "        NOP\n"
    "S0L1:\n"
    "S0L10:\n"
    "S0L12:\n"
    "S0L20:\n"
    "S0L24:\n"
    "        DB 1\n"
    "        EVEN\n"
    "S0L29:\n"
    "        MOV R4, #1234H\n"
    "        JMP cc_NZ, S0L128\n"
    "        DB 1\n"
    "        EVEN\n"
    "        MOV R4, #1234H\n"
    "S0L33:\n"
    "        DB 1\n"
    "        EVEN\n"
    "        CALL S0L29\n"
    "        MOV R4, #1234H\n"
    "S0L39:\n"
    "        DB 1\n"
    "        EVEN\n"
    "        NOP\n"
    "S0L43:\n"
    "        MOV R4, #1234H\n"
    "        JMP cc_NZ, S0L154\n"
    "        NOP\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "        NOP\n"
    "        JMP cc_NZ, S0L1\n"
    "        NOP\n"
    "        NOP\n"
    "        JMP cc_NZ, S0L94\n"
    "        MOV R4, #1234H\n"
    "        DD 12345678H\n"
    "        NOP\n"
    "        JMP cc_NZ, S0L39\n"
    "        CALL S0L89\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "        JMP cc_NZ, S0L12\n"
    "        JMP cc_NZ, S0L24\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "        JMP cc_NZ, S0L95\n"
    "        NOP\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "        NOP\n"
    "        NOP\n"
    "        JMP cc_NZ, S0L104\n"
    "        NOP\n"
    "        JMP cc_NZ, S0L95\n"
    "        DD 12345678H\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "S0L86:\n"
    "        MOV R4, #1234H\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "S0L89:\n"
    "        CALL S0L86\n"
    "        NOP\n"
    "        JMP cc_NZ, S0L43\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "S0L94:\n"
    "        CALL S0L94\n"
    "S0L95:\n"
    "        CALL S0L104\n"
    "        DB 1\n"
    "        EVEN\n"
    "        JMP cc_NZ, S0L101\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "        CALL S0L10\n"
    "S0L101:\n"
    "        NOP\n"
    "        DB 1\n"
    "        EVEN\n"
    "        CALL S0L101\n"
    "S0L104:\n"
    "        CALL S0L20\n"
    "        MOV R4, #1234H\n"
    "S0L106:\n"
    "        DD 12345678H\n"
    "        CALL S0L127\n"
    "        NOP\n"
    "        NOP\n"
    "        DD 12345678H\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "        CALL S0L146\n"
    "        JMP cc_NZ, S0L33\n"
    "S0L117:\n"
    "        CALL S0L125\n"
    "        MOV R4, #1234H\n"
    "S0L119:\n"
    "        DD 12345678H\n"
    "S0L125:\n"
    "        MOV R4, #1234H\n"
    "        JMP cc_NZ, S0L138\n"
    "S0L127:\n"
    "        CALL S0L39\n"
    "S0L128:\n"
    "        CALL S0L119\n"
    "        MOV R4, #1234H\n"
    "        JMP cc_NZ, S0L24\n"
    "        NOP\n"
    "S0L138:\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "S0L146:\n"
    "        DB 1\n"
    "        EVEN\n"
    "        NOP\n"
    "        MOV R4, #1234H\n"
    "        MOV R4, #1234H\n"
    "        NOP\n"
    "        NOP\n"
    "        JMP cc_NZ, S0L106\n"
    "        JMP cc_NZ, S0L117\n"
    "S0L154:\n"
    "        RET\n"
    "P ENDP\n"
    "CS ENDS\n"
    " END\n";
    auto r = test::run(src);
    REQUIRE(r.ok());
    CHECK(test::hex(flat_image(r).bytes) ==
        "CC0001FFE6F434123D7901FFE6F4341201FFBBF8E6F4341201FFCC00E6F43412EA302201CC00CC00E6F43412CC003DE9"
        "CC00CC003D3BE6F43412FFFF78563412CC003DEABB2CCC00E6F434123DDA3DD9CC00E6F43412E6F43412E6F43412E6F4"
        "34123D25CC00CC00E6F43412CC00CC003D29CC003D1CFFFF78563412E6F43412E6F43412CC00E6F43412CC00E6F43412"
        "E6F43412E6F43412CC00E6F43412BBFACC003DBCE6F43412E6F43412BBFFBB0A01FF3D05E6F43412E6F43412BBA2CC00"
        "01FFBBFDBB9EE6F43412FFFF78563412BB14CC00CC00FFFF78563412E6F43412E6F43412BB143D94BB05E6F43412FFFF"
        "78563412E6F434123D07BB8EBBF8E6F43412EA300200CC00CC00E6F4341201FFCC00E6F43412E6F43412CC00CC003DD5"
        "3DE3CB00");
}
