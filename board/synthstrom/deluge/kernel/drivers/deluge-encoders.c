// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge rotary encoders (6x).
 *
 * Each encoder has phase A on an RZ/A1 IRQC external-interrupt line and phase B
 * on a plain P1 GPIO. We can't use the mainline rotary-encoder driver: it needs
 * an IRQ on both phases, but the RZ/A1 has only 8 IRQ lines (the 6 A-phases take
 * 6) and the B-phases aren't IRQ-capable. So, mirroring the bare-metal SDK, we
 * take a both-edge IRQ on A and read A+B levels from the P1 port register in the
 * handler: cw = (A == B) (inverted for two encoders), 2 edges = 1 detent.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/of.h>

/* div = A-edges per reported step: 2 for detented encoders (1 step/detent),
 * 1 for the detentless gold knobs (full resolution, 1 step/edge). */
struct denc_desc { u8 a, b; bool inv; unsigned int code; u8 div; const char *name; };

/* Ordered to match the DT `interrupts` list (by IRQ line: 0,1,2,3,4,7). */
static const struct denc_desc DESC[] = {
	{ 8, 10, false, REL_WHEEL,   2, "scroll_y" },    /* IRQ0 */
	{ 5,  4, false, REL_RY,      1, "gold_top" },    /* IRQ1 (detentless) */
	{ 6,  7, true,  REL_MISC,    2, "tempo" },       /* IRQ2 */
	{ 11, 12, false, REL_HWHEEL, 2, "scroll_x" },    /* IRQ3 */
	{ 0, 15, false, REL_RX,      1, "gold_bottom" }, /* IRQ4 (detentless) */
	{ 3,  2, true,  REL_DIAL,    2, "select" },      /* IRQ7 */
};
#define NENC ARRAY_SIZE(DESC)

struct denc_drv;
struct denc { struct denc_drv *drv; const struct denc_desc *d; s8 acc; };
struct denc_drv {
	struct input_dev *input;
	void __iomem *ppr;
	struct denc enc[NENC];
};

static irqreturn_t denc_isr(int irq, void *data)
{
	struct denc *e = data;
	struct denc_drv *drv = e->drv;
	u16 p = readw(drv->ppr);
	int a = (p >> e->d->a) & 1;
	int b = (p >> e->d->b) & 1;
	int cw = e->d->inv ? (a != b) : (a == b);
	int d = e->d->div;

	e->acc += cw ? 1 : -1;
	while (e->acc >= d)  { e->acc -= d; input_report_rel(drv->input, e->d->code,  1); input_sync(drv->input); }
	while (e->acc <= -d) { e->acc += d; input_report_rel(drv->input, e->d->code, -1); input_sync(drv->input); }
	return IRQ_HANDLED;
}

static int denc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct denc_drv *drv;
	int i, ret, n;
	u32 ppr;

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	/*
	 * DESC[] is positional: DESC[i] describes the encoder whose phase-A IRQ is
	 * the i-th entry in the DT "interrupts" list (see the DESC[] comment above).
	 * Guard the length half of that contract — a DT interrupts list of the wrong
	 * count means the C table and the DT have drifted apart. (A same-length
	 * reorder isn't caught here, but shows up immediately as mis-routed encoders
	 * on first use.)
	 */
	n = platform_irq_count(pdev);
	if (n < 0)
		return dev_err_probe(dev, n, "counting encoder interrupts\n");
	if (n != (int)NENC)
		return dev_err_probe(dev, -EINVAL,
				     "DT lists %d encoder interrupts, DESC[] has %zu — reconcile the interrupts list with DESC[]\n",
				     n, (size_t)NENC);

	ret = of_property_read_u32(dev->of_node, "synthstrom,port-read-reg", &ppr);
	if (ret)
		return dev_err_probe(dev, ret, "no synthstrom,port-read-reg\n");
	drv->ppr = devm_ioremap(dev, ppr, 4);
	if (!drv->ppr)
		return -ENOMEM;

	drv->input = devm_input_allocate_device(dev);
	if (!drv->input)
		return -ENOMEM;
	drv->input->name = "Synthstrom Deluge Encoders";
	drv->input->phys = "deluge/encoders";
	drv->input->id.bustype = BUS_HOST;
	for (i = 0; i < NENC; i++)
		input_set_capability(drv->input, EV_REL, DESC[i].code);

	for (i = 0; i < NENC; i++) {
		int irq = platform_get_irq(pdev, i);

		if (irq < 0)
			return irq;
		drv->enc[i].drv = drv;
		drv->enc[i].d = &DESC[i];
		drv->enc[i].acc = 0;
		ret = devm_request_irq(dev, irq, denc_isr, 0, DESC[i].name, &drv->enc[i]);
		if (ret)
			return dev_err_probe(dev, ret, "irq %d (%s)\n", irq, DESC[i].name);
	}

	ret = input_register_device(drv->input);
	if (ret)
		return ret;
	dev_info(dev, "6 encoders registered\n");
	return 0;
}

static const struct of_device_id denc_of[] = {
	{ .compatible = "synthstrom,deluge-encoders" },
	{ }
};
MODULE_DEVICE_TABLE(of, denc_of);

static struct platform_driver denc_driver = {
	.probe = denc_probe,
	.driver = { .name = "deluge-encoders", .of_match_table = denc_of },
};
module_platform_driver(denc_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge rotary encoders");
