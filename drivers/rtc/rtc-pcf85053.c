// SPDX-License-Identifier: GPL-2.0
// Copyright 2025 NXP

#include <linux/bcd.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/pm_wakeirq.h>
#include <linux/regmap.h>

#define PCF85053_REG_SC		0x00 /* seconds */
#define PCF85053_REG_SCA	0x01 /* alarm */
#define PCF85053_REG_MN		0x02 /* minutes */
#define PCF85053_REG_MNA	0x03 /* alarm */
#define PCF85053_REG_HR		0x04 /* hour */
#define PCF85053_REG_HRA	0x05 /* alarm */
#define PCF85053_REG_DW		0x06 /* day of week */
#define PCF85053_REG_DM		0x07 /* day of month */
#define PCF85053_REG_MO		0x08 /* month */
#define PCF85053_REG_YR		0x09 /* year */
#define PCF85053_REG_CTRL	0x0A /* timer control */
#define PCF85053_REG_ST		0x0B /* status */
#define PCF85053_REG_CLKO	0x0C /* clock out */
#define PCF85053_REG_ACC	0x14 /* xclk access */

#define PCF85053_BIT_AF		BIT(7)
#define PCF85053_BIT_ST		BIT(7)
#define PCF85053_BIT_DM		BIT(6)
#define PCF85053_BIT_HF		BIT(5)
#define PCF85053_BIT_DSM	BIT(4)
#define PCF85053_BIT_AIE	BIT(3)
#define PCF85053_BIT_OFIE	BIT(2)
#define PCF85053_BIT_CIE	BIT(1)
#define PCF85053_BIT_TWO	BIT(0)
#define PCF85053_BIT_XCLK	BIT(7)

#define PCF85053_REG_BAT_MASK	0x07 /* Battery mask */
#define PCF85053A_BVL_MASK 0x07
#define PCF85053A_BVL_LOW_THRESHOLD 0x02
#define PCF85053_REG_CLKO_F_MASK	0x03 /* Frequenc mask */
#define PCF85053_REG_CLKO_CKE	0x80 /* clock out enabled */
#define PCF85053_BIT_OF	BIT(6)

#define PCF85053_HR_PM	BIT(7)
#define PCF85053_HR_24H_MASK	GENMASK(5, 0)

struct pcf85053_config {
	const struct regmap_config regmap;
	unsigned has_alarms:1;
};

struct pcf85053 {
	struct rtc_device *rtc;
	struct regmap	*regmap;
#ifdef CONFIG_COMMON_CLK
	struct clk_hw clkout_hw;
#endif
	bool is_primary;
};

static inline int pcf85053_read_two_bit(struct pcf85053 *pcf85053, bool *two)
{
	unsigned int ctrl;
	int err;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CTRL, &ctrl);
	if (err)
		return err;

	*two = !!(ctrl & PCF85053_BIT_TWO);

	return 0;
}

static inline bool pcf85053_time_write_access(struct pcf85053 *pcf85053)
{
	bool two;

	if (pcf85053_read_two_bit(pcf85053, &two))
		return false;

	/* Primary writes iff TWO=1; secondary writes iff TWO=0 */
	return pcf85053->is_primary ? two : !two;
}

static int pcf85053_set_alarm_mode(struct regmap *regmap, bool on)
{
	unsigned int val;
	int err;

	val = on ? PCF85053_BIT_AIE : 0;

	err = regmap_update_bits(regmap, PCF85053_REG_CTRL,
				 PCF85053_BIT_AIE,
				 val);
	if (err)
		return err;

	return regmap_update_bits(regmap, PCF85053_REG_ST,
				  PCF85053_BIT_AF, 0);
}

