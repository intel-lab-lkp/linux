// SPDX-License-Identifier: GPL-2.0
/*
 * RTC driver for the Micro Crystal RV8803
 *
 * Copyright (C) 2015 Micro Crystal SA
 * Alexandre Belloni <alexandre.belloni@bootlin.com>
 *
 */

#include <linux/bcd.h>
#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/rtc.h>
#include <linux/pm_wakeirq.h>

#define RV8803_I2C_TRY_COUNT		4

#define RV8803_SEC			0x00
#define RV8803_MIN			0x01
#define RV8803_HOUR			0x02
#define RV8803_WEEK			0x03
#define RV8803_DAY			0x04
#define RV8803_MONTH			0x05
#define RV8803_YEAR			0x06
#define RV8803_RAM			0x07
#define RV8803_ALARM_MIN		0x08
#define RV8803_ALARM_HOUR		0x09
#define RV8803_ALARM_WEEK_OR_DAY	0x0A
#define RV8803_EXT			0x0D
#define RV8803_FLAG			0x0E
#define RV8803_CTRL			0x0F
#define RV8803_OSC_OFFSET		0x2C

#define RV8803_EXT_WADA			BIT(6)

#define RV8803_FLAG_V1F			BIT(0)
#define RV8803_FLAG_V2F			BIT(1)
#define RV8803_FLAG_AF			BIT(3)
#define RV8803_FLAG_TF			BIT(4)
#define RV8803_FLAG_UF			BIT(5)

#define RV8803_CTRL_RESET		BIT(0)

#define RV8803_CTRL_EIE			BIT(2)
#define RV8803_CTRL_AIE			BIT(3)
#define RV8803_CTRL_TIE			BIT(4)
#define RV8803_CTRL_UIE			BIT(5)

#define RX8803_CTRL_CSEL		GENMASK(7, 6)

#define RX8900_BACKUP_CTRL		0x18
#define RX8900_FLAG_SWOFF		BIT(2)
#define RX8900_FLAG_VDETOFF		BIT(3)

#define RX8901_EVIN_EN			0x20
#define RX8901_EVIN1_CFG		0x21
#define RX8901_EVIN2_CFG		0x23
#define RX8901_EVIN3_CFG		0x25
#define RX8901_EVENTx_CFG_POL		GENMASK(1, 0)
#define RX8901_EVENTx_CFG_PUPD		GENMASK(4, 2)

#define RX8901_EVIN1_FLT		0x22
#define RX8901_EVIN2_FLT		0x24
#define RX8901_EVIN3_FLT		0x26

#define RX8901_BUF1_CFG1		0x27

#define RX8901_BUF1_STAT		0x28
#define RX8901_BUFx_STAT_PTR		GENMASK(5, 0)
#define RX8901_WRCMD_CFG		0x41
#define RX8901_WRCMD_TRG		0x42

#define RX8901_EVNT_INTE		0x43

#define RX8901_BUF_INTF			0x46
#define RX8901_BUF_INTF_BUF1F		BIT(5)

#define RX8901_EVNT_INTF		0x47

#define NO_OF_EVIN			3

#define EVIN_FILTER_MAX			40

enum rv8803_type {
	rv_8803,
	rx_8803,
	rx_8804,
	rx_8900,
	rx_8901,
};

enum evin_pull_resistor {
	no = 0b000,
	pull_up_500k = 0b001,
	pull_up_1M = 0b010,
	pull_up_10M = 0b011,
	pull_down_500k = 0b100,
};

enum evin_trigger {
	falling_edge = 0b00,
	rising_edge = 0b01,
	both_edges = 0b10,
};

struct cfg_val_txt {
	char *txt;
	u8 val;
	bool hide;
};

static const struct cfg_val_txt trg_status_txt[] = {
	{ "EVIN1", BIT(5) },
	{ "EVIN2", BIT(6) },
	{ "EVIN3", BIT(7) },
	{ "CMD", BIT(4) },
	{ "VBATL", BIT(3) },
	{ "VTMPL", BIT(2) },
	{ "VDDL", BIT(1) },
	{ "OSCSTP", BIT(0) },
	{ NULL }
};

static const u8 evin_cfg_regs[] = {
	RX8901_EVIN1_CFG,
	RX8901_EVIN2_CFG,
	RX8901_EVIN3_CFG
};

static const u8 evin_flt_regs[] = {
	RX8901_EVIN1_FLT,
	RX8901_EVIN2_FLT,
	RX8901_EVIN3_FLT
};

struct rv8803_data {
	struct i2c_client *client;
	struct rtc_device *rtc;
	struct mutex flags_lock;
	u8 ctrl;
	u8 backup;
	u8 alarm_invalid:1;
	enum rv8803_type type;
};

