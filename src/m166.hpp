// SPDX-License-Identifier: MIT
// The part of TASKING's m166 macro preprocessor that openc166 supports, for
// sources in the TASKING dialect: macro variables (@SET, @name), @EVAL,
// @DEFINED, conditional text (@IF, @ELSE, @ENDI) and @" comments. m166
// works on characters, not statements, so these work anywhere in a line.
// Defining macros (@DEFINE), loops and the string functions are not
// supported (OC29).
#pragma once

#include "diag.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace oc166 {

class M166 {
public:
    // What one source line becomes.
    struct Line {
        std::string text;  // the text the assembler reads
        bool calls = false;  // the line held an m166 call or comment
        bool active_before = true;  // its text was kept at its start ...
        bool active_after = true;   // ... and at its end
    };

    // Forgets all macros and open @IF blocks, for a new pass.
    void reset();
    // Defines a macro whose call expands to 'text', like m166's DEFINE
    // control. Replaces a macro of the same name.
    void define(std::string_view name, std::string text, bool case_sensitive);
    // Expands the m166 calls in one line. Problems go to 'rep', with columns
    // in 'text'. A @" comment without a closing quote takes the end of the
    // line with it, so what is left of the line is put before the next one.
    Line process(std::string_view text, bool case_sensitive, Reporter& rep);
    // @IF blocks not closed by @ENDI.
    std::size_t open_ifs() const { return ifs_.size(); }
    // The text is kept (not in a false @IF or @ELSE branch).
    bool active() const { return ifs_.empty() || ifs_.back().active; }

    // m166's text for a number: "0", the hexadecimal digits of the 32-bit
    // value in upper case, and "h" (16 gives "010h", -1 gives "0FFFFFFFFh").
    static std::string format(std::int64_t value);

private:
    struct If {
        bool parent_active = true;
        bool active = true;
        bool taken = false;
        bool in_else = false;
    };
    std::vector<If> ifs_;
    std::map<std::string, std::string> macros_;  // by name as written
    std::string pending_;  // text before a @" comment that ran to the end of its line

    const std::string* find(std::string_view name, bool case_sensitive) const;
    std::string expand(std::string_view text, int column, bool undefined_is_zero, bool case_sensitive,
                       Reporter& rep);
    bool call(std::string_view text, std::size_t& i, int base_column, bool undefined_is_zero,
              bool case_sensitive, Reporter& rep, std::string& out);
    std::int64_t evaluate(std::string_view text, int column, bool undefined_is_zero, bool case_sensitive,
                          Reporter& rep, bool& ok);
};

}  // namespace oc166
