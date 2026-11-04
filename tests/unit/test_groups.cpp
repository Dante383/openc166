// SPDX-License-Identifier: MIT
// DGROUP and CGROUP: sections kept together in one 16 KB data page or one
// 64 KB code segment, the value of a group name, and the errors. Expected
// values match Keil A166 and L166.
#include "doctest.h"
#include "helpers.hpp"

using namespace oc166;
using test::code;
using test::diags;
using test::run;

namespace {

std::int64_t address(const AssemblyResult& r, const std::string& name) {
    for (const auto& s : r.sections)
        if (s.name == name) return s.address;
    return -1;
}

std::string group(const AssemblyResult& r, const std::string& name) {
    for (const auto& s : r.sections)
        if (s.name == name) return s.group;
    return "?";
}

}  // namespace

TEST_CASE("a DGROUP goes to the first page with room for all its sections, largest first") {
    auto r = run("$MOD167\n$SEGMENTED\n"
                 "Fill SECTION DATA AT 10H\n DS 3FD0H\nFill ENDS\n"   // 3-5
                 "SecA SECTION DATA\n DW 1\nSecA ENDS\n"              // 6-8
                 "SecB SECTION DATA\n DW 2\n DS 2EH\nSecB ENDS\n"     // 9-12
                 "SecK SECTION CONST\n DW 3, 4\nSecK ENDS\n"          // 13-15
                 " ASSUME DPP2:Grp\n"                                 // 16
                 "Grp DGROUP SecA, SecB, SecK\n"                      // 17
                 "Tbl SECTION CONST\n DW Grp, PAG Grp, SOF Grp\nTbl ENDS\n"  // 18-20
                 " END\n");
    REQUIRE(r.ok());
    // Page 0 has room for each section but not for all of them.
    CHECK(address(r, "SecB") == 0x4000);
    CHECK(address(r, "SecK") == 0x4030);
    CHECK(address(r, "SecA") == 0x4034);
    // Sections outside groups come after the groups and fill gaps.
    CHECK(address(r, "Tbl") == 0);
    CHECK(code(r, 19) == "004001000040");
    CHECK(group(r, "SecK") == "Grp");
    CHECK(group(r, "Tbl") == "");
    CHECK(format_listing(r).find("SecK                             CONST  WORD           004030H  size 0004H  "
                               "located  group Grp\n") != std::string::npos);
}

TEST_CASE("a CGROUP goes to the segment of its section at a fixed address") {
    auto r = run("$MOD167\n$SEGMENTED\n"
                 "Far1 SECTION CODE AT 10000H\nP1 PROC NEAR\n RET\nP1 ENDP\nFar1 ENDS\n"  // 3-7
                 "Near1 SECTION CODE\nP2 PROC NEAR\n RET\nP2 ENDP\nNear1 ENDS\n"        // 8-12
                 "Codes CGROUP Near1, Far1\n"                                           // 13
                 "Tbl SECTION CONST\n DW SEG Codes, SOF Codes, SEG P2, SOF P2\nTbl ENDS\n"  // 14-16
                 " END\n");
    REQUIRE(r.ok());
    CHECK(address(r, "Far1") == 0x10000);
    CHECK(address(r, "Near1") == 0x10002);
    CHECK(address(r, "Tbl") == 0);
    CHECK(code(r, 15) == "0100000001000200");
}

TEST_CASE("the first section listed with a fixed address decides the page") {
    const std::string head = "$MOD167\n$SEGMENTED\n"
                             "Pin1 SECTION DATA AT 100H\n DW 1\nPin1 ENDS\n"
                             "Pin2 SECTION DATA AT 4100H\n DW 2\nPin2 ENDS\n"
                             "Free1 SECTION DATA\n DW 3\nFree1 ENDS\n";
    auto hi = run(head + "Grp DGROUP Pin2, Free1, Pin1\n END\n");
    REQUIRE(hi.ok());
    CHECK(address(hi, "Free1") == 0x4000);
    auto lo = run(head + "Grp DGROUP Free1, Pin1, Pin2\n END\n");
    REQUIRE(lo.ok());
    CHECK(address(lo, "Free1") == 0);
}

TEST_CASE("ASSUME on one section of a group covers the group") {
    auto r = run("$MOD167\n$SEGMENTED\n"
                 "Lone SECTION DATA\n DW 1\nLone ENDS\n"           // 3-5
                 "SecA SECTION DATA\nVarA: DW 2\nSecA ENDS\n"       // 6-8
                 "SecB SECTION CONST\nVarB: DW 3\nSecB ENDS\n"      // 9-11
                 " ASSUME DPP1:SecB\n"                             // 12
                 "Grp DGROUP SecA, SecB\n"                         // 13
                 "Prog SECTION CODE\nRun PROC NEAR\n"              // 14, 15
                 " MOV R1, VarA\n MOV R2, #Grp\n MOV R3, Grp\n"    // 16-18
                 " MOV R4, #POF Grp\n"                             // 19
                 " RET\nRun ENDP\nProg ENDS\n END\n");
    CHECK(code(r, 16) == "F2F10040");
    CHECK(code(r, 17) == "E6F20040");
    CHECK(code(r, 18) == "F2F30040");
    CHECK(diags(r, 19) == "A96E");
    CHECK(address(r, "SecA") == 0);
    CHECK(address(r, "SecB") == 2);
    CHECK(address(r, "Lone") == 4);
}

