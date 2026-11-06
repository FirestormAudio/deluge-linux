#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Pack raw binaries as PT_LOAD segments into an ARM ET_EXEC ELF32 for the
Deluge app-loader.

A thin CLI over deluge_mkimage.elf — the single ELF-packing implementation
shared with deluge-mkimage. Load addresses come from memory-map.json (never the
command line); the map's `generated` segment (the rootfs) is supplied with
--rootfs and wrapped in a uImage ramdisk header.

Usage: mk-app-elf.py --map MAP.json --dir DIR [--rootfs FILE] OUT.ELF
"""
import argparse

import memmap
from deluge_mkimage.elf import pack  # the single implementation (re-exported)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--rootfs")
    ap.add_argument("--entry", type=lambda v: int(v, 16))
    ap.add_argument("out")
    args = ap.parse_args()
    pack(args.out, memmap.load(args.map), args.dir, args.rootfs, args.entry)


if __name__ == "__main__":
    main()
