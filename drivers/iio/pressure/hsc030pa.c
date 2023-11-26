// SPDX-License-Identifier: GPL-2.0-only
/*
 * Honeywell TruStability HSC Series pressure/temperature sensor
 *
 * Copyright (c) 2023 Petre Rodan <petre.rodan@subdimension.ro>
 *
 * Datasheet: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/pressure-sensors/board-mount-pressure-sensors/trustability-hsc-series/documents/sps-siot-trustability-hsc-series-high-accuracy-board-mount-pressure-sensors-50099148-a-en-ciid-151133.pdf
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/init.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include <asm/unaligned.h>

#include "hsc030pa.h"

#define     HSC_PRESSURE_TRIPLET_LEN  6
#define              HSC_STATUS_MASK  GENMASK(7, 6)
#define         HSC_TEMPERATURE_MASK  GENMASK(15, 5)
#define            HSC_PRESSURE_MASK  GENMASK(29, 16)

struct hsc_func_spec {
	u32 output_min;
	u32 output_max;
};

/**
 * function A: 10% - 90% of 2^14
 * function B:  5% - 95% of 2^14
 * function C:  5% - 85% of 2^14
 * function F:  4% - 94% of 2^14
 */
static const struct hsc_func_spec hsc_func_spec[] = {
	[HSC_FUNCTION_A] = {.output_min = 1638, .output_max = 14746},
	[HSC_FUNCTION_B] = {.output_min =  819, .output_max = 15565},
	[HSC_FUNCTION_C] = {.output_min =  819, .output_max = 13926},
	[HSC_FUNCTION_F] = {.output_min =  655, .output_max = 15401},
};

/**
 * struct hsc_range_config - pressure ranges based on the nomenclature
 * @name: 5-char string triplet that defines the range, measurement unit
 *        and type
 * @pmin: lowest pressure that can be measured
 * @pmax: highest pressure that can be measured
 */
struct hsc_range_config {
	char name[HSC_PRESSURE_TRIPLET_LEN];
	s32 pmin;
	u32 pmax;
};

