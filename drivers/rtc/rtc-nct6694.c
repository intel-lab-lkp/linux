// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 RTC driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rtc.h>
#include <linux/bcd.h>
#include <linux/platform_device.h>
#include <linux/mfd/nct6694.h>

#define DRVNAME "nct6694-rtc"

/* Host interface */
#define REQUEST_RTC_MOD		0x08

/* Message Channel */
/* Command 00h */
#define REQUEST_RTC_CMD0_LEN	0x07
#define REQUEST_RTC_CMD0_OFFSET	0x0000	/* OFFSET = SEL|CMD */
#define RTC_SEC_IDX		0x00
#define RTC_MIN_IDX		0x01
#define RTC_HOUR_IDX		0x02
#define RTC_WEEK_IDX		0x03
#define RTC_DAY_IDX		0x04
#define RTC_MONTH_IDX		0x05
#define RTC_YEAR_IDX		0x06
/* Command 01h */
#define REQUEST_RTC_CMD1_LEN	0x05
#define REQUEST_RTC_CMD1_OFFSET	0x0001	/* OFFSET = SEL|CMD */
#define RTC_ALRM_EN_IDX		0x03
#define RTC_ALRM_PEND_IDX	0x04
/* Command 02h */
#define REQUEST_RTC_CMD2_LEN	0x02
#define REQUEST_RTC_CMD2_OFFSET	0x0002	/* OFFSET = SEL|CMD */
#define RTC_IRQ_EN_IDX		0x00
#define RTC_IRQ_PEND_IDX	0x01

#define RTC_IRQ_EN		(BIT(0) | BIT(5))
#define RTC_IRQ_INT_EN		BIT(0)	/* Transmit a USB INT-in when RTC alarm */
#define RTC_IRQ_GPO_EN		BIT(5)	/* Trigger a GPO Low Pulse when RTC alarm */
#define RTC_IRQ_STS		BIT(0)	/* Write 1 clear IRQ status */

struct nct6694_rtc_data {
	struct nct6694 *nct6694;
	struct rtc_device *rtc;
	struct work_struct alarm_work;
};

static int nct6694_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	unsigned char buf[REQUEST_RTC_CMD0_LEN];
	int ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_RTC_MOD,
			       REQUEST_RTC_CMD0_OFFSET, REQUEST_RTC_CMD0_LEN,
			       0, REQUEST_RTC_CMD0_LEN, buf);
	if (ret) {
		pr_err("%s: Failed to get rtc device!\n", __func__);
		return -EIO;
	}

	tm->tm_sec = bcd2bin(buf[RTC_SEC_IDX]);		/* tm_sec expect 0 ~ 59 */
	tm->tm_min = bcd2bin(buf[RTC_MIN_IDX]);		/* tm_min expect 0 ~ 59 */
	tm->tm_hour = bcd2bin(buf[RTC_HOUR_IDX]);	/* tm_hour expect 0 ~ 23 */
	tm->tm_wday = bcd2bin(buf[RTC_WEEK_IDX]) - 1;	/* tm_wday expect 0 ~ 6 */
	tm->tm_mday = bcd2bin(buf[RTC_DAY_IDX]);	/* tm_mday expect 1 ~ 31 */
	tm->tm_mon = bcd2bin(buf[RTC_MONTH_IDX]) - 1;	/* tm_month expect 0 ~ 11 */
	tm->tm_year = bcd2bin(buf[RTC_YEAR_IDX]) + 100;	/* tm_year expect since 1900 */

	return ret;
}

static int nct6694_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	unsigned char buf[REQUEST_RTC_CMD0_LEN];
	int ret;

	buf[RTC_SEC_IDX] = bin2bcd(tm->tm_sec);
	buf[RTC_MIN_IDX] = bin2bcd(tm->tm_min);
	buf[RTC_HOUR_IDX] = bin2bcd(tm->tm_hour);
	buf[RTC_WEEK_IDX] = bin2bcd(tm->tm_wday + 1);
	buf[RTC_DAY_IDX] = bin2bcd(tm->tm_mday);
	buf[RTC_MONTH_IDX] = bin2bcd(tm->tm_mon + 1);
	buf[RTC_YEAR_IDX] = bin2bcd(tm->tm_year - 100);

	ret = nct6694_write_msg(data->nct6694, REQUEST_RTC_MOD,
				REQUEST_RTC_CMD0_OFFSET, REQUEST_RTC_CMD0_LEN,
				buf);
	if (ret) {
		pr_err("%s: Failed to set rtc device!\n", __func__);
		return -EIO;
	}

	return ret;
}

