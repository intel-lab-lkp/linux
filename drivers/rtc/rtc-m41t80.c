// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C client/driver for the ST M41T80 family of i2c rtc chips.
 *
 * Author: Alexander Bigga <ab@mycable.de>
 *
 * Based on m41t00.c by Mark A. Greer <mgreer@mvista.com>
 *
 * 2006 (c) mycable GmbH
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bcd.h>
#include <linux/clk-provider.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/delay.h>
#ifdef CONFIG_RTC_DRV_M41T80_WDT
#include <linux/watchdog.h>
#endif

#define M41T80_REG_SSEC		0x00
#define M41T80_REG_SEC		0x01
#define M41T80_REG_MIN		0x02
#define M41T80_REG_HOUR		0x03
#define M41T80_REG_WDAY		0x04
#define M41T80_REG_DAY		0x05
#define M41T80_REG_MON		0x06
#define M41T80_REG_YEAR		0x07
#define M41T80_REG_ALARM_MON	0x0a
#define M41T80_REG_ALARM_DAY	0x0b
#define M41T80_REG_ALARM_HOUR	0x0c
#define M41T80_REG_ALARM_MIN	0x0d
#define M41T80_REG_ALARM_SEC	0x0e
#define M41T80_REG_FLAGS	0x0f
#define M41T80_REG_SQW		0x13

#define M41T80_DATETIME_REG_SIZE	(M41T80_REG_YEAR + 1)
#define M41T80_ALARM_REG_SIZE	\
	(M41T80_REG_ALARM_SEC + 1 - M41T80_REG_ALARM_MON)

#define M41T80_SQW_MAX_FREQ	32768

#define M41T80_SEC_ST		BIT(7)	/* ST: Stop Bit */
#define M41T80_ALMON_AFE	BIT(7)	/* AFE: AF Enable Bit */
#define M41T80_ALMON_SQWE	BIT(6)	/* SQWE: SQW Enable Bit */
#define M41T80_ALHOUR_HT	BIT(6)	/* HT: Halt Update Bit */
#define M41T80_FLAGS_OF		BIT(2)	/* OF: Oscillator Failure Bit */
#define M41T80_FLAGS_AF		BIT(6)	/* AF: Alarm Flag Bit */
#define M41T80_FLAGS_BATT_LOW	BIT(4)	/* BL: Battery Low Bit */
#define M41T80_WATCHDOG_RB2	BIT(7)	/* RB: Watchdog resolution */
#define M41T80_WATCHDOG_RB1	BIT(1)	/* RB: Watchdog resolution */
#define M41T80_WATCHDOG_RB0	BIT(0)	/* RB: Watchdog resolution */

#define M41T80_FEATURE_HT	BIT(0)	/* Halt feature */
#define M41T80_FEATURE_BL	BIT(1)	/* Battery low indicator */
#define M41T80_FEATURE_SQ	BIT(2)	/* Squarewave feature */
#define M41T80_FEATURE_WD	BIT(3)	/* Extra watchdog resolution */
#define M41T80_FEATURE_SQ_ALT	BIT(4)	/* RSx bits are in reg 4 */

static const struct i2c_device_id m41t80_id[] = {
	{ .name = "m41t62", .driver_data = M41T80_FEATURE_SQ | M41T80_FEATURE_SQ_ALT },
	{ .name = "m41t65", .driver_data = M41T80_FEATURE_WD },
	{ .name = "m41t80", .driver_data = M41T80_FEATURE_SQ },
	{ .name = "m41t81", .driver_data = M41T80_FEATURE_HT | M41T80_FEATURE_SQ},
	{ .name = "m41t81s", .driver_data = M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ },
	{ .name = "m41t82", .driver_data = M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ },
	{ .name = "m41t83", .driver_data = M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ },
	{ .name = "m41st84", .driver_data = M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ },
	{ .name = "m41st85", .driver_data = M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ },
	{ .name = "m41st87", .driver_data = M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ },
	{ .name = "rv4162", .driver_data = M41T80_FEATURE_SQ | M41T80_FEATURE_WD | M41T80_FEATURE_SQ_ALT },
	{ }
};
MODULE_DEVICE_TABLE(i2c, m41t80_id);

