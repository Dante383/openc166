// SPDX-License-Identifier: MIT
// Several modules linked into one program: EXTERN and PUBLIC, combining
// sections, and the L166 errors (OC15..OC21). Expected values match Keil
// A166 and L166 unless a comment says otherwise.
#include "doctest.h"
#include "helpers.hpp"

#include <map>

using namespace oc166;

namespace {

AssemblyResult link(std::vector<Source> sources, AssembleOptions o = {}) {
    return assemble(sources, o);
}

std::string image(const AssemblyResult& r) { return test::hex(flat_image(r).bytes); }

// The diagnostics as "file:line:CODE" plus E or W, separated by spaces.
std::string diags(const AssemblyResult& r) {
    std::string s;
    for (const auto& d : r.diagnostics) {
        if (!s.empty()) s += ' ';
        s += d.file + ':' + std::to_string(d.line) + ':' + d.code + (d.severity == Severity::Error ? "E" : "W");
    }
    return s;
}

const Symbol* symbol(const AssemblyResult& r, const std::string& name, int module) {
    for (const auto& s : r.symbols)
        if (s.name == name && s.module == module) return &s;
    return nullptr;
}

const SectionInfo* section(const AssemblyResult& r, const std::string& name) {
    for (const auto& s : r.sections)
        if (s.name == name) return &s;
    return nullptr;
}

}  // namespace

TEST_CASE("an EXTERN takes the value of a PUBLIC symbol in another module") {
    auto r = link({{"main.a66",
                    "$MOD167\n"
                    "EXTERN Delay:NEAR, Limit:DATA16, Count:WORD, Step:DATA8\n"
                    "P SECTION CODE PUBLIC\n"
                    "Main PROC NEAR\n CALL Delay\n MOV R1, #Limit\n MOV Count, R1\n MOVB RL2, #Step\n RET\n"
                    "Main ENDP\nP ENDS\nEND\n"},
                   {"lib.a66",
                    "$MOD167\n"
                    "PUBLIC Delay, Limit, Count, Step\n"
                    "Limit EQU 1234H\nStep EQU -1\n"
                    "P SECTION CODE PUBLIC\nDelay PROC NEAR\n RET\nDelay ENDP\nP ENDS\n"
                    "Vars SECTION DATA AT 8000H\nCount: DSW 1\nVars ENDS\nEND\n"}});
    REQUIRE(r.ok());
    // The two parts of P follow each other: Delay is at 12H.
    CHECK(image(r) == "CA001200E6F13412F6F10080E7F4FF00CB00CB00");
    REQUIRE(r.modules == std::vector<std::string>{"main.a66", "lib.a66"});
    const Symbol* ext = symbol(r, "Delay", 0);
    const Symbol* pub = symbol(r, "Delay", 1);
    REQUIRE(ext);
    REQUIRE(pub);
    CHECK(ext->kind == SymbolKind::Extern);
    CHECK(ext->text == "NEAR");
    CHECK_FALSE(ext->exported);
    CHECK(pub->kind == SymbolKind::Label);
    CHECK(pub->exported);
    CHECK(pub->value.num == 0x12);
    REQUIRE(section(r, "P"));
    CHECK(section(r, "P")->size == 0x14);
    CHECK(r.sections.size() == 2);
}

TEST_CASE("COMMON parts overlay each other; PUBLIC parts follow each other, aligned") {
    auto r = link({{"a.a66",
                    "$MOD167\n"
                    "Cm SECTION CONST WORD COMMON\n DW 1111H, 2222H\nCm ENDS\n"
                    "D SECTION CONST BYTE PUBLIC\n DB 1\nD ENDS\nEND\n"},
                   {"b.a66",
                    "$MOD167\n"
                    "Cm SECTION CONST WORD COMMON\n DW 3333H\nCm ENDS\n"
                    "D SECTION CONST WORD PUBLIC\n DW 4444H\nD ENDS\nEND\n"}});
    REQUIRE(r.ok());
    // The later module's bytes win where COMMON parts overlap.
    CHECK(image(r) == "3333222201FF4444");
    CHECK(section(r, "Cm")->size == 4);
    CHECK(section(r, "D")->size == 4);
}

TEST_CASE("unresolved, duplicate and slightly different externals") {
    auto r = link({{"main.a66",
                    "$MOD167\n"
                    "EXTERN Missing:NEAR, NeverUsed:WORD, Number:NEAR, Dup:NEAR\n"
                    "P SECTION CODE PUBLIC\n"
                    "Main PROC NEAR\n CALL Missing\n CALL Number\n CALL Dup\n RET\nMain ENDP\nP ENDS\nEND\n"},
                   {"one.a66",
                    "$MOD167\nPUBLIC Dup, Number\nNumber EQU 1234H\n"
                    "P SECTION CODE PUBLIC\nDup PROC NEAR\n RET\nDup ENDP\nP ENDS\nEND\n"},
                   {"two.a66",
                    "$MOD167\nPUBLIC Dup\n"
                    "Q SECTION CODE PUBLIC\nDup PROC NEAR\n RET\nDup ENDP\nQ ENDS\nEND\n"}});
    // L127 for each missing symbol, used or not (OC15); L21, a warning, for
    // a NEAR external that is a number (OC17); L104 for the second PUBLIC
    // (OC16).
    CHECK(diags(r) == "main.a66:2:OC15E main.a66:2:OC15E main.a66:2:OC17W two.a66:2:OC16E");
    CHECK_FALSE(r.ok());
}

