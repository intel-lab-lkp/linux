// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/drivers/regulator/aat2870-regulator.c
 *
 * Copyright (c) 2011, NVIDIA Corporation.
 * Author: Jin Park <jinyoungp@nvidia.com>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/mfd/aat2870.h>
#include <linux/regulator/of_regulator.h>

/* Device IDs */
enum aat2870_regulator_id {
	AAT2870_ID_LDOA,
	AAT2870_ID_LDOB,
	AAT2870_ID_LDOC,
	AAT2870_ID_LDOD
};

struct aat2870_regulator {
	struct aat2870_data *aat2870;
	struct regulator_desc desc;

	u8 enable_addr;
	u8 enable_shift;
	u8 enable_mask;

	u8 voltage_addr;
	u8 voltage_shift;
	u8 voltage_mask;
};

static int aat2870_ldo_set_voltage_sel(struct regulator_dev *rdev,
				       unsigned selector)
{
	struct aat2870_regulator *ri = rdev_get_drvdata(rdev);
	struct aat2870_data *aat2870 = ri->aat2870;

	return aat2870->update(aat2870, ri->voltage_addr, ri->voltage_mask,
			       selector << ri->voltage_shift);
}

static int aat2870_ldo_get_voltage_sel(struct regulator_dev *rdev)
{
	struct aat2870_regulator *ri = rdev_get_drvdata(rdev);
	struct aat2870_data *aat2870 = ri->aat2870;
	u8 val;
	int ret;

	ret = aat2870->read(aat2870, ri->voltage_addr, &val);
	if (ret)
		return ret;

	return (val & ri->voltage_mask) >> ri->voltage_shift;
}

static int aat2870_ldo_enable(struct regulator_dev *rdev)
{
	struct aat2870_regulator *ri = rdev_get_drvdata(rdev);
	struct aat2870_data *aat2870 = ri->aat2870;

	return aat2870->update(aat2870, ri->enable_addr, ri->enable_mask,
			       ri->enable_mask);
}

static int aat2870_ldo_disable(struct regulator_dev *rdev)
{
	struct aat2870_regulator *ri = rdev_get_drvdata(rdev);
	struct aat2870_data *aat2870 = ri->aat2870;

	return aat2870->update(aat2870, ri->enable_addr, ri->enable_mask, 0);
}

static int aat2870_ldo_is_enabled(struct regulator_dev *rdev)
{
	struct aat2870_regulator *ri = rdev_get_drvdata(rdev);
	struct aat2870_data *aat2870 = ri->aat2870;
	u8 val;
	int ret;

	ret = aat2870->read(aat2870, ri->enable_addr, &val);
	if (ret)
		return ret;

	return val & ri->enable_mask ? 1 : 0;
}

static const struct regulator_ops aat2870_ldo_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_ascend,
	.set_voltage_sel = aat2870_ldo_set_voltage_sel,
	.get_voltage_sel = aat2870_ldo_get_voltage_sel,
	.enable = aat2870_ldo_enable,
	.disable = aat2870_ldo_disable,
	.is_enabled = aat2870_ldo_is_enabled,
};

static const unsigned int aat2870_ldo_voltages[] = {
	1200000, 1300000, 1500000, 1600000,
	1800000, 2000000, 2200000, 2500000,
	2600000, 2700000, 2800000, 2900000,
	3000000, 3100000, 3200000, 3300000,
};

#define AAT2870_LDO(ids)				\
	{						\
		.desc = {				\
			.name = #ids,			\
			.id = AAT2870_ID_##ids,		\
			.n_voltages = ARRAY_SIZE(aat2870_ldo_voltages),	\
			.volt_table = aat2870_ldo_voltages, \
			.ops = &aat2870_ldo_ops,	\
			.type = REGULATOR_VOLTAGE,	\
			.owner = THIS_MODULE,		\
		},					\
	}

static struct aat2870_regulator aat2870_regulators[] = {
	AAT2870_LDO(LDOA),
	AAT2870_LDO(LDOB),
	AAT2870_LDO(LDOC),
	AAT2870_LDO(LDOD),
};

static struct of_regulator_match aat2870_regulator_matches[] = {
	{ .name = "ldo-a" },
	{ .name = "ldo-b" },
	{ .name = "ldo-c" },
	{ .name = "ldo-d" },
};

static struct aat2870_regulator *aat2870_get_regulator(int id)
{
	struct aat2870_regulator *ri = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(aat2870_regulators); i++) {
		ri = &aat2870_regulators[i];
		if (ri->desc.id == id)
			break;
	}

	if (i == ARRAY_SIZE(aat2870_regulators))
		return NULL;

	ri->enable_addr = AAT2870_LDO_EN;
	ri->enable_shift = id;
	ri->enable_mask = 0x1 << ri->enable_shift;

	ri->voltage_addr = id / 2 ? AAT2870_LDO_CD : AAT2870_LDO_AB;
	ri->voltage_shift = id % 2 ? 0 : 4;
	ri->voltage_mask = 0xF << ri->voltage_shift;

	return ri;
}

static int aat2870_regulator_probe(struct platform_device *pdev)
{
	struct aat2870_regulator *ri;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	int ret;

	ret = of_regulator_match(&pdev->dev, pdev->dev.of_node,
				 aat2870_regulator_matches,
				 ARRAY_SIZE(aat2870_regulator_matches));
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "Parsing of regulator node failed\n");

	for (int idx = 0; idx < ARRAY_SIZE(aat2870_regulator_matches); idx++) {
		if (!aat2870_regulator_matches[idx].of_node)
			continue;

		ri = aat2870_get_regulator(idx);
		if (!ri)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "Invalid device ID, %d\n", idx);

		ri->aat2870 = dev_get_drvdata(pdev->dev.parent);

		config.dev = &pdev->dev;
		config.driver_data = ri;

		rdev = devm_regulator_register(&pdev->dev, &ri->desc, &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "Failed to register regulator %s\n",
					     ri->desc.name);
	}

	return 0;
}

static const struct of_device_id aat2870_regulator_match_table[] = {
	{ .compatible = "skyworks,aat2870-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, aat2870_regulator_match_table);

static struct platform_driver aat2870_regulator_driver = {
	.driver = {
		.name	= "aat2870-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = aat2870_regulator_match_table,
	},
	.probe	= aat2870_regulator_probe,
};

static int __init aat2870_regulator_init(void)
{
	return platform_driver_register(&aat2870_regulator_driver);
}
subsys_initcall(aat2870_regulator_init);

static void __exit aat2870_regulator_exit(void)
{
	platform_driver_unregister(&aat2870_regulator_driver);
}
module_exit(aat2870_regulator_exit);

MODULE_DESCRIPTION("AnalogicTech AAT2870 Regulator");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jin Park <jinyoungp@nvidia.com>");
MODULE_ALIAS("platform:aat2870-regulator");