static const __maybe_unused struct of_device_id m41t80_of_match[] = {
	{
		.compatible = "st,m41t62",
		.data = (void *)(M41T80_FEATURE_SQ | M41T80_FEATURE_SQ_ALT)
	},
	{
		.compatible = "st,m41t65",
		.data = (void *)(M41T80_FEATURE_WD)
	},
	{
		.compatible = "st,m41t80",
		.data = (void *)(M41T80_FEATURE_SQ)
	},
	{
		.compatible = "st,m41t81",
		.data = (void *)(M41T80_FEATURE_HT | M41T80_FEATURE_SQ)
	},
	{
		.compatible = "st,m41t81s",
		.data = (void *)(M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ)
	},
	{
		.compatible = "st,m41t82",
		.data = (void *)(M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ)
	},
	{
		.compatible = "st,m41t83",
		.data = (void *)(M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ)
	},
	{
		.compatible = "st,m41t84",
		.data = (void *)(M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ)
	},
	{
		.compatible = "st,m41t85",
		.data = (void *)(M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ)
	},
	{
		.compatible = "st,m41t87",
		.data = (void *)(M41T80_FEATURE_HT | M41T80_FEATURE_BL | M41T80_FEATURE_SQ)
	},
	{
		.compatible = "microcrystal,rv4162",
		.data = (void *)(M41T80_FEATURE_SQ | M41T80_FEATURE_WD | M41T80_FEATURE_SQ_ALT)
	},
	/* DT compatibility only, do not use compatibles below: */
	{
		.compatible = "st,rv4162",
		.data = (void *)(M41T80_FEATURE_SQ | M41T80_FEATURE_WD | M41T80_FEATURE_SQ_ALT)
	},
	{
		.compatible = "rv4162",
		.data = (void *)(M41T80_FEATURE_SQ | M41T80_FEATURE_WD | M41T80_FEATURE_SQ_ALT)
	},
	{ }
};
MODULE_DEVICE_TABLE(of, m41t80_of_match);

struct m41t80_data {
	unsigned long features;
	struct i2c_client *client;
	struct rtc_device *rtc;
#ifdef CONFIG_RTC_DRV_M41T80_WDT
	struct watchdog_device wdt;
#endif
#ifdef CONFIG_COMMON_CLK
	struct clk_hw sqw;
	unsigned long freq;
	unsigned int sqwe;
#endif
};

static irqreturn_t m41t80_handle_irq(int irq, void *dev_id)
{
	struct i2c_client *client = dev_id;
	struct m41t80_data *m41t80 = i2c_get_clientdata(client);
	unsigned long events = 0;
	int flags, flags_afe;

	rtc_lock(m41t80->rtc);

	flags_afe = i2c_smbus_read_byte_data(client, M41T80_REG_ALARM_MON);
	if (flags_afe < 0) {
		rtc_unlock(m41t80->rtc);
		return IRQ_NONE;
	}

	flags = i2c_smbus_read_byte_data(client, M41T80_REG_FLAGS);
	if (flags <= 0) {
		rtc_unlock(m41t80->rtc);
		return IRQ_NONE;
	}

	if (flags & M41T80_FLAGS_AF) {
		flags &= ~M41T80_FLAGS_AF;
		flags_afe &= ~M41T80_ALMON_AFE;
		events |= RTC_AF;
	}

	if (events) {
		rtc_update_irq(m41t80->rtc, 1, events);
		i2c_smbus_write_byte_data(client, M41T80_REG_FLAGS, flags);
		i2c_smbus_write_byte_data(client, M41T80_REG_ALARM_MON,
					  flags_afe);
	}

	rtc_unlock(m41t80->rtc);

	return IRQ_HANDLED;
}