static int pcf85053_get_alarm_mode(struct device *dev,
				   unsigned char *alarm_enable, unsigned char *alarm_flag)
{
	struct pcf85053 *pcf85053 = dev_get_drvdata(dev);
	unsigned int val;
	int err;

	if (alarm_enable) {
		err = regmap_read(pcf85053->regmap, PCF85053_REG_CTRL, &val);
		if (err)
			return err;

		*alarm_enable = val & PCF85053_BIT_AIE;
	}

	if (alarm_flag) {
		err = regmap_read(pcf85053->regmap, PCF85053_REG_ST, &val);
		if (err)
			return err;

		*alarm_flag = val & PCF85053_BIT_AF;
	}

	return 0;
}

static irqreturn_t pcf85053_irq(int irq, void *dev_id)
{
	struct device *dev = dev_id;
	struct pcf85053 *pcf85053 = dev_get_drvdata(dev);
	bool changed;
	int err;

	err = regmap_update_bits_check(pcf85053->regmap,
				       PCF85053_REG_ST,
				       PCF85053_BIT_AF, 0,
				       &changed);

	if (err || !changed)
		return IRQ_NONE;

	rtc_update_irq(pcf85053->rtc, 1, RTC_IRQF | RTC_AF);
	return IRQ_HANDLED;
}

/*
 * In the routines that deal directly with the PCF85053 hardware, we use
 * rtc_time -- month 0-11, hour 0-23, yr = calendar year-epoch.
 */
static int pcf85053_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct pcf85053 *pcf85053 = dev_get_drvdata(dev);
	unsigned int ctrl, st, h12;
	bool is_24h, is_bin;
	u8 regs[10], hr;
	int err;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CTRL, &ctrl);
	if (err)
		return err;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_ST, &st);
	if (err)
		return err;

	if (ctrl & PCF85053_BIT_ST)
		return -EINVAL;

	err = regmap_bulk_read(pcf85053->regmap, PCF85053_REG_SC, regs, sizeof(regs));
	if (err)
		return err;

	if (ctrl & PCF85053_BIT_DM) {
		tm->tm_sec = regs[PCF85053_REG_SC] & 0x7F;
		tm->tm_min = regs[PCF85053_REG_MN] & 0x7F;
		tm->tm_mday = regs[PCF85053_REG_DM] & 0x3F;
		tm->tm_mon = (regs[PCF85053_REG_MO] & 0x1F) - 1;
		tm->tm_year = regs[PCF85053_REG_YR] + 100;
	} else {
		tm->tm_sec = bcd2bin(regs[PCF85053_REG_SC] & 0x7F);
		tm->tm_min = bcd2bin(regs[PCF85053_REG_MN] & 0x7F);
		tm->tm_mday = bcd2bin(regs[PCF85053_REG_DM] & 0x3F);
		tm->tm_mon = bcd2bin(regs[PCF85053_REG_MO] & 0x1F) - 1;
		tm->tm_year = bcd2bin(regs[PCF85053_REG_YR]) + 100;
	}
	tm->tm_wday = regs[PCF85053_REG_DW] & 0x07;

	hr = regs[PCF85053_REG_HR];
	is_24h = ctrl & PCF85053_BIT_HF;
	is_bin = ctrl & PCF85053_BIT_DM;

	if (is_24h) {
		tm->tm_hour = is_bin
		? (hr & PCF85053_HR_24H_MASK)
		: bcd2bin(hr & PCF85053_HR_24H_MASK);
	} else {
		h12 = is_bin ? (hr & PCF85053_HR_24H_MASK)
			     : bcd2bin(hr & PCF85053_HR_24H_MASK);

		tm->tm_hour = (h12 == 12) ? ((hr & PCF85053_HR_PM) ? 12 : 0) :
			       ((hr & PCF85053_HR_PM) ? h12 + 12 : h12);
	}

	return 0;
}

static int pcf85053_rtc_set_time(struct device *dev, struct rtc_time *tm)

