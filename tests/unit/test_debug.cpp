// SPDX-License-Identifier: MIT
// The debug core: breakpoints, watchpoints, stepping, reverse execution.
#include "doctest.h"
#include "helpers.hpp"

#include "sim/debug.hpp"

#include <memory>

using namespace oc166::sim;

namespace {

struct Program {
    std::unique_ptr<Machine> m;
    oc166::AssemblyResult r;
    Addr at(const std::string& label) const {
        for (const auto& s : r.symbols)
            if (s.name == label) return static_cast<Addr>(s.value.num);
        FAIL("no label " << label);
        return 0;
    }
};

Program load(const std::string& body) {
    Program p;
    std::string src = "$MOD167\n$SEGMENTED\n$INCLUDE(REG167.INC)\nCODE0 SECTION CODE AT 0\nDISWDT\nEINIT\n" + body +
                      "\nDONE: JMPR cc_UC, DONE\nCODE0 ENDS\n END\n";
    p.r = test::run(src);
    if (!p.r.ok())
        for (const auto& d : p.r.diagnostics) MESSAGE(oc166::format_diagnostic(d));
    REQUIRE(p.r.ok());
    auto img = oc166::flat_image(p.r, 0);
    img.bytes.resize(0x10000, 0xFF);
    p.m = std::make_unique<Machine>(simple_board(img.bytes));
    return p;
}

const char* kLoop =
    "MOV R4, #0\n"
    "LOOP: ADD R4, #1\n"
    "MOV 0FC40H, R4\n"
    "CALLR SUB1\n"
    "AFTER: CMP R4, #100\n"
    "JMPR cc_NE, LOOP\n"
    "JMPR cc_UC, DONE\n"
    "SUB1: NOP\n"
    "INNER: NOP\n"
    "RET\n";

}  // namespace

TEST_CASE("debug: breakpoints with conditions") {
    auto p = load(kLoop);
    Debugger d(*p.m);
    const int id = d.add_breakpoint(p.at("AFTER"), [](Machine& m) { return m.gpr(4) == 10; });
    StopInfo s = d.cont();
    CHECK(s.reason == Stop::Breakpoint);
    CHECK(s.id == id);
    CHECK(p.m->pc() == p.at("AFTER"));
    CHECK(p.m->gpr(4) == 10);
    // Continuing from a breakpoint does not stop on it again at once.
    d.remove_breakpoint(id);
    d.add_breakpoint(p.at("AFTER"));
    s = d.cont();
    CHECK(s.reason == Stop::Breakpoint);
    CHECK(p.m->gpr(4) == 11);
}

TEST_CASE("debug: watchpoints") {
    auto p = load(kLoop);
    Debugger d(*p.m);
    const int id = d.add_watchpoint(0xFC40, 2, static_cast<unsigned>(WatchKind::Write));
    StopInfo s = d.cont();
    CHECK(s.reason == Stop::Watchpoint);
    CHECK(s.id == id);
    REQUIRE(s.access);
    CHECK(s.access->address == 0xFC40);
    CHECK(s.access->value == 1);
    // Stops after the instruction that wrote.
    CHECK(p.m->pc() == p.at("LOOP") + 6);

    // A bit: PSW.Z changes.
    auto q = load("MOV R4, #1\nSUB R4, #1\nADD R4, #1\n");
    Debugger d2(*q.m);
    d2.add_watchpoint(reg::PSW, 2, static_cast<unsigned>(WatchKind::Change), 1u << 3);
    s = d2.cont(100);
    CHECK(s.reason == Stop::Watchpoint);
    CHECK((q.m->psw() & 8) != 0);  // Z was just set by SUB
}

TEST_CASE("debug: step, step over, step out") {
    auto p = load(kLoop);
    Debugger d(*p.m);
    const int loop = d.add_breakpoint(p.at("LOOP"));
    d.cont();
    d.remove_breakpoint(loop);
    d.step();  // ADD
    d.step();  // MOV
    CHECK(p.m->pc() == p.at("LOOP") + 6);
    d.step_over();  // CALLR SUB1 runs the whole routine
    CHECK(p.m->pc() == p.at("AFTER"));
    // step out of SUB1 from INNER
    d.add_breakpoint(p.at("INNER"));
    d.cont();
    CHECK(p.m->pc() == p.at("INNER"));
    d.step_out();
    CHECK(p.m->pc() == p.at("AFTER"));
}

TEST_CASE("debug: an EXT sequence is one step") {
    auto p = load("EXTP #3, #2\nMOV R4, 3C40H\nMOV R5, 3C42H\nNOP\n");
    Debugger d(*p.m);
    d.step();  // DISWDT
    d.step();  // EINIT
    const std::uint64_t n = p.m->instructions();
    d.step();
    CHECK(p.m->instructions() == n + 3);
    auto insn = p.m->decode_at(p.m->pc());
    REQUIRE(insn);
    CHECK(insn->mnemonic() == "NOP");
}

TEST_CASE("debug: reverse step and reverse continue") {
    auto p = load(kLoop);
    Debugger d(*p.m);
    d.set_checkpoint_interval(7);  // force replays across checkpoints
    d.add_breakpoint(p.at("AFTER"), [](Machine& m) { return m.gpr(4) == 50; });
    d.cont();
    REQUIRE(p.m->gpr(4) == 50);
    const std::uint64_t n = p.m->instructions();
    const Addr pc = p.m->pc();
    d.step();
    d.step();
    d.reverse_step();
    d.reverse_step();
    CHECK(p.m->instructions() == n);
    CHECK(p.m->pc() == pc);
    CHECK(p.m->gpr(4) == 50);

    // Who last wrote 0FC40H? Run on, then reverse-continue with a write
    // watchpoint.
    d.cont(30);
    const std::uint16_t now = p.m->gpr(4);
    d.add_watchpoint(0xFC40, 2, static_cast<unsigned>(WatchKind::Write));
    StopInfo s = d.reverse_continue();
    CHECK(s.reason == Stop::Watchpoint);
    REQUIRE(s.access);
    CHECK(s.access->value == now);
    CHECK(p.m->pc() == p.at("LOOP") + 6);  // just after the MOV that wrote it

    // Reverse-continue to the start of history.
    Debugger fresh(*p.m);
    s = fresh.reverse_continue();
    CHECK(s.history_start);
}

TEST_CASE("debug: replay is deterministic") {
    auto p = load(kLoop);
    Debugger d(*p.m);
    d.set_checkpoint_interval(5);
    d.cont(200);
    const auto a = p.m->save();
    d.goto_instruction(37);
    CHECK(p.m->instructions() == 37);
    d.goto_instruction(200);
    const auto b = p.m->save();
    CHECK(a.data == b.data);
}
