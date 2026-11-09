// SPDX-License-Identifier: MIT
// The C interface to the disassembler and the simulator. Every function
// catches all exceptions: none may cross into C.
#include "openc166/openc166.h"

#include "disasm.hpp"
#include "sim/boards.hpp"
#include "sim/debug.hpp"
#include "sim/peripherals.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <string>

struct oc166_sim {
    std::unique_ptr<oc166::sim::Machine> machine;
    std::unique_ptr<oc166::sim::Debugger> debugger;
    std::string text;  // for oc166_sim_describe()
    std::deque<std::pair<std::uint8_t, std::uint32_t>> serial_out;  // ASC0 output not yet handed over, with its baud rate
};

struct oc166_snapshot {
    oc166::sim::Snapshot snap;
};

namespace {

using namespace oc166::sim;

void copy_text(const std::string& t, char* out, std::size_t size) {
    if (!out || size == 0) return;
    const std::size_t n = std::min(t.size(), size - 1);
    std::memcpy(out, t.data(), n);
    out[n] = '\0';
}

int fill_stop(const StopInfo& info, oc166_stop* out) {
    if (!out) return 0;
    *out = oc166_stop{};
    out->reason = stop_name(info.reason);
    out->id = info.id;
    out->history_start = info.history_start ? 1 : 0;
    if (info.access) {
        out->access = static_cast<int>(info.access->kind);
        out->access_address = info.access->address;
        out->access_size = info.access->size;
        out->access_value = info.access->value;
        out->access_old = info.access->old;
    }
    return 0;
}

template <class F>
int guarded(F f) {
    try {
        return f();
    } catch (...) {
        return -1;
    }
}

int format_insn(const oc166::dis::Insn& insn, std::uint32_t address, const oc166::dis::ExtState& ext, int flags,
                char* text, std::size_t text_size, int64_t* target) {
    static const oc166::dis::Names kNoNames;
    oc166::dis::FormatOptions fo;
    fo.reassemble = flags & OC166_DIS_REASSEMBLE;
    const auto& names = (flags & (OC166_DIS_NO_NAMES | OC166_DIS_REASSEMBLE)) ? kNoNames : oc166::dis::c167_names();
    copy_text(oc166::dis::format(insn, address, ext, names, fo), text, text_size);
    if (target) {
        auto t = oc166::dis::branch_target(insn, address);
        *target = t ? static_cast<int64_t>(*t) : -1;
    }
    return insn.size;
}

}  // namespace

