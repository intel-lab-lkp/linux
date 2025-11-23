// SPDX-License-Identifier: GPL-2.0-only
/*
 * max77759_charger.c - Battery charger driver for MAX77759 charger device.
 *
 * Copyright 2025 Google LLC.
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/math64.h>
#include <linux/mfd/max77759.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/string_choices.h>

/* CHG_INT_OK */
#define CHG_INT_OK_AICL_OK			BIT(7)
#define CHG_INT_OK_CHGIN_OK			BIT(6)
#define CHG_INT_OK_CHG_OK			BIT(4)
#define CHG_INT_OK_INLIM_OK			BIT(2)

/* CHG_DETAILS_00 */
#define CHG_DETAILS_OO_CHGIN_DTLS		GENMASK(6, 5)

/*
 * Charger Input Status
 * @CHGIN_DTLS_VBUS_UNDERVOLTAGE: Charger input voltage (Vchgin) < Under Voltage
 *     Threshold (Vuvlo)
 * @CHGIN_DTLS_VBUS_MARGINAL_VOLTAGE: Vchgin > Vuvlo and
 *     Vchgin < (Battery Voltage (Vbatt) + system voltage (Vsys))
 * @CHGIN_DTLS_VBUS_OVERVOLTAGE: Vchgin > Over Voltage threshold (Vovlo)
 * @CHGIN_DTLS_VBUS_VALID: Vchgin > Vuvlo, Vchgin < Vovlo and
 *     Vchgin > (Vsys + Vbatt)
 */
enum chgin_dtls_status {
	CHGIN_DTLS_VBUS_UNDERVOLTAGE,
	CHGIN_DTLS_VBUS_MARGINAL_VOLTAGE,
	CHGIN_DTLS_VBUS_OVERVOLTAGE,
	CHGIN_DTLS_VBUS_VALID,
};

/* CHG_DETAILS_01 */
#define CHG_DETAILS_01_BAT_DTLS			GENMASK(6, 4)

/*
 * Battery Details
 * @BAT_DTLS_NO_BATT_CHG_SUSP:    No battery and the charger suspended
 * @BAT_DTLS_DEAD_BATTERY:        Vbatt < Vtrickle
 * @BAT_DTLS_BAT_CHG_TIMER_FAULT: Charging suspended due to timer fault
 * @BAT_DTLS_BAT_OKAY: Battery okay and Vbatt > Min Sys Voltage (Vsysmin)
 * @BAT_DTLS_BAT_UNDERVOLTAGE:    Battery is okay. Vtrickle < Vbatt < Vsysmin
 * @BAT_DTLS_BAT_OVERVOLTAGE:     Battery voltage > Overvoltage threshold
 * @BAT_DTLS_BAT_OVERCURRENT: Battery current exceeds overcurrent threshold
 * @BAT_DTLS_BAT_ONLY_MODE:   Battery only mode and battery level not available
 */
enum bat_dtls_states {
	BAT_DTLS_NO_BATT_CHG_SUSP,
	BAT_DTLS_DEAD_BATTERY,
	BAT_DTLS_BAT_CHG_TIMER_FAULT,
	BAT_DTLS_BAT_OKAY,
	BAT_DTLS_BAT_UNDERVOLTAGE,
	BAT_DTLS_BAT_OVERVOLTAGE,
	BAT_DTLS_BAT_OVERCURRENT,
	BAT_DTLS_BAT_ONLY_MODE,
};

#define CHG_DETAILS_01_CHG_DTLS			GENMASK(3, 0)

/*
 * Charger Details
 * @CHG_DTLS_PREQUAL: Charger in prequalification mode
 * @CHG_DTLS_CC:      Charger in fast charge const curr mode
 * @CHG_DTLS_CV:      Charger in fast charge const voltage mode
 * @CHG_DTLS_TO:      Charger is in top off mode
 * @CHG_DTLS_DONE:    Charger is done
 * @CHG_DTLS_RSVD_1:  Reserved
 * @CHG_DTLS_TIMER_FAULT:   Charger is in timer fault mode
 * @CHG_DTLS_SUSP_BATT_THM: Charger is suspended as bettery removal detected
 * @CHG_DTLS_OFF:     Charger is off. Input invalid or charger disabled
 * @CHG_DTLS_RSVD_2:  Reserved
 * @CHG_DTLS_RSVD_3:   Reserved
 * @CHG_DTLS_OFF_WDOG_TIMER:    Charger is off as watchdog timer expired
 * @CHG_DTLS_SUSP_JEITA:        Charger is in JEITA control mode
 */
