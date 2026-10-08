// SPDX-License-Identifier: GPL-2.0
/*
 * RISC-V RPMI Based CPUFreq Driver
 *
 * Copyright (C) 2026 Shanghai StarFive Technology Co., Ltd.
 *
 * Drives the performance domains that CPUs share. The RPMI protocol and the
 * domain enumeration live in the performance service group core, which owns
 * the mailbox channel and creates the device this driver binds to.
 */

#define pr_fmt(fmt) "riscv-rpmi-cpufreq: " fmt

#include <linux/cpufreq.h>
#include <linux/energy_model.h>
#include <linux/firmware/riscv/riscv-rpmi-performance.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

struct rpmi_perf_cpufreq_data {
	int nr_opp;
	struct device *cpu_dev;
	struct rpmi_perf_domain *domain;
};

static int rpmi_perf_set_target_index(struct cpufreq_policy *policy, unsigned int index)
{
	struct rpmi_perf_cpufreq_data *data = policy->driver_data;
	u32 level;
	int ret;

	/*
	 * cpufreq indexes its own frequency table, which is not the same thing
	 * as an RPMI level index. Go through the frequency so that the two
	 * only have to agree on what they mean, not on how they are numbered.
	 */
	ret = rpmi_perf_domain_freq_to_level(data->domain,
					     policy->freq_table[index].frequency,
					     &level);
	if (ret)
		return ret;

	if (rpmi_perf_domain_has_fast_channel(data->domain))
		return rpmi_perf_domain_set_level_fast(data->domain, level);

	return rpmi_perf_domain_set_level(data->domain, level);
}

static unsigned int rpmi_perf_fast_switch(struct cpufreq_policy *policy,
					  unsigned int target_freq)
{
	struct rpmi_perf_cpufreq_data *data = policy->driver_data;
	u32 level;

	if (rpmi_perf_domain_freq_to_level(data->domain, target_freq, &level))
		return 0;

	if (rpmi_perf_domain_set_level_fast(data->domain, level))
		return 0;

	return target_freq;
}

static unsigned int rpmi_perf_get_rate(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);
	struct rpmi_perf_cpufreq_data *data;
	u32 cpufreq, level;

	if (!policy)
		return 0;

	data = policy->driver_data;

	if (rpmi_perf_domain_get_level(data->domain, &level))
		return 0;

	if (rpmi_perf_domain_level_to_freq(data->domain, level, &cpufreq))
		return 0;

	return cpufreq;
}

static int rpmi_perf_init(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *freq_table;
	struct rpmi_perf_cpufreq_data *data;
	struct rpmi_perf_domain *domain;
	struct platform_device *pdev = cpufreq_get_driver_data();
	struct rpmi_perf **mpxy_perf = dev_get_platdata(&pdev->dev);
	struct of_phandle_args args;
	int ret, nr_opp;
	struct device *cpu_dev;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("failed to get cpu%d device\n", policy->cpu);
		return -ENODEV;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = of_perf_domain_get_sharing_cpumask(policy->cpu,
						 "performance-domains",
						 "#performance-domain-cells",
						 policy->cpus, &args);
	if (ret) {
		dev_err(cpu_dev, "%s: failed to get performance domain info: %d\n",
			__func__, ret);
		goto out_free_priv;
	}

	/* A domain ID only means something to the provider the CPU names. */
	if (args.np != dev_of_node(pdev->dev.parent)) {
		dev_err(cpu_dev, "performance domain of %pOF, not of %pOF\n",
			args.np, dev_of_node(pdev->dev.parent));
		of_node_put(args.np);
		ret = -ENODEV;
		goto out_free_priv;
	}

	domain = rpmi_perf_domain_by_id(*mpxy_perf, args.args[0]);
	of_node_put(args.np);
	if (!domain) {
		dev_err(cpu_dev, "performance domain %u is not usable\n",
			args.args[0]);
		ret = -EINVAL;
		goto out_free_priv;
	}

	ret = rpmi_perf_domain_opps_add(domain, cpu_dev);
	if (ret) {
		dev_warn(cpu_dev, "failed to add opps to the device\n");
		goto out_free_priv;
	}

	nr_opp = dev_pm_opp_get_opp_count(cpu_dev);
	if (nr_opp <= 0) {
		dev_err(cpu_dev, "performance domain has no operating points\n");
		ret = -ENODEV;
		goto out_free_opp;
	}

	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &freq_table);
	if (ret) {
		dev_err(cpu_dev, "failed to init cpufreq table: %d\n", ret);
		goto out_free_opp;
	}

	data->cpu_dev = cpu_dev;
	data->nr_opp = nr_opp;
	data->domain = domain;

	/* Allow DVFS request for any domain from any CPU */
	policy->dvfs_possible_from_any_cpu = true;
	policy->driver_data = data;
	policy->freq_table = freq_table;

	policy->cpuinfo.transition_latency =
		rpmi_perf_domain_trans_latency_us(domain) * 1000;
	policy->fast_switch_possible = rpmi_perf_domain_has_fast_channel(domain);

	return 0;