static int m41t80_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct i2c_client *client = to_i2c_client(dev);
	unsigned char buf[8];
	int err, flags;

	flags = i2c_smbus_read_byte_data(client, M41T80_REG_FLAGS);
	if (flags < 0)
		return flags;

	if (flags & M41T80_FLAGS_OF) {
		dev_err(&client->dev, "Oscillator failure, time may not be accurate, write time to RTC to fix it.\n");
		return -EINVAL;
	}

	err = i2c_smbus_read_i2c_block_data(client, M41T80_REG_SSEC,
					    sizeof(buf), buf);
	if (err < 0) {
		dev_dbg(&client->dev, "Unable to read date\n");
		return err;
	}

	tm->tm_sec = bcd2bin(buf[M41T80_REG_SEC] & 0x7f);
	tm->tm_min = bcd2bin(buf[M41T80_REG_MIN] & 0x7f);
	tm->tm_hour = bcd2bin(buf[M41T80_REG_HOUR] & 0x3f);
	tm->tm_mday = bcd2bin(buf[M41T80_REG_DAY] & 0x3f);
	tm->tm_wday = buf[M41T80_REG_WDAY] & 0x07;
	tm->tm_mon = bcd2bin(buf[M41T80_REG_MON] & 0x1f) - 1;

	/* assume 20YY not 19YY, and ignore the Century Bit */
	tm->tm_year = bcd2bin(buf[M41T80_REG_YEAR]) + 100;
	return 0;
}

static int m41t80_rtc_set_time(struct device *dev, struct rtc_time *in_tm)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct m41t80_data *clientdata = i2c_get_clientdata(client);
	struct rtc_time tm = *in_tm;
	unsigned char buf[8];
	int err, flags;
	time64_t time = 0;

	flags = i2c_smbus_read_byte_data(client, M41T80_REG_FLAGS);
	if (flags < 0)
		return flags;
	if (flags & M41T80_FLAGS_OF) {
		/* add 4sec of oscillator stablize time otherwise we are behind 4sec */
		time = rtc_tm_to_time64(&tm);
		rtc_time64_to_tm(time + 4, &tm);
	}
	buf[M41T80_REG_SSEC] = 0;
	buf[M41T80_REG_SEC] = bin2bcd(tm.tm_sec);
	buf[M41T80_REG_MIN] = bin2bcd(tm.tm_min);
	buf[M41T80_REG_HOUR] = bin2bcd(tm.tm_hour);
	buf[M41T80_REG_DAY] = bin2bcd(tm.tm_mday);
	buf[M41T80_REG_MON] = bin2bcd(tm.tm_mon + 1);
	buf[M41T80_REG_YEAR] = bin2bcd(tm.tm_year - 100);
	buf[M41T80_REG_WDAY] = tm.tm_wday;

	/* If the square wave output is controlled in the weekday register */
	if (clientdata->features & M41T80_FEATURE_SQ_ALT) {
		int val;

		val = i2c_smbus_read_byte_data(client, M41T80_REG_WDAY);
		if (val < 0)
			return val;

		buf[M41T80_REG_WDAY] |= (val & 0xf0);
	}

	err = i2c_smbus_write_i2c_block_data(client, M41T80_REG_SSEC,
					     sizeof(buf), buf);
	if (err < 0) {
		dev_dbg(&client->dev, "Unable to write to date registers\n");
		return err;
	}
	if (flags & M41T80_FLAGS_OF) {
		/* OF cannot be immediately reset: oscillator has to be restarted. */
		dev_warn(&client->dev, "OF bit is still set, kickstarting clock.\n");
		err = i2c_smbus_write_byte_data(client, M41T80_REG_SEC, M41T80_SEC_ST);
		if (err < 0) {
			dev_dbg(&client->dev, "Can't set ST bit\n");
			return err;
		}
		err = i2c_smbus_write_byte_data(client, M41T80_REG_SEC, flags & ~M41T80_SEC_ST);
		if (err < 0) {
			dev_dbg(&client->dev, "Can't clear ST bit\n");
			return err;
		}
		/* oscillator must run for 4sec before we attempt to reset OF bit */
		msleep(4000);
		/* Clear the OF bit of Flags Register */
		err = i2c_smbus_write_byte_data(client, M41T80_REG_FLAGS, flags & ~M41T80_FLAGS_OF);
		if (err < 0) {
			dev_dbg(&client->dev, "Unable to write flags register\n");
			return err;
		}
		flags = i2c_smbus_read_byte_data(client, M41T80_REG_FLAGS);
		if (flags < 0) {
			return flags;
		} else if (flags & M41T80_FLAGS_OF) {
			dev_dbg(&client->dev, "Can't clear the OF bit check battery\n");
			return err;
		}
	}

	return err;
}

