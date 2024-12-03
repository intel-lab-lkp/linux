// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Nuvoton nct7201 and nct7202 power monitor chips.
 *
 * Copyright (c) 2024 Nuvoton Inc.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define VIN_MAX					12	/* Counted from 1 */
#define NCT720X_IN_SCALING			4995
#define NCT720X_IN_SCALING_FACTOR		10000

#define REG_INTERRUPT_STATUS_1			0x0C
#define REG_INTERRUPT_STATUS_2			0x0D
#define REG_VOLT_LOW_BYTE			0x0F
#define REG_CONFIGURATION			0x10
#define  BIT_CONFIGURATION_START		BIT(0)
#define  BIT_CONFIGURATION_ALERT_MSK		BIT(1)
#define  BIT_CONFIGURATION_CONV_RATE		BIT(2)
#define  BIT_CONFIGURATION_RESET		BIT(7)

#define REG_ADVANCED_CONFIGURATION		0x11
#define  BIT_ADVANCED_CONF_MOD_ALERT		BIT(0)
#define  BIT_ADVANCED_CONF_MOD_STS		BIT(1)
#define  BIT_ADVANCED_CONF_FAULT_QUEUE		BIT(2)
#define  BIT_ADVANCED_CONF_EN_DEEP_SHUTDOWN	BIT(4)
#define  BIT_ADVANCED_CONF_EN_SMB_TIMEOUT	BIT(5)
#define  BIT_ADVANCED_CONF_MOD_RSTIN		BIT(7)

#define REG_CHANNEL_INPUT_MODE			0x12
#define REG_CHANNEL_ENABLE_1			0x13
#define  REG_CHANNEL_ENABLE_1_MASK		GENMASK(7, 0)
#define REG_CHANNEL_ENABLE_2			0x14
#define  REG_CHANNEL_ENABLE_2_MASK		GENMASK(3, 0)
#define REG_INTERRUPT_MASK_1			0x15
#define REG_INTERRUPT_MASK_2			0x16
#define REG_BUSY_STATUS				0x1E
#define  BIT_BUSY				BIT(0)
#define  BIT_PWR_UP				BIT(1)
#define REG_ONE_SHOT				0x1F
#define REG_SMUS_ADDRESS			0xFC
#define REG_VIN_LIMIT_LSB_MASK			GENMASK(4, 0)

static const u8 REG_VIN[VIN_MAX] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,	/* 0 -7 */
	0x08, 0x09, 0x0A, 0x0B,				/* 8 - 11 */
};
static const u8 REG_VIN_HIGH_LIMIT[VIN_MAX] = {
	0x20, 0x22, 0x24, 0x26, 0x28, 0x2A, 0x2C, 0x2E,
	0x30, 0x32, 0x34, 0x36,
};
static const u8 REG_VIN_LOW_LIMIT[VIN_MAX] = {
	0x21, 0x23, 0x25, 0x27, 0x29, 0x2B, 0x2D, 0x2F,
	0x31, 0x33, 0x35, 0x37,
};
static const u8 REG_VIN_HIGH_LIMIT_LSB[VIN_MAX] = {
	0x40, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x4C, 0x4E,
	0x50, 0x52, 0x54, 0x56,
};
static const u8 REG_VIN_LOW_LIMIT_LSB[VIN_MAX] = {
	0x41, 0x43, 0x45, 0x47, 0x49, 0x4B, 0x4D, 0x4F,
	0x51, 0x53, 0x55, 0x57,
};
static u8 nct720x_chan_to_index[] = {
	0 /* Not used */, 0, 1, 2, 3, 4, 5, 6,
	7, 8, 9, 10, 11,
};

struct nct720x_chip_info {
	struct i2c_client *client;
	struct mutex access_lock;	/* for multi-byte read and write operations */
	struct regmap *regmap;
	struct regmap *regmap16;
	int vin_max;			/* number of VIN channels */
	u32 vin_mask;
};

struct nct720x_adc_model_data {
	const char *model_name;
	const struct iio_chan_spec *channels;
	const int num_channels;
	int vin_max;
};

static const struct iio_event_spec nct720x_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
};

#define NCT720X_VOLTAGE_CHANNEL(chan, addr)				\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.channel = chan,					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.address = addr,					\
		.event_spec = nct720x_events,				\
		.num_event_specs = ARRAY_SIZE(nct720x_events),		\
	}

