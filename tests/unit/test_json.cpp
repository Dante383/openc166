// SPDX-License-Identifier: MIT
#include "doctest.h"

#include "json.hpp"

using oc166::json::parse;

TEST_CASE("json: values, comments and escapes") {
    auto v = parse(R"({
        // a comment
        "name": "5WY19 é\n",
        "clock": 24000000, /* another */ "ratio": 1.5,
        "list": [true, false, null, -3],
        "nested": {"a": {}}
    })");
    CHECK(v.find("name")->as_string() == "5WY19 \xc3\xa9\n");
    CHECK(v.find("clock")->as_int() == 24000000);
    CHECK(v.find("ratio")->as_number() == 1.5);
    const auto& list = v.find("list")->as_array();
    REQUIRE(list.size() == 4);
    CHECK(list[0].as_bool());
    CHECK(list[2].is_null());
    CHECK(list[3].as_int() == -3);
    CHECK(v.find("nested")->find("a")->as_object().empty());
    CHECK(v.find("missing") == nullptr);
    CHECK(v.as_object()[1].first == "clock");  // file order
}

TEST_CASE("json: errors say where") {
    CHECK_THROWS_WITH(parse("{\n  \"a\": 1,\n}"), "line 3, column 1: a comma before '}'");
    CHECK_THROWS_WITH(parse("{\"a\": tru}"), "line 1, column 7: unexpected character 't'");
    CHECK_THROWS_WITH(parse("{\"a\": 1, \"a\": 2}"), "line 1, column 13: duplicate member \"a\"");
    CHECK_THROWS_WITH(parse("[1] 2"), "line 1, column 5: unexpected text after the end of the document");
    CHECK_THROWS_WITH(parse("\"abc"), "line 1, column 5: unterminated string");
    CHECK_THROWS_WITH(parse("{\"a\": 1.5}").find("a")->as_int(), "line 1, column 7: expected a whole number");
}
