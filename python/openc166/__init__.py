# SPDX-License-Identifier: MIT
"""openc166: a Keil A166-compatible assembler for the Infineon C166/C167.

    >>> import openc166
    >>> result = openc166.assemble('''
    ... PROG    SECTION CODE AT 0E000H
    ...         MOV     R4, #0BEEFH
    ...         RET
    ... PROG    ENDS
    ...         END
    ... ''')
    >>> result.ok, result.diagnostics
    (True, [])
    >>> image = result.image()
    >>> hex(image.base), image.data.hex()
    ('0xe000', 'e6f4efbecb00')

assemble_many() and assemble_files() assemble several modules and link
them, as Keil's A166 and L166 would: EXTERN symbols take their values from
PUBLIC ones in other modules.

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
from typing import Dict, Iterable, List, Mapping, Optional, Tuple, Union

from . import _native

__all__ = [
    "assemble",
    "assemble_file",
    "assemble_many",
    "assemble_files",
    "Result",
    "Diagnostic",
    "Chunk",
    "Image",
    "Symbol",
    "Section",
    "AssemblyError",
    "__version__",
]

_lib = _native.lib

__version__: str = _lib.oc166_version().decode("ascii")

PathLike = Union[str, "os.PathLike[str]"]
SourceText = Union[str, bytes]


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

    kind is how it was defined: "label", "equ", "set", "bit", "lit" or
    "extern". An "extern" has the value of the PUBLIC symbol it names, and
    its 'text' is the type from the EXTERN line, such as "WORD".
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
    text: str = ""  # replacement text of a LIT, type of an EXTERN
    line: int = 0  # listing line of the definition, counted in its module
    module: str = ""  # file name of the module that defines it
    exported: bool = False  # named by PUBLIC or GLOBAL, so other modules can use it


@dataclass(frozen=True)
class Section:
    """Where a SECTION went.

    type is "CODE", "DATA", "CONST", "HDATA", "HCONST", "XDATA", "XCONST"
    or "BIT". align is "BIT", "BYTE", "WORD", "DWORD", "PAGE", "SEGMENT",
    "BITADDRESSABLE" or "PECADDRESSABLE". placement says how the address was chosen: "at"
    (AT in the source), "given" (the 'sections' argument of assemble()) or
    "located" (by openc166, in the first free place from the origin on).
    class_name is the class from the SECTION line ('ROM' gives "ROM"), or "".
    group is the DGROUP or CGROUP the section belongs to, or "".
    size counts bytes; a BIT section counts every byte it touches. For a BIT
    section, bit is the bit (0..7) of address it starts at and bit_size its
    size in bits; other sections have bit 0 and bit_size 8 * size.
    """

    name: str
    type: str
    align: str
    address: int
    size: int
    placement: str
    class_name: str = ""
    group: str = ""
    bit: int = 0
    bit_size: int = -1  # -1 gives 8 * size

    def __post_init__(self) -> None:
        if self.bit_size < 0:
            object.__setattr__(self, "bit_size", 8 * self.size)

    @property
    def end(self) -> int:
        """Address after the last byte."""
        return self.address + self.size


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

        self.modules: List[str] = [
            _text(_lib.oc166_result_module(handle, i))
            for i in range(_lib.oc166_result_module_count(handle))
        ]
        """File name of each module: one per source."""

        self.all_symbols: List[Symbol] = []
        """The names defined in every module, sorted by name, then module."""
        s = _native.Symbol()
        for i in range(_lib.oc166_result_symbol_count(handle)):
            _lib.oc166_result_symbol(handle, i, ctypes.byref(s))
            kind = _text(s.kind)
            sym_type = _text(s.type)
            module = _lib.oc166_result_symbol_module(handle, i)
            self.all_symbols.append(
                Symbol(
                    name=_text(s.name),
                    kind=kind,
                    type=sym_type,
                    value=s.value if s.known and kind != "lit" else None,
                    label_type=_text(s.label_type),
                    bit=s.bit if sym_type == "bit" else None,
                    text=_text(s.text),
                    line=s.line,
                    module=self.modules[module] if 0 <= module < len(self.modules) else "",
                    exported=_lib.oc166_result_symbol_exported(handle, i) == 1,
                )
            )

        self.symbols: Dict[str, Symbol] = {}
        """Defined names, spelled as in their definition, sorted by name.

        If several modules define a name, this holds the PUBLIC or GLOBAL
        one, or else the one in the first of them; all_symbols has them all.
        """
        for sym in self.all_symbols:
            if sym.name not in self.symbols or (sym.exported and not self.symbols[sym.name].exported):
                self.symbols[sym.name] = sym
        self._folded = {name.upper(): sym for name, sym in self.symbols.items()}

        self.sections: Dict[str, Section] = {}
        """Sections in order of first appearance, by name as first spelled."""
        sec = _native.Section()
        first_bit = ctypes.c_int()
        bit_count = ctypes.c_int64()
        for i in range(_lib.oc166_result_section_count(handle)):
            _lib.oc166_result_section(handle, i, ctypes.byref(sec))
            _lib.oc166_result_section_bits(handle, i, ctypes.byref(first_bit), ctypes.byref(bit_count))
            self.sections[_text(sec.name)] = Section(
                name=_text(sec.name),
                type=_text(sec.type),
                align=_text(sec.align),
                address=sec.address,
                size=sec.size,
                placement=_text(sec.placement),
                class_name=_text(_lib.oc166_result_section_class(handle, i)),
                group=_text(_lib.oc166_result_section_group(handle, i)),
                bit=first_bit.value,
                bit_size=bit_count.value,
            )

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

    def intel_hex(self, record_size: int = 16) -> str:
        """The code and data as Intel HEX text.

        Data records hold up to 'record_size' bytes (1..255). An extended
        linear address record (type 04) comes before each 64 KB bank, and
        lines end with CR LF, as Keil OH166 writes them. Only the bytes used
        are written: gaps stay empty. Raises AssemblyError if the source has
        errors, and ValueError for a bad record size.
        """
        return self._text_output(_lib.oc166_result_intel_hex, record_size)

    def srec(self, record_size: int = 16, address_size: Optional[int] = None) -> str:
        """The code and data as Motorola S-records.

        address_size is 2 for S1 records, 3 for S2, 4 for S3, or None for
        the smallest that holds every address. Data records hold up to
        'record_size' bytes. Raises AssemblyError if the source has errors,
        and ValueError if an address does not fit or a size is out of range.
        """
        return self._text_output(_lib.oc166_result_srec, record_size, address_size or 0)

    def _text_output(self, func, *args: int) -> str:
        if not self.ok:
            raise AssemblyError(self)
        if any(not -(2**31) <= a < 2**31 for a in args):
            raise ValueError("size out of range")
        with self._lock:
            text = func(self._handle, *args)
            if text is None:
                raise ValueError(_text(_lib.oc166_result_last_error(self._handle)))
        return text.decode("ascii")

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
    sections: Optional[Mapping[str, int]] = None,
) -> Result:
    """Assembles Keil A166 source text.

    source        the text. A str is encoded as UTF-8; pass bytes to keep
                  another encoding (Keil sources are often Windows-1252).
    file_name     names the source in diagnostics; $INCLUDE also searches
                  its directory. The source is not read from it.
    cpu           "c166" (the default) or "c167". $MOD167 in the source
                  selects the C167 too.
    origin        address of code outside any section, and the lowest
                  address for sections without AT.
    include_dirs  more directories searched by $INCLUDE.
    sections      addresses for sections without AT, by name (any case),
                  like the SECTIONS control of Keil's L166:
                  {"MYDATA": 0xF600}. The other sections go in order of
                  appearance to the first free place from the origin on.
    """
    source = _source_bytes(source)
    name = os.fsencode(os.fspath(file_name)) if file_name is not None else None
    return _run(
        lambda opts: _lib.oc166_assemble(opts, source, len(source), name),
        cpu,
        origin,
        include_dirs,
        sections,
    )


def _source_bytes(source: SourceText) -> bytes:
    if isinstance(source, str):
        return source.encode("utf-8")
    if not isinstance(source, (bytes, bytearray, memoryview)):
        raise TypeError("source must be str or bytes")
    return bytes(source)


def _run(call, cpu, origin, include_dirs, sections) -> Result:
    """Makes the options, calls call(options) and wraps the result."""
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
        for sec_name, address in (sections or {}).items():
            if not isinstance(address, int) or not 0 <= address <= 0xFFFFFF:
                raise ValueError(
                    "bad address {!r} for section {!r} (use 0..0FFFFFFH)".format(address, sec_name)
                )
            if _lib.oc166_options_set_section_address(opts, str(sec_name).encode("utf-8"), address) != 0:
                raise ValueError("bad section name {!r}".format(sec_name))
        handle = call(opts)
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
    sections: Optional[Mapping[str, int]] = None,
) -> Result:
    """Reads and assembles a source file. See assemble() for the options."""
    with open(path, "rb") as f:
        source = f.read()
    return assemble(
        source,
        file_name=path,
        cpu=cpu,
        origin=origin,
        include_dirs=include_dirs,
        sections=sections,
    )


def assemble_many(
    sources: Union[Mapping[PathLike, SourceText], Iterable[Tuple[PathLike, SourceText]]],
    *,
    cpu: Optional[str] = None,
    origin: int = 0,
    include_dirs: Iterable[PathLike] = (),
    sections: Optional[Mapping[str, int]] = None,
) -> Result:
    """Assembles several modules and links them into one program.

    sources is a dict or a list of (file name, source text) pairs; each is
    a module, as one .a66 file is for Keil's A166. As L166 would, the
    modules are joined: an EXTERN symbol takes the value of the PUBLIC or
    GLOBAL symbol of that name in another module, and sections with the
    same name and class are combined. Code outside sections starts at the
    origin in every module. See assemble() for the other options.

        >>> result = assemble_many({
        ...     "main.a66": "EXTERN LIMIT:DATA16\\n"
        ...                 "PROG SECTION CODE AT 0E000H\\n"
        ...                 "     MOV R4, #LIMIT\\n"
        ...                 "PROG ENDS\\n"
        ...                 "     END\\n",
        ...     "consts.a66": "PUBLIC LIMIT\\nLIMIT EQU 1234H\\n END\\n",
        ... })
        >>> result.image().data.hex(), result.symbol("LIMIT").module
        ('e6f43412', 'consts.a66')
    """
    items = list(sources.items()) if isinstance(sources, Mapping) else list(sources)
    if not items:
        raise ValueError("no sources")
    names = [os.fsencode(os.fspath(name)) for name, _ in items]
    texts = [_source_bytes(text) for _, text in items]
    count = len(items)
    c_texts = (ctypes.c_char_p * count)(*texts)
    c_lengths = (ctypes.c_size_t * count)(*(len(t) for t in texts))
    c_names = (ctypes.c_char_p * count)(*names)
    return _run(
        lambda opts: _lib.oc166_assemble_many(opts, count, c_texts, c_lengths, c_names),
        cpu,
        origin,
        include_dirs,
        sections,
    )


def assemble_files(
    paths: Iterable[PathLike],
    *,
    cpu: Optional[str] = None,
    origin: int = 0,
    include_dirs: Iterable[PathLike] = (),
    sections: Optional[Mapping[str, int]] = None,
) -> Result:
    """Reads source files and assembles them as modules of one program.

    See assemble_many().
    """
    items = []
    for path in paths:
        with open(path, "rb") as f:
            items.append((path, f.read()))
    return assemble_many(items, cpu=cpu, origin=origin, include_dirs=include_dirs, sections=sections)
