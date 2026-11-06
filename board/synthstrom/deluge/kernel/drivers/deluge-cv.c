// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge CV outputs — MAX5136 quad 16-bit SPI DAC (2 channels wired).
 *
 * The DAC shares RSPI0 with the OLED. To avoid switching the controller between
 * the OLED's 8-bit word size and a 32-bit frame (which the bare-metal SDK does
 * via enter_32bit()), we send the MAX5136's 32-bit write frame as a single
 * 4-byte transfer at bits_per_word = 8, with the P6.1 gpio chip-select held by
 * the SPI core across the whole frame. Same 32 CS-low clocks on the wire; the
 * controller is never reconfigured, so OLED transfers are undisturbed.
 *
 * MAX5136 write frame (MSB first): { 0x30 | (1<<ch), value[15:8], value[7:0], 0 }.
 */
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/iio/iio.h>

#define DELUGE_CV_CHANNELS	2

/*
 * MAX5136 + the Deluge output stage: 1 V out = 6552 DAC counts (so the 16-bit
 * range spans ~0..10 V, unipolar, 0 counts = 0 V). From the original
 * DelugeFirmware cv_engine.cpp calculateVoltage(): "Represents 1V as 6552. So
 * 10V is 65520." IIO voltage is in millivolts, so the per-count scale is
 * 1000 / 6552 mV.
 */
#define DELUGE_CV_COUNTS_PER_VOLT	6552

struct deluge_cv {
	struct spi_device *spi;
	u16 cache[DELUGE_CV_CHANNELS];
};

/* One 4-byte MAX5136 frame via a single CS-held transfer. */
static int deluge_cv_write_word(struct spi_device *spi, u32 word)
{
	u8 buf[4] = { word >> 24, word >> 16, word >> 8, word };

	return spi_write(spi, buf, sizeof(buf));
}

static int deluge_cv_set(struct deluge_cv *cv, unsigned int ch, u16 value)
{
	u32 word = ((0x30u | (1u << ch)) << 24) | ((u32)value << 8);
	int ret = deluge_cv_write_word(cv->spi, word);

	if (!ret)
		cv->cache[ch] = value;
	return ret;
}

#define DELUGE_CV_CHANNEL(n) {					\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.output = 1,						\
	.channel = (n),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.scan_type = { .sign = 'u', .realbits = 16, .storagebits = 16 }, \
}

static const struct iio_chan_spec deluge_cv_iio_channels[] = {
	DELUGE_CV_CHANNEL(0),
	DELUGE_CV_CHANNEL(1),
};

static int deluge_cv_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long info)
{
	struct deluge_cv *cv = iio_priv(indio_dev);

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		*val = cv->cache[chan->channel];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/* millivolts per count = 1000 / 6552 (see DELUGE_CV_COUNTS_PER_VOLT) */
		*val = 1000;
		*val2 = DELUGE_CV_COUNTS_PER_VOLT;
		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static int deluge_cv_write_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int val, int val2, long info)
{
	struct deluge_cv *cv = iio_priv(indio_dev);

	if (info != IIO_CHAN_INFO_RAW)
		return -EINVAL;
	if (val < 0 || val > 65535)
		return -EINVAL;
	return deluge_cv_set(cv, chan->channel, val);
}

static const struct iio_info deluge_cv_info = {
	.read_raw = deluge_cv_read_raw,
	.write_raw = deluge_cv_write_raw,
};

static int deluge_cv_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct deluge_cv *cv;
	int ret, i;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*cv));
	if (!indio_dev)
		return -ENOMEM;

	cv = iio_priv(indio_dev);
	cv->spi = spi;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "spi_setup\n");

	/* MAX5136 linearity init: LIN=1, >=10 ms, LIN=0 (datasheet). */
	ret = deluge_cv_write_word(spi, 0x05420000);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "LIN=1\n");
	msleep(10);
	ret = deluge_cv_write_word(spi, 0x05400000);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "LIN=0\n");

	for (i = 0; i < DELUGE_CV_CHANNELS; i++) {
		ret = deluge_cv_set(cv, i, 0);
		if (ret)
			return dev_err_probe(&spi->dev, ret, "zero ch%d\n", i);
	}

	indio_dev->name = "deluge-cv";
	indio_dev->info = &deluge_cv_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = deluge_cv_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(deluge_cv_iio_channels);

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct of_device_id deluge_cv_of[] = {
	{ .compatible = "synthstrom,deluge-cv" },
	{ }
};
MODULE_DEVICE_TABLE(of, deluge_cv_of);

static struct spi_driver deluge_cv_driver = {
	.driver = {
		.name = "deluge-cv",
		.of_match_table = deluge_cv_of,
	},
	.probe = deluge_cv_probe,
};
module_spi_driver(deluge_cv_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge CV (MAX5136 SPI DAC)");
