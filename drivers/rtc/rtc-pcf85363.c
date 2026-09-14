// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/rtc/rtc-pcf85363.c
 *
 * Driver for NXP PCF85363 real-time clock.
 *
 * Copyright (C) 2017 Eric Nelson
 *
 * Copyright 2025-2026 NXP
 * Added support for timestamps, battery switch-over,
 * watchdog, offset calibration.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/rtc.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/bcd.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/watchdog.h>

#include <dt-bindings/rtc/pcf85363-tsr.h>

/*
 * Date/Time registers
 */
#define DT_100THS	0x00
#define DT_SECS		0x01
#define DT_MINUTES	0x02
#define DT_HOURS	0x03
#define DT_DAYS		0x04
#define DT_WEEKDAYS	0x05
#define DT_MONTHS	0x06
#define DT_YEARS	0x07

/*
 * Alarm registers
 */
#define DT_SECOND_ALM1	0x08
#define DT_MINUTE_ALM1	0x09
#define DT_HOUR_ALM1	0x0a
#define DT_DAY_ALM1	0x0b
#define DT_MONTH_ALM1	0x0c
#define DT_MINUTE_ALM2	0x0d
#define DT_HOUR_ALM2	0x0e
#define DT_WEEKDAY_ALM2	0x0f
#define DT_ALARM_EN	0x10

/*
 * Time stamp registers
 */
#define DT_TIMESTAMP1	0x11
#define DT_TIMESTAMP2	0x17
#define DT_TIMESTAMP3	0x1d
#define DT_TS_MODE	0x23

/*
 * control registers
 */
#define CTRL_OFFSET	0x24
#define CTRL_OSCILLATOR	0x25
#define CTRL_BATTERY	0x26
#define CTRL_PIN_IO	0x27
#define CTRL_FUNCTION	0x28
#define CTRL_INTA_EN	0x29
#define CTRL_INTB_EN	0x2a
#define CTRL_FLAGS	0x2b
#define CTRL_RAMBYTE	0x2c
#define CTRL_WDOG	0x2d
#define CTRL_STOP_EN	0x2e
#define CTRL_RESETS	0x2f
#define CTRL_RAM	0x40

#define ALRM_SEC_A1E	BIT(0)
#define ALRM_MIN_A1E	BIT(1)
#define ALRM_HR_A1E	BIT(2)
#define ALRM_DAY_A1E	BIT(3)
#define ALRM_MON_A1E	BIT(4)
#define ALRM_MIN_A2E	BIT(5)
#define ALRM_HR_A2E	BIT(6)
#define ALRM_DAY_A2E	BIT(7)

#define INT_WDIE	BIT(0)
#define INT_BSIE	BIT(1)
#define INT_TSRIE	BIT(2)
#define INT_A2IE	BIT(3)
#define INT_A1IE	BIT(4)
#define INT_OIE		BIT(5)
#define INT_PIE		BIT(6)
#define INT_ILP		BIT(7)

#define FLAGS_TSR1F	BIT(0)
#define FLAGS_TSR2F	BIT(1)
#define FLAGS_TSR3F	BIT(2)
#define FLAGS_BSF	BIT(3)
#define FLAGS_WDF	BIT(4)
#define FLAGS_A1F	BIT(5)
#define FLAGS_A2F	BIT(6)
#define FLAGS_PIF	BIT(7)

#define PIN_IO_INTAPM	GENMASK(1, 0)
#define PIN_IO_INTA_CLK	0
#define PIN_IO_INTA_BAT	1
#define PIN_IO_INTA_OUT	2
#define PIN_IO_INTA_HIZ	3

#define PIN_IO_TSPM     GENMASK(3, 2)
#define PIN_IO_TSIM     BIT(4)

#define OSC_CAP_SEL	GENMASK(1, 0)
#define OSC_CAP_6000	0x01
#define OSC_CAP_12500	0x02
#define OSC_OFFM	BIT(6)

#define STOP_EN_STOP	BIT(0)

#define RTCM_BIT        BIT(4)

#define RESET_CPR	0xa4

#define NVRAM_SIZE	0x40

#define TSR1_MASK       0x03
#define TSR2_MASK       0x07
#define TSR3_MASK       0x03
#define TSR1_SHIFT      0
#define TSR2_SHIFT      2
#define TSR3_SHIFT      6

#define OFFSET_SIGN_BIT 7
#define OFFSET_MASK     0xFF
/* Offset step in ppb; fast mode (OFFM=1) is scaled x10 for integer maths. */
#define OFFSET_STEP_PPB		2170
#define OFFSET_STEP_PPB_FAST_X10	20345
#define OFFSET_PPB_MAX		(127 * OFFSET_STEP_PPB)
#define OFFSET_PPB_MIN		(-128 * OFFSET_STEP_PPB)

#define PCF85363_NUM_TS		3
/* Bytes latched per timestamp register (sec, min, hour, day, mon, year). */
#define PCF85363_TS_LEN		6
/* Bit 7 of the seconds byte is reserved, not time data; mask it off. */
#define PCF85363_SEC_MASK	0x7F
#define PCF85363_TS_READ_RETRIES	3

#define WD_TIMEOUT_SHIFT        2
#define WD_CLKSEL_MASK  GENMASK(1, 0)
#define WD_CLKSEL_0_25HZ        0x00
#define WD_CLKSEL_1HZ   0x01
#define WD_CLKSEL_4HZ   0x02
#define WD_CLKSEL_16HZ  0x03
#define WD_MODE_REPEAT	BIT(7)

