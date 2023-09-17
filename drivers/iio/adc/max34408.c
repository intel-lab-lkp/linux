// SPDX-License-Identifier: GPL-2.0
/*
 * IIO driver for Maxim MAX34409/34408 ADC, 4-Channels/2-Channels, 8bits, I2C
 *
 * Datasheet: https://www.analog.com/media/en/technical-documentation/data-sheets/MAX34408-MAX34409.pdf
 *
 * TODO: ALERT interrupt, Overcurrent delay, Shutdown delay
 */

#include <linux/bitfield.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/types.h>

#define MAX34408_STATUS		0x0
#define MAX34408_CONTROL	0x1
#define MAX34408_OCDELAY	0x2
#define MAX34408_SDDELAY	0x3

#define MAX34408_ADC1		0x4
#define MAX34408_ADC2		0x5
/* ADC3 & ADC4 always returns 0x0 on 34408 */
#define MAX34408_ADC3		0x6
#define MAX34408_ADC4		0x7

#define MAX34408_OCT1		0x8
#define MAX34408_OCT2		0x9
#define MAX34408_OCT3		0xA
#define MAX34408_OCT4		0xB

#define MAX34408_DID		0xC
#define MAX34408_DCYY		0xD
#define MAX34408_DCWW		0xE

#define MAX34408_CHANNELS	2
#define MAX34409_CHANNELS	4

/* Bit masks for status register */
#define STATUS_OC1		BIT(0)
#define STATUS_OC2		BIT(1)
/* OC3 & OC4 only for max34409 */
#define STATUS_OC3		BIT(2)
#define STATUS_OC4		BIT(3)
#define STATUS_SHTDN		BIT(4)
#define STATUS_ENA		BIT(5)

/* Bit masks for control register */
#define CONTROL_AVG0		BIT(0)
#define CONTROL_AVG1		BIT(1)
#define CONTROL_AVG2		BIT(2)
#define CONTROL_ALERT		BIT(3)

/* Bit masks for over current delay */
#define OCDELAY_OCD0		BIT(0)
#define OCDELAY_OCD1		BIT(1)
#define OCDELAY_OCD2		BIT(2)
#define OCDELAY_OCD3		BIT(3)
#define OCDELAY_OCD4		BIT(4)
#define OCDELAY_OCD5		BIT(5)
#define OCDELAY_OCD6		BIT(6)
#define OCDELAY_RESET		BIT(7)

/* Bit masks for shutdown delay */
#define SDDELAY_SHD0		BIT(0)
#define SDDELAY_SHD1		BIT(1)
#define SDDELAY_SHD2		BIT(2)
#define SDDELAY_SHD3		BIT(3)
#define SDDELAY_SHD4		BIT(4)
#define SDDELAY_SHD5		BIT(5)
#define SDDELAY_SHD6		BIT(6)
#define SDDELAY_RESET		BIT(7)

/**
 * struct max34408_data - max34408/max34409 specific data.
 * @regmap:	device register map.
 * @dev:	max34408 device.
 * @lock:	lock for protecting access to device hardware registers.
 * @rsense:	Rsense value in uOhm.
 */
struct max34408_data {
	struct regmap *regmap;
	struct device *dev;
	struct mutex lock;
	u32 rsense;
};

static const struct regmap_config max34408_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= MAX34408_DCWW,
};

static const struct iio_chan_spec max34408_channels[] = {
	{
		.type			= IIO_CURRENT,
		.info_mask_separate	= BIT(IIO_CHAN_INFO_RAW) |
					  BIT(IIO_CHAN_INFO_PROCESSED) |
					  BIT(IIO_CHAN_INFO_AVERAGE_RAW),
		.channel		= 0,
		.indexed = 1,
	},
	{
		.type			= IIO_CURRENT,
		.info_mask_separate	= BIT(IIO_CHAN_INFO_RAW) |
					  BIT(IIO_CHAN_INFO_PROCESSED) |
					  BIT(IIO_CHAN_INFO_AVERAGE_RAW),
		.channel		= 1,
		.indexed = 1,
	},
};

static const struct iio_chan_spec max34409_channels[] = {
	{
		.type			= IIO_CURRENT,
		.info_mask_separate	= BIT(IIO_CHAN_INFO_RAW) |
					  BIT(IIO_CHAN_INFO_PROCESSED) |
					  BIT(IIO_CHAN_INFO_AVERAGE_RAW),
		.channel		= 0,
		.indexed = 1,
	},
	{
		.type			= IIO_CURRENT,
		.info_mask_separate	= BIT(IIO_CHAN_INFO_RAW) |
					  BIT(IIO_CHAN_INFO_PROCESSED) |
					  BIT(IIO_CHAN_INFO_AVERAGE_RAW),
		.channel		= 1,
		.indexed = 1,
	},
	{
		.type			= IIO_CURRENT,
		.info_mask_separate	= BIT(IIO_CHAN_INFO_RAW) |
					  BIT(IIO_CHAN_INFO_PROCESSED) |
					  BIT(IIO_CHAN_INFO_AVERAGE_RAW),
		.channel		= 2,
		.indexed = 1,
	},
	{
		.type			= IIO_CURRENT,
		.info_mask_separate	= BIT(IIO_CHAN_INFO_RAW) |
					  BIT(IIO_CHAN_INFO_PROCESSED) |
					  BIT(IIO_CHAN_INFO_AVERAGE_RAW),
		.channel		= 3,
		.indexed = 1,
	},
};

