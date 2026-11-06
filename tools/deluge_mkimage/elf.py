#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Mirrors deluge-linux/tools/mk-app-elf.py. The formats must stay
byte-identical; test_elf.py pins them.

Pack raw binaries as PT_LOAD segments into an ARM ET_EXEC ELF32 for the
Deluge app-loader.

Load addresses come from memory-map.json — never from the command line — so
there is exactly one place they can be wrong. The map's `generated` segment (the
rootfs) is supplied at run time with --rootfs and wrapped in a uImage ramdisk
header here.
"""
import struct
from pathlib import Path

import memmap
import uimage

EHSIZE, PHENTSIZE = 52, 32
ET_EXEC, EM_ARM, PT_LOAD = 2, 40, 1
PF_RWX = 7
EF_ARM_EABI_VER5 = 0x05000000


def _blobs(segments, artifact_dir, rootfs_path):
    """Resolve each segment to its bytes, applying the map's size budget."""
    out = []
    for seg in segments:
        if seg.generated:
            if rootfs_path is None:
                raise ValueError(f"{seg.file}: map needs a rootfs (--rootfs)")
            blob = uimage.wrap_ramdisk(Path(rootfs_path).read_bytes())
        else:
            blob = (Path(artifact_dir) / seg.file).read_bytes()

        if seg.max_size is not None and len(blob) > seg.max_size:
            raise ValueError(
                f"{seg.file}: {len(blob)} bytes exceeds max_size "
                f"{seg.max_size} (0x{seg.max_size:x})"
            )
        out.append((seg, blob))
    return out


def _check_overlap(resolved):
    """Segments without a max_size are only bounded by their real length."""
    spans = sorted(
        ((seg.load, seg.load + len(blob), seg.file) for seg, blob in resolved)
    )
    for (_, a_end, a_file), (b_start, _, b_file) in zip(spans, spans[1:]):
        if a_end > b_start:
            raise ValueError(
                f"{a_file} (ends 0x{a_end:08x}) and {b_file} "
                f"(starts 0x{b_start:08x}) overlap"
            )


def pack(out_path, segments, artifact_dir, rootfs_path=None, entry=None):
    resolved = _blobs(segments, artifact_dir, rootfs_path)
    _check_overlap(resolved)

    if entry is None:
        entry = resolved[0][0].load

    n = len(resolved)
    dataoff = EHSIZE + PHENTSIZE * n  # 52 + 32n is always 4-aligned
    phdrs, databuf, cur = b"", bytearray(), dataoff
    for seg, blob in resolved:
        pad = (-cur) & 3  # 4-align each segment's file offset
        databuf += b"\x00" * pad
        cur += pad
        phdrs += struct.pack(
            "<8I", PT_LOAD, cur, seg.load, seg.load,
            len(blob), len(blob), PF_RWX, 4,
        )
        databuf += blob
        cur += len(blob)

    e_ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\x00" * 8
    ehdr = e_ident + struct.pack(
        "<HHIIIIIHHHHHH",
        ET_EXEC, EM_ARM, 1, entry, EHSIZE, 0, EF_ARM_EABI_VER5,
        EHSIZE, PHENTSIZE, n, 0, 0, 0,
    )
    Path(out_path).write_bytes(ehdr + phdrs + bytes(databuf))
    total = len(ehdr) + len(phdrs) + len(databuf)
    print(f"wrote {out_path}: {n} segment(s), entry 0x{entry:08x}, {total} bytes")


def _fail(msg):
    print(f"FAIL: {msg}")
    return False


def _ok(msg):
    print(f"ok:   {msg}")
    return True


def check(elf_path, segments) -> bool:
    """Validate an ELF against the Deluge app-loader's acceptance checks and
    against memory-map.json's segments. Mirrors
    deluge-sdk/crates/deluge-image/src/elf.rs.
    """
    data = open(elf_path, "rb").read()
    good = True

    good &= _ok("magic") if data[:4] == b"\x7fELF" else _fail(f"magic {data[:4]!r}")
    good &= _ok("ELFCLASS32") if data[4] == 1 else _fail(f"EI_CLASS {data[4]}")
    good &= _ok("ELFDATA2LSB") if data[5] == 1 else _fail(f"EI_DATA {data[5]}")

    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    e_entry, e_phoff = struct.unpack_from("<II", data, 24)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    good &= _ok("ET_EXEC") if e_type == 2 else _fail(f"e_type {e_type}")
    good &= _ok("EM_ARM") if e_machine == 40 else _fail(f"e_machine {e_machine}")
    good &= _ok("e_phentsize==32") if e_phentsize == 32 else _fail(
        f"e_phentsize {e_phentsize}")
    good &= _ok(f"e_phnum={e_phnum}<=8") if e_phnum <= 8 else _fail(
        f"e_phnum {e_phnum}")

    loads = []
    for i in range(e_phnum):
        p_type, p_offset, _, p_paddr, p_filesz, p_memsz, _, _ = struct.unpack_from(
            "<8I", data, e_phoff + i * e_phentsize
        )
        if p_type == PT_LOAD:
            loads.append((p_paddr, p_offset, p_filesz, p_memsz))

    good &= (
        _ok(f"{len(loads)} PT_LOAD segment(s) — one per map entry")
        if len(loads) == len(segments)
        else _fail(f"{len(loads)} PT_LOAD segments, map has {len(segments)}")
    )

    for (p_paddr, p_offset, p_filesz, p_memsz), seg in zip(loads, segments):
        end = p_paddr + p_memsz
        good &= (
            _ok(f"{seg.file} @ 0x{p_paddr:08x} matches the map")
            if p_paddr == seg.load
            else _fail(f"{seg.file} @ 0x{p_paddr:08x}, map says 0x{seg.load:08x}")
        )
        good &= (
            _ok(f"{seg.file} end 0x{end:08x} in SDRAM")
            if memmap.SDRAM_BASE <= p_paddr and end <= memmap.SDRAM_END
            else _fail(f"{seg.file} [0x{p_paddr:08x},0x{end:08x}) outside SDRAM")
        )
        if seg.max_size is not None:
            good &= (
                _ok(f"{seg.file} {p_filesz} <= max_size {seg.max_size}")
                if p_filesz <= seg.max_size
                else _fail(f"{seg.file} {p_filesz} exceeds max_size {seg.max_size}")
            )
        if seg.generated:
            try:
                parsed = uimage.parse(data[p_offset : p_offset + p_filesz])
                good &= _ok(
                    f"{seg.file} uImage ramdisk header valid ({parsed['size']} B)")
            except ValueError as e:
                good &= _fail(f"{seg.file} uImage header: {e}")

    ends = [p_paddr + p_memsz for p_paddr, _, _, p_memsz in loads]
    starts = [p_paddr for p_paddr, _, _, _ in loads]
    overlap = any(e > s for e, s in zip(ends, starts[1:]))
    good &= _ok("no overlapping segments") if not overlap else _fail(
        "segments overlap")

    good &= (
        _ok(f"entry=0x{e_entry:08x}")
        if memmap.SDRAM_BASE <= e_entry < memmap.SDRAM_END
        else _fail(f"entry 0x{e_entry:08x} outside SDRAM")
    )

    print("ALL CHECKS PASSED" if good else "VALIDATION FAILED")
    return bool(good)
