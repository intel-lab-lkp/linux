// SPDX-License-Identifier: GPL-2.0+
/*
 * Vishay VCNL36829 Proximity Sensor driver
 *
 * Copyright (C) 2026 Liu Yufei <lyf98405@gmail.com>
 */
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/iio/iio.h>
#include <linux/regmap.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>

#define VCNL36829_PS_CONF1 0x00
#define VCNL36829_PS_ON_MSK BIT(0)
#define VCNL36829_PS_SD_MSK BIT(8)

#define VCNL36829_PS_CONF2 0x01
#define VCNL36829_PS_IT_MSK GENMASK(11, 8)

#define VCNL36829_PS_CONF3 0x02
#define VCNL36829_PS_CURRENT_EN_MSK BIT(3)
#define VCNL36829_PS_CURRENT_MSK GENMASK(2, 0)
#define VCNL36829_PD1_EN_MSK BIT(8)
#define VCNL36829_PD2_EN_MSK BIT(9)
#define VCNL36829_PD3_EN_MSK BIT(10)

#define VCNL36829_PS_CONF5 0x08

/* VCNL36829 Read Only Registers */
#define VCNL36829_DATA 0xF8
#define VCNL36829_DEV_ID    0xFA

#define VCNL36829_PS_CONF5_INIT 0x00A0
#define VCNL36829_PS_CURRENT_18MA 0x7
#define VCNL36829_PS_INT_TIME_25 0x1
#define VCNL36829_DEV_ID_VAL 0x29

static const int vcnl36829_it_range[] = {0, 25, 0, 25, 0, 375};

struct vcnl36829_data {
	struct i2c_client *client;
	struct regmap *regmap;
};

static const struct regmap_config vcnl36829_regmap_config = {
	.name = "vcnl36829_regmap",
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = VCNL36829_DEV_ID,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int vcnl36829_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	int ret;
	unsigned int reg;
	struct vcnl36829_data *data = iio_priv(indio_dev);
	struct regmap *regmap = data->regmap;
	struct device *dev = &data->client->dev;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(regmap, chan->address, &reg);
		if (ret) {
			dev_err(dev, "Data read failed: %d\n", ret);
			return ret;
		}
		*val = reg;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_INT_TIME:
		ret = regmap_read(regmap, VCNL36829_PS_CONF2, &reg);
		if (ret) {
			dev_err(dev, "Data read failed: %d\n", ret);
			return ret;
		}
		/*
		 * PS_IT=0 (12.5us) is intentionally not exposed; the driver
		 * uses 25us as the minimum so the step stays a clean 25us.
		 */
		*val = 0;
		*val2 = FIELD_GET(VCNL36829_PS_IT_MSK, reg) * 25;
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static int vcnl36829_write_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan, int val,
			       int val2, long mask)
{
	struct vcnl36829_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		if (val != 0)
			return -EINVAL;
		if (val2 >= 25 && val2 <= 375 && val2 % 25 == 0)
			return regmap_update_bits(data->regmap,
					VCNL36829_PS_CONF2,
					VCNL36829_PS_IT_MSK,
					FIELD_PREP(VCNL36829_PS_IT_MSK, val2 / 25));
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static int vcnl36829_read_avail(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				const int **vals, int *type, int *length,
				long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		*length = 6;
		*vals = vcnl36829_it_range;
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_RANGE;

	default:
		return -EINVAL;
	}
}

static const struct iio_info vcnl36829_info = {
	.read_raw = vcnl36829_read_raw,
	.write_raw = vcnl36829_write_raw,
	.read_avail = vcnl36829_read_avail,
};

static const struct iio_chan_spec vcnl36829_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.address = VCNL36829_DATA,
		.indexed = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_shared_by_type_available = BIT(IIO_CHAN_INFO_INT_TIME),
	}
};

static void vcnl36829_shutdown_action(void *data)
{
	struct vcnl36829_data *vcnl36829_data = data;

	regmap_update_bits(vcnl36829_data->regmap,
			   VCNL36829_PS_CONF1,
			   VCNL36829_PS_ON_MSK | VCNL36829_PS_SD_MSK,
			   FIELD_PREP(VCNL36829_PS_ON_MSK, 0) |
			   FIELD_PREP(VCNL36829_PS_SD_MSK, 1));
}

