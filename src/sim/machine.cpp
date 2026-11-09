// SPDX-License-Identifier: MIT
// The machine: bus routing, SFRs, reset, interrupts and traps, watchdog,
// clock and snapshots. Instruction semantics are in cpu.cpp.
#include "sim/machine.hpp"

#include "sim/boards.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace oc166::sim {

namespace {

std::string hexaddr(Addr a) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%06XH", a);
    return std::string(buf[0] >= 'A' ? "0" : "") + buf;
}

constexpr std::size_t kDecodeCache = 1 << 16;

// "SFR S0TBUF (0FEB0H)" or "ESFR 0F0C0H" for the unmodelled-access log.
std::string sfr_label(Addr a) {
    std::string name = dis::c167_names().sfr(a, false);
    const std::string kind = a >= 0xFE00 ? "SFR " : "ESFR ";
    return name.empty() ? kind + hexaddr(a) : kind + name + " (" + hexaddr(a) + ")";
}

// TFR bits.
constexpr std::uint16_t kNmi = 1u << 15, kStkof = 1u << 14, kStkuf = 1u << 13, kUndopc = 1u << 7,
                        kPrtflt = 1u << 3, kIllopa = 1u << 2, kIllina = 1u << 1, kIllbus = 1u << 0;
constexpr std::uint16_t kClassB = kUndopc | kPrtflt | kIllopa | kIllina | kIllbus;

// WDTCON flags.
constexpr std::uint16_t kWdtin = 1u << 0, kWdtr = 1u << 1, kSwr = 1u << 2, kShwr = 1u << 3, kLhwr = 1u << 4;

bool event_later(const auto& a, const auto& b) { return a.time != b.time ? a.time > b.time : a.seq > b.seq; }

}  // namespace

// ---- Snapshot reader -------------------------------------------------------

const std::uint8_t* StateReader::take(std::size_t n) {
    if (pos_ + n > data_.size()) throw std::runtime_error("snapshot data is truncated");
    const std::uint8_t* p = data_.data() + pos_;
    pos_ += n;
    return p;
}

// ---- ROM -------------------------------------------------------------------

void Rom::write8(Addr a, std::uint8_t v) {
    if (machine) machine->report_unmodelled(a, true, v, name_ + " (write to ROM)");
}

void Rom::poke8(Addr a, std::uint8_t v) {
    const std::size_t i = a & mask_;
    if (i >= image_->size()) return;
    auto copy = std::make_shared<std::vector<std::uint8_t>>(*image_);
    (*copy)[i] = v;
    image_ = std::move(copy);
}

// ---- Boards ----------------------------------------------------------------

std::vector<InterruptSource> c167_interrupts() {
    std::vector<InterruptSource> v;
    for (int i = 0; i < 16; ++i)
        v.push_back({"CC" + std::to_string(i), static_cast<Addr>(0xFF78 + 2 * i), static_cast<std::uint8_t>(0x10 + i)});
    for (int i = 16; i <= 28; ++i)
        v.push_back({"CC" + std::to_string(i), static_cast<Addr>(0xF160 + 2 * (i - 16)), static_cast<std::uint8_t>(0x30 + i - 16)});
    const InterruptSource rest[] = {
        {"T0", 0xFF9C, 0x20},   {"T1", 0xFF9E, 0x21},   {"T2", 0xFF60, 0x22},   {"T3", 0xFF62, 0x23},
        {"T4", 0xFF64, 0x24},   {"T5", 0xFF66, 0x25},   {"T6", 0xFF68, 0x26},   {"CR", 0xFF6A, 0x27},
        {"ADC", 0xFF98, 0x28},  {"ADE", 0xFF9A, 0x29},  {"S0T", 0xFF6C, 0x2A},  {"S0R", 0xFF6E, 0x2B},
        {"S0E", 0xFF70, 0x2C},  {"SCT", 0xFF72, 0x2D},  {"SCR", 0xFF74, 0x2E},  {"SCE", 0xFF76, 0x2F},
        {"T7", 0xF17A, 0x3D},   {"T8", 0xF17C, 0x3E},   {"PWM", 0xF17E, 0x3F},  {"XP0", 0xF186, 0x40},
        {"XP1", 0xF18E, 0x41},  {"XP2", 0xF196, 0x42},  {"XP3", 0xF19E, 0x43},  {"CC29", 0xF184, 0x44},
        {"CC30", 0xF18C, 0x45}, {"CC31", 0xF194, 0x46}, {"S0TB", 0xF19C, 0x47},
    };
    for (const auto& s : rest) v.push_back(s);
    return v;
}

