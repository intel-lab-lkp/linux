// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2020 MediaTek Inc.
// Copyright (c) 2020 BayLibre, SAS.
// Author: Chen Zhong <chen.zhong@mediatek.com>
// Author: Fabien Parent <fparent@baylibre.com>
// Author: Luca Leonardo Scorcia <l.scorcia@gmail.com>
//
// The data sheet for MT6392 regulators is spotty to say the least,
// many important registers/fields are missing and the ones that aren't
// lack crucial information. Some useful details have been retrieved from
// Android sources.
// The driver code is mostly based on the MT6397 one.

#include <linux/module.h>
#include <linux/linear_range.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6392/registers.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/mt6392-regulator.h>
#include <linux/regulator/of_regulator.h>
#include <dt-bindings/regulator/mediatek,mt6392-regulator.h>

/**
 * MT6392 regulators' information
 *
 * @desc: standard fields of regulator description.
 * @qi_status_reg: Register to query enable signal status of regulators
 * @qi_status_mask: Mask to query enable signal status of regulators (RO)
 * @vselctrl_reg: Vsel control mode selector register
 * @vselctrl_mask: Vsel control mode selector mask (RO)
 * @vsel_reg_mode_reg: Vsel register when Vsel control mode selector = 0 (Register mode)
 * @vsel_reg_mode_mask: Vsel register mask in Register mode (RW)
 * @vsel_normal_mode_reg: Vsel register when Vsel control mode selector = 1 (Normal mode)
 * @vsel_normal_mode_mask: Vsel register mask in Register mode (RW)
 * @pwm_modeset_reg: Register to control buck mode (Auto/Force PWM)
 * @pwm_modeset_mask: Mask to control buck mode (RW)
 * @lp_modeget_reg: Register to get LDO low-power mode
 * @lp_modeget_mask: Mask to get LDO low-power mode (RO)
 * @lp_modeset_reg: Register to control LDO low-power mode
 * @lp_modeset_mask: Mask to control LDO low-power mode (WO)
 */
struct mt6392_regulator_info {
	struct regulator_desc desc;
	u32 qi_status_reg;
	u32 qi_status_mask;
	u32 vselctrl_reg;
	u32 vselctrl_mask;
	u32 vsel_reg_mode_reg;
	u32 vsel_reg_mode_mask;
	u32 vsel_normal_mode_reg;
	u32 vsel_normal_mode_mask;
	u32 pwm_modeset_reg;
	u32 pwm_modeset_mask;
	u32 lp_modeget_reg;
	u32 lp_modeget_mask;
	u32 lp_modeset_reg;
	u32 lp_modeset_mask;
};

#define MT6392_BUCK(match, vreg, supply, min, max, step, volt_ranges,	\
	_qi_status_reg, _qi_status_mask, _enable_reg, _enable_mask,	\
	_vselctrl_reg, _vselctrl_mask,					\
	_vsel_reg_mode_reg, _vsel_reg_mode_mask,			\
	_vsel_normal_mode_reg, _vsel_normal_mode_mask,			\
	_pwm_modeset_reg, _pwm_modeset_mask, _ramp_delay)		\
[MT6392_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.supply_name = supply,					\
		.of_match = of_match_ptr(match),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.ops = &mt6392_volt_range_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6392_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = ((max) - (min)) / (step) + 1,		\
		.linear_ranges = volt_ranges,				\
		.n_linear_ranges = ARRAY_SIZE(volt_ranges),		\
		.enable_reg = _enable_reg,				\
		.enable_mask = _enable_mask,				\
		.ramp_delay = _ramp_delay,				\
	},								\
	.qi_status_reg = _qi_status_reg,				\
	.qi_status_mask = _qi_status_mask,				\
	.vselctrl_reg = _vselctrl_reg,					\
	.vselctrl_mask = _vselctrl_mask,				\
	.vsel_reg_mode_reg = _vsel_reg_mode_reg,			\
	.vsel_reg_mode_mask = _vsel_reg_mode_mask,			\
	.vsel_normal_mode_reg = _vsel_normal_mode_reg,			\
	.vsel_normal_mode_mask = _vsel_normal_mode_mask,		\
	.pwm_modeset_reg = _pwm_modeset_reg,				\
	.pwm_modeset_mask = _pwm_modeset_mask,				\
}

