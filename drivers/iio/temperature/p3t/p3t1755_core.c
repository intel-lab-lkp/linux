// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP P3T175x Temperature Sensor Driver
 *
 * Copyright 2025 NXP
 */
#include <linux/err.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/device.h>
#include <linux/limits.h>
#include <linux/iio/events.h>

#include "p3t1755.h"

// Internal attribute enum for custom sysfs handlers
enum p3t1755_attr_index {
	P3T1755_ATTR_THERMOSTAT_MODE,
	P3T1755_ATTR_TRIGGER_ONE_SHOT,
	P3T1755_ATTR_FAULT_QUEUE_LENGTH,
};

// Conversion rate table: maps bits to sampling frequency
static const struct {
	u8 bits;
	int freq_hz;
} p3t1755_samp_freqs[] = {
	{ 0x00, 36 }, // 27.5 ms
	{ 0x01, 18 }, // 55 ms (default)
	{ 0x02, 9 }, // 110 ms
	{ 0x03, 4 }, // 220 ms
};

// Fault Queue values supported by hardware
static const int p3t1755_fault_queue_values[] = { 1, 2, 4, 6 };

int p3t1755_fault_queue_to_bits(int val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(p3t1755_fault_queue_values); i++)
		if (p3t1755_fault_queue_values[i] == val)
			return i;
	return -EINVAL;
}

int p3t1755_get_temp_and_limits(struct p3t1755_data *data,
				int *temp_mc, int *thigh_mc, int *tlow_mc)
{
	u8 buf[2];
	int ret;

	ret = regmap_bulk_read(data->regmap, P3T1755_REG_TEMP, buf, 2);
	if (ret) {
		dev_dbg(data->dev, "Failed to read TEMP register: %d\n", ret);
		return ret;
	}
	*temp_mc = (((buf[0] << 8) | buf[1]) >> 4) * P3T1755_RESOLUTION_10UC / 1000;
	dev_dbg(data->dev, "TEMP raw: 0x%02x%02x, temp_mc: %d\n",
		buf[0], buf[1], *temp_mc);

	ret = regmap_bulk_read(data->regmap, P3T1755_REG_HIGH_LIM, buf, 2);
	if (ret) {
		dev_dbg(data->dev, "Failed to read HIGH_LIM register: %d\n", ret);
		return ret;
	}
	*thigh_mc = (((buf[0] << 8) | buf[1]) >> 4) * P3T1755_RESOLUTION_10UC / 1000;
	dev_dbg(data->dev, "HIGH_LIM raw: 0x%02x%02x, thigh_mc: %d\n",
		buf[0], buf[1], *thigh_mc);

	ret = regmap_bulk_read(data->regmap, P3T1755_REG_LOW_LIM, buf, 2);
	if (ret) {
		dev_dbg(data->dev, "Failed to read LOW_LIM register: %d\n", ret);
		return ret;
	}
	*tlow_mc = (((buf[0] << 8) | buf[1]) >> 4) * P3T1755_RESOLUTION_10UC / 1000;
	dev_dbg(data->dev, "LOW_LIM raw: 0x%02x%02x, tlow_mc: %d\n",
		buf[0], buf[1], *tlow_mc);

	dev_dbg(data->dev, "Successfully read all temperature values\n");
	return 0;
}
EXPORT_SYMBOL_NS_GPL(p3t1755_get_temp_and_limits, IIO_P3T1755);

void p3t1755_push_thresh_event(struct iio_dev *indio_dev)
{
	struct p3t1755_data *data = iio_priv(indio_dev);
	enum iio_event_direction dir;
	int ret, temp, thigh, tlow;
	unsigned int cfgr;

	/* Read CFGR register to both check device mode
	 * and implicitly clear the ALERT latch.
	 * As per Datasheet: "Any register read will
	 * clear the interrupt"
	 */
	ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &cfgr);
	if (ret) {
		dev_dbg(data->dev, "Failed to read CFGR register: %d\n", ret);
		return;
	}

	if (cfgr & P3T1755_SHUTDOWN_BIT) {
		dev_dbg(data->dev, "Device is in shutdown mode, skipping event push\n");
		return;
	}

	ret = p3t1755_get_temp_and_limits(data, &temp, &thigh, &tlow);
	if (ret) {
		dev_dbg(data->dev, "Failed to get temperature and limits: %d\n", ret);
		return;
	}

	// Determine event direction based on threshold crossing
	if (temp >= thigh) {
		dir = IIO_EV_DIR_RISING;
	} else if (temp <= tlow) {
		dir = IIO_EV_DIR_FALLING;
	} else {
		dev_dbg(data->dev, "Temperature within limits: no event triggered (T=%d, TH=%d, TL=%d)\n",
			temp, thigh, tlow);
		return;
		}

	dev_dbg(data->dev, "Threshold event: %s (T=%d, TH=%d, TL=%d)\n",
		dir == IIO_EV_DIR_RISING ? "RISING" : "FALLING",
		temp, thigh, tlow);

	iio_push_event(indio_dev, IIO_MOD_EVENT_CODE(IIO_TEMP, 0, IIO_NO_MOD,
						     IIO_EV_TYPE_THRESH, dir),
		       iio_get_time_ns(indio_dev));
}
EXPORT_SYMBOL_NS_GPL(p3t1755_push_thresh_event, IIO_P3T1755);

