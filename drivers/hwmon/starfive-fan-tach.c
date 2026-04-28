// SPDX-License-Identifier: GPL-2.0
/*
 * FAN-TACH controller driver for StarFive JHB100
 *
 * Copyright (C) 2018-2024 StarFive Technology Co., Ltd.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/sysfs.h>

#define STARFIVE_FAN_TACH_CH			16

/* Fan-tach register offest */
#define STARFIVE_FAN_TACH_STATUS		0x0c

#define STARFIVE_FAN_TACH_SPEED(ch)		(((ch) * 0x04) + 0x10)
#define STARFIVE_FAN_TACH_SPEED_VALID		BIT(31)
#define STARFIVE_FAN_TACH_VALUE_MASK		GENMASK(30, 0)

#define STARFIVE_FAN_TACH_THRESHOLD(ch)		(((ch) * 0x04) + 0x50)

#define STARFIVE_FAN_TACH_INT_EN		0x90
#define STARFIVE_FAN_TACH_STALL_INT_MASK	GENMASK(15, 0)
#define STARFIVE_FAN_TACH_SLOW_INT_MASK		GENMASK(31, 16)
#define STARFIVE_FAN_TACH_STALL_INT(ch)		BIT(ch)
#define STARFIVE_FAN_TACH_SLOW_INT(ch)		(BIT(ch) << 16)

#define STARFIVE_FAN_TACH_MEASURE_TIME		0x94

#define STARFIVE_FAN_TACH_CH_EN			0x98
#define STARFIVE_FAN_TACH_EN(ch)		BIT(ch)

#define STARFIVE_FAN_DEFAULT_PULSE_PR		2
#define STARFIVE_FAN_DEFAULT_MEASURE_RATIO	2
#define STARFIVE_FAN_DEFAULT_RPM_PAUSE_TIME	(60 * STARFIVE_FAN_DEFAULT_MEASURE_RATIO)

#define STARFIVE_FAN_TACH_TIMEOUT \
	(USEC_PER_SEC / STARFIVE_FAN_DEFAULT_MEASURE_RATIO)

#define STARFIVE_FAN_TACH_TIMEOUT_JIFFIES \
	(msecs_to_jiffies(1000) / STARFIVE_FAN_DEFAULT_MEASURE_RATIO)

#define FAN_ATTRIBUTE_SET \
	(HWMON_F_INPUT | HWMON_F_MIN | HWMON_F_ENABLE | \
	 HWMON_F_FAULT | HWMON_F_MIN_ALARM)

struct starfive_fan_tach_data {
	struct device *dev;
	void __iomem *regs;
	bool tach_present[STARFIVE_FAN_TACH_CH];
	u32 clk_rate; /* APB clock frequency */
	struct completion comp_stall[STARFIVE_FAN_TACH_CH];
	struct completion comp_slow[STARFIVE_FAN_TACH_CH];
	u8 fan_stall[STARFIVE_FAN_TACH_CH];
	u8 fan_slow[STARFIVE_FAN_TACH_CH];
};

static void starfive_fan_tach_ch_enable(struct starfive_fan_tach_data *priv, u8 tach_ch,
					bool enable)
{
	if (enable) {
		writel(readl(priv->regs + STARFIVE_FAN_TACH_CH_EN) |
		       STARFIVE_FAN_TACH_EN(tach_ch),
		       priv->regs + STARFIVE_FAN_TACH_CH_EN);
	} else {
		writel(readl(priv->regs + STARFIVE_FAN_TACH_CH_EN) &
		       ~(STARFIVE_FAN_TACH_EN(tach_ch)),
		       priv->regs + STARFIVE_FAN_TACH_CH_EN);
	}
}

static void starfive_fan_tach_ch_stall_unmask(struct starfive_fan_tach_data *priv, u8 tach_ch,
					      bool unmask)
{
	if (unmask) {
		writel(readl(priv->regs + STARFIVE_FAN_TACH_INT_EN) |
		       STARFIVE_FAN_TACH_STALL_INT(tach_ch),
		       priv->regs + STARFIVE_FAN_TACH_INT_EN);
	} else {
		writel(readl(priv->regs + STARFIVE_FAN_TACH_INT_EN) &
		       ~STARFIVE_FAN_TACH_STALL_INT(tach_ch),
		       priv->regs + STARFIVE_FAN_TACH_INT_EN);
	}
}

