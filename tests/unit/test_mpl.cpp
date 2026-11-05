// SPDX-License-Identifier: MIT
// MPL, Keil's macro processing language. The expected texts were checked
// against Keil A166.
#include "helpers.hpp"
#include "mpl.hpp"

#include <doctest.h>

#include <map>

using oc166::AssembleOptions;
using oc166::AssemblyResult;

namespace {

AssembleOptions options(const std::map<std::string, std::string>* files = nullptr) {
    AssembleOptions o;
    o.file_name = "main.a66";
    o.cpu = oc166::isa::Cpu::C167;
    if (files)
        o.read_file = [files](const std::string& path) -> std::optional<std::string> {
            auto it = files->find(path);
            if (it == files->end()) return std::nullopt;
            return it->second;
        };
    return o;
}

// The listed text of each line MPL produced, each followed by the codes of
// its diagnostics, e.g. "; 07H" or "; BC [A152E]". An END line is added to
// the source and left out of the result.
std::string mpl(std::string source, const std::map<std::string, std::string>* files = nullptr) {
    AssemblyResult r = oc166::assemble(source + "\n END\n", options(files));
    std::string out;
    std::size_t lines = r.listing.size();
    if (lines > 0 && r.listing.back().source == " END") --lines;
    for (std::size_t n = 0; n < lines; ++n) {
        const auto& l = r.listing[n];
        if (n > 0) out += '\n';
        out += l.source;
        for (auto i : l.diagnostics) {
            const auto& d = r.diagnostics[i];
            out += " [" + d.code + (d.severity == oc166::Severity::Error ? "E" : "W") + "]";
        }
    }
    return out;
}

std::string eval(const std::string& expression) { return mpl("; %EVAL(" + expression + ")"); }

bool has(const AssemblyResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics)
        if (d.code == code) return true;
    return false;
}

}  // namespace

TEST_CASE("MPL numbers") {
    using oc166::mpl_number;
    CHECK(mpl_number(0) == "00H");
    CHECK(mpl_number(5) == "05H");
    CHECK(mpl_number(0x0A) == "0AH");
    CHECK(mpl_number(0xBC) == "0BCH");
    CHECK(mpl_number(0x100) == "0100H");
    CHECK(mpl_number(0xBC6) == "0BC6H");
    CHECK(mpl_number(0x1000) == "1000H");
    CHECK(mpl_number(0xFFFF) == "0FFFFH");
    CHECK(mpl_number(0x10000) == "65536");
    CHECK(mpl_number(-1) == "-1");
}

