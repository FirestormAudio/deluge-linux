#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Install the Deluge board support (new files + edits to stock u-boot files)
# into a U-Boot tree. Idempotent: safe to re-run.
#
# Usage: scripts/install-uboot-board.sh <u-boot-tree>
#
# Mirrors scripts/install-deluge-drivers.sh (the kernel equivalent). Runs as
# UBOOT_PRE_PATCH_HOOKS from external.mk. Buildroot's U-Boot tree is a throwaway
# upstream checkout; ALL Deluge customisation lives in
# board/synthstrom/deluge/uboot/ and is applied by this script. The defconfig is NOT
# applied here — Buildroot selects it (BR2_TARGET_UBOOT_BOARD_DEFCONFIG="deluge").
set -euo pipefail
TREE="${1:?usage: install-uboot-board.sh <u-boot-tree>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
U="$REPO/board/synthstrom/deluge/uboot"

[ -f "$TREE/Makefile" ] && [ -d "$TREE/board" ] || {
	echo "ERROR: $TREE does not look like a U-Boot tree" >&2; exit 1; }

# --- new Deluge files: uboot/ mirrors the real tree paths, so copy 1:1 ---
copy() { install -D -m0644 "$U/$1" "$TREE/$1"; echo "  copied $1"; }
while IFS= read -r f; do [ -n "$f" ] && copy "$f"; done <<'EOF'
arch/arm/dts/r7s72102-deluge-u-boot.dtsi
board/renesas/deluge/deluge.c
board/renesas/deluge/Kconfig
board/renesas/deluge/MAINTAINERS
board/renesas/deluge/Makefile
board/renesas/deluge/save_boot_params.S
configs/deluge_defconfig
dts/upstream/src/arm/renesas/r7s72102-deluge.dts
include/configs/deluge.h
drivers/serial/serial_rtt.c
EOF

# --- edits to stock u-boot files (board registration, RTT serial wiring,
#     dtb Makefile entry, JLink semihosting trap) as one patch ---
# NOTE: use patch(1), never `git apply` — see the same note in
# install-deluge-drivers.sh. `git apply` inside a git work tree (Buildroot builds
# under .buildroot/, inside this repo) silently skips every hunk and exits 0.
apply_patch() { # <patch-file> <sentinel-regex> <target-relpath>
	if grep -qE "$2" "$TREE/$3" 2>/dev/null; then
		echo "  patch already applied: $1"
	elif ( cd "$TREE" && patch -p1 --forward --silent < "$U/patches/$1" ); then
		echo "  patched <- $1"
		grep -qE "$2" "$TREE/$3" || {
			echo "ERROR: $1 reported success but $3 lacks '$2'" >&2; exit 1; }
	else
		echo "ERROR: failed to apply $1 to $TREE (u-boot tree drifted?)" >&2
		exit 1
	fi
}
apply_patch 0001-deluge-uboot-core-edits.patch 'serial_rtt' drivers/serial/Makefile

echo "install-uboot-board: done -> $TREE"
