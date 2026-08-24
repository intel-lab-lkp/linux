// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C driver for NXP PCF8525 RTC
 * Features:
 * - Time/date read + set
 * - Alarm (sec/min/hour/day) using RTC wkalrm API
 * - Optional IRQ support (alarm + timestamp) on INTA
 * - Timestamp1/2 exported via custom sysfs attributes: timestamp0/1
 * - Crystal load capacitance and crystal type configuration
 * - Crystal aging offset correction through the RTC offset interface
 *
 * Register map summary (key ones):
 * 0x00 Control_1: STOP, 12_24, MI, SI, etc
 * 0x01 Control_2: MSF, TI_TP, WDTF, AF, OSFE[1:0], AIE, SMBUS_TIMEOUT
 * 0x02 Control_3: PWRMNG[1:0], BF, OSIE, BIE
 * 0x03 Control_4: TSF
 * 0x04 Control_5: TSIE, TEMP_RD_EN, CL, XTL_TYP
 * 0x05 Reset: command register (CPR, CTS, SR)
 * 0x06 100th seconds
 * 0x07 seconds (OSF bit7)
 * 0x08 minutes (VLF bit7)
 * 0x09 hours
 * 0x0A days
 * 0x0B weekdays
 * 0x0C months
 * 0x0D years
 * 0x0E..0x14 alarm registers (sec/min/hour/day/weekday/month/year)
 * 0x16 timestamp control
 * 0x17..0x1C TS1 (sec/min/hour/day/month/year)
 * 0x1D..0x23 TS2 (subsec/sec/min/hour/day/month/year)
 * 0x26..0x27 aging offset registers
 * 0x28..0x2B interrupt mask regs (INTA/B)
 * 0x2E temperature (read-only), gated by TEMP_RD_EN
 */

#include <linux/bcd.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_wakeirq.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/watchdog.h>

/* Registers */
#define PCF8525_REG_CTRL1		0x00
#define PCF8525_REG_CTRL2		0x01
#define PCF8525_REG_CTRL3		0x02
#define PCF8525_REG_CTRL4		0x03
#define PCF8525_REG_CTRL5		0x04
#define PCF8525_REG_RESET		0x05

#define PCF8525_REG_100THS		0x06
#define PCF8525_REG_SECONDS		0x07
#define PCF8525_REG_MINUTES		0x08
#define PCF8525_REG_HOURS		0x09
#define PCF8525_REG_DAYS		0x0A
#define PCF8525_REG_WEEKDAYS		0x0B
#define PCF8525_REG_MONTHS		0x0C
#define PCF8525_REG_YEARS		0x0D

#define PCF8525_REG_ALM_SEC		0x0E
#define PCF8525_REG_ALM_MIN		0x0F
#define PCF8525_REG_ALM_HOUR		0x10
#define PCF8525_REG_ALM_DAY		0x11
#define PCF8525_REG_ALM_WDAY		0x12
#define PCF8525_REG_ALM_MONTH		0x13
#define PCF8525_REG_ALM_YEAR		0x14

#define PCF8525_REG_CLKOUT		0x15

#define PCF8525_REG_TS_CTL1		0x16
#define PCF8525_REG_TS1_SEC		0x17
#define PCF8525_REG_TS1_MIN		0x18
#define PCF8525_REG_TS1_HOUR		0x19
#define PCF8525_REG_TS1_DAY		0x1A
#define PCF8525_REG_TS1_MONTH		0x1B
#define PCF8525_REG_TS1_YEAR		0x1C

#define PCF8525_REG_TS2_SUBSEC		0x1D
#define PCF8525_REG_TS2_SEC		0x1E
#define PCF8525_REG_TS2_MIN		0x1F
#define PCF8525_REG_TS2_HOUR		0x20
#define PCF8525_REG_TS2_DAY		0x21
#define PCF8525_REG_TS2_MONTH		0x22
#define PCF8525_REG_TS2_YEAR		0x23

#define PCF8525_REG_INTA_MASK1		0x28
#define PCF8525_REG_INTA_MASK2		0x29
#define PCF8525_REG_INTB_MASK1		0x2A
#define PCF8525_REG_INTB_MASK2		0x2B

#define PCF8525_REG_WD_CTL              0x2C
#define PCF8525_REG_WD_VAL              0x2D
#define PCF8525_REG_TEMP		0x2E

/* CTRL1 */
#define PCF8525_CTRL1_STOP		BIT(5)
#define PCF8525_CTRL1_12_24		BIT(2)
#define PCF8525_CTRL1_MI		BIT(1)
#define PCF8525_CTRL1_SI		BIT(0)
/* CTRL2 */
#define PCF8525_CTRL2_MSF		BIT(7)
#define PCF8525_CTRL2_TI_TP		BIT(6)
#define PCF8525_CTRL2_WDTF              BIT(5)
#define PCF8525_CTRL2_AF		BIT(4)
#define PCF8525_CTRL2_OSFE_MASK		GENMASK(3, 2)
#define PCF8525_CTRL2_AIE		BIT(1)
#define PCF8525_CTRL2_SMBTO		BIT(0)
/* CTRL3 */
#define PCF8525_CTRL3_PWRMNG_MASK	GENMASK(6, 5)
#define PCF8525_CTRL3_BF		BIT(3)
#define PCF8525_CTRL3_OSIE		BIT(2)
#define PCF8525_CTRL3_BIE		BIT(1)
/* CTRL4/5 */
#define PCF8525_CTRL4_TSF		BIT(7)
#define PCF8525_CTRL5_TSIE		BIT(7)
#define PCF8525_CTRL5_TEMP_RD_EN	BIT(2)
#define PCF8525_CTRL5_CL		BIT(1)
#define PCF8525_CTRL5_XTL_TYP		BIT(0)

