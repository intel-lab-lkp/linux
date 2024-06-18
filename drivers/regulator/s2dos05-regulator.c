// SPDX-License-Identifier: GPL-2.0+
/*
 * s2dos05.c - Regulator driver for the Samsung s2dos05
 *
 * Copyright (C) 2016 Samsung Electronics
 * Copyright (C) 2023 Dzmitry Sankouski <dsankouski@gmail.com>
 *
 */

#include <linux/module.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/mfd/samsung/s2dos-core.h>
#include <linux/mfd/samsung/s2dos05.h>
#include <linux/i2c.h>
#include <linux/debugfs.h>

struct s2dos05_data {
	struct regmap *regmap;
	struct device *dev;
};

static int s2m_enable(struct regulator_dev *rdev)
{
	struct s2dos05_data *info = rdev_get_drvdata(rdev);
	struct regmap *regmap = info->regmap;

	return regmap_update_bits(regmap, rdev->desc->enable_reg,
				  rdev->desc->enable_mask,
					rdev->desc->enable_mask);
}

static int s2m_disable_regmap(struct regulator_dev *rdev)
{
	struct s2dos05_data *info = rdev_get_drvdata(rdev);
	struct regmap *regmap = info->regmap;
	u8 val;

	if (rdev->desc->enable_is_inverted)
		val = rdev->desc->enable_mask;
	else
		val = 0;

	return regmap_update_bits(regmap, rdev->desc->enable_reg, rdev->desc->enable_mask,
				   val);
}

static int s2m_is_enabled_regmap(struct regulator_dev *rdev)
{
	struct s2dos05_data *info = rdev_get_drvdata(rdev);
	struct regmap *regmap = info->regmap;
	int ret;
	unsigned int val;

	ret = regmap_read(regmap, rdev->desc->enable_reg, &val);
	if (ret < 0)
		return ret;

	if (rdev->desc->enable_is_inverted)
		return (val & rdev->desc->enable_mask) == 0;
	else
		return (val & rdev->desc->enable_mask) != 0;
}

static int s2m_get_voltage_sel_regmap(struct regulator_dev *rdev)
{
	struct s2dos05_data *info = rdev_get_drvdata(rdev);
	struct regmap *regmap = info->regmap;
	int ret;
	unsigned int val;

	ret = regmap_read(regmap, rdev->desc->vsel_reg, &val);
	if (ret < 0)
		return ret;

	val &= rdev->desc->vsel_mask;

	return val;
}

static int s2m_set_voltage_sel_regmap(struct regulator_dev *rdev,
					unsigned int sel)
{
	struct s2dos05_data *info = rdev_get_drvdata(rdev);
	struct regmap *regmap = info->regmap;
	int ret;

	ret = regmap_update_bits(regmap, rdev->desc->vsel_reg, rdev->desc->vsel_mask,
				sel);
	if (ret < 0)
		goto out;

	if (rdev->desc->apply_bit)
		ret = regmap_update_bits(regmap, rdev->desc->apply_reg,
					 rdev->desc->apply_bit,
					 rdev->desc->apply_bit);
	return ret;
out:
	pr_warn("%s: failed to set voltage_sel_regmap\n", rdev->desc->name);
	return ret;
}

static int s2m_set_voltage_sel_regmap_buck(struct regulator_dev *rdev,
					unsigned int sel)
{
	struct s2dos05_data *info = rdev_get_drvdata(rdev);
	struct regmap *regmap = info->regmap;
	int ret;

	ret = regmap_write(regmap, rdev->desc->vsel_reg, sel);
	if (ret < 0)
		goto out;

	if (rdev->desc->apply_bit)
		ret = regmap_update_bits(regmap, rdev->desc->apply_reg,
					 rdev->desc->apply_bit,
					 rdev->desc->apply_bit);
	return ret;
out:
	pr_warn("%s: failed to set voltage_sel_regmap\n", rdev->desc->name);
	return ret;
}

static int s2m_set_voltage_time_sel(struct regulator_dev *rdev,
				   unsigned int old_selector,
				   unsigned int new_selector)
{
	int old_volt, new_volt;

	/* sanity check */
	if (!rdev->desc->ops->list_voltage)
		return -EINVAL;

	old_volt = rdev->desc->ops->list_voltage(rdev, old_selector);
	new_volt = rdev->desc->ops->list_voltage(rdev, new_selector);

	if (old_selector < new_selector)
		return DIV_ROUND_UP(new_volt - old_volt, S2DOS05_RAMP_DELAY);

	return 0;
}

