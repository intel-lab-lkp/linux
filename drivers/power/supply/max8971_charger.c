// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/devm-helpers.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/types.h>

#define MAX8971_REG_CHGINT		0x0f
#define   MAX8971_REG_CHG_RST		BIT(0)
#define MAX8971_REG_CHGINT_MASK		0x01
#define   MAX8971_AICL_MASK		BIT(7)
#define MAX8971_REG_CHG_STAT		0x02
#define   MAX8971_CHG_MASK		BIT(3)
#define MAX8971_REG_DETAILS1		0x03
#define MAX8971_REG_DETAILS2		0x04
#define MAX8971_REG_CHGCNTL1		0x05
#define MAX8971_REG_FCHGCRNT		0x06
#define MAX8971_REG_DCCRNT		0x07
#define   MAX8971_CHGRSTRT_MASK		BIT(6)
#define   MAX8971_CHGRSTRT_SHIFT	6
#define MAX8971_REG_TOPOFF		0x08
#define MAX8971_REG_TEMPREG		0x09
#define MAX8971_REG_PROTCMD		0x0a
#define   MAX8971_CHGPROT_LOCKED	0x00
#define   MAX8971_CHGPROT_UNLOCKED	0x03

#define MAX8971_CHGCC_DCILMT_DEFAULT	500
#define MAX8971_FCHGT_DEFAULT		2
#define MAX8971_TOPOFFT_DEFAULT		3

static const char *max8971_manufacturer	= "Maxim Integrated";
static const char *max8971_model	= "MAX8971";

enum max8971_charging_state {
	MAX8971_CHARGING_DEAD_BATTERY,
	MAX8971_CHARGING_PREQUALIFICATION,
	MAX8971_CHARGING_FAST_CONST_CURRENT,
	MAX8971_CHARGING_FAST_CONST_VOLTAGE,
	MAX8971_CHARGING_TOP_OFF,
	MAX8971_CHARGING_DONE,
	MAX8971_CHARGING_TIMER_FAULT,
	MAX8971_CHARGING_SUSPENDED_THERMAL,
	MAX8971_CHARGING_OFF,
	MAX8971_CHARGING_THERMAL_LOOP,
};

enum max8971_health_state {
	MAX8971_HEALTH_UNKNOWN,
	MAX8971_HEALTH_COLD,
	MAX8971_HEALTH_COOL,
	MAX8971_HEALTH_WARM,
	MAX8971_HEALTH_HOT,
	MAX8971_HEALTH_OVERHEAT,
};

/* Fast-Charge current limit, 250..1550 mA, 50 mA steps */
#define MAX8971_CHG_CC_STEP			  50000U
#define MAX8971_CHG_CC_MIN			 250000U
#define MAX8971_CHG_CC_MAX			1550000U

/* Input current limit, 250..1500 mA, 25 mA steps */
#define MAX8971_DCILMT_STEP			  25000U
#define MAX8971_DCILMT_MIN			 250000U
#define MAX8971_DCILMT_MAX			1500000U

enum max8971_field_idx {
	THM_DTLS,		/* DETAILS1 */
	BAT_DTLS, CHG_DTLS,	/* DETAILS2 */
	CHG_CC, FCHG_T,		/* FCHGCRNT */
	DCI_LMT,		/* DCCRNT */
	TOPOFF_T, CHG_CV,	/* TOPOFF */
	CPROT,			/* PROTCMD */
	MAX8971_N_REGMAP_FIELDS
};

static const struct reg_field max8971_reg_field[MAX8971_N_REGMAP_FIELDS] = {
	[THM_DTLS] = REG_FIELD(MAX8971_REG_DETAILS1, 0, 2),
	[BAT_DTLS] = REG_FIELD(MAX8971_REG_DETAILS2, 4, 5),
	[CHG_DTLS] = REG_FIELD(MAX8971_REG_DETAILS2, 0, 3),
	[CHG_CC]   = REG_FIELD(MAX8971_REG_FCHGCRNT, 0, 4),
	[FCHG_T]   = REG_FIELD(MAX8971_REG_FCHGCRNT, 5, 7),
	[DCI_LMT]  = REG_FIELD(MAX8971_REG_DCCRNT,   0, 5),
	[TOPOFF_T] = REG_FIELD(MAX8971_REG_TOPOFF,   5, 7),
	[CHG_CV]   = REG_FIELD(MAX8971_REG_TOPOFF,   2, 3),
	[CPROT]    = REG_FIELD(MAX8971_REG_PROTCMD,  2, 3),
};

static const struct regmap_config max8971_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX8971_REG_CHGINT,
};

