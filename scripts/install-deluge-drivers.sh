#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Install the out-of-tree Deluge drivers, header, DTS, Kconfig stanzas and
# Makefile hooks into a kernel tree. Idempotent: safe to re-run.
#
# Usage: scripts/install-deluge-drivers.sh <kernel-tree>
#
# Runs as LINUX_PRE_PATCH_HOOKS from external.mk; Buildroot calls it with its
# freshly-extracted kernel tree. The config is NOT applied here — Buildroot applies
# board/synthstrom/deluge/kernel/deluge.config itself
# (BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE).
set -euo pipefail
TREE="${1:?usage: install-deluge-drivers.sh <kernel-tree>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
K="$REPO/board/synthstrom/deluge/kernel"

copy() { install -D -m0644 "$1" "$2"; echo "  copied $(basename "$1") -> ${2#$TREE/}"; }

# ASoC SH/Renesas drivers live under sound/soc/renesas on 7.x. Locate the dir
# by the canonical rz-ssi.c so we survive any future tree reshuffle.
SND="$(cd "$TREE" && d=$(find sound/soc -maxdepth 2 -name rz-ssi.c 2>/dev/null | head -1); echo "${d%/rz-ssi.c}")"
[ -n "$SND" ] && [ -d "$TREE/$SND" ] || { echo "ERROR: cannot locate rz-ssi.c dir under $TREE/sound/soc" >&2; exit 1; }
echo "  ASoC sound dir: $SND"

# --- driver .c -> subdir ---
declare -A DST=(
  [deluge-cv.c]=drivers/iio/dac
  [deluge-encoders.c]=drivers/input/misc [deluge-pic.c]=drivers/input/misc [deluge-clkin.c]=drivers/input/misc
  [deluge-oled.c]=drivers/video/fbdev [deluge-pad.c]=drivers/video/fbdev
  [leds-deluge.c]=drivers/leds
  [deluge-sram.c]=drivers/misc
)
DST[deluge-ssi.c]=$SND; DST[deluge-audio.c]=$SND; DST[deluge-midi.c]=$SND
for c in "${!DST[@]}"; do copy "$K/drivers/$c" "$TREE/${DST[$c]}/$c"; done

# --- header + DTS ---
copy "$K/include/deluge-pic.h" "$TREE/include/linux/deluge-pic.h"
copy "$K/include/uapi/deluge-sram.h" "$TREE/include/uapi/linux/deluge-sram.h"
copy "$K/include/deluge-rtt.h" "$TREE/include/linux/deluge-rtt.h"
copy "$K/include/deluge-scif.h" "$TREE/include/linux/deluge-scif.h"
copy "$K/dts/r7s72102-deluge.dts" "$TREE/arch/arm/boot/dts/renesas/r7s72102-deluge.dts"

# --- console drivers (SEGGER RTT earlycon + hvc), live at kernel/ root ---
copy "$K/hvc_rtt.c"      "$TREE/drivers/tty/hvc/hvc_rtt.c"
copy "$K/earlycon-rtt.c" "$TREE/drivers/tty/serial/earlycon-rtt.c"

# --- Makefile obj lines (append if absent) ---
addmk() { # <subdir> <line> — fail loudly if the target Makefile is missing
  [ -f "$TREE/$1/Makefile" ] || { echo "ERROR: $1/Makefile missing in $TREE (tree layout changed?)" >&2; exit 1; }
  grep -qF "$2" "$TREE/$1/Makefile" || { echo "$2" >> "$TREE/$1/Makefile"; echo "  Makefile += $2"; }; }
addmk "$SND" 'obj-$(CONFIG_SND_SOC_DELUGE_SSI)	+= deluge-ssi.o'
addmk "$SND" 'obj-$(CONFIG_SND_SOC_DELUGE_AUDIO)	+= deluge-audio.o'
addmk "$SND" 'obj-$(CONFIG_SND_SOC_DELUGE_MIDI)	+= deluge-midi.o'
addmk drivers/iio/dac 'obj-$(CONFIG_DELUGE_CV) += deluge-cv.o'
addmk drivers/input/misc 'obj-$(CONFIG_INPUT_DELUGE_ENCODERS)	+= deluge-encoders.o'
addmk drivers/input/misc 'obj-$(CONFIG_INPUT_DELUGE_PIC)		+= deluge-pic.o'
addmk drivers/input/misc 'obj-$(CONFIG_INPUT_DELUGE_CLKIN)	+= deluge-clkin.o'
addmk drivers/video/fbdev 'obj-$(CONFIG_FB_DELUGE_OLED)	  += deluge-oled.o'
addmk drivers/video/fbdev 'obj-$(CONFIG_FB_DELUGE_PADS)	  += deluge-pad.o'
addmk drivers/leds 'obj-$(CONFIG_LEDS_DELUGE)		+= leds-deluge.o'
addmk drivers/misc 'obj-$(CONFIG_DELUGE_SRAM)		+= deluge-sram.o'
addmk drivers/tty/hvc 'obj-$(CONFIG_HVC_RTT)		+= hvc_rtt.o'
addmk drivers/tty/serial 'obj-$(CONFIG_SERIAL_EARLYCON_RTT) += earlycon-rtt.o'