#define WD_DEFAULT_TIMEOUT  10
#define WD_TIMEOUT_MIN	1
#define WD_COUNT_MAX	0x1F
/* Longest guaranteed timeout at the 0.25 Hz step (WDR=31, one count margin). */
#define WD_TIMEOUT_MAX	120
/* Longest timeout served by the 1 Hz step (WDR=31, one count margin). */
#define WD_TIMEOUT_1HZ_MAX	30
/* CTRL_WDOG reads back the live countdown; retry until two reads agree. */
#define WD_READ_RETRIES		3

/* Cached timestamp; the flag is cleared once the value is copied here. */
struct pcf85363_ts {
	bool valid;
	u8 regs[PCF85363_TS_LEN];
};

struct pcf85363 {
	struct rtc_device	*rtc;
	struct regmap		*regmap;
	/* Serialises access to the cached event state below. */
	struct mutex		lock;
	bool			bsf;
	struct pcf85363_ts	ts[PCF85363_NUM_TS];
	/* Per-TSR: true when the register uses a last-event capture mode. */
	bool			ts_last_event[PCF85363_NUM_TS];
};

struct pcf85x63_config {
	struct regmap_config regmap;
	unsigned int num_nvram;
};

struct pcf85363_watchdog {
	struct watchdog_device wdd;
	struct pcf85363 *pcf85363;
	u8 timeout_val;
	u8 clock_sel;
};

/*
 * CTRL_FLAGS is write-0-to-clear, so write the complement of the mask to
 * clear only the requested bits without disturbing the others.
 */
static int pcf85363_clear_flags(struct pcf85363 *pcf85363, u8 mask)
{
	return regmap_write(pcf85363->regmap, CTRL_FLAGS, (u8)~mask);
}

static int pcf85363_load_capacitance(struct pcf85363 *pcf85363, struct device_node *node)
{
	u32 load = 7000;
	u8 value = 0;

	of_property_read_u32(node, "quartz-load-femtofarads", &load);

	switch (load) {
	default:
		dev_warn(&pcf85363->rtc->dev, "Unknown quartz-load-femtofarads value: %d. Assuming 7000",
			 load);
		fallthrough;
	case 7000:
		break;
	case 6000:
		value = OSC_CAP_6000;
		break;
	case 12500:
		value = OSC_CAP_12500;
		break;
	}

	return regmap_update_bits(pcf85363->regmap, CTRL_OSCILLATOR,
				  OSC_CAP_SEL, value);
}

static int pcf85363_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);
	unsigned char buf[DT_YEARS + 1];
	int ret, len = sizeof(buf);

	/* read the RTC date and time registers all at once */
	ret = regmap_bulk_read(pcf85363->regmap, DT_100THS, buf, len);
	if (ret) {
		dev_err(dev, "%s: error %d\n", __func__, ret);
		return ret;
	}

	tm->tm_year = bcd2bin(buf[DT_YEARS]);
	/* adjust for 1900 base of rtc_time */
	tm->tm_year += 100;

	tm->tm_wday = buf[DT_WEEKDAYS] & 7;
	buf[DT_SECS] &= 0x7F;
	tm->tm_sec = bcd2bin(buf[DT_SECS]);
	buf[DT_MINUTES] &= 0x7F;
	tm->tm_min = bcd2bin(buf[DT_MINUTES]);
	tm->tm_hour = bcd2bin(buf[DT_HOURS]);
	tm->tm_mday = bcd2bin(buf[DT_DAYS]);
	tm->tm_mon = bcd2bin(buf[DT_MONTHS]) - 1;

	return 0;
}

static int pcf85363_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);
	unsigned char tmp[11];
	unsigned char *buf = &tmp[2];
	int ret;

	tmp[0] = STOP_EN_STOP;
	tmp[1] = RESET_CPR;

	buf[DT_100THS] = 0;
	buf[DT_SECS] = bin2bcd(tm->tm_sec);
	buf[DT_MINUTES] = bin2bcd(tm->tm_min);
	buf[DT_HOURS] = bin2bcd(tm->tm_hour);
	buf[DT_DAYS] = bin2bcd(tm->tm_mday);
	buf[DT_WEEKDAYS] = tm->tm_wday;
	buf[DT_MONTHS] = bin2bcd(tm->tm_mon + 1);
	buf[DT_YEARS] = bin2bcd(tm->tm_year % 100);

	ret = regmap_bulk_write(pcf85363->regmap, CTRL_STOP_EN,
				tmp, 2);
	if (ret)
		return ret;

	ret = regmap_bulk_write(pcf85363->regmap, DT_100THS,
				buf, sizeof(tmp) - 2);
	if (ret)
		return ret;

	return regmap_write(pcf85363->regmap, CTRL_STOP_EN, 0);
}

static int pcf85363_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);
	unsigned char buf[DT_MONTH_ALM1 - DT_SECOND_ALM1 + 1];
	unsigned int val;
	int ret;

	ret = regmap_bulk_read(pcf85363->regmap, DT_SECOND_ALM1, buf,
			       sizeof(buf));
	if (ret)
		return ret;

	alrm->time.tm_sec = bcd2bin(buf[0]);
	alrm->time.tm_min = bcd2bin(buf[1]);
	alrm->time.tm_hour = bcd2bin(buf[2]);
	alrm->time.tm_mday = bcd2bin(buf[3]);
	alrm->time.tm_mon = bcd2bin(buf[4]) - 1;

	ret = regmap_read(pcf85363->regmap, CTRL_INTA_EN, &val);
	if (ret)
		return ret;

	alrm->enabled =  !!(val & INT_A1IE);

	return 0;
}

