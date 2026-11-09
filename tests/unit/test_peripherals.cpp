// SPDX-License-Identifier: MIT
// On-chip peripherals: ASC0, the CAPCOM timers and the A/D converter.
#include "doctest.h"
#include "helpers.hpp"

#include "sim/debug.hpp"
#include "sim/peripherals.hpp"

#include <memory>

using namespace oc166::sim;

namespace {

std::unique_ptr<Machine> load(const std::string& body) {
    std::string src = "$MOD167\n$SEGMENTED\n$INCLUDE(REG167.INC)\nCODE0 SECTION CODE AT 0\nDISWDT\nEINIT\n" + body +
                      "\nDONE: JMPR cc_UC, DONE\nCODE0 ENDS\n END\n";
    auto r = test::run(src);
    if (!r.ok())
        for (const auto& d : r.diagnostics) MESSAGE(oc166::format_diagnostic(d));
    REQUIRE(r.ok());
    auto img = oc166::flat_image(r, 0);
    img.bytes.resize(0x10000, 0xFF);
    Board b = simple_board(img.bytes);
    add_c167_peripherals(b);
    return std::make_unique<Machine>(std::move(b));
}

// 10416 baud at 25 MHz: S0BG = 74, 8N1.
const char* kAscInit = "MOV S0BG, #74\nMOV S0CON, #8011H\n";

}  // namespace

TEST_CASE("ASC0: transmit timing and output") {
    auto m = load(std::string(kAscInit) +
                  "MOV S0TBUF, #'O'\n"
                  "W1: EXTR #1\nJNB S0TBIR, W1\nEXTR #1\nBCLR S0TBIR\n"  // S0TBIC is an ESFR
                  "MOV S0TBUF, #'K'\n"
                  "W2: JNB S0TIR, W2\nBCLR S0TIR\n"
                  "W3: JNB S0TIR, W3\n");
    auto* asc = m->find_device<Asc0>();
    REQUIRE(asc);
    // Run until both frames are out: two frames of 10 bits at 32 * 75
    // cycles each, sent back to back.
    std::vector<std::uint8_t> out;
    while (out.size() < 2 && m->cycles() < 100000) {
        m->step();
        for (auto b : asc->take_output()) out.push_back(b);
    }
    CHECK(out == std::vector<std::uint8_t>{'O', 'K'});
    CHECK(asc->frame_cycles(*m) == 10 * 32 * 75);
    CHECK(m->cycles() >= 2 * 24000);
    CHECK(m->cycles() < 2 * 24000 + 100);
}

TEST_CASE("ASC0: receive, overrun and framing error") {
    auto m = load(std::string(kAscInit) + "MOV S0CON, #80D1H\n"  // FEN and OEN too
                  "L: JMPR cc_UC, L\n");
    auto* asc = m->find_device<Asc0>();
    m->run(10);
    asc->receive(*m, 0x81);
    m->run(10000);  // 24000 cycles: one frame
    CHECK(m->sfr_get(0xFEB2) == 0x81);
    CHECK((m->sfr_get(0xFF6E) & 0x80) != 0);  // S0RIR
    asc->receive(*m, 0x55);  // RIR still set: overrun
    m->run(10000);
    CHECK(m->sfr_get(0xFEB2) == 0x81);
    CHECK((m->sfr_get(0xFFB0) & 0x0400) != 0);  // OE
    CHECK((m->sfr_get(0xFF70) & 0x80) != 0);    // S0EIR
    m->sfr_set(0xFF6E, 0);
    asc->receive(*m, 0x00, true);  // a break: framing error
    m->run(10000);
    CHECK((m->sfr_get(0xFFB0) & 0x0200) != 0);  // FE
}

TEST_CASE("ASC0: loopback and K-line echo") {
    auto m = load(std::string(kAscInit) + "MOV S0CON, #0C011H\n"  // LB
                  "MOV S0TBUF, #5AH\nW: JNB S0RIR, W\nMOV R4, S0RBUF\n");
    m->run(20000);
    CHECK(m->gpr(4) == 0x5A);
}