enum chg_dtls_states {
	CHG_DTLS_PREQUAL,
	CHG_DTLS_CC,
	CHG_DTLS_CV,
	CHG_DTLS_TO,
	CHG_DTLS_DONE,
	CHG_DTLS_RSVD_1,
	CHG_DTLS_TIMER_FAULT,
	CHG_DTLS_SUSP_BATT_THM,
	CHG_DTLS_OFF,
	CHG_DTLS_RSVD_2,
	CHG_DTLS_RSVD_3,
	CHG_DTLS_OFF_WDOG_TIMER,
	CHG_DTLS_SUSP_JEITA,
};

/* CHG_DETAILS_02 */
#define CHG_DETAILS_02_CHGIN_STS		BIT(5)

/* CHG_CNFG_00 */
#define CHG_CNFG_00_MODE			GENMASK(3, 0)

enum chg_mode {
	CHG_MODE_OFF,
	CHG_MODE_CHG_BUCK_ON = 0x5,
	CHG_MODE_OTG_BOOST_ON = 0xA,
};

/* CHG_CNFG_02 */
/* Fast Charge Current Selection (in uA) */
#define CHG_CNFG_02_CHGCC			GENMASK(5, 0)
#define CHGCC_LIMIT_MIN_UA			133330
#define CHGCC_LIMIT_MAX_UA			4000000
#define CHGCC_LIMIT_STEP_UA			66670
#define CHGCC_LIMIT_REG_OFFSET			0x2

/* CHG_CNFG_04 */
/* Charge Termination Voltage Setting (in mV) */
#define CHG_CNFG_04_CHG_CV_PRM			GENMASK(5, 0)
/* [3.8, 3.9] V range */
#define CHG_CV_PRM_LIMIT_LO_MIN_MV		3800
#define CHG_CV_PRM_LIMIT_LO_MAX_MV		3900
#define CHG_CV_PRM_LIMIT_LO_STEP_MV		100
#define CHG_CV_PRM_LIMIT_LO_MIN_REG		0x38
#define CHG_CV_PRM_LIMIT_LO_MAX_REG		0x39
/* [4, 4.5] V range */
#define CHG_CV_PRM_LIMIT_HI_MIN_MV		4000
#define CHG_CV_PRM_LIMIT_HI_MAX_MV		4500
#define CHG_CV_PRM_LIMIT_HI_STEP_MV		10
#define CHG_CV_PRM_LIMIT_HI_MIN_REG		0x0
#define CHG_CV_PRM_LIMIT_HI_MAX_REG		0x32

/* CHG_CNFG_06 */
#define CHG_CNFG_06_CHGPROT			GENMASK(3, 2)

/* CHG_CNFG_09 */
/* CHGIN Input Current Limit (in uA) */
#define CHG_CNFG_09_CHGIN_ILIM			GENMASK(6, 0)
#define CHGIN_ILIM_MIN_UA			100000
#define CHGIN_ILIM_MAX_UA			3200000
#define CHGIN_ILIM_STEP_UA			25000
#define CHGIN_ILIM_REG_OFFSET			0x3

/* CHG_CNFG_12 (Protected) */
/* Wireless Charging input channel select */
#define CHG_CNFG_12_WCINSEL			BIT(6)
/* CHGIN/USB input channel select */
#define CHG_CNFG_12_CHGINSEL			BIT(5)

/* CHG_CNFG_18 (Protected) */
/* Watchdog Timer Enable Bit */
#define CHG_CNFG_18_WDTEN			BIT(0)

/* Default values for Fast Charge Current & Float Voltage */
#define CHG_CC_DEFAULT_UA			2266770
#define CHG_FV_DEFAULT_MV			4300

struct max77759_charger {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	int irq[2];
	struct regulator_dev *usb_otg_rdev;
	struct notifier_block nb;
	struct power_supply *tcpm_psy;
	struct work_struct psy_work;
	struct mutex lock; /* protects the state below */
	enum chg_mode mode;
};

static inline int regval_to_val(int reg, int reg_offset, int step, int minval)
{
	return ((reg - reg_offset) * step) + minval;
}

