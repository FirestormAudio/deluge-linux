// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge front-panel LEDs: 36 indicator LEDs (one per button) and
 * two gold-knob 4-LED columns, as Linux LED-class devices. Output is sent over
 * the PIC32 (deluge-pic) on SCIF1.
 */
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/deluge-pic.h>

#define DELUGE_INDICATORS	36
#define DELUGE_GOLD_KNOBS	2
#define DELUGE_GOLD_LEDS	4

struct deluge_leds;

struct deluge_led {
	struct led_classdev cdev;
	struct deluge_leds *parent;
	u8 idx;			/* indicator id 0..35, or gold LED 0..3 */
	u8 knob;		/* gold column 0/1 (unused for indicators) */
	char name[32];
};

struct deluge_leds {
	struct device *pic;
	struct mutex lock;	/* protects gold[] cache + PIC send, per-column */
	u8 gold[DELUGE_GOLD_KNOBS][DELUGE_GOLD_LEDS];
	struct deluge_led ind[DELUGE_INDICATORS];
	struct deluge_led gold_led[DELUGE_GOLD_KNOBS][DELUGE_GOLD_LEDS];
};

static int indicator_set(struct led_classdev *cdev, enum led_brightness b)
{
	struct deluge_led *l = container_of(cdev, struct deluge_led, cdev);

	return deluge_pic_led_set(l->parent->pic, l->idx, b != LED_OFF);
}

static int gold_set(struct led_classdev *cdev, enum led_brightness b)
{
	struct deluge_led *l = container_of(cdev, struct deluge_led, cdev);
	struct deluge_leds *g = l->parent;
	int ret;

	mutex_lock(&g->lock);
	g->gold[l->knob][l->idx] = b;
	ret = deluge_pic_gold_knob(g->pic, l->knob, g->gold[l->knob]);
	mutex_unlock(&g->lock);
	return ret;
}

static int deluge_leds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct deluge_leds *g;
	unsigned int i, k;
	int ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;

	g->pic = deluge_pic_attach(dev);
	if (IS_ERR(g->pic))
		return dev_err_probe(dev, PTR_ERR(g->pic), "attach pic\n");

	mutex_init(&g->lock);

	for (i = 0; i < DELUGE_INDICATORS; i++) {
		struct deluge_led *l = &g->ind[i];

		l->parent = g;
		l->idx = i;
		scnprintf(l->name, sizeof(l->name), "deluge:indicator:%u", i);
		l->cdev.name = l->name;
		l->cdev.max_brightness = 1;
		l->cdev.brightness_set_blocking = indicator_set;
		ret = devm_led_classdev_register(dev, &l->cdev);
		if (ret)
			goto err_pic;
	}

	for (k = 0; k < DELUGE_GOLD_KNOBS; k++) {
		for (i = 0; i < DELUGE_GOLD_LEDS; i++) {
			struct deluge_led *l = &g->gold_led[k][i];

			l->parent = g;
			l->knob = k;
			l->idx = i;
			scnprintf(l->name, sizeof(l->name),
				  "deluge:gold%u:%u", k, i);
			l->cdev.name = l->name;
			l->cdev.max_brightness = 255;
			l->cdev.brightness_set_blocking = gold_set;
			ret = devm_led_classdev_register(dev, &l->cdev);
			if (ret)
				goto err_pic;
		}
	}

	platform_set_drvdata(pdev, g);
	dev_info(dev, "Deluge LEDs: %u indicators + %u gold columns\n",
		 DELUGE_INDICATORS, DELUGE_GOLD_KNOBS);
	return 0;

err_pic:
	deluge_pic_put(g->pic);
	return ret;
}

static void deluge_leds_remove(struct platform_device *pdev)
{
	struct deluge_leds *g = platform_get_drvdata(pdev);

	deluge_pic_put(g->pic);
}

static const struct of_device_id deluge_leds_of[] = {
	{ .compatible = "synthstrom,deluge-leds" },
	{ }
};
MODULE_DEVICE_TABLE(of, deluge_leds_of);

static struct platform_driver deluge_leds_driver = {
	.probe = deluge_leds_probe,
	.remove = deluge_leds_remove,
	.driver = {
		.name = "deluge-leds",
		.of_match_table = deluge_leds_of,
	},
};
module_platform_driver(deluge_leds_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge front-panel LEDs over the PIC");
