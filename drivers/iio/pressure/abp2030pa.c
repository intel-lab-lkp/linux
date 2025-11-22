// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Honeywell ABP2 series pressure sensor driver
 *
 * Copyright (c) 2025 Petre Rodan <petre.rodan@subdimension.ro>
 *
 * Datasheet: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/pressure-sensors/board-mount-pressure-sensors/basic-abp2-series/documents/sps-siot-abp2-series-datasheet-32350268-en.pdf
 *
 */

#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#include "abp2030pa.h"

/* status byte flags */
#define ABP2_ST_POWER     BIT(6) /* 1 if device is powered */
#define ABP2_ST_BUSY      BIT(5) /* 1 if device is busy */

struct abp2_func_spec {
	u32 output_min;
	u32 output_max;
};

/*
 * transfer function A: 10%   to 90%   of 2^24
 */
static const struct abp2_func_spec abp2_func_spec[] = {
	[ABP2_FUNCTION_A] = { .output_min = 1677722, .output_max = 15099494 },
};

enum abp2_variants {
	ABP2001BA = 0x0, ABP21_6BA = 0x1, ABP22_5BA = 0x2, ABP2004BA = 0x3,
	ABP2006BA = 0x4, ABP2008BA = 0x5, ABP2010BA = 0x6, ABP2012BA = 0x7,
	ABP2001BD = 0x8, ABP21_6BD = 0x9, ABP22_5BD = 0xa, ABP2004BD = 0xb,
	ABP2001BG = 0xc, ABP21_6BG = 0xd, ABP22_5BG = 0xe, ABP2004BG = 0xf,
	ABP2006BG = 0x10, ABP2008BG = 0x11, ABP2010BG = 0x12, ABP2012BG = 0x13,
	ABP2001GG = 0x14, ABP21_2GG = 0x15, ABP2100KA = 0x16, ABP2160KA = 0x17,
	ABP2250KA = 0x18, ABP2001KD = 0x19, ABP21_6KD = 0x1a, ABP22_5KD = 0x1b,
	ABP2004KD = 0x1c, ABP2006KD = 0x1d, ABP2010KD = 0x1e, ABP2016KD = 0x1f,
	ABP2025KD = 0x20, ABP2040KD = 0x21, ABP2060KD = 0x22, ABP2100KD = 0x23,
	ABP2160KD = 0x24, ABP2250KD = 0x25, ABP2400KD = 0x26, ABP2001KG = 0x27,
	ABP21_6KG = 0x28, ABP22_5KG = 0x29, ABP2004KG = 0x2a, ABP2006KG = 0x2b,
	ABP2010KG = 0x2c, ABP2016KG = 0x2d, ABP2025KG = 0x2e, ABP2040KG = 0x2f,
	ABP2060KG = 0x30, ABP2100KG = 0x31, ABP2160KG = 0x32, ABP2250KG = 0x33,
	ABP2400KG = 0x34, ABP2600KG = 0x35, ABP2800KG = 0x36, ABP2250LD = 0x37,
	ABP2600LD = 0x38, ABP2600LG = 0x39, ABP22_5MD = 0x3a, ABP2006MD = 0x3b,
	ABP2010MD = 0x3c, ABP2016MD = 0x3d, ABP2025MD = 0x3e, ABP2040MD = 0x3f,
	ABP2060MD = 0x40, ABP2100MD = 0x41, ABP2160MD = 0x42, ABP2250MD = 0x43,
	ABP2400MD = 0x44, ABP2600MD = 0x45, ABP2006MG = 0x46, ABP2010MG = 0x47,
	ABP2016MG = 0x48, ABP2025MG = 0x49, ABP2040MG = 0x4a, ABP2060MG = 0x4b,
	ABP2100MG = 0x4c, ABP2160MG = 0x4d, ABP2250MG = 0x4e, ABP2400MG = 0x4f,
	ABP2600MG = 0x50, ABP2001ND = 0x51, ABP2002ND = 0x52, ABP2004ND = 0x53,
	ABP2005ND = 0x54, ABP2010ND = 0x55, ABP2020ND = 0x56, ABP2030ND = 0x57,
	ABP2002NG = 0x58, ABP2004NG = 0x59, ABP2005NG = 0x5a, ABP2010NG = 0x5b,
	ABP2020NG = 0x5c, ABP2030NG = 0x5d, ABP2015PA = 0x5e, ABP2030PA = 0x5f,
	ABP2060PA = 0x60, ABP2100PA = 0x61, ABP2150PA = 0x62, ABP2175PA = 0x63,
	ABP2001PD = 0x64, ABP2005PD = 0x65, ABP2015PD = 0x66, ABP2030PD = 0x67,
	ABP2060PD = 0x68, ABP2001PG = 0x69, ABP2005PG = 0x6a, ABP2015PG = 0x6b,
	ABP2030PG = 0x6c, ABP2060PG = 0x6d, ABP2100PG = 0x6e, ABP2150PG = 0x6f,
	ABP2175PG = 0x70, ABP2_VARIANTS_MAX
};

