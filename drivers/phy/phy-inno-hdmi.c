// SPDX-License-Identifier: GPL-2.0-or-later
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
 *
 * The same PHY IP is used by several SoCs, which differ in where the PHY
 * register block sits and in the pixel clock table they support, but share
 * the pre-PLL programming sequence.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/phy/inno-hdmi-phy.h>

#define INNO_PRE_PLL_CONTROL			0xa0
#define INNO_PRE_PLL_POWER_DOWN			BIT(0)
#define INNO_PCLK_VCO_DIV_5_MASK		BIT(1)
#define INNO_PCLK_VCO_DIV_5(x)			FIELD_PREP(BIT(1), x)

#define INNO_PRE_PLL_DIV_1			0xa1
#define INNO_PRE_PLL_PRE_DIV_MASK		GENMASK(5, 0)
#define INNO_PRE_PLL_PRE_DIV(x)			FIELD_PREP(GENMASK(5, 0), x)

#define INNO_PRE_PLL_DIV_2			0xa2
#define INNO_SPREAD_SPECTRUM_MOD_DISABLE	BIT(6)
#define INNO_PRE_PLL_FRAC_DIV_DISABLE		FIELD_PREP(GENMASK(5, 4), 3)
#define INNO_PRE_PLL_FB_DIV_11_8_MASK		GENMASK(3, 0)
#define INNO_PRE_PLL_FB_DIV_11_8(x)		FIELD_PREP(GENMASK(3, 0), (x) >> 8)

#define INNO_PRE_PLL_DIV_3			0xa3
#define INNO_PRE_PLL_FB_DIV_7_0(x)		FIELD_PREP(GENMASK(7, 0), x)

#define INNO_PRE_PLL_TMDSCLK_DIV		0xa4
#define INNO_PRE_PLL_TMDSCLK_DIV_C(x)		FIELD_PREP(GENMASK(1, 0), x)
#define INNO_PRE_PLL_TMDSCLK_DIV_B(x)		FIELD_PREP(GENMASK(3, 2), x)
#define INNO_PRE_PLL_TMDSCLK_DIV_A(x)		FIELD_PREP(GENMASK(5, 4), x)

#define INNO_PCLK_DIV_AB			0xa5
#define INNO_PCLK_DIV_B_MASK			GENMASK(6, 5)
#define INNO_PCLK_DIV_B(x)			FIELD_PREP(GENMASK(6, 5), x)
#define INNO_PCLK_DIV_A_MASK			GENMASK(4, 0)
#define INNO_PCLK_DIV_A(x)			FIELD_PREP(GENMASK(4, 0), x)

#define INNO_PCLK_DIV_CD			0xa6
#define INNO_PCLK_DIV_C(x)			FIELD_PREP(GENMASK(6, 5), x)
#define INNO_PCLK_DIV_D_MASK			GENMASK(4, 0)
#define INNO_PCLK_DIV_D(x)			FIELD_PREP(GENMASK(4, 0), x)

#define INNO_PRE_PLL_LOCK_STATUS		0xa9
#define INNO_PRE_PLL_LOCK			BIT(0)

#define INNO_PRE_PLL_FRAC_DIV_23_16		0xd1
#define INNO_PRE_PLL_FRAC_DIV_15_8		0xd2
#define INNO_PRE_PLL_FRAC_DIV_7_0		0xd3
#define INNO_PRE_PLL_FRAC_DIV(x)		FIELD_PREP(GENMASK(7, 0), x)

#define INNO_FRAC_DIV_WIDTH			24

static unsigned int inno_reg(const struct inno_hdmi_phy_pre_pll *pll,
			     unsigned int reg)
{
	return (pll->offset + reg) * 4;
}

static void inno_write(const struct inno_hdmi_phy_pre_pll *pll,
		       unsigned int reg, u8 val)
{
	regmap_write(pll->regmap, inno_reg(pll, reg), val);
}

static u8 inno_read(const struct inno_hdmi_phy_pre_pll *pll, unsigned int reg)
{
	unsigned int val;

	regmap_read(pll->regmap, inno_reg(pll, reg), &val);

	return val;
}

static void inno_update_bits(const struct inno_hdmi_phy_pre_pll *pll,
			     unsigned int reg, u8 mask, u8 val)
{
	regmap_update_bits(pll->regmap, inno_reg(pll, reg), mask, val);
}

/**
 * inno_hdmi_phy_pre_pll_lookup - find the settings for a pixel clock
 * @pll: pre-PLL instance
 * @pixclock: requested pixel clock, in Hz
 * @tmdsclock: requested TMDS clock, in Hz
 *
 * Return: the matching table entry, or an ERR_PTR if the PHY cannot generate
 * the requested combination.
 */
