// SPDX-License-Identifier: MIT
// Whole-program behaviour: passes, directives, sections, data and output.
// Expected values match Keil A166 unless a comment says otherwise.
#include "doctest.h"
#include "helpers.hpp"

#include <stdexcept>

using namespace oc166;
using test::code;
using test::diags;
using test::run;

TEST_CASE("RET follows the procedure type") {
    auto r = run("$MOD167\nS SECTION CODE AT 0C000H\n"
                 "N PROC NEAR\n RET\nN ENDP\n"        // 3..5
                 "F PROC FAR\n RET\nF ENDP\n"         // 6..8
                 "T PROC TASK INTNO=20H\n RET\nT ENDP\n"  // 9..11
                 "W PROC NEAR\n RETS\n RETI\nW ENDP\n"   // 12..15
                 "S ENDS\n END\n");
    CHECK(code(r, 4) == "CB00");
    CHECK(code(r, 7) == "DB00");
    CHECK(code(r, 10) == "FB88");
    CHECK(diags(r, 13) == "A78W");
    CHECK(diags(r, 14) == "A80W");
}

TEST_CASE("forward references converge") {
    auto r = run("$MOD167\nS SECTION CODE AT 0C000H\nP PROC NEAR\n"
                 " MOV R1, #FWD\n"           // 4: forward EQU, short form
                 " JMPR cc_UC, AHEAD\n"      // 5
                 " MOV R2, #(L2 - L1)\n"     // 6: forward distance > 15, long form
                 "L1: DSB 20\nL2:\n"         // 7, 8
                 "AHEAD: NOP\n"              // 9
                 "P ENDP\nS ENDS\nFWD EQU 5\n END\n");
    CHECK(r.ok());
    CHECK(code(r, 4) == "E051");
    CHECK(code(r, 5) == "0D0C");
    CHECK(code(r, 6) == "E6F21400");
    CHECK(r.passes >= 2);
}

TEST_CASE("EQU with a forward reference is an error but still defines the symbol") {
    auto r = run("A EQU B + 1\nB EQU 1\nS SECTION CODE AT 0C000H\nP PROC NEAR\n MOV R1, #A\nP ENDP\nS ENDS\n END\n");
    CHECK(diags(r, 1) == "A71E");
    CHECK(code(r, 5) == "E021");
    CHECK(diags(r, 5) == "");
}

TEST_CASE("short immediates from labels depend on the section") {
    auto r = run("$MOD167\nS SECTION CODE AT 0\nP PROC NEAR\n"
                 "L0: NOP\nL1: NOP\n"   // 4, 5
                 " MOV R1, #L1\n"       // 6: relocatable
                 " MOV R1, #L1 - 1\n"   // 7: a plain number in an AT section
                 " MOV R1, #L1 + 0\n"   // 8
                 "P ENDP\nS ENDS\n"
                 "R1S SECTION CODE\nQ PROC NEAR\n"
                 "M0: NOP\nM1: NOP\n"  // 13, 14
                 " MOV R1, #M1 - M0\n"  // 15
                 " MOV R1, #M1 - 1\n"   // 16: still relocatable
                 "Q ENDP\nR1S ENDS\n END\n");
    CHECK(r.ok());
    CHECK(code(r, 6) == "E6F10200");
    CHECK(code(r, 7) == "E011");
    CHECK(code(r, 8) == "E6F10200");
    CHECK(code(r, 15) == "E021");
    CHECK(code(r, 16).substr(0, 4) == "E6F1");
}

TEST_CASE("EXT sequences select the SFR space") {
    // Short (8-bit) register and bit addresses reach the extended SFRs only
    // inside an EXTR sequence.
    auto r = run("$MOD167\nS SECTION CODE AT 0C000H\nP PROC NEAR\n"
                 " BSET 0F100H.1\n"   // 4: ESFR outside a sequence
                 " EXTR #2\n"
                 " BSET 0F100H.1\n"   // 6: fine
                 " BSET 0FF10H.0\n"   // 7: normal SFR inside the sequence
                 " BSET 0F100H.1\n"   // 8: sequence is over
                 " MOV R1, 0F100H\n"  // 9: long addresses need no sequence
                 "P ENDP\nS ENDS\n END\n");
    CHECK(code(r, 4) == "1F80");
    CHECK(diags(r, 4) == "A111W");
    CHECK(code(r, 6) == "1F80");
    CHECK(diags(r, 6) == "");
    CHECK(diags(r, 7) == "A110W");
    CHECK(diags(r, 8) == "A111W");
    CHECK(code(r, 9) == "F2F100F1");
    CHECK(diags(r, 9) == "");
}

