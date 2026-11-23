// SPDX-License-Identifier: GPL-2.0
/*
 * Charger driver for cw2217
 *
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 *
 * Author: Xu Shengfei <xsf@rock-chips.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include <asm/byteorder.h>

#define CW2217_VERSION		0x00
#define CW2217_CHIP_ID		0xA0

#define CW2217_VCELL_H		0x02
#define CW2217_VCELL_L		0x03

#define CW2217_SOC_H		0x04
#define CW2217_SOC_L		0x05

#define CW2217_TEMP		0x06

#define CW2217_CONFIG		0x08
#define CW2217_CONFIG_ACT_MSK	GENMASK(7, 4)
#define CW2217_CONFIG_SLEEP	(0x03 << 6)
#define CW2217_CONFIG_RESTART	(0x03 << 4)
#define CW2217_CONFIG_ACTIVE	(0x00)

#define CW2217_INT_CONF		0x0A
#define CW2217_EN_SOC_INT	BIT(6)
#define CW2217_EN_TMX_INT	BIT(5)
#define CW2217_EN_TMN_INT	BIT(4)

#define CW2217_SOC_ALERT	0x0B
#define CW2217_SOC_UPDATE_FLG	BIT(7)
#define CW2217_SOC_ALERT_MSK	GENMASK(6, 0)
#define CW2217_SOC_ALERT_OFF	0x0

#define CW2217_TEMP_MAX		0x0C

#define CW2217_TEMP_MIN		0x0D

#define CW2217_CURRENT_H	0x0E
#define CW2217_CURRENT_L	0x0F

#define CW2217_BAT_PROFILE	0x10
#define CW2217_PROFILE_SZ	80

#define CW2217_T_HOST_H		0xA0
#define CW2217_T_HOST_L		0xA1

#define CW2217_USER_CONF	0xA2

#define CW2217_CYCLECNT_H	0xA4
#define CW2217_CYCLECNT_L	0xA5

#define CW2217_SOH		0xA6

/* This register not documented in datasheet. */
#define CW2217_IC_STATE		0xA7
#define CW2217_IC_READY_MASK	0x0C
#define CW2217_IC_READY		3

#define CW2217_STB_CUR_H	0xA8
#define CW2217_STB_CUR_L	0xA9

#define CW2217_FW_VERSION	0xAB
#define CW2217_ID_MASK		GENMASK(7, 6)
#define CW2215_ID		2
#define CW2217_ID		1
#define CW2218_ID		0

/* Default values from BSP driver */
#define CW2217_USER_RSENSE		1500
#define CW2217_DEFAULT_POLL_INTERVAL_MS	5000

struct cw2217_battery {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *cw_bat;
	u8 *bat_profile;
	struct workqueue_struct *battery_workqueue;
	struct delayed_work battery_delay_work;
	int user_rsense;
	int chip_id;
	int voltage;
	int soc;
	int temp;
	long cw_current;
	int cycle;
	int soh;
	int fw_version;
	u32 poll_interval_ms;
	bool update_profile;
	bool battery_changed;
};

/*
 * The CONFIG register is used for the host MCU to configure the fuel gauge IC.
 * The default value is 0xF0, SLEEP and RESTART bits are set. To power up the
 * IC, the host MCU needs to write 0x30 to exit shutdown mode, and then write
 * 0x00 to restart the gauge to enter active mode. To reset the IC, the host
 * MCU needs to write 0xF0, 0x30 and 0x00 in sequence to this register to
 * complete the restart procedure. The CW2217 will reload relevant parameters
 * and settings and restart SOC calculation. Note that the SOC may be a
 * different value after reset operation since it is a brand-new calculation
 * based on the latest battery status. The BSP driver slept 20ms after restart
 * and 10ms after setting active, though no delays are documented in the
 * datasheet.
 */
