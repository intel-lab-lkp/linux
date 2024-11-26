// SPDX-License-Identifier: GPL-2.0
/*
 * nct7718.c - Support for Nuvoton NCT7718W Thermal Sensor IC.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/iio/iio.h>
#include <linux/iio/events.h>

#define NCT7718_LDT_REG			0x00
#define NCT7718_RT1_TEMP_MSB		0x01
#define NCT7718_RT1_TEMP_LSB		0x10

#define NCT7718_ALERTSTATUS_REG		0x02
#define NCT7718_CONFIG_REG		0x03
#define NCT7718_ALERTMASK_REG		0x16
#define NCT7718_ALERTMODE_REG		0xBF

#define NCT7718_LT_HALERT_REG		0x05
#define NCT7718_RT1_HALERT_MSB_REG	0x0D
#define NCT7718_RT1_LALERT_MSB_REG	0x0E
#define NCT7718_RT1_HALERT_LSB_REG	0x13
#define NCT7718_RT1_LALERT_LSB_REG	0x14

#define NCT7718_CID_REG			0xFD
#define NCT7718_VID_REG			0xFE
#define NCT7718_DID_REG			0xFF

#define NCT7718_CHIP_ID			0x50
#define NCT7718_VENDOR_ID		0x50
#define NCT7718_DEVICE_ID		0x91

#define NCT7718_LSB_REG_MASK		GENMASK(7, 5)
#define NCT7718_STS_RT1LA		BIT(3)
#define NCT7718_STS_RT1HA		BIT(4)
#define NCT7718_STS_LTHA		BIT(6)
#define NCT7718_MSK_RT1L		BIT(3)
#define NCT7718_MSK_RT1H		BIT(4)
#define NCT7718_MSK_LTH			BIT(7)
#define NCT7718_MOD_COMP		BIT(0)

#define NCT7718_LOCAL_TEMP_MAX		125
#define NCT7718_LOCAL_TEMP_MIN		-40
#define NCT7718_REMOTE_TEMP_MAX_MICRO	127000000
#define NCT7718_REMOTE_TEMP_MIN_MICRO	-40000000

struct nct7718_data {
	struct i2c_client *client;
	struct mutex lock;
	u16 status_mask;
};

static const struct iio_event_spec nct7718_local_temp_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_event_spec nct7718_remote_temp_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_chan_spec nct7718_channels[] = {
	{
		.type = IIO_TEMP,
		.modified = 1,
		.channel2 = IIO_MOD_TEMP_AMBIENT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = nct7718_local_temp_events,
		.num_event_specs = ARRAY_SIZE(nct7718_local_temp_events),
	},
	{
		.type = IIO_TEMP,
		.modified = 1,
		.channel2 = IIO_MOD_TEMP_OBJECT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = nct7718_remote_temp_events,
		.num_event_specs = ARRAY_SIZE(nct7718_remote_temp_events),
	}
};

static int nct7718_read_temp_raw(struct iio_dev *indio_dev,
				 u8 msb_reg, u8 lsb_reg)
{
	struct nct7718_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	u16 reg_val;
	int ret;

	ret = i2c_smbus_read_byte_data(client, msb_reg);
	if (ret < 0)
		return ret;

	reg_val = (u16)(ret << 3);

	ret = i2c_smbus_read_byte_data(client, lsb_reg);
	if (ret < 0)
		return ret;

	reg_val |= FIELD_GET(NCT7718_LSB_REG_MASK, ret);

	return reg_val;
}

static int nct7718_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct nct7718_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->channel2) {
		case IIO_MOD_TEMP_AMBIENT:
			ret = i2c_smbus_read_byte_data(client,
						       NCT7718_LDT_REG);
			if (ret < 0)
				return ret;
			*val = sign_extend32(ret, 7) << 3;
			break;
		case IIO_MOD_TEMP_OBJECT:
			ret = nct7718_read_temp_raw(indio_dev,
						    NCT7718_RT1_TEMP_MSB,
						    NCT7718_RT1_TEMP_LSB);
			if (ret < 0)
				return ret;
			*val = sign_extend32(ret, 10);
			break;
		default:
			return -EINVAL;
		}
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 125;
		*val2 = 0;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int nct7718_read_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	struct nct7718_data *data = iio_priv(indio_dev);
	unsigned int mask;

	switch (chan->channel2) {
	case IIO_MOD_TEMP_AMBIENT:
		if (dir == IIO_EV_DIR_RISING)
			mask = NCT7718_MSK_LTH;
		break;
	case IIO_MOD_TEMP_OBJECT:
		if (dir == IIO_EV_DIR_RISING)
			mask = NCT7718_MSK_RT1H;
		else
			mask = NCT7718_MSK_RT1L;
		break;
	default:
		return -EINVAL;
	}

	return !(data->status_mask & mask);
}

static int nct7718_write_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir,
				      int state)
{
	struct nct7718_data *data = iio_priv(indio_dev);
	unsigned int status_mask;
	int ret;

	switch (chan->channel2) {
	case IIO_MOD_TEMP_AMBIENT:
		if (dir == IIO_EV_DIR_RISING)
			status_mask = NCT7718_MSK_LTH;
		break;
	case IIO_MOD_TEMP_OBJECT:
		if (dir == IIO_EV_DIR_RISING)
			status_mask = NCT7718_MSK_RT1H;
		else
			status_mask = NCT7718_MSK_RT1L;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&data->lock);
	ret = i2c_smbus_read_byte_data(data->client, NCT7718_ALERTMASK_REG);
	mutex_unlock(&data->lock);
	if (ret < 0)
		return ret;

	if (state)
		ret &= ~status_mask;
	else
		ret |=  status_mask;

	return i2c_smbus_write_byte_data(data->client, NCT7718_ALERTMASK_REG,
					 data->status_mask = ret);
}

static int nct7718_read_thresh(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir,
			       enum iio_event_info info,
			       int *val, int *val2)
{
	struct nct7718_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	u8 msb_reg, lsb_reg;
	int ret;

	switch (chan->channel2) {
	case IIO_MOD_TEMP_AMBIENT:
		if (dir == IIO_EV_DIR_RISING) {
			ret = i2c_smbus_read_byte_data(client,
						       NCT7718_LT_HALERT_REG);
			if (ret < 0)
				return ret;
			*val = sign_extend32(ret, 7);
			return IIO_VAL_INT;
		}
		return -EINVAL;
	case IIO_MOD_TEMP_OBJECT:
		if (dir == IIO_EV_DIR_RISING) {
			msb_reg = NCT7718_RT1_HALERT_MSB_REG;
			lsb_reg = NCT7718_RT1_HALERT_LSB_REG;
		} else {
			msb_reg = NCT7718_RT1_LALERT_MSB_REG;
			lsb_reg = NCT7718_RT1_LALERT_LSB_REG;
		}
		ret = nct7718_read_temp_raw(indio_dev, msb_reg, lsb_reg);
		if (ret < 0)
			return ret;

		*val = sign_extend32(ret, 10);
		*val2 = 8;
		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static int nct7718_write_thresh(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				enum iio_event_type type,
				enum iio_event_direction dir,
				enum iio_event_info info,
				int val, int val2)
{
	struct nct7718_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	u8 msb_reg, lsb_reg;
	s16 thresh;
	int ret, s_val;

	switch (chan->channel2) {
	case IIO_MOD_TEMP_AMBIENT:
		val = clamp_val(val, NCT7718_LOCAL_TEMP_MIN,
				NCT7718_LOCAL_TEMP_MAX);

		if (dir == IIO_EV_DIR_RISING) {
			return i2c_smbus_write_byte_data(client,
							 NCT7718_LT_HALERT_REG,
							 val);
		}
		break;
	case IIO_MOD_TEMP_OBJECT:
		s_val = (val < 0) ? ((val * 1000000) - val2) :
				    ((val * 1000000) + val2);

		s_val = clamp_val(s_val, NCT7718_REMOTE_TEMP_MIN_MICRO,
				  NCT7718_REMOTE_TEMP_MAX_MICRO);

		if (dir == IIO_EV_DIR_RISING) {
			msb_reg = NCT7718_RT1_HALERT_MSB_REG;
			lsb_reg = NCT7718_RT1_HALERT_LSB_REG;
		} else {
			msb_reg = NCT7718_RT1_LALERT_MSB_REG;
			lsb_reg = NCT7718_RT1_LALERT_LSB_REG;
		}

		thresh = (s16)(s_val / (1000000 >> 3));
		ret = i2c_smbus_write_byte_data(client,
						msb_reg, thresh >> 3);
		if (ret < 0)
			return ret;
		return i2c_smbus_write_byte_data(client,
						 lsb_reg, thresh << 5);
	default:
		break;
	}

	return -EINVAL;
}

static const struct iio_info nct7718_info = {
	.read_raw = nct7718_read_raw,
	.read_event_config = nct7718_read_event_config,
	.write_event_config = nct7718_write_event_config,
	.read_event_value = nct7718_read_thresh,
	.write_event_value = nct7718_write_thresh,
};

static irqreturn_t nct7718_alert_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct nct7718_data *data = iio_priv(indio_dev);
	int ret;

	guard(mutex)(&data->lock);
	ret = i2c_smbus_read_byte_data(data->client,
				       NCT7718_ALERTSTATUS_REG);
	if (ret < 0)
		return IRQ_NONE;

	if ((ret & NCT7718_STS_LTHA) &&
	    !(data->status_mask & NCT7718_MSK_LTH)) {
		iio_push_event(indio_dev,
			       IIO_MOD_EVENT_CODE(IIO_TEMP, 0,
						  IIO_MOD_TEMP_AMBIENT,
						  IIO_EV_TYPE_THRESH,
						  IIO_EV_DIR_RISING),
			       iio_get_time_ns(indio_dev));
		data->status_mask |= NCT7718_MSK_LTH;
	}
	if ((ret & NCT7718_STS_RT1HA) &&
	    !(data->status_mask & NCT7718_MSK_RT1H)) {
		iio_push_event(indio_dev,
			       IIO_MOD_EVENT_CODE(IIO_TEMP, 0,
						  IIO_MOD_TEMP_OBJECT,
						  IIO_EV_TYPE_THRESH,
						  IIO_EV_DIR_RISING),
			       iio_get_time_ns(indio_dev));
		data->status_mask |= NCT7718_MSK_RT1H;
	}
	if ((ret & NCT7718_STS_RT1LA) &&
	    !(data->status_mask & NCT7718_MSK_RT1L)) {
		iio_push_event(indio_dev,
			       IIO_MOD_EVENT_CODE(IIO_TEMP, 0,
						  IIO_MOD_TEMP_OBJECT,
						  IIO_EV_TYPE_THRESH,
						  IIO_EV_DIR_FALLING),
			       iio_get_time_ns(indio_dev));
		data->status_mask |= NCT7718_MSK_RT1L;
	}

	i2c_smbus_write_byte_data(data->client,
				  NCT7718_ALERTMASK_REG,
				  data->status_mask);

	return IRQ_HANDLED;
}

static bool nct7718_check_id(struct i2c_client *client)
{
	int chip_id, vendor_id, device_id;

	chip_id = i2c_smbus_read_byte_data(client, NCT7718_VID_REG);
	if (chip_id < 0)
		return false;

	vendor_id = i2c_smbus_read_byte_data(client, NCT7718_VID_REG);
	if (vendor_id < 0)
		return false;

	device_id = i2c_smbus_read_byte_data(client, NCT7718_DID_REG);
	if (device_id < 0)
		return false;

	return (chip_id == NCT7718_CHIP_ID &&
		vendor_id == NCT7718_VENDOR_ID &&
		device_id == NCT7718_DEVICE_ID);
}

static int nct7718_chip_config(struct nct7718_data *data)
{
	int ret;

	/* Enable MSK_LTH, MSK_RT1H, and MSK_RT1L to monitor alarm */
	ret = i2c_smbus_read_byte_data(data->client,
				       NCT7718_ALERTMASK_REG);
	if (ret < 0)
		return ret;
	data->status_mask = ret;
	data->status_mask &= ~(NCT7718_MSK_LTH	|
			       NCT7718_MSK_RT1H	|
			       NCT7718_MSK_RT1L);

	ret = i2c_smbus_write_byte_data(data->client,
					NCT7718_ALERTMASK_REG,
					data->status_mask);
	if (ret < 0)
		return ret;

	/* Config ALERT Mode Setting to comparator mode */
	return i2c_smbus_write_byte_data(data->client,
					 NCT7718_ALERTMODE_REG,
					 NCT7718_MOD_COMP);
}

