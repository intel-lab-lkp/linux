// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTC driver for RDA Micro
 *
 * Copyright (C) 2013-2014 RDA Microelectronics Inc.
 * Copyright (C) 2024 Dang Huynh <dang.huynh@mainlining.org>
 */

#include <linux/of.h>
#include <linux/module.h>
#include <linux/rtc.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>

struct rda_rtc {
	struct rtc_device *rtc_dev;
	struct regmap *regmap;
};

/* RTC Registers */
#define RDA_RTC_CTRL_REG 0x0
#define RDA_RTC_CMD_REG 0x4
#define RDA_RTC_STA_REG 0x8
#define RDA_RTC_CAL_LOAD_LOW_REG 0xC
#define RDA_RTC_CAL_LOAD_HIGH_REG 0x10
#define RDA_RTC_CUR_LOAD_LOW_REG 0x14
#define RDA_RTC_CUR_LOAD_HIGH_REG 0x18
#define RDA_RTC_ALARM_LOW_REG 0x1C
#define RDA_RTC_ALARM_HIGH_REG 0x20

/* RTC Bits */
#define RDA_RTC_CMD_CAL_LOAD BIT(0)
#define RDA_RTC_CMD_ALARM_LOAD BIT(4)
#define RDA_RTC_CMD_ALARM_ENABLE BIT(5)
#define RDA_RTC_CMD_ALARM_DISABLE BIT(6)
#define RDA_RTC_CMD_INVALID BIT(31)
#define RDA_RTC_STA_ALARM_ENABLE BIT(20)
#define RDA_RTC_STA_NOT_PROG BIT(31)

/* RTC Masks */
#define RDA_SEC_MASK GENMASK(7, 0)
#define RDA_MIN_MASK GENMASK(15, 8)
#define RDA_HRS_MASK GENMASK(23, 16)

#define RDA_MDAY_MASK GENMASK(7, 0)
#define RDA_MON_MASK GENMASK(11, 8)
#define RDA_YEAR_MASK GENMASK(22, 16)
#define RDA_WDAY_MASK GENMASK(26, 24)

static int rda_rtc_settime(struct device *dev, struct rtc_time *tm)
{
	struct rda_rtc *rtc = dev_get_drvdata(dev);
	u32 high, low;
	int ret;

	ret = rtc_valid_tm(tm);
	if (ret < 0)
		return ret;

	/*
	 * The number of years since 1900 in kernel,
	 * but it is defined since 2000 by HW.
	 * The number of mons' range is from 0 to 11 in kernel,
	 * but it is defined from 1 to 12 by HW.
	 */
	low = FIELD_PREP(RDA_SEC_MASK, tm->tm_sec) |
		FIELD_PREP(RDA_MIN_MASK, tm->tm_min) |
		FIELD_PREP(RDA_HRS_MASK, tm->tm_hour);

	high = FIELD_PREP(RDA_MDAY_MASK, tm->tm_mday) |
		FIELD_PREP(RDA_MON_MASK, tm->tm_mon + 1) |
		FIELD_PREP(RDA_YEAR_MASK, tm->tm_year - 100) |
		FIELD_PREP(RDA_WDAY_MASK, tm->tm_wday);

	ret = regmap_write(rtc->regmap, RDA_RTC_CAL_LOAD_LOW_REG, low);
	if (ret < 0) {
		dev_err(dev, "Failed to update RTC low register: %d\n", ret);
		return ret;
	}

	ret = regmap_write(rtc->regmap, RDA_RTC_CAL_LOAD_HIGH_REG, high);
	if (ret < 0) {
		dev_err(dev, "Failed to update RTC low register: %d\n", ret);
		return ret;
	}

	ret = regmap_update_bits(rtc->regmap, RDA_RTC_CMD_REG, RDA_RTC_CMD_CAL_LOAD, 1);
	if (ret < 0) {
		dev_err(dev, "Failed to update RTC cal load register: %d\n", ret);
		return ret;
	}

	return 0;
}

