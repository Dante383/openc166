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

enum class SymbolKind : std::uint8_t { Label, Equ, Set, Bit, Lit };

struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Label;
    Value value;  // unused for a LIT
    int line = 0;  // listing line (ListingLine::line) of the definition
    std::string text;  // replacement text of a LIT
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

// The emitted chunks as Intel HEX text: data records (type 00) of up to
// 'record_size' bytes (1..255), an extended linear address record (type 04)
// before the first record of each 64 KB bank, and an end-of-file record
// (type 01). Lines end with CR LF, as OH166 writes them. Only the bytes used
// are written: gaps stay empty. Where chunks overlap, the later one wins, as
// in flat_image(). Throws std::invalid_argument for a bad record size.
std::string format_intel_hex(const AssemblyResult& result, int record_size = 16);

// The emitted chunks as Motorola S-records: a header (S0), data records of
// up to 'record_size' bytes, a record count (S5) and an end record.
// 'address_size' selects the data records: 2 for S1/S9, 3 for S2/S8, 4 for
// S3/S7, or 0 for the smallest that holds every address. Lines end with CR
// LF. Throws std::invalid_argument if an address does not fit or the record
// size is out of range (1..252 for S1, 1..251 for S2, 1..250 for S3).
std::string format_srec(const AssemblyResult& result, int record_size = 16, int address_size = 0);

// "file:line:column: error A62: message"
std::string format_diagnostic(const Diagnostic& d);

// Human-readable listing with addresses, code bytes and diagnostics.
std::string format_listing(const AssemblyResult& result);

}  // namespace oc166