Board simple_board(std::vector<std::uint8_t> image, isa::Cpu cpu) {
    Board b;
    b.cpu = cpu;
    Addr mask = 1;
    while (mask < image.size()) mask <<= 1;
    b.chip_selects[0] = std::make_shared<Rom>("rom", std::move(image), mask - 1);
    b.interrupts = c167_interrupts();
    return b;
}

// ---- Watchdog --------------------------------------------------------------

struct Machine::WatchdogDevice final : Device {
    std::string name() const override { return "watchdog"; }
    std::uint8_t read8(Addr) override { return 0xFF; }
    void write8(Addr, std::uint8_t) override {}
    void event(Machine& m, std::uint32_t) override {
        if (!m.wdt_enabled_) return;
        if (m.watchdog_frozen) {
            m.watchdog_reload();
            return;
        }
        m.reset(ResetKind::Watchdog);
    }
};

std::uint16_t Machine::wdt_value() const {
    if (!wdt_enabled_) return wdt_start_;
    const std::uint64_t div = (sfr_get(reg::WDTCON) & kWdtin) ? 128 : 2;
    const std::uint64_t v = wdt_start_ + (cycles_ - wdt_start_cycle_) / div;
    return static_cast<std::uint16_t>(std::min<std::uint64_t>(v, 0xFFFF));
}

void Machine::watchdog_reload() {
    wdt_start_ = static_cast<std::uint16_t>(sfr_get(reg::WDTCON) & 0xFF00);
    wdt_start_cycle_ = cycles_;
    watchdog_schedule();
}

void Machine::watchdog_schedule() {
    cancel(*wdt_device_, 0);
    if (!wdt_enabled_) return;
    const std::uint64_t div = (sfr_get(reg::WDTCON) & kWdtin) ? 128 : 2;
    schedule(*wdt_device_, (0x10000u - wdt_start_) * div, 0);
}

// ---- Construction and reset -----------------------------------------------

Machine::Machine(Board board) : board_(std::move(board)), iram_(0x1000, 0) {
    decode_cache_.resize(kDecodeCache);
    ic_index_.assign(512, -1);
    for (std::size_t i = 0; i < board_.interrupts.size(); ++i) {
        const Addr a = board_.interrupts[i].ic;
        if (a >= 0xFE00) ic_index_[256 + (a - 0xFE00) / 2] = static_cast<int>(i);
        else if (a >= 0xF000 && a < 0xF200) ic_index_[(a - 0xF000) / 2] = static_cast<int>(i);
    }
    wdt_device_ = std::make_shared<WatchdogDevice>();
    add_device(wdt_device_);
    for (int cs = 0; cs < 5; ++cs)
        if (board_.chip_selects[cs]) add_device(board_.chip_selects[cs]);
    for (auto& r : board_.internal)
        if (r.device) add_device(r.device);
    for (auto& d : board_.devices) add_device(d);
    install_core_hooks();
    attached_ = true;
    for (auto& d : devices_) d->attach(*this);
    reset(ResetKind::PowerOn);
}

Machine::~Machine() = default;

void Machine::add_device(std::shared_ptr<Device> d) {
    if (!d || d->id >= 0) return;
    d->id = static_cast<int>(devices_.size());
    if (auto* rom = dynamic_cast<Rom*>(d.get())) rom->machine = this;
    if (auto* stub = dynamic_cast<StubDevice*>(d.get())) stub->machine = this;
    devices_.push_back(d);
    if (attached_) d->attach(*this);
}

void Machine::set_chip_select(int cs, std::shared_ptr<Device> d) {
    if (cs < 0 || cs > 4) throw std::invalid_argument("chip select must be 0..4");
    add_device(d);
    board_.chip_selects[static_cast<std::size_t>(cs)] = std::move(d);
    std::fill(decode_cache_.begin(), decode_cache_.end(), CachedInsn{});
}