static const char * const abp2_triplet_variants[ABP2_VARIANTS_MAX] = {
	[ABP2001BA] = "001BA", [ABP21_6BA] = "1.6BA", [ABP22_5BA] = "2.5BA",
	[ABP2004BA] = "004BA", [ABP2006BA] = "006BA", [ABP2008BA] = "008BA",
	[ABP2010BA] = "010BA", [ABP2012BA] = "012BA", [ABP2001BD] = "001BD",
	[ABP21_6BD] = "1.6BD", [ABP22_5BD] = "2.5BD", [ABP2004BD] = "004BD",
	[ABP2001BG] = "001BG", [ABP21_6BG] = "1.6BG", [ABP22_5BG] = "2.5BG",
	[ABP2004BG] = "004BG", [ABP2006BG] = "006BG", [ABP2008BG] = "008BG",
	[ABP2010BG] = "010BG", [ABP2012BG] = "012BG", [ABP2001GG] = "001GG",
	[ABP21_2GG] = "1.2GG", [ABP2100KA] = "100KA", [ABP2160KA] = "160KA",
	[ABP2250KA] = "250KA", [ABP2001KD] = "001KD", [ABP21_6KD] = "1.6KD",
	[ABP22_5KD] = "2.5KD", [ABP2004KD] = "004KD", [ABP2006KD] = "006KD",
	[ABP2010KD] = "010KD", [ABP2016KD] = "016KD", [ABP2025KD] = "025KD",
	[ABP2040KD] = "040KD", [ABP2060KD] = "060KD", [ABP2100KD] = "100KD",
	[ABP2160KD] = "160KD", [ABP2250KD] = "250KD", [ABP2400KD] = "400KD",
	[ABP2001KG] = "001KG", [ABP21_6KG] = "1.6KG", [ABP22_5KG] = "2.5KG",
	[ABP2004KG] = "004KG", [ABP2006KG] = "006KG", [ABP2010KG] = "010KG",
	[ABP2016KG] = "016KG", [ABP2025KG] = "025KG", [ABP2040KG] = "040KG",
	[ABP2060KG] = "060KG", [ABP2100KG] = "100KG", [ABP2160KG] = "160KG",
	[ABP2250KG] = "250KG", [ABP2400KG] = "400KG", [ABP2600KG] = "600KG",
	[ABP2800KG] = "800KG", [ABP2250LD] = "250LD", [ABP2600LD] = "600LD",
	[ABP2600LG] = "600LG", [ABP22_5MD] = "2.5MD", [ABP2006MD] = "006MD",
	[ABP2010MD] = "010MD", [ABP2016MD] = "016MD", [ABP2025MD] = "025MD",
	[ABP2040MD] = "040MD", [ABP2060MD] = "060MD", [ABP2100MD] = "100MD",
	[ABP2160MD] = "160MD", [ABP2250MD] = "250MD", [ABP2400MD] = "400MD",
	[ABP2600MD] = "600MD", [ABP2006MG] = "006MG", [ABP2010MG] = "010MG",
	[ABP2016MG] = "016MG", [ABP2025MG] = "025MG", [ABP2040MG] = "040MG",
	[ABP2060MG] = "060MG", [ABP2100MG] = "100MG", [ABP2160MG] = "160MG",
	[ABP2250MG] = "250MG", [ABP2400MG] = "400MG", [ABP2600MG] = "600MG",
	[ABP2001ND] = "001ND", [ABP2002ND] = "002ND", [ABP2004ND] = "004ND",
	[ABP2005ND] = "005ND", [ABP2010ND] = "010ND", [ABP2020ND] = "020ND",
	[ABP2030ND] = "030ND", [ABP2002NG] = "002NG", [ABP2004NG] = "004NG",
	[ABP2005NG] = "005NG", [ABP2010NG] = "010NG", [ABP2020NG] = "020NG",
	[ABP2030NG] = "030NG", [ABP2015PA] = "015PA", [ABP2030PA] = "030PA",
	[ABP2060PA] = "060PA", [ABP2100PA] = "100PA", [ABP2150PA] = "150PA",
	[ABP2175PA] = "175PA", [ABP2001PD] = "001PD", [ABP2005PD] = "005PD",
	[ABP2015PD] = "015PD", [ABP2030PD] = "030PD", [ABP2060PD] = "060PD",
	[ABP2001PG] = "001PG", [ABP2005PG] = "005PG", [ABP2015PG] = "015PG",
	[ABP2030PG] = "030PG", [ABP2060PG] = "060PG", [ABP2100PG] = "100PG",
	[ABP2150PG] = "150PG", [ABP2175PG] = "175PG"
};

