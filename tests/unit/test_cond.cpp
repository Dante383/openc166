// SPDX-License-Identifier: MIT
// Conditional assembly: $SET, $RESET, $IF and the IF statement.
#include "helpers.hpp"

#include <doctest.h>

using oc166::AssemblyResult;

namespace {

const oc166::ListingLine* line(const AssemblyResult& r, int n) {
    for (const auto& l : r.listing)
        if (l.line == n) return &l;
    return nullptr;
}

// Code bytes and diagnostic codes of a line, like test::one().
std::string at(const AssemblyResult& r, int n) {
    std::string c = test::code(r, n), d = test::diags(r, n);
    return c + (!c.empty() && !d.empty() ? " " : "") + d;
}

// Lines 3.. of a C167 code section at 0C000H.
AssemblyResult body(const std::string& lines) {
    return test::run("S SECTION CODE AT 0C000H\nP PROC NEAR\n" + lines + "P ENDP\nS ENDS\n END\n");
}

}  // namespace

TEST_CASE("only bit 0 of a condition counts, as in Keil") {
    auto r = body(" IF 3\n DB 1\n ENDIF\n"            // 3-5
                  " IF 2\n DB 2\n ENDIF\n"            // 6-8
                  " IF NOT 1\n DB 3\n ENDIF\n"        // 9-11
                  " IF NOT 0\n DB 4\n ENDIF\n"        // 12-14
                  "$IF (4)\n DB 5\n$ENDIF\n");        // 15-17
    CHECK(at(r, 4) == "01");
    CHECK(at(r, 6) == "OC10W");
    CHECK(line(r, 7)->skipped);
    // NOT 1 is 0FFFEH: false, but common enough not to warn about.
    CHECK(at(r, 9) == "");
    CHECK(line(r, 10)->skipped);
    CHECK(at(r, 13) == "04");
    CHECK(at(r, 15) == "OC10W");
    CHECK(line(r, 16)->skipped);
}

TEST_CASE("$SET and $RESET flags are seen by $IF only") {
    auto r = test::run("$SET (A, B = 3, C = 0)\n"         // 1
                       "$RESET (B)\n"                     // 2
                       "X EQU 1\n"                        // 3
                       "S SECTION CODE AT 0C000H\nP PROC NEAR\n"
                       "$IF (A AND NOT B AND NOT C)\n DB 1\n$ENDIF\n"  // 6-8
                       "$IF (X)\n DB 2\n$ENDIF\n"                      // 9-11: X is not a flag
                       "$IF (UNKNOWN)\n DB 3\n$ENDIF\n"                // 12-14
                       " IF X\n DB 4\n ENDIF\n"                        // 15-17
                       " IF A\n DB 5\n ENDIF\n"                        // 18-20
                       "P ENDP\nS ENDS\n END\n");
    CHECK(at(r, 7) == "01");
    CHECK(line(r, 10)->skipped);
    CHECK(line(r, 13)->skipped);
    CHECK(at(r, 16) == "04");
    CHECK(at(r, 18) == "A71E");
    CHECK(line(r, 19)->skipped);
}

TEST_CASE("$SET errors") {
    auto r = test::run("$SET (A = 1+2)\n$SET ()\n$SET (B = 'X')\n$SET C\n$RESET (D = 1)\n END\n");
    CHECK(test::diags(r, 1) == "A22E");
    CHECK(test::diags(r, 2) == "A23E");
    CHECK(test::diags(r, 3) == "A1E");
    CHECK(test::diags(r, 4) == "A22E");
    CHECK(test::diags(r, 5) == "A22E");
}

TEST_CASE("ELSEIF and ELSE pick the first true branch") {
    auto r = body(" IF 0\n DB 1\n ELSEIF 1\n DB 2\n ELSEIF 1\n DB 3\n ELSE\n DB 4\n ENDIF\n"   // 3-11
                  "$IF (0)\n DB 5\n$ELSEIF (0)\n DB 6\n$ELSE\n DB 7\n$ENDIF\n");              // 12-18
    CHECK(line(r, 4)->skipped);
    CHECK(at(r, 6) == "02");
    CHECK(line(r, 8)->skipped);
    CHECK(line(r, 10)->skipped);
    CHECK(line(r, 13)->skipped);
    CHECK(line(r, 15)->skipped);
    CHECK(at(r, 17) == "07");
}

