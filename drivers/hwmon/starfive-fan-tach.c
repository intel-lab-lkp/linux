// SPDX-License-Identifier: GPL-2.0
/*
 * FAN-TACH controller driver for StarFive JHB100
 *
 * Copyright (C) 2018-2026 StarFive Technology Co., Ltd.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

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
#define STARFIVE_FAN_DEFAULT_MEASURE_RATIO	10
#define STARFIVE_FAN_DEFAULT_RPM_PAUSE_TIME	(60 * STARFIVE_FAN_DEFAULT_MEASURE_RATIO)

#define STARFIVE_FAN_TACH_TIMEOUT \
	(USEC_PER_SEC / STARFIVE_FAN_DEFAULT_MEASURE_RATIO)

#define STARFIVE_FAN_TACH_TIMEOUT_JIFFIES \
	(msecs_to_jiffies(1000) / STARFIVE_FAN_DEFAULT_MEASURE_RATIO)

#define FAN_ATTRIBUTE_SET \
	(HWMON_F_INPUT | HWMON_F_MIN | HWMON_F_ENABLE | \
	 HWMON_F_FAULT | HWMON_F_MIN_ALARM)

struct starfive_fan_tach_data {
	void __iomem *regs;
	struct reset_control *rst;
	struct clk *clk;
	bool tach_present[STARFIVE_FAN_TACH_CH];
	unsigned long clk_rate; /* APB clock frequency */
	u8 pulses_per_rev[STARFIVE_FAN_TACH_CH];
	struct completion comp_stall[STARFIVE_FAN_TACH_CH];
	struct completion comp_slow[STARFIVE_FAN_TACH_CH];
	u8 fan_stall[STARFIVE_FAN_TACH_CH];
	u8 fan_slow[STARFIVE_FAN_TACH_CH];
	u32 armed_stall;
	u32 armed_slow;
	/* protects fan_stall[]/fan_slow[]/armed_* and the STATUS register */
	spinlock_t lock;
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

static u32 starfive_fan_tach_rpm_to_val(struct starfive_fan_tach_data *priv, u8 tach_ch, u32 rpm)
{
	u64 tach_val;
	u8 ppr;

	ppr = priv->pulses_per_rev[tach_ch];

	tach_val = (u64)rpm * ppr;
	tach_val = div_u64(tach_val, STARFIVE_FAN_DEFAULT_RPM_PAUSE_TIME);

	return min_t(u64, tach_val, STARFIVE_FAN_TACH_VALUE_MASK);
}

static long starfive_fan_tach_val_to_rpm(struct starfive_fan_tach_data *priv, u8 tach_ch,
					 u32 tach_val)
{
	u64 rpm;
	u8 ppr;

	ppr = priv->pulses_per_rev[tach_ch];

	rpm = (u64)tach_val * STARFIVE_FAN_DEFAULT_RPM_PAUSE_TIME;

	return div_u64(rpm, ppr);
}

static int starfive_fan_tach_get_rpm(struct starfive_fan_tach_data *priv,
				     u8 tach_ch, long *rpm)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout(priv->regs + STARFIVE_FAN_TACH_SPEED(tach_ch),
				 val, val & STARFIVE_FAN_TACH_SPEED_VALID,
				 100, STARFIVE_FAN_TACH_TIMEOUT);
	if (ret)
		return -ENODATA;

	val = FIELD_GET(STARFIVE_FAN_TACH_VALUE_MASK, val);
	if (!val) {
		/* Sampling = 0 may mean no valid capture; wait 1.5 more cycles for valid read. */
		fsleep(STARFIVE_FAN_TACH_TIMEOUT + STARFIVE_FAN_TACH_TIMEOUT / 2);
		ret = readl_poll_timeout(priv->regs + STARFIVE_FAN_TACH_SPEED(tach_ch),
					 val, val & STARFIVE_FAN_TACH_SPEED_VALID,
					 100, STARFIVE_FAN_TACH_TIMEOUT);
		if (ret)
			return -ENODATA;

		val = FIELD_GET(STARFIVE_FAN_TACH_VALUE_MASK, val);
	}

	*rpm = starfive_fan_tach_val_to_rpm(priv, tach_ch, val);

	return 0;
}

static long starfive_fan_tach_get_rpm_threshold(struct starfive_fan_tach_data *priv,
						u8 tach_ch)
{
	u32 val;

	val = readl(priv->regs + STARFIVE_FAN_TACH_THRESHOLD(tach_ch));

	val = FIELD_GET(STARFIVE_FAN_TACH_VALUE_MASK, val);

	return starfive_fan_tach_val_to_rpm(priv, tach_ch, val);
}