static int rv8803_read_reg(const struct i2c_client *client, u8 reg)
{
	int try = RV8803_I2C_TRY_COUNT;
	s32 ret;

	/*
	 * There is a 61µs window during which the RTC does not acknowledge I2C
	 * transfers. In that case, ensure that there are multiple attempts.
	 */
	do
		ret = i2c_smbus_read_byte_data(client, reg);
	while ((ret == -ENXIO || ret == -EIO) && --try);
	if (ret < 0)
		dev_err(&client->dev, "Unable to read register 0x%02x\n", reg);

	return ret;
}

static int rv8803_read_regs(const struct i2c_client *client,
			    u8 reg, u8 count, u8 *values)
{
	int try = RV8803_I2C_TRY_COUNT;
	s32 ret;

	do
		ret = i2c_smbus_read_i2c_block_data(client, reg, count, values);
	while ((ret == -ENXIO || ret == -EIO) && --try);
	if (ret != count) {
		dev_err(&client->dev,
			"Unable to read registers 0x%02x..0x%02x\n",
			reg, reg + count - 1);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static int rv8803_write_reg(const struct i2c_client *client, u8 reg, u8 value)
{
	int try = RV8803_I2C_TRY_COUNT;
	s32 ret;

	do
		ret = i2c_smbus_write_byte_data(client, reg, value);
	while ((ret == -ENXIO || ret == -EIO) && --try);
	if (ret)
		dev_err(&client->dev, "Unable to write register 0x%02x\n", reg);

	return ret;
}

static int rv8803_write_regs(const struct i2c_client *client,
			     u8 reg, u8 count, const u8 *values)
{
	int try = RV8803_I2C_TRY_COUNT;
	s32 ret;

	do
		ret = i2c_smbus_write_i2c_block_data(client, reg, count,
						     values);
	while ((ret == -ENXIO || ret == -EIO) && --try);
	if (ret)
		dev_err(&client->dev,
			"Unable to write registers 0x%02x..0x%02x\n",
			reg, reg + count - 1);

	return ret;
}

static int rv8803_regs_init(struct rv8803_data *rv8803)
{
	int ret;

	ret = rv8803_write_reg(rv8803->client, RV8803_OSC_OFFSET, 0x00);
	if (ret)
		return ret;

	ret = rv8803_write_reg(rv8803->client, RV8803_CTRL,
			       FIELD_PREP(RX8803_CTRL_CSEL, 1)); /* 2s */
	if (ret)
		return ret;

	ret = rv8803_write_regs(rv8803->client, RV8803_ALARM_MIN, 3,
				(u8[]){ 0, 0, 0 });
	if (ret)
		return ret;

	return rv8803_write_reg(rv8803->client, RV8803_RAM, 0x00);
}

static int rv8803_regs_configure(struct rv8803_data *rv8803);

static int rv8803_regs_reset(struct rv8803_data *rv8803, bool full)
{
	/*
	 * The RV-8803 resets all registers to POR defaults after voltage-loss,
	 * the Epson RTCs don't, so we manually reset the remainder here.
	 */
	if (full || rv8803->type == rx_8803 || rv8803->type == rx_8900 ||
	    rv8803->type == rx_8901) {
		int ret = rv8803_regs_init(rv8803);
		if (ret)
			return ret;
	}

	return rv8803_regs_configure(rv8803);
}

static irqreturn_t rv8803_handle_irq(int irq, void *dev_id)
{
	struct i2c_client *client = dev_id;
	struct rv8803_data *rv8803 = i2c_get_clientdata(client);
	unsigned long events = 0;
	int flags;

	mutex_lock(&rv8803->flags_lock);

	flags = rv8803_read_reg(client, RV8803_FLAG);
	if (flags <= 0) {
		mutex_unlock(&rv8803->flags_lock);
		return IRQ_NONE;
	}

	if (flags & RV8803_FLAG_V1F)
		dev_warn(&client->dev, "Voltage low, temperature compensation stopped.\n");

	if (flags & RV8803_FLAG_V2F)
		dev_warn(&client->dev, "Voltage low, data loss detected.\n");

	if (flags & RV8803_FLAG_TF) {
		flags &= ~RV8803_FLAG_TF;
		rv8803->ctrl &= ~RV8803_CTRL_TIE;
		events |= RTC_PF;
	}

	if (flags & RV8803_FLAG_AF) {
		flags &= ~RV8803_FLAG_AF;
		rv8803->ctrl &= ~RV8803_CTRL_AIE;
		events |= RTC_AF;
	}

	if (flags & RV8803_FLAG_UF) {
		flags &= ~RV8803_FLAG_UF;
		rv8803->ctrl &= ~RV8803_CTRL_UIE;
		events |= RTC_UF;
	}

	if (events) {
		rtc_update_irq(rv8803->rtc, 1, events);
		rv8803_write_reg(client, RV8803_FLAG, flags);
		rv8803_write_reg(rv8803->client, RV8803_CTRL, rv8803->ctrl);
	}

	mutex_unlock(&rv8803->flags_lock);

	return IRQ_HANDLED;
}

static int rv8803_get_time(struct device *dev, struct rtc_time *tm)
{
	struct rv8803_data *rv8803 = dev_get_drvdata(dev);
	u8 date1[7];
	u8 date2[7];
	u8 *date = date1;
	int ret, flags;

	if (rv8803->alarm_invalid) {
		dev_warn(dev, "Corruption detected, data may be invalid.\n");
		return -EINVAL;
	}

	flags = rv8803_read_reg(rv8803->client, RV8803_FLAG);
	if (flags < 0)
		return flags;

	if (flags & RV8803_FLAG_V2F) {
		dev_warn(dev, "Voltage low, data is invalid.\n");
		return -EINVAL;
	}

	ret = rv8803_read_regs(rv8803->client, RV8803_SEC, 7, date);
	if (ret)
		return ret;

	if ((date1[RV8803_SEC] & 0x7f) == bin2bcd(59)) {
		ret = rv8803_read_regs(rv8803->client, RV8803_SEC, 7, date2);
		if (ret)
			return ret;

		if ((date2[RV8803_SEC] & 0x7f) != bin2bcd(59))
			date = date2;
	}

	tm->tm_sec  = bcd2bin(date[RV8803_SEC] & 0x7f);
	tm->tm_min  = bcd2bin(date[RV8803_MIN] & 0x7f);
	tm->tm_hour = bcd2bin(date[RV8803_HOUR] & 0x3f);
	tm->tm_wday = ilog2(date[RV8803_WEEK] & 0x7f);
	tm->tm_mday = bcd2bin(date[RV8803_DAY] & 0x3f);
	tm->tm_mon  = bcd2bin(date[RV8803_MONTH] & 0x1f) - 1;
	tm->tm_year = bcd2bin(date[RV8803_YEAR]) + 100;

	return 0;
}

static int rv8803_set_time(struct device *dev, struct rtc_time *tm)
{
	struct rv8803_data *rv8803 = dev_get_drvdata(dev);
	u8 date[7];
	int ctrl, flags, ret;

	ctrl = rv8803_read_reg(rv8803->client, RV8803_CTRL);
	if (ctrl < 0)
		return ctrl;

	/* Stop the clock */
	ret = rv8803_write_reg(rv8803->client, RV8803_CTRL,
			       ctrl | RV8803_CTRL_RESET);
	if (ret)
		return ret;

	date[RV8803_SEC]   = bin2bcd(tm->tm_sec);
	date[RV8803_MIN]   = bin2bcd(tm->tm_min);
	date[RV8803_HOUR]  = bin2bcd(tm->tm_hour);
	date[RV8803_WEEK]  = 1 << (tm->tm_wday);
	date[RV8803_DAY]   = bin2bcd(tm->tm_mday);
	date[RV8803_MONTH] = bin2bcd(tm->tm_mon + 1);
	date[RV8803_YEAR]  = bin2bcd(tm->tm_year - 100);

	ret = rv8803_write_regs(rv8803->client, RV8803_SEC, 7, date);
	if (ret)
		return ret;

	/* Restart the clock */
	ret = rv8803_write_reg(rv8803->client, RV8803_CTRL,
			       ctrl & ~RV8803_CTRL_RESET);
	if (ret)
		return ret;

	mutex_lock(&rv8803->flags_lock);

	flags = rv8803_read_reg(rv8803->client, RV8803_FLAG);
	if (flags < 0) {
		mutex_unlock(&rv8803->flags_lock);
		return flags;
	}

	if ((flags & RV8803_FLAG_V2F) || rv8803->alarm_invalid) {
		/*
		 * If we sense corruption in the alarm registers, but see no
		 * voltage loss flag, we can't rely on other registers having
		 * sensible values. Reset them fully.
		 */
		ret = rv8803_regs_reset(rv8803, rv8803->alarm_invalid);
		if (ret) {
			mutex_unlock(&rv8803->flags_lock);
			return ret;
		}

		rv8803->alarm_invalid = false;
	}

	ret = rv8803_write_reg(rv8803->client, RV8803_FLAG,
			       flags & ~(RV8803_FLAG_V1F | RV8803_FLAG_V2F));

	mutex_unlock(&rv8803->flags_lock);

	return ret;
}

static int rv8803_get_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rv8803_data *rv8803 = dev_get_drvdata(dev);
	struct i2c_client *client = rv8803->client;
	u8 alarmvals[3];
	int flags, ret;

	ret = rv8803_read_regs(client, RV8803_ALARM_MIN, 3, alarmvals);
	if (ret)
		return ret;

	flags = rv8803_read_reg(client, RV8803_FLAG);
	if (flags < 0)
		return flags;

	alarmvals[0] &= 0x7f;
	alarmvals[1] &= 0x3f;
	alarmvals[2] &= 0x3f;

	if (!bcd_is_valid(alarmvals[0]) ||
	    !bcd_is_valid(alarmvals[1]) ||
	    !bcd_is_valid(alarmvals[2]))
		goto err_invalid;

	alrm->time.tm_sec  = 0;
	alrm->time.tm_min  = bcd2bin(alarmvals[0]);
	alrm->time.tm_hour = bcd2bin(alarmvals[1]);
	alrm->time.tm_mday = bcd2bin(alarmvals[2]);

	alrm->enabled = !!(rv8803->ctrl & RV8803_CTRL_AIE);
	alrm->pending = (flags & RV8803_FLAG_AF) && alrm->enabled;

	if ((unsigned int)alrm->time.tm_mday > 31 ||
	    (unsigned int)alrm->time.tm_hour >= 24 ||
	    (unsigned int)alrm->time.tm_min >= 60)
		goto err_invalid;

	return 0;

err_invalid:
	rv8803->alarm_invalid = true;
	return -EINVAL;
}

static int rv8803_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rv8803_data *rv8803 = dev_get_drvdata(dev);
	u8 alarmvals[3];
	u8 ctrl[2];
	int ret, err;

	/* The alarm has no seconds, round up to nearest minute */
	if (alrm->time.tm_sec) {
		time64_t alarm_time = rtc_tm_to_time64(&alrm->time);

		alarm_time += 60 - alrm->time.tm_sec;
		rtc_time64_to_tm(alarm_time, &alrm->time);
	}

	mutex_lock(&rv8803->flags_lock);

	ret = rv8803_read_regs(client, RV8803_FLAG, 2, ctrl);
	if (ret) {
		mutex_unlock(&rv8803->flags_lock);
		return ret;
	}

	alarmvals[0] = bin2bcd(alrm->time.tm_min);
	alarmvals[1] = bin2bcd(alrm->time.tm_hour);
	alarmvals[2] = bin2bcd(alrm->time.tm_mday);

	if (rv8803->ctrl & (RV8803_CTRL_AIE | RV8803_CTRL_UIE)) {
		rv8803->ctrl &= ~(RV8803_CTRL_AIE | RV8803_CTRL_UIE);
		err = rv8803_write_reg(rv8803->client, RV8803_CTRL,
				       rv8803->ctrl);
		if (err) {
			mutex_unlock(&rv8803->flags_lock);
			return err;
		}
	}

	ctrl[0] &= ~RV8803_FLAG_AF;
	err = rv8803_write_reg(rv8803->client, RV8803_FLAG, ctrl[0]);
	mutex_unlock(&rv8803->flags_lock);
	if (err)
		return err;

	err = rv8803_write_regs(rv8803->client, RV8803_ALARM_MIN, 3, alarmvals);
	if (err)
		return err;

	if (alrm->enabled) {
		if (rv8803->rtc->uie_rtctimer.enabled)
			rv8803->ctrl |= RV8803_CTRL_UIE;
		if (rv8803->rtc->aie_timer.enabled)
			rv8803->ctrl |= RV8803_CTRL_AIE;

		err = rv8803_write_reg(rv8803->client, RV8803_CTRL,
				       rv8803->ctrl);
		if (err)
			return err;
	}

	return 0;
}