#define MT6392_LDO(match, vreg, supply, ldo_volt_table,			\
	_qi_status_reg, _qi_status_mask,				\
	_enable_reg, _enable_mask,					\
	_vsel_reg, _vsel_mask,						\
	_lp_modeget_reg, _lp_modeget_mask,				\
	_lp_modeset_reg, _lp_modeset_mask,				\
	_enable_time)							\
[MT6392_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.supply_name = supply,					\
		.of_match = of_match_ptr(match),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.ops = &mt6392_volt_table_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6392_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = ARRAY_SIZE(ldo_volt_table),		\
		.volt_table = ldo_volt_table,				\
		.vsel_reg = _vsel_reg,					\
		.vsel_mask = _vsel_mask,				\
		.enable_reg = _enable_reg,				\
		.enable_mask = _enable_mask,				\
		.enable_time = _enable_time,				\
	},								\
	.qi_status_reg = _qi_status_reg,				\
	.qi_status_mask = _qi_status_mask,				\
	.lp_modeget_reg = _lp_modeget_reg,				\
	.lp_modeget_mask = _lp_modeget_mask,				\
	.lp_modeset_reg = _lp_modeset_reg,				\
	.lp_modeset_mask = _lp_modeset_mask,				\
}

#define MT6392_LDO_LINEAR(match, vreg, supply, min, max, step,		\
	volt_ranges,							\
	_qi_status_reg, _qi_status_mask,				\
	_enable_reg, _enable_mask,					\
	_vsel_reg, _vsel_mask,						\
	_lp_modeget_reg, _lp_modeget_mask,				\
	_lp_modeset_reg, _lp_modeset_mask,				\
	_enable_time)							\
[MT6392_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.supply_name = supply,					\
		.of_match = of_match_ptr(match),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.ops = &mt6392_volt_ldo_range_ops,			\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6392_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = ((max) - (min)) / (step) + 1,		\
		.linear_ranges = volt_ranges,				\
		.n_linear_ranges = ARRAY_SIZE(volt_ranges),		\
		.vsel_reg = _vsel_reg,					\
		.vsel_mask = _vsel_mask,				\
		.enable_reg = _enable_reg,				\
		.enable_mask = _enable_mask,				\
		.enable_time = _enable_time,				\
	},								\
	.qi_status_reg = _qi_status_reg,				\
	.qi_status_mask = _qi_status_mask,				\
	.lp_modeget_reg = _lp_modeget_reg,				\
	.lp_modeget_mask = _lp_modeget_mask,				\
	.lp_modeset_reg = _lp_modeset_reg,				\
	.lp_modeset_mask = _lp_modeset_mask,				\
}

#define MT6392_REG_FIXED(match, vreg, supply, volt,			\
	_qi_status_reg, _qi_status_mask,				\
	_enable_reg, _enable_mask,					\
	_lp_modeget_reg, _lp_modeget_mask,				\
	_lp_modeset_reg, _lp_modeset_mask,				\
	_enable_time)							\
[MT6392_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.supply_name = supply,					\
		.of_match = of_match_ptr(match),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.ops = &mt6392_volt_fixed_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6392_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = 1,					\
		.min_uV = volt,						\
		.enable_reg = _enable_reg,				\
		.enable_mask = _enable_mask,				\
		.enable_time = _enable_time,				\
	},								\
	.qi_status_reg = _qi_status_reg,				\
	.qi_status_mask = _qi_status_mask,				\
	.lp_modeget_reg = _lp_modeget_reg,				\
	.lp_modeget_mask = _lp_modeget_mask,				\
	.lp_modeset_reg = _lp_modeset_reg,				\
	.lp_modeset_mask = _lp_modeset_mask,				\
}

#define MT6392_REG_FIXED_NO_MODE(match, vreg, supply, volt,		\
	_qi_status_reg, _qi_status_mask,				\
	_enable_reg, _enable_mask, _enable_time)			\
[MT6392_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.supply_name = supply,					\
		.of_match = of_match_ptr(match),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.ops = &mt6392_volt_fixed_no_mode_ops,			\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6392_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = 1,					\
		.min_uV = volt,						\
		.enable_reg = _enable_reg,				\
		.enable_mask = _enable_mask,				\
		.enable_time = _enable_time,				\
	},								\
	.qi_status_reg = _qi_status_reg,				\
	.qi_status_mask = _qi_status_mask,				\
}

