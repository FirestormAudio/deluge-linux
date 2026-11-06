// SPDX-License-Identifier: GPL-2.0+
/*
 * SEGGER RTT serial console for U-Boot (Synthstrom Deluge / RZ/A1L bring-up).
 *
 * The Deluge has no accessible debug UART and J-Link semihosting is unusable as
 * a U-Boot console (it is tied to the SVC exception vector, which U-Boot moves
 * during relocation, and it collides with debugger breakpoints).  SEGGER RTT
 * instead uses a control block + ring buffers in RAM that J-Link reads/writes by
 * background memory access -- no CPU halt, no exception vector, no breakpoints.
 *
 * The control block is placed at a FIXED on-chip SRAM address.  U-Boot executes
 * from external SDRAM (0x0C000000) and relocates within it, but never touches
 * the RZ/A1L SRAM (0x20000000..0x202FFFFF), so the block never moves -- the
 * console works identically before and after relocation, and is interactive.
 *
 * Point J-Link at the block explicitly (it must not scan): in the GDB Server,
 *   monitor exec SetRTTAddr 0x20080000
 * then read/write it via the RTT telnet channel (JLinkRTTClient / port 19021).
 */

#include <dm.h>
#include <serial.h>
#include <linux/types.h>
#include <linux/errno.h>

/*
 * Fixed SRAM layout (non-retention region, >= 0x20020000; unused by U-Boot).
 *   0x20080000  control block (72 bytes)
 *   0x20080048  "Terminal" name string
 *   0x20080100  up ring buffer   (target -> host), 1024 bytes
 *   0x20080600  down ring buffer (host -> target),  256 bytes
 */
#define RTT_BASE	0x20080000UL
#define RTT_NAME	(RTT_BASE + 0x48)
#define RTT_UP_BUF	(RTT_BASE + 0x100)
#define RTT_UP_SIZE	1024U
#define RTT_DOWN_BUF	(RTT_BASE + 0x600)
#define RTT_DOWN_SIZE	256U

/* SEGGER RTT buffer flags */
#define RTT_FLAG_BLOCK_IF_FULL	2U

struct rtt_ring {
	u32 name;	/* target address of the NUL-terminated name  */
	u32 buffer;	/* target address of the ring buffer          */
	u32 size;	/* ring buffer size in bytes                  */
	u32 wr_off;	/* up: written by target; down: written by host */
	u32 rd_off;	/* up: written by host;   down: written by target */
	u32 flags;
};

struct rtt_cb {
	char id[16];		/* "SEGGER RTT" -- written LAST */
	s32  max_up;
	s32  max_down;
	struct rtt_ring up[1];
	struct rtt_ring down[1];
};

#define CB	((volatile struct rtt_cb *)RTT_BASE)

/*
 * Idempotent across re-probes and relocation: the "already initialised" state
 * lives in SRAM (the ID magic), not in a relocatable static, so a post-reloc
 * re-probe won't reset the ring offsets mid-stream.
 */
static void rtt_init(void)
{
	static const char id[16] = "SEGGER RTT\0\0\0\0\0";
	static const char nm[]   = "Terminal";
	volatile char *dst;
	unsigned int i;

	if (CB->id[0] == 'S' && CB->id[9] == 'T')	/* "SEGGER RTT" present */
		return;

	CB->max_up   = 1;
	CB->max_down = 1;

	CB->up[0].name   = RTT_NAME;
	CB->up[0].buffer = RTT_UP_BUF;
	CB->up[0].size   = RTT_UP_SIZE;
	CB->up[0].wr_off = 0;
	CB->up[0].rd_off = 0;
	CB->up[0].flags  = 0;	/* NoBlockSkip -- never stall the target */

	CB->down[0].name   = RTT_NAME;
	CB->down[0].buffer = RTT_DOWN_BUF;
	CB->down[0].size   = RTT_DOWN_SIZE;
	CB->down[0].wr_off = 0;
	CB->down[0].rd_off = 0;
	CB->down[0].flags  = 0;

	dst = (volatile char *)RTT_NAME;
	for (i = 0; i < sizeof(nm); i++)
		dst[i] = nm[i];

	/* Publish the magic ID last so J-Link never latches a half-built block. */
	dst = CB->id;
	for (i = 0; i < sizeof(id); i++)
		dst[i] = id[i];
}

static void rtt_putc_raw(char c)
{
	u32 wr  = CB->up[0].wr_off;
	u32 nxt = (wr + 1) % RTT_UP_SIZE;

	/*
	 * NoBlockSkip: if the ring is full (no RTT client draining, or it is
	 * slower than we produce), drop the character rather than block.  Never
	 * spin -- blocking here would stall the whole boot whenever no host is
	 * attached.  The buffer is large enough to hold a burst (e.g. the banner)
	 * as long as a client is connected.
	 */
	if (nxt == CB->up[0].rd_off)
		return;

	((volatile char *)RTT_UP_BUF)[wr] = c;
	CB->up[0].wr_off = nxt;
}

static int rtt_getc_raw(void)
{
	u32 rd = CB->down[0].rd_off;
	char c;

	if (rd == CB->down[0].wr_off)
		return -1;
	c = ((volatile char *)RTT_DOWN_BUF)[rd];
	CB->down[0].rd_off = (rd + 1) % RTT_DOWN_SIZE;
	return (int)(unsigned char)c;
}

static int rtt_serial_putc(struct udevice *dev, const char ch)
{
	rtt_putc_raw(ch);
	return 0;
}

static int rtt_serial_getc(struct udevice *dev)
{
	int c = rtt_getc_raw();

	return (c < 0) ? -EAGAIN : c;
}

static int rtt_serial_pending(struct udevice *dev, bool input)
{
	if (input)
		return CB->down[0].rd_off != CB->down[0].wr_off;
	return 0;
}

static int rtt_serial_probe(struct udevice *dev)
{
	rtt_init();
	return 0;
}

static const struct dm_serial_ops rtt_serial_ops = {
	.putc	 = rtt_serial_putc,
	.getc	 = rtt_serial_getc,
	.pending = rtt_serial_pending,
};

U_BOOT_DRIVER(rtt_serial) = {
	.name	= "serial_rtt",
	.id	= UCLASS_SERIAL,
	.probe	= rtt_serial_probe,
	.ops	= &rtt_serial_ops,
	.flags	= DM_FLAG_PRE_RELOC,
};

U_BOOT_DRVINFO(rtt_serial) = {
	.name = "serial_rtt",
};

#ifdef CONFIG_DEBUG_UART_RTT
#include <debug_uart.h>

static inline void _debug_uart_init(void)
{
	rtt_init();
}

static inline void _debug_uart_putc(int c)
{
	rtt_putc_raw((char)c);
}

DEBUG_UART_FUNCS
#endif
