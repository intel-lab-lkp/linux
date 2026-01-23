// SPDX-License-Identifier: GPL-2.0-only
/*
 * LED Disk Activity Trigger
 *
 * Copyright 2006 Openedhand Ltd.
 *
 * Author: Richard Purdie <rpurdie@openedhand.com>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/leds.h>
#include "../leds.h"

#define DEFAULT_BLINK_DELAY 30

struct ledtrig_disk_data {
	unsigned long delay_on;
	unsigned long delay_off;
	unsigned int invert;
};

static ssize_t led_delay_on_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = led_trigger_get_led(dev);
	struct ledtrig_disk_data *disk_data = led_get_trigger_data(led_cdev);

	return sprintf(buf, "%lu\n", disk_data->delay_on);
}

static ssize_t led_delay_on_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = led_trigger_get_led(dev);
	struct ledtrig_disk_data *disk_data = led_get_trigger_data(led_cdev);
	unsigned long state;
	ssize_t ret;

	ret = kstrtoul(buf, 10, &state);
	if (ret)
		return ret;

	disk_data->delay_on = state;

	return size;
}

static ssize_t led_delay_off_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = led_trigger_get_led(dev);
	struct ledtrig_disk_data *disk_data = led_get_trigger_data(led_cdev);

	return sprintf(buf, "%lu\n", disk_data->delay_off);
}

static ssize_t led_delay_off_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = led_trigger_get_led(dev);
	struct ledtrig_disk_data *disk_data = led_get_trigger_data(led_cdev);
	unsigned long state;
	ssize_t ret;

	ret = kstrtoul(buf, 10, &state);
	if (ret)
		return ret;

	disk_data->delay_off = state;

	return size;
}

static ssize_t led_invert_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ledtrig_disk_data *disk_data =
		led_trigger_get_drvdata(dev);

	return sprintf(buf, "%u\n", disk_data->invert);
}

static ssize_t led_invert_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = led_trigger_get_led(dev);
	struct ledtrig_disk_data *disk_data = led_get_trigger_data(led_cdev);
	unsigned long state;
	int ret;

	ret = kstrtoul(buf, 0, &state);
	if (ret)
		return ret;

	led_set_brightness_nosleep(led_cdev, state ? LED_FULL : LED_OFF);
	disk_data->invert = !!state;

	return size;
}

static DEVICE_ATTR(delay_on, 0644, led_delay_on_show, led_delay_on_store);
static DEVICE_ATTR(delay_off, 0644, led_delay_off_show, led_delay_off_store);
static DEVICE_ATTR(invert, 0644, led_invert_show, led_invert_store);

static struct attribute *ledtrig_disk_attrs[] = {
	&dev_attr_delay_on.attr,
	&dev_attr_delay_off.attr,
	&dev_attr_invert.attr,
	NULL
};
ATTRIBUTE_GROUPS(ledtrig_disk);

static void pattern_init(struct led_classdev *led_cdev, struct ledtrig_disk_data *disk_data)
{
	unsigned int size = 0;

	u32 *pattern __free(kfree) = led_get_default_pattern(led_cdev, &size);
	if (!pattern)
		return;

	if (size != 3) {
		dev_warn(led_cdev->dev,
			 "Expected 3 but got %u values for delays + invert pattern\n",
			 size);
		return;
	}

	disk_data->delay_on = pattern[0];
	disk_data->delay_off = pattern[1];
	disk_data->invert = !!pattern[2];
}

static int ledtrig_disk_activate(struct led_classdev *led_cdev)
{
	struct ledtrig_disk_data *disk_data;

	disk_data = kzalloc(sizeof(*disk_data), GFP_KERNEL);
	if (!disk_data)
		return -ENOMEM;

	disk_data->delay_on = DEFAULT_BLINK_DELAY;
	disk_data->delay_off = DEFAULT_BLINK_DELAY;

	led_set_trigger_data(led_cdev, disk_data);

	if (led_cdev->flags & LED_INIT_DEFAULT_TRIGGER) {
		pattern_init(led_cdev, disk_data);
		/*
		 * Mark as initialized even on pattern_init() error because
		 * any consecutive call to it would produce the same error.
		 */
		led_cdev->flags &= ~LED_INIT_DEFAULT_TRIGGER;
	}

	led_set_brightness_nosleep(led_cdev, disk_data->invert ? LED_FULL : LED_OFF);

	return 0;
}

