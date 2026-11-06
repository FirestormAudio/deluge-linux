import json
import struct

import pytest

import check_app_elf
import memmap
import mk_app_elf
import uimage

PT_LOAD = 1


def build_map(tmp_path, rootfs_max="0x1000000"):
    p = tmp_path / "map.json"
    p.write_text(json.dumps({
        "version": "0.2.0",
        "segments": [
            {"file": "u-boot.bin", "load": "0x0C000000"},
            {"file": "zImage", "load": "0x0E000000"},
            {"file": "rootfs.uimg", "load": "0x0E800000",
             "max_size": rootfs_max, "generated": True},
        ],
    }))
    return p


def build_artifacts(tmp_path):
    d = tmp_path / "out"
    d.mkdir()
    (d / "u-boot.bin").write_bytes(b"UBOOT" * 10)
    (d / "zImage").write_bytes(b"ZIMAGE" * 10)
    rootfs = tmp_path / "rootfs.cpio"
    rootfs.write_bytes(b"CPIO" * 100)
    return d, rootfs


def phdrs(elf: bytes):
    e_phoff, = struct.unpack_from("<I", elf, 28)
    e_phnum, = struct.unpack_from("<H", elf, 44)
    out = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _, _ = \
            struct.unpack_from("<8I", elf, e_phoff + i * 32)
        if p_type == PT_LOAD:
            out.append((p_paddr, p_offset, p_filesz))
    return out


def test_packs_one_pt_load_per_segment_at_the_mapped_addresses(tmp_path):
    mp = build_map(tmp_path)
    d, rootfs = build_artifacts(tmp_path)
    out = tmp_path / "LINUX.ELF"

    mk_app_elf.pack(out, memmap.load(mp), d, rootfs_path=rootfs)

    elf = out.read_bytes()
    loads = phdrs(elf)
    assert [addr for addr, _, _ in loads] == [0x0C000000, 0x0E000000, 0x0E800000]


def test_the_generated_segment_is_the_rootfs_wrapped_in_a_uimage(tmp_path):
    mp = build_map(tmp_path)
    d, rootfs = build_artifacts(tmp_path)
    out = tmp_path / "LINUX.ELF"

    mk_app_elf.pack(out, memmap.load(mp), d, rootfs_path=rootfs)

    elf = out.read_bytes()
    _, offset, filesz = phdrs(elf)[2]
    blob = elf[offset : offset + filesz]
    parsed = uimage.parse(blob)
    assert parsed["type"] == 3  # IH_TYPE_RAMDISK
    assert parsed["size"] == rootfs.stat().st_size
    assert blob[uimage.HEADER_SIZE :] == rootfs.read_bytes()


def test_entry_defaults_to_the_first_segment(tmp_path):
    mp = build_map(tmp_path)
    d, rootfs = build_artifacts(tmp_path)
    out = tmp_path / "LINUX.ELF"

    mk_app_elf.pack(out, memmap.load(mp), d, rootfs_path=rootfs)

    e_entry, = struct.unpack_from("<I", out.read_bytes(), 24)
    assert e_entry == 0x0C000000


def test_a_rootfs_over_max_size_is_a_hard_error(tmp_path):
    # 256-byte budget; the cpio is 400 bytes + a 64-byte header.
    mp = build_map(tmp_path, rootfs_max="0x100")
    d, rootfs = build_artifacts(tmp_path)

    with pytest.raises(ValueError, match="exceeds max_size"):
        mk_app_elf.pack(tmp_path / "x.ELF", memmap.load(mp), d, rootfs_path=rootfs)


def test_a_generated_segment_with_no_rootfs_is_an_error(tmp_path):
    mp = build_map(tmp_path)
    d, _ = build_artifacts(tmp_path)

    with pytest.raises(ValueError, match="rootfs"):
        mk_app_elf.pack(tmp_path / "x.ELF", memmap.load(mp), d)


def test_an_unbounded_segment_that_runs_into_the_next_one_is_an_error(tmp_path):
    mp = build_map(tmp_path)
    d, rootfs = build_artifacts(tmp_path)
    # zImage at 0x0E000000 grown past the rootfs at 0x0E800000.
    (d / "zImage").write_bytes(b"\x00" * (0x800000 + 1))

    with pytest.raises(ValueError, match="overlap"):
        mk_app_elf.pack(tmp_path / "x.ELF", memmap.load(mp), d, rootfs_path=rootfs)


def test_check_accepts_what_pack_produces(tmp_path):
    mp = build_map(tmp_path)
    d, rootfs = build_artifacts(tmp_path)
    out = tmp_path / "LINUX.ELF"
    mk_app_elf.pack(out, memmap.load(mp), d, rootfs_path=rootfs)

    assert check_app_elf.check(out, mp) is True


def test_check_rejects_an_elf_whose_rootfs_uimg_is_corrupt(tmp_path):
    mp = build_map(tmp_path)
    d, rootfs = build_artifacts(tmp_path)
    out = tmp_path / "LINUX.ELF"
    mk_app_elf.pack(out, memmap.load(mp), d, rootfs_path=rootfs)

    elf = bytearray(out.read_bytes())
    _, offset, _ = phdrs(bytes(elf))[2]
    elf[offset + 20] ^= 0xFF  # corrupt the uImage header
    out.write_bytes(bytes(elf))

    assert check_app_elf.check(out, mp) is False
