// SPDX-License-Identifier: GPL-2.0+
/*
 * Analog Devices LTC2378 ADC series driver
 *
 * Copyright (C) 2026 Analog Devices Inc.
 * Author: Ioan-Daniel Pop <pop.ioan-daniel@analog.com>
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/types.h>

#include <linux/iio/iio.h>
#include <linux/iio/types.h>

#include "ltc2378.h"

static const struct ltc2378_chip_info ltc2338_18_chip_info = {
	.name = "ltc2338-18",
	.resolution = 18,
	.max_sample_rate_hz = HZ_PER_MHZ,
	.tconv_ns = 527,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2364_16_chip_info = {
	.name = "ltc2364-16",
	.resolution = 16,
	.max_sample_rate_hz = 250 * HZ_PER_KHZ,
	.tconv_ns = 3000,
	.twos_comp = false,
};

static const struct ltc2378_chip_info ltc2364_18_chip_info = {
	.name = "ltc2364-18",
	.resolution = 18,
	.max_sample_rate_hz = 250 * HZ_PER_KHZ,
	.tconv_ns = 3000,
	.twos_comp = false,
};

static const struct ltc2378_chip_info ltc2367_16_chip_info = {
	.name = "ltc2367-16",
	.resolution = 16,
	.max_sample_rate_hz = 500 * HZ_PER_KHZ,
	.tconv_ns = 1500,
	.twos_comp = false,
};

static const struct ltc2378_chip_info ltc2367_18_chip_info = {
	.name = "ltc2367-18",
	.resolution = 18,
	.max_sample_rate_hz = 500 * HZ_PER_KHZ,
	.tconv_ns = 1500,
	.twos_comp = false,
};

static const struct ltc2378_chip_info ltc2368_16_chip_info = {
	.name = "ltc2368-16",
	.resolution = 16,
	.max_sample_rate_hz = HZ_PER_MHZ,
	.tconv_ns = 527,
	.twos_comp = false,

};

static const struct ltc2378_chip_info ltc2368_18_chip_info = {
	.name = "ltc2368-18",
	.resolution = 18,
	.max_sample_rate_hz = HZ_PER_MHZ,
	.tconv_ns = 527,
	.twos_comp = false,
};

static const struct ltc2378_chip_info ltc2369_18_chip_info = {
	.name = "ltc2369-18",
	.resolution = 18,
	.max_sample_rate_hz = 1600 * HZ_PER_KHZ,
	.tconv_ns = 412,
	.twos_comp = false,
};

static const struct ltc2378_chip_info ltc2370_16_chip_info = {
	.name = "ltc2370-16",
	.resolution = 16,
	.max_sample_rate_hz = 2 * HZ_PER_MHZ,
	.tconv_ns = 322,
	.twos_comp = false,
};

static const struct ltc2378_chip_info ltc2376_16_chip_info = {
	.name = "ltc2376-16",
	.resolution = 16,
	.max_sample_rate_hz = 250 * HZ_PER_KHZ,
	.tconv_ns = 3000,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2376_18_chip_info = {
	.name = "ltc2376-18",
	.resolution = 18,
	.max_sample_rate_hz = 250 * HZ_PER_KHZ,
	.tconv_ns = 3000,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2376_20_chip_info = {
	.name = "ltc2376-20",
	.resolution = 20,
	.max_sample_rate_hz = 250 * HZ_PER_KHZ,
	.tconv_ns = 3000,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2377_16_chip_info = {
	.name = "ltc2377-16",
	.resolution = 16,
	.max_sample_rate_hz = 500 * HZ_PER_KHZ,
	.tconv_ns = 1500,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2377_18_chip_info = {
	.name = "ltc2377-18",
	.resolution = 18,
	.max_sample_rate_hz = 500 * HZ_PER_KHZ,
	.tconv_ns = 1500,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2377_20_chip_info = {
	.name = "ltc2377-20",
	.resolution = 20,
	.max_sample_rate_hz = 500 * HZ_PER_KHZ,
	.tconv_ns = 1500,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2378_16_chip_info = {
	.name = "ltc2378-16",
	.resolution = 16,
	.max_sample_rate_hz = HZ_PER_MHZ,
	.tconv_ns = 527,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2378_18_chip_info = {
	.name = "ltc2378-18",
	.resolution = 18,
	.max_sample_rate_hz = HZ_PER_MHZ,
	.tconv_ns = 527,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2378_20_chip_info = {
	.name = "ltc2378-20",
	.resolution = 20,
	.max_sample_rate_hz = HZ_PER_MHZ,
	.tconv_ns = 675,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2379_18_chip_info = {
	.name = "ltc2379-18",
	.resolution = 18,
	.max_sample_rate_hz = 1600 * HZ_PER_KHZ,
	.tconv_ns = 412,
	.twos_comp = true,
};

static const struct ltc2378_chip_info ltc2380_16_chip_info = {
	.name = "ltc2380-16",
	.resolution = 16,
	.max_sample_rate_hz = 2 * HZ_PER_MHZ,
	.tconv_ns = 322,
	.twos_comp = true,
};

static int ltc2378_convert_and_acquire(struct ltc2378_state *st)
{
	int ret;

	/* Cause a rising edge of CNV to initiate a new ADC conversion */
	gpiod_set_value_cansleep(st->cnv_gpio, 1);
	fsleep(4);
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

	if (scan_type->realbits > 16)
		sample = st->scan.data.sample_buf32;
	else
		sample = st->scan.data.sample_buf16;

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
	case IIO_CHAN_INFO_RAW: {
		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		ret = ltc2378_channel_single_read(chan, st, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;
	}
	case IIO_CHAN_INFO_SCALE:
		*val = st->ref_uV / MILLI;
		/*
		 * For all LTC2378-like devices, the amount of bits that express
		 * voltage magnitude depend on the output code format:
		 * - straight binary: All precision/resolution bits are used.
		 * - 2's complement: One of the precision bits is used for sign.
		 */
		if (st->info->twos_comp)
			*val2 = st->info->resolution - 1;
		else
			*val2 = st->info->resolution;

		return IIO_VAL_FRACTIONAL_LOG2;

	case IIO_CHAN_INFO_SAMP_FREQ: {
		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		ret = ltc2378_get_sampling_frequency(st, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;
	}
	default:
		return -EINVAL;
	}
}

