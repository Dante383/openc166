// SPDX-License-Identifier: MIT
// Intel HEX and S-record output.
#include "doctest.h"
#include "helpers.hpp"

#include <stdexcept>

using namespace oc166;
using test::run;

namespace {

AssemblyResult chunks(std::vector<Chunk> c) {
    AssemblyResult r;
    r.chunks = std::move(c);
    return r;
}

}  // namespace

TEST_CASE("Intel HEX matches OH166 H167 records") {
    // OH166 H167 writes the same records for this source.
    AssemblyResult r = run(
        "$MOD167\n"
        "$SEGMENTED\n"
        "D2 SECTION HDATA AT 1FFF0H\n"
        " DW 1,2,3,4,5,6,7,8\n"
        "D2 ENDS\n"
        "D3 SECTION HDATA AT 2FFFCH\n"
        " DW 0AAAAH, 0BBBBH\n"
        "D3 ENDS\n"
        " END\n");
    REQUIRE(r.ok());
    CHECK(format_intel_hex(r) ==
          ":020000040001F9\r\n"
          ":10FFF00001000200030004000500060007000800DD\r\n"
          ":020000040002F8\r\n"
          ":04FFFC00AAAABBBB37\r\n"
          ":00000001FF\r\n");
}

TEST_CASE("Intel HEX records") {
    SUBCASE("no code gives only the end record") { CHECK(format_intel_hex(chunks({})) == ":00000001FF\r\n"); }
    SUBCASE("record size") {
        CHECK(format_intel_hex(chunks({{0x100, {1, 2, 3, 4, 5}}}), 2) ==
              ":020000040000FA\r\n"
              ":020100000102FA\r\n"
              ":020102000304F4\r\n"
              ":0101040005F5\r\n"
              ":00000001FF\r\n");
    }
    SUBCASE("a record does not cross a 64 KB boundary") {
        CHECK(format_intel_hex(chunks({{0xFFFE, {1, 2, 3, 4}}})) ==
              ":020000040000FA\r\n"
              ":02FFFE000102FE\r\n"
              ":020000040001F9\r\n"
              ":020000000304F7\r\n"
              ":00000001FF\r\n");
    }
    SUBCASE("chunks are sorted and joined; the later one wins where they overlap") {
        CHECK(format_intel_hex(chunks({{0x21, {2}}, {0x10, {1, 2, 3}}, {0x20, {1}}, {0x11, {9}}})) ==
              ":020000040000FA\r\n"
              ":03001000010903E0\r\n"
              ":020020000102DB\r\n"
              ":00000001FF\r\n");
    }
    SUBCASE("gaps stay empty") {
        CHECK(format_intel_hex(chunks({{0, {0xCC}}, {0x8000, {0xCC}}})) ==
              ":020000040000FA\r\n"
              ":01000000CC33\r\n"
              ":01800000CCB3\r\n"
              ":00000001FF\r\n");
    }
    SUBCASE("bad record size") {
        CHECK_THROWS_AS(format_intel_hex(chunks({}), 0), std::invalid_argument);
        CHECK_THROWS_AS(format_intel_hex(chunks({}), 256), std::invalid_argument);
    }
}

TEST_CASE("S-records") {
    SUBCASE("smallest address size by default") {
        CHECK(format_srec(chunks({{0x100, {1, 2, 3}}})) ==
              "S0030000FC\r\n"
              "S1060100010203F2\r\n"
              "S5030001FB\r\n"
              "S9030000FC\r\n");
        CHECK(format_srec(chunks({{0xFFFF, {1}}})).find("\r\nS9") != std::string::npos);
        CHECK(format_srec(chunks({{0xFFFF, {1, 2}}})).find("\r\nS8") != std::string::npos);
        CHECK(format_srec(chunks({})) == "S0030000FC\r\nS5030000FC\r\nS9030000FC\r\n");
    }
    SUBCASE("chosen address size") {
        CHECK(format_srec(chunks({{0x100, {1, 2, 3}}}), 16, 3) ==
              "S0030000FC\r\n"
              "S207000100010203F1\r\n"
              "S5030001FB\r\n"
              "S804000000FB\r\n");
        CHECK(format_srec(chunks({{0x100, {1, 2, 3}}}), 2, 4) ==
              "S0030000FC\r\n"
              "S307000001000102F4\r\n"
              "S3060000010203F3\r\n"
              "S5030002FA\r\n"
              "S70500000000FA\r\n");
    }
    SUBCASE("errors") {
        CHECK_THROWS_AS(format_srec(chunks({{0xFFFF, {1, 2}}}), 16, 2), std::invalid_argument);
        CHECK_THROWS_AS(format_srec(chunks({}), 16, 1), std::invalid_argument);
        CHECK_THROWS_AS(format_srec(chunks({}), 16, 5), std::invalid_argument);
        CHECK_THROWS_AS(format_srec(chunks({}), 0), std::invalid_argument);
        CHECK_NOTHROW(format_srec(chunks({}), 252, 2));
        CHECK_THROWS_AS(format_srec(chunks({}), 253, 2), std::invalid_argument);
        CHECK_NOTHROW(format_srec(chunks({}), 250, 4));
        CHECK_THROWS_AS(format_srec(chunks({}), 251, 4), std::invalid_argument);
    }
}
