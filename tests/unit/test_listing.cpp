// SPDX-License-Identifier: MIT
// Listing controls: $TITLE, $EJECT, $PAGING, $PAGELENGTH, $PAGEWIDTH,
// $SYMBOLS, $SYMLIST and $XREF.
#include "helpers.hpp"

#include <doctest.h>

#include <algorithm>

using oc166::AssemblyResult;

namespace {

// All diagnostic codes with their file lines, e.g. "2:A19E 5:A22E".
std::string all_diags(const AssemblyResult& r) {
    std::string s;
    for (const auto& d : r.diagnostics) {
        if (!s.empty()) s += ' ';
        s += std::to_string(d.line) + ':' + d.code + (d.severity == oc166::Severity::Error ? "E" : "W");
    }
    return s;
}

// 'head' (primary controls), then 'body' in a C167 code section at 0C000H.
AssemblyResult module(const std::string& head, const std::string& body) {
    return test::run(head + "S SECTION CODE AT 0C000H\nP PROC NEAR\n" + body + " RET\nP ENDP\nS ENDS\n END\n");
}

std::size_t count(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

std::string nops(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += " NOP\n";
    return s;
}

}  // namespace

TEST_CASE("no page breaks unless the source asks for them") {
    const std::string lst = oc166::format_listing(module("", nops(200)));
    CHECK(lst.find('\f') == std::string::npos);
    CHECK(lst.find("PAGE") == std::string::npos);
}

TEST_CASE("$EJECT starts a new page with a header") {
    auto r = module("", " NOP\n$EJECT\n NOP\n$TITLE(My Title)\n$NOLIST\n$EJECT\n$LIST\n NOP\n$EJ\n");
    CHECK(all_diags(r) == "");
    const std::string lst = oc166::format_listing(r);
    // Not for the $EJECT that $NOLIST hides. Like Keil, every header shows
    // the last title.
    CHECK(count(lst, "\f") == 2);
    CHECK(lst.find("$EJECT\n\fopenc166  My Title") != std::string::npos);
    CHECK(lst.find("PAGE 2\n\n") != std::string::npos);
    CHECK(lst.find("PAGE 3\n\n") != std::string::npos);
    CHECK(lst.find("PAGE 4") == std::string::npos);
    // Without a title the header names the file.
    CHECK(oc166::format_listing(module("", "$EJECT\n")).find("\fopenc166  <input> ") != std::string::npos);
}

TEST_CASE("$PAGING and $PAGELENGTH break the listing into pages") {
    // 60 rows on a page by default, the header and the blank row after it
    // included.
    std::string lst = oc166::format_listing(module("$PAGING\n", nops(150)));
    CHECK(count(lst, "\f") >= 2);
    const std::size_t second = lst.find('\f'), third = lst.find('\f', second + 1);
    CHECK(std::count(lst.begin() + static_cast<long>(second), lst.begin() + static_cast<long>(third), '\n') == 60);
    CHECK(std::count(lst.begin(), lst.begin() + static_cast<long>(second), '\n') == 60);

    lst = oc166::format_listing(module("$PAGELENGTH(20)\n", nops(50)));
    const std::size_t a = lst.find('\f'), b = lst.find('\f', a + 1);
    CHECK(std::count(lst.begin() + static_cast<long>(a), lst.begin() + static_cast<long>(b), '\n') == 20);

    // $NOPAGING leaves only the $EJECT breaks.
    lst = oc166::format_listing(module("$NOPAGING PAGELENGTH(20)\n", nops(50) + "$EJECT\n"));
    CHECK(count(lst, "\f") == 1);
}

TEST_CASE("$PAGEWIDTH wraps long lines") {
    const std::string text = "; " + std::string(70, 'x') + " " + std::string(70, 'y') + " end";
    auto r = module("$PAGEWIDTH(72)\n", text + "\n$EJECT\n");
    CHECK(all_diags(r) == "");
    const std::string lst = oc166::format_listing(r);
    std::size_t longest = 0;
    for (std::size_t at = 0, nl; (nl = lst.find('\n', at)) != std::string::npos; at = nl + 1) {
        // The symbol and section tables are not wrapped.
        if (lst.compare(at, 7, "SYMBOLS") == 0) break;
        longest = std::max(longest, nl - at - (lst[at] == '\f'));
    }
    CHECK(longest == 72);
    // Continuation rows start in the source column.
    CHECK(lst.find("\n" + std::string(39, ' ') + "x") != std::string::npos);
    CHECK(lst.find(" end\n") != std::string::npos);
    // Without the control lines are left alone.
    CHECK(oc166::format_listing(module("", text + "\n")).find(text + "\n") != std::string::npos);
}

TEST_CASE("arguments of the listing controls") {
    CHECK(all_diags(module("$PAGELENGTH(17)\n$PL(18)\n$PL(65535)\n$PL(65536)\n$PL\n$PL()\n$PL(20 30)\n$PL(20H)\n", "")) ==
          "1:A19E 4:A19E 5:A22E 6:A19E 7:A22E");
    CHECK(all_diags(module("$PAGEWIDTH(71)\n$PW(72)\n$PW(132)\n$PW(133)\n$PW\n$PW(80.)\n", "")) ==
          "1:A19E 4:A19E 5:A22E 6:A19E");
    CHECK(all_diags(module("$DATE(x)\n$DATE()\n$DATE\n$TITLE(a(b)c)\n$TITLE()\n$TITLE\n", "")) == "3:A22E 6:A22E");
    CHECK(all_diags(module("$PAGING\n$NOPAGING\n$XREF\n$NOXREF\n", "")) == "2:A7E 4:A7E");
    // Primary controls are too late after the first statement.
    CHECK(all_diags(module("", "$PAGELENGTH(30)\n$PAGEWIDTH(80)\n$XREF\n$NOSYMBOLS\n$PAGING\n")) ==
          "3:A8E 4:A8E 5:A8E 6:A8E 7:A8E");
    // A control with an argument it should not have is A16, but still counts.
    auto r = module("$XREF(1)\n$NOPAGING(1)\n", "$EJECT(1)\n$SYMLIST(1)\n$NOLIST(1)\n NOP\n$LIST(1)\n");
    CHECK(all_diags(r) == "1:A16E 2:A16E 5:A16E 6:A16E 7:A16E 9:A16E");
    REQUIRE(r.listing_options.size() == 1);
    CHECK(r.listing_options[0].xref);
    const std::string lst = oc166::format_listing(r);
    CHECK(count(lst, "\f") == 1);
    CHECK(lst.find(" NOP") == std::string::npos);
}

TEST_CASE("$NOSYMBOLS and $NOSYMLIST leave symbols out of the table") {
    const std::string body = "A1 EQU 1\n$NOSYMLIST\nA2 EQU 2\nL2:\n$SYMLIST\nA3 EQU 3\n MOV R0, #A2\n";
    auto r = module("", body);
    CHECK(all_diags(r) == "");
    std::string lst = oc166::format_listing(r);
    const std::string table = lst.substr(lst.find("\nSYMBOLS\n"));
    CHECK(table.find("\nA1 ") != std::string::npos);
    CHECK(table.find("\nA2 ") == std::string::npos);
    CHECK(table.find("\nL2 ") == std::string::npos);
    CHECK(table.find("\nA3 ") != std::string::npos);
    // The result still has them all.
    CHECK(std::count_if(r.symbols.begin(), r.symbols.end(), [](const oc166::Symbol& s) { return !s.listed; }) == 2);

    lst = oc166::format_listing(module("$NOSYMBOLS\n", body));
    CHECK(lst.find("\nSYMBOLS\n") == std::string::npos);
    CHECK(lst.find("\nA1 ") == std::string::npos);
    CHECK(lst.find("\nSECTIONS\n") != std::string::npos);
    // As in Keil, $XREF brings the table back.
    lst = oc166::format_listing(module("$NOSYMBOLS XREF\n", body));
    CHECK(lst.find("\nSYMBOLS\n") != std::string::npos);
}

TEST_CASE("$XREF lists where symbols are defined and used") {
    auto r = module("$XREF\n",                 // 1; S and P are lines 2 and 3
                    "A1 EQU 1\n"               // 4
                    "L1: MOV R0, #A1\n"        // 5
                    " MOV R1, #A1 + A1\n"      // 6
                    " JMPR CC_UC, L1\n"        // 7
                    " JMPR CC_UC, L2\n"        // 8
                    "L2:\n"                    // 9
                    "A0 EQU 0\n");             // 10
    CHECK(all_diags(r) == "");
    const std::string lst = oc166::format_listing(r);
    CHECK(lst.find("00000001H  4# 5 6\n") != std::string::npos);
    CHECK(lst.find("NEAR  5# 7\n") != std::string::npos);
    CHECK(lst.find("NEAR  9# 8\n") != std::string::npos);
    CHECK(lst.find("00000000H  10#\n") != std::string::npos);
    // Nothing is recorded without the control.
    r = module("", "A1 EQU 1\n MOV R0, #A1\n");
    CHECK(std::all_of(r.symbols.begin(), r.symbols.end(), [](const oc166::Symbol& s) { return s.refs.empty(); }));
    CHECK(oc166::format_listing(r).find("H  4#") == std::string::npos);
}
