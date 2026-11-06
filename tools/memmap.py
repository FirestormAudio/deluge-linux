#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Load and validate board/synthstrom/deluge/memory-map.json.

The map is the single source of truth for where the app-loader ELF's segments
land in SDRAM. `generated` marks the segment the packer is handed at run time
(the rootfs) rather than reading from disk alongside the others.
"""
import json
from dataclasses import dataclass
from pathlib import Path

SDRAM_BASE = 0x0C000000
# App-segment ceiling — must equal the app-loader's SDRAM_HI
# (deluge-sdk/crates/deluge-image/src/elf.rs). The top 2.875 MB
# (0x0FD20000..0x10000000) is the loader's SRAM staging window, not app-usable.
SDRAM_END = 0x0FD20000


@dataclass(frozen=True)
class Segment:
    file: str
    load: int
    max_size: int | None = None
    generated: bool = False


def _hex(value: str) -> int:
    return int(value, 16)


def load(path) -> list[Segment]:
    doc = json.loads(Path(path).read_text())
    segs = [
        Segment(
            file=s["file"],
            load=_hex(s["load"]),
            max_size=_hex(s["max_size"]) if "max_size" in s else None,
            generated=bool(s.get("generated", False)),
        )
        for s in doc["segments"]
    ]

    if sum(s.generated for s in segs) > 1:
        raise ValueError("at most one generated segment is allowed")

    for s in segs:
        if not (SDRAM_BASE <= s.load < SDRAM_END):
            raise ValueError(
                f"{s.file}: load 0x{s.load:08x} outside SDRAM "
                f"[0x{SDRAM_BASE:08x},0x{SDRAM_END:08x})"
            )
        if s.max_size is not None and s.load + s.max_size > SDRAM_END:
            raise ValueError(f"{s.file}: max_size runs outside SDRAM")

    # Overlap is only checkable for segments that declare a max_size; the others
    # are checked against their real length at pack time (see mk-app-elf.py).
    bounded = sorted(
        (s for s in segs if s.max_size is not None), key=lambda s: s.load
    )
    for a, b in zip(bounded, bounded[1:]):
        if a.load + a.max_size > b.load:
            raise ValueError(f"{a.file} and {b.file} overlap")

    return segs