/* all min max limits have been converted to pascals */
static const struct hsc_range_config hsc_range_config[] = {
	{.name = "001BA", .pmin =       0, .pmax =  100000 },
	{.name = "1.6BA", .pmin =       0, .pmax =  160000 },
	{.name = "2.5BA", .pmin =       0, .pmax =  250000 },
	{.name = "004BA", .pmin =       0, .pmax =  400000 },
	{.name = "006BA", .pmin =       0, .pmax =  600000 },
	{.name = "010BA", .pmin =       0, .pmax = 1000000 },
	{.name = "1.6MD", .pmin =    -160, .pmax =     160 },
	{.name = "2.5MD", .pmin =    -250, .pmax =     250 },
	{.name = "004MD", .pmin =    -400, .pmax =     400 },
	{.name = "006MD", .pmin =    -600, .pmax =     600 },
	{.name = "010MD", .pmin =   -1000, .pmax =    1000 },
	{.name = "016MD", .pmin =   -1600, .pmax =    1600 },
	{.name = "025MD", .pmin =   -2500, .pmax =    2500 },
	{.name = "040MD", .pmin =   -4000, .pmax =    4000 },
	{.name = "060MD", .pmin =   -6000, .pmax =    6000 },
	{.name = "100MD", .pmin =  -10000, .pmax =   10000 },
	{.name = "160MD", .pmin =  -16000, .pmax =   16000 },
	{.name = "250MD", .pmin =  -25000, .pmax =   25000 },
	{.name = "400MD", .pmin =  -40000, .pmax =   40000 },
	{.name = "600MD", .pmin =  -60000, .pmax =   60000 },
	{.name = "001BD", .pmin = -100000, .pmax =  100000 },
	{.name = "1.6BD", .pmin = -160000, .pmax =  160000 },
	{.name = "2.5BD", .pmin = -250000, .pmax =  250000 },
	{.name = "004BD", .pmin = -400000, .pmax =  400000 },
	{.name = "2.5MG", .pmin =       0, .pmax =     250 },
	{.name = "004MG", .pmin =       0, .pmax =     400 },
	{.name = "006MG", .pmin =       0, .pmax =     600 },
	{.name = "010MG", .pmin =       0, .pmax =    1000 },
	{.name = "016MG", .pmin =       0, .pmax =    1600 },
	{.name = "025MG", .pmin =       0, .pmax =    2500 },
	{.name = "040MG", .pmin =       0, .pmax =    4000 },
	{.name = "060MG", .pmin =       0, .pmax =    6000 },
	{.name = "100MG", .pmin =       0, .pmax =   10000 },
	{.name = "160MG", .pmin =       0, .pmax =   16000 },
	{.name = "250MG", .pmin =       0, .pmax =   25000 },
	{.name = "400MG", .pmin =       0, .pmax =   40000 },
	{.name = "600MG", .pmin =       0, .pmax =   60000 },
	{.name = "001BG", .pmin =       0, .pmax =  100000 },
	{.name = "1.6BG", .pmin =       0, .pmax =  160000 },
	{.name = "2.5BG", .pmin =       0, .pmax =  250000 },
	{.name = "004BG", .pmin =       0, .pmax =  400000 },
	{.name = "006BG", .pmin =       0, .pmax =  600000 },
	{.name = "010BG", .pmin =       0, .pmax = 1000000 },
	{.name = "100KA", .pmin =       0, .pmax =  100000 },
	{.name = "160KA", .pmin =       0, .pmax =  160000 },
	{.name = "250KA", .pmin =       0, .pmax =  250000 },
	{.name = "400KA", .pmin =       0, .pmax =  400000 },
	{.name = "600KA", .pmin =       0, .pmax =  600000 },
	{.name = "001GA", .pmin =       0, .pmax = 1000000 },
	{.name = "160LD", .pmin =    -160, .pmax =     160 },
	{.name = "250LD", .pmin =    -250, .pmax =     250 },
	{.name = "400LD", .pmin =    -400, .pmax =     400 },
	{.name = "600LD", .pmin =    -600, .pmax =     600 },
	{.name = "001KD", .pmin =   -1000, .pmax =    1000 },
	{.name = "1.6KD", .pmin =   -1600, .pmax =    1600 },
	{.name = "2.5KD", .pmin =   -2500, .pmax =    2500 },
	{.name = "004KD", .pmin =   -4000, .pmax =    4000 },
	{.name = "006KD", .pmin =   -6000, .pmax =    6000 },
	{.name = "010KD", .pmin =  -10000, .pmax =   10000 },
	{.name = "016KD", .pmin =  -16000, .pmax =   16000 },
	{.name = "025KD", .pmin =  -25000, .pmax =   25000 },
	{.name = "040KD", .pmin =  -40000, .pmax =   40000 },
	{.name = "060KD", .pmin =  -60000, .pmax =   60000 },
	{.name = "100KD", .pmin = -100000, .pmax =  100000 },
	{.name = "160KD", .pmin = -160000, .pmax =  160000 },
	{.name = "250KD", .pmin = -250000, .pmax =  250000 },
	{.name = "400KD", .pmin = -400000, .pmax =  400000 },
	{.name = "250LG", .pmin =       0, .pmax =     250 },
	{.name = "400LG", .pmin =       0, .pmax =     400 },
	{.name = "600LG", .pmin =       0, .pmax =     600 },
	{.name = "001KG", .pmin =       0, .pmax =    1000 },
	{.name = "1.6KG", .pmin =       0, .pmax =    1600 },
	{.name = "2.5KG", .pmin =       0, .pmax =    2500 },
	{.name = "004KG", .pmin =       0, .pmax =    4000 },
	{.name = "006KG", .pmin =       0, .pmax =    6000 },
	{.name = "010KG", .pmin =       0, .pmax =   10000 },
	{.name = "016KG", .pmin =       0, .pmax =   16000 },
	{.name = "025KG", .pmin =       0, .pmax =   25000 },
	{.name = "040KG", .pmin =       0, .pmax =   40000 },
	{.name = "060KG", .pmin =       0, .pmax =   60000 },
	{.name = "100KG", .pmin =       0, .pmax =  100000 },
	{.name = "160KG", .pmin =       0, .pmax =  160000 },
	{.name = "250KG", .pmin =       0, .pmax =  250000 },
	{.name = "400KG", .pmin =       0, .pmax =  400000 },
	{.name = "600KG", .pmin =       0, .pmax =  600000 },
	{.name = "001GG", .pmin =       0, .pmax = 1000000 },
	{.name = "015PA", .pmin =       0, .pmax =  103421 },
	{.name = "030PA", .pmin =       0, .pmax =  206843 },
	{.name = "060PA", .pmin =       0, .pmax =  413685 },
	{.name = "100PA", .pmin =       0, .pmax =  689476 },
	{.name = "150PA", .pmin =       0, .pmax = 1034214 },
	{.name = "0.5ND", .pmin =    -125, .pmax =     125 },
	{.name = "001ND", .pmin =    -249, .pmax =     249 },
	{.name = "002ND", .pmin =    -498, .pmax =     498 },
	{.name = "004ND", .pmin =    -996, .pmax =     996 },
	{.name = "005ND", .pmin =   -1245, .pmax =    1245 },
	{.name = "010ND", .pmin =   -2491, .pmax =    2491 },
	{.name = "020ND", .pmin =   -4982, .pmax =    4982 },
	{.name = "030ND", .pmin =   -7473, .pmax =    7473 },
	{.name = "001PD", .pmin =   -6895, .pmax =    6895 },
	{.name = "005PD", .pmin =  -34474, .pmax =   34474 },
	{.name = "015PD", .pmin = -103421, .pmax =  103421 },
	{.name = "030PD", .pmin = -206843, .pmax =  206843 },
	{.name = "060PD", .pmin = -413685, .pmax =  413685 },
	{.name = "001NG", .pmin =       0, .pmax =     249 },
	{.name = "002NG", .pmin =       0, .pmax =     498 },
	{.name = "004NG", .pmin =       0, .pmax =     996 },
	{.name = "005NG", .pmin =       0, .pmax =    1245 },
	{.name = "010NG", .pmin =       0, .pmax =    2491 },
	{.name = "020NG", .pmin =       0, .pmax =    4982 },
	{.name = "030NG", .pmin =       0, .pmax =    7473 },
	{.name = "001PG", .pmin =       0, .pmax =    6895 },
	{.name = "005PG", .pmin =       0, .pmax =   34474 },
	{.name = "015PG", .pmin =       0, .pmax =  103421 },
	{.name = "030PG", .pmin =       0, .pmax =  206843 },
	{.name = "060PG", .pmin =       0, .pmax =  413685 },
	{.name = "100PG", .pmin =       0, .pmax =  689476 },
	{.name = "150PG", .pmin =       0, .pmax = 1034214 }
};