#define NCT720X_VOLTAGE_CHANNEL_DIFF(chan1, chan2, addr)		\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.channel = (chan1),					\
		.channel2 = (chan2),					\
		.differential = 1,					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.address = addr,					\
		.event_spec = nct720x_events,				\
		.num_event_specs = ARRAY_SIZE(nct720x_events),		\
	}

static const struct iio_chan_spec nct7201_channels[] = {
	NCT720X_VOLTAGE_CHANNEL(1, 1),
	NCT720X_VOLTAGE_CHANNEL(2, 2),
	NCT720X_VOLTAGE_CHANNEL(3, 3),
	NCT720X_VOLTAGE_CHANNEL(4, 4),
	NCT720X_VOLTAGE_CHANNEL(5, 5),
	NCT720X_VOLTAGE_CHANNEL(6, 6),
	NCT720X_VOLTAGE_CHANNEL(7, 7),
	NCT720X_VOLTAGE_CHANNEL(8, 8),
	NCT720X_VOLTAGE_CHANNEL_DIFF(1, 2, 1),
	NCT720X_VOLTAGE_CHANNEL_DIFF(3, 4, 3),
	NCT720X_VOLTAGE_CHANNEL_DIFF(5, 6, 5),
	NCT720X_VOLTAGE_CHANNEL_DIFF(7, 8, 7),
};

static const struct iio_chan_spec nct7202_channels[] = {
	NCT720X_VOLTAGE_CHANNEL(1, 1),
	NCT720X_VOLTAGE_CHANNEL(2, 2),
	NCT720X_VOLTAGE_CHANNEL(3, 3),
	NCT720X_VOLTAGE_CHANNEL(4, 4),
	NCT720X_VOLTAGE_CHANNEL(5, 5),
	NCT720X_VOLTAGE_CHANNEL(6, 6),
	NCT720X_VOLTAGE_CHANNEL(7, 7),
	NCT720X_VOLTAGE_CHANNEL(8, 8),
	NCT720X_VOLTAGE_CHANNEL(9, 9),
	NCT720X_VOLTAGE_CHANNEL(10, 10),
	NCT720X_VOLTAGE_CHANNEL(11, 11),
	NCT720X_VOLTAGE_CHANNEL(12, 12),
	NCT720X_VOLTAGE_CHANNEL_DIFF(1, 2, 1),
	NCT720X_VOLTAGE_CHANNEL_DIFF(3, 4, 3),
	NCT720X_VOLTAGE_CHANNEL_DIFF(5, 6, 5),
	NCT720X_VOLTAGE_CHANNEL_DIFF(7, 8, 7),
	NCT720X_VOLTAGE_CHANNEL_DIFF(9, 10, 9),
	NCT720X_VOLTAGE_CHANNEL_DIFF(11, 12, 11),
};

static int nct720x_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	int index = nct720x_chan_to_index[chan->address];
	u16 volt;
	unsigned int value;
	int err;
	struct nct720x_chip_info *chip = iio_priv(indio_dev);

	if (chan->type != IIO_VOLTAGE)
		return -EOPNOTSUPP;

	guard(mutex)(&chip->access_lock);
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		err = regmap_read(chip->regmap16, REG_VIN[index], &value);
		if (err < 0)
			return err;
		volt = (u16)value;
		*val = volt >> 3;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/* From the datasheet, we have to multiply by 0.0004995 */
		*val = 0;
		*val2 = 499500;
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

static int nct720x_read_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info,
				    int *val, int *val2)
{
	struct nct720x_chip_info *chip = iio_priv(indio_dev);
	u16 volt;
	unsigned int value;
	int tmp, err;
	int index = nct720x_chan_to_index[chan->address];

	if (chan->type != IIO_VOLTAGE)
		return -EOPNOTSUPP;

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	if (dir == IIO_EV_DIR_FALLING) {
		err = regmap_read(chip->regmap16, REG_VIN_LOW_LIMIT[index], &value);
		if (err < 0)
			return err;
		volt = (u16)value;
	} else {
		err = regmap_read(chip->regmap16, REG_VIN_HIGH_LIMIT[index], &value);
		if (err < 0)
			return err;
		volt = (u16)value;
	}

	/* Voltage(V) = 13bitCountValue * 0.0004995 */
	tmp = (volt >> 3) * NCT720X_IN_SCALING;
	*val = tmp / NCT720X_IN_SCALING_FACTOR;

	return IIO_VAL_INT;
}