static int cw2217_active(struct cw2217_battery *cw_bat, bool active)
{
	int ret;

	ret = regmap_write(cw_bat->regmap, CW2217_CONFIG,
			   CW2217_CONFIG_RESTART);
	if (ret)
		return ret;

	fsleep(20000);

	if (active) {
		ret = regmap_write(cw_bat->regmap, CW2217_CONFIG,
				   CW2217_CONFIG_ACTIVE);
		if (ret)
			return ret;
	} else {
		ret = regmap_write(cw_bat->regmap, CW2217_CONFIG,
				   CW2217_CONFIG_SLEEP | CW2217_CONFIG_RESTART);
		if (ret)
			return ret;
	}

	fsleep(10000);

	return 0;
}

/*
 * The 0x00 register is an UNSIGNED 8bit read-only register. Its value is
 * fixed to 0xA0 in shutdown mode and active mode.
 */
static int cw2217_get_chip_id(struct cw2217_battery *cw_bat)
{
	u32 reg;
	int ret;

	ret = regmap_read(cw_bat->regmap, CW2217_VERSION, &reg);
	if (ret)
		return ret;

	cw_bat->chip_id = reg;

	return 0;
}

/*
 * The VCELL registers (0x02 0x03) are an UNSIGNED 14bit read-only register
 * that updates the battery voltage continuously. Battery voltage is measured
 * between the VCELL pin and VSS pin, which is the ground reference. A 14bit
 * sigma-delta A/D converter is used and the voltage resolution is 312.5uV
 * (0.3125mV is * 5/16).
 */
static int cw2217_get_voltage(struct cw2217_battery *cw_bat)
{
	int val = cw_bat->voltage;
	__be16 reg;
	int ret;

	ret = regmap_bulk_read(cw_bat->regmap, CW2217_VCELL_H,
			       &reg, sizeof(reg));
	if (ret)
		return ret;

	cw_bat->voltage = ((be16_to_cpu(reg) * 5) / 16) * 1000;

	if (val != cw_bat->voltage)
		cw_bat->battery_changed = true;

	return 0;
}

/*
 * The SOC registers (0x04 0x05) are an UNSIGNED 16bit read-only register that
 * indicates the SOC of the battery. The SOC shows in % format, which means
 * how much percent of the battery's total available capacity is remaining in
 * the battery now. The SOC can intrinsically adjust itself to cater to the
 * change of battery status; including load, temperature, and aging.
 * The high byte(0x04) contains the SOC in 1% unit which can be directly used
 * if this resolution is good enough for the application. The low byte (0x05)
 * provides more accurate fractional part of the SOC and its LSB is (1/256) %.
 * For now, we are only interested in the high byte.
 */
static int cw2217_get_capacity(struct cw2217_battery *cw_bat)
{
	int val = cw_bat->soc;
	u32 reg;
	int ret;

	ret = regmap_read(cw_bat->regmap, CW2217_SOC_H, &reg);
	if (ret)
		return ret;

	cw_bat->soc = clamp(reg, 0, 100);
	if (val != cw_bat->soc)
		cw_bat->battery_changed = true;

	return 0;
}

/*
 * The TEMP register is an UNSIGNED 8bit read only register.
 * It reports the real-time battery temperature
 * measured at TS pin. The scope is from -40 to 87.5 degrees Celsius,
 * LSB is 0.5 degree Celsius. TEMP(C) = - 40 + Value(0x06 Reg) / 2
 */
static int cw2217_get_temp(struct cw2217_battery *cw_bat)
{
	int val = cw_bat->temp;
	u32 reg;
	int ret;

	ret = regmap_read(cw_bat->regmap, CW2217_TEMP, &reg);
	if (ret)
		return ret;

	cw_bat->temp = (int)reg * 5 - 400;

	if (val != cw_bat->temp)
		cw_bat->battery_changed = true;

	return 0;
}

/*
 * Current is a signed 16bit register (0x0E 0x0F) that reports current A/D
 * converter result of the voltage across the current sense resistor,
 * 10mohm typical. The register value should be divided by the sense
 * resistance to convert to amperes. The value of the sense resistor
 * determines the resolution and the full-scale range of the current readings.
 * The LSB of 0x0F is (52.4/32768)uV for CW2215 and CW2217. The LSB of 0x0F is
 * (125/32768)uV for CW2218. The calculation in the datasheet sets the units
 * of the sense resistor at milliohms (not microohms) so multiply by 1000
 * to return the current in the correct units.
 */