/**
 * struct abp2_range_config - list of pressure ranges based on nomenclature
 * @pmin: lowest pressure that can be measured
 * @pmax: highest pressure that can be measured
 */
struct abp2_range_config {
	const s32 pmin;
	const s32 pmax;
};

/* All min max limits have been converted to pascals */
static const struct abp2_range_config abp2_range_config[ABP2_VARIANTS_MAX] = {
	[ABP2001BA] = { .pmin =       0, .pmax =  100000 },
	[ABP21_6BA] = { .pmin =       0, .pmax =  160000 },
	[ABP22_5BA] = { .pmin =       0, .pmax =  250000 },
	[ABP2004BA] = { .pmin =       0, .pmax =  400000 },
	[ABP2006BA] = { .pmin =       0, .pmax =  600000 },
	[ABP2008BA] = { .pmin =       0, .pmax =  800000 },
	[ABP2010BA] = { .pmin =       0, .pmax = 1000000 },
	[ABP2012BA] = { .pmin =       0, .pmax = 1200000 },
	[ABP2001BD] = { .pmin = -100000, .pmax =  100000 },
	[ABP21_6BD] = { .pmin = -160000, .pmax =  160000 },
	[ABP22_5BD] = { .pmin = -250000, .pmax =  250000 },
	[ABP2004BD] = { .pmin = -400000, .pmax =  400000 },
	[ABP2001BG] = { .pmin =       0, .pmax =  100000 },
	[ABP21_6BG] = { .pmin =       0, .pmax =  160000 },
	[ABP22_5BG] = { .pmin =       0, .pmax =  250000 },
	[ABP2004BG] = { .pmin =       0, .pmax =  400000 },
	[ABP2006BG] = { .pmin =       0, .pmax =  600000 },
	[ABP2008BG] = { .pmin =       0, .pmax =  800000 },
	[ABP2010BG] = { .pmin =       0, .pmax = 1000000 },
	[ABP2012BG] = { .pmin =       0, .pmax = 1200000 },
	[ABP2001GG] = { .pmin =       0, .pmax = 1000000 },
	[ABP21_2GG] = { .pmin =       0, .pmax = 1200000 },
	[ABP2100KA] = { .pmin =       0, .pmax =  100000 },
	[ABP2160KA] = { .pmin =       0, .pmax =  160000 },
	[ABP2250KA] = { .pmin =       0, .pmax =  250000 },
	[ABP2001KD] = { .pmin =   -1000, .pmax =    1000 },
	[ABP21_6KD] = { .pmin =   -1600, .pmax =    1600 },
	[ABP22_5KD] = { .pmin =   -2500, .pmax =    2500 },
	[ABP2004KD] = { .pmin =   -4000, .pmax =    4000 },
	[ABP2006KD] = { .pmin =   -6000, .pmax =    6000 },
	[ABP2010KD] = { .pmin =  -10000, .pmax =   10000 },
	[ABP2016KD] = { .pmin =  -16000, .pmax =   16000 },
	[ABP2025KD] = { .pmin =  -25000, .pmax =   25000 },
	[ABP2040KD] = { .pmin =  -40000, .pmax =   40000 },
	[ABP2060KD] = { .pmin =  -60000, .pmax =   60000 },
	[ABP2100KD] = { .pmin = -100000, .pmax =  100000 },
	[ABP2160KD] = { .pmin = -160000, .pmax =  160000 },
	[ABP2250KD] = { .pmin = -250000, .pmax =  250000 },
	[ABP2400KD] = { .pmin = -400000, .pmax =  400000 },
	[ABP2001KG] = { .pmin =       0, .pmax =    1000 },
	[ABP21_6KG] = { .pmin =       0, .pmax =    1600 },
	[ABP22_5KG] = { .pmin =       0, .pmax =    2500 },
	[ABP2004KG] = { .pmin =       0, .pmax =    4000 },
	[ABP2006KG] = { .pmin =       0, .pmax =    6000 },
	[ABP2010KG] = { .pmin =       0, .pmax =   10000 },
	[ABP2016KG] = { .pmin =       0, .pmax =   16000 },
	[ABP2025KG] = { .pmin =       0, .pmax =   25000 },
	[ABP2040KG] = { .pmin =       0, .pmax =   40000 },
	[ABP2060KG] = { .pmin =       0, .pmax =   60000 },
	[ABP2100KG] = { .pmin =       0, .pmax =  100000 },
	[ABP2160KG] = { .pmin =       0, .pmax =  160000 },
	[ABP2250KG] = { .pmin =       0, .pmax =  250000 },
	[ABP2400KG] = { .pmin =       0, .pmax =  400000 },
	[ABP2600KG] = { .pmin =       0, .pmax =  600000 },
	[ABP2800KG] = { .pmin =       0, .pmax =  800000 },
	[ABP2250LD] = { .pmin =    -250, .pmax =     250 },
	[ABP2600LD] = { .pmin =    -600, .pmax =     600 },
	[ABP2600LG] = { .pmin =       0, .pmax =     600 },
	[ABP22_5MD] = { .pmin =    -250, .pmax =     250 },
	[ABP2006MD] = { .pmin =    -600, .pmax =     600 },
	[ABP2010MD] = { .pmin =   -1000, .pmax =    1000 },
	[ABP2016MD] = { .pmin =   -1600, .pmax =    1600 },
	[ABP2025MD] = { .pmin =   -2500, .pmax =    2500 },
	[ABP2040MD] = { .pmin =   -4000, .pmax =    4000 },
	[ABP2060MD] = { .pmin =   -6000, .pmax =    6000 },
	[ABP2100MD] = { .pmin =  -10000, .pmax =   10000 },
	[ABP2160MD] = { .pmin =  -16000, .pmax =   16000 },
	[ABP2250MD] = { .pmin =  -25000, .pmax =   25000 },
	[ABP2400MD] = { .pmin =  -40000, .pmax =   40000 },
	[ABP2600MD] = { .pmin =  -60000, .pmax =   60000 },
	[ABP2006MG] = { .pmin =       0, .pmax =     600 },
	[ABP2010MG] = { .pmin =       0, .pmax =    1000 },
	[ABP2016MG] = { .pmin =       0, .pmax =    1600 },
	[ABP2025MG] = { .pmin =       0, .pmax =    2500 },
	[ABP2040MG] = { .pmin =       0, .pmax =    4000 },
	[ABP2060MG] = { .pmin =       0, .pmax =    6000 },
	[ABP2100MG] = { .pmin =       0, .pmax =   10000 },
	[ABP2160MG] = { .pmin =       0, .pmax =   16000 },
	[ABP2250MG] = { .pmin =       0, .pmax =   25000 },
	[ABP2400MG] = { .pmin =       0, .pmax =   40000 },
	[ABP2600MG] = { .pmin =       0, .pmax =   60000 },
	[ABP2001ND] = { .pmin =    -249, .pmax =     249 },
	[ABP2002ND] = { .pmin =    -498, .pmax =     498 },
	[ABP2004ND] = { .pmin =    -996, .pmax =     996 },
	[ABP2005ND] = { .pmin =   -1245, .pmax =    1245 },
	[ABP2010ND] = { .pmin =   -2491, .pmax =    2491 },
	[ABP2020ND] = { .pmin =   -4982, .pmax =    4982 },
	[ABP2030ND] = { .pmin =   -7473, .pmax =    7473 },
	[ABP2002NG] = { .pmin =       0, .pmax =     498 },
	[ABP2004NG] = { .pmin =       0, .pmax =     996 },
	[ABP2005NG] = { .pmin =       0, .pmax =    1245 },
	[ABP2010NG] = { .pmin =       0, .pmax =    2491 },
	[ABP2020NG] = { .pmin =       0, .pmax =    4982 },
	[ABP2030NG] = { .pmin =       0, .pmax =    7473 },
	[ABP2015PA] = { .pmin =       0, .pmax =  103421 },
	[ABP2030PA] = { .pmin =       0, .pmax =  206843 },
	[ABP2060PA] = { .pmin =       0, .pmax =  413685 },
	[ABP2100PA] = { .pmin =       0, .pmax =  689476 },
	[ABP2150PA] = { .pmin =       0, .pmax = 1034214 },
	[ABP2175PA] = { .pmin =       0, .pmax = 1206583 },
	[ABP2001PD] = { .pmin =   -6895, .pmax =    6895 },
	[ABP2005PD] = { .pmin =  -34474, .pmax =   34474 },
	[ABP2015PD] = { .pmin = -103421, .pmax =  103421 },
	[ABP2030PD] = { .pmin = -206843, .pmax =  206843 },
	[ABP2060PD] = { .pmin = -413685, .pmax =  413685 },
	[ABP2001PG] = { .pmin =       0, .pmax =    6895 },
	[ABP2005PG] = { .pmin =       0, .pmax =   34474 },
	[ABP2015PG] = { .pmin =       0, .pmax =  103421 },
	[ABP2030PG] = { .pmin =       0, .pmax =  206843 },
	[ABP2060PG] = { .pmin =       0, .pmax =  413685 },
	[ABP2100PG] = { .pmin =       0, .pmax =  689476 },
	[ABP2150PG] = { .pmin =       0, .pmax = 1034214 },
	[ABP2175PG] = { .pmin =       0, .pmax = 1206583 }
};