TEST_CASE("data directives") {
    auto r = run("S SECTION DATA AT 1000H\n"
                 " DB 1, 2, 3\n"          // 2
                 " DB 'ABC', 0\n"         // 3
                 " DB 256\n"              // 4
                 " DB -129\n"             // 5
                 " EVEN\n"                // 6
                 " DW 'AB', 1234H\n"      // 7
                 " DW 10000H\n"           // 8
                 " DD 12345678H\n"        // 9
                 " DSB 3\n"               // 10
                 "S ENDS\n END\n");
    CHECK(code(r, 2) == "010203");
    CHECK(code(r, 3) == "41424300");
    CHECK(code(r, 4) == "00");
    CHECK(diags(r, 4) == "A62W");
    CHECK(code(r, 5) == "7F");
    CHECK(diags(r, 5) == "");
    CHECK(code(r, 7) == "42413412");
    CHECK(code(r, 8) == "0000");
    CHECK(diags(r, 8) == "A62W");
    CHECK(code(r, 9) == "78563412");
    auto img = flat_image(r);
    CHECK(img.base == 0x1000);
    // DSB reserves space without emitting bytes.
    CHECK(test::hex(img.bytes) == "01020341424300007FFF424134120000" "78563412");
}

namespace {

std::int64_t address_of(const AssemblyResult& r, int line) {
    for (const auto& l : r.listing)
        if (l.line == line && l.has_address) return l.address;
    return -1;
}

std::int64_t symbol(const AssemblyResult& r, std::string_view name) {
    for (const auto& s : r.symbols)
        if (s.name == name) return s.value.num;
    return -1;
}

}  // namespace

TEST_CASE("word data is aligned, padding is not written") {
    auto r = run("$MOD167\nS SECTION CODE AT 0C000H\nP PROC NEAR\n"
                 " DB 1\n"        // 4: C000
                 " DW 2\n"        // 5: C002
                 " DB 3\n"        // 6: C004
                 " DD 4\n"        // 7: C008
                 " DB 5\n"        // 8: C00C
                 "T1: DW 6\n"     // 9: label takes the aligned address
                 " DB 7\n"        // 10: C010
                 "T2:\n"          // 11: a label alone is not aligned
                 " DSW 1\n"       // 12: C012
                 " DB 8\n"        // 13: C014
                 "T3: DSD 1\n"    // 14: C018
                 "P ENDP\nS ENDS\n END\n");
    CHECK(r.ok());
    CHECK(address_of(r, 5) == 0xC002);
    CHECK(address_of(r, 7) == 0xC008);
    CHECK(address_of(r, 9) == 0xC00E);
    CHECK(symbol(r, "T1") == 0xC00E);
    CHECK(symbol(r, "T2") == 0xC011);
    CHECK(address_of(r, 12) == 0xC012);
    CHECK(symbol(r, "T3") == 0xC018);
    auto img = flat_image(r, std::nullopt, 0xEE);
    CHECK(test::hex(img.bytes).substr(0, 24) == "01EE020003EEEEEE04000000");
}

TEST_CASE("labels at odd addresses") {
    // Keil warns (A48) when a NEAR label, a PROC or a LABEL of a word type
    // is not aligned. Labels in front of DB and labels in data sections are
    // byte labels.
    auto r = run("$MOD167\nS SECTION CODE AT 0C000H\nP PROC NEAR\n"
                 " DB 1\n"
                 "A1:\n"                  // 5: A48
                 "A2: DB 2\n"             // 6: fine
                 "A3 LABEL BYTE\n"        // 7: fine
                 "A4 LABEL WORD\n"        // 8: fine (C002)
                 " DB 3\n"
                 "A5 LABEL NEAR\n"        // 10: A48
                 "A6 LABEL FAR\n"         // 11: A48
                 " DB 4\n"
                 "A7 LABEL DWORD\n"       // 13: C004, fine
                 " DW 5\n"
                 "A8 LABEL DWORD\n"       // 15: C006, A48
                 " DB 6\n"
                 "P ENDP\n"
                 "Q PROC NEAR\n"          // 18: A48
                 "Q ENDP\nS ENDS\n"
                 "D SECTION DATA AT 0E001H\nD1:\nD ENDS\n END\n");  // 22: fine
    CHECK(diags(r, 5) == "A48W");
    CHECK(diags(r, 6) == "");
    CHECK(diags(r, 7) == "");
    CHECK(diags(r, 8) == "");
    CHECK(diags(r, 10) == "A48W");
    CHECK(diags(r, 11) == "A48W");
    CHECK(diags(r, 13) == "");
    CHECK(diags(r, 15) == "A48W");
    CHECK(diags(r, 18) == "A48W");
    CHECK(diags(r, 22) == "");
}