void Machine::reset(ResetKind kind) {
    const std::uint16_t old_wdtcon = sfr_get(reg::WDTCON);
    // Registers to their reset values; RAM keeps its contents.
    sfr_.fill(0);
    esfr_.fill(0);
    for (int i = 0; i < 4; ++i) sfr_set(reg::DPP0 + 2 * i, static_cast<std::uint16_t>(i));
    sfr_set(reg::CP, 0xFC00);
    sfr_set(reg::SP, 0xFC00);
    sfr_set(reg::STKOV, 0xFA00);
    sfr_set(reg::STKUN, 0xFC00);
    sfr_set(reg::SYSCON, board_.syscon_reset);
    sfr_set(reg::BUSCON0, board_.buscon0_reset);
    sfr_set(reg::RP0H, board_.rp0h_reset);
    sfr_set(reg::XPERCON, board_.xpercon_reset);
    sfr_set(reg::ONES, 0xFFFF);
    // Reset indication flags. Each reset sets its own flag and those of the
    // weaker resets. WDTR is cleared only by a hardware reset or SRVWDT;
    // SWR, SHWR and LHWR by EINIT.
    std::uint16_t flags = 0;
    switch (kind) {
    case ResetKind::PowerOn: flags = kLhwr | kShwr | kSwr; break;
    case ResetKind::Hardware: flags = kShwr | kSwr; break;
    case ResetKind::Watchdog: flags = kWdtr | kShwr | kSwr; break;
    case ResetKind::Software: flags = static_cast<std::uint16_t>((old_wdtcon & kWdtr) | kSwr); break;
    }
    sfr_set(reg::WDTCON, flags);
    if (SfrHook* h = hook(reg::SYSCON)) h->readonly_mask = 0;
    ip_ = 0;
    ext_ = {};
    einit_ = false;
    idle_ = false;
    pwrdn_ = false;
    pending_traps_ = 0;
    irq_dirty_ = true;
    events_.clear();
    if (kind == ResetKind::PowerOn) {
        std::fill(iram_.begin(), iram_.end(), 0);
        cycles_ = 0;
        instructions_ = 0;
    }
    for (auto& d : devices_) d->reset(kind == ResetKind::PowerOn);
    wdt_enabled_ = true;
    watchdog_reload();
    std::fill(decode_cache_.begin(), decode_cache_.end(), CachedInsn{});
}

// ---- SFRs ------------------------------------------------------------------

std::uint16_t* Machine::sfr_slot(Addr a) {
    if (a >= 0xFE00 && a <= 0xFFFF) return &sfr_[(a - 0xFE00) / 2];
    if (a >= 0xF000 && a < 0xF200) return &esfr_[(a - 0xF000) / 2];
    return nullptr;
}

const std::uint16_t* Machine::sfr_slot(Addr a) const { return const_cast<Machine*>(this)->sfr_slot(a); }

std::uint16_t Machine::sfr_get(Addr a) const {
    const std::uint16_t* p = sfr_slot(a & ~1u);
    return p ? *p : 0;
}

void Machine::sfr_set(Addr a, std::uint16_t v) {
    if (std::uint16_t* p = sfr_slot(a & ~1u)) *p = v;
    if (ic_index_.size() == 512 && ((a >= 0xFE00) || (a >= 0xF000 && a < 0xF200))) irq_dirty_ = true;
}

SfrHook* Machine::hook(Addr a) {
    a &= ~1u;
    if (a >= 0xFE00 && a <= 0xFFFF) return sfr_hooks_[(a - 0xFE00) / 2].get();
    if (a >= 0xF000 && a < 0xF200) return esfr_hooks_[(a - 0xF000) / 2].get();
    return nullptr;
}

void Machine::set_hook(Addr a, SfrHook h) {
    a &= ~1u;
    if (a >= 0xFE00 && a <= 0xFFFF)
        sfr_hooks_[(a - 0xFE00) / 2] = std::make_unique<SfrHook>(std::move(h));
    else if (a >= 0xF000 && a < 0xF200)
        esfr_hooks_[(a - 0xF000) / 2] = std::make_unique<SfrHook>(std::move(h));
    else
        throw std::invalid_argument("not an SFR address: " + hexaddr(a));
}

std::uint16_t Machine::sfr_read(Addr a) {
    a &= ~1u;
    const std::uint16_t stored = sfr_get(a);
    SfrHook* h = hook(a);
    if (!h) {
        report_unmodelled(a, false, stored, sfr_label(a));
        return stored;
    }
    return h->read ? h->read(*this, a, stored) : stored;
}