static int cw2217_get_current(struct cw2217_battery *cw_bat)
{
	int val = cw_bat->cw_current;
	s32 cw_current;
	__be16 reg;
	u8 fw_ver;
	int ret;

	if (cw_bat->fw_version == 0) {
		cw_bat->cw_current = 0;
		dev_err(cw_bat->dev, "Error! CW2217 firmware read error!\n");
		return -EIO;
	}

	ret = regmap_bulk_read(cw_bat->regmap, CW2217_CURRENT_H,
			       &reg, sizeof(reg));
	if (ret)
		return ret;

	cw_current = (s16)be16_to_cpu(reg);

	fw_ver = FIELD_GET(CW2217_ID_MASK, cw_bat->fw_version);
	switch (fw_ver) {
	case CW2215_ID:
	case CW2217_ID:
		cw_bat->cw_current = div_s64((s64)cw_current * 1600 * 1000, cw_bat->user_rsense);
		break;
	case CW2218_ID:
		cw_bat->cw_current = div_s64((s64)cw_current * 3815 * 1000, cw_bat->user_rsense);
		break;
	default:
		cw_bat->cw_current = 0;
		dev_err(cw_bat->dev, "Error! CW2217 firmware read error!\n");
		return -EIO;
	}

	if (val != cw_bat->cw_current)
		cw_bat->battery_changed = true;

	return 0;
}

/*
 * CYCLECNT is an UNSIGNED 16bit register (0xA4 0xA5) that counts cycle life
 * of the battery. The LSB of 0xA5 stands for 1/16 cycle. This register will
 * be cleared after entering shutdown mode.
 */
static int cw2217_get_cycle_count(struct cw2217_battery *cw_bat)
{
	int val = cw_bat->cycle;
	__be16 reg;
	int ret;

	ret = regmap_bulk_read(cw_bat->regmap, CW2217_CYCLECNT_H,
			       &reg, sizeof(reg));
	if (ret)
		return ret;

	cw_bat->cycle = be16_to_cpu(reg) / 16;

	if (val != cw_bat->cycle)
		cw_bat->battery_changed = true;

	return 0;
}

/*
 * SOH (State of Health) is an UNSIGNED 8bit register(0xA6) that represents the level of
 * battery aging by tracking battery internal impedance increment. When the device enters
 * active mode, this register refresh to 0x64 by default. Its range is 0x00 to 0x64,
 * indicating 0 to 100%. This register will be clear after enters shutdown mode.
 */
static int cw2217_get_soh(struct cw2217_battery *cw_bat)
{
	int val = cw_bat->soh;
	u32 reg;
	int ret;

	ret = regmap_read(cw_bat->regmap, CW2217_SOH, &reg);
	if (ret)
		return ret;

	cw_bat->soh = clamp(reg, 0, 100);

	if (val != cw_bat->soh)
		cw_bat->battery_changed = true;

	return 0;
}

/*
 * FW_VERSION register reports the firmware (FW) running in the chip. It is
 * fixed to 0x00 when the chip is in shutdown mode. When in active mode,
 * Bit [7:6] = '01' stand for the CW2217, Bit [7:6] = '00' stand for the
 * CW2218 and Bit [7:6] = '10' stand for CW2215. Bit[5:0] stand for the FW
 * version running in the chip. Note that the FW version is subject to update.
 */
static int cw2217_get_fw_version(struct cw2217_battery *cw_bat)
{
	u32 reg;
	int ret;

	ret = regmap_read(cw_bat->regmap, CW2217_FW_VERSION, &reg);
	if (ret)
		return ret;

	cw_bat->fw_version = reg;

	return 0;
}

static int cw2217_update_data(struct cw2217_battery *cw_bat)
{
	int ret;

	ret = cw2217_get_voltage(cw_bat);
	if (ret)
		return ret;

	ret = cw2217_get_capacity(cw_bat);
	if (ret)
		return ret;

	ret = cw2217_get_temp(cw_bat);
	if (ret)
		return ret;

	ret = cw2217_get_current(cw_bat);
	if (ret)
		return ret;

	ret = cw2217_get_cycle_count(cw_bat);
	if (ret)
		return ret;

	return cw2217_get_soh(cw_bat);
}

