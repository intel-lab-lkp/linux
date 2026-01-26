/* SPDX-License-Identifier: GPL-2.0 */
/**
 * Copyright 2026 NXP
 */

#ifndef __LINUX_PCS_NXP_XPCS_H
#define __LINUX_PCS_NXP_XPCS_H

#include <linux/phylink.h>

struct phylink_pcs *s32g_serdes_pcs_create(struct device *dev, struct device_node *np);

#endif /* __LINUX_PCS_XPCS_H */
