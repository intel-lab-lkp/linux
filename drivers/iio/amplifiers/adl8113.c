// SPDX-License-Identifier: GPL-2.0
/*
 * ADL8113 Low Noise Amplifier with integrated bypass switches
 *
 * Copyright 2025 Analog Devices Inc.
 */

#include <linux/array_size.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

enum adl8113_gain_mode {
	ADL8113_AMPLIFIER,
	ADL8113_BYPASS,
};

enum adl8113_signal_path {
	ADL8113_INTERNAL,
	ADL8113_EXTERNAL_A,
	ADL8113_EXTERNAL_B,
};

struct adl8113_state {
	struct gpio_desc *gpio_va;
	struct gpio_desc *gpio_vb;
	enum adl8113_gain_mode gain_mode;
	enum adl8113_signal_path signal_path;
};

static const char * const adl8113_supply_names[] = {
	"vdd1",
	"vdd2",
	"vss2"
};

static const char * const adl8113_signal_path_names[] = {
	[ADL8113_INTERNAL] = "internal",
	[ADL8113_EXTERNAL_A] = "external_a",
	[ADL8113_EXTERNAL_B] = "external_b",
};

static int adl8113_update_gpio(struct adl8113_state *st)
{
	int va, vb;

	/* Determine GPIO values based on gain mode and signal path */
	switch (st->gain_mode) {
	case ADL8113_AMPLIFIER:
		if (st->signal_path != ADL8113_INTERNAL)
			return -EINVAL;
		va = 0; vb = 0; /* Internal amplifier */
		break;
	case ADL8113_BYPASS:
		switch (st->signal_path) {
		case ADL8113_INTERNAL:
			va = 1; vb = 1; /* Internal bypass */
			break;
		case ADL8113_EXTERNAL_A:
			va = 0; vb = 1; /* External bypass A */
			break;
		case ADL8113_EXTERNAL_B:
			va = 1; vb = 0; /* External bypass B */
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	gpiod_set_value(st->gpio_va, va);
	gpiod_set_value(st->gpio_vb, vb);
	return 0;
}

static int adl8113_get_signal_path(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan)
{
	struct adl8113_state *st = iio_priv(indio_dev);

	return st->signal_path;
}

static int adl8113_set_signal_path(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan,
				   unsigned int path)
{
	struct adl8113_state *st = iio_priv(indio_dev);

	if (path >= ARRAY_SIZE(adl8113_signal_path_names))
		return -EINVAL;

	/* External paths require bypass mode, so switch to it automatically */
	if (path != ADL8113_INTERNAL)
		st->gain_mode = ADL8113_BYPASS;

	st->signal_path = path;
	return adl8113_update_gpio(st);
}

static const struct iio_enum adl8113_signal_path_enum = {
	.items = adl8113_signal_path_names,
	.num_items = ARRAY_SIZE(adl8113_signal_path_names),
	.get = adl8113_get_signal_path,
	.set = adl8113_set_signal_path,
};

static const struct iio_chan_spec_ext_info adl8113_ext_info[] = {
	IIO_ENUM("signal_path", IIO_SHARED_BY_ALL, &adl8113_signal_path_enum),
	IIO_ENUM_AVAILABLE("signal_path", IIO_SHARED_BY_ALL,
			   &adl8113_signal_path_enum),
	{ },
};

static const struct iio_chan_spec adl8113_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.indexed = 1,
		.channel = 0,
		.ext_info = adl8113_ext_info,
	},
};

static int adl8113_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct adl8113_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		if (st->gain_mode == ADL8113_AMPLIFIER) {
			/* Amplifier mode: 14dB gain */
			*val = 14;
			*val2 = 0;
			return IIO_VAL_INT_PLUS_MICRO_DB;
		}

		/* Bypass mode: 0dB gain */
		*val = 0;
		*val2 = 0;
		return IIO_VAL_INT_PLUS_MICRO_DB;
	default:
		return -EINVAL;
	}
}

static int adl8113_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct adl8113_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		/* Accept dB values: 0dB (bypass) or 14dB (amplifier) */
		if (val == 0 && val2 == 0) {
			st->gain_mode = ADL8113_BYPASS;
		} else if (val == 14 && val2 == 0) {
			st->gain_mode = ADL8113_AMPLIFIER;
			/* Force internal path for amplifier mode */
			st->signal_path = ADL8113_INTERNAL;
		} else {
			return -EINVAL;
		}
		return adl8113_update_gpio(st);
	default:
		return -EINVAL;
	}
}

static const struct iio_info adl8113_info = {
	.read_raw = adl8113_read_raw,
	.write_raw = adl8113_write_raw,
};

static int adl8113_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct adl8113_state *st;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	st->gpio_va = devm_gpiod_get(dev, "va", GPIOD_OUT_LOW);
	if (IS_ERR(st->gpio_va))
		return dev_err_probe(dev, PTR_ERR(st->gpio_va),
				     "failed to get VA GPIO\n");

	st->gpio_vb = devm_gpiod_get(dev, "vb", GPIOD_OUT_LOW);
	if (IS_ERR(st->gpio_vb))
		return dev_err_probe(dev, PTR_ERR(st->gpio_vb),
				     "failed to get VB GPIO\n");

	ret = devm_regulator_bulk_get_enable(dev,
					     ARRAY_SIZE(adl8113_supply_names),
					     adl8113_supply_names);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get and enable supplies\n");

	/* Initialize to amplifier mode with internal path */
	st->gain_mode = ADL8113_AMPLIFIER;
	st->signal_path = ADL8113_INTERNAL;
	ret = adl8113_update_gpio(st);
	if (ret)
		return ret;

	indio_dev->info = &adl8113_info;
	indio_dev->name = "adl8113";
	indio_dev->channels = adl8113_channels;
	indio_dev->num_channels = ARRAY_SIZE(adl8113_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id adl8113_of_match[] = {
	{ .compatible = "adi,adl8113" },
	{ }
};
MODULE_DEVICE_TABLE(of, adl8113_of_match);

static struct platform_driver adl8113_driver = {
	.driver = {
		.name = "adl8113",
		.of_match_table = adl8113_of_match,
	},
	.probe = adl8113_probe,
};

module_platform_driver(adl8113_driver);

MODULE_AUTHOR("Antoniu Miclaus <antoniu.miclaus@analog.com>");
MODULE_DESCRIPTION("Analog Devices ADL8113 Low Noise Amplifier");
MODULE_LICENSE("GPL");
