// SPDX-License-Identifier: GPL-2.0+
/*
 * ens21x.c - Support for ScioSense ens21x
 *           temperature & humidity sensor
 *
 * (7-bit I2C slave address 0x43 ENS210)
 * (7-bit I2C slave address 0x43 ENS210A)
 * (7-bit I2C slave address 0x44 ENS211)
 * (7-bit I2C slave address 0x45 ENS212)
 * (7-bit I2C slave address 0x46 ENS213A)
 * (7-bit I2C slave address 0x47 ENS215)
 *
 * Datasheet:
 *  https://www.sciosense.com/wp-content/uploads/2024/04/ENS21x-Datasheet.pdf
 *  https://www.sciosense.com/wp-content/uploads/2023/12/ENS210-Datasheet.pdf
 */

#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/crc7.h>

/* register definitions */
#define ENS21X_REG_PART_ID		0x00
#define ENS21X_REG_DIE_REV		0x02
#define ENS21X_REG_UID			0x04
#define ENS21X_REG_SYS_CTRL		0x10
#define ENS21X_REG_SYS_STAT		0x11
#define ENS21X_REG_SENS_RUN		0x21
#define ENS21X_REG_SENS_START		0x22
#define ENS21X_REG_SENS_STOP		0x23
#define ENS21X_REG_SENS_STAT		0x24
#define ENS21X_REG_T_VAL		0x30
#define ENS21X_REG_H_VAL		0x33

/* value definitions */
#define ENS21X_SENS_START_T_START		BIT(0)
#define ENS21X_SENS_START_H_START		BIT(1)

#define ENS21X_SENS_STAT_T_ACTIVE		BIT(0)
#define ENS21X_SENS_STAT_H_ACTIVE		BIT(1)

#define ENS21X_SYS_CTRL_LOW_POWER_ENABLE	BIT(0)
#define ENS21X_SYS_CTRL_SYS_RESET		BIT(7)

#define ENS21X_SYS_STAT_SYS_ACTIVE		BIT(0)

/* magic constants */
#define ENS21X_CONST_TEMP_SCALE_INT 15 /* integer part of temperature scale (1/64) */
#define ENS21X_CONST_TEMP_SCALE_DEC 625000 /* decimal part of temperature scale */
#define ENS21X_CONST_HUM_SCALE_INT 1 /* integer part of humidity scale (1/512) */
#define ENS21X_CONST_HUM_SCALE_DEC 953125 /* decimal part of humidity scale */
#define ENS21X_CONST_TEMP_OFFSET_INT -17481 /* temperature offset (64 * -273.15) */
#define ENS21X_CONST_TEMP_OFFSET_DEC 600000 /* decimal part of offset */
#define ENS210_CONST_CONVERSION_TIME 130
#define ENS212_CONST_CONVERSION_TIME 32
#define ENS215_CONST_CONVERSION_TIME 132

static const struct of_device_id ens21x_of_match[];

struct ens21x_dev {
	struct i2c_client *client;
	struct mutex lock;
	int part_id;
};

enum ens21x_partnumber {
	ENS210	= 0x0210,
	ENS210A	= 0xa210,
	ENS211	= 0x0211,
	ENS212	= 0x0212,
	ENS213A	= 0xa213,
	ENS215	= 0x0215,
};

/* calculate 17-bit crc7 */
static u8 ens21x_crc7(u32 val)
{
	u32 val_be = (htonl(val & 0x1ffff) >> 0x8);

	return crc7_be(0xde, (u8 *)&val_be, 3) >> 1;
}

