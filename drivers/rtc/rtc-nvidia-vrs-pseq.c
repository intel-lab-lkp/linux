// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * RTC driver for NVIDIA Voltage Regulator Power Sequencer
 *
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/rtc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/mfd/nvidia-vrs-pseq.h>
#include <linux/irqdomain.h>
#include <linux/regmap.h>
#include <linux/bits.h>

#define ALARM_RESET_VAL		0xffffffff /* Alarm reset/disable value */
#define NVVRS_INT_RTC_INDEX	0	   /* Only one RTC interrupt register */

struct nvvrs_rtc_info {
	struct device          *dev;
	struct i2c_client      *client;
	struct rtc_device      *rtc_dev;
	unsigned int           rtc_irq;
	const struct regmap_irq_chip *rtc_irq_chip;
	struct regmap_irq_chip_data *rtc_irq_data;
	/* Mutex to protect RTC operations */
	struct mutex           lock;
};

static const struct regmap_irq nvvrs_rtc_irq[] = {
	REGMAP_IRQ_REG(NVVRS_INT_RTC_INDEX, 0, NVVRS_PSEQ_INT_SRC1_RTC_MASK),
};

static const struct regmap_irq_chip nvvrs_rtc_irq_chip = {
	.name	   = "nvvrs-rtc",
	.status_base    = NVVRS_PSEQ_REG_INT_SRC1,
	.num_regs       = 1,
	.irqs	   = nvvrs_rtc_irq,
	.num_irqs       = ARRAY_SIZE(nvvrs_rtc_irq),
};

static int nvvrs_update_bits(struct nvvrs_rtc_info *info, u8 reg,
			     u8 mask, u8 value)
{
	int ret;
	u8 val;

	ret = i2c_smbus_read_byte_data(info->client, reg);
	if (ret < 0)
		return ret;

	val = (u8)ret;
	val &= ~mask;
	val |= (value & mask);

	return i2c_smbus_write_byte_data(info->client, reg, val);
}

static int nvvrs_rtc_update_alarm_reg(struct i2c_client *client,
				      struct nvvrs_rtc_info *info, u8 *time)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, NVVRS_PSEQ_REG_RTC_A3, time[3]);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_byte_data(client, NVVRS_PSEQ_REG_RTC_A2, time[2]);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_byte_data(client, NVVRS_PSEQ_REG_RTC_A1, time[1]);
	if (ret < 0)
		return ret;

	return i2c_smbus_write_byte_data(client, NVVRS_PSEQ_REG_RTC_A0, time[0]);
}

static int nvvrs_rtc_enable_alarm(struct nvvrs_rtc_info *info)
{
	int ret;

	/* Set RTC_WAKE bit for autonomous wake from sleep */
	ret = nvvrs_update_bits(info, NVVRS_PSEQ_REG_CTL_2,
				NVVRS_PSEQ_REG_CTL_2_RTC_WAKE,
				NVVRS_PSEQ_REG_CTL_2_RTC_WAKE);
	if (ret < 0) {
		dev_dbg(info->dev, "Failed to set RTC_WAKE bit (%d)\n", ret);
		return ret;
	}

	/* Set RTC_PU bit for autonomous wake from shutdown */
	ret = nvvrs_update_bits(info, NVVRS_PSEQ_REG_CTL_2,
				NVVRS_PSEQ_REG_CTL_2_RTC_PU,
				NVVRS_PSEQ_REG_CTL_2_RTC_PU);
	if (ret < 0) {
		dev_dbg(info->dev, "Failed to set RTC_PU bit (%d)\n", ret);
		return ret;
	}

	return ret;
}

static int nvvrs_rtc_disable_alarm(struct nvvrs_rtc_info *info)
{
	struct i2c_client *client = info->client;
	u8 val[4];
	int ret;

	/* Clear RTC_WAKE bit */
	ret = nvvrs_update_bits(info, NVVRS_PSEQ_REG_CTL_2,
				NVVRS_PSEQ_REG_CTL_2_RTC_WAKE, 0);
	if (ret < 0) {
		dev_dbg(info->dev, "Failed to clear RTC_WAKE bit (%d)\n", ret);
		return ret;
	}

	/* Clear RTC_PU bit */
	ret = nvvrs_update_bits(info, NVVRS_PSEQ_REG_CTL_2,
				NVVRS_PSEQ_REG_CTL_2_RTC_PU, 0);
	if (ret < 0) {
		dev_dbg(info->dev, "Failed to clear RTC_PU bit (%d)\n", ret);
		return ret;
	}

	/* Write ALARM_RESET_VAL in RTC Alarm register to disable alarm */
	val[0] = 0xff;
	val[1] = 0xff;
	val[2] = 0xff;
	val[3] = 0xff;

	ret = nvvrs_rtc_update_alarm_reg(client, info, val);
	if (ret < 0)
		dev_dbg(info->dev, "Failed to disable Alarm (%d)\n", ret);

	return ret;
}