{
	struct pcf85053 *pcf85053 = dev_get_drvdata(dev);
	unsigned int ctrl;
	int err, ret;
	u8 buf[10];

	/*
	 * By default, secondary have write access to time registers as TWO
	 * bit is 0 by default, if we set nxp,interface = "primary" and the
	 * nxp,write-access in device tree, then TWO bits gets set and primary
	 * gets write access to time registers.
	 */
	if (!pcf85053_time_write_access(pcf85053))
		return -EACCES;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CTRL, &ctrl);
	if (err)
		return err;

	/*
	 * HF = 1, 24-hour mode
	 * DM = 1, binary (not BCD)
	 */
	if (!(ctrl & PCF85053_BIT_HF) ||
	    !(ctrl & PCF85053_BIT_DM)) {
		err = regmap_update_bits(pcf85053->regmap, PCF85053_REG_CTRL,
					 PCF85053_BIT_HF | PCF85053_BIT_DM,
					 PCF85053_BIT_HF | PCF85053_BIT_DM);
		if (err)
			return err;
	}

	buf[0] = tm->tm_sec & 0x7F;
	buf[1] = 0;
	buf[2] = tm->tm_min & 0x7F;
	buf[3] = 0;
	buf[4] = tm->tm_hour & PCF85053_HR_24H_MASK;
	buf[5] = 0;
	buf[6] = tm->tm_wday & 0x07;
	buf[7] = tm->tm_mday & 0x3F;
	buf[8] = (tm->tm_mon + 1) & 0x1F;
	buf[9] = (tm->tm_year - 100) & 0xFF;

	if (pcf85053->is_primary) {
		err = regmap_update_bits(pcf85053->regmap, PCF85053_REG_CTRL,
					 PCF85053_BIT_ST, PCF85053_BIT_ST);
		if (err)
			return err;

		ret = regmap_bulk_write(pcf85053->regmap, PCF85053_REG_SC, buf, sizeof(buf));
		err = regmap_update_bits(pcf85053->regmap, PCF85053_REG_CTRL,
					 PCF85053_BIT_ST, 0);
		return ret ? ret : err;
	}

	return regmap_bulk_write(pcf85053->regmap, PCF85053_REG_SC, buf, sizeof(buf));
}

static int pcf85053_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *tm)
{
	struct pcf85053 *pcf85053 = dev_get_drvdata(dev);
	unsigned int ctrl, h12;
	bool is_24h, is_bin, pm;
	u8 buf[5];
	u8 hr;
	int err;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CTRL, &ctrl);
	if (err)
		return err;

	err = regmap_bulk_read(pcf85053->regmap, PCF85053_REG_SCA, buf, sizeof(buf));
	if (err)
		return err;

	if (ctrl & PCF85053_BIT_DM) {
		tm->time.tm_sec = buf[0] & 0x7F; /* SCA */
		tm->time.tm_min = buf[2] & 0x7F; /* MNA */
	} else {
		tm->time.tm_sec = bcd2bin(buf[0] & 0x7F);
		tm->time.tm_min = bcd2bin(buf[2] & 0x7F);
	}

	hr = buf[4];
	is_24h = !!(ctrl & PCF85053_BIT_HF);
	is_bin = !!(ctrl & PCF85053_BIT_DM);

	if (is_24h) {
		tm->time.tm_hour = is_bin
		? (hr & PCF85053_HR_24H_MASK)
		: bcd2bin(hr & PCF85053_HR_24H_MASK);
	} else {
		pm = !!(hr & PCF85053_HR_PM);

		if (is_bin)
			h12 = (hr & PCF85053_HR_24H_MASK);
		else
			h12 = (bcd2bin(hr & PCF85053_HR_24H_MASK));

		if (h12 == 12)
			h12 = 0;
		tm->time.tm_hour = pm ? (h12 + 12) : h12;
	}

	return pcf85053_get_alarm_mode(dev, &tm->enabled, &tm->pending);
}

