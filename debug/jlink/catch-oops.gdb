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
# vmlinux from the pinned kernel build (tracks the Linux version in the defconfig)
file .buildroot/output/build/linux-7.1/vmlinux
hbreak *0xc010c1c4
hbreak *0xc0109ea8
continue
printf "\n=== FAULT HANDLER HIT: PC=0x%08x ===\n", $pc
printf "r0=0x%08x r1=0x%08x lr=0x%08x\n", $r0, $r1, $lr
info symbol $lr
printf "--- backtrace ---\n"
bt
quit