#define MT6392_REG(match, vreg, supply, volt)				\
[MT6392_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.supply_name = supply,					\
		.of_match = of_match_ptr(match),			\
		.regulators_node = of_match_ptr("regulators"),		\
		.ops = &mt6392_volt_no_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6392_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = 1,					\
		.min_uV = volt,						\
	},								\
}

static const struct linear_range buck_volt_range1[] = {
	REGULATOR_LINEAR_RANGE(700000, 0, 0x7f, 6250),
};

static const struct linear_range buck_volt_range2[] = {
	REGULATOR_LINEAR_RANGE(1400000, 0, 0x7f, 12500),
};

static const u32 ldo_volt_table1[] = {
	1800000, 1900000, 2000000, 2200000,
};

static const struct linear_range ldo_volt_range2[] = {
	REGULATOR_LINEAR_RANGE(3300000, 0, 3, 100000),
};

static const u32 ldo_volt_table3[] = {
	1800000, 3300000,
};

static const u32 ldo_volt_table4[] = {
	3000000, 3300000,
};

static const u32 ldo_volt_table5[] = {
	1200000, 1300000, 1500000, 1800000, 2000000, 2800000, 3000000, 3300000,
};

static const u32 ldo_volt_table6[] = {
	1240000, 1390000,
};

static const u32 ldo_volt_table7[] = {
	1200000, 1300000, 1500000, 1800000,
};

static const u32 ldo_volt_table8[] = {
	1800000, 2000000,
};

static int mt6392_buck_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	int ret, val = 0;
	struct mt6392_regulator_info *info = rdev_get_drvdata(rdev);
	u32 reg_value;

	if (!info->pwm_modeset_mask) {
		dev_err(&rdev->dev, "regulator %s doesn't support set_mode\n", info->desc.name);
		return -EINVAL;
	}

	switch (mode) {
	case REGULATOR_MODE_FAST:
		val = MT6392_BUCK_MODE_FORCE_PWM;
		break;
	case REGULATOR_MODE_NORMAL:
		val = MT6392_BUCK_MODE_AUTO;
		break;
	default:
		return -EINVAL;
	}

	val <<= ffs(info->pwm_modeset_mask) - 1;

	ret = regmap_update_bits(rdev->regmap, info->pwm_modeset_reg,
				 info->pwm_modeset_mask, val);

	if (regmap_read(rdev->regmap, info->pwm_modeset_reg, &reg_value) < 0) {
		dev_err(&rdev->dev, "Failed to read register value\n");
		return -EIO;
	}

	dev_info(&rdev->dev, "%s: info->pwm_modeset_reg 0x%x = 0x%x\n",
		 info->desc.name, info->pwm_modeset_reg, reg_value);

	return ret;
}

static unsigned int mt6392_buck_get_mode(struct regulator_dev *rdev)
{
	unsigned int val;
	unsigned int mode;
	int ret;
	struct mt6392_regulator_info *info = rdev_get_drvdata(rdev);

	if (!info->pwm_modeset_mask) {
		dev_err(&rdev->dev, "regulator %s doesn't support get_mode\n", info->desc.name);
		return -EINVAL;
	}

	ret = regmap_read(rdev->regmap, info->pwm_modeset_reg, &val);
	if (ret < 0)
		return ret;

	val &= info->pwm_modeset_mask;
	val >>= ffs(info->pwm_modeset_mask) - 1;

	if (val & 0x1)
		mode = REGULATOR_MODE_FAST;
	else
		mode = REGULATOR_MODE_NORMAL;

	return mode;
}

static int mt6392_ldo_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	int ret, val = 0;
	struct mt6392_regulator_info *info = rdev_get_drvdata(rdev);

	if (!info->lp_modeset_mask) {
		dev_err(&rdev->dev, "regulator %s doesn't support set_mode\n",
			info->desc.name);
		return -EINVAL;
	}

	switch (mode) {
	case REGULATOR_MODE_STANDBY:
		val = MT6392_LDO_MODE_LP;
		break;
	case REGULATOR_MODE_NORMAL:
		val = MT6392_LDO_MODE_NORMAL;
		break;
	default:
		return -EINVAL;
	}

	val <<= ffs(info->lp_modeset_mask) - 1;

	ret = regmap_update_bits(rdev->regmap, info->lp_modeset_reg,
				 info->lp_modeset_mask, val);

	return ret;
}