static struct ledtrig_disk_trigger ledtrig_disk = {
	.all = {
		.name = "disk-activity",
		.activate = ledtrig_disk_activate,
		.groups = ledtrig_disk_groups,
	},
	.read = {
		.name = "disk-read",
		.activate = ledtrig_disk_activate,
		.groups = ledtrig_disk_groups,
	},
	.write = {
		.name = "disk-write",
		.activate = ledtrig_disk_activate,
		.groups = ledtrig_disk_groups,
	},
};

static void ledtrig_disk_blink_oneshot(struct led_trigger *trig)
{
	struct led_classdev *led_cdev;
	struct ledtrig_disk_data *disk_data;

	rcu_read_lock();
	list_for_each_entry_rcu(led_cdev, &trig->led_cdevs, trig_list) {
		disk_data = led_get_trigger_data(led_cdev);
		led_blink_set_oneshot(led_cdev, &disk_data->delay_on, &disk_data->delay_off,
				      disk_data->invert);
	}
	rcu_read_unlock();
}

static void ledtrig_disk_trigger_activity(struct ledtrig_disk_trigger *trig, bool write)
{
	if (IS_ERR_OR_NULL(trig))
		return;
	ledtrig_disk_blink_oneshot(&trig->all);
	if (write)
		ledtrig_disk_blink_oneshot(&trig->write);
	else
		ledtrig_disk_blink_oneshot(&trig->read);
}

void ledtrig_disk_activity(struct ledtrig_disk_trigger *port, bool write)
{
	ledtrig_disk_trigger_activity(&ledtrig_disk, write);
	ledtrig_disk_trigger_activity(port, write);
}
EXPORT_SYMBOL(ledtrig_disk_activity);

struct ledtrig_disk_trigger *ledtrig_disk_trigger_register(const char *name)
{
	struct ledtrig_disk_trigger *trigger = kzalloc(sizeof(*trigger), GFP_KERNEL);
	int ret, n;

	if (!trigger)
		return ERR_PTR(-ENOMEM);

	trigger->all.name = kzalloc(TRIG_NAME_MAX, GFP_KERNEL);
	if (!trigger->all.name) {
		ret = -ENOMEM;
		goto err1;
	}

	n = snprintf((char *)trigger->all.name, TRIG_NAME_MAX, "%s-disk-activity", name);
	if (n >= TRIG_NAME_MAX) {
		ret = -E2BIG;
		goto err1;
	}

	trigger->all.activate = ledtrig_disk_activate;
	trigger->all.groups = ledtrig_disk_groups;

	ret = led_trigger_register(&trigger->all);
	if (ret)
		goto err1;

	trigger->read.name = kzalloc(TRIG_NAME_MAX, GFP_KERNEL);
	if (!trigger->read.name) {
		ret = -ENOMEM;
		goto err2;
	}

	n = snprintf((char *)trigger->read.name, TRIG_NAME_MAX, "%s-disk-read", name);
	if (n >= TRIG_NAME_MAX) {
		ret = -E2BIG;
		goto err2;
	}

	trigger->read.activate = ledtrig_disk_activate;
	trigger->read.groups = ledtrig_disk_groups;

	ret = led_trigger_register(&trigger->read);
	if (ret)
		goto err2;

	trigger->write.name = kzalloc(TRIG_NAME_MAX, GFP_KERNEL);
	if (!trigger->write.name) {
		ret = -ENOMEM;
		goto err3;
	}

	n = snprintf((char *)trigger->write.name, TRIG_NAME_MAX, "%s-disk-write", name);
	if (n >= TRIG_NAME_MAX) {
		ret = -E2BIG;
		goto err3;
	}

	trigger->write.activate = ledtrig_disk_activate;
	trigger->write.groups = ledtrig_disk_groups;

	ret = led_trigger_register(&trigger->write);
	if (ret)
		goto err3;

	return trigger;

err3:
	led_trigger_unregister(&trigger->read);
err2:
	led_trigger_unregister(&trigger->all);
err1:
	kfree(trigger->all.name);
	kfree(trigger->read.name);
	kfree(trigger->write.name);
	kfree(trigger);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL(ledtrig_disk_trigger_register);

void ledtrig_disk_trigger_unregister(struct ledtrig_disk_trigger *trig)
{
	if (IS_ERR_OR_NULL(trig))
		return;

	led_trigger_unregister(&trig->all);
	led_trigger_unregister(&trig->read);
	led_trigger_unregister(&trig->write);
}
EXPORT_SYMBOL(ledtrig_disk_trigger_unregister);

static int __init ledtrig_disk_init(void)
{
	led_trigger_register(&ledtrig_disk.all);
	led_trigger_register(&ledtrig_disk.read);
	led_trigger_register(&ledtrig_disk.write);

	return 0;
}
device_initcall(ledtrig_disk_init);