struct max8971_config {
	u32 chgcc;
	u32 fchgt;
	u32 chgrstrt;
	u32 dcilmt;
	u32 tofft;
	u32 toffs;
};

struct max8971_data {
	struct i2c_client *client;
	struct device *dev;
	struct power_supply *psy_mains;

	struct extcon_dev *edev;
	struct notifier_block extcon_nb;
	struct delayed_work extcon_work;

	struct regmap *regmap;
	struct regmap_field *rfield[MAX8971_N_REGMAP_FIELDS];

	struct max8971_config config;
	enum power_supply_usb_type usb_type;

	u32 chgcc_usb;
	u32 chgcc_ac;
	u32 dcilmt_usb;
	u32 dcilmt_ac;

	bool present;
};

static int max8971_get_status(struct max8971_data *priv, int *val)
{
	u32 regval;
	int err;

	err = regmap_field_read(priv->rfield[CHG_DTLS], &regval);
	if (err)
		return err;

	switch (regval) {
	case MAX8971_CHARGING_DEAD_BATTERY:
	case MAX8971_CHARGING_PREQUALIFICATION:
	case MAX8971_CHARGING_FAST_CONST_CURRENT:
	case MAX8971_CHARGING_FAST_CONST_VOLTAGE:
	case MAX8971_CHARGING_TOP_OFF:
	case MAX8971_CHARGING_THERMAL_LOOP:
		*val = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case MAX8971_CHARGING_DONE:
		*val = POWER_SUPPLY_STATUS_FULL;
		break;
	case MAX8971_CHARGING_TIMER_FAULT:
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case MAX8971_CHARGING_OFF:
	case MAX8971_CHARGING_SUSPENDED_THERMAL:
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	default:
		*val = POWER_SUPPLY_STATUS_UNKNOWN;
	}

	return 0;
}

static int max8971_get_charge_type(struct max8971_data *priv, int *val)
{
	u32 regval;
	int err;

	err = regmap_field_read(priv->rfield[CHG_DTLS], &regval);
	if (err)
		return err;

	switch (regval) {
	case MAX8971_CHARGING_DEAD_BATTERY:
	case MAX8971_CHARGING_PREQUALIFICATION:
		*val = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case MAX8971_CHARGING_FAST_CONST_CURRENT:
	case MAX8971_CHARGING_FAST_CONST_VOLTAGE:
		*val = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case MAX8971_CHARGING_TOP_OFF:
	case MAX8971_CHARGING_THERMAL_LOOP:
		*val = POWER_SUPPLY_CHARGE_TYPE_STANDARD;
		break;
	case MAX8971_CHARGING_DONE:
	case MAX8971_CHARGING_TIMER_FAULT:
	case MAX8971_CHARGING_SUSPENDED_THERMAL:
	case MAX8971_CHARGING_OFF:
		*val = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	default:
		*val = POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
	}

	return 0;
}

