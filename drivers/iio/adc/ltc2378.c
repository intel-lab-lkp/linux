// SPDX-License-Identifier: GPL-2.0+
/*
 * Analog Devices LTC2378 ADC series driver
 *
 * Copyright (C) 2026 Analog Devices Inc.
 * Author: Ioan-Daniel Pop <pop.ioan-daniel@analog.com>
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include "ltc2378.h"

enum ltc2378_supported_device_ids {
	ID_LTC2338_18,
	ID_LTC2364_16,
	ID_LTC2364_18,
	ID_LTC2367_16,
	ID_LTC2367_18,
	ID_LTC2368_16,
	ID_LTC2368_18,
	ID_LTC2369_18,
	ID_LTC2370_16,
	ID_LTC2376_16,
	ID_LTC2376_18,
	ID_LTC2376_20,
	ID_LTC2377_16,
	ID_LTC2377_18,
	ID_LTC2377_20,
	ID_LTC2378_16,
	ID_LTC2378_18,
	ID_LTC2378_20,
	ID_LTC2379_18,
	ID_LTC2380_16,
};

static const struct ltc2378_chip_info ltc2378_chip_info[] = {
	[ID_LTC2338_18] = {
		.name = "ltc2338-18",
		.resolution = 18,
		.max_sample_rate_hz = HZ_PER_MHZ,
		.tconv_ns = 527,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2364_16] = {
		.name = "ltc2364-16",
		.resolution = 16,
		.max_sample_rate_hz = 250 * HZ_PER_KHZ,
		.tconv_ns = 3000,
		.out_format = IIO_SCAN_FORMAT_UNSIGNED_INT,
	},
	[ID_LTC2364_18] = {
		.name = "ltc2364-18",
		.resolution = 18,
		.max_sample_rate_hz = 250 * HZ_PER_KHZ,
		.tconv_ns = 3000,
		.out_format = IIO_SCAN_FORMAT_UNSIGNED_INT,
	},
	[ID_LTC2367_16] = {
		.name = "ltc2367-16",
		.resolution = 16,
		.max_sample_rate_hz = 500 * HZ_PER_KHZ,
		.tconv_ns = 1500,
		.out_format = IIO_SCAN_FORMAT_UNSIGNED_INT,
	},
	[ID_LTC2367_18] = {
		.name = "ltc2367-18",
		.resolution = 18,
		.max_sample_rate_hz = 500 * HZ_PER_KHZ,
		.tconv_ns = 1500,
		.out_format = IIO_SCAN_FORMAT_UNSIGNED_INT,
	},
	[ID_LTC2368_16] = {
		.name = "ltc2368-16",
		.resolution = 16,
		.max_sample_rate_hz = HZ_PER_MHZ,
		.tconv_ns = 527,
		.out_format = IIO_SCAN_FORMAT_UNSIGNED_INT,
	},
	[ID_LTC2368_18] = {
		.name = "ltc2368-18",
		.resolution = 18,
		.max_sample_rate_hz = HZ_PER_MHZ,
		.tconv_ns = 527,
		.out_format = IIO_SCAN_FORMAT_UNSIGNED_INT,
	},
	[ID_LTC2369_18] = {
		.name = "ltc2369-18",
		.resolution = 18,
		.max_sample_rate_hz = 1600 * HZ_PER_KHZ,
		.tconv_ns = 412,
		.out_format = IIO_SCAN_FORMAT_UNSIGNED_INT,
	},
	[ID_LTC2370_16] = {
		.name = "ltc2370-16",
		.resolution = 16,
		.max_sample_rate_hz = 2 * HZ_PER_MHZ,
		.tconv_ns = 322,
		.out_format = IIO_SCAN_FORMAT_UNSIGNED_INT,
	},
	[ID_LTC2376_16] = {
		.name = "ltc2376-16",
		.resolution = 16,
		.max_sample_rate_hz = 250 * HZ_PER_KHZ,
		.tconv_ns = 3000,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2376_18] = {
		.name = "ltc2376-18",
		.resolution = 18,
		.max_sample_rate_hz = 250 * HZ_PER_KHZ,
		.tconv_ns = 3000,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2376_20] = {
		.name = "ltc2376-20",
		.resolution = 20,
		.max_sample_rate_hz = 250 * HZ_PER_KHZ,
		.tconv_ns = 3000,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2377_16] = {
		.name = "ltc2377-16",
		.resolution = 16,
		.max_sample_rate_hz = 500 * HZ_PER_KHZ,
		.tconv_ns = 1500,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2377_18] = {
		.name = "ltc2377-18",
		.resolution = 18,
		.max_sample_rate_hz = 500 * HZ_PER_KHZ,
		.tconv_ns = 1500,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2377_20] = {
		.name = "ltc2377-20",
		.resolution = 20,
		.max_sample_rate_hz = 500 * HZ_PER_KHZ,
		.tconv_ns = 1500,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2378_16] = {
		.name = "ltc2378-16",
		.resolution = 16,
		.max_sample_rate_hz = HZ_PER_MHZ,
		.tconv_ns = 527,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2378_18] = {
		.name = "ltc2378-18",
		.resolution = 18,
		.max_sample_rate_hz = HZ_PER_MHZ,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
		.tconv_ns = 527,
	},
	[ID_LTC2378_20] = {
		.name = "ltc2378-20",
		.resolution = 20,
		.max_sample_rate_hz = HZ_PER_MHZ,
		.tconv_ns = 675,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2379_18] = {
		.name = "ltc2379-18",
		.resolution = 18,
		.max_sample_rate_hz = 1600 * HZ_PER_KHZ,
		.tconv_ns = 412,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
	[ID_LTC2380_16] = {
		.name = "ltc2380-16",
		.resolution = 16,
		.max_sample_rate_hz = 2 * HZ_PER_MHZ,
		.tconv_ns = 322,
		.out_format = IIO_SCAN_FORMAT_SIGNED_INT,
	},
};

static int ltc2378_convert_and_acquire(struct ltc2378_state *st)
{
	int ret;

	/* Cause a rising edge of CNV to initiate a new ADC conversion */
	gpiod_set_value_cansleep(st->cnv_gpio, 1);
	ret = spi_sync_transfer(st->spi, &st->xfer, 1);
	gpiod_set_value_cansleep(st->cnv_gpio, 0);

	return ret;
}