/* CLKOUT_ctl */
#define PCF8525_CLKOUT_CLKOE            BIT(3)

/* Timestamp control */
#define PCF8525_TS_CTL1_TSM             BIT(7)
#define PCF8525_TS_CTL1_TSOFF           BIT(6)

/* Seconds/Minutes flags */
#define PCF8525_SC_OSF			BIT(7)
#define PCF8525_MN_VLF			BIT(7)

/* Alarm AE bits */
#define PCF8525_ALM_AE			BIT(7)

/* RESET commands */
#define PCF8525_RESET_CPR_CMD		0xA4 /* clear prescaler */
#define PCF8525_RESET_CTS_CMD		0x25 /* clear timestamp */
#define PCF8525_RESET_CPR_CTS_CMD	0xA5 /* clear prescaler + timestamp */
#define PCF8525_RESET_SR_CMD		0x2C /* software reset */

/* Interrupt masks (mask bit = 1 means masked/disabled) - INTA_MASK1 bits */
#define PCF8525_MASK1_WD_CD            BIT(0)
#define PCF8525_MASK1_BIE		BIT(1)
#define PCF8525_MASK1_AIE		BIT(2)
#define PCF8525_MASK1_OSIE		BIT(3)
#define PCF8525_MASK1_SI		BIT(4)
#define PCF8525_MASK1_MI		BIT(5)
/* INTA_MASK2 - timestamp mask bit3 */
#define PCF8525_MASK2_TSIE		BIT(3)

#define PCF8525_ALM_AE_MONTH		BIT(7)
#define PCF8525_ALM_AE_YEAR		BIT(6)
#define PCF8525_WD_CTL_WD_CD            BIT(7)
#define PCF8525_WD_CTL_TF0              BIT(0)
#define PCF8525_WD_CTL_TF1              BIT(1)

#define PCF8525_WD_CLOCK_HZ_X1000       250 /* 1/4 Hz */
#define PCF8525_WD_MIN_HW_HEARTBEAT_MS  4000
#define PCF8525_WD_VAL_STOP             0
#define PCF8525_WD_DEFAULT_TIMEOUT_S    60
#define PCF8525_CLKOUT_TCR_MASK		GENMASK(7, 5)

#define PCF8525_REG_AGING_OFFSET_HI	0x26
#define PCF8525_REG_AGING_OFFSET_LO	0x27
/*
 * One aging-offset register step is 0.0298 ppm, or 29.8 ppb.
 * The RTC core offset interface uses ppb.
 */
#define PCF8525_AGING_OFFSET_STEP_NUM	298L
#define PCF8525_AGING_OFFSET_STEP_DEN	10L

#define PCF8525_AGING_OFFSET_MIN	(-32768L)
#define PCF8525_AGING_OFFSET_MAX	32767L

struct pcf8525 {
	struct rtc_device *rtc;
	struct watchdog_device wdd;
	struct regmap *regmap;
	spinlock_t ts_lock;	/* protects ts[] and ts_valid[] */
	bool irq_enabled;
	time64_t ts[2];
	bool ts_valid[2];
	int irq_inta;
	int irq_intb;
};

static const struct regmap_config pcf8525_regmap_cfg = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = PCF8525_REG_TEMP,
};

static unsigned int pcf8525_wdt_timeout_to_val(unsigned int timeout)
{
	unsigned int val;

	val = DIV_ROUND_UP(timeout * PCF8525_WD_CLOCK_HZ_X1000, 1000) + 1;

	if (val < 2)
		return 2;
	if (val > 255)
		return 255;

	return val;
}

static int pcf8525_wdt_ping(struct watchdog_device *wdd)
{
	struct pcf8525 *pcf8525 = watchdog_get_drvdata(wdd);
	unsigned int wd_val;

	wd_val = pcf8525_wdt_timeout_to_val(wdd->timeout);

	return regmap_write(pcf8525->regmap, PCF8525_REG_WD_VAL, wd_val);
}

static int pcf8525_wdt_active_ping(struct watchdog_device *wdd)
{
	if (watchdog_active(wdd))
		return pcf8525_wdt_ping(wdd);

	return 0;
}

static int pcf8525_wdt_start(struct watchdog_device *wdd)
{
	return pcf8525_wdt_ping(wdd);
}

static int pcf8525_wdt_stop(struct watchdog_device *wdd)
{
	struct pcf8525 *pcf8525 = watchdog_get_drvdata(wdd);

	return regmap_write(pcf8525->regmap, PCF8525_REG_WD_VAL,
				    PCF8525_WD_VAL_STOP);
}

static int pcf8525_wdt_set_timeout(struct watchdog_device *wdd,
				   unsigned int timeout)
{
	wdd->timeout = timeout;

	return pcf8525_wdt_active_ping(wdd);
}

static const struct watchdog_info pcf8525_wdt_info = {
	.identity = "NXP PCF8525 Watchdog",
	.options = WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
};

static const struct watchdog_ops pcf8525_watchdog_ops = {
	.owner = THIS_MODULE,
	.start = pcf8525_wdt_start,
	.stop = pcf8525_wdt_stop,
	.ping = pcf8525_wdt_ping,
	.set_timeout = pcf8525_wdt_set_timeout,
};

static int pcf8525_watchdog_get_period(int n, int f1000)
{
	return (1000 * (n - 1)) / f1000;
}

static irqreturn_t pcf8525_wdt_irq(int irq, void *data)
{
	struct device *dev = data;
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int ctrl2;
	int ret;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_CTRL2, &ctrl2);
	if (ret)
		return IRQ_NONE;

	if (!(ctrl2 & PCF8525_CTRL2_WDTF))
		return IRQ_NONE;

	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL2,
				 PCF8525_CTRL2_WDTF, 0);
	if (ret)
		return IRQ_NONE;

	watchdog_notify_pretimeout(&pcf8525->wdd);
	return IRQ_HANDLED;
}

