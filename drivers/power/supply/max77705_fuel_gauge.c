// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail>
//
// Fuel gauge driver for MAXIM 77705 charger/power-supply.

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/max77693-common.h>
#include <linux/mfd/max77705-private.h>
#include <linux/power/max77705_fuelgauge.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define I2C_ADDR_FG     0x36

static const char *max77705_fuelgauge_model		= "max77705";
static const char *max77705_fuelgauge_manufacturer	= "Maxim Integrated";

static const struct regmap_config max77705_fg_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_FG_END,
};

static enum power_supply_property max77705_fuelgauge_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
};

static int max77705_fg_read_reg(struct max77705_fuelgauge_data *fuelgauge,
				unsigned int reg, unsigned int *val)
{
	struct regmap *regmap = fuelgauge->regmap;
	u8 data[2];
	int ret;

	ret = regmap_noinc_read(regmap, reg, data, sizeof(data));
	if (ret < 0)
		return ret;

	*val = (data[1] << 8) + data[0];

	return 0;
}

static int max77705_fg_read_temp(struct max77705_fuelgauge_data *fuelgauge,
				 int *val)
{
	struct regmap *regmap = fuelgauge->regmap;
	u8 data[2] = { 0, 0 };
	int ret, temperature = 0;

	ret = regmap_noinc_read(regmap, TEMPERATURE_REG, data, sizeof(data));
	if (ret < 0)
		return ret;

	if (data[1] & BIT(7))
		temperature = ((~(data[1])) & 0xFF) + 1;
	else
		temperature = data[1] & 0x7f;

	temperature *= 10;
	temperature += data[0] * 10 / 256;
	*val = temperature;

	return 0;
}

static int max77705_fg_check_battery_present(struct max77705_fuelgauge_data
					     *fuelgauge, int *val)
{
	struct regmap *regmap = fuelgauge->regmap;
	u8 status_data[2];
	int ret;

	ret = regmap_noinc_read(regmap, STATUS_REG, status_data, sizeof(status_data));
	if (ret < 0)
		return ret;

	*val = !(status_data[0] & MAX77705_BAT_ABSENT_MASK);

	return 0;
}

static int max77705_battery_get_status(struct max77705_fuelgauge_data *fuelgauge,
					int *val)
{
	int current_now;
	int am_i_supplied;
	int ret;
	unsigned int soc_rep;

	am_i_supplied = power_supply_am_i_supplied(fuelgauge->psy_fg);
	if (am_i_supplied) {
		if (am_i_supplied == -ENODEV) {
			dev_err(fuelgauge->dev,
				"power supply not found, fall back to current-based method\n");
		} else {
			*val = POWER_SUPPLY_STATUS_CHARGING;
			return 0;
		}
	}
	ret = max77705_fg_read_reg(fuelgauge, SOCREP_REG, &soc_rep);
	if (ret)
		return ret;

	if (soc_rep < 100) {
		ret = max77705_fg_read_reg(fuelgauge, CURRENT_REG, &current_now);
		if (ret)
			return ret;

		if (current_now > 0)
			*val = POWER_SUPPLY_STATUS_CHARGING;
		else if (current_now < 0)
			*val = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
	} else {
		*val = POWER_SUPPLY_STATUS_FULL;
	}

	return 0;
}

static void max77705_unit_adjustment(struct max77705_fuelgauge_data *fuelgauge,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	const unsigned int base_unit_conversion = 1000;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		val->intval = max77705_fg_vs_convert(val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = max77705_fg_cs_convert(val->intval,
						     fuelgauge->rsense_conductance);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval *= base_unit_conversion;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = min(val->intval, 100);
		break;
	default:
		dev_dbg(fuelgauge->dev,
			"%s: no need for unit conversion %d\n", __func__, psp);
	}
}

