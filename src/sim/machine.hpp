// SPDX-License-Identifier: MIT
// The C166/C167 machine: CPU core, bus with chip selects, SFR dispatch,
// interrupts and traps, watchdog and a virtual clock. See
// docs/debugger-design.md, sections 3.3 to 3.7.
#pragma once

#include "disasm.hpp"
#include "isa/isa_types.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace oc166::sim {

using Addr = std::uint32_t;  // physical 24-bit address

// ---- Serialization for snapshots ------------------------------------------

class StateWriter {
public:
    template <class T>
    void put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        data.insert(data.end(), p, p + sizeof(T));
    }
    void bytes(const std::vector<std::uint8_t>& v) {
        put<std::uint64_t>(v.size());
        data.insert(data.end(), v.begin(), v.end());
    }
    // A large buffer that may be shared with the live machine (flash
    // contents): the snapshot keeps a reference instead of a copy.
    void shared(std::shared_ptr<const std::vector<std::uint8_t>> p) {
        put<std::uint64_t>(shared_.size());
        shared_.push_back(std::move(p));
    }
    std::vector<std::uint8_t> data;
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> shared_;
};

class StateReader {
public:
    StateReader(const std::vector<std::uint8_t>& d, const std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>& s)
        : data_(d), shared_(s) {}
    template <class T>
    T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v;
        std::memcpy(&v, take(sizeof(T)), sizeof(T));
        return v;
    }
    template <class T>
    void get(T& v) {
        v = get<T>();
    }
    void bytes(std::vector<std::uint8_t>& v) {
        const auto n = get<std::uint64_t>();
        const std::uint8_t* p = take(n);
        v.assign(p, p + n);
    }
    std::shared_ptr<const std::vector<std::uint8_t>> shared() { return shared_.at(get<std::uint64_t>()); }

private:
    const std::uint8_t* take(std::size_t n);
    const std::vector<std::uint8_t>& data_;
    const std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>& shared_;
    std::size_t pos_ = 0;
};

// The full machine state at one point in time.
struct Snapshot {
    std::uint64_t cycles = 0;
    std::uint64_t instructions = 0;
    std::vector<std::uint8_t> data;
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> shared;
};

class Machine;

// ---- Devices ---------------------------------------------------------------

// Something on the bus: memory, a flash chip, a latch, an on-chip peripheral.
// Addresses passed in are full physical addresses. A device that keeps state
// saves and restores it for snapshots, and may schedule events on the clock.
class Device {
public:
    virtual ~Device() = default;
    virtual std::string name() const = 0;
    virtual std::uint8_t read8(Addr a) = 0;
    virtual void write8(Addr a, std::uint8_t v) = 0;
    virtual std::uint16_t read16(Addr a) { return static_cast<std::uint16_t>(read8(a) | (read8(a + 1) << 8)); }
    virtual void write16(Addr a, std::uint16_t v) {
        write8(a, static_cast<std::uint8_t>(v));
        write8(a + 1, static_cast<std::uint8_t>(v >> 8));
    }
    // Reads without side effects, for the debugger's memory view.
    virtual std::uint8_t peek8(Addr a) { return read8(a); }
    // Loads bytes without side effects (a debugger or loader write).
    virtual void poke8(Addr a, std::uint8_t v) { write8(a, v); }
    virtual void reset(bool power_on) { (void)power_on; }
    virtual void save(StateWriter&) const {}
    virtual void load(StateReader&) {}
    // A scheduled event of this device has come due.
    virtual void event(Machine&, std::uint32_t tag) { (void)tag; }
    // Called once when the device joins a machine, after the core's SFR
    // hooks exist: on-chip peripherals install their SFR hooks here.
    virtual void attach(Machine&) {}
    // Called after a snapshot is loaded, for state that lives outside
    // snapshots: input logged from the host is scheduled again when
    // 'replay' is set (reverse execution), and dropped when not (a new
    // timeline from an older state).
    virtual void after_load(Machine&, bool replay) { (void)replay; }

    int id = -1;  // set by Machine::add_device
};