static unsigned int mt6392_ldo_get_mode(struct regulator_dev *rdev)
{
	unsigned int val;
	unsigned int mode;
	int ret;
	struct mt6392_regulator_info *info = rdev_get_drvdata(rdev);

	if (!info->lp_modeset_mask) {
		dev_err(&rdev->dev, "regulator %s doesn't support get_mode\n",
			info->desc.name);
		return -EINVAL;
	}

	ret = regmap_read(rdev->regmap, info->lp_modeset_reg, &val);
	if (ret < 0)
		return ret;

	val &= info->lp_modeset_mask;
	val >>= ffs(info->lp_modeset_mask) - 1;

	if (val & 0x1)
		mode = REGULATOR_MODE_STANDBY;
	else
		mode = REGULATOR_MODE_NORMAL;

	return mode;
}

static int mt6392_get_status(struct regulator_dev *rdev)
{
	int ret;
	u32 regval;
	struct mt6392_regulator_info *info = rdev_get_drvdata(rdev);

	ret = regmap_read(rdev->regmap, info->qi_status_reg, &regval);
	if (ret != 0) {
		dev_err(&rdev->dev, "Failed to read qi_status_reg: %d\n", ret);
		return ret;
	}

	return (regval & info->qi_status_mask) ? REGULATOR_STATUS_ON : REGULATOR_STATUS_OFF;
}

static const struct regulator_ops mt6392_volt_range_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6392_get_status,
	.set_mode = mt6392_buck_set_mode,
	.get_mode = mt6392_buck_get_mode,
};

static const struct regulator_ops mt6392_volt_table_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6392_get_status,
	.set_mode = mt6392_ldo_set_mode,
	.get_mode = mt6392_ldo_get_mode,
};

static const struct regulator_ops mt6392_volt_ldo_range_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6392_get_status,
	.set_mode = mt6392_ldo_set_mode,
	.get_mode = mt6392_ldo_get_mode,
};

static const struct regulator_ops mt6392_volt_fixed_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6392_get_status,
	.set_mode = mt6392_ldo_set_mode,
	.get_mode = mt6392_ldo_get_mode,
};

static const struct regulator_ops mt6392_volt_fixed_no_mode_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6392_get_status,
};

static const struct regulator_ops mt6392_volt_no_ops = {
	.list_voltage = regulator_list_voltage_linear,
};

/* The array is indexed by id(MT6392_ID_XXX) */
static struct mt6392_regulator_info mt6392_regulators[] = {
	MT6392_BUCK("vproc", VPROC, "vproc", 700000, 1493750, 6250,
		    buck_volt_range1,
		    MT6392_VPROC_CON7, BIT(13), // Regulator status
		    MT6392_VPROC_CON7, BIT(0),  // Regulator enable
		    MT6392_VPROC_CON5, BIT(0),  // Vsel ctrl mode selector,not present in data sheet
		    MT6392_VPROC_CON9, GENMASK(6, 0),  // Vsel when control mode = register (0)
		    MT6392_VPROC_CON10, GENMASK(6, 0), // Vsel when control mode = normal (1)
		    MT6392_VPROC_CON2, BIT(8),  // Auto / Force PWM mode
		    12500),
	MT6392_BUCK("vsys", VSYS, "vsys", 1400000, 2987500, 12500,
		    buck_volt_range2,
		    MT6392_VSYS_CON7, BIT(13),
		    MT6392_VSYS_CON7, BIT(0),
		    MT6392_VSYS_CON5, BIT(0), // Not present in data sheet
		    MT6392_VSYS_CON9, GENMASK(6, 0),
		    MT6392_VSYS_CON10, GENMASK(6, 0),
		    MT6392_VSYS_CON2, BIT(8),
		    25000),
	MT6392_BUCK("vcore", VCORE, "vcore", 700000, 1493750, 6250,
		    buck_volt_range1,
		    MT6392_VCORE_CON7, BIT(13),
		    MT6392_VCORE_CON7, BIT(0),
		    MT6392_VCORE_CON5, BIT(0), // Not present in data sheet
		    MT6392_VCORE_CON9, GENMASK(6, 0),
		    MT6392_VCORE_CON10, GENMASK(6, 0),
		    MT6392_VCORE_CON2, BIT(8),
		    12500),