static int cw2217_parse_properties(struct cw2217_battery *cw_bat)
{
	struct device *dev = cw_bat->dev;
	int length;
	int ret;

	ret = device_property_read_u32(dev, "cellwise,monitor-interval-ms",
				       &cw_bat->poll_interval_ms);
	if (ret) {
		dev_dbg(cw_bat->dev, "Using default poll interval\n");
		cw_bat->poll_interval_ms = CW2217_DEFAULT_POLL_INTERVAL_MS;
	}

	/*
	 * The hardware reports a minimum resolution of 10ms for certain values,
	 * but the previously used minimum value of 250ms seems like a sane
	 * minimum value to allow.
	 */
	if (cw_bat->poll_interval_ms < 250) {
		dev_warn(cw_bat->dev, "Monitor interval too low, setting to 250ms\n");
		cw_bat->poll_interval_ms = 250;
	}

	/*
	 * Datasheet only lists options of between 1M and 50M for resistor
	 * values, so assuming that is all and clamping if out of range.
	 */
	ret = device_property_read_u32(dev,
				       "shunt-resistor-micro-ohms",
				       &cw_bat->user_rsense);
	if (ret) {
		cw_bat->user_rsense = CW2217_USER_RSENSE;
		dev_warn(cw_bat->dev,
			 "Using default resistor value, reported mA may be wrong\n");
	}

	/* Clamp, but also warn we're clamping due to out of range. */
	if ((cw_bat->user_rsense < 1000) || (cw_bat->user_rsense > 50000)) {
		cw_bat->user_rsense = clamp(cw_bat->user_rsense, 1000, 50000);
		dev_warn(cw_bat->dev, "Shunt resistor value out of range, setting to %d",
			 cw_bat->user_rsense);
	}

	length = device_property_count_u8(dev, "cellwise,battery-profile");
	if (length < 0) {
		dev_warn(cw_bat->dev,
			 "No battery-profile found, using current flash contents\n");
	} else if (length != CW2217_PROFILE_SZ) {
		dev_err(cw_bat->dev, "battery-profile must be %d bytes\n",
			CW2217_PROFILE_SZ);
		return -EINVAL;
	} else {
		cw_bat->bat_profile = devm_kzalloc(dev, length, GFP_KERNEL);
		if (!cw_bat->bat_profile)
			return -ENOMEM;

		ret = device_property_read_u8_array(dev,
						    "cellwise,battery-profile",
						    cw_bat->bat_profile,
						    length);
		if (ret)
			return ret;
	}

	return 0;
}

/* Load a new battery profile provided from the device tree. */
static int cw2217_update_config(struct cw2217_battery *cw_bat)
{
	u32 reg;
	int ret;

	/* Set the battery monitor state to shutdown. */
	ret = cw2217_active(cw_bat, false);
	if (ret)
		return ret;

	/*
	 * Update new battery info if we have a profile, otherwise continue
	 * like we updated the profile (without actually updating it).
	 */
	if (cw_bat->bat_profile) {
		ret = regmap_raw_write(cw_bat->regmap, CW2217_BAT_PROFILE,
				       cw_bat->bat_profile, CW2217_PROFILE_SZ);
		if (ret)
			return ret;
	}

	/*
	 * Set update flag for new profile (even if we didn't update it) and
	 * disable SOC alert.
	 */
	ret = regmap_write(cw_bat->regmap, CW2217_SOC_ALERT,
			   CW2217_SOC_UPDATE_FLG | CW2217_SOC_ALERT_OFF);
	if (ret)
		return ret;

	/* mask all interrupts */
	ret = regmap_clear_bits(cw_bat->regmap, CW2217_INT_CONF,
				CW2217_EN_SOC_INT | CW2217_EN_TMX_INT | CW2217_EN_TMN_INT);
	if (ret)
		return ret;

	ret = cw2217_active(cw_bat, true);
	if (ret)
		return ret;

	/*
	 * Check the undocumented register for bits 2 and 3 to be 1. Note that
	 * the BSP driver would sleep 100ms after every iteration and retry up
	 * to 50 times, this felt excessive so reducing to 10.
	 */
	ret = regmap_read_poll_timeout(cw_bat->regmap, CW2217_IC_STATE, reg,
				       FIELD_GET(CW2217_IC_READY_MASK, reg) == CW2217_IC_READY,
				       100 * USEC_PER_MSEC, 10 * USEC_PER_SEC);
	if (ret) {
		dev_err(cw_bat->dev,
			"Gauge did not become ready after profile upload\n");
		cw2217_active(cw_bat, false);
		return ret;
	}

	return 0;
}

