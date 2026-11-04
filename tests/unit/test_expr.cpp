// SPDX-License-Identifier: MIT
#include "diag.hpp"
#include "doctest.h"
#include "expr.hpp"
#include "lexer.hpp"
#include "predef.hpp"

#include <cstdio>
#include <map>
#include <string>

using namespace oc166;

namespace {

struct Context : SymbolContext {
    std::map<std::string, Value> symbols;
    bool final = true;
    bool fixed = false;

    std::optional<Value> lookup(std::string_view name) override {
        if (auto p = predefined(name, isa::Cpu::C167, false)) return p;
        auto it = symbols.find(std::string(name));
        if (it == symbols.end()) return std::nullopt;
        return it->second;
    }
    std::int64_t location() const override { return 0xC000; }
    bool location_fixed() const override { return fixed; }
    bool final_pass() const override { return final; }
};

Value eval(std::string_view text, Context& ctx) {
    auto tokens = tokenize(text);
    TokenCursor cur(tokens);
    Value v = evaluate(cur, ctx);
    CHECK(cur.at_end());
    return v;
}

std::int64_t num(std::string_view text) {
    Context ctx;
    Value v = eval(text, ctx);
    CHECK(v.type == VType::Number);
    CHECK(v.known);
    return v.num;
}

std::string error_code(std::string_view text, Context& ctx) {
    try {
        eval(text, ctx);
    } catch (const AsmError& e) {
        return e.code();
    }
    return "";
}

std::string error_code(std::string_view text) {
    Context ctx;
    return error_code(text, ctx);
}

// "off.bit" of a bit address, e.g. "01.1", "88.3"
std::string bit(std::string_view text) {
    Context ctx;
    Value v = eval(text, ctx);
    REQUIRE(v.type == VType::Bit);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02X.%d%s", static_cast<unsigned>(v.num), v.bit, v.ext ? " ext" : "");
    return buf;
}

}  // namespace

TEST_CASE("arithmetic is 64-bit with C-like division") {
    CHECK(num("1 SHL 32") == 0x100000000);
    CHECK(num("(1 SHL 32) SHR 32") == 1);
    CHECK(num("-1 SHR 60") == 0xF);  // logical shift
    CHECK(num("-7 / 2") == -3);
    CHECK(num("-7 MOD 4") == -3);
    CHECK(num("7 MOD -4") == 3);
    CHECK(num("10 / 3") == 3);
    CHECK(error_code("1 / 0") == "A24");
    CHECK(error_code("1 MOD 0") == "A24");
}

TEST_CASE("Keil operator precedence") {
    CHECK(num("3 + 4 * 5") == 23);
    CHECK(num("(3 + 4) * 5") == 35);
    CHECK(num("2 + 3 SHL 1") == 10);    // + binds tighter than SHL
    CHECK(num("1 OR 2 AND 3") == 3);    // AND/OR/XOR share one level
    CHECK(num("1 AND 2 OR 3") == 3);
    CHECK(num("NOT 1 + 1") == -1);      // unary operators bind tightest
    CHECK(num("LOW 1234H + 1") == 0x35);
    CHECK(num("HIGH (1234H + 100H)") == 0x13);
    CHECK(num("1 + 2 EQ 3") == 1);      // comparisons are lowest
}

TEST_CASE("comparisons: signed and unsigned") {
    CHECK(num("-1 GT 0") == 0);
    CHECK(num("-1 UGT 0") == 1);
    CHECK(num("-1 LT 0") == 1);
    CHECK(num("-1 ULT 0") == 0);
    CHECK(num("5 >= 5") == 1);
    CHECK(num("1 <> 2") == 1);
    CHECK(num("1 != 1") == 0);
}

TEST_CASE("byte, word and memory operators") {
    CHECK(num("SEG 12345H") == 1);
    CHECK(num("PAG 12345H") == 4);
    CHECK(num("SOF 12345H") == 0x2345);
    CHECK(num("POF 12345H") == 0x2345);
    CHECK(num("BYTE1 12345678H") == 0x56);
    CHECK(num("WORD2 12345678H") == 0x1234);
    CHECK(num("WORD6 123456789ABCDEF0H") == 0x1234);
    CHECK(num("BYTE7 123456789ABCDEF0H") == 0x12);
    // WORD3, BYTE8 and the like are ordinary names, as in Keil.
    CHECK(error_code("WORD3 12345678H") == "A42");
    CHECK(error_code("BYTE8") == "A42");
    CHECK(num("0F0H & 3CH") == 0x30);
    CHECK(num("0FFH ^ 0AAH") == 0x55);
}

