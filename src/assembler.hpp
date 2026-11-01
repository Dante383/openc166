// SPDX-License-Identifier: MIT
// The assembler driver: runs passes over a source text until every label
// address is stable, then emits machine code, a listing and diagnostics.
#pragma once

#include "diag.hpp"
#include "isa/isa_types.hpp"
#include "value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oc166 {

struct AssembleOptions {
    std::string file_name = "<input>";
    // CPU selected before the source is read; $MOD167 in the source also
    // selects the C167.
    isa::Cpu cpu = isa::Cpu::C166;
    // Address of code outside any section, and of the first section that has
    // no AT address.
    std::int64_t origin = 0;
};

// Bytes emitted at consecutive addresses.
struct Chunk {
    std::int64_t address = 0;
    std::vector<std::uint8_t> bytes;
};

struct ListingLine {
    int line = 0;
    bool has_address = false;
    std::int64_t address = 0;
    std::vector<std::uint8_t> bytes;
    std::optional<Value> value;  // value assigned by EQU, SET or BIT
    std::string source;
};

enum class SymbolKind : std::uint8_t { Label, Equ, Set, Bit };

struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Label;
    Value value;
    int line = 0;
};

struct AssemblyResult {
    std::vector<Diagnostic> diagnostics;
    std::vector<Chunk> chunks;  // in source order
    std::vector<ListingLine> listing;
    std::vector<Symbol> symbols;  // sorted by name
    isa::Cpu cpu = isa::Cpu::C166;
    int passes = 0;

    std::size_t error_count() const;
    std::size_t warning_count() const;
    bool ok() const { return error_count() == 0; }
};

// Assembles a complete source text.
AssemblyResult assemble(std::string_view source, const AssembleOptions& options = {});

// One contiguous memory image.
struct FlatImage {
    std::int64_t base = 0;
    std::vector<std::uint8_t> bytes;
};

// Lays the emitted chunks out as a single image starting at 'base' (default:
// the lowest emitted address). Gaps are filled with 'fill'. Throws
// std::invalid_argument if a chunk lies below 'base' or the image would
// exceed 16 MB.
FlatImage flat_image(const AssemblyResult& result, std::optional<std::int64_t> base = std::nullopt,
                     std::uint8_t fill = 0xFF);

// "file:line:column: error A62: message"
std::string format_diagnostic(const Diagnostic& d);

// Human-readable listing with addresses, code bytes and diagnostics.
std::string format_listing(const AssemblyResult& result);

}  // namespace oc166
