// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 * Copyright (c) 2023, Linaro Ltd.
 */

#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#include "phy-qcom-hdmi-preqmp.h"

static int qcom_hdmi_preqmp_phy_init(struct phy *phy)
{
	struct qcom_hdmi_preqmp_phy *hdmi_phy = phy_get_drvdata(phy);

	return pm_runtime_resume_and_get(hdmi_phy->dev);
}

static int qcom_hdmi_preqmp_phy_exit(struct phy *phy)
{
	struct qcom_hdmi_preqmp_phy *hdmi_phy = phy_get_drvdata(phy);

	pm_runtime_put_noidle(hdmi_phy->dev);

	return 0;
}

static int qcom_hdmi_preqmp_phy_power_on(struct phy *phy)
{
	struct qcom_hdmi_preqmp_phy *hdmi_phy = phy_get_drvdata(phy);

	return hdmi_phy->power_on(hdmi_phy);
};

static int qcom_hdmi_preqmp_phy_power_off(struct phy *phy)
{
	struct qcom_hdmi_preqmp_phy *hdmi_phy = phy_get_drvdata(phy);

	return hdmi_phy->power_off(hdmi_phy);
};

static int qcom_hdmi_preqmp_phy_configure(struct phy *phy, union phy_configure_opts *opts)
{
	const struct phy_configure_opts_hdmi *hdmi_opts = &opts->hdmi;
	struct qcom_hdmi_preqmp_phy *hdmi_phy = phy_get_drvdata(phy);
	int ret = 0;

	memcpy(&hdmi_phy->hdmi_opts, hdmi_opts, sizeof(*hdmi_opts));

	return ret;
}

static int __maybe_unused qcom_hdmi_preqmp_runtime_resume(struct device *dev)
{
	struct qcom_hdmi_preqmp_phy *hdmi_phy = dev_get_drvdata(dev);
	int ret;

	ret = regulator_bulk_enable(hdmi_phy->num_regs, hdmi_phy->regs);
	if (ret)
		return ret;

	ret = clk_bulk_prepare_enable(hdmi_phy->num_clks, hdmi_phy->clks);
	if (ret)
		goto out_disable_supplies;

	return 0;

out_disable_supplies:
	regulator_bulk_disable(hdmi_phy->num_regs, hdmi_phy->regs);

	return ret;
}

static int __maybe_unused qcom_hdmi_preqmp_runtime_suspend(struct device *dev)
{
	struct qcom_hdmi_preqmp_phy *hdmi_phy = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(hdmi_phy->num_clks, hdmi_phy->clks);
	regulator_bulk_disable(hdmi_phy->num_regs, hdmi_phy->regs);

	return 0;
}

static const struct phy_ops qcom_hdmi_preqmp_phy_ops = {
	.init		= qcom_hdmi_preqmp_phy_init,
	.configure	= qcom_hdmi_preqmp_phy_configure,
	.power_on	= qcom_hdmi_preqmp_phy_power_on,
	.power_off	= qcom_hdmi_preqmp_phy_power_off,
	.exit		= qcom_hdmi_preqmp_phy_exit,
	.owner		= THIS_MODULE,
};

static int qcom_hdmi_preqmp_probe(struct platform_device *pdev)
{
	struct clk_init_data init;
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct qcom_hdmi_preqmp_phy *hdmi_phy;
	const struct qcom_hdmi_preqmp_cfg *cfg;
	int i, ret;

	cfg = of_device_get_match_data(dev);
	if (!cfg)
		return -EINVAL;

	hdmi_phy = devm_kzalloc(dev, sizeof(*hdmi_phy), GFP_KERNEL);
	if (!hdmi_phy)
		return -ENOMEM;

	hdmi_phy->dev = dev;

	hdmi_phy->phy_reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hdmi_phy->phy_reg))
		return PTR_ERR(hdmi_phy->phy_reg);

	hdmi_phy->pll_reg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(hdmi_phy->pll_reg))
		return PTR_ERR(hdmi_phy->pll_reg);

	hdmi_phy->num_clks = cfg->num_clks;
	for (i = 0; i < cfg->num_clks; i++)
		hdmi_phy->clks[i].id = cfg->clk_names[i];
	ret = devm_clk_bulk_get(dev, hdmi_phy->num_clks, hdmi_phy->clks);
	if (ret)
		return ret;

	hdmi_phy->num_regs = cfg->num_regs;
	for (i = 0; i < cfg->num_regs; i++) {
		hdmi_phy->regs[i].supply = cfg->reg_names[i];
		hdmi_phy->regs[i].init_load_uA = cfg->reg_init_load[i];
	}
	ret = devm_regulator_bulk_get(dev, hdmi_phy->num_regs, hdmi_phy->regs);
	if (ret)
		return ret;

	hdmi_phy->power_on = cfg->power_on;
	hdmi_phy->power_off = cfg->power_off;

	platform_set_drvdata(pdev, hdmi_phy);

	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret)
		return ret;

	/* FIXME: msm8x60 doesn't yet have PLL ops */
	if (cfg->pll_ops) {
		init.name = "hdmipll";
		init.ops = cfg->pll_ops;
		init.flags = CLK_GET_RATE_NOCACHE;
		init.parent_data = cfg->pll_parent;
		init.num_parents = 1;

		hdmi_phy->pll_hw.init = &init;
		ret = devm_clk_hw_register(hdmi_phy->dev, &hdmi_phy->pll_hw);
		if (ret)
			goto err;

		ret = devm_of_clk_add_hw_provider(hdmi_phy->dev, of_clk_hw_simple_get, &hdmi_phy->pll_hw);
		if (ret)
			goto err;
	}

	hdmi_phy->phy = devm_phy_create(dev, pdev->dev.of_node, &qcom_hdmi_preqmp_phy_ops);
	if (IS_ERR(hdmi_phy->phy)) {
		ret = PTR_ERR(hdmi_phy->phy);
		goto err;
	}

	phy_set_drvdata(hdmi_phy->phy, hdmi_phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	pm_runtime_put_noidle(&pdev->dev);
	return PTR_ERR_OR_ZERO(phy_provider);

err:
	pm_runtime_put_noidle(&pdev->dev);
	return ret;
}

static const struct of_device_id qcom_hdmi_preqmp_of_match_table[] = {
	{ .compatible = "qcom,hdmi-phy-8x60", .data = &msm8x60_hdmi_phy_cfg, },
	{ .compatible = "qcom,hdmi-phy-8960", .data = &msm8960_hdmi_phy_cfg, },
	{ .compatible = "qcom,hdmi-phy-8974", .data = &msm8974_hdmi_phy_cfg, },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_hdmi_preqmp_of_match_table);

DEFINE_RUNTIME_DEV_PM_OPS(qcom_hdmi_preqmp_pm_ops,
			  qcom_hdmi_preqmp_runtime_suspend,
			  qcom_hdmi_preqmp_runtime_resume,
			  NULL);

static struct platform_driver qcom_hdmi_preqmp_driver = {
	.probe		= qcom_hdmi_preqmp_probe,
	.driver = {
		.name	= "qcom-preqmp-hdmi-phy",
		.of_match_table = qcom_hdmi_preqmp_of_match_table,
		.pm     = &qcom_hdmi_preqmp_pm_ops,
	},
};

module_platform_driver(qcom_hdmi_preqmp_driver);

MODULE_AUTHOR("Dmitry Baryshkov <dmitry.baryshkov@linaro.org>");
MODULE_DESCRIPTION("Qualcomm MSMpreqmp HDMI PHY driver");
MODULE_LICENSE("GPL");
