// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge RGB pad grid (18x8) as an fbdev. Colours are streamed to
 * the PIC (deluge-pic) as nine column-pairs, sending only the pairs that
 * changed since the last flush, then a refresh trigger (the SDK frame-diff).
 * Pixel (x,y) == input pad (x,y).
 */
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/vmalloc.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/deluge-pic.h>

#define PAD_W		18
#define PAD_H		8
#define PAD_PAIRS	9			/* PAD_W / 2 */
#define PAD_PAIR_BYTES	48			/* 16 LEDs * 3 */
#define PAD_BPP		24
#define PAD_LINE	(PAD_W * 3)		/* 54 */
#define PAD_FB_SIZE	(PAD_LINE * PAD_H)	/* 432 */
#define PAD_REFRESH_MS	33			/* ~30 fps */
#define PIC_PADS	144

struct deluge_pad {
	struct device *pic;
	struct fb_info *info;
	struct fb_deferred_io defio;
	struct mutex lock;	/* protects last_sent[]/dirty_all cache + PIC send */
	u8 last_sent[PAD_PAIRS][PAD_PAIR_BYTES];
	bool dirty_all;
	struct input_dev *input;	/* pad grid, evdev MT type-B */
};

/*
 * Single source of truth for the pad grid geometry. The PIC uses two hardware
 * encodings — an input pad-id scheme and an output LED pair/index scheme — both
 * anchored here to one logical (col 0..17, row 0..7), top-left origin, y-down.
 */

/* Input decode: PIC pad id (0..143) -> logical (col, row). */
static void pad_id_to_coord(u8 id, unsigned int *col, unsigned int *row)
{
	unsigned int y = id / 9;
	unsigned int x = (id - y * 9) * 2;

	if (y >= 8) {		/* second bank = odd columns */
		y -= 8;
		x += 1;
	}
	*col = x;
	*row = (PAD_H - 1) - y;	/* PIC rows run bottom-up; report top-left */
}

/* Output encode: logical (col, row) -> column-pair and the LED's byte slot
 * (0..15) within that pair's 48-byte buffer. PIC LED 0 of a half = bottom. */
static void coord_to_led(unsigned int col, unsigned int row,
			  unsigned int *pair, unsigned int *led_index)
{
	*pair = col / 2;
	*led_index = (col & 1) * 8 + ((PAD_H - 1) - row);
}

/* Boot-time consistency check against a hand-verified anchor table. */
static void pad_map_selfcheck(struct device *dev)
{
	static const struct { u8 id; unsigned int col, row; } in_anchor[] = {
		{ 0, 0, 7 }, { 8, 16, 7 }, { 72, 1, 7 }, { 143, 17, 0 },
	};
	static const struct { unsigned int col, row, pair, idx; } out_anchor[] = {
		{ 0, 0, 0, 7 }, { 0, 7, 0, 0 }, { 17, 0, 8, 15 }, { 17, 7, 8, 8 },
	};
	unsigned int i, col, row, pair, idx;

	for (i = 0; i < ARRAY_SIZE(in_anchor); i++) {
		pad_id_to_coord(in_anchor[i].id, &col, &row);
		if (col != in_anchor[i].col || row != in_anchor[i].row)
			dev_warn(dev, "pad map: id %u -> (%u,%u), expected (%u,%u)\n",
				 in_anchor[i].id, col, row,
				 in_anchor[i].col, in_anchor[i].row);
	}
	for (i = 0; i < ARRAY_SIZE(out_anchor); i++) {
		coord_to_led(out_anchor[i].col, out_anchor[i].row, &pair, &idx);
		if (pair != out_anchor[i].pair || idx != out_anchor[i].idx)
			dev_warn(dev, "pad map: (%u,%u) -> pair %u idx %u, expected pair %u idx %u\n",
				 out_anchor[i].col, out_anchor[i].row, pair, idx,
				 out_anchor[i].pair, out_anchor[i].idx);
	}
}

static void pad_pack_pair(const u8 *vmem, unsigned int pair, u8 out[PAD_PAIR_BYTES])
{
	unsigned int col, row;

	for (col = pair * 2; col < pair * 2 + 2; col++) {
		for (row = 0; row < PAD_H; row++) {
			const u8 *px = vmem + row * PAD_LINE + col * 3;
			unsigned int p, idx;

			coord_to_led(col, row, &p, &idx);
			memcpy(&out[idx * 3], px, 3);
		}
	}
}

static void pad_blit(struct deluge_pad *pad)
{
	const u8 *vmem = pad->info->screen_buffer;
	struct device *dev = pad->info->device;
	u8 buf[PAD_PAIR_BYTES];
	unsigned int pair;
	int ret;

	mutex_lock(&pad->lock);
	for (pair = 0; pair < PAD_PAIRS; pair++) {
		pad_pack_pair(vmem, pair, buf);
		if (!pad->dirty_all &&
		    !memcmp(buf, pad->last_sent[pair], PAD_PAIR_BYTES))
			continue;
		ret = deluge_pic_pad_pair(pad->pic, pair, buf);
		if (ret) {
			dev_warn_ratelimited(dev, "pad pair %u: %d\n", pair, ret);
			continue;
		}
		memcpy(pad->last_sent[pair], buf, PAD_PAIR_BYTES);
	}
	pad->dirty_all = false;
	ret = deluge_pic_pad_refresh(pad->pic);
	if (ret)
		dev_warn_ratelimited(dev, "pad refresh: %d\n", ret);
	mutex_unlock(&pad->lock);
}

static void pad_deferred_io(struct fb_info *info, struct list_head *pagereflist)
{
	pad_blit(info->par);
}

static void pad_damage_range(struct fb_info *info, off_t off, size_t len)
{
	pad_blit(info->par);
}