static int pcf8525_watchdog_config(struct device *dev,
				   struct pcf8525 *pcf8525)
{
	unsigned int m1, m2;
	int ret;

	/* Configure nINTB/CLKOUT as nINTB open-drain interrupt output. */
	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CLKOUT,
				 PCF8525_CLKOUT_CLKOE, 0);
	if (ret)
		return ret;

	/* Enable watchdog interrupt and select 1/4 Hz source: TF[1:0] = 10. */
	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_WD_CTL,
				 PCF8525_WD_CTL_WD_CD |
				 PCF8525_WD_CTL_TF1 |
				 PCF8525_WD_CTL_TF0,
				 PCF8525_WD_CTL_WD_CD |
				 PCF8525_WD_CTL_TF1);
	if (ret)
		return ret;

	/* Keep watchdog masked on INTA. */
	ret = regmap_read(pcf8525->regmap, PCF8525_REG_INTA_MASK1, &m1);
	if (ret)
		return ret;

	m1 |= PCF8525_MASK1_WD_CD;
	ret = regmap_write(pcf8525->regmap, PCF8525_REG_INTA_MASK1, m1);
	if (ret)
		return ret;

	/* Route watchdog only to INTB, and keep RTC interrupts masked on INTB. */
	ret = regmap_read(pcf8525->regmap, PCF8525_REG_INTB_MASK1, &m1);
	if (ret)
		return ret;

	/*
	 * Clear any stale WDTF before unmasking the watchdog on INTB.
	 * With battery backup the flag survives a power cycle and would
	 * assert INTB immediately on the next boot, causing an infinite
	 * reset loop if INTB is wired to a hardware reset line.
	 */
	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL2,
				 PCF8525_CTRL2_WDTF, 0);
	if (ret)
		return ret;

	m1 |= PCF8525_MASK1_BIE |
	      PCF8525_MASK1_AIE |
	      PCF8525_MASK1_OSIE |
	      PCF8525_MASK1_SI |
	      PCF8525_MASK1_MI;
	m1 &= ~PCF8525_MASK1_WD_CD;

	ret = regmap_write(pcf8525->regmap, PCF8525_REG_INTB_MASK1, m1);
	if (ret)
		return ret;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_INTB_MASK2, &m2);
	if (ret)
		return ret;

	m2 |= PCF8525_MASK2_TSIE;

	return regmap_write(pcf8525->regmap, PCF8525_REG_INTB_MASK2, m2);
}

static int pcf8525_watchdog_init(struct device *dev,
				 struct pcf8525 *pcf8525)
{
	int ret;

	if (!IS_ENABLED(CONFIG_WATCHDOG_CORE) ||
	    !device_property_read_bool(dev, "reset-source"))
		return 0;

	if (pcf8525->irq_intb > 0) {
		ret = devm_request_threaded_irq(dev, pcf8525->irq_intb,
						NULL, pcf8525_wdt_irq,
						IRQF_ONESHOT,
						"pcf8525-wdt", dev);
		if (ret)
			return ret;
	}

	ret = pcf8525_watchdog_config(dev, pcf8525);
	if (ret)
		return ret;

	pcf8525->wdd.parent = dev;
	pcf8525->wdd.info = &pcf8525_wdt_info;
	pcf8525->wdd.ops = &pcf8525_watchdog_ops;
	pcf8525->wdd.min_timeout = pcf8525_watchdog_get_period(2,
							       PCF8525_WD_CLOCK_HZ_X1000);
	pcf8525->wdd.max_timeout = pcf8525_watchdog_get_period(255,
							       PCF8525_WD_CLOCK_HZ_X1000);
	pcf8525->wdd.timeout = PCF8525_WD_DEFAULT_TIMEOUT_S;
	watchdog_init_timeout(&pcf8525->wdd, 0, dev);
	pcf8525->wdd.min_hw_heartbeat_ms = PCF8525_WD_MIN_HW_HEARTBEAT_MS;
	pcf8525->wdd.status = WATCHDOG_NOWAYOUT_INIT_STATUS;

	watchdog_set_drvdata(&pcf8525->wdd, pcf8525);
	watchdog_stop_on_reboot(&pcf8525->wdd);

	/*
	 * If the watchdog countdown register is non-zero the bootloader
	 * left the watchdog running.  Tell the watchdog core so that it
	 * keeps pinging the hardware until userspace takes over.
	 */
	{
		unsigned int wd_val;

		if (!regmap_read(pcf8525->regmap, PCF8525_REG_WD_VAL, &wd_val) &&
		    wd_val != 0)
			set_bit(WDOG_HW_RUNNING, &pcf8525->wdd.status);
	}

	return devm_watchdog_register_device(dev, &pcf8525->wdd);
}

static int pcf8525_get_irqs(struct i2c_client *client, struct pcf8525 *pcf8525)
{
	struct device *dev = &client->dev;
	int irq;

	irq = fwnode_irq_get_byname(dev_fwnode(dev), "inta");
	if (irq == -ENOENT || irq == -EINVAL)
		irq = client->irq;
	else if (irq < 0)
		return irq;
	pcf8525->irq_inta = irq;

	irq = fwnode_irq_get_byname(dev_fwnode(dev), "intb");
	if (irq == -ENOENT || irq == -EINVAL)
		irq = 0;
	else if (irq < 0)
		return irq;
	pcf8525->irq_intb = irq;

	return 0;
}

/*
 * Configure only the crystal fields explicitly provided by firmware.
 * If a property is absent, preserve the current hardware setting.
 *
 * quartz-load-femtofarads:
 *   6000 -> Control_5.CL = 0
 *   7000 -> Control_5.CL = 1
 *
 * nxp,xtal-type:
 *   1 -> Control_5.XTL_TYP = 0 (-0.035 ppm/degree C^2 model)
 *   2 -> Control_5.XTL_TYP = 1 (-0.04 ppm/degree C^2 model)
 */
