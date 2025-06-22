// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the RTC found in the SpacemiT P1 PMIC
 *
 * Copyright (C) 2025 by RISCstar Solutions Corporation.  All rights reserved.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>

#define MOD_NAME	"spacemit-p1-rtc"

/* Offset to byte containing the given time unit */
enum time_unit {
	tu_second = 0,		/* 0-59 */
	tu_minute,		/* 0-59 */
	tu_hour,		/* 0-59 */
	tu_day,			/* 0-30 (struct tm uses 1-31) */
	tu_month,		/* 0-11 */
	tu_year,		/* Years since 2000 (struct tm uses 1900) */
	tu_count,		/* Last; not a time unit */
};

/* Consecutive bytes contain seconds, minutes, etc. */
#define RTC_COUNT_BASE		0x0d

#define RTC_CTRL		0x1d
#define RTC_EN		BIT(2)

struct p1_rtc {
	struct regmap *regmap;
	struct rtc_device *rtc;
};

static int p1_rtc_read_time(struct device *dev, struct rtc_time *t)
{
	struct p1_rtc *p1 = dev_get_drvdata(dev);
	u8 time[tu_count];
	int ret;

	ret = regmap_bulk_read(p1->regmap, RTC_COUNT_BASE, &time, sizeof(time));
	if (ret)
		return ret;

	t->tm_sec = time[tu_second] & GENMASK(5, 0);
	t->tm_min = time[tu_minute] & GENMASK(5, 0);
	t->tm_hour = time[tu_hour] & GENMASK(4, 0);
	t->tm_mday = (time[tu_day] & GENMASK(4, 0)) + 1;
	t->tm_mon = time[tu_month] & GENMASK(3, 0);
	t->tm_year = (time[tu_year] & GENMASK(5, 0)) + 100;
	/* tm_wday, tm_yday, and tm_isdst aren't used */

	return 0;
}

static int p1_rtc_set_time(struct device *dev, struct rtc_time *t)
{
	struct p1_rtc *p1 = dev_get_drvdata(dev);
	u8 time[tu_count];
	int ret;

	time[tu_second] = t->tm_sec;
	time[tu_minute] = t->tm_min;
	time[tu_hour] = t->tm_hour;
	time[tu_day] = t->tm_mday - 1;
	time[tu_month] = t->tm_mon;
	time[tu_year] = t->tm_year - 100;

	/* Disable the RTC to update; re-enable again when done */
	ret = regmap_update_bits(p1->regmap, RTC_CTRL, RTC_EN, 0);
	if (ret)
		return ret;

	ret = regmap_bulk_write(p1->regmap, RTC_COUNT_BASE, time, sizeof(time));

	(void)regmap_update_bits(p1->regmap, RTC_CTRL, RTC_EN, RTC_EN);

	return ret;
}

static const struct rtc_class_ops p1_rtc_class_ops = {
	.read_time = p1_rtc_read_time,
	.set_time = p1_rtc_set_time,
};

static int p1_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtc_device *rtc;
	struct p1_rtc *p1;
	int ret;

	p1 = devm_kzalloc(dev, sizeof(*p1), GFP_KERNEL);
	if (!p1)
		return -ENOMEM;
	dev_set_drvdata(dev, p1);

	p1->regmap = dev_get_regmap(dev->parent, NULL);
	if (!p1->regmap)
		return dev_err_probe(dev, -ENODEV, "failed to get regmap\n");

	rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc))
		return dev_err_probe(dev, PTR_ERR(rtc),
				     "error allocating device\n");
	p1->rtc = rtc;

	rtc->ops = &p1_rtc_class_ops;
	rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rtc->range_max = RTC_TIMESTAMP_END_2063;

	clear_bit(RTC_FEATURE_ALARM, rtc->features);
	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, rtc->features);

	ret = devm_rtc_register_device(rtc);
	if (ret)
		return dev_err_probe(dev, ret, "error registering RTC\n");

	return 0;
}

static struct platform_driver p1_rtc_driver = {
	.probe = p1_rtc_probe,
	.driver = {
		.name = MOD_NAME,
	},
};

module_platform_driver(p1_rtc_driver);

MODULE_DESCRIPTION("SpacemiT P1 RTC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" MOD_NAME);
