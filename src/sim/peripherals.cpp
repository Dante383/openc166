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

enum : std::uint32_t { kTxDone = 1, kRxDone = 2, kInput = 3, kLowDone = 4 };

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

std::uint64_t Asc0::bit_cycles(const Machine& m) const {
    const std::uint16_t con = m.sfr_get(S0CON);
    return 16u * (2u + ((con & kBrs) ? 1u : 0u)) * (static_cast<std::uint64_t>(m.sfr_get(S0BG) & 0x1FFF) + 1u);
}

std::uint64_t Asc0::frame_cycles(const Machine& m) const {
    const std::uint16_t con = m.sfr_get(S0CON);
    const std::uint64_t bit = bit_cycles(m);
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
    low_pending_.clear();
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

std::uint64_t Asc0::drop_future(Machine& m) {
    // Input recorded for times after now belongs to a timeline the user has
    // left by going back (or by loading an older snapshot): drop it, with
    // the arrivals already scheduled for it.
    const std::uint64_t now = m.cycles();
    bool dropped = false;
    while (!log_.empty() && log_.back().cycle > now) {
        m.cancel(*this, kInput | static_cast<std::uint32_t>((log_.size() - 1) << 4));
        log_.pop_back();
        dropped = true;
    }
    if (inputs_seen_ > log_.size()) inputs_seen_ = log_.size();
    // From here the output differs from what was reported before.
    if (dropped || last_out_cycle_ > now) last_out_cycle_ = now;
    return now;
}

void Asc0::receive(Machine& m, std::uint8_t byte, bool framing_error) {
    const std::uint64_t now = drop_future(m);
    log_.push_back({now, byte, framing_error, 0});
    schedule_input(m, log_.size() - 1);
    inputs_seen_ = log_.size();
}

void Asc0::receive_low(Machine& m, std::uint64_t low_cycles) {
    const std::uint64_t now = drop_future(m);
    log_.push_back({now, 0, false, std::max<std::uint64_t>(low_cycles, 1)});
    schedule_input(m, log_.size() - 1);
    inputs_seen_ = log_.size();
}

std::vector<std::uint8_t> Asc0::take_output() {
    std::vector<std::uint8_t> v;
    for (const auto& [b, baud] : take_output_with_baud()) v.push_back(b);
    return v;
}

std::vector<std::pair<std::uint8_t, std::uint32_t>> Asc0::take_output_with_baud() {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> v(out_.begin() + static_cast<std::ptrdiff_t>(out_taken_),
                                                          out_.end());
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
            const std::uint64_t bit = bit_cycles(m);
            out_.push_back({shift_, static_cast<std::uint32_t>((m.board().clock_hz + bit / 2) / bit)});
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
        if (i >= log_.size()) break;
        const Input& in = log_[i];
        if (in.low_cycles == 0) {
            rx_fifo_.push_back(static_cast<std::uint16_t>(in.byte | (in.framing_error ? 0x100 : 0)));
            start_rx(m);
            break;
        }
        // The start bit and the data bits that fell in the low phase read
        // as 0, the rest as 1 (LSB first). If the line was still low at the
        // stop bit, it is a framing error. The frame has already ended.
        const std::uint64_t bits = (in.low_cycles + bit_cycles(m) / 2) / bit_cycles(m);
        const int mode = con & 7;
        const std::uint64_t data = (mode == 1 || mode == 3) ? 8 : 9;
        std::uint16_t v = 0;
        if (bits <= 1) v = 0xFF;
        else if (bits - 1 < 8) v = static_cast<std::uint16_t>((0xFFu << (bits - 1)) & 0xFF);
        if (bits >= data + 2) v |= 0x100;  // the stop bit was low too
        // The UART completes the frame when it samples the stop bit, one
        // frame after the falling edge: that may still be ahead.
        const std::uint64_t frame = frame_cycles(m);
        low_pending_.push_back(v);
        m.schedule(*this, frame > in.low_cycles ? frame - in.low_cycles : 0, kLowDone);
        break;
    }
    case kLowDone: {
        if (low_pending_.empty()) break;
        const std::uint16_t v = low_pending_.front();
        low_pending_.pop_front();
        if (rx_busy_)
            rx_fifo_.push_back(v);
        else
            deliver(m, v);
        break;
    }
    case kRxDone: {
        rx_busy_ = false;
        if (rx_fifo_.empty()) break;
        const std::uint16_t in = rx_fifo_.front();
        rx_fifo_.pop_front();
        deliver(m, in);
        start_rx(m);
        break;
    }
    }
}

