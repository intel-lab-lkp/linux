/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2021-2026 NXP
 */
#ifndef PHY_NXP_S32G_SERDES_H
#define PHY_NXP_S32G_SERDES_H

#include <linux/err.h>

struct device;
struct device_node;
struct phylink_pcs;

#if IS_REACHABLE(CONFIG_PHY_S32G_SERDES)
struct phylink_pcs *s32g_serdes_pcs_create(struct device *dev,
					   struct device_node *np);
#else
static inline struct phylink_pcs *
s32g_serdes_pcs_create(struct device *dev, struct device_node *np)
{
	return ERR_PTR(-ENODEV);
}
#endif

#endif /* PHY_NXP_S32G_SERDES_H */