out_free_opp:
	dev_pm_opp_remove_all_dynamic(cpu_dev);

out_free_priv:
	kfree(data);

	return ret;
}

static void rpmi_perf_exit(struct cpufreq_policy *policy)
{
	struct rpmi_perf_cpufreq_data *data = policy->driver_data;

	dev_pm_opp_free_cpufreq_table(data->cpu_dev, &policy->freq_table);
	dev_pm_opp_remove_all_dynamic(data->cpu_dev);
	kfree(data);
}

static int __maybe_unused
rpmi_perf_get_cpu_power(struct device *cpu_dev, unsigned long *uW,
			unsigned long *kHz)
{
	struct rpmi_perf_cpufreq_data *data;
	struct rpmi_perf_level level;
	struct cpufreq_policy *policy;
	u32 idx, count;

	policy = cpufreq_cpu_get_raw(cpu_dev->id);
	if (!policy)
		return -EINVAL;

	data = policy->driver_data;
	count = rpmi_perf_domain_level_count(data->domain);

	/* The levels are in ascending order of frequency. */
	for (idx = 0; idx < count; idx++) {
		if (rpmi_perf_domain_level_info(data->domain, idx, &level))
			return -EINVAL;

		if (level.clock_freq < *kHz)
			continue;

		*uW = level.power_cost;
		*kHz = level.clock_freq;
		return 0;
	}

	/* No level at or above *kHz: there is no state to describe. */
	return -EINVAL;
}

static void rpmi_perf_register_em(struct cpufreq_policy *policy)
{
	struct em_data_callback em_cb = EM_DATA_CB(rpmi_perf_get_cpu_power);
	struct rpmi_perf_cpufreq_data *data = policy->driver_data;

	em_dev_register_perf_domain(get_cpu_device(policy->cpu), data->nr_opp,
				    &em_cb, policy->cpus, true);
}

static struct cpufreq_driver  rpmi_perf_cpufreq_driver = {
	.name = "rpmi-cpufreq",
	.flags = CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
		 CPUFREQ_NEED_INITIAL_FREQ_CHECK |
		 CPUFREQ_IS_COOLING_DEV,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = rpmi_perf_set_target_index,
	.fast_switch = rpmi_perf_fast_switch,
	.get = rpmi_perf_get_rate,
	.init = rpmi_perf_init,
	.exit = rpmi_perf_exit,
	.register_em = rpmi_perf_register_em,
};

static int rpmi_cpufreq_probe(struct platform_device *pdev)
{
	struct rpmi_perf **mpxy_perf = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	int ret;

	if (!mpxy_perf || !*mpxy_perf)
		return -EINVAL;

	/*
	 * There is one cpufreq driver for the whole system, so only one
	 * provider can drive the CPUs. Refuse another one rather than point
	 * the registered driver at its domains.
	 */
	if (rpmi_perf_cpufreq_driver.driver_data)
		return dev_err_probe(dev, -EBUSY,
				     "CPUs are already driven by another provider\n");

	rpmi_perf_cpufreq_driver.driver_data = pdev;

	ret = cpufreq_register_driver(&rpmi_perf_cpufreq_driver);
	if (ret) {
		rpmi_perf_cpufreq_driver.driver_data = NULL;
		return dev_err_probe(dev, ret, "registering cpufreq failed\n");
	}

	dev_info(dev, "%d MPXY cpufreq domains registered\n",
		 rpmi_perf_num_domains(*mpxy_perf));

	return 0;
}

static void rpmi_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&rpmi_perf_cpufreq_driver);
	rpmi_perf_cpufreq_driver.driver_data = NULL;
}

static struct platform_driver rpmi_cpufreq_platdrv = {
	.driver = {
		.name = "riscv-rpmi-cpufreq",
	},
	.probe = rpmi_cpufreq_probe,
	.remove = rpmi_cpufreq_remove,
};

module_platform_driver(rpmi_cpufreq_platdrv);

MODULE_ALIAS("platform:riscv-rpmi-cpufreq");
MODULE_AUTHOR("Joshua Yeong <joshua.yeong@starfivetech.com>");
MODULE_DESCRIPTION("RISC-V RPMI performance cpufreq driver");
MODULE_LICENSE("GPL");