static int max77705_fg_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct max77705_fuelgauge_data *fuelgauge =
	    power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = max77705_battery_get_status(fuelgauge, &val->intval);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = max77705_fg_check_battery_present(fuelgauge, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = max77705_fg_read_reg(fuelgauge, VCELL_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		ret = max77705_fg_read_reg(fuelgauge, VFOCV_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		ret = max77705_fg_read_reg(fuelgauge, AVR_VCELL_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = max77705_fg_read_reg(fuelgauge, CURRENT_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		ret = max77705_fg_read_reg(fuelgauge, AVG_CURRENT_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = max77705_fg_read_reg(fuelgauge, REMCAP_REP_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = max77705_fg_read_reg(fuelgauge, FULLCAP_REP_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = max77705_fg_read_reg(fuelgauge, DESIGNCAP_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = max77705_fg_read_reg(fuelgauge, SOCREP_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = max77705_fg_read_temp(fuelgauge, &val->intval);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = max77705_fg_read_reg(fuelgauge, TIME_TO_EMPTY_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		ret = max77705_fg_read_reg(fuelgauge, TIME_TO_FULL_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = max77705_fg_read_reg(fuelgauge, CYCLES_REG, &val->intval);
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = max77705_fuelgauge_model;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = max77705_fuelgauge_manufacturer;
		break;
	default:
		return -EINVAL;
	}

	if (ret)
		return ret;

	max77705_unit_adjustment(fuelgauge, psp, val);

	return 0;
}

static const struct power_supply_desc max77705_fg_desc = {
	.name = "max77705-fuel-gauge",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = max77705_fuelgauge_props,
	.num_properties = ARRAY_SIZE(max77705_fuelgauge_props),
	.get_property = max77705_fg_get_property,
};

static int max77705_fg_set_charge_design(struct regmap *regmap, int value)
{
	u8 data[2];
	int value_mah;

	value_mah = value / 1000;
	data[0] = value_mah & 0xFF;
	data[1] = (value_mah >> 8) & 0xFF;

	return regmap_noinc_write(regmap, DESIGNCAP_REG, data, sizeof(data));
}

static int max77705_write_bat_info(struct max77705_fuelgauge_data *fuelgauge)
{
	struct power_supply_battery_info *info = fuelgauge->bat_info;

	if (info->energy_full_design_uwh != info->charge_full_design_uah) {
		if (info->charge_full_design_uah == -EINVAL)
			dev_warn(fuelgauge->dev, "missing battery:charge-full-design-microamp-hours\n");
		return max77705_fg_set_charge_design(fuelgauge->regmap,
						info->charge_full_design_uah);
	}

	return 0;
}

static int max77705_fuelgauge_parse_dt(struct max77705_fuelgauge_data
				       *fuelgauge)
{
	struct device *dev = fuelgauge->dev;
	struct device_node *np = dev->of_node;
	int ret;
	unsigned int rsense;

	if (!np) {
		dev_err(dev, "no fuelgauge OF node\n");
		return -EINVAL;
	}
	ret = of_property_read_u32(np, "shunt-resistor-micro-ohms",
				   &rsense);
	if (ret < 0) {
		dev_warn(dev, "No shunt-resistor-micro-ohms property, assume default\n");
		fuelgauge->rsense_conductance = 100;
	} else
		fuelgauge->rsense_conductance = 1000000 / rsense; /* rsense conductance in Ohm^-1 */

	return 0;
}

static int max77705_fuelgauge_probe(struct platform_device *pdev)
{
	struct i2c_client *i2c_fg;
	struct max77693_dev *max77705 = dev_get_drvdata(pdev->dev.parent);
	struct max77705_fuelgauge_data *fuelgauge;
	struct power_supply_config fuelgauge_cfg = { };
	struct device *dev = &pdev->dev;
	int ret = 0;

	fuelgauge = devm_kzalloc(dev, sizeof(*fuelgauge), GFP_KERNEL);
	if (!fuelgauge)
		return -ENOMEM;

	fuelgauge->dev = dev;

	ret = max77705_fuelgauge_parse_dt(fuelgauge);
	if (ret < 0)
		return ret;

	fuelgauge_cfg.drv_data = fuelgauge;
	fuelgauge_cfg.of_node = fuelgauge->dev->of_node;

	fuelgauge->psy_fg = devm_power_supply_register(&pdev->dev,
							&max77705_fg_desc,
							&fuelgauge_cfg);

	if (IS_ERR(fuelgauge->psy_fg))
		return PTR_ERR(fuelgauge->psy_fg);

	ret = power_supply_get_battery_info(fuelgauge->psy_fg,
					&fuelgauge->bat_info);

	if (ret)
		return ret;

	i2c_fg = devm_i2c_new_dummy_device(max77705->dev, max77705->i2c->adapter,
						I2C_ADDR_FG);

	if (IS_ERR(i2c_fg))
		return PTR_ERR(i2c_fg);

	fuelgauge->regmap = devm_regmap_init_i2c(i2c_fg,
						   &max77705_fg_regmap_config);

	return max77705_write_bat_info(fuelgauge);
}

static const struct of_device_id max77705_fg_of_match[] = {
	{ .compatible = "maxim,max77705-fuel-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77705_fg_of_match);

static struct platform_driver max77705_fuelgauge_driver = {
	.driver = {
		.name = "max77705-fuel-gauge",
		.of_match_table = max77705_fg_of_match,
	},
	.probe = max77705_fuelgauge_probe,
};
module_platform_driver(max77705_fuelgauge_driver);

MODULE_DESCRIPTION("Maxim MAX77705 Fuel Gauge Driver");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail>");
MODULE_LICENSE("GPL");
