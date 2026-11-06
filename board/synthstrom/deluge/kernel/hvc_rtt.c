// SPDX-License-Identifier: GPL-2.0
/*
 * Bidirectional SEGGER RTT console for the Synthstrom Deluge (RZ/A1L).
 *
 * A hypervisor-console (hvc) backend that carries the interactive kernel
 * console over a SEGGER RTT control block in on-chip SRAM, read/written by a
 * J-Link probe.  The hvc layer provides the tty + polling; this just moves
 * bytes to/from the RTT ring buffers.  Pairs with earlycon-rtt.c (early
 * output) and U-Boot's serial_rtt.c, all sharing the block at 0x20080000.
 *
 * Use with:  console=hvc0   (and earlycon=rtt,0x20080000 for early output)
 *
 * Control block layout (matches serial_rtt.c / earlycon-rtt.c):
 *   0x00 id[16]
 *   0x18 up[0]   { name, buffer, size, wr_off(+12), rd_off(+16), flags }
 *   0x30 down[0] { name, buffer, size, wr_off(+12), rd_off(+16), flags }
 *   up  ring buffer @ CB+0x100 (1024 B, target->host)
 *   down ring buffer @ CB+0x600 (256 B, host->target)
 */

#include <linux/console.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/types.h>

#include "hvc_console.h"
#include <linux/deluge-rtt.h>

static void __iomem *rtt_cb;

static ssize_t hvc_rtt_put_chars(uint32_t vt, const u8 *buf, size_t count)
{
	size_t i;

	if (!rtt_cb)
		return count;		/* pre-ioremap: pretend consumed */

	for (i = 0; i < count; i++) {
		u32 wr = readl(rtt_cb + DELUGE_RTT_UP_WROFF);
		u32 rd = readl(rtt_cb + DELUGE_RTT_UP_RDOFF);
		u32 nxt = (wr + 1) & (DELUGE_RTT_UP_SIZE - 1);

		if (nxt == rd)		/* ring full: stop, hvc will retry */
			break;
		writeb(buf[i], rtt_cb + DELUGE_RTT_UP_BUF + wr);
		writel(nxt, rtt_cb + DELUGE_RTT_UP_WROFF);
	}
	return i;
}

static ssize_t hvc_rtt_get_chars(uint32_t vt, u8 *buf, size_t count)
{
	size_t i;

	if (!rtt_cb)
		return 0;

	for (i = 0; i < count; i++) {
		u32 rd = readl(rtt_cb + DELUGE_RTT_DOWN_RDOFF);
		u32 wr = readl(rtt_cb + DELUGE_RTT_DOWN_WROFF);

		if (rd == wr)		/* nothing from host */
			break;
		buf[i] = readb(rtt_cb + DELUGE_RTT_DOWN_BUF + rd);
		writel((rd + 1) & (DELUGE_RTT_DOWN_SIZE - 1), rtt_cb + DELUGE_RTT_DOWN_RDOFF);
	}
	return i;
}

static const struct hv_ops hvc_rtt_ops = {
	.get_chars = hvc_rtt_get_chars,
	.put_chars = hvc_rtt_put_chars,
};

static int __init hvc_rtt_console_init(void)
{
	rtt_cb = ioremap(DELUGE_RTT_BASE, DELUGE_RTT_MAP);
	if (!rtt_cb)
		return -ENOMEM;
	return hvc_instantiate(0, 0, &hvc_rtt_ops);
}
console_initcall(hvc_rtt_console_init);

static int __init hvc_rtt_init(void)
{
	struct hvc_struct *hp;

	if (!rtt_cb) {
		rtt_cb = ioremap(DELUGE_RTT_BASE, DELUGE_RTT_MAP);
		if (!rtt_cb)
			return -ENOMEM;
	}
	hp = hvc_alloc(0, 0, &hvc_rtt_ops, 128);
	return PTR_ERR_OR_ZERO(hp);
}
device_initcall(hvc_rtt_init);