static int pcf85053_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *tm)
{
	struct pcf85053 *pcf85053 = dev_get_drvdata(dev);
	unsigned int ctrl;
	u8 sec, min, hr;
	int err;

	/*
	 * Only primary can set alarm, as secondary have read only access
	 * to alarm, control and status registers
	 */
	if (!pcf85053->is_primary)
		return -EACCES;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CTRL, &ctrl);
	if (err)
		return err;

	/*
	 * HF = 1, 24-hour mode
	 * DM = 1, binary (not BCD)
	 */
	if (!(ctrl & PCF85053_BIT_HF) ||
	    !(ctrl & PCF85053_BIT_DM)) {
		err = regmap_update_bits(pcf85053->regmap, PCF85053_REG_CTRL,
					 PCF85053_BIT_HF | PCF85053_BIT_DM,
					 PCF85053_BIT_HF | PCF85053_BIT_DM);
		if (err)
			return err;
	}

	err = regmap_update_bits(pcf85053->regmap, PCF85053_REG_ST,
				 PCF85053_BIT_AF, 0);
	if (err)
		return err;

	sec = tm->time.tm_sec & 0x7F;
	min = tm->time.tm_min & 0x7F;
	hr  = tm->time.tm_hour & PCF85053_HR_24H_MASK;

	err = regmap_write(pcf85053->regmap, PCF85053_REG_SCA, sec);
	if (err)
		return err;

	err = regmap_write(pcf85053->regmap, PCF85053_REG_MNA, min);
	if (err)
		return err;

	err = regmap_write(pcf85053->regmap, PCF85053_REG_HRA, hr);
	if (err)
		return err;

	return pcf85053_set_alarm_mode(pcf85053->regmap, tm->enabled);
}

static int pcf85053_irq_enable(struct device *dev, unsigned int enabled)
{
	struct pcf85053 *pcf85053 = dev_get_drvdata(dev);

	dev_dbg(dev, "%s: alarm enable=%d\n", __func__, enabled);

	return pcf85053_set_alarm_mode(pcf85053->regmap, enabled);
}

static int pcf85053_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	struct pcf85053 *pcf85053 = dev_get_drvdata(dev);
	unsigned int val = 0, vl_status = 0;
	unsigned int bvl;
	int status;

	switch (cmd) {
	case RTC_VL_READ:
		status = regmap_read(pcf85053->regmap, PCF85053_REG_ST, &val);
		if (status)
			return status;

		if (val & PCF85053_BIT_OF)
			vl_status |= RTC_VL_DATA_INVALID;

		bvl = val & PCF85053A_BVL_MASK;

		if (bvl == 0x00)
			vl_status |= RTC_VL_BACKUP_EMPTY;
		else if (bvl <= PCF85053A_BVL_LOW_THRESHOLD)
			vl_status |= RTC_VL_BACKUP_LOW;

		return put_user(vl_status, (unsigned int __user *)arg);

	default:
		return -ENOIOCTLCMD;
	}
}

#ifdef CONFIG_COMMON_CLK
/*
 * Handling of the clkout
 */

#define clkout_hw_to_pcf85053(_hw) container_of(_hw, struct pcf85053, clkout_hw)

static const int clkout_rates[] = {
	32768,
	1024,
	32,
	1,
};

static unsigned long pcf85053_clkout_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct pcf85053 *pcf85053 = clkout_hw_to_pcf85053(hw);
	unsigned int val = 0;
	int err;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CLKO, &val);
	if (err)
		return 0;

	val &= PCF85053_REG_CLKO_F_MASK;
	return clkout_rates[val];
}

static int pcf85053_clkout_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	int i;
	unsigned long best = 0;

	for (i = 0; i < ARRAY_SIZE(clkout_rates); i++) {
		if (clkout_rates[i] <= req->rate) {
			best = clkout_rates[i];
			break;
		}
	}
	if (!best)
		best = clkout_rates[ARRAY_SIZE(clkout_rates) - 1];

	req->rate = best;
	return 0;
}

static int pcf85053_clkout_set_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	struct pcf85053 *pcf85053 = clkout_hw_to_pcf85053(hw);
	unsigned int val = 0;
	int err, i;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CLKO, &val);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(clkout_rates); i++)
		if (clkout_rates[i] == rate) {
			val &= ~PCF85053_REG_CLKO_F_MASK;
			val |= i;
			return regmap_write(pcf85053->regmap, PCF85053_REG_CLKO, val);
		}

	return -EINVAL;
}

