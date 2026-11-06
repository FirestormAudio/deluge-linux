/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Configuration for the Synthstrom Audible Deluge
 * SoC: Renesas RZ/A1L (R7S72102)
 */

#ifndef __DELUGE_H
#define __DELUGE_H

/*
 * 64 MB external SDRAM on CS3, base 0x0C000000.
 * Pre-initialised by the Deluge's preliminary bootloader before U-Boot runs.
 */
#define CFG_SYS_SDRAM_BASE	0x0C000000
#define CFG_SYS_SDRAM_SIZE	(64 * 1024 * 1024)

/*
 * The app-loader ELF has already placed the ramdisk at the address in
 * memory-map.json. initrd_high=0xffffffff keeps it there ("use in place"):
 * without it U-Boot relocates the ramdisk to the top of RAM — where it has
 * itself relocated — and can land it on its own stack.
 *
 * Do NOT pin fdt_high the same way. U-Boot must be free to copy the DTB (with
 * SYS_FDT_PAD headroom) so it can add /chosen/linux,initrd-start/-end during
 * fixup. Pinned in place, the fixup fails NOSPACE in the tightly-packed 17 KB
 * DTB and the kernel boots with no initramfs -> "unable to mount root fs".
 * The relocated DTB lands at the top of RAM, clear of the ramdisk.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"initrd_high=0xffffffff\0"

#endif /* __DELUGE_H */