/*
 * hsc_measurement_is_valid() - validate last conversion via status bits
 * @data: structure containing instantiated sensor data
 * Return: true only if both status bits are zero
 *
 * the two MSB from the first transfered byte contain a status code
 *   00 - normal operation, valid data
 *   01 - device in factory programming mode
 *   10 - stale data
 *   11 - diagnostic condition
 */
static bool hsc_measurement_is_valid(struct hsc_data *data)
{
	return !(data->buffer[0] & HSC_STATUS_MASK);
}

static int hsc_get_measurement(struct hsc_data *data)
{
	const struct hsc_chip_data *chip = data->chip;
	int ret;

	guard(mutex)(&data->lock);
	ret = data->xfer(data);
	if (ret < 0)
		return ret;

	data->is_valid = chip->valid(data);
	if (!data->is_valid)
		return -EAGAIN;

	return 0;
}

static int hsc_read_raw(struct iio_dev *indio_dev,
			struct iio_chan_spec const *channel, int *val,
			int *val2, long mask)
{
	struct hsc_data *data = iio_priv(indio_dev);
	int ret;
	u32 xfer;
	int raw;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = hsc_get_measurement(data);
		if (ret)
			return ret;

		xfer = get_unaligned_be32(data->buffer);
		switch (channel->type) {
		case IIO_PRESSURE:
			raw = FIELD_GET(HSC_PRESSURE_MASK, xfer);
			*val = raw;
			return IIO_VAL_INT;
		case IIO_TEMP:
			raw = FIELD_GET(HSC_TEMPERATURE_MASK, xfer);
			*val = raw;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}

/**
 * IIO ABI expects
 * value = (conv + offset) * scale
 *
 * datasheet provides the following formula for determining the temperature
 * temp[C] = conv * a + b
 *   where a = 200/2047; b = -50
 *
 *  temp[C] = (conv + (b/a)) * a * (1000)
 *  =>
 *  scale = a * 1000 = .097703957 * 1000 = 97.703957
 *  offset = b/a = -50 / .097703957 = -50000000 / 97704
 *
 *  based on the datasheet
 *  pressure = (conv - Omin) * Q + Pmin =
 *          ((conv - Omin) + Pmin/Q) * Q
 *  =>
 *  scale = Q = (Pmax - Pmin) / (Omax - Omin)
 *  offset = Pmin/Q - Omin = Pmin * (Omax - Omin) / (Pmax - Pmin) - Omin
 */

	case IIO_CHAN_INFO_SCALE:
		switch (channel->type) {
		case IIO_TEMP:
			*val = 97;
			*val2 = 703957;
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_PRESSURE:
			*val = data->p_scale;
			*val2 = data->p_scale_dec;
			return IIO_VAL_INT_PLUS_NANO;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		switch (channel->type) {
		case IIO_TEMP:
			*val = -50000000;
			*val2 = 97704;
			return IIO_VAL_FRACTIONAL;
		case IIO_PRESSURE:
			*val = data->p_offset;
			*val2 = data->p_offset_dec;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}
	}

	return -EINVAL;
}

