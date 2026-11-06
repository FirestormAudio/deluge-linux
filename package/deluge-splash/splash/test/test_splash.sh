#!/bin/sh
# Exercises the splash CLI against temp files via SPLASH_FB / SPLASH_PCT.
set -e
BIN=build/splash
tmp=$(mktemp -d)
export SPLASH_FB="$tmp/fb" SPLASH_PCT="$tmp/pct"

"$BIN" 0
"$BIN" 50

# framebuffer file is exactly one frame (688 bytes)
sz=$(wc -c < "$SPLASH_FB")
[ "$sz" = "688" ] || { echo "FAIL: fb size $sz != 688"; exit 1; }

# state file holds the last target
[ "$(cat "$SPLASH_PCT")" = "50" ] || { echo "FAIL: pct $(cat "$SPLASH_PCT") != 50"; exit 1; }

# dump mode emits a P1 PBM of the right dimensions
"$BIN" --dump "$tmp/out.pbm" 50
head -1 "$tmp/out.pbm" | grep -q '^P1$'    || { echo "FAIL: not P1"; exit 1; }
sed -n 2p "$tmp/out.pbm" | grep -q '^128 43$' || { echo "FAIL: bad dims"; exit 1; }

# missing framebuffer path must NOT fail (never block boot)
SPLASH_FB="$tmp/nonexistent-dir/fb" "$BIN" 75
echo "PASS: test_splash"