TEST_CASE("listing line numbers of conditional lines follow Keil") {
    auto r = body(" IF 0\n NOP\n ELSE\n NOP\n ENDIF\n"   // 3-7
                  "$IF (0)\n NOP\n$ELSE\n NOP\n$ENDIF\n");  // 8-12
    // An IF statement is numbered when the lines before it are assembled...
    CHECK_FALSE(line(r, 3)->skipped);
    CHECK(line(r, 4)->skipped);
    CHECK(line(r, 5)->skipped);
    CHECK_FALSE(line(r, 7)->skipped);
    // ...a $IF line when the lines after it are.
    CHECK(line(r, 8)->skipped);
    CHECK_FALSE(line(r, 10)->skipped);
    CHECK_FALSE(line(r, 12)->skipped);
    std::string lst = oc166::format_listing(r);
    // Skipped lines have no number.
    CHECK(lst.find("\n" + std::string(39, ' ') + " NOP\n") != std::string::npos);
}

TEST_CASE("$NOCOND leaves out the lines after which assembly is off") {
    auto r = body("$NOCOND\n IF 0\n NOP\n ENDIF\n$IF (0)\n NOP\n$ENDIF\n NOP\n$COND\n IF 0\n ENDIF\n");
    for (int n : {4, 5, 7, 8}) CHECK(line(r, n)->hidden);
    for (int n : {6, 9, 10, 12, 13}) CHECK_FALSE(line(r, n)->hidden);
    std::string lst = oc166::format_listing(r);
    CHECK(lst.find("IF 0") == lst.rfind("IF 0"));
    CHECK(lst.find("$IF (0)") == std::string::npos);
    CHECK(lst.find("12     IF 0") != std::string::npos);
}

TEST_CASE("nesting") {
    SUBCASE("statement IFs nest inside $IF blocks and close them in any order") {
        auto r = body("$IF (1)\n IF 0\n DB 1\n$ENDIF\n DB 2\n ENDIF\n");
        CHECK(line(r, 5)->skipped);
        CHECK(at(r, 7) == "02");
        CHECK(r.ok());
    }
    SUBCASE("$IF allows 9 levels") {
        std::string src;
        for (int i = 0; i < 10; ++i) src += "$IF (1)\n";
        src += " DB 1\n";
        for (int i = 0; i < 10; ++i) src += "$ENDIF\n";
        auto r = body(src);
        CHECK(test::diags(r, 12) == "A6E");
        CHECK(at(r, 13) == "01");
        CHECK(test::diags(r, 23) == "A17E");
    }
    SUBCASE("a tenth level in a false block is ignored without a word") {
        std::string src = "$IF (0)\n";
        for (int i = 0; i < 9; ++i) src += "$IF (1)\n";
        for (int i = 0; i < 9; ++i) src += "$ENDIF\n";
        auto r = body(src);
        CHECK(r.ok());
    }
}

TEST_CASE("errors in conditions") {
    auto r = body("L: NOP\n"                         // 3
                  " IF FWD\n NOP\n ENDIF\n"          // 4-6
                  " IF L\n NOP\n ENDIF\n"            // 7-9
                  " IF R1\n NOP\n ENDIF\n"           // 10-12
                  " IF $\n NOP\n ENDIF\n"            // 13-15
                  " IF 1.0\n NOP\n ENDIF\n"          // 16-18
                  " IF\n NOP\n ENDIF\n"              // 19-21
                  "FWD EQU 1\n");
    CHECK(test::diags(r, 4) == "A71E");
    CHECK(test::diags(r, 7) == "A11E");
    CHECK(test::diags(r, 10) == "A11E");
    CHECK(test::diags(r, 13) == "A6E");
    CHECK(test::diags(r, 16) == "A6E");
    CHECK(test::diags(r, 19) == "A6E");
    // A condition with an error counts as false.
    for (int n : {5, 8, 11, 14, 17, 20}) CHECK(line(r, n)->skipped);
}

