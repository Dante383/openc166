/* SPDX-License-Identifier: MIT */
/* Tests of the C interface, written in C to check that the header is
 * valid C and that the shared library exports what it declares. */
#include "openc166/openc166.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static oc166_result *assemble_text(const oc166_options *opts, const char *src) {
    return oc166_assemble(opts, src, strlen(src), "test.a66");
}

static const oc166_symbol *find_symbol(const oc166_result *r, const char *name, oc166_symbol *buf) {
    size_t i;
    for (i = 0; i < oc166_result_symbol_count(r); ++i)
        if (oc166_result_symbol(r, i, buf) == 0 && strcmp(buf->name, name) == 0) return buf;
    return NULL;
}

static void test_version(void) {
    CHECK(oc166_api_version() == OC166_API_VERSION);
    CHECK(oc166_version() != NULL && oc166_version()[0] != '\0');
}

static void test_options(void) {
    oc166_options *opts = oc166_options_new();
    CHECK(opts != NULL);
    CHECK(oc166_options_set_cpu(opts, "C167") == 0);
    CHECK(oc166_options_set_cpu(opts, "z80") == -1);
    CHECK(oc166_options_set_cpu(NULL, "c167") == -1);
    CHECK(oc166_options_set_origin(opts, 0xE000) == 0);
    CHECK(oc166_options_set_origin(opts, -1) == -1);
    CHECK(oc166_options_add_include_dir(opts, "inc") == 0);
    CHECK(oc166_options_add_include_dir(opts, NULL) == -1);
    oc166_options_free(opts);
    oc166_options_free(NULL);
}

static void test_code_and_image(void) {
    static const uint8_t expected[] = {0xE6, 0xF4, 0xEF, 0xBE, 0xF6, 0xF4, 0x02, 0xC0, 0xCB, 0x00};
    oc166_options *opts = oc166_options_new();
    oc166_result *r;
    oc166_chunk chunk;
    oc166_image image;

    oc166_options_set_origin(opts, 0xE000);
    r = assemble_text(opts,
                      "START: MOV R4, #0BEEFH\n"
                      "       MOV 0C002H, R4\n"
                      "       RET\n"
                      "       END\n");
    CHECK(r != NULL);
    CHECK(oc166_result_error_count(r) == 0);
    CHECK(oc166_result_warning_count(r) == 0);
    CHECK(strcmp(oc166_result_cpu(r), "c166") == 0);

    CHECK(oc166_result_chunk_count(r) >= 1);
    CHECK(oc166_result_chunk(r, 0, &chunk) == 0);
    CHECK(chunk.address == 0xE000);
    CHECK(oc166_result_chunk(r, 99, &chunk) == -1);

    CHECK(oc166_result_image(r, 0, 0, 0xFF, &image) == 0);
    CHECK(image.base == 0xE000);
    CHECK(image.size == sizeof expected);
    CHECK(image.size == sizeof expected && memcmp(image.data, expected, sizeof expected) == 0);
    CHECK(oc166_result_last_error(r)[0] == '\0');

    /* An explicit base below the code pads with the fill byte. */
    CHECK(oc166_result_image(r, 1, 0xDFFE, 0x00, &image) == 0);
    CHECK(image.base == 0xDFFE && image.size == sizeof expected + 2);
    CHECK(image.data[0] == 0x00 && image.data[1] == 0x00 && image.data[2] == 0xE6);

    /* A base above the code fails. */
    CHECK(oc166_result_image(r, 1, 0xE002, 0xFF, &image) == -1);
    CHECK(oc166_result_last_error(r)[0] != '\0');

    {
        const char *hex = oc166_result_intel_hex(r, 16);
        const char *srec = oc166_result_srec(r, 16, 0);
        CHECK(hex != NULL && strcmp(hex,
                                    ":020000040000FA\r\n"
                                    ":0AE00000E6F4EFBEF6F402C0CB0018\r\n"
                                    ":00000001FF\r\n") == 0);
        CHECK(srec != NULL && strcmp(srec,
                                     "S0030000FC\r\n"
                                     "S10DE000E6F4EFBEF6F402C0CB0014\r\n"
                                     "S5030001FB\r\n"
                                     "S9030000FC\r\n") == 0);
        hex = oc166_result_intel_hex(r, 4);
        CHECK(hex != NULL && strstr(hex, ":04E00400F6F402C06C\r\n:02E00800CB004B\r\n") != NULL);
        CHECK(oc166_result_last_error(r)[0] == '\0');
        CHECK(oc166_result_intel_hex(r, 0) == NULL);
        CHECK(oc166_result_last_error(r)[0] != '\0');
        CHECK(oc166_result_srec(r, 16, 5) == NULL);
        CHECK(oc166_result_srec(NULL, 16, 0) == NULL);
        CHECK(oc166_result_intel_hex(NULL, 16) == NULL);
    }

    CHECK(oc166_result_listing(r) != NULL);
    CHECK(strstr(oc166_result_listing(r), "MOV R4, #0BEEFH") != NULL);

    oc166_result_free(r);
    oc166_options_free(opts);
}

