// SPDX-License-Identifier: GPL-2.0-only
/*
 * Analog Devices AD7944/85/86 PulSAR ADC family driver.
 *
 * Copyright 2024 Analog Devices, Inc.
 * Copyright 2024 Baylibre, SAS
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define AD7944_INTERNAL_REF_MV		4096

struct ad7944_timing_spec {
	/* Normal mode minimum CNV pulse width in nanoseconds. */
	unsigned int cnv_ns;
	/* TURBO mode minimum CNV pulse width in nanoseconds. */
	unsigned int turbo_cnv_ns;
};

struct ad7944_adc {
	struct spi_device *spi;
	/* Chip-specific timing specifications. */
	const struct ad7944_timing_spec *t;
	/* GPIO connected to CNV pin. */
	struct gpio_desc *cnv;
	/* Optional GPIO to enable turbo mode. */
	struct gpio_desc *turbo;
	/* Indicates TURBO is hard-wired to be always enabled. */
	bool always_turbo;
	/* Reference voltage (millivolts). */
	unsigned int ref_mv;

	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	struct {
		union {
			u16 u16;
			u32 u32;
		} raw;
		u64 timestamp __aligned(8);
	 } sample __aligned(IIO_DMA_MINALIGN);
};

static const struct ad7944_timing_spec ad7944_timing_spec = {
	.cnv_ns = 420,
	.turbo_cnv_ns = 320,
};

static const struct ad7944_timing_spec ad7986_timing_spec = {
	.cnv_ns = 500,
	.turbo_cnv_ns = 400,
};

struct ad7944_chip_info {
	const char *name;
	const struct ad7944_timing_spec *t;
	const struct iio_chan_spec channels[2];
};

#define AD7944_DEFINE_CHIP_INFO(_name, _t, _bits, _sign)		\
static const struct ad7944_chip_info _name##_chip_info = {		\
	.name = #_name,							\
	.t = &_t##_timing_spec,						\
	.channels = {							\
		{							\
			.type = IIO_VOLTAGE,				\
			.indexed = 1,					\
			.differential = 1,				\
			.channel = 0,					\
			.channel2 = 1,					\
			.scan_index = 0,				\
			.scan_type.sign = _sign,			\
			.scan_type.realbits = _bits,			\
			.scan_type.storagebits = _bits > 16 ? 32 : 16,	\
			.scan_type.endianness = IIO_CPU,		\
			.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)	\
					| BIT(IIO_CHAN_INFO_SCALE),	\
		},							\
		IIO_CHAN_SOFT_TIMESTAMP(1),				\
	},								\
}

AD7944_DEFINE_CHIP_INFO(ad7944, ad7944, 14, 'u');
AD7944_DEFINE_CHIP_INFO(ad7985, ad7944, 16, 'u');
AD7944_DEFINE_CHIP_INFO(ad7986, ad7986, 18, 's');

/**
 * ad7944_4_wire_mode_conversion - Perform a 4-wire mode conversion and acquisition
 * @adc: The ADC device structure
 * @chan: The channel specification
 * Return: 0 on success, a negative error code on failure
 *
 * Upon successful return adc->sample.raw will contain the conversion result.
 */
static int ad7944_4_wire_mode_conversion(struct ad7944_adc *adc,
					 const struct iio_chan_spec *chan)
{
	unsigned int t_cnv_ns = adc->always_turbo ? adc->t->turbo_cnv_ns
						  : adc->t->cnv_ns;
	struct spi_transfer xfers[] = {
		{
			/*
			 * NB: can get better performance from some SPI
			 * controllers if we use the same bits_per_word
			 * in every transfer.
			 */
			.bits_per_word = chan->scan_type.realbits,
			/*
			 * CS has to be high for full conversion time to avoid
			 * triggering the busy indication.
			 */
			.cs_off = 1,
			.delay = {
				.value = t_cnv_ns,
				.unit = SPI_DELAY_UNIT_NSECS,
			},

		},
		{
			.rx_buf = &adc->sample.raw,
			.len = BITS_TO_BYTES(chan->scan_type.storagebits),
			.bits_per_word = chan->scan_type.realbits,
		},
	};
	int ret;

	/*
	 * In 4-wire mode, the CNV line is held high for the entire conversion
	 * and acquisition process.
	 */
	gpiod_set_value_cansleep(adc->cnv, 1);

	ret = spi_sync_transfer(adc->spi, xfers, ARRAY_SIZE(xfers));
	if (ret)
		return ret;

	gpiod_set_value_cansleep(adc->cnv, 0);

	return 0;
}

