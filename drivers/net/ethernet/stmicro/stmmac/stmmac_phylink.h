/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026, StarFive Corporation. */

#ifndef _STMMAC_PHYLINK_H_
#define _STMMAC_PHYLINK_H_

#include <linux/phylink.h>

#ifdef CONFIG_NET_NCSI
static inline bool stmmac_phylink_expects_phy(struct phylink *link)
{
	if (link)
		return phylink_expects_phy(link);

	return false;
}

static inline int stmmac_phylink_pcs_pre_init(struct phylink *link, struct phylink_pcs *pcs)
{
	if (link)
		return phylink_pcs_pre_init(link, pcs);

	return 0;
}

static inline void stmmac_phylink_start(struct phylink *link)
{
	if (link)
		phylink_start(link);
}

static inline void stmmac_phylink_stop(struct phylink *link)
{
	if (link)
		phylink_stop(link);
}

static inline void stmmac_phylink_speed_up(struct phylink *link)
{
	if (link)
		phylink_speed_up(link);
}

static inline void stmmac_phylink_speed_down(struct phylink *link, bool sync)
{
	if (link)
		phylink_speed_down(link, sync);
}

static inline void stmmac_phylink_disconnect_phy(struct phylink *link)
{
	if (link)
		phylink_disconnect_phy(link);
}

static inline void stmmac_phylink_destroy(struct phylink *link)
{
	if (link)
		phylink_destroy(link);
}

static inline void stmmac_phylink_suspend(struct phylink *link, bool mac_wol)
{
	if (link)
		phylink_suspend(link, mac_wol);
}

static inline void stmmac_phylink_prepare_resume(struct phylink *link)
{
	if (link)
		phylink_prepare_resume(link);
}

static inline void stmmac_phylink_resume(struct phylink *link)
{
	if (link)
		phylink_resume(link);
}

static inline void stmmac_phylink_rx_clk_stop_block(struct phylink *link)
{
	if (link)
		phylink_rx_clk_stop_block(link);
}

static inline void stmmac_phylink_rx_clk_stop_unblock(struct phylink *link)
{
	if (link)
		phylink_rx_clk_stop_unblock(link);
}
#else
#define stmmac_phylink_expects_phy		phylink_expects_phy
#define stmmac_phylink_pcs_pre_init		phylink_pcs_pre_init
#define stmmac_phylink_start			phylink_start
#define stmmac_phylink_stop			phylink_stop
#define stmmac_phylink_speed_up			phylink_speed_up
#define stmmac_phylink_speed_down		phylink_speed_down
#define stmmac_phylink_disconnect_phy		phylink_disconnect_phy
#define stmmac_phylink_destroy			phylink_destroy
#define stmmac_phylink_suspend			phylink_suspend
#define stmmac_phylink_prepare_resume		phylink_prepare_resume
#define stmmac_phylink_resume			phylink_resume
#define stmmac_phylink_rx_clk_stop_block	phylink_rx_clk_stop_block
#define stmmac_phylink_rx_clk_stop_unblock	phylink_rx_clk_stop_unblock
#endif /* NET_NCSI */
#endif /* _STMMAC_PHYLINK_H_ */
