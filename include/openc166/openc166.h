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

/* The assembly language of the sources: "keil" (the default) for Keil
 * A166, or "tasking" for TASKING a166, whose sources pass through the
 * m166 macro preprocessor first (openc166 supports @SET, @EVAL, @IF,
 * @DEFINED and macro calls). TASKING sources are always for the C167.
 * Returns 0, or -1 for an unknown name. */
OC166_API int oc166_options_set_dialect(oc166_options *opts, const char *dialect);

/* Adds controls as they would be given on the assembler's command line,
 * e.g. "CASE" or "STDNAMES(reg.def) DEFINE(KLINE,1)". They are read before
 * the source, in the order added, like a $ line at the top of it.
 * Returns 0, or -1 on failure. */
OC166_API int oc166_options_add_control(oc166_options *opts, const char *controls);

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

/* Assembles 'count' (at least 1) sources as separate modules and links
 * them into one program, as Keil's A166 and L166 would: an EXTERN symbol
 * takes the value of the PUBLIC or GLOBAL symbol of that name in another
 * module, and sections with the same name and class are combined. Source i
 * is 'lengths[i]' bytes at 'sources[i]', named 'file_names[i]'.
 * 'file_names' may be NULL, and so may any name in it; such a module is
 * called "moduleN.a66". The file name set in 'opts' is not used. Code
 * outside sections starts at the origin in each module.
 *
 * Returns NULL only if out of memory or the arguments are not valid. */
OC166_API oc166_result *oc166_assemble_many(const oc166_options *opts, size_t count, const char *const *sources,
                                            const size_t *lengths, const char *const *file_names);

/* The modules of the result: one for oc166_assemble(), one per source for
 * oc166_assemble_many(). oc166_result_module() returns the file name of
 * module 'index', or NULL if 'index' is out of range. */
OC166_API size_t oc166_result_module_count(const oc166_result *r);
OC166_API const char *oc166_result_module(const oc166_result *r, size_t index);

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

/* ---- Debug information ------------------------------------------------- */

/* Debug information as JSON text (UTF-8): files, sections, symbols, and for
 * every line that emitted bytes its address, size, source file and line,
 * the $INCLUDE lines that led to it and, for code from a macro, the line of
 * the macro body. docs/debug-info.md describes the format. Returns NULL if
 * out of memory. */
OC166_API const char *oc166_result_debug_info(oc166_result *r);

/* ---- Symbols ----------------------------------------------------------- */

typedef struct oc166_symbol {
    const char *name;
    /* How it was defined: "label", "equ", "set", "bit", "lit" or "extern".
     * An "extern" symbol has the value of the PUBLIC symbol it names; its
     * 'text' is the type from the EXTERN line, such as "WORD" or "DATA8". */
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
    const char *text; /* replacement text of a LIT, type of an EXTERN, else "" */
    int line;         /* listing line of the definition, counted in its module */
} oc166_symbol;

OC166_API size_t oc166_result_symbol_count(const oc166_result *r);

/* Fills '*out' with symbol 'index' (sorted by name, then module). Returns
 * 0, or -1 if 'index' is out of range. */
OC166_API int oc166_result_symbol(const oc166_result *r, size_t index, oc166_symbol *out);

/* The module that defines symbol 'index', or -1 if 'index' is out of
 * range. */
OC166_API int oc166_result_symbol_module(const oc166_result *r, size_t index);

/* 1 if symbol 'index' is named by PUBLIC or GLOBAL, so other modules can
 * use it; 0 if not; -1 if 'index' is out of range. */
OC166_API int oc166_result_symbol_exported(const oc166_result *r, size_t index);

/* ---- Sections ---------------------------------------------------------- */

