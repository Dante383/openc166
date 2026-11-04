// SPDX-License-Identifier: MIT
// Sections without AT: where they go, what may be computed from their
// addresses, and how SECTION and ENDS nest. Expected values match Keil A166
// and L166 unless a comment says otherwise.
#include "doctest.h"
#include "helpers.hpp"

using namespace oc166;
using test::code;
using test::diags;
using test::run;

namespace {

const SectionInfo* find(const AssemblyResult& r, const std::string& name) {
    for (const auto& s : r.sections)
        if (s.name == name) return &s;
    return nullptr;
}

std::int64_t address(const AssemblyResult& r, const std::string& name) {
    const SectionInfo* s = find(r, name);
    return s ? s->address : -1;
}

bool has(const AssemblyResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics)
        if (d.code == code) return true;
    return false;
}

// Four sections: A (4 bytes of code), B at 6 (2 bytes), G (3 bytes) and D
// (2 bytes).
const char* const kFour =
    "$MOD167\n"
    "A SECTION CODE\nP PROC NEAR\n NOP\n RET\nP ENDP\nA ENDS\n"
    "B SECTION DATA AT 6\n DW 1\nB ENDS\n"
    "G SECTION CONST\n DB 1, 2, 3\nG ENDS\n"
    "D SECTION CONST\nDV: DW 5\nD ENDS\n"
    " END\n";

}  // namespace

TEST_CASE("sections are placed first fit in order of appearance") {
    auto r = run(kFour);
    REQUIRE(r.ok());
    REQUIRE(r.sections.size() == 4);
    // G does not fit between A and B, but D does.
    CHECK(address(r, "A") == 0);
    CHECK(address(r, "B") == 6);
    CHECK(address(r, "G") == 8);
    CHECK(address(r, "D") == 4);
    const SectionInfo& g = *find(r, "G");
    CHECK(g.type == "CONST");
    CHECK(g.align == "WORD");
    CHECK(g.size == 3);
    CHECK(g.placement == SectionInfo::Placement::Located);
    CHECK(find(r, "B")->placement == SectionInfo::Placement::At);
    CHECK(test::hex(flat_image(r).bytes) == "CC00CB0005000100010203");
}

TEST_CASE("located sections start at the origin") {
    AssembleOptions o;
    o.origin = 0x8000;
    auto r = assemble("K SECTION CONST\n DW K\nK ENDS\n END\n", o);
    REQUIRE(r.ok());
    CHECK(address(r, "K") == 0x8000);
    CHECK(test::hex(flat_image(r).bytes) == "0080");
}

TEST_CASE("section addresses can be given") {
    AssembleOptions o;
    o.cpu = isa::Cpu::C167;
    o.section_addresses = {{"g", 0x8000}};
    auto r = assemble(kFour, o);
    REQUIRE(r.ok());
    CHECK(address(r, "G") == 0x8000);
    CHECK(find(r, "G")->placement == SectionInfo::Placement::Given);
    CHECK(address(r, "D") == 4);

    o.section_addresses = {{"B", 0x100}, {"NONE", 0x200}, {"G", 0x101}};
    r = assemble(kFour, o);
    CHECK(r.error_count() == 1);
    CHECK(has(r, "OC14"));  // B has AT
    CHECK(r.warning_count() == 2);
    CHECK(has(r, "OC13"));  // NONE does not exist, G is odd
    CHECK(address(r, "G") == 0x101);
}

TEST_CASE("sections that do not fit") {
    auto r = run("$MOD167\nBITS SECTION DATA BITADDRESSABLE\n DS 102H\nBITS ENDS\n END\n");
    CHECK(has(r, "OC12"));
    auto ok = run("$MOD167\nBITS SECTION DATA BITADDRESSABLE\n DS 100H\nBITS ENDS\n END\n");
    CHECK(ok.ok());
    CHECK(address(ok, "BITS") == 0xFD00);
}

TEST_CASE("DATA and CONST sections stay within a 16 KB page") {
    auto r = run("$MOD167\nBIG SECTION XDATA\n DS 3F00H\nBIG ENDS\nD SECTION DATA\n DS 200H\nD ENDS\n END\n");
    REQUIRE(r.ok());
    CHECK(address(r, "D") == 0x4000);
    // XDATA may cross.
    r = run("$MOD167\nBIG SECTION XDATA\n DS 3F00H\nBIG ENDS\nX SECTION XDATA\n DS 200H\nX ENDS\n END\n");
    REQUIRE(r.ok());
    CHECK(address(r, "X") == 0x3F00);
}

TEST_CASE("values that move with their section") {
    auto r = run("$MOD167\n"
                 "D SECTION DATA\nX: DW 1\nY: DW 2\nD ENDS\n"  // 2..5
                 "F SECTION DATA AT 100H\nFX: DW 3\nF ENDS\n"  // 6..8
                 "G SECTION CODE\nP PROC NEAR\n"               // 9, 10
                 " MOV R1, #Y - X\n"                            // 11: a plain number
                 " MOV R1, #POF Y\n"                            // 12: always the long form
                 " MOV R1, #FX\n"                               // 13: AT sections are plain numbers
                 " MOV R1, #X AND 1\n"                          // 14
                 " MOV R1, #X - FX\n"                           // 15: Keil silently gives 0
                 " MOV R1, #X + Y\n"                            // 16
                 " MOV R1, #2 - POF X\n"                        // 17
                 " EXTR #POF X\n"                               // 18
                 "P ENDP\nG ENDS\n END\n");
    CHECK(code(r, 11) == "E021");
    CHECK(code(r, 12) == "E6F10200");
    CHECK(code(r, 13) == "E6F10001");
    CHECK(diags(r, 14) == "A63E");
    CHECK(diags(r, 15) == "A63E");
    CHECK(diags(r, 16) == "A63E");
    CHECK(diags(r, 17) == "A63E");
    CHECK(diags(r, 18) == "A72E");
}