static int m41t80_rtc_proc(struct device *dev, struct seq_file *seq)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct m41t80_data *clientdata = i2c_get_clientdata(client);
	int reg;

	if (clientdata->features & M41T80_FEATURE_BL) {
		reg = i2c_smbus_read_byte_data(client, M41T80_REG_FLAGS);
		if (reg < 0)
			return reg;
		seq_printf(seq, "battery\t\t: %s\n",
			   (reg & M41T80_FLAGS_BATT_LOW) ? "exhausted" : "ok");
	}
	return 0;
}

static int m41t80_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct i2c_client *client = to_i2c_client(dev);
	int flags, retval;

	flags = i2c_smbus_read_byte_data(client, M41T80_REG_ALARM_MON);
	if (flags < 0)
		return flags;

	if (enabled)
		flags |= M41T80_ALMON_AFE;
	else
		flags &= ~M41T80_ALMON_AFE;

	retval = i2c_smbus_write_byte_data(client, M41T80_REG_ALARM_MON, flags);
	if (retval < 0) {
		dev_dbg(dev, "Unable to enable alarm IRQ %d\n", retval);
		return retval;
	}
	return 0;
}

static int m41t80_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct i2c_client *client = to_i2c_client(dev);
	u8 alarmvals[5];
	int ret, err;

	alarmvals[0] = bin2bcd(alrm->time.tm_mon + 1);
	alarmvals[1] = bin2bcd(alrm->time.tm_mday);
	alarmvals[2] = bin2bcd(alrm->time.tm_hour);
	alarmvals[3] = bin2bcd(alrm->time.tm_min);
	alarmvals[4] = bin2bcd(alrm->time.tm_sec);

	/* Clear AF and AFE flags */
	ret = i2c_smbus_read_byte_data(client, M41T80_REG_ALARM_MON);
	if (ret < 0)
		return ret;
	err = i2c_smbus_write_byte_data(client, M41T80_REG_ALARM_MON,
					ret & ~(M41T80_ALMON_AFE));
	if (err < 0) {
		dev_dbg(dev, "Unable to clear AFE bit\n");
		return err;
	}

	/* Keep SQWE bit value */
	alarmvals[0] |= (ret & M41T80_ALMON_SQWE);

	ret = i2c_smbus_read_byte_data(client, M41T80_REG_FLAGS);
	if (ret < 0)
		return ret;

	err = i2c_smbus_write_byte_data(client, M41T80_REG_FLAGS,
					ret & ~(M41T80_FLAGS_AF));
	if (err < 0) {
		dev_dbg(dev, "Unable to clear AF bit\n");
		return err;
	}

	/* Write the alarm */
	err = i2c_smbus_write_i2c_block_data(client, M41T80_REG_ALARM_MON,
					     5, alarmvals);
	if (err)
		return err;

	/* Enable the alarm interrupt */
	if (alrm->enabled) {
		alarmvals[0] |= M41T80_ALMON_AFE;
		err = i2c_smbus_write_byte_data(client, M41T80_REG_ALARM_MON,
						alarmvals[0]);
		if (err)
			return err;
	}

	return 0;
}

