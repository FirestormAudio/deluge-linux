// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge PIC32 front-panel over SCIF1 — self-contained driver.
 *
 * The PIC owns the 144 pads, 36 buttons, LEDs and 7-seg, and streams pad/button
 * changes to the CPU as a flat, unframed byte stream over SCIF1 @ 200000 baud.
 * It streams autonomously (the app-loader configured it), so no host handshake
 * is needed for input.
 *
 * On the RZ/A1 the SCIF RX-FIFO request is wired to the DMAC, not the GIC, so
 * interrupt-driven RX never fires. Rather than fight the tty/serial layer (which
 * kept disabling the receiver behind our back), this driver owns SCIF1 directly
 * and drives the SCIF1 RX FIFO via a re-armed slave_sg DMAC transfer over a ring
 * — exactly the deluge-sdk model: the DMAC refills the ring on each completion,
 * and an hrtimer polls the write position and decodes new bytes. Pad
 * events are dispatched to a registered handler (see deluge_pic_set_pad_handler(),
 * consumed by the pad driver); button events become raw keys on a separate
 * "Deluge Buttons" key device. LEDs (TX) and OLED chip-select/DC mediation are
 * implemented here (see the TX helpers below).
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/hrtimer.h>
#include <linux/input.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/deluge-pic.h>
#include <linux/deluge-scif.h>

#define PIC_BTNS	36
#define PIC_BTN_BASE	BTN_TRIGGER_HAPPY1	/* 0x2c0, 40 codes -> fits 36 */

#define SCSCR_RX_ON	0x00f0	/* TIE|RIE|TE|RE; RIE gates the RX DMA request */

#define PIC_OLED_ENABLE		247
#define PIC_OLED_SELECT		248
#define PIC_OLED_DESELECT	249
#define PIC_OLED_DC_LOW		250
#define PIC_OLED_DC_HIGH	251

/* Output command bytes (CPU -> PIC). */
#define PIC_LED_OFF_BASE	152	/* 152+id, id 0..35 */
#define PIC_LED_ON_BASE		188	/* 188+id, id 0..35 */
#define PIC_GOLD_KNOB0		20
#define PIC_GOLD_KNOB1		21
#define PIC_COLS_BASE		1	/* 1+pair, pair 0..8: column-pair colour */
#define PIC_DONE_ROWS		240	/* trigger PIC display refresh */
#define PIC_SET_REFRESH		19	/* + interval 0..25 (lower = brighter) */

/*
 * rz-dmac has no cyclic mode, so RX is a single slave_sg transfer over a ring
 * buffer, re-armed on each completion. An hrtimer polls the DMA write offset
 * (len - residue) every PIC_POLL_US and decodes whatever has arrived so far,
 * so single/sparse bytes (a button press, the OLED handshake echo) get
 * bounded latency instead of stalling until the buffer fills. This is the
 * same poll model as the cyclic nbpf path; only the re-arm-on-completion
 * differs. 256 B ~= 12.8 ms at 200000 baud, ample slack over the 1 ms poll.
 */
#define PIC_RING_LEN	256
#define PIC_POLL_US	1000

struct deluge_pic {
	struct device *dev;
	void __iomem *scif;
	struct clk *clk;
	struct gen_pool *sram;		/* on-chip SRAM pool for the RX ring (or NULL) */
	struct dma_chan *rx;
	u32 rx_overruns;		/* SCIF RX-FIFO overruns (dropped bytes) */
	void *ring;			/* DMA RX ring buffer */
	dma_addr_t ring_dma;
	dma_cookie_t cookie;		/* current slave_sg transfer */
	unsigned int pos;		/* last byte consumed from the ring */
	spinlock_t rx_lock;		/* serialises poll vs completion decode */
	struct hrtimer poll;
	struct input_dev *btn_input;	/* buttons only (keys) */
	spinlock_t pad_lock;		/* guards pad_cb/pad_ctx; clear-under-lock and
					 * no-callback-after-clear rely on this being a
					 * single-core SoC — on SMP an in-flight callback
					 * on another CPU would need an explicit flush
					 * (e.g. synchronize_irq()-style) before the
					 * consumer frees itself.
					 */
	void (*pad_cb)(void *ctx, const struct deluge_pic_pad_event *ev);
	void *pad_ctx;
	bool next_is_off;		/* byte 252 seen -> next pad/btn is a release */
	bool fw_next;			/* byte 245 seen -> next byte is fw version */
	struct completion oled_sel;	/* completed on 248 echo */
	struct completion oled_desel;	/* completed on 249 echo */
	struct mutex tx_lock;		/* serialise PIC TX */
	int refresh_time;		/* last-written SET_REFRESH value, -1 = unset */
};