void Machine::sfr_write(Addr a, std::uint16_t v, std::uint16_t mask) {
    a &= ~1u;
    std::uint16_t* p = sfr_slot(a);
    if (!p) return;
    SfrHook* h = hook(a);
    const std::uint16_t old = *p;
    const std::uint16_t writable = static_cast<std::uint16_t>(mask & ~(h ? h->readonly_mask : 0));
    const std::uint16_t value = static_cast<std::uint16_t>((old & ~writable) | (v & writable));
    *p = value;
    if (ic_index_[a >= 0xFE00 ? 256 + (a - 0xFE00) / 2 : (a - 0xF000) / 2] >= 0) irq_dirty_ = true;
    if (!h) {
        report_unmodelled(a, true, v, sfr_label(a));
        return;
    }
    if (h->write) h->write(*this, a, old, value);
}

void Machine::install_core_hooks() {
    auto plain = [this](Addr a, std::uint16_t ro = 0) {
        SfrHook h;
        h.readonly_mask = ro;
        set_hook(a, std::move(h));
    };
    for (int i = 0; i < 4; ++i) plain(reg::DPP0 + 2 * i, 0xFC00);
    plain(reg::CSP, 0xFFFF);
    plain(reg::CP, 0xF001);
    plain(reg::SP, 0x0001);
    plain(reg::STKOV, 0x0001);
    plain(reg::STKUN, 0x0001);
    plain(reg::ZEROS, 0xFFFF);
    plain(reg::ONES, 0xFFFF);
    plain(reg::RP0H, 0xFFFF);
    plain(reg::TFR);
    plain(reg::XPERCON);
    plain(reg::BUSCON0);
    for (int i = 0; i < 4; ++i) {
        plain(reg::BUSCON1 + 2 * i);
        plain(reg::ADDRSEL1 + 2 * i);
    }
    for (int i = 0; i < 8; ++i) plain(reg::PECC0 + 2 * i);
    {
        SfrHook h;
        h.write = [](Machine& m, Addr, std::uint16_t, std::uint16_t) { m.irq_dirty_ = true; };
        set_hook(reg::PSW, h);
    }
    {
        // MDRIU (MDC.4) says MD is in use: set by writes to MDH or MDL (and by
        // multiply and divide), cleared by reading MDL.
        SfrHook h;
        h.write = [](Machine& m, Addr, std::uint16_t, std::uint16_t) {
            m.sfr_set(reg::MDC, static_cast<std::uint16_t>(m.sfr_get(reg::MDC) | 0x10));
        };
        set_hook(reg::MDH, h);
        SfrHook l = h;
        l.read = [](Machine& m, Addr, std::uint16_t stored) {
            m.sfr_set(reg::MDC, static_cast<std::uint16_t>(m.sfr_get(reg::MDC) & ~0x10));
            return stored;
        };
        set_hook(reg::MDL, l);
        plain(reg::MDC);
    }
    plain(reg::SYSCON);
    {
        SfrHook h;
        h.readonly_mask = kWdtr | kSwr | kShwr | kLhwr;
        set_hook(reg::WDTCON, h);
    }
    {
        SfrHook h;
        h.readonly_mask = 0xFFFF;
        h.read = [](Machine& m, Addr, std::uint16_t) { return m.wdt_value(); };
        set_hook(reg::WDT, h);
    }
    for (const auto& src : board_.interrupts) plain(src.ic, 0xFF00);
}

// ---- Bus -------------------------------------------------------------------

bool Machine::internal(Addr a, Region** region) {
    const bool xpen = sfr_get(reg::SYSCON) & 0x0004;
    const std::uint16_t xpercon = sfr_get(reg::XPERCON);
    for (auto& r : board_.internal) {
        if (a < r.start || a >= r.start + r.size) continue;
        if (r.xpercon_bit >= 0 && !(xpen && (xpercon >> r.xpercon_bit) & 1)) continue;
        *region = &r;
        return true;
    }
    return false;
}

Device* Machine::external_device(Addr a, int* cs) {
    // ADDRSEL2 has priority over ADDRSEL1 and ADDRSEL4 over ADDRSEL3.
    static constexpr int kOrder[] = {2, 4, 1, 3};
    for (int x : kOrder) {
        const std::uint16_t buscon = sfr_get(reg::BUSCON1 + 2 * (x - 1));
        if (!(buscon & 0x0400)) continue;  // BUSACTx
        const std::uint16_t sel = sfr_get(reg::ADDRSEL1 + 2 * (x - 1));
        const unsigned size_bits = 12 + std::min<unsigned>(sel & 0xF, 11);
        const Addr start = static_cast<Addr>(sel & 0xFFF0) << 8;
        if ((a >> size_bits) == (start >> size_bits)) {
            *cs = x;
            return board_.chip_selects[x].get();
        }
    }
    *cs = 0;
    return board_.chip_selects[0].get();
}

