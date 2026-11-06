import struct
import zlib

import pytest

import uimage


def test_header_is_64_bytes_and_precedes_the_data():
    blob = uimage.wrap_ramdisk(b"hello")
    assert len(blob) == 64 + 5
    assert blob[64:] == b"hello"


def test_magic_and_fields_are_big_endian():
    blob = uimage.wrap_ramdisk(b"x" * 100)
    magic, = struct.unpack_from(">I", blob, 0)
    assert magic == 0x27051956
    size, = struct.unpack_from(">I", blob, 12)
    assert size == 100


def test_ramdisk_type_os_arch_and_no_compression():
    parsed = uimage.parse(uimage.wrap_ramdisk(b"x"))
    assert parsed["os"] == 5     # IH_OS_LINUX
    assert parsed["arch"] == 2   # IH_ARCH_ARM
    assert parsed["type"] == 3   # IH_TYPE_RAMDISK
    assert parsed["comp"] == 0   # IH_COMP_NONE


def test_load_and_entry_are_zero_so_u_boot_uses_the_ramdisk_in_place():
    parsed = uimage.parse(uimage.wrap_ramdisk(b"x"))
    assert parsed["load"] == 0
    assert parsed["ep"] == 0


def test_data_crc_covers_the_payload():
    payload = b"the rootfs"
    parsed = uimage.parse(uimage.wrap_ramdisk(payload))
    assert parsed["dcrc"] == zlib.crc32(payload) & 0xFFFFFFFF


def test_header_crc_is_computed_with_the_hcrc_field_zeroed():
    blob = uimage.wrap_ramdisk(b"x")
    hdr = bytearray(blob[:64])
    stored, = struct.unpack_from(">I", hdr, 4)
    struct.pack_into(">I", hdr, 4, 0)
    assert stored == zlib.crc32(bytes(hdr)) & 0xFFFFFFFF


def test_name_is_nul_padded_to_32_bytes_and_truncated_if_too_long():
    parsed = uimage.parse(uimage.wrap_ramdisk(b"x", name="a" * 50))
    assert parsed["name"] == "a" * 31  # 31 chars + NUL


def test_parse_rejects_a_corrupt_header():
    blob = bytearray(uimage.wrap_ramdisk(b"x"))
    blob[20] ^= 0xFF  # flip a bit in ih_load, invalidating hcrc
    with pytest.raises(ValueError, match="header CRC"):
        uimage.parse(bytes(blob))


def test_parse_rejects_bad_magic():
    with pytest.raises(ValueError, match="magic"):
        uimage.parse(b"\x00" * 64)
