// SPDX-License-Identifier: MIT
// The assembler driver: runs passes over a source text until every label
// address is stable, then emits machine code, a listing and diagnostics.
#pragma once

#include "diag.hpp"
#include "isa/isa_types.hpp"
#include "value.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oc166 {

// Reads a file, or returns std::nullopt if it cannot be read.
using FileReader = std::function<std::optional<std::string>(const std::string& path)>;

struct AssembleOptions {
    // Name of the source file, used in diagnostics. $INCLUDE also searches
    // the directory of this file.
    std::string file_name = "<input>";
    // CPU selected before the source is read; $MOD167 in the source also
    // selects the C167.
    isa::Cpu cpu = isa::Cpu::C166;
    // Address of code outside any section, and of the first section that has
    // no AT address.
    std::int64_t origin = 0;
    // Directories searched by $INCLUDE. The order is: the current directory,
    // the directory of file_name, these directories, then the directories
    // given by $INCDIR in the source. A bare file name that is found nowhere
    // falls back to the copy built into openc166 (inc/REG167.INC).
    std::vector<std::string> include_dirs;
    // Reads include files. The default reads from the file system and, like
    // Windows, ignores the case of file names.
    FileReader read_file;
};

// Bytes emitted at consecutive addresses.
struct Chunk {
    std::int64_t address = 0;
    std::vector<std::uint8_t> bytes;
};

struct ListingLine {
    int line = 0;       // counts the lines of all files, as Keil's listing does
    std::string file;   // file the line comes from
    int file_line = 0;  // line number within 'file'
    int level = 0;      // $INCLUDE nesting depth; 0 for the main source
    bool skipped = false;  // in a false IF block: not assembled, listed without a line number
    bool hidden = false;   // left out of the printed listing by $NOCOND (assembly is off after it)
    bool has_address = false;
    std::int64_t address = 0;
    std::vector<std::uint8_t> bytes;
    std::optional<Value> value;  // value assigned by EQU, SET or BIT
    std::string source;
    std::vector<std::size_t> diagnostics;  // indices into AssemblyResult::diagnostics
};

enum class SymbolKind : std::uint8_t { Label, Equ, Set, Bit };

struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Label;
    Value value;
    int line = 0;  // listing line (ListingLine::line) of the definition
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