static int pcf85053_clkout_control(struct clk_hw *hw, bool enable)
{
	struct pcf85053 *pcf85053 = clkout_hw_to_pcf85053(hw);
	unsigned int val = 0;
	int err;

	if (!pcf85053->is_primary)
		return -EACCES;

	val = PCF85053_BIT_XCLK;
	err = regmap_write(pcf85053->regmap, PCF85053_REG_ACC, val);
	if (err)
		return err;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CLKO, &val);
	if (err)
		return err;

	if (enable)
		val |= PCF85053_REG_CLKO_CKE;
	else
		val &= ~PCF85053_REG_CLKO_CKE;

	return regmap_write(pcf85053->regmap, PCF85053_REG_CLKO, val);
}

static int pcf85053_clkout_prepare(struct clk_hw *hw)
{
	return pcf85053_clkout_control(hw, 1);
}

static void pcf85053_clkout_unprepare(struct clk_hw *hw)
{
	pcf85053_clkout_control(hw, 0);
}

static int pcf85053_clkout_is_prepared(struct clk_hw *hw)
{
	struct pcf85053 *pcf85053 = clkout_hw_to_pcf85053(hw);
	unsigned int val = 0;
	int err;

	err = regmap_read(pcf85053->regmap, PCF85053_REG_CLKO, &val);
	if (err)
		return err;

	return val & PCF85053_REG_CLKO_CKE;
}

static const struct clk_ops pcf85053_clkout_ops = {
	.prepare = pcf85053_clkout_prepare,
	.unprepare = pcf85053_clkout_unprepare,
	.is_prepared = pcf85053_clkout_is_prepared,
	.recalc_rate = pcf85053_clkout_recalc_rate,
	.determine_rate = pcf85053_clkout_determine_rate,
	.set_rate = pcf85053_clkout_set_rate,
};

static struct clk *pcf85053_clkout_register_clk(struct pcf85053 *pcf85053)
{
	struct device *dev = pcf85053->rtc->dev.parent;
	struct device_node *node = dev->of_node;
	struct clk *clk;
	struct clk_init_data init;

	init.name = "pcf85053-clkout";
	init.ops = &pcf85053_clkout_ops;
	init.flags = 0;
	init.parent_names = NULL;
	init.num_parents = 0;
	pcf85053->clkout_hw.init = &init;

	/* optional override of the clockname */
	of_property_read_string(node, "clock-output-names", &init.name);

	/* register the clock */
	clk = devm_clk_register(dev, &pcf85053->clkout_hw);

	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);

	return clk;
}
#endif

static const struct rtc_class_ops pcf85053_rtc_ops = {
	.read_time	= pcf85053_rtc_read_time,
	.set_time	= pcf85053_rtc_set_time,
	.read_alarm	= pcf85053_rtc_read_alarm,
	.set_alarm	= pcf85053_rtc_set_alarm,
	.alarm_irq_enable = pcf85053_irq_enable,
	.ioctl		= pcf85053_ioctl,
};

static const struct pcf85053_config config_pcf85053 = {
	.regmap = {
		.reg_bits = 8,
		.val_bits = 8,
		.max_register = 0x1D,
	},
	.has_alarms = 1,
};

