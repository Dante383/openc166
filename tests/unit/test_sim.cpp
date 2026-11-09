// SPDX-License-Identifier: MIT
// The simulator core: instruction semantics, traps, interrupts, watchdog
// and snapshots. Programs are assembled with openc166 and run on a plain
// C167 board with the code as ROM at address 0.
#include "doctest.h"
#include "helpers.hpp"

#include "sim/boards.hpp"
#include "sim/machine.hpp"

#include <memory>

using namespace oc166::sim;

namespace {

// Assembles 'body' as code at 0 (after a $MOD167) and returns a machine
// with it as ROM. 'tail' is appended after the code section.
std::unique_ptr<Machine> load(const std::string& body, const std::string& tail = "") {
    std::string src = "$MOD167\n$SEGMENTED\n$INCLUDE(REG167.INC)\nCODE0 SECTION CODE AT 0\n" + body + "\nDONE: JMPR cc_UC, DONE\nCODE0 ENDS\n" +
                      tail + "\n END\n";
    auto r = test::run(src);
    if (!r.ok())
        for (const auto& d : r.diagnostics) MESSAGE(oc166::format_diagnostic(d));
    REQUIRE(r.ok());
    auto img = oc166::flat_image(r, 0);
    img.bytes.resize(0x10000, 0xFF);
    return std::make_unique<Machine>(simple_board(img.bytes));
}

// Runs until the CPU loops on DONE (a jump to itself) or 'limit' steps.
void finish(Machine& m, int limit = 100000) {
    for (int i = 0; i < limit; ++i) {
        const Addr before = m.pc();
        m.step();
        if (m.pc() == before && !m.idle()) {
            auto insn = m.decode_at(before);
            if (insn && insn->mnemonic() == "JMPR") return;
        }
    }
    FAIL("program did not reach DONE");
}

std::uint16_t psw_flags(Machine& m) { return m.psw() & 0x1F; }
constexpr std::uint16_t N = 1, C = 2, V = 4, Z = 8, E = 16;

}  // namespace

TEST_CASE("sim: reset state") {
    auto m = load("NOP");
    CHECK(m->pc() == 0);
    CHECK(m->sfr_get(reg::SP) == 0xFC00);
    CHECK(m->sfr_get(reg::CP) == 0xFC00);
    CHECK(m->sfr_get(reg::STKOV) == 0xFA00);
    CHECK(m->sfr_get(reg::DPP0 + 6) == 3);
    CHECK(m->sfr_get(reg::ONES) == 0xFFFF);
    CHECK((m->sfr_get(reg::WDTCON) & 0x1E) == 0x1C);  // LHWR, SHWR, SWR after power on
}

TEST_CASE("sim: word arithmetic and flags") {
    struct Case {
        const char* code;
        std::uint16_t r4;
        std::uint16_t flags;
    } cases[] = {
        {"MOV R4, #1\nADD R4, #DATA16 0FFFFH", 0x0000, Z | C},
        {"MOV R4, #7FFFH\nADD R4, #1", 0x8000, N | V},
        {"MOV R4, #5\nSUB R4, #6", 0xFFFF, N | C},
        {"MOV R4, #8000H\nSUB R4, #1", 0x7FFF, V},
        {"MOV R4, #3\nMOV R5, #8000H\nADD R4, R5", 0x8003, N | E},
        {"MOV R4, #0F0FH\nAND R4, #00FFH", 0x000F, 0},
        {"MOV R4, #0F0FH\nXOR R4, #0F0FH", 0x0000, Z},
        {"MOV R4, #1234H\nCPL R4", 0xEDCB, N},
        {"MOV R4, #1\nNEG R4", 0xFFFF, N | C},
        {"MOV R4, #0\nNEG R4", 0x0000, Z},
        {"MOV R4, #8000H\nNEG R4", 0x8000, N | V | C | E},
        {"MOV R4, #5\nCMP R4, #5", 0x0005, Z},
    };
    for (const auto& c : cases) {
        CAPTURE(c.code);
        auto m = load(c.code);
        finish(*m);
        CHECK(m->gpr(4) == c.r4);
        CHECK(psw_flags(*m) == c.flags);
    }
}

TEST_CASE("sim: 32-bit add and subtract with carry") {
    // 0001FFFFh + 00000001h = 00020000h; Z of ADDC stays clear because the
    // low word was zero but the high word is not.
    auto m = load("MOV R4, #0FFFFH\nMOV R5, #1\nADD R4, #1\nADDC R5, #0");
    finish(*m);
    CHECK(m->gpr(4) == 0x0000);
    CHECK(m->gpr(5) == 0x0002);
    CHECK(psw_flags(*m) == 0);
    // ADDC keeps Z only if the result is zero and Z was set.
    m = load("MOV R4, #0FFFFH\nMOV R5, #0FFFFH\nADD R4, #1\nADDC R5, #0");
    finish(*m);
    CHECK(m->gpr(5) == 0);
    CHECK(psw_flags(*m) == (Z | C));
}