static int nct720x_write_event_value(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info,
				     int val, int val2)
{
	struct nct720x_chip_info *chip = iio_priv(indio_dev);
	int index, err = 0;
	long v1, v2, volt;

	index = nct720x_chan_to_index[chan->address];
	volt = (val * NCT720X_IN_SCALING_FACTOR) / NCT720X_IN_SCALING;
	v1 = volt >> 5;
	v2 = (volt & REG_VIN_LIMIT_LSB_MASK) << 3;

	if (chan->type != IIO_VOLTAGE)
		return -EOPNOTSUPP;

	if (info == IIO_EV_INFO_VALUE) {
		if (dir == IIO_EV_DIR_FALLING) {
			guard(mutex)(&chip->access_lock);
			err = regmap_write(chip->regmap, REG_VIN_LOW_LIMIT[index], v1);
			if (err < 0)
				dev_err(&indio_dev->dev, "Failed to write REG_VIN%d_LOW_LIMIT\n",
					index + 1);

			err = regmap_write(chip->regmap, REG_VIN_LOW_LIMIT_LSB[index], v2);
			if (err < 0)
				dev_err(&indio_dev->dev, "Failed to write REG_VIN%d_LOW_LIMIT_LSB\n",
					index + 1);

		} else {
			guard(mutex)(&chip->access_lock);
			err = regmap_write(chip->regmap, REG_VIN_HIGH_LIMIT[index], v1);
			if (err < 0)
				dev_err(&indio_dev->dev, "Failed to write REG_VIN%d_HIGH_LIMIT\n",
					index + 1);

			err = regmap_write(chip->regmap, REG_VIN_HIGH_LIMIT_LSB[index], v2);
			if (err < 0)
				dev_err(&indio_dev->dev, "Failed to write REG_VIN%d_HIGH_LIMIT_LSB\n",
					index + 1);
		}
	}
	return err;
}

static int nct720x_read_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	struct nct720x_chip_info *chip = iio_priv(indio_dev);
	int index = nct720x_chan_to_index[chan->address];

	if (chan->type != IIO_VOLTAGE)
		return -EOPNOTSUPP;

	return !!(chip->vin_mask & BIT(index));
}

static int nct720x_write_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir,
				      bool state)
{
	int err = 0;
	struct nct720x_chip_info *chip = iio_priv(indio_dev);
	int index = nct720x_chan_to_index[chan->address];
	unsigned int mask;

	if (chan->type != IIO_VOLTAGE)
		return -EOPNOTSUPP;

	mask = BIT(index);

	if (!state && (chip->vin_mask & mask))
		chip->vin_mask &= ~mask;
	else if (state && !(chip->vin_mask & mask))
		chip->vin_mask |= mask;

	guard(mutex)(&chip->access_lock);

	err = regmap_write(chip->regmap, REG_CHANNEL_ENABLE_1,
			   chip->vin_mask & REG_CHANNEL_ENABLE_1_MASK);
	if (err < 0)
		dev_err(&indio_dev->dev, "Failed to write REG_CHANNEL_ENABLE_1\n");

	if (chip->vin_max == 12) {
		err = regmap_write(chip->regmap, REG_CHANNEL_ENABLE_2, chip->vin_mask >> 8);
		if (err < 0)
			dev_err(&indio_dev->dev, "Failed to write REG_CHANNEL_ENABLE_2\n");
	}
	return err;
}

static const struct iio_info nct720x_info = {
	.read_raw = nct720x_read_raw,
	.read_event_config = nct720x_read_event_config,
	.write_event_config = nct720x_write_event_config,
	.read_event_value = nct720x_read_event_value,
	.write_event_value = nct720x_write_event_value,
};

static const struct nct720x_adc_model_data nct7201_model_data = {
	.model_name = "nct7201",
	.channels = nct7201_channels,
	.num_channels = ARRAY_SIZE(nct7201_channels),
	.vin_max = 8,
};

static const struct nct720x_adc_model_data nct7202_model_data = {
	.model_name = "nct7202",
	.channels = nct7202_channels,
	.num_channels = ARRAY_SIZE(nct7202_channels),
	.vin_max = 12,
};