std::uint8_t Machine::bus_read8(Addr a) {
    a &= 0xFFFFFF;
    if (a >= 0xF000 && a <= 0xFFFF) {
        if (a >= 0xFE00 || a < 0xF200) return static_cast<std::uint8_t>(sfr_read(a) >> (8 * (a & 1)));
        if (a >= board_.iram_start) return iram_[a - 0xF000];
        report_unmodelled(a, false, 0, "reserved " + hexaddr(a));
        return 0xFF;
    }
    Region* r = nullptr;
    if (internal(a, &r)) return r->device->read8(a);
    int cs = 0;
    if (Device* d = external_device(a, &cs)) return d->read8(a);
    report_unmodelled(a, false, 0, "CS" + std::to_string(cs) + " " + hexaddr(a));
    return 0xFF;
}

void Machine::bus_write8(Addr a, std::uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xF000 && a <= 0xFFFF) {
        if (a >= 0xFE00 || a < 0xF200) {
            const int shift = 8 * (a & 1);
            sfr_write(a, static_cast<std::uint16_t>(v << shift), static_cast<std::uint16_t>(0xFF << shift));
            return;
        }
        if (a >= board_.iram_start) {
            iram_[a - 0xF000] = v;
            return;
        }
        report_unmodelled(a, true, v, "reserved " + hexaddr(a));
        return;
    }
    Region* r = nullptr;
    if (internal(a, &r)) return r->device->write8(a, v);
    int cs = 0;
    if (Device* d = external_device(a, &cs)) return d->write8(a, v);
    report_unmodelled(a, true, v, "CS" + std::to_string(cs) + " " + hexaddr(a));
}

std::uint8_t Machine::read8(Addr a) {
    const std::uint8_t v = bus_read8(a);
    if (on_access) on_access({AccessKind::Read, a & 0xFFFFFF, 1, v, 0});
    return v;
}

std::uint16_t Machine::read16(Addr a) {
    a &= 0xFFFFFF;
    std::uint16_t v;
    if ((a >= 0xFE00 && a <= 0xFFFF) || (a >= 0xF000 && a < 0xF200))
        v = sfr_read(a);  // one access, so read side effects happen once
    else
        v = static_cast<std::uint16_t>(bus_read8(a) | (bus_read8(a + 1) << 8));
    if (on_access) on_access({AccessKind::Read, a, 2, v, 0});
    return v;
}

void Machine::write8(Addr a, std::uint8_t v) {
    a &= 0xFFFFFF;
    const std::uint8_t old = on_access ? peek8(a) : 0;
    bus_write8(a, v);
    CachedInsn& c = decode_cache_[(a >> 1) % kDecodeCache];
    if (c.addr == (a & ~1u)) c.valid = false;
    CachedInsn& c2 = decode_cache_[((a >> 1) - 1) % kDecodeCache];
    if (c2.addr == (a & ~1u) - 2) c2.valid = false;
    if (on_access) on_access({AccessKind::Write, a, 1, v, old});
}

void Machine::write16(Addr a, std::uint16_t v) {
    a &= 0xFFFFFF;
    const std::uint16_t old = on_access ? peek16(a) : 0;
    if ((a >= 0xFE00 && a <= 0xFFFF) || (a >= 0xF000 && a < 0xF200)) {
        sfr_write(a, v, 0xFFFF);
    } else {
        bus_write8(a, static_cast<std::uint8_t>(v));
        bus_write8(a + 1, static_cast<std::uint8_t>(v >> 8));
        for (Addr x : {a, a - 2, a + 2}) {
            CachedInsn& c = decode_cache_[(x >> 1) % kDecodeCache];
            if (c.addr == (x & ~1u)) c.valid = false;
        }
    }
    if (on_access) on_access({AccessKind::Write, a, 2, v, old});
}

std::uint8_t Machine::peek8(Addr a) {
    a &= 0xFFFFFF;
    if (a >= 0xF000 && a <= 0xFFFF) {
        if (a >= 0xFE00 || a < 0xF200) {
            std::uint16_t v = sfr_get(a);
            if ((a & ~1u) == reg::WDT) v = wdt_value();
            return static_cast<std::uint8_t>(v >> (8 * (a & 1)));
        }
        return a >= board_.iram_start ? iram_[a - 0xF000] : 0xFF;
    }
    Region* r = nullptr;
    if (internal(a, &r)) return r->device->peek8(a);
    int cs = 0;
    Device* d = external_device(a, &cs);
    return d ? d->peek8(a) : 0xFF;
}