// RAM: 'mask' selects the address bits the chip sees, so a 64 KB RAM
// mirrors through its whole chip-select window.
class Ram final : public Device {
public:
    Ram(std::string name, std::size_t size, Addr mask) : name_(std::move(name)), mem_(size, 0), mask_(mask) {}
    std::string name() const override { return name_; }
    std::uint8_t read8(Addr a) override { return mem_[(a & mask_) % mem_.size()]; }
    void write8(Addr a, std::uint8_t v) override { mem_[(a & mask_) % mem_.size()] = v; }
    void save(StateWriter& w) const override { w.bytes(mem_); }
    void load(StateReader& r) override { r.bytes(mem_); }
    std::vector<std::uint8_t>& memory() { return mem_; }

private:
    std::string name_;
    std::vector<std::uint8_t> mem_;
    Addr mask_;
};

// ROM or flash read as an image. Writes are ignored and reported as
// unmodelled. The contents are shared with snapshots and copied only when
// changed (poke8), so snapshots of large images cost nothing.
class Rom : public Device {
public:
    Rom(std::string name, std::vector<std::uint8_t> image, Addr mask)
        : name_(std::move(name)), image_(std::make_shared<std::vector<std::uint8_t>>(std::move(image))), mask_(mask) {}
    std::string name() const override { return name_; }
    std::uint8_t read8(Addr a) override {
        const std::size_t i = a & mask_;
        return i < image_->size() ? (*image_)[i] : 0xFF;
    }
    void write8(Addr a, std::uint8_t v) override;
    void poke8(Addr a, std::uint8_t v) override;
    void save(StateWriter& w) const override { w.shared(image_); }
    void load(StateReader& r) override { image_ = r.shared(); }
    const std::vector<std::uint8_t>& image() const { return *image_; }

    Machine* machine = nullptr;  // for reporting writes

protected:
    std::string name_;
    std::shared_ptr<const std::vector<std::uint8_t>> image_;
    Addr mask_;
};

// A device whose behaviour comes from callbacks (the Python binding uses it
// for board devices written in Python). It keeps no snapshot state.
class CallbackDevice final : public Device {
public:
    std::string device_name;
    std::function<std::uint8_t(Addr)> on_read;
    std::function<void(Addr, std::uint8_t)> on_write;
    std::string name() const override { return device_name; }
    std::uint8_t read8(Addr a) override { return on_read ? on_read(a) : 0xFF; }
    void write8(Addr a, std::uint8_t v) override {
        if (on_write) on_write(a, v);
    }
    std::uint8_t peek8(Addr) override { return 0xFF; }
    void poke8(Addr, std::uint8_t) override {}
};

// ---- SFR hooks -------------------------------------------------------------

// Behaviour of one SFR or ESFR word. Without a hook a register is plain
// storage, and the first accesses to it are logged as unmodelled.
struct SfrHook {
    // Value read by the CPU; default: the stored value.
    std::function<std::uint16_t(Machine&, Addr, std::uint16_t stored)> read;
    // Called after a write: 'old' is the value before, 'value' the one stored
    // (already merged for byte writes). It may change the stored value.
    std::function<void(Machine&, Addr, std::uint16_t old, std::uint16_t value)> write;
    // Bits the CPU cannot change (read-only bits keep their value).
    std::uint16_t readonly_mask = 0;
    bool modelled = true;
};

// ---- Board -----------------------------------------------------------------

struct InterruptSource {
    std::string name;
    Addr ic = 0;          // address of the xxIC register
    std::uint8_t trap = 0;  // trap number: the vector is at trap * 4
};

struct Region {
    Addr start = 0;
    Addr size = 0;
    int xpercon_bit = -1;  // enabled by this XPERCON bit (and SYSCON.XPEN); -1: always
    std::shared_ptr<Device> device;
};

