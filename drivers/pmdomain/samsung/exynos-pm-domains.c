// SPDX-License-Identifier: GPL-2.0
//
// Exynos Generic power domain support.
//
// Copyright (c) 2012 Samsung Electronics Co., Ltd.
//		http://www.samsung.com
//
// Implementation of Exynos specific power domain control which is used in
// conjunction with runtime-pm. Support for both device-tree and non-device-tree
// based power domain support is included.

#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mfd/syscon.h>
#include <linux/pm_domain.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

struct exynos_pm_domain_config {
	/* Value for LOCAL_PWR_CFG and STATUS fields for each domain */
	u32 local_pwr_cfg;
	bool use_parent_regmap;
};

/*
 * Exynos specific wrapper around the generic power domain
 */
struct exynos_pm_domain {
	struct regmap *regmap;
	struct device *dev;
	struct generic_pm_domain pd;
	u32 local_pwr_cfg;
	u32 configuration_reg;
	u32 status_reg;
};

static int exynos_pd_power(struct generic_pm_domain *domain, bool power_on)
{
	struct exynos_pm_domain *pd;
	u32 timeout, pwr;
	int err;

	pd = container_of(domain, struct exynos_pm_domain, pd);

	pwr = power_on ? pd->local_pwr_cfg : 0;
	err = regmap_write(pd->regmap, pd->configuration_reg, pwr);
	if (err) {
		dev_err(pd->dev,
			"Regmap write for power domain %s %sable failed: %d\n",
			domain->name, power_on ? "en" : "dis", err);
		return err;
	}

	/* Wait max 1ms */
	timeout = 10;
	while (timeout-- > 0) {
		unsigned int val;

		err = regmap_read(pd->regmap, pd->status_reg, &val);
		if (err || ((val & pd->local_pwr_cfg) != pwr)) {
			cpu_relax();
			usleep_range(80, 100);
			continue;
		}

		break;
	}

	if (!timeout && !err)
		/* Only return timeout if no other error also occurred. */
		err = -ETIMEDOUT;
	if (err)
		dev_err(pd->dev, "Power domain %s %sable failed: %d\n",
			domain->name, power_on ? "en" : "dis", err);

	return err;
}

static int exynos_pd_power_on(struct generic_pm_domain *domain)
{
	return exynos_pd_power(domain, true);
}

static int exynos_pd_power_off(struct generic_pm_domain *domain)
{
	return exynos_pd_power(domain, false);
}

static const struct exynos_pm_domain_config exynos4210_cfg = {
	.local_pwr_cfg		= 0x7,
};

static const struct exynos_pm_domain_config exynos5433_cfg = {
	.local_pwr_cfg		= 0xf,
};

static const struct exynos_pm_domain_config gs101_cfg = {
	.local_pwr_cfg		= BIT(0),
	.use_parent_regmap	= true,
};

static const struct of_device_id exynos_pm_domain_of_match[] = {
	{
		.compatible = "google,gs101-pd",
		.data = &gs101_cfg,
	}, {
		.compatible = "samsung,exynos4210-pd",
		.data = &exynos4210_cfg,
	}, {
		.compatible = "samsung,exynos5433-pd",
		.data = &exynos5433_cfg,
	},
	{ },
};

static const char *exynos_get_domain_name(struct device *dev,
					  struct device_node *node)
{
	const char *name;

	if (of_property_read_string(node, "label", &name) < 0)
		name = kbasename(node->full_name);
	return devm_kstrdup_const(dev, name, GFP_KERNEL);
}

static int exynos_pd_probe(struct platform_device *pdev)
{
	const struct exynos_pm_domain_config *pm_domain_cfg;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct of_phandle_args child, parent;
	struct exynos_pm_domain *pd;
	struct resource *res;
	unsigned int val;
	int on, ret;

	pm_domain_cfg = of_device_get_match_data(dev);
	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->dev = dev;

	pd->pd.name = exynos_get_domain_name(dev, np);
	if (!pd->pd.name)
		return -ENOMEM;

	/*
	 * The resource typically points into the address space of the PMU and
	 * we have to consider two cases:
	 *   1) some implementations require a custom regmap (from PMU parent)
	 *   2) this driver might map the same addresses as the PMU driver
	 * Therefore, avoid using devm_platform_get_and_ioremap_resource() and
	 * instead use platform_get_resource() here, and below for case 1) use
	 * syscon_node_to_regmap() while for case 2) use devm_ioremap() to avoid
	 * conflicts due to address space overlap.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -ENXIO, "missing IO resources");

	if (pm_domain_cfg->use_parent_regmap) {
		pd->regmap = syscon_node_to_regmap(dev->parent->of_node);
		if (IS_ERR(pd->regmap))
			return dev_err_probe(dev, PTR_ERR(pd->regmap),
					     "failed to acquire PMU regmap");

		pd->configuration_reg = res->start;
		pd->status_reg = res->start;
	} else {
		void __iomem *base;

		const struct regmap_config reg_config = {
			.reg_bits = 32,
			.val_bits = 32,
			.reg_stride = 4,
			.use_relaxed_mmio = true,
			.max_register = (resource_size(res)
					 - reg_config.reg_stride),
		};

		base = devm_ioremap(dev, res->start, resource_size(res));
		if (!base)
			return dev_err_probe(dev, -ENOMEM,
					     "failed to ioremap PMU registers");

		pd->regmap = devm_regmap_init_mmio(dev, base, &reg_config);
		if (IS_ERR(pd->regmap))
			return dev_err_probe(dev, PTR_ERR(base),
					     "failed to init regmap");
	}

	pd->pd.power_off = exynos_pd_power_off;
	pd->pd.power_on = exynos_pd_power_on;
	pd->local_pwr_cfg = pm_domain_cfg->local_pwr_cfg;
	pd->configuration_reg += 0;
	pd->status_reg += 4;

	/*
	 * Some Samsung platforms with bootloaders turning on the splash-screen
	 * and handing it over to the kernel, requires the power-domains to be
	 * reset during boot.
	 */
	if (IS_ENABLED(CONFIG_ARM) &&
	    of_device_is_compatible(np, "samsung,exynos4210-pd"))
		exynos_pd_power_off(&pd->pd);

	ret = regmap_read(pd->regmap, pd->status_reg, &val);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read status");

	on = val & pd->local_pwr_cfg;

	pm_genpd_init(&pd->pd, NULL, !on);
	ret = of_genpd_add_provider_simple(np, &pd->pd);

	if (ret == 0 && of_parse_phandle_with_args(np, "power-domains",
				      "#power-domain-cells", 0, &parent) == 0) {
		child.np = np;
		child.args_count = 0;

		if (of_genpd_add_subdomain(&parent, &child))
			pr_warn("%pOF failed to add subdomain: %pOF\n",
				parent.np, child.np);
		else
			pr_info("%pOF has as child subdomain: %pOF.\n",
				parent.np, child.np);
	}

	pm_runtime_enable(dev);
	return ret;
}

static struct platform_driver exynos_pd_driver = {
	.probe	= exynos_pd_probe,
	.driver	= {
		.name		= "exynos-pd",
		.of_match_table	= exynos_pm_domain_of_match,
		.suppress_bind_attrs = true,
	}
};

static __init int exynos4_pm_init_power_domain(void)
{
	return platform_driver_register(&exynos_pd_driver);
}
core_initcall(exynos4_pm_init_power_domain);