TEST_CASE("MPL expressions") {
    SUBCASE("precedence: unary, then * / MOD, + -, SHL SHR, relations, AND OR XOR") {
        CHECK(eval("1 + 2 * 3") == "; 07H");
        CHECK(eval("(1 + 2) * 3") == "; 09H");
        CHECK(eval("-2 * 3") == "; -6");
        CHECK(eval("2 + 3 SHL 1") == "; 0AH");
        CHECK(eval("1 SHL 2 EQ 4") == "; 01H");
        CHECK(eval("1 EQ 1 AND 0") == "; 00H");
        CHECK(eval("2 LT 3 AND 3 LT 2") == "; 00H");
    }
    SUBCASE("operators") {
        CHECK(eval("1 + + 2") == "; 03H");
        CHECK(eval("NOT 0") == "; 01H");
        CHECK(eval("NOT 5") == "; 00H");
        CHECK(eval("HIGH 1234H") == "; 12H");
        CHECK(eval("LOW 1234H") == "; 34H");
        CHECK(eval("7 MOD 4") == "; 03H");
        CHECK(eval("-7 / 2") == "; -3");
        CHECK(eval("-7 MOD 2") == "; -1");
        CHECK(eval("6 AND 3") == "; 02H");
        CHECK(eval("6 OR 3") == "; 07H");
        CHECK(eval("6 XOR 3") == "; 05H");
        CHECK(eval("2 GT 1") == "; 01H");
        CHECK(eval("2 GE 3") == "; 00H");
        CHECK(eval("1 NE 2") == "; 01H");
    }
    SUBCASE("shift counts are taken modulo 32") {
        CHECK(eval("1 SHL 33") == "; 02H");
        CHECK(eval("-1 SHR 40") == "; -1");
        CHECK(eval("1 SHL -1") == "; -2147483648");
    }
    SUBCASE("numbers and strings") {
        CHECK(eval("12$3") == "; 7BH");
        CHECK(eval("1$") == "; 01H");
        CHECK(eval("0ffh") == "; 0FFH");
        CHECK(eval("101B") == "; 05H");
        CHECK(eval("17Q") == "; 0FH");
        CHECK(eval("17O") == "; 0FH");
        CHECK(eval("10D") == "; 0AH");
        CHECK(eval("10000H") == "; 65536");
        CHECK(eval("'A") == "; 41H");
        CHECK(eval("'AB") == "; 4142H");
        CHECK(eval("\"A\"") == "; 41H");
    }
    SUBCASE("a missing operand makes the value 0 without an error") {
        CHECK(mpl("; %EVAL(1 +) %EVAL(()) %EVAL(NOT)\n; %EVAL(1)") == "; 00H 00H 00H\n; 01H");
    }
    SUBCASE("errors give 00H and turn MPL off") {
        const std::map<std::string, std::string> cases = {
            {"1 2", "A168"}, {"Z", "A165"},   {"19Q", "A162"}, {"'", "A164"},
            {"=", "A166"},   {"1/0", "A167"}, {"% 3", "A166"},
        };
        for (const auto& [expression, code] : cases) {
            CAPTURE(expression);
            CHECK(mpl("; %EVAL(" + expression + ")\n; %EVAL(1)") == "; 00H [" + code + "E]\n; %EVAL(1)");
        }
        CHECK(mpl("; %EVAL('A\nB')\n; %EVAL(1)") == "; 00H [A163E]\n; %EVAL(1)");
    }
}

TEST_CASE("MPL macros") {
    SUBCASE("parameters; names ignore case") {
        CHECK(mpl("%*DEFINE (TWICE (X)) (%X %X)\n; %TWICE(ab) %twice(cd)") == "\n; ab ab cd cd");
    }
    SUBCASE("%DEFINE expands its body at once, %*DEFINE at each call") {
        CHECK(mpl("%SET (A, 1)\n%DEFINE (EAGER) (%A)\n%*DEFINE (LAZY) (%A)\n%SET (A, 2)\n; %EAGER %LAZY %*LAZY") ==
              "\n\n\n\n; 01H 02H %A");
    }
    SUBCASE("a multi-line body") {
        CHECK(mpl("%*DEFINE (TWO) (\n NOP\n NOP)\n%TWO") == "\n\n\n\n NOP\n NOP");
    }
    SUBCASE("LOCAL names") {
        CHECK(mpl("%*DEFINE (LBL) LOCAL L M (%L %M)\n; %LBL / %LBL") == "\n; ??L?2 ??M?3 / ??L?6 ??M?7");
    }
    SUBCASE("delimiters other than commas") {
        // The last parameter ends at a blank and takes that blank along.
        CHECK(mpl("%*DEFINE (CP A @TO B) (%A<-%B)\n; %CP R10 TO R11 !") == "\n;  R10 <-R11!");
        CHECK(mpl("%*DEFINE (PAIR[X;Y]) (%Y,%X)\n; %PAIR[1;2]") == "\n; 2,1");
    }
    SUBCASE("a built-in wins over a macro; a parameter wins over a built-in") {
        CHECK(mpl("%*DEFINE (LEN(X)) (macro)\n; %LEN(AB)") == "\n; 2");
        CHECK(mpl("%*DEFINE (M(SET)) (%SET)\n; %M(5)") == "\n; 5");
    }
    SUBCASE("an undefined macro") {
        CHECK(mpl("; 50% done\n; %EVAL(1)") == "; 50 [A160E]\n; %EVAL(1)");
    }
    SUBCASE("endless recursion stops") {
        AssemblyResult r = oc166::assemble("%*DEFINE (R) (%R)\n%R\n END\n", options());
        CHECK(has(r, "OC33"));
        CHECK(!r.ok());
    }
}

