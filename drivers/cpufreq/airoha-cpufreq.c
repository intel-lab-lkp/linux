// SPDX-License-Identifier: GPL-2.0

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "cpufreq-dt.h"

#define AIROHA_SIP_AVS_HANDLE			0x82000301
#define AIROHA_AVS_OP_BASE			0xddddddd0
#define AIROHA_AVS_OP_MASK			GENMASK(1, 0)
#define AIROHA_AVS_OP_FREQ_DYN_ADJ		(AIROHA_AVS_OP_BASE | \
						 FIELD_PREP(AIROHA_AVS_OP_MASK, 0x1))
#define AIROHA_AVS_OP_GET_FREQ			(AIROHA_AVS_OP_BASE | \
						 FIELD_PREP(AIROHA_AVS_OP_MASK, 0x2))

struct airoha_cpufreq_priv {
	struct clk_hw hw;
	struct generic_pm_domain pd;

	int opp_token;
	struct dev_pm_domain_list *pd_list;
	struct platform_device *cpufreq_dt;
};

static long airoha_cpufreq_clk_round(struct clk_hw *hw, unsigned long rate,
				     unsigned long *parent_rate)
{
	return rate;
}

static unsigned long airoha_cpufreq_clk_get(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	const struct arm_smccc_1_2_regs args = {
		.a0 = AIROHA_SIP_AVS_HANDLE,
		.a1 = AIROHA_AVS_OP_GET_FREQ,
	};
	struct arm_smccc_1_2_regs res;

	arm_smccc_1_2_smc(&args, &res);

	/* SMCCC returns freq in MHz */
	return res.a0 * 1000 * 1000;
}

/* Airoha CPU clk SMCC is always enabled */
static int airoha_cpufreq_clk_is_enabled(struct clk_hw *hw)
{
	return true;
}

static const struct clk_ops airoha_cpufreq_clk_ops = {
	.recalc_rate = airoha_cpufreq_clk_get,
	.is_enabled = airoha_cpufreq_clk_is_enabled,
	.round_rate = airoha_cpufreq_clk_round,
};

static const char * const airoha_cpufreq_clk_names[] = { "cpu", NULL };

/* NOP function to disable OPP from setting clock */
static int airoha_cpufreq_config_clks_nop(struct device *dev,
					  struct opp_table *opp_table,
					  struct dev_pm_opp *opp,
					  void *data, bool scaling_down)
{
	return 0;
}

static const char * const airoha_cpufreq_pd_names[] = { "perf" };

static int airoha_cpufreq_set_performance_state(struct generic_pm_domain *domain,
						unsigned int state)
{
	const struct arm_smccc_1_2_regs args = {
		.a0 = AIROHA_SIP_AVS_HANDLE,
		.a1 = AIROHA_AVS_OP_FREQ_DYN_ADJ,
		.a3 = state,
	};
	struct arm_smccc_1_2_regs res;

	arm_smccc_1_2_smc(&args, &res);

	/* SMC signal correct apply by unsetting BIT 0 */
	return res.a0 & BIT(0) ? -EINVAL : 0;
}

static int airoha_cpufreq_probe(struct platform_device *pdev)
{
	const struct dev_pm_domain_attach_data attach_data = {
		.pd_names = airoha_cpufreq_pd_names,
		.num_pd_names = ARRAY_SIZE(airoha_cpufreq_pd_names),
		.pd_flags = PD_FLAG_DEV_LINK_ON | PD_FLAG_REQUIRED_OPP,
	};
	struct dev_pm_opp_config config = {
		.clk_names = airoha_cpufreq_clk_names,
		.config_clks = airoha_cpufreq_config_clks_nop,
	};
	struct platform_device *cpufreq_dt;
	struct airoha_cpufreq_priv *priv;
	struct device *dev = &pdev->dev;
	const struct clk_init_data init = {
		.name = "cpu",
		.ops = &airoha_cpufreq_clk_ops,
		/* Clock with no set_rate, can't cache */
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct generic_pm_domain *pd;
	struct device *cpu_dev;
	int ret;

	/* CPUs refer to the same OPP table */
	cpu_dev = get_cpu_device(0);
	if (!cpu_dev)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Init and register a get-only clk for Cpufreq */
	priv->hw.init = &init;
	ret = devm_clk_hw_register(dev, &priv->hw);
	if (ret)
		return ret;

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					  &priv->hw);
	if (ret)
		return ret;

	/* Init and register a PD for Cpufreq */
	pd = &priv->pd;
	pd->name = "cpu_pd";
	pd->flags = GENPD_FLAG_ALWAYS_ON;
	pd->set_performance_state = airoha_cpufreq_set_performance_state;

	ret = pm_genpd_init(pd, NULL, false);
	if (ret)
		return ret;

	ret = of_genpd_add_provider_simple(dev->of_node, pd);
	if (ret)
		goto err_add_provider;

	/* Set OPP table conf with NOP config_clks */
	priv->opp_token = dev_pm_opp_set_config(cpu_dev, &config);
	if (priv->opp_token < 0) {
		ret = priv->opp_token;
		dev_err(dev, "Failed to set OPP config\n");
		goto err_set_config;
	}

	/* Attach PM for OPP */
	ret = dev_pm_domain_attach_list(cpu_dev, &attach_data,
					&priv->pd_list);
	if (ret)
		goto err_attach_pm;

	cpufreq_dt = platform_device_register_simple("cpufreq-dt", -1, NULL, 0);
	ret = PTR_ERR_OR_ZERO(cpufreq_dt);
	if (ret) {
		dev_err(dev, "failed to create cpufreq-dt device: %d\n", ret);
		goto err_register_cpufreq;
	}

	priv->cpufreq_dt = cpufreq_dt;
	platform_set_drvdata(pdev, priv);

	return 0;

err_register_cpufreq:
	dev_pm_domain_detach_list(priv->pd_list);
err_attach_pm:
	dev_pm_opp_clear_config(priv->opp_token);
err_set_config:
	of_genpd_del_provider(dev->of_node);
err_add_provider:
	pm_genpd_remove(pd);

	return ret;
}

static void airoha_cpufreq_remove(struct platform_device *pdev)
{
	struct airoha_cpufreq_priv *priv = platform_get_drvdata(pdev);

	platform_device_unregister(priv->cpufreq_dt);

	dev_pm_domain_detach_list(priv->pd_list);

	dev_pm_opp_clear_config(priv->opp_token);

	of_genpd_del_provider(pdev->dev.of_node);
	pm_genpd_remove(&priv->pd);
}

static const struct of_device_id airoha_cpufreq_of_match[] = {
	{ .compatible = "airoha,en7581-cpufreq" },
	{ },
};
MODULE_DEVICE_TABLE(of, airoha_cpufreq_of_match);

static struct platform_driver airoha_cpufreq_driver = {
	.probe = airoha_cpufreq_probe,
	.remove = airoha_cpufreq_remove,
	.driver = {
		.name = "airoha-cpufreq",
		.of_match_table = airoha_cpufreq_of_match,
	},
};
module_platform_driver(airoha_cpufreq_driver);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("CPUfreq driver for Airoha SoCs");
MODULE_LICENSE("GPL");
