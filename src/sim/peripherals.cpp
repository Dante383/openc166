// SPDX-License-Identifier: MIT
#include "sim/peripherals.hpp"

#include <algorithm>

namespace oc166::sim {

namespace {

// ASC0 registers.
constexpr Addr S0TBUF = 0xFEB0, S0RBUF = 0xFEB2, S0BG = 0xFEB4, S0CON = 0xFFB0, S0TIC = 0xFF6C, S0RIC = 0xFF6E,
               S0EIC = 0xFF70, S0TBIC = 0xF19C;
// S0CON bits.
constexpr std::uint16_t kStp = 1u << 3, kRen = 1u << 4, kFen = 1u << 6, kOen = 1u << 7, kFe = 1u << 9,
                        kOe = 1u << 10, kBrs = 1u << 13, kLb = 1u << 14, kRun = 1u << 15;

enum : std::uint32_t { kTxDone = 1, kRxDone = 2, kInput = 3 };

void set_ir(Machine& m, Addr ic) { m.request_interrupt(ic); }

// CAPCOM timer registers, in T0, T1, T7, T8 order.
constexpr Addr kTimer[4] = {0xFE50, 0xFE52, 0xF050, 0xF052};
constexpr Addr kReload[4] = {0xFE54, 0xFE56, 0xF054, 0xF056};
constexpr Addr kTimerIc[4] = {0xFF9C, 0xFF9E, 0xF17A, 0xF17C};
constexpr Addr T01CON = 0xFF50, T78CON = 0xFF20;

// ADC registers.
constexpr Addr ADCON = 0xFFA0, ADDAT = 0xFEA0, ADCIC = 0xFF98;
constexpr std::uint16_t kAdst = 1u << 7, kAdbsy = 1u << 8;

}  // namespace

// ---- ASC0 ------------------------------------------------------------------

std::uint64_t Asc0::frame_cycles(const Machine& m) const {
    const std::uint16_t con = m.sfr_get(S0CON);
    const std::uint64_t bit = 16u * (2u + ((con & kBrs) ? 1u : 0u)) * (static_cast<std::uint64_t>(m.sfr_get(S0BG) & 0x1FFF) + 1u);
    const int mode = con & 7;
    const int data = (mode == 1 || mode == 3) ? 8 : 9;  // 7 bits + parity counts as 8
    return bit * static_cast<std::uint64_t>(1 + data + ((con & kStp) ? 2 : 1));
}

void Asc0::attach(Machine& m) {
    m_ = &m;
    SfrHook plain;
    m.set_hook(S0BG, plain);
    SfrHook con;
    con.write = [this](Machine& mm, Addr, std::uint16_t, std::uint16_t v) {
        if ((v & kRun) && !tx_busy_ && tbuf_full_) start_tx(mm);
    };
    m.set_hook(S0CON, con);
    SfrHook tbuf;
    tbuf.write = [this](Machine& mm, Addr, std::uint16_t, std::uint16_t v) {
        tbuf_ = static_cast<std::uint8_t>(v);
        tbuf_full_ = true;
        if (!tx_busy_) start_tx(mm);
    };
    m.set_hook(S0TBUF, tbuf);
    SfrHook rbuf;
    rbuf.readonly_mask = 0xFFFF;
    m.set_hook(S0RBUF, rbuf);
}

void Asc0::reset(bool power_on) {
    tx_busy_ = tbuf_full_ = rx_busy_ = false;
    rx_fifo_.clear();
    if (power_on) {
        // A new timeline: nothing logged from before applies.
        log_.clear();
        any_out_ = false;
        last_out_cycle_ = 0;
    } else if (m_) {
        // The machine's events were cleared: bytes logged as arriving after
        // this reset still arrive.
        for (std::size_t i = 0; i < log_.size(); ++i)
            if (log_[i].cycle >= m_->cycles()) schedule_input(*m_, i);
    }
    inputs_seen_ = log_.size();
}

void Asc0::start_tx(Machine& m) {
    if (!(m.sfr_get(S0CON) & kRun) || !tbuf_full_) return;
    shift_ = tbuf_;
    tbuf_full_ = false;
    tx_busy_ = true;
    // The buffer is free again as soon as its byte moves to the shift
    // register.
    set_ir(m, S0TBIC);
    m.schedule(*this, frame_cycles(m), kTxDone);
}

void Asc0::start_rx(Machine& m) {
    if (rx_busy_ || rx_fifo_.empty()) return;
    rx_busy_ = true;
    m.schedule(*this, frame_cycles(m), kRxDone);
}

void Asc0::schedule_input(Machine& m, std::size_t index) {
    const std::uint64_t now = m.cycles();
    const std::uint64_t at = log_[index].cycle;
    m.schedule(*this, at > now ? at - now : 0, kInput | static_cast<std::uint32_t>(index << 4));
}

void Asc0::receive(Machine& m, std::uint8_t byte, bool framing_error) {
    // Input recorded for times after now belongs to a timeline that the
    // user has left by going back: it is dropped.
    const std::uint64_t now = m.cycles();
    while (!log_.empty() && log_.back().cycle > now) log_.pop_back();
    if (inputs_seen_ > log_.size()) inputs_seen_ = log_.size();
    log_.push_back({now, byte, framing_error});
    schedule_input(m, log_.size() - 1);
    inputs_seen_ = log_.size();
}

std::vector<std::uint8_t> Asc0::take_output() {
    std::vector<std::uint8_t> v(out_.begin() + static_cast<std::ptrdiff_t>(out_taken_), out_.end());
    out_taken_ = out_.size();
    return v;
}

void Asc0::event(Machine& m, std::uint32_t tag) {
    const std::uint16_t con = m.sfr_get(S0CON);
    switch (tag & 0xF) {
    case kTxDone: {
        tx_busy_ = false;
        const std::uint64_t now = m.cycles();
        if (!any_out_ || now > last_out_cycle_) {
            out_.push_back(shift_);
            last_out_cycle_ = now;
            any_out_ = true;
        }
        if ((con & kLb) || echo) {
            rx_fifo_.push_back(shift_);
            start_rx(m);
        }
        set_ir(m, S0TIC);
        if (tbuf_full_) start_tx(m);
        break;
    }
    case kInput: {
        const std::size_t i = tag >> 4;
        if (i < log_.size()) {
            rx_fifo_.push_back(static_cast<std::uint16_t>(log_[i].byte | (log_[i].framing_error ? 0x100 : 0)));
            start_rx(m);
        }
        break;
    }
    case kRxDone: {
        rx_busy_ = false;
        if (rx_fifo_.empty()) break;
        const std::uint16_t in = rx_fifo_.front();
        rx_fifo_.pop_front();
        if ((con & kRun) && (con & kRen)) {
            std::uint16_t c = con;
            bool error = false;
            if (m.sfr_get(S0RIC) & 0x80) {
                if (con & kOen) {
                    c |= kOe;
                    error = true;
                }
            } else {
                m.sfr_set(S0RBUF, in & 0xFF);
                set_ir(m, S0RIC);
            }
            if ((in & 0x100) && (con & kFen)) {
                c |= kFe;
                error = true;
            }
            if (c != con) m.sfr_set(S0CON, c);
            if (error) set_ir(m, S0EIC);
        }
        start_rx(m);
        break;
    }
    }
}

void Asc0::save(StateWriter& w) const {
    w.put(tx_busy_);
    w.put(tbuf_full_);
    w.put(tbuf_);
    w.put(shift_);
    w.put(rx_busy_);
    w.put<std::uint64_t>(rx_fifo_.size());
    for (auto v : rx_fifo_) w.put(v);
    w.put(inputs_seen_);
}

void Asc0::load(StateReader& r) {
    r.get(tx_busy_);
    r.get(tbuf_full_);
    r.get(tbuf_);
    r.get(shift_);
    r.get(rx_busy_);
    rx_fifo_.resize(r.get<std::uint64_t>());
    for (auto& v : rx_fifo_) r.get(v);
    r.get(inputs_seen_);
}

void Asc0::after_load(Machine& m) {
    // Input the host sent after the snapshot was taken.
    for (std::size_t i = static_cast<std::size_t>(inputs_seen_); i < log_.size(); ++i) schedule_input(m, i);
    inputs_seen_ = log_.size();
}

// ---- CAPCOM timers ---------------------------------------------------------

std::uint16_t CapcomTimers::value(const Machine& m, int i) const {
    const Timer& t = t_[static_cast<std::size_t>(i)];
    if (!t.running) return t.base;
    const std::uint64_t ticks = (m.cycles() - t.base_cycle) >> t.shift;
    std::uint64_t v = t.base + ticks;
    if (v > 0xFFFF) {
        const std::uint64_t rel = m.sfr_get(kReload[i]);
        const std::uint64_t period = 0x10000 - rel;
        v = rel + (v - 0x10000) % period;
    }
    return static_cast<std::uint16_t>(v);
}

void CapcomTimers::schedule(Machine& m, int i) {
    m.cancel(*this, static_cast<std::uint32_t>(i));
    const Timer& t = t_[static_cast<std::size_t>(i)];
    if (!t.running) return;
    // Cycles until the count passes 0FFFFH.
    const std::uint64_t ticks_left = 0x10000u - t.base;
    const std::uint64_t at = t.base_cycle + (ticks_left << t.shift);
    m.schedule(*this, at > m.cycles() ? at - m.cycles() : 0, static_cast<std::uint32_t>(i));
}

void CapcomTimers::rebase(Machine& m, int i) {
    Timer& t = t_[static_cast<std::size_t>(i)];
    t.base = value(m, i);
    // Keep the prescaler phase: count from the last tick.
    if (t.running) t.base_cycle = m.cycles() - ((m.cycles() - t.base_cycle) & ((1ull << t.shift) - 1));
    else t.base_cycle = m.cycles();
}

void CapcomTimers::configure(Machine& m, int i) {
    const std::uint16_t con = m.sfr_get(i < 2 ? T01CON : T78CON);
    const int shift = (i & 1) ? 8 : 0;
    Timer& t = t_[static_cast<std::size_t>(i)];
    rebase(m, i);
    const bool run = ((con >> (shift + 6)) & 1) && !((con >> (shift + 3)) & 1);
    if (run && !t.running) t.base_cycle = m.cycles();
    t.running = run;
    t.shift = ((con >> shift) & 7) + 3;
    schedule(m, i);
}

void CapcomTimers::attach(Machine& m) {
    for (int i = 0; i < 4; ++i) {
        SfrHook h;
        h.read = [this, i](Machine& mm, Addr, std::uint16_t) { return value(mm, i); };
        h.write = [this, i](Machine& mm, Addr, std::uint16_t, std::uint16_t v) {
            Timer& t = t_[static_cast<std::size_t>(i)];
            t.base = v;
            t.base_cycle = mm.cycles();
            schedule(mm, i);
        };
        m.set_hook(kTimer[i], h);
        m.set_hook(kReload[i], SfrHook{});
    }
    for (Addr con : {T01CON, T78CON}) {
        SfrHook h;
        h.write = [this, con](Machine& mm, Addr, std::uint16_t, std::uint16_t) {
            const int first = con == T01CON ? 0 : 2;
            configure(mm, first);
            configure(mm, first + 1);
        };
        m.set_hook(con, h);
    }
}

void CapcomTimers::reset(bool) { t_ = {}; }

void CapcomTimers::event(Machine& m, std::uint32_t tag) {
    const int i = static_cast<int>(tag & 3);
    Timer& t = t_[static_cast<std::size_t>(i)];
    if (!t.running) return;
    const std::uint64_t ticks_left = 0x10000u - t.base;
    t.base = m.sfr_get(kReload[i]);
    t.base_cycle += ticks_left << t.shift;
    set_ir(m, kTimerIc[i]);
    schedule(m, i);
}

void CapcomTimers::save(StateWriter& w) const { w.put(t_); }
void CapcomTimers::load(StateReader& r) { r.get(t_); }

// ---- ADC -------------------------------------------------------------------

void Adc::attach(Machine& m) {
    SfrHook con;
    con.readonly_mask = kAdbsy;
    con.write = [this](Machine& mm, Addr, std::uint16_t old, std::uint16_t v) {
        if ((v & kAdst) && !(old & kAdst) && !busy_) {
            channel_ = v & 0xF;
            busy_ = true;
            mm.sfr_set(ADCON, static_cast<std::uint16_t>(v | kAdbsy));
            mm.cancel(*this, 0);
            mm.schedule(*this, conversion_cycles, 0);
        }
        // Clearing ADST ends continuous and scan modes after the conversion
        // in progress (see event()).
    };
    m.set_hook(ADCON, con);
    SfrHook dat;
    dat.readonly_mask = 0xFFFF;
    m.set_hook(ADDAT, dat);
}

void Adc::reset(bool power_on) {
    busy_ = false;
    channel_ = 0;
    if (power_on) inputs_.fill(0x200);
}

void Adc::event(Machine& m, std::uint32_t) {
    std::uint16_t con = m.sfr_get(ADCON);
    m.sfr_set(ADDAT, static_cast<std::uint16_t>((channel_ << 12) | inputs_[static_cast<std::size_t>(channel_)]));
    set_ir(m, ADCIC);
    const int mode = (con >> 4) & 3;  // 0 single, 1 continuous, 2 scan, 3 continuous scan
    bool more = false;
    if (mode == 1 && (con & kAdst)) {
        more = true;
    } else if (mode >= 2) {
        if (channel_ > 0) {
            --channel_;
            more = true;
        } else if (mode == 3 && (con & kAdst)) {
            channel_ = con & 0xF;
            more = true;
        }
    }
    if (more) {
        m.schedule(*this, conversion_cycles, 0);
        return;
    }
    busy_ = false;
    con = static_cast<std::uint16_t>(con & ~(kAdbsy | kAdst));
    m.sfr_set(ADCON, con);
}

void Adc::save(StateWriter& w) const {
    w.put(channel_);
    w.put(busy_);
    w.put(inputs_);
}

void Adc::load(StateReader& r) {
    r.get(channel_);
    r.get(busy_);
    r.get(inputs_);
}

void add_c167_peripherals(Board& b) {
    auto asc = std::make_shared<Asc0>();
    b.devices.push_back(asc);
    b.devices.push_back(std::make_shared<CapcomTimers>());
    b.devices.push_back(std::make_shared<Adc>());
}

}  // namespace oc166::sim
