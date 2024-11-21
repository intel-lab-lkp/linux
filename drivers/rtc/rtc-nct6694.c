// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 RTC driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/bcd.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/slab.h>

/* Host interface */
#define NCT6694_RTC_MOD		0x08

/* Message Channel */
/* Command 00h */
#define NCT6694_RTC_CMD0_LEN	0x07
#define NCT6694_RTC_CMD0_OFFSET	0x0000	/* OFFSET = SEL|CMD */
/* Command 01h */
#define NCT6694_RTC_CMD1_LEN	0x05
#define NCT6694_RTC_CMD1_OFFSET	0x0001	/* OFFSET = SEL|CMD */
/* Command 02h */
#define NCT6694_RTC_CMD2_LEN	0x02
#define NCT6694_RTC_CMD2_OFFSET	0x0002	/* OFFSET = SEL|CMD */

#define NCT6694_RTC_IRQ_INT_EN		BIT(0)	/* Transmit a USB INT-in when RTC alarm */
#define NCT6694_RTC_IRQ_GPO_EN		BIT(5)	/* Trigger a GPO Low Pulse when RTC alarm */

#define NCT6694_RTC_IRQ_EN		(NCT6694_RTC_IRQ_INT_EN | NCT6694_RTC_IRQ_GPO_EN)
#define NCT6694_RTC_IRQ_STS		BIT(0)	/* Write 1 clear IRQ status */

struct __packed nct6694_rtc_cmd0 {
	u8 sec;
	u8 min;
	u8 hour;
	u8 week;
	u8 day;
	u8 month;
	u8 year;
};

struct __packed nct6694_rtc_cmd1 {
	u8 sec;
	u8 min;
	u8 hour;
	u8 alarm_en;
	u8 alarm_pend;
};

struct __packed nct6694_rtc_cmd2 {
	u8 irq_en;
	u8 irq_pend;
};

struct nct6694_rtc_data {
	struct nct6694 *nct6694;
	struct rtc_device *rtc;
	struct mutex lock;
	unsigned char *xmit_buf;
};

static int nct6694_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	struct nct6694_rtc_cmd0 *buf = (struct nct6694_rtc_cmd0 *)data->xmit_buf;
	int ret;

	guard(mutex)(&data->lock);

	ret = nct6694_read_msg(data->nct6694, NCT6694_RTC_MOD,
			       NCT6694_RTC_CMD0_OFFSET,
			       NCT6694_RTC_CMD0_LEN,
			       buf);
	if (ret)
		return ret;

	tm->tm_sec = bcd2bin(buf->sec);		/* tm_sec expect 0 ~ 59 */
	tm->tm_min = bcd2bin(buf->min);		/* tm_min expect 0 ~ 59 */
	tm->tm_hour = bcd2bin(buf->hour);	/* tm_hour expect 0 ~ 23 */
	tm->tm_wday = bcd2bin(buf->week) - 1;	/* tm_wday expect 0 ~ 6 */
	tm->tm_mday = bcd2bin(buf->day);	/* tm_mday expect 1 ~ 31 */
	tm->tm_mon = bcd2bin(buf->month) - 1;	/* tm_month expect 0 ~ 11 */
	tm->tm_year = bcd2bin(buf->year) + 100;	/* tm_year expect since 1900 */

	return ret;
}

static int nct6694_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	struct nct6694_rtc_cmd0 *buf = (struct nct6694_rtc_cmd0 *)data->xmit_buf;

	guard(mutex)(&data->lock);

	buf->sec = bin2bcd(tm->tm_sec);
	buf->min = bin2bcd(tm->tm_min);
	buf->hour = bin2bcd(tm->tm_hour);
	buf->week = bin2bcd(tm->tm_wday + 1);
	buf->day = bin2bcd(tm->tm_mday);
	buf->month = bin2bcd(tm->tm_mon + 1);
	buf->year = bin2bcd(tm->tm_year - 100);

	return nct6694_write_msg(data->nct6694, NCT6694_RTC_MOD,
				 NCT6694_RTC_CMD0_OFFSET,
				 NCT6694_RTC_CMD0_LEN,
				 buf);
}