static void starfive_fan_tach_ch_slow_unmask(struct starfive_fan_tach_data *priv, u8 tach_ch,
					     bool unmask)
{
	if (unmask) {
		writel(readl(priv->regs + STARFIVE_FAN_TACH_INT_EN) |
		       STARFIVE_FAN_TACH_SLOW_INT(tach_ch),
		       priv->regs + STARFIVE_FAN_TACH_INT_EN);
	} else {
		writel(readl(priv->regs + STARFIVE_FAN_TACH_INT_EN) &
		       ~STARFIVE_FAN_TACH_SLOW_INT(tach_ch),
		       priv->regs + STARFIVE_FAN_TACH_INT_EN);
	}
}

static int starfive_fan_tach_rpm_to_val(struct starfive_fan_tach_data *priv, u32 rpm)
{
	u64 tach_val;

	rpm *= STARFIVE_FAN_DEFAULT_PULSE_PR;
	tach_val = rpm / STARFIVE_FAN_DEFAULT_RPM_PAUSE_TIME;

	return (int)tach_val;
}

static int starfive_fan_tach_val_to_rpm(struct starfive_fan_tach_data *priv, u32 tach_val)
{
	u64 rpm;

	rpm = tach_val * STARFIVE_FAN_DEFAULT_RPM_PAUSE_TIME;

	do_div(rpm, STARFIVE_FAN_DEFAULT_PULSE_PR);

	return (int)rpm;
}

static int starfive_fan_tach_get_rpm(struct starfive_fan_tach_data *priv,
				     u8 fan_tach_ch)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout(priv->regs + STARFIVE_FAN_TACH_SPEED(fan_tach_ch),
				 val, val & STARFIVE_FAN_TACH_SPEED_VALID,
				 100, STARFIVE_FAN_TACH_TIMEOUT);
	if (ret)
		return ret;

	val = FIELD_GET(STARFIVE_FAN_TACH_VALUE_MASK, val);

	return starfive_fan_tach_val_to_rpm(priv, val);
}

static int starfive_fan_tach_get_rpm_threshold(struct starfive_fan_tach_data *priv,
					       u8 fan_tach_ch)
{
	u32 val;

	val = readl(priv->regs + STARFIVE_FAN_TACH_THRESHOLD(fan_tach_ch));

	val = FIELD_GET(STARFIVE_FAN_TACH_VALUE_MASK, val);

	return starfive_fan_tach_val_to_rpm(priv, (int)val);
}

static void starfive_set_tach_rpm_threshold(struct starfive_fan_tach_data *priv,
					    u8 fan_tach_ch, u32 val)
{
	val = clamp_val(starfive_fan_tach_rpm_to_val(priv, val), 1, STARFIVE_FAN_TACH_VALUE_MASK);

	writel(val, priv->regs + STARFIVE_FAN_TACH_THRESHOLD(fan_tach_ch));
}

static int starfive_fan_tach_get_ch_en(struct starfive_fan_tach_data *priv,
				       u8 fan_tach_ch)
{
	u32 enable;

	enable = readl(priv->regs + STARFIVE_FAN_TACH_CH_EN);

	return (enable & (1 << fan_tach_ch)) ? 1 : 0;
}

static void starfive_fan_tach_set_ch_en(struct starfive_fan_tach_data *priv,
					u8 fan_tach_ch, u32 val)
{
	bool enable;

	switch (val) {
	case 0:
		enable = false;
		break;
	case 1:
		enable = true;
		break;
	default:
		return;
	}

	starfive_fan_tach_ch_enable(priv, fan_tach_ch, enable);
}