static int ens21x_get_measurement(struct iio_dev *indio_dev, bool temp, int *val)
{
	u32 regval, regval_le;
	int ret, tries;
	struct ens21x_dev *dev_data = iio_priv(indio_dev);

	/* assert read */
	i2c_smbus_write_byte_data(dev_data->client, ENS21X_REG_SENS_START,
				  temp ? ENS21X_SENS_START_T_START :
					 ENS21X_SENS_START_H_START);

	/* wait for conversion to be ready */
	switch (dev_data->part_id) {
	case ENS210:
	case ENS210A:
		msleep(ENS210_CONST_CONVERSION_TIME);
		break;
	case ENS211:
	case ENS212:
		msleep(ENS212_CONST_CONVERSION_TIME);
		break;
	case ENS213A:
	case ENS215:
		msleep(ENS215_CONST_CONVERSION_TIME);
		break;
	default:
		dev_err(&dev_data->client->dev, "unrecognised device");
		return -ENODEV;
	}

	tries = 10;
	while (tries-- > 0) {
		usleep_range(4000, 5000);
		ret = i2c_smbus_read_byte_data(dev_data->client,
					       ENS21X_REG_SENS_STAT);
		if (ret < 0)
			continue;
		if (!(ret & (temp ? ENS21X_SENS_STAT_T_ACTIVE :
				    ENS21X_SENS_STAT_H_ACTIVE)))
			break;
	}
	if (tries < 0) {
		dev_err(&indio_dev->dev, "timeout waiting for sensor reading\n");
		return -EIO;
	}

	/* perform read */
	ret = i2c_smbus_read_i2c_block_data(
		dev_data->client, temp ? ENS21X_REG_T_VAL : ENS21X_REG_H_VAL, 3,
		(u8 *)&regval_le);
	if (ret < 0) {
		dev_err(&dev_data->client->dev, "failed to read register");
		return -EIO;
	} else if (ret == 3) {
		regval = le32_to_cpu(regval_le);
		if (ens21x_crc7(regval) == ((regval >> 17) & 0x7f)) {
			*val = regval & 0xffff;
			return IIO_VAL_INT;
		}
		/* crc fail */
		dev_err(&indio_dev->dev, "ens invalid crc\n");
		return -EIO;
	}

	dev_err(&indio_dev->dev, "expected 3 bytes, received %d\n", ret);
	return -EIO;
}

static int ens21x_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *channel, int *val,
			   int *val2, long mask)
{
	struct ens21x_dev *dev_data = iio_priv(indio_dev);
	int ret = -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&dev_data->lock);
		ret = ens21x_get_measurement(
			indio_dev, channel->type == IIO_TEMP, val);
		mutex_unlock(&dev_data->lock);
		break;
	case IIO_CHAN_INFO_SCALE:
		if (channel->type == IIO_TEMP) {
			*val = ENS21X_CONST_TEMP_SCALE_INT;
			*val2 = ENS21X_CONST_TEMP_SCALE_DEC;
		} else {
			*val = ENS21X_CONST_HUM_SCALE_INT;
			*val2 = ENS21X_CONST_HUM_SCALE_DEC;
		}
		ret = IIO_VAL_INT_PLUS_MICRO;
		break;
	case IIO_CHAN_INFO_OFFSET:
		if (channel->type == IIO_TEMP) {
			*val = ENS21X_CONST_TEMP_OFFSET_INT;
			*val2 = ENS21X_CONST_TEMP_OFFSET_DEC;
			ret = IIO_VAL_INT_PLUS_MICRO;
			break;
		}
		*val = 0;
		ret =  IIO_VAL_INT;
		break;
	default:
		break;
	}
	return ret;
}

static const struct iio_chan_spec ens21x_channels[] = {
	/* Temperature channel */
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
	},
	/* Humidity channel */
	{
		.type = IIO_HUMIDITYRELATIVE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
	}
};

static const struct iio_info ens21x_info = {
	.read_raw = ens21x_read_raw,
};