static int rv8803_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rv8803_data *rv8803 = dev_get_drvdata(dev);
	int ctrl, flags, err;

	ctrl = rv8803->ctrl;

	if (enabled) {
		if (rv8803->rtc->uie_rtctimer.enabled)
			ctrl |= RV8803_CTRL_UIE;
		if (rv8803->rtc->aie_timer.enabled)
			ctrl |= RV8803_CTRL_AIE;
	} else {
		if (!rv8803->rtc->uie_rtctimer.enabled)
			ctrl &= ~RV8803_CTRL_UIE;
		if (!rv8803->rtc->aie_timer.enabled)
			ctrl &= ~RV8803_CTRL_AIE;
	}

	mutex_lock(&rv8803->flags_lock);
	flags = rv8803_read_reg(client, RV8803_FLAG);
	if (flags < 0) {
		mutex_unlock(&rv8803->flags_lock);
		return flags;
	}
	flags &= ~(RV8803_FLAG_AF | RV8803_FLAG_UF);
	err = rv8803_write_reg(client, RV8803_FLAG, flags);
	mutex_unlock(&rv8803->flags_lock);
	if (err)
		return err;

	if (ctrl != rv8803->ctrl) {
		rv8803->ctrl = ctrl;
		err = rv8803_write_reg(client, RV8803_CTRL, rv8803->ctrl);
		if (err)
			return err;
	}

	return 0;
}

