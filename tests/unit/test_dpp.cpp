// SPDX-License-Identifier: MIT
// Segmented mode: DPPn: prefixes, ASSUME, section names, pointer directives,
// and the DATAn and BOF operators. Expected values were checked against Keil
// A166.
#include "doctest.h"
#include "helpers.hpp"

using namespace oc166;
using test::code;
using test::diags;
using test::one;
using test::run;

namespace {

// Lines 1..10; the code section starts on line 11 and its PROC on line 12.
constexpr std::string_view kSegmentedHead =
    "$MOD167\n"                   // 1
    "$SEGMENTED\n"                // 2
    "DS1 SECTION DATA AT 0E000H\n"  // 3
    "V1 DSW 2\n"                  // 4
    "VB DSB 2\n"                  // 5
    "DS1 ENDS\n"                  // 6
    "DS2 SECTION DATA AT 12340H\n"  // 7
    "W1 DSW 2\n"                  // 8
    "DS2 ENDS\n"                  // 9
    "X1 EQU DPP2:3FFEH+2\n"       // 10
    "CSEC SECTION CODE AT 0C000H\n"  // 11
    "P PROC NEAR\n";              // 12

std::string segmented(std::string_view body) {
    std::string s(kSegmentedHead);
    s += body;
    s += "P ENDP\nCSEC ENDS\n END\n";
    return s;
}

std::string result(const AssemblyResult& r, int line) {
    std::string c = code(r, line), d = diags(r, line);
    return c + (!c.empty() && !d.empty() ? " " : "") + d;
}

}  // namespace

TEST_CASE("DPPn: prefixes select the page of an address") {
    auto r = run(segmented(" MOV R1,DPP2:1234H\n"   // 13
                           " MOV R1,DPP1:V1\n"      // 14
                           " MOV R1,X1\n"           // 15
                           " MOV R1,#DPP3:1234H\n"  // 16
                           " DW DPP2:1234H, DPP2:V1\n"));  // 17
    CHECK(result(r, 13) == "F2F13492");
    CHECK(result(r, 14) == "F2F10060");
    // X1 EQU DPP2:3FFEH+2 folds the DPP number into the value: C000H.
    CHECK(result(r, 15) == "F2F100C0");
    CHECK(result(r, 16) == "E6F134D2");
    // Data keeps a label's address even with a prefix.
    CHECK(result(r, 17) == "349200E0");
}

TEST_CASE("ASSUME ties a section or SYSTEM to a DPP register") {
    auto r = run(segmented(" MOV R1,W1\n"                   // 13
                           " MOV R1,PSW\n"                  // 14
                           " ASSUME DPP1:DS1, DPP2:DS2\n"   // 15
                           " MOV R1,V1\n"                   // 16
                           " MOV R1,W1+2\n"                 // 17
                           " MOV R1,#W1\n"                  // 18
                           " MOV R1,[R2+#V1]\n"             // 19
                           " MOV R1,V1+1\n"                 // 20
                           " ASSUME DPP3:SYSTEM\n"          // 21
                           " MOV R1,PSW\n"                  // 22
                           " ASSUME DPP0:SYSTEM\n"          // 23
                           " ASSUME DPP4:DS1\n"             // 24
                           " ASSUME DPP1\n"                 // 25
                           " EXTP #4,#1\n"                  // 26
                           " MOV R1,0FF10H\n"               // 27
                           " ASSUME NOTHING\n"              // 28
                           " MOV R1,V1\n"));                // 29
    // Without an ASSUME for its section, an address keeps its low 16 bits.
    CHECK(result(r, 13) == "F2F14023 A77W");
    CHECK(result(r, 14) == "F2F110FF A77W");
    CHECK(result(r, 16) == "F2F10060");
    CHECK(result(r, 17) == "F2F142A3");
    CHECK(result(r, 18) == "E6F140A3");
    CHECK(result(r, 19) == "D4120060");
    CHECK(result(r, 20) == "F2F10160 A134W");
    CHECK(result(r, 22) == "F2F110FF");
    // A section or SYSTEM can have only one DPP.
    CHECK(diags(r, 23) == "A94E");
    CHECK(diags(r, 24) == "A35E");
    CHECK(diags(r, 25) == "A36E");
    // EXTP gives the page itself, so no DPP is needed.
    CHECK(result(r, 26) == "D7400400");
    CHECK(result(r, 27) == "F2F110FF");
    CHECK(result(r, 29) == "F2F100E0 A77W");
}

TEST_CASE("Pointer directives and $ in data lists") {
    auto r = run(segmented(" DPPTR W1, V1\n"   // 13
                           " DSPTR W1\n"       // 14
                           " DBPTR PSW.3\n"    // 15
                           " DW $, $\n"));     // 16
    CHECK(result(r, 13) == "4023040000200300");
    CHECK(result(r, 14) == "40230100");
    CHECK(result(r, 15) == "0300103F0300");
    // $ is the address of each item.
    CHECK(result(r, 16) == "12C014C0");
}

