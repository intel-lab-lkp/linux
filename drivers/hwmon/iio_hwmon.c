// SPDX-License-Identifier: GPL-2.0-only
/* Hwmon client for industrial I/O devices
 *
 * Copyright (c) 2011 Jonathan Cameron
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>

/**
 * struct iio_hwmon_state - device instance state
 * @channels:		filled with array of channels from iio
 * @num_channels:	number of channels in channels (saves counting twice)
 * @attr_group:		the group of attributes
 * @groups:		null terminated array of attribute groups
 * @attrs:		null terminated array of attribute pointers.
 * @num_attrs:          length of @attrs
 */
struct iio_hwmon_state {
	struct iio_channel *channels;
	struct attribute_group attr_group;
	const struct attribute_group *groups[2];
	struct attribute **attrs;
	size_t num_attrs;
	int num_channels;
};

static ssize_t iio_hwmon_read_label(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	struct iio_hwmon_state *state = dev_get_drvdata(dev);
	struct iio_channel *chan = &state->channels[sattr->index];

	return iio_read_channel_label(chan, buf);
}

/**
 * iio_hwmon_scale() - Look up the scaling factor for a channel
 * @chan: Channel to get the scale of
 *
 * Determine the scale to use with @chan in the case where IIO and HWMON use
 * different units.
 *
 * Return: scale of @chan
 */
static int iio_hwmon_scale(struct iio_channel *chan)
{
	enum iio_chan_type type;
	int ret;

	ret = iio_get_channel_type(chan, &type);
	if (ret < 0)
		return ret;

	/* mili-Watts to micro-Watts conversion */
	if (type == IIO_POWER)
		return 1000;
	return 1;
}

/*
 * Assumes that IIO and hwmon operate in the same base units.
 * This is supposed to be true, but needs verification for
 * new channel types.
 */
static ssize_t iio_hwmon_read_val(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	struct iio_hwmon_state *state = dev_get_drvdata(dev);
	struct iio_channel *chan = &state->channels[sattr->index];
	int ret, result, scale;

	scale = iio_hwmon_scale(chan);
	if (scale < 0)
		return scale;

	ret = iio_read_channel_processed_scale(chan, &result, scale);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%d\n", result);
}

static ssize_t iio_hwmon_read_event(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct iio_hwmon_state *state = dev_get_drvdata(dev);
	struct iio_channel *chan = &state->channels[sattr->index];
	int ret, result, scale;

	scale = iio_hwmon_scale(chan);
	if (scale < 0)
		return scale;

	ret = iio_read_event_processed_scale(chan, IIO_EV_TYPE_THRESH,
					     sattr->nr, IIO_EV_INFO_VALUE,
					     &result, scale);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%d\n", result);
}

static ssize_t iio_hwmon_write_event(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	struct iio_hwmon_state *state = dev_get_drvdata(dev);
	struct iio_channel *chan = &state->channels[sattr->index];
	int ret, scale, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	scale = iio_hwmon_scale(chan);
	if (scale < 0)
		return scale;

	ret = iio_write_event_processed_scale(chan, IIO_EV_TYPE_THRESH,
					      sattr->nr, IIO_EV_INFO_VALUE,
					      val, scale);
	if (ret < 0)
		return ret;

	return count;
}

static int add_device_attr(struct device *dev, struct iio_hwmon_state *st,
			   ssize_t (*show)(struct device *dev,
					   struct device_attribute *attr,
					   char *buf),
			   int i, const char *fmt, ...)
{
	struct sensor_device_attribute *a;
	va_list ap;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	sysfs_attr_init(&a->dev_attr.attr);
	va_start(ap, fmt);
	a->dev_attr.attr.name = devm_kvasprintf(dev, GFP_KERNEL, fmt, ap);
	va_end(ap);
	if (!a->dev_attr.attr.name)
		return -ENOMEM;

	a->dev_attr.show = show;
	a->dev_attr.attr.mode = 0444;
	a->index = i;

	st->attrs[st->num_attrs++] = &a->dev_attr.attr;
	return 0;
}

