/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NXP Wireless LAN device driver: vendor command interface
 *
 * Copyright 2026 NXP
 */

#ifndef __NXPWIFI_VENDOR_H__
#define __NXPWIFI_VENDOR_H__

#include <uapi/linux/nl80211-vnd-nxp.h>

void nxpwifi_set_vendor_commands(struct wiphy *wiphy);
#endif /* __NXPWIFI_VENDOR_H__ */
