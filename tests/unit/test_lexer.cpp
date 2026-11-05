// SPDX-License-Identifier: MIT
#include "diag.hpp"
#include "doctest.h"
#include "lexer.hpp"

#include <string>

using namespace oc166;

namespace {

std::uint64_t number(std::string_view text) {
    auto t = tokenize(text);
    REQUIRE(t.size() == 2);
    REQUIRE(t[0].kind == TokKind::Number);
    return t[0].num;
}

std::string error_code(std::string_view text) {
    try {
        tokenize(text);
    } catch (const AsmError& e) {
        return e.code();
    }
    return "";
}

}  // namespace

TEST_CASE("numbers in every radix") {
    CHECK(number("1234") == 1234);
    CHECK(number("1234D") == 1234);
    CHECK(number("0FFH") == 0xFF);
    CHECK(number("0ffh") == 0xFF);
    CHECK(number("1010B") == 10);
    CHECK(number("1$000B") == 8);
    CHECK(number("17Q") == 15);
    CHECK(number("17O") == 15);
    CHECK(number("0x1F") == 0x1F);
}

TEST_CASE("number errors") {
    CHECK(error_code("12AB") == "A1");
    CHECK(error_code("102B") == "A1");
    CHECK(error_code("1.5") == "A70");  // Keil reads this as floating point
}

TEST_CASE("tokens, columns and comments") {
    auto t = tokenize("LBL:  MOV  R1, #'A' ; comment");
    REQUIRE(t.size() == 8);
    CHECK(t[0].kind == TokKind::Ident);
    CHECK(t[0].text == "LBL");
    CHECK(t[0].column == 1);
    CHECK(t[1].is_op(":"));
    CHECK(t[2].text == "MOV");
    CHECK(t[2].column == 7);
    CHECK(t[5].is_op("#"));
    CHECK(t[6].kind == TokKind::String);
    CHECK(t[7].kind == TokKind::End);
}

TEST_CASE("multi-character operators") {
    auto t = tokenize("<= >= <> != == << >>");
    std::string ops;
    for (const auto& k : t)
        if (k.kind == TokKind::Op) ops += std::string(k.text) + " ";
    CHECK(ops == "<= >= <> != == << >> ");
}

TEST_CASE("strings") {
    auto t = tokenize("'It''s'");
    REQUIRE(t[0].kind == TokKind::String);
    auto b = string_bytes(t[0]);
    CHECK(std::string(b.begin(), b.end()) == "It's");
    CHECK(error_code("'open") == "A2");
    CHECK(error_code("MOV R1, @R2") == "A6");
}
