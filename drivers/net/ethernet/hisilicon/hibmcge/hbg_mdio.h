/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef __HBG_MDIO_H
#define __HBG_MDIO_H

#include "hbg_common.h"

int hbg_mdio_init(struct hbg_priv *priv);
void hbg_phy_start(struct hbg_priv *priv);
void hbg_phy_stop(struct hbg_priv *priv);
void hbg_fix_np_link_fail(struct hbg_priv *priv);
int hbg_convert_mac_speed_to_phy(u32 mac_speed);
u32 hbg_convert_phy_speed_to_mac(int phy_speed);
void hbg_print_link_status(struct hbg_priv *priv);

#endif