static int p3t1755_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *channel, int *val,
			    int *val2, long mask)
{
	struct p3t1755_data *data = iio_priv(indio_dev);
	u8 buf[2];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		// Read raw 12-bit temperature register (two bytes)
		ret = regmap_bulk_read(data->regmap, P3T1755_REG_TEMP, buf, 2);
		if (ret < 0) {
			dev_err(data->dev, "Failed to read temperature register\n");
			return ret;
		}
		*val = ((buf[0] << 8) | buf[1]) >> 4;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_ENABLE:
		// Read configuration register to check shutdown bit
		ret = regmap_read(data->regmap, P3T1755_REG_CFGR, val);
		if (ret < 0) {
			dev_err(data->dev, "Failed to read configuration register\n");
			return ret;
		}
		dev_dbg(data->dev, "Read CONFIG: 0x%02x\n", *val);
		*val = !(*val & P3T1755_SHUTDOWN_BIT); // Return 1 if enabled, 0 if shutdown
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_PROCESSED:
		// Read temperature and convert to celsius using resolution
		ret = regmap_bulk_read(data->regmap, P3T1755_REG_TEMP, buf, 2);
		if (ret < 0) {
			dev_err(data->dev, "Failed to read temperature register\n");
			return ret;
		}
		*val = (((buf[0] << 8) | buf[1]) >> 4) *
		       P3T1755_RESOLUTION_10UC / 1000;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ: {
		unsigned int cfgr;
		int ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &cfgr);

		if (ret < 0) {
			dev_err(data->dev, "Failed to read configuration register\n");
			return ret;
		}

		u8 sel = (cfgr >> 5) & 0x03; // Extract R1:R0 sampling rate bits

		if (sel >= ARRAY_SIZE(p3t1755_samp_freqs))
			return -EINVAL;

		*val = p3t1755_samp_freqs[sel].freq_hz;
		return IIO_VAL_INT;
	}
	default:
		return -EINVAL;
	}
}

static int p3t1755_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct p3t1755_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		// Toggle shutdown bit to enable/disable sensor
		ret = regmap_update_bits(data->regmap, P3T1755_REG_CFGR,
					 P3T1755_SHUTDOWN_BIT,
					 val == 0 ? P3T1755_SHUTDOWN_BIT : 0);
		if (ret < 0) {
			dev_err(data->dev, "Failed to update SHUTDOWN bit\n");
			return ret;
		}
		return 0;
	case IIO_CHAN_INFO_SAMP_FREQ: {
		u8 sel = 0xFF;

		// Match desired frequency with supported values
		for (int i = 0; i < ARRAY_SIZE(p3t1755_samp_freqs); i++) {
			if (p3t1755_samp_freqs[i].freq_hz == val) {
				sel = i;
				break;
			}
		}
		if (sel == 0xFF)
			return -EINVAL;

		// Update conversion rate bits R1:R0 in CFGR register
		return regmap_update_bits(data->regmap, P3T1755_REG_CFGR,
					  P3T1755_R0_BIT | P3T1755_R1_BIT,
					  sel << 5);
	}

	default:
		return -EINVAL;
	}
}

