// SPDX-License-Identifier: GPL-2.0
/*
 * SEGGER RTT earlycon for the Synthstrom Deluge (Renesas RZ/A1L).
 *
 * Emits kernel boot output into a SEGGER RTT control block at a fixed on-chip
 * SRAM address, read by a J-Link probe via background memory access -- the same
 * mechanism U-Boot uses (drivers/serial/serial_rtt.c).  U-Boot initialises the
 * control block and hands off to the kernel; the block lives in SRAM which the
 * kernel never touches (it runs from SDRAM), so it survives the handoff and
 * J-Link keeps draining it.
 *
 * Enable with:  earlycon=rtt,0x20080000   (plus CONFIG_SERIAL_EARLYCON=y)
 *
 * Control block layout (must match U-Boot's serial_rtt.c):
 *   0x00 char id[16]        ("SEGGER RTT")
 *   0x10 s32  max_up
 *   0x14 s32  max_down
 *   0x18 up[0]   { u32 name, buffer, size, wr_off, rd_off, flags }   (24 bytes)
 *   0x30 down[0] { ... }
 * The up ring buffer sits at CB + 0x100 (phys 0x20080100), size 1024.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <linux/deluge-rtt.h>

static void rtt_putc(struct uart_port *port, unsigned char c)
{
	void __iomem *cb = port->membase;
	u32 wr = readl(cb + DELUGE_RTT_UP_WROFF);
	u32 rd = readl(cb + DELUGE_RTT_UP_RDOFF);
	u32 nxt = (wr + 1) & (DELUGE_RTT_UP_SIZE - 1);

	if (nxt == rd)			/* ring full: drop (never block the boot) */
		return;
	writeb(c, cb + DELUGE_RTT_UP_BUF + wr);
	writel(nxt, cb + DELUGE_RTT_UP_WROFF);
}

static void rtt_early_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, rtt_putc);
}

static int __init rtt_early_setup(struct earlycon_device *dev, const char *opt)
{
	if (!dev->port.membase)		/* need earlycon=rtt,<addr> */
		return -ENODEV;
	dev->con->write = rtt_early_write;
	return 0;
}
EARLYCON_DECLARE(rtt, rtt_early_setup);