static int _pcf85363_rtc_alarm_irq_enable(struct pcf85363 *pcf85363, unsigned
					  int enabled)
{
	unsigned int alarm_flags = ALRM_SEC_A1E | ALRM_MIN_A1E | ALRM_HR_A1E |
				   ALRM_DAY_A1E | ALRM_MON_A1E;
	int ret;

	ret = regmap_update_bits(pcf85363->regmap, DT_ALARM_EN, alarm_flags,
				 enabled ? alarm_flags : 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(pcf85363->regmap, CTRL_INTA_EN,
				 INT_A1IE, enabled ? INT_A1IE : 0);

	if (ret || enabled)
		return ret;

	/* clear current flags */
	return pcf85363_clear_flags(pcf85363, FLAGS_A1F);
}

static int pcf85363_rtc_alarm_irq_enable(struct device *dev,
					 unsigned int enabled)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);

	return _pcf85363_rtc_alarm_irq_enable(pcf85363, enabled);
}

static int pcf85363_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);
	unsigned char buf[DT_MONTH_ALM1 - DT_SECOND_ALM1 + 1];
	int ret;

	buf[0] = bin2bcd(alrm->time.tm_sec);
	buf[1] = bin2bcd(alrm->time.tm_min);
	buf[2] = bin2bcd(alrm->time.tm_hour);
	buf[3] = bin2bcd(alrm->time.tm_mday);
	buf[4] = bin2bcd(alrm->time.tm_mon + 1);

	/*
	 * Disable the alarm interrupt before changing the value to avoid
	 * spurious interrupts
	 */
	ret = _pcf85363_rtc_alarm_irq_enable(pcf85363, 0);
	if (ret)
		return ret;

	ret = regmap_bulk_write(pcf85363->regmap, DT_SECOND_ALM1, buf,
				sizeof(buf));
	if (ret)
		return ret;

	return _pcf85363_rtc_alarm_irq_enable(pcf85363, alrm->enabled);
}

static const u8 pcf85363_ts_base[PCF85363_NUM_TS] = {
	DT_TIMESTAMP1, DT_TIMESTAMP2, DT_TIMESTAMP3,
};

static const u8 pcf85363_ts_flag[PCF85363_NUM_TS] = {
	FLAGS_TSR1F, FLAGS_TSR2F, FLAGS_TSR3F,
};

/* Mark which TSRs use a last-event mode; those need torn-read handling. */
static void pcf85363_classify_ts_modes(struct pcf85363 *pcf85363, u8 tsmode)
{
	u8 m1 = (tsmode >> TSR1_SHIFT) & TSR1_MASK;
	u8 m2 = (tsmode >> TSR2_SHIFT) & TSR2_MASK;
	u8 m3 = (tsmode >> TSR3_SHIFT) & TSR3_MASK;

	pcf85363->ts_last_event[0] = (m1 == PCF85363_TSR1_LE);
	pcf85363->ts_last_event[1] = (m2 == PCF85363_TSR2_LB ||
				      m2 == PCF85363_TSR2_LV ||
				      m2 == PCF85363_TSR2_LE);
	pcf85363->ts_last_event[2] = (m3 == PCF85363_TSR3_LB ||
				      m3 == PCF85363_TSR3_LV);
}

/*
 * Last-event registers can change under us; re-read the block until two
 * consecutive reads agree, then publish the stable value. Reject a torn
 * read with -EAGAIN once the retry budget is exhausted.
 */
static int pcf85363_read_ts_stable(struct pcf85363 *pcf85363, int i)
{
	u8 prev[PCF85363_TS_LEN], cur[PCF85363_TS_LEN];
	int retries, ret;

	ret = regmap_bulk_read(pcf85363->regmap, pcf85363_ts_base[i],
			       prev, PCF85363_TS_LEN);
	if (ret)
		return ret;

	for (retries = 0; retries < PCF85363_TS_READ_RETRIES; retries++) {
		ret = regmap_bulk_read(pcf85363->regmap, pcf85363_ts_base[i],
				       cur, PCF85363_TS_LEN);
		if (ret)
			return ret;

		if (!memcmp(prev, cur, PCF85363_TS_LEN)) {
			memcpy(pcf85363->ts[i].regs, cur, PCF85363_TS_LEN);
			return 0;
		}

		memcpy(prev, cur, PCF85363_TS_LEN);
	}

	return -EAGAIN;
}

/*
 * Latch pending events into the cache. First-event modes hold the value
 * until the flag is cleared, so cache before clearing; last-event modes
 * overwrite every event, so clear first then read until stable. Runs from
 * the IRQ and sysfs paths; caller holds the lock. Returns a negative errno
 * on a register-access failure (including -EAGAIN for a torn last-event
 * read), otherwise the handled flag mask.
 */