TEST_CASE("character constants") {
    CHECK(num("'A'") == 0x41);
    CHECK(num("'AB'") == 0x4142);
    CHECK(num("'''A'") == 0x2741);
    CHECK(error_code("'ABC'") == "A41");
}

TEST_CASE("symbols and the location counter") {
    Context ctx;
    ctx.symbols["X"] = Value::number(0x10);
    CHECK(eval("X * 2", ctx).num == 0x20);
    CHECK(eval("$", ctx).num == 0xC000);
    CHECK(error_code("NOWHERE") == "A42");
    ctx.final = false;
    CHECK_FALSE(eval("NOWHERE + 1", ctx).known);
}

TEST_CASE("relocatable values") {
    Context ctx;
    Value l0 = Value::number(0xC000), l2 = Value::number(0xC002);
    l0.rel = l2.rel = Rel::Addr;
    l0.unsig = l2.unsig = true;
    ctx.symbols["L0"] = l0;
    ctx.symbols["L2"] = l2;
    CHECK(eval("$", ctx).rel == Rel::Addr);
    CHECK(eval("L2 + 2", ctx).rel == Rel::Addr);
    CHECK(eval("2 + L2", ctx).rel == Rel::Addr);
    CHECK(eval("L2 - SEG L2", ctx).rel == Rel::Addr);
    CHECK_FALSE(eval("LOW L2", ctx).relocatable());
    CHECK_FALSE(eval("SOF L2", ctx).relocatable());
    CHECK_FALSE(eval("L2 * 1", ctx).relocatable());

    // Every SEG is relocatable unless something non-zero is added to it.
    CHECK(eval("SEG 5", ctx).rel == Rel::Seg);
    CHECK(eval("SEG L2 + 0", ctx).rel == Rel::Seg);
    CHECK(eval("+SEG L2", ctx).rel == Rel::Seg);
    CHECK(eval("SEG L2 + 1", ctx).rel == Rel::None);
    CHECK(eval("0 + SEG L2", ctx).rel == Rel::None);
    CHECK(eval("-SEG L2", ctx).rel == Rel::None);
    CHECK(eval("SEG L2 OR 0", ctx).rel == Rel::None);
}

TEST_CASE("label plus a number in a fixed-address section") {
    Context ctx;
    ctx.fixed = true;
    Value l0 = Value::number(0xC000), l2 = Value::number(0xC002);
    l0.rel = l2.rel = Rel::Addr;
    l0.unsig = l2.unsig = true;
    l0.fixed = l2.fixed = true;
    ctx.symbols["L0"] = l0;
    ctx.symbols["L2"] = l2;
    for (const char* text : {"L2 + 1", "1 + L2", "L2 - 1", "L2 + 1 - 1", "L2 + (L2 - L0)", "(L2 - L0) + L2",
                             "L2 - (L2 - L0)", "$ + 2"})
        CHECK_MESSAGE(eval(text, ctx).rel == Rel::Diff, text);
    for (const char* text : {"L2", "L2 + 0", "0 + L2", "L2 + (1 - 1)", "L2 + (L0 - L0)", "L2 - SEG L2", "$"})
        CHECK_MESSAGE(eval(text, ctx).rel == Rel::Addr, text);
    CHECK(eval("L2 - 1", ctx).num == 0xC001);
    CHECK(eval("(L2 + 1) - L0", ctx).num == 3);
    CHECK(eval("(L2 - 0C003H) GT 0", ctx).num == 1);  // still unsigned
    CHECK_FALSE(fits_width(eval("L2 - 0C003H", ctx), 16));
    CHECK(error_code("-(L2 - 1)", ctx) == "A63");
    CHECK(error_code("NOT (L2 + 1)", ctx) == "A63");

    // Labels in relocatable sections stay relocatable.
    l2.fixed = false;
    ctx.symbols["L2"] = l2;
    CHECK(eval("L2 + 1", ctx).rel == Rel::Addr);
    CHECK(eval("L2 + (L2 - L0)", ctx).rel == Rel::Addr);
}