static int vcnl36829_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct vcnl36829_data *data;
	struct iio_dev *indio_dev;
	struct regmap *regmap;

	int ret;
	unsigned int reg;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "I2C adapter doesn't support plain I2C\n");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(client, &vcnl36829_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Regmap setup failed\n");

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	data->client = client;
	data->regmap = regmap;

	indio_dev->name = "vcnl36829";
	indio_dev->info = &vcnl36829_info;
	indio_dev->channels = vcnl36829_channels;
	indio_dev->num_channels = ARRAY_SIZE(vcnl36829_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vdd\n");

	ret = devm_regulator_get_enable(dev, "vddio");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vddio\n");

	fsleep(3000);

	ret = regmap_read(regmap, VCNL36829_DEV_ID, &reg);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read device ID\n");
	if ((reg & 0xFF) != VCNL36829_DEV_ID_VAL)
		return dev_err_probe(dev, -ENODEV,
				     "Unknown device ID 0x%x\n", reg);

	ret = regmap_update_bits(regmap, VCNL36829_PS_CONF2,
				 VCNL36829_PS_IT_MSK,
				 FIELD_PREP(VCNL36829_PS_IT_MSK, VCNL36829_PS_INT_TIME_25));
	if (ret)
		return dev_err_probe(dev, ret,
				     "Could not configure PS_IT\n");

	ret = regmap_update_bits(regmap, VCNL36829_PS_CONF3,
				 VCNL36829_PS_CURRENT_EN_MSK |
				 VCNL36829_PS_CURRENT_MSK |
				 VCNL36829_PD1_EN_MSK |
				 VCNL36829_PD2_EN_MSK |
				 VCNL36829_PD3_EN_MSK,
				 FIELD_PREP(VCNL36829_PS_CURRENT_EN_MSK, 1) |
				 FIELD_PREP(VCNL36829_PS_CURRENT_MSK, VCNL36829_PS_CURRENT_18MA) |
				 FIELD_PREP(VCNL36829_PD1_EN_MSK, 1) |
				 FIELD_PREP(VCNL36829_PD2_EN_MSK, 1) |
				 FIELD_PREP(VCNL36829_PD3_EN_MSK, 1));
	if (ret)
		return dev_err_probe(dev, ret,
				     "Could not configure VCSEL/PD\n");

	ret = regmap_write(regmap, VCNL36829_PS_CONF5, VCNL36829_PS_CONF5_INIT);
	if (ret)
		return dev_err_probe(dev, ret, "Could not set PS_CONF5\n");

	ret = regmap_update_bits(regmap, VCNL36829_PS_CONF1,
				 VCNL36829_PS_ON_MSK | VCNL36829_PS_SD_MSK,
				 FIELD_PREP(VCNL36829_PS_ON_MSK, 1) |
				 FIELD_PREP(VCNL36829_PS_SD_MSK, 0));
	if (ret)
		return dev_err_probe(dev, ret,
				     "Could not set initial config\n");

	ret = devm_add_action_or_reset(dev, vcnl36829_shutdown_action, data);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id vcnl36829_id_table[] = {
	{ .name = "vcnl36829" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, vcnl36829_id_table);

static const struct of_device_id vcnl36829_of_match[] = {
	{ .compatible = "vishay,vcnl36829" },
	{ }
};
MODULE_DEVICE_TABLE(of, vcnl36829_of_match);

static struct i2c_driver vcnl36829_driver = {
	.probe = vcnl36829_probe,
	.id_table = vcnl36829_id_table,
	.driver = {
		.name = "vcnl36829",
		.of_match_table = vcnl36829_of_match,
	},
};
module_i2c_driver(vcnl36829_driver);

MODULE_DESCRIPTION("Vishay VCNL36829 proximity sensor driver");
MODULE_AUTHOR("Liu Yufei <lyf98405@gmail.com>");
MODULE_LICENSE("GPL");