static int pcf85363_collect_events(struct pcf85363 *pcf85363)
{
	unsigned int flags;
	int handled = 0;
	int i, ret;

	ret = regmap_read(pcf85363->regmap, CTRL_FLAGS, &flags);
	if (ret)
		return ret;

	for (i = 0; i < PCF85363_NUM_TS; i++) {
		if (!(flags & pcf85363_ts_flag[i]))
			continue;

		if (pcf85363->ts_last_event[i]) {
			ret = pcf85363_clear_flags(pcf85363,
						   pcf85363_ts_flag[i]);
			if (ret)
				return ret;

			ret = pcf85363_read_ts_stable(pcf85363, i);
			if (ret)
				return ret;

			pcf85363->ts[i].valid = true;
			handled |= pcf85363_ts_flag[i];
		} else {
			ret = regmap_bulk_read(pcf85363->regmap,
					       pcf85363_ts_base[i],
					       pcf85363->ts[i].regs,
					       PCF85363_TS_LEN);
			if (ret)
				return ret;

			pcf85363->ts[i].valid = true;

			ret = pcf85363_clear_flags(pcf85363,
						   pcf85363_ts_flag[i]);
			if (ret)
				return ret;

			handled |= pcf85363_ts_flag[i];
		}
	}

	if (flags & FLAGS_BSF) {
		pcf85363->bsf = true;

		ret = pcf85363_clear_flags(pcf85363, FLAGS_BSF);
		if (ret)
			return ret;

		handled |= FLAGS_BSF;
	}

	return handled;
}

static irqreturn_t pcf85363_rtc_handle_irq(int irq, void *dev_id)
{
	struct pcf85363 *pcf85363 = i2c_get_clientdata(dev_id);
	bool handled = false;
	unsigned int flags;
	int err;

	err = regmap_read(pcf85363->regmap, CTRL_FLAGS, &flags);
	if (err)
		return IRQ_NONE;

	if (flags) {
		dev_dbg(&pcf85363->rtc->dev, "IRQ flags: 0x%02x%s%s%s%s%s%s\n",
			flags, (flags & FLAGS_A1F) ? " [A1F]" : "",
			(flags & FLAGS_TSR1F) ? " [TSR1F]" : "",
			(flags & FLAGS_TSR2F) ? " [TSR2F]" : "",
			(flags & FLAGS_TSR3F) ? " [TSR3F]" : "",
			(flags & FLAGS_BSF) ? " [BSF]" : "",
			(flags & FLAGS_WDF) ? " [WDF]" : "");
	}

	if (flags & FLAGS_A1F) {
		rtc_update_irq(pcf85363->rtc, 1, RTC_IRQF | RTC_AF);
		pcf85363_clear_flags(pcf85363, FLAGS_A1F);
		handled = true;
	}

	if (flags & (FLAGS_TSR1F | FLAGS_TSR2F | FLAGS_TSR3F | FLAGS_BSF)) {
		guard(mutex)(&pcf85363->lock);

		err = pcf85363_collect_events(pcf85363);
		if (err < 0)
			dev_err_ratelimited(&pcf85363->rtc->dev,
					    "failed to collect events: %d\n",
					    err);
		/*
		 * These are our interrupt sources even if servicing them hit
		 * an I/O error, so acknowledge the interrupt either way.
		 */
		handled = true;
	}

	if (flags & FLAGS_WDF) {
		dev_warn_ratelimited(&pcf85363->rtc->dev,
				     "watchdog timer expired\n");
		if (pcf85363_clear_flags(pcf85363, FLAGS_WDF))
			dev_err(&pcf85363->rtc->dev,
				"failed to clear watchdog flag\n");
		handled = true;
	}

	/*
	 * Clear flags this handler does not service (e.g. A2F/PIF); otherwise
	 * they hold the level-triggered INTA line asserted and storm the IRQ.
	 */
	if (flags & (FLAGS_A2F | FLAGS_PIF)) {
		pcf85363_clear_flags(pcf85363, FLAGS_A2F | FLAGS_PIF);
		handled = true;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/*
 * CTRL_OFFSET is a signed step count; the step is 2170 ppb (normal) or
 * 2034.5 ppb (fast/OFFM, scaled x10 to keep the arithmetic integer).
 */
static int pcf85363_read_offset(struct device *dev, long *offset)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);
	unsigned int val, osc;
	long steps;
	int ret;

	ret = regmap_read(pcf85363->regmap, CTRL_OFFSET, &val);
	if (ret)
		return ret;

	ret = regmap_read(pcf85363->regmap, CTRL_OSCILLATOR, &osc);
	if (ret)
		return ret;

	steps = sign_extend32(val, OFFSET_SIGN_BIT);

	if (osc & OSC_OFFM)
		*offset = steps * OFFSET_STEP_PPB_FAST_X10 / 10;
	else
		*offset = steps * OFFSET_STEP_PPB;

	return 0;
}

static int pcf85363_set_offset(struct device *dev, long offset)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);
	unsigned int osc;
	long steps;
	int ret;

	ret = regmap_read(pcf85363->regmap, CTRL_OSCILLATOR, &osc);
	if (ret)
		return ret;

	/* Range-check before the x10 scaling so the multiply cannot overflow. */
	if (offset > OFFSET_PPB_MAX || offset < OFFSET_PPB_MIN)
		return -ERANGE;

	if (osc & OSC_OFFM)
		steps = DIV_ROUND_CLOSEST(offset * 10, OFFSET_STEP_PPB_FAST_X10);
	else
		steps = DIV_ROUND_CLOSEST(offset, OFFSET_STEP_PPB);

	if (steps < -128 || steps > 127)
		return -ERANGE;

	return regmap_write(pcf85363->regmap, CTRL_OFFSET,
			    steps & OFFSET_MASK);
}