/* Determine whether the battery profile needs to be updated. */
static int cw2217_check_profile(struct cw2217_battery *cw_bat)
{
	u8 bat_info[CW2217_PROFILE_SZ];
	int ret;
	u32 reg;

	cw_bat->update_profile = false;

	ret = regmap_read(cw_bat->regmap, CW2217_CONFIG, &reg);
	if (ret)
		return ret;

	/* Active is 0x00, anything else is inactive */
	if (FIELD_GET(CW2217_CONFIG_ACT_MSK, reg)) {
		cw_bat->update_profile = true;
		return 0;
	}

	ret = regmap_read(cw_bat->regmap, CW2217_SOC_ALERT, &reg);
	if (ret)
		return ret;

	if (FIELD_GET(CW2217_SOC_UPDATE_FLG, reg) == 0) {
		cw_bat->update_profile = true;
		return 0;
	}

	/* We're using the existing profile, so checking here isn't needed. */
	if (!cw_bat->bat_profile)
		return 0;

	ret = regmap_raw_read(cw_bat->regmap, CW2217_BAT_PROFILE, bat_info, CW2217_PROFILE_SZ);
	if (ret) {
		dev_err(cw_bat->dev, "Failed to read existing battery profile\n");
		return ret;
	}

	if (memcmp(bat_info, cw_bat->bat_profile, CW2217_PROFILE_SZ)) {
		dev_warn(cw_bat->dev, "Replacing stored battery profile\n");
		cw_bat->update_profile = true;
	}

	return 0;
}

static int cw2217_init(struct cw2217_battery *cw_bat)
{
	int ret;

	ret = cw2217_get_chip_id(cw_bat);
	if (ret)
		return dev_err_probe(cw_bat->dev, ret, "I2C read write error");

	/*
	 * All of the known Cellwise 221x chips set the ID bits to 0xa0. If it
	 * doesn't match, we're trying to probe the wrong hardware.
	 */
	if (cw_bat->chip_id != CW2217_CHIP_ID)
		return dev_err_probe(cw_bat->dev, -ENODEV, "Not a cw221x\n");

	ret = cw2217_check_profile(cw_bat);
	if (ret) {
		dev_err(cw_bat->dev, "i2c read write error");
		return ret;
	}

	if (cw_bat->update_profile) {
		ret = cw2217_update_config(cw_bat);
		if (ret)
			return ret;
	}

	return 0;
}

static void cw2217_bat_work(struct work_struct *work)
{
	struct delayed_work *delay_work;
	struct cw2217_battery *cw_bat;
	int ret;

	delay_work = container_of(work,
				  struct delayed_work,
				  work);

	cw_bat = container_of(delay_work,
			      struct cw2217_battery,
			      battery_delay_work);

	ret = cw2217_update_data(cw_bat);
	if (ret)
		dev_err(cw_bat->dev, "i2c read error when update data");

	/*
	 * Note that the very first run of this will always have a
	 * battery_changed = true because the initial values will be compared
	 * against zero, that's not a big deal for us.
	 */
	if (cw_bat->battery_changed) {
		power_supply_changed(cw_bat->cw_bat);
		cw_bat->battery_changed = false;
	}

	queue_delayed_work(cw_bat->battery_workqueue,
			   &cw_bat->battery_delay_work,
			   msecs_to_jiffies(cw_bat->poll_interval_ms));
}