static int ens21x_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	const struct of_device_id *match;
	struct ens21x_dev *dev_data;
	struct iio_dev *indio_dev;
	uint16_t part_id_le, part_id;
	int ret, tries;

	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_WRITE_BYTE_DATA |
			I2C_FUNC_SMBUS_WRITE_BYTE |
			I2C_FUNC_SMBUS_READ_I2C_BLOCK)) {
		dev_err(&client->dev,
			"adapter does not support some i2c transactions\n");
		return -EOPNOTSUPP;
	}

	match = i2c_of_match_device(ens21x_of_match, client);
	if (!match)
		return -ENODEV;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*dev_data));
	if (!indio_dev)
		return -ENOMEM;

	dev_data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	dev_data->client = client;
	mutex_init(&dev_data->lock);

	/* reset device */
	ret = i2c_smbus_write_byte_data(client, ENS21X_REG_SYS_CTRL,
					ENS21X_SYS_CTRL_SYS_RESET);
	if (ret)
		return ret;

	/* wait for device to become active */
	usleep_range(4000, 5000);

	/* disable low power mode */
	ret = i2c_smbus_write_byte_data(client, ENS21X_REG_SYS_CTRL, 0x00);
	if (ret)
		return ret;

	/* wait for device to become active */
	tries = 10;
	while (tries-- > 0) {
		msleep(20);
		ret = i2c_smbus_read_byte_data(client, ENS21X_REG_SYS_STAT);
		if (ret < 0)
			return ret;
		if (ret & ENS21X_SYS_STAT_SYS_ACTIVE)
			break;
	}
	if (tries < 0) {
		dev_err(&client->dev,
			"timeout waiting for ens21x to become active\n");
		return -EIO;
	}

	/* get part_id */
	part_id_le = i2c_smbus_read_word_data(client, ENS21X_REG_PART_ID);
	if (part_id_le < 0)
		return part_id_le;
	part_id = le16_to_cpu(part_id_le);

	if (part_id != id->driver_data) {
		dev_err(&client->dev,
			"Part ID does not match (0x%04x != 0x%04lx)\n", part_id,
			id->driver_data);
		return -ENODEV;
	}

	/* reenable low power */
	ret = i2c_smbus_write_byte_data(client, ENS21X_REG_SYS_CTRL,
					ENS21X_SYS_CTRL_LOW_POWER_ENABLE);
	if (ret)
		return ret;

	dev_data->part_id = part_id;

	indio_dev->name = id->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ens21x_channels;
	indio_dev->num_channels = ARRAY_SIZE(ens21x_channels);
	indio_dev->info = &ens21x_info;

	return devm_iio_device_register(&client->dev, indio_dev);
}


static const struct of_device_id ens21x_of_match[] = {
	{ .compatible = "sciosense,ens210", .data = (void *)ENS210},
	{ .compatible = "sciosense,ens210a", .data = (void *)ENS210A },
	{ .compatible = "sciosense,ens211", .data = (void *)ENS211},
	{ .compatible = "sciosense,ens212", .data = (void *)ENS212},
	{ .compatible = "sciosense,ens213a", .data = (void *)ENS213A },
	{ .compatible = "sciosense,ens215", .data = (void *)ENS215},
	{},
};
MODULE_DEVICE_TABLE(of, ens21x_of_match);

static const struct i2c_device_id ens21x_id[] = {
	{"ens210", ENS210},
	{"ens210a", ENS210A},
	{"ens211", ENS211},
	{"ens212", ENS212},
	{"ens213a", ENS213A},
	{"ens215", ENS215},
	{}
};
MODULE_DEVICE_TABLE(i2c, ens21x_id);

static struct i2c_driver ens21x_driver = {
	.probe = ens21x_probe,
	.id_table = ens21x_id,
	.driver = {
		.name = "ens21x",
		.of_match_table = ens21x_of_match,
	},
};

module_i2c_driver(ens21x_driver);

MODULE_DESCRIPTION("ScioSense ENS21x temperature and humidity sensor driver");
MODULE_AUTHOR("Joshua Felmeden <jfelmeden@thegoodpenguin.co.uk>");
MODULE_LICENSE("GPL");

