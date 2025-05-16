// SPDX-License-Identifier: GPL-2.0-only
/*
 * Analog Devices AD7405 driver
 *
 * Copyright 2025 Analog Devices Inc.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/util_macros.h>

#include <linux/iio/backend.h>
#include <linux/iio/iio.h>

static const unsigned int ad7405_scale_table[][2] = {
	{640, 0},
};

static const unsigned int adum7702_scale_table[][2] = {
	{128, 0},
};

static const unsigned int ad7405_dec_rates[] = {
	4096, 2048, 1024, 512, 256, 128, 64, 32,
};

struct ad7405_chip_info {
	const char *name;
	struct iio_chan_spec channel;
	const unsigned int (*scale_table)[2];
};

struct ad7405_state {
	struct iio_backend *back;
	const struct ad7405_chip_info *info;
	unsigned int sample_frequency_tbl[ARRAY_SIZE(ad7405_dec_rates)];
	unsigned int sample_frequency;
	unsigned int ref_frequency;
	unsigned int dec_rate;
};

static void ad7405_fill_samp_freq_table(struct ad7405_state *st)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ad7405_dec_rates); i++)
		st->sample_frequency_tbl[i] =
			DIV_ROUND_CLOSEST_ULL(st->ref_frequency, ad7405_dec_rates[i]);
}

static int ad7405_set_dec_rate(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       unsigned int dec_rate)
{
	struct ad7405_state *st = iio_priv(indio_dev);
	int ret;

	ret = iio_backend_oversampling_ratio_set(st->back, 0, dec_rate);
	if (ret)
		return ret;

	st->dec_rate = dec_rate;

	return 0;
}

static int ad7405_get_scale(struct ad7405_state *st, int *val, int *val2)
{
	unsigned int tmp;

	tmp = (st->info->scale_table[0][0] * 1000000ULL) >>
		    st->info->channel.scan_type.realbits;
	*val = tmp / 1000000;
	*val2 = tmp % 1000000;

	return IIO_VAL_INT_PLUS_NANO;
}

static int ad7405_read_raw(struct iio_dev *indio_dev,
			   const struct iio_chan_spec *chan, int *val,
			   int *val2, long info)
{
	struct ad7405_state *st = iio_priv(indio_dev);

	switch (info) {
	case IIO_CHAN_INFO_SCALE:
		return ad7405_get_scale(st, val, val2);
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		*val = st->dec_rate;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = DIV_ROUND_CLOSEST_ULL(st->ref_frequency, st->dec_rate);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ad7405_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int val,
			    int val2, long info)
{
	switch (info) {
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		return ad7405_set_dec_rate(indio_dev, chan, val);
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (val < 1)
			return -EINVAL;
		return ad7405_set_dec_rate(indio_dev, chan, val);
	default:
		return -EINVAL;
	}
}

static int ad7405_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     const int **vals, int *type, int *length,
			     long info)
{
	switch (info) {
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		*vals = ad7405_dec_rates;
		*length = ARRAY_SIZE(ad7405_dec_rates);
		*type = IIO_VAL_INT;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static const struct iio_info ad7405_iio_info = {
	.read_raw = &ad7405_read_raw,
	.write_raw = &ad7405_write_raw,
	.read_avail = &ad7405_read_avail,
};

#define AD7405_IIO_CHANNEL {					\
	.type = IIO_VOLTAGE,					\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.info_mask_shared_by_all = IIO_CHAN_INFO_SAMP_FREQ |	\
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),	\
	.info_mask_shared_by_all_available =			\
			BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO),	\
	.indexed = 1,						\
	.channel = 0,						\
	.channel2 = 1,						\
	.differential = 1,					\
	.scan_index = 0,					\
	.scan_type = {						\
		.sign = 'u',					\
		.realbits = 16,					\
		.storagebits = 16,				\
	},							\
}

static const struct ad7405_chip_info ad7405_chip_info = {
	.name = "AD7405",
	.scale_table = ad7405_scale_table,
	.channel = AD7405_IIO_CHANNEL,
};

static const struct ad7405_chip_info adum7701_chip_info = {
	.name = "ADUM7701",
	.scale_table = ad7405_scale_table,
	.channel = AD7405_IIO_CHANNEL,
};

static const struct ad7405_chip_info adum7702_chip_info = {
	.name = "ADUM7702",
	.scale_table = adum7702_scale_table,
	.channel = AD7405_IIO_CHANNEL,
};

static const struct ad7405_chip_info adum7703_chip_info = {
	.name = "ADUM7703",
	.scale_table = ad7405_scale_table,
	.channel = AD7405_IIO_CHANNEL,
};

static const char * const ad7405_power_supplies[] = {
	"vdd1",	"vdd2",
};

static int ad7405_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct ad7405_state *st;
	struct clk *clk;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	st->info = device_get_match_data(dev);
	if (!st->info)
		return dev_err_probe(dev, -EINVAL, "no chip info\n");

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(ad7405_power_supplies),
					     ad7405_power_supplies);

	if (ret)
		return dev_err_probe(dev, ret, "failed to get and enable supplies");

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	st->ref_frequency = clk_get_rate(clk);
	if (!st->ref_frequency)
		return -EINVAL;

	ad7405_fill_samp_freq_table(st);

	indio_dev->dev.parent = dev;
	indio_dev->name = st->info->name;
	indio_dev->channels = &st->info->channel;
	indio_dev->num_channels = 1;
	indio_dev->info = &ad7405_iio_info;

	st->back = devm_iio_backend_get(dev, NULL);
	if (IS_ERR(st->back))
		return dev_err_probe(dev, PTR_ERR(st->back),
				     "failed to get IIO backend");

	ret = iio_backend_chan_enable(st->back, 0);
	if (ret)
		return ret;

	ret = devm_iio_backend_request_buffer(dev, st->back, indio_dev);
	if (ret)
		return ret;

	ret = devm_iio_backend_enable(dev, st->back);
	if (ret)
		return ret;

	ret = ad7405_set_dec_rate(indio_dev, &indio_dev->channels[0], 256);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

/* Match table for of_platform binding */
static const struct of_device_id ad7405_of_match[] = {
	{ .compatible = "adi,ad7405", .data = &ad7405_chip_info, },
	{ .compatible = "adi,adum7701", .data = &adum7701_chip_info, },
	{ .compatible = "adi,adum7702", .data = &adum7702_chip_info, },
	{ .compatible = "adi,adum7703", .data = &adum7703_chip_info, },
	{ },
};
MODULE_DEVICE_TABLE(of, ad7405_of_match);

static struct platform_driver ad7405_driver = {
	.driver = {
		.name = "ad7405",
		.owner = THIS_MODULE,
		.of_match_table = ad7405_of_match,
	},
	.probe = ad7405_probe,
};
module_platform_driver(ad7405_driver);

MODULE_AUTHOR("Dragos Bogdan <dragos.bogdan@analog.com>");
MODULE_AUTHOR("Pop Ioan Daniel <pop.ioan-daniel@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD7405 driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_BACKEND");