TEST_CASE("data keeps going after a bad element") {
    // An undefined name keeps its place; other bad elements are dropped.
    auto r = run("S SECTION DATA AT 1000H\n"
                 " DB 1, NOPE, 2, 1/0, 3, (4, 5\n"
                 " DW 6\n"
                 "S ENDS\n END\n");
    CHECK(code(r, 2) == "0100020305");
    CHECK(diags(r, 2) == "A42E A24E A22E");
    CHECK(address_of(r, 3) == 0x1006);
}

TEST_CASE("ORG, sections and alignment") {
    auto r = run("$MOD167\nS SECTION CODE AT 0C000H\nP PROC NEAR\n NOP\n ORG 0C010H\nX: NOP\n ORG 0BF00H\n"
                 "P ENDP\nS ENDS\n END\n");
    CHECK(r.listing[5].address == 0xC010);
    CHECK(diags(r, 7) == "A132E");
    auto odd = run("S SECTION CODE AT 0C001H\nP PROC NEAR\n NOP\nP ENDP\nS ENDS\n END\n");
    // Keil leaves odd code addresses to its linker; openc166 reports them.
    CHECK(diags(odd, 3) == "A134E");
}

TEST_CASE("symbol errors") {
    auto r = run("X EQU 1\nX EQU 2\nPSW EQU 5\nMOV: \nS SECTION CODE AT 0C000H\nP PROC NEAR\n MOV R1, #NOPE\n"
                 "P ENDP\nS ENDS\n END\n");
    CHECK(diags(r, 2) == "A25E");
    CHECK(diags(r, 3) == "A25E");
    CHECK(diags(r, 4).rfind("A4E", 0) == 0);
    CHECK(diags(r, 7) == "A42E");
    CHECK_FALSE(r.ok());
    CHECK(r.error_count() == 4);
}

TEST_CASE("missing END") {
    auto r = run("S SECTION CODE AT 0C000H\nP PROC NEAR\n NOP\nP ENDP\nS ENDS\n");
    CHECK(r.ok());
    CHECK(r.warning_count() == 1);
    CHECK(r.diagnostics[0].code == "A89");
}

TEST_CASE("CPU selection") {
    CHECK(run("$MOD167\n END\n").cpu == isa::Cpu::C167);
    CHECK(run(" END\n", isa::Cpu::C166).cpu == isa::Cpu::C166);
    AssembleOptions o;
    o.cpu = isa::Cpu::C167;
    CHECK(assemble(" EXTR #1\n END\n", o).ok());
}

TEST_CASE("flat image layout") {
    auto r = run("A SECTION CODE AT 0C000H\nP PROC NEAR\n NOP\nP ENDP\nA ENDS\n"
                 "B SECTION CODE AT 0C006H\nQ PROC NEAR\n RET\nQ ENDP\nB ENDS\n END\n");
    REQUIRE(r.ok());
    auto img = flat_image(r);
    CHECK(img.base == 0xC000);
    CHECK(test::hex(img.bytes) == "CC00FFFFFFFFCB00");
    auto zero = flat_image(r, 0xBFFE, 0x00);
    CHECK(test::hex(zero.bytes) == "0000CC0000000000CB00");
    CHECK_THROWS_AS(flat_image(r, 0xC002), std::invalid_argument);
}

TEST_CASE("code outside sections starts at the origin") {
    AssembleOptions o;
    o.origin = 0x8000;
    auto r = assemble(" NOP\n RET\n END\n", o);
    REQUIRE(r.ok());
    auto img = flat_image(r);
    CHECK(img.base == 0x8000);
    CHECK(test::hex(img.bytes) == "CC00CB00");
}

TEST_CASE("diagnostic format") {
    AssembleOptions o;
    o.file_name = "x.a66";
    auto r = assemble(" MOV R1, #NOPE\n END\n", o);
    REQUIRE(r.diagnostics.size() == 1);
    CHECK(format_diagnostic(r.diagnostics[0]).rfind("x.a66:1:", 0) == 0);
    CHECK(format_diagnostic(r.diagnostics[0]).find("error A42") != std::string::npos);
}