TEST_CASE("sim: byte operations and GPR byte layout") {
    auto m = load("MOV R4, #1234H\nMOVB RL4, #0FFH\nADDB RL4, #1\nMOVB RH5, RH4");
    finish(*m);
    CHECK(m->gpr(4) == 0x1200);
    CHECK(m->gpr(5) == 0x1200);
    CHECK(psw_flags(*m) == C);  // MOVB sets N, Z and E from 12H; C stays from ADDB
    m = load("MOVB RL4, #80H\nMOVBS R5, RL4\nMOVBZ R6, RL4");
    finish(*m);
    CHECK(m->gpr(5) == 0xFF80);
    CHECK(m->gpr(6) == 0x0080);
    CHECK(psw_flags(*m) == E);  // MOVBZ: N cleared, E set for 80h
}

TEST_CASE("sim: multiply and divide") {
    auto m = load("MOV R4, #0FFFEH\nMOV R5, #3\nMUL R4, R5\nMOV R6, MDH\nMOV R7, MDL");
    finish(*m);
    CHECK(m->gpr(6) == 0xFFFF);
    CHECK(m->gpr(7) == 0xFFFA);
    m = load("MOV R4, #0FFFFH\nMULU R4, R4\nMOV R6, MDH\nMOV R7, MDL");
    finish(*m);
    CHECK(m->gpr(6) == 0xFFFE);
    CHECK(m->gpr(7) == 0x0001);
    m = load("MOV R4, #100\nMOV MDL, R4\nMOV R5, #7\nDIVU R5\nMOV R6, MDL\nMOV R7, MDH");
    finish(*m);
    CHECK(m->gpr(6) == 14);
    CHECK(m->gpr(7) == 2);
    m = load("MOV R4, #0FFF9H\nMOV MDL, R4\nMOV R5, #2\nDIV R5\nMOV R6, MDL\nMOV R7, MDH");
    finish(*m);
    CHECK(m->gpr(6) == 0xFFFD);  // -7 / 2 = -3 rest -1
    CHECK(m->gpr(7) == 0xFFFF);
    m = load("MOV R4, #1\nMOV MDH, R4\nMOV R4, #0\nMOV MDL, R4\nMOV R5, #10H\nDIVLU R5\nMOV R6, MDL");
    finish(*m);
    CHECK(m->gpr(6) == 0x1000);  // 10000h / 10h
    m = load("MOV R5, #0\nDIVU R5");
    finish(*m);
    CHECK((psw_flags(*m) & V) == V);
}

TEST_CASE("sim: shifts and rotates") {
    auto m = load("MOV R4, #8001H\nSHL R4, #1");
    finish(*m);
    CHECK(m->gpr(4) == 0x0002);
    CHECK(psw_flags(*m) == C);
    m = load("MOV R4, #0007H\nSHR R4, #2");
    finish(*m);
    CHECK(m->gpr(4) == 0x0001);
    CHECK(psw_flags(*m) == (C | V));  // bit 1 into C, bit 0 shifted through it
    m = load("MOV R4, #8000H\nASHR R4, #4");
    finish(*m);
    CHECK(m->gpr(4) == 0xF800);
    m = load("MOV R4, #8001H\nROL R4, #1");
    finish(*m);
    CHECK(m->gpr(4) == 0x0003);
    CHECK(psw_flags(*m) == C);
    m = load("MOV R4, #0001H\nROR R4, #1");
    finish(*m);
    CHECK(m->gpr(4) == 0x8000);
    CHECK(psw_flags(*m) == (N | C));
    m = load("MOV R4, #0100H\nPRIOR R5, R4");
    finish(*m);
    CHECK(m->gpr(5) == 7);
}

TEST_CASE("sim: bit instructions") {
    auto m = load("MOV R4, #0\nBSET R4.3\nBMOV R4.15, R4.3\nBCLR R4.3");
    finish(*m);
    CHECK(m->gpr(4) == 0x8000);
    CHECK(psw_flags(*m) == N);  // BCLR of a set bit
    m = load("MOV R4, #1\nBAND R4.0, R4.1");
    finish(*m);
    CHECK(m->gpr(4) == 0);
    CHECK(psw_flags(*m) == (V | N));
    m = load("MOV R4, #1234H\nBFLDH R4, #0F0H, #0A0H\nBFLDL R4, #0FH, #0FH");
    finish(*m);
    CHECK(m->gpr(4) == 0xA23F);
    // Bit-addressable RAM at 0FD00H.
    m = load("BSET 0FD02H.5\nMOV R4, 0FD02H");
    finish(*m);
    CHECK(m->gpr(4) == 0x0020);
    m = load("MOV R4, #4\nJBC R4.2, L1\nMOV R5, #1\nL1: JNB R4.2, L2\nMOV R5, #2\nL2: NOP");
    finish(*m);
    CHECK(m->gpr(4) == 0);
    CHECK(m->gpr(5) == 0);
}