static void pic_pad(struct deluge_pic *p, u8 id, bool press)
{
	void (*cb)(void *ctx, const struct deluge_pic_pad_event *ev);
	struct deluge_pic_pad_event ev = { .pad_id = id, .press = press };
	unsigned long flags;
	void *ctx;

	spin_lock_irqsave(&p->pad_lock, flags);
	cb = p->pad_cb;
	ctx = p->pad_ctx;
	spin_unlock_irqrestore(&p->pad_lock, flags);

	if (cb)
		cb(ctx, &ev);
}

static void pic_btn(struct deluge_pic *p, u8 id, bool press)
{
	input_report_key(p->btn_input, PIC_BTN_BASE + id, press);
	input_sync(p->btn_input);
}

/* Decode one byte of the flat PIC stream into input events. */
static void pic_parse_byte(struct deluge_pic *p, u8 b)
{
	if (p->fw_next) {
		p->fw_next = false;
		dev_info(p->dev, "PIC firmware v%u\n", b);
	} else if (b <= 143) {
		pic_pad(p, b, !p->next_is_off);
		p->next_is_off = false;
	} else if (b <= 179) {
		pic_btn(p, b - 144, !p->next_is_off);
		p->next_is_off = false;
	} else if (b == 245) {
		p->fw_next = true;
	} else if (b == 252) {
		p->next_is_off = true;
	} else if (b == 254) {
		p->next_is_off = false;	/* no presses happening */
	} else if (b == PIC_OLED_SELECT) {
		complete(&p->oled_sel);
	} else if (b == PIC_OLED_DESELECT) {
		complete(&p->oled_desel);
	}
	/* 247 echo and everything else ignored */
}

static void pic_dma_done(void *arg);

/* Decode ring bytes [p->pos, target) into input events, advancing p->pos.
 * target is a monotonic offset within the current transfer (0..PIC_RING_LEN).
 * Caller holds rx_lock. */
static void pic_decode_locked(struct deluge_pic *p, unsigned int target)
{
	const u8 *ring = p->ring;

	while (p->pos < target)
		pic_parse_byte(p, ring[p->pos++]);
}

/* (Re)arm a single slave_sg transfer covering the whole ring, with a
 * completion callback. rz-dmac has no cyclic mode, so we re-arm on every
 * completion; the hrtimer drains partial data in between. Callable from the
 * completion callback (atomic) — rz-dmac prep/submit are spinlock-based. */
static void pic_dma_arm(struct deluge_pic *p)
{
	struct dma_async_tx_descriptor *desc;
	struct scatterlist sg;

	sg_init_table(&sg, 1);
	sg_dma_address(&sg) = p->ring_dma;
	sg_dma_len(&sg) = PIC_RING_LEN;

	desc = dmaengine_prep_slave_sg(p->rx, &sg, 1, DMA_DEV_TO_MEM,
				       DMA_PREP_INTERRUPT);
	if (!desc) {
		dev_err_ratelimited(p->dev, "RX DMA re-arm failed\n");
		return;
	}
	desc->callback = pic_dma_done;
	desc->callback_param = p;
	p->cookie = dmaengine_submit(desc);
	dma_async_issue_pending(p->rx);
}

/* Ring transfer completed: decode any tail the poll hasn't reached, then
 * re-arm from the start of the ring. */
static void pic_dma_done(void *arg)
{
	struct deluge_pic *p = arg;
	unsigned long flags;

	spin_lock_irqsave(&p->rx_lock, flags);
	pic_decode_locked(p, PIC_RING_LEN);
	p->pos = 0;
	pic_dma_arm(p);
	spin_unlock_irqrestore(&p->rx_lock, flags);
}

/* Poll the in-flight transfer: the DMA write offset is len - residue. Decode
 * whatever has arrived since last time, without waiting for completion — this
 * is what gives single/sparse bytes bounded latency. */
static enum hrtimer_restart pic_poll(struct hrtimer *t)
{
	struct deluge_pic *p = container_of(t, struct deluge_pic, poll);
	struct dma_tx_state state;
	unsigned long flags;
	unsigned int pos;

