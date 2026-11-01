// SPDX-License-Identifier: MIT
// Instruction encodings. Every expected value below was produced by Keil
// A166 V5.38 for the same statement (see tests/diff); these tests keep them
// fixed without needing Keil.
#include "doctest.h"
#include "helpers.hpp"

#include <utility>

using test::one;
using oc166::isa::Cpu;

TEST_CASE("encodings match Keil A166") {
    static const std::pair<const char*, const char*> kCases[] = {
        // Short immediate forms only for values that fit without sign.
        {"ADD R2, #3", "0823"},
        {"ADD R2, #7", "0827"},
        {"ADD R2, #8", "06F20800"},
        {"ADD R2, #-1", "06F2FFFF"},
        {"MOV R1, #0", "E001"},
        {"MOV R1, #15", "E0F1"},
        {"MOV R1, #16", "E6F11000"},
        {"MOV R1, #-1", "E6F1FFFF"},
        {"MOV R1, #10000H", "E6F10000 A62W"},
        {"MOVB RL1, #255", "E7F2FF00"},
        {"MOVB RL1, #256", "E7F20000 A62W"},
        {"SHL R1, #16", "5C01 A62W"},
        {"TRAP #7FH", "9BFE"},
        {"TRAP #80H", "9B00 A62W"},
        {"EXTR #4", "D1B0"},
        {"EXTR #0", "D180 A19E"},
        {"EXTP #0FFFFH, #1", "D740FF3F A62W"},
        {"EXTS #100H, #1", "D7000000 A62W"},
        {"ATOMIC #2", "D110"},
        // Bits and bit fields.
        {"BSET 0FDFFH.7", "FF7F"},
        {"BSET 0F100H.1", "1F80 A111W"},
        {"BCLR 0FD80H.15", "FE40"},
        {"BMOV C, Z", "4A888831"},
        {"BFLDL PSW, #0FH, #05H", "0A880F05"},
        {"BFLDH R3, #0F0H, #50H", "1AF350F0"},
        {"BFLDL 0FD01H, #1, #2", "0A000102"},
        // Registers, memory and indirect addressing.
        {"MOVB RL1, PSW", "F3F210FF"},
        {"MOV R1, SRCP0", "F2F1E0FC"},
        {"PUSH PSW", "EC88"},
        {"MOV [R2+#-2], R3", "C432FEFF"},
        {"MOV R3, [R2+#1234H]", "D4323412"},
        {"MOV [-R0], R1", "8810"},
        {"MOV R1, [R0+]", "9810"},
        {"MOV [R1], [R2+]", "E812"},
        {"MOV 0FA00H, [R2]", "940200FA"},
        {"ADD R1, [R3]", "081B"},
        {"MOVBZ R1, RL2", "C041"},
        {"MUL R1, R2", "0B12"},
        {"DIVU R3", "5B33"},
        {"CMPD2 R1, #1234H", "B6F13412"},
        // Jumps and calls.
        {"JMPA cc_UC, 0C000H", "EA0000C0"},
        {"CALLA cc_Z, 0C010H", "CA2010C0"},
        {"JMPI cc_NE, [R4]", "9C34"},
        {"JMPS 1, 2000H", "FA010020"},
        {"CALLS 0, 0C000H", "DA0000C0"},
        {"PCALL R1, 0C000H", "E2F100C0"},
        {"SCXT PSW, 0FA00H", "D68800FA"},
        {"RETP R5", "EBF5"},
        // Fixed encodings.
        {"SRST", "B748B7B7"},
        {"IDLE", "87788787"},
        {"DISWDT", "A55AA5A5"},
        {"EINIT", "B54AB5B5"},
        {"NOP", "CC00"},
        // Errors.
        {"ADD R1, [R4]", "A57E"},
        {"MOV R1", "A73E"},
        {"ADD R1, R2, R3", "A73E"},
        {"MOVB R1, #1", "A74E"},
    };
    for (const auto& [statement, expected] : kCases) {
        CAPTURE(statement);
        CHECK(one(statement) == expected);
    }
}

TEST_CASE("C167-only instructions need $MOD167") {
    CHECK(one("ATOMIC #2", Cpu::C166) == "A108E");
    CHECK(one("EXTP R1, #1", Cpu::C166) == "A108E");
    CHECK(one("SCXT R1, #1", Cpu::C166) == "C6F10100");
}

TEST_CASE("relocatable immediates use the long form") {
    // In Keil, a label is relocatable even in an absolute section.
    auto r = test::run("$MOD167\nS SECTION CODE AT 0\nP PROC NEAR\nL0: NOP\n MOV R1, #L0\n MOV R1, #L0-L0\n"
                       " MOV R1, #SEG L0\n MOV R1, #LOW L0\n EXTR #L0\nP ENDP\nS ENDS\n END\n");
    CHECK(test::code(r, 5) == "E6F10000");
    CHECK(test::code(r, 6) == "E001");
    CHECK(test::code(r, 7) == "E6F10000");
    CHECK(test::code(r, 8) == "E001");
    CHECK(test::diags(r, 9) == "A72E");
}