static int abp2_get_measurement(struct abp2_data *data)
{
	struct device *dev = data->dev;
	int ret;

	memset(data->buffer, 0, sizeof(data->buffer));
	reinit_completion(&data->completion);

	ret = data->ops->write(data, ABP2_CMD_SYNC, ABP2_PKT_SYNC_LEN);
	if (ret < 0)
		return ret;

	if (data->irq > 0) {
		ret = wait_for_completion_timeout(&data->completion, HZ);
		if (!ret) {
			dev_err(dev, "timeout waiting for EOC interrupt\n");
			return -ETIMEDOUT;
		}
	} else
		fsleep(5000);

	ret = data->ops->read(data, ABP2_CMD_NOP, ABP2_PKT_NOP_LEN);
	if (ret < 0)
		return ret;

	/*
	 * status byte flags
	 *  bit7 SANITY_CHK     - must always be 0
	 *  bit6 ABP2_ST_POWER  - 1 if device is powered
	 *  bit5 ABP2_ST_BUSY   - 1 if device has no new conversion ready
	 *  bit4 SANITY_CHK     - must always be 0
	 *  bit3 SANITY_CHK     - must always be 0
	 *  bit2 MEMORY_ERR     - 1 if integrity test has failed
	 *  bit1 SANITY_CHK     - must always be 0
	 *  bit0 MATH_ERR       - 1 during internal math saturation err
	 */

	if (data->buffer[0] == (ABP2_ST_POWER | ABP2_ST_BUSY))
		return -ETIMEDOUT;

	if (data->buffer[0] != ABP2_ST_POWER) {
		dev_err(data->dev,
			"unexpected status byte 0x%02x\n", data->buffer[0]);
		return -ETIMEDOUT;
	}

	return 0;
}

