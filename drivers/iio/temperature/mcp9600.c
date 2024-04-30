// SPDX-License-Identifier: GPL-2.0+
/*
 * mcp9600.c - Support for Microchip MCP9600 thermocouple EMF converter
 *
 * Copyright (c) 2022 Andrew Hepp
 * Author: <andrew.hepp@ahepp.dev>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/units.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>

/* MCP9600 registers */
#define MCP9600_HOT_JUNCTION		0x0
#define MCP9600_COLD_JUNCTION		0x2
#define MCP9600_STATUS			0x4
#define MCP9600_STATUS_ALERT(x)		BIT(x)
#define MCP9600_ALERT_CFG1		0x8
#define MCP9600_ALERT_CFG(x)		(MCP9600_ALERT_CFG1 + (x - 1))
#define MCP9600_ALERT_CFG_ENABLE	BIT(0)
#define MCP9600_ALERT_CFG_ACTIVE_HIGH	BIT(2)
#define MCP9600_ALERT_CFG_FALLING	BIT(3)
#define MCP9600_ALERT_CFG_COLD_JUNCTION	BIT(4)
#define MCP9600_ALERT_HYSTERESIS1	0xc
#define MCP9600_ALERT_HYSTERESIS(x)	(MCP9600_ALERT_HYSTERESIS1 + (x - 1))
#define MCP9600_ALERT_LIMIT1		0x10
#define MCP9600_ALERT_LIMIT(x)		(MCP9600_ALERT_LIMIT1 + (x - 1))

#define MCP9600_DEVICE_ID		0x20

/* MCP9600 device id value */
#define MCP9600_DEVICE_ID_MCP9600	0x40

#define MCP9600_ALERT_COUNT		4

#define MCP9600_MIN_TEMP_HOT_JUNCTION	-200
#define MCP9600_MAX_TEMP_HOT_JUNCTION	1800

#define MCP9600_MIN_TEMP_COLD_JUNCTION	-40
#define MCP9600_MAX_TEMP_COLD_JUNCTION	125

enum mcp9600_alert {
	MCP9600_ALERT1,
	MCP9600_ALERT2,
	MCP9600_ALERT3,
	MCP9600_ALERT4
};

static const char * const mcp9600_alert_name[MCP9600_ALERT_COUNT] = {
	[MCP9600_ALERT1] = "alert1",
	[MCP9600_ALERT2] = "alert2",
	[MCP9600_ALERT3] = "alert3",
	[MCP9600_ALERT4] = "alert4",
};

static const struct iio_event_spec mcp9600_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE) |
				 BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_HYSTERESIS),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE) |
				 BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_HYSTERESIS),
	},
};

static const struct iio_chan_spec mcp9600_channels[] = {
	{
		.type = IIO_TEMP,
		.address = MCP9600_HOT_JUNCTION,
		.channel2 = IIO_MOD_TEMP_OBJECT,
		.modified = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = mcp9600_events,
		.num_event_specs = ARRAY_SIZE(mcp9600_events),
	},
	{
		.type = IIO_TEMP,
		.address = MCP9600_COLD_JUNCTION,
		.channel2 = IIO_MOD_TEMP_AMBIENT,
		.modified = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = mcp9600_events,
		.num_event_specs = ARRAY_SIZE(mcp9600_events),
	},
};

struct mcp9600_data {
	struct i2c_client *client;
	struct mutex lock[MCP9600_ALERT_COUNT];
	int irq[MCP9600_ALERT_COUNT];
};

static int mcp9600_read(struct mcp9600_data *data,
			struct iio_chan_spec const *chan, int *val)
{
	int ret;

	ret = i2c_smbus_read_word_swapped(data->client, chan->address);

	if (ret < 0)
		return ret;
	*val = ret;

	return 0;
}

static int mcp9600_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct mcp9600_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = mcp9600_read(data, chan, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 62;
		*val2 = 500000;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int mcp9600_get_alert_index(int channel2, enum iio_event_direction dir)
{
	switch (channel2) {
	case IIO_MOD_TEMP_OBJECT:
		if (dir == IIO_EV_DIR_RISING)
			return MCP9600_ALERT1;
		else
			return MCP9600_ALERT2;
	case IIO_MOD_TEMP_AMBIENT:
		if (dir == IIO_EV_DIR_RISING)
			return MCP9600_ALERT3;
		else
			return MCP9600_ALERT4;
	default:
		return -EINVAL;
	}
}

static int mcp9600_read_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	struct mcp9600_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	int i, ret;

	i = mcp9600_get_alert_index(chan->channel2, dir);
	if (i < 0)
		return i;

	ret = i2c_smbus_read_byte_data(client, MCP9600_ALERT_CFG(i + 1));
	if (ret < 0)
		return ret;

	return (ret & MCP9600_ALERT_CFG_ENABLE);
}

