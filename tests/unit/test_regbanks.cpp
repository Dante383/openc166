// SPDX-License-Identifier: MIT
// Register banks (REGDEF, REGBANK) and the linker directives NAME, SSKDEF,
// PECDEF and TYPEDEC. Expected values match Keil A166 and L166 unless a
// comment says otherwise.
#include "doctest.h"
#include "helpers.hpp"

using namespace oc166;

namespace {

// All diagnostic codes with their file lines, e.g. "2:A87E 3:A25E".
std::string all_diags(const AssemblyResult& r) {
    std::string s;
    for (const auto& d : r.diagnostics) {
        if (!s.empty()) s += ' ';
        s += std::to_string(d.line) + ':' + d.code + (d.severity == Severity::Error ? "E" : "W");
    }
    return s;
}

const SectionInfo* section(const AssemblyResult& r, const std::string& name) {
    for (const auto& s : r.sections)
        if (s.name == name) return &s;
    return nullptr;
}

// 'head' at the top of the file, then 'body' in a procedure at 0C000H.
AssemblyResult with_code(const std::string& head, const std::string& body) {
    return test::run("$MOD167\n" + head + "CS1 SECTION CODE AT 0C000H\nP PROC\n" + body + "P ENDP\nCS1 ENDS\n END\n");
}

}  // namespace

TEST_CASE("a named bank is a DATA section of class *REG* at 0FC00H") {
    auto r = with_code("RB REGBANK R0-R3\n REGDEF R0-R15\nRB2 REGDEF R5, R1\n",
                       " MOV R1, #RB\n MOV R2, #RB2\n MOV R3, RB\n");
    REQUIRE(r.ok());
    const SectionInfo* rb = section(r, "RB");
    REQUIRE(rb);
    CHECK(rb->address == 0xFC00);
    CHECK(rb->size == 8);
    CHECK(rb->class_name == "*REG*");
    // The size runs up to the highest register; banks sit at even addresses.
    const SectionInfo* rb2 = section(r, "RB2");
    REQUIRE(rb2);
    CHECK(rb2->address == 0xFC08);
    CHECK(rb2->size == 12);
    // An unnamed REGDEF makes no bank.
    CHECK(r.sections.size() == 3);
    CHECK(test::code(r, 7) == "E6F100FC");
    CHECK(test::code(r, 8) == "E6F208FC");
    CHECK(test::code(r, 9) == "F2F300FC");
}

TEST_CASE("PAG of a bank is 3; SEG is 0 only in segmented mode") {
    auto flat = with_code("RB REGBANK R0\n", " MOV R1, #PAG RB\n MOV R2, #SEG RB\n");
    REQUIRE(flat.ok());
    CHECK(test::code(flat, 5) == "E031");
    CHECK(test::code(flat, 6) == "E6F20000");
    auto seg = with_code("$SEGMENTED\nRB REGBANK R0\n", " MOV R1, #PAG RB\n MOV R2, #SEG RB\n MOV R3, RB\n");
    REQUIRE(seg.ok());
    CHECK(test::code(seg, 6) == "E031");
    CHECK(test::code(seg, 7) == "E002");
    // No ASSUME needed: the bank is in page 3.
    CHECK(test::code(seg, 8) == "F2F300FC");
}

TEST_CASE("misused bank names") {
    auto r = with_code("$SEGMENTED\nRB REGBANK R0\n PUBLIC RB\n EXTERN RX:REGBANK\n ASSUME DPP2:RB\n"
                       "RB SECTION DATA\nG DGROUP RB\nRB REGDEF R1\n",
                       " BSET RB.1\n MOV R1, #BOF RB\n JMPS RB\n");
    CHECK(all_diags(r) == "4:A39E 5:A113E 6:A56E 7:A25E 8:A25E 9:A5E 12:A69E 13:A61E 14:A73E");
}

TEST_CASE("bank definition errors") {
    CHECK(all_diags(test::run(" REGDEF R3-R1\n END\n")) == "1:A52E");
    CHECK(all_diags(test::run(" REGDEF R16\n END\n")) == "1:A6E");
    CHECK(all_diags(test::run(" REGDEF R0,\n END\n")) == "1:A6E");
    CHECK(all_diags(test::run(" REGDEF\n END\n")) == "1:A6E");
    CHECK(all_diags(test::run("RB REGDEF\n END\n")).empty());
    CHECK(all_diags(test::run("X EQU 1\nX REGBANK R0\n END\n")) == "2:A5E");
}

TEST_CASE("banks fill 0FC00H..0FDFFH; one more is an error") {
    std::string src = "$MOD167\n";
    for (int i = 0; i < 17; ++i) src += "B" + std::to_string(i) + " REGBANK R0-R15\n";
    // L166 reports L123 for the bank that does not fit.
    CHECK(all_diags(test::run(src + " END\n")) == "18:OC28E");
}

TEST_CASE("banks of the same name in several modules become the largest one") {
    auto r = assemble(std::vector<Source>{{"a.a66", "RB REGBANK R0-R1\nRC REGBANK R0\n END\n"},
                                          {"b.a66", "RB REGBANK R0-R7\n END\n"}},
                      {});
    REQUIRE(r.ok());
    REQUIRE(section(r, "RB"));
    CHECK(section(r, "RB")->address == 0xFC00);
    CHECK(section(r, "RB")->size == 16);
    REQUIRE(section(r, "RC"));
    CHECK(section(r, "RC")->address == 0xFC10);
}

TEST_CASE("NAME, SSKDEF, PECDEF and TYPEDEC") {
    CHECK(all_diags(test::run(" NAME FIRST\n NAME SECOND\n END\n")) == "2:A25E");
    CHECK(all_diags(test::run(" NAME MOV\n END\n")) == "1:A6E 1:A23E");
    CHECK(all_diags(test::run(" SSKDEF 3\n END\n", isa::Cpu::C166)).empty());
    CHECK(all_diags(test::run(" SSKDEF 4\n END\n", isa::Cpu::C166)) == "1:A66E");
    CHECK(all_diags(test::run("$MOD167\n SSKDEF 7\n END\n")).empty());
    // An SSKDEF out of range does not count; one with a syntax error does.
    CHECK(all_diags(test::run(" SSKDEF 9\n SSKDEF 1 junk\n SSKDEF 2\n END\n")) == "1:A66E 2:A6E 3:A87E");
    CHECK(all_diags(test::run("X EQU 1\n SSKDEF X\n END\n")).empty());
    CHECK(all_diags(test::run(" SSKDEF Y\nY EQU 1\n END\n")) == "1:A71E");
    CHECK(all_diags(test::run(" SSKDEF R1\n END\n")) == "1:A70E");
    CHECK(all_diags(test::run(" PECDEF PECC7-PECC2, PECC0, PECC0\n END\n")).empty());
    CHECK(all_diags(test::run(" PECDEF PECC8\n PECDEF PECC1\n END\n")) == "1:A88E 2:A87E");
    CHECK(all_diags(test::run(" PECDEF PECC1\n SSKDEF 1\n END\n")).empty());
    CHECK(all_diags(test::run(" TYPEDEC anything, at all\n END\n")).empty());
    CHECK(all_diags(test::run("X TYPEDEC\n END\n")) == "1:A6E");
    CHECK(all_diags(test::run(" COMREG\n END\n")) == "1:A6E");
}
