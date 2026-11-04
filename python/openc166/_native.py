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

if lib.oc166_api_version() != API_VERSION:
    raise ImportError(
        "the openc166 library has interface version {}, this module needs {}".format(
            lib.oc166_api_version(), API_VERSION
        )
    )