static int max34408_read_adc(struct max34408_data *max34408, int channel,
			     int *val)
{
	int rc;
	u32 adc_reg;

	switch (channel) {
	case 0:
		adc_reg = MAX34408_ADC1;
		break;
	case 1:
		adc_reg = MAX34408_ADC2;
		break;
	case 2:
		adc_reg = MAX34408_ADC3;
		break;
	case 3:
		adc_reg = MAX34408_ADC4;
		break;
	default:
		return -EINVAL;
	}

	rc = regmap_read(max34408->regmap, adc_reg, val);
	if (rc)
		return rc;

	return 0;
}

static int max34408_read_adc_avg(struct max34408_data *max34408, int channel, int *val)
{
	unsigned long ctrl;
	int rc;
	u8 tmp;

	mutex_lock(&max34408->lock);
	rc = regmap_read(max34408->regmap, MAX34408_CONTROL, (u32 *)&ctrl);
	if (rc)
		goto err_unlock;

	/* set averaging (0b100) default values*/
	tmp = ctrl;
	set_bit(CONTROL_AVG2, &ctrl);
	clear_bit(CONTROL_AVG1, &ctrl);
	clear_bit(CONTROL_AVG0, &ctrl);
	rc = regmap_write(max34408->regmap, MAX34408_CONTROL, ctrl);
	if (rc) {
		dev_err(max34408->dev,
			"Error (%d) writing control register\n", rc);
		goto err_unlock;
	}

	rc = max34408_read_adc(max34408, channel, val);
	if (rc)
		goto err_unlock;

	/* back to old values */
	rc = regmap_write(max34408->regmap, MAX34408_CONTROL, tmp);
	if (rc)
		dev_err(max34408->dev,
			"Error (%d) writing control register\n", rc);

err_unlock:
	mutex_unlock(&max34408->lock);

	return rc;
}

static int max34408_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct max34408_data *max34408 = iio_priv(indio_dev);
	int rc;

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
	case IIO_CHAN_INFO_AVERAGE_RAW:
		rc = max34408_read_adc_avg(max34408, chan->channel,
					   val);
		if (rc)
			return rc;

		if (mask == IIO_CHAN_INFO_PROCESSED) {
			/*
			 * calcluate current for 8bit ADC with Rsense
			 * value.
			 * 10 mV * 1000 / Rsense uOhm = max current
			 * (max current * adc val * 1000) / (2^8 - 1) mA
			 */
			*val = DIV_ROUND_CLOSEST((10000 / max34408->rsense) *
						 *val * 1000, 256);
		}

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_RAW:
		rc = max34408_read_adc(max34408, chan->channel, val);
		if (rc)
			return rc;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info max34408_info = {
	.read_raw	= max34408_read_raw,
};

static int max34408_probe(struct i2c_client *client)
{
	struct device_node *np = client->dev.of_node;
	struct max34408_data *max34408;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	int rc;

	regmap = devm_regmap_init_i2c(client, &max34408_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "regmap_init failed\n");
		return PTR_ERR(regmap);
	}

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*max34408));
	if (!indio_dev)
		return -ENOMEM;

	max34408 = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	max34408->regmap = regmap;
	max34408->dev = &client->dev;
	mutex_init(&max34408->lock);
	rc = device_property_read_u32(&client->dev,
				      "maxim,rsense-val-micro-ohms",
				      &max34408->rsense);
	if (rc)
		return -EINVAL;

	/* disable ALERT and averaging */
	rc = regmap_write(max34408->regmap, MAX34408_CONTROL, 0x0);
	if (rc)
		return rc;

	if (of_device_is_compatible(np, "maxim,max34408")) {
		indio_dev->channels = max34408_channels;
		indio_dev->num_channels = ARRAY_SIZE(max34408_channels);
		indio_dev->name = "max34408";
	} else if (of_device_is_compatible(np, "maxim,max34409")) {
		indio_dev->channels = max34409_channels;
		indio_dev->num_channels = ARRAY_SIZE(max34409_channels);
		indio_dev->name = "max34409";
	}
	indio_dev->info = &max34408_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->dev.of_node = np;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct of_device_id max34408_of_match[] = {
	{
		.compatible = "maxim,max34408",
	},
	{
		.compatible = "maxim,max34409",
	},
	{}
};
MODULE_DEVICE_TABLE(of, max34408_of_match);

static struct i2c_driver max34408_driver = {
	.driver = {
		.name   = "max34408",
		.of_match_table = max34408_of_match,
	},
	.probe = max34408_probe,
};
module_i2c_driver(max34408_driver);

MODULE_AUTHOR("Ivan Mikhaylov <fr0st61te@gmail.com>");
MODULE_DESCRIPTION("Maxim MAX34408/34409 ADC driver");
MODULE_LICENSE("GPL");
