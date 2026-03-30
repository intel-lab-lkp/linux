// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2020 MediaTek Inc.
// Copyright (c) 2020 BayLibre, SAS.
// Author: Chen Zhong <chen.zhong@mediatek.com>
// Author: Fabien Parent <fparent@baylibre.com>
//
// Based on mt6397-regulator.c

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

/*
 * MT6392 regulators' information
 *
 * @desc: standard fields of regulator description.
 * @qi: Mask for query enable signal status of regulators
 * @vselon_reg: Register sections for hardware control mode of bucks
 * @vselctrl_reg: Register for controlling the buck control mode.
 * @vselctrl_mask: Mask for query buck's voltage control mode.
 */
struct mt6392_regulator_info {
	struct regulator_desc desc;
	u32 qi;
	u32 vselon_reg;
	u32 vselctrl_reg;
	u32 vselctrl_mask;
	u32 modeset_reg;
	u32 modeset_mask;
};

#define MT6392_BUCK(match, vreg, supply, min, max, step, volt_ranges,	\
		enreg, vosel_reg, vosel_mask, voselon_reg, vosel_ctrl,	\
		_modeset_reg, _modeset_mask, rampdelay)			\
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
		.vsel_reg = vosel_reg,					\
		.vsel_mask = vosel_mask,				\
		.enable_reg = enreg,					\
		.enable_mask = BIT(0),					\
		.ramp_delay = rampdelay,				\
	},								\
	.qi = BIT(13),							\
	.vselon_reg = voselon_reg,					\
	.vselctrl_reg = vosel_ctrl,					\
	.vselctrl_mask = BIT(1),					\
	.modeset_reg = _modeset_reg,					\
	.modeset_mask = _modeset_mask,					\
}

#define MT6392_LDO(match, vreg, supply, ldo_volt_table, enreg, enbit,	\
		vosel_reg, vosel_mask, _modeset_reg, _modeset_mask,	\
		entime)							\
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
		.vsel_reg = vosel_reg,					\
		.vsel_mask = vosel_mask,				\
		.enable_reg = enreg,					\
		.enable_mask = BIT(enbit),				\
		.enable_time = entime,					\
	},								\
	.qi = BIT(15),							\
	.modeset_reg = _modeset_reg,					\
	.modeset_mask = _modeset_mask,					\
}

#define MT6392_LDO_LINEAR(match, vreg, supply, min, max, step,		\
		volt_ranges, enreg, enbit, vosel_reg, vosel_mask,	\
		_modeset_reg, _modeset_mask, entime)			\
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
		.vsel_reg = vosel_reg,					\
		.vsel_mask = vosel_mask,				\
		.enable_reg = enreg,					\
		.enable_mask = BIT(enbit),				\
		.enable_time = entime,					\
	},								\
	.qi = BIT(15),							\
	.modeset_reg = _modeset_reg,					\
	.modeset_mask = _modeset_mask,					\
}

#define MT6392_REG_FIXED(match, vreg, supply, enreg, enbit, volt,	\
		_modeset_reg, _modeset_mask, entime)			\
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
		.enable_reg = enreg,					\
		.enable_mask = BIT(enbit),				\
		.enable_time = entime,					\
		.min_uV = volt,						\
	},								\
	.qi = BIT(15),							\
	.modeset_reg = _modeset_reg,					\
	.modeset_mask = _modeset_mask,					\
}

#define MT6392_REG_FIXED_NO_MODE(match, vreg, supply, enreg, enbit,	\
	volt, entime)							\
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
		.enable_reg = enreg,					\
		.enable_mask = BIT(enbit),				\
		.enable_time = entime,					\
		.min_uV = volt,						\
	},								\
	.qi = BIT(15),							\
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

static const u32 ldo_volt_table1b[] = {
	1500000, 1800000, 2500000, 2800000,
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

	val <<= ffs(info->modeset_mask) - 1;

	ret = regmap_update_bits(rdev->regmap, info->modeset_reg,
				 info->modeset_mask, val);

	return ret;
}

static unsigned int mt6392_buck_get_mode(struct regulator_dev *rdev)
{
	unsigned int val;
	unsigned int mode;
	int ret;
	struct mt6392_regulator_info *info = rdev_get_drvdata(rdev);

	ret = regmap_read(rdev->regmap, info->modeset_reg, &val);
	if (ret < 0)
		return ret;

	val &= info->modeset_mask;
	val >>= ffs(info->modeset_mask) - 1;

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

	val <<= ffs(info->modeset_mask) - 1;

	ret = regmap_update_bits(rdev->regmap, info->modeset_reg,
				 info->modeset_mask, val);

	return ret;
}

