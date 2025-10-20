// SPDX-License-Identifier: GPL-2.0+
/*
 * ams AS5600 -- 12-Bit Programmable Contactless Potentiometer
 *
 * Copyright (c) 2021 Frank Zago
 * Copyright (c) 2025 Aditya Dutt
 *
 * datasheet
 *    https://ams-osram.com/products/sensor-solutions/position-sensors/ams-as5600-position-sensor
 *
 * The rotating magnet is installed from 0.5mm to 3mm parallel to and
 * above the chip.
 *
 * The raw angle value returned by the chip is [0..4095]. The channel
 * 0 (in_angl0_raw) returns the unscaled and unmodified angle, always
 * covering the 360 degrees. The channel 1 returns the chip adjusted
 * angle, covering from 18 to 360 degrees, as modified by its
 * ZPOS/MPOS/MANG values,
 *
 * ZPOS and MPOS can be programmed through their debugfs entries. The
 * MANG register doesn't appear to be programmable without flashing
 * the chip.
 *
 * If the DIR pin is grounded, angles will increase when the magnet is
 * turned clockwise. If DIR is connected to Vcc, it will be the opposite.
 *
 * The i2c address of the device is 0x36.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>

/* Register definitions */
#define AS5600_REG_ZMCO              0x00
#define     AS5600_MASK_ZMCO         GENMASK(1, 0)
#define AS5600_REG_ZPOS_H            0x01
#define     AS5600_MASK_ZPOS_H       GENMASK(3, 0) /* bits 11:8 */
#define AS5600_REG_ZPOS_L            0x02
#define AS5600_REG_MPOS_H            0x03
#define     AS5600_MASK_MPOS_H       GENMASK(3, 0) /* bits 11:8 */
#define AS5600_REG_MPOS_L            0x04
#define AS5600_REG_MANG_H            0x05
#define     AS5600_MASK_MANG_H       GENMASK(3, 0) /* bits 11:8 */
#define AS5600_REG_MANG_L            0x06
#define AS5600_REG_CONF_H            0x07
#define     AS5600_MASK_CONF_H       GENMASK(5, 0)
#define     AS5600_MASK_SF           GENMASK(1, 0)
#define     AS5600_MASK_FTH          GENMASK(4, 2)
#define     AS5600_MASK_WD           BIT(5)
#define AS5600_REG_CONF_L            0x08
#define     AS5600_MASK_PM           GENMASK(1, 0)
#define     AS5600_MASK_HYST         GENMASK(3, 2)
#define     AS5600_MASK_OUTS         GENMASK(5, 4)
#define     AS5600_MASK_PWMF         GENMASK(7, 6)
#define AS5600_REG_STATUS            0x0B
#define     AS5600_MASK_STATUS       GENMASK(5, 3)
#define     AS5600_MASK_MH           BIT(3)
#define     AS5600_MASK_ML           BIT(4)
#define     AS5600_MASK_MD           BIT(5)
#define AS5600_REG_RAW_ANGLE_H       0x0C
#define     AS5600_MASK_RAW_ANGLE_H  GENMASK(3, 0) /* bits 11:8 */
#define AS5600_REG_RAW_ANGLE_L       0x0D
#define AS5600_REG_ANGLE_H           0x0E
#define     AS5600_MASK_ANGLE_H      GENMASK(3, 0) /* bits 11:8 */
#define AS5600_REG_ANGLE_L           0x0F
#define AS5600_REG_AGC               0x1A
#define AS5600_REG_MAGN_H            0x1B
#define     AS5600_MASK_MAGN_H       GENMASK(3, 0) /* bits 11:8 */
#define AS5600_REG_MAGN_L            0x1C
#define AS5600_REG_BURN              0xFF

/* Combined 16-bit register addresses for clarity */
#define AS5600_REG_ZPOS              0x01
#define AS5600_REG_MPOS              0x03
#define AS5600_REG_RAW_ANGLE         0x0C
#define AS5600_REG_ANGLE             0x0E

