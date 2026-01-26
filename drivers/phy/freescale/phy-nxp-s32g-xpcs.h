/* SPDX-License-Identifier: GPL-2.0 */
/**
 * Copyright 2021-2026 NXP
 */
#ifndef NXP_S32G_XPCS_H
#define NXP_S32G_XPCS_H

#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/types.h>
#include <linux/phylink.h>

enum pcie_xpcs_mode {
	NOT_SHARED,
	PCIE_XPCS_1G,
	PCIE_XPCS_2G5,
};

enum s32g_xpcs_pll {
	XPCS_PLLA,	/* Slow PLL */
	XPCS_PLLB,	/* Fast PLL */
};

struct s32g_xpcs {
	void __iomem *base;
	struct device *dev;
	unsigned char id;
	struct regmap *regmap;
	enum s32g_xpcs_pll ref;
	bool ext_clk;
	bool mhz125;
	bool an;
	enum pcie_xpcs_mode pcie_shared;
	struct phylink_pcs pcs;
};

int s32g_xpcs_init(struct s32g_xpcs *xpcs, struct device *dev,
		   unsigned char id, void __iomem *base, bool ext_clk,
		   unsigned long rate, enum pcie_xpcs_mode pcie_shared);
int s32g_xpcs_init_plls(struct s32g_xpcs *xpcs);
int s32g_xpcs_pre_pcie_2g5(struct s32g_xpcs *xpcs);
int s32g_xpcs_vreset(struct s32g_xpcs *xpcs);
int s32g_xpcs_wait_vreset(struct s32g_xpcs *xpcs);
int s32g_xpcs_reset_rx(struct s32g_xpcs *xpcs);
int s32g_xpcs_disable_an(struct s32g_xpcs *xpcs);
#endif

