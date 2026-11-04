/* SPDX-License-Identifier: MIT */
/*
 * openc166 C interface.
 *
 * A plain C API over the assembler, exported by the openc166 shared library
 * (libopenc166.so, libopenc166.dylib, openc166.dll). It is meant for other
 * languages: the Python package uses it through ctypes, and any language
 * that can call C functions can do the same.
 *
 * Typical use:
 *
 *     oc166_options *opts = oc166_options_new();
 *     oc166_options_set_origin(opts, 0xE000);
 *     oc166_result *r = oc166_assemble(opts, src, strlen(src), "payload.a66");
 *     if (oc166_result_error_count(r) == 0) {
 *         oc166_image img;
 *         if (oc166_result_image(r, 0, 0, 0xFF, &img) == 0)
 *             fwrite(img.data, 1, img.size, out);
 *     }
 *     oc166_result_free(r);
 *     oc166_options_free(opts);
 *
 * Rules that apply to every function:
 *  - Strings are NUL-terminated. Source text and file names are passed
 *    through unchanged (Keil sources are usually Windows-1252; openc166
 *    does not convert them).
 *  - Pointers returned by oc166_result_* functions belong to the result and
 *    stay valid until oc166_result_free(), unless documented otherwise.
 *  - No function throws or aborts on bad input. Functions that can fail
 *    return -1 or NULL.
 *  - Different objects may be used from different threads at the same time.
 */
#ifndef OPENC166_OPENC166_H
#define OPENC166_OPENC166_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(OPENC166_BUILDING_LIBRARY)
#    define OC166_API __declspec(dllexport)
#  else
#    define OC166_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define OC166_API __attribute__((visibility("default")))
#else
#  define OC166_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Version of this interface. It changes when a function or structure below
 * changes in an incompatible way. */
#define OC166_API_VERSION 1

/* OC166_API_VERSION of the loaded library. */
OC166_API int oc166_api_version(void);

/* Version of openc166, e.g. "0.1.0". */
OC166_API const char *oc166_version(void);

/* ---- Options ----------------------------------------------------------- */

typedef struct oc166_options oc166_options;

/* New options with the defaults: CPU c166, origin 0, no include
 * directories. Returns NULL if out of memory. */
OC166_API oc166_options *oc166_options_new(void);
OC166_API void oc166_options_free(oc166_options *opts);

/* "c166" or "c167" (also "80c166", "80c167", "st10"; any case). $MOD167 in
 * the source selects the C167 too. Returns 0, or -1 for an unknown name. */
OC166_API int oc166_options_set_cpu(oc166_options *opts, const char *cpu);

/* Address of code outside any section, and of the first section without an
 * AT address. Returns 0, or -1 if 'origin' is negative. */
OC166_API int oc166_options_set_origin(oc166_options *opts, int64_t origin);

/* Adds a directory searched by $INCLUDE. Returns 0, or -1 on failure. */
OC166_API int oc166_options_add_include_dir(oc166_options *opts, const char *dir);

/* Places the section 'name' (any case), which must not have an AT address
 * in the source, at 'address', like the SECTIONS control of Keil's L166.
 * Other sections without AT go where openc166 finds room for them, from
 * the origin on. Setting a name again replaces its address. Returns 0, or
 * -1 for an empty name or an address outside 0..0FFFFFFH. */
OC166_API int oc166_options_set_section_address(oc166_options *opts, const char *name, int64_t address);

/* ---- Assembling -------------------------------------------------------- */

typedef struct oc166_result oc166_result;

/* Assembles 'length' bytes of source text. 'opts' may be NULL for the
 * defaults. 'file_name' (may be NULL) names the source in diagnostics, and
 * $INCLUDE searches its directory; the source itself is not read from it.
 *
 * Errors in the source do not make this fail: they are reported as
 * diagnostics of the result. Returns NULL only if out of memory or 'source'
 * is NULL while 'length' is not 0. */
OC166_API oc166_result *oc166_assemble(const oc166_options *opts, const char *source, size_t length,
                                       const char *file_name);
OC166_API void oc166_result_free(oc166_result *r);

OC166_API size_t oc166_result_error_count(const oc166_result *r);
OC166_API size_t oc166_result_warning_count(const oc166_result *r);

/* CPU the source was assembled for: "c166" or "c167". */
OC166_API const char *oc166_result_cpu(const oc166_result *r);

/* ---- Diagnostics (errors and warnings) --------------------------------- */

typedef struct oc166_diagnostic {
    int is_error;         /* 1 for an error, 0 for a warning */
    const char *code;     /* Keil A166 number such as "A62", or "OCn" */
    const char *message;
    const char *file;
    int line;             /* 1-based; 0 if not tied to a line */
    int column;           /* 1-based; 0 if unknown */
    const char *text;     /* "file:line:column: error A62: message" */
} oc166_diagnostic;

OC166_API size_t oc166_result_diagnostic_count(const oc166_result *r);

/* Fills '*out' with diagnostic 'index' (in source order). Returns 0, or -1
 * if 'index' is out of range. */
OC166_API int oc166_result_diagnostic(const oc166_result *r, size_t index, oc166_diagnostic *out);