	spin_lock_irqsave(&p->rx_lock, flags);
	if (readw(p->scif + SCIF_SCLSR) & SCLSR_ORER) {
		p->rx_overruns++;
		writew(0, p->scif + SCIF_SCLSR);	/* write-0-to-clear ORER */
	}
	dmaengine_tx_status(p->rx, p->cookie, &state);
	pos = PIC_RING_LEN - state.residue;
	if (pos > PIC_RING_LEN)
		pos = PIC_RING_LEN;
	pic_decode_locked(p, pos);
	spin_unlock_irqrestore(&p->rx_lock, flags);

	hrtimer_forward_now(&p->poll, ns_to_ktime((u64)PIC_POLL_US * 1000));
	return HRTIMER_RESTART;
}

static int pic_start_dma(struct deluge_pic *p, resource_size_t scif_phys)
{
	struct dma_slave_config cfg = {};
	int ret;

	p->rx = dma_request_chan(p->dev, "rx");
	if (IS_ERR(p->rx))
		return dev_err_probe(p->dev, PTR_ERR(p->rx), "no rx DMA channel\n");

	cfg.direction = DMA_DEV_TO_MEM;
	cfg.src_addr = scif_phys + SCIF_SCFRDR;
	cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	ret = dmaengine_slave_config(p->rx, &cfg);
	if (ret)
		goto release;

	/* Prefer the on-chip SRAM pool (0-wait, off the SDRAM bus) for the RX
	 * ring; fall back to the default coherent allocator in SDRAM. */
	p->sram = of_gen_pool_get(p->dev->of_node, "sram", 0);
	if (p->sram)
		p->ring = gen_pool_dma_alloc(p->sram, PIC_RING_LEN, &p->ring_dma);
	else
		p->ring = dma_alloc_coherent(p->dev, PIC_RING_LEN, &p->ring_dma,
					     GFP_KERNEL);
	if (!p->ring) {
		ret = -ENOMEM;
		goto release;
	}
	pic_dma_arm(p);
	return 0;

release:
	dma_release_channel(p->rx);
	return ret;
}

/* Polled-FIFO TX of one byte to the PIC. Caller holds tx_lock. */
static int pic_tx_byte(struct deluge_pic *p, u8 b)
{
	return scif_tx_byte(p->scif, b);
}

/* Send a whole PIC command atomically wrt other senders (per-command tx_lock). */
static int pic_tx_cmd(struct deluge_pic *p, const u8 *buf, size_t len)
{
	int ret = 0;
	size_t i;

	mutex_lock(&p->tx_lock);
	for (i = 0; i < len; i++) {
		ret = pic_tx_byte(p, buf[i]);
		if (ret)
			break;
	}
	mutex_unlock(&p->tx_lock);
	return ret;
}

static struct deluge_pic *pic_from_dev(struct device *dev)
{
	return dev ? dev_get_drvdata(dev) : NULL;
}

struct device *deluge_pic_get(struct device_node *np)
{
	struct platform_device *pdev = of_find_device_by_node(np);

	if (!pdev)
		return NULL;
	if (!platform_get_drvdata(pdev)) {	/* not probed yet */
		put_device(&pdev->dev);
		return NULL;
	}
	return &pdev->dev;			/* ref from of_find_device_by_node held */
}
EXPORT_SYMBOL_GPL(deluge_pic_get);

void deluge_pic_put(struct device *pic)
{
	if (pic)
		put_device(pic);
}
EXPORT_SYMBOL_GPL(deluge_pic_put);

struct device *deluge_pic_attach(struct device *consumer)
{
	struct device_node *pn;
	struct device *pic;

