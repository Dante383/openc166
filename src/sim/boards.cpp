// SPDX-License-Identifier: MIT
#include "sim/boards.hpp"

#include "sim/peripherals.hpp"

#include <algorithm>
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

}  // namespace oc166::sim

namespace oc166::sim {

// ---- Flash -----------------------------------------------------------------

Flash::Geometry Flash::am29f400bb() {
    Geometry g;
    g.sectors = {{0x00000, 0x4000}, {0x04000, 0x2000}, {0x06000, 0x2000}, {0x08000, 0x8000}};
    for (std::uint32_t a = 0x10000; a < 0x80000; a += 0x10000) g.sectors.push_back({a, 0x10000});
    g.manufacturer = 0x0001;  // AMD
    g.device = 0x22AB;        // Am29F400BB, word mode
    return g;
}

std::optional<Flash::Geometry> Flash::model(const std::string& name) {
    if (name == "am29f400bb") return am29f400bb();
    if (name == "am29f400bt") {
        Geometry g;
        for (std::uint32_t a = 0; a < 0x70000; a += 0x10000) g.sectors.push_back({a, 0x10000});
        g.sectors.insert(g.sectors.end(), {{0x70000, 0x8000}, {0x78000, 0x2000}, {0x7A000, 0x2000}, {0x7C000, 0x4000}});
        g.manufacturer = 0x0001;
        g.device = 0x2223;
        return g;
    }
    return std::nullopt;
}

Flash::Flash(std::string name, std::vector<std::uint8_t> image, Addr mask, Geometry geometry, int bus_width)
    : name_(std::move(name)),
      image_(std::make_shared<std::vector<std::uint8_t>>(std::move(image))),
      mask_(mask),
      geometry_(std::move(geometry)),
      bus_width_(bus_width) {
    std::uint32_t size = 0;
    for (const auto& [off, len] : geometry_.sectors) size = std::max(size, off + len);
    image_->resize(std::max<std::size_t>(size, image_->size() ? image_->size() : 1), 0xFF);
}

std::uint64_t Flash::cycles(double seconds) const {
    const double hz = m_ ? m_->board().clock_hz : 25e6;
    return static_cast<std::uint64_t>(seconds * hz + 0.5);
}

// The address the chip decodes for commands: the word address on a 16-bit
// bus, the byte address on an 8-bit one; only the low 11 bits count.
std::uint32_t Flash::command_address(Addr a) const {
    const std::uint32_t off = a & mask_;
    return (bus_width_ == 16 ? off >> 1 : off) & 0x7FF;
}

int Flash::sector_of(std::uint32_t off) const {
    for (std::size_t i = 0; i < geometry_.sectors.size(); ++i)
        if (off >= geometry_.sectors[i].first && off < geometry_.sectors[i].first + geometry_.sectors[i].second)
            return static_cast<int>(i);
    return -1;
}

std::uint16_t Flash::array16(std::uint32_t off) const {
    off &= ~1u;
    const auto& img = *image_;
    return static_cast<std::uint16_t>(img[off % img.size()] | (img[(off + 1) % img.size()] << 8));
}

std::vector<std::uint8_t>& Flash::writable() {
    // Snapshots share the contents: copy them before the first change.
    if (image_.use_count() > 1) image_ = std::make_shared<std::vector<std::uint8_t>>(*image_);
    return *image_;
}

std::uint16_t Flash::status(std::uint32_t off) {
    toggle_ ^= 1;
    std::uint16_t s = 0;
    if (op_ == Op::Program) {
        s = static_cast<std::uint16_t>(~op_data_ & 0x80);  // DQ7: the complement of the data
    } else {
        s = 0x08;  // DQ7 = 0, DQ3: the erase has started
        const int sec = sector_of(off);
        const bool erasing = op_ == Op::EraseChip || (sec >= 0 && ((erase_mask_ >> sec) & 1));
        if (erasing && toggle_) s |= 0x04;  // DQ2 toggles in a sector being erased
    }
    if (toggle_) s |= 0x40;  // DQ6
    return s;
}

std::uint16_t Flash::read16(Addr a) {
    const std::uint32_t off = offset(a);
    if (state_ == State::Busy) return status(off);
    if (state_ == State::Autoselect) {
        switch (command_address(a) & 0xFF) {
        case 0x00: return geometry_.manufacturer;
        case 0x01: return geometry_.device;
        case 0x02: return 0;  // sector not protected
        default: return 0;
        }
    }
    return array16(off);
}

std::uint8_t Flash::read8(Addr a) {
    const std::uint16_t w = read16(a & ~1u);
    return static_cast<std::uint8_t>((a & 1) ? w >> 8 : w);
}

std::uint8_t Flash::peek8(Addr a) { return (*image_)[offset(a)]; }

void Flash::poke8(Addr a, std::uint8_t v) {
    writable()[offset(a)] = v;
    if (m_) m_->invalidate_code();
}

void Flash::write8(Addr a, std::uint8_t v) {
    // A byte write on a 16-bit flash reaches it as a word on the bus.
    if (bus_width_ == 16 && (a & 1)) return;
    command(a, v);
}

void Flash::write16(Addr a, std::uint16_t v) { command(a, v); }

void Flash::command(Addr a, std::uint16_t v) {
    const std::uint32_t ca = command_address(a);
    const std::uint8_t c = static_cast<std::uint8_t>(v);
    const bool unlock1 = ca == (bus_width_ == 16 ? 0x555u : 0xAAAu) && c == 0xAA;
    const bool unlock2 = ca == (bus_width_ == 16 ? 0x2AAu : 0x555u) && c == 0x55;
    const bool cmd_addr = ca == (bus_width_ == 16 ? 0x555u : 0xAAAu);
    switch (state_) {
    case State::Busy:
        return;  // commands are ignored while the chip works
    case State::Read:
    case State::Autoselect:
        if (c == 0xF0) {
            state_ = State::Read;
            return;
        }
        if (unlock1) {
            resume_ = state_;
            state_ = State::Unlock1;
        } else if (m_) {
            m_->report_unmodelled(a, true, v, name_ + " (write without a command)");
        }
        return;
    case State::Unlock1:
        state_ = unlock2 ? State::Unlock2 : State::Read;
        return;
    case State::Unlock2:
        if (!cmd_addr) {
            state_ = State::Read;
            return;
        }
        switch (c) {
        case 0xA0: state_ = State::ProgramSetup; return;
        case 0x80: state_ = State::Erase1; return;
        case 0x90: state_ = State::Autoselect; return;
        case 0xF0:
        default: state_ = State::Read; return;
        }
    case State::ProgramSetup: {
        const std::uint32_t off = offset(a) & (bus_width_ == 16 ? ~1u : ~0u);
        op_ = Op::Program;
        op_offset_ = off;
        op_data_ = bus_width_ == 16 ? v : c;
        state_ = State::Busy;
        resume_ = State::Read;
        m_->schedule(*this, cycles(program_us * 1e-6), 0);
        return;
    }
    case State::Erase1:
        state_ = unlock1 ? State::Erase2 : State::Read;
        return;
    case State::Erase2:
        state_ = unlock2 ? State::Erase3 : State::Read;
        return;
    case State::Erase3:
        if (c == 0x10 && cmd_addr) {
            op_ = Op::EraseChip;
            state_ = State::Busy;
            resume_ = State::Read;
            m_->schedule(*this, cycles(chip_erase_ms * 1e-3), 0);
        } else if (c == 0x30) {
            const int sec = sector_of(offset(a));
            if (sec >= 0) erase_mask_ |= 1u << sec;
            state_ = State::EraseSectors;
            // More sectors may follow within the 50 us time-out.
            m_->cancel(*this, 1);
            m_->schedule(*this, cycles(50e-6), 1);
        } else {
            state_ = State::Read;
        }
        return;
    case State::EraseSectors:
        if (c == 0x30) {
            const int sec = sector_of(offset(a));
            if (sec >= 0) erase_mask_ |= 1u << sec;
            m_->cancel(*this, 1);
            m_->schedule(*this, cycles(50e-6), 1);
        } else if (c == 0xB0) {
            // Erase suspend is not modelled: finish now.
            m_->cancel(*this, 1);
            start_erase();
        }
        return;
    }
}

void Flash::start_erase() {
    op_ = Op::EraseSectors;
    state_ = State::Busy;
    resume_ = State::Read;
    int n = 0;
    for (std::uint32_t mask = erase_mask_; mask; mask &= mask - 1) ++n;
    m_->schedule(*this, cycles(sector_erase_ms * 1e-3 * std::max(n, 1)), 0);
}

void Flash::event(Machine& m, std::uint32_t tag) {
    if (tag == 1) {  // the sector erase time-out has passed
        if (state_ == State::EraseSectors) start_erase();
        return;
    }
    auto& img = writable();
    switch (op_) {
    case Op::Program:
        img[op_offset_ % img.size()] &= static_cast<std::uint8_t>(op_data_);
        if (bus_width_ == 16) img[(op_offset_ + 1) % img.size()] &= static_cast<std::uint8_t>(op_data_ >> 8);
        break;
    case Op::EraseSectors:
        for (std::size_t i = 0; i < geometry_.sectors.size(); ++i)
            if ((erase_mask_ >> i) & 1) {
                const auto& [off, len] = geometry_.sectors[i];
                std::fill(img.begin() + off, img.begin() + off + len, 0xFF);
            }
        break;
    case Op::EraseChip: std::fill(img.begin(), img.end(), 0xFF); break;
    case Op::None: break;
    }
    op_ = Op::None;
    erase_mask_ = 0;
    state_ = resume_;
    m.invalidate_code();
}

void Flash::reset(bool) {
    // A reset aborts what the chip is doing; the array keeps its contents.
    state_ = State::Read;
    op_ = Op::None;
    erase_mask_ = 0;
}

void Flash::save(StateWriter& w) const {
    w.shared(image_);
    w.put(state_);
    w.put(resume_);
    w.put(op_);
    w.put(op_offset_);
    w.put(op_data_);
    w.put(erase_mask_);
    w.put(toggle_);
}

void Flash::load(StateReader& r) {
    // Shared with the snapshot: writable() copies before a change.
    image_ = std::const_pointer_cast<std::vector<std::uint8_t>>(r.shared());
    r.get(state_);
    r.get(resume_);
    r.get(op_);
    r.get(op_offset_);
    r.get(op_data_);
    r.get(erase_mask_);
    r.get(toggle_);
}

}  // namespace oc166::sim