TEST_CASE("MPL built-in functions") {
    SUBCASE("strings") {
        CHECK(mpl("; %LEN(ABCDE) %LEN() %EQS(AB,AB) %NES(AB,AB) %LTS(A,B) %GES(A,B)") == "; 5 0 1 0 1 0");
        CHECK(mpl("; %SUBSTR(HELLO,2,3) [%SUBSTR(HELLO,9,1)]") == "; ELL []");
    }
    SUBCASE("%IF, %WHILE, %REPEAT and %EXIT") {
        CHECK(mpl("; %IF (1) THEN (yes) ELSE (no) FI %IF (0) THEN (yes) ELSE (no) FI %IF (0) THEN (yes) FI.") ==
              "; yes no .");
        CHECK(mpl("%SET (I, 0)\n; %WHILE (%I LT 3) (%I %SET (I, %I + 1)) %REPEAT (3) (r) %REPEAT (0) (z)") ==
              "\n; 00H 01H 02H  rrr ");
        CHECK(mpl("%SET (J, 0)\n; %WHILE (1) (%SET (J, %J + 1)%IF (%J GT 4) THEN (%EXIT) FI%J )") ==
              "\n; 01H 02H 03H 04H ");
        CHECK(mpl("%EXIT\n; %EVAL(1)") == " [A157E]\n; %EVAL(1)");
    }
    SUBCASE("%IF without FI drops a character") {
        CHECK(mpl("%IF (1) THEN (a)\n DW 2\n; %EVAL(1)") ==
              "aW 2 [A154E] [A6E]\n; %EVAL(1)");
    }
    SUBCASE("%MATCH") {
        CHECK(mpl("%MATCH (FIRST,REST) (R7,R8,R9)\n; %FIRST / %REST") == "\n; R7 / R8,R9");
        CHECK(mpl("%MATCH (P Q) (  ONE TWO THREE)\n; %P / %Q") == "\n; ONE / TWO THREE");
    }
    SUBCASE("a missing '(' drops the next character") {
        CHECK(mpl("; %LEN ABC\n; %EVAL(1)") == "; BC [A152E]\n; %EVAL(1)");
    }
    SUBCASE("%METACHAR") {
        CHECK(mpl("%SET (A, 2)\n%METACHAR (&)\n; &A %A &EVAL(&A + 1)\n&METACHAR (%)\n; %A") ==
              "\n\n; 02H %A 03H\n\n; 02H");
    }
    SUBCASE("%IN and %OUT have no console") {
        AssemblyResult r = oc166::assemble("; %IN %OUT(hi)\n END\n", options());
        CHECK(r.ok());
        CHECK(r.warning_count() == 2);
        CHECK(has(r, "OC34"));
    }
}

TEST_CASE("MPL comments and escapes") {
    CHECK(mpl("; a%' comment'b %3%%% %(%N is not called) %0end") == "; ab %%% %N is not called end");
    CHECK(mpl("%' a whole comment line\n; x") == "; x");
    CHECK(mpl("; % 3x\n; %EVAL(1)") == "; %x [A151E]\n; %EVAL(1)");
}

TEST_CASE("MPL and include files") {
    std::map<std::string, std::string> files = {{"m.inc", "%*DEFINE (INCV) (five)\n"}, {"x/a.inc", "; a\n"}};
    SUBCASE("macros of an include file") {
        CHECK(mpl("$INCLUDE (m.inc)\n; %INCV", &files) == "$INCLUDE (m.inc)\n\n; five");
    }
    SUBCASE("a file name made by MPL") {
        CHECK(mpl("$INCDIR (x)\n%DEFINE (F) (a.inc)\n$INCLUDE (%F)", &files) == "$INCDIR (x)\n\n$INCLUDE (a.inc)\n; a");
    }
    SUBCASE("$INCDIR after another control is ignored") {
        CHECK(mpl("$NOLIST INCDIR (x)\n$INCLUDE (a.inc)", &files) ==
              "$NOLIST INCDIR (x) [OC7W]\n$INCLUDE (a.inc) [OC5E]");
    }
    SUBCASE("no control may follow $INCLUDE") {
        CHECK(mpl("$INCLUDE (x/a.inc) LIST", &files) == "$INCLUDE (x/a.inc) LIST [A171E]");
    }
    SUBCASE("$NOMPL") {
        CHECK(mpl("$NOMPL\n; %EVAL(1)") == "$NOMPL\n; %EVAL(1)");
    }
}
