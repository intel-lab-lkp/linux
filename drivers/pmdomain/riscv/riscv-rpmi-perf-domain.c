// SPDX-License-Identifier: GPL-2.0
/*
 * RISC-V RPMI performance domains as generic power domains
 *
 * Copyright (C) 2026 Shanghai StarFive Technology Co., Ltd.
 *
 * A device that is not a CPU names its RPMI performance domain through
 * "power-domains". Each domain is registered here as a generic power domain
 * whose performance states are the levels the platform microcontroller
 * advertises, so the device's driver drives it through the OPP library like
 * any other power domain with performance states, and genpd settles on the
 * highest state any device in the domain asks for.
 *
 * Performance state 0 means that no device in the domain asks for anything,
 * while 0 is a valid RPMI level index. State N is therefore level index
 * N - 1, and state 0 leaves the domain at whatever level it is running.
 *
 * genpd runs the domain at the highest state asked for, which is the highest
 * level index. That is the level to run at: the specification makes the
 * level index the platform's performance scale, which its limit services
 * also order levels by, so a higher index is a higher level whatever the
 * indices in between.
 *
 * A domain that CPUs name through "performance-domains" belongs to cpufreq,
 * which sets its level directly, so no device is attached to it as well.
 *
 * The RPMI protocol and the domain enumeration live in the performance
 * service group core, which owns the mailbox channel and creates the device
 * this driver binds to.
 */

#include <linux/firmware/riscv/riscv-rpmi-performance.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/units.h>

struct rpmi_perf_genpd {
	struct generic_pm_domain genpd;
	struct rpmi_perf_domain *domain;
	bool cpu_domain;
};

struct rpmi_perf_genpd_provider {
	struct genpd_onecell_data data;
	struct rpmi_perf_genpd *pds;
};

#define to_rpmi_perf_genpd(gpd) container_of(gpd, struct rpmi_perf_genpd, genpd)

static int rpmi_perf_genpd_set_performance_state(struct generic_pm_domain *genpd,
						 unsigned int state)
{
	struct rpmi_perf_genpd *pd = to_rpmi_perf_genpd(genpd);
	int ret;

	/* No device in the domain asks for a level, so leave it where it is. */
	if (!state)
		return 0;

	ret = rpmi_perf_domain_set_level(pd->domain, state - 1);
	if (ret)
		dev_warn(&genpd->dev, "failed to set level %u: %d\n",
			 state - 1, ret);

	return ret;
}

/*
 * The levels come from the platform microcontroller, so there is no
 * operating-points-v2 table in the device tree. Hand each device that
 * attaches an operating point per level instead, carrying the level's
 * frequency and its performance state.
 */
static int rpmi_perf_genpd_attach_dev(struct generic_pm_domain *genpd,
				      struct device *dev)
{
	struct rpmi_perf_genpd *pd = to_rpmi_perf_genpd(genpd);
	struct dev_pm_opp_data data = {};
	struct rpmi_perf_level level;
	u32 idx;
	int ret;

	/* cpufreq and genpd would each set the level behind the other's back. */
	if (pd->cpu_domain) {
		dev_err(dev, "%s is the performance domain of a CPU\n", genpd->name);
		return -EBUSY;
	}

	/* A domain whose level cannot be set has no state to offer. */
	if (!rpmi_perf_domain_can_set_level(pd->domain))
		return 0;

	for (idx = 0; idx < rpmi_perf_domain_level_count(pd->domain); idx++) {
		ret = rpmi_perf_domain_level_info(pd->domain, idx, &level);
		if (ret)
			goto err;

		/* Frequency from RPMI is in kHz */
		data.freq = (unsigned long)level.clock_freq * HZ_PER_KHZ;
		data.level = level.index + 1;

		ret = dev_pm_opp_add_dynamic(dev, &data);
		if (ret)
			goto err;
	}

	return 0;

err:
	dev_warn(dev, "failed to add operating points for %s: %d\n",
		 genpd->name, ret);
	dev_pm_opp_remove_all_dynamic(dev);

	return ret;
}

static void rpmi_perf_genpd_detach_dev(struct generic_pm_domain *genpd,
				       struct device *dev)
{
	struct rpmi_perf_genpd *pd = to_rpmi_perf_genpd(genpd);

	if (rpmi_perf_domain_can_set_level(pd->domain))
		dev_pm_opp_remove_all_dynamic(dev);
}