static int rv8803_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rv8803_data *rv8803 = dev_get_drvdata(dev);
	unsigned int vl = 0;
	int flags, ret = 0;

	switch (cmd) {
	case RTC_VL_READ:
		flags = rv8803_read_reg(client, RV8803_FLAG);
		if (flags < 0)
			return flags;

		if (flags & RV8803_FLAG_V1F) {
			dev_warn(&client->dev, "Voltage low, temperature compensation stopped.\n");
			vl = RTC_VL_ACCURACY_LOW;
		}

		if (flags & RV8803_FLAG_V2F)
			vl |= RTC_VL_DATA_INVALID;

		return put_user(vl, (unsigned int __user *)arg);

	case RTC_VL_CLR:
		mutex_lock(&rv8803->flags_lock);
		flags = rv8803_read_reg(client, RV8803_FLAG);
		if (flags < 0) {
			mutex_unlock(&rv8803->flags_lock);
			return flags;
		}

		flags &= ~RV8803_FLAG_V1F;
		ret = rv8803_write_reg(client, RV8803_FLAG, flags);
		mutex_unlock(&rv8803->flags_lock);
		if (ret)
			return ret;

		return 0;

	default:
		return -ENOIOCTLCMD;
	}
}

static int rv8803_nvram_write(void *priv, unsigned int offset, void *val,
			      size_t bytes)
{
	return rv8803_write_reg(priv, RV8803_RAM, *(u8 *)val);
}