typedef struct oc166_section {
    const char *name;
    const char *type;       /* "CODE", "DATA", "CONST", "HDATA", "HCONST", "XDATA", "XCONST"
                               or "BIT" */
    const char *align;      /* "BIT", "BYTE", "WORD", "DWORD", "PAGE", "SEGMENT",
                               "BITADDRESSABLE" or "PECADDRESSABLE" */
    int64_t address;
    int64_t size;           /* in bytes; a BIT section counts every byte it touches */
    /* How the address was chosen: "at" (AT in the source), "given"
     * (oc166_options_set_section_address) or "located" (by openc166). */
    const char *placement;
} oc166_section;

OC166_API size_t oc166_result_section_count(const oc166_result *r);

/* Fills '*out' with section 'index' (in order of first appearance).
 * Returns 0, or -1 if 'index' is out of range. */
OC166_API int oc166_result_section(const oc166_result *r, size_t index, oc166_section *out);

/* The class of section 'index' as in its SECTION line ('ROM' gives "ROM"),
 * "" if it has none, or NULL if 'index' is out of range. */
OC166_API const char *oc166_result_section_class(const oc166_result *r, size_t index);

/* The DGROUP or CGROUP that section 'index' belongs to, "" if none, or NULL
 * if 'index' is out of range. */
OC166_API const char *oc166_result_section_group(const oc166_result *r, size_t index);

/* For a BIT section: sets '*first_bit' to the bit (0..7) of its address it
 * starts at and '*bit_count' to its size in bits. For other sections they are
 * 0 and 8 times the size. Either pointer may be NULL. Returns 0, or -1 if
 * 'index' is out of range. */
OC166_API int oc166_result_section_bits(const oc166_result *r, size_t index, int *first_bit,
                                        int64_t *bit_count);

/* ---- Disassembler ------------------------------------------------------ */

/* Flags for oc166_disassemble(). */
#define OC166_DIS_C166 1        /* C166 core: no EXT and ATOMIC instructions */
#define OC166_DIS_NO_NAMES 2    /* numbers instead of SFR and bit names */
#define OC166_DIS_REASSEMBLE 4  /* text that assembles back to the same bytes */
#define OC166_DIS_ESFR 8        /* inside an EXTR sequence: short addresses are ESFRs */

/* Disassembles the instruction at 'bytes' ('size' bytes readable), which
 * lies at 'address'. Writes the text, NUL-terminated, to 'text' (up to
 * 'text_size' bytes) and returns the instruction's length (2 or 4), or 0 if
 * the bytes are not an instruction. If 'target' is not NULL it receives the
 * branch target, or -1 if there is none. */
OC166_API int oc166_disassemble(const uint8_t *bytes, size_t size, uint32_t address, int flags, char *text,
                                size_t text_size, int64_t *target);

/* A memory image as openc166 source that assembles back to exactly the same
 * bytes ($SEGMENTED, one CODE section per 64 KB segment, DB for bytes that
 * are not instructions). Returns a string to release with oc166_free(), or
 * NULL if out of memory. '*fallbacks', if not NULL, receives the number of
 * instructions written as DB because their text would not reassemble. */
OC166_API char *oc166_disassemble_module(const uint8_t *bytes, size_t size, uint32_t base, int flags,
                                         size_t *fallbacks);

/* C167 register and bit names (REG167.INC and the core registers). Returns
 * the SFR or ESFR address of 'name' (any case), or -1. */
OC166_API int64_t oc166_sfr_address(const char *name);
/* The word address of a named bit such as "IEN" or "Z", with its bit number
 * in '*bit', or -1. */
OC166_API int64_t oc166_bit_address(const char *name, int *bit);
/* The name of the SFR or ESFR at 'address', or "" if it has none. */
OC166_API const char *oc166_sfr_name(uint32_t address);

/* Releases a string returned by oc166_disassemble_module(). */
OC166_API void oc166_free(void *p);

/* ---- Simulator --------------------------------------------------------- */

typedef struct oc166_sim oc166_sim;

/* A machine for 'board' ("5wy19", "c167cs", "c167" or "c166") with 'image'
 * as its flash or ROM (from address 0), with a debugger attached. Returns
 * NULL for an unknown board or if out of memory. */
