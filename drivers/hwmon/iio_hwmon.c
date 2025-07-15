// SPDX-License-Identifier: GPL-2.0-only
/* Hwmon client for industrial I/O devices
 *
 * Copyright (c) 2011 Jonathan Cameron
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/iio/consumer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <uapi/linux/iio/events.h>

/* Protects iio_hwmon_listeners and listeners' refcnt */
DEFINE_MUTEX(iio_hwmon_listener_lock);
LIST_HEAD(iio_hwmon_listeners);

/**
 * struct iio_hwmon_listener - Listener for IIO events
 * @block: Notifier for events
 * @ids: Array of IIO event ids, one per alarm
 * @alarms: Bitmap of alarms
 * @num_alarms: Length of @ids and @alarms
 * @indio_dev: Device we are listening to
 * @list: List of all listeners
 * @refcnt: Reference count
 */
struct iio_hwmon_listener {
	struct notifier_block block;
	u64 *ids;
	unsigned long *alarms;
	size_t num_alarms;

	struct iio_dev *indio_dev;
	struct list_head list;
	unsigned int refcnt;
};

/**
 * iio_hwmon_lookup_alarm() - Find an alarm by id
 * @listener: Event listener
 * @id: IIO event id
 *
 * Return: index of @id in @listener->ids, or -1 if not found
 */
static ssize_t iio_hwmon_lookup_alarm(struct iio_hwmon_listener *listener,
				      u64 id)
{
	ssize_t i;

	for (i = 0; i < listener->num_alarms; i++)
		if (listener->ids[i] == id)
			return i;

	return -1;
}

static int iio_hwmon_listener_callback(struct notifier_block *block,
				       unsigned long action, void *data)
{
	struct iio_hwmon_listener *listener =
		container_of(block, struct iio_hwmon_listener, block);
	struct iio_event_data *ev = data;
	ssize_t i;

	if (action != IIO_NOTIFY_EVENT)
		return NOTIFY_DONE;

	i = iio_hwmon_lookup_alarm(listener, ev->id);
	if (i >= 0)
		set_bit(i, listener->alarms);
	else
		dev_warn_once(&listener->indio_dev->dev,
			      "unknown event %016llx\n", ev->id);

	return NOTIFY_DONE;
}

/**
 * iio_event_id() - Calculate an IIO event id
 * @channel: IIO channel for this event
 * @type: Event type (theshold, rate-of-change, etc.)
 * @dir: Event direction (rising, falling, etc.)
 *
 * Return: IIO event id corresponding to this event's IIO id
 */
static u64 iio_event_id(struct iio_chan_spec const *chan,
			enum iio_event_type type,
			enum iio_event_direction dir)
{
	if (chan->differential)
		return IIO_DIFF_EVENT_CODE(chan->type, chan->channel,
					   chan->channel2, type, dir);
	if (chan->modified)
		return IIO_MOD_EVENT_CODE(chan->type, chan->channel,
					  chan->channel2, type, dir);
	return IIO_UNMOD_EVENT_CODE(chan->type, chan->channel, type, dir);
}

/**
 * iio_hwmon_listener_get() - Get a listener for an IIO device
 * @indio_dev: IIO device to listen to
 *
 * Look up or create a new listener for @indio_dev. The returned listener is
 * registered with @indio_dev, but events still need to be manually enabled.
 * You must call iio_hwmon_listener_put() when you are done.
 *
 * Return: Listener for @indio_dev, or an error pointer
 */
static struct iio_hwmon_listener *iio_hwmon_listener_get(struct iio_dev *indio_dev)
{
	struct iio_hwmon_listener *listener;
	int err = -ENOMEM;
	size_t i, j;

	guard(mutex)(&iio_hwmon_listener_lock);
	list_for_each_entry(listener, &iio_hwmon_listeners, list) {
		if (listener->indio_dev == indio_dev) {
			if (likely(listener->refcnt != UINT_MAX))
				listener->refcnt++;
			return listener;
		}
	}

	listener = kzalloc(sizeof(*listener), GFP_KERNEL);
	if (!listener)
		goto err_unlock;

	listener->refcnt = 1;
	listener->indio_dev = indio_dev;
	listener->block.notifier_call = iio_hwmon_listener_callback;
	for (i = 0; i < indio_dev->num_channels; i++)
		listener->num_alarms += indio_dev->channels[i].num_event_specs;

	listener->ids = kcalloc(listener->num_alarms, sizeof(*listener->ids),
				GFP_KERNEL);
	listener->alarms = bitmap_zalloc(listener->num_alarms, GFP_KERNEL);
	if (!listener->ids || !listener->alarms)
		goto err_listener;

	i = 0;
	for (j = 0; j < indio_dev->num_channels; j++) {
		struct iio_chan_spec const *chan = &indio_dev->channels[j];
		size_t k;

		for (k = 0; k < chan->num_event_specs; k++)
			listener->ids[i++] =
				iio_event_id(chan, chan->event_spec[k].type,
					     chan->event_spec[k].dir);
	}

	err = iio_event_register(indio_dev, &listener->block);
	if (err)
		goto err_alarms;

	list_add(&listener->list, &iio_hwmon_listeners);
	mutex_unlock(&iio_hwmon_listener_lock);
	return listener;

err_alarms:
	kfree(listener->alarms);
	kfree(listener->ids);
err_listener:
	kfree(listener);
err_unlock:
	mutex_unlock(&iio_hwmon_listener_lock);
	return ERR_PTR(err);
}