static int add_event_attr(struct device *dev, struct iio_hwmon_state *st,
			  int i, enum iio_event_direction dir,
			  const char *fmt, ...)
{
	struct sensor_device_attribute_2 *a;
	umode_t mode;
	va_list ap;

	mode = iio_event_mode(&st->channels[i], IIO_EV_TYPE_THRESH, dir,
			      IIO_EV_INFO_VALUE);
	if (!mode)
		return 0;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	sysfs_attr_init(&a->dev_attr.attr);
	va_start(ap, fmt);
	a->dev_attr.attr.name = devm_kvasprintf(dev, GFP_KERNEL, fmt, ap);
	va_end(ap);
	if (!a->dev_attr.attr.name)
		return -ENOMEM;

	a->dev_attr.show = iio_hwmon_read_event;
	a->dev_attr.store = iio_hwmon_write_event;
	a->dev_attr.attr.mode = mode;
	a->index = i;
	a->nr = dir;

	st->attrs[st->num_attrs++] = &a->dev_attr.attr;
	return 0;
}

static int iio_hwmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_hwmon_state *st;
	int ret, i;
	int in_i = 1, temp_i = 1, curr_i = 1, humidity_i = 1, power_i = 1;
	enum iio_chan_type type;
	struct iio_channel *channels;
	struct device *hwmon_dev;
	char *sname;
	void *buf;

	channels = devm_iio_channel_get_all(dev);
	if (IS_ERR(channels)) {
		ret = PTR_ERR(channels);
		if (ret == -ENODEV)
			ret = -EPROBE_DEFER;
		return dev_err_probe(dev, ret,
				     "Failed to get channels\n");
	}

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	buf = (void *)devm_get_free_pages(dev, GFP_KERNEL, 0);
	if (!st || !buf)
		return -ENOMEM;

	st->channels = channels;

	/* count how many channels we have */
	while (st->channels[st->num_channels].indio_dev)
		st->num_channels++;

	st->attrs = devm_kcalloc(dev,
				 4 * st->num_channels + 1, sizeof(*st->attrs),
				 GFP_KERNEL);
	if (st->attrs == NULL)
		return -ENOMEM;

	for (i = 0; i < st->num_channels; i++) {
		const char *prefix;
		int n;

		ret = iio_get_channel_type(&st->channels[i], &type);
		if (ret < 0)
			return ret;

		switch (type) {
		case IIO_VOLTAGE:
			n = in_i++;
			prefix = "in";
			break;
		case IIO_TEMP:
			n = temp_i++;
			prefix = "temp";
			break;
		case IIO_CURRENT:
			n = curr_i++;
			prefix = "curr";
			break;
		case IIO_POWER:
			n = power_i++;
			prefix = "power";
			break;
		case IIO_HUMIDITYRELATIVE:
			n = humidity_i++;
			prefix = "humidity";
			break;
		default:
			return -EINVAL;
		}

		ret = add_device_attr(dev, st, iio_hwmon_read_val, i,
				      "%s%d_input", prefix, n);
		if (ret)
			return ret;

		/* Let's see if we have a label... */
		if (iio_read_channel_label(&st->channels[i], buf) >= 0) {
			ret = add_device_attr(dev, st, iio_hwmon_read_label,
					      i, "%s%d_label", prefix, n);
			if (ret)
				return ret;
		}

		ret = add_event_attr(dev, st, i, IIO_EV_DIR_FALLING,
				     "%s%d_min", prefix, n);
		if (ret)
			return ret;

		ret = add_event_attr(dev, st, i, IIO_EV_DIR_RISING,
				     "%s%d_max", prefix, n);
		if (ret)
			return ret;
	}

	devm_free_pages(dev, (unsigned long)buf);

	st->attr_group.attrs = st->attrs;
	st->groups[0] = &st->attr_group;

	if (dev_fwnode(dev)) {
		sname = devm_kasprintf(dev, GFP_KERNEL, "%pfwP", dev_fwnode(dev));
		if (!sname)
			return -ENOMEM;
		strreplace(sname, '-', '_');
	} else {
		sname = "iio_hwmon";
	}

	hwmon_dev = devm_hwmon_device_register_with_groups(dev, sname, st,
							   st->groups);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id iio_hwmon_of_match[] = {
	{ .compatible = "iio-hwmon", },
	{ }
};
MODULE_DEVICE_TABLE(of, iio_hwmon_of_match);

static struct platform_driver iio_hwmon_driver = {
	.driver = {
		.name = "iio_hwmon",
		.of_match_table = iio_hwmon_of_match,
	},
	.probe = iio_hwmon_probe,
};

module_platform_driver(iio_hwmon_driver);

MODULE_AUTHOR("Jonathan Cameron <jic23@kernel.org>");
MODULE_DESCRIPTION("IIO to hwmon driver");
MODULE_LICENSE("GPL v2");