TEST_CASE("group errors") {
    auto r = run("$MOD167\n"
                 "SecA SECTION DATA\nVarA: DW 1\nSecA ENDS\n"  // 2-4
                 "SecB SECTION DATA\n DW 2\nSecB ENDS\n"       // 5-7
                 "Prog SECTION CODE\nProg ENDS\n"               // 8, 9
                 "Grp1 DGROUP SecA\n"                           // 10
                 "Grp1 DGROUP SecB\n"                           // 11: no extending
                 "Grp2 DGROUP SecA\n"                           // 12: already in Grp1
                 "Grp3 DGROUP Prog\n"                           // 13: wrong type, joins anyway
                 "Grp4 CGROUP Prog\n"                           // 14: already in Grp3
                 "Grp5 DGROUP Nowhere\n"                        // 15
                 "VarA DGROUP SecB\n"                           // 16
                 "Grp6 DGROUP VarA\n"                           // 17
                 "Grp7 DGROUP\n"                                // 18
                 "Grp8 DGROUP SecB,\n"                          // 19
                 " DGROUP SecB\n"                               // 20
                 "Grp1 SECTION DATA\n"                          // 21
                 "Grp1 ENDS\n"                                  // 22
                 " PUBLIC Grp1\n"                               // 23
                 " END\n");
    CHECK(diags(r, 10) == "");
    CHECK(diags(r, 11) == "A25E");
    CHECK(diags(r, 12) == "A53E");
    CHECK(diags(r, 13) == "A34E");
    CHECK(diags(r, 14) == "A53E");
    CHECK(diags(r, 15) == "A93E");
    CHECK(diags(r, 16) == "A25E");
    CHECK(diags(r, 17) == "A25E");
    CHECK(diags(r, 18) == "A6E");
    CHECK(diags(r, 19) == "A6E");
    CHECK(diags(r, 20) == "A6E");
    CHECK(diags(r, 21) == "A25E");
    CHECK(diags(r, 22) == "A6E A27E");
    CHECK(diags(r, 23) == "A39E");
}

TEST_CASE("a group must fit in one page or segment") {
    auto big = run("$MOD167\n"
                   "Big1 SECTION DATA\n DS 2100H\nBig1 ENDS\n"
                   "Big2 SECTION DATA\n DS 2100H\nBig2 ENDS\n"
                   "Grp DGROUP Big1, Big2\n"  // 8
                   " END\n");
    CHECK(diags(big, 8) == "OC22E");
    // Tied to a page by a fixed address, each section without room there
    // is an error of its own.
    auto tied = run("$MOD167\n$SEGMENTED\n"
                    "Fill SECTION DATA AT 200H\n DS 3E00H\nFill ENDS\n"  // 3-5
                    "Pin1 SECTION DATA AT 0H\n DS 1F0H\nPin1 ENDS\n"     // 6-8
                    "Free1 SECTION DATA\n DW 3\nFree1 ENDS\n"            // 9-11
                    "Free2 SECTION DATA\n DS 20H\nFree2 ENDS\n"          // 12-14
                    "Free3 SECTION DATA\n DS 30H\nFree3 ENDS\n"          // 15-17
                    "Grp DGROUP Pin1, Free1, Free2, Free3\n"             // 18
                    " END\n");
    CHECK(diags(tied, 9) == "");
    CHECK(diags(tied, 12) == "OC22E");
    CHECK(diags(tied, 15) == "OC22E");
    CHECK(diags(tied, 18) == "");
    CHECK(address(tied, "Free1") == 0x1F0);
}

TEST_CASE("groups of the same name in different modules are one group") {
    AssembleOptions o;
    auto r = assemble(std::vector<Source>{{"a.a66",
                                           "$MOD167\n$SEGMENTED\n"
                                           "Fill SECTION DATA AT 10H\n DS 3FD0H\nFill ENDS\n"
                                           "Small SECTION DATA\n DW 1\nSmall ENDS\n"
                                           "Grp DGROUP Small\n END\n"},
                                          {"b.a66",
                                           "$MOD167\n$SEGMENTED\n"
                                           "Big SECTION DATA\n DW 2\n DS 3EH\nBig ENDS\n"
                                           "Grp DGROUP Big\n END\n"}},
                      o);
    REQUIRE(r.ok());
    CHECK(address(r, "Big") == 0x4000);
    CHECK(address(r, "Small") == 0x4040);
}