static int nct7718_probe(struct i2c_client *client)
{
	struct nct7718_data *data;
	struct iio_dev *indio_dev;
	int ret;

	if (!nct7718_check_id(client)) {
		dev_err(&client->dev, "No NCT7718 device\n");
		return -ENODEV;
	}

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	mutex_init(&data->lock);

	indio_dev->name = client->name;
	indio_dev->channels = nct7718_channels;
	indio_dev->num_channels = ARRAY_SIZE(nct7718_channels);
	indio_dev->info = &nct7718_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = nct7718_chip_config(data);
	if (ret)
		return ret;

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev,
						client->irq,
						NULL,
						nct7718_alert_handler,
						IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT,
						"nct7718", indio_dev);
		if (ret) {
			dev_err(&client->dev, "Failed to request irq!\n");
			return ret;
		}
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct of_device_id nct7718_of_match[] = {
	{ .compatible = "nuvoton,nct7718" },
	{ }
};
MODULE_DEVICE_TABLE(of, nct7718_of_match);

static const struct i2c_device_id nct7718_id[] = {
	{ "nct7718" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nct7718_id);

static struct i2c_driver nct7718_driver = {
	.driver = {
		.name	= "nct7718",
		.of_match_table = nct7718_of_match,
	},
	.probe		= nct7718_probe,
	.id_table	= nct7718_id,
};
module_i2c_driver(nct7718_driver);

MODULE_DESCRIPTION("Thermal sensor driver for NCT7718W");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
