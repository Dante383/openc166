// SPDX-License-Identifier: MIT
// Output files: Intel HEX and Motorola S-records.
#include "assembler.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace oc166 {

namespace {

// The chunks as sorted runs of consecutive bytes. Chunks that touch or
// overlap are joined; where they overlap the later chunk wins, as in
// flat_image().
std::vector<Chunk> memory_runs(const AssemblyResult& result) {
    std::vector<Chunk> runs;
    std::vector<const Chunk*> sorted;
    for (const auto& c : result.chunks)
        if (!c.bytes.empty()) sorted.push_back(&c);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Chunk* a, const Chunk* b) { return a->address < b->address; });
    std::vector<std::int64_t> ends;
    for (const Chunk* c : sorted) {
        std::int64_t end = c->address + static_cast<std::int64_t>(c->bytes.size());
        if (!runs.empty() && c->address <= ends.back()) {
            ends.back() = std::max(ends.back(), end);
        } else {
            runs.push_back({c->address, {}});
            ends.push_back(end);
        }
    }
    for (std::size_t i = 0; i < runs.size(); ++i)
        runs[i].bytes.resize(static_cast<std::size_t>(ends[i] - runs[i].address));
    for (const auto& c : result.chunks) {
        if (c.bytes.empty()) continue;
        auto it = std::upper_bound(runs.begin(), runs.end(), c.address,
                                   [](std::int64_t a, const Chunk& r) { return a < r.address; });
        Chunk& run = *(it - 1);
        std::copy(c.bytes.begin(), c.bytes.end(), run.bytes.begin() + (c.address - run.address));
    }
    return runs;
}

// Builds one record: the bytes are written as hex digits after 'start', and
// their checksum is appended.
class Record {
public:
    explicit Record(std::string& out) : out_(out) {}
    void byte(std::uint8_t b) {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        out_ += kDigits[b >> 4];
        out_ += kDigits[b & 0xF];
        sum_ = static_cast<std::uint8_t>(sum_ + b);
    }
    void bytes(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    std::uint8_t sum() const { return sum_; }

private:
    std::string& out_;
    std::uint8_t sum_ = 0;
};

std::string hex_address(std::int64_t v) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string s;
    for (int shift = 20; shift >= 0; shift -= 4) s += kDigits[(v >> shift) & 0xF];
    return s + "H";
}

}  // namespace

std::string format_intel_hex(const AssemblyResult& result, int record_size) {
    if (record_size < 1 || record_size > 255) throw std::invalid_argument("record size must be 1..255");
    std::string out;
    auto record = [&](int type, std::int64_t address, const std::uint8_t* data, std::size_t n) {
        out += ':';
        Record r(out);
        r.byte(static_cast<std::uint8_t>(n));
        r.bytes(static_cast<std::uint64_t>(address & 0xFFFF), 2);
        r.byte(static_cast<std::uint8_t>(type));
        for (std::size_t i = 0; i < n; ++i) r.byte(data[i]);
        r.byte(static_cast<std::uint8_t>(-r.sum()));
        out += "\r\n";
    };
    std::int64_t upper = -1;  // address bits 16..31 set by the last type 04 record
    for (const auto& run : memory_runs(result)) {
        std::int64_t end = run.address + static_cast<std::int64_t>(run.bytes.size());
        for (std::int64_t a = run.address; a < end;) {
            // A record must not cross a 64 KB boundary.
            std::int64_t n = std::min({static_cast<std::int64_t>(record_size), end - a, 0x10000 - (a & 0xFFFF)});
            if ((a >> 16) != upper) {
                upper = a >> 16;
                const std::uint8_t ela[2] = {static_cast<std::uint8_t>(upper >> 8), static_cast<std::uint8_t>(upper)};
                record(4, 0, ela, 2);
            }
            record(0, a, run.bytes.data() + (a - run.address), static_cast<std::size_t>(n));
            a += n;
        }
    }
    record(1, 0, nullptr, 0);
    return out;
}

std::string format_srec(const AssemblyResult& result, int record_size, int address_size) {
    std::vector<Chunk> runs = memory_runs(result);
    std::int64_t last = runs.empty() ? 0 : runs.back().address + static_cast<std::int64_t>(runs.back().bytes.size()) - 1;
    int needed = last <= 0xFFFF ? 2 : last <= 0xFFFFFF ? 3 : 4;
    if (address_size == 0) address_size = needed;
    if (address_size < 2 || address_size > 4) throw std::invalid_argument("S-record address size must be 2, 3 or 4");
    if (needed > address_size)
        throw std::invalid_argument("address " + hex_address(last) + " does not fit in " +
                                    (address_size == 2 ? "S1" : "S2") + " records");
    int max_size = 255 - address_size - 1;
    if (record_size < 1 || record_size > max_size)
        throw std::invalid_argument("record size must be 1.." + std::to_string(max_size));

    std::string out;
    auto record = [&](int type, int addr_bytes, std::uint64_t address, const std::uint8_t* data, std::size_t n) {
        out += 'S';
        out += static_cast<char>('0' + type);
        Record r(out);
        r.byte(static_cast<std::uint8_t>(addr_bytes + n + 1));
        r.bytes(address, addr_bytes);
        for (std::size_t i = 0; i < n; ++i) r.byte(data[i]);
        r.byte(static_cast<std::uint8_t>(~r.sum()));
        out += "\r\n";
    };
    record(0, 2, 0, nullptr, 0);
    std::uint64_t count = 0;
    for (const auto& run : runs) {
        for (std::size_t off = 0; off < run.bytes.size(); off += static_cast<std::size_t>(record_size)) {
            std::size_t n = std::min(static_cast<std::size_t>(record_size), run.bytes.size() - off);
            record(address_size - 1, address_size, static_cast<std::uint64_t>(run.address) + off,
                   run.bytes.data() + off, n);
            ++count;
        }
    }
    if (count <= 0xFFFF)
        record(5, 2, count, nullptr, 0);
    else if (count <= 0xFFFFFF)
        record(6, 3, count, nullptr, 0);
    record(11 - address_size, address_size, 0, nullptr, 0);
    return out;
}

}  // namespace oc166