static int pcf85363_rtc_ioctl(struct device *dev,
			      unsigned int cmd, unsigned long arg)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);
	int ret;

	switch (cmd) {
	case RTC_VL_READ: {
		u32 status = 0;

		guard(mutex)(&pcf85363->lock);

		/* Refresh so a poll-only setup still latches the BSF flag. */
		ret = pcf85363_collect_events(pcf85363);
		if (ret < 0)
			return ret;

		if (pcf85363->bsf)
			status |= RTC_VL_BACKUP_SWITCH;

		return put_user(status, (u32 __user *)arg);
	}

	case RTC_VL_CLR: {
		guard(mutex)(&pcf85363->lock);

		ret = pcf85363_clear_flags(pcf85363, FLAGS_BSF);
		if (ret)
			return ret;

		pcf85363->bsf = false;

		return 0;
	}

	default:
		return -ENOIOCTLCMD;
	}
}

static const struct rtc_class_ops rtc_ops = {
	.ioctl  = pcf85363_rtc_ioctl,
	.read_time	= pcf85363_rtc_read_time,
	.set_time	= pcf85363_rtc_set_time,
	.read_alarm	= pcf85363_rtc_read_alarm,
	.set_alarm	= pcf85363_rtc_set_alarm,
	.alarm_irq_enable = pcf85363_rtc_alarm_irq_enable,
	.read_offset = pcf85363_read_offset,
	.set_offset = pcf85363_set_offset,
};

static int pcf85363_nvram_read(void *priv, unsigned int offset, void *val,
			       size_t bytes)
{
	struct pcf85363 *pcf85363 = priv;

	return regmap_bulk_read(pcf85363->regmap, CTRL_RAM + offset,
				val, bytes);
}

static int pcf85363_nvram_write(void *priv, unsigned int offset, void *val,
				size_t bytes)
{
	struct pcf85363 *pcf85363 = priv;

	return regmap_bulk_write(pcf85363->regmap, CTRL_RAM + offset,
				 val, bytes);
}

static int pcf85x63_nvram_read(void *priv, unsigned int offset, void *val,
			       size_t bytes)
{
	struct pcf85363 *pcf85363 = priv;
	unsigned int tmp_val;
	int ret;

	ret = regmap_read(pcf85363->regmap, CTRL_RAMBYTE, &tmp_val);
	(*(unsigned char *) val) = (unsigned char) tmp_val;

	return ret;
}

static int pcf85x63_nvram_write(void *priv, unsigned int offset, void *val,
				size_t bytes)
{
	struct pcf85363 *pcf85363 = priv;
	unsigned char tmp_val;

	tmp_val = *((unsigned char *)val);
	return regmap_write(pcf85363->regmap, CTRL_RAMBYTE,
				(unsigned int)tmp_val);
}

static const struct pcf85x63_config pcf_85263_config = {
	.regmap = {
		.reg_bits = 8,
		.val_bits = 8,
		.max_register = 0x2f,
	},
	.num_nvram = 1
};

static const struct pcf85x63_config pcf_85363_config = {
	.regmap = {
		.reg_bits = 8,
		.val_bits = 8,
		.max_register = 0x7f,
	},
	.num_nvram = 2
};

/*
 * Program the watchdog counter (WDR) so the reported timeout is never
 * shorter than requested: the first period after a reload lasts between
 * WDR and WDR-1 counts, so add one count of margin. Timeouts up to 30 s
 * use the 1 Hz step (1 s/count); longer ones the 0.25 Hz step (4 s/count).
 */
static void pcf85363_wdt_select_clock(struct pcf85363_watchdog *wd)
{
	unsigned int timeout = wd->wdd.timeout;

	if (timeout <= WD_TIMEOUT_1HZ_MAX) {
		wd->clock_sel = WD_CLKSEL_1HZ;
		wd->timeout_val = timeout + 1;
		wd->wdd.timeout = timeout;
	} else {
		wd->clock_sel = WD_CLKSEL_0_25HZ;
		wd->timeout_val = DIV_ROUND_UP(timeout, 4) + 1;
		wd->wdd.timeout = DIV_ROUND_UP(timeout, 4) * 4;
	}
}

/* Repeat mode restarts the watchdog automatically after each period. */
static int pcf85363_wdt_reload(struct pcf85363_watchdog *wd)
{
	u8 val;

	val = WD_MODE_REPEAT |
	      ((wd->timeout_val & WD_COUNT_MAX) << WD_TIMEOUT_SHIFT) |
	      (wd->clock_sel & WD_CLKSEL_MASK);

	return regmap_write(wd->pcf85363->regmap, CTRL_WDOG, val);
}

static int pcf85363_wdt_start(struct watchdog_device *wdd)
{
	struct pcf85363_watchdog *wd = watchdog_get_drvdata(wdd);
	int ret;

	/* Route WDF to INTA; alarm-only, the chip has no reset output. */
	ret = regmap_update_bits(wd->pcf85363->regmap, CTRL_INTA_EN,
				 INT_WDIE, INT_WDIE);
	if (ret)
		return ret;

	ret = pcf85363_wdt_reload(wd);
	if (ret)
		regmap_update_bits(wd->pcf85363->regmap, CTRL_INTA_EN,
				   INT_WDIE, 0);

	return ret;
}

static int pcf85363_wdt_stop(struct watchdog_device *wdd)
{
	struct pcf85363_watchdog *wd = watchdog_get_drvdata(wdd);
	int ret;

	/* Halt the counter first so a failed disable cannot leave it armed. */
	ret = regmap_write(wd->pcf85363->regmap, CTRL_WDOG, 0);
	if (ret)
		return ret;

	return regmap_update_bits(wd->pcf85363->regmap, CTRL_INTA_EN,
				  INT_WDIE, 0);
}