void Machine::poke8(Addr a, std::uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xF000 && a <= 0xFFFF) {
        if (a >= 0xFE00 || a < 0xF200) {
            const int shift = 8 * (a & 1);
            sfr_set(a, static_cast<std::uint16_t>((sfr_get(a) & ~(0xFF << shift)) | (v << shift)));
        } else if (a >= board_.iram_start) {
            iram_[a - 0xF000] = v;
        }
    } else {
        Region* r = nullptr;
        int cs = 0;
        if (internal(a, &r))
            r->device->poke8(a, v);
        else if (Device* d = external_device(a, &cs))
            d->poke8(a, v);
    }
    std::fill(decode_cache_.begin(), decode_cache_.end(), CachedInsn{});
}

std::string Machine::describe(Addr a) {
    a &= 0xFFFFFF;
    if (a >= 0xFE00 && a <= 0xFFFF) return "SFR";
    if (a >= 0xF000 && a < 0xF200) return "ESFR";
    if (a >= 0xF000 && a <= 0xFFFF) return a >= board_.iram_start ? "IRAM" : "reserved";
    Region* r = nullptr;
    if (internal(a, &r)) return r->device->name();
    int cs = 0;
    Device* d = external_device(a, &cs);
    return "CS" + std::to_string(cs) + (d ? ":" + d->name() : std::string(":none"));
}

Addr Machine::data_address(std::uint16_t a) const {
    switch (ext_.kind) {
    case dis::ExtState::Kind::Page: return ((ext_.value & 0x3FF) << 14) | (a & 0x3FFFu);
    case dis::ExtState::Kind::Seg: return ((ext_.value & 0xFF) << 16) | a;
    case dis::ExtState::Kind::None: break;
    }
    const std::uint16_t dpp = sfr_get(reg::DPP0 + 2 * (a >> 14));
    return (static_cast<Addr>(dpp & 0x3FF) << 14) | (a & 0x3FFFu);
}

void Machine::report_unmodelled(Addr a, bool write, std::uint16_t value, std::string what) {
    const std::uint32_t key = (a & 0xFFFFFF) | (write ? 0x1000000u : 0);
    if (!unmodelled_seen_.insert(key).second) return;
    unmodelled_.push_back({instructions_, cur_pc_, a, write, value, std::move(what)});
    if (stop_on_unmodelled && stop_pending_ == Stop::None) stop_pending_ = Stop::Unmodelled;
}

std::uint16_t Machine::gpr(int n) const {
    const Addr a = static_cast<Addr>(sfr_get(reg::CP) + 2 * n) & 0xFFFF;
    if (a < 0xF000 || a < board_.iram_start || a >= 0xFE00) return 0;
    return static_cast<std::uint16_t>(iram_[a - 0xF000] | (iram_[a - 0xF000 + 1] << 8));
}

void Machine::set_gpr(int n, std::uint16_t v) {
    const Addr a = static_cast<Addr>(sfr_get(reg::CP) + 2 * n) & 0xFFFF;
    if (a < 0xF000 || a < board_.iram_start || a >= 0xFE00) return;
    iram_[a - 0xF000] = static_cast<std::uint8_t>(v);
    iram_[a - 0xF000 + 1] = static_cast<std::uint8_t>(v >> 8);
}

// ---- Clock and events -----------------------------------------------------

void Machine::schedule(Device& d, std::uint64_t delay, std::uint32_t tag) {
    events_.push_back({cycles_ + delay, event_seq_++, d.id, tag});
    std::push_heap(events_.begin(), events_.end(), [](const Event& a, const Event& b) { return event_later(a, b); });
}

void Machine::cancel(Device& d, std::uint32_t tag) {
    auto it = std::remove_if(events_.begin(), events_.end(),
                             [&](const Event& e) { return e.device == d.id && e.tag == tag; });
    if (it == events_.end()) return;
    events_.erase(it, events_.end());
    std::make_heap(events_.begin(), events_.end(), [](const Event& a, const Event& b) { return event_later(a, b); });
}