static void starfive_set_tach_rpm_threshold(struct starfive_fan_tach_data *priv,
					    u8 tach_ch, u32 val)
{
	u32 reg_val;

	reg_val = starfive_fan_tach_rpm_to_val(priv, tach_ch, val);
	reg_val = clamp_val(reg_val, 1, STARFIVE_FAN_TACH_VALUE_MASK);

	writel(reg_val, priv->regs + STARFIVE_FAN_TACH_THRESHOLD(tach_ch));
}

static bool starfive_fan_tach_get_ch_en(struct starfive_fan_tach_data *priv,
					u8 tach_ch)
{
	u32 enable;

	enable = readl(priv->regs + STARFIVE_FAN_TACH_CH_EN);

	return !!(enable & STARFIVE_FAN_TACH_EN(tach_ch));
}

static int starfive_fan_tach_hwmon_read(struct device *dev,
					enum hwmon_sensor_types type, u32 attr,
					int channel, long *val)
{
	struct starfive_fan_tach_data *priv = dev_get_drvdata(dev);
	int ret = 0;

	switch (attr) {
	case hwmon_fan_fault:
		scoped_guard(spinlock_irqsave, &priv->lock) {
			writel(STARFIVE_FAN_TACH_STALL_INT(channel),
			       priv->regs + STARFIVE_FAN_TACH_STATUS);
			/* clear fan_stall first */
			priv->fan_stall[channel] = 0;
			reinit_completion(&priv->comp_stall[channel]);
			priv->armed_stall |= BIT(channel);
		}

		starfive_fan_tach_ch_stall_unmask(priv, channel, true);

		/* Waiting for hardware to measure */
		wait_for_completion_timeout(&priv->comp_stall[channel],
					    2 * STARFIVE_FAN_TACH_TIMEOUT_JIFFIES);

		starfive_fan_tach_ch_stall_unmask(priv, channel, false);

		scoped_guard(spinlock_irqsave, &priv->lock) {
			priv->armed_stall &= ~BIT(channel);
			*val = priv->fan_stall[channel];
		}

		break;
	case hwmon_fan_input:
		if (!starfive_fan_tach_get_ch_en(priv, channel))
			return -ENODATA;

		ret = starfive_fan_tach_get_rpm(priv, channel, val);
		if (ret < 0)
			return ret;

		break;
	case hwmon_fan_min:
		*val = starfive_fan_tach_get_rpm_threshold(priv, channel);

		break;
	case hwmon_fan_min_alarm:
		scoped_guard(spinlock_irqsave, &priv->lock) {
			writel(STARFIVE_FAN_TACH_SLOW_INT(channel),
			       priv->regs + STARFIVE_FAN_TACH_STATUS);
			/* clear fan_slow first */
			priv->fan_slow[channel] = 0;
			reinit_completion(&priv->comp_slow[channel]);
			priv->armed_slow |= BIT(channel);
		}

		starfive_fan_tach_ch_slow_unmask(priv, channel, true);

		/* Waiting for hardware to measure */
		wait_for_completion_timeout(&priv->comp_slow[channel],
					    2 * STARFIVE_FAN_TACH_TIMEOUT_JIFFIES);

		starfive_fan_tach_ch_slow_unmask(priv, channel, false);

		scoped_guard(spinlock_irqsave, &priv->lock) {
			priv->armed_slow &= ~BIT(channel);
			*val = priv->fan_slow[channel];
		}

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
		if (val < 0 || val > U32_MAX)
			return -EINVAL;

		starfive_set_tach_rpm_threshold(priv, channel, val);
		break;
	case hwmon_fan_enable:
		if (val != 0 && val != 1)
			return -EINVAL;

		starfive_fan_tach_ch_enable(priv, channel, val);
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

static int starfive_fan_tach_create_fan(struct device *dev, struct device_node *child,
					struct starfive_fan_tach_data *priv)
{
	u8 tach_ch[STARFIVE_FAN_TACH_CH];
	int ret, count;
	u32 ppr, index;

	count = of_property_count_u8_elems(child, "tach-ch");
	if (count < 1 || count > STARFIVE_FAN_TACH_CH)
		return -EINVAL;

	ret = of_property_read_u8_array(child, "tach-ch", tach_ch, count);
	if (ret)
		return ret;

	/* Parse pulses-per-revolution, default to 2 if not specified */
	ppr = STARFIVE_FAN_DEFAULT_PULSE_PR;
	of_property_read_u32(child, "pulses-per-revolution", &ppr);

	if (!ppr || ppr > 4)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid pulses-per-revolution %u, must be 1-4\n", ppr);

	for (index = 0; index < count; index++) {
		u8 ch = tach_ch[index];

		if (ch >= STARFIVE_FAN_TACH_CH)
			return dev_err_probe(dev, -EINVAL, "Invalid tach-ch %d\n", ch);

		priv->pulses_per_rev[ch] = ppr;
		priv->tach_present[ch] = true;
		starfive_fan_tach_ch_enable(priv, ch, true);
	}

	return 0;
}

static irqreturn_t starfive_fan_tach_irq(int irq, void *dev_id)
{
	struct starfive_fan_tach_data *priv = dev_id;
	u32 fan_status;
	int i;

	guard(spinlock)(&priv->lock);

	fan_status = readl(priv->regs + STARFIVE_FAN_TACH_STATUS);
	if (!fan_status)
		return IRQ_NONE;

	writel(fan_status, priv->regs + STARFIVE_FAN_TACH_STATUS);

	for (i = 0; i < STARFIVE_FAN_TACH_CH; i++) {
		if (!priv->tach_present[i])
			continue;

		if ((fan_status & STARFIVE_FAN_TACH_STALL_INT(i)) &&
		    (priv->armed_stall & BIT(i))) {
			priv->fan_stall[i] = 1;
			complete(&priv->comp_stall[i]);
		}

		if ((fan_status & STARFIVE_FAN_TACH_SLOW_INT(i)) &&
		    (priv->armed_slow & BIT(i))) {
			priv->fan_slow[i] = 1;
			complete(&priv->comp_slow[i]);
		}
	}

	return IRQ_HANDLED;
}

static void starfive_fan_tach_disable(void *data)
{
	struct starfive_fan_tach_data *priv = data;

	clk_disable_unprepare(priv->clk);
	reset_control_assert(priv->rst);
}

static int starfive_fan_tach_probe(struct platform_device *pdev)
{
	struct device *hwmon_dev, *dev = &pdev->dev;
	struct starfive_fan_tach_data *priv;
	int irq;
	int ret;
	u32 i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spin_lock_init(&priv->lock);

	for (i = 0; i < STARFIVE_FAN_TACH_CH; i++) {
		init_completion(&priv->comp_stall[i]);
		init_completion(&priv->comp_slow[i]);
	}

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "Unable to map IO resources\n");

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "Unable to get fan tach's clock\n");

	priv->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(priv->rst))
		return dev_err_probe(dev, PTR_ERR(priv->rst),
				     "Unable to get fan tach's reset\n");

	ret = clk_prepare_enable(priv->clk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable clock\n");

	priv->clk_rate = clk_get_rate(priv->clk);
	if (!priv->clk_rate) {
		clk_disable_unprepare(priv->clk);
		return dev_err_probe(dev, -EINVAL, "Unable to get clock's rate\n");
	}

	ret = reset_control_deassert(priv->rst);
	if (ret) {
		clk_disable_unprepare(priv->clk);
		return dev_err_probe(dev, ret, "Failed to deassert reset\n");
	}

	ret = devm_add_action_or_reset(dev, starfive_fan_tach_disable, priv);
	if (ret)
		return ret;

	writel(STARFIVE_FAN_TACH_STALL_INT_MASK | STARFIVE_FAN_TACH_SLOW_INT_MASK,
	       priv->regs + STARFIVE_FAN_TACH_STATUS);
	writel(priv->clk_rate / STARFIVE_FAN_DEFAULT_MEASURE_RATIO,
	       priv->regs + STARFIVE_FAN_TACH_MEASURE_TIME);

	for_each_child_of_node_scoped(dev->of_node, child) {
		ret = starfive_fan_tach_create_fan(dev, child, priv);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to create fan %pOF\n", child);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return dev_err_probe(dev, irq, "Failed to get IRQ\n");

	ret = devm_request_irq(dev, irq, starfive_fan_tach_irq, 0, pdev->name, priv);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "starfive_fan_tach",
							 priv, &starfive_fan_tach_chip_info,
							 NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id starfive_fan_tach_of_match[] = {
	{ .compatible = "starfive,jhb100-fan-tach", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, starfive_fan_tach_of_match);

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