static int pcf85363_wdt_ping(struct watchdog_device *wdd)
{
	struct pcf85363_watchdog *wd = watchdog_get_drvdata(wdd);
	int ret;

	ret = pcf85363_clear_flags(wd->pcf85363, FLAGS_WDF);
	if (ret)
		return ret;

	return pcf85363_wdt_reload(wd);
}

static int pcf85363_wdt_set_timeout(struct watchdog_device *wdd,
				    unsigned int timeout)
{
	struct pcf85363_watchdog *wd = watchdog_get_drvdata(wdd);

	wdd->timeout = timeout;

	pcf85363_wdt_select_clock(wd);

	/*
	 * Programming the counter with a non-zero value starts it, so only
	 * reprogram when the watchdog is already running; the core keeps the
	 * new timeout for the next start otherwise.
	 */
	if (!watchdog_active(wdd))
		return 0;

	return pcf85363_wdt_reload(wd);
}

static const struct watchdog_info pcf85363_wdt_info = {
	.identity = "PCF85363 Watchdog",
	.options = WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT | WDIOF_ALARMONLY,
};

static const struct watchdog_ops pcf85363_wdt_ops = {
	.owner = THIS_MODULE,
	.start = pcf85363_wdt_start,
	.stop = pcf85363_wdt_stop,
	.ping = pcf85363_wdt_ping,
	.set_timeout = pcf85363_wdt_set_timeout,
};

/*
 * CTRL_WDOG reads back the live countdown, which decrements while the
 * watchdog runs, so a single read can catch a transient value. Read until
 * two consecutive reads agree before deciding whether the watchdog is armed.
 */
static int pcf85363_read_wdog_stable(struct pcf85363 *pcf85363, unsigned int *out)
{
	unsigned int prev, cur;
	int retries, ret;

	ret = regmap_read(pcf85363->regmap, CTRL_WDOG, &prev);
	if (ret)
		return ret;

	for (retries = 0; retries < WD_READ_RETRIES; retries++) {
		ret = regmap_read(pcf85363->regmap, CTRL_WDOG, &cur);
		if (ret)
			return ret;

		if (cur == prev) {
			*out = cur;
			return 0;
		}

		prev = cur;
	}

	*out = cur;
	return 0;
}

static int pcf85363_watchdog_init(struct device *dev, struct pcf85363 *pcf85363)
{
	struct pcf85363_watchdog *wd;
	unsigned int regval;
	bool running;
	int ret;

	if (!IS_ENABLED(CONFIG_WATCHDOG))
		return 0;

	wd = devm_kzalloc(dev, sizeof(*wd), GFP_KERNEL);
	if (!wd)
		return -ENOMEM;

	wd->pcf85363 = pcf85363;

	wd->wdd.info = &pcf85363_wdt_info;
	wd->wdd.ops = &pcf85363_wdt_ops;
	wd->wdd.min_timeout = WD_TIMEOUT_MIN;
	wd->wdd.max_timeout = WD_TIMEOUT_MAX;
	wd->wdd.timeout = WD_DEFAULT_TIMEOUT;
	wd->wdd.parent = dev;
	wd->wdd.status = WATCHDOG_NOWAYOUT_INIT_STATUS;

	/*
	 * Fixed default timeout; userspace can change it via WDIOC_SETTIMEOUT.
	 * CTRL_WDOG reads back the live countdown, not the programmed reload
	 * value, so it must not be decoded into wdd.timeout: adopt the known
	 * default and let the watchdog core's first keep-alive reload it.
	 */
	pcf85363_wdt_select_clock(wd);

	/*
	 * The counter and WDIE are battery-backed and survive a reboot, so a
	 * bootloader may leave the watchdog armed. Detect that and hand it to
	 * the core as already-running rather than silently disabling it; only
	 * a stopped watchdog gets WDIE masked so a stale enable cannot storm
	 * the level-triggered INTA line.
	 */
	ret = pcf85363_read_wdog_stable(pcf85363, &regval);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read watchdog register\n");

	running = ((regval >> WD_TIMEOUT_SHIFT) & WD_COUNT_MAX) != 0;

	ret = pcf85363_clear_flags(pcf85363, FLAGS_WDF);
	if (ret)
		return dev_err_probe(dev, ret, "failed to clear WDF\n");

	/*
	 * Once WDOG_HW_RUNNING is set the core keeps the watchdog alive via
	 * .ping(), which does not touch WDIE, so arm WDIE here for an adopted
	 * watchdog; otherwise keep it masked.
	 */
	ret = regmap_update_bits(pcf85363->regmap, CTRL_INTA_EN, INT_WDIE,
				 running ? INT_WDIE : 0);
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure WDIE\n");

	if (running)
		set_bit(WDOG_HW_RUNNING, &wd->wdd.status);

	watchdog_set_drvdata(&wd->wdd, wd);

	return devm_watchdog_register_device(dev, &wd->wdd);
}