void Machine::run_events() {
    auto later = [](const Event& a, const Event& b) { return event_later(a, b); };
    while (!events_.empty() && events_.front().time <= cycles_) {
        std::pop_heap(events_.begin(), events_.end(), later);
        const Event e = events_.back();
        events_.pop_back();
        if (e.device >= 0 && static_cast<std::size_t>(e.device) < devices_.size())
            devices_[static_cast<std::size_t>(e.device)]->event(*this, e.tag);
    }
}

// ---- Stack, traps and interrupts ------------------------------------------

// The circular stack that SYSCON.STKSZ selects is not modelled: SP moves
// freely and STKOV/STKUN raise their traps. (The stock 5WY19 firmware sets
// STKSZ to 111b, no wrap-around.)
void Machine::push(std::uint16_t v) {
    const std::uint16_t sp = static_cast<std::uint16_t>(sfr_get(reg::SP) - 2);
    sfr_set(reg::SP, sp);
    write16(sp, v);
    if (!in_trap_entry_ && sp < sfr_get(reg::STKOV)) pending_traps_ |= kStkof;
}

std::uint16_t Machine::pop() {
    const std::uint16_t sp = sfr_get(reg::SP);
    const std::uint16_t v = read16(sp);
    const std::uint16_t next = static_cast<std::uint16_t>(sp + 2);
    sfr_set(reg::SP, next);
    if (next > sfr_get(reg::STKUN)) pending_traps_ |= kStkuf;
    return v;
}

void Machine::raise_trap(std::uint16_t bit) { pending_traps_ |= bit; }

void Machine::nmi() { pending_traps_ |= kNmi; }

void Machine::enter_trap(std::uint8_t trap, bool hardware) {
    in_trap_entry_ = true;
    const bool segmented = !(sfr_get(reg::SYSCON) & 0x0800);
    push(psw());
    if (segmented) push(csp());
    push(ip_);
    in_trap_entry_ = false;
    sfr_set(reg::CSP, 0);
    ip_ = static_cast<std::uint16_t>(trap * 4);
    if (hardware) set_psw(static_cast<std::uint16_t>((psw() & 0x0FFF) | 0xF000));
    idle_ = false;
    cycles_ += 4;
}

void Machine::check_irq() {
    if (!irq_dirty_) return;
    irq_dirty_ = false;
    irq_best_ = -1;
    irq_best_level_ = -1;
    for (std::size_t i = 0; i < board_.interrupts.size(); ++i) {
        const std::uint16_t ic = sfr_get(board_.interrupts[i].ic);
        if ((ic & 0xC0) != 0xC0) continue;
        const int prio = ic & 0x3F;  // ILVL and GLVL
        if ((prio >> 2) == 0) continue;
        if (prio > irq_best_level_) {
            irq_best_level_ = prio;
            irq_best_ = static_cast<int>(i);
        }
    }
}

void Machine::request_interrupt(Addr ic) {
    sfr_set(ic, static_cast<std::uint16_t>(sfr_get(ic) | 0x80));
    irq_dirty_ = true;
}

bool Machine::take_interrupt() {
    check_irq();
    if (irq_best_ < 0) return false;
    const std::uint16_t p = psw();
    if (!(p & static_cast<std::uint16_t>(Psw::IEN))) return false;
    if (ext_.left > 0) return false;  // EXT and ATOMIC sequences are not interrupted
    const int level = irq_best_level_ >> 2;
    if (level <= (p >> 12)) return false;
    const InterruptSource& src = board_.interrupts[static_cast<std::size_t>(irq_best_)];
    const std::uint16_t ic = sfr_get(src.ic);
    irq_dirty_ = true;
    if (level >= 14) {
        // Peripheral event controller: a single data transfer instead of
        // an interrupt, while the channel's count is not zero.
        const int ch = (level - 14) * 4 + (ic & 3);
        const Addr pecc_addr = reg::PECC0 + 2 * static_cast<Addr>(ch);
        std::uint16_t pecc = sfr_get(pecc_addr);
        const int count = pecc & 0xFF;
        if (count != 0) {
            const Addr srcp = 0xFCE0 + 4 * static_cast<Addr>(ch), dstp = srcp + 2;
            std::uint16_t s = peek16(srcp), d = peek16(dstp);
            const bool byte = pecc & 0x100;
            const int inc = (pecc >> 9) & 3;
            if (byte)
                write8(d, read8(s));
            else
                write16(d & ~1u, read16(s & ~1u));
            const std::uint16_t step = byte ? 1 : 2;
            if (inc == 1) d = static_cast<std::uint16_t>(d + step);
            if (inc == 2) s = static_cast<std::uint16_t>(s + step);
            poke8(srcp, static_cast<std::uint8_t>(s));
            poke8(srcp + 1, static_cast<std::uint8_t>(s >> 8));
            poke8(dstp, static_cast<std::uint8_t>(d));
            poke8(dstp + 1, static_cast<std::uint8_t>(d >> 8));
            bool keep_request = false;
            if (count != 0xFF) {
                pecc = static_cast<std::uint16_t>((pecc & 0xFF00) | (count - 1));
                sfr_set(pecc_addr, pecc);
                keep_request = count == 1;  // then a normal interrupt follows
            }
            if (!keep_request) sfr_set(src.ic, static_cast<std::uint16_t>(ic & ~0x80));
            cycles_ += 2;
            return true;
        }
    }
    sfr_set(src.ic, static_cast<std::uint16_t>(ic & ~0x80));
    const std::uint16_t old_psw = psw();
    enter_trap(src.trap, false);
    (void)old_psw;
    set_psw(static_cast<std::uint16_t>((psw() & 0x0FFF) | (level << 12)));
    return true;
}

