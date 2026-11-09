// SPDX-License-Identifier: MIT
#include "sim/boards.hpp"

#include <stdexcept>

namespace oc166::sim {

std::uint8_t StubDevice::read8(Addr a) {
    if (machine) machine->report_unmodelled(a, false, 0, name_);
    return peek8(a);
}

void StubDevice::write8(Addr a, std::uint8_t v) {
    if (machine) machine->report_unmodelled(a, true, v, name_);
    poke8(a, v);
}

Board c167cs_board() {
    Board b;
    b.name = "C167CS";
    b.cpu = isa::Cpu::C167;
    b.iram_start = 0xF200;
    // XPERCON: CAN1EN (bit 0), CAN2EN (1), XRAM1EN (10), XRAM2EN (11). After
    // reset CAN1 and XRAM1 are on.
    b.xpercon_reset = 0x0401;
    b.internal.push_back({0xE000, 0x0800, 10, std::make_shared<Ram>("XRAM1", 0x800, 0x7FF)});
    b.internal.push_back({0xC000, 0x1800, 11, std::make_shared<Ram>("XRAM2", 0x1800, 0x1FFF)});
    b.internal.push_back({0xEF00, 0x0100, 0, std::make_shared<StubDevice>("CAN1", 0xEF00, 0x100)});
    b.internal.push_back({0xEE00, 0x0100, 1, std::make_shared<StubDevice>("CAN2", 0xEE00, 0x100)});
    b.interrupts = c167_interrupts();
    return b;
}

Board siemens_5wy19(std::vector<std::uint8_t> flash) {
    Board b = c167cs_board();
    b.name = "Siemens 5WY19";
    b.clock_hz = 25'000'000;
    flash.resize(0x80000, 0xFF);
    // Only address lines A0..A18 reach the flash, so it mirrors every 512 KB.
    b.chip_selects[0] = std::make_shared<Rom>("flash", std::move(flash), 0x7FFFF);
    // The startup code points ADDRSEL2 at a 64 KB window at 90000H and runs
    // a data bus and march test there; it reports an error unless RAM
    // answers. The size of the chip is not known: 64 KB fills the window.
    b.chip_selects[2] = std::make_shared<Ram>("ram", 0x10000, 0xFFFF);
    b.chip_selects[3] = std::make_shared<Latch>("latch");
    return b;
}

std::vector<std::string> board_names() { return {"c167", "c167cs", "5wy19"}; }

Board board_by_name(const std::string& name, std::vector<std::uint8_t> image) {
    if (name == "5wy19") return siemens_5wy19(std::move(image));
    if (name == "c167" || name == "c166") {
        return simple_board(std::move(image), name == "c166" ? isa::Cpu::C166 : isa::Cpu::C167);
    }
    if (name == "c167cs") {
        Board b = c167cs_board();
        Addr mask = 1;
        while (mask < image.size()) mask <<= 1;
        b.chip_selects[0] = std::make_shared<Rom>("rom", std::move(image), mask - 1);
        return b;
    }
    throw std::invalid_argument("unknown board '" + name + "'");
}

}  // namespace oc166::sim