/**
 * iio_hwmon_listener_put() - Release a listener
 * @data: &struct iio_hwmon_listener to release
 *
 * For convenience, @data is void.
 */
static void iio_hwmon_listener_put(void *data)
{
	struct iio_hwmon_listener *listener = data;

	scoped_guard(mutex, &iio_hwmon_listener_lock) {
		if (unlikely(listener->refcnt == UINT_MAX))
			return;

		if (--listener->refcnt)
			return;

		list_del(&listener->list);
		iio_event_unregister(listener->indio_dev, &listener->block);
	}

	kfree(listener->alarms);
	kfree(listener->ids);
	kfree(listener);
}

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

/**
 * struct iio_hwmon_alarm_attribute - IIO HWMON alarm attribute
 * @dev_attr: Base device attribute
 * @listener: Listener for this alarm
 * @index: Index of the channel in the IIO HWMON
 * @alarm: Index of the alarm within @listener
 */
struct iio_hwmon_alarm_attribute {
	struct device_attribute dev_attr;
	struct iio_hwmon_listener *listener;
	size_t index;
	size_t alarm;
};
#define to_alarm_attr(_dev_attr) \
	container_of(_dev_attr, struct iio_hwmon_alarm_attribute, dev_attr)

/**
 * iio_hwmon_alarm_toggle() - Turn an event off and back on again
 * @chan: Channel of the event
 * @dir: Event direction (rising, falling, etc.)
 *
 * Toggle an event's enable so we get notified if the alarm is already
 * triggered. We use this to convert IIO's event-triggered events into
 * level-triggered alarms.
 *
 * Return: 0 on success or negative error on failure
 */
static int iio_hwmon_alarm_toggle(struct iio_channel *chan,
				  enum iio_event_direction dir)
{
	int ret;

	ret = iio_write_event_processed_scale(chan, IIO_EV_TYPE_THRESH, dir,
					      IIO_EV_INFO_ENABLE, 0, 1);
	if (ret)
		return ret;

	return iio_write_event_processed_scale(chan, IIO_EV_TYPE_THRESH, dir,
					       IIO_EV_INFO_ENABLE, 1, 1);
}

static ssize_t iio_hwmon_read_alarm(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct iio_hwmon_alarm_attribute *sattr = to_alarm_attr(attr);
	struct iio_hwmon_state *state = dev_get_drvdata(dev);
	struct iio_channel *chan = &state->channels[sattr->index];

	if (test_and_clear_bit(sattr->alarm, sattr->listener->alarms)) {
		u64 id = sattr->listener->ids[sattr->alarm];
		enum iio_event_direction dir = IIO_EVENT_CODE_EXTRACT_DIR(id);

		WARN_ON(iio_hwmon_alarm_toggle(chan, dir));
		strcpy(buf, "1\n");
		return 2;
	}

	strcpy(buf, "0\n");
	return 2;
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

static int add_alarm_attr(struct device *dev, struct iio_hwmon_state *st,
			  int i, enum iio_event_direction dir,
			  const char *fmt, ...)
{
	struct iio_hwmon_alarm_attribute *a;
	struct iio_hwmon_listener *listener;
	ssize_t alarm;
	umode_t mode;
	va_list ap;
	int ret;

	mode = iio_event_mode(&st->channels[i], IIO_EV_TYPE_THRESH, dir,
			      IIO_EV_INFO_ENABLE);
	if (!(mode & 0200))
		return 0;

	listener = iio_hwmon_listener_get(st->channels[i].indio_dev);
	if (listener == ERR_PTR(-EBUSY))
		return 0;
	if (IS_ERR(listener))
		return PTR_ERR(listener);

	ret = devm_add_action_or_reset(dev, iio_hwmon_listener_put, listener);
	if (ret)
		return ret;

	alarm = iio_hwmon_lookup_alarm(listener,
				       iio_event_id(st->channels[i].channel,
						    IIO_EV_TYPE_THRESH, dir));
	if (WARN_ON_ONCE(alarm < 0))
		return -ENOENT;

	ret = iio_hwmon_alarm_toggle(&st->channels[i], dir);
	if (ret)
		return ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	sysfs_attr_init(&a->dev_attr.attr);
	va_start(ap, fmt);
	a->dev_attr.attr.name = devm_kvasprintf(dev, GFP_KERNEL, fmt, ap);
	va_end(ap);
	if (!a->dev_attr.attr.name)
		return -ENOMEM;

	a->dev_attr.show = iio_hwmon_read_alarm;
	a->dev_attr.attr.mode = 0400;
	a->listener = listener;
	a->index = i;
	a->alarm = alarm;

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
				 7 * st->num_channels + 1, sizeof(*st->attrs),
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

		ret = add_alarm_attr(dev, st, i, IIO_EV_DIR_RISING,
				     "%s%d_max_alarm", prefix, n);
		if (ret)
			return ret;

		ret = add_alarm_attr(dev, st, i, IIO_EV_DIR_FALLING,
				     "%s%d_min_alarm", prefix, n);
		if (ret)
			return ret;

		ret = add_alarm_attr(dev, st, i, IIO_EV_DIR_EITHER,
				     "%s%d_alarm", prefix, n);
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
