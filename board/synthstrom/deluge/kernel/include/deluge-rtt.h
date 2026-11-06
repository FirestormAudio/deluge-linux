/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DELUGE_RTT_H
#define _LINUX_DELUGE_RTT_H

#include <linux/types.h>
#include <linux/stddef.h>	/* offsetof */

/*
 * SEGGER RTT control block shared by the Deluge earlycon, hvc console, and
 * U-Boot's serial_rtt.c. The block lives at a fixed on-chip SRAM address
 * (set up by U-Boot) and is read/written by a J-Link probe via background
 * memory access. The kernel maps it and moves bytes through the up
 * (target->host) and down (host->target) rings.
 */
#define DELUGE_RTT_BASE		0x20080000UL
#define DELUGE_RTT_MAP		0x1000U

#define DELUGE_RTT_UP_BUF	0x100U	/* up ring buffer offset from CB base */
#define DELUGE_RTT_UP_SIZE	1024U	/* target -> host; power of two */
#define DELUGE_RTT_DOWN_BUF	0x600U	/* down ring buffer offset from CB base */
#define DELUGE_RTT_DOWN_SIZE	256U	/* host -> target; power of two */

struct deluge_rtt_ring {
	u32 name;	/* target addr of the ring's name string */
	u32 buffer;	/* target addr of the ring buffer */
	u32 size;	/* ring size in bytes */
	u32 wr_off;	/* up: written by target; down: written by host */
	u32 rd_off;	/* up: written by host;   down: written by target */
	u32 flags;
};

struct deluge_rtt_cb {
	char id[16];			/* "SEGGER RTT" — written last */
	s32  max_up;
	s32  max_down;
	struct deluge_rtt_ring up[1];
	struct deluge_rtt_ring down[1];
};

/* Field offsets for readl/writel against the ioremapped control block. */
#define DELUGE_RTT_UP_WROFF	offsetof(struct deluge_rtt_cb, up[0].wr_off)	/* 0x24 */
#define DELUGE_RTT_UP_RDOFF	offsetof(struct deluge_rtt_cb, up[0].rd_off)	/* 0x28 */
#define DELUGE_RTT_DOWN_WROFF	offsetof(struct deluge_rtt_cb, down[0].wr_off)	/* 0x3c */
#define DELUGE_RTT_DOWN_RDOFF	offsetof(struct deluge_rtt_cb, down[0].rd_off)	/* 0x40 */

#endif /* _LINUX_DELUGE_RTT_H */