/* ---- Code -------------------------------------------------------------- */

/* Bytes emitted at consecutive addresses. */
typedef struct oc166_chunk {
    int64_t address;
    const uint8_t *data;
    size_t size;
} oc166_chunk;

OC166_API size_t oc166_result_chunk_count(const oc166_result *r);

/* Fills '*out' with chunk 'index' (in source order). Returns 0, or -1 if
 * 'index' is out of range. */
OC166_API int oc166_result_chunk(const oc166_result *r, size_t index, oc166_chunk *out);

/* One contiguous memory image. */
typedef struct oc166_image {
    int64_t base;  /* address of data[0] */
    const uint8_t *data;
    size_t size;
} oc166_image;

/* Lays all chunks out as one image starting at 'base', or at the lowest
 * address used if 'has_base' is 0. Gaps are filled with 'fill'. Returns 0,
 * or -1 if a chunk lies below 'base' or the image would exceed 16 MB; then
 * oc166_result_last_error() says why. The image is built even if the source
 * has errors, so check oc166_result_error_count() first. 'out->data' stays
 * valid until the next oc166_result_image() call on 'r'. */
OC166_API int oc166_result_image(oc166_result *r, int has_base, int64_t base, uint8_t fill, oc166_image *out);

/* The code and data as Intel HEX text: data records of up to 'record_size'
 * bytes (1..255; 16 is usual), an extended linear address record (type 04)
 * before each 64 KB bank, and an end-of-file record. Lines end with CR LF.
 * Only the bytes used are written; gaps stay empty. Returns NULL on failure;
 * then oc166_result_last_error() says why. Like oc166_result_image(), it
 * works even if the source has errors. The text stays valid until the next
 * oc166_result_intel_hex() call on 'r'. */
OC166_API const char *oc166_result_intel_hex(oc166_result *r, int record_size);

/* The code and data as Motorola S-records: S0 header, data records of up to
 * 'record_size' bytes, S5 record count and end record. 'address_size' is 2
 * (S1/S9), 3 (S2/S8), 4 (S3/S7), or 0 for the smallest that holds every
 * address. Lines end with CR LF. Returns NULL if an address does not fit or
 * 'record_size' is out of range; then oc166_result_last_error() says why.
 * The text stays valid until the next oc166_result_srec() call on 'r'. */
OC166_API const char *oc166_result_srec(oc166_result *r, int record_size, int address_size);

/* Message of the last failed oc166_result_image(), oc166_result_intel_hex()
 * or oc166_result_srec() call, or "". */
OC166_API const char *oc166_result_last_error(const oc166_result *r);

/* ---- Listing ----------------------------------------------------------- */

/* The listing: addresses, code bytes, source lines and diagnostics, the
 * same text as "openc166 -l". Returns NULL if out of memory. */
OC166_API const char *oc166_result_listing(oc166_result *r);

/* ---- Symbols ----------------------------------------------------------- */

typedef struct oc166_symbol {
    const char *name;
    /* How it was defined: "label", "equ", "set", "bit" or "lit". */
    const char *kind;
    /* What the value is, and what 'value' holds:
     *   "number"    a number or address
     *   "gpr"       a word register; value = 0..15 for R0..R15
     *   "gpr_byte"  a byte register; value = 0..15 for RL0, RH0, RL1, ...
     *   "sfr"       a special function register; value = its address
     *   "bit"       a bit; value = address of the word that holds it
     *   "cc"        a condition code; value = its 4-bit encoding
     *   ""          a LIT, which has no value */
    const char *type;
    /* Type of a label: "near", "far", "byte", "word", "dword", or "". */
    const char *label_type;
    int64_t value;
    int bit;          /* bit number 0..15 for "bit", else 0 */
    int known;        /* 0 if the value could not be worked out (an error) */
    const char *text; /* replacement text of a LIT, else "" */
    int line;         /* listing line of the definition */
} oc166_symbol;

OC166_API size_t oc166_result_symbol_count(const oc166_result *r);

/* Fills '*out' with symbol 'index' (sorted by name). Returns 0, or -1 if
 * 'index' is out of range. */
OC166_API int oc166_result_symbol(const oc166_result *r, size_t index, oc166_symbol *out);

/* ---- Sections ---------------------------------------------------------- */

typedef struct oc166_section {
    const char *name;
    const char *type;       /* "CODE", "DATA", "CONST", "HDATA", "HCONST", "XDATA" or "XCONST" */
    const char *align;      /* "BYTE", "WORD", "DWORD", "PAGE", "SEGMENT", "BITADDRESSABLE" or
                               "PECADDRESSABLE" */
    int64_t address;
    int64_t size;           /* in bytes */
    /* How the address was chosen: "at" (AT in the source), "given"
     * (oc166_options_set_section_address) or "located" (by openc166). */
    const char *placement;
} oc166_section;

OC166_API size_t oc166_result_section_count(const oc166_result *r);

/* Fills '*out' with section 'index' (in order of first appearance).
 * Returns 0, or -1 if 'index' is out of range. */
OC166_API int oc166_result_section(const oc166_result *r, size_t index, oc166_section *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENC166_OPENC166_H */