extern "C" {

int oc166_disassemble(const uint8_t* bytes, size_t size, uint32_t address, int flags, char* text, size_t text_size,
                      int64_t* target) {
    return guarded([&] {
        copy_text("", text, text_size);
        if (target) *target = -1;
        if (!bytes) return 0;
        auto insn = oc166::dis::decode(bytes, size, (flags & OC166_DIS_C166) ? oc166::isa::Cpu::C166 : oc166::isa::Cpu::C167);
        if (!insn) return 0;
        oc166::dis::ExtState ext;
        ext.esfr = flags & OC166_DIS_ESFR;
        ext.left = ext.esfr ? 1 : 0;
        return format_insn(*insn, address, ext, flags, text, text_size, target);
    });
}

char* oc166_disassemble_module(const uint8_t* bytes, size_t size, uint32_t base, int flags, size_t* fallbacks) {
    try {
        if (!bytes && size) return nullptr;
        auto m = oc166::dis::disassemble_module(bytes, size, base,
                                                (flags & OC166_DIS_C166) ? oc166::isa::Cpu::C166 : oc166::isa::Cpu::C167);
        if (fallbacks) *fallbacks = m.fallbacks;
        char* out = static_cast<char*>(std::malloc(m.text.size() + 1));
        if (!out) return nullptr;
        std::memcpy(out, m.text.c_str(), m.text.size() + 1);
        return out;
    } catch (...) {
        return nullptr;
    }
}

void oc166_free(void* p) { std::free(p); }

int64_t oc166_sfr_address(const char* name) {
    try {
        if (!name) return -1;
        auto a = oc166::dis::c167_sfr_address(name);
        return a ? static_cast<int64_t>(*a) : -1;
    } catch (...) {
        return -1;
    }
}

int64_t oc166_bit_address(const char* name, int* bit) {
    try {
        if (!name) return -1;
        auto b = oc166::dis::c167_bit(name);
        if (!b) return -1;
        if (bit) *bit = b->second;
        return b->first;
    } catch (...) {
        return -1;
    }
}

const char* oc166_sfr_name(uint32_t address) {
    try {
        // Valid until the next call on this thread.
        static thread_local std::string name;
        name = oc166::dis::c167_names().sfr(address, false);
        return name.c_str();
    } catch (...) {
        return "";
    }
}

oc166_sim* oc166_sim_new(const char* board, const uint8_t* image, size_t size) {
    try {
        if (!board || (!image && size)) return nullptr;
        auto s = std::make_unique<oc166_sim>();
        std::vector<std::uint8_t> img(image, image + size);
        s->machine = std::make_unique<Machine>(board_by_name(board, std::move(img)));
        s->debugger = std::make_unique<Debugger>(*s->machine);
        return s.release();
    } catch (...) {
        return nullptr;
    }
}

void oc166_sim_free(oc166_sim* s) { delete s; }

void oc166_sim_reset(oc166_sim* s, int kind) {
    if (!s) return;
    try {
        s->machine->reset(static_cast<Machine::ResetKind>(kind < 0 || kind > 3 ? 0 : kind));
        s->debugger->clear_history();
    } catch (...) {
    }
}

int oc166_sim_step(oc166_sim* s, oc166_stop* out) {
    return guarded([&] { return s ? fill_stop(s->debugger->step(), out) : -1; });
}
int oc166_sim_continue(oc166_sim* s, uint64_t max, oc166_stop* out) {
    return guarded([&] { return s ? fill_stop(s->debugger->cont(max), out) : -1; });
}
int oc166_sim_step_over(oc166_sim* s, uint64_t max, oc166_stop* out) {
    return guarded([&] { return s ? fill_stop(s->debugger->step_over(max), out) : -1; });
}
int oc166_sim_step_out(oc166_sim* s, uint64_t max, oc166_stop* out) {
    return guarded([&] { return s ? fill_stop(s->debugger->step_out(max), out) : -1; });
}
int oc166_sim_reverse_step(oc166_sim* s, oc166_stop* out) {
    return guarded([&] { return s ? fill_stop(s->debugger->reverse_step(), out) : -1; });
}
int oc166_sim_reverse_continue(oc166_sim* s, oc166_stop* out) {
    return guarded([&] { return s ? fill_stop(s->debugger->reverse_continue(), out) : -1; });
}
int oc166_sim_goto(oc166_sim* s, uint64_t n) {
    return guarded([&] { return s && s->debugger->goto_instruction(n) ? 0 : -1; });
}
void oc166_sim_request_stop(oc166_sim* s) {
    if (s) s->machine->request_stop();
}
void oc166_sim_set_history(oc166_sim* s, uint64_t interval, size_t limit) {
    if (!s) return;
    s->debugger->set_checkpoint_interval(interval);
    s->debugger->set_checkpoint_limit(limit);
}
void oc166_sim_clear_history(oc166_sim* s) {
    if (s) guarded([&] { s->debugger->clear_history(); return 0; });
}
uint64_t oc166_sim_history_start(const oc166_sim* s) { return s ? s->debugger->history_start() : 0; }

uint32_t oc166_sim_pc(const oc166_sim* s) { return s ? s->machine->pc() : 0; }
void oc166_sim_set_pc(oc166_sim* s, uint32_t pc) {
    if (!s) return;
    s->machine->set_csp(static_cast<std::uint16_t>((pc >> 16) & 0xFF));
    s->machine->set_ip(static_cast<std::uint16_t>(pc));
}
uint64_t oc166_sim_instructions(const oc166_sim* s) { return s ? s->machine->instructions() : 0; }
uint64_t oc166_sim_cycles(const oc166_sim* s) { return s ? s->machine->cycles() : 0; }
uint32_t oc166_sim_clock_hz(const oc166_sim* s) { return s ? s->machine->board().clock_hz : 0; }
uint16_t oc166_sim_gpr(const oc166_sim* s, int n) { return s && n >= 0 && n < 16 ? s->machine->gpr(n) : 0; }
void oc166_sim_set_gpr(oc166_sim* s, int n, uint16_t v) {
    if (s && n >= 0 && n < 16) s->machine->set_gpr(n, v);
}
uint16_t oc166_sim_sfr(const oc166_sim* s, uint32_t a) { return s ? s->machine->sfr_get(a) : 0; }
void oc166_sim_set_sfr(oc166_sim* s, uint32_t a, uint16_t v) {
    if (s) s->machine->sfr_set(a, v);
}
void oc166_sim_ext(const oc166_sim* s, int* left, int* esfr, int* kind, uint32_t* value) {
    if (!s) return;
    const auto& e = s->machine->ext();
    if (left) *left = e.left;
    if (esfr) *esfr = e.esfr ? 1 : 0;
    if (kind) *kind = static_cast<int>(e.kind);
    if (value) *value = e.value;
}
int oc166_sim_einit_done(const oc166_sim* s) { return s && s->machine->einit_done() ? 1 : 0; }
int oc166_sim_idle(const oc166_sim* s) { return s && s->machine->idle() ? 1 : 0; }

void oc166_sim_read(oc166_sim* s, uint32_t address, uint8_t* out, size_t size) {
    if (!s || !out) return;
    guarded([&] {
        for (size_t i = 0; i < size; ++i) out[i] = s->machine->peek8(address + static_cast<uint32_t>(i));
        return 0;
    });
}
void oc166_sim_write(oc166_sim* s, uint32_t address, const uint8_t* data, size_t size) {
    if (!s || !data) return;
    guarded([&] {
        for (size_t i = 0; i < size; ++i) s->machine->poke8(address + static_cast<uint32_t>(i), data[i]);
        return 0;
    });
}
uint32_t oc166_sim_data_address(const oc166_sim* s, uint16_t a) { return s ? s->machine->data_address(a) : 0; }
const char* oc166_sim_describe(oc166_sim* s, uint32_t address) {
    if (!s) return "";
    try {
        s->text = s->machine->describe(address);
        return s->text.c_str();
    } catch (...) {
        return "";
    }
}
int oc166_sim_disassemble(oc166_sim* s, uint32_t address, int flags, char* text, size_t text_size, int64_t* target) {
    return guarded([&] {
        copy_text("", text, text_size);
        if (target) *target = -1;
        if (!s) return 0;
        auto insn = s->machine->decode_at(address);
        if (!insn) return 0;
        return format_insn(*insn, address, s->machine->ext(), flags & OC166_DIS_NO_NAMES, text, text_size, target);
    });
}

int oc166_sim_add_breakpoint(oc166_sim* s, uint32_t address, oc166_condition cond, void* user) {
    return guarded([&] {
        if (!s) return -1;
        std::function<bool(Machine&)> c;
        if (cond) c = [cond, user](Machine&) { return cond(user) != 0; };
        return s->debugger->add_breakpoint(address, std::move(c));
    });
}
int oc166_sim_remove_breakpoint(oc166_sim* s, int id) { return s && s->debugger->remove_breakpoint(id) ? 0 : -1; }
int oc166_sim_add_watchpoint(oc166_sim* s, uint32_t address, uint32_t size, int kinds, uint16_t mask,
                             oc166_condition cond, void* user) {
    return guarded([&] {
        if (!s || kinds <= 0 || kinds > 7) return -1;
        std::function<bool(Machine&, const Access&)> c;
        if (cond) c = [cond, user](Machine&, const Access&) { return cond(user) != 0; };
        return s->debugger->add_watchpoint(address, size, static_cast<unsigned>(kinds), mask, std::move(c));
    });
}
int oc166_sim_remove_watchpoint(oc166_sim* s, int id) { return s && s->debugger->remove_watchpoint(id) ? 0 : -1; }

int oc166_sim_run_until_cycle(oc166_sim* s, uint64_t cycle, oc166_stop* out) {
    return guarded([&] { return s ? fill_stop(s->debugger->run_until_cycle(cycle), out) : -1; });
}

int oc166_sim_serial_send(oc166_sim* s, const uint8_t* data, size_t size, int flags) {
    return guarded([&] {
        if (!s || (!data && size)) return -1;
        auto* asc = s->machine->find_device<Asc0>();
        if (!asc) return -1;
        for (size_t i = 0; i < size; ++i) asc->receive(*s->machine, data[i], (flags & 1) != 0);
        return 0;
    });
}

int oc166_sim_serial_line_low(oc166_sim* s, uint64_t cycles) {
    return guarded([&] {
        if (!s) return -1;
        auto* asc = s->machine->find_device<Asc0>();
        if (!asc) return -1;
        asc->receive_low(*s->machine, cycles);
        return 0;
    });
}

size_t oc166_sim_serial_receive_baud(oc166_sim* s, uint8_t* out, uint32_t* baud, size_t size) {
    try {
        if (!s || !out) return 0;
        if (auto* asc = s->machine->find_device<Asc0>())
            for (auto b : asc->take_output_with_baud()) s->serial_out.push_back(b);
        size_t n = 0;
        for (; n < size && !s->serial_out.empty(); ++n) {
            out[n] = s->serial_out.front().first;
            if (baud) baud[n] = s->serial_out.front().second;
            s->serial_out.pop_front();
        }
        return n;
    } catch (...) {
        return 0;
    }
}

size_t oc166_sim_serial_receive(oc166_sim* s, uint8_t* out, size_t size) {
    return oc166_sim_serial_receive_baud(s, out, nullptr, size);
}

uint64_t oc166_sim_serial_frame_cycles(const oc166_sim* s) {
    if (!s) return 0;
    auto* asc = s->machine->find_device<Asc0>();
    return asc ? asc->frame_cycles(*s->machine) : 0;
}

int oc166_sim_set_adc(oc166_sim* s, int channel, uint16_t value) {
    return guarded([&] {
        if (!s || channel < 0 || channel > 15) return -1;
        auto* adc = s->machine->find_device<Adc>();
        if (!adc) return -1;
        adc->set_input(channel, value);
        return 0;
    });
}

int oc166_sim_capcom_edge(oc166_sim* s, int ch, int rising) {
    return guarded([&] {
        if (!s || ch < 0 || ch > 31) return -1;
        auto* cc = s->machine->find_device<Capcom>();
        if (!cc) return -1;
        cc->input_edge(*s->machine, ch, rising != 0);
        return 0;
    });
}

int oc166_sim_capcom_output(const oc166_sim* s, int ch) {
    if (!s || ch < 0 || ch > 31) return -1;
    auto* cc = s->machine->find_device<Capcom>();
    return cc ? (cc->output(ch) ? 1 : 0) : -1;
}

int oc166_sim_set_ssc_handler(oc166_sim* s, oc166_ssc_fn fn, void* user) {
    return guarded([&] {
        if (!s) return -1;
        auto* ssc = s->machine->find_device<Ssc>();
        if (!ssc) return -1;
        if (fn)
            ssc->reply = [fn, user](std::uint16_t sent, std::uint16_t p6) { return fn(user, sent, p6); };
        else
            ssc->reply = nullptr;
        return 0;
    });
}

oc166_snapshot* oc166_sim_save(const oc166_sim* s) {
    try {
        if (!s) return nullptr;
        auto snap = std::make_unique<oc166_snapshot>();
        snap->snap = s->machine->save();
        return snap.release();
    } catch (...) {
        return nullptr;
    }
}
int oc166_sim_load(oc166_sim* s, const oc166_snapshot* snap) {
    return guarded([&] {
        if (!s || !snap) return -1;
        s->machine->load(snap->snap, false);
        s->debugger->clear_history();
        return 0;
    });
}
void oc166_snapshot_free(oc166_snapshot* snap) { delete snap; }
uint64_t oc166_snapshot_instructions(const oc166_snapshot* snap) { return snap ? snap->snap.instructions : 0; }

void oc166_sim_request_interrupt(oc166_sim* s, uint32_t ic) {
    if (s) s->machine->request_interrupt(ic);
}
void oc166_sim_nmi(oc166_sim* s) {
    if (s) s->machine->nmi();
}

int oc166_sim_set_option(oc166_sim* s, const char* name, int value) {
    if (!s || !name) return -1;
    const std::string n = name;
    if (n == "stop_on_unmodelled")
        s->machine->stop_on_unmodelled = value != 0;
    else if (n == "stop_on_undefined")
        s->machine->stop_on_undefined = value != 0;
    else if (n == "watchdog_frozen")
        s->machine->watchdog_frozen = value != 0;
    else if (n == "serial_echo") {
        auto* asc = s->machine->find_device<Asc0>();
        if (!asc) return -1;
        asc->echo = value != 0;
    }
    else
        return -1;
    return 0;
}

size_t oc166_sim_unmodelled_count(const oc166_sim* s) { return s ? s->machine->unmodelled().size() : 0; }
int oc166_sim_unmodelled(const oc166_sim* s, size_t index, oc166_unmodelled* out) {
    if (!s || !out || index >= s->machine->unmodelled().size()) return -1;
    const auto& u = s->machine->unmodelled()[index];
    out->instruction = u.instruction;
    out->pc = u.pc;
    out->address = u.address;
    out->write = u.write ? 1 : 0;
    out->value = u.value;
    out->what = u.what.c_str();
    return 0;
}
void oc166_sim_clear_unmodelled(oc166_sim* s) {
    if (s) s->machine->clear_unmodelled();
}

int oc166_sim_set_chip_select(oc166_sim* s, int cs, const char* name, oc166_read_fn read, oc166_write_fn write,
                              void* user) {
    return guarded([&] {
        if (!s || cs < 0 || cs > 4) return -1;
        auto d = std::make_shared<CallbackDevice>();
        d->device_name = name ? name : "device";
        if (read) d->on_read = [read, user](Addr a) { return read(user, a); };
        if (write) d->on_write = [write, user](Addr a, std::uint8_t v) { write(user, a, v); };
        s->machine->set_chip_select(cs, std::move(d));
        return 0;
    });
}

}  // extern "C"
