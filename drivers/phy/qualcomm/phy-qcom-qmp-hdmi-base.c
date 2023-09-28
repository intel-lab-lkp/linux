// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Linaro Ltd.
 */

#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#include "phy-qcom-qmp-hdmi.h"

int qmp_hdmi_phy_init(struct phy *phy)
{
	struct qmp_hdmi_phy *hdmi_phy = phy_get_drvdata(phy);

	return pm_runtime_resume_and_get(hdmi_phy->dev);
}

int qmp_hdmi_phy_configure(struct phy *phy, union phy_configure_opts *opts)
{
        const struct phy_configure_opts_hdmi *hdmi_opts = &opts->hdmi;
	struct qmp_hdmi_phy *hdmi_phy = phy_get_drvdata(phy);
        int ret = 0;

        memcpy(&hdmi_phy->hdmi_opts, hdmi_opts, sizeof(*hdmi_opts));

        return ret;
}

int qmp_hdmi_phy_exit(struct phy *phy)
{
	struct qmp_hdmi_phy *hdmi_phy = phy_get_drvdata(phy);

	pm_runtime_put_noidle(hdmi_phy->dev);

	return 0;
}

static int __maybe_unused qmp_hdmi_runtime_resume(struct device *dev)
{
	struct qmp_hdmi_phy *hdmi_phy = dev_get_drvdata(dev);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(hdmi_phy->supplies), hdmi_phy->supplies);
	if (ret)
		return ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(hdmi_phy->clks), hdmi_phy->clks);
	if (ret)
		goto out_disable_supplies;

	return 0;

out_disable_supplies:
	regulator_bulk_disable(ARRAY_SIZE(hdmi_phy->supplies), hdmi_phy->supplies);

	return ret;
}

static int __maybe_unused qmp_hdmi_runtime_suspend(struct device *dev)
{
	struct qmp_hdmi_phy *hdmi_phy = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(ARRAY_SIZE(hdmi_phy->clks), hdmi_phy->clks);
	regulator_bulk_disable(ARRAY_SIZE(hdmi_phy->supplies), hdmi_phy->supplies);

	return 0;
}

static int qmp_hdmi_probe(struct platform_device *pdev)
{
	struct clk_init_data init = {
		.name = "hdmipll",
		.parent_data = (const struct clk_parent_data[]) {
			{ .fw_name = "xo", .name = "xo_board" },
		},
		.flags = CLK_GET_RATE_NOCACHE,
		.num_parents = 1,
	};
	const struct qmp_hdmi_cfg *cfg = of_device_get_match_data(&pdev->dev);
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct qmp_hdmi_phy *hdmi_phy;
	int ret, i;

	hdmi_phy = devm_kzalloc(dev, sizeof(*hdmi_phy), GFP_KERNEL);
	if (!hdmi_phy)
		return -ENOMEM;

	hdmi_phy->dev = dev;

	hdmi_phy->serdes = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hdmi_phy->serdes))
		return PTR_ERR(hdmi_phy->serdes);

	for (i = 0; i < HDMI_NUM_TX_CHANNEL; i++) {
		hdmi_phy->tx[i] = devm_platform_ioremap_resource(pdev, 1 + i);
		if (IS_ERR(hdmi_phy->tx[i]))
			return PTR_ERR(hdmi_phy->tx[i]);
	}

	hdmi_phy->phy_reg = devm_platform_ioremap_resource(pdev, 5);
	if (IS_ERR(hdmi_phy->phy_reg))
		return PTR_ERR(hdmi_phy->phy_reg);

	hdmi_phy->clks[0].id = "iface";
	hdmi_phy->clks[1].id = "ref";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(hdmi_phy->clks), hdmi_phy->clks);
	if (ret)
		return ret;

	hdmi_phy->supplies[0].supply = "vddio";
	hdmi_phy->supplies[0].init_load_uA = 100000;
	hdmi_phy->supplies[1].supply = "vcca";
	hdmi_phy->supplies[1].init_load_uA = 10000;
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(hdmi_phy->supplies), hdmi_phy->supplies);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, hdmi_phy);

	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret)
		return ret;

	init.ops = cfg->pll_ops;
	hdmi_phy->pll_hw.init = &init;
	ret = devm_clk_hw_register(hdmi_phy->dev, &hdmi_phy->pll_hw);
	if (ret)
		goto err;

	ret = devm_of_clk_add_hw_provider(hdmi_phy->dev, of_clk_hw_simple_get, &hdmi_phy->pll_hw);
	if (ret)
		goto err;

	hdmi_phy->phy = devm_phy_create(dev, pdev->dev.of_node, cfg->phy_ops);
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

static const struct of_device_id qmp_hdmi_of_match_table[] = {
	{
		.compatible = "qcom,hdmi-phy-8996", .data = &qmp_hdmi_8996_cfg,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, qmp_hdmi_of_match_table);

DEFINE_RUNTIME_DEV_PM_OPS(qmp_hdmi_pm_ops,
			  qmp_hdmi_runtime_suspend,
			  qmp_hdmi_runtime_resume,
			  NULL);

static struct platform_driver qmp_hdmi_driver = {
	.probe		= qmp_hdmi_probe,
	.driver = {
		.name	= "qcom-qmp-hdmi-phy",
		.of_match_table = qmp_hdmi_of_match_table,
		.pm     = &qmp_hdmi_pm_ops,
	},
};

module_platform_driver(qmp_hdmi_driver);

MODULE_AUTHOR("Dmitry Baryshkov <dmitry.baryshkov@linaro.org>");
MODULE_DESCRIPTION("Qualcomm QMP HDMI PHY driver");
MODULE_LICENSE("GPL");
