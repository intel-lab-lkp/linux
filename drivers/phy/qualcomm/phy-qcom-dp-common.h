/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef PHY_QCOM_QMP_DP_COMMON_H
#define PHY_QCOM_QMP_DP_COMMON_H

#include <linux/clk-provider.h>
#include <linux/phy/phy-dp.h>

struct qcom_dp_common {
	struct phy_configure_opts_dp dp_opts;
	struct clk_hw link_hw;
	struct clk_hw pixel_hw;
};

int devm_qcom_dp_clks_register(struct device *dev,
			       struct qcom_dp_common *dp);

#endif
