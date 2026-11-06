#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Validate an ELF against the Deluge app-loader's acceptance checks and against
memory-map.json. Mirrors deluge-sdk/crates/deluge-image/src/elf.rs.

A thin CLI over deluge_mkimage.elf.check — the single validator shared with
deluge-mkimage. Exit 0 iff all pass.

Usage: check-app-elf.py --map MAP.json IMAGE.ELF
"""
import argparse
import sys

import memmap
from deluge_mkimage import elf


def check(elf_path, map_path) -> bool:
    """Validate `elf_path` against the segments in `map_path`."""
    return elf.check(elf_path, memmap.load(map_path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", required=True)
    ap.add_argument("elf")
    args = ap.parse_args()
    sys.exit(0 if check(args.elf, args.map) else 1)


if __name__ == "__main__":
    main()
