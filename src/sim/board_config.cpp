// SPDX-License-Identifier: MIT
#include "sim/board_config.hpp"

#include "bundled.hpp"
#include "json.hpp"
#include "sim/boards.hpp"
#include "sim/peripherals.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace oc166::sim {

namespace {

using json::Value;

[[noreturn]] void fail(const Value& v, const std::string& msg) { throw BoardError(v.where() + ": " + msg); }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

enum class Unit { Plain, Size, Frequency };

// A whole number given as a JSON number or as text: "0x7FFFF", "7FFFFH",
// "512K", "1M" (sizes) or "24MHz" (frequencies).
std::uint64_t number(const Value& v, Unit unit = Unit::Plain) {
    if (v.is_number()) {
        const std::int64_t n = v.as_int();
        if (n < 0) fail(v, "expected a number that is not negative");
        return static_cast<std::uint64_t>(n);
    }
    if (!v.is_string()) fail(v, "expected a number");
    std::string s = lower(v.as_string());
    s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
    std::uint64_t scale = 1;
    auto strip = [&](std::string_view suffix, std::uint64_t factor) {
        if (s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) {
            s.resize(s.size() - suffix.size());
            scale = factor;
            return true;
        }
        return false;
    };
    if (unit == Unit::Frequency) {
        strip("mhz", 1000000) || strip("khz", 1000) || strip("hz", 1);
    } else if (unit == Unit::Size) {
        strip("k", 1024) || strip("m", 1024 * 1024);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && s[1] == 'x') {
        s = s.substr(2);
        base = 16;
    } else if (s.size() > 1 && s.back() == 'h') {
        s.pop_back();
        base = 16;
    }
    std::size_t used = 0;
    unsigned long long n = 0;
    try {
        n = std::stoull(s, &used, base);
    } catch (const std::exception&) {
        used = 0;
    }
    if (s.empty() || used != s.size()) fail(v, "'" + v.as_string() + "' is not a number");
    return n * scale;
}

void allow(const Value& obj, std::initializer_list<std::string_view> keys) {
    for (const auto& [k, v] : obj.as_object())
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            std::string list;
            for (auto key : keys) list += (list.empty() ? "" : ", ") + std::string(key);
            fail(v, "unknown member \"" + k + "\" (expected " + list + ")");
        }
}

Addr mask_for(std::uint64_t size) {
    Addr m = 1;
    while (m < size) m <<= 1;
    return m - 1;
}

// The chip: CPU core, internal RAM, on-chip XRAM and X-peripherals, the
// interrupt table and the on-chip peripherals.
void make_chip(Board& b, const std::string& chip, const Value& where) {
    b.interrupts = c167_interrupts();
    if (chip == "c166") {
        b.cpu = isa::Cpu::C166;
        b.iram_start = 0xFA00;  // 1 KB
    } else if (chip == "c167") {
        b.cpu = isa::Cpu::C167;
        b.iram_start = 0xF600;  // 2 KB
    } else if (chip == "c167cr") {
        b.cpu = isa::Cpu::C167;
        b.iram_start = 0xF600;
        // 2 KB XRAM and the CAN module, enabled by SYSCON.XPEN.
        b.internal.push_back({0xE000, 0x0800, Region::kXpenOnly, std::make_shared<Ram>("XRAM", 0x800, 0x7FF)});
        b.internal.push_back({0xEF00, 0x0100, Region::kXpenOnly, std::make_shared<StubDevice>("CAN1", 0xEF00, 0x100)});
    } else if (chip == "c167cs") {
        b.cpu = isa::Cpu::C167;
        b.iram_start = 0xF200;  // 3 KB
        // XPERCON: CAN1EN (bit 0), CAN2EN (1), XRAM1EN (10), XRAM2EN (11).
        // After reset CAN1 and XRAM1 are on.
        b.xpercon_reset = 0x0401;
        b.internal.push_back({0xE000, 0x0800, 10, std::make_shared<Ram>("XRAM1", 0x800, 0x7FF)});
        b.internal.push_back({0xC000, 0x1800, 11, std::make_shared<Ram>("XRAM2", 0x1800, 0x1FFF)});
        b.internal.push_back({0xEF00, 0x0100, 0, std::make_shared<StubDevice>("CAN1", 0xEF00, 0x100)});
        b.internal.push_back({0xEE00, 0x0100, 1, std::make_shared<StubDevice>("CAN2", 0xEE00, 0x100)});
    } else {
        std::string list;
        for (const auto& c : builtin_chips()) list += (list.empty() ? "" : ", ") + c;
        fail(where, "unknown chip '" + chip + "' (expected " + list + ")");
    }
    add_c167_peripherals(b);
}