static int nct720x_init_chip(struct nct720x_chip_info *chip)
{
	u8 data[2];
	unsigned int value;
	int err;

	err = regmap_write(chip->regmap, REG_CONFIGURATION, BIT_CONFIGURATION_RESET);
	if (err) {
		dev_err(&chip->client->dev, "Failed to write REG_CONFIGURATION\n");
		return err;
	}

	/*
	 * After about 25 msecs, the device should be ready and then
	 * the Power Up bit will be set to 1. If not, wait for it.
	 */
	mdelay(25);
	err  = regmap_read(chip->regmap, REG_BUSY_STATUS, &value);
	if (err < 0)
		return err;
	if (!(value & BIT_PWR_UP))
		return err;

	/* Enable Channel */
	err = regmap_write(chip->regmap, REG_CHANNEL_ENABLE_1, REG_CHANNEL_ENABLE_1_MASK);
	if (err) {
		dev_err(&chip->client->dev, "Failed to write REG_CHANNEL_ENABLE_1\n");
		return err;
	}

	if (chip->vin_max == 12) {
		err = regmap_write(chip->regmap, REG_CHANNEL_ENABLE_2, REG_CHANNEL_ENABLE_2_MASK);
		if (err) {
			dev_err(&chip->client->dev, "Failed to write REG_CHANNEL_ENABLE_2\n");
			return err;
		}
	}

	guard(mutex)(&chip->access_lock);
	err  = regmap_read(chip->regmap, REG_CHANNEL_ENABLE_1, &value);
	if (err < 0)
		return err;
	data[0] = (u8)value;

	err  = regmap_read(chip->regmap, REG_CHANNEL_ENABLE_2, &value);
	if (err < 0)
		return err;
	data[1] = (u8)value;

	value = get_unaligned_le16(data);
	chip->vin_mask = value;

	/* Start monitoring if needed */
	err = regmap_read(chip->regmap, REG_CONFIGURATION, &value);
	if (err < 0) {
		dev_err(&chip->client->dev, "Failed to read REG_CONFIGURATION\n");
		return value;
	}

	value |= BIT_CONFIGURATION_START;
	err = regmap_write(chip->regmap, REG_CONFIGURATION, value);
	if (err < 0) {
		dev_err(&chip->client->dev, "Failed to write REG_CONFIGURATION\n");
		return err;
	}

	return 0;
}

static const struct regmap_config nct720x_regmap8_config = {
	.name = "vin-data-read-byte",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static const struct regmap_config nct720x_regmap16_config = {
	.name = "vin-data-read-word",
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static int nct720x_probe(struct i2c_client *client)
{
	const struct nct720x_adc_model_data *model_data;
	struct nct720x_chip_info *chip;
	struct iio_dev *indio_dev;
	int ret;

	model_data = i2c_get_match_data(client);
	if (!model_data)
		return -EINVAL;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*chip));
	if (!indio_dev)
		return -ENOMEM;
	chip = iio_priv(indio_dev);

	chip->regmap = devm_regmap_init_i2c(client, &nct720x_regmap8_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(chip->regmap),
			"Failed to init regmap\n");

	chip->regmap16 = devm_regmap_init_i2c(client, &nct720x_regmap16_config);
	if (IS_ERR(chip->regmap16))
		return dev_err_probe(&client->dev, PTR_ERR(chip->regmap16),
			"Failed to init regmap16\n");

	chip->vin_max = model_data->vin_max;

	ret = devm_mutex_init(&client->dev, &chip->access_lock);
	if (ret)
		return ret;

	chip->client = client;

	ret = nct720x_init_chip(chip);
	if (ret < 0)
		return ret;

	indio_dev->name = model_data->model_name;
	indio_dev->channels = model_data->channels;
	indio_dev->num_channels = model_data->num_channels;
	indio_dev->info = &nct720x_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id nct720x_id[] = {
	{ "nct7201", (kernel_ulong_t)&nct7201_model_data },
	{ "nct7202", (kernel_ulong_t)&nct7202_model_data },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nct720x_id);

static const struct of_device_id nct720x_of_match[] = {
	{
		.compatible = "nuvoton,nct7201",
		.data = &nct7201_model_data,
	},
	{
		.compatible = "nuvoton,nct7202",
		.data = &nct7202_model_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, nct720x_of_match);

static struct i2c_driver nct720x_driver = {
	.driver = {
		.name	= "nct720x",
		.of_match_table = nct720x_of_match,
	},
	.probe = nct720x_probe,
	.id_table = nct720x_id,
};
module_i2c_driver(nct720x_driver);

MODULE_AUTHOR("Eason Yang <j2anfernee@gmail.com>");
MODULE_DESCRIPTION("Nuvoton NCT720x voltage monitor driver");
MODULE_LICENSE("GPL");