static int nct6694_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	unsigned char buf[REQUEST_RTC_CMD1_LEN];
	int ret;

	ret = nct6694_read_msg(data->nct6694, REQUEST_RTC_MOD,
			       REQUEST_RTC_CMD1_OFFSET, REQUEST_RTC_CMD1_LEN,
			       0, REQUEST_RTC_CMD1_LEN, buf);
	if (ret) {
		pr_err("%s: Failed to get rtc device!\n", __func__);
		return -EIO;
	}

	alrm->time.tm_sec = bcd2bin(buf[RTC_SEC_IDX]);
	alrm->time.tm_min = bcd2bin(buf[RTC_MIN_IDX]);
	alrm->time.tm_hour = bcd2bin(buf[RTC_HOUR_IDX]);

	alrm->enabled = buf[RTC_ALRM_EN_IDX];
	alrm->pending = buf[RTC_ALRM_PEND_IDX];

	return ret;
}

static int nct6694_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	unsigned char buf[REQUEST_RTC_CMD1_LEN];
	int ret;

	buf[RTC_SEC_IDX] = bin2bcd(alrm->time.tm_sec);
	buf[RTC_MIN_IDX] = bin2bcd(alrm->time.tm_min);
	buf[RTC_HOUR_IDX] = bin2bcd(alrm->time.tm_hour);
	buf[RTC_ALRM_EN_IDX] = alrm->enabled ? RTC_IRQ_EN : 0;
	buf[RTC_ALRM_PEND_IDX] = 0;

	ret = nct6694_write_msg(data->nct6694, REQUEST_RTC_MOD,
				REQUEST_RTC_CMD1_OFFSET, REQUEST_RTC_CMD1_LEN,
				buf);
	if (ret) {
		pr_err("%s: Failed to set rtc device!\n", __func__);
		return -EIO;
	}

	return ret;
}

static int nct6694_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	unsigned char buf[REQUEST_RTC_CMD2_LEN] = {0};
	int ret;

	if (enabled)
		buf[RTC_IRQ_EN_IDX] |= RTC_IRQ_EN;
	else
		buf[RTC_IRQ_EN_IDX] &= ~RTC_IRQ_EN;

	ret = nct6694_write_msg(data->nct6694, REQUEST_RTC_MOD,
				REQUEST_RTC_CMD2_OFFSET, REQUEST_RTC_CMD2_LEN,
				buf);
	if (ret) {
		pr_err("%s: Failed to set rtc device!\n", __func__);
		return -EIO;
	}

	return ret;
}

static const struct rtc_class_ops nct6694_rtc_ops = {
	.read_time = nct6694_rtc_read_time,
	.set_time = nct6694_rtc_set_time,
	.read_alarm = nct6694_rtc_read_alarm,
	.set_alarm = nct6694_rtc_set_alarm,
	.alarm_irq_enable = nct6694_rtc_alarm_irq_enable,
};

static void nct6694_rtc_alarm(struct work_struct *work)
{
	struct nct6694_rtc_data *data;
	unsigned char buf[REQUEST_RTC_CMD2_LEN] = {0};

	data = container_of(work, struct nct6694_rtc_data, alarm_work);

	pr_info("%s: Got RTC alarm!\n", __func__);
	buf[RTC_IRQ_EN_IDX] = RTC_IRQ_EN;
	buf[RTC_IRQ_PEND_IDX] = RTC_IRQ_STS;
	nct6694_write_msg(data->nct6694, REQUEST_RTC_MOD,
			  REQUEST_RTC_CMD2_OFFSET,
			  REQUEST_RTC_CMD2_LEN, buf);
}

static void nct6694_rtc_handler(void *private_data)
{
	struct nct6694_rtc_data *data = private_data;
	struct nct6694 *nct6694 = data->nct6694;

	queue_work(nct6694->async_workqueue, &data->alarm_work);
}

static int nct6694_rtc_probe(struct platform_device *pdev)
{
	struct nct6694_rtc_data *data;
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(data->rtc))
		return PTR_ERR(data->rtc);

	data->nct6694 = nct6694;
	data->rtc->ops = &nct6694_rtc_ops;
	data->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	data->rtc->range_max = RTC_TIMESTAMP_END_2099;

	INIT_WORK(&data->alarm_work, nct6694_rtc_alarm);

	ret = nct6694_register_handler(nct6694, RTC_IRQ_STATUS,
				       nct6694_rtc_handler, data);
	if (ret) {
		dev_err(&pdev->dev, "%s:  Failed to register handler: %pe\n",
			__func__, ERR_PTR(ret));
		return ret;
	}

	device_set_wakeup_capable(&pdev->dev, 1);

	platform_set_drvdata(pdev, data);

	/* Register rtc device to RTC framework */
	ret = devm_rtc_register_device(data->rtc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register rtc device!\n");
		return ret;
	}

	return 0;
}

static struct platform_driver nct6694_rtc_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_rtc_probe,
};

static int __init nct6694_init(void)
{
	int err;

	err = platform_driver_register(&nct6694_rtc_driver);
	if (!err) {
		if (err)
			platform_driver_unregister(&nct6694_rtc_driver);
	}

	return err;
}
subsys_initcall(nct6694_init);

static void __exit nct6694_exit(void)
{
	platform_driver_unregister(&nct6694_rtc_driver);
}
module_exit(nct6694_exit);

MODULE_DESCRIPTION("USB-RTC driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