static void pad_damage_area(struct fb_info *info, u32 x, u32 y,
			    u32 width, u32 height)
{
	pad_blit(info->par);
}

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(pad, pad_damage_range, pad_damage_area)

static const struct fb_ops pad_ops = {
	.owner = THIS_MODULE,
	FB_DEFAULT_DEFERRED_OPS(pad),
};

static const struct fb_fix_screeninfo pad_fix = {
	.id = "deluge-pads", .type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_TRUECOLOR, .accel = FB_ACCEL_NONE,
	.line_length = PAD_LINE,
};
static const struct fb_var_screeninfo pad_var = {
	.xres = PAD_W, .yres = PAD_H, .xres_virtual = PAD_W,
	.yres_virtual = PAD_H, .bits_per_pixel = PAD_BPP,
	.red   = { .offset = 0,  .length = 8 },
	.green = { .offset = 8,  .length = 8 },
	.blue  = { .offset = 16, .length = 8 },
};

/* Called from deluge-pic's DMA-poll hrtimer: soft/atomic (hrtimer) context — must not sleep. */
static void deluge_pad_event(void *ctx, const struct deluge_pic_pad_event *ev)
{
	struct deluge_pad *pad = ctx;
	unsigned int col, row;

	pad_id_to_coord(ev->pad_id, &col, &row);
	input_mt_slot(pad->input, ev->pad_id);
	input_mt_report_slot_state(pad->input, MT_TOOL_FINGER, ev->press);
	if (ev->press) {
		input_report_abs(pad->input, ABS_MT_POSITION_X, col * 256 + 128);
		input_report_abs(pad->input, ABS_MT_POSITION_Y, row * 256 + 128);
		input_report_abs(pad->input, ABS_MT_PRESSURE, 4095);
	}
	input_mt_sync_frame(pad->input);
	input_sync(pad->input);
}

static void pad_framebuffer_release(void *data) { framebuffer_release(data); }
static void pad_defio_cleanup(void *data) { fb_deferred_io_cleanup(data); }
static void pad_vfree(void *data)
{
	struct fb_info *info = data;

	vfree(info->screen_buffer);
}
static void pad_pic_put(void *data) { deluge_pic_put(data); }
static void pad_clear_handler(void *data)
{
	struct deluge_pad *pad = data;

	deluge_pic_set_pad_handler(pad->pic, NULL, NULL);
}

static int deluge_pad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct deluge_pad *pad;
	struct fb_info *info;
	int ret;

	info = framebuffer_alloc(sizeof(*pad), dev);
	if (!info)
		return -ENOMEM;
	pad = info->par;
	pad->info = info;
	pad->dirty_all = true;
	mutex_init(&pad->lock);
	pad_map_selfcheck(dev);
	ret = devm_add_action_or_reset(dev, pad_framebuffer_release, info);
	if (ret)
		return ret;

	pad->pic = deluge_pic_attach(dev);
	if (IS_ERR(pad->pic))
		return dev_err_probe(dev, PTR_ERR(pad->pic), "attach pic\n");
	ret = devm_add_action_or_reset(dev, pad_pic_put, pad->pic);
	if (ret)
		return ret;

	info->screen_buffer = vzalloc(PAD_FB_SIZE);
	if (!info->screen_buffer)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, pad_vfree, info);
	if (ret)
		return ret;
	info->fbops = &pad_ops;
	info->fix = pad_fix;
	info->var = pad_var;
	info->screen_size = PAD_FB_SIZE;
	info->fix.smem_len = PAD_FB_SIZE;	/* fb_deferred_io_init needs this */

	pad->defio.delay = HZ * PAD_REFRESH_MS / 1000;
	pad->defio.deferred_io = pad_deferred_io;
	info->fbdefio = &pad->defio;
	ret = fb_deferred_io_init(info);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, pad_defio_cleanup, info);
	if (ret)
		return ret;

	ret = devm_register_framebuffer(dev, info);
	if (ret)
		return ret;

	pad->input = devm_input_allocate_device(dev);
	if (!pad->input)
		return -ENOMEM;
	pad->input->name = "Deluge Pads";
	pad->input->phys = "deluge/pic/pads";
	pad->input->id.bustype = BUS_HOST;
	input_set_abs_params(pad->input, ABS_MT_POSITION_X, 0, PAD_W * 256 - 1, 0, 0);
	input_set_abs_params(pad->input, ABS_MT_POSITION_Y, 0, PAD_H * 256 - 1, 0, 0);
	input_set_abs_params(pad->input, ABS_MT_PRESSURE, 0, 4095, 0, 0);
	ret = input_mt_init_slots(pad->input, PIC_PADS, INPUT_MT_DIRECT);
	if (ret)
		return ret;
	ret = input_register_device(pad->input);
	if (ret)
		return ret;

	ret = deluge_pic_set_pad_handler(pad->pic, deluge_pad_event, pad);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, pad_clear_handler, pad);
	if (ret)
		return ret;

	pad_blit(pad);				/* paint the initial (black) frame */
	dev_info(dev, "Deluge pad grid %ux%u fb%d\n", PAD_W, PAD_H, info->node);
	return 0;
}

static const struct of_device_id deluge_pad_of[] = {
	{ .compatible = "synthstrom,deluge-pads" },
	{ }
};
MODULE_DEVICE_TABLE(of, deluge_pad_of);

static struct platform_driver deluge_pad_driver = {
	.probe = deluge_pad_probe,
	.driver = {
		.name = "deluge-pad",
		.of_match_table = deluge_pad_of,
	},
};
module_platform_driver(deluge_pad_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge pad grid (18x8): evdev multi-touch + RGB fbdev over the PIC");