static inline int val_to_regval(int val, int minval, int step, int reg_offset)
{
	s64 dividend;

	if (unlikely(step == 0))
		return reg_offset;

	dividend = (s64)val - minval;
	return DIV_S64_ROUND_CLOSEST(dividend, step) + reg_offset;
}

static inline int unlock_prot_regs(struct max77759_charger *chg, bool unlock)
{
	return regmap_update_bits(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_06,
				  CHG_CNFG_06_CHGPROT,
				  unlock ? CHG_CNFG_06_CHGPROT : 0);
}

static int charger_input_valid(struct max77759_charger *chg)
{
	u32 val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_INT_OK, &val);
	if (ret)
		return ret;

	return (val & CHG_INT_OK_CHG_OK) && (val & CHG_INT_OK_CHGIN_OK);
}

static int get_online(struct max77759_charger *chg)
{
	u32 val;
	int ret;

	ret = charger_input_valid(chg);
	if (ret <= 0)
		return ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_DETAILS_02, &val);
	if (ret)
		return ret;

	guard(mutex)(&chg->lock);
	return (val & CHG_DETAILS_02_CHGIN_STS) &&
		(chg->mode == CHG_MODE_CHG_BUCK_ON);
}

static int get_status(struct max77759_charger *chg)
{
	u32 val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_DETAILS_01,
			  &val);
	if (ret)
		return ret;

	switch (FIELD_GET(CHG_DETAILS_01_CHG_DTLS, val)) {
	case CHG_DTLS_PREQUAL:
	case CHG_DTLS_CC:
	case CHG_DTLS_CV:
	case CHG_DTLS_TO:
		return POWER_SUPPLY_STATUS_CHARGING;
	case CHG_DTLS_DONE:
		return POWER_SUPPLY_STATUS_FULL;
	case CHG_DTLS_TIMER_FAULT:
	case CHG_DTLS_SUSP_BATT_THM:
	case CHG_DTLS_OFF_WDOG_TIMER:
	case CHG_DTLS_SUSP_JEITA:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	case CHG_DTLS_OFF:
		return POWER_SUPPLY_STATUS_DISCHARGING;
	default:
		break;
	}

	return POWER_SUPPLY_STATUS_UNKNOWN;
}

static int get_charge_type(struct max77759_charger *chg)
{
	u32 val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_DETAILS_01,
			  &val);
	if (ret)
		return ret;

	switch (FIELD_GET(CHG_DETAILS_01_CHG_DTLS, val)) {
	case CHG_DTLS_PREQUAL:
		return POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
	case CHG_DTLS_CC:
		return POWER_SUPPLY_CHARGE_TYPE_FAST;
	case CHG_DTLS_CV:
	case CHG_DTLS_TO:
		return POWER_SUPPLY_CHARGE_TYPE_STANDARD;
	case CHG_DTLS_DONE:
	case CHG_DTLS_TIMER_FAULT:
	case CHG_DTLS_SUSP_BATT_THM:
	case CHG_DTLS_OFF_WDOG_TIMER:
	case CHG_DTLS_SUSP_JEITA:
	case CHG_DTLS_OFF:
		return POWER_SUPPLY_CHARGE_TYPE_NONE;
	default:
		break;
	}

	return POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
}

static int get_chg_health(struct max77759_charger *chg)
{
	u32 val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_DETAILS_00,
			  &val);
	if (ret)
		return ret;

	switch (FIELD_GET(CHG_DETAILS_OO_CHGIN_DTLS, val)) {
	case CHGIN_DTLS_VBUS_UNDERVOLTAGE:
	case CHGIN_DTLS_VBUS_MARGINAL_VOLTAGE:
		return POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
	case CHGIN_DTLS_VBUS_OVERVOLTAGE:
		return POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	case CHGIN_DTLS_VBUS_VALID:
		return POWER_SUPPLY_HEALTH_GOOD;
	default:
		break;
	}

	return POWER_SUPPLY_HEALTH_UNKNOWN;
}