TEST_CASE("odd structures that Keil accepts get a warning") {
    auto r = body(" ENDIF\n"                                  // 3
                  " IF 1 2\n NOP\n ELSE\n ELSE\n NOP\n ENDIF\n"  // 4-9
                  " IF 1\n ENDIF junk\n");                    // 10-11
    CHECK(test::diags(r, 3) == "OC9W");
    CHECK(test::diags(r, 4) == "OC9W");
    CHECK(test::diags(r, 7) == "OC9W");
    CHECK(line(r, 8)->skipped);
    CHECK(test::diags(r, 11) == "OC9W");
    CHECK(r.ok());
}

TEST_CASE("misplaced $ELSE and $ENDIF") {
    auto r = test::run("$ELSE\n$ENDIF\n$IF (1)\n$ELSE\n$ELSE\n$ELSEIF (1)\n$ENDIF\n END\n");
    CHECK(test::diags(r, 1) == "A17E");
    CHECK(test::diags(r, 2) == "A17E");
    CHECK(test::diags(r, 5) == "A17E");
    CHECK(test::diags(r, 6) == "A6E");
    CHECK(test::diags(r, 7) == "");
}

TEST_CASE("a second conditional keyword on a line is an error and the line is ignored") {
    auto r = body(" IF 0\n ELSE IF 1\n NOP\n ENDIF\n IF 1\n ELSE ELSE\n NOP\n ENDIF ENDIF\n ENDIF\n");
    CHECK(test::diags(r, 4) == "A6E");
    CHECK(line(r, 5)->skipped);
    CHECK(test::diags(r, 8) == "A6E");
    CHECK(test::code(r, 9) == "CC00");
    CHECK(test::diags(r, 10) == "A6E");
    CHECK(test::diags(r, 11) == "");
}

TEST_CASE("a label before IF is an error, but the IF still counts") {
    auto r = body("L: IF 0\n NOP\n ENDIF\n");
    CHECK(test::diags(r, 3) == "A6E");
    CHECK(line(r, 4)->skipped);
    CHECK(test::diags(r, 5) == "");
}

TEST_CASE("controls may be indented") {
    auto r = body("   $IF (0)\n NOP\n\t$ENDIF\n");
    CHECK(line(r, 4)->skipped);
    CHECK(r.ok());
}

TEST_CASE("conditionals do not end the primary controls") {
    auto r = test::run("$SET (X)\n$IF (X)\n IF 1\n$MOD167\n ENDIF\n$ENDIF\n"
                       "S SECTION CODE AT 0C000H\nP PROC NEAR\n EXTR #1\nP ENDP\nS ENDS\n END\n",
                       oc166::isa::Cpu::C166);
    CHECK(r.ok());
    CHECK(r.cpu == oc166::isa::Cpu::C167);
}

TEST_CASE("problems at the end of the file") {
    SUBCASE("an unclosed IF is reported on the last line") {
        auto r = body(" IF 1\n");
        CHECK(test::diags(r, 6) == "A12E");
        CHECK(r.listing.back().diagnostics.size() == 1);
    }
    SUBCASE("END in a false block does not count") {
        auto r = test::run(" IF 0\n END\n");
        CHECK(test::diags(r, 2) == "A89W A12E");
    }
    SUBCASE("lines after END are listed but not assembled") {
        auto r = test::run(" IF 1\n END\n NOP\n bogus\n ENDIF\n$IF (1)\n");
        CHECK(r.listing.size() == 6);
        CHECK(test::diags(r, 6) == "A12E");
        CHECK(r.error_count() == 1);
    }
    SUBCASE("one A106 on the END line for an open procedure and section") {
        auto r = test::run("S SECTION CODE AT 0C000H\nP PROC NEAR\n NOP\n END\n\n");
        CHECK(test::diags(r, 4) == "A106E");
        CHECK(r.error_count() == 1);
    }
}