static const struct iio_info ltc2378_iio_info = {
	.read_raw = &ltc2378_read_raw,
	.write_raw = LTC2378_WRITE_RAW,
	.read_avail = LTC2378_READ_AVAIL,
};

static int ltc2378_probe(struct spi_device *spi)
{
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

	ret = devm_regulator_get_enable_read_voltage(dev, "ref");
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to read ref regulator\n");

	st->ref_uV = ret;
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

	st->chans[0].type = IIO_VOLTAGE;
	st->chans[0].info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
					  BIT(IIO_CHAN_INFO_SCALE);

	struct iio_scan_type ltc2378_scan;

	ret = ltc2378_offload_buffer_setup(indio_dev, spi);
	if (ret == -ENODEV) {
		/* SPI offloading is unavailable. Fall back to triggered buffer. */
		dev_dbg(dev, "triggered data capture not supported\n");
		ltc2378_scan.format = st->info->twos_comp ? IIO_SCAN_FORMAT_SIGNED_INT :
							    IIO_SCAN_FORMAT_UNSIGNED_INT;
		ltc2378_scan.realbits = st->info->resolution;
		ltc2378_scan.storagebits = st->info->resolution > 16 ? 32 : 16;
	} else if (ret) {
		return dev_err_probe(dev, ret, "error on SPI offload setup\n");
	} else {
		/*
		 * Currently, the available offload hardware + DMA configuration
		 * only supports pushing 32-bit data elements to IIO buffers in
		 * CPU endianness.
		 */
		st->chans[0].info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ);
		st->chans[0].info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ);

		ltc2378_scan.format = st->info->twos_comp ? IIO_SCAN_FORMAT_SIGNED_INT :
							    IIO_SCAN_FORMAT_UNSIGNED_INT;
		ltc2378_scan.realbits = st->info->resolution;
		ltc2378_scan.storagebits = 32;
	}

	st->chans[0].scan_type = ltc2378_scan;

	st->xfer.rx_buf = &st->scan.data;
	st->xfer.len = BITS_TO_BYTES(st->chans[0].scan_type.storagebits);
	st->xfer.bits_per_word = st->info->resolution > 16 ? 32 : 16;

	indio_dev->channels = st->chans;
	indio_dev->num_channels = num_iio_chans;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct of_device_id ltc2378_of_match[] = {
	{ .compatible = "adi,ltc2338-18", .data = &ltc2338_18_chip_info },
	{ .compatible = "adi,ltc2364-16", .data = &ltc2364_16_chip_info },
	{ .compatible = "adi,ltc2364-18", .data = &ltc2364_18_chip_info },
	{ .compatible = "adi,ltc2367-16", .data = &ltc2367_16_chip_info },
	{ .compatible = "adi,ltc2367-18", .data = &ltc2367_18_chip_info },
	{ .compatible = "adi,ltc2368-16", .data = &ltc2368_16_chip_info },
	{ .compatible = "adi,ltc2368-18", .data = &ltc2368_18_chip_info },
	{ .compatible = "adi,ltc2369-18", .data = &ltc2369_18_chip_info },
	{ .compatible = "adi,ltc2370-16", .data = &ltc2370_16_chip_info },
	{ .compatible = "adi,ltc2376-16", .data = &ltc2376_16_chip_info },
	{ .compatible = "adi,ltc2376-18", .data = &ltc2376_18_chip_info },
	{ .compatible = "adi,ltc2376-20", .data = &ltc2376_20_chip_info },
	{ .compatible = "adi,ltc2377-16", .data = &ltc2377_16_chip_info },
	{ .compatible = "adi,ltc2377-18", .data = &ltc2377_18_chip_info },
	{ .compatible = "adi,ltc2377-20", .data = &ltc2377_20_chip_info },
	{ .compatible = "adi,ltc2378-16", .data = &ltc2378_16_chip_info },
	{ .compatible = "adi,ltc2378-18", .data = &ltc2378_18_chip_info },
	{ .compatible = "adi,ltc2378-20", .data = &ltc2378_20_chip_info },
	{ .compatible = "adi,ltc2379-18", .data = &ltc2379_18_chip_info },
	{ .compatible = "adi,ltc2380-16", .data = &ltc2380_16_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(of, ltc2378_of_match);

static const struct spi_device_id ltc2378_spi_id[] = {
	{ .name = "ltc2338-18", .driver_data = (kernel_ulong_t)&ltc2338_18_chip_info },
	{ .name = "ltc2364-16", .driver_data = (kernel_ulong_t)&ltc2364_16_chip_info },
	{ .name = "ltc2364-18", .driver_data = (kernel_ulong_t)&ltc2364_18_chip_info },
	{ .name = "ltc2367-16", .driver_data = (kernel_ulong_t)&ltc2367_16_chip_info },
	{ .name = "ltc2367-18", .driver_data = (kernel_ulong_t)&ltc2367_18_chip_info },
	{ .name = "ltc2368-16", .driver_data = (kernel_ulong_t)&ltc2368_16_chip_info },
	{ .name = "ltc2368-18", .driver_data = (kernel_ulong_t)&ltc2368_18_chip_info },
	{ .name = "ltc2369-18", .driver_data = (kernel_ulong_t)&ltc2369_18_chip_info },
	{ .name = "ltc2370-16", .driver_data = (kernel_ulong_t)&ltc2370_16_chip_info },
	{ .name = "ltc2376-16", .driver_data = (kernel_ulong_t)&ltc2376_16_chip_info },
	{ .name = "ltc2376-18", .driver_data = (kernel_ulong_t)&ltc2376_18_chip_info },
	{ .name = "ltc2376-20", .driver_data = (kernel_ulong_t)&ltc2376_20_chip_info },
	{ .name = "ltc2377-16", .driver_data = (kernel_ulong_t)&ltc2377_16_chip_info },
	{ .name = "ltc2377-18", .driver_data = (kernel_ulong_t)&ltc2377_18_chip_info },
	{ .name = "ltc2377-20", .driver_data = (kernel_ulong_t)&ltc2377_20_chip_info },
	{ .name = "ltc2378-16", .driver_data = (kernel_ulong_t)&ltc2378_16_chip_info },
	{ .name = "ltc2378-18", .driver_data = (kernel_ulong_t)&ltc2378_18_chip_info },
	{ .name = "ltc2378-20", .driver_data = (kernel_ulong_t)&ltc2378_20_chip_info },
	{ .name = "ltc2379-18", .driver_data = (kernel_ulong_t)&ltc2379_18_chip_info },
	{ .name = "ltc2380-16", .driver_data = (kernel_ulong_t)&ltc2380_16_chip_info },
	{ }
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
