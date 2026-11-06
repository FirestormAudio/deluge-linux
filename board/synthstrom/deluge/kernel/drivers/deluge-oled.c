// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge OLED (SSD1309, 128x48 mono) as an fbdev.
 * Pixels go over RSPI0; chip-select / data-command / power are mediated by the
 * PIC (see deluge-pic). Whole 768-byte frame is blitted on each deferred-io
 * refresh, bracketed by the PIC select/deselect echo handshake.
 */
#include <linux/module.h>
#include <linux/err.h>
#include <linux/spi/spi.h>
#include <linux/fb.h>
#include <linux/vmalloc.h>
#include <linux/of.h>
#include <linux/deluge-pic.h>

#define OLED_W		128
#define OLED_H		48			/* rows the SSD1309 drives (pages 2..7) */
#define OLED_PAGES	(OLED_H / 8)		/* 6 */
#define OLED_FRAME	(OLED_PAGES * OLED_W)	/* 768 (hardware frame) */
#define OLED_PAGE0	2			/* visible window = pages 2..7 */
/*
 * The top 5 of the panel's 48 driven rows sit behind the case faceplate and are
 * never visible (DelugeFirmware OLED_MAIN_TOPMOST_PIXEL / SDK VISIBLE_TOP). The
 * fbdev therefore presents only the 43 visible rows; oled_pack() offsets the fb
 * down by OLED_TOP into the hardware window and blanks the hidden top rows.
 */
#define OLED_TOP	5			/* hidden rows behind the bezel */
#define OLED_VIS_H	(OLED_H - OLED_TOP)	/* 43 visible rows (the fb height) */
#define OLED_REFRESH_MS	33			/* ~30 fps */

struct deluge_oled {
	struct spi_device *spi;
	struct device *pic;		/* deluge-pic device (CS/DC) */
	struct fb_info *info;
	struct fb_deferred_io defio;
	u8 *page_buf;			/* 768-byte packed frame for SPI */
};

/*
 * fb is 1bpp row-major, MSB-first (bit7 = leftmost pixel). SSD1309 wants
 * page-major: byte[page][col] packs 8 vertical pixels, bit0 = top row of page.
 * The fb holds only the 43 visible rows, so hardware row y maps to fb row
 * y - OLED_TOP; the OLED_TOP hidden rows behind the bezel are packed as blank.
 */
static void oled_pack(struct deluge_oled *o)
{
	const u8 *vmem = o->info->screen_buffer;
	unsigned int line = o->info->fix.line_length;	/* 16 */
	unsigned int page, col, bit;

	for (page = 0; page < OLED_PAGES; page++) {
		for (col = 0; col < OLED_W; col++) {
			u8 b = 0;

			for (bit = 0; bit < 8; bit++) {
				unsigned int y = page * 8 + bit;	/* hardware row 0..47 */
				u8 px = 0;

				if (y >= OLED_TOP) {
					unsigned int fb_row = y - OLED_TOP;
					unsigned int idx = fb_row * line + (col >> 3);

					px = (vmem[idx] >> (7 - (col & 7))) & 1;
				}
				b |= px << bit;
			}
			o->page_buf[page * OLED_W + col] = b;
		}
	}
}

static void oled_blit(struct deluge_oled *o)
{
	struct spi_controller *ctlr = o->spi->controller;
	struct spi_transfer xfer = { .tx_buf = o->page_buf, .len = OLED_FRAME };
	struct spi_message msg;
	int ret;

	oled_pack(o);

	/*
	 * The OLED's chip-select is asserted by the PIC out-of-band (cs-gpios=<0>),
	 * so the SPI core can't see that the OLED is selected. Lock the whole bus
	 * across select->blit->deselect so a CV transfer (core-managed CS on the
	 * same RSPI0) can't interleave and clock its bytes into the selected OLED.
	 * Cost: CV writes block for the blit, including the PIC select echo.
	 */
	spi_bus_lock(ctlr);

	ret = deluge_pic_oled_select(o->pic);
	if (ret) {
		spi_bus_unlock(ctlr);
		dev_warn_ratelimited(&o->spi->dev, "OLED select: %d\n", ret);
		return;				/* skip frame, never hang */
	}

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	ret = spi_sync_locked(o->spi, &msg);	/* bus already locked */
	if (ret)
		dev_warn_ratelimited(&o->spi->dev, "OLED blit: %d\n", ret);

	deluge_pic_oled_deselect(o->pic);
	spi_bus_unlock(ctlr);
}