static int pcf8525_configure_crystal(struct device *dev,
				     struct pcf8525 *pcf8525)
{
	unsigned int mask = 0;
	unsigned int value = 0;
	u32 xtal_type;
	u32 load;
	int ret;

	ret = device_property_read_u32(dev,
				       "quartz-load-femtofarads",
				       &load);
	if (!ret) {
		switch (load) {
		case 6000:
			mask |= PCF8525_CTRL5_CL;
			break;

		case 7000:
			mask |= PCF8525_CTRL5_CL;
			value |= PCF8525_CTRL5_CL;
			break;

		default:
			dev_warn(dev,
				 "unsupported quartz-load-femtofarads=%u; "
				 "preserving CL bit\n",
				 load);
			break;
		}
	}

	ret = device_property_read_u32(dev,
				       "nxp,xtal-type",
				       &xtal_type);
	if (!ret) {
		switch (xtal_type) {
		case 1:
			mask |= PCF8525_CTRL5_XTL_TYP;
			break;

		case 2:
			mask |= PCF8525_CTRL5_XTL_TYP;
			value |= PCF8525_CTRL5_XTL_TYP;
			break;

		default:
			dev_warn(dev,
				 "unsupported nxp,xtal-type=%u; "
				 "preserving XTL_TYP bit\n",
				 xtal_type);
			break;
		}
	}

	if (!mask)
		return 0;

	return regmap_update_bits(pcf8525->regmap,
				  PCF8525_REG_CTRL5,
				  mask, value);
}

static int pcf8525_read_aging_offset(struct pcf8525 *pcf8525, s16 *offset)
{
	u8 buf[2];
	int ret;

	ret = regmap_bulk_read(pcf8525->regmap,
			       PCF8525_REG_AGING_OFFSET_HI,
			       buf, sizeof(buf));
	if (ret)
		return ret;

	*offset = (s16)((buf[0] << 8) | buf[1]);

	return 0;
}

static int pcf8525_write_aging_offset(struct pcf8525 *pcf8525, s16 offset)
{
	u8 buf[2];

	buf[0] = (offset >> 8) & 0xff;
	buf[1] = offset & 0xff;

	/*
	 * Write high byte first and low byte last. The new aging correction
	 * starts after AgingOffset_Low is written.
	 */
	return regmap_bulk_write(pcf8525->regmap,
				 PCF8525_REG_AGING_OFFSET_HI,
				 buf, sizeof(buf));
}

static int pcf8525_rtc_read_offset(struct device *dev, long *offset)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	s16 raw;
	int ret;

	ret = pcf8525_read_aging_offset(pcf8525, &raw);
	if (ret)
		return ret;

	/*
	 * Convert the signed 16-bit hardware value to ppb:
	 *
	 * one raw step = 0.0298 ppm = 29.8 ppb
	 */
	*offset = DIV_ROUND_CLOSEST((long)raw *
				    PCF8525_AGING_OFFSET_STEP_NUM,
				    PCF8525_AGING_OFFSET_STEP_DEN);

	return 0;
}

static int pcf8525_rtc_set_offset(struct device *dev, long offset)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	long min_offset;
	long max_offset;
	long raw;

	min_offset =
		DIV_ROUND_CLOSEST(PCF8525_AGING_OFFSET_MIN *
				  PCF8525_AGING_OFFSET_STEP_NUM,
				  PCF8525_AGING_OFFSET_STEP_DEN);

	max_offset =
		DIV_ROUND_CLOSEST(PCF8525_AGING_OFFSET_MAX *
				  PCF8525_AGING_OFFSET_STEP_NUM,
				  PCF8525_AGING_OFFSET_STEP_DEN);

	if (offset < min_offset || offset > max_offset)
		return -ERANGE;

	/*
	 * Convert the requested ppb value to the closest signed
	 * 16-bit hardware value.
	 */
	raw = DIV_ROUND_CLOSEST(offset *
				PCF8525_AGING_OFFSET_STEP_DEN,
				PCF8525_AGING_OFFSET_STEP_NUM);

	if (raw < PCF8525_AGING_OFFSET_MIN ||
	    raw > PCF8525_AGING_OFFSET_MAX)
		return -ERANGE;

	return pcf8525_write_aging_offset(pcf8525, (s16)raw);
}

static int pcf8525_hwmon_read_temp(struct device *dev, long *temp)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int regval;
	int ret;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_TEMP, &regval);
	if (ret)
		return ret;

	/*
	 * PCF8525: signed 8-bit, 1 degree C per LSB.
	 * HWMON requires millidegree Celsius.
	 */
	*temp = (long)(s8)(u8)regval * 1000L;

	return 0;
}

static int pcf8525_hwmon_read_update_interval(struct device *dev, long *val)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int regval;
	unsigned int tcr;
	int ret;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_CLKOUT, &regval);
	if (ret)
		return ret;

	tcr = FIELD_GET(PCF8525_CLKOUT_TCR_MASK, regval);

	switch (tcr) {
	case 0:
		*val = 32 * 60 * 1000L;
		break;
	case 1:
		*val = 16 * 60 * 1000L;
		break;
	case 2:
		*val = 8 * 60 * 1000L;
		break;
	case 3:
		*val = 4 * 60 * 1000L;
		break;
	case 4:
		*val = 2 * 60 * 1000L;
		break;
	default:
		*val = 60 * 1000L;
		break;
	}

	return 0;
}

