// SPDX-License-Identifier: MIT
// BIT sections and DBIT: bit variables in the bit-addressable area
// 0FD00H..0FDFFH, where they go, how they are used, and the errors. Expected
// values match Keil A166 and L166 unless a comment says otherwise.
#include "doctest.h"
#include "helpers.hpp"

using namespace oc166;
using test::code;
using test::diags;
using test::run;

namespace {

const SectionInfo* section(const AssemblyResult& r, const std::string& name) {
    for (const auto& s : r.sections)
        if (s.name == name) return &s;
    return nullptr;
}

// Address of a listing line as Keil shows it in a BIT section: word.bit.
std::string bit_loc(const AssemblyResult& r, int line) {
    for (const auto& l : r.listing) {
        if (l.line != line) continue;
        if (!l.has_address || !l.bit_address) return "-";
        std::int64_t word = (l.address >> 4) << 1;
        char buf[16];
        std::snprintf(buf, sizeof buf, "%04X.%d", static_cast<unsigned>(word), static_cast<int>(l.address & 15));
        return buf;
    }
    return "?";
}

}  // namespace

TEST_CASE("DBIT reserves bits, and a BIT section without AT starts at 0FD00H.0") {
    auto r = run("$MOD167\n"
                 "Flags SECTION BIT\n"                  // 2
                 "F1: DBIT\n"                           // 3
                 "F2: DBIT 3\n"                         // 4
                 "F3: DBIT 13\n"                        // 5
                 "F4: DBIT\n"                           // 6
                 "Flags ENDS\n"                         // 7
                 "Prog SECTION CODE AT 0C000H\n"        // 8
                 "P PROC NEAR\n"                        // 9
                 " BSET F1\n BCLR F2\n BSET F4\n"       // 10-12
                 " JB F3, P\n"                          // 13
                 " BFLDL F4, #3, #1\n"                  // 14
                 "P ENDP\nProg ENDS\n END\n");
    REQUIRE(r.ok());
    CHECK(bit_loc(r, 3) == "FD00.0");
    CHECK(bit_loc(r, 4) == "FD00.1");
    CHECK(bit_loc(r, 5) == "FD00.4");
    CHECK(bit_loc(r, 6) == "FD02.1");
    CHECK(code(r, 10) == "0F00");
    CHECK(code(r, 11) == "1E00");
    CHECK(code(r, 12) == "1F01");
    CHECK(code(r, 13) == "8A00FB40");
    // BFLDL takes the word the bit is in.
    CHECK(code(r, 14) == "0A010301");
    const SectionInfo* s = section(r, "Flags");
    REQUIRE(s);
    CHECK(s->type == "BIT");
    CHECK(s->align == "BIT");
    CHECK(s->address == 0xFD00);
    CHECK(s->bit == 0);
    CHECK(s->bit_size == 18);
    CHECK(s->size == 3);  // bytes touched
    CHECK(format_listing(r).find("FD00.4                            5    F3: DBIT 13\n") != std::string::npos);
    CHECK(format_listing(r).find("Flags                            BIT    BIT            00FD00H.0  size 0002H.2  "
                                 "located\n") != std::string::npos);
}

TEST_CASE("a BIT section with AT starts at a byte or at a bit") {
    auto r = run("$MOD167\n"
                 "A1 SECTION BIT AT 0FD10H\nG1: DBIT 2\nA1 ENDS\n"      // 2-4
                 "A2 SECTION BIT AT 0FD20H.11\nG2: DBIT 9\nA2 ENDS\n"   // 5-7
                 "Prog SECTION CODE AT 0C000H\nP PROC NEAR\n"           // 8-9
                 " BSET G1\n BSET G2\n BSET G2+5\n"                     // 10-12
                 "P ENDP\nProg ENDS\n END\n");
    REQUIRE(r.ok());
    CHECK(bit_loc(r, 3) == "FD10.0");
    CHECK(bit_loc(r, 6) == "FD20.11");
    CHECK(code(r, 10) == "0F08");
    CHECK(code(r, 11) == "BF10");
    CHECK(code(r, 12) == "0F11");
    const SectionInfo* s = section(r, "A2");
    REQUIRE(s);
    CHECK(s->address == 0xFD21);
    CHECK(s->bit == 3);
    CHECK(s->bit_size == 9);
    CHECK(s->size == 2);
    CHECK(s->placement == SectionInfo::Placement::At);
}