	pn = of_parse_phandle(consumer->of_node, "synthstrom,pic", 0);
	if (!pn)
		return ERR_PTR(-EINVAL);
	pic = deluge_pic_get(pn);
	of_node_put(pn);
	if (!pic)
		return ERR_PTR(-EPROBE_DEFER);
	if (!device_link_add(consumer, pic, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		deluge_pic_put(pic);
		return ERR_PTR(-ENODEV);
	}
	return pic;
}
EXPORT_SYMBOL_GPL(deluge_pic_attach);

int deluge_pic_set_pad_handler(struct device *dev,
			       void (*cb)(void *ctx, const struct deluge_pic_pad_event *ev),
			       void *ctx)
{
	struct deluge_pic *p = pic_from_dev(dev);
	unsigned long flags;

	if (!p)
		return -ENODEV;

	spin_lock_irqsave(&p->pad_lock, flags);
	p->pad_cb = cb;
	p->pad_ctx = ctx;
	spin_unlock_irqrestore(&p->pad_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(deluge_pic_set_pad_handler);

int deluge_pic_oled_enable(struct device *dev)
{
	struct deluge_pic *p = pic_from_dev(dev);
	u8 b = PIC_OLED_ENABLE;

	if (!p)
		return -ENODEV;
	return pic_tx_cmd(p, &b, 1);
}
EXPORT_SYMBOL_GPL(deluge_pic_oled_enable);

int deluge_pic_oled_dc(struct device *dev, bool data)
{
	struct deluge_pic *p = pic_from_dev(dev);
	u8 b = data ? PIC_OLED_DC_HIGH : PIC_OLED_DC_LOW;

	if (!p)
		return -ENODEV;
	return pic_tx_cmd(p, &b, 1);
}
EXPORT_SYMBOL_GPL(deluge_pic_oled_dc);

static int pic_oled_cs(struct deluge_pic *p, u8 cmd, struct completion *echo)
{
	long ret;
	int tret;

	mutex_lock(&p->tx_lock);
	while (try_wait_for_completion(echo))
		;
	tret = pic_tx_byte(p, cmd);
	if (tret) {
		mutex_unlock(&p->tx_lock);
		return tret;
	}
	mutex_unlock(&p->tx_lock);
	ret = wait_for_completion_timeout(echo, msecs_to_jiffies(100));
	return ret ? 0 : -ETIMEDOUT;
}

int deluge_pic_oled_select(struct device *dev)
{
	struct deluge_pic *p = pic_from_dev(dev);

	return p ? pic_oled_cs(p, PIC_OLED_SELECT, &p->oled_sel) : -ENODEV;
}
EXPORT_SYMBOL_GPL(deluge_pic_oled_select);

int deluge_pic_oled_deselect(struct device *dev)
{
	struct deluge_pic *p = pic_from_dev(dev);

	return p ? pic_oled_cs(p, PIC_OLED_DESELECT, &p->oled_desel) : -ENODEV;
}
EXPORT_SYMBOL_GPL(deluge_pic_oled_deselect);

int deluge_pic_led_set(struct device *dev, u8 id, bool on)
{
	struct deluge_pic *p = pic_from_dev(dev);
	u8 b;

	if (!p)
		return -ENODEV;
	if (id >= PIC_BTNS)
		return -EINVAL;
	b = (on ? PIC_LED_ON_BASE : PIC_LED_OFF_BASE) + id;
	return pic_tx_cmd(p, &b, 1);
}
EXPORT_SYMBOL_GPL(deluge_pic_led_set);

int deluge_pic_gold_knob(struct device *dev, u8 knob, const u8 b[4])
{
	struct deluge_pic *p = pic_from_dev(dev);
	u8 buf[5];

	if (!p)
		return -ENODEV;
	if (knob > 1)
		return -EINVAL;
	buf[0] = knob ? PIC_GOLD_KNOB1 : PIC_GOLD_KNOB0;
	memcpy(&buf[1], b, 4);
	return pic_tx_cmd(p, buf, sizeof(buf));
}
EXPORT_SYMBOL_GPL(deluge_pic_gold_knob);

int deluge_pic_pad_pair(struct device *dev, u8 pair, const u8 rgb[48])
{
	struct deluge_pic *p = pic_from_dev(dev);
	u8 buf[49];

	if (!p)
		return -ENODEV;
	if (pair > 8)
		return -EINVAL;
	buf[0] = PIC_COLS_BASE + pair;
	memcpy(&buf[1], rgb, 48);
	return pic_tx_cmd(p, buf, sizeof(buf));
}
EXPORT_SYMBOL_GPL(deluge_pic_pad_pair);

int deluge_pic_pad_refresh(struct device *dev)
{
	struct deluge_pic *p = pic_from_dev(dev);
	u8 b = PIC_DONE_ROWS;

	if (!p)
		return -ENODEV;
	return pic_tx_cmd(p, &b, 1);
}
EXPORT_SYMBOL_GPL(deluge_pic_pad_refresh);

static ssize_t refresh_time_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct deluge_pic *p = dev_get_drvdata(dev);

	if (p->refresh_time < 0)
		return sysfs_emit(buf, "unset\n");
	return sysfs_emit(buf, "%d\n", p->refresh_time);
}

static ssize_t refresh_time_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct deluge_pic *p = dev_get_drvdata(dev);
	u8 cmd[2];
	u8 val;
	int ret;

	ret = kstrtou8(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 25)
		return -EINVAL;
	cmd[0] = PIC_SET_REFRESH;
	cmd[1] = val;
	ret = pic_tx_cmd(p, cmd, sizeof(cmd));
	if (ret)
		return ret;
	p->refresh_time = val;
	return count;
}
static DEVICE_ATTR_RW(refresh_time);

static ssize_t rx_overruns_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct deluge_pic *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", p->rx_overruns);
}
static DEVICE_ATTR_RO(rx_overruns);