static const struct iio_chan_spec hsc_channels[] = {
	{.type = IIO_PRESSURE, .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
	 BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET) },
	{.type = IIO_TEMP, .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
	 BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET) },
};

static const struct iio_info hsc_info = {
	.read_raw = hsc_read_raw,
};

static const struct hsc_chip_data hsc_chip = {
	.valid = hsc_measurement_is_valid,
	.channels = hsc_channels,
	.num_channels = ARRAY_SIZE(hsc_channels),
};

int hsc_probe(struct iio_dev *indio_dev, struct device *dev,
	      const char *name, int type)
{
	struct hsc_data *hsc;
	const char *triplet;
	u64 tmp;
	int index;
	int found = 0;
	int ret;

	hsc = iio_priv(indio_dev);
	hsc->chip = &hsc_chip;

	ret = device_property_read_u32(dev,
				       "honeywell,transfer-function",
				       &hsc->function);
	if (ret)
		return dev_err_probe(dev, ret,
			    "honeywell,transfer-function could not be read\n");
	if (hsc->function > HSC_FUNCTION_F)
		return dev_err_probe(dev, -EINVAL,
				     "honeywell,transfer-function %d invalid\n",
				     hsc->function);

	ret = device_property_read_string(dev,
		"honeywell,pressure-triplet", &triplet);
	if (ret)
		return dev_err_probe(dev, ret,
			"honeywell,pressure-triplet could not be read\n");

	if (strncmp(triplet, "NA", 2) == 0) {
		// "not available" in the nomenclature
		// we got a custom-range chip so extract pmin, pmax from dt
		ret = device_property_read_u32(dev,
					       "honeywell,pmin-pascal",
					       &hsc->pmin);
		if (ret)
			return dev_err_probe(dev, ret,
				"honeywell,pmin-pascal could not be read\n");
		ret = device_property_read_u32(dev,
					       "honeywell,pmax-pascal",
					       &hsc->pmax);
		if (ret)
			return dev_err_probe(dev, ret,
				"honeywell,pmax-pascal could not be read\n");
	} else {
		// chip should be defined in the nomenclature
		for (index = 0; index < ARRAY_SIZE(hsc_range_config); index++) {
			if (strncmp(hsc_range_config[index].name,
					triplet,
					HSC_PRESSURE_TRIPLET_LEN - 1) == 0) {
				hsc->pmin = hsc_range_config[index].pmin;
				hsc->pmax = hsc_range_config[index].pmax;
				found = 1;
				break;
			}
		}
		if (hsc->pmin == hsc->pmax || !found)
			return dev_err_probe(dev, -EINVAL,
				"honeywell,pressure-triplet is invalid\n");
	}

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "can't get vdd supply\n");

	hsc->outmin = hsc_func_spec[hsc->function].output_min;
	hsc->outmax = hsc_func_spec[hsc->function].output_max;

	/* multiply with MICRO and then divide by NANO since the output needs
	   to be in Pa * 1000 as per IIO ABI requirement */
	tmp = div_s64(((s64)(hsc->pmax - hsc->pmin)) * MICRO,
		      (hsc->outmax - hsc->outmin));
	hsc->p_scale = div_s64_rem(tmp, NANO, &hsc->p_scale_dec);
	tmp = div_s64(((s64)hsc->pmin * (s64)(hsc->outmax - hsc->outmin)) *
		      MICRO, hsc->pmax - hsc->pmin);
	tmp -= (s64)hsc->outmin * MICRO;
	hsc->p_offset = div_s64_rem(tmp, MICRO, &hsc->p_offset_dec);

	mutex_init(&hsc->lock);
	indio_dev->name = name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &hsc_info;
	indio_dev->channels = hsc->chip->channels;
	indio_dev->num_channels = hsc->chip->num_channels;

	return devm_iio_device_register(dev, indio_dev);
}
EXPORT_SYMBOL_NS(hsc_probe, IIO_HONEYWELL_HSC030PA);

MODULE_AUTHOR("Petre Rodan <petre.rodan@subdimension.ro>");
MODULE_DESCRIPTION("Honeywell HSC and SSC pressure sensor core driver");
MODULE_LICENSE("GPL");
