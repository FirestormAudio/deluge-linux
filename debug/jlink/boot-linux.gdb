# boot-linux.gdb — load U-Boot + zImage + DTB into SDRAM via JLink, boot
# U-Boot to its RTT prompt. Then (over the RTT console) run:
#     bootz 0x0e000000 - 0x0df00000
# The kernel's RTT earlycon takes over the same SRAM block (0x20080000).
#
# Memory map (all in the 64 MB SDRAM at 0x0C000000):
#   0x0C000000  U-Boot load / kernel decompress target (ZRELADDR 0x0C008000)
#   0x0DF00000  device tree blob
#   0x0E000000  zImage (decompresses down to 0x0C008000; no overlap)
#   0x0E800000  rootfs initrd (not loaded by this script)
set pagination off
set confirm off
set architecture arm
target remote :2331
monitor reset
monitor cp15 1, 0, 0, 0 = 0x00C50078
monitor cp15 12, 0, 0, 0 = 0x0C000000
set $cpsr = 0x1D3
load out/u-boot
restore out/r7s72102-deluge.dtb binary 0x0df00000
restore out/zImage binary 0x0e000000
monitor exec SetRTTAddr 0x20080000
# Clear any STALE RTT down-buffer input before releasing the CPU. The control
# block lives in SRAM at 0x20080000 and survives a warm reset; serial_rtt's
# rtt_init() is idempotent (skips re-init when the "SEGGER RTT" magic is already
# present), so leftover down-ring offsets from a prior session persist. If
# down.rd_off != down.wr_off, U-Boot's pending() reports phantom keystrokes and
# aborts autoboot -> sits at => and never runs bootz. Zero both offsets here.
set {unsigned int}0x2008003c = 0
set {unsigned int}0x20080040 = 0
python
gdb.execute("continue")
import time
time.sleep(90)
end