static int m41t80_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct i2c_client *client = to_i2c_client(dev);
	u8 alarmvals[5];
	int flags, ret;

	ret = i2c_smbus_read_i2c_block_data(client, M41T80_REG_ALARM_MON,
					    5, alarmvals);
	if (ret != 5)
		return ret < 0 ? ret : -EIO;

	flags = i2c_smbus_read_byte_data(client, M41T80_REG_FLAGS);
	if (flags < 0)
		return flags;

	alrm->time.tm_sec  = bcd2bin(alarmvals[4] & 0x7f);
	alrm->time.tm_min  = bcd2bin(alarmvals[3] & 0x7f);
	alrm->time.tm_hour = bcd2bin(alarmvals[2] & 0x3f);
	alrm->time.tm_mday = bcd2bin(alarmvals[1] & 0x3f);
	alrm->time.tm_mon  = bcd2bin(alarmvals[0] & 0x3f) - 1;

	alrm->enabled = !!(alarmvals[0] & M41T80_ALMON_AFE);
	alrm->pending = (flags & M41T80_FLAGS_AF) && alrm->enabled;

	return 0;
}

static const struct rtc_class_ops m41t80_rtc_ops = {
	.read_time = m41t80_rtc_read_time,
	.set_time = m41t80_rtc_set_time,
	.proc = m41t80_rtc_proc,
	.read_alarm = m41t80_read_alarm,
	.set_alarm = m41t80_set_alarm,
	.alarm_irq_enable = m41t80_alarm_irq_enable,
};

#ifdef CONFIG_PM_SLEEP
static int m41t80_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	if (client->irq >= 0 && device_may_wakeup(dev))
		enable_irq_wake(client->irq);

	return 0;
}

static int m41t80_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	if (client->irq >= 0 && device_may_wakeup(dev))
		disable_irq_wake(client->irq);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(m41t80_pm, m41t80_suspend, m41t80_resume);

#ifdef CONFIG_COMMON_CLK
#define sqw_to_m41t80_data(_hw) container_of(_hw, struct m41t80_data, sqw)

static unsigned long m41t80_decode_freq(int setting)
{
	return (setting == 0) ? 0 : (setting == 1) ? M41T80_SQW_MAX_FREQ :
		M41T80_SQW_MAX_FREQ >> setting;
}

static unsigned long m41t80_get_freq(struct m41t80_data *m41t80)
{
	struct i2c_client *client = m41t80->client;
	int reg_sqw = (m41t80->features & M41T80_FEATURE_SQ_ALT) ?
		M41T80_REG_WDAY : M41T80_REG_SQW;
	int ret = i2c_smbus_read_byte_data(client, reg_sqw);

	if (ret < 0)
		return 0;
	return m41t80_decode_freq(ret >> 4);
}

static unsigned long m41t80_sqw_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	return sqw_to_m41t80_data(hw)->freq;
}

static int m41t80_sqw_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	if (req->rate >= M41T80_SQW_MAX_FREQ)
		req->rate = M41T80_SQW_MAX_FREQ;
	else if (req->rate >= M41T80_SQW_MAX_FREQ / 4)
		req->rate = M41T80_SQW_MAX_FREQ / 4;
	else if (req->rate)
		req->rate = 1 << ilog2(req->rate);

	return 0;
}

static int m41t80_sqw_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct m41t80_data *m41t80 = sqw_to_m41t80_data(hw);
	struct i2c_client *client = m41t80->client;
	int reg_sqw = (m41t80->features & M41T80_FEATURE_SQ_ALT) ?
		M41T80_REG_WDAY : M41T80_REG_SQW;
	int reg, ret, val = 0;

	if (rate >= M41T80_SQW_MAX_FREQ)
		val = 1;
	else if (rate >= M41T80_SQW_MAX_FREQ / 4)
		val = 2;
	else if (rate)
		val = 15 - ilog2(rate);

	reg = i2c_smbus_read_byte_data(client, reg_sqw);
	if (reg < 0)
		return reg;

	reg = (reg & 0x0f) | (val << 4);

	ret = i2c_smbus_write_byte_data(client, reg_sqw, reg);
	if (!ret)
		m41t80->freq = m41t80_decode_freq(val);
	return ret;
}

