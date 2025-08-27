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
#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/regmap.h>
#include <linux/device.h>
#include <linux/iio/events.h>

#include "p3t1755.h"

enum p3t1755_attr_index {
	P3T1755_ATTR_THERMOSTAT_MODE,
	P3T1755_ATTR_TRIGGER_ONE_SHOT,
	P3T1755_ATTR_FAULT_QUEUE_LENGTH,
};

static const struct {
	u8 bits;
	unsigned int freq_hz;
} p3t1755_samp_freqs[] = {
	{ 0x00, 36 },
	{ 0x01, 18 },
	{ 0x02, 9 },
	{ 0x03, 4 },
};

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
				int *temp_raw, int *thigh_raw, int *tlow_raw)
{
	__be16 be;
	int ret;
	int raw12;

	ret = regmap_bulk_read(data->regmap, P3T1755_REG_TEMP, &be, sizeof(be));
	if (ret) {
		dev_dbg(data->dev, "TEMP read failed: %d\n", ret);
		return ret;
	}

	raw12 = sign_extend32(be16_to_cpu(be) >> 4, 11);
	*temp_raw = raw12;

	ret = regmap_bulk_read(data->regmap, P3T1755_REG_HIGH_LIM, &be, sizeof(be));
	if (ret) {
		dev_dbg(data->dev, "HIGH_LIM read failed: %d\n", ret);
		return ret;
	}

	raw12 = sign_extend32(be16_to_cpu(be) >> 4, 11);
	*thigh_raw = raw12;

	ret = regmap_bulk_read(data->regmap, P3T1755_REG_LOW_LIM, &be, sizeof(be));
	if (ret) {
		dev_dbg(data->dev, "LOW_LIM read failed: %d\n", ret);
		return ret;
	}

	raw12 = sign_extend32(be16_to_cpu(be) >> 4, 11);
	*tlow_raw = raw12;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(p3t1755_get_temp_and_limits, IIO_P3T1755);

void p3t1755_push_thresh_event(struct iio_dev *indio_dev)
{
	struct p3t1755_data *data = iio_priv(indio_dev);
	int ret, temp, thigh, tlow;
	unsigned int cfgr;

	/* Read CFGR register to check device mode and implicitly clear the ALERT latch.
	 * As per Datasheet: "Any register read will clear the interrupt"
	 */
	ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &cfgr);
	if (ret) {
		dev_err(data->dev, "Failed to read CFGR register: %d\n", ret);
		return;
	}

	if (FIELD_GET(P3T1755_SHUTDOWN_BIT, cfgr)) {
		dev_dbg(data->dev, "Device is in shutdown mode, skipping event push\n");
		return;
	}

	ret = p3t1755_get_temp_and_limits(data, &temp, &thigh, &tlow);
	if (ret) {
		dev_err(data->dev, "Failed to get temperature and limits: %d\n", ret);
		return;
	}

	if (temp >= thigh || temp <= tlow) {
		dev_dbg(data->dev, "Threshold event: DIR_EITHER (T=%d, TH=%d, TL=%d)\n",
			temp, thigh, tlow);

		iio_push_event(indio_dev, IIO_MOD_EVENT_CODE(IIO_TEMP, 0, IIO_NO_MOD,
							     IIO_EV_TYPE_THRESH, IIO_EV_DIR_EITHER),
			       iio_get_time_ns(indio_dev));
	} else {
		dev_dbg(data->dev, "Temperature within limits: no event triggered (T=%d, TH=%d, TL=%d)\n",
			temp, thigh, tlow);
	}
}
EXPORT_SYMBOL_NS_GPL(p3t1755_push_thresh_event, IIO_P3T1755);

