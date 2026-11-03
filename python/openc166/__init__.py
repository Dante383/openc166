# SPDX-License-Identifier: MIT
"""openc166: a Keil A166-compatible assembler for the Infineon C166/C167.

    >>> import openc166
    >>> result = openc166.assemble('''
    ... CODE    SECTION CODE AT 0E000H
    ...         MOV     R4, #0BEEFH
    ...         RET
    ... CODE    ENDS
    ...         END
    ... ''')
    >>> result.ok, result.diagnostics
    (True, [])
    >>> image = result.image()
    >>> hex(image.base), image.data.hex()
    ('0xe000', 'e6f4efbecb00')

assemble() never raises for mistakes in the source: they are listed in
Result.diagnostics. Result.image() raises AssemblyError if there are
errors, so code that only wants the bytes can write
``openc166.assemble(src).image().data``.
"""

from __future__ import annotations

import ctypes
import os
import threading
import weakref
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Union

from . import _native

__all__ = [
    "assemble",
    "assemble_file",
    "Result",
    "Diagnostic",
    "Chunk",
    "Image",
    "Symbol",
    "AssemblyError",
    "__version__",
]

_lib = _native.lib

__version__: str = _lib.oc166_version().decode("ascii")

PathLike = Union[str, "os.PathLike[str]"]


def _text(b: Optional[bytes]) -> str:
    return b.decode("utf-8", "replace") if b else ""


@dataclass(frozen=True)
class Diagnostic:
    """An error or warning. str() gives "file:line:column: error A62: message"."""

    severity: str  # "error" or "warning"
    code: str  # Keil A166 number such as "A62", or "OCn" for openc166's own
    message: str
    file: str
    line: int  # 1-based; 0 if not tied to a line
    column: int  # 1-based; 0 if unknown
    text: str = field(repr=False, compare=False, default="")

    @property
    def is_error(self) -> bool:
        return self.severity == "error"

    def __str__(self) -> str:
        return self.text


@dataclass(frozen=True)
class Chunk:
    """Bytes emitted at consecutive addresses."""

    address: int
    data: bytes

    @property
    def end(self) -> int:
        """Address after the last byte."""
        return self.address + len(self.data)


@dataclass(frozen=True)
class Image:
    """One contiguous memory image: data[0] belongs at address 'base'."""

    base: int
    data: bytes

    @property
    def end(self) -> int:
        """Address after the last byte."""
        return self.base + len(self.data)

    def __bytes__(self) -> bytes:
        return self.data

    def __len__(self) -> int:
        return len(self.data)


@dataclass(frozen=True)
class Symbol:
    """A name defined in the source.

    kind is how it was defined: "label", "equ", "set", "bit" or "lit".
    type says what value holds:
      "number"    a number or address
      "gpr"       a word register: 0..15 for R0..R15
      "gpr_byte"  a byte register: 0..15 for RL0, RH0, RL1, ...
      "sfr"       a special function register: its address
      "bit"       a bit: address of the word that holds it; see 'bit'
      "cc"        a condition code: its 4-bit encoding
      ""          a LIT, which has no value; see 'text'
    value is None for a LIT or if it could not be worked out (an error).
    """

    name: str
    kind: str
    type: str
    value: Optional[int]
    label_type: str = ""  # "near", "far", "byte", "word", "dword" or ""
    bit: Optional[int] = None  # bit number 0..15 of a "bit"
    text: str = ""  # replacement text of a LIT
    line: int = 0  # listing line of the definition


class AssemblyError(Exception):
    """The source has errors. 'diagnostics' lists them; 'result' is the whole Result."""

    def __init__(self, result: "Result"):
        self.result = result
        self.diagnostics: List[Diagnostic] = result.errors
        lines = [str(d) for d in self.diagnostics[:20]]
        if len(self.diagnostics) > 20:
            lines.append("... and {} more".format(len(self.diagnostics) - 20))
        super().__init__("{} error(s):\n{}".format(len(self.diagnostics), "\n".join(lines)))