static int get_batt_health(struct max77759_charger *chg)
{
	u32 val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_DETAILS_01,
			  &val);
	if (ret)
		return ret;

	switch (FIELD_GET(CHG_DETAILS_01_BAT_DTLS, val)) {
	case BAT_DTLS_NO_BATT_CHG_SUSP:
		return POWER_SUPPLY_HEALTH_NO_BATTERY;
	case BAT_DTLS_DEAD_BATTERY:
		return POWER_SUPPLY_HEALTH_DEAD;
	case BAT_DTLS_BAT_CHG_TIMER_FAULT:
		return POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
	case BAT_DTLS_BAT_OKAY:
	case BAT_DTLS_BAT_ONLY_MODE:
		return POWER_SUPPLY_HEALTH_GOOD;
	case BAT_DTLS_BAT_UNDERVOLTAGE:
		return POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
	case BAT_DTLS_BAT_OVERVOLTAGE:
		return POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	case BAT_DTLS_BAT_OVERCURRENT:
		return POWER_SUPPLY_HEALTH_OVERCURRENT;
	default:
		break;
	}

	return POWER_SUPPLY_HEALTH_UNKNOWN;
}

static int get_health(struct max77759_charger *chg)
{
	int ret;

	ret = get_online(chg);
	if (ret < 0)
		return ret;

	if (ret) {
		ret = get_chg_health(chg);
		if (ret < 0 || ret != POWER_SUPPLY_HEALTH_GOOD)
			return ret;
	}

	return get_batt_health(chg);
}

static int get_fast_charge_current(struct max77759_charger *chg)
{
	u32 regval;
	int ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_02, &regval);
	if (ret)
		return ret;

	ret = FIELD_GET(CHG_CNFG_02_CHGCC, regval);
	if (ret <= CHGCC_LIMIT_REG_OFFSET)
		return CHGCC_LIMIT_MIN_UA;

	return regval_to_val(ret, CHGCC_LIMIT_REG_OFFSET, CHGCC_LIMIT_STEP_UA,
			     CHGCC_LIMIT_MIN_UA);
}

static int set_fast_charge_current_limit(struct max77759_charger *chg, u32 cc_max_ua)
{
	u32 val;

	if (cc_max_ua < CHGCC_LIMIT_MIN_UA || cc_max_ua > CHGCC_LIMIT_MAX_UA)
		return -EINVAL;

	val = val_to_regval(cc_max_ua, CHGCC_LIMIT_MIN_UA, CHGCC_LIMIT_STEP_UA,
			    CHGCC_LIMIT_REG_OFFSET);
	return regmap_update_bits(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_02,
				  CHG_CNFG_02_CHGCC, val);
}

static int get_float_voltage(struct max77759_charger *chg)
{
	u32 regval;
	int ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_04, &regval);
	if (ret)
		return ret;

	ret = FIELD_GET(CHG_CNFG_04_CHG_CV_PRM, regval);
	switch (ret) {
	case CHG_CV_PRM_LIMIT_HI_MIN_REG ... CHG_CV_PRM_LIMIT_HI_MAX_REG:
		return regval_to_val(ret, CHG_CV_PRM_LIMIT_HI_MIN_REG,
				     CHG_CV_PRM_LIMIT_HI_STEP_MV,
				     CHG_CV_PRM_LIMIT_HI_MIN_MV);
	case CHG_CV_PRM_LIMIT_LO_MIN_REG ... CHG_CV_PRM_LIMIT_LO_MAX_REG:
		return regval_to_val(ret, CHG_CV_PRM_LIMIT_LO_MIN_REG,
				     CHG_CV_PRM_LIMIT_LO_STEP_MV,
				     CHG_CV_PRM_LIMIT_LO_MIN_MV);
	default:
		return -EINVAL;
	}

	return 0;
}

static int set_float_voltage_limit(struct max77759_charger *chg, u32 fv_mv)
{
	u32 regval;

	if (fv_mv >= CHG_CV_PRM_LIMIT_LO_MIN_MV &&
	    fv_mv <= CHG_CV_PRM_LIMIT_LO_MAX_MV) {
		regval = val_to_regval(fv_mv, CHG_CV_PRM_LIMIT_LO_MIN_MV,
				       CHG_CV_PRM_LIMIT_LO_STEP_MV,
				       CHG_CV_PRM_LIMIT_LO_MIN_REG);
	} else if (fv_mv >= CHG_CV_PRM_LIMIT_HI_MIN_MV &&
		   fv_mv <= CHG_CV_PRM_LIMIT_HI_MAX_MV) {
		regval = val_to_regval(fv_mv, CHG_CV_PRM_LIMIT_HI_MIN_MV,
				       CHG_CV_PRM_LIMIT_HI_STEP_MV,
				       CHG_CV_PRM_LIMIT_HI_MIN_REG);
	} else {
		return -EINVAL;
	}

	return regmap_update_bits(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_04,
				  CHG_CNFG_04_CHG_CV_PRM, regval);
}

