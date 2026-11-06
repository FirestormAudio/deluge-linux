/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DELUGE_SCIF_H
#define _LINUX_DELUGE_SCIF_H

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/types.h>

/*
 * RZ/A1 SCIF (SH-2 FIFO SCI) register map, shared by deluge-pic (SCIF1) and
 * deluge-midi (SCIF0). 16-bit registers except SCBRR/SCFTDR/SCFRDR (8-bit).
 * Driver-specific SCSCR control words (RX_ON / MIDI / MIDI_TX) stay in each
 * driver — only the offsets and generic status/control bits are shared.
 */
#define SCIF_SCSMR	0x00	/* serial mode */
#define SCIF_SCBRR	0x04	/* bit rate */
#define SCIF_SCSCR	0x08	/* serial control */
#define SCIF_SCFTDR	0x0c	/* transmit FIFO data */
#define SCIF_SCFSR	0x10	/* FIFO status */
#define SCIF_SCFRDR	0x14	/* receive FIFO data */
#define SCIF_SCFCR	0x18	/* FIFO control */
#define SCIF_SCFDR	0x1c	/* FIFO data count */
#define SCIF_SCLSR	0x24	/* line status */

#define SCFCR_RFRST	0x0002	/* reset receive FIFO */
#define SCFCR_TFRST	0x0004	/* reset transmit FIFO */

#define SCFSR_DR	0x0001	/* receive data ready (sub-trigger, timed out) */
#define SCFSR_RDF	0x0002	/* receive FIFO reached trigger */
#define SCFSR_BRK	0x0010	/* break detected */
#define SCFSR_TDFE	0x0020	/* transmit FIFO has space */
#define SCFSR_TEND	0x0040	/* transmit ended */
#define SCFSR_ER	0x0080	/* receive error (framing/parity) */
/* Init mask: clear ER/BRK/DR, keep the rest — 0xff6e KEEPS RDF (0x02). */
#define SCFSR_KEEP	0xff6e

#define SCFDR_RXCNT	0x001f	/* receive FIFO byte count (0..16) */
#define SCLSR_ORER	0x0001	/* receive overrun */

/*
 * Push one byte: wait for TX FIFO space, write it, then clear TDFE|TEND so the
 * next status poll reflects it draining. Returns 0, or -ETIMEDOUT after ~10 ms.
 * Callers that already know TDFE is set (e.g. an ISR drain loop) return on the
 * first iteration without spinning.
 */
static inline int scif_tx_byte(void __iomem *scif, u8 b)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (readw(scif + SCIF_SCFSR) & SCFSR_TDFE)
			break;
		udelay(1);
	}
	if (i == 10000)
		return -ETIMEDOUT;
	writeb(b, scif + SCIF_SCFTDR);
	writew(readw(scif + SCIF_SCFSR) & ~(SCFSR_TDFE | SCFSR_TEND),
	       scif + SCIF_SCFSR);
	return 0;
}

#endif /* _LINUX_DELUGE_SCIF_H */
