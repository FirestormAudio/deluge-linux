"""Read one ELF's dynamic linking metadata.

Hand-rolled with struct, like elf.py and appelf.py — deluge_mkimage carries no
third-party dependencies, because every app developer would inherit them.

Only what the closure check needs: the interpreter, DT_NEEDED, DT_SONAME and
the search path. Nothing here resolves anything; that is depcheck's job.
"""
import struct
from dataclasses import dataclass, field

PT_LOAD, PT_DYNAMIC, PT_INTERP = 1, 2, 3
DT_NULL, DT_NEEDED, DT_STRTAB, DT_SONAME, DT_RPATH, DT_RUNPATH = 0, 1, 5, 14, 15, 29


class MalformedELF(Exception):
    pass


@dataclass
class DynInfo:
    interp: str | None = None
    needed: list = field(default_factory=list)
    soname: str | None = None
    runpath: list = field(default_factory=list)


def _phdrs(data):
    """Yield (p_type, p_offset, p_vaddr, p_filesz) for each program header."""
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise MalformedELF("not a little-endian 32-bit ELF (ELFCLASS32/LSB)")
    try:
        e_phoff, = struct.unpack_from("<I", data, 28)
        e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
        for i in range(e_phnum):
            p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from(
                "<8I", data, e_phoff + i * e_phentsize)
            yield p_type, p_offset, p_vaddr, p_filesz
    except struct.error:
        raise MalformedELF("malformed program header table")


def _vaddr_to_offset(data, vaddr):
    """Map a virtual address into a file offset using the PT_LOAD segments.

    DT_STRTAB is recorded as a vaddr, but we are reading a file on disk, so it
    has to be translated through whichever segment contains it.
    """
    for p_type, p_offset, p_vaddr, p_filesz in _phdrs(data):
        if p_type == PT_LOAD and p_vaddr <= vaddr < p_vaddr + p_filesz:
            return p_offset + (vaddr - p_vaddr)
    raise MalformedELF(f"vaddr 0x{vaddr:x} is in no PT_LOAD segment")


def _cstr(data, off):
    end = data.find(b"\x00", off)
    if end < 0:
        raise MalformedELF("unterminated string in .dynstr")
    return data[off:end].decode()


def is_dynamic(data: bytes) -> bool:
    return any(p_type == PT_DYNAMIC for p_type, _, _, _ in _phdrs(data))


def parse(data: bytes) -> DynInfo:
    info = DynInfo()
    dyn_seg = None

    for p_type, p_offset, _, p_filesz in _phdrs(data):
        if p_type == PT_INTERP:
            info.interp = _cstr(data, p_offset)
        elif p_type == PT_DYNAMIC:
            dyn_seg = (p_offset, p_filesz)

    if dyn_seg is None:
        return info

    # Two passes: DT_STRTAB may appear after the entries that index into it.
    off, size = dyn_seg
    raw = []
    try:
        for i in range(size // 8):
            d_tag, d_val = struct.unpack_from("<iI", data, off + i * 8)
            if d_tag == DT_NULL:
                break
            raw.append((d_tag, d_val))
    except struct.error:
        raise MalformedELF("malformed dynamic section")

    strtab = next((v for t, v in raw if t == DT_STRTAB), None)
    if strtab is None:
        raise MalformedELF("dynamic section has no DT_STRTAB")
    base = _vaddr_to_offset(data, strtab)

    rpath = runpath = None
    for d_tag, d_val in raw:
        if d_tag == DT_NEEDED:
            info.needed.append(_cstr(data, base + d_val))
        elif d_tag == DT_SONAME:
            info.soname = _cstr(data, base + d_val)
        elif d_tag == DT_RPATH:
            rpath = _cstr(data, base + d_val)
        elif d_tag == DT_RUNPATH:
            runpath = _cstr(data, base + d_val)

    chosen = runpath if runpath is not None else rpath
    if chosen:
        info.runpath = [p for p in chosen.split(":") if p]
    return info