/* Mark the domains that CPUs name through "performance-domains". */
static void rpmi_perf_genpd_mark_cpu_domains(struct device_node *np,
					     struct rpmi_perf_genpd *pds,
					     u32 num_domains)
{
	struct of_phandle_args args;
	struct device_node *cpu_np;

	for_each_of_cpu_node(cpu_np) {
		if (of_parse_phandle_with_args(cpu_np, "performance-domains",
					       "#performance-domain-cells", 0,
					       &args))
			continue;

		if (args.np == np && args.args_count >= 1 &&
		    args.args[0] < num_domains)
			pds[args.args[0]].cpu_domain = true;

		of_node_put(args.np);
	}
}

static int rpmi_perf_genpd_probe(struct platform_device *pdev)
{
	struct rpmi_perf **mpxy_perf = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct rpmi_perf_genpd_provider *provider;
	struct generic_pm_domain **domains;
	u32 i, num_domains, registered = 0;
	int ret;

	if (!mpxy_perf || !*mpxy_perf)
		return -EINVAL;

	num_domains = rpmi_perf_num_domains(*mpxy_perf);

	provider = devm_kzalloc(dev, sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;

	provider->pds = devm_kcalloc(dev, num_domains, sizeof(*provider->pds),
				     GFP_KERNEL);
	if (!provider->pds)
		return -ENOMEM;

	domains = devm_kcalloc(dev, num_domains, sizeof(*domains), GFP_KERNEL);
	if (!domains)
		return -ENOMEM;

	rpmi_perf_genpd_mark_cpu_domains(dev_of_node(dev->parent), provider->pds,
					 num_domains);

	for (i = 0; i < num_domains; i++) {
		struct rpmi_perf_genpd *pd = &provider->pds[i];

		/*
		 * A domain that could not be enumerated is left out, the way
		 * the device power provider leaves out a domain it cannot use.
		 * A device that names it then never gets its power domain.
		 */
		pd->domain = rpmi_perf_domain_by_id(*mpxy_perf, i);
		if (!pd->domain)
			continue;

		pd->genpd.name = rpmi_perf_domain_name(pd->domain);
		/*
		 * A performance domain has no off state, its operating points
		 * come from the platform microcontroller rather than from the
		 * device tree, and its name is not guaranteed to be unique.
		 */
		pd->genpd.flags = GENPD_FLAG_ALWAYS_ON |
				  GENPD_FLAG_OPP_TABLE_FW |
				  GENPD_FLAG_DEV_NAME_FW;
		pd->genpd.set_performance_state = rpmi_perf_genpd_set_performance_state;
		pd->genpd.attach_dev = rpmi_perf_genpd_attach_dev;
		pd->genpd.detach_dev = rpmi_perf_genpd_detach_dev;

		ret = pm_genpd_init(&pd->genpd, NULL, false);
		if (ret)
			goto err;

		domains[i] = &pd->genpd;
		registered++;
	}

	provider->data.domains = domains;
	provider->data.num_domains = num_domains;

	/* The domains belong to the node the core was probed from. */
	ret = of_genpd_add_provider_onecell(dev_of_node(dev->parent),
					    &provider->data);
	if (ret)
		goto err;

	platform_set_drvdata(pdev, provider);

	dev_info(dev, "%u MPXY performance domains registered as power domains\n",
		 registered);

	return 0;

err:
	while (i--) {
		if (domains[i])
			pm_genpd_remove(domains[i]);
	}

	return dev_err_probe(dev, ret, "failed to register power domains\n");
}

static void rpmi_perf_genpd_remove(struct platform_device *pdev)
{
	struct rpmi_perf_genpd_provider *provider = platform_get_drvdata(pdev);
	u32 i;

	of_genpd_del_provider(dev_of_node(pdev->dev.parent));

	for (i = 0; i < provider->data.num_domains; i++) {
		if (provider->data.domains[i])
			pm_genpd_remove(provider->data.domains[i]);
	}
}

static struct platform_driver rpmi_perf_genpd_platdrv = {
	.driver = {
		.name = "riscv-rpmi-perf-domain",
	},
	.probe = rpmi_perf_genpd_probe,
	.remove = rpmi_perf_genpd_remove,
};

module_platform_driver(rpmi_perf_genpd_platdrv);

MODULE_ALIAS("platform:riscv-rpmi-perf-domain");
MODULE_AUTHOR("Joshua Yeong <joshua.yeong@starfivetech.com>");
MODULE_DESCRIPTION("Power domains for the RISC-V RPMI performance service");
MODULE_LICENSE("GPL");