static int get_input_current_limit(struct max77759_charger *chg)
{
	u32 regval;
	int ret;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_09, &regval);
	if (ret)
		return ret;

	ret = FIELD_GET(CHG_CNFG_09_CHGIN_ILIM, regval);
	if (ret <= CHGIN_ILIM_REG_OFFSET)
		return CHGIN_ILIM_MIN_UA;

	return regval_to_val(ret, CHGIN_ILIM_REG_OFFSET, CHGIN_ILIM_STEP_UA,
			     CHGIN_ILIM_MIN_UA);
}

static int set_input_current_limit(struct max77759_charger *chg, int ilim_ua)
{
	u32 regval;

	if (ilim_ua < 0)
		return -EINVAL;

	if (ilim_ua == 0)
		ilim_ua = CHGIN_ILIM_MIN_UA;
	else if (ilim_ua > CHGIN_ILIM_MAX_UA)
		ilim_ua = CHGIN_ILIM_MAX_UA;

	regval = val_to_regval(ilim_ua, CHGIN_ILIM_MIN_UA,
			       CHGIN_ILIM_STEP_UA, CHGIN_ILIM_REG_OFFSET);
	return regmap_update_bits(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_09,
				  CHG_CNFG_09_CHGIN_ILIM, regval);
}

static const enum power_supply_property max77759_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static int max77759_charger_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *pval)
{
	struct max77759_charger *chg = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = get_online(chg);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = charger_input_valid(chg);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = get_status(chg);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = get_charge_type(chg);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = get_health(chg);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = get_fast_charge_current(chg);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = get_float_voltage(chg);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = get_input_current_limit(chg);
		break;
	default:
		ret = -EINVAL;
	}

	pval->intval = ret;
	return ret < 0 ? ret : 0;
}

static const struct power_supply_desc max77759_charger_desc = {
	.name = "max77759-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = max77759_charger_props,
	.num_properties = ARRAY_SIZE(max77759_charger_props),
	.get_property = max77759_charger_get_property,
};

static int charger_set_mode(struct max77759_charger *chg, enum chg_mode mode)
{
	int ret;

	guard(mutex)(&chg->lock);

	if (chg->mode == mode)
		return 0;

	if ((mode == CHG_MODE_CHG_BUCK_ON || mode == CHG_MODE_OTG_BOOST_ON) &&
	    chg->mode != CHG_MODE_OFF) {
		dev_err(chg->dev, "Invalid mode transition from %d to %d",
			chg->mode, mode);
		return -EINVAL;
	}

	ret = regmap_update_bits(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_00,
				 CHG_CNFG_00_MODE, mode);
	if (ret)
		return ret;

	chg->mode = mode;
	return 0;
}

static int enable_usb_otg(struct regulator_dev *rdev)
{
	struct max77759_charger *chg = rdev_get_drvdata(rdev);

	return charger_set_mode(chg, CHG_MODE_OTG_BOOST_ON);
}

static int disable_usb_otg(struct regulator_dev *rdev)
{
	struct max77759_charger *chg = rdev_get_drvdata(rdev);

	return charger_set_mode(chg, CHG_MODE_OFF);
}

static int usb_otg_status(struct regulator_dev *rdev)
{
	struct max77759_charger *chg = rdev_get_drvdata(rdev);

	guard(mutex)(&chg->lock);
	return chg->mode == CHG_MODE_OTG_BOOST_ON;
}

static const struct regulator_ops usb_otg_reg_ops = {
	.enable = enable_usb_otg,
	.disable = disable_usb_otg,
	.is_enabled = usb_otg_status,
};

static const struct regulator_desc usb_otg_reg_desc = {
	.name = "usb-otg-vbus",
	.of_match = of_match_ptr("usb-otg-vbus-regulator"),
	.owner = THIS_MODULE,
	.ops = &usb_otg_reg_ops,
	.fixed_uV = 5000000,
	.n_voltages = 1,
};