static unsigned int mt6392_ldo_get_mode(struct regulator_dev *rdev)
{
	unsigned int val;
	unsigned int mode;
	int ret;
	struct mt6392_regulator_info *info = rdev_get_drvdata(rdev);

	ret = regmap_read(rdev->regmap, info->modeset_reg, &val);
	if (ret < 0)
		return ret;

	val &= info->modeset_mask;
	val >>= ffs(info->modeset_mask) - 1;

	if (val & 0x1)
		mode = REGULATOR_MODE_STANDBY;
	else
		mode = REGULATOR_MODE_NORMAL;

	return mode;
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
	.set_mode = mt6392_ldo_set_mode,
	.get_mode = mt6392_ldo_get_mode,
};

static const struct regulator_ops mt6392_volt_fixed_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_mode = mt6392_ldo_set_mode,
	.get_mode = mt6392_ldo_get_mode,
};

static const struct regulator_ops mt6392_volt_fixed_no_mode_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

/* The array is indexed by id(MT6392_ID_XXX) */
static struct mt6392_regulator_info mt6392_regulators[] = {
	MT6392_BUCK("vproc", VPROC, "vproc", 700000, 1493750, 6250,
		    buck_volt_range1, MT6392_VPROC_CON7, MT6392_VPROC_CON9, 0x7f,
		    MT6392_VPROC_CON10, MT6392_VPROC_CON5, MT6392_VPROC_CON2, 0x100,
		    12500),
	MT6392_BUCK("vsys", VSYS, "vsys", 1400000, 2987500, 12500,
		    buck_volt_range2, MT6392_VSYS_CON7, MT6392_VSYS_CON9, 0x7f,
		    MT6392_VSYS_CON10, MT6392_VSYS_CON5, MT6392_VSYS_CON2, 0x100,
		    25000),
	MT6392_BUCK("vcore", VCORE, "vcore", 700000, 1493750, 6250,
		    buck_volt_range1, MT6392_VCORE_CON7, MT6392_VCORE_CON9, 0x7f,
		    MT6392_VCORE_CON10, MT6392_VCORE_CON5, MT6392_VCORE_CON2, 0x100,
		    12500),
	MT6392_REG_FIXED("vxo22", VXO22, "ldo1", MT6392_ANALDO_CON1, 10, 2200000,
			 MT6392_ANALDO_CON1, 0x2, 110),
	MT6392_LDO("vaud22", VAUD22, "ldo1", ldo_volt_table1,
		   MT6392_ANALDO_CON2, 14, MT6392_ANALDO_CON8, 0x60,
		   MT6392_ANALDO_CON2, 0x2, 264),
	MT6392_REG_FIXED_NO_MODE("vcama", VCAMA, "ldo1", MT6392_ANALDO_CON4, 15,
				 2800000, 264),
	MT6392_REG_FIXED("vaud28", VAUD28, "ldo1", MT6392_ANALDO_CON23, 14, 2800000,
			 MT6392_ANALDO_CON23, 0x2, 264),
	MT6392_REG_FIXED("vadc18", VADC18, "ldo1", MT6392_ANALDO_CON25, 14, 1800000,
			 MT6392_ANALDO_CON25, 0x2, 264),
	MT6392_LDO_LINEAR("vcn35", VCN35, "ldo2", 3300000, 3600000, 100000,
			  ldo_volt_range2, MT6392_ANALDO_CON21, 12,
			  MT6392_ANALDO_CON16, 0xC, MT6392_ANALDO_CON21, 0x2, 264),
	MT6392_REG_FIXED("vio28", VIO28, "ldo2", MT6392_DIGLDO_CON0, 14, 2800000,
			 MT6392_DIGLDO_CON0, 0x2, 264),
	MT6392_REG_FIXED("vusb", VUSB, "ldo3", MT6392_DIGLDO_CON2, 14, 3300000,
			 MT6392_DIGLDO_CON2, 0x2, 264),
	MT6392_LDO("vmc", VMC, "ldo2", ldo_volt_table3,
		   MT6392_DIGLDO_CON3, 12, MT6392_DIGLDO_CON24, 0x10,
		   MT6392_DIGLDO_CON3, 0x2, 264),
	MT6392_LDO("vmch", VMCH, "ldo2", ldo_volt_table4,
		   MT6392_DIGLDO_CON5, 14, MT6392_DIGLDO_CON26, 0x80,
		   MT6392_DIGLDO_CON5, 0x2, 264),
	MT6392_LDO("vemc3v3", VEMC3V3, "ldo3", ldo_volt_table4,
		   MT6392_DIGLDO_CON6, 14, MT6392_DIGLDO_CON27, 0x80,
		   MT6392_DIGLDO_CON6, 0x2, 264),
	MT6392_LDO("vgp1", VGP1, "ldo3", ldo_volt_table5,
		   MT6392_DIGLDO_CON7, 15, MT6392_DIGLDO_CON28, 0xE0,
		   MT6392_DIGLDO_CON7, 0x2, 264),
	MT6392_LDO("vgp2", VGP2, "ldo3", ldo_volt_table5,
		   MT6392_DIGLDO_CON8, 15, MT6392_DIGLDO_CON29, 0xE0,
		   MT6392_DIGLDO_CON8, 0x2, 264),
	MT6392_REG_FIXED("vcn18", VCN18, "avddldo", MT6392_DIGLDO_CON11, 14, 1800000,
			 MT6392_DIGLDO_CON11, 0x2, 264),
	MT6392_LDO("vcamaf", VCAMAF, "ldo3", ldo_volt_table5,
		   MT6392_DIGLDO_CON31, 15, MT6392_DIGLDO_CON32, 0xE0,
		   MT6392_DIGLDO_CON31, 0x2, 264),
	MT6392_LDO("vm", VM, "avddldo", ldo_volt_table6,
		   MT6392_DIGLDO_CON47, 14, MT6392_DIGLDO_CON48, 0x30,
		   MT6392_DIGLDO_CON47, 0x2, 264),
	MT6392_REG_FIXED("vio18", VIO18, "avddldo", MT6392_DIGLDO_CON49, 14, 1800000,
			 MT6392_DIGLDO_CON49, 0x2, 264),
	MT6392_LDO("vcamd", VCAMD, "avddldo", ldo_volt_table7,
		   MT6392_DIGLDO_CON51, 14, MT6392_DIGLDO_CON52, 0x60,
		   MT6392_DIGLDO_CON51, 0x2, 264),
	MT6392_REG_FIXED("vcamio", VCAMIO, "avddldo", MT6392_DIGLDO_CON53, 14, 1800000,
			 MT6392_DIGLDO_CON53, 0x2, 264),
	MT6392_REG_FIXED("vm25", VM25, "ldo3", MT6392_DIGLDO_CON55, 14, 2500000,
			 MT6392_DIGLDO_CON55, 0x2, 264),
	MT6392_LDO("vefuse", VEFUSE, "ldo2", ldo_volt_table8,
		   MT6392_DIGLDO_CON57, 14, MT6392_DIGLDO_CON58, 0x10,
		   MT6392_DIGLDO_CON57, 0x2, 264),
	MT6392_REG_FIXED_NO_MODE("vdig18", VDIG18, "ldo2", MT6392_DIGLDO_CON12, 15,
				 1800000, 264),
	MT6392_REG_FIXED_NO_MODE("vrtc", VRTC, "ldo1", MT6392_DIGLDO_CON15, 15,
				 2800000, 264)
};

static int mt6392_set_buck_vosel_reg(struct platform_device *pdev)
{
	struct mt6397_chip *mt6392 = dev_get_drvdata(pdev->dev.parent);
	int i;
	u32 regval;

	for (i = 0; i < MT6392_MAX_REGULATOR; i++) {
		if (mt6392_regulators[i].vselctrl_reg) {
			if (regmap_read(mt6392->regmap,
					mt6392_regulators[i].vselctrl_reg,
					&regval) < 0) {
				dev_err(&pdev->dev,
					"Failed to read buck ctrl\n");
				return -EIO;
			}

			if (regval & mt6392_regulators[i].vselctrl_mask) {
				mt6392_regulators[i].desc.vsel_reg =
				mt6392_regulators[i].vselon_reg;
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

	pdev->dev.of_node = pdev->dev.parent->of_node;

	/* Query buck controller to select activated voltage register part */
	if (mt6392_set_buck_vosel_reg(pdev))
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
