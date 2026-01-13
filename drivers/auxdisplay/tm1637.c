// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/*
 * TM1637 7-segment display driver
 *
 * Copyright (C) 2026 Siratul Islam <email@sirat.me>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/map_to_7segment.h>

/* Commands */
#define TM1637_CMD_DATA_AUTO_INC 0x40
#define TM1637_CMD_ADDR_BASE 0xC0
#define TM1637_CMD_DISPLAY_CTRL 0x80

/* Display control bits */
#define TM1637_DISPLAY_ON BIT(3)
#define TM1637_BRIGHTNESS_MASK GENMASK(2, 0)
#define TM1637_BRIGHTNESS_MAX 7

#define TM1637_SEG_DP BIT(7)

/* Protocol timing */
#define TM1637_BIT_DELAY_MIN 100
#define TM1637_BIT_DELAY_MAX 120

#define TM1637_DIGITS 4

struct tm1637 {
	struct device *dev;
	struct gpio_desc *clk;
	struct gpio_desc *dio;
	struct mutex lock; /* Protects display buffer and brightness */
	u8 brightness;
	u8 buf[TM1637_DIGITS];
};

/* Defines a static const 'initial_map' variable */
static const SEG7_DEFAULT_MAP(initial_map);

static void tm1637_delay(void)
{
	usleep_range(TM1637_BIT_DELAY_MIN, TM1637_BIT_DELAY_MAX);
}

static void tm1637_start(struct tm1637 *tm)
{
	gpiod_direction_output(tm->dio, 1);
	gpiod_set_value(tm->clk, 1);
	tm1637_delay();
	gpiod_set_value(tm->dio, 0);
	tm1637_delay();
	gpiod_set_value(tm->clk, 0);
	tm1637_delay();
}

static void tm1637_stop(struct tm1637 *tm)
{
	gpiod_direction_output(tm->dio, 0);
	gpiod_set_value(tm->clk, 1);
	tm1637_delay();
	gpiod_set_value(tm->dio, 1);
	tm1637_delay();
}

static bool tm1637_write_byte(struct tm1637 *tm, u8 data)
{
	bool ack;
	int i;

	for (i = 0; i < 8; i++) {
		gpiod_set_value(tm->clk, 0);
		tm1637_delay();

		if (data & BIT(i))
			gpiod_direction_input(tm->dio);
		else
			gpiod_direction_output(tm->dio, 0);

		tm1637_delay();
		gpiod_set_value(tm->clk, 1);
		tm1637_delay();
	}

	gpiod_set_value(tm->clk, 0);
	gpiod_direction_input(tm->dio);
	tm1637_delay();

	gpiod_set_value(tm->clk, 1);
	tm1637_delay();

	ack = !gpiod_get_value(tm->dio);

	if (!ack)
		gpiod_direction_output(tm->dio, 0);

	tm1637_delay();
	gpiod_set_value(tm->clk, 0);

	return ack;
}

static void tm1637_update_display_locked(struct tm1637 *tm)
{
	u8 ctrl_cmd;
	int i;

	tm1637_start(tm);
	tm1637_write_byte(tm, TM1637_CMD_DATA_AUTO_INC);
	tm1637_stop(tm);

	tm1637_start(tm);
	tm1637_write_byte(tm, TM1637_CMD_ADDR_BASE);
	for (i = 0; i < TM1637_DIGITS; i++)
		tm1637_write_byte(tm, tm->buf[i]);
	tm1637_stop(tm);

	if (tm->brightness == 0)
		ctrl_cmd = 0;
	else
		ctrl_cmd = TM1637_DISPLAY_ON | ((tm->brightness - 1) & TM1637_BRIGHTNESS_MASK);

	tm1637_start(tm);
	tm1637_write_byte(tm, ctrl_cmd);
	tm1637_stop(tm);
}

static void tm1637_update_display(struct tm1637 *tm)
{
	mutex_lock(&tm->lock);
	tm1637_update_display_locked(tm);
	mutex_unlock(&tm->lock);
}

static ssize_t message_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tm1637 *tm = dev_get_drvdata(dev);
	int i, pos = 0;

	mutex_lock(&tm->lock);
	for (i = 0; i < TM1637_DIGITS; i++) {
		pos += sysfs_emit_at(buf, pos, "0x%02x", tm->buf[i]);
		if (i < TM1637_DIGITS - 1)
			pos += sysfs_emit_at(buf, pos, " ");
	}
	pos += sysfs_emit_at(buf, pos, "\n");
	mutex_unlock(&tm->lock);

	return pos;
}