static struct attribute *pic_attrs[] = {
	&dev_attr_refresh_time.attr,
	&dev_attr_rx_overruns.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pic);

static int pic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct deluge_pic *p;
	struct resource *res;
	int i, ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	init_completion(&p->oled_sel);
	init_completion(&p->oled_desel);
	mutex_init(&p->tx_lock);
	p->refresh_time = -1;
	platform_set_drvdata(pdev, p);

	p->scif = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(p->scif))
		return PTR_ERR(p->scif);

	p->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(p->clk))
		return PTR_ERR(p->clk);

	spin_lock_init(&p->pad_lock);
	spin_lock_init(&p->rx_lock);

	p->btn_input = devm_input_allocate_device(dev);
	if (!p->btn_input)
		return -ENOMEM;
	p->btn_input->name = "Deluge Buttons";
	p->btn_input->phys = "deluge/pic/buttons";
	p->btn_input->id.bustype = BUS_HOST;
	for (i = 0; i < PIC_BTNS; i++)
		input_set_capability(p->btn_input, EV_KEY, PIC_BTN_BASE + i);
	ret = input_register_device(p->btn_input);
	if (ret)
		return ret;

	ret = pic_start_dma(p, res->start);
	if (ret)
		return ret;

	/*
	 * Bring up the SCIF receiver (deluge-sdk init order): disable during
	 * reconfig, flush the RX/TX FIFOs, clear stale status, release the FIFO
	 * reset with RX trigger = 1 byte (RTRG=0) so every byte raises a DMA
	 * request, clear any latched overrun, then enable RX. RIE is what gates
	 * the RX DMA request on RZ/A1. Baud was already set by the app-loader.
	 */
	writew(0, p->scif + SCIF_SCSCR);
	writew(SCFCR_RFRST | SCFCR_TFRST, p->scif + SCIF_SCFCR);
	writew(readw(p->scif + SCIF_SCFSR) & SCFSR_KEEP, p->scif + SCIF_SCFSR);
	writew(0, p->scif + SCIF_SCFCR);
	writew(0, p->scif + SCIF_SCLSR);
	writew(SCSCR_RX_ON, p->scif + SCIF_SCSCR);

	hrtimer_setup(&p->poll, pic_poll, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&p->poll, ns_to_ktime((u64)PIC_POLL_US * 1000),
		      HRTIMER_MODE_REL);

	dev_info(dev, "Deluge PIC panel ready (SCIF1 @ %pa, rz-dmac slave_sg RX + %u us poll)\n",
		 &res->start, PIC_POLL_US);
	return 0;
}

static void pic_remove(struct platform_device *pdev)
{
	struct deluge_pic *p = platform_get_drvdata(pdev);

	writew(0, p->scif + SCIF_SCSCR);	/* disable RX */
	hrtimer_cancel(&p->poll);
	dmaengine_terminate_sync(p->rx);
	dma_release_channel(p->rx);
	if (p->sram)
		gen_pool_free(p->sram, (unsigned long)p->ring, PIC_RING_LEN);
	else
		dma_free_coherent(p->dev, PIC_RING_LEN, p->ring, p->ring_dma);
}

static const struct of_device_id pic_of[] = {
	{ .compatible = "synthstrom,deluge-pic" },
	{ }
};
MODULE_DEVICE_TABLE(of, pic_of);

static struct platform_driver pic_driver = {
	.probe = pic_probe,
	.remove = pic_remove,
	.driver = {
		.name = "deluge-pic",
		.of_match_table = pic_of,
		.dev_groups = pic_groups,
	},
};
module_platform_driver(pic_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge PIC front-panel link (SCIF1 + re-armed slave_sg DMA RX)");
