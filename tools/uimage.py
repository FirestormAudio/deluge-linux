#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Legacy U-Boot image header (`struct image_header`, include/image.h).

We wrap the rootfs cpio in a ramdisk header so U-Boot reads its length from the
image rather than from the boot command — which lets CONFIG_BOOTCOMMAND stay
static even though every app changes the rootfs size.

All header fields are big-endian, regardless of target endianness.
"""
import struct
import zlib

MAGIC = 0x27051956
HEADER_SIZE = 64
NAME_SIZE = 32

IH_OS_LINUX = 5
IH_ARCH_ARM = 2
IH_TYPE_RAMDISK = 3
IH_COMP_NONE = 0

# magic, hcrc, time, size, load, ep, dcrc, os, arch, type, comp
_FMT = ">7I4B"


def wrap_ramdisk(data: bytes, name: str = "deluge rootfs") -> bytes:
    """Return `data` prefixed with a 64-byte uImage ramdisk header.

    ih_load and ih_ep are left at 0: a ramdisk has no load address of its own.
    U-Boot is told not to relocate it (initrd_high=0xffffffff), so it is used
    where the app-loader already placed it.
    """
    label = name.encode()[: NAME_SIZE - 1].ljust(NAME_SIZE, b"\x00")
    # ih_time is 0 rather than time.time(): a fixed timestamp keeps the image
    # byte-reproducible, which is what makes the golden tests meaningful.
    hdr = bytearray(
        struct.pack(
            _FMT,
            MAGIC,
            0,  # hcrc, filled in below
            0,  # ih_time
            len(data),
            0,  # ih_load
            0,  # ih_ep
            zlib.crc32(data) & 0xFFFFFFFF,
            IH_OS_LINUX,
            IH_ARCH_ARM,
            IH_TYPE_RAMDISK,
            IH_COMP_NONE,
        )
        + label
    )
    struct.pack_into(">I", hdr, 4, zlib.crc32(bytes(hdr)) & 0xFFFFFFFF)
    return bytes(hdr) + data


def parse(blob: bytes) -> dict:
    """Parse and verify a uImage header. Raises ValueError if it is not one."""
    if len(blob) < HEADER_SIZE:
        raise ValueError(f"short header: {len(blob)} bytes")
    fields = struct.unpack_from(_FMT, blob, 0)
    magic, hcrc, time_, size, load, ep, dcrc, os_, arch, type_, comp = fields
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x} (expected 0x{MAGIC:08x})")

    hdr = bytearray(blob[:HEADER_SIZE])
    struct.pack_into(">I", hdr, 4, 0)
    if zlib.crc32(bytes(hdr)) & 0xFFFFFFFF != hcrc:
        raise ValueError("header CRC mismatch")

    name = blob[HEADER_SIZE - NAME_SIZE : HEADER_SIZE].split(b"\x00", 1)[0]
    return {
        "magic": magic, "hcrc": hcrc, "time": time_, "size": size,
        "load": load, "ep": ep, "dcrc": dcrc, "os": os_, "arch": arch,
        "type": type_, "comp": comp, "name": name.decode(),
    }