static int pcf8525_hwmon_write_update_interval(struct device *dev, long val)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int tcr;

	switch (val) {
	case 32 * 60 * 1000L:
		tcr = 0;
		break;
	case 16 * 60 * 1000L:
		tcr = 1;
		break;
	case 8 * 60 * 1000L:
		tcr = 2;
		break;
	case 4 * 60 * 1000L:
		tcr = 3;
		break;
	case 2 * 60 * 1000L:
		tcr = 4;
		break;
	case 60 * 1000L:
		tcr = 5;
		break;
	default:
		return -EINVAL;
	}

	/* Update only TCR[2:0]; preserve OTPR, CLKOE and COF[2:0]. */
	return regmap_update_bits(pcf8525->regmap, PCF8525_REG_CLKOUT,
				  PCF8525_CLKOUT_TCR_MASK,
				  FIELD_PREP(PCF8525_CLKOUT_TCR_MASK, tcr));
}

static umode_t pcf8525_hwmon_is_visible(const void *data,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	switch (type) {
	case hwmon_chip:
		if (attr == hwmon_chip_update_interval)
			return 0644;
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input && channel == 0)
			return 0444;
		break;
	default:
		break;
	}

	return 0;
}

static int pcf8525_hwmon_read(struct device *dev,
			      enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_chip:
		if (attr == hwmon_chip_update_interval)
			return pcf8525_hwmon_read_update_interval(dev, val);
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input && channel == 0)
			return pcf8525_hwmon_read_temp(dev, val);
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int pcf8525_hwmon_write(struct device *dev,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel, long val)
{
	if (type == hwmon_chip && attr == hwmon_chip_update_interval)
		return pcf8525_hwmon_write_update_interval(dev, val);

	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info * const pcf8525_hwmon_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops pcf8525_hwmon_ops = {
	.is_visible = pcf8525_hwmon_is_visible,
	.read = pcf8525_hwmon_read,
	.write = pcf8525_hwmon_write,
};

static const struct hwmon_chip_info pcf8525_hwmon_chip_info = {
	.ops = &pcf8525_hwmon_ops,
	.info = pcf8525_hwmon_info,
};

/*
 * Keep HWMON optional and non-fatal so RTC and watchdog registration remain
 * usable even if the temperature interface cannot be registered.
 */
static void pcf8525_hwmon_register(struct device *dev,
				   struct pcf8525 *pcf8525)
{
	struct device *hwmon_dev;
	int ret;

	if (!IS_ENABLED(CONFIG_RTC_DRV_PCF8525_HWMON))
		return;

	/*
	 * Register the hwmon device first.  Only enable the hardware
	 * temperature readout afterwards so that the device is not left
	 * enabled if registration fails.
	 */
	hwmon_dev = devm_hwmon_device_register_with_info(dev, "pcf8525",
							 pcf8525,
							 &pcf8525_hwmon_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev)) {
		dev_warn(dev, "failed to register HWMON device: %ld\n",
			 PTR_ERR(hwmon_dev));
		return;
	}

	/* Enable digital readout; preserve TSIE, CL and XTL_TYP. */
	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL5,
				 PCF8525_CTRL5_TEMP_RD_EN,
				 PCF8525_CTRL5_TEMP_RD_EN);
	if (ret)
		dev_warn(dev, "failed to enable temperature readout: %d\n", ret);
}

static int pcf8525_read_time(struct device *dev, struct rtc_time *tm)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	u8 buf[7];
	int ret;

	/* Read seconds..years (0x07..0x0D) */
	ret = regmap_bulk_read(pcf8525->regmap, PCF8525_REG_SECONDS, buf, sizeof(buf));
	if (ret)
		return ret;

	/* OSF: oscillator stop => time not reliable */
	if (buf[0] & PCF8525_SC_OSF)
		return -EINVAL;

	/*
	 * VLF is on minutes register bit7;
	 * Keep policy conservative: warn but still allow read.
	 */
	if (buf[1] & PCF8525_MN_VLF)
		dev_warn(dev, "VLF set: clock integrity not guaranteed\n");

	tm->tm_sec =  bcd2bin(buf[0] & 0x7f);
	tm->tm_min =  bcd2bin(buf[1] & 0x7f);
	tm->tm_hour = bcd2bin(buf[2] & 0x3f);
	tm->tm_mday = bcd2bin(buf[3] & 0x3f);
	tm->tm_wday = buf[4] & 0x07;
	tm->tm_mon =  bcd2bin(buf[5] & 0x1f) - 1;
	tm->tm_year = bcd2bin(buf[6]) + 100; /* 20xx */

	return rtc_valid_tm(tm);
}

static int pcf8525_set_time(struct device *dev, struct rtc_time *tm)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	u8 buf[7];
	int ret, ret2;

	ret = rtc_valid_tm(tm);
	if (ret)
		return ret;

	/*
	 * Use STOP + clear prescaler (CPR) like RESET register describes.
	 */
	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL1,
				 PCF8525_CTRL1_STOP, PCF8525_CTRL1_STOP);
	if (ret)
		return ret;

	ret = regmap_write(pcf8525->regmap, PCF8525_REG_RESET, PCF8525_RESET_CPR_CMD);
	if (ret)
		goto out_start;

	buf[0] = bin2bcd(tm->tm_sec); /* writing seconds also clears OSF per many NXP RTCs */
	buf[1] = bin2bcd(tm->tm_min);
	buf[2] = bin2bcd(tm->tm_hour);
	buf[3] = bin2bcd(tm->tm_mday);
	buf[4] = tm->tm_wday & 0x07;
	buf[5] = bin2bcd(tm->tm_mon + 1);
	buf[6] = bin2bcd(tm->tm_year - 100);

	ret = regmap_bulk_write(pcf8525->regmap, PCF8525_REG_SECONDS, buf, sizeof(buf));

out_start:
	/* Clear STOP regardless of bulk write outcome */
	ret2 = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL1, PCF8525_CTRL1_STOP, 0);
	if (ret2)
		return ret2;
	return ret;
}