static int s2m_set_active_discharge(struct regulator_dev *rdev,
					bool enable)
{
	struct s2dos05_data *info = rdev_get_drvdata(rdev);
	struct regmap *regmap = info->regmap;
	int ret;
	u8 val;

	if (enable)
		val = rdev->desc->active_discharge_on;
	else
		val = rdev->desc->active_discharge_off;

	ret = regmap_update_bits(regmap, rdev->desc->active_discharge_reg,
				rdev->desc->active_discharge_mask, val);
	return ret;
}

static const struct regulator_ops s2dos05_ldo_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.is_enabled		= s2m_is_enabled_regmap,
	.enable			= s2m_enable,
	.disable		= s2m_disable_regmap,
	.get_voltage_sel	= s2m_get_voltage_sel_regmap,
	.set_voltage_sel	= s2m_set_voltage_sel_regmap,
	.set_voltage_time_sel	= s2m_set_voltage_time_sel,
	.set_active_discharge	= s2m_set_active_discharge,
};

static const struct regulator_ops s2dos05_buck_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.is_enabled		= s2m_is_enabled_regmap,
	.enable			= s2m_enable,
	.disable		= s2m_disable_regmap,
	.get_voltage_sel	= s2m_get_voltage_sel_regmap,
	.set_voltage_sel	= s2m_set_voltage_sel_regmap_buck,
	.set_voltage_time_sel	= s2m_set_voltage_time_sel,
	.set_active_discharge	= s2m_set_active_discharge,
};

#define _BUCK(macro)	S2DOS05_BUCK##macro
#define _buck_ops(num)	s2dos05_buck_ops##num

#define _LDO(macro)	S2DOS05_LDO##macro
#define _REG(ctrl)	S2DOS05_REG##ctrl
#define _ldo_ops(num)	s2dos05_ldo_ops##num
#define _MASK(macro)	S2DOS05_ENABLE_MASK##macro
#define _TIME(macro)	S2DOS05_ENABLE_TIME##macro

#define BUCK_DESC(_name, _id, _ops, m, s, v, e, em, t, a) {	\
	.name		= _name,				\
	.id		= _id,					\
	.ops		= _ops,					\
	.type		= REGULATOR_VOLTAGE,			\
	.owner		= THIS_MODULE,				\
	.min_uV		= m,					\
	.uV_step	= s,					\
	.n_voltages	= S2DOS05_BUCK_N_VOLTAGES,		\
	.vsel_reg	= v,					\
	.vsel_mask	= S2DOS05_BUCK_VSEL_MASK,		\
	.enable_reg	= e,					\
	.enable_mask	= em,					\
	.enable_time	= t,					\
	.active_discharge_off = 0,				\
	.active_discharge_on = S2DOS05_BUCK_FD_MASK,		\
	.active_discharge_reg	= a,				\
	.active_discharge_mask	= S2DOS05_BUCK_FD_MASK		\
}

#define LDO_DESC(_name, _id, _ops, m, s, v, e, em, t, a) {	\
	.name		= _name,				\
	.id		= _id,					\
	.ops		= _ops,					\
	.type		= REGULATOR_VOLTAGE,			\
	.owner		= THIS_MODULE,				\
	.min_uV		= m,					\
	.uV_step	= s,					\
	.n_voltages	= S2DOS05_LDO_N_VOLTAGES,		\
	.vsel_reg	= v,					\
	.vsel_mask	= S2DOS05_LDO_VSEL_MASK,		\
	.enable_reg	= e,					\
	.enable_mask	= em,					\
	.enable_time	= t,					\
	.active_discharge_off = 0,				\
	.active_discharge_on = S2DOS05_LDO_FD_MASK,		\
	.active_discharge_reg	= a,				\
	.active_discharge_mask	= S2DOS05_LDO_FD_MASK		\
}

