// SPDX-License-Identifier: GPL-2.0
/*
 * ADMFM2000 Dual Microwave Down Converter
 *
 * Copyright 2023 Analog Devices Inc.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#define ADMFM2000_MIXER_MODE		0
#define ADMFM2000_DIRECT_IF_MODE	1
#define ADMF20000_DSA_GPIOS		5
#define ADMF20000_MODE_GPIOS		2
#define ADMF20000_MAX_GAIN		0
#define ADMF20000_MIN_GAIN		-31000
#define ADMF20000_DEFAULT_GAIN		-0x20

struct admfm2000_state {
	struct mutex			lock; /* protect sensor state */
	struct gpio_descs		*sw_ch[2];
	struct gpio_descs		*dsa_gpios[2];
	u32				gain[2];
};

static int admfm2000_mode(struct iio_dev *indio_dev, u32 reg, u32 mode)
{
	struct admfm2000_state *st = iio_priv(indio_dev);
	DECLARE_BITMAP(values, 2);

	switch (mode) {
	case ADMFM2000_MIXER_MODE:
		values[0] = (reg == 0) ? 1 : 2;
		gpiod_set_array_value_cansleep(st->sw_ch[reg]->ndescs,
					       st->sw_ch[reg]->desc,
					       NULL, values);
		break;
	case ADMFM2000_DIRECT_IF_MODE:
		values[0] = (reg == 0) ? 2 : 1;
		gpiod_set_array_value_cansleep(st->sw_ch[reg]->ndescs,
					       st->sw_ch[reg]->desc,
					       NULL, values);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int admfm2000_attenuation(struct iio_dev *indio_dev, u32 chan,
				 u32 value)
{
	struct admfm2000_state *st = iio_priv(indio_dev);
	DECLARE_BITMAP(values, BITS_PER_TYPE(value));

	values[0] = value;

	gpiod_set_array_value_cansleep(st->dsa_gpios[chan]->ndescs,
				       st->dsa_gpios[chan]->desc,
				       NULL, values);
	return 0;
}

static int admfm2000_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, int *val,
			      int *val2, long mask)
{
	struct admfm2000_state *st = iio_priv(indio_dev);
	int gain;

	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		mutex_lock(&st->lock);
		gain = ~(st->gain[chan->channel]) * -1000;
		*val = gain / 1000;
		*val2 = (gain % 1000) * 1000;
		mutex_unlock(&st->lock);

		return  IIO_VAL_INT_PLUS_MICRO_DB;
	default:
		return -EINVAL;
	}
}

static int admfm2000_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct admfm2000_state *st = iio_priv(indio_dev);
	int gain, ret;

	if (val < 0)
		gain = (val * 1000) - (val2 / 1000);
	else
		gain = (val * 1000) + (val2 / 1000);

	if (gain > ADMF20000_MAX_GAIN || gain < ADMF20000_MIN_GAIN)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		mutex_lock(&st->lock);
		st->gain[chan->channel] = ~((abs(gain) / 1000) & 0x1F);

		ret = admfm2000_attenuation(indio_dev, chan->channel,
					    st->gain[chan->channel]);

		mutex_unlock(&st->lock);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int admfm2000_write_raw_get_fmt(struct iio_dev *indio_dev,
				       struct iio_chan_spec const *chan,
				       long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		return IIO_VAL_INT_PLUS_MICRO_DB;
	default:
		return -EINVAL;
	}
}

static const struct iio_info admfm2000_info = {
	.read_raw = &admfm2000_read_raw,
	.write_raw = &admfm2000_write_raw,
	.write_raw_get_fmt = &admfm2000_write_raw_get_fmt,
};

#define ADMFM2000_CHAN(_channel) {					\
	.type = IIO_VOLTAGE,						\
	.output = 1,							\
	.indexed = 1,							\
	.channel = _channel,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN),		\
}

static const struct iio_chan_spec admfm2000_channels[] = {
	ADMFM2000_CHAN(0),
	ADMFM2000_CHAN(1),
};

static int admfm2000_channel_config(struct admfm2000_state *st,
				    struct iio_dev *indio_dev)
{
	struct platform_device *pdev = to_platform_device(indio_dev->dev.parent);
	struct device *dev = &pdev->dev;
	struct fwnode_handle *child;
	u32 reg, mode;
	int ret;

	device_for_each_child_node(dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret) {
			fwnode_handle_put(child);
			return dev_err_probe(dev, ret,
					     "Failed to get reg property\n");
		}

