// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support for AMS AS7343 14-channel multi-spectral sensor.
 * (7-bit I2C slave address 0x39)
 *
 * Based on the work of:
 *   Christian Eggers <ceggers@arri.de> (AS73211 driver)
 *
 * Copyright (c) 2026 Chang Yu <marcus.yu.56@gmail.com>
 *
 * Datasheets:
 *   https://look.ams-osram.com/m/5f2d27fff9a874d2/original/AS7343-14-Channel-Multi-Spectral-Sensor.pdf
 *
 * TODO:
 *   - Support for configurable gain and integration time
 *   - Interrupt support
 *   - Add support for reading the VIS channel
 *   - Flicker detection
 */

#include "linux/array_size.h"
#include "linux/regmap.h"
#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/stringify.h>

#define AS7343_DRV_NAME "as7343"
#define AS7343_DEVICE_ID 0x81

/* AS7343 registers */
#define AS7343_REG_ID 0x5a
#define AS7343_REG_ENABLE 0x80
#define AS7343_REG_ATIME 0x81
#define AS7343_REG_CFG0 0xbf
#define AS7343_REG_CFG1 0xc6
#define AS7343_REG_CFG20 0xd6
#define AS7343_REG_CONTROL 0xfa
#define AS7343_REG_ASTATUS 0x94
/* AS7343 data registers */
#define AS7343_REG_DATA_FZ 0x95
#define AS7343_REG_DATA_FY 0x97
#define AS7343_REG_DATA_FXL 0x99
#define AS7343_REG_DATA_NIR 0x9b
#define AS7343_REG_DATA_F2 0xa1
#define AS7343_REG_DATA_F3 0xa3
#define AS7343_REG_DATA_F4 0xa5
#define AS7343_REG_DATA_F6 0xa7
#define AS7343_REG_DATA_F1 0xad
#define AS7343_REG_DATA_F7 0xaf
#define AS7343_REG_DATA_F8 0xb1
#define AS7343_REG_DATA_F5 0xb3
#define AS7343_REG_MAX 0xff

/* AS7343 register bit masks */
#define AS7343_ENABLE_PON BIT(0)
#define AS7343_ENABLE_SP_EN BIT(1)
#define AS7343_CFG0_REG_BANK BIT(4)
#define AS7343_CFG20_AUTO_SMUX GENMASK(6, 5)
#define AS7343_CONTROL_SW_RESET BIT(3)
#define AS7343_CFG1_AGAIN GENMASK(4, 0)

/* AS7343 settings */
#define AS7343_INT_TIME 29 /* (29 + 1) * 2.87ms = 83.4ms */
#define AS7343_GAIN 7 /* 64x gain */
#define AS7343_AUTO_CHANNEL_READOUT 3 /* Automatic all-channel readout */

/* AS7343 scan indices */
#define AS7343_SCAN_INDEX_F1 0
#define AS7343_SCAN_INDEX_F2 1
#define AS7343_SCAN_INDEX_FZ 2
#define AS7343_SCAN_INDEX_F3 3
#define AS7343_SCAN_INDEX_F4 4
#define AS7343_SCAN_INDEX_FY 5
#define AS7343_SCAN_INDEX_F5 6
#define AS7343_SCAN_INDEX_FXL 7
#define AS7343_SCAN_INDEX_F6 8
#define AS7343_SCAN_INDEX_F7 9
#define AS7343_SCAN_INDEX_F8 10
#define AS7343_SCAN_INDEX_NIR 11
#define AS7343_SCAN_INDEX_TS 12

#define AS7343_SCAN_MASK_ALL                                      \
	(BIT(AS7343_SCAN_INDEX_F1) | BIT(AS7343_SCAN_INDEX_F2) |  \
	 BIT(AS7343_SCAN_INDEX_FZ) | BIT(AS7343_SCAN_INDEX_F3) |  \
	 BIT(AS7343_SCAN_INDEX_F4) | BIT(AS7343_SCAN_INDEX_FY) |  \
	 BIT(AS7343_SCAN_INDEX_F5) | BIT(AS7343_SCAN_INDEX_FXL) | \
	 BIT(AS7343_SCAN_INDEX_F6) | BIT(AS7343_SCAN_INDEX_F7) |  \
	 BIT(AS7343_SCAN_INDEX_F8) | BIT(AS7343_SCAN_INDEX_NIR))

static const unsigned long as7343_scan_masks[] = { AS7343_SCAN_MASK_ALL, 0 };

#define AS7343_CHAN(_chan) \
	{ \
	.type = IIO_INTENSITY, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.address = AS7343_REG_DATA_##_chan, \
	.extend_name = __stringify(_chan), \
	.scan_index = AS7343_SCAN_INDEX_##_chan, \
	.scan_type = { \
		.sign = 'u', \
		.realbits = 16, \
		.storagebits = 16, \
		.endianness = IIO_LE, \
	}, \
}

static const struct iio_chan_spec as7343_channels[] = {
	AS7343_CHAN(F1),
	AS7343_CHAN(F2),
	AS7343_CHAN(FZ),
	AS7343_CHAN(F3),
	AS7343_CHAN(F4),
	AS7343_CHAN(FY),
	AS7343_CHAN(F5),
	AS7343_CHAN(FXL),
	AS7343_CHAN(F6),
	AS7343_CHAN(F7),
	AS7343_CHAN(F8),
	AS7343_CHAN(NIR),
	IIO_CHAN_SOFT_TIMESTAMP(AS7343_SCAN_INDEX_TS),
};

