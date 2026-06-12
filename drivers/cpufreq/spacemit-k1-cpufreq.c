// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU frequency scaling driver for SpacemiT K1 SoC.
 *
 * Copyright (c) 2026 Shuwei Wu <shuwei.wu@mailbox.org>
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct k1_cpufreq_priv {
	struct device *cpu_dev;
	struct clk *cluster0_clk;
	struct clk *cluster1_clk;
	struct cpufreq_frequency_table *freq_table;
	cpumask_var_t cpus;
	int opp_token;
};

static struct platform_device *k1_cpufreq_pdev;

static int k1_cpufreq_set_target(struct cpufreq_policy *policy,
				 unsigned int index)
{
	struct k1_cpufreq_priv *priv = policy->driver_data;
	unsigned long old_freq = policy->cur * 1000UL;
	unsigned long new_freq = policy->freq_table[index].frequency * 1000UL;
	int ret;

	if (!old_freq)
		old_freq = clk_get_rate(priv->cluster0_clk);

	if (new_freq > old_freq) {
		ret = dev_pm_opp_set_rate(priv->cpu_dev, new_freq);
		if (ret)
			return ret;

		ret = clk_set_rate(priv->cluster1_clk, new_freq);
		if (ret)
			dev_pm_opp_set_rate(priv->cpu_dev, old_freq);

		return ret;
	}

	ret = clk_set_rate(priv->cluster1_clk, new_freq);
	if (ret)
		return ret;

	ret = dev_pm_opp_set_rate(priv->cpu_dev, new_freq);
	if (ret)
		clk_set_rate(priv->cluster1_clk, old_freq);

	return ret;
}

static int k1_cpufreq_init_policy(struct cpufreq_policy *policy)
{
	struct k1_cpufreq_priv *priv = cpufreq_get_driver_data();
	unsigned int transition_latency;

	cpumask_copy(policy->cpus, priv->cpus);
	policy->clk = priv->cluster0_clk;
	policy->freq_table = priv->freq_table;
	policy->driver_data = priv;
	policy->dvfs_possible_from_any_cpu = true;

	transition_latency = dev_pm_opp_get_max_transition_latency(priv->cpu_dev);
	if (!transition_latency)
		transition_latency = CPUFREQ_DEFAULT_TRANSITION_LATENCY_NS;
	policy->cpuinfo.transition_latency = transition_latency;

	return 0;
}

static struct cpufreq_driver k1_cpufreq_driver = {
	.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK | CPUFREQ_IS_COOLING_DEV,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = k1_cpufreq_set_target,
	.get = cpufreq_generic_get,
	.init = k1_cpufreq_init_policy,
	.register_em = cpufreq_register_em_with_opp,
	.name = "k1-cpufreq",
};

