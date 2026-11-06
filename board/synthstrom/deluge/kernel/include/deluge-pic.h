/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DELUGE_PIC_H
#define _LINUX_DELUGE_PIC_H

#include <linux/types.h>

struct device;
struct device_node;

/*
 * The Deluge PIC front-panel link (deluge-pic, owns SCIF1) also mediates the
 * OLED chip-select and data/command lines. These let the OLED driver drive them.
 * deluge_pic_get() returns the deluge-pic device for a phandle node (with a
 * reference held), or NULL if it isn't bound yet (caller should -EPROBE_DEFER).
 */
struct device *deluge_pic_get(struct device_node *np);

/*
 * Attach a consumer to the PIC: resolve its "synthstrom,pic" phandle, take a
 * reference to the deluge-pic device, and add a device link so the consumer is
 * unbound before the PIC. Returns the pic device (reference held — release with
 * deluge_pic_put() from the consumer's remove()), or an ERR_PTR:
 *   -EINVAL       the phandle is missing
 *   -EPROBE_DEFER the PIC is not bound yet
 *   -ENODEV       the device link could not be created
 */
struct device *deluge_pic_attach(struct device *consumer);

/* Release a reference obtained from deluge_pic_attach()/deluge_pic_get(). */
void deluge_pic_put(struct device *pic);

int deluge_pic_oled_enable(struct device *pic);         /* TX 247 */
int deluge_pic_oled_dc(struct device *pic, bool data);  /* TX 251 (data) / 250 (cmd) */
int deluge_pic_oled_select(struct device *pic);         /* TX 248, wait 248 echo */
int deluge_pic_oled_deselect(struct device *pic);       /* TX 249, wait 249 echo */

/* Front-panel LED output (deluge-pic drives the PIC TX). */
int deluge_pic_led_set(struct device *pic, u8 id, bool on);          /* id 0..35 */
int deluge_pic_gold_knob(struct device *pic, u8 knob, const u8 b[4]); /* knob 0/1, 4 bytes */
int deluge_pic_pad_pair(struct device *pic, u8 pair, const u8 rgb[48]); /* pair 0..8, 16*RGB */
int deluge_pic_pad_refresh(struct device *pic);                      /* TX 240 */

/*
 * Pad grid input dispatch. The pad driver registers a handler; deluge-pic
 * calls it for each decoded pad event from the DMA-poll hrtimer, which runs in
 * soft (atomic, non-hardirq) context, so the callback must not sleep
 * (input_report_* is fine). Pass cb = NULL to clear (do this before the pad driver tears its
 * input device down).
 */
struct deluge_pic_pad_event {
	u8 pad_id;	/* 0..143 */
	bool press;	/* true = press, false = release */
};
int deluge_pic_set_pad_handler(struct device *pic,
			       void (*cb)(void *ctx, const struct deluge_pic_pad_event *ev),
			       void *ctx);

#endif