	MT6392_REG_FIXED("vxo22", VXO22, "ldo1", 2200000,
			 MT6392_ANALDO_CON1, BIT(15),
			 MT6392_ANALDO_CON1, BIT(10), // Not present in data sheet
			 MT6392_ANALDO_CON1, BIT(7),
			 MT6392_ANALDO_CON1, BIT(1), // Not present in data sheet
			 110),
	MT6392_LDO("vaud22", VAUD22, "ldo1", ldo_volt_table1,
		   MT6392_ANALDO_CON2, BIT(15),
		   MT6392_ANALDO_CON2, BIT(14), // Not present in data sheet
		   MT6392_ANALDO_CON8, GENMASK(6, 5), // Not present in data sheet
		   MT6392_ANALDO_CON2, BIT(7),
		   MT6392_ANALDO_CON2, BIT(1),  // Not present in data sheet
		   264),
	MT6392_REG_FIXED_NO_MODE("vcama", VCAMA, "ldo1", 2800000,
				 MT6392_ANALDO_CON4, BIT(15),
				 MT6392_ANALDO_CON4, BIT(15),
				 264),
	MT6392_REG_FIXED("vaud28", VAUD28, "ldo1", 2800000,
			 MT6392_ANALDO_CON23, BIT(15),
			 MT6392_ANALDO_CON23, BIT(14), // Not present in data sheet
			 MT6392_ANALDO_CON23, BIT(7),
			 MT6392_ANALDO_CON23, BIT(1), // Not present in data sheet
			 264),
	MT6392_REG_FIXED("vadc18", VADC18, "ldo1", 1800000,
			 MT6392_ANALDO_CON25, BIT(15),
			 MT6392_ANALDO_CON25, BIT(14), // Not present in data sheet
			 MT6392_ANALDO_CON25, BIT(7),
			 MT6392_ANALDO_CON25, BIT(1), // Not present in data sheet
			 264),
	MT6392_LDO_LINEAR("vcn35", VCN35, "ldo2", 3300000, 3600000, 100000, ldo_volt_range2,
			  MT6392_ANALDO_CON17, BIT(15), // Not present in data sheet
			  MT6392_ANALDO_CON21, BIT(12), // Not present in data sheet
			  MT6392_ANALDO_CON16, GENMASK(4, 3),
			  MT6392_ANALDO_CON21, BIT(7),
			  MT6392_ANALDO_CON21, BIT(1), // Not present in data sheet
			  264),
	MT6392_REG_FIXED("vio28", VIO28, "ldo2", 2800000,
			 MT6392_DIGLDO_CON0, BIT(15),
			 MT6392_DIGLDO_CON0, BIT(14), // Not present in data sheet
			 MT6392_DIGLDO_CON0, BIT(7),
			 MT6392_DIGLDO_CON0, BIT(1), // Not present in data sheet
			 264),
	MT6392_REG_FIXED("vusb", VUSB, "ldo3", 3300000,
			 MT6392_DIGLDO_CON2, BIT(15),
			 MT6392_DIGLDO_CON2, BIT(14), // Not present in data sheet
			 MT6392_DIGLDO_CON2, BIT(7),
			 MT6392_DIGLDO_CON2, BIT(1), // Not present in data sheet
			 264),
	MT6392_LDO("vmc", VMC, "ldo2", ldo_volt_table3,
		   MT6392_DIGLDO_CON3, BIT(15),
		   MT6392_DIGLDO_CON3, BIT(12),
		   MT6392_DIGLDO_CON24, BIT(4),
		   MT6392_DIGLDO_CON3, BIT(7),
		   MT6392_DIGLDO_CON3, BIT(1), // Not present in data sheet
		   264),
	MT6392_LDO("vmch", VMCH, "ldo2", ldo_volt_table4,
		   MT6392_DIGLDO_CON5, BIT(15),
		   MT6392_DIGLDO_CON5, BIT(14),
		   MT6392_DIGLDO_CON26, BIT(7),
		   MT6392_DIGLDO_CON5, BIT(7),
		   MT6392_DIGLDO_CON5, BIT(1), // Not present in data sheet
		   264),
	MT6392_LDO("vemc3v3", VEMC3V3, "ldo3", ldo_volt_table4,
		   MT6392_DIGLDO_CON6, BIT(15),
		   MT6392_DIGLDO_CON6, BIT(14), // Not present in data sheet
		   MT6392_DIGLDO_CON27, BIT(7),
		   MT6392_DIGLDO_CON6, BIT(7),
		   MT6392_DIGLDO_CON6, BIT(1), // Not present in data sheet
		   264),
	MT6392_LDO("vgp1", VGP1, "ldo3", ldo_volt_table5,
		   MT6392_DIGLDO_CON7, BIT(15),
		   MT6392_DIGLDO_CON7, BIT(15),
		   MT6392_DIGLDO_CON28, GENMASK(7, 5),
		   MT6392_DIGLDO_CON7, BIT(7),
		   MT6392_DIGLDO_CON7, BIT(1), // Not present in data sheet
		   264),
	MT6392_LDO("vgp2", VGP2, "ldo3", ldo_volt_table5,
		   MT6392_DIGLDO_CON8, BIT(15),
		   MT6392_DIGLDO_CON8, BIT(15),
		   MT6392_DIGLDO_CON29, GENMASK(7, 5),
		   MT6392_DIGLDO_CON8, BIT(7),
		   MT6392_DIGLDO_CON8, BIT(1), // Not present in data sheet
		   264),
	MT6392_REG_FIXED("vcn18", VCN18, "avddldo", 1800000,
			 MT6392_DIGLDO_CON11, BIT(15),
			 MT6392_DIGLDO_CON11, BIT(14), // Not present in data sheet
			 MT6392_DIGLDO_CON11, BIT(7),
			 MT6392_DIGLDO_CON11, BIT(1), // Not present in data sheet
			 264),
	MT6392_LDO("vcamaf", VCAMAF, "ldo3", ldo_volt_table5,
		   MT6392_DIGLDO_CON31, BIT(15),
		   MT6392_DIGLDO_CON31, BIT(15),
		   MT6392_DIGLDO_CON32, GENMASK(7, 5),
		   MT6392_DIGLDO_CON31, BIT(7),
		   MT6392_DIGLDO_CON31, BIT(1), // Not present in data sheet
		   264),
	MT6392_LDO("vm", VM, "avddldo", ldo_volt_table6,
		   MT6392_DIGLDO_CON47, BIT(15),
		   MT6392_DIGLDO_CON47, BIT(14), // Not present in data sheet
		   MT6392_DIGLDO_CON48, GENMASK(5, 4), // Not present in data sheet
		   MT6392_DIGLDO_CON47, BIT(7), // Not present in data sheet
		   MT6392_DIGLDO_CON47, BIT(1),
		   264),
	MT6392_REG_FIXED("vio18", VIO18, "avddldo", 1800000,
			 MT6392_DIGLDO_CON49, BIT(15),
			 MT6392_DIGLDO_CON49, BIT(14), // Not present in data sheet
			 MT6392_DIGLDO_CON49, BIT(7),
			 MT6392_DIGLDO_CON49, BIT(1), // Not present in data sheet
			 264),
	MT6392_LDO("vcamd", VCAMD, "avddldo", ldo_volt_table7,
		   MT6392_DIGLDO_CON51, BIT(15),
		   MT6392_DIGLDO_CON51, BIT(14),
		   MT6392_DIGLDO_CON52, GENMASK(6, 5),
		   MT6392_DIGLDO_CON51, BIT(7),
		   MT6392_DIGLDO_CON51, BIT(1),
		   264),
	MT6392_REG_FIXED("vcamio", VCAMIO, "avddldo", 1800000,
			 MT6392_DIGLDO_CON53, BIT(15),
			 MT6392_DIGLDO_CON53, BIT(14),
			 MT6392_DIGLDO_CON53, BIT(7),
			 MT6392_DIGLDO_CON53, BIT(1), // Not present in data sheet
			 264),
	MT6392_REG_FIXED("vm25", VM25, "ldo3", 2500000,
			 MT6392_DIGLDO_CON55, BIT(15),
			 MT6392_DIGLDO_CON55, BIT(14), // Not present in data sheet
			 MT6392_DIGLDO_CON55, BIT(7),
			 MT6392_DIGLDO_CON55, BIT(1), // Not present in data sheet
			 264),
	MT6392_LDO("vefuse", VEFUSE, "ldo2", ldo_volt_table8,
		   MT6392_DIGLDO_CON57, BIT(15),
		   MT6392_DIGLDO_CON57, BIT(14), // Not present in data sheet
		   MT6392_DIGLDO_CON58, BIT(5), // Not present in data sheet
		   MT6392_DIGLDO_CON57, BIT(7),
		   MT6392_DIGLDO_CON57, BIT(1), // Not present in data sheet
		   264),
	MT6392_REG("vdig18", VDIG18, "ldo2", 1800000), // Internal non changeable regulator
	MT6392_REG_FIXED_NO_MODE("vrtc", VRTC, "ldo1", 2800000,
				 MT6392_DIGLDO_CON15, BIT(15),
				 MT6392_DIGLDO_CON15, BIT(8), // Not present in data sheet
				 264)
};

