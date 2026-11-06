import json

import pytest

import memmap


def write_map(tmp_path, segments):
    p = tmp_path / "memory-map.json"
    p.write_text(json.dumps({"version": "0.2.0", "segments": segments}))
    return p


def test_loads_hex_strings_as_ints(tmp_path):
    p = write_map(tmp_path, [{"file": "zImage", "load": "0x0E000000"}])
    segs = memmap.load(p)
    assert segs[0].file == "zImage"
    assert segs[0].load == 0x0E000000
    assert segs[0].max_size is None
    assert segs[0].generated is False


def test_generated_and_max_size_round_trip(tmp_path):
    p = write_map(tmp_path, [
        {"file": "rootfs.uimg", "load": "0x0E800000",
         "max_size": "0x1000000", "generated": True},
    ])
    seg = memmap.load(p)[0]
    assert seg.generated is True
    assert seg.max_size == 0x1000000


def test_rejects_a_segment_outside_sdram(tmp_path):
    p = write_map(tmp_path, [{"file": "x", "load": "0x00000000"}])
    with pytest.raises(ValueError, match="outside SDRAM"):
        memmap.load(p)


def test_rejects_overlapping_segments_using_max_size(tmp_path):
    # zImage is capped at 8 MB starting at 0x0E000000, so it runs to 0x0E800000;
    # a rootfs at 0x0E7FFFFF would collide with it.
    p = write_map(tmp_path, [
        {"file": "zImage", "load": "0x0E000000", "max_size": "0x800000"},
        {"file": "rootfs.uimg", "load": "0x0E7FFFFF",
         "max_size": "0x1000", "generated": True},
    ])
    with pytest.raises(ValueError, match="overlap"):
        memmap.load(p)


def test_rejects_more_than_one_generated_segment(tmp_path):
    p = write_map(tmp_path, [
        {"file": "a", "load": "0x0C000000", "generated": True},
        {"file": "b", "load": "0x0D000000", "generated": True},
    ])
    with pytest.raises(ValueError, match="one generated"):
        memmap.load(p)


def test_the_real_map_is_valid_and_has_a_generated_rootfs():
    from pathlib import Path
    root = Path(__file__).resolve().parent.parent
    segs = memmap.load(root / "board/synthstrom/deluge/memory-map.json")
    assert [s.file for s in segs] == [
        "u-boot.bin", "r7s72102-deluge.dtb", "zImage", "rootfs.uimg",
    ]
    rootfs = segs[-1]
    assert rootfs.generated is True
    assert rootfs.max_size == 0x1000000
    assert rootfs.load == 0x0E800000
