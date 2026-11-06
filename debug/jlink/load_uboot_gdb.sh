#!/usr/bin/env bash
# load_uboot_gdb.sh
#
# Load U-Boot into SDRAM via JLink GDB server and capture semihosting output.
#
# Semihosting routes U-Boot's console output back through the JLink debug
# connection — no physical UART needed.  Output appears in the GDB server
# terminal (Terminal A below).
#
# Prerequisites:
#   JLinkGDBServerCLExe  (SEGGER J-Link Software)
#   arm-none-eabi-gdb
#   out/u-boot.bin  (from `make base` or `make platform`)
#
# Usage:
#   Terminal A (GDB server + semihosting output):
#     ./jlink/load_uboot_gdb.sh server
#
#   Terminal B (GDB load + run):
#     ./jlink/load_uboot_gdb.sh load
#
# Or run both in one tmux/screen session:
#     ./jlink/load_uboot_gdb.sh both     (opens two panes if tmux is available)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
UBOOT_ELF="$REPO_DIR/src/u-boot/u-boot"
GDB="arm-none-eabi-gdb"
GDB_PORT=2331

start_server() {
    echo "Starting JLink GDB server with semihosting..."
    echo "U-Boot output will appear here once 'load_uboot_gdb.sh load' runs."
    echo ""
    exec JLinkGDBServerCLExe \
        -device R7S721020 \
        -if JTAG \
        -speed 4000 \
        -port "$GDB_PORT" \
        -jlinkscriptfile "$SCRIPT_DIR/rza1_debug.JLinkScript" \
        -semihosting \
        -nogui \
        -nohalt
}

_cp15_setup=(
    # Halt + clean CPU state (mirrors embassy launch.json overrideLaunchCommands).
    # SCTLR = 0x00C50078: MMU off (bit 0), D-cache off (bit 2), I-cache off (bit 12).
    # Without this the existing firmware's MMU/TLB is still active when U-Boot
    # starts and cache-maintenance instructions fault on stale TLB entries.
    "-ex" "monitor reset"
    "-ex" "monitor cp15 1, 0, 0, 0 = 0x00C50078"
    # VBAR = 0x0C000000: point ARM exception vectors at U-Boot's vector table.
    # (Embassy uses 0x20000000 for SRAM; we use SDRAM base.)
    "-ex" "monitor cp15 12, 0, 0, 0 = 0x0C000000"
    # CPSR: supervisor mode, ARM (not Thumb), IRQ+FIQ+async-abort masked.
    "-ex" "set \$cpsr = 0x1D3"
)

load_and_run() {
    if [ ! -f "$UBOOT_ELF" ]; then
        echo "ERROR: $UBOOT_ELF not found — build U-Boot first"
        exit 1
    fi

    echo "Connecting to JLink GDB server on :$GDB_PORT ..."
    "$GDB" -batch \
        -ex "set architecture arm" \
        -ex "target remote :$GDB_PORT" \
        "${_cp15_setup[@]}" \
        -ex "monitor semihosting enable" \
        -ex "monitor semihosting IOClient 3" \
        -ex "load" \
        -ex "continue" \
        "$UBOOT_ELF"
}

# To drop to interactive GDB instead of -batch (useful for debugging):
load_interactive() {
    if [ ! -f "$UBOOT_ELF" ]; then
        echo "ERROR: $UBOOT_ELF not found"
        exit 1
    fi

    "$GDB" \
        -ex "set architecture arm" \
        -ex "target remote :$GDB_PORT" \
        "${_cp15_setup[@]}" \
        -ex "monitor semihosting enable" \
        -ex "monitor semihosting IOClient 3" \
        -ex "load" \
        "$UBOOT_ELF"
    # At the (gdb) prompt: 'continue' to run, 'c' to resume after a break.
}

start_tmux() {
    if ! command -v tmux &>/dev/null; then
        echo "tmux not found — run 'server' and 'load' in separate terminals."
        exit 1
    fi

    SESSION="deluge-uboot"
    tmux new-session -d -s "$SESSION" -x 220 -y 50

    # Left pane: GDB server (semihosting output)
    tmux send-keys -t "$SESSION" \
        "cd '$REPO_DIR' && ./jlink/load_uboot_gdb.sh server" Enter

    # Right pane: GDB load (wait briefly for server to start)
    tmux split-window -h -t "$SESSION"
    tmux send-keys -t "$SESSION" \
        "sleep 2 && cd '$REPO_DIR' && ./jlink/load_uboot_gdb.sh interactive" Enter

    tmux attach-session -t "$SESSION"
}

case "${1:-help}" in
    server)      start_server ;;
    load)        load_and_run ;;
    interactive) load_interactive ;;
    both)        start_tmux ;;
    help|*)
        echo "Usage: $0 {server|load|interactive|both}"
        echo ""
        echo "  server      — start JLink GDB server (semihosting output appears here)"
        echo "  load        — load u-boot.bin and run (non-interactive)"
        echo "  interactive — load and drop to GDB prompt"
        echo "  both        — open tmux with server + interactive side by side"
        ;;
esac