struct Board {
    std::string name = "C167";
    isa::Cpu cpu = isa::Cpu::C167;
    std::uint32_t clock_hz = 25'000'000;
    Addr iram_start = 0xF600;  // internal RAM ends at 0FDFFH
    // On-chip XRAM and X-peripherals, in segment 0.
    std::vector<Region> internal;
    // Devices on the external bus by chip select CS0..CS4. A missing one
    // reads 0FFh and logs the access as unmodelled.
    std::array<std::shared_ptr<Device>, 5> chip_selects;
    // Register values after reset that depend on configuration pins.
    std::uint16_t syscon_reset = 0x0000;
    std::uint16_t buscon0_reset = 0x0000;
    std::uint16_t rp0h_reset = 0x00FF;
    std::uint16_t xpercon_reset = 0x0005;
    std::vector<InterruptSource> interrupts;
    // Other devices that need resets, snapshots or events (peripherals).
    std::vector<std::shared_ptr<Device>> devices;
};

// A bare C167CR-like board: 'image' as ROM on CS0 from address 0, mirrored
// every image size, and 64 KB of RAM nowhere. For tests.
Board simple_board(std::vector<std::uint8_t> image, isa::Cpu cpu = isa::Cpu::C167);

// The standard C167 interrupt sources (CAPCOM, GPT, ASC0, SSC, ADC, ...).
std::vector<InterruptSource> c167_interrupts();

// ---- Machine ---------------------------------------------------------------

// Why run() or step() returned.
enum class Stop : std::uint8_t {
    None,         // the requested number of instructions ran
    Breakpoint,   // an address breakpoint was reached (before executing it)
    Watchpoint,   // a data watchpoint matched (after the instruction)
    Unmodelled,   // an unmodelled register or address was accessed (stop_on_unmodelled)
    PowerDown,    // PWRDN: only a reset continues
    Idle,         // IDLE with no interrupt that could wake the CPU
    Halted,       // a client asked to stop (request_stop)
    Undefined,    // an undefined opcode with stop_on_undefined set
};

enum class AccessKind : std::uint8_t { Read = 1, Write = 2 };

struct Access {
    AccessKind kind;
    Addr address;
    std::uint8_t size;  // 1 or 2
    std::uint16_t value;
    std::uint16_t old;  // for writes: the value before
};

struct UnmodelledAccess {
    std::uint64_t instruction;  // count at the access
    Addr pc;                    // CSP:IP of the instruction
    Addr address;
    bool write;
    std::uint16_t value;
    std::string what;  // e.g. "SFR 0FEB0H" or "CS3"
};

enum class Psw : std::uint16_t {
    N = 1 << 0, C = 1 << 1, V = 1 << 2, Z = 1 << 3, E = 1 << 4, MULIP = 1 << 5, USR0 = 1 << 6,
    HLDEN = 1 << 10, IEN = 1 << 11,
};

// SFR addresses used by the core.
namespace reg {
constexpr Addr DPP0 = 0xFE00, CSP = 0xFE08, MDH = 0xFE0C, MDL = 0xFE0E, CP = 0xFE10, SP = 0xFE12,
               STKOV = 0xFE14, STKUN = 0xFE16, ADDRSEL1 = 0xFE18, WDT = 0xFEAE, PECC0 = 0xFEC0,
               BUSCON0 = 0xFF0C, MDC = 0xFF0E, PSW = 0xFF10, SYSCON = 0xFF12, BUSCON1 = 0xFF14,
               RP0H = 0xF108, ZEROS = 0xFF1C, ONES = 0xFF1E, TFR = 0xFFAC, WDTCON = 0xFFAE,
               XPERCON = 0xF024;
}

class Machine {
public:
    explicit Machine(Board board);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    const Board& board() const { return board_; }

    // ---- Running
    // Power-on reset (all reset flags) or the reset that SRST and the
    // watchdog cause.
    enum class ResetKind : std::uint8_t { PowerOn, Software, Watchdog, Hardware };
    void reset(ResetKind kind = ResetKind::PowerOn);
    // Executes one instruction (or takes one interrupt or trap, or idles one
    // tick). An EXT or ATOMIC sequence is not split by interrupts, but each
    // of its instructions is a step.
    Stop step();
    // Runs up to 'max_instructions' (0: no limit) until a stop.
    Stop run(std::uint64_t max_instructions);
    void request_stop() { stop_requested_ = true; }

