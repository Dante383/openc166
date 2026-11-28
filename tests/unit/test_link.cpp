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

TEST_CASE("an unresolved external that differs only in case says so") {
    // A C module ($CASE) and an assembly module without it: the assembly's
    // names reach the linker in upper case, the C module's as written.
    auto r = link({{"c.a66",
                    "$MOD167\n$CASE\nEXTERN Sat_Add:NEAR\nPUBLIC c_func\n"
                    "P SECTION CODE\nc_func PROC NEAR\n CALL Sat_Add\n RET\nc_func ENDP\nP ENDS\nEND\n"},
                   {"asm.a66",
                    "$MOD167\nEXTERN c_func:NEAR\nPUBLIC Sat_Add\n"
                    "Q SECTION CODE\nSat_Add PROC NEAR\n CALL c_func\n RET\nSat_Add ENDP\nQ ENDS\nEND\n"}});
    CHECK(diags(r) == "c.a66:3:OC15E asm.a66:2:OC15E");
    REQUIRE(r.diagnostics.size() == 2);
    CHECK(r.diagnostics[0].message.find("asm.a66 makes 'SAT_ADD' public: a module without $CASE") !=
          std::string::npos);
    CHECK(r.diagnostics[0].message.find("spell it 'SAT_ADD' here") != std::string::npos);
    CHECK(r.diagnostics[1].message.find("c.a66 makes 'c_func' public with $CASE") != std::string::npos);
    CHECK(r.diagnostics[1].message.find("looks for 'C_FUNC'") != std::string::npos);
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

// Library modules: openc166's equivalent of the members of an L166 library.
namespace {

// A library module that defines 'name' (a NOP, RET procedure) and, if
// 'needs' is not empty, calls that external.
Source lib_module(const std::string& file, const std::string& name, const std::string& needs = "") {
    std::string text = " PUBLIC " + name + "\n";
    if (!needs.empty()) text += " EXTERN " + needs + ":NEAR\n";
    text += "RT SECTION CODE PUBLIC\n" + name + " PROC NEAR\n";
    text += needs.empty() ? " NOP\n" : " CALL " + needs + "\n";
    text += " RET\n" + name + " ENDP\nRT ENDS\n END\n";
    return {file, text};
}

const Source kCaller{"main.a66",
                     " EXTERN MUL32:NEAR\nRT SECTION CODE PUBLIC\nMAIN PROC NEAR\n CALL MUL32\n RET\nMAIN ENDP\nRT ENDS\n END\n"};

}  // namespace

TEST_CASE("library modules are linked only when a name they define is needed") {
    AssembleOptions o;
    o.libraries = {lib_module("div.a66", "DIV32"), lib_module("mul.a66", "MUL32", "HELP"),
                   lib_module("help.a66", "HELP")};
    auto r = link({kCaller}, o);
    REQUIRE(r.ok());
    // MUL32 pulls in HELP; DIV32 is not needed. Library modules follow in
    // the order given.
    CHECK(r.modules == std::vector<std::string>{"main.a66", "mul.a66", "help.a66"});
    CHECK(r.library_modules == 2);
    // MAIN: CALLA MUL32, RET; MUL32: CALLA HELP, RET; HELP: NOP, RET.
    CHECK(image(r) == "CA000600CB00CA000C00CB00CC00CB00");
}

TEST_CASE("a library module is not linked for a name that a module defines") {
    AssembleOptions o;
    o.libraries = {lib_module("mul.a66", "MUL32")};
    auto r = link({kCaller, lib_module("mine.a66", "MUL32")}, o);
    REQUIRE(r.ok());
    CHECK(r.modules.size() == 2);
    CHECK(r.library_modules == 0);
    // Nothing needs the library at all.
    r = link({lib_module("mine.a66", "OTHER")}, o);
    REQUIRE(r.ok());
    CHECK(r.modules == std::vector<std::string>{"mine.a66"});
}

TEST_CASE("the first library module that defines a name is used") {
    AssembleOptions o;
    o.libraries = {lib_module("a.a66", "MUL32"), lib_module("b.a66", "MUL32")};
    auto r = link({kCaller}, o);
    REQUIRE(r.ok());
    CHECK(r.modules == std::vector<std::string>{"main.a66", "a.a66"});
}

TEST_CASE("errors in library modules that are not linked do not count") {
    AssembleOptions o;
    o.libraries = {{"bad.a66", " PUBLIC X\nX EQU 1\n NOT AN INSTRUCTION\n END\n"}, lib_module("mul.a66", "MUL32")};
    auto r = link({kCaller}, o);
    CHECK(r.ok());
    // An external that no library module defines is still OC15.
    r = link({kCaller}, AssembleOptions{});
    CHECK(diags(r) == "main.a66:1:OC15E");
}

TEST_CASE("a single source can use library modules") {
    AssembleOptions o;
    o.file_name = "main.a66";
    o.libraries = {lib_module("mul.a66", "MUL32")};
    auto r = assemble(kCaller.text, o);
    REQUIRE(r.ok());
    CHECK(r.modules == std::vector<std::string>{"main.a66", "mul.a66"});
}

// openc166's own symbols for where sections and classes went.
TEST_CASE("the linker makes symbols for where sections and classes went") {
    AssembleOptions o;
    o.class_ranges = {{"NDATA", {0xC000, 0xDFFF}}};
    auto r = link({{"start.a66",
                    "$CASE\n"
                    " EXTERN ?SECTION_START??BSS:WORD, ?SECTION_END??BSS:WORD, ?SECTION_SIZE??BSS:DATA16\n"
                    " EXTERN ?CLASS_START?ndata:DATA16, ?CLASS_END?NDATA:DATA16, ?CLASS_SIZE?NDATA:DATA16\n"
                    "CS SECTION CODE AT 0\n"
                    " MOV R4, #POF ?SECTION_START??BSS\n"
                    " MOV R5, #POF ?SECTION_END??BSS\n"
                    " MOV R6, #?SECTION_SIZE??BSS\n"
                    " MOV R7, #?CLASS_START?ndata\n"
                    " MOV R8, #?CLASS_END?NDATA\n"
                    " MOV R9, #?CLASS_SIZE?NDATA\n"
                    "CS ENDS\n"
                    "?BSS SECTION DATA PUBLIC 'NDATA'\n DSW 3\n?BSS ENDS\n END\n"},
                   {"vars.a66",
                    "$CASE\n?BSS SECTION DATA PUBLIC 'NDATA'\n DSW 5\n?BSS ENDS\n"
                    "D2 SECTION DATA 'ndata'\n DSB 3\nD2 ENDS\n END\n"}},
                  o);
    CHECK(diags(r) == "");
    REQUIRE(r.ok());
    // ?BSS: both parts, 10H bytes at 0C000H; class NDATA: ?BSS and D2.
    CHECK(image(r).substr(0, 48) == "E6F40000E6F51000E6F61000E6F700C0E6F813C0E6F91300");
}

TEST_CASE("linker symbols without $CASE and for a module's own section") {
    AssembleOptions o;
    o.origin = 0x100;
    auto r = link({{"a.a66", " EXTERN ?section_start?p:DATA16\np SECTION CODE\n MOV R4, #?section_start?p\np ENDS\n END\n"}},
                  o);
    REQUIRE(r.ok());
    CHECK(image(r) == "E6F40001");
}

TEST_CASE("linker symbols for sections and classes that do not exist") {
    auto r = link({{"a.a66", " EXTERN ?SECTION_SIZE?NOPE:DATA16, ?CLASS_START?NONE:DATA16, OTHER:DATA16\n END\n"}});
    CHECK(diags(r) == "a.a66:1:OC15E a.a66:1:OC15E a.a66:1:OC15E");
    CHECK(r.diagnostics[0].message.find("there is no section 'NOPE'") != std::string::npos);
    CHECK(r.diagnostics[1].message.find("no section has class 'NONE'") != std::string::npos);
    CHECK(r.diagnostics[2].message.find("no other module") != std::string::npos);
}

// NOF and --dpp: an openc166 extension, like the SND control of TASKING's
// locator, so there is no Keil reference.
TEST_CASE("NOF gives the near address through the DPP that holds the page") {
    const std::string src =
        "$SEGMENTED\n$MOD167\n"
        "NC SECTION DATA WORD PUBLIC 'CNEARROM'\ntab: DB 1,2,3,4\nNC ENDS\n"
        "ND SECTION DATA WORD PUBLIC 'CNEAR'\nvar: DSW 1\nND ENDS\n"
        "PR SECTION CODE WORD PUBLIC 'CPROGRAM'\n"
        "f PROC FAR\n MOVB RL1, [R1+#NOF tab]\n MOV R4, NOF var\n MOV R5, #NOF tab\n RETS\nf ENDP\nPR ENDS\n"
        "IN SECTION CONST WORD PUBLIC 'CNEARROM'\n DW NOF tab, NOF var, NOF 0FD7AH\nIN ENDS\nEND\n";
    AssembleOptions o;
    o.class_ranges = {{"CNEARROM", {0x94000, 0x97FFF}}, {"CNEAR", {0xC000, 0xEFFF}}, {"CPROGRAM", {0, 0x7FFF}}};
    SUBCASE("with the DPPs of the program") {
        o.dpps = {0x24, 0x28, 0x25, 3};
        o.dpps_given = true;
        auto r = link({{"t.a66", src}}, o);
        REQUIRE(r.ok());
        CHECK(symbol(r, "tab", 0)->value.num == 0x94000);
        CHECK(symbol(r, "var", 0)->value.num == 0xC000);
        // tab through DPP2 (25H): 8000H; var through DPP3: C000H.
        CHECK(image(r).substr(0, 28) == "F4210080F2F400C0E6F50080DB00");
        const SectionInfo* in = section(r, "IN");
        REQUIRE(in);
        CHECK((in->address >> 14) == 0x25);  // with --dpp, CONST goes in a page a DPP holds
    }
    SUBCASE("with DPPn = n, page 25H has no near address") {
        auto r = link({{"t.a66", src}}, o);
        CHECK(diags(r) == "t.a66:11:OC38E t.a66:13:OC38E t.a66:18:OC38E");
    }
    SUBCASE("with --dpp, a DATA section goes only in pages a DPP holds") {
        o.class_ranges = {{"CNEARROM", {0xB0000, 0xB3FFF}}, {"CNEAR", {0xC000, 0xEFFF}}, {"CPROGRAM", {0, 0x7FFF}}};
        o.dpps = {0x24, 0x28, 0x25, 3};
        o.dpps_given = true;
        auto r = link({{"t.a66", src}}, o);
        CHECK_FALSE(r.ok());
        CHECK(diags(r).find("OC12E") != std::string::npos);
    }
}
