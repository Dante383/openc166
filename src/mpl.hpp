// SPDX-License-Identifier: MIT
// Keil A166's MPL macro processor (Macro Processing Language). MPL reads
// the source as a stream of characters before the assembler sees it: the
// metacharacter '%' starts a call of a built-in function (%DEFINE, %SET,
// %IF, %EVAL, %WHILE, ...) or of a macro defined with %DEFINE, anywhere in a
// line, even in comments and strings. Its output is cut into lines again for
// the assembler. MPL acts on the $INCLUDE and $INCDIR lines of its output
// itself, as Keil's does, so included files are macro processed too.
//
// After its first error MPL stops processing (as Keil's does): the rest of
// the source passes through unchanged.
#pragma once

#include "diag.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oc166 {

// One line of MPL output.
struct MplLine {
    std::string text;
    std::size_t file = 0;  // the host's index of the file being read when the line started,
    int line = 0;          // the line in it (1-based)
    int level = 0;         // and its include depth
    std::vector<Diagnostic> diagnostics;  // MPL's problems, with columns in 'text'
    std::string joined;  // an include file whose last line, without a newline at its end, was joined to this one
    bool fatal = false;  // processing stopped after this line
};

// What MPL needs from the assembler.
struct MplHost {
    std::function<const std::string&(std::size_t file)> text;
    std::function<const std::string&(std::size_t file)> name;
    // Finds the file of '$INCLUDE (name)' in a line from 'file' at include
    // depth 'level'; throws AsmError if it cannot.
    std::function<std::size_t(std::string_view name, std::size_t file, int level, int column)> include;
    // '$INCDIR (dirs)'.
    std::function<void(std::string_view dirs)> incdirs;
};

// Runs MPL over the files (index and include depth) one after the other.
std::vector<MplLine> run_mpl(const std::vector<std::pair<std::size_t, int>>& files, const MplHost& host);

// MPL's text for a number: 0..0FFFFH in hexadecimal with at least two
// digits, a leading 0 before A-F and an 'H' ("05H", "0BC6H"); any other
// value in signed decimal ("-1", "65536").
std::string mpl_number(std::int64_t value);

}  // namespace oc166