static int m41t80_sqw_control(struct clk_hw *hw, bool enable)
{
	struct m41t80_data *m41t80 = sqw_to_m41t80_data(hw);
	struct i2c_client *client = m41t80->client;
	int ret = i2c_smbus_read_byte_data(client, M41T80_REG_ALARM_MON);

	if (ret < 0)
		return ret;

	if (enable)
		ret |= M41T80_ALMON_SQWE;
	else
		ret &= ~M41T80_ALMON_SQWE;

	ret = i2c_smbus_write_byte_data(client, M41T80_REG_ALARM_MON, ret);
	if (!ret)
		m41t80->sqwe = enable;
	return ret;
}

static int m41t80_sqw_prepare(struct clk_hw *hw)
{
	return m41t80_sqw_control(hw, 1);
}

static void m41t80_sqw_unprepare(struct clk_hw *hw)
{
	m41t80_sqw_control(hw, 0);
}

static int m41t80_sqw_is_prepared(struct clk_hw *hw)
{
	return sqw_to_m41t80_data(hw)->sqwe;
}

static const struct clk_ops m41t80_sqw_ops = {
	.prepare = m41t80_sqw_prepare,
	.unprepare = m41t80_sqw_unprepare,
	.is_prepared = m41t80_sqw_is_prepared,
	.recalc_rate = m41t80_sqw_recalc_rate,
	.determine_rate = m41t80_sqw_determine_rate,
	.set_rate = m41t80_sqw_set_rate,
};

static struct clk *m41t80_sqw_register_clk(struct m41t80_data *m41t80)
{
	struct i2c_client *client = m41t80->client;
	struct device_node *node = client->dev.of_node;
	struct device_node *fixed_clock;
	struct clk *clk;
	struct clk_init_data init;
	int ret;

	fixed_clock = of_get_child_by_name(node, "clock");
	if (fixed_clock) {
		/*
		 * skip registering square wave clock when a fixed
		 * clock has been registered. The fixed clock is
		 * registered automatically when being referenced.
		 */
		of_node_put(fixed_clock);
		return NULL;
	}

	/* First disable the clock */
	ret = i2c_smbus_read_byte_data(client, M41T80_REG_ALARM_MON);
	if (ret < 0)
		return ERR_PTR(ret);
	ret = i2c_smbus_write_byte_data(client, M41T80_REG_ALARM_MON,
					ret & ~(M41T80_ALMON_SQWE));
	if (ret < 0)
		return ERR_PTR(ret);

	init.name = "m41t80-sqw";
	init.ops = &m41t80_sqw_ops;
	init.flags = 0;
	init.parent_names = NULL;
	init.num_parents = 0;
	m41t80->sqw.init = &init;
	m41t80->freq = m41t80_get_freq(m41t80);

	/* optional override of the clockname */
	of_property_read_string(node, "clock-output-names", &init.name);

	/* register the clock */
	clk = clk_register(&client->dev, &m41t80->sqw);
	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);

	return clk;
}
#endif

#ifdef CONFIG_RTC_DRV_M41T80_WDT
/*
 *****************************************************************************
 *
 * Watchdog Driver
 *
 *****************************************************************************
 */
/* Default margin */
#define M41T80_WDT_DEFAULT_TIMEOUT	60
#define M41T80_WDT_MIN_TIMEOUT	1
#define M41T80_WDT_MAX_TIMEOUT	124

static int wdt_margin = M41T80_WDT_DEFAULT_TIMEOUT;
module_param(wdt_margin, int, 0);
MODULE_PARM_DESC(wdt_margin, "Watchdog timeout in seconds (default 60s)");