static struct regulator_desc regulators[S2DOS05_REGULATOR_MAX] = {
		/* name, id, ops, min_uv, uV_step, vsel_reg, enable_reg */
		LDO_DESC("ldo1", _LDO(1), &_ldo_ops(), _LDO(_MIN1),
			_LDO(_STEP1), _REG(_LDO1_CFG),
			_REG(_EN), _MASK(_L1), _TIME(_LDO), _REG(_LDO1_CFG)),
		LDO_DESC("ldo2", _LDO(2), &_ldo_ops(), _LDO(_MIN1),
			_LDO(_STEP1), _REG(_LDO2_CFG),
			_REG(_EN), _MASK(_L2), _TIME(_LDO), _REG(_LDO2_CFG)),
		LDO_DESC("ldo3", _LDO(3), &_ldo_ops(), _LDO(_MIN2),
			_LDO(_STEP1), _REG(_LDO3_CFG),
			_REG(_EN), _MASK(_L3), _TIME(_LDO), _REG(_LDO3_CFG)),
		LDO_DESC("ldo4", _LDO(4), &_ldo_ops(), _LDO(_MIN2),
			_LDO(_STEP1), _REG(_LDO4_CFG),
			_REG(_EN), _MASK(_L4), _TIME(_LDO), _REG(_LDO4_CFG)),
		BUCK_DESC("buck1", _BUCK(1), &_buck_ops(), _BUCK(_MIN1),
			_BUCK(_STEP1), _REG(_BUCK_VOUT),
			_REG(_EN), _MASK(_B1), _TIME(_BUCK), _REG(_BUCK_CFG)),
};

static int s2dos05_pmic_dt_parse_pdata(struct device *dev,
					struct of_regulator_match *rdata,
					unsigned int rdev_num)
{
	struct device_node *reg_np;
	int err;

	reg_np = of_get_child_by_name(dev->parent->of_node, "regulators");
	if (!reg_np) {
		dev_err(dev, "could not find regulators sub-node\n");
		return -EINVAL;
	}

	err = of_regulator_match(dev, reg_np, rdata, rdev_num);
	of_node_put(reg_np);

	return err;
}

static int s2dos05_pmic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s2dos_core *iodev = dev_get_drvdata(pdev->dev.parent);
	struct of_regulator_match *rdata = NULL;
	struct s2dos05_data *s2dos05;
	struct regulator_config config = { };
	unsigned int rdev_num = ARRAY_SIZE(regulators);
	int i;
	int ret, err = 0;

	s2dos05 = devm_kzalloc(dev, sizeof(struct s2dos05_data),
				GFP_KERNEL);
	if (!s2dos05) {
		ret = -ENOMEM;
		goto err_data;
	}
	platform_set_drvdata(pdev, s2dos05);

	rdata = kcalloc(rdev_num, sizeof(*rdata), GFP_KERNEL);
	if (!rdata)
		return -ENOMEM;

	for (i = 0; i < rdev_num; i++)
		rdata[i].name = regulators[i].name;

	err = s2dos05_pmic_dt_parse_pdata(dev, rdata, rdev_num);
	if (err < 0) {
		dev_err(dev, "Failed to parse regulators device of_node\n");
		goto err_data;
	}

	s2dos05->regmap = iodev->regmap;
	s2dos05->dev = dev;

	for (i = 0; i < rdev_num; i++) {
		struct regulator_dev *regulator;

		config.init_data = rdata[i].init_data;
		config.of_node = rdata[i].of_node;
		config.dev = dev;
		config.driver_data = s2dos05;
		regulator = devm_regulator_register(&pdev->dev,
						&regulators[i], &config);
		if (IS_ERR(regulator)) {
			ret = PTR_ERR(regulator);
			dev_err(&pdev->dev, "regulator init failed for %d\n",
				i);
			goto out;
		}
	}

out:
	kfree(rdata);

	return ret;

err_data:
	devm_kfree(dev, (void *)s2dos05);
	kfree(s2dos05);

	return ret;
}

static const struct platform_device_id s2dos05_pmic_id[] = {
	{ "s2dos05-regulator" },
	{ },
};
MODULE_DEVICE_TABLE(platform, s2dos05_pmic_id);

static struct platform_driver s2dos05_platform_driver = {
	.driver = {
		.name = "s2dos05",
	},
	.probe = s2dos05_pmic_probe,
	.id_table = s2dos05_pmic_id,
};
module_platform_driver(s2dos05_platform_driver);

MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_DESCRIPTION("SAMSUNG s2dos05 Regulator Driver");
MODULE_LICENSE("GPL");