static int pcf8525_rtc_ioctl(struct device *dev,
			     unsigned int cmd, unsigned long arg)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int val;
	unsigned int flags = 0;
	int ret;

	switch (cmd) {
	case RTC_VL_READ:
		ret = regmap_read(pcf8525->regmap, PCF8525_REG_CTRL3, &val);
		if (ret)
			return ret;

		if (val & PCF8525_CTRL3_BF)
			flags |= RTC_VL_BACKUP_SWITCH;

		return put_user(flags, (unsigned int __user *)arg);

	case RTC_VL_CLR:
		/* Clear BF by writing 0 (AND semantics respected) */
		return regmap_update_bits(pcf8525->regmap,
					  PCF8525_REG_CTRL3,
					  PCF8525_CTRL3_BF,
					  0);

	default:
		return -ENOIOCTLCMD;
	}
}

static int pcf8525_param_get(struct device *dev, struct rtc_param *param)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int val;
	unsigned int mode;
	int ret;

	switch (param->param) {
	case RTC_PARAM_BACKUP_SWITCH_MODE:
		ret = regmap_read(pcf8525->regmap, PCF8525_REG_CTRL3, &val);
		if (ret)
			return ret;

		mode = FIELD_GET(PCF8525_CTRL3_PWRMNG_MASK, val);

		switch (mode) {
		case 0:
			param->uvalue = RTC_BSM_LEVEL;
			break;
		case 1:
			param->uvalue = RTC_BSM_DIRECT;
			break;
		default:
			param->uvalue = RTC_BSM_DISABLED;
			break;
		}
		return 0;

	default:
		return -EINVAL;
	}
}

static int pcf8525_param_set(struct device *dev, struct rtc_param *param)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int mode;

	switch (param->param) {
	case RTC_PARAM_BACKUP_SWITCH_MODE:
		switch (param->uvalue) {
		case RTC_BSM_LEVEL:
			mode = 0;
			break;
		case RTC_BSM_DIRECT:
			mode = 1;
			break;
		case RTC_BSM_DISABLED:
			mode = 2;
			break;
		default:
			return -EINVAL;
		}

	return regmap_update_bits(pcf8525->regmap,
				  PCF8525_REG_CTRL3,
				  PCF8525_CTRL3_PWRMNG_MASK,
				  FIELD_PREP(PCF8525_CTRL3_PWRMNG_MASK, mode));
	default:
		return -EINVAL;
	}
}

/* Alarm uses RTC wkalrm: map to sec/min/hour/day and disable others */
static int pcf8525_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int ctrl2;
	u8 buf[7];
	int ret;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_CTRL2, &ctrl2);
	if (ret)
		return ret;

	ret = regmap_bulk_read(pcf8525->regmap, PCF8525_REG_ALM_SEC, buf, sizeof(buf));
	if (ret)
		return ret;

	alrm->enabled = !!(ctrl2 & PCF8525_CTRL2_AIE);
	alrm->pending = !!(ctrl2 & PCF8525_CTRL2_AF);

	alrm->time.tm_sec = bcd2bin(buf[0] & 0x7f);
	alrm->time.tm_min = bcd2bin(buf[1] & 0x7f);
	alrm->time.tm_hour = bcd2bin(buf[2] & 0x3f);
	alrm->time.tm_mday = bcd2bin(buf[3] & 0x3f);

	return 0;
}

static int pcf8525_alarm_irq_enable(struct device *dev, unsigned int enable)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	int ret;

	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL2,
				 PCF8525_CTRL2_AIE,
				 enable ? PCF8525_CTRL2_AIE : 0);
	if (ret)
		dev_err(dev, "alarm_irq_enable: failed ret=%d\n", ret);

	return ret;
}

static int pcf8525_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	u8 buf[7];
	int ret;

	/* Clear AF first */
	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL2,
				 PCF8525_CTRL2_AF, 0);
	if (ret)
		return ret;

	buf[0] = bin2bcd(alrm->time.tm_sec) & 0x7f;
	buf[1] = bin2bcd(alrm->time.tm_min) & 0x7f;
	buf[2] = bin2bcd(alrm->time.tm_hour) & 0x3f;
	buf[3] = bin2bcd(alrm->time.tm_mday) & 0x3f;

	/* Disable match on weekday/month/year by setting AE bits */
	buf[4] = PCF8525_ALM_AE; /* weekday alarm ignored */
	buf[5] = PCF8525_ALM_AE_MONTH | PCF8525_ALM_AE_YEAR;  /* disable month + year alarm */
	buf[6] = 0x00; /* year alarm value (ignored if month/year disabled) */

	ret = regmap_bulk_write(pcf8525->regmap, PCF8525_REG_ALM_SEC, buf, sizeof(buf));
	if (ret)
		return ret;

	return pcf8525_alarm_irq_enable(dev, alrm->enabled);
}

/* ---- Timestamp handling (sysfs) ---- */
static int pcf8525_ts_read(struct device *dev, int id, time64_t *ts_out)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	struct rtc_time tm;
	u8 buf[6];
	int ret;
	u8 base;

	/*
	 * TS0 maps to TS1 (0x17..0x1C)
	 * TS1 maps to TS2 (0x1E..0x23) (skip subsec at 0x1D)
	 */
	if (id == 0)
		base = PCF8525_REG_TS1_SEC;
	else if (id == 1)
		base = PCF8525_REG_TS2_SEC;
	else
		return -EINVAL;

	ret = regmap_bulk_read(pcf8525->regmap, base, buf, sizeof(buf));
	if (ret)
		return ret;

	tm.tm_sec = bcd2bin(buf[0] & 0x7f);
	tm.tm_min = bcd2bin(buf[1] & 0x7f);
	tm.tm_hour = bcd2bin(buf[2] & 0x3f);
	tm.tm_mday = bcd2bin(buf[3] & 0x3f);
	tm.tm_mon = bcd2bin(buf[4] & 0x1f) - 1;
	tm.tm_year = bcd2bin(buf[5]) + 100;

	ret = rtc_valid_tm(&tm);
	if (ret)
		return ret;

	*ts_out = rtc_tm_to_time64(&tm);
	return 0;
}