static int rv8803_nvram_read(void *priv, unsigned int offset,
			     void *val, size_t bytes)
{
	int ret;

	ret = rv8803_read_reg(priv, RV8803_RAM);
	if (ret < 0)
		return ret;

	*(u8 *)val = ret;

	return 0;
}

static int rv8803_ts_event_write_evin(int evin, struct rv8803_data *rv8803,
				      enum evin_pull_resistor pullup_down,
				      enum evin_trigger trigger, int filter)
{
	int ret;
	u8 reg_mask;
	struct i2c_client *client = rv8803->client;

	/* according to data-sheet, "1" is not valid for filter */
	if (evin >= NO_OF_EVIN || filter == 1 || filter > EVIN_FILTER_MAX)
		return -EINVAL;

	guard(mutex)(&rv8803->flags_lock);

	/* set EVENTx pull-up edge trigger */
	ret = rv8803_read_reg(client, evin_cfg_regs[evin]);
	if (ret < 0)
		return ret;
	reg_mask = ret;
	reg_mask &= ~(RX8901_EVENTx_CFG_PUPD | RX8901_EVENTx_CFG_POL);
	reg_mask |= FIELD_PREP(RX8901_EVENTx_CFG_PUPD, pullup_down);
	reg_mask |= FIELD_PREP(RX8901_EVENTx_CFG_POL, trigger);
	ret = rv8803_write_reg(client, evin_cfg_regs[evin], reg_mask);
	if (ret < 0)
		return ret;