static int k1_cpufreq_probe(struct platform_device *pdev)
{
	struct k1_cpufreq_priv *priv;
	struct device *cpu4_dev;
	static const char * const reg_names[] = { "cpu", NULL };
	int cpu, ret;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;

	if (!zalloc_cpumask_var(&priv->cpus, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto free_data;
	}

	priv->cpu_dev = get_cpu_device(0);
	cpu4_dev = get_cpu_device(4);
	if (!priv->cpu_dev || !cpu4_dev) {
		ret = -EPROBE_DEFER;
		goto free_cpumask;
	}

	for_each_present_cpu(cpu)
		cpumask_set_cpu(cpu, priv->cpus);

	priv->cluster0_clk = clk_get(priv->cpu_dev, NULL);
	if (IS_ERR(priv->cluster0_clk)) {
		ret = PTR_ERR(priv->cluster0_clk);
		dev_err_probe(priv->cpu_dev, ret, "failed to get cluster0 clock\n");
		goto free_cpumask;
	}

	priv->cluster1_clk = clk_get(cpu4_dev, NULL);
	if (IS_ERR(priv->cluster1_clk)) {
		ret = PTR_ERR(priv->cluster1_clk);
		dev_err_probe(cpu4_dev, ret, "failed to get cluster1 clock\n");
		goto put_clk_c0;
	}

	priv->opp_token = dev_pm_opp_set_regulators(priv->cpu_dev, reg_names);
	if (priv->opp_token < 0) {
		ret = priv->opp_token;
		dev_err_probe(priv->cpu_dev, ret, "failed to set regulators\n");
		goto put_clk_c1;
	}

	ret = dev_pm_opp_of_cpumask_add_table(priv->cpus);
	if (ret) {
		dev_err_probe(priv->cpu_dev, ret, "failed to add OPP table\n");
		goto put_opp_regulators;
	}

	ret = dev_pm_opp_get_opp_count(priv->cpu_dev);
	if (ret <= 0) {
		dev_err(priv->cpu_dev, "OPP table can't be empty\n");
		ret = -ENODEV;
		goto remove_opp_table;
	}

	ret = dev_pm_opp_init_cpufreq_table(priv->cpu_dev, &priv->freq_table);
	if (ret) {
		dev_err(priv->cpu_dev, "failed to init cpufreq table: %d\n", ret);
		goto remove_opp_table;
	}

	k1_cpufreq_driver.driver_data = priv;
	ret = cpufreq_register_driver(&k1_cpufreq_driver);
	if (ret)
		goto free_freq_table;

	platform_set_drvdata(pdev, priv);

	return 0;

free_freq_table:
	k1_cpufreq_driver.driver_data = NULL;
	dev_pm_opp_free_cpufreq_table(priv->cpu_dev, &priv->freq_table);
remove_opp_table:
	dev_pm_opp_of_cpumask_remove_table(priv->cpus);
put_opp_regulators:
	dev_pm_opp_put_regulators(priv->opp_token);
put_clk_c1:
	clk_put(priv->cluster1_clk);
put_clk_c0:
	clk_put(priv->cluster0_clk);
free_cpumask:
	free_cpumask_var(priv->cpus);
free_data:
	kfree(priv);

	return ret;
}

static void k1_cpufreq_remove(struct platform_device *pdev)
{
	struct k1_cpufreq_priv *priv = platform_get_drvdata(pdev);

	if (!priv)
		return;

	cpufreq_unregister_driver(&k1_cpufreq_driver);
	k1_cpufreq_driver.driver_data = NULL;
	dev_pm_opp_free_cpufreq_table(priv->cpu_dev, &priv->freq_table);
	dev_pm_opp_of_cpumask_remove_table(priv->cpus);
	dev_pm_opp_put_regulators(priv->opp_token);
	clk_put(priv->cluster1_clk);
	clk_put(priv->cluster0_clk);
	free_cpumask_var(priv->cpus);
	kfree(priv);
}

static struct platform_driver k1_cpufreq_platdrv = {
	.probe = k1_cpufreq_probe,
	.remove = k1_cpufreq_remove,
	.driver = {
		.name = "spacemit-k1-cpufreq",
	},
};

static const struct of_device_id k1_cpufreq_match_list[] __initconst = {
	{ .compatible = "spacemit,k1" },
	{ }
};
MODULE_DEVICE_TABLE(of, k1_cpufreq_match_list);

/*
 * K1 has no dedicated cpufreq controller device. Register a logical platform
 * device so clock/regulator dependencies can defer probe.
 */
static int __init k1_cpufreq_init(void)
{
	int ret;

	if (!of_machine_device_match(k1_cpufreq_match_list))
		return -ENODEV;

	ret = platform_driver_register(&k1_cpufreq_platdrv);
	if (ret)
		return ret;

	k1_cpufreq_pdev = platform_device_register_simple("spacemit-k1-cpufreq", -1, NULL, 0);
	ret = PTR_ERR_OR_ZERO(k1_cpufreq_pdev);
	if (ret)
		platform_driver_unregister(&k1_cpufreq_platdrv);

	return ret;
}
module_init(k1_cpufreq_init);

static void __exit k1_cpufreq_exit(void)
{
	platform_device_unregister(k1_cpufreq_pdev);
	platform_driver_unregister(&k1_cpufreq_platdrv);
}
module_exit(k1_cpufreq_exit);

MODULE_DESCRIPTION("SpacemiT K1 CPUFreq driver");
MODULE_AUTHOR("Shuwei Wu <shuwei.wu@mailbox.org>");
MODULE_LICENSE("GPL");