static int ltc2378_channel_single_read(const struct iio_chan_spec *chan,
				       struct ltc2378_state *st, int *val)
{
	const struct iio_scan_type *scan_type = &chan->scan_type;
	u32 sample;
	int ret;

	ret = ltc2378_convert_and_acquire(st);
	if (ret)
		return ret;

	if (scan_type->endianness == IIO_BE) {
		if (scan_type->realbits > 16)
			sample = be32_to_cpu(st->scan.data.sample_buf32_be);
		else
			sample = be16_to_cpu(st->scan.data.sample_buf16_be);
	} else {
		if (scan_type->realbits > 16)
			sample = st->scan.data.sample_buf32;
		else
			sample = st->scan.data.sample_buf16;
	}

	sample >>= scan_type->shift;

	if (scan_type->format == IIO_SCAN_FORMAT_SIGNED_INT)
		*val = sign_extend32(sample, scan_type->realbits - 1);
	else
		*val = sample;

	return 0;
}

static int ltc2378_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			int *val, int *val2, long info)
{
	struct ltc2378_state *st = iio_priv(indio_dev);
	int ret;

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		ret = ltc2378_channel_single_read(chan, st, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = st->ref_uV / MILLI;
		/*
		 * For all LTC2378-like devices, the amount of bits that express
		 * voltage magnitude depend on the output code format:
		 * - straight binary: All precision/resolution bits are used.
		 * - 2's complement: One of the precision bits is used for sign.
		 */
		if (st->info->out_format == IIO_SCAN_FORMAT_SIGNED_INT)
			*val2 = st->info->resolution - 1;
		else
			*val2 = st->info->resolution;

		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static const struct iio_info ltc2378_iio_info = {
#ifdef CONFIG_LTC2378_OFFLOAD_BUFFER
	.attrs = &ltc2378_offload_attribute_group,
#endif
	.read_raw = &ltc2378_read_raw,
};

static irqreturn_t ltc2378_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ltc2378_state *st = iio_priv(indio_dev);
	int ret;

	ret = ltc2378_convert_and_acquire(st);
	if (ret < 0)
		goto err_out;

	iio_push_to_buffers_with_ts(indio_dev, &st->scan, sizeof(st->scan),
				    pf->timestamp);

err_out:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int ltc2378_probe(struct spi_device *spi)
{
	struct iio_chan_spec *ltc2378_chan;
	struct device *dev = &spi->dev;
	unsigned int num_iio_chans = 1;
	struct iio_dev *indio_dev;
	struct ltc2378_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;

	st->ref_uV = devm_regulator_get_enable_read_voltage(dev, "ref");
	if (st->ref_uV < 0)
		return dev_err_probe(dev, -ENODEV, "failed to read ref regulator\n");

	st->info = spi_get_device_match_data(spi);
	if (!st->info)
		return -EINVAL;

	indio_dev->name = st->info->name;
	indio_dev->info = &ltc2378_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	st->cnv_gpio = devm_gpiod_get_optional(dev, "cnv", GPIOD_OUT_LOW);
	if (st->cnv_gpio && IS_ERR(st->cnv_gpio))
		return dev_err_probe(dev, PTR_ERR(st->cnv_gpio),
				     "failed to get CNV GPIO");

	ltc2378_chan = devm_kzalloc(&spi->dev, 2 * sizeof(struct iio_chan_spec), GFP_KERNEL);
	if (!ltc2378_chan)
		return -ENOMEM;

	ltc2378_chan[0] = (struct iio_chan_spec) {
		.type = IIO_VOLTAGE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_type = {
			.format = st->info->out_format,
			.realbits = st->info->resolution,
			.storagebits = st->info->resolution > 16 ? 32 : 16,
			.shift = st->info->resolution > 16 ? 8 : 0,
			.endianness = IIO_BE,
		},
	};
	st->xfer.rx_buf = &st->scan.data;
	st->xfer.len = BITS_TO_BYTES(ltc2378_chan->scan_type.storagebits);

	ret = ltc2378_offload_buffer_setup(indio_dev, spi);
	if (ret == -ENODEV) {
		/* SPI offloading is unavailable. Fall back to triggered buffer. */
		ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
						      &iio_pollfunc_store_time,
						      &ltc2378_trigger_handler,
						      NULL);
		if (ret)
			return ret;

		/* Add timestamp channel */
		struct iio_chan_spec ts_chan = IIO_CHAN_SOFT_TIMESTAMP(1);

		ltc2378_chan[1] = ts_chan;
		num_iio_chans++;
	} else if (ret) {
		return dev_err_probe(dev, ret, "error on SPI offload setup\n");
	} else {
		/*
		 * Currently, the available offload hardware + DMA configuration
		 * only supports pushing data to IIO buffers in CPU endianness.
		 * That also requires we apply no shift to scan elements to
		 * correctly read ADC sample data.
		 */
		ltc2378_chan->scan_type.shift = 0;
		ltc2378_chan->scan_type.endianness = IIO_CPU;
	}

	indio_dev->channels = ltc2378_chan;
	indio_dev->num_channels = num_iio_chans;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct of_device_id ltc2378_of_match[] = {
	{ .compatible = "adi,ltc2338-18", .data = &ltc2378_chip_info[ID_LTC2338_18] },
	{ .compatible = "adi,ltc2364-16", .data = &ltc2378_chip_info[ID_LTC2364_16] },
	{ .compatible = "adi,ltc2364-18", .data = &ltc2378_chip_info[ID_LTC2364_18] },
	{ .compatible = "adi,ltc2367-16", .data = &ltc2378_chip_info[ID_LTC2367_16] },
	{ .compatible = "adi,ltc2367-18", .data = &ltc2378_chip_info[ID_LTC2367_18] },
	{ .compatible = "adi,ltc2368-16", .data = &ltc2378_chip_info[ID_LTC2368_16] },
	{ .compatible = "adi,ltc2368-18", .data = &ltc2378_chip_info[ID_LTC2368_18] },
	{ .compatible = "adi,ltc2369-18", .data = &ltc2378_chip_info[ID_LTC2369_18] },
	{ .compatible = "adi,ltc2370-16", .data = &ltc2378_chip_info[ID_LTC2370_16] },
	{ .compatible = "adi,ltc2376-16", .data = &ltc2378_chip_info[ID_LTC2376_16] },
	{ .compatible = "adi,ltc2376-18", .data = &ltc2378_chip_info[ID_LTC2376_18] },
	{ .compatible = "adi,ltc2376-20", .data = &ltc2378_chip_info[ID_LTC2376_20] },
	{ .compatible = "adi,ltc2377-16", .data = &ltc2378_chip_info[ID_LTC2377_16] },
	{ .compatible = "adi,ltc2377-18", .data = &ltc2378_chip_info[ID_LTC2377_18] },
	{ .compatible = "adi,ltc2377-20", .data = &ltc2378_chip_info[ID_LTC2377_20] },
	{ .compatible = "adi,ltc2378-16", .data = &ltc2378_chip_info[ID_LTC2378_16] },
	{ .compatible = "adi,ltc2378-18", .data = &ltc2378_chip_info[ID_LTC2378_18] },
	{ .compatible = "adi,ltc2378-20", .data = &ltc2378_chip_info[ID_LTC2378_20] },
	{ .compatible = "adi,ltc2379-18", .data = &ltc2378_chip_info[ID_LTC2379_18] },
	{ .compatible = "adi,ltc2380-16", .data = &ltc2378_chip_info[ID_LTC2380_16] },
	{ },
};
MODULE_DEVICE_TABLE(of, ltc2378_of_match);

static const struct spi_device_id ltc2378_spi_id[] = {
	{ "ltc2338-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2338_18] },
	{ "ltc2364-16", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2364_16] },
	{ "ltc2364-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2364_18] },
	{ "ltc2367-16", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2367_16] },
	{ "ltc2367-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2367_18] },
	{ "ltc2368-16", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2368_16] },
	{ "ltc2368-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2368_18] },
	{ "ltc2369-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2369_18] },
	{ "ltc2370-16", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2370_16] },
	{ "ltc2376-16", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2376_16] },
	{ "ltc2376-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2376_18] },
	{ "ltc2376-20", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2376_20] },
	{ "ltc2377-16", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2377_16] },
	{ "ltc2377-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2377_18] },
	{ "ltc2377-20", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2377_20] },
	{ "ltc2378-16", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2378_16] },
	{ "ltc2378-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2378_18] },
	{ "ltc2378-20", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2378_20] },
	{ "ltc2379-18", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2379_18] },
	{ "ltc2380-16", (kernel_ulong_t)&ltc2378_chip_info[ID_LTC2380_16] },
	{ },
};
MODULE_DEVICE_TABLE(spi, ltc2378_spi_id);

static struct spi_driver ltc2378_driver = {
	.driver = {
		.name = "ltc2378",
		.of_match_table = ltc2378_of_match
	},
	.probe = ltc2378_probe,
	.id_table = ltc2378_spi_id,
};
module_spi_driver(ltc2378_driver);

MODULE_AUTHOR("Ioan-Daniel Pop <pop.ioan-daniel@analog.com>");
MODULE_AUTHOR("Marcelo Schmitt <marcelo.schmitt@analog.com>");
MODULE_DESCRIPTION("Analog Devices LTC2378 ADC series driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_LTC2378");