static int starfive_fan_tach_hwmon_read(struct device *dev,
					enum hwmon_sensor_types type, u32 attr,
					int channel, long *val)
{
	struct starfive_fan_tach_data *priv = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_fan_fault:
		starfive_fan_tach_ch_stall_unmask(priv, channel, true);
		/* clear fan_stall first */
		priv->fan_stall[channel] = 0;
		reinit_completion(&priv->comp_stall[channel]);
		wait_for_completion_timeout(&priv->comp_stall[channel],
					    2 * STARFIVE_FAN_TACH_TIMEOUT_JIFFIES);
		*val = priv->fan_stall[channel];
		starfive_fan_tach_ch_stall_unmask(priv, channel, false);
		break;
	case hwmon_fan_input:
		*val = starfive_fan_tach_get_rpm(priv, channel);
		break;
	case hwmon_fan_min:
		*val = starfive_fan_tach_get_rpm_threshold(priv, channel);
		break;
	case hwmon_fan_min_alarm:
		starfive_fan_tach_ch_slow_unmask(priv, channel, true);
		/* clear fan_slow first */
		priv->fan_slow[channel] = 0;
		reinit_completion(&priv->comp_slow[channel]);
		wait_for_completion_timeout(&priv->comp_stall[channel],
					    2 * STARFIVE_FAN_TACH_TIMEOUT_JIFFIES);
		*val = priv->fan_slow[channel];
		starfive_fan_tach_ch_slow_unmask(priv, channel, false);
		break;
	case hwmon_fan_enable:
		*val = starfive_fan_tach_get_ch_en(priv, channel);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int starfive_fan_tach_hwmon_write(struct device *dev,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel, long val)
{
	struct starfive_fan_tach_data *priv = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_fan_min:
		starfive_set_tach_rpm_threshold(priv, channel, (u32)val);
		break;
	case hwmon_fan_enable:
		starfive_fan_tach_set_ch_en(priv, channel, (u32)val);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static umode_t starfive_fan_tach_dev_is_visible(const void *drvdata,
						enum hwmon_sensor_types type,
						u32 attr, int channel)
{
	const struct starfive_fan_tach_data *priv = drvdata;

	if (!priv->tach_present[channel])
		return 0;

	switch (attr) {
	case hwmon_fan_input:
	case hwmon_fan_fault:
	case hwmon_fan_min_alarm:
		return 0444;
	case hwmon_fan_min:
	case hwmon_fan_pulses:
	case hwmon_fan_enable:
		return 0644;
	}

	return 0;
}

static const struct hwmon_ops starfive_fan_tach_ops = {
	.is_visible = starfive_fan_tach_dev_is_visible,
	.read = starfive_fan_tach_hwmon_read,
	.write = starfive_fan_tach_hwmon_write
};

static const struct hwmon_channel_info *starfive_fan_tach_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   FAN_ATTRIBUTE_SET, FAN_ATTRIBUTE_SET,
			   FAN_ATTRIBUTE_SET, FAN_ATTRIBUTE_SET,
			   FAN_ATTRIBUTE_SET, FAN_ATTRIBUTE_SET,
			   FAN_ATTRIBUTE_SET, FAN_ATTRIBUTE_SET,
			   FAN_ATTRIBUTE_SET, FAN_ATTRIBUTE_SET,
			   FAN_ATTRIBUTE_SET, FAN_ATTRIBUTE_SET,
			   FAN_ATTRIBUTE_SET, FAN_ATTRIBUTE_SET,
			   FAN_ATTRIBUTE_SET, FAN_ATTRIBUTE_SET),
	NULL
};

static const struct hwmon_chip_info starfive_fan_tach_chip_info = {
	.ops = &starfive_fan_tach_ops,
	.info = starfive_fan_tach_info,
};

static void starfive_fan_tach_present(struct starfive_fan_tach_data *priv, u8 *tach_ch,
				      int count)
{
	u8 ch, index;

	for (index = 0; index < count; index++) {
		ch = tach_ch[index];
		priv->tach_present[ch] = true;

		starfive_fan_tach_ch_enable(priv, ch, true);
		init_completion(&priv->comp_stall[ch]);
		init_completion(&priv->comp_slow[ch]);
	}
}

static int starfive_fan_tach_create_fan(struct device *dev, struct device_node *child,
					struct starfive_fan_tach_data *priv)
{
	int ret, count;
	u8 *tach_ch;