static int max8971_get_health(struct max8971_data *priv, int *val)
{
	u32 regval;
	int err;

	err = regmap_field_read(priv->rfield[THM_DTLS], &regval);
	if (err)
		return err;

	switch (regval) {
	case MAX8971_HEALTH_COLD:
		*val = POWER_SUPPLY_HEALTH_COLD;
		break;
	case MAX8971_HEALTH_COOL:
		*val = POWER_SUPPLY_HEALTH_COOL;
		break;
	case MAX8971_HEALTH_WARM:
		*val = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case MAX8971_HEALTH_HOT:
		*val = POWER_SUPPLY_HEALTH_HOT;
		break;
	case MAX8971_HEALTH_OVERHEAT:
		*val = POWER_SUPPLY_HEALTH_OVERHEAT;
		break;
	case MAX8971_HEALTH_UNKNOWN:
	default:
		*val = POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	return 0;
}

static int max8971_get_online(struct max8971_data *priv, int *val)
{
	u32 regval;
	int err;

	err = regmap_read(priv->regmap, MAX8971_REG_CHG_STAT, &regval);
	if (err)
		return err;

	if (priv->present)
		/* CHG_OK bit is 0 when charger is online */
		*val = !(regval & MAX8971_CHG_MASK);
	else
		*val = priv->present;

	return 0;
}

static int max8971_get_integer(struct max8971_data *priv, enum max8971_field_idx fidx,
			       u32 clamp_min, u32 clamp_max, u32 mult, int *val)
{
	u32 regval;
	int err;

	err = regmap_field_read(priv->rfield[fidx], &regval);
	if (err)
		return err;

	*val = clamp_val(regval * mult, clamp_min, clamp_max);

	return 0;
}

static int max8971_set_integer(struct max8971_data *priv, enum max8971_field_idx fidx,
			       u32 clamp_min, u32 clamp_max, u32 div, int val)
{
	u32 regval;

	regval = clamp_val(val, clamp_min, clamp_max) / div;

	return regmap_field_write(priv->rfield[fidx], regval);
}

static int max8971_get_property(struct power_supply *psy, enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct max8971_data *priv = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = max8971_get_status(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = max8971_get_charge_type(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = priv->usb_type;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = max8971_get_health(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = max8971_get_online(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = priv->present;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		val->intval = MAX8971_CHG_CC_MAX;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		ret = max8971_get_integer(priv, CHG_CC, MAX8971_CHG_CC_MIN, MAX8971_CHG_CC_MAX,
					  MAX8971_CHG_CC_STEP, &val->intval);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = max8971_get_integer(priv, DCI_LMT, MAX8971_DCILMT_MIN, MAX8971_DCILMT_MAX,
					  MAX8971_DCILMT_STEP, &val->intval);
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = max8971_model;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = max8971_manufacturer;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int max8971_set_property(struct power_supply *psy, enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct max8971_data *priv = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		ret = max8971_set_integer(priv, CHG_CC, MAX8971_CHG_CC_MIN, MAX8971_CHG_CC_MAX,
					  MAX8971_CHG_CC_STEP, val->intval);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = max8971_set_integer(priv, DCI_LMT, MAX8971_DCILMT_MIN, MAX8971_DCILMT_MAX,
					  MAX8971_DCILMT_STEP, val->intval);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
};

static int max8971_property_is_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return true;
	default:
		return false;
	}
}

static enum power_supply_property max8971_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static const struct power_supply_desc max8971_charger_desc = {
	.name = "charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA),
	.properties = max8971_properties,
	.num_properties = ARRAY_SIZE(max8971_properties),
	.get_property = max8971_get_property,
	.set_property = max8971_set_property,
	.property_is_writeable = max8971_property_is_writeable,
};

static void max8971_extcon_evt_worker(struct work_struct *work)
{
	struct max8971_data *priv =
		container_of(work, struct max8971_data, extcon_work.work);
	struct device *dev = priv->dev;
	struct extcon_dev *edev = priv->edev;
	u32 chgcc, dcilmt;

	if (extcon_get_state(edev, EXTCON_CHG_USB_SDP) > 0) {
		dev_dbg(dev, "USB SDP charger is connected\n");
		priv->usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		chgcc = priv->chgcc_usb;
		dcilmt = priv->dcilmt_usb;
	} else if (extcon_get_state(edev, EXTCON_USB) > 0) {
		dev_dbg(dev, "USB charger is connected\n");
		priv->usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		chgcc = priv->chgcc_usb;
		dcilmt = priv->dcilmt_usb;
	} else if (extcon_get_state(edev, EXTCON_DISP_MHL) > 0) {
		dev_dbg(dev, "MHL plug is connected\n");
		priv->usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		chgcc = priv->chgcc_usb;
		dcilmt = priv->dcilmt_usb;
	} else if (extcon_get_state(edev, EXTCON_CHG_USB_DCP) > 0) {
		dev_dbg(dev, "USB DCP charger is connected\n");
		priv->usb_type = POWER_SUPPLY_USB_TYPE_DCP;
		chgcc = priv->chgcc_ac;
		dcilmt = priv->dcilmt_ac;
	} else if (extcon_get_state(edev, EXTCON_CHG_USB_FAST) > 0) {
		dev_dbg(dev, "USB FAST charger is connected\n");
		priv->usb_type = POWER_SUPPLY_USB_TYPE_ACA;
		chgcc = priv->chgcc_ac;
		dcilmt = priv->dcilmt_ac;
	} else if (extcon_get_state(edev, EXTCON_CHG_USB_SLOW) > 0) {
		dev_dbg(dev, "USB SLOW charger is connected\n");
		priv->usb_type = POWER_SUPPLY_USB_TYPE_ACA;
		chgcc = priv->chgcc_ac;
		dcilmt = priv->dcilmt_ac;
	} else if (extcon_get_state(edev, EXTCON_CHG_USB_CDP) > 0) {
		dev_dbg(dev, "USB CDP charger is connected\n");
		priv->usb_type = POWER_SUPPLY_USB_TYPE_CDP;
		chgcc = priv->chgcc_ac;
		dcilmt = priv->dcilmt_ac;
	} else {
		dev_dbg(dev, "USB state is unknown\n");
		priv->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return;
	}

	regmap_field_write(priv->rfield[CPROT], MAX8971_CHGPROT_UNLOCKED);

	max8971_set_integer(priv, CHG_CC, MAX8971_CHG_CC_MIN, MAX8971_CHG_CC_MAX,
			    MAX8971_CHG_CC_STEP, chgcc);
	max8971_set_integer(priv, DCI_LMT, MAX8971_DCILMT_MIN, MAX8971_DCILMT_MAX,
			    MAX8971_DCILMT_STEP, dcilmt);

	regmap_field_write(priv->rfield[CPROT], MAX8971_CHGPROT_LOCKED);
}

static int extcon_get_charger_type(struct notifier_block *nb,
				   unsigned long state, void *data)
{
	struct max8971_data *priv =
		container_of(nb, struct max8971_data, extcon_nb);
	schedule_delayed_work(&priv->extcon_work, 0);
	return NOTIFY_OK;
}

static void max8971_update_config(struct max8971_data *priv)
{
	struct max8971_config *config = &priv->config;

	regmap_field_write(priv->rfield[CPROT], MAX8971_CHGPROT_UNLOCKED);

	/* Construct Fast-Charge Current and Timer Control register */
	if (config->chgcc != MAX8971_CHGCC_DCILMT_DEFAULT)
		max8971_set_integer(priv, CHG_CC, MAX8971_CHG_CC_MIN, MAX8971_CHG_CC_MAX,
				    MAX8971_CHG_CC_STEP, config->chgcc);

	if (config->fchgt != MAX8971_FCHGT_DEFAULT)
		regmap_field_write(priv->rfield[FCHG_T], config->fchgt);

	/* Construct Input-Current Limit and Charger Restart Threshold register */
	if (config->chgrstrt)
		regmap_write_bits(priv->regmap, MAX8971_REG_DCCRNT, MAX8971_CHGRSTRT_MASK,
				  config->chgrstrt << MAX8971_CHGRSTRT_SHIFT);

	if (config->dcilmt != MAX8971_CHGCC_DCILMT_DEFAULT)
		max8971_set_integer(priv, DCI_LMT, MAX8971_DCILMT_MIN, MAX8971_DCILMT_MAX,
				    MAX8971_DCILMT_STEP, config->dcilmt);

	/* Construct Done Current, Timer, and Battery Regulation Voltage register */
	if (config->tofft != MAX8971_TOPOFFT_DEFAULT)
		regmap_field_write(priv->rfield[TOPOFF_T], config->tofft);

	if (config->toffs)
		regmap_field_write(priv->rfield[CHG_CV], config->toffs);

	regmap_field_write(priv->rfield[CPROT], MAX8971_CHGPROT_LOCKED);
}

static irqreturn_t max8971_interrupt(int irq, void *dev_id)
{
	struct max8971_data *priv = dev_id;
	struct device *dev = priv->dev;
	int ret, state;

	ret = regmap_read(priv->regmap, MAX8971_REG_CHGINT, &state);
	if (ret)
		dev_err(dev, "interrupt reg read failed %d\n", ret);

	ret = regmap_write_bits(priv->regmap, MAX8971_REG_CHGINT_MASK,
				MAX8971_AICL_MASK, MAX8971_AICL_MASK);
	if (ret)
		dev_err(dev, "failed to mask IRQ\n");

	/* set presence prop */
	priv->present = state & MAX8971_REG_CHG_RST;

	/* on every plug chip resets to default */
	if (priv->present)
		max8971_update_config(priv);

	/* update supply status */
	power_supply_changed(priv->psy_mains);

	return IRQ_HANDLED;
}

static void max8971_dt_init(struct max8971_data *priv)
{
	struct device *dev = priv->dev;
	struct max8971_config *config = &priv->config;

	/* Construct Fast-Charge Current and Timer Control register */
	config->chgcc = 500 * 1000;
	device_property_read_u32(dev, "maxim,fcharge-current-limit-microamp", &config->chgcc);

	config->fchgt = 5;
	device_property_read_u32(dev, "maxim,fcharge-timer-hours", &config->fchgt);
	config->fchgt -= 3;
	if (config->fchgt < 0 || config->fchgt > 7)
		config->fchgt = 0;

	/* Construct Input-Current Limit and Charger Restart Threshold register */
	config->chgrstrt = device_property_read_bool(dev, "maxim,fcharge-rst-threshold-high");

	config->dcilmt = 500 * 1000;
	device_property_read_u32(dev, "maxim,in-current-limit-microamp", &config->dcilmt);

	/* Construct Done Current, Timer, and Battery Regulation Voltage register */
	config->tofft = 30;
	device_property_read_u32(dev, "maxim,topoff-timer-minutes", &config->tofft);
	config->tofft /= 10;
	if (config->tofft > 7)
		config->tofft = 7;

	config->toffs = 50 * 1000;
	device_property_read_u32(dev, "maxim,topoff-current-threshold-microamp", &config->toffs);
	config->toffs = (config->toffs / 1000 - 50) / 50;

	/* Perform initial setup */
	max8971_update_config(priv);

	/* Get additional variables */
	priv->chgcc_usb = 500 * 1000;
	device_property_read_u32(dev, "maxim,fcharge-usb-current-limit-microamp", &priv->chgcc_usb);

	priv->chgcc_ac = 500 * 1000;
	device_property_read_u32(dev, "maxim,fcharge-ac-current-limit-microamp", &priv->chgcc_ac);

	priv->dcilmt_usb = 500 * 1000;
	device_property_read_u32(dev, "maxim,usb-in-current-limit-microamp", &priv->dcilmt_usb);

	priv->dcilmt_ac = 500 * 1000;
	device_property_read_u32(dev, "maxim,ac-in-current-limit-microamp", &priv->dcilmt_ac);
}

static char *max8971_supplied_to[] = {
	"battery",
};

static int max8971_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct max8971_data *priv;
	struct power_supply_config cfg = { };
	int ret, i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->dev = dev;
	priv->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;

	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &max8971_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap), "cannot allocate regmap\n");

	for (i = 0; i < MAX8971_N_REGMAP_FIELDS; i++) {
		priv->rfield[i] = devm_regmap_field_alloc(dev, priv->regmap, max8971_reg_field[i]);
		if (IS_ERR(priv->rfield[i]))
			return dev_err_probe(dev, PTR_ERR(priv->rfield[i]),
					     "cannot allocate regmap field\n");
	}

	max8971_dt_init(priv);

	cfg.of_node = dev->of_node;
	cfg.drv_data = priv;
	cfg.supplied_to = max8971_supplied_to;
	cfg.num_supplicants = ARRAY_SIZE(max8971_supplied_to);

	priv->psy_mains = devm_power_supply_register(dev, &max8971_charger_desc, &cfg);
	if (IS_ERR(priv->psy_mains))
		return dev_err_probe(dev, PTR_ERR(priv->psy_mains),
				     "failed to register mains supply\n");

	ret = regmap_write_bits(priv->regmap, MAX8971_REG_CHGINT_MASK, MAX8971_AICL_MASK,
				MAX8971_AICL_MASK);
	if (ret)
		return dev_err_probe(dev, ret, "failed to mask IRQ\n");

	ret = devm_request_threaded_irq(dev, client->irq, NULL, &max8971_interrupt,
					IRQF_ONESHOT | IRQF_SHARED, client->name, priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register IRQ %d\n", client->irq);

	if (device_property_present(dev, "extcon")) {
		priv->edev = extcon_get_edev_by_phandle(dev, 0);
		if (IS_ERR(priv->edev))
			return dev_err_probe(dev, PTR_ERR(priv->edev),
					     "failed to register extcon\n");

		ret = devm_delayed_work_autocancel(dev, &priv->extcon_work,
						   max8971_extcon_evt_worker);
		if (ret)
			return dev_err_probe(dev, ret, "failed to add extcon evt stop action\n");

		priv->extcon_nb.notifier_call = extcon_get_charger_type;

		ret = devm_extcon_register_notifier_all(dev, priv->edev, &priv->extcon_nb);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register notifier\n");

		/* Initial configuration work with 1 sec delay */
		schedule_delayed_work(&priv->extcon_work, msecs_to_jiffies(1000));
	}

	return 0;
}

static int max8971_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct max8971_data *priv = i2c_get_clientdata(client);

	irq_wake_thread(client->irq, priv);

	return 0;
}

static SIMPLE_DEV_PM_OPS(max8971_pm_ops, NULL, max8971_resume);

static const struct of_device_id max8971_match_ids[] = {
	{ .compatible = "maxim,max8971" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, max8971_match_ids);

static const struct i2c_device_id max8971_i2c_id[] = {
	{ "max8971" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max8971_i2c_id);

static struct i2c_driver max8971_driver = {
	.driver = {
		.name = "max8971-charger",
		.of_match_table = max8971_match_ids,
		.pm = &max8971_pm_ops,
	},
	.probe = max8971_probe,
	.id_table = max8971_i2c_id,
};
module_i2c_driver(max8971_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("MAX8971 Charger Driver");
MODULE_LICENSE("GPL");
