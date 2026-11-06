import struct

import pytest

from deluge_mkimage import cpio, elf
import memmap
import uimage

PT_LOAD = 1


def phdrs(blob: bytes):
    e_phoff, = struct.unpack_from("<I", blob, 28)
    e_phnum, = struct.unpack_from("<H", blob, 44)
    out = []
    for i in range(e_phnum):
        p_type, p_offset, _, p_paddr, p_filesz, _, _, _ = struct.unpack_from(
            "<8I", blob, e_phoff + i * 32
        )
        if p_type == PT_LOAD:
            out.append((p_paddr, p_offset, p_filesz))
    return out


def test_one_pt_load_per_manifest_segment_at_the_mapped_addresses(
    fake_bundle, tmp_path
):
    segments = memmap.load(fake_bundle / "manifest.json")
    rootfs = tmp_path / "rootfs.cpio"
    rootfs.write_bytes(cpio.build({"usr/bin/deluge-app": (b"APP", 0o755)}))
    out = tmp_path / "SPARK.ELF"

    elf.pack(out, segments, fake_bundle / "boot" / "base", rootfs)

    loads = phdrs(out.read_bytes())
    assert len(loads) == len(segments)
    assert [addr for addr, _, _ in loads] == [s.load for s in segments]


def test_the_rootfs_segment_is_uimage_wrapped(fake_bundle, tmp_path):
    segments = memmap.load(fake_bundle / "manifest.json")
    rootfs = tmp_path / "rootfs.cpio"
    rootfs.write_bytes(cpio.build({"usr/bin/deluge-app": (b"APP", 0o755)}))
    out = tmp_path / "SPARK.ELF"

    elf.pack(out, segments, fake_bundle / "boot" / "base", rootfs)

    blob = out.read_bytes()
    _, offset, filesz = phdrs(blob)[-1]
    parsed = uimage.parse(blob[offset : offset + filesz])
    assert parsed["type"] == 3  # IH_TYPE_RAMDISK
    assert parsed["size"] == rootfs.stat().st_size


def test_a_rootfs_over_max_size_is_a_hard_error(fake_bundle, tmp_path):
    segments = memmap.load(fake_bundle / "manifest.json")
    # Shrink the budget rather than inflating the rootfs to 16 MB.
    segments[-1] = memmap.Segment(
        file=segments[-1].file, load=segments[-1].load,
        max_size=0x100, generated=True,
    )
    rootfs = tmp_path / "rootfs.cpio"
    rootfs.write_bytes(b"\x00" * 1024)

    with pytest.raises(ValueError, match="exceeds max_size"):
        elf.pack(tmp_path / "x.ELF", segments, fake_bundle / "boot" / "base", rootfs)


def test_check_accepts_what_pack_produces(fake_bundle, tmp_path):
    segments = memmap.load(fake_bundle / "manifest.json")
    rootfs = tmp_path / "rootfs.cpio"
    rootfs.write_bytes(cpio.build({"usr/bin/deluge-app": (b"APP", 0o755)}))
    out = tmp_path / "SPARK.ELF"

    elf.pack(out, segments, fake_bundle / "boot" / "base", rootfs)

    assert elf.check(out, segments) is True


def test_check_rejects_a_corrupted_uimage_header(fake_bundle, tmp_path):
    segments = memmap.load(fake_bundle / "manifest.json")
    rootfs = tmp_path / "rootfs.cpio"
    rootfs.write_bytes(cpio.build({"usr/bin/deluge-app": (b"APP", 0o755)}))
    out = tmp_path / "SPARK.ELF"
    elf.pack(out, segments, fake_bundle / "boot" / "base", rootfs)

    blob = bytearray(out.read_bytes())
    _, offset, _ = phdrs(bytes(blob))[-1]
    blob[offset + 20] ^= 0xFF
    out.write_bytes(bytes(blob))

    assert elf.check(out, segments) is False


def test_check_rejects_a_segment_outside_sdram_bounds(tmp_path):
    # A segment whose (packed) p_paddr sits at SDRAM_END is entirely outside
    # the [SDRAM_BASE, SDRAM_END) window `check` accepts.
    seg = memmap.Segment(file="a.bin", load=memmap.SDRAM_END)
    (tmp_path / "a.bin").write_bytes(b"\x01\x02\x03\x04")
    out = tmp_path / "x.ELF"

    # An explicit, in-bounds entry isolates the failure to the segment bounds
    # check rather than also tripping the entry-point check.
    elf.pack(out, [seg], tmp_path, rootfs_path=None, entry=memmap.SDRAM_BASE)

    assert elf.check(out, [seg]) is False


def test_check_rejects_overlapping_segments(tmp_path):
    a = memmap.Segment(file="a.bin", load=memmap.SDRAM_BASE)
    b = memmap.Segment(file="b.bin", load=memmap.SDRAM_BASE + 0x1000)
    (tmp_path / "a.bin").write_bytes(b"\x01" * 4)
    (tmp_path / "b.bin").write_bytes(b"\x02" * 4)
    out = tmp_path / "x.ELF"

    elf.pack(out, [a, b], tmp_path, rootfs_path=None, entry=a.load)

    # `pack` itself refuses to produce overlapping segments (see
    # test_a_rootfs_over_max_size_is_a_hard_error's sibling checks), so to
    # exercise `check`'s own overlap detection, patch b's on-disk p_paddr to
    # land on top of a after the fact.
    blob = bytearray(out.read_bytes())
    e_phoff, = struct.unpack_from("<I", blob, 28)
    p_paddr_off = e_phoff + 1 * 32 + 12  # 2nd PT_LOAD phdr, p_paddr is word 3
    struct.pack_into("<I", blob, p_paddr_off, a.load)
    out.write_bytes(bytes(blob))

    # Tell `check` to expect b at its new (overlapping) address too, so
    # "matches the map" still passes and the failure is isolated to overlap.
    b_moved = memmap.Segment(
        file=b.file, load=a.load, max_size=b.max_size, generated=b.generated
    )

    assert elf.check(out, [a, b_moved]) is False


def test_sdram_end_matches_device_ceiling():
    # Must equal the app-loader's SDRAM_HI in
    # deluge-sdk/crates/deluge-image/src/elf.rs; the top 2.875 MB is the SRAM
    # staging window, off-limits to app segments.
    assert memmap.SDRAM_END == 0x0FD20000


def test_rootfs_max_size_over_ceiling_is_rejected(tmp_path):
    # A rootfs at 0x0E800000 may be at most 0x0FD20000 - 0x0E800000 = 0x1520000;
    # one byte more must be rejected by memmap.load.
    import json
    doc = {
        "segments": [
            {"file": "rootfs.uimg", "load": "0x0E800000",
             "max_size": "0x1520001", "generated": True},
        ]
    }
    p = tmp_path / "map.json"
    p.write_text(json.dumps(doc))
    with pytest.raises(ValueError):
        memmap.load(p)


def test_uimage_format_matches_deluge_linux_byte_for_byte():
    # The golden bytes. deluge-linux's tests/test_uimage.py pins the same values;
    # if this fails, the two repos' formats have drifted and images built here
    # will not boot.
    blob = uimage.wrap_ramdisk(b"hello")
    assert len(blob) == 64 + 5
    assert struct.unpack_from(">I", blob, 0)[0] == 0x27051956
    assert struct.unpack_from(">I", blob, 12)[0] == 5  # ih_size
    parsed = uimage.parse(blob)
    assert (parsed["os"], parsed["arch"], parsed["type"], parsed["comp"]) == (5, 2, 3, 0)
    assert parsed["load"] == 0 and parsed["ep"] == 0