const struct inno_hdmi_phy_pre_pll_config *
inno_hdmi_phy_pre_pll_lookup(const struct inno_hdmi_phy_pre_pll *pll,
			     unsigned long pixclock, unsigned long tmdsclock)
{
	const struct inno_hdmi_phy_pre_pll_config *cfg;

	for (cfg = pll->table; cfg->pixclock != 0; cfg++)
		if (cfg->pixclock == pixclock && cfg->tmdsclock == tmdsclock)
			return cfg;

	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(inno_hdmi_phy_pre_pll_lookup);

/**
 * inno_hdmi_phy_pre_pll_determine_rate - clk_ops.determine_rate helper
 * @pll: pre-PLL instance
 * @req: rate request, updated with the rate the PHY would produce
 *
 * The PHY can only generate the pixel clocks described by its table, so a
 * request that does not appear there is rejected rather than rounded.
 *
 * Return: 0 on success, -EINVAL if the rate is not supported.
 */
int inno_hdmi_phy_pre_pll_determine_rate(const struct inno_hdmi_phy_pre_pll *pll,
					 struct clk_rate_request *req)
{
	const struct inno_hdmi_phy_pre_pll_config *cfg;
	unsigned long rate = rounddown(req->rate, 1000);

	for (cfg = pll->table; cfg->pixclock != 0; cfg++) {
		if (cfg->pixclock == rate) {
			req->rate = cfg->pixclock;
			return 0;
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(inno_hdmi_phy_pre_pll_determine_rate);

/**
 * inno_hdmi_phy_pre_pll_recalc_rate - clk_ops.recalc_rate helper
 * @pll: pre-PLL instance
 * @parent_rate: rate of the reference clock, in Hz
 *
 * Return: the pixel clock the pre-PLL is currently programmed for, in Hz.
 */
unsigned long
inno_hdmi_phy_pre_pll_recalc_rate(const struct inno_hdmi_phy_pre_pll *pll,
				  unsigned long parent_rate)
{
	u8 nd, no_a, no_b, no_d;
	unsigned long frac;
	u64 vco;
	u16 nf;

	nd = inno_read(pll, INNO_PRE_PLL_DIV_1) & INNO_PRE_PLL_PRE_DIV_MASK;
	nf = (inno_read(pll, INNO_PRE_PLL_DIV_2) &
	      INNO_PRE_PLL_FB_DIV_11_8_MASK) << 8;
	nf |= inno_read(pll, INNO_PRE_PLL_DIV_3);
	vco = parent_rate * nf;

	if (!(inno_read(pll, INNO_PRE_PLL_DIV_2) &
	      INNO_PRE_PLL_FRAC_DIV_DISABLE)) {
		frac = inno_read(pll, INNO_PRE_PLL_FRAC_DIV_7_0) |
		       (inno_read(pll, INNO_PRE_PLL_FRAC_DIV_15_8) << 8) |
		       (inno_read(pll, INNO_PRE_PLL_FRAC_DIV_23_16) << 16);
		vco += DIV_ROUND_CLOSEST(parent_rate * frac,
					 BIT(INNO_FRAC_DIV_WIDTH));
	}

	if (inno_read(pll, INNO_PRE_PLL_CONTROL) & INNO_PCLK_VCO_DIV_5_MASK) {
		do_div(vco, nd * 5);
	} else {
		no_a = inno_read(pll, INNO_PCLK_DIV_AB) & INNO_PCLK_DIV_A_MASK;
		no_b = FIELD_GET(INNO_PCLK_DIV_B_MASK,
				 inno_read(pll, INNO_PCLK_DIV_AB)) + 2;
		no_d = inno_read(pll, INNO_PCLK_DIV_CD) & INNO_PCLK_DIV_D_MASK;

		do_div(vco, nd * (no_a == 1 ? no_b : no_a) * no_d * 2);
	}

	return DIV_ROUND_CLOSEST((unsigned long)vco, 1000) * 1000;
}
EXPORT_SYMBOL_GPL(inno_hdmi_phy_pre_pll_recalc_rate);

/**
 * inno_hdmi_phy_pre_pll_configure - program the pre-PLL dividers
 * @pll: pre-PLL instance
 * @cfg: settings to program
 *
 * The caller is responsible for powering the pre-PLL down beforehand and back
 * up afterwards, and for waiting for it to lock.
 */
void inno_hdmi_phy_pre_pll_configure(const struct inno_hdmi_phy_pre_pll *pll,
				     const struct inno_hdmi_phy_pre_pll_config *cfg)
{
	u8 val;

	inno_update_bits(pll, INNO_PRE_PLL_CONTROL, INNO_PCLK_VCO_DIV_5_MASK,
			 INNO_PCLK_VCO_DIV_5(cfg->vco_div_5_en));
	inno_write(pll, INNO_PRE_PLL_DIV_1, INNO_PRE_PLL_PRE_DIV(cfg->prediv));

	val = INNO_SPREAD_SPECTRUM_MOD_DISABLE;
	if (!cfg->fracdiv)
		val |= INNO_PRE_PLL_FRAC_DIV_DISABLE;

	inno_write(pll, INNO_PRE_PLL_DIV_2,
		   INNO_PRE_PLL_FB_DIV_11_8(cfg->fbdiv) | val);
	inno_write(pll, INNO_PRE_PLL_DIV_3,
		   INNO_PRE_PLL_FB_DIV_7_0(cfg->fbdiv));

	inno_write(pll, INNO_PCLK_DIV_AB,
		   INNO_PCLK_DIV_A(cfg->pclk_div_a) |
		   INNO_PCLK_DIV_B(cfg->pclk_div_b));
	inno_write(pll, INNO_PCLK_DIV_CD,
		   INNO_PCLK_DIV_C(cfg->pclk_div_c) |
		   INNO_PCLK_DIV_D(cfg->pclk_div_d));

	inno_write(pll, INNO_PRE_PLL_TMDSCLK_DIV,
		   INNO_PRE_PLL_TMDSCLK_DIV_A(cfg->tmds_div_a) |
		   INNO_PRE_PLL_TMDSCLK_DIV_B(cfg->tmds_div_b) |
		   INNO_PRE_PLL_TMDSCLK_DIV_C(cfg->tmds_div_c));

	inno_write(pll, INNO_PRE_PLL_FRAC_DIV_7_0,
		   INNO_PRE_PLL_FRAC_DIV(cfg->fracdiv));
	inno_write(pll, INNO_PRE_PLL_FRAC_DIV_15_8,
		   INNO_PRE_PLL_FRAC_DIV(cfg->fracdiv >> 8));
	inno_write(pll, INNO_PRE_PLL_FRAC_DIV_23_16,
		   INNO_PRE_PLL_FRAC_DIV(cfg->fracdiv >> 16));
}
EXPORT_SYMBOL_GPL(inno_hdmi_phy_pre_pll_configure);

/**
 * inno_hdmi_phy_pre_pll_power_down - power the pre-PLL down or up
 * @pll: pre-PLL instance
 * @power_down: true to power down, false to power up
 */
void inno_hdmi_phy_pre_pll_power_down(const struct inno_hdmi_phy_pre_pll *pll,
				      bool power_down)
{
	inno_update_bits(pll, INNO_PRE_PLL_CONTROL, INNO_PRE_PLL_POWER_DOWN,
			 power_down ? INNO_PRE_PLL_POWER_DOWN : 0);
}
EXPORT_SYMBOL_GPL(inno_hdmi_phy_pre_pll_power_down);

/**
 * inno_hdmi_phy_pre_pll_is_powered - report whether the pre-PLL is powered up
 * @pll: pre-PLL instance
 */
bool inno_hdmi_phy_pre_pll_is_powered(const struct inno_hdmi_phy_pre_pll *pll)
{
	return !(inno_read(pll, INNO_PRE_PLL_CONTROL) & INNO_PRE_PLL_POWER_DOWN);
}
EXPORT_SYMBOL_GPL(inno_hdmi_phy_pre_pll_is_powered);

/**
 * inno_hdmi_phy_pre_pll_is_locked - report whether the pre-PLL has locked
 * @pll: pre-PLL instance
 */
bool inno_hdmi_phy_pre_pll_is_locked(const struct inno_hdmi_phy_pre_pll *pll)
{
	return inno_read(pll, INNO_PRE_PLL_LOCK_STATUS) & INNO_PRE_PLL_LOCK;
}
EXPORT_SYMBOL_GPL(inno_hdmi_phy_pre_pll_is_locked);

/**
 * inno_hdmi_phy_pre_pll_wait_locked - wait for the pre-PLL to lock
 * @pll: pre-PLL instance
 * @timeout_us: how long to wait, in microseconds
 *
 * Return: 0 once locked, -ETIMEDOUT otherwise.
 */
int inno_hdmi_phy_pre_pll_wait_locked(const struct inno_hdmi_phy_pre_pll *pll,
				      unsigned int timeout_us)
{
	unsigned int val;

	return regmap_read_poll_timeout(pll->regmap,
					inno_reg(pll, INNO_PRE_PLL_LOCK_STATUS),
					val, val & INNO_PRE_PLL_LOCK,
					1000, timeout_us);
}
EXPORT_SYMBOL_GPL(inno_hdmi_phy_pre_pll_wait_locked);

MODULE_DESCRIPTION("Innosilicon HDMI PHY common helpers");
MODULE_LICENSE("GPL");