OC166_API oc166_sim *oc166_sim_new(const char *board, const uint8_t *image, size_t size);
OC166_API void oc166_sim_free(oc166_sim *s);

/* Resets the machine: 0 power-on, 1 software (SRST), 2 watchdog, 3 hardware. */
OC166_API void oc166_sim_reset(oc166_sim *s, int kind);

/* Why execution stopped. */
typedef struct oc166_stop {
    /* "none", "breakpoint", "watchpoint", "unmodelled", "powerdown", "idle",
     * "halted" or "undefined". */
    const char *reason;
    int id;                   /* breakpoint or watchpoint */
    int history_start;        /* reverse execution reached the oldest state */
    int access;               /* watchpoint: 1 read, 2 write; else 0 */
    uint32_t access_address;
    int access_size;
    uint16_t access_value;
    uint16_t access_old;      /* for writes: the value before */
} oc166_stop;

/* Run control. Each returns 0 and fills '*out' (may be NULL), or -1.
 * 'max_instructions' 0 means no limit. */
OC166_API int oc166_sim_step(oc166_sim *s, oc166_stop *out);
OC166_API int oc166_sim_continue(oc166_sim *s, uint64_t max_instructions, oc166_stop *out);
OC166_API int oc166_sim_step_over(oc166_sim *s, uint64_t max_instructions, oc166_stop *out);
OC166_API int oc166_sim_step_out(oc166_sim *s, uint64_t max_instructions, oc166_stop *out);
OC166_API int oc166_sim_reverse_step(oc166_sim *s, oc166_stop *out);
OC166_API int oc166_sim_reverse_continue(oc166_sim *s, oc166_stop *out);
/* Goes to the state after 'instructions' instructions. Returns 0, or -1 if
 * that lies before the recorded history. */
OC166_API int oc166_sim_goto(oc166_sim *s, uint64_t instructions);
/* Asks a running oc166_sim_continue() (on another thread, or from a
 * callback) to stop; it returns with reason "halted". */
OC166_API void oc166_sim_request_stop(oc166_sim *s);
/* Snapshot spacing for reverse execution, and how many to keep. */
OC166_API void oc166_sim_set_history(oc166_sim *s, uint64_t interval, size_t limit);
/* Forgets the recorded history (call after changing state by hand). */
OC166_API void oc166_sim_clear_history(oc166_sim *s);
OC166_API uint64_t oc166_sim_history_start(const oc166_sim *s);

/* State. */
OC166_API uint32_t oc166_sim_pc(const oc166_sim *s);  /* CSP:IP */
OC166_API void oc166_sim_set_pc(oc166_sim *s, uint32_t pc);
OC166_API uint64_t oc166_sim_instructions(const oc166_sim *s);
OC166_API uint64_t oc166_sim_cycles(const oc166_sim *s);
OC166_API uint32_t oc166_sim_clock_hz(const oc166_sim *s);
OC166_API uint16_t oc166_sim_gpr(const oc166_sim *s, int n);
OC166_API void oc166_sim_set_gpr(oc166_sim *s, int n, uint16_t v);
/* SFR or ESFR word as stored, without side effects. */
OC166_API uint16_t oc166_sim_sfr(const oc166_sim *s, uint32_t address);
OC166_API void oc166_sim_set_sfr(oc166_sim *s, uint32_t address, uint16_t v);
/* EXT/ATOMIC state: instructions left, 1 if short addresses are ESFRs, the
 * kind (0 none, 1 page, 2 segment) and its page or segment. Any pointer
 * may be NULL. */
OC166_API void oc166_sim_ext(const oc166_sim *s, int *left, int *esfr, int *kind, uint32_t *value);
/* 1 after EINIT, 1 while in IDLE. */
OC166_API int oc166_sim_einit_done(const oc166_sim *s);
OC166_API int oc166_sim_idle(const oc166_sim *s);