// ---- Snapshots -------------------------------------------------------------

Snapshot Machine::save() const {
    StateWriter w;
    w.put(ip_);
    w.put(cycles_);
    w.put(instructions_);
    w.put(ext_);
    w.put(einit_);
    w.put(idle_);
    w.put(pwrdn_);
    w.put(pending_traps_);
    w.bytes(iram_);
    w.put(sfr_);
    w.put(esfr_);
    w.put(wdt_enabled_);
    w.put(wdt_start_);
    w.put(wdt_start_cycle_);
    w.put(event_seq_);
    w.put<std::uint64_t>(events_.size());
    for (const Event& e : events_) w.put(e);
    for (const auto& d : devices_) d->save(w);
    Snapshot s;
    s.cycles = cycles_;
    s.instructions = instructions_;
    s.data = std::move(w.data);
    s.shared = std::move(w.shared_);
    return s;
}

void Machine::load(const Snapshot& s, bool replay) {
    StateReader r(s.data, s.shared);
    r.get(ip_);
    r.get(cycles_);
    r.get(instructions_);
    r.get(ext_);
    r.get(einit_);
    r.get(idle_);
    r.get(pwrdn_);
    r.get(pending_traps_);
    r.bytes(iram_);
    r.get(sfr_);
    r.get(esfr_);
    r.get(wdt_enabled_);
    r.get(wdt_start_);
    r.get(wdt_start_cycle_);
    r.get(event_seq_);
    events_.resize(r.get<std::uint64_t>());
    for (Event& e : events_) r.get(e);
    for (auto& d : devices_) d->load(r);
    for (auto& d : devices_) d->after_load(*this, replay);
    if (SfrHook* h = hook(reg::SYSCON)) h->readonly_mask = einit_ ? 0x1FFF : 0;
    irq_dirty_ = true;
    stop_pending_ = Stop::None;
    std::fill(decode_cache_.begin(), decode_cache_.end(), CachedInsn{});
}

std::optional<dis::Insn> Machine::decode_at(Addr a) {
    a &= 0xFFFFFF;
    CachedInsn& c = decode_cache_[(a >> 1) % kDecodeCache];
    if (c.valid && c.addr == a) return c.insn;
    std::uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = peek8((a & 0xFF0000) | ((a + static_cast<Addr>(i)) & 0xFFFF));
    auto insn = dis::decode(b, 4, board_.cpu);
    if (!insn) return std::nullopt;
    // Only cache code outside RAM and SFRs, where writes are rare.
    if (!(a >= 0xF000 && a <= 0xFFFF)) {
        c.addr = a;
        c.insn = *insn;
        c.valid = true;
    }
    return insn;
}

const char* stop_name(Stop s) {
    switch (s) {
    case Stop::None: return "none";
    case Stop::Breakpoint: return "breakpoint";
    case Stop::Watchpoint: return "watchpoint";
    case Stop::Unmodelled: return "unmodelled";
    case Stop::PowerDown: return "powerdown";
    case Stop::Idle: return "idle";
    case Stop::Halted: return "halted";
    case Stop::Undefined: return "undefined";
    }
    return "?";
}

}  // namespace oc166::sim