static int mcp9600_write_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir,
				      int state)
{
	struct mcp9600_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	int i, ret;

	i = mcp9600_get_alert_index(chan->channel2, dir);
	if (i < 0)
		return i;

	ret = i2c_smbus_read_byte_data(client, MCP9600_ALERT_CFG(i + 1));
	if (ret < 0)
		return ret;

	if (state)
		ret |= MCP9600_ALERT_CFG_ENABLE;
	else
		ret &= ~MCP9600_ALERT_CFG_ENABLE;

	return i2c_smbus_write_byte_data(client, MCP9600_ALERT_CFG(i + 1), ret);
}

static int mcp9600_read_thresh(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir,
			       enum iio_event_info info, int *val, int *val2)
{
	struct mcp9600_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	s32 ret;
	int i;

	i = mcp9600_get_alert_index(chan->channel2, dir);
	if (i < 0)
		return i;

	guard(mutex)(&data->lock[i]);
	ret = i2c_smbus_read_word_swapped(client, MCP9600_ALERT_LIMIT(i + 1));
	if (ret < 0)
		return ret;

	/*
	 * Temperature is stored in two’s complement format in bits(15:2),
	 * LSB is 0.25 degree celsius.
	 */
	*val = sign_extend32(ret, 15) >> 2;
	*val2 = 4;
	if (info == IIO_EV_INFO_VALUE)
		return IIO_VAL_FRACTIONAL;

	ret = i2c_smbus_read_byte_data(client, MCP9600_ALERT_HYSTERESIS(i + 1));
	if (ret < 0)
		return ret;

	/*
	 * Hysteresis is stored as offset which is not signed, therefore we have
	 * to include directions when calculating the real hysteresis value.
	 */
	if (dir == IIO_EV_DIR_RISING)
		*val -= (*val2 * ret);
	else
		*val += (*val2 * ret);

	return IIO_VAL_FRACTIONAL;
}

static int mcp9600_write_thresh(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				enum iio_event_type type,
				enum iio_event_direction dir,
				enum iio_event_info info, int val, int val2)
{
	struct mcp9600_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	int s_val, s_thresh, i;
	s16 thresh;
	s32 ret;
	u8 hyst;

	/* Scale value to include decimal part into calculations */
	s_val = (val < 0) ? ((val * (int)MICRO) - val2) :
			    ((val * (int)MICRO) + val2);

	/* Hot junction temperature range is from –200 to 1800 degree celsius */
	if (chan->channel2 == IIO_MOD_TEMP_OBJECT &&
	   (s_val < (MCP9600_MIN_TEMP_HOT_JUNCTION * (int)MICRO) ||
	    s_val > (MCP9600_MAX_TEMP_HOT_JUNCTION * (int)MICRO)))
		return -EINVAL;

	/* Cold junction temperature range is from –40 to 125 degree celsius */
	if (chan->channel2 == IIO_MOD_TEMP_AMBIENT &&
	   (s_val < (MCP9600_MIN_TEMP_COLD_JUNCTION * (int)MICRO) ||
	    s_val > (MCP9600_MAX_TEMP_COLD_JUNCTION * (int)MICRO)))
		return -EINVAL;

	i = mcp9600_get_alert_index(chan->channel2, dir);
	if (i < 0)
		return i;

	guard(mutex)(&data->lock[i]);
	if (info == IIO_EV_INFO_VALUE) {
		/*
		 * Shift length 4 bits = 2(15:2) + 2(0.25 LSB), temperature is
		 * stored in two’s complement format.
		 */
		thresh = (s16)(s_val / (int)(MICRO >> 4));
		return i2c_smbus_write_word_swapped(client,
						    MCP9600_ALERT_LIMIT(i + 1),
						    thresh);
	}

	/* Read out threshold, hysteresis is stored as offset */
	ret = i2c_smbus_read_word_swapped(client, MCP9600_ALERT_LIMIT(i + 1));
	if (ret < 0)
		return ret;

	/* Shift length 4 bits = 2(15:2) + 2(0.25 LSB), see above. */
	s_thresh = sign_extend32(ret, 15) * (int)(MICRO >> 4);

	/*
	 * Hysteresis is stored as offset, for rising temperatures, the
	 * hysteresis range is below the alert limit where, as for falling
	 * temperatures, the hysteresis range is above the alert limit.
	 */
	hyst = min(255, abs(s_thresh - s_val) / MICRO);

	return i2c_smbus_write_byte_data(client,
					 MCP9600_ALERT_HYSTERESIS(i + 1),
					 hyst);
}

static const struct iio_info mcp9600_info = {
	.read_raw = mcp9600_read_raw,
	.read_event_config = mcp9600_read_event_config,
	.write_event_config = mcp9600_write_event_config,
	.read_event_value = mcp9600_read_thresh,
	.write_event_value = mcp9600_write_thresh,
};