TEST_CASE("sim: addressing modes") {
    auto m = load(
        "MOV R0, #0FC40H\n"
        "MOV R4, #1111H\nMOV [R0], R4\n"
        "MOV R4, #2222H\nMOV [-R0], R4\n"  // R0 = 0FC3EH
        "MOV R5, [R0+]\n"                  // 2222H, R0 = 0FC40H
        "MOV R6, [R0]\n"                   // 1111H
        "MOV R7, [R0+#-2]\n"               // 2222H
        "MOV R1, #0FC40H\nADD R6, [R1+]\n"  // R6 = 2222H, R1 = 0FC42H
        "MOV R8, 0FC3EH\n");
    finish(*m);
    CHECK(m->gpr(0) == 0xFC40);
    CHECK(m->gpr(5) == 0x2222);
    CHECK(m->gpr(6) == 0x2222);
    CHECK(m->gpr(7) == 0x2222);
    CHECK(m->gpr(1) == 0xFC42);
    CHECK(m->gpr(8) == 0x2222);
}

TEST_CASE("sim: CMPI and CMPD loops") {
    auto m = load("MOV R4, #0\nL: CMPI1 R4, #9\nJMPR cc_NE, L");
    finish(*m);
    CHECK(m->gpr(4) == 10);
}

TEST_CASE("sim: register banks through CP") {
    auto m = load("MOV R0, #1\nMOV CP, #0FB00H\nMOV R0, #2\nMOV R1, 0FC00H");
    finish(*m);
    CHECK(m->gpr(0) == 2);
    CHECK(m->gpr(1) == 1);
    CHECK(m->peek16(0xFB00) == 2);
}

TEST_CASE("sim: calls, returns and the stack") {
    auto m = load(
        "MOV R4, #0\nCALLA cc_UC, SUB1\nCALLR SUB1\nCALLS 0, SUB2\nPUSH R4\nPOP R5\nJMPR cc_UC, DONE\n"
        "SUB1: ADD R4, #1\nRET\n"
        "SUB2: ADD R4, #10H\nRETS\n");
    finish(*m);
    CHECK(m->gpr(4) == 0x12);
    CHECK(m->gpr(5) == 0x12);
    CHECK(m->sfr_get(reg::SP) == 0xFC00);
}

TEST_CASE("sim: DPP paging and EXTP/EXTS") {
    auto m = load(
        "MOV DPP1, #3\n"             // page 3 (0C000H..0FFFFH) through DPP1
        "MOV R4, #5A5AH\nMOV 7C00H, R4\n"  // 7C00H via DPP1 -> 0FC00H
        "EXTP #3, #1\nMOV R5, 3C02H\n"     // page 3: 0FC02H
        "EXTS #0, #1\nMOV R6, 0FC00H\n");
    finish(*m);
    CHECK(m->peek16(0xFC00) == 0x5A5A);
    CHECK(m->gpr(6) == 0x5A5A);
}

TEST_CASE("sim: undefined opcode and stack overflow traps") {
    // An undefined opcode enters the class B trap at 28H with UNDOPC set.
    auto m = load("DW 008BH\nNOP");  // 8BH is not an opcode
    m->run(1);  // the undefined word
    m->step();  // enters the trap
    CHECK(m->pc() == 0x28);
    CHECK(m->sfr_get(reg::TFR) == 0x0080);
    // Stack overflow traps to 10H after the push.
    m = load("MOV STKOV, #0FBFEH\nPUSH R0\nPUSH R0\n");
    m->run(3);
    m->step();
    CHECK(m->pc() == 0x10);
    CHECK((m->sfr_get(reg::TFR) & 0x4000) != 0);
    CHECK((m->psw() >> 12) == 15);
}

TEST_CASE("sim: TRAP and RETI") {
    auto m = load("BSET IEN\nMOV R4, #0\nTRAP #20H\nADD R4, #1\nJMPR cc_UC, DONE\n",
                  "VEC SECTION CODE AT 80H\nADD R4, #10H\nRETI\nVEC ENDS\n");
    finish(*m);
    CHECK(m->gpr(4) == 0x11);
    CHECK((m->psw() & 0x800) != 0);
    CHECK(m->sfr_get(reg::SP) == 0xFC00);
}

