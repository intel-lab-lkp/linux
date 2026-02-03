/* SPDX-License-Identifier: GPL-2.0 */
/**
 * Copyright 2021-2026 NXP
 */
#ifndef PCS_NXP_S32G_XPCS_H
#define PCS_NXP_S32G_XPCS_H

#include <linux/phylink.h>

enum s32g_xpcs_shared {
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
	enum s32g_xpcs_shared pcie_shared;
	struct phylink_pcs pcs;
};

#define phylink_pcs_to_s32g_xpcs(pl_pcs) \
	container_of((pl_pcs), struct s32g_xpcs, pcs)

int s32g_xpcs_init(struct s32g_xpcs *xpcs, struct device *dev,
		   unsigned char id, void __iomem *base, bool ext_clk,
		   unsigned long rate, enum s32g_xpcs_shared pcie_shared);
int s32g_xpcs_init_plls(struct s32g_xpcs *xpcs);
int s32g_xpcs_pre_pcie_2g5(struct s32g_xpcs *xpcs);
void s32g_xpcs_vreset(struct s32g_xpcs *xpcs);
int s32g_xpcs_wait_vreset(struct s32g_xpcs *xpcs);
int s32g_xpcs_reset_rx(struct s32g_xpcs *xpcs);
void s32g_xpcs_disable_an(struct s32g_xpcs *xpcs);

struct phylink_pcs *s32g_serdes_pcs_create(struct device *dev, struct device_node *np);
void s32g_serdes_pcs_destroy(struct phylink_pcs *pcs);

#endif /* PCS_NXP_S32G_XPCS_H */

