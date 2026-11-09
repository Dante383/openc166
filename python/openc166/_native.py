# SPDX-License-Identifier: MIT
"""Loads the openc166 shared library and declares its C functions.

The C interface is documented in include/openc166/openc166.h. The library
is looked for next to this file; the OPENC166_LIBRARY environment variable
names a different file (used to test a build tree).
"""

import ctypes
import os
import sys

# OC166_API_VERSION this module was written for.
API_VERSION = 1


class Diagnostic(ctypes.Structure):
    _fields_ = [
        ("is_error", ctypes.c_int),
        ("code", ctypes.c_char_p),
        ("message", ctypes.c_char_p),
        ("file", ctypes.c_char_p),
        ("line", ctypes.c_int),
        ("column", ctypes.c_int),
        ("text", ctypes.c_char_p),
    ]


class Chunk(ctypes.Structure):
    _fields_ = [
        ("address", ctypes.c_int64),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("size", ctypes.c_size_t),
    ]


class Image(ctypes.Structure):
    _fields_ = [
        ("base", ctypes.c_int64),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("size", ctypes.c_size_t),
    ]


class Symbol(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("kind", ctypes.c_char_p),
        ("type", ctypes.c_char_p),
        ("label_type", ctypes.c_char_p),
        ("value", ctypes.c_int64),
        ("bit", ctypes.c_int),
        ("known", ctypes.c_int),
        ("text", ctypes.c_char_p),
        ("line", ctypes.c_int),
    ]


class Section(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("type", ctypes.c_char_p),
        ("align", ctypes.c_char_p),
        ("address", ctypes.c_int64),
        ("size", ctypes.c_int64),
        ("placement", ctypes.c_char_p),
    ]


class Stop(ctypes.Structure):
    _fields_ = [
        ("reason", ctypes.c_char_p),
        ("id", ctypes.c_int),
        ("history_start", ctypes.c_int),
        ("access", ctypes.c_int),
        ("access_address", ctypes.c_uint32),
        ("access_size", ctypes.c_int),
        ("access_value", ctypes.c_uint16),
        ("access_old", ctypes.c_uint16),
    ]


class Unmodelled(ctypes.Structure):
    _fields_ = [
        ("instruction", ctypes.c_uint64),
        ("pc", ctypes.c_uint32),
        ("address", ctypes.c_uint32),
        ("write", ctypes.c_int),
        ("value", ctypes.c_uint16),
        ("what", ctypes.c_char_p),
    ]


CONDITION = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
READ_FN = ctypes.CFUNCTYPE(ctypes.c_uint8, ctypes.c_void_p, ctypes.c_uint32)
WRITE_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint8)


def _library_paths():
    override = os.environ.get("OPENC166_LIBRARY")
    if override:
        return [override]
    here = os.path.dirname(os.path.abspath(__file__))
    if sys.platform in ("win32", "cygwin"):
        names = ["openc166.dll", "libopenc166.dll", "cygopenc166.dll"]
    elif sys.platform == "darwin":
        names = ["libopenc166.dylib"]
    else:
        names = ["libopenc166.so"]
    return [os.path.join(here, name) for name in names]


def _load():
    problems = []
    for path in _library_paths():
        if not os.path.exists(path):
            problems.append(path + ": not found")
            continue
        try:
            return ctypes.CDLL(path)
        except OSError as e:
            problems.append("{}: {}".format(path, e))
    raise ImportError("cannot load the openc166 library:\n  " + "\n  ".join(problems))


lib = _load()


def _declare(name, restype, *argtypes):
    f = getattr(lib, name)
    f.restype = restype
    f.argtypes = list(argtypes)


_p = ctypes.c_void_p
_size = ctypes.c_size_t
_int = ctypes.c_int
_str = ctypes.c_char_p

_declare("oc166_api_version", _int)
_declare("oc166_version", _str)
_declare("oc166_options_new", _p)
_declare("oc166_options_free", None, _p)
_declare("oc166_options_set_cpu", _int, _p, _str)
_declare("oc166_options_set_dialect", _int, _p, _str)
_declare("oc166_options_add_control", _int, _p, _str)
_declare("oc166_options_set_origin", _int, _p, ctypes.c_int64)
_declare("oc166_options_add_include_dir", _int, _p, _str)
_declare("oc166_options_set_section_address", _int, _p, _str, ctypes.c_int64)
_declare("oc166_assemble", _p, _p, _str, _size, _str)
_declare("oc166_result_free", None, _p)
_declare("oc166_result_error_count", _size, _p)
_declare("oc166_result_warning_count", _size, _p)
_declare("oc166_result_cpu", _str, _p)
_declare("oc166_result_diagnostic_count", _size, _p)
_declare("oc166_result_diagnostic", _int, _p, _size, ctypes.POINTER(Diagnostic))
_declare("oc166_result_chunk_count", _size, _p)
_declare("oc166_result_chunk", _int, _p, _size, ctypes.POINTER(Chunk))
_declare("oc166_result_image", _int, _p, _int, ctypes.c_int64, ctypes.c_uint8, ctypes.POINTER(Image))
_declare("oc166_result_intel_hex", _str, _p, _int)
_declare("oc166_result_srec", _str, _p, _int, _int)
_declare("oc166_result_last_error", _str, _p)
_declare("oc166_result_listing", _str, _p)
_declare("oc166_result_debug_info", _str, _p)
_declare("oc166_result_symbol_count", _size, _p)
_declare("oc166_result_symbol", _int, _p, _size, ctypes.POINTER(Symbol))
_declare("oc166_result_section_count", _size, _p)
_declare("oc166_result_section", _int, _p, _size, ctypes.POINTER(Section))
_declare("oc166_assemble_many", _p, _p, _size, ctypes.POINTER(_str), ctypes.POINTER(_size), ctypes.POINTER(_str))
_declare("oc166_result_module_count", _size, _p)
_declare("oc166_result_module", _str, _p, _size)
_declare("oc166_result_symbol_module", _int, _p, _size)
_declare("oc166_result_symbol_exported", _int, _p, _size)
_declare("oc166_result_section_class", _str, _p, _size)
_declare("oc166_result_section_group", _str, _p, _size)
_declare(
    "oc166_result_section_bits", _int, _p, _size, ctypes.POINTER(_int), ctypes.POINTER(ctypes.c_int64)
)