// Buck regulators can be in Register mode or Normal mode.
// Each mode uses a different register to set the desired voltage.
static int mt6392_set_buck_vsel_reg(struct platform_device *pdev)
{
	struct mt6397_chip *mt6392 = dev_get_drvdata(pdev->dev.parent);
	int i;
	u32 regval;

	for (i = 0; i < MT6392_MAX_REGULATOR; i++) {
		if (mt6392_regulators[i].vselctrl_reg) {
			// Read the vselctrl_reg register
			if (regmap_read(mt6392->regmap,
					mt6392_regulators[i].vselctrl_reg,
					&regval) < 0) {
				dev_err(&pdev->dev,
					"Failed to read buck ctrl\n");
				return -EIO;
			}

			// vselctrl_reg[vselctrl_mask] defines the mode
			if (regval & mt6392_regulators[i].vselctrl_mask) {
				// Regulator in Normal mode
				mt6392_regulators[i].desc.vsel_reg =
					mt6392_regulators[i].vsel_normal_mode_reg;
				mt6392_regulators[i].desc.vsel_mask =
					mt6392_regulators[i].vsel_normal_mode_mask;
			} else {
				// Regulator in Register mode
				mt6392_regulators[i].desc.vsel_reg =
					mt6392_regulators[i].vsel_reg_mode_reg;
				mt6392_regulators[i].desc.vsel_mask =
					mt6392_regulators[i].vsel_reg_mode_mask;
			}
		}
	}

	return 0;
}