TEST_CASE("bytes and words in a BIT section start on a byte or a word") {
    auto r = run("$MOD167\n"
                 "W SECTION BIT WORD\n"   // 2
                 "B1: DBIT 2\n"           // 3
                 "B2: DSB 1\n"            // 4
                 "B3: DSW 1\n"            // 5
                 " DS 1\n"                // 6
                 "B4: DBIT\n"             // 7
                 " EVEN\n"                // 8
                 "B5: DBIT\n"             // 9
                 "W ENDS\n END\n");
    REQUIRE(r.ok());
    CHECK(bit_loc(r, 4) == "FD00.8");
    CHECK(bit_loc(r, 5) == "FD02.0");
    CHECK(bit_loc(r, 7) == "FD04.8");
    CHECK(bit_loc(r, 9) == "FD06.0");
}

TEST_CASE("EQU, SET and BIT name bits; EQU of a register bit gives its number") {
    auto r = run("$MOD167\n"
                 "X BIT 0FD10H.3\n"                         // 2
                 "E1 EQU X\n"                               // 3
                 "E2 SET PSW.3\n"                           // 4
                 "E3 EQU R3.4\n"                            // 5
                 "Fx SECTION BIT AT 0FD20H\nG1: DBIT 3\nFx ENDS\n"  // 6-8
                 "E4 EQU G1+2\n"                            // 9
                 "Prog SECTION CODE AT 0C000H\nP PROC NEAR\n"  // 10-11
                 " BSET E1\n BSET E2\n MOV R1, #E3\n BSET E4\n"  // 12-15
                 " BFLDH E1, #0F0H, #E3\n"                  // 16
                 "P ENDP\nProg ENDS\n END\n");
    REQUIRE(r.ok());
    CHECK(code(r, 12) == "3F08");
    CHECK(code(r, 13) == "3F88");
    CHECK(code(r, 14) == "E041");
    CHECK(code(r, 15) == "2F10");
    CHECK(code(r, 16) == "1A0804F0");
    CHECK(format_listing(r).find("E3                               EQU    00000004H\n") != std::string::npos);
}

TEST_CASE("L166 places BITADDRESSABLE sections first, then bits in the gaps") {
    auto r = run("$MOD167\n"
                 "Flags SECTION BIT\nF1: DBIT 6\nFlags ENDS\n"           // 2-4
                 "Pins SECTION BIT AT 0FD04H.6\nP1: DBIT 2\nPins ENDS\n"  // 5-7
                 "Late SECTION BIT\nL1: DBIT 9\nLate ENDS\n"             // 8-10
                 "Ba SECTION DATA BITADDRESSABLE\n DW 0, 0\nBa ENDS\n"   // 11-13
                 "Mo SECTION DATA BITADDRESSABLE\n DB 1\nMo ENDS\n"      // 14-16
                 " END\n");
    REQUIRE(r.ok());
    CHECK(section(r, "Ba")->address == 0xFD00);
    // The byte at 0FD04H holds bits of Pins.
    CHECK(section(r, "Mo")->address == 0xFD06);
    // Flags fits in front of Pins, Late only after Mo.
    CHECK(section(r, "Flags")->address == 0xFD04);
    CHECK(section(r, "Flags")->bit == 0);
    CHECK(section(r, "Late")->address == 0xFD07);
    CHECK(section(r, "Late")->bit_size == 9);
}

TEST_CASE("BIT sections of several modules are joined or overlap") {
    auto r = assemble(std::vector<Source>{
                          {"a.a66", "$MOD167\nPUBLIC A1\nEXTRN B1:BIT\n"
                                    "S1 SECTION BIT PUBLIC\nA1: DBIT 3\nS1 ENDS\n"
                                    "Cm SECTION BIT COMMON\nC1: DBIT 2\nCm ENDS\n"
                                    "Prog SECTION CODE AT 0C000H\nP PROC NEAR\n BSET B1\n BSET C1\nP ENDP\nProg ENDS\n"
                                    " END\n"},
                          {"b.a66", "$MOD167\nPUBLIC B1\n"
                                    "S1 SECTION BIT PUBLIC\nB1: DBIT 5\nS1 ENDS\n"
                                    "Cm SECTION BIT COMMON\nD1: DBIT 7\nCm ENDS\n END\n"}},
                      AssembleOptions{});
    REQUIRE(r.ok());
    CHECK(test::hex(r.chunks[0].bytes) == "3F008F00");
    const SectionInfo* s1 = section(r, "S1");
    REQUIRE(s1);
    CHECK(s1->bit_size == 8);
    const SectionInfo* cm = section(r, "Cm");
    REQUIRE(cm);
    CHECK(cm->address == 0xFD01);
    CHECK(cm->bit_size == 7);
}