static irqreturn_t irq_handler(int irq, void *data)
{
	struct max77759_charger *chg = data;
	u32 irq_status, chgint_ok, idx = 0;
	int ret;

	if (irq == chg->irq[0])
		idx = 0;
	else
		idx = 1;

	ret = regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_INT + idx,
			  &irq_status);
	if (ret) {
		dev_err(chg->dev, "regmap_read_error idx=%d ret=%d", idx, ret);
		return IRQ_HANDLED;
	}

	regmap_write(chg->regmap, MAX77759_CHGR_REG_CHG_INT + idx,
		     irq_status);
	regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_INT_OK, &chgint_ok);

	if (idx == 0) {
		if (irq_status & MAX77759_CHGR_REG_CHG_INT_AICL)
			dev_dbg(chg->dev, "AICL mode: %s",
				str_no_yes(chgint_ok & CHG_INT_OK_AICL_OK));

		if (irq_status & MAX77759_CHGR_REG_CHG_INT_CHGIN)
			dev_dbg(chg->dev, "CHGIN input valid: %s",
				str_yes_no(chgint_ok & CHG_INT_OK_CHGIN_OK));

		if (irq_status & MAX77759_CHGR_REG_CHG_INT_CHG)
			dev_dbg(chg->dev, "CHG status okay/off: %s",
				str_yes_no(chgint_ok & CHG_INT_OK_CHG_OK));

		if (irq_status & MAX77759_CHGR_REG_CHG_INT_INLIM)
			dev_dbg(chg->dev, "Current Limit reached: %s",
				str_no_yes(chgint_ok & CHG_INT_OK_INLIM_OK));
	} else {
		if (irq_status & MAX77759_CHGR_REG_CHG_INT2_BAT_OILO)
			dev_dbg(chg->dev,
				"Battery over-current threshold crossed");

		if (irq_status & MAX77759_CHGR_REG_CHG_INT2_CHG_STA_CC)
			dev_dbg(chg->dev, "Charger reached CC stage");

		if (irq_status & MAX77759_CHGR_REG_CHG_INT2_CHG_STA_CV)
			dev_dbg(chg->dev, "Charger reached CV stage");

		if (irq_status & MAX77759_CHGR_REG_CHG_INT2_CHG_STA_TO)
			dev_dbg(chg->dev, "Charger reached TO stage");

		if (irq_status & MAX77759_CHGR_REG_CHG_INT2_CHG_STA_DONE)
			dev_dbg(chg->dev, "Charger reached Done stage");
	}

	power_supply_changed(chg->psy);
	return IRQ_HANDLED;
}

static int max77759_init_irqhandler(struct max77759_charger *chg)
{
	static const char * const irq_res_names[] = { "INT1", "INT2" };
	struct device *dev = chg->dev;
	unsigned long irq_flags;
	struct irq_data *irqd;
	int *irq = chg->irq;
	int ret, i;

	for (i = 0; i < 2; i++) {
		irq[i] = platform_get_irq_byname(to_platform_device(dev),
						 irq_res_names[i]);
		if (irq[i] < 0) {
			dev_err(dev, "unable to find %s irq", irq_res_names[i]);
			return irq[i];
		}

		irq_flags = IRQF_ONESHOT | IRQF_SHARED;
		irqd = irq_get_irq_data(irq[i]);
		if (irqd)
			irq_flags |= irqd_get_trigger_type(irqd);

		ret = devm_request_threaded_irq(dev, irq[i], NULL, irq_handler,
						irq_flags, dev_name(dev), chg);
		if (ret) {
			dev_err(dev, "Unable to register threaded irq handler");
			return ret;
		}
	}

	return 0;
}

static int max77759_charger_init(struct max77759_charger *chg)
{
	int ret;
	u32 regval;

	regmap_read(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_00, &regval);
	chg->mode = FIELD_GET(CHG_CNFG_00_MODE, regval);
	ret = charger_set_mode(chg, CHG_MODE_OFF);
	if (ret)
		return ret;

	ret = set_fast_charge_current_limit(chg, CHG_CC_DEFAULT_UA);
	if (ret)
		return ret;

	ret = set_float_voltage_limit(chg, CHG_FV_DEFAULT_MV);
	if (ret)
		return ret;

	ret = unlock_prot_regs(chg, true);
	if (ret)
		return ret;

	/* Disable wireless charging input */
	regmap_update_bits(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_12,
			   CHG_CNFG_12_WCINSEL, 0);

	regmap_update_bits(chg->regmap, MAX77759_CHGR_REG_CHG_CNFG_18,
			   CHG_CNFG_18_WDTEN, 0);

	return unlock_prot_regs(chg, false);
}