static irqreturn_t nvvrs_rtc_irq_handler(int irq, void *data)
{
	struct nvvrs_rtc_info *info = data;

	dev_dbg(info->dev, "RTC alarm IRQ: %d\n", irq);

	rtc_lock(info->rtc_dev);
	rtc_update_irq(info->rtc_dev, 1, RTC_IRQF | RTC_AF);
	rtc_unlock(info->rtc_dev);

	return IRQ_HANDLED;
}

static int nvvrs_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct nvvrs_rtc_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	time64_t secs = 0;
	int ret;
	u8 val;

	mutex_lock(&info->lock);

	/* Multi-byte transfers are not supported with PEC enabled */
	/* Read MSB first to avoid coherency issues */
	ret = i2c_smbus_read_byte_data(client, NVVRS_PSEQ_REG_RTC_T3);
	if (ret < 0)
		goto out;

	val = (u8)ret;
	secs |= (time64_t)val << 24;

	ret = i2c_smbus_read_byte_data(client, NVVRS_PSEQ_REG_RTC_T2);
	if (ret < 0)
		goto out;

	val = (u8)ret;
	secs |= (time64_t)val << 16;

	ret = i2c_smbus_read_byte_data(client, NVVRS_PSEQ_REG_RTC_T1);
	if (ret < 0)
		goto out;

	val = (u8)ret;
	secs |= (time64_t)val << 8;

	ret = i2c_smbus_read_byte_data(client, NVVRS_PSEQ_REG_RTC_T0);
	if (ret < 0)
		goto out;

	val = (u8)ret;
	secs |= val;

	rtc_time64_to_tm(secs, tm);
out:
	mutex_unlock(&info->lock);
	return ret;
}

static int nvvrs_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct nvvrs_rtc_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	u8 time[4];
	time64_t secs;
	int ret;

	mutex_lock(&info->lock);

	secs = rtc_tm_to_time64(tm);
	time[0] = secs & 0xff;
	time[1] = (secs >> 8) & 0xff;
	time[2] = (secs >> 16) & 0xff;
	time[3] = (secs >> 24) & 0xff;

	ret = i2c_smbus_write_byte_data(client, NVVRS_PSEQ_REG_RTC_T3, time[3]);
	if (ret < 0)
		goto out;

	ret = i2c_smbus_write_byte_data(client, NVVRS_PSEQ_REG_RTC_T2, time[2]);
	if (ret < 0)
		goto out;

	ret = i2c_smbus_write_byte_data(client, NVVRS_PSEQ_REG_RTC_T1, time[1]);
	if (ret < 0)
		goto out;

	ret = i2c_smbus_write_byte_data(client, NVVRS_PSEQ_REG_RTC_T0, time[0]);

out:
	mutex_unlock(&info->lock);
	return ret;
}

static int nvvrs_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct nvvrs_rtc_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	time64_t alarm_val = 0;
	int ret;
	u8 val;

	mutex_lock(&info->lock);

	/* Multi-byte transfers are not supported with PEC enabled */
	ret = i2c_smbus_read_byte_data(client, NVVRS_PSEQ_REG_RTC_A3);
	if (ret < 0)
		goto out;

	val = (u8)ret;
	alarm_val |= (time64_t)val << 24;

	ret = i2c_smbus_read_byte_data(client, NVVRS_PSEQ_REG_RTC_A2);
	if (ret < 0)
		goto out;

	val = (u8)ret;
	alarm_val |= (time64_t)val << 16;

	ret = i2c_smbus_read_byte_data(client, NVVRS_PSEQ_REG_RTC_A1);
	if (ret < 0)
		goto out;

	val = (u8)ret;
	alarm_val |= (time64_t)val << 8;

	ret = i2c_smbus_read_byte_data(client, NVVRS_PSEQ_REG_RTC_A0);
	if (ret < 0)
		goto out;

	val = (u8)ret;
	alarm_val |= val;

	if (alarm_val == ALARM_RESET_VAL)
		alrm->enabled = 0;
	else
		alrm->enabled = 1;

	rtc_time64_to_tm(alarm_val, &alrm->time);
out:
	mutex_unlock(&info->lock);
	return ret;
}

static int nvvrs_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct nvvrs_rtc_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	u8 time[4];
	time64_t secs;
	int ret;

	mutex_lock(&info->lock);

	ret = nvvrs_rtc_enable_alarm(info);
	if (ret < 0) {
		dev_err(info->dev, "Failed to enable alarm! (%d)\n", ret);
		goto out;
	}

	secs = rtc_tm_to_time64(&alrm->time);
	time[0] = secs & 0xff;
	time[1] = (secs >> 8) & 0xff;
	time[2] = (secs >> 16) & 0xff;
	time[3] = (secs >> 24) & 0xff;

	ret = nvvrs_rtc_update_alarm_reg(client, info, time);

	alrm->enabled = 1;
