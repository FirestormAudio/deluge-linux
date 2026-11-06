// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge analog clock input.
 *
 * A rear-panel clock signal reaches P1.14 (via a transistor that inverts it),
 * routed to RZ/A1 IRQC external interrupt IRQ6. Each external pulse appears as a
 * falling edge. We take the IRQC interrupt (as deluge-encoders does for the
 * encoder A-phases) and emit one timestamped evdev event per pulse, carrying a
 * monotonically increasing counter so a consumer can detect dropped pulses.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/of.h>

struct deluge_clkin {
	struct input_dev *input;
	u32 count;
};

static irqreturn_t deluge_clkin_isr(int irq, void *data)
{
	struct deluge_clkin *c = data;

	input_event(c->input, EV_MSC, MSC_RAW, ++c->count);
	input_sync(c->input);
	return IRQ_HANDLED;
}

static int deluge_clkin_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct deluge_clkin *c;
	int irq, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->input = devm_input_allocate_device(dev);
	if (!c->input)
		return -ENOMEM;
	c->input->name = "Deluge Clock In";
	c->input->phys = "deluge/clkin";
	c->input->id.bustype = BUS_HOST;
	input_set_capability(c->input, EV_MSC, MSC_RAW);

	ret = input_register_device(c->input);
	if (ret)
		return dev_err_probe(dev, ret, "register input\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, deluge_clkin_isr, 0, "deluge-clkin", c);
	if (ret)
		return dev_err_probe(dev, ret, "request irq %d\n", irq);

	dev_info(dev, "Deluge clock input ready (IRQ %d, falling edge)\n", irq);
	return 0;
}

static const struct of_device_id deluge_clkin_of[] = {
	{ .compatible = "synthstrom,deluge-clkin" },
	{ }
};
MODULE_DEVICE_TABLE(of, deluge_clkin_of);

static struct platform_driver deluge_clkin_driver = {
	.probe = deluge_clkin_probe,
	.driver = {
		.name = "deluge-clkin",
		.of_match_table = deluge_clkin_of,
	},
};
module_platform_driver(deluge_clkin_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge analog clock input (IRQC IRQ6 -> evdev)");