TEST_CASE("a BIT section can be given an address") {
    AssembleOptions o;
    o.cpu = isa::Cpu::C167;
    o.section_addresses = {{"flags", 0xFD40}};
    auto r = assemble("Flags SECTION BIT\nF1: DBIT 2\nF2: DBIT\nFlags ENDS\n"
                      "Prog SECTION CODE AT 0C000H\nP PROC NEAR\n BSET F2\nP ENDP\nProg ENDS\n END\n",
                      o);
    REQUIRE(r.ok());
    CHECK(code(r, 7) == "2F20");
    CHECK(section(r, "Flags")->placement == SectionInfo::Placement::Given);
}

TEST_CASE("mistakes with BIT sections and DBIT") {
    auto r = run("$MOD167\n$SEGMENTED\n"
                 " DBIT 1\n"                                         // 3
                 "D SECTION DATA\n DBIT 1\nD ENDS\n"                 // 4-6
                 "B2 SECTION BIT PAGE\nB2 ENDS\n"                    // 7-8
                 "A1 SECTION BIT AT 0FDE0H\nA1 ENDS\n"               // 9-10
                 "A3 SECTION BIT AT Later\nA3 ENDS\n"                // 11-12
                 "D1 SECTION DATA BITADDRESSABLE AT 0FE00H\nD1 ENDS\n"  // 13-14
                 "Flags SECTION BIT\n"                               // 15
                 "F1: DBIT 3\n"                                      // 16
                 "F2: DBIT Later\n"                                  // 17
                 "F3: DBIT F1\n"                                     // 18
                 "L1: DB 1\n"                                        // 19
                 " DW 1\n"                                           // 20
                 " NOP\n"                                            // 21
                 "Flags ENDS\n"                                      // 22
                 "Fix SECTION BIT AT 0FDD0H\n DBIT 3\nFx1: DBIT 381\nFix ENDS\n"  // 23-26
                 "Rel SECTION BIT\n DBIT 4\n DBIT -10\nRel ENDS\n"   // 27-30
                 "Later EQU 2\n"                                     // 31
                 "Bad1 EQU F1.1\n"                                   // 32
                 "Prog SECTION CODE\nP PROC NEAR\n"                  // 33-34
                 " MOV R1, #Flags\n"                                 // 35
                 " BFLDL R3.4, #1, #1\n"                             // 36
                 " MOV R2, #BOF Fx1\n"                               // 37
                 " MOV R3, #POF F1\n"                                // 38
                 "P ENDP\nProg ENDS\n"                               // 39-40
                 " ASSUME DPP1:Flags\n"                              // 41
                 " END\n");
    CHECK(diags(r, 3) == "A44E");
    CHECK(diags(r, 5) == "A45E");
    CHECK(diags(r, 7) == "A43E");
    CHECK(diags(r, 9) == "A91E");
    CHECK(diags(r, 10) == "");  // the section is open anyway
    CHECK(diags(r, 11) == "A71E");
    CHECK(diags(r, 12) == "");
    CHECK(diags(r, 13) == "A91E");
    CHECK(diags(r, 17) == "A71E");
    CHECK(diags(r, 18) == "A72E");
    CHECK(diags(r, 19) == "A48E");
    CHECK(diags(r, 20) == "A48E");
    CHECK(diags(r, 21) == "A45E");
    CHECK(diags(r, 25) == "A86E");
    CHECK(diags(r, 29) == "A86E");
    CHECK(diags(r, 32) == "A69E");
    CHECK(diags(r, 35) == "A70E");
    CHECK(diags(r, 36) == "A74E");
    // Keil gives 0 here (from the byte address 0FDD0H), not 3.
    CHECK(diags(r, 37) == "OC23E");
    // Keil gives an odd value for POF, SOF, PAG and SEG of a bit.
    CHECK(diags(r, 38) == "A61E");
    CHECK(diags(r, 41) == "A56E");
}