TEST_CASE("ASC0: host input is replayed by reverse execution") {
    auto m = load(std::string(kAscInit) +
                  "MOV R4, #0\n"
                  "L: JNB S0RIR, L\nBCLR S0RIR\nMOV R5, S0RBUF\nADD R4, R5\nJMPR cc_UC, L\n");
    auto* asc = m->find_device<Asc0>();
    Debugger d(*m);
    d.set_checkpoint_interval(500);
    d.cont(1000);
    asc->receive(*m, 1);
    d.cont(20000);
    const std::uint64_t after_first = m->instructions();
    REQUIRE(m->gpr(4) == 1);
    asc->receive(*m, 2);
    d.cont(20000);
    asc->receive(*m, 4);
    d.cont(20000);
    CHECK(m->gpr(4) == 7);
    const std::uint64_t end = m->instructions();
    d.goto_instruction(after_first);  // before the second byte arrived
    CHECK(m->gpr(4) == 1);
    d.goto_instruction(500);  // before the first
    CHECK(m->gpr(4) == 0);
    d.goto_instruction(end);
    CHECK(m->gpr(4) == 7);
    CHECK(asc->take_output().empty());
}

TEST_CASE("CAPCOM timers: T1 counts and overflows with reload") {
    // T1 at fCPU / 2^(4 + 3) = fCPU / 128, reload 0FF00H: overflows every
    // 256 * 128 cycles.
    auto m = load("MOV T1REL, #0FF00H\nMOV T1, #0FF00H\nOR T01CON, #4400H\nL: JMPR cc_UC, L\n");
    m->run(5);
    const std::uint64_t start = m->cycles();
    m->run(4000);  // 16000 cycles
    const std::uint16_t t1 = m->sfr_get(0xFE52);
    (void)t1;
    auto* tm = m->find_device<CapcomTimers>();
    CHECK(tm->value(*m, 1) == 0xFF00 + (m->cycles() - start) / 128);
    CHECK((m->sfr_get(0xFF9E) & 0x80) == 0);
    m->run(5000);  // past 32768 cycles
    CHECK((m->sfr_get(0xFF9E) & 0x80) != 0);  // T1IR
    CHECK(tm->value(*m, 1) >= 0xFF00);
}

TEST_CASE("ADC: single conversion and scan") {
    auto m = load(
        "MOV ADCON, #0081H\nW1: JB ADBSY, W1\nBCLR ADCIR\nMOV R4, ADDAT\n"
        "MOV ADCON, #00A2H\n"  // auto scan from channel 2
        "W2: JNB ADCIR, W2\nBCLR ADCIR\nMOV R5, ADDAT\n"
        "W3: JNB ADCIR, W3\nBCLR ADCIR\nMOV R6, ADDAT\n"
        "W4: JNB ADCIR, W4\nBCLR ADCIR\nMOV R7, ADDAT\n");
    auto* adc = m->find_device<Adc>();
    adc->set_input(0, 0x100);
    adc->set_input(1, 0x2A0);
    adc->set_input(2, 0x3FF);
    m->run(5000);
    CHECK(m->gpr(4) == 0x12A0);
    CHECK(m->gpr(5) == 0x23FF);
    CHECK(m->gpr(6) == 0x12A0);
    CHECK(m->gpr(7) == 0x0100);
    CHECK((m->sfr_get(0xFFA0) & 0x0180) == 0);  // ADST and ADBSY clear
}

TEST_CASE("ASC0: a low pulse is framed at the current baud rate") {
    // 25 ms low. At 250 baud (25 MHz, S0BG = 3124) that is the start bit and
    // five data bits: 0E0H. At 10416 baud it covers the whole frame: 00H
    // with a framing error.
    auto m = load("MOV S0BG, #3124\nMOV S0CON, #8051H\nL: JMPR cc_UC, L\n");
    auto* asc = m->find_device<Asc0>();
    m->run(10);
    asc->receive_low(*m, 625000);
    m->run(10);
    CHECK((m->sfr_get(0xFF6E) & 0x80) == 0);  // the stop bit is 15 ms ahead
    m->run(100000);
    CHECK(m->sfr_get(0xFEB2) == 0xE0);
    CHECK((m->sfr_get(0xFFB0) & 0x0200) == 0);
    m->sfr_set(0xFF6E, 0);
    m->sfr_set(0xFEB4, 74);
    asc->receive_low(*m, 625000);
    m->run(10);
    CHECK(m->sfr_get(0xFEB2) == 0x00);
    CHECK((m->sfr_get(0xFFB0) & 0x0200) != 0);
}
