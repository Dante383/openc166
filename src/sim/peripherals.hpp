// SPDX-License-Identifier: MIT
// On-chip peripherals of the C167: the asynchronous serial port ASC0, the
// CAPCOM timers T0, T1, T7 and T8, and the A/D converter. Each works on the
// machine's virtual clock and keeps its state in snapshots.
#pragma once

#include "sim/machine.hpp"

#include <array>
#include <deque>
#include <vector>

namespace oc166::sim {

// ASC0 in its asynchronous modes. Bytes from the host are logged with the
// cycle they arrive at, so a replay after a snapshot restore (reverse
// execution) sees them again at the same time. Bytes the CPU sends are
// collected for the host once each, even when a replay sends them again.
class Asc0 final : public Device {
public:
    std::string name() const override { return "ASC0"; }
    std::uint8_t read8(Addr) override { return 0xFF; }
    void write8(Addr, std::uint8_t) override {}
    void attach(Machine& m) override;
    void reset(bool power_on) override;
    void save(StateWriter& w) const override;
    void load(StateReader& r) override;
    void after_load(Machine& m, bool replay) override;
    void event(Machine& m, std::uint32_t tag) override;

    // A byte arrives on RXD0 now (it is received one frame later).
    // 'framing_error' sends a frame without a stop bit, as a break or the
    // low phase of a K-line fast init looks to the UART.
    void receive(Machine& m, std::uint8_t byte, bool framing_error = false);
    // The line was held low for 'low_cycles' and has just gone high again
    // (a break, or the low phase of a K-line fast init). The UART frames
    // what it saw at its current baud rate: 25 ms low is 0E0H at 250 baud,
    // or 00H with a framing error at 10400 baud. Like a real UART it has
    // the byte when it samples the stop bit, one frame after the falling
    // edge.
    void receive_low(Machine& m, std::uint64_t low_cycles);
    // Bytes the CPU has sent since the last call.
    std::vector<std::uint8_t> take_output();
    // The same, each with the baud rate it was sent at (rounded), so a
    // bridge can drop bytes the tester could not have received: the 5WY19
    // boot block times its response delay by sending a byte at 250 kbaud.
    std::vector<std::pair<std::uint8_t, std::uint32_t>> take_output_with_baud();
    // Cycles one frame takes at the current settings.
    std::uint64_t frame_cycles(const Machine& m) const;
    // The CPU hears what it sends, as on a K-line (half duplex).
    bool echo = false;

private:
    struct Input {
        std::uint64_t cycle;
        std::uint8_t byte;
        bool framing_error;
        std::uint64_t low_cycles;  // a low pulse instead of a byte
    };
    std::uint64_t bit_cycles(const Machine& m) const;
    void deliver(Machine& m, std::uint16_t in);
    void start_tx(Machine& m);
    void start_rx(Machine& m);
    void schedule_input(Machine& m, std::size_t index);
    std::uint64_t drop_future(Machine& m);

    // Snapshot state.
    bool tx_busy_ = false;
    bool tbuf_full_ = false;
    std::uint8_t tbuf_ = 0;
    std::uint8_t shift_ = 0;
    bool rx_busy_ = false;
    std::deque<std::uint16_t> rx_fifo_;  // byte | 0x100 for a framing error
    std::deque<std::uint16_t> low_pending_;  // low pulses waiting for their stop bit
    std::uint64_t inputs_seen_ = 0;      // log entries scheduled before the snapshot