    // ---- State
    Addr pc() const { return (static_cast<Addr>(csp()) << 16) | ip_; }
    std::uint16_t ip() const { return ip_; }
    void set_ip(std::uint16_t v) { ip_ = v; }
    std::uint16_t csp() const { return sfr_get(reg::CSP); }
    void set_csp(std::uint16_t v) { sfr_set(reg::CSP, v & 0xFF); }
    std::uint16_t psw() const { return sfr_get(reg::PSW); }
    void set_psw(std::uint16_t v) { sfr_set(reg::PSW, v); irq_dirty_ = true; }
    std::uint16_t gpr(int n) const;
    void set_gpr(int n, std::uint16_t v);
    std::uint64_t cycles() const { return cycles_; }
    std::uint64_t instructions() const { return instructions_; }
    double seconds() const { return static_cast<double>(cycles_) / board_.clock_hz; }
    bool einit_done() const { return einit_; }
    bool idle() const { return idle_; }
    bool powered_down() const { return pwrdn_; }
    const dis::ExtState& ext() const { return ext_; }

    // SFR words as stored (no side effects), by address 0F000H..0F1FFH or
    // 0FE00H..0FFFFH.
    std::uint16_t sfr_get(Addr a) const;
    void sfr_set(Addr a, std::uint16_t v);
    void set_hook(Addr sfr, SfrHook hook);

    // ---- Memory
    // Accesses as the CPU makes them (side effects, watchpoints, logging).
    std::uint8_t read8(Addr a);
    std::uint16_t read16(Addr a);
    void write8(Addr a, std::uint8_t v);
    void write16(Addr a, std::uint16_t v);
    // Debugger access without side effects or watchpoints.
    std::uint8_t peek8(Addr a);
    std::uint16_t peek16(Addr a) { return static_cast<std::uint16_t>(peek8(a) | (peek8(a + 1) << 8)); }
    void poke8(Addr a, std::uint8_t v);
    // Name of what answers at 'a': "IRAM", "SFR", "CS0:flash", ...
    std::string describe(Addr a);
    // The 16-bit data address 'a' as the CPU would map it now (DPPs, or the
    // page or segment of an active EXTP/EXTS).
    Addr data_address(std::uint16_t a) const;

    // ---- Clock and events
    // Schedules 'tag' on 'device' 'delay' cycles from now.
    void schedule(Device& device, std::uint64_t delay, std::uint32_t tag);
    void cancel(Device& device, std::uint32_t tag);

    // ---- Interrupts
    // Sets the request flag (IR) of the interrupt with this IC register.
    void request_interrupt(Addr ic);
    // Requests the NMI trap.
    void nmi();

    // ---- Watchdog
    bool watchdog_frozen = false;  // the debugger can stop it from resetting the CPU
    bool watchdog_enabled() const { return wdt_enabled_; }

    // ---- Debug support
    // Called for every data access while set (watchpoints, tracing).
    std::function<void(const Access&)> on_access;
    // Called before each instruction while set; return true to stop
    // (breakpoints). Keep it cheap.
    std::function<bool(Addr pc)> on_fetch;
    bool stop_on_unmodelled = false;
    bool stop_on_undefined = false;
    // Unmodelled accesses, each address reported once.
    const std::vector<UnmodelledAccess>& unmodelled() const { return unmodelled_; }
    void clear_unmodelled() {
        unmodelled_.clear();
        unmodelled_seen_.clear();
    }
    void report_unmodelled(Addr a, bool write, std::uint16_t value, std::string what);
    // Set by a watchpoint callback to stop after the current instruction.
    void stop_after_instruction() { stop_pending_ = Stop::Watchpoint; }

    // ---- Snapshots
    Snapshot save() const;
    // 'replay': host input logged after the snapshot happens again, as when
    // the debugger re-runs history; otherwise it is dropped.
    void load(const Snapshot& s, bool replay = true);