static void test_diagnostics(void) {
    oc166_diagnostic d;
    oc166_image image;
    oc166_result *r = assemble_text(NULL, " NOP\n MOV R1, UNDEFINED_THING\n END\n");
    CHECK(r != NULL);
    CHECK(oc166_result_error_count(r) == 1);
    CHECK(oc166_result_diagnostic_count(r) == 1);
    CHECK(oc166_result_diagnostic(r, 0, &d) == 0);
    CHECK(d.is_error == 1);
    CHECK(strcmp(d.code, "A42") == 0);
    CHECK(strcmp(d.file, "test.a66") == 0);
    CHECK(d.line == 2);
    CHECK(d.column == 10);
    CHECK(strcmp(d.text, "test.a66:2:10: error A42: undefined identifier 'UNDEFINED_THING'") == 0);
    CHECK(oc166_result_diagnostic(r, 1, &d) == -1);
    /* The image can still be built; callers check the error count. */
    CHECK(oc166_result_image(r, 0, 0, 0xFF, &image) == 0);
    oc166_result_free(r);
}

static void test_symbols(void) {
    oc166_symbol s;
    oc166_options *opts = oc166_options_new();
    oc166_result *r;
    oc166_options_set_cpu(opts, "c167");
    r = assemble_text(opts,
                      "$INCLUDE (REG167.INC)\n"
                      "COUNT  EQU 10\n"
                      "STEP   SET 2\n"
                      "LED    BIT P2.3\n"
                      "FLAG   BIT 0FD10H.7\n"
                      "IDX    EQU R6\n"
                      "PORT   EQU P2\n"
                      "GREET  LIT 'MOV R1, #COUNT'\n"
                      "CODE   SECTION CODE AT 0C000H\n"
                      "MAIN   PROC FAR\n"
                      "       GREET\n"
                      "       RETS\n"
                      "MAIN   ENDP\n"
                      "TABLE: DW 1, 2\n"
                      "CODE   ENDS\n"
                      "       END\n");
    CHECK(oc166_result_error_count(r) == 0);
    CHECK(strcmp(oc166_result_cpu(r), "c167") == 0);

    CHECK(find_symbol(r, "COUNT", &s) != NULL);
    CHECK(strcmp(s.kind, "equ") == 0 && strcmp(s.type, "number") == 0 && s.value == 10 && s.known == 1);
    CHECK(find_symbol(r, "STEP", &s) != NULL);
    CHECK(strcmp(s.kind, "set") == 0 && s.value == 2);
    CHECK(find_symbol(r, "LED", &s) != NULL);
    CHECK(strcmp(s.kind, "bit") == 0 && strcmp(s.type, "bit") == 0 && s.value == 0xFFC0 && s.bit == 3);
    CHECK(find_symbol(r, "FLAG", &s) != NULL);
    CHECK(strcmp(s.type, "bit") == 0 && s.value == 0xFD10 && s.bit == 7);
    CHECK(find_symbol(r, "IDX", &s) != NULL);
    CHECK(strcmp(s.type, "gpr") == 0 && s.value == 6);
    CHECK(find_symbol(r, "PORT", &s) != NULL);
    CHECK(strcmp(s.type, "sfr") == 0 && s.value == 0xFFC0);
    CHECK(find_symbol(r, "GREET", &s) != NULL);
    CHECK(strcmp(s.kind, "lit") == 0 && strcmp(s.type, "") == 0 && strcmp(s.text, "MOV R1, #COUNT") == 0);
    CHECK(find_symbol(r, "MAIN", &s) != NULL);
    CHECK(strcmp(s.kind, "label") == 0 && strcmp(s.label_type, "far") == 0 && s.value == 0xC000);
    CHECK(find_symbol(r, "TABLE", &s) != NULL);
    CHECK(strcmp(s.label_type, "word") == 0 && s.value == 0xC004);
    CHECK(find_symbol(r, "NOT_THERE", &s) == NULL);
    CHECK(oc166_result_symbol(r, oc166_result_symbol_count(r), &s) == -1);

    oc166_result_free(r);
    oc166_options_free(opts);
}

static void test_edge_cases(void) {
    oc166_result *r = oc166_assemble(NULL, NULL, 0, NULL);
    CHECK(r != NULL);
    CHECK(oc166_result_error_count(r) == 0);
    CHECK(oc166_result_chunk_count(r) == 0);
    oc166_result_free(r);
    CHECK(oc166_assemble(NULL, NULL, 5, NULL) == NULL);
    oc166_result_free(NULL);
    CHECK(oc166_result_error_count(NULL) == 0);
    CHECK(oc166_result_listing(NULL) == NULL);
}

int main(void) {
    test_version();
    test_options();
    test_code_and_image();
    test_diagnostics();
    test_symbols();
    test_edge_cases();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("C API: all checks passed\n");
    return 0;
}