out:
	mutex_unlock(&info->lock);
	return ret;
}

static int nvvrs_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct nvvrs_rtc_info *info = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&info->lock);
	if (enabled)
		ret = nvvrs_rtc_enable_alarm(info);
	else
		ret = nvvrs_rtc_disable_alarm(info);

	mutex_unlock(&info->lock);
	return ret;
}

static const struct rtc_class_ops nvvrs_rtc_ops = {
	.read_time = nvvrs_rtc_read_time,
	.set_time = nvvrs_rtc_set_time,
	.read_alarm = nvvrs_rtc_read_alarm,
	.set_alarm = nvvrs_rtc_set_alarm,
	.alarm_irq_enable = nvvrs_rtc_alarm_irq_enable,
};

static int nvvrs_rtc_probe(struct platform_device *pdev)
{
	struct nvvrs_rtc_info *info;
	struct device *parent;
	struct i2c_client *client;
	int ret;

	info = devm_kzalloc(&pdev->dev, sizeof(struct nvvrs_rtc_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	mutex_init(&info->lock);

	ret = platform_get_irq(pdev, 0);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to get irq\n");
		return ret;
	}
	info->rtc_irq = ret;

	info->dev = &pdev->dev;
	parent = info->dev->parent;
	client = to_i2c_client(parent);
	client->flags |= I2C_CLIENT_PEC;
	i2c_set_clientdata(client, info);
	info->client = client;
	info->rtc_irq_chip = &nvvrs_rtc_irq_chip;
	platform_set_drvdata(pdev, info);

	/* Allocate RTC device */
	info->rtc_dev = devm_rtc_allocate_device(info->dev);
	if (IS_ERR(info->rtc_dev))
		return PTR_ERR(info->rtc_dev);

	info->rtc_dev->ops = &nvvrs_rtc_ops;
	info->rtc_dev->range_min = RTC_TIMESTAMP_BEGIN_2000;
	info->rtc_dev->range_max = RTC_TIMESTAMP_END_2099;

	ret = devm_request_threaded_irq(info->dev, info->rtc_irq, NULL,
					nvvrs_rtc_irq_handler, 0, "rtc-alarm", info);
	if (ret < 0)
		dev_err(&pdev->dev, "Failed to request alarm IRQ: %d: %d\n",
			info->rtc_irq, ret);

	/* RTC as a wakeup source */
	device_init_wakeup(info->dev, true);

	return devm_rtc_register_device(info->rtc_dev);
}

#ifdef CONFIG_PM_SLEEP
static int nvvrs_rtc_suspend(struct device *dev)
{
	struct nvvrs_rtc_info *info = dev_get_drvdata(dev);
	int ret = 0;

	if (device_may_wakeup(dev)) {
		/* Set RTC_WAKE bit for auto wake system from suspend state */
		ret = nvvrs_update_bits(info, NVVRS_PSEQ_REG_CTL_2,
					NVVRS_PSEQ_REG_CTL_2_RTC_WAKE,
					NVVRS_PSEQ_REG_CTL_2_RTC_WAKE);
		if (ret < 0) {
			dev_err(info->dev, "Failed to set RTC_WAKE bit (%d)\n", ret);
			return ret;
		}

		ret = enable_irq_wake(info->rtc_irq);
	}

	return ret;
}

static int nvvrs_rtc_resume(struct device *dev)
{
	struct nvvrs_rtc_info *info = dev_get_drvdata(dev);
	int ret;

	if (device_may_wakeup(dev)) {
		/* Clear FORCE_ACT bit */
		ret = nvvrs_update_bits(info, NVVRS_PSEQ_REG_CTL_1,
					NVVRS_PSEQ_REG_CTL_1_FORCE_ACT, 0);
		if (ret < 0) {
			dev_err(info->dev, "Failed to clear FORCE_ACT bit (%d)\n", ret);
			return ret;
		}

		return disable_irq_wake(info->rtc_irq);
	}

	return 0;
}

#endif
static SIMPLE_DEV_PM_OPS(nvvrs_rtc_pm_ops, nvvrs_rtc_suspend, nvvrs_rtc_resume);

static const struct platform_device_id nvvrs_rtc_id[] = {
	{ "nvvrs-pseq-rtc", },
	{ },
};
MODULE_DEVICE_TABLE(platform, nvvrs_rtc_id);

static struct platform_driver nvvrs_rtc_driver = {
	.driver		= {
		.name   = "nvvrs-pseq-rtc",
		.pm     = &nvvrs_rtc_pm_ops,
	},
	.probe		= nvvrs_rtc_probe,
	.id_table       = nvvrs_rtc_id,
};

module_platform_driver(nvvrs_rtc_driver);

MODULE_AUTHOR("Shubhi Garg <shgarg@nvidia.com>");
MODULE_DESCRIPTION("NVVRS PSEQ RTC driver");
MODULE_LICENSE("GPL");