/* Six BCD bytes; bit 7 of the seconds byte is reserved, not time data. */
static ssize_t pcf85363_format_timestamp(const u8 *regs, char *buf)
{
	struct rtc_time tm;

	tm.tm_sec = bcd2bin(regs[0] & PCF85363_SEC_MASK);
	tm.tm_min = bcd2bin(regs[1]);
	tm.tm_hour = bcd2bin(regs[2]);
	tm.tm_mday = bcd2bin(regs[3]);
	tm.tm_mon = bcd2bin(regs[4]) - 1;
	tm.tm_year = bcd2bin(regs[5]) + 100;

	return sysfs_emit(buf, "%04d-%02d-%02d %02d:%02d:%02d\n",
			  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			  tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Refresh from hardware first so poll-only setups still latch events. */
static ssize_t pcf85363_timestamp_show(struct device *dev, char *buf,
				       unsigned int index)
{
	struct pcf85363 *pcf85363 = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&pcf85363->lock);

	ret = pcf85363_collect_events(pcf85363);
	if (ret < 0)
		return ret;

	if (!pcf85363->ts[index].valid)
		return sysfs_emit(buf, "00-00-00 00:00:00\n");

	return pcf85363_format_timestamp(pcf85363->ts[index].regs, buf);
}

static ssize_t timestamp1_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return pcf85363_timestamp_show(dev, buf, 0);
}
static DEVICE_ATTR_RO(timestamp1);

static ssize_t timestamp2_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return pcf85363_timestamp_show(dev, buf, 1);
}
static DEVICE_ATTR_RO(timestamp2);

static ssize_t timestamp3_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return pcf85363_timestamp_show(dev, buf, 2);
}
static DEVICE_ATTR_RO(timestamp3);

static struct attribute *pcf85363_attrs[] = {
	&dev_attr_timestamp1.attr,
	&dev_attr_timestamp2.attr,
	&dev_attr_timestamp3.attr,
	NULL,
};

static const struct attribute_group pcf85363_attr_group = {
	.attrs = pcf85363_attrs,
};