static void psy_work_item(struct work_struct *work)
{
	struct max77759_charger *chg =
		container_of(work, struct max77759_charger, psy_work);
	union power_supply_propval current_limit = { 0 }, online = { 0 };
	int ret;

	power_supply_get_property(chg->tcpm_psy, POWER_SUPPLY_PROP_CURRENT_MAX,
				  &current_limit);
	power_supply_get_property(chg->tcpm_psy, POWER_SUPPLY_PROP_ONLINE,
				  &online);

	if (online.intval && current_limit.intval) {
		ret = set_input_current_limit(chg, current_limit.intval);
		if (ret)
			dev_err(chg->dev,
				"Unable to set current limit, ret=%d", ret);

		charger_set_mode(chg, CHG_MODE_CHG_BUCK_ON);
	} else {
		charger_set_mode(chg, CHG_MODE_OFF);
	}
}

static int psy_changed(struct notifier_block *nb, unsigned long evt, void *data)
{
	struct max77759_charger *chg = container_of(nb, struct max77759_charger,
						    nb);
	const char *psy_name = "tcpm-source";
	struct power_supply *psy = data;

	if (!strnstr(psy->desc->name, psy_name, strlen(psy_name)) ||
	    evt != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	chg->tcpm_psy = psy;
	schedule_work(&chg->psy_work);
	return NOTIFY_OK;
}

static void max_tcpci_unregister_psy_notifier(void *nb)
{
	power_supply_unreg_notifier(nb);
}

static int max77759_charger_probe(struct platform_device *pdev)
{
	struct regulator_config usb_otg_reg_cfg;
	struct power_supply_config psy_cfg;
	struct device *dev = &pdev->dev;
	struct max77759_charger *chg;
	int ret;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	platform_set_drvdata(pdev, chg);
	chg->dev = dev;
	chg->regmap = dev_get_regmap(dev->parent, "charger");
	if (!chg->regmap)
		return dev_err_probe(dev, -ENODEV, "Missing regmap");

	ret = devm_mutex_init(dev, &chg->lock);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize lock");

	ret = max77759_charger_init(chg);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to initialize max77759 charger");

	usb_otg_reg_cfg.dev = dev;
	usb_otg_reg_cfg.driver_data = chg;
	usb_otg_reg_cfg.of_node = dev_of_node(dev);
	chg->usb_otg_rdev = devm_regulator_register(dev, &usb_otg_reg_desc,
						    &usb_otg_reg_cfg);
	if (IS_ERR(chg->usb_otg_rdev))
		return dev_err_probe(dev, PTR_ERR(chg->usb_otg_rdev),
				     "Failed to register usb otg regulator");

	psy_cfg.fwnode = dev_fwnode(dev);
	psy_cfg.drv_data = chg;
	chg->psy = devm_power_supply_register(dev, &max77759_charger_desc,
					      &psy_cfg);
	if (IS_ERR(chg->psy))
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "Failed to register psy, ret=%ld",
				     PTR_ERR(chg->psy));

	ret = devm_work_autocancel(dev, &chg->psy_work, psy_work_item);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize psy work");

	chg->nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&chg->nb);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to register psy notifier");

	ret = devm_add_action_or_reset(dev, max_tcpci_unregister_psy_notifier,
				       &chg->nb);
	if (ret)
		return ret;

	ret = max77759_init_irqhandler(chg);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to initialize irq handler");
	return 0;
}

static const struct of_device_id max77759_charger_ids[] = {
	{ .compatible = "maxim,max77759-charger", },
	{ },
};
MODULE_DEVICE_TABLE(of, max77759_charger_ids);

static struct platform_driver max77759_charger_driver = {
	.driver = {
		.name = "max77759-charger",
		.of_match_table = max77759_charger_ids,
	},
	.probe = max77759_charger_probe,
};
module_platform_driver(max77759_charger_driver);

MODULE_AUTHOR("Amit Sunil Dhamne <amitsd@google.com>");
MODULE_DESCRIPTION("Maxim MAX77759 charger driver");
MODULE_LICENSE("GPL");
