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
#include <linux/printk.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include <asm/unaligned.h>

#include "hsc030pa.h"

/*
 * HSC_PRESSURE_TRIPLET_LEN - length for the string that defines the
 * pressure range, measurement unit and type as per the part nomenclature.
 * Consult honeywell,pressure-triplet in the bindings file for details.
 */
#define HSC_PRESSURE_TRIPLET_LEN 6
#define HSC_STATUS_MASK          GENMASK(7, 6)
#define HSC_TEMPERATURE_MASK     GENMASK(15, 5)
#define HSC_PRESSURE_MASK        GENMASK(29, 16)

struct hsc_func_spec {
	u32 output_min;
	u32 output_max;
};

/*
 * function A: 10% - 90% of 2^14
 * function B:  5% - 95% of 2^14
 * function C:  5% - 85% of 2^14
 * function F:  4% - 94% of 2^14
 */
static const struct hsc_func_spec hsc_func_spec[] = {
	[HSC_FUNCTION_A] = { .output_min = 1638, .output_max = 14746 },
	[HSC_FUNCTION_B] = { .output_min =  819, .output_max = 15565 },
	[HSC_FUNCTION_C] = { .output_min =  819, .output_max = 13926 },
	[HSC_FUNCTION_F] = { .output_min =  655, .output_max = 15401 },
};

/**
 * struct hsc_range_config - list of pressure ranges based on nomenclature
 * @triplet: string that defines the range, measurement unit and type
 * @pmin: lowest pressure that can be measured
 * @pmax: highest pressure that can be measured
 */
struct hsc_range_config {
	char triplet[HSC_PRESSURE_TRIPLET_LEN];
	s32 pmin;
	u32 pmax;
};

