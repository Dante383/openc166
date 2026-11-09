// SPDX-License-Identifier: MIT
// The debug core: breakpoints, watchpoints, stepping and reverse execution
// on top of a Machine. Frontends (the Python API, the DAP server) use only
// this and the Machine. See docs/debugger-design.md, sections 3.7 and 3.8.
#pragma once

#include "sim/machine.hpp"

#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace oc166::sim {

enum class WatchKind : std::uint8_t { Read = 1, Write = 2, Change = 4 };

struct Breakpoint {
    int id = 0;
    Addr address = 0;  // physical CSP:IP
    // Called when reached; stop only if it returns true. Empty: always stop.
    std::function<bool(Machine&)> condition;
    bool enabled = true;
    std::uint64_t hits = 0;
    bool temporary = false;  // removed when hit (step over)
};

struct Watchpoint {
    int id = 0;
    Addr start = 0;  // physical address range [start, end)
    Addr end = 0;
    unsigned kinds = static_cast<unsigned>(WatchKind::Write);
    // For a bit or a field: only these bits of the word count (Change
    // compares them, Read and Write ignore it). 0xFFFF for the whole word.
    std::uint16_t mask = 0xFFFF;
    std::function<bool(Machine&, const Access&)> condition;
    bool enabled = true;
    std::uint64_t hits = 0;
};

struct StopInfo {
    Stop reason = Stop::None;
    int id = 0;                    // the breakpoint or watchpoint
    std::optional<Access> access;  // what a watchpoint saw
    // Reverse execution reached the oldest recorded state.
    bool history_start = false;
};

class Debugger {
public:
    explicit Debugger(Machine& m);
    ~Debugger();
    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    Machine& machine() { return m_; }

    int add_breakpoint(Addr address, std::function<bool(Machine&)> condition = {});
    bool remove_breakpoint(int id);
    int add_watchpoint(Addr start, Addr size, unsigned kinds, std::uint16_t mask = 0xFFFF,
                       std::function<bool(Machine&, const Access&)> condition = {});
    bool remove_watchpoint(int id);
    const std::map<int, Breakpoint>& breakpoints() const { return breakpoints_; }
    const std::map<int, Watchpoint>& watchpoints() const { return watchpoints_; }

    // Runs until a breakpoint, watchpoint or other stop, or until
    // 'max_instructions' have run (0: no limit; reason None).
    StopInfo cont(std::uint64_t max_instructions = 0);
    // Runs until the clock reaches 'cycle' (or a stop comes first).
    StopInfo run_until_cycle(std::uint64_t cycle);
    // One instruction; an EXT or ATOMIC sequence runs as one unit, and
    // interrupt entries before it are included.
    StopInfo step();
    // Steps over calls (CALLA, CALLI, CALLR, CALLS, PCALL, TRAP).
    StopInfo step_over(std::uint64_t max_instructions = 0);
    // Runs until the current routine returns (RET, RETS, RETP or RETI that
    // leaves the stack above where it is now).
    StopInfo step_out(std::uint64_t max_instructions = 0);

    // ---- Reverse execution
    // Back one instruction (one step()).
    StopInfo reverse_step();
    // Back to the last breakpoint or watchpoint hit before now, or to the
    // start of the recorded history.
    StopInfo reverse_continue();
    // Goes to the state after 'instructions' instructions, forwards or
    // backwards. Returns false if that lies before the recorded history.
    bool goto_instruction(std::uint64_t instructions);
    // A checkpoint is kept every 'interval' instructions; reverse steps
    // re-run at most that many. The oldest are thinned out beyond 'limit'.
    void set_checkpoint_interval(std::uint64_t interval) { interval_ = interval ? interval : 1; }
    void set_checkpoint_limit(std::size_t limit) { limit_ = limit < 4 ? 4 : limit; }
    std::uint64_t history_start() const { return checkpoints_.empty() ? 0 : checkpoints_.begin()->first; }
    // Forgets the history (after the user changes state by hand, which a
    // replay would not repeat).
    void clear_history();

private:
    // Runs single machine steps until an instruction completes; returns the
    // stop. 'stop_at_breakpoints' is off while replaying.
    Stop raw_step(bool stop_at_breakpoints, bool skip_current_breakpoint);
    void checkpoint();
    void install();
    // Replays from the checkpoint at or before 'from' up to 'to' (exclusive),
    // noting positions where a breakpoint or watchpoint would stop.
    std::optional<std::uint64_t> last_hit_between(std::uint64_t from, std::uint64_t to);

    Machine& m_;
    std::map<int, Breakpoint> breakpoints_;
    std::unordered_multimap<Addr, int> by_address_;
    std::map<int, Watchpoint> watchpoints_;
    int next_id_ = 1;

    bool replaying_ = false;
    bool skip_bp_at_pc_ = false;
    StopInfo hit_;  // filled by the hooks

    std::map<std::uint64_t, Snapshot> checkpoints_;  // by instruction count
    std::uint64_t interval_ = 20000;
    std::size_t limit_ = 256;
};

// True for CALLA, CALLI, CALLR, CALLS, PCALL and TRAP.
bool is_call(const dis::Insn& insn);
// True for RET, RETS, RETP and RETI.
bool is_return(const dis::Insn& insn);

}  // namespace oc166::sim