static int cw2217_battery_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct cw2217_battery *cw_bat = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = cw_bat->cycle;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = cw_bat->soc;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		/*
		 * Observed when battery was full that it would report
		 * approximately -12mv at between 97-99 SoC. Assume that if the
		 * battery current is above 0 it's charging, it's below
		 * 0 but with an SoC 95% or above it's not charging because
		 * it's full, otherwise it's simply not charging. If it's not
		 * supplied it is discharging.
		 */
		if (power_supply_is_system_supplied()) {
			if (cw_bat->cw_current > 0)
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			else if (cw_bat->soc >= 95)
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		} else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = (cw_bat->voltage <= 0) ? 0 : 1;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = cw_bat->voltage;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = cw_bat->cw_current;
		break;
	case POWER_SUPPLY_PROP_STATE_OF_HEALTH:
		val->intval = cw_bat->soh;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = cw_bat->temp;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static enum power_supply_property cw2217_battery_properties[] = {
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_STATE_OF_HEALTH,
	POWER_SUPPLY_PROP_TEMP,
};

static const struct power_supply_desc cw2217_bat_desc = {
	.name		= "cw2217-battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= cw2217_battery_properties,
	.num_properties	= ARRAY_SIZE(cw2217_battery_properties),
	.get_property	= cw2217_battery_get_property,
};

static const struct regmap_range cw2217_ranges_rd_yes[] = {
	regmap_reg_range(CW2217_VERSION, CW2217_VERSION),
	regmap_reg_range(CW2217_VCELL_H, CW2217_TEMP),
	regmap_reg_range(CW2217_CONFIG, CW2217_CONFIG),
	regmap_reg_range(CW2217_INT_CONF, CW2217_CURRENT_L),
	regmap_reg_range(CW2217_BAT_PROFILE,
			 CW2217_BAT_PROFILE + CW2217_PROFILE_SZ - 1),
	regmap_reg_range(CW2217_T_HOST_H, CW2217_USER_CONF),
	regmap_reg_range(CW2217_CYCLECNT_H, CW2217_STB_CUR_L),
	regmap_reg_range(CW2217_FW_VERSION, CW2217_FW_VERSION),
};

static const struct regmap_access_table cw2217_rd_table = {
	.yes_ranges = cw2217_ranges_rd_yes,
	.n_yes_ranges = ARRAY_SIZE(cw2217_ranges_rd_yes),
};

static const struct regmap_range cw2217_ranges_wr_yes[] = {
	regmap_reg_range(CW2217_TEMP, CW2217_TEMP),
	regmap_reg_range(CW2217_CONFIG, CW2217_CONFIG),
	regmap_reg_range(CW2217_INT_CONF, CW2217_CURRENT_L),
	regmap_reg_range(CW2217_BAT_PROFILE,
			 CW2217_BAT_PROFILE + CW2217_PROFILE_SZ - 1),
	regmap_reg_range(CW2217_T_HOST_H, CW2217_USER_CONF),
};

static const struct regmap_access_table cw2217_wr_table = {
	.yes_ranges = cw2217_ranges_wr_yes,
	.n_yes_ranges = ARRAY_SIZE(cw2217_ranges_wr_yes),
};

static const struct regmap_range cw2217_ranges_vol_yes[] = {
	regmap_reg_range(CW2217_VCELL_H, CW2217_TEMP),
	regmap_reg_range(CW2217_INT_CONF, CW2217_SOC_ALERT),
	regmap_reg_range(CW2217_CURRENT_H, CW2217_CURRENT_L),
	regmap_reg_range(CW2217_CYCLECNT_H, CW2217_STB_CUR_L),
};

static const struct regmap_access_table cw2217_vol_table = {
	.yes_ranges = cw2217_ranges_vol_yes,
	.n_yes_ranges = ARRAY_SIZE(cw2217_ranges_vol_yes),
};

static const struct regmap_config cw2217_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.rd_table = &cw2217_rd_table,
	.wr_table = &cw2217_wr_table,
	.volatile_table = &cw2217_vol_table,
	.cache_type = REGCACHE_MAPLE,
	.max_register = CW2217_FW_VERSION,
};