/* all min max limits have been converted to pascals */
static const struct hsc_range_config hsc_range_config[] = {
	{ .triplet = "001BA", .pmin =       0, .pmax =  100000 },
	{ .triplet = "1.6BA", .pmin =       0, .pmax =  160000 },
	{ .triplet = "2.5BA", .pmin =       0, .pmax =  250000 },
	{ .triplet = "004BA", .pmin =       0, .pmax =  400000 },
	{ .triplet = "006BA", .pmin =       0, .pmax =  600000 },
	{ .triplet = "010BA", .pmin =       0, .pmax = 1000000 },
	{ .triplet = "1.6MD", .pmin =    -160, .pmax =     160 },
	{ .triplet = "2.5MD", .pmin =    -250, .pmax =     250 },
	{ .triplet = "004MD", .pmin =    -400, .pmax =     400 },
	{ .triplet = "006MD", .pmin =    -600, .pmax =     600 },
	{ .triplet = "010MD", .pmin =   -1000, .pmax =    1000 },
	{ .triplet = "016MD", .pmin =   -1600, .pmax =    1600 },
	{ .triplet = "025MD", .pmin =   -2500, .pmax =    2500 },
	{ .triplet = "040MD", .pmin =   -4000, .pmax =    4000 },
	{ .triplet = "060MD", .pmin =   -6000, .pmax =    6000 },
	{ .triplet = "100MD", .pmin =  -10000, .pmax =   10000 },
	{ .triplet = "160MD", .pmin =  -16000, .pmax =   16000 },
	{ .triplet = "250MD", .pmin =  -25000, .pmax =   25000 },
	{ .triplet = "400MD", .pmin =  -40000, .pmax =   40000 },
	{ .triplet = "600MD", .pmin =  -60000, .pmax =   60000 },
	{ .triplet = "001BD", .pmin = -100000, .pmax =  100000 },
	{ .triplet = "1.6BD", .pmin = -160000, .pmax =  160000 },
	{ .triplet = "2.5BD", .pmin = -250000, .pmax =  250000 },
	{ .triplet = "004BD", .pmin = -400000, .pmax =  400000 },
	{ .triplet = "2.5MG", .pmin =       0, .pmax =     250 },
	{ .triplet = "004MG", .pmin =       0, .pmax =     400 },
	{ .triplet = "006MG", .pmin =       0, .pmax =     600 },
	{ .triplet = "010MG", .pmin =       0, .pmax =    1000 },
	{ .triplet = "016MG", .pmin =       0, .pmax =    1600 },
	{ .triplet = "025MG", .pmin =       0, .pmax =    2500 },
	{ .triplet = "040MG", .pmin =       0, .pmax =    4000 },
	{ .triplet = "060MG", .pmin =       0, .pmax =    6000 },
	{ .triplet = "100MG", .pmin =       0, .pmax =   10000 },
	{ .triplet = "160MG", .pmin =       0, .pmax =   16000 },
	{ .triplet = "250MG", .pmin =       0, .pmax =   25000 },
	{ .triplet = "400MG", .pmin =       0, .pmax =   40000 },
	{ .triplet = "600MG", .pmin =       0, .pmax =   60000 },
	{ .triplet = "001BG", .pmin =       0, .pmax =  100000 },
	{ .triplet = "1.6BG", .pmin =       0, .pmax =  160000 },
	{ .triplet = "2.5BG", .pmin =       0, .pmax =  250000 },
	{ .triplet = "004BG", .pmin =       0, .pmax =  400000 },
	{ .triplet = "006BG", .pmin =       0, .pmax =  600000 },
	{ .triplet = "010BG", .pmin =       0, .pmax = 1000000 },
	{ .triplet = "100KA", .pmin =       0, .pmax =  100000 },
	{ .triplet = "160KA", .pmin =       0, .pmax =  160000 },
	{ .triplet = "250KA", .pmin =       0, .pmax =  250000 },
	{ .triplet = "400KA", .pmin =       0, .pmax =  400000 },
	{ .triplet = "600KA", .pmin =       0, .pmax =  600000 },
	{ .triplet = "001GA", .pmin =       0, .pmax = 1000000 },
	{ .triplet = "160LD", .pmin =    -160, .pmax =     160 },
	{ .triplet = "250LD", .pmin =    -250, .pmax =     250 },
	{ .triplet = "400LD", .pmin =    -400, .pmax =     400 },
	{ .triplet = "600LD", .pmin =    -600, .pmax =     600 },
	{ .triplet = "001KD", .pmin =   -1000, .pmax =    1000 },
	{ .triplet = "1.6KD", .pmin =   -1600, .pmax =    1600 },
	{ .triplet = "2.5KD", .pmin =   -2500, .pmax =    2500 },
	{ .triplet = "004KD", .pmin =   -4000, .pmax =    4000 },
	{ .triplet = "006KD", .pmin =   -6000, .pmax =    6000 },
	{ .triplet = "010KD", .pmin =  -10000, .pmax =   10000 },
	{ .triplet = "016KD", .pmin =  -16000, .pmax =   16000 },
	{ .triplet = "025KD", .pmin =  -25000, .pmax =   25000 },
	{ .triplet = "040KD", .pmin =  -40000, .pmax =   40000 },
	{ .triplet = "060KD", .pmin =  -60000, .pmax =   60000 },
	{ .triplet = "100KD", .pmin = -100000, .pmax =  100000 },
	{ .triplet = "160KD", .pmin = -160000, .pmax =  160000 },
	{ .triplet = "250KD", .pmin = -250000, .pmax =  250000 },
	{ .triplet = "400KD", .pmin = -400000, .pmax =  400000 },
	{ .triplet = "250LG", .pmin =       0, .pmax =     250 },
	{ .triplet = "400LG", .pmin =       0, .pmax =     400 },
	{ .triplet = "600LG", .pmin =       0, .pmax =     600 },
	{ .triplet = "001KG", .pmin =       0, .pmax =    1000 },
	{ .triplet = "1.6KG", .pmin =       0, .pmax =    1600 },
	{ .triplet = "2.5KG", .pmin =       0, .pmax =    2500 },
	{ .triplet = "004KG", .pmin =       0, .pmax =    4000 },
	{ .triplet = "006KG", .pmin =       0, .pmax =    6000 },
	{ .triplet = "010KG", .pmin =       0, .pmax =   10000 },
	{ .triplet = "016KG", .pmin =       0, .pmax =   16000 },
	{ .triplet = "025KG", .pmin =       0, .pmax =   25000 },
	{ .triplet = "040KG", .pmin =       0, .pmax =   40000 },
	{ .triplet = "060KG", .pmin =       0, .pmax =   60000 },
	{ .triplet = "100KG", .pmin =       0, .pmax =  100000 },
	{ .triplet = "160KG", .pmin =       0, .pmax =  160000 },
	{ .triplet = "250KG", .pmin =       0, .pmax =  250000 },
	{ .triplet = "400KG", .pmin =       0, .pmax =  400000 },
	{ .triplet = "600KG", .pmin =       0, .pmax =  600000 },
	{ .triplet = "001GG", .pmin =       0, .pmax = 1000000 },
	{ .triplet = "015PA", .pmin =       0, .pmax =  103421 },
	{ .triplet = "030PA", .pmin =       0, .pmax =  206843 },
	{ .triplet = "060PA", .pmin =       0, .pmax =  413685 },
	{ .triplet = "100PA", .pmin =       0, .pmax =  689476 },
	{ .triplet = "150PA", .pmin =       0, .pmax = 1034214 },
	{ .triplet = "0.5ND", .pmin =    -125, .pmax =     125 },
	{ .triplet = "001ND", .pmin =    -249, .pmax =     249 },
	{ .triplet = "002ND", .pmin =    -498, .pmax =     498 },
	{ .triplet = "004ND", .pmin =    -996, .pmax =     996 },
	{ .triplet = "005ND", .pmin =   -1245, .pmax =    1245 },
	{ .triplet = "010ND", .pmin =   -2491, .pmax =    2491 },
	{ .triplet = "020ND", .pmin =   -4982, .pmax =    4982 },
	{ .triplet = "030ND", .pmin =   -7473, .pmax =    7473 },
	{ .triplet = "001PD", .pmin =   -6895, .pmax =    6895 },
	{ .triplet = "005PD", .pmin =  -34474, .pmax =   34474 },
	{ .triplet = "015PD", .pmin = -103421, .pmax =  103421 },
	{ .triplet = "030PD", .pmin = -206843, .pmax =  206843 },
	{ .triplet = "060PD", .pmin = -413685, .pmax =  413685 },
	{ .triplet = "001NG", .pmin =       0, .pmax =     249 },
	{ .triplet = "002NG", .pmin =       0, .pmax =     498 },
	{ .triplet = "004NG", .pmin =       0, .pmax =     996 },
	{ .triplet = "005NG", .pmin =       0, .pmax =    1245 },
	{ .triplet = "010NG", .pmin =       0, .pmax =    2491 },
	{ .triplet = "020NG", .pmin =       0, .pmax =    4982 },
	{ .triplet = "030NG", .pmin =       0, .pmax =    7473 },
	{ .triplet = "001PG", .pmin =       0, .pmax =    6895 },
	{ .triplet = "005PG", .pmin =       0, .pmax =   34474 },
	{ .triplet = "015PG", .pmin =       0, .pmax =  103421 },
	{ .triplet = "030PG", .pmin =       0, .pmax =  206843 },
	{ .triplet = "060PG", .pmin =       0, .pmax =  413685 },
	{ .triplet = "100PG", .pmin =       0, .pmax =  689476 },
	{ .triplet = "150PG", .pmin =       0, .pmax = 1034214 },
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
	ret = data->recv_cb(data);
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
	u32 recvd;
	int raw;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = hsc_get_measurement(data);
		if (ret)
			return ret;

		recvd = get_unaligned_be32(data->buffer);
		switch (channel->type) {
		case IIO_PRESSURE:
			raw = FIELD_GET(HSC_PRESSURE_MASK, recvd);
			*val = raw;
			return IIO_VAL_INT;
		case IIO_TEMP:
			raw = FIELD_GET(HSC_TEMPERATURE_MASK, recvd);
			*val = raw;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}

/*
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

	default:
		return -EINVAL;
	}
}

static const struct iio_chan_spec hsc_channels[] = {
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
	},
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
	},
};

static const struct iio_info hsc_info = {
	.read_raw = hsc_read_raw,
};

static const struct hsc_chip_data hsc_chip = {
	.valid = hsc_measurement_is_valid,
	.channels = hsc_channels,
	.num_channels = ARRAY_SIZE(hsc_channels),
};

int hsc_common_probe(struct device *dev, void *client, hsc_recv_fn recv_fn,
			const char *name)
{
	struct hsc_data *hsc;
	struct iio_dev *indio_dev;
	const char *triplet;
	u64 tmp;
	int index;
	int found = 0;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*hsc));
	if (!indio_dev)
		return -ENOMEM;

	hsc = iio_priv(indio_dev);

	hsc->chip = &hsc_chip;
	hsc->recv_cb = recv_fn;
	hsc->client = client;

	ret = device_property_read_u32(dev, "honeywell,transfer-function",
				     &hsc->function);
	if (ret)
		return dev_err_probe(dev, ret,
			"honeywell,transfer-function could not be read\n");
	if (hsc->function > HSC_FUNCTION_F)
		return dev_err_probe(dev, -EINVAL,
				     "honeywell,transfer-function %d invalid\n",
				     hsc->function);

	ret = device_property_read_string(dev, "honeywell,pressure-triplet",
					&triplet);
	if (ret)
		return dev_err_probe(dev, ret,
			"honeywell,pressure-triplet could not be read\n");

	if (strncmp(triplet, "NA", 2) == 0) {
		ret = device_property_read_u32(dev, "honeywell,pmin-pascal",
					       &hsc->pmin);
		if (ret)
			return dev_err_probe(dev, ret,
				"honeywell,pmin-pascal could not be read\n");
		ret = device_property_read_u32(dev, "honeywell,pmax-pascal",
					       &hsc->pmax);
		if (ret)
			return dev_err_probe(dev, ret,
				"honeywell,pmax-pascal could not be read\n");
	} else {
		for (index = 0; index < ARRAY_SIZE(hsc_range_config); index++) {
			if (strncmp(hsc_range_config[index].triplet,
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

	tmp = div_s64(((s64)(hsc->pmax - hsc->pmin)) * MICRO,
		      hsc->outmax - hsc->outmin);
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
EXPORT_SYMBOL_NS(hsc_common_probe, IIO_HONEYWELL_HSC030PA);

MODULE_AUTHOR("Petre Rodan <petre.rodan@subdimension.ro>");
MODULE_DESCRIPTION("Honeywell HSC and SSC pressure sensor core driver");
MODULE_LICENSE("GPL");