static ssize_t timestamp_show_common(struct device *dev, char *buf, int id)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev->parent);
	unsigned int ctrl4;
	time64_t ts;
	int ret;

	if (id < 0 || id > 1)
		return 0;

	if (pcf8525->irq_enabled) {
		unsigned long flags;
		bool valid;

		spin_lock_irqsave(&pcf8525->ts_lock, flags);
		valid = pcf8525->ts_valid[id];
		if (valid)
			ts = pcf8525->ts[id];
		spin_unlock_irqrestore(&pcf8525->ts_lock, flags);

		if (!valid)
			return 0;
		return sysfs_emit(buf, "%llu\n", (unsigned long long)ts);
	}

	/* polling mode: only report if TSF is currently set */
	ret = regmap_read(pcf8525->regmap, PCF8525_REG_CTRL4, &ctrl4);
	if (ret)
		return 0;

	if (!(ctrl4 & PCF8525_CTRL4_TSF))
		return 0;

	ret = pcf8525_ts_read(dev->parent, id, &ts);
	if (ret)
		return 0;

	return sysfs_emit(buf, "%llu\n", (unsigned long long)ts);
}

static ssize_t timestamp_store_common(struct device *dev, const char *buf,
				      size_t count, int id)
{
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev->parent);
	int ret;

	if (id < 0 || id > 1)
		return -EINVAL;

	if (pcf8525->irq_enabled) {
		unsigned long flags;

		spin_lock_irqsave(&pcf8525->ts_lock, flags);
		pcf8525->ts_valid[id] = false;
		spin_unlock_irqrestore(&pcf8525->ts_lock, flags);

		/* Also clear the hardware timestamp registers. */
		regmap_write(pcf8525->regmap, PCF8525_REG_RESET,
			     PCF8525_RESET_CTS_CMD);
		return count;
	}

	/*
	 * Clear timestamp via RESET CTS command.
	 * This also clears the timestamp flag path.
	 */
	ret = regmap_write(pcf8525->regmap, PCF8525_REG_RESET, PCF8525_RESET_CTS_CMD);
	if (ret)
		return ret;

	/* Also clear TSF flag (belt-and-suspenders) */
	regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL4, PCF8525_CTRL4_TSF, 0);

	return count;
}

static ssize_t timestamp0_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return timestamp_show_common(dev, buf, 0);
}

static ssize_t timestamp1_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return timestamp_show_common(dev, buf, 1);
}

static ssize_t timestamp0_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	return timestamp_store_common(dev, buf, count, 0);
}

static ssize_t timestamp1_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	return timestamp_store_common(dev, buf, count, 1);
}

static DEVICE_ATTR_RW(timestamp0);
static DEVICE_ATTR_RW(timestamp1);

static struct attribute *pcf8525_attrs[] = {
	&dev_attr_timestamp0.attr,
	&dev_attr_timestamp1.attr,
	NULL
};

static const struct attribute_group pcf8525_attr_group = {
	.attrs = pcf8525_attrs,
};

static irqreturn_t pcf8525_irq(int irq, void *data)
{
	struct device *dev = data;
	struct pcf8525 *pcf8525 = dev_get_drvdata(dev);
	unsigned int ctrl2, ctrl4;
	int ret;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_CTRL2, &ctrl2);
	if (ret)
		return IRQ_NONE;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_CTRL4, &ctrl4);
	if (ret)
		return IRQ_NONE;

	if (!(ctrl2 & (PCF8525_CTRL2_AF | PCF8525_CTRL2_MSF)) &&
	    !(ctrl4 & PCF8525_CTRL4_TSF))
		return IRQ_NONE;

	/* Timestamp */
	if (ctrl4 & PCF8525_CTRL4_TSF) {
		time64_t ts0, ts1;
		bool ok0, ok1;
		unsigned long flags;

		/*
		 * W0C: clear TSF before reading the timestamp registers so
		 * that a new event that fires during the I2C read is not lost.
		 */
		regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL4,
				   PCF8525_CTRL4_TSF, 0);

		/* Read hardware outside the spinlock (I2C operations may sleep). */
		ok0 = !pcf8525_ts_read(dev, 0, &ts0);
		ok1 = !pcf8525_ts_read(dev, 1, &ts1);

		spin_lock_irqsave(&pcf8525->ts_lock, flags);
		if (ok0 && !pcf8525->ts_valid[0]) {
			pcf8525->ts[0] = ts0;
			pcf8525->ts_valid[0] = true;
		}
		if (ok1 && !pcf8525->ts_valid[1]) {
			pcf8525->ts[1] = ts1;
			pcf8525->ts_valid[1] = true;
		}
		spin_unlock_irqrestore(&pcf8525->ts_lock, flags);
	}

	/* Alarm */
	if (ctrl2 & PCF8525_CTRL2_AF) {
		/*
		 * W0C: clear AF before notifying so that a new alarm event
		 * that fires during rtc_update_irq() is not lost.
		 */
		regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL2,
				   PCF8525_CTRL2_AF, 0);
		rtc_update_irq(pcf8525->rtc, 1, RTC_IRQF | RTC_AF);
	}

	if (ctrl2 & PCF8525_CTRL2_MSF) {
		ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL2, PCF8525_CTRL2_MSF, 0);
		if (ret)
			return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static const struct rtc_class_ops pcf8525_rtc_ops = {
	.ioctl = pcf8525_rtc_ioctl,
	.read_time = pcf8525_read_time,
	.set_time = pcf8525_set_time,
	.read_alarm = pcf8525_read_alarm,
	.set_alarm = pcf8525_set_alarm,
	.alarm_irq_enable = pcf8525_alarm_irq_enable,
	.read_offset = pcf8525_rtc_read_offset,
	.set_offset = pcf8525_rtc_set_offset,
	.param_get = pcf8525_param_get,
	.param_set = pcf8525_param_set,
};