static int p3t1755_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *channel, int *val,
			    int *val2, long mask)
{
	struct p3t1755_data *data = iio_priv(indio_dev);
	unsigned int cfgr;
	__be16 be;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_bulk_read(data->regmap, P3T1755_REG_TEMP, &be, sizeof(be));
		if (ret < 0) {
			dev_err(data->dev, "Failed to read temperature register\n");
			return ret;
		}
		*val = sign_extend32(be16_to_cpu(be) >> 4, 11);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 625;
		*val2 = 10000;

		return IIO_VAL_FRACTIONAL;

	case IIO_CHAN_INFO_ENABLE:
		ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &cfgr);
		if (ret < 0) {
			dev_err(data->dev, "Failed to read configuration register\n");
			return ret;
		}
		*val = !FIELD_GET(P3T1755_SHUTDOWN_BIT, cfgr);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		u8 sel;

		ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &cfgr);
		if (ret < 0) {
			dev_err(data->dev, "Failed to read configuration register\n");
			return ret;
		}

		sel = FIELD_GET(P3T1755_CONVERSION_TIME_BITS, cfgr);
		if (sel >= ARRAY_SIZE(p3t1755_samp_freqs))
			return -EINVAL;

		*val = p3t1755_samp_freqs[sel].freq_hz;

		return IIO_VAL_INT;
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
		ret = regmap_update_bits(data->regmap, P3T1755_REG_CFGR,
					 P3T1755_SHUTDOWN_BIT,
					 val == 0 ? P3T1755_SHUTDOWN_BIT : 0);
		if (ret < 0) {
			dev_err(data->dev, "Failed to update SHUTDOWN bit\n");
			return ret;
		}
		return 0;
	case IIO_CHAN_INFO_SAMP_FREQ:
		u32 regbits;
		int i;

		for (i = 0; i < ARRAY_SIZE(p3t1755_samp_freqs); i++) {
			if (p3t1755_samp_freqs[i].freq_hz == val)
				break;
		}

		if (i == ARRAY_SIZE(p3t1755_samp_freqs))
			return -EINVAL;

		regbits = FIELD_PREP(P3T1755_CONVERSION_TIME_BITS, (u32)i);

		return regmap_update_bits(data->regmap, P3T1755_REG_CFGR,
					  P3T1755_CONVERSION_TIME_BITS,
					  regbits);
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
	__be16 be;
	int ret;

	if (type != IIO_EV_TYPE_THRESH || info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	reg = (dir == IIO_EV_DIR_RISING) ? P3T1755_REG_HIGH_LIM :
					   P3T1755_REG_LOW_LIM;

	ret = regmap_bulk_read(data->regmap, reg, &be, sizeof(be));
	if (ret < 0) {
		dev_err(data->dev, "Failed to read Thigh or Tlow register\n");
		return ret;
	}

	*val = sign_extend32(be16_to_cpu(be) >> 4, 11);

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
	__be16 be;

	if (type != IIO_EV_TYPE_THRESH || info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	reg = (dir == IIO_EV_DIR_RISING) ? P3T1755_REG_HIGH_LIM :
					   P3T1755_REG_LOW_LIM;

	if (val < -2048 || val > 2047)
		return -ERANGE;

	be = cpu_to_be16((u16)((val & 0xfff) << 4));

	return regmap_bulk_write(data->regmap, reg, &be, sizeof(be));
}

static int p3t1755_trigger_one_shot(struct p3t1755_data *data)
{
	unsigned int config;
	int ret;

	mutex_lock(&data->lock);

	ret = regmap_read(data->regmap, P3T1755_REG_CFGR, &config);
	if (ret)
		goto out;

	if (!(config & P3T1755_SHUTDOWN_BIT)) {
		ret = -EBUSY;
		goto out;
	}

	config |= P3T1755_ONE_SHOT_BIT;
	ret = regmap_write(data->regmap, P3T1755_REG_CFGR, config);

out:
	mutex_unlock(&data->lock);
	return ret;
}

static ssize_t p3t1755_attr_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct iio_dev_attr *iattr = to_iio_dev_attr(attr);
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct p3t1755_data *data = iio_priv(indio_dev);
	int ret;
	bool enable;

	switch (iattr->address) {
	case P3T1755_ATTR_TRIGGER_ONE_SHOT:
		ret = kstrtobool(buf, &enable);
		if (ret || !enable)
			return ret ? ret : -EINVAL;
		ret = p3t1755_trigger_one_shot(data);
		return ret ?: count;

	default:
		return -EINVAL;
		}
	}

