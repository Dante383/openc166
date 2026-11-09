// SPDX-License-Identifier: MIT
#include "sim/debug.hpp"

#include <algorithm>

namespace oc166::sim {

bool is_call(const dis::Insn& insn) {
    const auto m = insn.mnemonic();
    return m == "CALLA" || m == "CALLI" || m == "CALLR" || m == "CALLS" || m == "PCALL" || m == "TRAP";
}

bool is_return(const dis::Insn& insn) {
    const auto m = insn.mnemonic();
    return m == "RET" || m == "RETS" || m == "RETP" || m == "RETI";
}

Debugger::Debugger(Machine& m) : m_(m) {
    install();
    checkpoint();
}

Debugger::~Debugger() {
    m_.on_access = nullptr;
    m_.on_fetch = nullptr;
}

void Debugger::install() {
    m_.on_fetch = [this](Addr pc) {
        if (replaying_ || by_address_.empty()) return false;
        if (skip_bp_at_pc_) {
            skip_bp_at_pc_ = false;
            return false;
        }
        auto [lo, hi] = by_address_.equal_range(pc);
        for (auto it = lo; it != hi; ++it) {
            Breakpoint& bp = breakpoints_.at(it->second);
            if (!bp.enabled) continue;
            if (bp.condition && !bp.condition(m_)) continue;
            ++bp.hits;
            hit_.reason = Stop::Breakpoint;
            hit_.id = bp.id;
            return true;
        }
        return false;
    };
    m_.on_access = [this](const Access& a) {
        if (watchpoints_.empty()) return;
        for (auto& [id, w] : watchpoints_) {
            if (!w.enabled) continue;
            if (a.address + a.size <= w.start || a.address >= w.end) continue;
            bool match = false;
            if ((w.kinds & static_cast<unsigned>(WatchKind::Read)) && a.kind == AccessKind::Read) match = true;
            if ((w.kinds & static_cast<unsigned>(WatchKind::Write)) && a.kind == AccessKind::Write) match = true;
            if ((w.kinds & static_cast<unsigned>(WatchKind::Change)) && a.kind == AccessKind::Write) {
                // Compare the bytes of the watched word that changed.
                std::uint16_t diff = static_cast<std::uint16_t>(a.value ^ a.old);
                std::uint16_t mask = w.mask;
                if (a.size == 1) mask = static_cast<std::uint16_t>((a.address & 1) ? (mask >> 8) : (mask & 0xFF));
                if (diff & mask) match = true;
            }
            if (!match) continue;
            if (w.condition && !w.condition(m_, a)) continue;
            if (!replaying_) ++w.hits;
            if (hit_.reason == Stop::None) {
                hit_.reason = Stop::Watchpoint;
                hit_.id = id;
                hit_.access = a;
            }
            m_.stop_after_instruction();
        }
    };
}

int Debugger::add_breakpoint(Addr address, std::function<bool(Machine&)> condition) {
    Breakpoint bp;
    bp.id = next_id_++;
    bp.address = address & 0xFFFFFF;
    bp.condition = std::move(condition);
    by_address_.emplace(bp.address, bp.id);
    breakpoints_.emplace(bp.id, std::move(bp));
    return next_id_ - 1;
}

bool Debugger::remove_breakpoint(int id) {
    auto it = breakpoints_.find(id);
    if (it == breakpoints_.end()) return false;
    auto [lo, hi] = by_address_.equal_range(it->second.address);
    for (auto i = lo; i != hi; ++i)
        if (i->second == id) {
            by_address_.erase(i);
            break;
        }
    breakpoints_.erase(it);
    return true;
}

int Debugger::add_watchpoint(Addr start, Addr size, unsigned kinds, std::uint16_t mask,
                             std::function<bool(Machine&, const Access&)> condition) {
    Watchpoint w;
    w.id = next_id_++;
    w.start = start & 0xFFFFFF;
    w.end = w.start + std::max<Addr>(size, 1);
    w.kinds = kinds;
    w.mask = mask;
    w.condition = std::move(condition);
    watchpoints_.emplace(w.id, std::move(w));
    return next_id_ - 1;
}

bool Debugger::remove_watchpoint(int id) { return watchpoints_.erase(id) > 0; }

void Debugger::checkpoint() {
    const std::uint64_t n = m_.instructions();
    if (!checkpoints_.empty() && checkpoints_.rbegin()->first >= n) {
        // After going back in time, later checkpoints are still valid (the
        // run is deterministic) unless the state was changed by hand.
        if (checkpoints_.count(n)) return;
    }
    checkpoints_[n] = m_.save();
    if (checkpoints_.size() > limit_) {
        // Thin out: drop every second checkpoint of the older half, keeping
        // the first (the start of the history).
        std::size_t i = 0;
        const std::size_t half = checkpoints_.size() / 2;
        for (auto it = std::next(checkpoints_.begin()); it != checkpoints_.end() && i < half; ++i) {
            if (i % 2 == 0)
                it = checkpoints_.erase(it);
            else
                ++it;
        }
    }
}

void Debugger::clear_history() {
    checkpoints_.clear();
    checkpoint();
}

Stop Debugger::raw_step(bool stop_at_breakpoints, bool skip_current_breakpoint) {
    const bool was_replaying = replaying_;
    if (!stop_at_breakpoints) replaying_ = true;
    skip_bp_at_pc_ = skip_current_breakpoint;
    const std::uint64_t before = m_.instructions();
    Stop s = Stop::None;
    // Interrupt and trap entries do not count as instructions: keep going
    // until one instruction has completed.
    for (int guard = 0; guard < 64 && m_.instructions() == before; ++guard) {
        s = m_.step();
        if (s != Stop::None) break;
    }
    skip_bp_at_pc_ = false;
    replaying_ = was_replaying;
    if (!replaying_ && m_.instructions() > before) {
        // The newest checkpoint at or before now (after going back in time,
        // newer ones exist).
        auto it = checkpoints_.upper_bound(m_.instructions());
        if (it == checkpoints_.begin() || m_.instructions() - std::prev(it)->first >= interval_) checkpoint();
    }
    return s;
}

StopInfo Debugger::step() {
    hit_ = {};
    Stop s = raw_step(true, true);
    // EXT and ATOMIC sequences step as one unit.
    while (s == Stop::None && m_.ext().left > 0) s = raw_step(true, true);
    StopInfo r = hit_;
    r.reason = s;
    return r;
}

StopInfo Debugger::cont(std::uint64_t max_instructions) {
    hit_ = {};
    const std::uint64_t end = max_instructions ? m_.instructions() + max_instructions : ~std::uint64_t{0};
    bool first = true;
    while (m_.instructions() < end) {
        const Stop s = raw_step(true, first);
        first = false;
        if (s != Stop::None) {
            StopInfo r = hit_;
            r.reason = s;
            return r;
        }
    }
    return {};
}

StopInfo Debugger::step_over(std::uint64_t max_instructions) {
    auto insn = m_.decode_at(m_.pc());
    if (!insn || !is_call(*insn)) return step();
    const Addr ret = (m_.pc() & 0xFF0000) | ((m_.ip() + insn->size) & 0xFFFF);
    const std::uint16_t sp = m_.sfr_get(reg::SP);
    const int id = add_breakpoint(ret, [sp](Machine& mm) { return mm.sfr_get(reg::SP) >= sp; });
    breakpoints_.at(id).temporary = true;
    StopInfo r = cont(max_instructions);
    remove_breakpoint(id);
    if (r.reason == Stop::Breakpoint && r.id == id) r = {};
    return r;
}

StopInfo Debugger::step_out(std::uint64_t max_instructions) {
    hit_ = {};
    const std::uint16_t sp = m_.sfr_get(reg::SP);
    const std::uint64_t end = max_instructions ? m_.instructions() + max_instructions : ~std::uint64_t{0};
    bool first = true;
    while (m_.instructions() < end) {
        auto insn = m_.decode_at(m_.pc());
        const bool ret = insn && is_return(*insn);
        const Stop s = raw_step(true, first);
        first = false;
        if (s != Stop::None) {
            StopInfo r = hit_;
            r.reason = s;
            return r;
        }
        if (ret && m_.sfr_get(reg::SP) > sp) return {};
    }
    return {};
}

bool Debugger::goto_instruction(std::uint64_t target) {
    if (target < m_.instructions()) {
        auto it = checkpoints_.upper_bound(target);
        if (it == checkpoints_.begin()) return false;
        --it;
        m_.load(it->second);
    }
    replaying_ = true;
    while (m_.instructions() < target) {
        const std::uint64_t before = m_.instructions();
        const Stop s = raw_step(false, false);
        if (s != Stop::None && s != Stop::Watchpoint && m_.instructions() == before) break;
    }
    replaying_ = false;
    hit_ = {};
    return m_.instructions() == target;
}

StopInfo Debugger::reverse_step() {
    const std::uint64_t now = m_.instructions();
    StopInfo r;
    if (now == 0 || now <= history_start()) {
        r.history_start = true;
        return r;
    }
    // A step may cover several instructions (an EXT sequence): go back to
    // the start of the step that ended here.
    std::uint64_t target = now - 1;
    goto_instruction(target);
    // If 'target' is inside a sequence, back up to the sequence's start.
    while (m_.ext().left > 0 && target > history_start()) goto_instruction(--target);
    return r;
}

std::optional<std::uint64_t> Debugger::last_hit_between(std::uint64_t from, std::uint64_t to) {
    if (!goto_instruction(from)) return std::nullopt;
    std::optional<std::uint64_t> last;
    replaying_ = true;  // breakpoints do not stop; watchpoints still report
    while (m_.instructions() < to) {
        for (auto [lo, hi] = by_address_.equal_range(m_.pc()); lo != hi; ++lo) {
            const Breakpoint& bp = breakpoints_.at(lo->second);
            if (bp.enabled && !bp.temporary && (!bp.condition || bp.condition(m_))) last = m_.instructions();
        }
        hit_ = {};
        const std::uint64_t before = m_.instructions();
        raw_step(false, false);
        if (m_.instructions() == before) break;
        if (hit_.reason == Stop::Watchpoint && m_.instructions() < to) last = m_.instructions();
    }
    replaying_ = false;
    hit_ = {};
    return last;
}

StopInfo Debugger::reverse_continue() {
    const std::uint64_t now = m_.instructions();
    // Search the history segment by segment, newest first.
    std::vector<std::uint64_t> starts;
    for (const auto& [n, s] : checkpoints_)
        if (n < now) starts.push_back(n);
    std::uint64_t end = now;
    // Watchpoint hit counters must not count replays.
    std::map<int, std::uint64_t> wp_hits, bp_hits;
    for (auto& [id, w] : watchpoints_) wp_hits[id] = w.hits;
    for (auto& [id, b] : breakpoints_) bp_hits[id] = b.hits;
    std::optional<std::uint64_t> found;
    for (auto it = starts.rbegin(); it != starts.rend() && !found; ++it) {
        found = last_hit_between(*it, end);
        end = *it;
    }
    for (auto& [id, w] : watchpoints_) w.hits = wp_hits[id];
    for (auto& [id, b] : breakpoints_) b.hits = bp_hits[id];
    StopInfo r;
    if (!found) {
        goto_instruction(history_start());
        r.history_start = true;
        return r;
    }
    goto_instruction(*found);
    // A breakpoint here, or else the watchpoint of the instruction before:
    // re-run that instruction to report its access.
    for (auto [lo, hi] = by_address_.equal_range(m_.pc()); lo != hi; ++lo) {
        r.reason = Stop::Breakpoint;
        r.id = lo->second;
        return r;
    }
    if (*found > 0 && goto_instruction(*found - 1)) {
        hit_ = {};
        replaying_ = true;
        raw_step(false, false);
        replaying_ = false;
        r = hit_;
        r.reason = Stop::Watchpoint;
        hit_ = {};
    }
    return r;
}

}  // namespace oc166::sim
