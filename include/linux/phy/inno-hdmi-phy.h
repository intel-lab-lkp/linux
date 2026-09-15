/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Common helpers for the Innosilicon HDMI PHY.
 *
 * Copyright (c) 2017 Rockchip Electronics Co. Ltd.
 * Copyright (c) 2025 Samsung Electronics Co., Ltd.
 *
 * Author: Zheng Yang <zhengyang@rock-chips.com>
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * Derived from drivers/phy/rockchip/phy-rockchip-inno-hdmi.c.
 */

#ifndef __PHY_INNO_HDMI_H__
#define __PHY_INNO_HDMI_H__

#include <linux/types.h>

struct clk_rate_request;
struct regmap;

/**
 * struct inno_hdmi_phy_pre_pll_config - pre-PLL settings for one pixel clock
 * @pixclock: pixel clock this entry describes, in Hz
 * @tmdsclock: TMDS clock this entry describes, in Hz
 * @prediv: pre-PLL reference divider
 * @fbdiv: pre-PLL feedback divider
 * @tmds_div_a: TMDS clock divider A
 * @tmds_div_b: TMDS clock divider B
 * @tmds_div_c: TMDS clock divider C
 * @pclk_div_a: pixel clock divider A
 * @pclk_div_b: pixel clock divider B
 * @pclk_div_c: pixel clock divider C
 * @pclk_div_d: pixel clock divider D
 * @vco_div_5_en: divide the VCO output by five
 * @fracdiv: pre-PLL fractional divider, zero to use integer mode
 */
struct inno_hdmi_phy_pre_pll_config {
	unsigned long pixclock;
	unsigned long tmdsclock;
	u8 prediv;
	u16 fbdiv;
	u8 tmds_div_a;
	u8 tmds_div_b;
	u8 tmds_div_c;
	u8 pclk_div_a;
	u8 pclk_div_b;
	u8 pclk_div_c;
	u8 pclk_div_d;
	u8 vco_div_5_en;
	u32 fracdiv;
};

/**
 * struct inno_hdmi_phy_pre_pll - a pre-PLL instance
 * @regmap: register map covering the PHY registers
 * @offset: offset of the PHY register block, in units of the register index.
 *	    Zero where the PHY block starts the register space, 0x100 on the
 *	    StarFive JH7110 where it is preceded by the HDMI controller.
 * @table: pixel clock table, terminated by an entry with a zero pixclock
 */
struct inno_hdmi_phy_pre_pll {
	struct device *dev;
	struct regmap *regmap;
	unsigned int offset;
	const struct inno_hdmi_phy_pre_pll_config *table;
};

const struct inno_hdmi_phy_pre_pll_config *
inno_hdmi_phy_pre_pll_lookup(const struct inno_hdmi_phy_pre_pll *pll,
			     unsigned long pixclock, unsigned long tmdsclock);
int inno_hdmi_phy_pre_pll_determine_rate(const struct inno_hdmi_phy_pre_pll *pll,
					 struct clk_rate_request *req);
unsigned long
inno_hdmi_phy_pre_pll_recalc_rate(const struct inno_hdmi_phy_pre_pll *pll,
				  unsigned long parent_rate);
void inno_hdmi_phy_pre_pll_configure(const struct inno_hdmi_phy_pre_pll *pll,
				     const struct inno_hdmi_phy_pre_pll_config *cfg);
void inno_hdmi_phy_pre_pll_power_down(const struct inno_hdmi_phy_pre_pll *pll,
				      bool power_down);
bool inno_hdmi_phy_pre_pll_is_locked(const struct inno_hdmi_phy_pre_pll *pll);
int inno_hdmi_phy_pre_pll_wait_locked(const struct inno_hdmi_phy_pre_pll *pll,
				      unsigned int timeout_us);

#endif /* __PHY_INNO_HDMI_H__ */
