// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos ACPM-based CPUFreq DVFS driver
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#include <linux/firmware/samsung/exynos-acpm-protocol.h>

#define ACPM_DVFS_TRANSITION_TIMEOUT 400

struct acpm_cpu_priv {
	struct device *cpu_dev;
	struct acpm_handle *handle;
	unsigned int acpm_chan_id;
	unsigned int clk_id;
};

static unsigned int acpm_soc_cpufreq_get_rate(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);
	struct acpm_cpu_priv *priv;

	if (unlikely(!policy))
		return 0;

	priv = policy->driver_data;

	/* return priv->handle->ops->dvfs.get_rate(priv->handle, priv->acpm_chan_id,
	 *					priv->clk_id) / 1000;
	 */

	/* TODO: FIXME. Exynos850 doesn't return rate via ACPM */
	return policy->cur;
}

static int acpm_soc_cpufreq_set_target(struct cpufreq_policy *policy,
				       unsigned int index)
{
	struct acpm_cpu_priv *priv = policy->driver_data;
	unsigned long freq_khz = policy->freq_table[index].frequency;

	/* standard slow path */
	return priv->handle->ops->dvfs.set_rate(priv->handle, priv->acpm_chan_id,
						priv->clk_id, freq_khz * 1000);
}

static unsigned int acpm_soc_cpufreq_fast_switch(struct cpufreq_policy *policy,
						 unsigned int target_freq)
{
	struct acpm_cpu_priv *priv = policy->driver_data;
	int ret;

	ret = priv->handle->ops->dvfs.set_rate_fast(priv->handle, priv->acpm_chan_id,
						    priv->clk_id, target_freq * 1000);
	if (ret < 0)
		return 0;

	return target_freq;
}

static int acpm_soc_cpufreq_init(struct cpufreq_policy *policy)
{
	struct device *cpu_dev = get_cpu_device(policy->cpu);
	struct cpufreq_frequency_table *freq_table;
	struct acpm_cpu_priv *priv;
	struct of_phandle_args args;
	unsigned int transition_latency;
	int ret;

	if (!cpu_dev)
		return -ENODEV;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = of_parse_phandle_with_args(cpu_dev->of_node, "clocks",
					 "#clock-cells", 0, &args);
	if (ret) {
		dev_err(cpu_dev, "failed to parse clocks property\n");
		goto out_free_priv;
	}

	priv->clk_id = args.args[0];

	/*
	 * DVFS communication is expected to happen only via channel 0
	 * for now for known SoCs with ACPM firmware. Hardcoding.
	 */
	priv->acpm_chan_id = 0;

	priv->handle = devm_acpm_get_by_node(cpu_dev, args.np);
	of_node_put(args.np);

	if (IS_ERR(priv->handle)) {
		ret = PTR_ERR(priv->handle);
		goto out_free_priv;
	}

	priv->cpu_dev = cpu_dev;

	ret = dev_pm_opp_of_add_table(cpu_dev);
	if (ret < 0) {
		dev_err(cpu_dev, "failed to add OPP table: %d\n", ret);
		goto out_free_priv;
	}

	dev_pm_opp_of_get_sharing_cpus(cpu_dev, policy->cpus);

	ret = dev_pm_opp_get_opp_count(cpu_dev);
	if (ret <= 0) {
		ret = -EPROBE_DEFER;
		goto out_remove_table;
	}

	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &freq_table);
	if (ret) {
		dev_err(cpu_dev, "failed to init cpufreq table: %d\n", ret);
		goto out_remove_table;
	}

	policy->driver_data = priv;
	policy->freq_table = freq_table;

	transition_latency = dev_pm_opp_get_max_transition_latency(cpu_dev);
	if (!transition_latency)
		transition_latency = ACPM_DVFS_TRANSITION_TIMEOUT * NSEC_PER_USEC;

	policy->cpuinfo.transition_latency = transition_latency;
	policy->dvfs_possible_from_any_cpu = true;
	policy->fast_switch_possible = true;
	policy->suspend_freq = freq_table[0].frequency;

	/* TODO: FIXME. Exynos850 doesn't expose rate of clocks via ACPM  (get_rate doesn't work) */
	policy->cur = freq_table[0].frequency;

	return 0;

out_remove_table:
	dev_pm_opp_of_remove_table(cpu_dev);
out_free_priv:
	kfree(priv);
	return ret;
}

static void acpm_soc_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct acpm_cpu_priv *priv = policy->driver_data;

	dev_pm_opp_free_cpufreq_table(priv->cpu_dev, &policy->freq_table);
	dev_pm_opp_of_remove_table(priv->cpu_dev);
	kfree(priv);
}

static struct cpufreq_driver acpm_soc_cpufreq_driver = {
	.name		= "acpm-cpufreq",
	.flags		= CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
			  CPUFREQ_NEED_INITIAL_FREQ_CHECK | CPUFREQ_IS_COOLING_DEV,
	.verify		= cpufreq_generic_frequency_table_verify,
	.get		= acpm_soc_cpufreq_get_rate,
	.init		= acpm_soc_cpufreq_init,
	.exit		= acpm_soc_cpufreq_exit,
	.target_index	= acpm_soc_cpufreq_set_target,
	.fast_switch	= acpm_soc_cpufreq_fast_switch,
	.register_em	= cpufreq_register_em_with_opp,
	.set_boost	= cpufreq_boost_set_sw,
	.suspend	= cpufreq_generic_suspend,
};

static int __init acpm_soc_cpufreq_module_init(void)
{
	if (!of_machine_is_compatible("google,gs101") &&
	    !of_machine_is_compatible("samsung,exynos850"))
		return -ENODEV;

	return cpufreq_register_driver(&acpm_soc_cpufreq_driver);
}
module_init(acpm_soc_cpufreq_module_init);

static void __exit acpm_soc_cpufreq_module_exit(void)
{
	cpufreq_unregister_driver(&acpm_soc_cpufreq_driver);
}
module_exit(acpm_soc_cpufreq_module_exit);

MODULE_AUTHOR("Alexey Klimov <alexey.klimov@linaro.org>");
MODULE_DESCRIPTION("ACPM-based CPUfreq DVFS driver");
MODULE_LICENSE("GPL");