    // ---- Devices
    void add_device(std::shared_ptr<Device> d);
    const std::vector<std::shared_ptr<Device>>& devices() const { return devices_; }
    // The first device of type T, or nullptr.
    template <class T>
    T* find_device() const {
        for (const auto& d : devices_)
            if (auto* p = dynamic_cast<T*>(d.get())) return p;
        return nullptr;
    }
    // Puts 'd' on chip select 'cs' (0..4), replacing what was there.
    void set_chip_select(int cs, std::shared_ptr<Device> d);

    // Decoded instruction at a physical address (cached).
    std::optional<dis::Insn> decode_at(Addr a);

private:
    friend class Cpu;
    friend struct Exec;

    // Bus routing.
    Device* external_device(Addr a, int* cs);
    bool internal(Addr a, Region** region);
    std::uint8_t bus_read8(Addr a);
    void bus_write8(Addr a, std::uint8_t v);
    std::uint16_t sfr_read(Addr a);
    void sfr_write(Addr a, std::uint16_t v, std::uint16_t mask);
    std::uint16_t* sfr_slot(Addr a);
    const std::uint16_t* sfr_slot(Addr a) const;
    SfrHook* hook(Addr a);

    void install_core_hooks();
    void enter_trap(std::uint8_t trap, bool hardware);
    bool take_interrupt();
    void raise_trap(std::uint16_t tfr_bit);
    void push(std::uint16_t v);
    std::uint16_t pop();
    void check_irq();
    void watchdog_reload();
    void watchdog_schedule();

    Board board_;
    std::vector<std::shared_ptr<Device>> devices_;
    bool attached_ = false;  // devices added from now on are attached at once
    std::vector<std::uint8_t> iram_;  // 0F000H..0FFFFH (only iram_start..0FDFFH is RAM)
    std::array<std::uint16_t, 256> sfr_{};   // 0FE00H..0FFFFH
    std::array<std::uint16_t, 256> esfr_{};  // 0F000H..0F1FFH
    std::array<std::unique_ptr<SfrHook>, 256> sfr_hooks_;
    std::array<std::unique_ptr<SfrHook>, 256> esfr_hooks_;

    std::uint16_t ip_ = 0;
    std::uint64_t cycles_ = 0;
    std::uint64_t instructions_ = 0;
    dis::ExtState ext_;
    bool einit_ = false;
    bool idle_ = false;
    bool pwrdn_ = false;
    bool in_trap_entry_ = false;
    std::uint16_t pending_traps_ = 0;  // TFR bits raised by the current instruction
    Stop stop_pending_ = Stop::None;
    bool stop_requested_ = false;
    Addr cur_pc_ = 0;

    // Interrupts.
    bool irq_dirty_ = true;
    int irq_best_ = -1;  // index into board_.interrupts of the best pending request
    int irq_best_level_ = 0;
    std::vector<int> ic_index_;  // per SFR/ESFR word: interrupt source, or -1 (512 entries)

    // Watchdog: the counter is computed from the cycle count.
    bool wdt_enabled_ = true;
    std::uint16_t wdt_start_ = 0;
    std::uint64_t wdt_start_cycle_ = 0;
    struct WatchdogDevice;
    std::shared_ptr<WatchdogDevice> wdt_device_;
    std::uint16_t wdt_value() const;

    // Event queue: (time, sequence, device, tag), kept as a heap.
    struct Event {
        std::uint64_t time;
        std::uint64_t seq;
        int device;
        std::uint32_t tag;
    };
    std::vector<Event> events_;
    std::uint64_t event_seq_ = 0;
    void run_events();

    // Decode cache by physical address; cleared on writes to cached code.
    struct CachedInsn {
        Addr addr = 0xFFFFFFFF;
        dis::Insn insn;
        bool valid = false;
    };
    std::vector<CachedInsn> decode_cache_;

    std::vector<UnmodelledAccess> unmodelled_;
    std::unordered_set<std::uint32_t> unmodelled_seen_;
};

const char* stop_name(Stop s);

}  // namespace oc166::sim
