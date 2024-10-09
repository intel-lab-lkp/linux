/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NXP Wireless LAN device driver: CFG80211
 *
 * Copyright 2011-2020 NXP
 */

#ifndef __MWIFIEX_CFG80211__
#define __MWIFIEX_CFG80211__

#include <net/cfg80211.h>

#include "main.h"

int mwifiex_register_cfg80211(struct mwifiex_adapter *);
int mwifiex_cfg80211_change_beacon_data(struct wiphy *wiphy,
					struct net_device *dev,
					struct cfg80211_beacon_data *data);
#endif
