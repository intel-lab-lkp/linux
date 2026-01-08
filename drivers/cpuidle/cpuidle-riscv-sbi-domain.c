// SPDX-License-Identifier: GPL-2.0-only
/*
 * PM domains for CPUs via genpd - managed by cpuidle-riscv-sbi.
 *
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#define pr_fmt(fmt) "cpuidle-riscv-sbi-domain: " fmt

#include <linux/device.h>
#include <linux/pm_domain.h>

#include "cpuidle-riscv-sbi.h"
#include "dt_idle_genpd.h"

struct sbi_pd_provider {
	struct list_head link;
	struct device_node *node;
};

static LIST_HEAD(sbi_pd_providers);

static int sbi_cpuidle_pd_power_off(struct generic_pm_domain *pd)
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

static int sbi_pd_init(struct device_node *np, bool use_osi)
{
	struct generic_pm_domain *pd;
	struct sbi_pd_provider *pd_provider;
	struct dev_power_governor *pd_gov;
	int ret = -ENOMEM;

	pd = dt_idle_pd_alloc(np, sbi_dt_parse_state_node);
	if (!pd)
		goto out;

	pd_provider = kzalloc(sizeof(*pd_provider), GFP_KERNEL);
	if (!pd_provider)
		goto free_pd;

	pd->flags |= GENPD_FLAG_IRQ_SAFE | GENPD_FLAG_CPU_DOMAIN;

	/* Allow power off when OSI is available. */
	if (use_osi)
		pd->power_off = sbi_cpuidle_pd_power_off;
	else
		pd->flags |= GENPD_FLAG_ALWAYS_ON;

	/* Use governor for CPU PM domains if it has some states to manage. */
	pd_gov = pd->states ? &pm_domain_cpu_gov : NULL;

	ret = pm_genpd_init(pd, pd_gov, false);
	if (ret)
		goto free_pd_prov;

	ret = of_genpd_add_provider_simple(np, pd);
	if (ret)
		goto remove_pd;

	pd_provider->node = of_node_get(np);
	list_add(&pd_provider->link, &sbi_pd_providers);

	pr_debug("init PM domain %s\n", pd->name);
	return 0;

remove_pd:
	pm_genpd_remove(pd);
free_pd_prov:
	kfree(pd_provider);
free_pd:
	dt_idle_pd_free(pd);
out:
	pr_err("failed to init PM domain ret=%d %pOF\n", ret, np);
	return ret;
}

static void sbi_pd_remove(void)
{
	struct sbi_pd_provider *pd_provider, *it;
	struct generic_pm_domain *genpd;

	list_for_each_entry_safe(pd_provider, it, &sbi_pd_providers, link) {
		of_genpd_del_provider(pd_provider->node);

		genpd = of_genpd_remove_last(pd_provider->node);
		if (!IS_ERR(genpd))
			kfree(genpd);

		of_node_put(pd_provider->node);
		list_del(&pd_provider->link);
		kfree(pd_provider);
	}
}

static int sbi_genpd_probe(struct device_node *np, bool use_osi)
{
	int ret = 0, pd_count = 0;

	if (!np)
		return -ENODEV;

	/*
	 * Parse child nodes for the "#power-domain-cells" property and
	 * initialize a genpd/genpd-of-provider pair when it's found.
	 */
	for_each_child_of_node_scoped(np, node) {
		if (!of_property_present(node, "#power-domain-cells"))
			continue;

		ret = sbi_pd_init(node, use_osi);
		if (ret)
			goto remove_pd;

		pd_count++;
	}

	/* Bail out if not using the hierarchical CPU topology. */
	if (!pd_count)
		goto no_pd;

	/* Link genpd masters/subdomains to model the CPU topology. */
	ret = dt_idle_pd_init_topology(np);
	if (ret)
		goto remove_pd;

	return 0;

remove_pd:
	sbi_pd_remove();
	pr_err("failed to create CPU PM domains ret=%d\n", ret);
no_pd:
	return ret;
}

static int __init riscv_sbi_idle_init_domains(void)
{
	bool use_osi = true;
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

	/* Populate generic power domains from DT nodes */
	struct device_node *pds_node __free(device_node) =
			of_find_node_by_path("/cpus/power-domains");
	if (!pds_node)
		return 0;

	return sbi_genpd_probe(pds_node, use_osi);
}
core_initcall(riscv_sbi_idle_init_domains);