std::vector<std::uint8_t> read_path(const BoardSource& src, const std::string& name, const Value& where) {
    std::string path = name;
    const bool absolute = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
    if (!absolute && !src.base_dir.empty()) path = src.base_dir + "/" + path;
    if (src.read_file) {
        if (auto data = src.read_file(path)) return *data;
    } else {
        std::ifstream f(path, std::ios::binary);
        if (f) return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
    }
    fail(where, "cannot read '" + path + "'");
}

// The contents of a rom, ram or flash: "image" ("$IMAGE" or a file),
// "offset" into it, "size" (default: the image's), "fill" for the rest.
std::vector<std::uint8_t> contents(const Value& dev, const BoardSource& src, std::optional<std::uint64_t> fixed_size,
                                   bool image_required) {
    std::vector<std::uint8_t> data;
    if (const Value* img = dev.find("image")) {
        const std::string& name = img->as_string();
        data = name == "$IMAGE" ? src.image : read_path(src, name, *img);
        if (const Value* off = dev.find("offset")) {
            const std::uint64_t o = number(*off, Unit::Size);
            if (o > data.size()) fail(*off, "the offset is past the end of the image");
            data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(o));
        }
    } else if (image_required && !dev.find("size") && !fixed_size) {
        fail(dev, "give \"image\" or \"size\"");
    }
    std::uint64_t size = data.size();
    if (fixed_size) size = *fixed_size;
    if (const Value* sz = dev.find("size")) size = number(*sz, Unit::Size);
    if (size == 0) fail(dev, "the size is 0 (an empty image?)");
    if (size > 0x1000000) fail(dev, "larger than 16 MB");
    const std::uint8_t fill = dev.find("fill") ? static_cast<std::uint8_t>(number(*dev.find("fill"))) : 0xFF;
    data.resize(size, fill);
    return data;
}