static int pcf8525_unmask_irqs_intA(struct pcf8525 *pcf8525)
{
	/*
	 * Mask registers: bit=1 means masked/disabled.
	 * We want alarm + timestamp unmasked on INTA by default:
	 * - AIE => mask bit2 must be 0
	 * - TSIE => mask2 bit3 must be 0
	 */
	unsigned int m1, m2;
	int ret;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_INTA_MASK1, &m1);
	if (ret)
		return ret;

	ret = regmap_read(pcf8525->regmap, PCF8525_REG_INTA_MASK2, &m2);
	if (ret)
		return ret;

	m1 |= PCF8525_MASK1_WD_CD;
	m1 &= ~PCF8525_MASK1_AIE;    /* unmask alarm interrupt on INTA */
	m2 &= ~PCF8525_MASK2_TSIE;   /* unmask timestamp interrupt on INTA */

	ret = regmap_write(pcf8525->regmap, PCF8525_REG_INTA_MASK1, m1);
	if (ret)
		return ret;

	return regmap_write(pcf8525->regmap, PCF8525_REG_INTA_MASK2, m2);
}

static int pcf8525_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct pcf8525 *pcf8525;
	int ret;

	pcf8525 = devm_kzalloc(dev, sizeof(*pcf8525), GFP_KERNEL);
	if (!pcf8525)
		return -ENOMEM;

	spin_lock_init(&pcf8525->ts_lock);

	pcf8525->regmap = devm_regmap_init_i2c(client, &pcf8525_regmap_cfg);
	if (IS_ERR(pcf8525->regmap))
		return dev_err_probe(dev, PTR_ERR(pcf8525->regmap),
				     "failed to init regmap\n");

	i2c_set_clientdata(client, pcf8525);

	ret = pcf8525_get_irqs(client, pcf8525);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get IRQs\n");

	pcf8525->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(pcf8525->rtc))
		return dev_err_probe(dev, PTR_ERR(pcf8525->rtc),
				     "failed to allocate RTC device\n");

	pcf8525->rtc->ops = &pcf8525_rtc_ops;
	pcf8525->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	pcf8525->rtc->range_max = RTC_TIMESTAMP_END_2099;
	pcf8525->rtc->set_start_time = true;

	clear_bit(RTC_FEATURE_ALARM, pcf8525->rtc->features);
	set_bit(RTC_FEATURE_UPDATE_INTERRUPT, pcf8525->rtc->features);
	/* Use 24-hour mode */
	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL1,
				 PCF8525_CTRL1_12_24, 0);
	if (ret)
		return ret;

	ret = pcf8525_configure_crystal(dev, pcf8525);
	if (ret)
		return ret;
	/*
	 * Enable timestamp engine + timestamp interrupt (TSIE).
	 */
	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_TS_CTL1,
				 PCF8525_TS_CTL1_TSM | PCF8525_TS_CTL1_TSOFF,
				 PCF8525_TS_CTL1_TSM);
	if (ret)
		return ret;

	ret = regmap_update_bits(pcf8525->regmap, PCF8525_REG_CTRL5,
				 PCF8525_CTRL5_TSIE, PCF8525_CTRL5_TSIE);
	if (ret)
		return ret;

	/* Create sysfs timestamp0/1 */
	ret = rtc_add_group(pcf8525->rtc, &pcf8525_attr_group);
	if (ret)
		return ret;

	/* Optional IRQ */
	if (pcf8525->irq_inta > 0) {
		ret = devm_request_threaded_irq(dev, pcf8525->irq_inta,
						NULL, pcf8525_irq,
						IRQF_ONESHOT,
						dev_name(dev), dev);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request INTA IRQ\n");

		pcf8525->irq_enabled = true;

		/* Route/unmask alarm + timestamp to INTA by default */
		ret = pcf8525_unmask_irqs_intA(pcf8525);
		if (ret)
			return ret;

		device_init_wakeup(dev, true);
		dev_pm_set_wake_irq(dev, pcf8525->irq_inta);
		set_bit(RTC_FEATURE_ALARM, pcf8525->rtc->features);
	}

	ret = pcf8525_watchdog_init(dev, pcf8525);
	if (ret)
		return ret;

	/* Register device */
	ret = devm_rtc_register_device(pcf8525->rtc);
	if (ret)
		return ret;

	pcf8525_hwmon_register(dev, pcf8525);

	return 0;
}

static const struct of_device_id pcf8525_of_match[] = {
	{ .compatible = "nxp,pcf8525" },
	{ }
};
MODULE_DEVICE_TABLE(of, pcf8525_of_match);

static const struct i2c_device_id pcf8525_i2c_id[] = {
	{ .name = "pcf8525" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pcf8525_i2c_id);

static struct i2c_driver pcf8525_driver = {
	.driver = {
		.name = "rtc-pcf8525",
		.of_match_table = pcf8525_of_match,
	},
	.probe = pcf8525_probe,
	.id_table = pcf8525_i2c_id,
};
module_i2c_driver(pcf8525_driver);

MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_AUTHOR("Shiv Prakash Gupta <shivprakash.gupta@nxp.com>");
MODULE_DESCRIPTION("NXP PCF8525 RTC driver");
MODULE_LICENSE("GPL");