static int ad7944_single_conversion(struct ad7944_adc *adc,
				    const struct iio_chan_spec *chan,
				    int *val)
{
	int ret;

	ret = ad7944_4_wire_mode_conversion(adc, chan);
	if (ret)
		return ret;

	if (chan->scan_type.storagebits > 16)
		*val = adc->sample.raw.u32;
	else
		*val = adc->sample.raw.u16;

	if (chan->scan_type.sign == 's')
		*val = sign_extend32(*val, chan->scan_type.realbits - 1);

	return IIO_VAL_INT;
}

static int ad7944_read_raw(struct iio_dev *indio_dev,
			   const struct iio_chan_spec *chan,
			   int *val, int *val2, long info)
{
	struct ad7944_adc *adc = iio_priv(indio_dev);
	int ret;

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;

		ret = ad7944_single_conversion(adc, chan, val);
		iio_device_release_direct_mode(indio_dev);
		return ret;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_VOLTAGE:
			*val = adc->ref_mv;
			*val2 = chan->scan_type.realbits;

			return IIO_VAL_FRACTIONAL_LOG2;
		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static const struct iio_info ad7944_iio_info = {
	.read_raw = &ad7944_read_raw,
};

static irqreturn_t ad7944_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad7944_adc *adc = iio_priv(indio_dev);
	int ret;

	ret = ad7944_4_wire_mode_conversion(adc, &indio_dev->channels[0]);
	if (ret)
		goto out;

	iio_push_to_buffers_with_timestamp(indio_dev, &adc->sample.raw,
					   indio_dev->scan_timestamp);

out:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static const char * const ad7944_power_supplies[] = {
	"avdd",	"dvdd",	"bvdd", "vio"
};

static void ad7944_ref_disable(void *ref)
{
	regulator_disable(ref);
}

static int ad7944_probe(struct spi_device *spi)
{
	const struct ad7944_chip_info *chip_info;
	struct iio_dev *indio_dev;
	struct ad7944_adc *adc;
	struct regulator *ref;
	const char *str_val;
	int ret;

	/* adi,spi-mode property defaults to "4-wire" if not present */
	if (device_property_read_string(&spi->dev, "adi,spi-mode", &str_val) < 0)
		str_val = "4-wire";

	if (strcmp(str_val, "4-wire"))
		return dev_err_probe(&spi->dev, -EINVAL,
				     "only \"4-wire\" mode is currently supported\n");

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->spi = spi;

	chip_info = spi_get_device_match_data(spi);
	if (!chip_info)
		return dev_err_probe(&spi->dev, -EINVAL, "no chip info\n");

	adc->t = chip_info->t;

	/*
	 * Some chips use unusual word sizes, so check now instead of waiting
	 * for the first xfer.
	 */
	if (!spi_is_bpw_supported(spi, chip_info->channels[0].scan_type.realbits))
		return dev_err_probe(&spi->dev, -EINVAL,
				"SPI host does not support %d bits per word\n",
				chip_info->channels[0].scan_type.realbits);

	ret = devm_regulator_bulk_get_enable(&spi->dev,
					     ARRAY_SIZE(ad7944_power_supplies),
					     ad7944_power_supplies);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "failed to get and enable supplies\n");

	/* adi,reference property defaults to "internal" if not present */
	if (device_property_read_string(&spi->dev, "adi,reference", &str_val) < 0)
		str_val = "internal";

	/* sort out what is being used for the reference voltage */
	if (strcmp(str_val, "internal") == 0) {
		/* internal reference is used */
		adc->ref_mv = AD7944_INTERNAL_REF_MV;
	} else if (strcmp(str_val, "internal-buffer") == 0) {
		/* external 1.2V REFIN and internal buffer is used */
		ret = devm_regulator_get_enable_optional(&spi->dev, "refin");
		if (ret)
			return dev_err_probe(&spi->dev, ret,
					"failed to get and enable REFIN supply\n");

		adc->ref_mv = AD7944_INTERNAL_REF_MV;
	} else if (strcmp(str_val, "external") == 0) {
		/* external reference is used */
		ref = devm_regulator_get_optional(&spi->dev, "ref");
		if (IS_ERR(ref))
			return dev_err_probe(&spi->dev, PTR_ERR(ref),
					     "failed to get REF supply\n");

		ret = regulator_enable(ref);
		if (ret)
			return dev_err_probe(&spi->dev, ret,
					     "failed to enable REF supply\n");

		ret = devm_add_action_or_reset(&spi->dev,
					       ad7944_ref_disable, ref);
		if (ret)
			return ret;

		ret = regulator_get_voltage(ref);
		if (ret < 0)
			return dev_err_probe(&spi->dev, ret,
					     "failed to get REF voltage\n");

		adc->ref_mv = ret / 1000;
	} else {
		return dev_err_probe(&spi->dev, -EINVAL,
				     "invalid adi,reference property: %s\n",
				     str_val);
	}

	adc->cnv = devm_gpiod_get(&spi->dev, "cnv", GPIOD_OUT_LOW);
	if (IS_ERR(adc->cnv))
		return dev_err_probe(&spi->dev, PTR_ERR(adc->cnv),
				     "failed to get CNV GPIO\n");

	adc->turbo = devm_gpiod_get_optional(&spi->dev, "turbo", GPIOD_OUT_LOW);
	if (IS_ERR(adc->turbo))
		return dev_err_probe(&spi->dev, PTR_ERR(adc->turbo),
				     "failed to get TURBO GPIO\n");

	if (device_property_present(&spi->dev, "adi,always-turbo"))
		adc->always_turbo = true;

	if (adc->turbo && adc->always_turbo)
		return dev_err_probe(&spi->dev, -EINVAL,
			"cannot have both turbo-gpios and adi,always-turbo\n");

	indio_dev->name = chip_info->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ad7944_iio_info;
	indio_dev->channels = chip_info->channels;
	indio_dev->num_channels = ARRAY_SIZE(chip_info->channels);

	ret = devm_iio_triggered_buffer_setup(&spi->dev, indio_dev,
					      iio_pollfunc_store_time,
					      ad7944_trigger_handler, NULL);
	if (ret)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct of_device_id ad7944_of_match[] = {
	{ .compatible = "adi,ad7944", .data = &ad7944_chip_info },
	{ .compatible = "adi,ad7985", .data = &ad7985_chip_info },
	{ .compatible = "adi,ad7986", .data = &ad7986_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(of, ad7944_of_match);

static const struct spi_device_id ad7944_spi_id[] = {
	{ "ad7944", (kernel_ulong_t)&ad7944_chip_info },
	{ "ad7985", (kernel_ulong_t)&ad7985_chip_info },
	{ "ad7986", (kernel_ulong_t)&ad7986_chip_info },
	{ }

};
MODULE_DEVICE_TABLE(spi, ad7944_spi_id);

static struct spi_driver ad7944_driver = {
	.driver = {
		.name = "ad7944",
		.of_match_table = ad7944_of_match,
	},
	.probe = ad7944_probe,
	.id_table = ad7944_spi_id,
};
module_spi_driver(ad7944_driver);

MODULE_AUTHOR("David Lechner <dlechner@baylibre.com>");
MODULE_DESCRIPTION("Analog Devices AD7944 PulSAR ADC family driver");
MODULE_LICENSE("GPL");