static int mt6392_regulator_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6392 = dev_get_drvdata(pdev->dev.parent);
	struct regulator_config config = {};
	struct regulator_dev *rdev;
	int i;

	device_set_of_node_from_dev(&pdev->dev, pdev->dev.parent);

	// Initialize the bucks' vsel_reg and vsel_mask according to current HW state
	if (mt6392_set_buck_vsel_reg(pdev))
		return -EIO;

	config.dev = mt6392->dev;
	config.regmap = mt6392->regmap;
	for (i = 0; i < MT6392_MAX_REGULATOR; i++) {
		config.driver_data = &mt6392_regulators[i];

		rdev = devm_regulator_register(&pdev->dev,
					       &mt6392_regulators[i].desc,
					       &config);
		if (IS_ERR(rdev)) {
			dev_err(&pdev->dev, "failed to register %s\n",
				mt6392_regulators[i].desc.name);
			return PTR_ERR(rdev);
		}
	}

	return 0;
}

static const struct platform_device_id mt6392_platform_ids[] = {
	{"mt6392-regulator", 0},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, mt6392_platform_ids);

static struct platform_driver mt6392_regulator_driver = {
	.driver = {
		.name = "mt6392-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = mt6392_regulator_probe,
	.id_table = mt6392_platform_ids,
};

module_platform_driver(mt6392_regulator_driver);

MODULE_AUTHOR("Chen Zhong <chen.zhong@mediatek.com>");
MODULE_DESCRIPTION("Regulator Driver for MediaTek MT6392 PMIC");
MODULE_LICENSE("GPL");