std::shared_ptr<Device> make_device(const std::string& cs, const Value& dev, const BoardSource& src) {
    const Value* type = dev.find("device");
    if (!type) fail(dev, "missing \"device\" (rom, ram, flash, latch or stub)");
    const std::string kind = lower(type->as_string());
    std::string name = dev.find("name") ? dev.find("name")->as_string() : kind;
    auto mask_of = [&](std::uint64_t size) {
        return dev.find("mask") ? static_cast<Addr>(number(*dev.find("mask"), Unit::Size)) : mask_for(size);
    };
    if (kind == "rom") {
        allow(dev, {"device", "name", "image", "offset", "size", "fill", "mask"});
        auto data = contents(dev, src, std::nullopt, true);
        const Addr mask = mask_of(data.size());
        return std::make_shared<Rom>(name, std::move(data), mask);
    }
    if (kind == "ram") {
        allow(dev, {"device", "name", "image", "offset", "size", "fill", "mask"});
        if (!dev.find("size") && !dev.find("image")) fail(dev, "a ram needs \"size\"");
        auto data = contents(dev, src, std::nullopt, false);
        auto ram = std::make_shared<Ram>(name, data.size(), mask_of(data.size()));
        ram->memory() = std::move(data);
        return ram;
    }
    if (kind == "flash") {
        allow(dev, {"device", "name", "model", "image", "offset", "fill", "mask", "bus_width", "program_us",
                    "sector_erase_ms", "chip_erase_ms"});
        const Value* model = dev.find("model");
        if (!model) fail(dev, "a flash needs \"model\" (am29f400bb or am29f400bt)");
        auto geometry = Flash::model(lower(model->as_string()));
        if (!geometry) fail(*model, "unknown flash model '" + model->as_string() + "' (expected am29f400bb or am29f400bt)");
        std::uint64_t size = 0;
        for (const auto& [off, len] : geometry->sectors) size = std::max<std::uint64_t>(size, off + len);
        auto data = contents(dev, src, size, false);
        int width = 16;
        if (const Value* bw = dev.find("bus_width")) {
            width = static_cast<int>(number(*bw));
            if (width != 8 && width != 16) fail(*bw, "the bus width is 8 or 16");
        }
        auto flash = std::make_shared<Flash>(name, std::move(data), mask_of(size), *geometry, width);
        if (const Value* t = dev.find("program_us")) flash->program_us = t->as_number();
        if (const Value* t = dev.find("sector_erase_ms")) flash->sector_erase_ms = t->as_number();
        if (const Value* t = dev.find("chip_erase_ms")) flash->chip_erase_ms = t->as_number();
        return flash;
    }
    if (kind == "latch") {
        allow(dev, {"device", "name"});
        return std::make_shared<Latch>(name);
    }
    if (kind == "stub") {
        allow(dev, {"device", "name", "size"});
        const std::uint64_t size = dev.find("size") ? number(*dev.find("size"), Unit::Size) : 0x1000;
        (void)cs;
        return std::make_shared<StubDevice>(name, 0, static_cast<Addr>(size));
    }
    fail(*type, "unknown device '" + type->as_string() + "' (expected rom, ram, flash, latch or stub)");
}

}  // namespace

std::vector<std::string> builtin_chips() { return {"c166", "c167", "c167cr", "c167cs"}; }

std::vector<std::string> builtin_boards() { return {"c166", "c167", "c167cr", "c167cs", "5wy19"}; }