static int nct6694_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	struct nct6694_rtc_cmd1 *buf = (struct nct6694_rtc_cmd1 *)data->xmit_buf;
	int ret;

	guard(mutex)(&data->lock);

	ret = nct6694_read_msg(data->nct6694, NCT6694_RTC_MOD,
			       NCT6694_RTC_CMD1_OFFSET,
			       NCT6694_RTC_CMD1_LEN,
			       buf);
	if (ret)
		return ret;

	alrm->time.tm_sec = bcd2bin(buf->sec);
	alrm->time.tm_min = bcd2bin(buf->min);
	alrm->time.tm_hour = bcd2bin(buf->hour);
	alrm->enabled = buf->alarm_en;
	alrm->pending = buf->alarm_pend;

	return ret;
}

static int nct6694_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	struct nct6694_rtc_cmd1 *buf = (struct nct6694_rtc_cmd1 *)data->xmit_buf;

	guard(mutex)(&data->lock);

	buf->sec = bin2bcd(alrm->time.tm_sec);
	buf->min = bin2bcd(alrm->time.tm_min);
	buf->hour = bin2bcd(alrm->time.tm_hour);
	buf->alarm_en = alrm->enabled ? NCT6694_RTC_IRQ_EN : 0;
	buf->alarm_pend = 0;

	return nct6694_write_msg(data->nct6694, NCT6694_RTC_MOD,
				 NCT6694_RTC_CMD1_OFFSET,
				 NCT6694_RTC_CMD1_LEN,
				 buf);
}

static int nct6694_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct nct6694_rtc_data *data = dev_get_drvdata(dev);
	struct nct6694_rtc_cmd2 *buf = (struct nct6694_rtc_cmd2 *)data->xmit_buf;

	guard(mutex)(&data->lock);

	if (enabled)
		buf->irq_en |= NCT6694_RTC_IRQ_EN;
	else
		buf->irq_en &= ~NCT6694_RTC_IRQ_EN;

	buf->irq_pend = 0;

	return nct6694_write_msg(data->nct6694, NCT6694_RTC_MOD,
				 NCT6694_RTC_CMD2_OFFSET,
				 NCT6694_RTC_CMD2_LEN,
				 buf);
}

static const struct rtc_class_ops nct6694_rtc_ops = {
	.read_time = nct6694_rtc_read_time,
	.set_time = nct6694_rtc_set_time,
	.read_alarm = nct6694_rtc_read_alarm,
	.set_alarm = nct6694_rtc_set_alarm,
	.alarm_irq_enable = nct6694_rtc_alarm_irq_enable,
};

static irqreturn_t nct6694_irq(int irq, void *dev_id)
{
	struct nct6694_rtc_data *data = dev_id;
	struct nct6694_rtc_cmd2 *buf = (struct nct6694_rtc_cmd2 *)data->xmit_buf;
	int ret;

	guard(mutex)(&data->lock);

	buf->irq_en = NCT6694_RTC_IRQ_EN;
	buf->irq_pend = NCT6694_RTC_IRQ_STS;
	ret = nct6694_write_msg(data->nct6694, NCT6694_RTC_MOD,
				NCT6694_RTC_CMD2_OFFSET,
				NCT6694_RTC_CMD2_LEN,
				buf);
	if (ret)
		return IRQ_NONE;

	rtc_update_irq(data->rtc, 1, RTC_IRQF | RTC_AF);

	return IRQ_HANDLED;
}

static int nct6694_rtc_probe(struct platform_device *pdev)
{
	struct nct6694_rtc_data *data;
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	int ret, irq;

	irq = irq_create_mapping(nct6694->domain, NCT6694_IRQ_RTC);
	if (!irq)
		return -EINVAL;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->xmit_buf = devm_kcalloc(&pdev->dev, NCT6694_MAX_PACKET_SZ,
				      sizeof(unsigned char), GFP_KERNEL);
	if (!data->xmit_buf)
		return -ENOMEM;

	data->rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(data->rtc))
		return PTR_ERR(data->rtc);

	data->nct6694 = nct6694;
	data->rtc->ops = &nct6694_rtc_ops;
	data->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	data->rtc->range_max = RTC_TIMESTAMP_END_2099;

	mutex_init(&data->lock);

	device_set_wakeup_capable(&pdev->dev, 1);

	platform_set_drvdata(pdev, data);

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					nct6694_irq, IRQF_ONESHOT,
					"nct6694-rtc", data);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "Failed to request irq\n");

	/* Register rtc device to RTC framework */
	return devm_rtc_register_device(data->rtc);
}

static struct platform_driver nct6694_rtc_driver = {
	.driver = {
		.name	= "nct6694-rtc",
	},
	.probe		= nct6694_rtc_probe,
};

module_platform_driver(nct6694_rtc_driver);

MODULE_DESCRIPTION("USB-RTC driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
