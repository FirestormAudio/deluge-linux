import struct

import pytest

from deluge_mkimage import dynelf

PT_LOAD, PT_DYNAMIC, PT_INTERP = 1, 2, 3
DT_NULL, DT_NEEDED, DT_STRTAB, DT_SONAME, DT_RPATH, DT_RUNPATH = 0, 1, 5, 14, 15, 29


def build_dynamic_elf(needed=(), soname=None, runpath=None, rpath=None,
                      interp="/lib/ld-musl-armhf.so.1", dynamic=True):
    """A synthetic ELF32 carrying a real PT_DYNAMIC and string table.

    Laid out with vaddr == file offset, so the vaddr->offset mapping the parser
    performs is exercised but trivially checkable.

    dynamic=False omits the PT_DYNAMIC segment entirely (needed/soname/runpath/
    rpath are then ignored), producing a binary that carries PT_INTERP but no
    PT_DYNAMIC -- the shape appelf.validate() tolerates under allow_dynamic via
    PT_INTERP alone, while dynelf.is_dynamic() (which keys on PT_DYNAMIC)
    reports False for it.
    """
    strtab = b"\x00"
    offsets = {}

    def intern(s):
        nonlocal strtab
        if s not in offsets:
            offsets[s] = len(strtab)
            strtab += s.encode() + b"\x00"
        return offsets[s]

    dyn = [(DT_NEEDED, intern(n)) for n in needed]
    if soname:
        dyn.append((DT_SONAME, intern(soname)))
    if rpath:
        dyn.append((DT_RPATH, intern(rpath)))
    if runpath:
        dyn.append((DT_RUNPATH, intern(runpath)))

    ehsize, phentsize = 52, 32
    phnum = 1 + (1 if interp else 0) + (1 if dynamic else 0)
    interp_bytes = (interp.encode() + b"\x00") if interp else b""

    cur = ehsize + phentsize * phnum
    interp_off = cur
    cur += len(interp_bytes)

    if dynamic:
        strtab_off = cur
        cur += len(strtab)
        dyn_off = cur

        dyn_full = dyn + [(DT_STRTAB, strtab_off), (DT_NULL, 0)]
        dyn_bytes = b"".join(struct.pack("<iI", t, v) for t, v in dyn_full)
    else:
        dyn_bytes = b""

    phdrs = b""
    if interp:
        phdrs += struct.pack("<8I", PT_INTERP, interp_off, interp_off,
                             interp_off, len(interp_bytes), len(interp_bytes), 4, 1)
    if dynamic:
        phdrs += struct.pack("<8I", PT_DYNAMIC, dyn_off, dyn_off, dyn_off,
                             len(dyn_bytes), len(dyn_bytes), 6, 4)
    phdrs += struct.pack("<8I", PT_LOAD, 0, 0, 0, cur + len(dyn_bytes),
                         cur + len(dyn_bytes), 5, 4)

    e_ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8
    ehdr = e_ident + struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0x8000, ehsize,
                                 0, 0x05000400, ehsize, phentsize, phnum, 0, 0, 0)
    if dynamic:
        return ehdr + phdrs + interp_bytes + strtab + dyn_bytes
    return ehdr + phdrs + interp_bytes


def test_reads_needed_libraries():
    info = dynelf.parse(build_dynamic_elf(needed=["libc.so", "libstdc++.so.6"]))
    assert info.needed == ["libc.so", "libstdc++.so.6"]


def test_reads_the_interpreter():
    info = dynelf.parse(build_dynamic_elf())
    assert info.interp == "/lib/ld-musl-armhf.so.1"


def test_reads_soname():
    info = dynelf.parse(build_dynamic_elf(soname="libplugin.so.1"))
    assert info.soname == "libplugin.so.1"


def test_runpath_is_split_on_colons():
    info = dynelf.parse(build_dynamic_elf(runpath="$ORIGIN/../lib/app:/usr/lib"))
    assert info.runpath == ["$ORIGIN/../lib/app", "/usr/lib"]


def test_runpath_wins_over_rpath():
    info = dynelf.parse(build_dynamic_elf(rpath="/old", runpath="/new"))
    assert info.runpath == ["/new"]


def test_rpath_used_when_no_runpath():
    info = dynelf.parse(build_dynamic_elf(rpath="/old"))
    assert info.runpath == ["/old"]


def test_static_binary_has_no_dynamic_section():
    from conftest import make_elf
    assert dynelf.is_dynamic(make_elf()) is False


def test_dynamic_binary_is_detected():
    assert dynelf.is_dynamic(build_dynamic_elf()) is True


def test_interp_without_dynamic_is_not_is_dynamic_but_has_interp():
    """The PT_INTERP-without-PT_DYNAMIC shape: appelf tolerates it via
    PT_INTERP, but dynelf.is_dynamic() (PT_DYNAMIC-keyed) disagrees."""
    data = build_dynamic_elf(dynamic=False)
    assert dynelf.is_dynamic(data) is False
    assert dynelf.parse(data).interp == "/lib/ld-musl-armhf.so.1"


def test_truncated_elf_raises():
    with pytest.raises(dynelf.MalformedELF):
        dynelf.parse(b"\x7fELF" + b"\x00" * 8)


def test_64bit_elf_raises():
    """64-bit ELF (EI_CLASS=2) should raise MalformedELF."""
    data = bytearray(build_dynamic_elf())
    data[4] = 2  # ELFCLASS64
    with pytest.raises(dynelf.MalformedELF):
        dynelf.parse(bytes(data))


def test_big_endian_elf_raises():
    """Big-endian ELF (EI_DATA=2) should raise MalformedELF."""
    data = bytearray(build_dynamic_elf())
    data[5] = 2  # ELFDATA2MSB
    with pytest.raises(dynelf.MalformedELF):
        dynelf.parse(bytes(data))