/**
 * struct as7343_data - Instance data for one AS7343
 * @client: I2C client.
 * @regmap: Register map.
 */
struct as7343_data {
	struct i2c_client *client;
	struct regmap *regmap;
};

static int as7343_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan, int *val,
			   int *val2, long mask)
{
	struct as7343_data *data = iio_priv(indio_dev);
	unsigned int low, high;
	unsigned int unused;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW: {
		/* Reading ASTATUS latches all data registers to this read.
		 * We don't care about the returned saturation/gain status for
		 * now.
		 */
		ret = regmap_read(data->regmap, AS7343_REG_ASTATUS, &unused);
		if (ret < 0)
			return ret;

		ret = regmap_read(data->regmap, chan->address, &low);
		if (ret < 0)
			return ret;
		ret = regmap_read(data->regmap, chan->address + 1, &high);
		if (ret < 0)
			return ret;
		*val = (high << 8) | low;
		return IIO_VAL_INT;
	}

	default:
		return -EINVAL;
	}
}

static const struct iio_info as7343_info = {
	.read_raw = as7343_read_raw,
};

static const struct regmap_config as7343_regmap_config = {
	.name = "as7343",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AS7343_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.cache_type = REGCACHE_NONE,
};

static int as7343_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct as7343_data *data;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	unsigned int val;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(client, &as7343_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;
	data->regmap = regmap;

	indio_dev->name = AS7343_DRV_NAME;
	indio_dev->info = &as7343_info;
	indio_dev->channels = as7343_channels;
	indio_dev->num_channels = ARRAY_SIZE(as7343_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->available_scan_masks = as7343_scan_masks;

	ret = devm_regulator_get_enable(&client->dev, "vdd");
	if (ret < 0)
		return ret;
	/* Power on */
	ret = regmap_set_bits(data->regmap, AS7343_REG_ENABLE,
			      AS7343_ENABLE_PON);
	if (ret < 0)
		return ret;

	/* Need to set REG_BANK to 1 before we can access ID */
	ret = regmap_set_bits(data->regmap, AS7343_REG_CFG0,
			      AS7343_CFG0_REG_BANK);
	if (ret < 0)
		return ret;
	/* Check device ID */
	ret = regmap_read(data->regmap, AS7343_REG_ID, &val);
	if (val != AS7343_DEVICE_ID)
		return -ENODEV;
	/* Unset REG_BANK */
	ret = regmap_clear_bits(data->regmap, AS7343_REG_CFG0,
				AS7343_CFG0_REG_BANK);
	if (ret < 0)
		return ret;

	/* Configure the SMUX to readout all channels */
	ret = regmap_update_bits(data->regmap, AS7343_REG_CFG20,
				 AS7343_CFG20_AUTO_SMUX,
				 FIELD_PREP(AS7343_CFG20_AUTO_SMUX,
					    AS7343_AUTO_CHANNEL_READOUT));
	if (ret < 0)
		return ret;

	/* Set 83.4ms integration time and x64 gain for now */
	ret = regmap_write(data->regmap, AS7343_REG_ATIME, AS7343_INT_TIME);
	if (ret < 0)
		return ret;
	ret = regmap_update_bits(data->regmap, AS7343_REG_CFG1,
				 AS7343_CFG1_AGAIN,
				 FIELD_PREP(AS7343_CFG1_AGAIN, AS7343_GAIN));
	if (ret < 0)
		return ret;

	/* Start measurements */
	ret = regmap_set_bits(data->regmap, AS7343_REG_ENABLE,
			      AS7343_ENABLE_SP_EN);
	if (ret < 0)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static int as7343_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct as7343_data *data = iio_priv(indio_dev);

	return regmap_clear_bits(data->regmap, AS7343_REG_ENABLE,
				 AS7343_ENABLE_SP_EN);
}

static int as7343_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct as7343_data *data = iio_priv(indio_dev);

	return regmap_set_bits(data->regmap, AS7343_REG_ENABLE,
			       AS7343_ENABLE_SP_EN);
}

static DEFINE_SIMPLE_DEV_PM_OPS(as7343_pm_ops, as7343_suspend, as7343_resume);

static const struct of_device_id as7343_of_match[] = {
	{ .compatible = "ams,as7343" },
	{},
};
MODULE_DEVICE_TABLE(of, as7343_of_match);

static const struct i2c_device_id as7343_id[] = {
	{ .name = "as7343" },
	{},
};
MODULE_DEVICE_TABLE(i2c, as7343_id);

static struct i2c_driver as7343_driver = {
	.driver = {
		.name           = AS7343_DRV_NAME,
		.of_match_table = as7343_of_match,
		.pm             = pm_sleep_ptr(&as7343_pm_ops),
	},
	.probe      = as7343_probe,
	.id_table   = as7343_id,
};
module_i2c_driver(as7343_driver);

MODULE_AUTHOR("Chang Yu <marcus.yu.56@gmail.com>");
MODULE_DESCRIPTION("AS7343 14 Channel Multi-Spectral Sensor driver");
MODULE_LICENSE("GPL");