TEST_CASE("sim: interrupt priorities and EXT sequences") {
    // Two requests: T3 at level 5 and T2 at level 9. The handler for T2
    // runs first; each records its order.
    auto m = load(
        "MOV R4, #0\n"
        "MOV T3IC, #0D4H\n"  // IR, IE, ILVL 5
        "MOV T2IC, #0E4H\n"  // IR, IE, ILVL 9
        "ATOMIC #2\nBSET IEN\nNOP\n"
        "NOP\nNOP\n",
        "V2 SECTION CODE AT 88H\nJMPR cc_UC, H2\nV2 ENDS\n"
        "V3 SECTION CODE AT 8CH\nJMPR cc_UC, H3\nV3 ENDS\n"
        "HND SECTION CODE AT 100H\nH2: SHL R4, #4\nADD R4, #2\nRETI\n"
        "H3: SHL R4, #4\nADD R4, #3\nRETI\nHND ENDS\n");
    finish(*m);
    CHECK(m->gpr(4) == 0x23);
    CHECK((m->sfr_get(0xFF60) & 0x80) == 0);
}

TEST_CASE("sim: PEC transfer") {
    // Level 14 group 0 uses PEC channel 0: one word moves per request.
    auto m = load(
        "MOV R4, #1234H\nMOV 0FC80H, R4\n"
        "MOV R4, #0FC80H\nMOV 0FCE0H, R4\n"  // SRCP0
        "MOV R4, #0FC90H\nMOV 0FCE2H, R4\n"  // DSTP0
        "MOV PECC0, #0202H\n"                // word, increment destination, count 2
        "BSET IEN\nMOV T2IC, #0F8H\nNOP\nNOP\n");
    finish(*m);
    CHECK(m->peek16(0xFC90) == 0x1234);
    CHECK(m->peek16(0xFCE2) == 0xFC92);
    CHECK((m->sfr_get(reg::PECC0) & 0xFF) == 1);
    CHECK((m->sfr_get(0xFF60) & 0x80) == 0);  // the request was served
}

TEST_CASE("sim: watchdog resets the CPU unless serviced or disabled") {
    auto m = load("L: JMPR cc_UC, L");
    m->run(70000);  // 65536 * 2 cycles at fCPU/2
    CHECK((m->sfr_get(reg::WDTCON) & 0x2) == 0x2);  // WDTR
    m = load("DISWDT\nEINIT\nL: JMPR cc_UC, L");
    m->run(100000);
    CHECK((m->sfr_get(reg::WDTCON) & 0x2) == 0);
    CHECK(m->instructions() == 100000);
}

TEST_CASE("sim: SRST and EINIT reset flags") {
    auto m = load("EINIT\nSRST");
    m->run(2);
    CHECK(m->pc() == 0);
    CHECK((m->sfr_get(reg::WDTCON) & 0x1E) == 0x04);  // SWR only
    m = load("EINIT");
    m->run(1);
    CHECK((m->sfr_get(reg::WDTCON) & 0x1C) == 0);
}

TEST_CASE("sim: snapshots restore the exact state") {
    auto m = load("MOV R4, #0\nL: ADD R4, #3\nPUSH R4\nPOP R5\nJMPR cc_UC, L");
    m->run(100);
    const Snapshot s = m->save();
    m->run(1000);
    const std::uint16_t r4 = m->gpr(4);
    const std::uint64_t cycles = m->cycles();
    m->load(s);
    CHECK(m->instructions() == 100);
    m->run(1000);
    CHECK(m->gpr(4) == r4);
    CHECK(m->cycles() == cycles);
}

TEST_CASE("sim: unmodelled SFRs are logged once") {
    auto m = load("MOV R4, 0FEB0H\nMOV R4, 0FEB0H\nMOV 0FEB0H, R4");
    finish(*m);
    REQUIRE(m->unmodelled().size() == 2);
    CHECK(m->unmodelled()[0].address == 0xFEB0);
    CHECK(!m->unmodelled()[0].write);
    CHECK(m->unmodelled()[0].pc == 0);
    CHECK(m->unmodelled()[1].write);
}

TEST_CASE("sim: chip selects route the external bus") {
    Board b = simple_board(std::vector<std::uint8_t>(0x10000, 0xFF));
    auto latch = std::make_shared<Latch>("latch");
    b.chip_selects[3] = latch;
    Machine m(std::move(b));
    m.sfr_set(reg::ADDRSEL1 + 4, 0x00A0);  // ADDRSEL3: 4 KB at 0A000H
    m.sfr_set(reg::BUSCON1 + 4, 0x0400);   // BUSACT3
    m.write8(0xA000, 0x5A);
    CHECK(latch->value == 0x5A);
    CHECK(m.describe(0xA000) == "CS3:latch");
    CHECK(m.describe(0xB000) == "CS0:rom");
}