	/* set EVENTx noise filter */
	if (filter != -1) {
		ret = rv8803_write_reg(client, evin_flt_regs[evin], filter);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int rv8803_ts_enable_events(struct rv8803_data *rv8803, u8 enable_mask)
{
	int ret;
	u8 reg_mask;
	struct i2c_client *client = rv8803->client;

	guard(mutex)(&rv8803->flags_lock);

	/* event detection interrupt */
	ret = rv8803_read_reg(client, RV8803_CTRL);
	if (ret < 0)
		return ret;

	reg_mask = ret & ~RV8803_CTRL_EIE;
	if (enable_mask)
		reg_mask |= RV8803_CTRL_EIE;

	ret = rv8803_write_reg(client, RV8803_CTRL, reg_mask);
	if (ret)
		return ret;

	/* events for configuration */
	ret = rv8803_write_reg(client, RX8901_EVIN_EN, enable_mask);
	if (ret < 0)
		return ret;

	return 0;
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr, const char *buf,
			    size_t count)
{
	int ret;
	int i;
	unsigned long tmo;
	u8 reg;
	u8 reg_mask;
	struct rv8803_data *rv8803 = dev_get_drvdata(dev->parent);
	struct i2c_client *client = rv8803->client;

	/* EVINxCPEN | EVINxEN */;
	const u8 reg_mask_evin_en = GENMASK(5, 3) | GENMASK(2, 0);

	bool enable = (strstr(buf, "1") == buf) ? true : false;

	/* check if event detection status match requested mode */
	ret = rv8803_read_reg(client, RX8901_EVIN_EN);
	if (ret < 0)
		return ret;

	/* requested mode match current state -> nothing to do */
	if (ret == (enable ? reg_mask_evin_en : 0))
		return count;

	dev_info(&client->dev, "%s time-stamp event detection\n",
		 (enable) ? "configure" : "disable");

	/*
	 * 1. disable event detection interrupt
	 * 2. disable events for configuration
	 */
	ret = rv8803_ts_enable_events(rv8803, 0);
	if (ret < 0)
		return ret;

	/* for disable no configuration is needed */
	if (!enable)
		return count;

	/* 3. set EVENTx pull-up edge trigger and noise filter */
	for (i = 0; i < NO_OF_EVIN; ++i) {
		ret = rv8803_ts_event_write_evin(i, rv8803, pull_up_1M, falling_edge, 0);
		if (ret < 0)
			return ret;
	}

	scoped_guard(mutex, &rv8803->flags_lock) {
		/* 4. enable EVENTx interrupt */
		ret = rv8803_read_reg(client, RX8901_EVNT_INTE);
		if (ret < 0)
			return ret;
		reg_mask = BIT(5) | BIT(6) | BIT(7); /* EVINxIEN 1-3 */
		reg = (ret & ~reg_mask) | reg_mask;
		ret = rv8803_write_reg(client, RX8901_EVNT_INTE, reg);
		if (ret < 0)
			return ret;
	}

	/*
	 * 5. set BUF1 inhibit and interrupt every 1 event
	 *    NOTE: BUF2-3 are not used in FIFO-mode
	 */
	ret = rv8803_write_reg(client, RX8901_BUF1_CFG1, 0x01);
	if (ret < 0)
		return ret;

	/* 6. clean and init for BUFx and event counter 1-3 and trigger cmd */
	reg = BIT(7) | GENMASK(6, 4);
	reg |= BIT(0); /* CMDTRGEN */
	ret = rv8803_write_reg(client, RX8901_WRCMD_CFG, reg);
	if (ret < 0)
		return ret;
	ret = rv8803_write_reg(client, RX8901_WRCMD_TRG, 0xFF);
	if (ret < 0)
		return ret;
	tmo = jiffies + msecs_to_jiffies(100); /* timeout 100ms */
	do {
		usleep_range(10, 2000);
		ret = rv8803_read_reg(client, RX8901_WRCMD_TRG);
		if (ret < 0)
			return ret;
		if (time_after(jiffies, tmo))
			return -EBUSY;
	} while (ret);

	/*
	 * 7. enable event detection interrupt
	 * 8. / 10. enable events for configuration in FIFO mode
	 */
	ret = rv8803_ts_enable_events(rv8803, reg_mask_evin_en);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t read_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret;
	int ev_idx;
	int num_events;
	unsigned long long time_s;
	int time_ms;
	int offset = 0;
	u8 reg_mask;
	u8 data[10];
	struct rtc_time tm;

	struct i2c_client *client = to_i2c_client(dev->parent);
	struct rv8803_data *rv8803 = dev_get_drvdata(dev->parent);

	guard(mutex)(&rv8803->flags_lock);

	/*
	 * For detailed description see datasheet:
	 *  - Reading Time Stamp Data (FIFO mode)
	 */

	/* check interrupt source is from event 1-3 */
	ret = rv8803_read_reg(client, RX8901_EVNT_INTF);
	if (ret < 0)
		return ret;

	/* CHECK for EVF bit */
	if (ret & BIT(2)) {
		/* clear EVINxF 1-3 */
		reg_mask = BIT(5) | BIT(6) | BIT(7);
		ret = rv8803_write_reg(client, RX8901_EVNT_INTF, ret & ~reg_mask);
		if (ret < 0)
			return ret;
	}

	/* check interrupt source is from event 1-3 */
	ret = rv8803_read_reg(client, RX8901_BUF_INTF);
	if (ret < 0)
		return ret;
	if (ret & RX8901_BUF_INTF_BUF1F) {
		/* disable interrupts */
		ret = rv8803_read_reg(client, RV8803_CTRL);
		if (ret < 0)
			return ret;
		ret = rv8803_write_reg(client, RV8803_CTRL, ret & ~RV8803_CTRL_EIE);
		if (ret < 0)
			return ret;

		/* clear interrupt flag */
		ret = rv8803_read_reg(client, RX8901_BUF_INTF);
		if (ret < 0)
			return ret;
		ret = rv8803_write_reg(client, RX8901_BUF_INTF, ret & ~RX8901_BUF_INTF_BUF1F);
		if (ret < 0)
			return ret;
	}

	/* test if there are events available */
	ret = rv8803_read_reg(client, RX8901_BUF1_STAT);
	if (ret < 0)
		return ret;
	num_events = ret & RX8901_BUFx_STAT_PTR;

	if (num_events) {
		ret = rv8803_read_regs(client, 0x60, ARRAY_SIZE(data), data);
		if (ret < 0)
			return ret;

		tm.tm_sec = bcd2bin(data[2]);
		tm.tm_min = bcd2bin(data[3]);
		tm.tm_hour = bcd2bin(data[4]);
		tm.tm_mday = bcd2bin(data[5]);
		tm.tm_mon = bcd2bin(data[6]) - 1;
		tm.tm_year = bcd2bin(data[7]) + 100;
		tm.tm_wday = -1;
		tm.tm_yday = -1;
		tm.tm_isdst = -1;

		ret = rtc_valid_tm(&tm);
		if (ret)
			return ret;

		/* calculate 1/1024 -> ms */
		time_ms = (1000 * ((data[1] << 2) | (data[0] >> 6))) / 1024;
		time_s = rtc_tm_to_time64(&tm);

		offset += snprintf(buf + offset, PAGE_SIZE - offset, "%llu.%03d", time_s, time_ms);
		for (ev_idx = 0; trg_status_txt[ev_idx].txt; ++ev_idx)
			if (data[9] & trg_status_txt[ev_idx].val)
				offset += snprintf(buf + offset, PAGE_SIZE - offset, " %s=%d",
						   trg_status_txt[ev_idx].txt,
						   !!(trg_status_txt[ev_idx].val & data[8]));
		offset += snprintf(buf + offset, PAGE_SIZE - offset, "\n");

		/* according to the datasheet we have to wait for 1ms */
		usleep_range(1000, 2000);
	}

	/* re-enable interrupts */
	ret = rv8803_read_reg(client, RV8803_CTRL);
	if (ret < 0)
		return ret;
	ret = rv8803_write_reg(client, RV8803_CTRL, ret | RV8803_CTRL_EIE);
	if (ret < 0)
		return ret;

	return offset;
}

static ssize_t trigger_store(struct device *dev, struct device_attribute *attr, const char *buf,
			     size_t count)
{
	struct rv8803_data *rv8803 = dev_get_drvdata(dev->parent);
	struct i2c_client *client = rv8803->client;
	int ret;
	unsigned long tmo;

	guard(mutex)(&rv8803->flags_lock);

	/* CMDTRGEN */
	ret = rv8803_write_reg(client, RX8901_WRCMD_CFG, BIT(0));
	if (ret < 0)
		return ret;
	ret = rv8803_write_reg(client, RX8901_WRCMD_TRG, 0xFF);
	if (ret < 0)
		return ret;

	tmo = jiffies + msecs_to_jiffies(100); /* timeout 100ms */
	do {
		usleep_range(10, 2000);
		ret = rv8803_read_reg(client, RX8901_WRCMD_TRG);
		if (ret < 0)
			return ret;
		if (time_after(jiffies, tmo))
			return -EBUSY;
	} while (ret);

	return count;
}

static DEVICE_ATTR_WO(enable);
static DEVICE_ATTR_ADMIN_RO(read);
static DEVICE_ATTR_WO(trigger);

static struct attribute *rv8803_rtc_event_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_read.attr,
	&dev_attr_trigger.attr,
	NULL
};

static const struct attribute_group rv8803_rtc_sysfs_event_files = {
	.name = "tamper",
	.attrs	= rv8803_rtc_event_attrs,
};

static const struct rtc_class_ops rv8803_rtc_ops = {
	.read_time = rv8803_get_time,
	.set_time = rv8803_set_time,
	.ioctl = rv8803_ioctl,
	.read_alarm = rv8803_get_alarm,
	.set_alarm = rv8803_set_alarm,
	.alarm_irq_enable = rv8803_alarm_irq_enable,
};

static int rx8900_trickle_charger_init(struct rv8803_data *rv8803)
{
	struct i2c_client *client = rv8803->client;
	struct device_node *node = client->dev.of_node;
	int err;
	u8 flags;

	if (!node)
		return 0;

	if (rv8803->type != rx_8900)
		return 0;

	err = i2c_smbus_read_byte_data(rv8803->client, RX8900_BACKUP_CTRL);
	if (err < 0)
		return err;

	flags = (u8)err;
	flags &= ~(RX8900_FLAG_VDETOFF | RX8900_FLAG_SWOFF);
	flags |= rv8803->backup;

	return i2c_smbus_write_byte_data(rv8803->client, RX8900_BACKUP_CTRL,
					 flags);
}

/* configure registers with values different than the Power-On reset defaults */
static int rv8803_regs_configure(struct rv8803_data *rv8803)
{
	int err;

	err = rv8803_write_reg(rv8803->client, RV8803_EXT, RV8803_EXT_WADA);
	if (err)
		return err;

	err = rx8900_trickle_charger_init(rv8803);
	if (err) {
		dev_err(&rv8803->client->dev, "failed to init charger\n");
		return err;
	}

	return 0;
}

static int rv8803_resume(struct device *dev)
{
	struct rv8803_data *rv8803 = dev_get_drvdata(dev);

	if (rv8803->client->irq > 0 && device_may_wakeup(dev))
		disable_irq_wake(rv8803->client->irq);

	return 0;
}

static int rv8803_suspend(struct device *dev)
{
	struct rv8803_data *rv8803 = dev_get_drvdata(dev);

	if (rv8803->client->irq > 0 && device_may_wakeup(dev))
		enable_irq_wake(rv8803->client->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rv8803_pm_ops, rv8803_suspend, rv8803_resume);

static const struct i2c_device_id rv8803_id[] = {
	{ "rv8803", rv_8803 },
	{ "rv8804", rx_8804 },
	{ "rx8803", rx_8803 },
	{ "rx8900", rx_8900 },
	{ "rx8901", rx_8901 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rv8803_id);

static int rv8803_probe(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	struct rv8803_data *rv8803;
	int err, flags;
	struct nvmem_config nvmem_cfg = {
		.name = "rv8803_nvram",
		.word_size = 1,
		.stride = 1,
		.size = 1,
		.reg_read = rv8803_nvram_read,
		.reg_write = rv8803_nvram_write,
		.priv = client,
	};

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_I2C_BLOCK)) {
		dev_err(&adapter->dev, "doesn't support I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_I2C_BLOCK\n");
		return -EIO;
	}

	rv8803 = devm_kzalloc(&client->dev, sizeof(struct rv8803_data),
			      GFP_KERNEL);
	if (!rv8803)
		return -ENOMEM;

	mutex_init(&rv8803->flags_lock);
	rv8803->client = client;
	if (client->dev.of_node) {
		rv8803->type = (uintptr_t)of_device_get_match_data(&client->dev);
	} else {
		const struct i2c_device_id *id = i2c_match_id(rv8803_id, client);

		rv8803->type = id->driver_data;
	}
	i2c_set_clientdata(client, rv8803);

	flags = rv8803_read_reg(client, RV8803_FLAG);
	if (flags < 0)
		return flags;

	if (flags & RV8803_FLAG_V1F)
		dev_warn(&client->dev, "Voltage low, temperature compensation stopped.\n");

	if (flags & RV8803_FLAG_V2F)
		dev_warn(&client->dev, "Voltage low, data loss detected.\n");

	if (flags & RV8803_FLAG_AF)
		dev_warn(&client->dev, "An alarm maybe have been missed.\n");

	rv8803->rtc = devm_rtc_allocate_device(&client->dev);
	if (IS_ERR(rv8803->rtc))
		return PTR_ERR(rv8803->rtc);

	if (client->irq > 0) {
		unsigned long irqflags = IRQF_TRIGGER_LOW;

		if (dev_fwnode(&client->dev))
			irqflags = 0;

		err = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, rv8803_handle_irq,
						irqflags | IRQF_ONESHOT,
						"rv8803", client);
		if (err) {
			dev_warn(&client->dev, "unable to request IRQ, alarms disabled\n");
			client->irq = 0;
		} else {
			device_init_wakeup(&client->dev, true);
			err = dev_pm_set_wake_irq(&client->dev, client->irq);
			if (err)
				dev_err(&client->dev, "failed to set wake IRQ\n");
		}
	} else {
		if (device_property_read_bool(&client->dev, "wakeup-source"))
			device_init_wakeup(&client->dev, true);
		else
			clear_bit(RTC_FEATURE_ALARM, rv8803->rtc->features);
	}