		if (reg >= indio_dev->num_channels) {
			fwnode_handle_put(child);
			return dev_err_probe(dev, -EINVAL, "reg bigger than: %d\n",
					     indio_dev->num_channels);
		}

		ret = fwnode_property_read_u32(child, "adi,mode", &mode);
		if (ret) {
			fwnode_handle_put(child);
			return dev_err_probe(dev, ret,
					     "Failed to get mode property\n");
		}

		if (mode >= 2) {
			fwnode_handle_put(child);
			return dev_err_probe(dev, -EINVAL, "mode bigger than: 1\n");
		}

		ret = admfm2000_mode(indio_dev, reg, mode);
		if (ret) {
			fwnode_handle_put(child);
			return ret;
		}
	}

	return 0;
}

static int admfm2000_setup(struct admfm2000_state *st,
			   struct iio_dev *indio_dev)
{
	struct platform_device *pdev = to_platform_device(indio_dev->dev.parent);
	struct device *dev = &pdev->dev;

	st->sw_ch[0] = devm_gpiod_get_array(dev, "switch1", GPIOD_OUT_LOW);
	if (IS_ERR(st->sw_ch[0]))
		return dev_err_probe(dev, PTR_ERR(st->sw_ch[0]),
				     "Failed to get gpios\n");

	if (st->sw_ch[0]->ndescs != ADMF20000_MODE_GPIOS) {
		dev_err_probe(dev, -ENODEV, "%d GPIOs needed to operate\n",
			      ADMF20000_MODE_GPIOS);
		return -ENODEV;
	}

	st->sw_ch[1] = devm_gpiod_get_array(dev, "switch2", GPIOD_OUT_LOW);
	if (IS_ERR(st->sw_ch[1]))
		return dev_err_probe(dev, PTR_ERR(st->sw_ch[1]),
				     "Failed to get gpios\n");

	if (st->sw_ch[1]->ndescs != ADMF20000_MODE_GPIOS) {
		dev_err_probe(dev, -ENODEV, "%d GPIOs needed to operate\n",
			      ADMF20000_MODE_GPIOS);
		return -ENODEV;
	}

	st->dsa_gpios[0] = devm_gpiod_get_array(dev, "attenuation1",
						GPIOD_OUT_LOW);
	if (IS_ERR(st->dsa_gpios[0]))
		return dev_err_probe(dev, PTR_ERR(st->dsa_gpios[0]),
				     "Failed to get gpios\n");

	if (st->dsa_gpios[0]->ndescs != ADMF20000_DSA_GPIOS) {
		dev_err_probe(dev, -ENODEV, "%d GPIOs needed to operate\n",
			      ADMF20000_DSA_GPIOS);
		return -ENODEV;
	}

	st->dsa_gpios[1] = devm_gpiod_get_array(dev, "attenuation2",
						GPIOD_OUT_LOW);
	if (IS_ERR(st->dsa_gpios[1]))
		return dev_err_probe(dev, PTR_ERR(st->dsa_gpios[1]),
				     "Failed to get gpios\n");

	if (st->dsa_gpios[1]->ndescs != ADMF20000_DSA_GPIOS) {
		dev_err_probe(dev, -ENODEV, "%d GPIOs needed to operate\n",
			      ADMF20000_DSA_GPIOS);
	}

	return 0;
}

static int admfm2000_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct admfm2000_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	indio_dev->name = "admfm2000";
	indio_dev->num_channels = ARRAY_SIZE(admfm2000_channels);
	indio_dev->channels = admfm2000_channels;
	indio_dev->info = &admfm2000_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	st->gain[0] = ADMF20000_DEFAULT_GAIN;
	st->gain[1] = ADMF20000_DEFAULT_GAIN;

	mutex_init(&st->lock);

	ret = admfm2000_setup(st, indio_dev);
	if (ret)
		return ret;

	ret = admfm2000_channel_config(st, indio_dev);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id admfm2000_of_match[] = {
	{ .compatible = "adi,admfm2000" },
	{ }
};
MODULE_DEVICE_TABLE(of, admfm2000_of_match);

static struct platform_driver admfm2000_driver = {
	.driver = {
		.name = "admfm2000",
		.of_match_table = admfm2000_of_match,
	},
	.probe = admfm2000_probe,
};
module_platform_driver(admfm2000_driver);

MODULE_AUTHOR("Kim Seer Paller <kimseer.paller@analog.com>");
MODULE_DESCRIPTION("ADMFM2000 Dual Microwave Down Converter");
MODULE_LICENSE("GPL");