static const struct watchdog_info m41t80_wdt_info = {
	.options = WDIOF_POWERUNDER | WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
	.firmware_version = 1,
	.identity = "M41T80 Watchdog",
};

/**
 * m41t80_wdt_ping - Reload the watchdog counter.
 */
static int m41t80_wdt_ping(struct watchdog_device *wdt)
{
	struct m41t80_data *m41t80 = watchdog_get_drvdata(wdt);
	struct i2c_client *client = m41t80->client;
	unsigned char i2c_data[2];
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = 2,
		.buf = i2c_data,
	};
	int ret;

	i2c_data[0] = 0x09; /* watchdog register */

	if (wdt->timeout > 31)
		i2c_data[1] = (wdt->timeout & 0xFC) | 0x83;
	else
		/*
		 * WDS = 1 (0x80), multiplier = timeout, resolution = 1s (0x02)
		 */
		i2c_data[1] = wdt->timeout << 2 | 0x82;

	/*
	 * M41T65 has three bits for watchdog resolution. Don't set bit 7, as
	 * that would be an invalid resolution.
	 */
	if (m41t80->features & M41T80_FEATURE_WD)
		i2c_data[1] &= ~M41T80_WATCHDOG_RB2;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret == 1)
		return 0;

	return ret < 0 ? ret : -EIO;
}

/**
 * m41t80_wdt_stop - Disable the watchdog.
 */
static int m41t80_wdt_stop(struct watchdog_device *wdt)
{
	struct m41t80_data *m41t80 = watchdog_get_drvdata(wdt);
	struct i2c_client *client = m41t80->client;
	unsigned char i2c_data[2], i2c_buf[0x10];
	struct i2c_msg msgs0[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = i2c_data,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = i2c_buf,
		},
	};
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = 2,
		.buf = i2c_data,
	};
	int ret;

	i2c_data[0] = 0x09;
	ret = i2c_transfer(client->adapter, msgs0, 2);
	if (ret != 2)
		return ret < 0 ? ret : -EIO;

	i2c_data[0] = 0x09;
	i2c_data[1] = 0x00;
	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret == 1)
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int m41t80_wdt_set_timeout(struct watchdog_device *wdt, unsigned int timeout)
{
	wdt->timeout = timeout;

	return m41t80_wdt_ping(wdt);
}

static const struct watchdog_ops m41t80_wdt_ops = {
	.owner = THIS_MODULE,
	.start = m41t80_wdt_ping,
	.stop = m41t80_wdt_stop,
	.ping = m41t80_wdt_ping,
	.set_timeout = m41t80_wdt_set_timeout,
};
#endif /* CONFIG_RTC_DRV_M41T80_WDT */

/*
 *****************************************************************************
 *
 *	Driver Interface
 *
 *****************************************************************************
 */

