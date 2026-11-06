// SPDX-License-Identifier: GPL-2.0+
/*
 * Board support for the Synthstrom Audible Deluge.
 * SoC: Renesas RZ/A1L (R7S72102), ARM Cortex-A9.
 *
 * The Deluge's preliminary bootloader (in SPI NOR) initialises CPG, BSC
 * and the 64 MB SDRAM on CS3 before handing off.  When U-Boot is loaded
 * via JLink ("warm" path), SDRAM is already live; we just declare it.
 *
 * NOR flash timing (CS0/CS1) is set here because U-Boot may need to read
 * SPI NOR via the parallel bus window at 0x18000000.
 *
 * Console: SCIF0 on P6_14 (RxD) / P6_15 (TxD), mux 5.
 * Physically accessible via the MIDI DIN 5-pin connector on the back panel:
 *   DIN pin 2 = GND
 *   DIN pin 4 = P6_14 RxD  (connect to adapter TX)
 *   DIN pin 5 = P6_15 TxD  (connect to adapter RX)
 * Use 115200 baud, 8N1.
 */

#include <init.h>
#include <asm/global_data.h>
#include <asm/io.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * RZ/A1 watchdog timer (WDT) registers.
 * The WDT requires magic-byte-prefixed 16-bit writes to modify its registers.
 * rza1-lowlevel_init.S normally disables it, but CONFIG_SKIP_LOWLEVEL_INIT
 * bypasses that.  Disable it here before DM init can take long enough to fire.
 */
#define RZA1_WDT_BASE	0xFCFE0000
#define WTCNT		(RZA1_WDT_BASE + 0x00)   /* WDT count register */
#define WTCSR		(RZA1_WDT_BASE + 0x02)   /* WDT control/status */
#define WTCNT_WRITE	0x5A00   /* magic prefix 0x5A | count 0x00 */
#define WTCSR_DISABLE	0xA518   /* magic prefix 0xA5 | stop WDT */

#define RZA1_BCR_BASE	0x3FFFC000
#define CS0BCR		(RZA1_BCR_BASE + 0x04)
#define CS0WCR		(RZA1_BCR_BASE + 0x28)
#define CS1BCR		(RZA1_BCR_BASE + 0x08)
#define CS1WCR		(RZA1_BCR_BASE + 0x2c)

int board_early_init_f(void)
{
	/* Disable the WDT immediately.  rza1-lowlevel_init.S normally does this
	 * but CONFIG_SKIP_LOWLEVEL_INIT bypasses it; if the Deluge firmware left
	 * the WDT running, it would fire during board_init_f and reset the CPU. */
	writew(WTCSR_DISABLE, WTCSR);
	writew(WTCNT_WRITE,   WTCNT);
	return 0;
}

int board_init(void)
{
	gd->bd->bi_boot_params = CFG_SYS_SDRAM_BASE + 0x100;

	/* NOR flash timing on CS0/CS1 (16-bit bus, 6 wait states).
	 * Values from rza1l-hal/src/bsc.rs in the Deluge firmware. */
	writel(0x00000b40, CS0WCR);
	writel(0x10000c00, CS0BCR);
	writel(0x00000b40, CS1WCR);
	writel(0x10000c00, CS1BCR);

	return 0;
}

/*
 * 64 MB external SDRAM on CS3, initialised by the preliminary bootloader.
 * U-Boot uses this as its working memory after relocation.
 */
int dram_init(void)
{
	gd->ram_base = CFG_SYS_SDRAM_BASE;
	gd->ram_size = CFG_SYS_SDRAM_SIZE;
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = CFG_SYS_SDRAM_BASE;
	gd->bd->bi_dram[0].size  = CFG_SYS_SDRAM_SIZE;
	return 0;
}