/* Field masks for the entire 2 byte */
#define AS5600_FIELD_ZPOS            GENMASK(11, 0)
#define AS5600_FIELD_MPOS            GENMASK(11, 0)
#define AS5600_FIELD_RAW_ANGLE       GENMASK(11, 0)
#define AS5600_FIELD_ANGLE           GENMASK(11, 0)

struct as5600_priv {
	struct i2c_client *client;
	struct mutex lock;
	u16 zpos;
	u16 mpos;
};

static int as5600_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct as5600_priv *priv = iio_priv(indio_dev);
	u16 bitmask;
	s32 ret;
	u16 reg;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->channel == 0) {
			reg = AS5600_REG_RAW_ANGLE;
			bitmask = AS5600_FIELD_RAW_ANGLE;
		} else {
			reg = AS5600_REG_ANGLE;
			bitmask = AS5600_FIELD_ANGLE;
		}
		ret = i2c_smbus_read_word_swapped(priv->client, reg);

		if (ret < 0)
			return ret;
		*val = ret & bitmask;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/* Always 4096 steps, but angle range varies between
		 * 18 and 360 degrees.
		 */
		if (chan->channel == 0) {
			/* Whole angle range = 2*pi / 4096 */
			*val = 2 * 3141592;
			*val2 = 4096000000;
		} else {
			s32 range;

			/* MPOS - ZPOS defines the active angle selection */
			/* Partial angle = (range / 4096) * (2*pi / 4096) */
			mutex_lock(&priv->lock);
			range = priv->mpos - priv->zpos;
			mutex_unlock(&priv->lock);
			if (range <= 0)
				range += 4096;

			*val = range * 2 * 314159;
			*val /= 4096;
			*val2 = 409600000;
		}

		return IIO_VAL_FRACTIONAL;

	default:
		return -EINVAL;
	}
}

static ssize_t as5600_reg_access_read(struct as5600_priv *priv,
				      unsigned int reg, unsigned int *val)
{
	int ret;
	u8 mask;

	switch (reg) {
	case AS5600_REG_ZMCO:
		mask = AS5600_MASK_ZMCO;
		break;
	case AS5600_REG_ZPOS_H:
		mask = AS5600_MASK_ZPOS_H;
		break;
	case AS5600_REG_MPOS_H:
		mask = AS5600_MASK_MPOS_H;
		break;
	case AS5600_REG_MANG_H:
		mask = AS5600_MASK_MANG_H;
		break;
	case AS5600_REG_CONF_H:
		mask = AS5600_MASK_CONF_H;
		break;
	case AS5600_REG_STATUS:
		mask = AS5600_MASK_STATUS;
		break;
	case AS5600_REG_RAW_ANGLE_H:
		mask = AS5600_MASK_RAW_ANGLE_H;
		break;
	case AS5600_REG_ANGLE_H:
		mask = AS5600_MASK_ANGLE_H;
		break;
	case AS5600_REG_MAGN_H:
		mask = AS5600_MASK_MAGN_H;
		break;
	case AS5600_REG_ZPOS_L:
	case AS5600_REG_MPOS_L:
	case AS5600_REG_MANG_L:
	case AS5600_REG_CONF_L:
	case AS5600_REG_RAW_ANGLE_L:
	case AS5600_REG_ANGLE_L:
	case AS5600_REG_AGC:
	case AS5600_REG_MAGN_L:
		mask = 0xFF;
		break;
	default:
		/* Not a readable register */
		return -EINVAL;
	}


	ret = i2c_smbus_read_byte_data(priv->client, reg);
	if (ret < 0)
		return ret;

	/* because the chip may return garbage data in the unused bits */
	*val = ret & mask;
	return 0;
}

