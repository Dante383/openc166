// SPDX-License-Identifier: MIT
// Board definitions: which CPU, memory and devices make up an ECU.
#pragma once

#include "sim/machine.hpp"

#include <optional>

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

// An AMD-style NOR flash (AM29F400BB and its kind) on the external bus:
// reads give the array, and the command set works as on the chip: the
// unlock cycles (AAH at 555H, 55H at 2AAH, as word addresses on a 16-bit
// bus or AAAH/555H on an 8-bit one), program (A0H), sector and chip erase
// (80H, then 30H at each sector or 10H), autoselect (90H: manufacturer,
// device ID, sector protection) and reset (F0H). While the chip programs or
// erases, reads give the status: DQ7 data polling, DQ6 toggling on every
// read, DQ3 once the erase has started and DQ2 toggling in a sector being
// erased. Program and erase take the times given (the datasheet's typical
// values by default), on the machine's clock. Programming only clears bits.
class Flash final : public Device {
public:
    struct Geometry {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sectors;  // byte offset and size
        std::uint16_t manufacturer;
        std::uint16_t device;
    };
    // 512 KB bottom boot block: 16, 8, 8 and 32 KB, then seven 64 KB sectors.
    static Geometry am29f400bb();
    // Geometry by model name ("am29f400bb", "am29f400bt"), or std::nullopt.
    static std::optional<Geometry> model(const std::string& name);

    Flash(std::string name, std::vector<std::uint8_t> image, Addr mask, Geometry geometry, int bus_width = 16);
    std::string name() const override { return name_; }
    std::uint8_t read8(Addr a) override;
    std::uint16_t read16(Addr a) override;
    void write8(Addr a, std::uint8_t v) override;
    void write16(Addr a, std::uint16_t v) override;
    std::uint8_t peek8(Addr a) override;
    void poke8(Addr a, std::uint8_t v) override;
    void attach(Machine& m) override { m_ = &m; }
    void reset(bool power_on) override;
    void save(StateWriter& w) const override;
    void load(StateReader& r) override;
    void event(Machine& m, std::uint32_t tag) override;

    double program_us = 12;      // per word (or byte)
    double sector_erase_ms = 1000;
    double chip_erase_ms = 8000;

    const std::vector<std::uint8_t>& image() const { return *image_; }
    bool busy() const { return state_ == State::Busy; }

private:
    enum class State : std::uint8_t {
        Read, Unlock1, Unlock2, Autoselect, ProgramSetup, Erase1, Erase2, Erase3, EraseSectors, Busy,
    };
    enum class Op : std::uint8_t { None, Program, EraseSectors, EraseChip };
    std::uint32_t offset(Addr a) const { return (a & mask_) % static_cast<std::uint32_t>(image_->size()); }
    std::uint32_t command_address(Addr a) const;
    int sector_of(std::uint32_t offset) const;
    std::uint16_t array16(std::uint32_t off) const;
    std::uint16_t status(std::uint32_t off);
    void command(Addr a, std::uint16_t v);
    void start_erase();
    std::vector<std::uint8_t>& writable();
    std::uint64_t cycles(double seconds) const;

    std::string name_;
    std::shared_ptr<std::vector<std::uint8_t>> image_;
    Addr mask_;
    Geometry geometry_;
    int bus_width_;
    Machine* m_ = nullptr;

    // Snapshot state.
    State state_ = State::Read;
    State resume_ = State::Read;  // where Busy goes back to
    Op op_ = Op::None;
    std::uint32_t op_offset_ = 0;
    std::uint16_t op_data_ = 0;
    std::uint32_t erase_mask_ = 0;  // sectors to erase
    std::uint8_t toggle_ = 0;
};

}  // namespace oc166::sim