void Asc0::deliver(Machine& m, std::uint16_t in) {
    const std::uint16_t con = m.sfr_get(S0CON);
    if (!(con & kRun) || !(con & kRen)) return;
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

void Asc0::save(StateWriter& w) const {
    w.put(tx_busy_);
    w.put(tbuf_full_);
    w.put(tbuf_);
    w.put(shift_);
    w.put(rx_busy_);
    w.put<std::uint64_t>(rx_fifo_.size());
    for (auto v : rx_fifo_) w.put(v);
    w.put<std::uint64_t>(low_pending_.size());
    for (auto v : low_pending_) w.put(v);
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
    low_pending_.resize(r.get<std::uint64_t>());
    for (auto& v : low_pending_) r.get(v);
    r.get(inputs_seen_);
}

void Asc0::after_load(Machine& m, bool replay) {
    if (!replay) {
        // A new timeline: forget what the host sent after the snapshot.
        while (log_.size() > inputs_seen_) log_.pop_back();
        while (!log_.empty() && log_.back().cycle > m.cycles()) log_.pop_back();
        inputs_seen_ = log_.size();
        // Output from now on is new, even where it repeats old output.
        last_out_cycle_ = m.cycles();
        any_out_ = false;
        return;
    }
    // Input the host sent after the snapshot was taken.
    for (std::size_t i = static_cast<std::size_t>(inputs_seen_); i < log_.size(); ++i) schedule_input(m, i);
    inputs_seen_ = log_.size();
}

// ---- CAPCOM ----------------------------------------------------------------

namespace {
constexpr Addr kCcm[8] = {0xFF52, 0xFF54, 0xFF56, 0xFF58, 0xFF22, 0xFF24, 0xFF26, 0xFF28};
Addr cc_addr(int ch) { return ch < 16 ? 0xFE80u + 2u * static_cast<Addr>(ch) : 0xFE60u + 2u * static_cast<Addr>(ch - 16); }
Addr cc_ic(int ch) {
    if (ch < 16) return 0xFF78u + 2u * static_cast<Addr>(ch);
    if (ch <= 28) return 0xF160u + 2u * static_cast<Addr>(ch - 16);
    return 0xF184u + 8u * static_cast<Addr>(ch - 29);  // CC29IC 0F184H, CC30IC 0F18CH, CC31IC 0F194H
}
enum : std::uint32_t { kOverflow = 0, kCompare = 0x100 };
}  // namespace

int Capcom::mode(const Machine& m, int ch) const {
    return (m.sfr_get(kCcm[ch / 4]) >> (4 * (ch % 4))) & 7;
}

int Capcom::timer_of(const Machine& m, int ch) const {
    const bool acc = (m.sfr_get(kCcm[ch / 4]) >> (4 * (ch % 4) + 3)) & 1;
    return (ch < 16 ? 0 : 2) + (acc ? 1 : 0);
}

std::uint16_t Capcom::value(const Machine& m, int i) const {
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

void Capcom::schedule_overflow(Machine& m, int i) {
    m.cancel(*this, kOverflow | static_cast<std::uint32_t>(i));
    const Timer& t = t_[static_cast<std::size_t>(i)];
    if (!t.running) return;
    const std::uint64_t at = t.base_cycle + ((0x10000u - std::uint64_t{t.base}) << t.shift);
    m.schedule(*this, at > m.cycles() ? at - m.cycles() : 0, kOverflow | static_cast<std::uint32_t>(i));
}

void Capcom::schedule_compare(Machine& m, int ch) {
    m.cancel(*this, kCompare | static_cast<std::uint32_t>(ch));
    const int md = mode(m, ch);
    if (md < 4) return;
    const int ti = timer_of(m, ch);
    const Timer& t = t_[static_cast<std::size_t>(ti)];
    if (!t.running) return;
    // Modes 2 and 3 match once per timer period.
    if (md >= 6 && ((matched_this_period_ >> ch) & 1)) return;
    // The timer counts up from 'base' at base_cycle until it overflows (the
    // overflow event then rebases it), so only a match before then is
    // scheduled here: the timer passes CCx at tick (c - base).
    const std::uint16_t c = m.sfr_get(cc_addr(ch));
    const std::uint16_t now = value(m, ti);
    if (c <= now || c < t.base) return;  // next period (rescheduled at the overflow)
    const std::uint64_t at = t.base_cycle + (std::uint64_t{static_cast<std::uint16_t>(c - t.base)} << t.shift);
    m.schedule(*this, at > m.cycles() ? at - m.cycles() : 0, kCompare | static_cast<std::uint32_t>(ch));
}

void Capcom::schedule_timer_channels(Machine& m, int ti) {
    for (int ch = 0; ch < 32; ++ch)
        if ((ch < 16) == (ti < 2) && timer_of(m, ch) == ti) schedule_compare(m, ch);
}

void Capcom::rebase(Machine& m, int i) {
    Timer& t = t_[static_cast<std::size_t>(i)];
    t.base = value(m, i);
    // Keep the prescaler phase: count from the last tick.
    if (t.running) t.base_cycle = m.cycles() - ((m.cycles() - t.base_cycle) & ((1ull << t.shift) - 1));
    else t.base_cycle = m.cycles();
}

void Capcom::configure(Machine& m, int i) {
    const std::uint16_t con = m.sfr_get(i < 2 ? T01CON : T78CON);
    const int shift = (i & 1) ? 8 : 0;
    Timer& t = t_[static_cast<std::size_t>(i)];
    rebase(m, i);
    const bool run = ((con >> (shift + 6)) & 1) && !((con >> (shift + 3)) & 1);
    if (run && !t.running) t.base_cycle = m.cycles();
    t.running = run;
    t.shift = ((con >> shift) & 7) + 3;
    schedule_overflow(m, i);
    schedule_timer_channels(m, i);
}

void Capcom::attach(Machine& m) {
    for (int i = 0; i < 4; ++i) {
        SfrHook h;
        h.read = [this, i](Machine& mm, Addr, std::uint16_t) { return value(mm, i); };
        h.write = [this, i](Machine& mm, Addr, std::uint16_t, std::uint16_t v) {
            Timer& t = t_[static_cast<std::size_t>(i)];
            t.base = v;
            t.base_cycle = mm.cycles();
            schedule_overflow(mm, i);
            schedule_timer_channels(mm, i);
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
    for (int g = 0; g < 8; ++g) {
        SfrHook h;
        h.write = [this, g](Machine& mm, Addr, std::uint16_t, std::uint16_t) {
            for (int ch = 4 * g; ch < 4 * g + 4; ++ch) schedule_compare(mm, ch);
        };
        m.set_hook(kCcm[g], h);
    }
    for (int ch = 0; ch < 32; ++ch) {
        SfrHook h;
        h.write = [this, ch](Machine& mm, Addr, std::uint16_t, std::uint16_t) { schedule_compare(mm, ch); };
        m.set_hook(cc_addr(ch), h);
    }
}

void Capcom::reset(bool) {
    t_ = {};
    outputs_ = 0;
    matched_this_period_ = 0;
}

void Capcom::event(Machine& m, std::uint32_t tag) {
    if (tag & kCompare) {
        const int ch = static_cast<int>(tag & 0x1F);
        const int md = mode(m, ch);
        if (md < 4) return;
        ++matches_[static_cast<std::size_t>(ch)];
        if (md == 5) outputs_ ^= 1u << ch;
        if (md == 7) outputs_ |= 1u << ch;
        if (md >= 6) matched_this_period_ |= 1u << ch;
        set_ir(m, cc_ic(ch));
        return;
    }
    const int i = static_cast<int>(tag & 3);
    Timer& t = t_[static_cast<std::size_t>(i)];
    if (!t.running) return;
    t.base_cycle += (0x10000u - std::uint64_t{t.base}) << t.shift;
    t.base = m.sfr_get(kReload[i]);
    set_ir(m, kTimerIc[i]);
    // A new period: modes 2 and 3 may match again; mode 3 outputs clear.
    for (int ch = 0; ch < 32; ++ch) {
        if ((ch < 16) != (i < 2) || timer_of(m, ch) != i) continue;
        matched_this_period_ &= ~(1u << ch);
        if (mode(m, ch) == 7) outputs_ &= ~(1u << ch);
        // A channel equal to the reload value matches as the timer reloads.
        if (mode(m, ch) >= 4 && m.sfr_get(cc_addr(ch)) == t.base) {
            m.cancel(*this, kCompare | static_cast<std::uint32_t>(ch));
            m.schedule(*this, 0, kCompare | static_cast<std::uint32_t>(ch));
            continue;
        }
        schedule_compare(m, ch);
    }
    schedule_overflow(m, i);
}

void Capcom::input_edge(Machine& m, int ch, bool rising) {
    if (ch < 0 || ch > 31) return;
    const int md = mode(m, ch);
    const bool take = (md == 1 && rising) || (md == 2 && !rising) || md == 3;
    if (!take) return;
    m.sfr_set(cc_addr(ch), value(m, timer_of(m, ch)));
    set_ir(m, cc_ic(ch));
}

void Capcom::save(StateWriter& w) const {
    w.put(t_);
    w.put(outputs_);
    w.put(matched_this_period_);
    w.put(matches_);
}

void Capcom::load(StateReader& r) {
    r.get(t_);
    r.get(outputs_);
    r.get(matched_this_period_);
    r.get(matches_);
}

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

// ---- SSC -------------------------------------------------------------------

namespace {
constexpr Addr SSCCON = 0xFFB2, SSCTB = 0xF0B0, SSCRB = 0xF0B2, SSCBR = 0xF0B4, SSCTIC = 0xFF72, SSCRIC = 0xFF74;
constexpr Addr P6 = 0xFFCC;
constexpr std::uint16_t kSscEn = 1u << 15, kSscMs = 1u << 14, kSscBsy = 1u << 12;
}  // namespace

void Ssc::attach(Machine& m) {
    SfrHook con;
    m.set_hook(SSCCON, con);
    m.set_hook(SSCBR, SfrHook{});
    SfrHook tb;
    tb.write = [this](Machine& mm, Addr, std::uint16_t, std::uint16_t v) {
        tb_ = v;
        tb_full_ = true;
        if (!busy_) start(mm);
    };
    m.set_hook(SSCTB, tb);
    SfrHook rb;
    rb.readonly_mask = 0xFFFF;
    m.set_hook(SSCRB, rb);
}

void Ssc::reset(bool) {
    busy_ = tb_full_ = false;
}

void Ssc::start(Machine& m) {
    const std::uint16_t con = m.sfr_get(SSCCON);
    if (!(con & kSscEn) || !(con & kSscMs) || !tb_full_) return;
    shift_ = tb_;
    tb_full_ = false;
    busy_ = true;
    m.sfr_set(SSCCON, static_cast<std::uint16_t>(con | kSscBsy));
    set_ir(m, SSCTIC);
    const std::uint64_t bits = (con & 0xF) + 1u;
    const std::uint64_t bit = 2u * (static_cast<std::uint64_t>(m.sfr_get(SSCBR)) + 1u);
    m.schedule(*this, bits * bit, 0);
}

void Ssc::event(Machine& m, std::uint32_t) {
    busy_ = false;
    const std::uint16_t con = m.sfr_get(SSCCON);
    const std::uint16_t mask = static_cast<std::uint16_t>((1u << ((con & 0xF) + 1u)) - 1u);
    const std::uint16_t in = reply ? reply(static_cast<std::uint16_t>(shift_ & mask), m.sfr_get(P6)) : 0;
    ++transfers_;
    m.sfr_set(SSCRB, static_cast<std::uint16_t>(in & mask));
    m.sfr_set(SSCCON, static_cast<std::uint16_t>(con & ~kSscBsy));
    set_ir(m, SSCRIC);
    if (tb_full_) start(m);
}

void Ssc::save(StateWriter& w) const {
    w.put(busy_);
    w.put(tb_full_);
    w.put(tb_);
    w.put(shift_);
    w.put(transfers_);
}

void Ssc::load(StateReader& r) {
    r.get(busy_);
    r.get(tb_full_);
    r.get(tb_);
    r.get(shift_);
    r.get(transfers_);
}

void add_c167_peripherals(Board& b) {
    auto asc = std::make_shared<Asc0>();
    b.devices.push_back(asc);
    b.devices.push_back(std::make_shared<Capcom>());
    b.devices.push_back(std::make_shared<Adc>());
    b.devices.push_back(std::make_shared<Ssc>());
}

}  // namespace oc166::sim
