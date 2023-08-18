// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2023 NXP Semiconductor, Inc.
 */

#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>

struct regulator_power_domain {
	struct generic_pm_domain genpd;
	struct device *dev;
	struct regulator *reg;
	int idx;
};

#define to_regulator_power_domain(_genpd) container_of(_genpd, struct regulator_power_domain, genpd)

static int regulator_power_domain_on(struct generic_pm_domain *genpd)
{
	struct regulator_power_domain *domain = to_regulator_power_domain(genpd);

	if (IS_ERR_OR_NULL(domain->reg))
		return -ENODEV;

	return regulator_enable(domain->reg);
}

static int regulator_power_domain_off(struct generic_pm_domain *genpd)
{
	struct regulator_power_domain *domain = to_regulator_power_domain(genpd);

	if (IS_ERR_OR_NULL(domain->reg))
		return -ENODEV;

	return regulator_disable(domain->reg);
}

static struct generic_pm_domain *
regulator_power_domain_xlate(struct of_phandle_args *spec, void *data)
{
	struct generic_pm_domain *domain = ERR_PTR(-ENOENT);
	struct genpd_onecell_data *pd_data = data;
	struct regulator_power_domain *rpd;
	unsigned int i;

	for (i = 0; i < pd_data->num_domains; i++) {
		rpd = to_regulator_power_domain(pd_data->domains[i]);
		if (rpd->idx == spec->args[0]) {
			domain = &rpd->genpd;
			break;
		}
	}

	return domain;
}

static int regulator_power_domain_probe(struct platform_device *pdev)
{
	struct regulator_power_domain *gpd;
	struct genpd_onecell_data *pd_data;
	struct device *dev = &pdev->dev;
	struct generic_pm_domain **pds;
	char name[16];
	u32 num;
	int i;

	if (of_property_read_u32(dev->of_node, "regulator-number", &num))
		return -ENODEV;

	if (num < 1)
		return -EINVAL;

	/* Limit the regulator count to <=100 */
	if (num > 100)
		num = 100;

	pd_data = devm_kzalloc(dev, sizeof(*pd_data), GFP_KERNEL);
	pds = devm_kcalloc(dev, num, sizeof(*pds), GFP_KERNEL);
	gpd = devm_kcalloc(dev, num, sizeof(*gpd), GFP_KERNEL);

	if (!pds || !pd_data || !gpd)
		return -ENOMEM;

	for (i = 0; i < num; i++) {
		bool is_off = 1;

		snprintf(name, 16, "regulator-%d", i);
		gpd->reg = devm_regulator_get_optional(dev, name);

		/* Let the initial pd state as is as the regulator's */
		if (!(IS_ERR_OR_NULL(gpd->reg)))
			is_off = (regulator_is_enabled(gpd->reg) > 0) ? 0 : 1;

		pm_genpd_init(&gpd->genpd, NULL, is_off);

		gpd->genpd.power_off = regulator_power_domain_off;
		gpd->genpd.power_on = regulator_power_domain_on;
		gpd->genpd.name = dev_name(dev);
		gpd->dev = dev;
		gpd->idx = i;
		pds[i] = &gpd->genpd;
		gpd++;
	}

	pd_data->domains = pds;
	pd_data->num_domains = num;
	pd_data->xlate = regulator_power_domain_xlate;
	platform_set_drvdata(pdev, pd_data);

	return of_genpd_add_provider_onecell(dev->of_node, pd_data);
}

static int regulator_power_domain_remove(struct platform_device *pdev)
{
	struct genpd_onecell_data *pd_data = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int i;

	of_genpd_del_provider(np);
	for (i = 0; i < pd_data->num_domains; i++)
		pm_genpd_remove(pd_data->domains[i]);

	return 0;
}

static const struct of_device_id regulator_power_domain_dt_ids[] = {
	{ .compatible = "regulator-power-domain"},
	{ }
};

static struct platform_driver regulator_power_domain_driver = {
	.driver = {
		.name = "regulator2pd",
		.of_match_table = regulator_power_domain_dt_ids,
	},
	.probe = regulator_power_domain_probe,
	.remove = regulator_power_domain_remove,
};
builtin_platform_driver(regulator_power_domain_driver)

MODULE_AUTHOR("Shenwei Wang <shenwei.wang@nxp.com>");
MODULE_DESCRIPTION("regulator power domain driver");
MODULE_LICENSE("GPL");
