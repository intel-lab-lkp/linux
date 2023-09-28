// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Linaro Ltd.
 */

#ifndef PHY_QCOM_QMP_HDMI_H
#define PHY_QCOM_QMP_HDMI_H

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/regulator/consumer.h>
#include <linux/phy/phy-hdmi.h>

#define MAX_CLKS 2
#define MAX_SUPPLIES 2

#define HDMI_NUM_TX_CHANNEL 4

struct qmp_hdmi_phy {
	struct device *dev;
	struct phy *phy;
	void __iomem *serdes;
	void __iomem *tx[HDMI_NUM_TX_CHANNEL];
	void __iomem *phy_reg;

	struct phy_configure_opts_hdmi hdmi_opts;

	struct clk_hw pll_hw;
	struct clk_bulk_data clks[MAX_CLKS];
	struct regulator_bulk_data supplies[MAX_SUPPLIES];
};

struct qmp_hdmi_cfg {
	const struct clk_ops *pll_ops;
	const struct phy_ops *phy_ops;
};

#define hw_clk_to_pll(x) container_of(x, struct qmp_hdmi_phy, pll_hw)

static inline void hdmi_phy_write(struct qmp_hdmi_phy *phy, int offset,
				  u32 data)
{
	writel(data, phy->phy_reg + offset);
}

static inline u32 hdmi_phy_read(struct qmp_hdmi_phy *phy, int offset)
{
	return readl(phy->phy_reg + offset);
}

static inline void hdmi_pll_write(struct qmp_hdmi_phy *phy, int offset,
				  u32 data)
{
	writel(data, phy->serdes + offset);
}

static inline u32 hdmi_pll_read(struct qmp_hdmi_phy *phy, int offset)
{
	return readl(phy->serdes + offset);
}

static inline void hdmi_tx_chan_write(struct qmp_hdmi_phy *phy, int channel,
				      int offset, int data)
{
	writel(data, phy->tx[channel] + offset);
}

int qmp_hdmi_phy_init(struct phy *phy);
int qmp_hdmi_phy_configure(struct phy *phy, union phy_configure_opts *opts);
int qmp_hdmi_phy_exit(struct phy *phy);

extern const struct qmp_hdmi_cfg qmp_hdmi_8996_cfg;

#endif
