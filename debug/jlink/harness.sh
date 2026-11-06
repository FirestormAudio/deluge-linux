#!/usr/bin/env bash
# harness.sh — drive U-Boot bring-up over JLink non-interactively.
#
# Lets an automated caller run scripted GDB experiments against the Deluge
# without a human at the keyboard. A physical power-cycle is only needed if
# JTAG is lost or SDRAM appears dead (the target must be warm — booted to the
# app-loader menu once — so its SDRAM/CPG are initialised).
#
# Subcommands:
#   server           Start the JLink GDB server (run this via run_in_background).
#   run <gdbfile>    Wait for the server port, then run `arm-none-eabi-gdb -batch`
#                    with the given command file. Honors $TIMEOUT (default 40s).
#   stop             Kill any running JLink GDB server.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GDB_PORT=2331
SRVLOG="$SCRIPT_DIR/.server.log"

case "${1:-}" in
  server)
    # tee to a fixed log so `run` can gate on the target actually attaching
    # ("Waiting for GDB connection") rather than just the port opening --
    # attaching gdb mid-connect races and wedges the JTAG link.
    JLinkGDBServerCLExe \
        -device R7S721020 \
        -if JTAG \
        -speed 4000 \
        -port "$GDB_PORT" \
        -jlinkscriptfile "$SCRIPT_DIR/rza1_debug.JLinkScript" \
        -semihosting \
        -nogui \
        -nohalt 2>&1 | tee "$SRVLOG"
    ;;
  run)
    [ -n "${2:-}" ] || { echo "usage: $0 run <gdbfile>"; exit 2; }
    # Wait for the target to be ATTACHED (not just the port open).
    timeout 25 bash -c 'until grep -q "Waiting for GDB connection\|Connected to target" "'"$SRVLOG"'" 2>/dev/null; do :; done' \
        || { echo "HARNESS: target never attached (see $SRVLOG)"; exit 3; }
    exec timeout "${TIMEOUT:-40}" arm-none-eabi-gdb -q -nx -batch -x "$2"
    ;;
  stop)
    pkill -f JLinkGDBServerCLExe && echo "HARNESS: server stopped" || echo "HARNESS: no server running"
    ;;
  *)
    echo "usage: $0 {server|run <gdbfile>|stop}"; exit 1 ;;
esac