# --- Kconfig stanzas (insert before the last endmenu if the symbol is absent) ---
addkc() { # <subdir> <fragment-file> <sentinel-symbol>
  local kc="$TREE/$1/Kconfig"
  [ -f "$kc" ] || { echo "ERROR: $1/Kconfig missing in $TREE (tree layout changed?)" >&2; exit 1; }
  grep -qE "^config $3\\b" "$kc" && return 0
  if grep -q '^endmenu' "$kc"; then
    awk -v frag="$(cat "$K/kconfig/$2")" '
      {lines[NR]=$0} /^endmenu/{last=NR}
      END{ for(i=1;i<=NR;i++){ if(i==last){print frag; print ""} print lines[i] } }' "$kc" > "$kc.tmp"
    mv "$kc.tmp" "$kc"
  else
    { echo; cat "$K/kconfig/$2"; } >> "$kc"
  fi
  echo "  Kconfig += $3 (+ siblings) in $1"
}
addkc "$SND" sound-sh.kconfig SND_SOC_DELUGE_SSI
addkc drivers/iio/dac iio-dac.kconfig DELUGE_CV
addkc drivers/input/misc input-misc.kconfig INPUT_DELUGE_ENCODERS
addkc drivers/video/fbdev fbdev.kconfig FB_DELUGE_OLED
addkc drivers/leds leds.kconfig LEDS_DELUGE
addkc drivers/misc misc.kconfig DELUGE_SRAM
addkc drivers/tty/hvc hvc-rtt.kconfig HVC_RTT
addkc drivers/tty/serial serial-rtt.kconfig SERIAL_EARLYCON_RTT

# --- out-of-tree patches to mainline drivers (idempotent via a sentinel grep;
# fail loud on tree drift) ---
# NOTE: use patch(1), never `git apply`. When the kernel tree sits inside a git
# work tree (it does — Buildroot builds under .buildroot/, which is inside this
# repo), `git apply` resolves patch paths against the *repository root* rather
# than the tree, silently skips every hunk ("Skipped patch ...") and still exits
# 0. That reports success while applying nothing. patch(1) is repo-agnostic and
# exits non-zero when a hunk does not apply.
apply_patch() { # <patch-file> <sentinel-regex> <target-relpath>
  if grep -qE "$2" "$TREE/$3" 2>/dev/null; then
    echo "  patch already applied: $1"
  elif ( cd "$TREE" && patch -p1 --forward --silent < "$K/patches/$1" ); then
    echo "  patched $3 <- $1"
    # Belt and braces: the sentinel MUST be present now. If it isn't, the patch
    # applied somewhere unexpected and the build would silently lose the fix.
    grep -qE "$2" "$TREE/$3" || {
      echo "ERROR: $1 reported success but $3 lacks '$2'" >&2; exit 1; }
  else
    echo "ERROR: failed to apply $1 to $TREE/$3 (kernel tree drifted?)" >&2
    exit 1
  fi
}

# renesas_usbhs: deliver ->resume on bus resume (handle the RESM interrupt).
# 7.1's u_serial gates RX/TX on the port's suspended flag, cleared only by
# gserial_resume(); the UDC delivered ->suspend but never ->resume, so the
# CDC-ACM console wedged after the host autosuspended it (~2 s idle).
apply_patch renesas-usbhs-resume.patch 'USBHSG_STATUS_SUSPENDED' drivers/usb/renesas_usbhs/mod_gadget.c

# renesas_usbhs: manual host<->peripheral role switch via the "role" sysfs
# attribute (Deluge single jack; VBUS sense strapped high defeats autonomous
# hotplug, so the switch is driven explicitly). See docs/hardware.md.
apply_patch renesas-usbhs-role-switch.patch 'usbhs_role_switch' drivers/usb/renesas_usbhs/common.c

# f_uac2: implicit-feedback mode so async full-duplex needs only 2 ISO endpoints
# (playback OUT + capture IN, capture doubling as the sync source) instead of 3.
# The RZ/A1 usbhsc pipe table has only 2 ISO-capable pipes, so the mainline
# dedicated-feedback layout can't bind alongside f_midi2/CDC. Proven on hardware
# (composite enumerates: 1d6b:0104, ALSA "Deluge" card).
apply_patch renesas-uac2-implicit-fback.patch 'IMPLICIT_FBACK_EN' drivers/usb/gadget/function/f_uac2.c

# f_midi2: use a BULK (not INTERRUPT) MIDI-2.0 IN endpoint. The RZ/A1
# renesas_usbhs UDC can't service an interrupt IN pipe (irq_ready run_error
# -EBUSY), which silently stalls process_ump_transmit — MIDI-2.0 data never
# leaves the device. Bulk works on this UDC. Proven on hardware.
apply_patch f_midi2-bulk-in-ep.patch 'Deluge: BULK, not INTERRUPT' drivers/usb/gadget/function/f_midi2.c

# --- DTS build hook ---
MK="$TREE/arch/arm/boot/dts/renesas/Makefile"
grep -qF 'r7s72102-deluge.dtb' "$MK" || {
  echo 'dtb-$(CONFIG_ARCH_R7S72100) += r7s72102-deluge.dtb' >> "$MK"
  echo "  DTS Makefile += r7s72102-deluge.dtb"
}
echo "install-deluge-drivers: done -> $TREE"
