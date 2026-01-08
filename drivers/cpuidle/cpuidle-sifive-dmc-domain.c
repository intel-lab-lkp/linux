// SPDX-License-Identifier: GPL-2.0-only
/*
 * SiFive CPUIDLE DMC driver
 */

#define pr_fmt(fmt) "sifive_dmc_cpuidle_domain: " fmt

#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "cpuidle-riscv-sbi.h"
#include "dt_idle_genpd.h"

static bool use_osi = true;

static int sifive_cpuidle_dmc_power_off(struct generic_pm_domain *pd)
{
	struct genpd_power_state *state = &pd->states[pd->state_idx];
	u32 *pd_state;

	if (!state->data)
		return 0;

	/* OSI mode is enabled, set the corresponding domain state. */
	pd_state = state->data;
	sbi_set_domain_state(*pd_state);

	return 0;
}

static int sifive_dmc_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct of_phandle_args child, parent;
	struct device *dev = &pdev->dev;
	struct generic_pm_domain *pd;
	int ret = -ENOMEM;

	pd = dt_idle_pd_alloc(np, sbi_dt_parse_state_node);
	if (!pd)
		goto out;

	pd->flags |= GENPD_FLAG_IRQ_SAFE | GENPD_FLAG_CPU_DOMAIN;
	if (use_osi)
		pd->power_off = sifive_cpuidle_dmc_power_off;
	else
		pd->flags |= GENPD_FLAG_ALWAYS_ON;

	ret = pm_genpd_init(pd, &pm_domain_cpu_gov, false);
	if (ret)
		goto free_pd;

	ret = of_genpd_add_provider_simple(np, pd);
	if (ret)
		goto remove_pd;

	if (!of_parse_phandle_with_args(np, "power-domains", "#power-domain-cells", 0, &parent)) {
		child.np = np;
		child.args_count = 0;

		ret = of_genpd_add_subdomain(&parent, &child);
		of_node_put(parent.np);
		if (ret) {
			pr_err("%pOF failed to add subdomain: %pOF\n", parent.np, child.np);
			goto remove_pd;
		}
	}

	pm_runtime_enable(dev);
	pr_info("init PM domain %s\n", dev_name(dev));
	return 0;

remove_pd:
	pm_genpd_remove(pd);
free_pd:
	dt_idle_pd_free(pd);
out:
	pr_err("failed to init PM domain ret=%d %pOF\n", ret, np);
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
	.driver = {
		.name = "sifive_dmc",
		.of_match_table = sifive_dmc_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init sifive_dmc_domain_init(void)
{
	int cpu;

	/* Detect OSI support based on CPU DT nodes */
	for_each_possible_cpu(cpu) {
		struct device_node *np __free(device_node) = of_cpu_device_node_get(cpu);
		if (np &&
		    of_property_present(np, "power-domains") &&
		    of_property_present(np, "power-domain-names")) {
			continue;
		} else {
			use_osi = false;
			break;
		}
	}

	sbi_set_osi_mode(use_osi);

	/* Only probe the DMCs when OSI supported */
	return platform_driver_register(&sifive_dmc_driver);
}
core_initcall(sifive_dmc_domain_init);