BoardConfig load_board(std::string_view text, const BoardSource& src, const std::string& origin) {
    try {
        const Value root = json::parse(text);
        if (!root.is_object()) fail(root, "a board file is a JSON object");
        allow(root, {"format", "version", "name", "description", "chip", "clock_hz", "reset", "chip_selects",
                     "inputs", "ssc", "options"});
        if (const Value* fmt = root.find("format"); fmt && fmt->as_string() != "openc166-board")
            fail(*fmt, "not an openc166 board file");
        if (const Value* ver = root.find("version"); ver && ver->as_int() != 1)
            fail(*ver, "this openc166 reads version 1 of the board format");

        BoardConfig cfg;
        Board& b = cfg.board;
        const Value* chip = root.find("chip");
        if (!chip) fail(root, "missing \"chip\"");
        make_chip(b, lower(chip->as_string()), *chip);
        b.name = root.find("name") ? root.find("name")->as_string() : chip->as_string();
        if (const Value* d = root.find("description")) cfg.description = d->as_string();
        if (const Value* clk = root.find("clock_hz")) {
            const std::uint64_t hz = number(*clk, Unit::Frequency);
            if (hz < 1000 || hz > 100000000) fail(*clk, "the clock is out of range");
            b.clock_hz = static_cast<std::uint32_t>(hz);
        }
        if (const Value* reset = root.find("reset")) {
            allow(*reset, {"syscon", "buscon0", "rp0h", "xpercon"});
            auto word = [&](const char* key, std::uint16_t& out) {
                if (const Value* v = reset->find(key)) {
                    const std::uint64_t n = number(*v);
                    if (n > 0xFFFF) fail(*v, "a register value is 16 bits");
                    out = static_cast<std::uint16_t>(n);
                }
            };
            word("syscon", b.syscon_reset);
            word("buscon0", b.buscon0_reset);
            word("rp0h", b.rp0h_reset);
            word("xpercon", b.xpercon_reset);
        }
        if (const Value* css = root.find("chip_selects")) {
            allow(*css, {"cs0", "cs1", "cs2", "cs3", "cs4"});
            for (const auto& [key, dev] : css->as_object()) {
                const int cs = key[2] - '0';
                if (dev.is_null()) continue;
                b.chip_selects[static_cast<std::size_t>(cs)] = make_device(key, dev, src);
            }
        }
        if (const Value* in = root.find("inputs")) {
            allow(*in, {"adc"});
            if (const Value* adc = in->find("adc")) {
                for (const auto& [ch, v] : adc->as_object()) {
                    char* end = nullptr;
                    const long n = std::strtol(ch.c_str(), &end, 10);
                    if (ch.empty() || *end || n < 0 || n > 15) fail(v, "A/D channel '" + ch + "' is not 0..15");
                    const std::uint64_t value = number(v);
                    if (value > 0x3FF) fail(v, "A/D values are 10 bits (0..3FFH)");
                    cfg.adc[static_cast<int>(n)] = static_cast<std::uint16_t>(value);
                }
            }
        }
        if (const Value* ssc = root.find("ssc")) {
            allow(*ssc, {"default_reply"});
            if (const Value* r = ssc->find("default_reply")) cfg.ssc_reply = static_cast<std::uint16_t>(number(*r));
        }
        if (const Value* opt = root.find("options")) {
            allow(*opt, {"watchdog_frozen", "serial_echo"});
            if (const Value* v = opt->find("watchdog_frozen")) cfg.watchdog_frozen = v->as_bool();
            if (const Value* v = opt->find("serial_echo")) cfg.serial_echo = v->as_bool();
        }
        return cfg;
    } catch (const json::Error& e) {
        throw BoardError(origin + ": " + e.what());
    } catch (const BoardError& e) {
        throw BoardError(origin + ": " + e.what());
    }
}

BoardConfig load_board(const std::string& name_or_path, std::vector<std::uint8_t> image) {
    BoardSource src;
    src.image = std::move(image);
    std::string text;
    std::string origin = name_or_path;
    const std::string trimmed = name_or_path.substr(std::min(name_or_path.find_first_not_of(" \t\r\n"), name_or_path.size()));
    if (!trimmed.empty() && trimmed[0] == '{') {
        text = name_or_path;
        origin = "board";
    } else if (auto bundled = bundled_file(lower(name_or_path) + ".json"); bundled && name_or_path.find('/') == std::string::npos) {
        text = std::string(bundled->text);
    } else {
        std::ifstream f(name_or_path, std::ios::binary);
        if (!f) {
            std::string list;
            for (const auto& b : builtin_boards()) list += (list.empty() ? "" : ", ") + b;
            throw BoardError("unknown board '" + name_or_path + "': not a built-in board (" + list +
                             ") nor a readable .json file");
        }
        text.assign(std::istreambuf_iterator<char>(f), {});
        const std::size_t slash = name_or_path.find_last_of("/\\");
        src.base_dir = slash == std::string::npos ? "." : name_or_path.substr(0, slash);
    }
    return load_board(text, src, origin);
}

void apply_board(Machine& m, const BoardConfig& cfg) {
    if (auto* adc = m.find_device<Adc>())
        for (const auto& [ch, v] : cfg.adc) adc->set_input(ch, v);
    if (cfg.ssc_reply)
        if (auto* ssc = m.find_device<Ssc>()) {
            const std::uint16_t r = *cfg.ssc_reply;
            ssc->reply = [r](std::uint16_t, std::uint16_t) { return r; };
        }
    if (cfg.watchdog_frozen) m.watchdog_frozen = *cfg.watchdog_frozen;
    if (cfg.serial_echo)
        if (auto* asc = m.find_device<Asc0>()) asc->echo = *cfg.serial_echo;
}

}  // namespace oc166::sim
