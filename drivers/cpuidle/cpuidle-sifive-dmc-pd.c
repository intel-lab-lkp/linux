// SPDX-License-Identifier: GPL-2.0-only
/*
 * SiFive CPUIDLE SBI PD driver
 */

#define pr_fmt(fmt) "sifive_cpuidle_sbi_pd: " fmt

#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "cpuidle-riscv-sbi.h"
#include "dt_idle_genpd.h"

static void sifive_dmc_remove(struct platform_device *pdev)
{
	struct generic_pm_domain *pd = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	pm_runtime_disable(dev);
	of_genpd_del_provider(dev->of_node);
	pm_genpd_remove(pd);
	dt_idle_pd_free(pd);
}

static int sifive_dmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct generic_pm_domain *pd;
	struct of_phandle_args child, parent;
	int ret = -ENOMEM;

	pd = dt_idle_pd_alloc(np, sbi_dt_parse_state_node);
	if (!pd)
		goto fail;

	pd->flags |= GENPD_FLAG_IRQ_SAFE | GENPD_FLAG_CPU_DOMAIN;
	pd->power_off = sbi_cpuidle_pd_power_off;

	ret = pm_genpd_init(pd, &pm_domain_cpu_gov, false);
	if (ret)
		goto free_pd;

	ret = of_genpd_add_provider_simple(np, pd);
	if (ret)
		goto remove_pd;

	if (of_parse_phandle_with_args(np, "power-domains",
				       "#power-domain-cells", 0,
				       &parent) == 0) {
		child.np = np;
		child.args_count = 0;

		if (of_genpd_add_subdomain(&parent, &child))
			pr_warn("%pOF failed to add subdomain: %pOF\n",
				parent.np, child.np);
		else
			pr_debug("%pOF has a child subdomain: %pOF.\n",
				 parent.np, child.np);
	}

	platform_set_drvdata(pdev, pd);
	pm_runtime_enable(dev);
	pr_info("%s create success\n", pd->name);
	return 0;

remove_pd:
	pm_genpd_remove(pd);
free_pd:
	dt_idle_pd_free(pd);
fail:
	pr_info("%s create fail\n", pd->name);

	return ret;
}

static const struct of_device_id sifive_dmc_of_match[] = {
	{ .compatible = "sifive,tmc1", },
	{ .compatible = "sifive,tmc0", },
	{ .compatible = "sifive,smc1", },
	{ .compatible = "sifive,smc0", },
	{ .compatible = "sifive,cmc2", },
	{}
};

static struct platform_driver sifive_dmc_driver = {
	.probe = sifive_dmc_probe,
	.remove = sifive_dmc_remove,
	.driver = {
		.name = "sifive_dmc",
		.of_match_table = sifive_dmc_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init sifive_dmc_init(void)
{
	return platform_driver_register(&sifive_dmc_driver);
}
arch_initcall(sifive_dmc_init);