static irqreturn_t mcp9600_alert_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct mcp9600_data *data = iio_priv(indio_dev);
	enum iio_event_direction dir;
	enum iio_modifier mod;
	int i, ret;

	for (i = 0; i < MCP9600_ALERT_COUNT; i++) {
		if (data->irq[i] == irq)
			break;
	}

	if (i >= MCP9600_ALERT_COUNT)
		return IRQ_NONE;

	ret = i2c_smbus_read_byte_data(data->client, MCP9600_STATUS);
	if (ret < 0)
		return IRQ_HANDLED;

	switch (ret & MCP9600_STATUS_ALERT(i)) {
	case 0:
		return IRQ_NONE;
	case MCP9600_STATUS_ALERT(MCP9600_ALERT1):
		mod = IIO_MOD_TEMP_OBJECT;
		dir = IIO_EV_DIR_RISING;
		break;
	case MCP9600_STATUS_ALERT(MCP9600_ALERT2):
		mod = IIO_MOD_TEMP_OBJECT;
		dir = IIO_EV_DIR_FALLING;
		break;
	case MCP9600_STATUS_ALERT(MCP9600_ALERT3):
		mod = IIO_MOD_TEMP_AMBIENT;
		dir = IIO_EV_DIR_RISING;
		break;
	case MCP9600_STATUS_ALERT(MCP9600_ALERT4):
		mod = IIO_MOD_TEMP_AMBIENT;
		dir = IIO_EV_DIR_FALLING;
		break;
	default:
		return IRQ_HANDLED;
	}

	iio_push_event(indio_dev,
		       IIO_MOD_EVENT_CODE(IIO_TEMP, 0, mod,
					  IIO_EV_TYPE_THRESH, dir),
		       iio_get_time_ns(indio_dev));

	return IRQ_HANDLED;
}

static int mcp9600_probe_alerts(struct iio_dev *indio_dev)
{
	struct mcp9600_data *data = iio_priv(indio_dev);
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	unsigned int irq_type;
	int ret, irq, i;
	u8 val;

	/*
	 * alert1: hot junction, rising temperature
	 * alert2: hot junction, falling temperature
	 * alert3: cold junction, rising temperature
	 * alert4: cold junction, falling temperature
	 */
	for (i = 0; i < MCP9600_ALERT_COUNT; i++) {
		data->irq[i] = 0;
		mutex_init(&data->lock[i]);
		irq = fwnode_irq_get_byname(fwnode, mcp9600_alert_name[i]);
		if (irq <= 0)
			continue;

		val = 0;
		irq_type = irq_get_trigger_type(irq);
		if (irq_type == IRQ_TYPE_EDGE_RISING)
			val |= MCP9600_ALERT_CFG_ACTIVE_HIGH;

		if (i == MCP9600_ALERT2 || i == MCP9600_ALERT4)
			val |= MCP9600_ALERT_CFG_FALLING;

		if (i == MCP9600_ALERT3 || i == MCP9600_ALERT4)
			val |= MCP9600_ALERT_CFG_COLD_JUNCTION;

		ret = i2c_smbus_write_byte_data(client,
						MCP9600_ALERT_CFG(i + 1),
						val);
		if (ret < 0)
			return ret;

		ret = devm_request_threaded_irq(dev, irq, NULL,
						mcp9600_alert_handler,
						IRQF_ONESHOT, "mcp9600",
						indio_dev);
		if (ret)
			return ret;

		data->irq[i] = irq;
	}

	return 0;
}

static int mcp9600_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct mcp9600_data *data;
	int ret;

	ret = i2c_smbus_read_byte_data(client, MCP9600_DEVICE_ID);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to read device ID\n");

	if (ret != MCP9600_DEVICE_ID_MCP9600)
		dev_warn(dev, "Expected ID %x, got %x\n",
			 MCP9600_DEVICE_ID_MCP9600, ret);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;

	mcp9600_probe_alerts(indio_dev);

	indio_dev->info = &mcp9600_info;
	indio_dev->name = "mcp9600";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = mcp9600_channels;
	indio_dev->num_channels = ARRAY_SIZE(mcp9600_channels);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id mcp9600_id[] = {
	{ "mcp9600" },
	{}
};
MODULE_DEVICE_TABLE(i2c, mcp9600_id);

static const struct of_device_id mcp9600_of_match[] = {
	{ .compatible = "microchip,mcp9600" },
	{}
};
MODULE_DEVICE_TABLE(of, mcp9600_of_match);

static struct i2c_driver mcp9600_driver = {
	.driver = {
		.name = "mcp9600",
		.of_match_table = mcp9600_of_match,
	},
	.probe = mcp9600_probe,
	.id_table = mcp9600_id
};
module_i2c_driver(mcp9600_driver);

MODULE_AUTHOR("Dimitri Fedrau <dima.fedrau@gmail.com>");
MODULE_AUTHOR("Andrew Hepp <andrew.hepp@ahepp.dev>");
MODULE_DESCRIPTION("Microchip MCP9600 thermocouple EMF converter driver");
MODULE_LICENSE("GPL");