/* fbdefio flush callback (mmap page-fault path — fbcon, boot logo, mmap draws). */
static void oled_deferred_io(struct fb_info *info, struct list_head *pagereflist)
{
	oled_blit(info->par);
}

static void oled_damage_range(struct fb_info *info, off_t off, size_t len)
{
	oled_blit(info->par);
}

static void oled_damage_area(struct fb_info *info, u32 x, u32 y,
			      u32 width, u32 height)
{
	oled_blit(info->par);
}

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(oled, oled_damage_range, oled_damage_area)

static const struct fb_ops oled_ops = {
	.owner = THIS_MODULE,
	FB_DEFAULT_DEFERRED_OPS(oled),
};

static const struct fb_fix_screeninfo oled_fix = {
	.id = "deluge-oled", .type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_MONO10, .accel = FB_ACCEL_NONE,
	.line_length = OLED_W / 8,
};
static const struct fb_var_screeninfo oled_var = {
	.xres = OLED_W, .yres = OLED_VIS_H, .xres_virtual = OLED_W,
	.yres_virtual = OLED_VIS_H, .bits_per_pixel = 1,
	.red = { .length = 1 }, .green = { .length = 1 }, .blue = { .length = 1 },
};

static void oled_framebuffer_release(void *data) { framebuffer_release(data); }
static void oled_defio_cleanup(void *data) { fb_deferred_io_cleanup(data); }
static void oled_vfree(void *data)
{
	struct fb_info *info = data;

	vfree(info->screen_buffer);
}
static void oled_pic_put(void *data) { deluge_pic_put(data); }

static int oled_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct deluge_oled *o;
	struct fb_info *info;
	int ret;

	info = framebuffer_alloc(sizeof(*o), dev);
	if (!info)
		return -ENOMEM;
	o = info->par;
	o->info = info;
	o->spi = spi;
	ret = devm_add_action_or_reset(dev, oled_framebuffer_release, info);
	if (ret)
		return ret;

	o->pic = deluge_pic_attach(&spi->dev);
	if (IS_ERR(o->pic))
		return dev_err_probe(&spi->dev, PTR_ERR(o->pic), "attach pic\n");
	ret = devm_add_action_or_reset(dev, oled_pic_put, o->pic);
	if (ret)
		return ret;

	o->page_buf = devm_kzalloc(dev, OLED_FRAME, GFP_KERNEL);
	if (!o->page_buf)
		return -ENOMEM;

	spi->bits_per_word = 8;			/* mode 0 default */
	ret = spi_setup(spi);
	if (ret)
		return ret;

	info->screen_buffer = vzalloc(OLED_W / 8 * OLED_VIS_H);
	if (!info->screen_buffer)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, oled_vfree, info);
	if (ret)
		return ret;
	info->fbops = &oled_ops;
	info->fix = oled_fix;
	info->var = oled_var;
	info->screen_size = OLED_W / 8 * OLED_VIS_H;
	info->fix.smem_len = info->screen_size;	/* fb_deferred_io_init needs this */

	o->defio.delay = HZ * OLED_REFRESH_MS / 1000;
	o->defio.deferred_io = oled_deferred_io;
	info->fbdefio = &o->defio;
	ret = fb_deferred_io_init(info);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, oled_defio_cleanup, info);
	if (ret)
		return ret;

	/*
	 * Warm attach: the app-loader already powered and configured the panel
	 * and left the Deluge droplet in GDDRAM. Do NOT re-init and do NOT clear
	 * or blit — just ensure D/C is in data mode for subsequent pixel blits.
	 */
	ret = deluge_pic_oled_dc(o->pic, true);
	if (ret)
		return dev_err_probe(dev, ret, "OLED D/C setup failed\n");

	ret = devm_register_framebuffer(dev, info);
	if (ret)
		return ret;
	dev_info(dev, "Deluge OLED %ux%u fb%d (warm attach)\n", OLED_W, OLED_VIS_H, info->node);
	return 0;
}

static const struct of_device_id oled_of[] = {
	{ .compatible = "synthstrom,deluge-oled" }, { }
};
MODULE_DEVICE_TABLE(of, oled_of);
static struct spi_driver oled_driver = {
	.probe = oled_probe,
	.driver = { .name = "deluge-oled", .of_match_table = oled_of },
};
module_spi_driver(oled_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge OLED (SSD1309) fbdev over RSPI0 + PIC CS");
