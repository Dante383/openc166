// SPDX-License-Identifier: MIT
// Standard macros (MACRO, REPT, IRP, IRPC), '%' macro arguments, tab
// expansion, $TABS and respecified primary controls (A7).
#include "helpers.hpp"

#include <doctest.h>

using oc166::AssemblyResult;

namespace {

const oc166::ListingLine* line(const AssemblyResult& r, int n) {
    for (const auto& l : r.listing)
        if (l.line == n) return &l;
    return nullptr;
}

// All code bytes, in listing order.
std::string bytes(const AssemblyResult& r) {
    std::string s;
    for (const auto& l : r.listing) s += test::hex(l.bytes);
    return s;
}

// All diagnostic codes with their file lines, e.g. "2:A7E 9:OC26W".
std::string all_diags(const AssemblyResult& r) {
    std::string s;
    for (const auto& d : r.diagnostics) {
        if (!s.empty()) s += ' ';
        s += std::to_string(d.line) + ':' + d.code + (d.severity == oc166::Severity::Error ? "E" : "W");
    }
    return s;
}

// 'head' (definitions, controls) followed by 'body' in a DATA section.
// $NOMPL makes '%' a standard macro operator.
AssemblyResult data(const std::string& head, const std::string& body, bool nompl = true) {
    return test::run(std::string(nompl ? "$NOMPL\n" : "") + head + "D SECTION DATA AT 0F000H\n" + body +
                     "D ENDS\n END\n");
}

}  // namespace

