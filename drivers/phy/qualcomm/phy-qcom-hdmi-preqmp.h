/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 * Copyright (c) 2023, Linaro Ltd.
 */

#ifndef PHY_QCOM_HDMI_PREQMP_H
#define PHY_QCOM_HDMI_PREQMP_H

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/phy/phy-hdmi.h>
#include <linux/regulator/consumer.h>

#define MAX_CLKS 2
#define MAX_SUPPLIES 2

struct qcom_hdmi_preqmp_phy {
	struct device *dev;
	struct phy *phy;
	void __iomem *pll_reg;
	void __iomem *phy_reg;

	struct phy_configure_opts_hdmi hdmi_opts;

	struct clk_hw pll_hw;
	struct clk_bulk_data clks[MAX_CLKS];
	int num_clks;

	struct regulator_bulk_data regs[MAX_SUPPLIES];
	int num_regs;

	int (*power_on)(struct qcom_hdmi_preqmp_phy *phy);
	int (*power_off)(struct qcom_hdmi_preqmp_phy *phy);
};

#define hw_clk_to_phy(x) container_of(x, struct qcom_hdmi_preqmp_phy, pll_hw)

struct qcom_hdmi_preqmp_cfg {
	const char * const clk_names[MAX_CLKS];
	int num_clks;

	const char * const reg_names[MAX_SUPPLIES];
	int reg_init_load[MAX_SUPPLIES];
	int num_regs;

	int (*power_on)(struct qcom_hdmi_preqmp_phy *phy);
	int (*power_off)(struct qcom_hdmi_preqmp_phy *phy);

	const struct clk_ops *pll_ops;
	const struct clk_parent_data *pll_parent;
};

static inline void hdmi_phy_write(struct qcom_hdmi_preqmp_phy *phy, int offset,
				  u32 data)
{
	writel(data, phy->phy_reg + offset);
}

static inline u32 hdmi_phy_read(struct qcom_hdmi_preqmp_phy *phy, int offset)
{
	return readl(phy->phy_reg + offset);
}

static inline void hdmi_pll_write(struct qcom_hdmi_preqmp_phy *phy, int offset,
				  u32 data)
{
	writel(data, phy->pll_reg + offset);
}

static inline u32 hdmi_pll_read(struct qcom_hdmi_preqmp_phy *phy, int offset)
{
	return readl(phy->pll_reg + offset);
}

extern const struct qcom_hdmi_preqmp_cfg msm8x60_hdmi_phy_cfg;
extern const struct qcom_hdmi_preqmp_cfg msm8960_hdmi_phy_cfg;
extern const struct qcom_hdmi_preqmp_cfg msm8974_hdmi_phy_cfg;

#endif
