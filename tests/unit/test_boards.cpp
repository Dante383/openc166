// SPDX-License-Identifier: MIT
// Board files and the flash model.
#include "doctest.h"

#include "sim/board_config.hpp"
#include "sim/boards.hpp"
#include "sim/peripherals.hpp"

#include <memory>
#include <string>

using namespace oc166::sim;

namespace {

std::unique_ptr<Machine> machine(const BoardConfig& cfg) {
    auto m = std::make_unique<Machine>(cfg.board);
    apply_board(*m, cfg);
    return m;
}

std::string error_of(const std::string& json) {
    try {
        load_board(json, BoardSource{});
    } catch (const BoardError& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST_CASE("boards: the built-in 5WY19 file") {
    std::vector<std::uint8_t> image(0x80000, 0xFF);
    image[0] = 0xFA;
    BoardConfig cfg = load_board("5wy19", image);
    CHECK(cfg.board.name == "Siemens 5WY19");
    CHECK(cfg.board.clock_hz == 24000000);
    CHECK(cfg.board.iram_start == 0xF200);
    CHECK(cfg.board.xpercon_reset == 0x0401);
    CHECK(dynamic_cast<Flash*>(cfg.board.chip_selects[0].get()) != nullptr);
    CHECK(dynamic_cast<Ram*>(cfg.board.chip_selects[2].get()) != nullptr);
    CHECK(dynamic_cast<Latch*>(cfg.board.chip_selects[3].get()) != nullptr);
    CHECK(cfg.board.chip_selects[1] == nullptr);
    CHECK(cfg.adc.at(1) == 0x2A0);
    CHECK(cfg.serial_echo == true);
    auto m = machine(cfg);
    CHECK(m->peek8(0x00000) == 0xFA);
    CHECK(m->peek8(0x80000) == 0xFA);  // the mirror
    CHECK(m->describe(0x80000) == "CS0:flash");
    CHECK(m->find_device<Adc>()->input(1) == 0x2A0);
    CHECK(m->find_device<Asc0>()->echo);
}

TEST_CASE("boards: every built-in board loads") {
    for (const auto& name : builtin_boards()) {
        CAPTURE(name);
        BoardConfig cfg = load_board(name, std::vector<std::uint8_t>(0x1000, 0x00));
        Machine m(cfg.board);
        CHECK(m.describe(0) == "CS0:" + std::string(name == "5wy19" ? "flash" : "rom"));
    }
    CHECK(load_board("c166", {1, 2}).board.cpu == oc166::isa::Cpu::C166);
    CHECK(load_board("c166", {1, 2}).board.iram_start == 0xFA00);
}

TEST_CASE("boards: numbers, sizes, frequencies and files") {
    BoardSource src;
    src.base_dir = "dir";
    src.read_file = [](const std::string& path) -> std::optional<std::vector<std::uint8_t>> {
        if (path == "dir/boot.bin") return std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6};
        return std::nullopt;
    };
    BoardConfig cfg = load_board(R"({
        "chip": "c167cr", "clock_hz": "20MHz",
        "reset": {"syscon": "0E004H", "buscon0": 1234},
        "chip_selects": {
            "cs0": {"device": "rom", "image": "boot.bin", "offset": 2, "size": "1K"},
            "cs1": {"device": "ram", "size": "0x8000", "mask": "7FFFH"}
        },
        "ssc": {"default_reply": "0x5A5A"},
        "options": {"watchdog_frozen": true}
    })", src);
    CHECK(cfg.board.clock_hz == 20000000);
    CHECK(cfg.board.syscon_reset == 0xE004);
    CHECK(cfg.board.buscon0_reset == 1234);
    auto m = machine(cfg);
    CHECK(m->peek8(0) == 3);
    CHECK(m->peek8(3) == 6);
    CHECK(m->peek8(4) == 0xFF);   // filled up to 1 KB
    CHECK(m->peek8(0x400) == 3);  // mirrored every 1 KB
    CHECK(m->watchdog_frozen);
    CHECK(m->find_device<Ssc>()->reply(0, 0) == 0x5A5A);
}

TEST_CASE("boards: errors name the place") {
    CHECK(error_of(R"({"chip": "c167", "colck_hz": 1})").find("line 1, column 30: unknown member \"colck_hz\"") !=
          std::string::npos);
    CHECK(error_of(R"({"chip": "c168"})").find("unknown chip 'c168'") != std::string::npos);
    CHECK(error_of(R"({"chip": "c167", "chip_selects": {"cs5": {}}})").find("unknown member \"cs5\"") !=
          std::string::npos);
    CHECK(error_of(R"({"chip": "c167", "chip_selects": {"cs0": {"device": "eeprom"}}})")
              .find("unknown device 'eeprom'") != std::string::npos);
    CHECK(error_of(R"({"chip": "c167", "chip_selects": {"cs0": {"device": "ram"}}})").find("needs \"size\"") !=
          std::string::npos);
    CHECK(error_of(R"({"chip": "c167", "chip_selects": {"cs0": {"device": "rom", "image": "x.bin"}}})")
              .find("cannot read 'x.bin'") != std::string::npos);
    CHECK(error_of(R"({"chip": "c167", "clock_hz": "fast"})").find("'fast' is not a number") != std::string::npos);
    CHECK(error_of("{\"chip\": \"c167\",}").find("line 1, column 17: a comma before '}'") != std::string::npos);
    CHECK_THROWS_AS(load_board("no-such-board", std::vector<std::uint8_t>{}), BoardError);
}

TEST_CASE("flash: autoselect, program and status polling") {
    BoardConfig cfg = load_board("5wy19", std::vector<std::uint8_t>(0x80000, 0xFF));
    auto m = machine(cfg);
    auto* flash = dynamic_cast<Flash*>(m->board().chip_selects[0].get());
    REQUIRE(flash);
    // The unlock cycles are word addresses 555H and 2AAH: bytes 0AAAH, 0554H.
    auto unlock = [&](std::uint16_t cmd) {
        m->write16(0xAAA, 0xAA);
        m->write16(0x554, 0x55);
        m->write16(0xAAA, cmd);
    };
    unlock(0x90);
    CHECK(m->read16(0x0) == 0x0001);  // AMD
    CHECK(m->read16(0x2) == 0x22AB);  // Am29F400BB
    CHECK(m->read16(0x4) == 0x0000);  // sector 0 is not protected
    m->write16(0x0, 0xF0);
    CHECK(m->read16(0x0) == 0xFFFF);

    // Through the mirror at 80000H, as the 5WY19 firmware does.
    m->write16(0x80AAA, 0xAA);
    m->write16(0x80554, 0x55);
    m->write16(0x80AAA, 0xA0);
    m->write16(0x81000, 0x1234);
    CHECK(flash->busy());
    const std::uint16_t s1 = m->read16(0x81000), s2 = m->read16(0x81000);
    CHECK((s1 & 0x80) == (~0x1234 & 0x80));  // DQ7: the complement of the data
    CHECK(((s1 ^ s2) & 0x40) == 0x40);         // DQ6 toggles
    for (int i = 0; i < 1000 && flash->busy(); ++i) m->run(10);
    CHECK(!flash->busy());
    CHECK(m->read16(0x81000) == 0x1234);
    CHECK(m->read16(0x01000) == 0x1234);
    // Programming only clears bits.
    unlock(0xA0);
    m->write16(0x1000, 0xFF0F);
    m->run(1000);
    CHECK(m->read16(0x1000) == 0x1204);
    // A write without a command changes nothing and is reported.
    m->write16(0x2000, 0x0000);
    CHECK(m->read16(0x2000) == 0xFFFF);
    CHECK(!m->unmodelled().empty());
}

TEST_CASE("flash: sector and chip erase, snapshots") {
    std::vector<std::uint8_t> image(0x80000, 0x00);
    BoardConfig cfg = load_board("5wy19", image);
    auto m = machine(cfg);
    auto* flash = dynamic_cast<Flash*>(m->board().chip_selects[0].get());
    flash->sector_erase_ms = 1;  // keep the test quick
    const Snapshot before = m->save();
    auto erase_setup = [&] {
        m->write16(0xAAA, 0xAA);
        m->write16(0x554, 0x55);
        m->write16(0xAAA, 0x80);
        m->write16(0xAAA, 0xAA);
        m->write16(0x554, 0x55);
    };
    // Two of the boot sectors: 4000H (8 KB) and 8000H (32 KB).
    erase_setup();
    m->write16(0x4000, 0x30);
    m->write16(0x8000, 0x30);
    CHECK(!flash->busy());  // within the 50 us window more sectors may follow
    for (int i = 0; i < 10000 && !flash->busy(); ++i) m->run(10);
    CHECK(flash->busy());
    CHECK((m->read16(0x4000) & 0x08) == 0x08);  // DQ3: erasing
    for (int i = 0; i < 100000 && flash->busy(); ++i) m->run(10);
    CHECK(!flash->busy());
    CHECK(m->peek8(0x3FFF) == 0x00);
    CHECK(m->peek8(0x4000) == 0xFF);
    CHECK(m->peek8(0x5FFF) == 0xFF);
    CHECK(m->peek8(0x6000) == 0x00);
    CHECK(m->peek8(0x8000) == 0xFF);
    CHECK(m->peek8(0x8FFFF) == 0xFF);  // 0FFFFH itself is an SFR
    CHECK(m->peek8(0x10000) == 0x00);
    // The snapshot still has the contents from before.
    m->load(before);
    CHECK(m->peek8(0x4000) == 0x00);
    // Chip erase.
    flash->chip_erase_ms = 1;
    erase_setup();
    m->write16(0xAAA, 0x10);
    CHECK(flash->busy());
    for (int i = 0; i < 100000 && flash->busy(); ++i) m->run(10);
    CHECK(m->peek8(0x7FFFF) == 0xFF);
    CHECK(m->peek8(0x00000) == 0xFF);
}
