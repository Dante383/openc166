// SPDX-License-Identifier: MIT
// DEFR, DEFA and DEFB, and register names used as immediates.
// Expected values were checked against Keil A166.
#include "doctest.h"
#include "helpers.hpp"

using namespace oc166;
using test::code;
using test::diags;
using test::one;
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

}  // namespace

TEST_CASE("DEFR names an SFR") {
    auto r = run(program("CONFIG DEFR 0FF0CH, RW\n"  // 2
                         "EREG DEFR 0F000H\n"        // 3
                         "N1 EQU 0FF10H\n"           // 4
                         "NREG DEFR N1+2, w\n",      // 5
                         " MOV CONFIG, R1\n"         // 8
                         " ADD CONFIG, #1\n"         // 9
                         " MOVB RL1, CONFIG\n"       // 10
                         " MOV R1, CONFIG+2\n"       // 11
                         " MOV R2, EREG\n"           // 12
                         " MOV R1, NREG\n"           // 13
                         " BSET CONFIG.4\n"));       // 14
    REQUIRE(r.ok());
    CHECK(code(r, 8) == "F6F10CFF");
    CHECK(code(r, 9) == "06860100");
    CHECK(code(r, 10) == "F3F20CFF");
    CHECK(code(r, 11) == "F2F10EFF");
    CHECK(code(r, 12) == "F2F200F0");
    CHECK(code(r, 13) == "F2F112FF");
    CHECK(code(r, 14) == "4F86");
}

TEST_CASE("DEFR rejects addresses outside the SFR areas and leaves the name undefined") {
    auto r = run(program("R1X DEFR 0FF1DH\n"    // 2  odd
                         "R2X DEFR 0FDFEH\n"    // 3  below the SFRs
                         "R3X DEFR 0FFE0H\n"    // 4  the GPRs
                         "R4X DEFR 0F1E0H\n"    // 5  past the ESFRs
                         "R5X DEFR PSW\n"       // 6  already an SFR
                         "R6X DEFR 0FF14H, X\n",  // 7  bad attribute; still defined
                         " MOV R1, R1X\n"         // 10
                         " MOV R1, R6X\n"));      // 11
    for (int line = 2; line <= 6; ++line) CHECK(diags(r, line) == "A101E");
    CHECK(diags(r, 7) == "A6E");
    CHECK(diags(r, 10) == "A42E");
    CHECK(code(r, 11) == "F2F114FF");
}

TEST_CASE("the C166 has no extended SFRs") {
    auto r = run(program("X1 DEFR 0F000H\n"
                         "X2 DEFB 0F100H.1\n", " NOP\n", "\n"),
                 isa::Cpu::C166);
    CHECK(diags(r, 2) == "A101E");
    CHECK(diags(r, 3) == "A68E");
    CHECK(one("BSET 0F100H.1", isa::Cpu::C166) == "A68E");
}

TEST_CASE("DEFA names a word of internal RAM") {
    auto r = run(program("XFA00 DEFA 0FA00H\n"  // 2
                         "XFDFE DEFA 0FDFEH, R\n"  // 3
                         "DA1 DEFA 0F9FEH\n"    // 4
                         "DA2 DEFA 0FA01H\n"    // 5
                         "DA3 DEFA 0FE00H\n",   // 6
                         " MOV R1, XFA00\n"     // 9
                         " MOV XFDFE, R1\n"     // 10
                         " MOV R1, [R2+#XFA00]\n"  // 11
                         " JMP XFA00\n"));      // 12
    CHECK(code(r, 9) == "F2F100FA");
    CHECK(code(r, 10) == "F6F1FEFD");
    CHECK(code(r, 11) == "D41200FA");
    CHECK(code(r, 12) == "EA0000FA");
    CHECK(diags(r, 12) == "A74W");
    for (int line = 4; line <= 6; ++line) CHECK(diags(r, line) == "A102E");
}

TEST_CASE("DEFB names a bit and needs word.bit") {
    auto r = run(program("CONFIG DEFR 0FF0CH\n"   // 2
                         "XBIT DEFB CONFIG.3\n"   // 3
                         "P0P1 DEFB 0FF00H.1\n"   // 4
                         "XB2 DEFB (0FF00H).(1+1)\n"  // 5
                         "B1 DEFB PSW.3+1\n"      // 6
                         "B2 DEFB C\n"            // 7
                         "B3 DEFB R1.2\n",        // 8
                         " BSET XBIT\n"           // 11
                         " BSET P0P1\n"           // 12
                         " BSET XB2\n"));         // 13
    CHECK(code(r, 11) == "3F86");
    CHECK(code(r, 12) == "1F80");
    CHECK(code(r, 13) == "2F80");
    CHECK(diags(r, 6) == "A103E");
    CHECK(diags(r, 7) == "A103E");
    CHECK(diags(r, 8) == "A70E");
}

TEST_CASE("forward references in DEFR, DEFA and DEFB") {
    // Keil reports each one; it keeps the DEFA and DEFB names but not DEFR's.
    auto r = run(program("DR DEFR FR\n"          // 2
                         "DA DEFA FA\n"          // 3
                         "DB1 DEFB FR.2\n"       // 4
                         "FR EQU 0FF12H\n"
                         "FA EQU 0FA06H\n",
                         " MOV R1, DR\n"         // 9
                         " MOV R1, DA\n"         // 10
                         " BSET DB1\n"));        // 11
    CHECK(diags(r, 2) == "A101E");
    CHECK(diags(r, 3) == "A102E");
    CHECK(diags(r, 4) == "A103E");
    CHECK(diags(r, 9) == "A42E");
    CHECK(code(r, 10) == "F2F106FA");
    CHECK(code(r, 11) == "2F89");
}

TEST_CASE("a forward bit position is an error but still selects the bit") {
    auto r = run(program("", " BSET 0FF00H.POS\nPOS EQU 2\n"));
    CHECK(code(r, 4) == "2F80");
    CHECK(diags(r, 4) == "A67E");
}

TEST_CASE("register names in immediates stand for their address") {
    CHECK(one("MOV R1, #PSW") == "E6F110FF");
    CHECK(one("CMP R1, #DPP0") == "46F100FE");
    CHECK(one("MOV R1, #ZEROS-1") == "E6F11BFF");
    CHECK(one("MOV R1, #R1") == "E6F1E2FF");
    CHECK(one("MOV R1, #R1+0") == "E6F1E2FF");
    CHECK(one("MOV R1, #RL1") == "E6F1E4FF");
    CHECK(one("MOV R1, #RH7") == "E6F1FEFF");
    CHECK(one("MOV [R1+#R2], R2") == "C421E4FF");
    CHECK(one("MOVB RL1, [R1+#PSW]") == "F42110FF");
    CHECK(one("MOVB RL1, #PSW") == "E7F21000 A62W");
    CHECK(one("TRAP #PSW") == "9B20 A62W");
    CHECK(one("MOV R1, #C") == "A70E");
    CHECK(one("MOV R1, #PSW.1") == "A70E");
    CHECK(one("MOV R1, #CC_Z") == "A6E");
    CHECK(one("DW R1") == "A70E");
}

TEST_CASE("LIT belongs to AX51") {
    auto r = run(program("LT LIT 'R1'\n", " NOP\n"));
    CHECK(diags(r, 2) == "A6E");
}