    // Outside snapshots: what the host sent and what it was given.
    std::vector<Input> log_;
    std::vector<std::pair<std::uint8_t, std::uint32_t>> out_;
    std::size_t out_taken_ = 0;
    std::uint64_t last_out_cycle_ = 0;
    bool any_out_ = false;
    Machine* m_ = nullptr;
};

// The capture/compare unit (CAPCOM1 and CAPCOM2 of the C167): timers T0
// and T1 (T01CON) and T7 and T8 (T78CON) in timer mode, counting
// fCPU / 2^(TxI + 3) and reloading from TxREL on overflow, which sets TxIR
// (counter mode, TxM, holds the timer); and the 32 channels CC0..CC31 with
// their modes in CCM0..CCM7. CC0..CC15 count with T0 or T1, CC16..CC31
// with T7 or T8.
//
// Compare modes 0 and 1 interrupt when the timer reaches CCx (mode 1 also
// toggles the channel's output); modes 2 and 3 likewise, once per timer
// period (mode 3 sets the output on the match and clears it when the timer
// overflows). Matches are exact events on the clock. Capture modes latch
// the timer into CCx on an input edge given by input_edge().
// Double-register compare mode is not modelled.
class Capcom final : public Device {
public:
    std::string name() const override { return "CAPCOM"; }
    std::uint8_t read8(Addr) override { return 0xFF; }
    void write8(Addr, std::uint8_t) override {}
    void attach(Machine& m) override;
    void reset(bool power_on) override;
    void save(StateWriter& w) const override;
    void load(StateReader& r) override;
    void event(Machine& m, std::uint32_t tag) override;

    // Timer 0..3 = T0, T1, T7, T8.
    std::uint16_t value(const Machine& m, int timer) const;
    // An edge on the input of channel 'ch' (a crank or cam sensor, say).
    void input_edge(Machine& m, int ch, bool rising);
    // The level of channel 'ch's compare output (modes 1 and 3).
    bool output(int ch) const { return (outputs_ >> ch) & 1; }
    // Compare events so far on channel 'ch'.
    std::uint64_t matches(int ch) const { return matches_[static_cast<std::size_t>(ch)]; }

private:
    struct Timer {
        std::uint16_t base = 0;   // value at base_cycle
        std::uint64_t base_cycle = 0;
        bool running = false;
        int shift = 3;            // log2 of the prescaler
    };
    void rebase(Machine& m, int timer);
    void configure(Machine& m, int timer);
    void schedule_overflow(Machine& m, int timer);
    void schedule_compare(Machine& m, int ch);
    void schedule_timer_channels(Machine& m, int timer);
    int mode(const Machine& m, int ch) const;   // CCMODx, 0..7
    int timer_of(const Machine& m, int ch) const;  // 0..3
    std::array<Timer, 4> t_{};  // T0, T1, T7, T8
    std::uint32_t outputs_ = 0;
    std::uint32_t matched_this_period_ = 0;  // modes 2 and 3
    std::array<std::uint64_t, 32> matches_{};
};

// The 10-bit A/D converter: single or continuous conversion of one
// channel, or a scan from ADCH down to channel 0. A conversion takes
// 'conversion_cycles'; the result goes to ADDAT with the channel in bits
// 15..12 and sets ADCIR. Channel inputs come from set_input().
class Adc final : public Device {
public:
    std::string name() const override { return "ADC"; }
    std::uint8_t read8(Addr) override { return 0xFF; }
    void write8(Addr, std::uint8_t) override {}
    void attach(Machine& m) override;
    void reset(bool power_on) override;
    void save(StateWriter& w) const override;
    void load(StateReader& r) override;
    void event(Machine& m, std::uint32_t tag) override;

    // The 10-bit value channel 'ch' (0..15) converts to.
    void set_input(int ch, std::uint16_t value) { inputs_.at(static_cast<std::size_t>(ch)) = value & 0x3FF; }
    std::uint16_t input(int ch) const { return inputs_.at(static_cast<std::size_t>(ch)); }
    std::uint64_t conversion_cycles = 250;  // about 10 us at 25 MHz

private:
    std::array<std::uint16_t, 16> inputs_;
    int channel_ = 0;
    bool busy_ = false;
};

// The peripherals above, added to a board with the C167's addresses.
void add_c167_peripherals(Board& b);

}  // namespace oc166::sim