/* Memory, without side effects (the debugger's view). */
OC166_API void oc166_sim_read(oc166_sim *s, uint32_t address, uint8_t *out, size_t size);
OC166_API void oc166_sim_write(oc166_sim *s, uint32_t address, const uint8_t *data, size_t size);
/* The physical address of the 16-bit data address 'a' (DPPs, or an active
 * EXTP/EXTS). */
OC166_API uint32_t oc166_sim_data_address(const oc166_sim *s, uint16_t a);
/* What answers at 'address' ("IRAM", "SFR", "CS0:flash", ...). The string
 * stays valid until the next call. */
OC166_API const char *oc166_sim_describe(oc166_sim *s, uint32_t address);
/* Disassembles at 'address' as the CPU would now (EXTR state). Like
 * oc166_disassemble(); 'flags' takes OC166_DIS_NO_NAMES. */
OC166_API int oc166_sim_disassemble(oc166_sim *s, uint32_t address, int flags, char *text, size_t text_size,
                                    int64_t *target);

/* Breakpoints and watchpoints. A condition callback, if not NULL, decides
 * whether to stop (non-zero: stop). Return an id > 0, or -1. */
typedef int (*oc166_condition)(void *user);
OC166_API int oc166_sim_add_breakpoint(oc166_sim *s, uint32_t address, oc166_condition cond, void *user);
OC166_API int oc166_sim_remove_breakpoint(oc166_sim *s, int id);
/* 'kinds': 1 read, 2 write, 4 value change (bits in 'mask' only). */
OC166_API int oc166_sim_add_watchpoint(oc166_sim *s, uint32_t address, uint32_t size, int kinds, uint16_t mask,
                                       oc166_condition cond, void *user);
OC166_API int oc166_sim_remove_watchpoint(oc166_sim *s, int id);

/* Snapshots. */
typedef struct oc166_snapshot oc166_snapshot;
OC166_API oc166_snapshot *oc166_sim_save(const oc166_sim *s);
OC166_API int oc166_sim_load(oc166_sim *s, const oc166_snapshot *snap);
OC166_API void oc166_snapshot_free(oc166_snapshot *snap);
OC166_API uint64_t oc166_snapshot_instructions(const oc166_snapshot *snap);

/* Interrupts: sets the request flag of the source whose control register
 * is at 'ic'; oc166_sim_nmi() requests the NMI trap. */
OC166_API void oc166_sim_request_interrupt(oc166_sim *s, uint32_t ic);
OC166_API void oc166_sim_nmi(oc166_sim *s);

/* Options: "stop_on_unmodelled", "stop_on_undefined", "watchdog_frozen".
 * Returns 0, or -1 for an unknown name. */
OC166_API int oc166_sim_set_option(oc166_sim *s, const char *name, int value);

/* Accesses to registers and addresses that nothing models, each reported
 * once. */
typedef struct oc166_unmodelled {
    uint64_t instruction;
    uint32_t pc;
    uint32_t address;
    int write;
    uint16_t value;
    const char *what;
} oc166_unmodelled;
OC166_API size_t oc166_sim_unmodelled_count(const oc166_sim *s);
OC166_API int oc166_sim_unmodelled(const oc166_sim *s, size_t index, oc166_unmodelled *out);
OC166_API void oc166_sim_clear_unmodelled(oc166_sim *s);

/* A device on chip select 'cs' (0..4) driven by callbacks: read returns
 * the byte at a physical address, write receives one. The device keeps no
 * state of its own in snapshots. Returns 0, or -1. */
typedef uint8_t (*oc166_read_fn)(void *user, uint32_t address);
typedef void (*oc166_write_fn)(void *user, uint32_t address, uint8_t value);
OC166_API int oc166_sim_set_chip_select(oc166_sim *s, int cs, const char *name, oc166_read_fn read,
                                        oc166_write_fn write, void *user);

#ifdef __cplusplus
}
#endif

#endif /* OPENC166_OPENC166_H */
