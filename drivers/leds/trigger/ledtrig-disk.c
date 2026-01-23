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

static struct led_trigger ledtrig_disk = {
	.name = "disk-activity",
	.activate = ledtrig_disk_activate,
	.groups = ledtrig_disk_groups,
};
static struct led_trigger ledtrig_disk_read = {
	.name = "disk-read",
	.activate = ledtrig_disk_activate,
	.groups = ledtrig_disk_groups,
};
static struct led_trigger ledtrig_disk_write = {
	.name = "disk-write",
	.activate = ledtrig_disk_activate,
	.groups = ledtrig_disk_groups,
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

void ledtrig_disk_activity(bool write)
{
	ledtrig_disk_blink_oneshot(&ledtrig_disk);
	if (write)
		ledtrig_disk_blink_oneshot(&ledtrig_disk_write);
	else
		ledtrig_disk_blink_oneshot(&ledtrig_disk_read);
}
EXPORT_SYMBOL(ledtrig_disk_activity);

static int __init ledtrig_disk_init(void)
{
	led_trigger_register(&ledtrig_disk);
	led_trigger_register(&ledtrig_disk_read);
	led_trigger_register(&ledtrig_disk_write);

	return 0;
}
device_initcall(ledtrig_disk_init);