static ssize_t message_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct tm1637 *tm = dev_get_drvdata(dev);
	size_t i, pos = 0;
	size_t len;
	u8 segment_data[TM1637_DIGITS] = {0};

	len = count;
	if (len > 0 && buf[len - 1] == '\n')
		len--;

	for (i = 0; i < len && pos < TM1637_DIGITS; i++) {
		char c = buf[i];

		if (c == '.')
			continue;

		segment_data[pos] = map_to_seg7(&initial_map, c);

		if (i + 1 < len && buf[i + 1] == '.')
			segment_data[pos] |= TM1637_SEG_DP;

		pos++;
	}

	mutex_lock(&tm->lock);
	memcpy(tm->buf, segment_data, sizeof(tm->buf));
	tm1637_update_display_locked(tm);
	mutex_unlock(&tm->lock);

	return count;
}
static DEVICE_ATTR_RW(message);

static ssize_t brightness_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tm1637 *tm = dev_get_drvdata(dev);
	unsigned int brightness;

	mutex_lock(&tm->lock);
	brightness = tm->brightness;
	mutex_unlock(&tm->lock);

	return sysfs_emit(buf, "%u\n", brightness);
}

static ssize_t brightness_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct tm1637 *tm = dev_get_drvdata(dev);
	unsigned int brightness;
	int ret;

	ret = kstrtouint(buf, 10, &brightness);
	if (ret)
		return ret;

	if (brightness > TM1637_BRIGHTNESS_MAX + 1)
		brightness = TM1637_BRIGHTNESS_MAX + 1;

	mutex_lock(&tm->lock);
	if (tm->brightness != brightness) {
		tm->brightness = brightness;
		tm1637_update_display_locked(tm);
	}
	mutex_unlock(&tm->lock);

	return count;
}
static DEVICE_ATTR_RW(brightness);

static struct attribute *tm1637_attrs[] = {
	&dev_attr_message.attr,
	&dev_attr_brightness.attr,
	NULL};
ATTRIBUTE_GROUPS(tm1637);

static int tm1637_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tm1637 *tm;

	tm = devm_kzalloc(dev, sizeof(*tm), GFP_KERNEL);
	if (!tm)
		return -ENOMEM;

	tm->dev = dev;

	tm->clk = devm_gpiod_get(dev, "clk", GPIOD_OUT_LOW);
	if (IS_ERR(tm->clk))
		return dev_err_probe(dev, PTR_ERR(tm->clk), "Failed to get clk GPIO\n");

	tm->dio = devm_gpiod_get(dev, "dio", GPIOD_OUT_LOW);
	if (IS_ERR(tm->dio))
		return dev_err_probe(dev, PTR_ERR(tm->dio), "Failed to get dio GPIO\n");

	mutex_init(&tm->lock);

	tm->brightness = TM1637_BRIGHTNESS_MAX + 1;

	platform_set_drvdata(pdev, tm);
	tm1637_update_display(tm);

	return 0;
}

static void tm1637_remove(struct platform_device *pdev)
{
	struct tm1637 *tm = platform_get_drvdata(pdev);

	mutex_lock(&tm->lock);
	tm->brightness = 0;
	memset(tm->buf, 0, sizeof(tm->buf));
	tm1637_update_display_locked(tm);
	mutex_unlock(&tm->lock);

	mutex_destroy(&tm->lock);
}

static const struct of_device_id tm1637_of_match[] = {
	{.compatible = "titanmec,tm1637"},
	{}};
MODULE_DEVICE_TABLE(of, tm1637_of_match);

static struct platform_driver tm1637_driver = {
	.probe = tm1637_probe,
	.remove = tm1637_remove,
	.driver = {
		.name = "tm1637",
		.of_match_table = tm1637_of_match,
		.dev_groups = tm1637_groups,
	},
};
module_platform_driver(tm1637_driver);

MODULE_DESCRIPTION("TM1637 7-segment display driver");
MODULE_AUTHOR("Siratul Islam <email@sirat.me>");
MODULE_LICENSE("Dual BSD/GPL");
