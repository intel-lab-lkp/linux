// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the DFRobot SEN0322 oxygen sensor.
 *
 * Datasheet:
 *	https://wiki.dfrobot.com/Gravity_I2C_Oxygen_Sensor_SKU_SEN0322
 *
 * Possible I2C slave addresses:
 *	0x70
 *	0x71
 *	0x72
 *	0x73
 *
 * Copyright (C) 2025 Tóth János <gomba007@gmail.com>
 */

#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>

#define DRIVER_NAME "sen0322"

#define SEN0322_REG_DATA	0x03
#define SEN0322_REG_COEFF	0x0A

#define FIXED_FRAC_BITS		18
#define FIXED_INT(x)		((fixed_t)((x) << FIXED_FRAC_BITS))

typedef u32 fixed_t;

struct sen0322 {
	struct i2c_client	*client;
	struct regmap		*regmap;
	fixed_t			coeff;
};

static fixed_t fixed_mul(fixed_t a, fixed_t b)
{
	u64 tmp;

	tmp = (u64)a * (u64)b;
	tmp = (tmp >> FIXED_FRAC_BITS) + ((tmp >> FIXED_FRAC_BITS) & 1);

	if (tmp > U32_MAX)
		return (fixed_t)U32_MAX;
	else
		return (fixed_t)tmp;
}

static fixed_t fixed_div(fixed_t a, fixed_t b)
{
	u64 tmp;

	tmp = (uint64_t)a << FIXED_FRAC_BITS;
	tmp += (b >> 1);

	return (fixed_t)(div_u64(tmp, b));
}

static int sen0322_read_coeff(struct sen0322 *sen0322)
{
	u32 val;
	int ret;

	ret = regmap_read(sen0322->regmap, SEN0322_REG_COEFF, &val);
	if (ret < 0)
		return ret;

	if (val)
		sen0322->coeff = fixed_div(FIXED_INT(val), FIXED_INT(1000));
	else
		sen0322->coeff = fixed_div(FIXED_INT(209), FIXED_INT(1200));

	dev_dbg(&sen0322->client->dev, "coeff: %08X\n", sen0322->coeff);

	return 0;
}

static int sen0322_read_data(struct sen0322 *sen0322)
{
	u8 data[4] = { 0 };
	int ret;

	ret = regmap_bulk_read(sen0322->regmap, SEN0322_REG_DATA, data, 3);
	if (ret < 0)
		return ret;

	ret = data[0] * 100 +  data[1] * 10 + data[2];

	dev_dbg(&sen0322->client->dev, "raw data: %d\n", ret);

	return ret;
}

static int sen0322_read_prep_data(struct sen0322 *sen0322)
{
	fixed_t val;
	int ret;

	if (!sen0322->coeff) {
		ret = sen0322_read_coeff(sen0322);
		if (ret < 0)
			return ret;
	}

	ret = sen0322_read_data(sen0322);
	if (ret < 0)
		return ret;

	val = fixed_mul(sen0322->coeff, FIXED_INT(ret));

	dev_dbg(&sen0322->client->dev, "prep data: %08X\n", val);

	return val >> FIXED_FRAC_BITS;
}

static int sen0322_read_raw(struct iio_dev *iio_dev,
			    const struct iio_chan_spec *chan,
			    int *val, int *val2, long mask)
{
	struct sen0322 *sen0322 = iio_priv(iio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_CONCENTRATION:
			ret = sen0322_read_data(sen0322);
			if (ret < 0)
				return ret;

			*val = ret;
			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_PROCESSED:
		switch (chan->type) {
		case IIO_CONCENTRATION:
			ret = sen0322_read_prep_data(sen0322);
			if (ret < 0)
				return ret;

			*val = ret;
			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_CONCENTRATION:
			*val = 1;
			*val2 = 100;
			return IIO_VAL_FRACTIONAL;

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static const struct iio_info sen0322_info = {
	.read_raw = sen0322_read_raw,
};

static const struct regmap_config sen0322_regmap_conf = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const struct iio_chan_spec sen0322_channels[] = {
	{
		.type = IIO_CONCENTRATION,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_PROCESSED) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int sen0322_probe(struct i2c_client *client)
{
	struct sen0322 *sen0322;
	struct iio_dev *iio_dev;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	iio_dev = devm_iio_device_alloc(&client->dev, sizeof(*sen0322));
	if (!iio_dev)
		return -ENOMEM;

	sen0322 = iio_priv(iio_dev);
	sen0322->client = client;
	sen0322->coeff = 0;

	sen0322->regmap = devm_regmap_init_i2c(client, &sen0322_regmap_conf);
	if (IS_ERR(sen0322->regmap))
		return PTR_ERR(sen0322->regmap);

	i2c_set_clientdata(client, sen0322);

	iio_dev->info = &sen0322_info;
	iio_dev->name = DRIVER_NAME;
	iio_dev->channels = sen0322_channels;
	iio_dev->num_channels = ARRAY_SIZE(sen0322_channels);
	iio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(&client->dev, iio_dev);
}

static const struct of_device_id sen0322_of_match[] = {
	{ .compatible = "dfrobot,sen0322" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sen0322_of_match);

static struct i2c_driver sen0322_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = sen0322_of_match,
	},
	.probe = sen0322_probe,
};
module_i2c_driver(sen0322_driver);

MODULE_AUTHOR("Tóth János <gomba007@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SEN0322 oxygen sensor driver");