TEST_CASE("parameters, LOCAL names and ';;' comments") {
    auto r = test::run("M MACRO A, B\n"       // 1
                       " LOCAL L\n"           // 2
                       "L: DB A, B ;; gone\n"  // 3
                       " DW L ; kept: A\n"    // 4
                       " ENDM\n"              // 5
                       "D SECTION DATA AT 0F000H\n"
                       " M 1, 2\n"            // 7, expanded as 8-9
                       " M 3, 4\n"            // 10, expanded as 11-12
                       "D ENDS\n END\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "010200F0030404F0");
    REQUIRE(line(r, 8));
    CHECK(line(r, 8)->macro_level == 1);
    CHECK(line(r, 8)->source.rfind("??0000: DB 1, 2", 0) == 0);
    CHECK(line(r, 8)->source.find("gone") == std::string::npos);
    CHECK(line(r, 9)->source.find("kept: 1") != std::string::npos);
    CHECK(line(r, 11)->source.rfind("??0001:", 0) == 0);
    // $NOGEN is the default: the expanded lines are left out of the listing.
    CHECK(line(r, 8)->hidden);
    CHECK(!line(r, 7)->hidden);
}

TEST_CASE("'&' joins a parameter to the text around it") {
    auto r = data("J MACRO N\n DB 'R&N', N&0H\n ENDM\n", " J 3\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "523330");
}

TEST_CASE("argument splitting") {
    auto r = data("S MACRO A, B\n DB A\n DB B\n ENDM\n",
                  " S <1, 2>, 3 ; a comment, 4\n"  // '<>' protects commas, ';' ends
                  " S '1,2', 4, 5\n"              // quotes protect; extra arguments are ignored
                  " S <1>2\n"                     // a '<>' argument ends at the '>'
                  " S <1>>2\n");                  // and a '>' right after it is skipped
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "010203" "312C3204" "0102" "0102");
}

TEST_CASE("'!' at the start of an argument passes the next character") {
    auto r = data("S MACRO A, B, C\n DB '[A|B|C]'\n ENDM\n",
                  " S !<, 7, 8\n"     // 6, expanded as 7
                  " S !<a, 7, 8\n"    // 8: the argument ends after the '<'
                  " S a!<b, 7\n"      // 10: elsewhere '!' is ordinary
                  " S <!>>, 8\n"      // 12: also in brackets
                  " S !!\n");         // 14
    CHECK(all_diags(r) == "");
    CHECK(test::code(r, 7) == "5B3C7C377C385D");
    CHECK(test::code(r, 9) == "5B3C7C617C375D");
    CHECK(test::code(r, 11) == "5B61213C627C377C5D");
    CHECK(test::code(r, 13) == "5B217C387C5D");
    CHECK(test::code(r, 15) == "5B217C7C5D");
    // Keil passes garbage after '!;', and the line break for a '!' at the end.
    CHECK(all_diags(data("S MACRO A, B, C\n DB '[A|B|C]'\n ENDM\n", " S !;z, 7\n S !\n")) ==
          "6:OC27W 7:OC27W");
}

TEST_CASE("no macro argument starts after a ';' or a quote without its end") {
    auto r = data("S MACRO A, B, C\n DB '[A|B|C]'\n ENDM\n",
                  " S <a;b>, 6, 7\n"   // 6, expanded as 7
                  " S 1, \"x, 7\n"     // 8 (file line 7)
                  " S <a\"b>, 6, 7\n"  // 10 (file line 8)
                  " S !\", 7, 8\n");   // 12 (file line 9)
    CHECK(test::code(r, 7) == "5B613B627C7C5D");
    CHECK(test::code(r, 9) == "5B317C22782C20377C5D");
    CHECK(test::code(r, 11) == "5B6122627C7C5D");
    CHECK(test::code(r, 13) == "5B227C7C5D");
    CHECK(all_diags(r) == "7:A2E 8:A2E 9:A2E");
}

TEST_CASE("NUL tells whether an argument is missing") {
    auto r = data("O MACRO A, B\n IF NUL B\n DB A\n ELSE\n DB A+B\n ENDIF\n ENDM\n",
                  " O 2\n O 2, 3\n O 2, <>\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "020502");
}

TEST_CASE("REPT, IRP and IRPC") {
    auto r = data("",
                  " REPT 3\n DB 7\n ENDM\n"
                  " IRP X, <1, 2>\n DB X\n ENDM\n"
                  " IRPC C, AB\n DB 'C'\n ENDM\n"
                  " IRP X, <>\n DB 9 X\n ENDM\n"  // one round, with X empty
                  " REPT 0\n DB 8\n ENDM\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "070707" "0102" "4142" "09");
}

TEST_CASE("EXITM ends the innermost expansion and the IF blocks opened in it") {
    auto r = data("K SET 0\n",
                  " REPT 5\n"
                  "K SET K+1\n"
                  " IF K EQ 3\n"
                  " EXITM\n"
                  " ENDIF\n"
                  " DB K\n"
                  " ENDM\n"
                  " DB 0FFH\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "0102FF");
}

TEST_CASE("macros can call themselves, up to 9 levels deep") {
    auto r = data("CNT SET 3\n"
                  "DOWN MACRO\n IF CNT GT 0\n DB CNT\nCNT SET CNT-1\n DOWN\n ENDIF\n ENDM\n",
                  " DOWN\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "030201");

    // Keil A166 hangs at 10 levels; openc166 stops with OC24 at the call.
    r = data("LOOP MACRO\n DB 1\n LOOP\n ENDM\n", " LOOP\n");
    CHECK(all_diags(r) == "7:OC24E");
    CHECK(bytes(r) == "010101010101010101");
}

TEST_CASE("macro definition errors") {
    CHECK(all_diags(data("", " ENDM\n LOCAL X\n EXITM\n")) == "3:A6E 4:A6E 5:A124E");
    // A duplicate parameter: the macro is not defined.
    CHECK(all_diags(data("M MACRO A, A\n DB A\n ENDM\n", "")) == "2:A123E");
    // A local named like a parameter: the macro is defined.
    CHECK(all_diags(data("M MACRO A\n LOCAL A\n DB A\n ENDM\n", " M 1\n")) == "3:A123E");
    // No ENDM before END.
    auto r = test::run("M MACRO\n DB 1\n END\n");
    CHECK(all_diags(r) == "3:A120E 3:A89W");
}

TEST_CASE("$GEN, $NOGEN, $SAVE and $RESTORE") {
    auto r = data("M MACRO\n DB 1\n ENDM\n",
                  " M\n"            // 6, 7
                  "$SAVE GEN\n"     // 8
                  " M\n"            // 9, 10
                  "$RESTORE\n"      // 11
                  " M\n");          // 12, 13
    CHECK(all_diags(r) == "");
    CHECK(line(r, 7)->hidden);
    CHECK(!line(r, 10)->hidden);
    CHECK(line(r, 13)->hidden);
    // The listing marks expanded lines with their level, as Keil does.
    const std::string listing = oc166::format_listing(r);
    CHECK(listing.find("  10+1 ") != std::string::npos);
    CHECK(listing.find("  7+1 ") == std::string::npos);

    CHECK(all_diags(data("", "$RESTORE\n")) == "3:A13E");
    std::string saves;
    for (int i = 0; i < 11; ++i) saves += "$SAVE\n";
    CHECK(all_diags(data("", saves)) == "13:A14E");
}

TEST_CASE("'%' under $NOMPL passes the value of an expression") {
    const std::string defs = "CNT SET 5\n"
                             "Q MACRO A, B\n DB A\n IF NUL B\n DB 0FFH\n ELSE\n DB B\n ENDIF\n ENDM\n";
    auto r = data(defs, " Q %CNT\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "05FF");

    // Keil counts numbers as 0, so %(CNT+1) passes 5. openc166 does the
    // same and warns.
    r = data(defs, " Q %(CNT+1)\n");
    CHECK(all_diags(r) == "12:OC26W");
    CHECK(bytes(r) == "05FF");

    // After a ',' Keil starts the next argument right after the first
    // token, so B is empty here and 4 is a third argument.
    r = data(defs, " Q %CNT, 4\n");
    CHECK(all_diags(r) == "12:OC26W");
    CHECK(bytes(r) == "05FF");
    r = data(defs, " Q 4, %CNT\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "0405");

    // After an error the value is 0.
    r = data(defs, " Q %UNDEF\n");
    CHECK(all_diags(r) == "12:A71E");
    CHECK(bytes(r) == "00FF");
    CHECK(all_diags(data(defs, " Q %\n")) == "12:A6E");

    // $MPL (the default) makes '%' a call of the macro processor: CNT is
    // not an MPL macro.
    CHECK(all_diags(data(defs, " Q %CNT\n", false)).rfind("11:A160E", 0) == 0);
}

TEST_CASE("$NOMPL in the primary controls covers the lines above it") {
    CHECK(all_diags(test::run("; 50% sure\n$NOMPL\n END\n")) == "");
    // The last of them counts. MPL's error about the undefined macro SURE
    // turns it off for the rest of the source, as in Keil.
    auto r = test::run("; 50% sure\n$NOMPL\n; 60% sure\n$MPL\n; 70% sure\n END\n");
    CHECK(all_diags(r) == "1:A160E 4:A7E");
    CHECK(r.listing[0].source == "; 50");
    // After the first statement it is too late for $NOMPL.
    CHECK(all_diags(test::run("X EQU 1\n; 50% sure\n$NOMPL\n END\n")) == "2:A160E 3:A8E");
}

TEST_CASE("REPT counts") {
    CHECK(all_diags(data("", " REPT UNDEF\n DB 1\n ENDM\n")) == "3:A71E");
    CHECK(all_diags(data("", " REPT\n DB 1\n ENDM\n")) == "3:A6E");
    CHECK(all_diags(test::run("D SECTION DATA\nL: DB 0\n REPT L\n DB 1\n ENDM\nD ENDS\n END\n")) == "3:A72E");
}

TEST_CASE("tabs are expanded before a line is read, also in strings") {
    // The tab at column 1 goes to column 9, the one in the string from
    // column 13 to 17.
    auto r = data("", "\tDB '\tC'\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "2020202043");

    r = data("$TABS(3)\n", "\tDB '\tC'\n");
    CHECK(all_diags(r) == "");
    CHECK(bytes(r) == "202043");
    CHECK(bytes(data("$TABS(3H)\n", "\tDB '\tC'\n")) == "202043");
    CHECK(bytes(data("$TA(3)\n", "\tDB '\tC'\n")) == "202043");

    // Out of range or not a number: A19, and each tab is one space.
    r = data("$TABS(17)\n", "\tDB '\tC'\n");
    CHECK(all_diags(r) == "2:A19E");
    CHECK(bytes(r) == "2043");
    CHECK(all_diags(data("$TABS(0)\n", "")) == "2:A19E");
    CHECK(all_diags(data("$TABS(X)\n", "")) == "2:A19E");
    // No '(': A22, and each tab is one space.
    r = data("$TABS\n", "\tDB '\tC'\n");
    CHECK(all_diags(r) == "2:A22E");
    CHECK(bytes(r) == "2043");
    // More than a number: A22, but the number is used.
    r = data("$TABS(7+1)\n", "\tDB '\tC'\n");
    CHECK(all_diags(r) == "2:A22E");
    CHECK(bytes(r) == "20202043");
    // $TABS is a primary control.
    CHECK(all_diags(data("", "$TABS(4)\n")) == "3:A8E");
}

TEST_CASE("respecified primary controls: A7") {
    // The control is still used, but the rest of its line is not.
    auto r = data("$CASE\n$NOCASE TABS(3)\nabc EQU 1\nABC EQU 2\n", "\tDB '\tC'\n");
    CHECK(test::diags(r, 3) == "A7E");
    CHECK(test::diags(r, 5) != "");  // $NOCASE is used: ABC is abc
    CHECK(bytes(r) == "2020202043");  // $TABS(3) is not

    // The same setting again is fine, as is any number of $TABS.
    CHECK(all_diags(test::run("$SYMBOLS\n$SYMBOLS\n$TABS(4)\n$TABS(5)\n END\n")) == "");
    CHECK(all_diags(test::run("$SYMBOLS\n$NOSYMBOLS\n$SYMBOLS\n END\n")) == "2:A7E 3:A7E");
    CHECK(all_diags(test::run("$MPL\n$NOMACRO\n END\n")) == "");
    CHECK(all_diags(test::run("$MACRO\n$NOMPL\n END\n")) == "");
    CHECK(all_diags(test::run("$MACRO\n$NOMACRO\n END\n")) == "2:A7E");

    // $MOD166 and $MODV2 may follow $MOD167, but not each other, and
    // $MOD167 may not follow either.
    CHECK(all_diags(test::run("$MOD167\n$MOD166\n END\n")) == "");
    CHECK(all_diags(test::run("$MOD166\n$MOD167\n END\n")) == "2:A7E");
    CHECK(all_diags(test::run("$MODV2\n$MOD166\n END\n")) == "1:OC2W 2:A7E");
    CHECK(all_diags(test::run("$MOD166\n$NOMOD166\n END\n")) == "");
}