	if (of_property_read_bool(client->dev.of_node, "epson,vdet-disable"))
		rv8803->backup |= RX8900_FLAG_VDETOFF;

	if (of_property_read_bool(client->dev.of_node, "trickle-diode-disable"))
		rv8803->backup |= RX8900_FLAG_SWOFF;

	err = rv8803_regs_configure(rv8803);
	if (err)
		return err;

	if (rv8803->type == rx_8901) {
		err = rtc_add_group(rv8803->rtc, &rv8803_rtc_sysfs_event_files);
		if (err)
			return err;
	}

	rv8803->rtc->ops = &rv8803_rtc_ops;
	rv8803->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rv8803->rtc->range_max = RTC_TIMESTAMP_END_2099;
	err = devm_rtc_register_device(rv8803->rtc);
	if (err)
		return err;

	devm_rtc_nvmem_register(rv8803->rtc, &nvmem_cfg);

	rv8803->rtc->max_user_freq = 1;

	return 0;
}

static const __maybe_unused struct of_device_id rv8803_of_match[] = {
	{
		.compatible = "microcrystal,rv8803",
		.data = (void *)rv_8803
	},
	{
		.compatible = "epson,rx8803",
		.data = (void *)rx_8803
	},
	{
		.compatible = "epson,rx8804",
		.data = (void *)rx_8804
	},
	{
		.compatible = "epson,rx8900",
		.data = (void *)rx_8900
	},
	{
		.compatible = "epson,rx8901",
		.data = (void *)rx_8901
	},
	{ }
};
MODULE_DEVICE_TABLE(of, rv8803_of_match);

static struct i2c_driver rv8803_driver = {
	.driver = {
		.name = "rtc-rv8803",
		.of_match_table = of_match_ptr(rv8803_of_match),
		.pm = &rv8803_pm_ops,
	},
	.probe		= rv8803_probe,
	.id_table	= rv8803_id,
};
module_i2c_driver(rv8803_driver);

MODULE_AUTHOR("Alexandre Belloni <alexandre.belloni@bootlin.com>");
MODULE_DESCRIPTION("Micro Crystal RV8803 RTC driver");
MODULE_LICENSE("GPL v2");