static int pcf85053_probe(struct i2c_client *client)
{
	const struct pcf85053_config *config;
	struct device *dev = &client->dev;
	const char *iface = NULL;
	struct pcf85053 *pcf85053;
	int err;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C |
				     I2C_FUNC_SMBUS_BYTE |
				     I2C_FUNC_SMBUS_BLOCK_DATA))
		return -ENODEV;

	pcf85053 = devm_kzalloc(dev, sizeof(struct pcf85053),
				GFP_KERNEL);
	if (!pcf85053)
		return -ENOMEM;

	config = i2c_get_match_data(client);
	if (!config)
		return -ENODEV;

	pcf85053->regmap = devm_regmap_init_i2c(client, &config->regmap);
	if (IS_ERR(pcf85053->regmap))
		return PTR_ERR(pcf85053->regmap);

	dev_set_drvdata(dev, pcf85053);

	pcf85053->is_primary = true;

	if (of_property_read_string(dev->of_node, "nxp,interface", &iface))
		return dev_err_probe(dev, -EINVAL,
				     "Missing mandatory property: nxp,interface\n");
	if (!strcmp(iface, "primary"))
		pcf85053->is_primary = true;
	else if (!strcmp(iface, "secondary"))
		pcf85053->is_primary = false;
	else
		return dev_err_probe(dev, -EINVAL,
				     "Invalid value for nxp,interface: %s\n", iface);

	if (pcf85053->is_primary) {
		unsigned int ctrl;
		int err;

		err = regmap_read(pcf85053->regmap, PCF85053_REG_CTRL, &ctrl);
		if (err)
			return err;

		if (of_property_read_bool(dev->of_node, "nxp,write-access")) {
			if (!(ctrl & PCF85053_BIT_TWO)) {
				err = regmap_update_bits(pcf85053->regmap, PCF85053_REG_CTRL,
							 PCF85053_BIT_TWO, PCF85053_BIT_TWO);
				if (err)
					return err;
			}
			dev_dbg(dev, "Ownership set: TWO=1 (primary writes)\n");
		} else {
			/* TWO (Time Write Ownership) bit defaults to 0 (Secondary) */
			dev_dbg(dev, "Default ownership set: TWO=0 (secondary writes)\n");
		}
	}

	pcf85053->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(pcf85053->rtc))
		return PTR_ERR(pcf85053->rtc);

	/*
	 * The year register stores 00-99 and the RTC treats any value where
	 * (year % 4) == 0 as a leap year, with no special handling for the
	 * centuries. Mapping 00-99 to 2000-2099 keeps the leap-year logic
	 * correct over the whole usable range.
	 */
	pcf85053->rtc->ops = &pcf85053_rtc_ops;
	pcf85053->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	pcf85053->rtc->range_max = RTC_TIMESTAMP_END_2099;
	clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, pcf85053->rtc->features);
	clear_bit(RTC_FEATURE_ALARM, pcf85053->rtc->features);

	if (config->has_alarms && client->irq > 0) {
		err = devm_request_threaded_irq(dev, client->irq,
						NULL, pcf85053_irq,
						IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
						"pcf85053", dev);
		if (err) {
			dev_err(dev, "unable to request IRQ %d\n", client->irq);
		} else {
			set_bit(RTC_FEATURE_ALARM, pcf85053->rtc->features);
			devm_device_init_wakeup(dev);
			err = dev_pm_set_wake_irq(dev, client->irq);
			if (err)
				dev_err(dev, "failed to enable irq wake\n");
		}
	}

#ifdef CONFIG_COMMON_CLK
	/* register clk in common clk framework */
	pcf85053_clkout_register_clk(pcf85053);
#endif

	return devm_rtc_register_device(pcf85053->rtc);
}

static const struct i2c_device_id pcf85053_id[] = {
	{ "pcf85053", .driver_data = (kernel_ulong_t)&config_pcf85053 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pcf85053_id);

static const struct of_device_id pcf85053_of_match[] = {
	{ .compatible = "nxp,pcf85053", .data = &config_pcf85053 },
	{}
};
MODULE_DEVICE_TABLE(of, pcf85053_of_match);

static struct i2c_driver pcf85053_driver = {
	.driver		= {
		.name	= "rtc-pcf85053",
		.of_match_table = of_match_ptr(pcf85053_of_match),
	},
	.probe		= pcf85053_probe,
	.id_table	= pcf85053_id,
};

module_i2c_driver(pcf85053_driver);

MODULE_AUTHOR("Pankit Garg <pankit.garg@nxp.com>");
MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("NXP pcf85053 RTC driver");
MODULE_LICENSE("GPL");