TEST_CASE("Data directives page a data section's name through ASSUME") {
    auto r = run(segmented(" DW DS2\n"                       // 13
                           " ASSUME DPP1:DS1, DPP2:DS2\n"    // 14
                           " DW DS2\n"                       // 15
                           " DW DS1\n"                       // 16
                           " DW CSEC\n"                      // 17
                           " DD DS2\n"                       // 18
                           " DB DS2\n"                       // 19
                           " DW 2+DS2, DS2-2, (DS2)\n"       // 20
                           " DW DPP1:DS2\n"                  // 21
                           " DW DS2/2, DS2 AND 0FFFFH\n"     // 22
                           " DW W1, W1+2\n"                  // 23
                           " DPPTR DS2\n"                    // 24
                           " DSPTR DS2\n"                    // 25
                           "Y1 EQU DS2\n"                    // 26
                           "Y2 SET W1\n"                     // 27
                           "Y3 EQU W1+2\n"                   // 28
                           " DW Y1\n"                        // 29
                           " MOV R1,Y1\n"                    // 30
                           " MOV R1,Y2\n"                    // 31
                           " MOV R1,Y3\n"));                 // 32
    CHECK(result(r, 13) == "4023 A62W");
    CHECK(result(r, 15) == "40A3");
    CHECK(result(r, 16) == "0060");
    // A code section's name is not paged.
    CHECK(result(r, 17) == "00C0");
    CHECK(result(r, 18) == "40A30000");
    CHECK(result(r, 19) == "40 A62W");
    CHECK(result(r, 20) == "42A33EA340A3");
    CHECK(result(r, 21) == "4063");
    // Other operators work on the plain address.
    CHECK(result(r, 22) == "A0914023");
    // So do labels in data directives.
    CHECK(result(r, 23) == "40234223 A62W A62W");
    CHECK(result(r, 24) == "40230200");
    CHECK(result(r, 25) == "40A30000");
    // EQU and SET keep the page only for a plain label.
    CHECK(result(r, 29) == "4023 A62W");
    CHECK(result(r, 30) == "F2F14023 A62W");
    CHECK(result(r, 31) == "F2F140A3");
    CHECK(result(r, 32) == "F2F14223 A62W");
}

TEST_CASE("Nonsegmented mode ignores DPPn: prefixes and ASSUME") {
    CHECK(one("MOV R1,DPP2:1234H") == "F2F13412 A55W");
    CHECK(one("MOV R1,DPP2:12345H") == "F2F14523 A55W");
    CHECK(one("MOV R1,12345H") == "F2F14523 A62W");
    CHECK(one("ASSUME DPP1:S") == "A54W");
    CHECK(one("MOV R1,0E001H") == "F2F101E0 A134W");
}

TEST_CASE("DATAn gives an immediate a size class") {
    CHECK(one("MOV R1,#DATA3 5") == "E051");
    CHECK(one("MOV R1,#DATA4 -1+20") == "E6F12300");
    CHECK(one("MOV R1,#DATA16 5") == "E6F10500");
    CHECK(one("MOV R1,#DATA8 1234H") == "E6F13400");
    CHECK(one("MOV R1,#DATA32 1") == "A6E");
}

TEST_CASE("BOF is the position of a bit") {
    auto r = run("$MOD167\n"                      // 1
                 "FB1 BIT 0FD00H.8\n"             // 2
                 "S SECTION CODE AT 0C000H\n"     // 3
                 "P PROC NEAR\n"                  // 4
                 " MOV R1,#BOF FB1+1\n"           // 5
                 " MOV R1,#BOF 12H\n"             // 6
                 " MOV R1,#BOF PSW.15\n"          // 7
                 "P ENDP\nS ENDS\n END\n");
    CHECK(result(r, 5) == "E091");
    CHECK(result(r, 6) == "E001 A66E");
    CHECK(result(r, 7) == "A61E");
}

TEST_CASE("Data sections with a fixed address") {
    auto r = run("$MOD167\n"                       // 1
                 "DS1 SECTION DATA AT 0F000H\n"    // 2
                 " DS 4002H\n"                     // 3
                 "DS1 ENDS\n"                      // 4
                 "DS2 SECTION DATA AT 12340H\n"    // 5
                 "DS2 ENDS\n"                      // 6
                 " END\n");
    // A DATA section must fit in the 16 KB page it starts in.
    CHECK(diags(r, 3) == "A86E");
    // Addresses above 0FFFFH need $SEGMENTED.
    CHECK(diags(r, 5) == "A19E");
}