TEST_CASE("ORG and DS in sections without AT") {
    auto r = run("$MOD167\n"
                 "D SECTION DATA\n DW 1\nX: DW 2\nY:\n"  // 2..5
                 " ORG $ + 2\n DW 3\n"                    // 6, 7: offsets in the section
                 " ORG X + 10H\n DW 4\n"                  // 8, 9
                 " DS Y - X\n"                            // 10
                 " DS X\n"                                // 11
                 "D ENDS\n END\n");
    CHECK(r.listing[6].address == 6);
    CHECK(r.listing[8].address == 0x12);
    CHECK(diags(r, 10) == "");
    CHECK(diags(r, 11) == "A72E");
    CHECK(find(r, "D")->size == 0x16);
}

TEST_CASE("sections nest") {
    auto r = run("$MOD167\n"
                 "T1 SECTION CONST\n DW 1\n"   // 2, 3
                 "T2 SECTION CONST\n DW 2\n"   // 4, 5
                 "T1 SECTION CONST\n DW 3\n"   // 6, 7: T1 again, inside T2
                 "T1 ENDS\n DW 4\n"            // 8, 9: back in T2
                 "T2 ENDS\n DW 5\n"            // 10, 11: back in T1
                 "T1 ENDS\n END\n");
    REQUIRE(r.ok());
    CHECK(test::hex(flat_image(r).bytes) == "01000300050002000400");
}

TEST_CASE("ENDS errors") {
    auto r = run("$MOD167\n"
                 " ENDS\n"                       // 2
                 "N1 SECTION DATA\nN2 SECTION DATA\n"
                 "N9 ENDS\n"                     // 5: unknown, N2 stays open
                 " DW 2\n"                       // 6
                 "N1 ENDS\n"                     // 7: closes N2
                 " DW 3\n"                       // 8: in N1
                 "N1 ENDS\n"                     // 9
                 "N1 ENDS\n"                     // 10: nothing open
                 " END\n");
    CHECK(diags(r, 2) == "A6E");
    CHECK(diags(r, 5) == "A28E");
    CHECK(diags(r, 7) == "A29E");
    CHECK(diags(r, 9) == "");
    CHECK(diags(r, 10) == "A6E A27E");
    CHECK(find(r, "N1")->size == 2);
    CHECK(find(r, "N2")->size == 2);
}

TEST_CASE("section names must be new") {
    auto r = run("$MOD167\n"
                 "S1 SECTION DATA AT 100H\nL1: DW 1\nQ1 EQU 5\nS1 ENDS\n"  // 2-5
                 "L1 SECTION DATA\nL1 ENDS\n"                             // 6, 7
                 "Q1 SECTION DATA\nQ1 ENDS\n"                             // 8, 9
                 "R1 SECTION DATA\nR1 ENDS\n"                             // 10, 11
                 "CODE SECTION DATA\n"                                    // 12
                 "cc_Z SECTION DATA\n"                                    // 13
                 "S1 EQU 6\n"                                             // 14
                 "CODE ENDS\n"                                            // 15
                 " END\n");
    CHECK(diags(r, 6) == "A25E");
    CHECK(diags(r, 7) == "A6E A27E");
    CHECK(diags(r, 8) == "A25E");
    CHECK(diags(r, 10) == "A25E");
    CHECK(diags(r, 11) == "A6E A27E");
    CHECK(diags(r, 12) == "A6E");
    CHECK(diags(r, 13) == "A6E");
    CHECK(diags(r, 14) == "A25E");
    CHECK(diags(r, 15) == "A6E");
    CHECK(find(r, "L1") == nullptr);
}

TEST_CASE("a section name defined later is still a clash") {
    auto r = run("$MOD167\n"
                 "S1 SECTION DATA\n DW 1\nS1 ENDS\n"  // 2-4
                 "S2 SECTION DATA\nS1: DW 2\nS2 ENDS\n"  // 5-7
                 " END\n");
    CHECK(diags(r, 2) == "");
    CHECK(diags(r, 6) == "A25E");
}

TEST_CASE("AT with an address that moves") {
    auto r = run("$MOD167\n"
                 "D SECTION DATA\nX: DW 1\nD ENDS\n"   // 2-4
                 "E1 SECTION DATA AT X\n DW 5\n"       // 5, 6
                 "E1 ENDS\n END\n");                   // 7
    CHECK(diags(r, 5) == "A72E");
    CHECK(diags(r, 6) == "");
    CHECK(diags(r, 7) == "");
    REQUIRE(find(r, "E1") != nullptr);
    CHECK(find(r, "E1")->size == 2);
}