TEST_CASE("an external value that does not fit is reported at each use") {
    auto r = link({{"main.a66",
                    "$MOD167\n"
                    "EXTERN Big:DATA8, NegOne:DATA8, Wide:DATA4, Flag:BIT\n"
                    "P SECTION CODE\nMain PROC NEAR\n"
                    " MOVB RL1, #Big\n"     // 5
                    " MOVB RL2, #NegOne\n"  // 6
                    " SHL R1, #Wide\n"      // 7
                    " BSET Flag\n"          // 8
                    " MOVB RL1, #Big\n"     // 9
                    " RET\nMain ENDP\nP ENDS\n"
                    "T SECTION CONST\n"
                    " DB Big, NegOne\n"  // 14
                    " DW Big\n"          // 15
                    "T ENDS\nEND\n"},
                   {"lib.a66",
                    "$MOD167\nPUBLIC Big, NegOne, Wide, Flag\n"
                    "Big EQU 200H\nNegOne EQU -1\nWide EQU 15\n"
                    "Vars SECTION DATA\nFlag: DSW 1\nVars ENDS\nEND\n"}});
    // L121 at each use that does not fit, never at the declaration.
    CHECK(diags(r) == "main.a66:5:OC18E main.a66:8:OC18E main.a66:9:OC18E main.a66:14:OC18E");
}

TEST_CASE("each part of a section is checked against the first part only") {
    auto r = link({{"a.a66", "$MOD167\nX SECTION CONST\n DW 1\nX ENDS\nY SECTION CONST PUBLIC\n DW 2\nY ENDS\nEND\n"},
                   {"b.a66", "$MOD167\nX SECTION CONST PUBLIC\n DW 3\nX ENDS\nY SECTION CONST\n DW 4\nY ENDS\nEND\n"},
                   {"c.a66",
                    "$MOD167\nX SECTION CONST PUBLIC\n DW 5\nX ENDS\nY SECTION CONST PUBLIC\n DW 6\nY ENDS\nEND\n"}});
    // L101 for both parts of X after the PRIVATE first one, and for the
    // PRIVATE part of Y. The third part of Y still combines with the first.
    CHECK(diags(r) == "b.a66:2:OC19E b.a66:5:OC19E c.a66:2:OC19E");
}

TEST_CASE("parts of a section must have the same type and combine type") {
    auto r = link({{"a.a66",
                    "$MOD167\nK SECTION CODE PUBLIC\nKp PROC NEAR\n RET\nKp ENDP\nK ENDS\nS SECTION DATA COMMON\n DSW 1\nS ENDS\n"
                    "G SECTION CONST PUBLIC\n DW 2\nG ENDS\nEND\n"},
                   {"b.a66",
                    "$MOD167\nK SECTION CONST PUBLIC\n DW 3\nK ENDS\nS SECTION DATA PUBLIC\n DSW 1\nS ENDS\n"
                    "G SECTION CONST GLOBAL\n DW 5\nG ENDS\nEND\n"}});
    CHECK(diags(r) == "b.a66:2:OC19E b.a66:5:OC19E b.a66:8:OC19E");
}

TEST_CASE("the first module sets the CPU mode") {
    const char* flat = "$MOD167\nP SECTION CODE PUBLIC\nF PROC NEAR\n RET\nF ENDP\nP ENDS\nEND\n";
    const char* seg = "$SEGMENTED\n$MOD167\nP SECTION CODE PUBLIC\nS PROC NEAR\n NOP\nS ENDP\nP ENDS\nEND\n";
    auto r = link({{"seg.a66", seg}, {"flat.a66", flat}, {"seg2.a66", seg}});
    // L15, a warning, for each module whose mode differs from the first.
    CHECK(diags(r) == "flat.a66:0:OC20W");
    CHECK(r.ok());
    CHECK(image(r) == "CC00CB00CC00");
    CHECK(diags(link({{"flat.a66", flat}, {"seg.a66", seg}})) == "seg.a66:0:OC20W");
}

TEST_CASE("combined DATA parts must fit in a 16 KB page") {
    const char* big = "$MOD167\nBig SECTION DATA WORD PUBLIC\n DSB 2001H\nBig ENDS\nEND\n";
    // L129; a single part over 4000H gives it too.
    CHECK(diags(link({{"a.a66", big}, {"b.a66", big}})) == "a.a66:2:OC21E");
    CHECK(diags(link({{"a.a66", big}})).empty());
}

TEST_CASE("each module finds its include files from its own directory") {
    std::map<std::string, std::string> files{{"a/defs.inc", "Val EQU 1\n"}, {"b/defs.inc", "Val EQU 2\n"}};
    AssembleOptions o;
    o.read_file = [&](const std::string& path) -> std::optional<std::string> {
        auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return it->second;
    };
    const char* src = "$MOD167\n$INCLUDE (defs.inc)\nT SECTION CONST PUBLIC\n DW Val\nT ENDS\nEND\n";
    auto r = link({{"a/main.a66", src}, {"b/lib.a66", src}}, o);
    REQUIRE(r.ok());
    CHECK(image(r) == "01000200");
}