	count = of_property_count_u8_elems(child, "tach-ch");
	if (count < 1)
		return -EINVAL;

	tach_ch = devm_kcalloc(dev, count, sizeof(*tach_ch), GFP_KERNEL);
	if (!tach_ch)
		return -ENOMEM;

	ret = of_property_read_u8_array(child, "tach-ch", tach_ch, count);
	if (ret)
		return ret;

	starfive_fan_tach_present(priv, tach_ch, count);

	return 0;
}

static irqreturn_t starfive_fan_tach_irq(int irq, void *dev_id)
{
	struct starfive_fan_tach_data *priv = dev_id;
	u32 fan_status;
	int i;

	fan_status = readl(priv->regs + STARFIVE_FAN_TACH_STATUS);

	for (i = 0; i < STARFIVE_FAN_TACH_CH; i++) {
		if (fan_status & STARFIVE_FAN_TACH_STALL_INT(i)) {
			priv->fan_stall[i] = 1;
			complete(&priv->comp_stall[i]);
		}

		if (fan_status & STARFIVE_FAN_TACH_SLOW_INT(i)) {
			priv->fan_slow[i] = 1;
			complete(&priv->comp_slow[i]);
		}
	}

	writel(fan_status, priv->regs + STARFIVE_FAN_TACH_STATUS);

	return IRQ_HANDLED;
}

static void starfive_fan_tach_reset_control_assert(void *data)
{
	reset_control_assert(data);
}

static int starfive_fan_tach_probe(struct platform_device *pdev)
{
	struct device *hwmon_dev, *dev = &pdev->dev;
	struct starfive_fan_tach_data *priv;
	struct reset_control *rst;
	struct clk *clk;
	int irq;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "Unable to map IO resources\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return dev_err_probe(dev, irq, "Failed to get IRQ\n");

	ret = devm_request_irq(dev, irq, starfive_fan_tach_irq, 0, pdev->name,
			       (void *)priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register interrupt handler\n");

	rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst),
				     "Unable to get fan tach's reset\n");

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Unable to get & enable fan tach's clock\n");

	priv->clk_rate = clk_get_rate(clk);
	if (priv->clk_rate <= 0)
		return dev_err_probe(dev, priv->clk_rate,
				     "Unable to get clock's rate\n");

	reset_control_deassert(rst);
	ret = devm_add_action_or_reset(dev, starfive_fan_tach_reset_control_assert, rst);
	if (ret)
		return ret;

	writel(STARFIVE_FAN_TACH_STALL_INT_MASK | STARFIVE_FAN_TACH_SLOW_INT_MASK,
	       priv->regs + STARFIVE_FAN_TACH_STATUS);
	writel(priv->clk_rate / STARFIVE_FAN_DEFAULT_MEASURE_RATIO,
	       priv->regs + STARFIVE_FAN_TACH_MEASURE_TIME);

	for_each_child_of_node_scoped(dev->of_node, child) {
		ret = starfive_fan_tach_create_fan(dev, child, priv);
		if (ret < 0) {
			dev_warn(dev, "Failed to create fan %d", ret);
			return 0;
		}
	}

	platform_set_drvdata(pdev, priv);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "starfive_fan_tach",
							 priv, &starfive_fan_tach_chip_info,
							 NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id starfive_fan_tach_of_match[] = {
	{ .compatible = "starfive,jhb100-fan-tach", },
	{ /* sentinel */ }
};

static struct platform_driver starfive_fan_tach_driver = {
	.probe = starfive_fan_tach_probe,
	.driver	= {
		.name = "starfive-fan-tach",
		.of_match_table = starfive_fan_tach_of_match,
	},
};

module_platform_driver(starfive_fan_tach_driver);

MODULE_AUTHOR("William Qiu <william.qiu@starfivetech.com>");
MODULE_AUTHOR("Changhuang Liang <changhuang.liang@starfivetech.com>");
MODULE_DESCRIPTION("StarFive JHB100 Fan Tach device driver");
MODULE_LICENSE("GPL");
