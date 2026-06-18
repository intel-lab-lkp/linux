// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Linus Walleij <linusw@kernel.org>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/slab.h>

struct db8500_regulator_info {
	struct regulator_desc desc;
	struct regulator_init_data init_data;
	struct device pd_dev;
	bool enabled;
};

struct db8500_regulator_match {
	const char *name;
	const char *supply_name;
	const char *constraint_name;
	int fixed_uV;
	bool always_on;
};

static const struct db8500_regulator_match db8500_regulator_matches[] = {
	{
		.name = "db8500_vape",
		.supply_name = "db8500-vape",
		.constraint_name = "db8500-vape",
		.always_on = true,
	}, {
		.name = "db8500_vsmps2",
		.supply_name = "db8500-vsmps2",
		.constraint_name = "db8500-vsmps2",
		.fixed_uV = 1800000,
	},
};

static int db8500_regulator_enable(struct regulator_dev *rdev)
{
	struct db8500_regulator_info *info = rdev_get_drvdata(rdev);
	int ret;

	ret = pm_runtime_resume_and_get(&info->pd_dev);
	if (ret)
		return ret;

	info->enabled = true;
	return 0;
}

static int db8500_regulator_disable(struct regulator_dev *rdev)
{
	struct db8500_regulator_info *info = rdev_get_drvdata(rdev);
	int ret;

	ret = pm_runtime_put_sync_suspend(&info->pd_dev);
	if (ret)
		return ret;

	info->enabled = false;
	return 0;
}

static int db8500_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct db8500_regulator_info *info = rdev_get_drvdata(rdev);

	return info->enabled;
}

static int db8500_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct db8500_regulator_info *info = rdev_get_drvdata(rdev);

	if (!info->desc.fixed_uV)
		return -EINVAL;

	return info->desc.fixed_uV;
}

static const struct regulator_ops db8500_regulator_ops = {
	.enable = db8500_regulator_enable,
	.disable = db8500_regulator_disable,
	.is_enabled = db8500_regulator_is_enabled,
	.get_voltage = db8500_regulator_get_voltage,
};

static void db8500_regulator_release(struct device *dev)
{
}

static void db8500_regulator_cleanup(void *data)
{
	struct db8500_regulator_info *info = data;

	pm_runtime_disable(&info->pd_dev);
	dev_pm_domain_detach(&info->pd_dev, true);
	put_device(&info->pd_dev);
}

static const struct db8500_regulator_match *
db8500_regulator_match(struct device_node *np)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(db8500_regulator_matches); i++) {
		if (of_node_name_eq(np, db8500_regulator_matches[i].name))
			return &db8500_regulator_matches[i];
	}

	return NULL;
}

static int db8500_regulator_register(struct platform_device *pdev,
				     struct device_node *np)
{
	const struct db8500_regulator_match *match;
	struct regulator_config config = { };
	struct db8500_regulator_info *info;
	struct of_phandle_args pd_args;
	struct regulator_dev *rdev;
	const char *cells = "#power-domain-cells";
	int ret;

	match = db8500_regulator_match(np);
	if (!match)
		return 0;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	device_initialize(&info->pd_dev);
	info->pd_dev.parent = &pdev->dev;
	info->pd_dev.of_node = np;
	info->pd_dev.release = db8500_regulator_release;
	ret = dev_set_name(&info->pd_dev, "%s-pd", match->name);
	if (ret)
		goto put_device;

	ret = of_parse_phandle_with_args(np, "power-domains", cells, 0, &pd_args);
	if (ret)
		goto put_device;

	ret = of_genpd_add_device(&pd_args, &info->pd_dev);
	of_node_put(pd_args.np);
	if (ret)
		goto put_device;

	pm_runtime_enable(&info->pd_dev);
	ret = devm_add_action_or_reset(&pdev->dev, db8500_regulator_cleanup, info);
	if (ret)
		return ret;

	info->init_data.constraints.name = match->constraint_name;
	info->init_data.constraints.valid_ops_mask = REGULATOR_CHANGE_STATUS;
	info->init_data.constraints.always_on = match->always_on;

	info->desc.name = match->supply_name;
	info->desc.of_match = match->name;
	info->desc.ops = &db8500_regulator_ops;
	info->desc.type = REGULATOR_VOLTAGE;
	info->desc.owner = THIS_MODULE;
	info->desc.fixed_uV = match->fixed_uV;
	config.dev = &pdev->dev;
	config.init_data = &info->init_data;
	config.driver_data = info;
	config.of_node = np;
	rdev = devm_regulator_register(&pdev->dev, &info->desc, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	return 0;

put_device:
	put_device(&info->pd_dev);
	return ret;
}

static int db8500_regulator_probe(struct platform_device *pdev)
{
	struct device_node *np;
	int ret;

	for_each_available_child_of_node(pdev->dev.of_node, np) {
		ret = db8500_regulator_register(pdev, np);
		if (ret) {
			of_node_put(np);
			return ret;
		}
	}

	return 0;
}

static const struct of_device_id db8500_regulator_match_table[] = {
	{ .compatible = "stericsson,db8500-prcmu-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, db8500_regulator_match_table);

static struct platform_driver db8500_regulator_driver = {
	.driver = {
		.name = "db8500-prcmu-regulators",
		.of_match_table = db8500_regulator_match_table,
	},
	.probe = db8500_regulator_probe,
};
module_platform_driver(db8500_regulator_driver);

MODULE_AUTHOR("Linus Walleij <linusw@kernel.org>");
MODULE_DESCRIPTION("DB8500 power-domain regulator driver");
MODULE_LICENSE("GPL");