# Disassembler and simulator.
_u8p = ctypes.POINTER(ctypes.c_uint8)
_u32, _u64, _u16, _i64 = ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint16, ctypes.c_int64
_declare("oc166_disassemble", _int, _u8p, _size, _u32, _int, ctypes.c_char_p, _size, ctypes.POINTER(_i64))
_declare("oc166_disassemble_module", ctypes.c_void_p, _u8p, _size, _u32, _int, ctypes.POINTER(_size))
_declare("oc166_free", None, ctypes.c_void_p)
_declare("oc166_sfr_address", _i64, _str)
_declare("oc166_bit_address", _i64, _str, ctypes.POINTER(_int))
_declare("oc166_sfr_name", _str, _u32)
_declare("oc166_sim_new", _p, _str, _u8p, _size)
_declare("oc166_sim_free", None, _p)
_declare("oc166_sim_reset", None, _p, _int)
for _name in ("oc166_sim_step", "oc166_sim_reverse_step", "oc166_sim_reverse_continue"):
    _declare(_name, _int, _p, ctypes.POINTER(Stop))
for _name in ("oc166_sim_continue", "oc166_sim_step_over", "oc166_sim_step_out"):
    _declare(_name, _int, _p, _u64, ctypes.POINTER(Stop))
_declare("oc166_sim_goto", _int, _p, _u64)
_declare("oc166_sim_request_stop", None, _p)
_declare("oc166_sim_set_history", None, _p, _u64, _size)
_declare("oc166_sim_clear_history", None, _p)
_declare("oc166_sim_history_start", _u64, _p)
_declare("oc166_sim_pc", _u32, _p)
_declare("oc166_sim_set_pc", None, _p, _u32)
_declare("oc166_sim_instructions", _u64, _p)
_declare("oc166_sim_cycles", _u64, _p)
_declare("oc166_sim_clock_hz", _u32, _p)
_declare("oc166_sim_gpr", _u16, _p, _int)
_declare("oc166_sim_set_gpr", None, _p, _int, _u16)
_declare("oc166_sim_sfr", _u16, _p, _u32)
_declare("oc166_sim_set_sfr", None, _p, _u32, _u16)
_declare(
    "oc166_sim_ext", None, _p, ctypes.POINTER(_int), ctypes.POINTER(_int), ctypes.POINTER(_int), ctypes.POINTER(_u32)
)
_declare("oc166_sim_einit_done", _int, _p)
_declare("oc166_sim_idle", _int, _p)
_declare("oc166_sim_read", None, _p, _u32, _u8p, _size)
_declare("oc166_sim_write", None, _p, _u32, _u8p, _size)
_declare("oc166_sim_data_address", _u32, _p, _u16)
_declare("oc166_sim_describe", _str, _p, _u32)
_declare("oc166_sim_disassemble", _int, _p, _u32, _int, ctypes.c_char_p, _size, ctypes.POINTER(_i64))
_declare("oc166_sim_add_breakpoint", _int, _p, _u32, CONDITION, ctypes.c_void_p)
_declare("oc166_sim_remove_breakpoint", _int, _p, _int)
_declare("oc166_sim_add_watchpoint", _int, _p, _u32, _u32, _int, _u16, CONDITION, ctypes.c_void_p)
_declare("oc166_sim_remove_watchpoint", _int, _p, _int)
_declare("oc166_sim_save", _p, _p)
_declare("oc166_sim_load", _int, _p, _p)
_declare("oc166_snapshot_free", None, _p)
_declare("oc166_snapshot_instructions", _u64, _p)
_declare("oc166_sim_request_interrupt", None, _p, _u32)
_declare("oc166_sim_nmi", None, _p)
_declare("oc166_sim_set_option", _int, _p, _str, _int)
_declare("oc166_sim_unmodelled_count", _size, _p)
_declare("oc166_sim_unmodelled", _int, _p, _size, ctypes.POINTER(Unmodelled))
_declare("oc166_sim_clear_unmodelled", None, _p)
_declare("oc166_sim_run_until_cycle", _int, _p, _u64, ctypes.POINTER(Stop))
_declare("oc166_sim_serial_send", _int, _p, _u8p, _size, _int)
_declare("oc166_sim_serial_receive", _size, _p, _u8p, _size)
_declare("oc166_sim_serial_receive_baud", _size, _p, _u8p, ctypes.POINTER(_u32), _size)
_declare("oc166_sim_serial_line_low", _int, _p, _u64)
_declare("oc166_sim_serial_frame_cycles", _u64, _p)
_declare("oc166_sim_set_adc", _int, _p, _int, _u16)
_declare("oc166_sim_set_chip_select", _int, _p, _int, _str, READ_FN, WRITE_FN, ctypes.c_void_p)

if lib.oc166_api_version() != API_VERSION:
    raise ImportError(
        "the openc166 library has interface version {}, this module needs {}".format(
            lib.oc166_api_version(), API_VERSION
        )
    )
