/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2021-2026 NXP
 */
#ifndef PCS_NXP_S32G_XPCS_H
#define PCS_NXP_S32G_XPCS_H

#include <linux/types.h>

struct device;
struct phylink_pcs;

/*
 * Opaque. The XPCS state, including its regmap and the embedded
 * phylink_pcs, is private to the PCS driver; the SerDes PHY driver only
 * ever holds pointers handed out by s32g_xpcs_create().
 */
struct s32g_xpcs;

/*
 * Whether the XPCS shares the combo PHY with the PCIe lane. In the shared
 * modes the PMA power-up is driven from the PCIe side, which changes the
 * bring-up sequence.
 */
enum s32g_xpcs_shared {
	S32G_NOT_SHARED,
	S32G_PCIE_XPCS_1G,
	S32G_PCIE_XPCS_2G5,
};

struct s32g_xpcs *s32g_xpcs_create(struct device *dev, unsigned char id,
				   void __iomem *base, bool ext_clk,
				   unsigned long rate,
				   enum s32g_xpcs_shared pcie_shared);
struct phylink_pcs *s32g_xpcs_pcs(struct s32g_xpcs *xpcs);

/*
 * Bring-up steps, called by the SerDes PHY driver. They are separate
 * because the dual-XPCS mode needs cross-instance barriers: every
 * instance must complete a step before any instance starts the next.
 */
int s32g_xpcs_init_plls(struct s32g_xpcs *xpcs);
void s32g_xpcs_vreset(struct s32g_xpcs *xpcs);
int s32g_xpcs_wait_vreset(struct s32g_xpcs *xpcs);
int s32g_xpcs_reset_rx(struct s32g_xpcs *xpcs);
void s32g_xpcs_disable_an(struct s32g_xpcs *xpcs);

#endif /* PCS_NXP_S32G_XPCS_H */