class Result:
    """The outcome of assembling a source."""

    def __init__(self, handle: int):
        self._handle = handle
        self._lock = threading.Lock()
        self._finalizer = weakref.finalize(self, _lib.oc166_result_free, handle)
        self._listing: Optional[str] = None

        self.cpu: str = _lib.oc166_result_cpu(handle).decode("ascii")
        """CPU the source was assembled for: "c166" or "c167"."""

        self.diagnostics: List[Diagnostic] = []
        """Errors and warnings in source order."""
        d = _native.Diagnostic()
        for i in range(_lib.oc166_result_diagnostic_count(handle)):
            _lib.oc166_result_diagnostic(handle, i, ctypes.byref(d))
            self.diagnostics.append(
                Diagnostic(
                    "error" if d.is_error else "warning",
                    _text(d.code),
                    _text(d.message),
                    _text(d.file),
                    d.line,
                    d.column,
                    _text(d.text),
                )
            )

        self.chunks: List[Chunk] = []
        """Code and data in source order."""
        c = _native.Chunk()
        for i in range(_lib.oc166_result_chunk_count(handle)):
            _lib.oc166_result_chunk(handle, i, ctypes.byref(c))
            data = ctypes.string_at(c.data, c.size) if c.size else b""
            self.chunks.append(Chunk(c.address, data))

        self.symbols: Dict[str, Symbol] = {}
        """Defined names, spelled as in their definition, sorted by name."""
        s = _native.Symbol()
        for i in range(_lib.oc166_result_symbol_count(handle)):
            _lib.oc166_result_symbol(handle, i, ctypes.byref(s))
            kind = _text(s.kind)
            sym_type = _text(s.type)
            self.symbols[_text(s.name)] = Symbol(
                name=_text(s.name),
                kind=kind,
                type=sym_type,
                value=s.value if s.known and kind != "lit" else None,
                label_type=_text(s.label_type),
                bit=s.bit if sym_type == "bit" else None,
                text=_text(s.text),
                line=s.line,
            )
        self._folded = {name.upper(): sym for name, sym in self.symbols.items()}

    @property
    def errors(self) -> List[Diagnostic]:
        return [d for d in self.diagnostics if d.is_error]

    @property
    def warnings(self) -> List[Diagnostic]:
        return [d for d in self.diagnostics if not d.is_error]

    @property
    def ok(self) -> bool:
        """True if there are no errors (warnings are allowed)."""
        return not any(d.is_error for d in self.diagnostics)

    def symbol(self, name: str) -> Symbol:
        """Looks a symbol up ignoring case, as the assembler does. Raises KeyError."""
        return self._folded[name.upper()]

    @property
    def listing(self) -> str:
        """The listing: addresses, code bytes, source lines and diagnostics."""
        if self._listing is None:
            with self._lock:
                text = _lib.oc166_result_listing(self._handle)
            if text is None:
                raise MemoryError("cannot format the listing")
            self._listing = _text(text)
        return self._listing

    def image(self, base: Optional[int] = None, fill: int = 0xFF) -> Image:
        """Lays all code and data out as one image.

        The image starts at 'base', or at the lowest address used. Gaps are
        filled with 'fill'. Raises AssemblyError if the source has errors,
        and ValueError if code lies below 'base' or the image would be
        larger than 16 MB.
        """
        if not self.ok:
            raise AssemblyError(self)
        if not 0 <= fill <= 0xFF:
            raise ValueError("fill must be 0..255")
        if base is not None and base < 0:
            raise ValueError("base must not be negative")
        img = _native.Image()
        with self._lock:
            rc = _lib.oc166_result_image(
                self._handle, base is not None, base or 0, fill, ctypes.byref(img)
            )
            if rc != 0:
                raise ValueError(_text(_lib.oc166_result_last_error(self._handle)))
            data = ctypes.string_at(img.data, img.size) if img.size else b""
        return Image(img.base, data)

    def __repr__(self) -> str:
        return "<openc166.Result cpu={} errors={} warnings={} chunks={}>".format(
            self.cpu, len(self.errors), len(self.warnings), len(self.chunks)
        )


def assemble(
    source: Union[str, bytes],
    *,
    file_name: Optional[PathLike] = None,
    cpu: Optional[str] = None,
    origin: int = 0,
    include_dirs: Iterable[PathLike] = (),
) -> Result:
    """Assembles Keil A166 source text.

    source        the text. A str is encoded as UTF-8; pass bytes to keep
                  another encoding (Keil sources are often Windows-1252).
    file_name     names the source in diagnostics; $INCLUDE also searches
                  its directory. The source is not read from it.
    cpu           "c166" (the default) or "c167". $MOD167 in the source
                  selects the C167 too.
    origin        address of code outside any section, and of the first
                  section without an AT address.
    include_dirs  more directories searched by $INCLUDE.
    """
    if isinstance(source, str):
        source = source.encode("utf-8")
    elif not isinstance(source, (bytes, bytearray, memoryview)):
        raise TypeError("source must be str or bytes")
    source = bytes(source)

    opts = _lib.oc166_options_new()
    if not opts:
        raise MemoryError("cannot allocate options")
    try:
        if cpu is not None and _lib.oc166_options_set_cpu(opts, cpu.encode("ascii", "replace")) != 0:
            raise ValueError("unknown CPU {!r} (use 'c166' or 'c167')".format(cpu))
        if _lib.oc166_options_set_origin(opts, origin) != 0:
            raise ValueError("origin must not be negative")
        for d in include_dirs:
            _lib.oc166_options_add_include_dir(opts, os.fsencode(os.fspath(d)))
        name = os.fsencode(os.fspath(file_name)) if file_name is not None else None
        handle = _lib.oc166_assemble(opts, source, len(source), name)
    finally:
        _lib.oc166_options_free(opts)
    if not handle:
        raise MemoryError("out of memory while assembling")
    return Result(handle)


def assemble_file(
    path: PathLike,
    *,
    cpu: Optional[str] = None,
    origin: int = 0,
    include_dirs: Iterable[PathLike] = (),
) -> Result:
    """Reads and assembles a source file. See assemble() for the options."""
    with open(path, "rb") as f:
        source = f.read()
    return assemble(source, file_name=path, cpu=cpu, origin=origin, include_dirs=include_dirs)