static int rda_rtc_readtime(struct device *dev, struct rtc_time *tm)
{
	struct rda_rtc *rtc = dev_get_drvdata(dev);
	unsigned int high, low;
	int ret;

	/*
	 * Check if RTC data is valid.
	 *
	 * When this bit is set, it means the data in the RTC is invalid
	 * or not configured.
	 */
	ret = regmap_test_bits(rtc->regmap, RDA_RTC_STA_REG, RDA_RTC_STA_NOT_PROG);
	if (ret < 0) {
		dev_err(dev, "Failed to read RTC status: %d\n", ret);
		return ret;
	} else if (ret > 0)
		return -EINVAL;

	ret = regmap_read(rtc->regmap, RDA_RTC_CUR_LOAD_HIGH_REG, &high);
	if (ret) {
		dev_err(dev, "Failed to read RTC high reg: %d\n", ret);
		return ret;
	}

	ret = regmap_read(rtc->regmap, RDA_RTC_CUR_LOAD_LOW_REG, &low);
	if (ret) {
		dev_err(dev, "Failed to read RTC low reg: %d\n", ret);
		return ret;
	}

	tm->tm_sec = FIELD_GET(RDA_SEC_MASK, low);
	tm->tm_min = FIELD_GET(RDA_MIN_MASK, low);
	tm->tm_hour = FIELD_GET(RDA_HRS_MASK, low);
	tm->tm_mday = FIELD_GET(RDA_MDAY_MASK, high);
	tm->tm_mon = FIELD_GET(RDA_MON_MASK, high);
	tm->tm_year = FIELD_GET(RDA_YEAR_MASK, high);
	tm->tm_wday = FIELD_GET(RDA_WDAY_MASK, high);

	/*
	 * The number of years since 1900 in kernel,
	 * but it is defined since 2000 by HW.
	 */
	tm->tm_year += 100;
	/*
	 * The number of mons' range is from 0 to 11 in kernel,
	 * but it is defined from 1 to 12 by HW.
	 */
	tm->tm_mon -= 1;

	return 0;
}

static int rda_rtc_readalarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rda_rtc *rtc = dev_get_drvdata(dev);
	struct rtc_time *tm = &alrm->time;
	unsigned int high, low;
	int ret;

	ret = regmap_read(rtc->regmap, RDA_RTC_ALARM_HIGH_REG, &high);
	if (ret) {
		dev_err(dev, "Failed to read alarm low reg: %d\n", ret);
		return ret;
	}

	ret = regmap_read(rtc->regmap, RDA_RTC_ALARM_LOW_REG, &low);
	if (ret) {
		dev_err(dev, "Failed to read alarm low reg: %d\n", ret);
		return ret;
	}

	tm->tm_sec = FIELD_GET(RDA_SEC_MASK, low);
	tm->tm_min = FIELD_GET(RDA_MIN_MASK, low);
	tm->tm_hour = FIELD_GET(RDA_HRS_MASK, low);
	tm->tm_mday = FIELD_GET(RDA_MDAY_MASK, high);
	tm->tm_mon = FIELD_GET(RDA_MON_MASK, high);
	tm->tm_year = FIELD_GET(RDA_YEAR_MASK, high);
	tm->tm_wday = FIELD_GET(RDA_WDAY_MASK, high);

	/*
	 * The number of years since 1900 in kernel,
	 * but it is defined since 2000 by HW.
	 */
	tm->tm_year += 100;
	/*
	 * The number of mons' range is from 0 to 11 in kernel,
	 * but it is defined from 1 to 12 by HW.
	 */
	tm->tm_mon -= 1;

	return 0;
}

static int rda_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct rda_rtc *rtc = dev_get_drvdata(dev);

	if (enabled)
		return regmap_update_bits(rtc->regmap, RDA_RTC_CMD_REG,
				RDA_RTC_CMD_ALARM_ENABLE, 1);

	return regmap_update_bits(rtc->regmap, RDA_RTC_CMD_REG,
			RDA_RTC_CMD_ALARM_DISABLE, 1);
}

