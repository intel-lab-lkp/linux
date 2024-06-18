// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  max77705_fuelgauge.c
 *  Samsung max77705 Fuel Gauge Driver
 *
 *  Copyright (C) 2015 Samsung Electronics
 *  Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.com>
 */

#include <linux/of_gpio.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/power/max77705_fuelgauge.h>
#include <linux/mfd/max77705-private.h>

static const char *max77705_fuelgauge_model		= "max77705";
static const char *max77705_fuelgauge_manufacturer	= "Maxim Integrated";
static struct dentry *debugfs_file;

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

static int max77705_fg_read_vcell(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	u16 w_data;

	if (regmap_noinc_read(regmap, VCELL_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read VCELL_REG\n", __func__);
		return -1;
	}

	w_data = (data[1] << 8) | data[0];

	return max77705_fg_vs_convert(w_data);
}

static int max77705_fg_read_vfocv(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	u16 w_data;

	if (regmap_noinc_read(regmap, VFOCV_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read VFOCV_REG\n", __func__);
		return -1;
	}

	w_data = (data[1] << 8) | data[0];

	return max77705_fg_vs_convert(w_data);
}

static int max77705_fg_read_avg_vcell(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	u16 w_data;

	if (regmap_noinc_read(regmap, AVR_VCELL_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read AVR_VCELL_REG\n", __func__);
		return -1;
	}

	w_data = (data[1] << 8) | data[0];
	return max77705_fg_vs_convert(w_data);
}

static int max77705_fg_check_battery_present(struct max77705_fuelgauge_data
					     *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 status_data[2];

	if (regmap_noinc_read(regmap, STATUS_REG, status_data, sizeof(status_data)) < 0) {
		dev_err(fuelgauge->dev, "Failed to read STATUS_REG\n");
		return 0;
	}

	return !(status_data[0] & MAX77705_BAT_ABSENT_MASK);
}

static int max77705_fg_read_temp(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2] = { 0, 0 };
	int temper = 0;

	if (regmap_noinc_read(regmap, TEMPERATURE_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read TEMPERATURE_REG\n", __func__);
		return -1;
	}

	if (data[1] & BIT(7))
		temper = ((~(data[1])) & 0xFF) + 1;
	else
		temper = data[1] & 0x7f;

	temper *= 10;
	temper += data[0] * 10 / 256;

	return temper;
}

static int max77705_fg_read_socrep(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	int soc;

	if (regmap_noinc_read(regmap, SOCREP_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read SOCREP_REG\n", __func__);
		return -1;
	}

	soc = data[1];

	return min(soc, 100);
}

static int max77705_fg_read_fullcaprep(struct max77705_fuelgauge_data
				       *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	int ret;

	if (regmap_noinc_read(regmap, FULLCAP_REP_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read FULLCAP_REP_REG\n", __func__);
		return -1;
	}

	ret = (data[1] << 8) + data[0];

	return ret * 1000;
}

static int max77705_fg_read_repcap(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	int ret;

	if (regmap_noinc_read(regmap, REMCAP_REP_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read REMCAP_REP_REG\n", __func__);
		return -1;
	}

	ret = (data[1] << 8) + data[0];

	return ret * 1000;
}

static int max77705_fg_read_charge_design(struct max77705_fuelgauge_data
				       *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	int reg;

	if (regmap_noinc_read(regmap, DESIGNCAP_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read DESIGNCAP_REG\n", __func__);
		return -1;
	}

	reg = (data[1] << 8) | (data[0]);

	return reg * 1000;
}

static int max77705_fg_set_charge_design(struct regmap *regmap, int value)
{
	u8 data[2];
	int value_mah;

	value_mah = value / 1000;
	data[0] = value_mah & 0xFF;
	data[1] = (value_mah >> 8) & 0xFF;

	if (regmap_noinc_write(regmap, DESIGNCAP_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to write DESIGNCAP_REG\n", __func__);
		return -1;
	}

	return 0;
}

static int max77705_fg_read_current(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 reg_data[2];
	s16 reg_value;
	s32 i_current;

	if (regmap_noinc_read(regmap, CURRENT_REG, reg_data, sizeof(reg_data)) < 0) {
		pr_err("%s: Failed to read CURRENT\n", __func__);
		return -1;
	}

	reg_value = ((reg_data[1] << 8) | reg_data[0]);
	i_current = max77705_fg_cs_convert(reg_value, fuelgauge->rsense_conductance);

	return i_current;
}

static int max77705_fg_read_avg_current(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 reg_data[2];
	u32 reg_value;
	s32 avg_current;

	if (regmap_noinc_read(regmap, AVG_CURRENT_REG, reg_data, sizeof(reg_data)) < 0) {
		pr_err("%s: Failed to read AVG_CURRENT_REG\n", __func__);
		return -1;
	}

	reg_value = ((reg_data[1] << 8) | reg_data[0]);
	avg_current = max77705_fg_cs_convert(reg_value, fuelgauge->rsense_conductance);

	return avg_current;
}

static int max77705_fg_read_tte(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	int reg;

	if (regmap_noinc_read(regmap, TIME_TO_EMPTY_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read TIME_TO_EMPTY_REG\n", __func__);
		return -1;
	}

	reg = (data[1] << 8) | (data[0]);

	return reg;
}

static int max77705_fg_read_ttf(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	int reg;

	if (regmap_noinc_read(regmap, TIME_TO_FULL_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read TIME_TO_FULL_REG\n", __func__);
		return -1;
	}

	reg = (data[1] << 8) | (data[0]);

	return reg;
}

static int max77705_fg_read_cycle(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2];
	int ret;

	if (regmap_noinc_read(regmap, CYCLES_REG, data, sizeof(data)) < 0) {
		pr_err("%s: Failed to read CYCLES_REG\n", __func__);
		return -1;
	}

	ret = (data[1] << 8) + data[0];

	return ret;
}

static int max77705_battery_get_status(struct max77705_fuelgauge_data *fuelgauge)
{
	int current_now;
	int am_i_supplied;

	am_i_supplied = power_supply_am_i_supplied(fuelgauge->psy_fg);
	if (am_i_supplied) {
		if (am_i_supplied == -ENODEV)
			dev_err(fuelgauge->dev,
				"power supply not found, fall back to current-based status checking\n");
		else
			return POWER_SUPPLY_STATUS_CHARGING;
	}
	if (max77705_fg_read_socrep(fuelgauge) < 100) {
		current_now = max77705_fg_read_current(fuelgauge);
		if (current_now > 0)
			return POWER_SUPPLY_STATUS_CHARGING;
		else if (current_now < 0)
			return POWER_SUPPLY_STATUS_DISCHARGING;
		else
			return POWER_SUPPLY_STATUS_NOT_CHARGING;
	} else {
		return POWER_SUPPLY_STATUS_FULL;
	}
	return POWER_SUPPLY_STATUS_DISCHARGING;
}

static bool max77705_fg_init(struct max77705_fuelgauge_data *fuelgauge)
{
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	u8 data[2] = { 0, 0 };
	u32 volt_threshold = 0;
	u32 temp_threshold = 0;

	if (fuelgauge->auto_discharge_en) {
		/* Auto discharge EN & Alert Enable */
		regmap_noinc_read(regmap, CONFIG2_REG, data, sizeof(data));
		data[1] |= MAX77705_AUTO_DISCHARGE_EN_MASK >> 8;
		regmap_noinc_write(regmap, CONFIG2_REG, data, sizeof(data));

		/* Set Auto Discharge temperature & Voltage threshold */
		volt_threshold =
		    fuelgauge->discharge_volt_threshold < 3900 ? 0x0 :
		    fuelgauge->discharge_volt_threshold > 4540 ? 0x20 :
		    (fuelgauge->discharge_volt_threshold - 3900) / 20;

		temp_threshold =
		    fuelgauge->discharge_temp_threshold < 470 ? 0x0 :
		    fuelgauge->discharge_temp_threshold > 630 ? 0x20 :
		    (fuelgauge->discharge_temp_threshold - 470) / 5;

		regmap_noinc_read(regmap, DISCHARGE_THRESHOLD_REG, data, sizeof(data));
		data[1] &= ~MAX77705_AUTO_DISCHARGE_VALUE_MASK;
		data[1] |= volt_threshold << MAX77705_AUTO_DISCHARGE_VALUE_SHIFT;

		data[0] &= ~MAX77705_AUTO_DISCHARGE_VALUE_MASK;
		data[0] |= temp_threshold << MAX77705_AUTO_DISCHARGE_VALUE_SHIFT;

		regmap_noinc_write(regmap, DISCHARGE_THRESHOLD_REG, data, sizeof(data));

		pr_info("%s: DISCHARGE_THRESHOLD Value : 0x%x\n",
			__func__, (data[1] << 8) | data[0]);
	}

	return true;
}

static int max77705_fg_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct max77705_fuelgauge_data *fuelgauge =
	    power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = max77705_battery_get_status(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = max77705_fg_check_battery_present(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = max77705_fg_read_vcell(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = max77705_fg_read_vfocv(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		val->intval = max77705_fg_read_avg_vcell(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = max77705_fg_read_current(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = max77705_fg_read_avg_current(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = max77705_fg_read_repcap(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = max77705_fg_read_fullcaprep(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = max77705_fg_read_charge_design(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = max77705_fg_read_socrep(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = max77705_fg_read_temp(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		val->intval = max77705_fg_read_tte(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		val->intval = max77705_fg_read_ttf(fuelgauge);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = max77705_fg_read_cycle(fuelgauge);
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
	return 0;
}

static int max77705_fuelgauge_debugfs_show(struct seq_file *s, void *data)
{
	struct max77705_fuelgauge_data *fuelgauge = s->private;
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;
	int i;
	u16 reg_data;
	int regs[] = { 0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x08, 0x09, 0x11, 0x13, 0x0A,
			0x0B, 0x0D, 0x0E, 0x0F, 0x10, 0x15, 0x16, 0x17, 0x18, 0x19, 0x20,
			0x1D, 0x1E, 0x1F, 0x23, 0x28, 0x29, 0x2B, 0x32, 0x35, 0x38, 0x3A,
			0x3D, 0x40, 0x42, 0x43, 0x45, 0x46, 0x4B, 0x4D, 0xB1, 0xB2, 0xB3,
			0xBB, 0xD0, 0xEE, 0xFB, 0xFF, };

	seq_puts(s, "MAX77705 FUELGAUGE IC :\n");
	seq_puts(s, "===================\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		regmap_noinc_read(regmap, regs[i], &reg_data, 2);
		seq_printf(s, "0x%02x:\t0x%02x\n", regs[i], reg_data);
	}
	seq_puts(s, "\n");
	return 0;
}

static int max77705_fuelgauge_debugfs_open(struct inode *inode,
					   struct file *file)
{
	return single_open(file, max77705_fuelgauge_debugfs_show, inode->i_private);
}

static const struct file_operations max77705_fuelgauge_debugfs_fops = {
	.open = max77705_fuelgauge_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void max77705_battery_settings(struct max77705_fuelgauge_data *fuelgauge)
{
	struct power_supply_battery_info *info;
	struct regmap *regmap = fuelgauge->max77705->regmap_fg;

	if (power_supply_get_battery_info(fuelgauge->psy_fg, &info) < 0)
		return;

	fuelgauge->bat_info = info;

	if (!regmap) {
		dev_warn(fuelgauge->dev, "data memory update not supported for chip\n");
		return;
	}

	if (info->energy_full_design_uwh != info->charge_full_design_uah) {
		if (info->charge_full_design_uah == -EINVAL)
			dev_warn(fuelgauge->dev, "missing battery:charge-full-design-microamp-hours\n");
		max77705_fg_set_charge_design(regmap, info->charge_full_design_uah);
	}
}

static int max77705_fuelgauge_parse_dt(struct max77705_fuelgauge_data
				       *fuelgauge)
{
	struct device *dev = fuelgauge->dev;
	struct device_node *np = dev->of_node;
	unsigned int rsense;

	if (!np) {
		dev_err(dev, "no fuelgauge OF node\n");
		return -EINVAL;
	}

	int ret;

	if (np == NULL) {
		pr_err("%s np NULL\n", __func__);
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "rsense",
				   &rsense);
	if (ret < 0) {
		pr_err("%s error reading rsense %d\n",
		       __func__, ret);
		fuelgauge->rsense_conductance = 100;
	} else
		fuelgauge->rsense_conductance = 1000 / rsense; /* rsense in Ohm^-1 */

	fuelgauge->auto_discharge_en = of_property_read_bool(np,
							     "auto_discharge_en");
	if (fuelgauge->auto_discharge_en) {
		ret = of_property_read_u32(np,
						"discharge_temp_threshold",
						&fuelgauge->discharge_temp_threshold);
		if (ret < 0) {
			dev_err(dev, "error reading rsense_conductance %d\n", ret);
			fuelgauge->discharge_temp_threshold = 600;
		}

		ret = of_property_read_u32(np,
						"discharge_volt_threshold",
						&fuelgauge->discharge_volt_threshold);
		if (ret < 0)
			fuelgauge->discharge_volt_threshold = 4200;
	}

	return 0;
}

static const struct power_supply_desc max77705_fuelgauge_power_supply_desc = {
	.name = "max77705-fuelgauge",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = max77705_fuelgauge_props,
	.num_properties = ARRAY_SIZE(max77705_fuelgauge_props),
	.get_property = max77705_fg_get_property,
};

static int max77705_fuelgauge_probe(struct platform_device *pdev)
{
	struct max77705_dev *max77705 = dev_get_drvdata(pdev->dev.parent);
	struct max77705_platform_data *pdata = dev_get_platdata(max77705->dev);
	struct max77705_fuelgauge_data *fuelgauge;
	struct power_supply_config fuelgauge_cfg = { };
	int ret = 0;

	fuelgauge = kzalloc(sizeof(*fuelgauge), GFP_KERNEL);
	if (!fuelgauge)
		return -ENOMEM;

	mutex_init(&fuelgauge->fg_lock);

	fuelgauge->dev = &pdev->dev;
	fuelgauge->max77705 = max77705;
	fuelgauge->max77705_pdata = pdata;

	ret = max77705_fuelgauge_parse_dt(fuelgauge);
	if (ret < 0)
		pr_err("%s not found charger dt! ret[%d]\n", __func__, ret);

	platform_set_drvdata(pdev, fuelgauge);


	debugfs_file = debugfs_create_file("max77705-fuelgauge-regs",
				0664, NULL, (void *)fuelgauge,
				  &max77705_fuelgauge_debugfs_fops);
	if (!debugfs_file)
		dev_err(fuelgauge->dev, "Failed to create debugfs file\n");

	fuelgauge_cfg.drv_data = fuelgauge;
	fuelgauge_cfg.of_node = fuelgauge->dev->of_node;

	fuelgauge->psy_fg =
	    devm_power_supply_register(&pdev->dev,
				  &max77705_fuelgauge_power_supply_desc,
				  &fuelgauge_cfg);

	if (IS_ERR(fuelgauge->psy_fg)) {
		pr_err("%s: Failed to Register psy_fg\n", __func__);
		goto err_data_free;
	}

	fuelgauge->fg_irq = max77705->irq_base + MAX77705_FG_IRQ_ALERT;
	pr_info("[%s]IRQ_BASE(%d) FG_IRQ(%d)\n",
		__func__, max77705->irq_base, fuelgauge->fg_irq);

	if (!max77705_fg_init(fuelgauge)) {
		pr_err("%s: Failed to Initialize Fuelgauge\n", __func__);
		goto err_supply_unreg;
	}

	max77705_battery_settings(fuelgauge);

	return 0;

err_supply_unreg:
	power_supply_unregister(fuelgauge->psy_fg);
	kfree(fuelgauge->bat_info);
err_data_free:
	mutex_destroy(&fuelgauge->fg_lock);

	return ret;
}

static void max77705_fuelgauge_remove(struct platform_device *pdev)
{
	if (debugfs_file)
		debugfs_remove(debugfs_file);
}
static const struct platform_device_id max77705_fuelgauge_id[] = {
	{ "max77705-fuelgauge", 0, },
	{ }
};
MODULE_DEVICE_TABLE(platform, max77705_fuelgauge_id);

static struct platform_driver max77705_fuelgauge_driver = {
	.driver = {
		.name = "max77705-fuelgauge",
	},
	.probe = max77705_fuelgauge_probe,
	.remove_new = max77705_fuelgauge_remove,
	.id_table	= max77705_fuelgauge_id,
};
module_platform_driver(max77705_fuelgauge_driver);

MODULE_DESCRIPTION("Samsung max77705 Fuel Gauge Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