static IIO_DEVICE_ATTR(trigger_one_shot, 0200, NULL, p3t1755_attr_store,
		       P3T1755_ATTR_TRIGGER_ONE_SHOT);

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

static const struct iio_chan_spec p3t1755_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_ENABLE) |
				      BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = p3t1755_events,
		.num_event_specs = ARRAY_SIZE(p3t1755_events),
	},
};

const struct p3t1755_info p3t1755_channels_info = {
	.name = "p3t1755",
	.channels = p3t1755_channels,
	.num_channels = ARRAY_SIZE(p3t1755_channels)
};
EXPORT_SYMBOL_NS(p3t1755_channels_info, IIO_P3T1755);

const struct p3t1755_info p3t1750_channels_info = {
	.name = "p3t1750",
	.channels = p3t1755_channels,
	.num_channels = ARRAY_SIZE(p3t1755_channels)
};
EXPORT_SYMBOL_NS(p3t1750_channels_info, IIO_P3T1755);

static struct attribute *p3t1755_attributes[] = {
	&iio_dev_attr_trigger_one_shot.dev_attr.attr,
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

static irqreturn_t p3t1755_irq_handler(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;

	dev_dbg(&indio_dev->dev, "IRQ triggered, processing threshold event\n");

	p3t1755_push_thresh_event(indio_dev);

	return IRQ_HANDLED;
}

int p3t1755_probe(struct device *dev, const struct p3t1755_info *chip,
		  struct regmap *regmap, bool tm_mode, int fq_bits, int irq)
{
	struct p3t1755_data *data;
	struct iio_dev *iio_dev;
	unsigned long irq_flags;
	int ret;

	iio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!iio_dev)
		return -ENOMEM;

	data = iio_priv(iio_dev);
	data->dev = dev;
	data->regmap = regmap;
	data->tm_mode = tm_mode;

	mutex_init(&data->lock);

	iio_dev->name = chip->name;
	iio_dev->modes = INDIO_DIRECT_MODE;
	iio_dev->info = &p3t1755_info;
	iio_dev->channels = chip->channels;
	iio_dev->num_channels = chip->num_channels;

	dev_set_drvdata(dev, iio_dev);

	ret = regmap_update_bits(data->regmap, P3T1755_REG_CFGR,
				 P3T1755_TM_BIT,
				(tm_mode ? P3T1755_TM_BIT : 0));
	if (ret)
		return dev_err_probe(data->dev, ret, "Failed to update TM bit\n");

	if (fq_bits >= 0)
		regmap_update_bits(data->regmap, P3T1755_REG_CFGR, P3T1755_FAULT_QUEUE_MASK,
				   fq_bits << P3T1755_FAULT_QUEUE_SHIFT);

	ret = devm_iio_device_register(dev, iio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Temperature sensor failed to register\n");

	if (irq > 0) {
		iio_dev = dev_get_drvdata(dev);
		data = iio_priv(iio_dev);

		if (tm_mode)
			irq_flags = IRQF_TRIGGER_FALLING;
		else
			irq_flags = (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING);

		ret = devm_request_threaded_irq(dev, irq, NULL,
						p3t1755_irq_handler, irq_flags | IRQF_ONESHOT,
						"p3t175x", iio_dev);
		if (ret)
			dev_err_probe(dev, ret, "Failed to request IRQ: %d\n", ret);
	}

	return 0;
}
EXPORT_SYMBOL_NS(p3t1755_probe, IIO_P3T1755);

MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("NXP P3T175x Driver");
MODULE_LICENSE("GPL");
