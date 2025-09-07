// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017, 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021, Linaro Ltd.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/module.h>

#include "phy-qcom-dp-common.h"

/*
 * Display Port PLL driver block diagram for branch clocks
 *
 *              +------------------------------+
 *              |         DP_VCO_CLK           |
 *              |                              |
 *              |    +-------------------+     |
 *              |    |   (DP PLL/VCO)    |     |
 *              |    +---------+---------+     |
 *              |              v               |
 *              |   +----------+-----------+   |
 *              |   | hsclk_divsel_clk_src |   |
 *              |   +----------+-----------+   |
 *              +------------------------------+
 *                              |
 *          +---------<---------v------------>----------+
 *          |                                           |
 * +--------v----------------+                          |
 * |    dp_phy_pll_link_clk  |                          |
 * |     link_clk            |                          |
 * +--------+----------------+                          |
 *          |                                           |
 *          |                                           |
 *          v                                           v
 * Input to DISPCC block                                |
 * for link clk, crypto clk                             |
 * and interface clock                                  |
 *                                                      |
 *                                                      |
 *      +--------<------------+-----------------+---<---+
 *      |                     |                 |
 * +----v---------+  +--------v-----+  +--------v------+
 * | vco_divided  |  | vco_divided  |  | vco_divided   |
 * |    _clk_src  |  |    _clk_src  |  |    _clk_src   |
 * |              |  |              |  |               |
 * |divsel_six    |  |  divsel_two  |  |  divsel_four  |
 * +-------+------+  +-----+--------+  +--------+------+
 *         |                 |                  |
 *         v---->----------v-------------<------v
 *                         |
 *              +----------+-----------------+
 *              |   dp_phy_pll_vco_div_clk   |
 *              +---------+------------------+
 *                        |
 *                        v
 *              Input to DISPCC block
 *              for DP pixel clock
 *
 */
static int qmp_dp_pixel_clk_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	switch (req->rate) {
	case 1620000000UL / 2:
	case 2700000000UL / 2:
	/* 5.4 and 8.1 GHz are same link rate as 2.7GHz, i.e. div 4 and div 6 */
		return 0;
	default:
		return -EINVAL;
	}
}

static unsigned long qmp_dp_pixel_clk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	const struct qcom_dp_common *common;
	const struct phy_configure_opts_dp *dp_opts;

	common = container_of(hw, struct qcom_dp_common, pixel_hw);
	dp_opts = &common->dp_opts;

	switch (dp_opts->link_rate) {
	case 1620:
		return 1620000000UL / 2;
	case 2700:
		return 2700000000UL / 2;
	case 5400:
		return 5400000000UL / 4;
	case 8100:
		return 8100000000UL / 6;
	default:
		return 0;
	}
}

static const struct clk_ops qmp_dp_pixel_clk_ops = {
	.determine_rate	= qmp_dp_pixel_clk_determine_rate,
	.recalc_rate	= qmp_dp_pixel_clk_recalc_rate,
};

static int qmp_dp_link_clk_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	switch (req->rate) {
	case 162000000:
	case 270000000:
	case 540000000:
	case 810000000:
		return 0;
	default:
		return -EINVAL;
	}
}

static unsigned long qmp_dp_link_clk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	const struct qcom_dp_common *common;
	const struct phy_configure_opts_dp *dp_opts;

	common = container_of(hw, struct qcom_dp_common, link_hw);
	dp_opts = &common->dp_opts;

	switch (dp_opts->link_rate) {
	case 1620:
	case 2700:
	case 5400:
	case 8100:
		return dp_opts->link_rate * 100000;
	default:
		return 0;
	}
}

static const struct clk_ops qmp_dp_link_clk_ops = {
	.determine_rate	= qmp_dp_link_clk_determine_rate,
	.recalc_rate	= qmp_dp_link_clk_recalc_rate,
};

int devm_qcom_dp_clks_register(struct device *dev,
			       struct qcom_dp_common *dp)
{
	struct clk_init_data init = { };
	char name[64];
	int ret;

	snprintf(name, sizeof(name), "%s::link_clk", dev_name(dev));
	init.ops = &qmp_dp_link_clk_ops;
	init.name = name;
	dp->link_hw.init = &init;
	ret = devm_clk_hw_register(dev, &dp->link_hw);
	if (ret)
		return ret;

	snprintf(name, sizeof(name), "%s::vco_div_clk", dev_name(dev));
	init.ops = &qmp_dp_pixel_clk_ops;
	init.name = name;
	dp->pixel_hw.init = &init;
	ret = devm_clk_hw_register(dev, &dp->pixel_hw);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(devm_qcom_dp_clks_register);
