// SPDX-License-Identifier: MIT
// Board definitions: which CPU, memory and devices make up an ECU.
#pragma once

#include "sim/machine.hpp"

namespace oc166::sim {

// RAM-like storage for on-chip modules that are not modelled yet (the CAN
// controllers): reads return what was written, and the first access to each
// address is logged as unmodelled.
class StubDevice final : public Device {
public:
    StubDevice(std::string name, Addr start, Addr size) : name_(std::move(name)), start_(start), mem_(size, 0) {}
    std::string name() const override { return name_; }
    std::uint8_t read8(Addr a) override;
    void write8(Addr a, std::uint8_t v) override;
    std::uint8_t peek8(Addr a) override { return mem_[(a - start_) % mem_.size()]; }
    void poke8(Addr a, std::uint8_t v) override { mem_[(a - start_) % mem_.size()] = v; }
    void reset(bool power_on) override {
        if (power_on) std::fill(mem_.begin(), mem_.end(), 0);
    }
    void save(StateWriter& w) const override { w.bytes(mem_); }
    void load(StateReader& r) override { r.bytes(mem_); }
    Machine* machine = nullptr;

private:
    std::string name_;
    Addr start_;
    std::vector<std::uint8_t> mem_;
};

// An output latch (74HC574): keeps the last byte written; reads see the bus
// float (0FFh). 'writes' counts the writes, for tests and scripts.
class Latch final : public Device {
public:
    explicit Latch(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    std::uint8_t read8(Addr) override { return 0xFF; }
    void write8(Addr, std::uint8_t v) override {
        value = v;
        ++writes;
    }
    std::uint8_t peek8(Addr) override { return value; }
    void poke8(Addr, std::uint8_t v) override { value = v; }
    void reset(bool power_on) override {
        if (power_on) value = 0;
    }
    void save(StateWriter& w) const override {
        w.put(value);
        w.put(writes);
    }
    void load(StateReader& r) override {
        r.get(value);
        r.get(writes);
    }
    std::uint8_t value = 0;
    std::uint64_t writes = 0;

private:
    std::string name_;
};

// A C167CS: 3 KB internal RAM, 8 KB XRAM in two blocks, two CAN controllers
// (stubs for now), enabled through XPERCON.
Board c167cs_board();

// The Siemens 5WY19 engine ECU (docs/debugger-design.md, section 4):
// C167CS-LM at 24 MHz, a 512 KB AM29F400BB flash on CS0 that appears at
// both 0 and 80000H, external RAM on CS2 (wherever the firmware puts its
// window) and the 74HC574 output latch in the CS3 window. The
// flash is a read-only image until the flash model exists.
Board siemens_5wy19(std::vector<std::uint8_t> flash);

// Names of the boards that board_by_name() knows.
std::vector<std::string> board_names();
// The board called 'name' ("c167", "c167cs", "5wy19") with 'image' as its
// ROM or flash. Throws std::invalid_argument for an unknown name.
Board board_by_name(const std::string& name, std::vector<std::uint8_t> image);

}  // namespace oc166::sim