static int m41t80_probe(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	int rc = 0;
	struct rtc_time tm;
	struct m41t80_data *m41t80_data = NULL;
	bool wakeup_source = false;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK |
				     I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&adapter->dev, "doesn't support I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_I2C_BLOCK\n");
		return -ENODEV;
	}

	m41t80_data = devm_kzalloc(&client->dev, sizeof(*m41t80_data),
				   GFP_KERNEL);
	if (!m41t80_data)
		return -ENOMEM;

	m41t80_data->client = client;
	m41t80_data->features = (unsigned long)i2c_get_match_data(client);
	i2c_set_clientdata(client, m41t80_data);

	m41t80_data->rtc =  devm_rtc_allocate_device(&client->dev);
	if (IS_ERR(m41t80_data->rtc))
		return PTR_ERR(m41t80_data->rtc);

	wakeup_source = device_property_read_bool(&client->dev, "wakeup-source");
	if (client->irq > 0) {
		unsigned long irqflags = IRQF_TRIGGER_LOW;

		if (dev_fwnode(&client->dev))
			irqflags = 0;

		rc = devm_request_threaded_irq(&client->dev, client->irq,
					       NULL, m41t80_handle_irq,
					       irqflags | IRQF_ONESHOT,
					       "m41t80", client);
		if (rc) {
			dev_warn(&client->dev, "unable to request IRQ, alarms disabled\n");
			client->irq = 0;
			wakeup_source = false;
		}
	}
	if (client->irq > 0 || wakeup_source)
		device_init_wakeup(&client->dev, true);
	else
		clear_bit(RTC_FEATURE_ALARM, m41t80_data->rtc->features);

	m41t80_data->rtc->ops = &m41t80_rtc_ops;
	m41t80_data->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	m41t80_data->rtc->range_max = RTC_TIMESTAMP_END_2099;

	if (client->irq <= 0)
		clear_bit(RTC_FEATURE_UPDATE_INTERRUPT, m41t80_data->rtc->features);

	/* Make sure HT (Halt Update) bit is cleared */
	rc = i2c_smbus_read_byte_data(client, M41T80_REG_ALARM_HOUR);

	if (rc >= 0 && rc & M41T80_ALHOUR_HT) {
		if (m41t80_data->features & M41T80_FEATURE_HT) {
			m41t80_rtc_read_time(&client->dev, &tm);
			dev_info(&client->dev, "HT bit was set!\n");
			dev_info(&client->dev, "Power Down at %ptR\n", &tm);
		}
		rc = i2c_smbus_write_byte_data(client, M41T80_REG_ALARM_HOUR,
					       rc & ~M41T80_ALHOUR_HT);
	}

	if (rc < 0) {
		dev_err(&client->dev, "Can't clear HT bit\n");
		return rc;
	}

	/* Make sure ST (stop) bit is cleared */
	rc = i2c_smbus_read_byte_data(client, M41T80_REG_SEC);

	if (rc >= 0 && rc & M41T80_SEC_ST)
		rc = i2c_smbus_write_byte_data(client, M41T80_REG_SEC,
					       rc & ~M41T80_SEC_ST);
	if (rc < 0) {
		dev_err(&client->dev, "Can't clear ST bit\n");
		return rc;
	}

#ifdef CONFIG_COMMON_CLK
	if (m41t80_data->features & M41T80_FEATURE_SQ)
		m41t80_sqw_register_clk(m41t80_data);
#endif

	rc = devm_rtc_register_device(m41t80_data->rtc);
	if (rc)
		return rc;

#ifdef CONFIG_RTC_DRV_M41T80_WDT
	if (m41t80_data->features & M41T80_FEATURE_HT) {
		m41t80_data->wdt.info = &m41t80_wdt_info;
		m41t80_data->wdt.ops = &m41t80_wdt_ops;
		m41t80_data->wdt.timeout = M41T80_WDT_DEFAULT_TIMEOUT;
		m41t80_data->wdt.min_timeout = M41T80_WDT_MIN_TIMEOUT;
		m41t80_data->wdt.max_timeout = M41T80_WDT_MAX_TIMEOUT;

		watchdog_init_timeout(&m41t80_data->wdt, wdt_margin, &client->dev);
		watchdog_stop_on_reboot(&m41t80_data->wdt);
		watchdog_stop_on_unregister(&m41t80_data->wdt);
		watchdog_set_drvdata(&m41t80_data->wdt, m41t80_data);

		rc = devm_watchdog_register_device(&client->dev, &m41t80_data->wdt);
		if (rc)
			return rc;
	}
#endif

	return 0;
}

static struct i2c_driver m41t80_driver = {
	.driver = {
		.name = "rtc-m41t80",
		.of_match_table = of_match_ptr(m41t80_of_match),
		.pm = &m41t80_pm,
	},
	.probe = m41t80_probe,
	.id_table = m41t80_id,
};

module_i2c_driver(m41t80_driver);

MODULE_AUTHOR("Alexander Bigga <ab@mycable.de>");
MODULE_DESCRIPTION("ST Microelectronics M41T80 series RTC I2C Client Driver");
MODULE_LICENSE("GPL");