static irqreturn_t abp2_eoc_handler(int irq, void *private)
{
	struct abp2_data *data = private;

	complete(&data->completion);

	return IRQ_HANDLED;
}

static irqreturn_t abp2_trigger_handler(int irq, void *private)
{
	int ret;
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct abp2_data *data = iio_priv(indio_dev);

	ret = abp2_get_measurement(data);
	if (ret < 0)
		goto err;

	data->scan.chan[0] = get_unaligned_be24(&data->buffer[1]);
	data->scan.chan[1] = get_unaligned_be24(&data->buffer[4]);

	iio_push_to_buffers_with_ts(indio_dev, &data->scan, sizeof(data->scan),
				    iio_get_time_ns(indio_dev));

err:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

/*
 * IIO ABI expects
 * value = (conv + offset) * scale
 *
 * datasheet provides the following formula for determining the temperature
 * temp[C] = conv * a + b
 *   where a = 200/16777215; b = -50
 *
 *  temp[C] = (conv + (b/a)) * a * (1000)
 *  =>
 *  scale = a * 1000 = .0000119209296 * 1000 = .01192092966562
 *  offset = b/a = -50 * 16777215 / 200 = -4194303.75
 *
 *  based on the datasheet
 *  pressure = (conv - Omin) * Q + Pmin =
 *          ((conv - Omin) + Pmin/Q) * Q
 *  =>
 *  scale = Q = (Pmax - Pmin) / (Omax - Omin)
 *  offset = Pmin/Q - Omin = Pmin * (Omax - Omin) / (Pmax - Pmin) - Omin
 */
static int abp2_read_raw(struct iio_dev *indio_dev,
			 struct iio_chan_spec const *channel, int *val,
			 int *val2, long mask)
{
	struct abp2_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = abp2_get_measurement(data);
		if (ret < 0)
			return ret;

		switch (channel->type) {
		case IIO_PRESSURE:
			*val = get_unaligned_be24(&data->buffer[1]);
			return IIO_VAL_INT;
		case IIO_TEMP:
			*val = get_unaligned_be24(&data->buffer[4]);
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		switch (channel->type) {
		case IIO_TEMP:
			*val = 0;
			*val2 = 11920929;
			return IIO_VAL_INT_PLUS_NANO;
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
			*val = -4194304;
			return IIO_VAL_INT;
		case IIO_PRESSURE:
			*val = data->p_offset;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static const struct iio_info abp2_info = {
	.read_raw = &abp2_read_raw,
};

static const struct iio_chan_spec abp2_channels[] = {
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
					BIT(IIO_CHAN_INFO_SCALE) |
					BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = 0,
		.scan_type = {
			.sign = 'u',
			.realbits = 24,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = 1,
		.scan_type = {
			.sign = 'u',
			.realbits = 24,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(2),
};


int abp2_common_probe(struct device *dev, const struct abp2_ops *ops, int irq)
{
	int ret;
	struct abp2_data *data;
	struct iio_dev *indio_dev;
	const char *triplet;
	s64 tmp;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->dev = dev;
	data->ops = ops;
	data->irq = irq;

	init_completion(&data->completion);

	indio_dev->name = "abp2030pa";
	indio_dev->info = &abp2_info;
	indio_dev->channels = abp2_channels;
	indio_dev->num_channels = ARRAY_SIZE(abp2_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
				     "can't get and enable vdd supply\n");

	ret = data->ops->init(data->dev);
	if (ret)
		return ret;

	ret = device_property_read_string(dev, "honeywell,pressure-triplet",
					  &triplet);
	if (ret) {
		ret = device_property_read_u32(dev, "honeywell,pmin-pascal",
					       &data->pmin);
		if (ret)
			return dev_err_probe(dev, ret,
				   "honeywell,pmin-pascal could not be read\n");

		ret = device_property_read_u32(dev, "honeywell,pmax-pascal",
					       &data->pmax);
		if (ret)
			return dev_err_probe(dev, ret,
				   "honeywell,pmax-pascal could not be read\n");
	} else {
		ret = device_property_match_property_string(dev,
						   "honeywell,pressure-triplet",
						   abp2_triplet_variants,
						   ABP2_VARIANTS_MAX);
		if (ret < 0)
			return dev_err_probe(dev, -EINVAL,
				     "honeywell,pressure-triplet is invalid\n");

		data->pmin = abp2_range_config[ret].pmin;
		data->pmax = abp2_range_config[ret].pmax;
	}

	if (data->pmin >= data->pmax)
		return dev_err_probe(dev, -EINVAL,
				     "pressure limits are invalid\n");

	data->outmin = abp2_func_spec[data->function].output_min;
	data->outmax = abp2_func_spec[data->function].output_max;

	tmp = div_s64(((s64)(data->pmax - data->pmin)) * NANO,
		      data->outmax - data->outmin);
	data->p_scale = div_s64_rem(tmp, NANO, &data->p_scale_dec);

	tmp = div_s64((s64)data->pmin * (s64)(data->outmax - data->outmin),
		      (s64)(data->pmax - data->pmin)) - (s64)(data->outmin);
	data->p_offset = tmp;

	if (data->irq > 0) {
		ret = devm_request_irq(dev, data->irq, abp2_eoc_handler,
				       IRQF_TRIGGER_RISING,
				       dev_name(dev),
				       data);
		if (ret)
			return dev_err_probe(dev, ret,
					  "request irq %d failed\n", data->irq);
	}

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev, NULL,
					      abp2_trigger_handler, NULL);
	if (ret)
		return dev_err_probe(dev, ret,
				     "iio triggered buffer setup failed\n");

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "unable to register iio device\n");

	return 0;
}
EXPORT_SYMBOL_NS(abp2_common_probe, "IIO_HONEYWELL_ABP2030PA");

MODULE_AUTHOR("Petre Rodan <petre.rodan@subdimension.ro>");
MODULE_DESCRIPTION("Honeywell ABP2 pressure sensor core driver");
MODULE_LICENSE("GPL");
