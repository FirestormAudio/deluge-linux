# `deluge.config` design notes

`deluge.config` is a "DO NOT EDIT" snapshot of the built kernel `.config` —
every time it's re-synced from a build, hand-authored comments inside it are
wiped out. This doc is the durable home for the *why* behind a handful of
non-obvious settings in that file, so the rationale survives future
snapshots. Update this file (not the generated config) when the reasoning
behind one of these settings changes.

## `CONFIG_BLK_DEV_INITRD`, `CONFIG_INITRAMFS_SOURCE`

The rootfs is not embedded in the kernel. It is handed over as an external
initrd by U-Boot (`bootz <zImage> <initrd> <dtb>`), so
`CONFIG_INITRAMFS_SOURCE` is empty — and with it empty, kconfig drops the
`INITRAMFS_COMPRESSION` choice entirely, which is correct: nothing is being
generated here to compress.

`BLK_DEV_INITRD` stays on; it is what lets the kernel accept the initrd
U-Boot passes. The `RD_*` decompressors stay so a compressed initrd remains
possible.

## `CONFIG_WATCHDOG`, `CONFIG_RENESAS_RZAWDT` (Hardware watchdog, RZ/A1 WDT)

Auto-reset on kernel/audio-thread hang. Note: the RZ/A1 WDT max timeout is very
short (~0.1-0.2s, DIVIDER_3BIT); a userspace pinger must ping faster than
that. Also gives a clean WDT-based restart handler for reboot.

## `CONFIG_FTRACE`, `CONFIG_FUNCTION_TRACER` (RT latency tracers, dev/tuning)

`FUNCTION_TRACER` grows the zImage (dev tool) — set to `n` for production
builds. osnoise/timerlat + cyclictest are the go-to for measuring worst-case
latency; irqsoff/preempt tracers pinpoint the offending code path.