static int cw2217_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = { 0 };
	struct cw2217_battery *cw_bat;
	int ret;

	cw_bat = devm_kzalloc(&client->dev, sizeof(*cw_bat), GFP_KERNEL);
	if (!cw_bat)
		return -ENOMEM;

	i2c_set_clientdata(client, cw_bat);
	cw_bat->dev = &client->dev;

	cw_bat->regmap = devm_regmap_init_i2c(client, &cw2217_regmap_config);
	if (IS_ERR(cw_bat->regmap))
		return dev_err_probe(cw_bat->dev, PTR_ERR(cw_bat->regmap),
				     "Failed to allocate regmap: %ld\n",
				     PTR_ERR(cw_bat->regmap));

	ret = cw2217_parse_properties(cw_bat);
	if (ret)
		return ret;

	ret = cw2217_init(cw_bat);
	if (ret)
		return dev_err_probe(cw_bat->dev, ret, "cw2217 init fail\n");

	ret = cw2217_get_fw_version(cw_bat);
	if (ret)
		return dev_err_probe(cw_bat->dev, ret,
				     "Unable to read fw version\n");

	ret = cw2217_update_data(cw_bat);
	if (ret)
		return dev_err_probe(cw_bat->dev, ret,
				     "Unable to read initial data\n");

	psy_cfg.fwnode = dev_fwnode(&client->dev);
	psy_cfg.drv_data = cw_bat;
	cw_bat->cw_bat = devm_power_supply_register(&client->dev,
						    &cw2217_bat_desc, &psy_cfg);
	if (IS_ERR(cw_bat->cw_bat))
		return dev_err_probe(cw_bat->dev, PTR_ERR(cw_bat->cw_bat),
				     "Failed to register battery: %ld\n",
				     PTR_ERR(cw_bat->cw_bat));

	cw_bat->battery_workqueue = devm_alloc_ordered_workqueue(&client->dev,
								 "cw2217_gauge", 0);
	if (!cw_bat->battery_workqueue)
		return -ENOMEM;

	ret = devm_delayed_work_autocancel(&client->dev,
					   &cw_bat->battery_delay_work,
					   cw2217_bat_work);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register delayed work\n");

	queue_delayed_work(cw_bat->battery_workqueue,
			   &cw_bat->battery_delay_work,
			   msecs_to_jiffies(50));

	return 0;
}

static int __maybe_unused cw2217_bat_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct cw2217_battery *cw_bat = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&cw_bat->battery_delay_work);
	return 0;
}

static int __maybe_unused cw2217_bat_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct cw2217_battery *cw_bat = i2c_get_clientdata(client);

	queue_delayed_work(cw_bat->battery_workqueue,
			   &cw_bat->battery_delay_work,
			   msecs_to_jiffies(20));
	return 0;
}

static SIMPLE_DEV_PM_OPS(cw2217_bat_pm_ops, cw2217_bat_suspend,
			 cw2217_bat_resume);

static const struct i2c_device_id cw2217_id_table[] = {
	{ .name = "cw2215" },
	{ .name = "cw2217" },
	{ .name = "cw2218" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cw2217_id_table);

static const struct of_device_id cw2217_match_table[] = {
	{ .compatible = "cellwise,cw2215", },
	{ .compatible = "cellwise,cw2217", },
	{ .compatible = "cellwise,cw2218", },
	{ },
};
MODULE_DEVICE_TABLE(of, cw2217_match_table);

static struct i2c_driver cw2217_driver = {
	.driver = {
		.name = "cw2217",
		.pm = &cw2217_bat_pm_ops,
		.of_match_table = cw2217_match_table,
	},
	.probe = cw2217_probe,
	.id_table = cw2217_id_table,
};

module_i2c_driver(cw2217_driver);

MODULE_AUTHOR("Xu Shengfei <xsf@rock-chips.com>");
MODULE_AUTHOR("Chris Morgan <macromorgan@hotmail.com>");
MODULE_DESCRIPTION("cw2217 battery driver");
MODULE_LICENSE("GPL");