static ssize_t as5600_reg_access_write(struct as5600_priv *priv,
				       unsigned int reg, unsigned int writeval)
{
	int ret;
	u8 mask;

	if (writeval > 0xFF)
		return -EINVAL;

	switch (reg) {
	case AS5600_REG_ZPOS_H:
		mask = AS5600_MASK_ZPOS_H;
		break;
	case AS5600_REG_MPOS_H:
		mask = AS5600_MASK_MPOS_H;
		break;
	case AS5600_REG_MANG_H:
		mask = AS5600_MASK_MANG_H;
		break;
	case AS5600_REG_CONF_H:
		mask = AS5600_MASK_CONF_H;
		break;
	case AS5600_REG_ZPOS_L:
	case AS5600_REG_MPOS_L:
	case AS5600_REG_MANG_L:
	case AS5600_REG_CONF_L:
	case AS5600_REG_BURN:
		mask = 0xFF;
		break;
	default:
		/* Not a writable register */
		return -EINVAL;
	}

	ret = i2c_smbus_write_byte_data(priv->client, reg, writeval & mask);
	if (ret < 0)
		return ret;

	/* update priv->zpos and priv->mpos */
	mutex_lock(&priv->lock);
	switch (reg) {
	case AS5600_REG_ZPOS_H:
		priv->zpos = (priv->zpos & 0x00FF) | ((writeval & mask) << 8);
		break;
	case AS5600_REG_ZPOS_L:
		priv->zpos = (priv->zpos & 0xFF00) | (writeval & mask);
		break;
	case AS5600_REG_MPOS_H:
		priv->mpos = (priv->mpos & 0x00FF) | ((writeval & mask) << 8);
		break;
	case AS5600_REG_MPOS_L:
		priv->mpos = (priv->mpos & 0xFF00) | (writeval & mask);
		break;
	}
	mutex_unlock(&priv->lock);
	return 0;
}

static int as5600_reg_access(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct as5600_priv *priv = iio_priv(indio_dev);
	int ret;

	if (readval) {
		ret = as5600_reg_access_read(priv, reg, readval);
	} else {
		ret = as5600_reg_access_write(priv, reg, writeval);
	}

	return ret;
}

static const struct iio_chan_spec as5600_channels[] = {
	{
		.type = IIO_ANGL,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE),
		.indexed = 1,
		.channel = 0,
	},
	{
		.type = IIO_ANGL,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_SCALE),
		.indexed = 1,
		.channel = 1,
	},
};

static const struct iio_info as5600_info = {
	.read_raw = &as5600_read_raw,
	.debugfs_reg_access = &as5600_reg_access,
};

static int as5600_probe(struct i2c_client *client)
{
	struct as5600_priv *priv;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	priv->client = client;
	mutex_init(&priv->lock);

	indio_dev->info = &as5600_info;
	indio_dev->name = "as5600";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = as5600_channels;
	indio_dev->num_channels = ARRAY_SIZE(as5600_channels);

	ret = i2c_smbus_read_byte_data(client, AS5600_REG_STATUS);
	if (ret < 0)
		return ret;

	/* No magnet present could be a problem. */
	if ((ret & AS5600_MASK_MD) == 0)
		dev_warn(&client->dev, "Magnet not detected\n");

	ret = i2c_smbus_read_word_swapped(client, AS5600_REG_ZPOS);
	if (ret < 0)
		return ret;
	priv->zpos = ret & AS5600_FIELD_ZPOS;

	ret = i2c_smbus_read_word_swapped(client, AS5600_REG_MPOS);
	if (ret < 0)
		return ret;
	priv->mpos = ret & AS5600_FIELD_MPOS;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id as5600_id[] = {
	{ "as5600" },
	{}
};
MODULE_DEVICE_TABLE(i2c, as5600_id);

static const struct of_device_id as5600_match[] = {
	{ .compatible = "ams,as5600" },
	{ },
};
MODULE_DEVICE_TABLE(of, as5600_match);

static struct i2c_driver as5600_driver = {
	.driver = {
		.name = "as5600",
		.of_match_table = as5600_match,
	},
	.probe = as5600_probe,
	.id_table   = as5600_id,
};

module_i2c_driver(as5600_driver);

MODULE_AUTHOR("Frank Zago <frank@zago.net>");
MODULE_AUTHOR("Aditya Dutt <duttaditya18@gmail.com>");
MODULE_DESCRIPTION("ams AS5600 Position Sensor");
MODULE_LICENSE("GPL");