TEST_CASE("label differences are unsigned") {
    Context ctx;
    Value l0 = Value::number(0xC000), l2 = Value::number(0xC002);
    l0.rel = l2.rel = Rel::Addr;
    l0.unsig = l2.unsig = true;
    ctx.symbols["L0"] = l0;
    ctx.symbols["L2"] = l2;
    Value d = eval("L0 - L2", ctx);
    CHECK(d.rel == Rel::Diff);
    CHECK(d.unsig);
    CHECK(d.num == 0x1FFFFFFFFFFFFFFE);  // Keil works modulo 2^61
    CHECK_FALSE(fits_width(d, 16));
    CHECK_FALSE(fits_width(d, 32));
    CHECK(fits_width(eval("(L0 - L2) AND 0FFFFH", ctx), 16));
    CHECK(fits_width(eval("1 + (L0 - L2)", ctx), 16));    // signed: left operand decides
    CHECK(fits_width(eval("1 - (L2 - L0)", ctx), 16));
    CHECK_FALSE(fits_width(eval("(L2 - L0) - 3", ctx), 16));
    CHECK_FALSE(fits_width(eval("3 * (L0 - L2)", ctx), 16));
    CHECK(fits_width(eval("-((L2 - L0) * 1)", ctx), 16));
    CHECK_FALSE(fits_width(eval("L2 - 0D000H", ctx), 16));
    CHECK(eval("(L0 - L2) GT 0", ctx).num == 1);
    CHECK(eval("(L0 - L2) LT 1", ctx).num == 0);
    CHECK(eval("(L0 - L2) MOD 7", ctx).num == 0);
    CHECK(eval("(L0 - L2) / 4", ctx).num == 0x07FFFFFFFFFFFFFF);
    CHECK(eval("(L0 - L2) SHR 48", ctx).num == 0x1FFF);
    CHECK(eval("(L0 - L2) + 10000H", ctx).num == 0xFFFE);
    CHECK(eval("(L0 - L2) + L2", ctx).num == 0xC000);
    CHECK(eval("1 + (L0 - L2)", ctx).num == -1);
    CHECK_FALSE(fits_width(eval("-((L0 - L2) / 1)", ctx), 16));
    CHECK(eval("(0 - (L2 - L0)) / 2", ctx).num == -1);
    CHECK(eval("(L2 - L0) - L0", ctx).unsig);
    CHECK(eval("L2 + (L0 - L2)", ctx).rel == Rel::Addr);
}

TEST_CASE("negated addresses are refused") {
    Context ctx;
    Value l0 = Value::number(0xC000), l2 = Value::number(0xC002);
    l0.rel = l2.rel = Rel::Addr;
    l0.unsig = l2.unsig = true;
    ctx.symbols["L0"] = l0;
    ctx.symbols["L2"] = l2;
    for (const char* text : {"-L2", "NOT L2", "-$", "-(L2 - L0)", "NOT (L2 - L0)", "-((L2 - L0) - 3)", "0 - L2",
                             "10H - L2"})
        CHECK_MESSAGE(error_code(text, ctx) == "A63", text);
    CHECK(eval("-((L2 - L0) / 1)", ctx).num == -2);
    CHECK(eval("0 - (L2 - L0)", ctx).num == -2);
    CHECK(eval("-(1 + (L2 - L0))", ctx).num == -3);
    CHECK(eval("(L2 - L0) - L0", ctx).num == 2 - 0xC000);
}

TEST_CASE("bit addresses") {
    CHECK(bit("PSW.3") == "88.3");
    CHECK(bit("R1.15") == "F1.15");
    CHECK(bit("0FD00H.0") == "00.0");
    CHECK(bit("0FD01H.3") == "00.11");   // high byte of word 0FD00H
    CHECK(bit("0FD00H.17") == "01.1");   // positions carry into the next word
    CHECK(bit("0FE00H.-1") == "7F.15");
    CHECK(bit("0FF00H.0") == "80.0");
    CHECK(bit("80H.16") == "81.0");      // raw bitoff numbers count in words
    CHECK(bit("0F100H.2") == "80.2 ext");
    CHECK(bit("0FD00H.1 + 1") == "00.2");
    CHECK(bit("0FD00H.15 + 1") == "01.0");
    CHECK(error_code("R1.16") == "A67");
    CHECK(error_code("PSW.16") == "A67");
    CHECK(error_code("1000H.0") == "A69");
    CHECK(error_code("0F0H.0") == "A69");
    CHECK(error_code("0FE00H.0") == "A103");  // Keil silently encodes bitoff 0
    CHECK(error_code("10H.0") == "A103");
    CHECK(error_code("1 + PSW.1") == "A61");
}