static int pcf85363_probe(struct i2c_client *client)
{
	const struct pcf85x63_config *config = &pcf_85363_config;
	const void *data = of_device_get_match_data(&client->dev);
	struct device *dev = &client->dev;
	struct pcf85363 *pcf85363;
	int irq_a = client->irq;
	bool ts_mode_configured = false;
	bool wakeup_source;
	int ret, i, err;
	u32 tsr_mode[3];
	u8 val;

	struct nvmem_config nvmem_cfg[] = {
		{
			.name = "pcf85x63-",
			.word_size = 1,
			.stride = 1,
			.size = 1,
			.reg_read = pcf85x63_nvram_read,
			.reg_write = pcf85x63_nvram_write,
		}, {
			.name = "pcf85363-",
			.word_size = 1,
			.stride = 1,
			.size = NVRAM_SIZE,
			.reg_read = pcf85363_nvram_read,
			.reg_write = pcf85363_nvram_write,
		},
	};

	if (data)
		config = data;

	pcf85363 = devm_kzalloc(&client->dev, sizeof(*pcf85363), GFP_KERNEL);
	if (!pcf85363)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &pcf85363->lock);
	if (ret)
		return ret;

	pcf85363->regmap = devm_regmap_init_i2c(client, &config->regmap);

	if (IS_ERR(pcf85363->regmap))
		return dev_err_probe(dev, PTR_ERR(pcf85363->regmap), "regmap init failed\n");

	i2c_set_clientdata(client, pcf85363);

	ret = regmap_update_bits(pcf85363->regmap, CTRL_FUNCTION, RTCM_BIT, 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable RTC mode\n");

	if (!device_property_read_u32_array(dev, "nxp,timestamp-mode", tsr_mode, 3)) {
		bool ts_pin_used;

		tsr_mode[0] &= TSR1_MASK;
		tsr_mode[1] &= TSR2_MASK;
		tsr_mode[2] &= TSR3_MASK;

		val = (tsr_mode[2] << TSR3_SHIFT) |
		      (tsr_mode[1] << TSR2_SHIFT) |
		      (tsr_mode[0] << TSR1_SHIFT);

		ret = regmap_write(pcf85363->regmap, DT_TS_MODE, val);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to write timestamp mode register\n");

		pcf85363_classify_ts_modes(pcf85363, val);

		ts_mode_configured = tsr_mode[0] || tsr_mode[1] || tsr_mode[2];

		/*
		 * Only the TS-pin capture modes drive the TS pin. Select the
		 * timestamp function (TSPM) for those and leave the input mode
		 * (TSIM) at its reset default rather than forcing the
		 * mechanical-switch detector.
		 */
		ts_pin_used = tsr_mode[0] == PCF85363_TSR1_FE ||
			      tsr_mode[0] == PCF85363_TSR1_LE ||
			      tsr_mode[1] == PCF85363_TSR2_FE ||
			      tsr_mode[1] == PCF85363_TSR2_LE;

		if (ts_pin_used) {
			ret = regmap_update_bits(pcf85363->regmap, CTRL_PIN_IO,
						 PIN_IO_TSPM, PIN_IO_TSPM);
			if (ret)
				return dev_err_probe(dev, ret,
						     "Failed to configure TS pin\n");
		}

		dev_dbg(dev, "Timestamp mode set: TSR1=0x%x TSR2=0x%x TSR3=0x%x\n",
			tsr_mode[0], tsr_mode[1], tsr_mode[2]);
	}

	pcf85363->rtc = devm_rtc_allocate_device(&client->dev);
	if (IS_ERR(pcf85363->rtc))
		return PTR_ERR(pcf85363->rtc);

	err = pcf85363_load_capacitance(pcf85363, client->dev.of_node);
	if (err < 0)
		return dev_err_probe(&client->dev, err,
				     "failed to set xtal load capacitance\n");

	pcf85363->rtc->ops = &rtc_ops;
	pcf85363->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	pcf85363->rtc->range_max = RTC_TIMESTAMP_END_2099;

	wakeup_source = device_property_read_bool(dev, "wakeup-source");

	/*
	 * Latch pre-probe events instead of blanket-clearing CTRL_FLAGS, so
	 * pre-existing timestamps and the battery-switch flag are not lost.
	 */
	scoped_guard(mutex, &pcf85363->lock) {
		ret = pcf85363_collect_events(pcf85363);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "Failed to latch boot-time events\n");
	}

	/*
	 * Battery-backed registers can retain stale state across a power cycle.
	 * A stale asserted flag would storm the level-triggered INTA line, so
	 * disable the interrupt sources and Alarm2 enables this driver does not
	 * arm here and clear their flags; leave the managed sources (A1IE, BSIE,
	 * TSRIE) and Alarm1 for their own paths to arm. WDIE is masked too so a
	 * bootloader-armed watchdog cannot storm INTA before the driver is ready
	 * to service it.
	 */
	ret = regmap_update_bits(pcf85363->regmap, CTRL_INTA_EN,
				 INT_WDIE | INT_A2IE | INT_OIE | INT_PIE | INT_ILP,
				 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to mask unused INTA sources\n");

	ret = regmap_update_bits(pcf85363->regmap, DT_ALARM_EN,
				 ALRM_MIN_A2E | ALRM_HR_A2E | ALRM_DAY_A2E, 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to mask Alarm2 enables\n");

	ret = pcf85363_clear_flags(pcf85363, FLAGS_A2F | FLAGS_PIF);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to clear stale flags\n");

	if (irq_a > 0) {
		unsigned long irqflags = IRQF_TRIGGER_LOW;

		if (dev_fwnode(&client->dev))
			irqflags = 0;

		ret = devm_request_threaded_irq(dev, irq_a, NULL,
						pcf85363_rtc_handle_irq,
						irqflags | IRQF_ONESHOT,
						"pcf85363-inta", client);
		/*
		 * Let the driver core retry when the interrupt provider is not
		 * ready yet; only a genuine failure disables interrupt-driven
		 * alarms while leaving RTC timekeeping (and any wakeup-source
		 * path) intact.
		 */
		if (ret == -EPROBE_DEFER)
			return ret;
		if (ret) {
			dev_warn(dev, "unable to request IRQ, alarms disabled: %d\n",
				 ret);
			irq_a = 0;
		}
	}

	if (irq_a > 0 || wakeup_source) {
		/*
		 * The alarm can be delivered either through our own IRQ line or
		 * via an external wakeup path (INTA routed to a PMIC), so route
		 * INTA to its interrupt output and keep the alarm feature in
		 * both cases; rtcwake relies on it for wakeup-source-only boards.
		 */
		ret = regmap_update_bits(pcf85363->regmap, CTRL_PIN_IO,
					 PIN_IO_INTAPM, PIN_IO_INTA_OUT);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to configure INTA pin\n");

		if (irq_a > 0) {
			u8 inta_en = INT_BSIE;

			/*
			 * Enable the timestamp interrupt only when a capture
			 * mode is configured; always enable battery-switch.
			 * Only relevant when we service the IRQ ourselves.
			 */
			if (ts_mode_configured)
				inta_en |= INT_TSRIE;

			ret = regmap_update_bits(pcf85363->regmap, CTRL_INTA_EN,
						 INT_BSIE | INT_TSRIE, inta_en);
			if (ret)
				return dev_err_probe(dev, ret,
						     "Failed to enable INTA sources\n");
		}

		device_init_wakeup(dev, true);
		set_bit(RTC_FEATURE_ALARM, pcf85363->rtc->features);
	} else {
		/*
		 * Neither an interrupt line nor a wakeup source: the alarm
		 * cannot be delivered, so drop the alarm feature.
		 */
		clear_bit(RTC_FEATURE_ALARM, pcf85363->rtc->features);
	}

	/*
	 * Watchdog expiry is only signalled via INTA (no reset output), so it
	 * needs an interrupt line to be usable.
	 */
	if (irq_a > 0) {
		ret = pcf85363_watchdog_init(dev, pcf85363);
		if (ret)
			return dev_err_probe(dev, ret, "Watchdog init failed\n");
	}

	dev_set_drvdata(&pcf85363->rtc->dev, pcf85363);

	ret = rtc_add_group(pcf85363->rtc, &pcf85363_attr_group);
	if (ret)
		return ret;

	ret = devm_rtc_register_device(pcf85363->rtc);
	if (ret)
		return dev_err_probe(dev, ret, "RTC registration failed\n");

	for (i = 0; i < config->num_nvram; i++) {
		nvmem_cfg[i].priv = pcf85363;
		devm_rtc_nvmem_register(pcf85363->rtc, &nvmem_cfg[i]);
	}

	return ret;
}

static const __maybe_unused struct of_device_id dev_ids[] = {
	{ .compatible = "nxp,pcf85263", .data = &pcf_85263_config },
	{ .compatible = "nxp,pcf85363", .data = &pcf_85363_config },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dev_ids);

static struct i2c_driver pcf85363_driver = {
	.driver	= {
		.name	= "pcf85363",
		.of_match_table = of_match_ptr(dev_ids),
	},
	.probe = pcf85363_probe,
};

module_i2c_driver(pcf85363_driver);

MODULE_AUTHOR("Eric Nelson");
MODULE_DESCRIPTION("pcf85263/pcf85363 I2C RTC driver");
MODULE_LICENSE("GPL");