static int rda_rtc_setalarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rda_rtc *rtc = dev_get_drvdata(dev);
	struct rtc_time *tm = &alrm->time;
	u32 high, low;
	int ret;

	ret = rtc_valid_tm(tm);
	if (ret < 0)
		return ret;

	/* TODO: Check if it's necessary to disable IRQ first */
	rda_rtc_alarm_irq_enable(dev, 0);

	/*
	 * The number of years since 1900 in kernel,
	 * but it is defined since 2000 by HW.
	 * The number of mons' range is from 0 to 11 in kernel,
	 * but it is defined from 1 to 12 by HW.
	 */
	low = FIELD_PREP(RDA_SEC_MASK, tm->tm_sec) |
		FIELD_PREP(RDA_MIN_MASK, tm->tm_min) |
		FIELD_PREP(RDA_HRS_MASK, tm->tm_hour);

	high = FIELD_PREP(RDA_MDAY_MASK, tm->tm_mday) |
		FIELD_PREP(RDA_MON_MASK, tm->tm_mon + 1) |
		FIELD_PREP(RDA_YEAR_MASK, tm->tm_year - 100) |
		FIELD_PREP(RDA_WDAY_MASK, tm->tm_wday);


	ret = regmap_write(rtc->regmap, RDA_RTC_ALARM_LOW_REG, low);
	if (ret < 0) {
		dev_err(dev, "Failed to set low alarm register: %d\n", ret);
		return ret;
	}

	ret = regmap_write(rtc->regmap, RDA_RTC_ALARM_HIGH_REG, high);
	if (ret < 0) {
		dev_err(dev, "Failed to set low alarm register: %d\n", ret);
		return ret;
	}

	ret = regmap_update_bits(rtc->regmap, RDA_RTC_CMD_REG, RDA_RTC_CMD_ALARM_LOAD, 1);
	if (ret < 0) {
		dev_err(dev, "Failed to set alarm register: %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "Alarm set: %4d-%02d-%02d %02d:%02d:%02d\n",
			2000 + (tm->tm_year - 100), tm->tm_mon + 1, tm->tm_mday,
			tm->tm_hour, tm->tm_min, tm->tm_sec);

	return 0;
}

static int rda_rtc_proc(struct device *dev, struct seq_file *seq)
{
	struct rda_rtc *rtc = dev_get_drvdata(dev);
	int ret;

	ret = regmap_test_bits(rtc->regmap, RDA_RTC_STA_REG, RDA_RTC_STA_ALARM_ENABLE);
	if (ret < 0) {
		dev_err(dev, "Failed to read alarm status: %d\n", ret);
		return ret;
	}

	seq_printf(seq, "alarm enable\t: %s\n", (ret > 0) ? "yes" : "no");

	return 0;
}

static const struct rtc_class_ops rda_rtc_ops = {
	.read_time = rda_rtc_readtime,
	.set_time = rda_rtc_settime,
	.read_alarm = rda_rtc_readalarm,
	.set_alarm = rda_rtc_setalarm,
	.proc = rda_rtc_proc,
	.alarm_irq_enable = rda_rtc_alarm_irq_enable,
};

#ifdef CONFIG_PM_SLEEP
static int rda_rtc_suspend(struct platform_device *pdev, pm_message_t state)
{
	/* TODO: Check if it's okay to turn on alarm IRQ when it's not set */
	return rda_rtc_alarm_irq_enable(&pdev->dev, 1);
}

static int rda_rtc_resume(struct platform_device *pdev)
{
	/* If alarms were left, we turn them off. */
	return rda_rtc_alarm_irq_enable(&pdev->dev, 0);
}
#endif

static SIMPLE_DEV_PM_OPS(rda_rtc_pm_ops, rda_rtc_suspend, rda_rtc_resume);

static const struct regmap_config regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int rda_rtc_probe(struct platform_device *pdev)
{
	struct rda_rtc *rda_rtc;
	void __iomem *base;

	rda_rtc = devm_kzalloc(&pdev->dev, sizeof(*rda_rtc), GFP_KERNEL);
	if (!rda_rtc)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return dev_err_probe(&pdev->dev, PTR_ERR(base),
				"failed to remap resource\n");

	rda_rtc->regmap = devm_regmap_init_mmio(&pdev->dev, base, &regmap_config);
	if (!rda_rtc->regmap)
		return dev_err_probe(&pdev->dev, PTR_ERR(rda_rtc->regmap),
				"can't find regmap\n");

	rda_rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rda_rtc->rtc_dev))
		return dev_err_probe(&pdev->dev, PTR_ERR(rda_rtc->rtc_dev),
				"failed to allocate rtc device\n");

	rda_rtc->rtc_dev->ops = &rda_rtc_ops;
	rda_rtc->rtc_dev->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rda_rtc->rtc_dev->range_max = RTC_TIMESTAMP_END_2127;

	platform_set_drvdata(pdev, rda_rtc);

	return devm_rtc_register_device(rda_rtc->rtc_dev);
}

static const struct of_device_id rda_rtc_id_table[] = {
	{ .compatible = "rda,8810pl-rtc", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rda_rtc_id_table);

static struct platform_driver rda_rtc_driver = {
	.probe = rda_rtc_probe,
	.driver = {
		.name = "rtc-rda",
		.pm = &rda_rtc_pm_ops,
		.of_match_table = rda_rtc_id_table,
	},
};
module_platform_driver(rda_rtc_driver);

MODULE_AUTHOR("Dang Huynh <dang.huynh@mainlining.org>");
MODULE_DESCRIPTION("RDA Micro RTC driver");
MODULE_LICENSE("GPL");