static int p3t1755_read_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info, int *val,
				    int *val2)
{
	struct p3t1755_data *data = iio_priv(indio_dev);
	unsigned int reg;
	u8 buf[2];
	int ret;

	if (type != IIO_EV_TYPE_THRESH || info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	/* Select high or low limit register based on direction */
	reg = (dir == IIO_EV_DIR_RISING) ? P3T1755_REG_HIGH_LIM :
					   P3T1755_REG_LOW_LIM;

	/* Convert raw register value to temperature in milli°C */
	ret = regmap_bulk_read(data->regmap, reg, buf, 2);
	if (ret < 0) {
		dev_err(data->dev, "Failed to read Thigh or Tlow register\n");
		return ret;
	}
	*val = (((buf[0] << 8) | buf[1]) >> 4) * P3T1755_RESOLUTION_10UC / 1000;

	return IIO_VAL_INT;
}

static int p3t1755_write_event_value(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info, int val,
				     int val2)
{
	struct p3t1755_data *data = iio_priv(indio_dev);
	unsigned int reg;
	u8 buf[2];
	int regval;

	if (type != IIO_EV_TYPE_THRESH || info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	/* Select high or low limit register based on direction */
	reg = (dir == IIO_EV_DIR_RISING) ? P3T1755_REG_HIGH_LIM :
					   P3T1755_REG_LOW_LIM;

	/* Convert temperature in milli°C to register format */
	regval = DIV_ROUND_CLOSEST(val * 1000, P3T1755_RESOLUTION_10UC) << 4;
	buf[0] = regval >> 8;
	buf[1] = regval & 0xff;

	return regmap_bulk_write(data->regmap, reg, buf, 2);
}

static int p3t1755_trigger_one_shot(struct p3t1755_data *data)
{
	unsigned int config;
	int ret;

	ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &config);
	if (ret < 0) {
		dev_err(data->dev, "Failed to read configuration register\n");
		return ret;
	}

	/* One-shot mode is only allowed when the device is in shutdown mode */
	if (!(config & P3T1755_SHUTDOWN_BIT))
		return -EBUSY;
	/* Set the one-shot bit to trigger a single temperature conversion */
	config |= P3T1755_ONE_SHOT_BIT;

	return regmap_write(data->regmap, P3T1755_REG_CFGR, config);
}

static ssize_t p3t1755_attr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct iio_dev_attr *iattr = to_iio_dev_attr(attr);
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct p3t1755_data *data = iio_priv(indio_dev);
	unsigned int val;
	int ret;

	switch (iattr->address) {
	case P3T1755_ATTR_THERMOSTAT_MODE:
		/* Read TM bit from configuration register */
		ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &val);
		if (ret < 0) {
			dev_err(data->dev, "Failed to read configuration register\n");
			return ret;
		}
		return sysfs_emit(buf, "%d\n", !!(val & P3T1755_TM_BIT));
	case P3T1755_ATTR_FAULT_QUEUE_LENGTH:
		/* Read fault queue length bits and map to user-visible value */
		ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &val);
		if (ret < 0) {
			dev_err(data->dev, "Failed to read configuration register\n");
			return ret;
		}
		val = (val & P3T1755_FAULT_QUEUE_MASK) >> P3T1755_FAULT_QUEUE_SHIFT;
		if (val > 3)
			return -EINVAL;
		return sysfs_emit(buf, "%d\n", p3t1755_fault_queue_values[val]);
	default:
		return -EINVAL;
	}
}

static ssize_t p3t1755_attr_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct iio_dev_attr *iattr = to_iio_dev_attr(attr);
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct p3t1755_data *data = iio_priv(indio_dev);
	int ret, val;
	bool enable;

	switch (iattr->address) {
	case P3T1755_ATTR_TRIGGER_ONE_SHOT:
		/* Trigger a one-shot conversion if input is '1' */
		ret = kstrtobool(buf, &enable);
		if (ret || !enable)
			return ret ? ret : -EINVAL;
		ret = p3t1755_trigger_one_shot(data);
		return ret ?: count;
	case P3T1755_ATTR_FAULT_QUEUE_LENGTH:
		/* Set fault queue length if input matches supported values */
		ret = kstrtoint(buf, 10, &val);
		if (ret)
			return ret;
		for (int i = 0; i < ARRAY_SIZE(p3t1755_fault_queue_values); i++) {
			if (val == p3t1755_fault_queue_values[i]) {
				ret = regmap_update_bits(data->regmap, P3T1755_REG_CFGR,
							 P3T1755_FAULT_QUEUE_MASK,
							 i << P3T1755_FAULT_QUEUE_SHIFT);
				return ret ?: count;
			}
		}
		return -EINVAL;
	default:
		return -EINVAL;
		}
	}

static IIO_DEVICE_ATTR(thermostat_mode, 0444, p3t1755_attr_show,
		NULL, P3T1755_ATTR_THERMOSTAT_MODE);

static IIO_DEVICE_ATTR(trigger_one_shot, 0200, NULL, p3t1755_attr_store,
		P3T1755_ATTR_TRIGGER_ONE_SHOT);

static IIO_DEVICE_ATTR(fault_queue_length, 0664, p3t1755_attr_show,
		p3t1755_attr_store, P3T1755_ATTR_FAULT_QUEUE_LENGTH);

static const struct iio_event_spec p3t1755_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE)
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE)
	},
};

static const struct iio_chan_spec p3t175x_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_PROCESSED) |
				      BIT(IIO_CHAN_INFO_ENABLE) |
				      BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.event_spec = p3t1755_events,
		.num_event_specs = ARRAY_SIZE(p3t1755_events),
	},
};

const struct p3t17xx_info p3t1755_channels_info = {
	.name = "p3t1755",
	.channels = p3t175x_channels,
	.num_channels = ARRAY_SIZE(p3t175x_channels)
};
EXPORT_SYMBOL_NS(p3t1755_channels_info, IIO_P3T1755);

const struct p3t17xx_info p3t1750_channels_info = {
	.name = "p3t1750",
	.channels = p3t175x_channels,
	.num_channels = ARRAY_SIZE(p3t175x_channels)
};
EXPORT_SYMBOL_NS(p3t1750_channels_info, IIO_P3T1755);

const struct p3t17xx_info p3t175x_channels_info = {
	.name = "p3t175x",
	.channels = p3t175x_channels,
	.num_channels = ARRAY_SIZE(p3t175x_channels)
};
EXPORT_SYMBOL_NS(p3t175x_channels_info, IIO_P3T1755);

static struct attribute *p3t1755_attributes[] = {
	&iio_dev_attr_thermostat_mode.dev_attr.attr,
	&iio_dev_attr_trigger_one_shot.dev_attr.attr,
	&iio_dev_attr_fault_queue_length.dev_attr.attr,
	NULL,
};

static const struct attribute_group p3t1755_attr_group = {
	.attrs = p3t1755_attributes,
};

static const struct iio_info p3t1755_info = {
	.read_raw = p3t1755_read_raw,
	.write_raw = p3t1755_write_raw,
	.read_event_value = p3t1755_read_event_value,
	.write_event_value = p3t1755_write_event_value,
	.attrs = &p3t1755_attr_group,
};

int p3t1755_probe(struct device *dev, const struct p3t17xx_info *chip,
		  struct regmap *regmap, bool tm_mode, bool alert_active_high, int fq_bits)
{
	struct p3t1755_data *data;
	struct iio_dev *iio_dev;
	int ret;

	iio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!iio_dev)
		return -ENOMEM;

	data = iio_priv(iio_dev);
	data->dev = dev;
	data->regmap = regmap;
	data->tm_mode = tm_mode;
	data->alert_active_high = alert_active_high;

	iio_dev->name = chip->name;
	iio_dev->modes = INDIO_DIRECT_MODE;
	iio_dev->info = &p3t1755_info;
	iio_dev->channels = chip->channels;
	iio_dev->num_channels = chip->num_channels;

	dev_set_drvdata(dev, iio_dev);

	/* Configure thermostat mode and alert polarity and (optional) fault queue */
	ret = regmap_update_bits(data->regmap, P3T1755_REG_CFGR,
				 P3T1755_TM_BIT | P3T1755_POL_BIT,
				(tm_mode ? P3T1755_TM_BIT : 0) |
				(alert_active_high ? P3T1755_POL_BIT : 0));
	if (ret) {
		dev_err_probe(data->dev, ret, "Failed to update TM and POL bits\n");
		return ret;
	}

	if (fq_bits >= 0) /* -1 means "leave reset default" */
		regmap_update_bits(data->regmap, P3T1755_REG_CFGR, P3T1755_FAULT_QUEUE_MASK,
				   fq_bits << P3T1755_FAULT_QUEUE_SHIFT);

	ret = devm_iio_device_register(dev, iio_dev);
	if (ret)
		dev_info(dev, "Temperature sensor failed to register: %d\n", ret);
	else
		dev_info(dev, "Temperature sensor registered successfully\n");

	return ret;
}
EXPORT_SYMBOL_NS(p3t1755_probe, IIO_P3T1755);

MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("NXP P3T175x Driver");
MODULE_LICENSE("GPL");
