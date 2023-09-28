// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 * Copyright (c) 2023, Linaro Ltd.
 */

#include <linux/delay.h>
#include <linux/iopoll.h>

#include "phy-qcom-hdmi-preqmp.h"
#include "phy-qcom-uniphy.h"

#define REG_HDMI_8x74_ANA_CFG0					0x00000000
#define REG_HDMI_8x74_ANA_CFG1					0x00000004
#define REG_HDMI_8x74_ANA_CFG2					0x00000008
#define REG_HDMI_8x74_ANA_CFG3					0x0000000c
#define REG_HDMI_8x74_PD_CTRL0					0x00000010
#define REG_HDMI_8x74_PD_CTRL1					0x00000014
#define REG_HDMI_8x74_GLB_CFG					0x00000018
#define REG_HDMI_8x74_DCC_CFG0					0x0000001c
#define REG_HDMI_8x74_DCC_CFG1					0x00000020
#define REG_HDMI_8x74_TXCAL_CFG0				0x00000024
#define REG_HDMI_8x74_TXCAL_CFG1				0x00000028
#define REG_HDMI_8x74_TXCAL_CFG2				0x0000002c
#define REG_HDMI_8x74_TXCAL_CFG3				0x00000030
#define REG_HDMI_8x74_BIST_CFG0					0x00000034
#define REG_HDMI_8x74_BIST_PATN0				0x0000003c
#define REG_HDMI_8x74_BIST_PATN1				0x00000040
#define REG_HDMI_8x74_BIST_PATN2				0x00000044
#define REG_HDMI_8x74_BIST_PATN3				0x00000048
#define REG_HDMI_8x74_STATUS					0x0000005c

#define HDMI_8974_VCO_MAX_FREQ 1800000000UL
#define HDMI_8974_VCO_MIN_FREQ  600000000UL

#define HDMI_8974_COMMON_DIV 5

static void qcom_uniphy_setup(void __iomem *base, unsigned int ref_freq,
			      bool sdm_mode,
			      bool ref_freq_mult_2,
			      bool dither,
			      unsigned int refclk_div,
			      unsigned int vco_freq)
{
	unsigned int int_ref_freq = ref_freq * (ref_freq_mult_2 ? 2 : 1);
	unsigned int div_in_freq = vco_freq / refclk_div;
	unsigned int dc_offset = div_in_freq / int_ref_freq - 1;
	unsigned int sdm_freq_seed;
	unsigned int val;
	unsigned int remain = div_in_freq - (dc_offset + 1) * int_ref_freq;
	sdm_freq_seed = mult_frac(remain, 0x10000, int_ref_freq);

	val = (ref_freq_mult_2 ? BIT(0) : 0) |
		((refclk_div - 1) << 2);
	writel(val, base + UNIPHY_PLL_REFCLK_CFG);

	writel(sdm_mode ? 0 : 0x40 + dc_offset, base + UNIPHY_PLL_SDM_CFG0);

	writel(dither ? 0x40 + dc_offset: 0, base + UNIPHY_PLL_SDM_CFG1);

	writel(sdm_freq_seed & 0xff, base + UNIPHY_PLL_SDM_CFG2);

	writel((sdm_freq_seed >> 8) & 0xff, base + UNIPHY_PLL_SDM_CFG3);

	writel(sdm_freq_seed >> 16, base + UNIPHY_PLL_SDM_CFG4);

	ref_freq = ref_freq * 5 / 1000;
	writel(ref_freq & 0xff, base + UNIPHY_PLL_CAL_CFG8);

	writel(ref_freq >> 8, base + UNIPHY_PLL_CAL_CFG9);

	vco_freq /= 1000;
	writel(vco_freq & 0xff, base + UNIPHY_PLL_CAL_CFG10);

	writel(vco_freq >> 8, base + UNIPHY_PLL_CAL_CFG11);
}

static unsigned long qcom_uniphy_recalc(void __iomem *base, unsigned long parent_rate)
{
	unsigned long rate;
	u32 refclk_cfg;
	u32 dc_offset;
	u64 fraq_n;
	u32 val;

	refclk_cfg = readl(base + UNIPHY_PLL_REFCLK_CFG);
	if (refclk_cfg & BIT(0))
		parent_rate *= 2;

	val = readl(base + UNIPHY_PLL_SDM_CFG0);
	if (val & 0x40) {
		dc_offset = val & 0x3f;
		fraq_n = 0;
	} else {
		dc_offset = readl(base + UNIPHY_PLL_SDM_CFG1) & 0x3f;
		fraq_n = readl(base + UNIPHY_PLL_SDM_CFG2) |
			(readl(base + UNIPHY_PLL_SDM_CFG3) << 8);
	}

	rate = (dc_offset + 1) * parent_rate;
	rate += mult_frac(fraq_n, parent_rate, 0x10000);

	rate *= (refclk_cfg >> 2) * 0x3 + 1;

	return rate;
}

static const unsigned int qcom_hdmi_8974_divs[] = {1, 2, 4, 6};

static unsigned long qcom_hdmi_8974_pll_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct qcom_hdmi_preqmp_phy *hdmi_phy = hw_clk_to_phy(hw);
	u32 div_idx = hdmi_pll_read(hdmi_phy, UNIPHY_PLL_POSTDIV1_CFG);
	unsigned long rate = qcom_uniphy_recalc(hdmi_phy->pll_reg, parent_rate);

	return  rate / HDMI_8974_COMMON_DIV / qcom_hdmi_8974_divs[div_idx & 0x3];
}

static long qcom_hdmi_8974_pll_round_rate(struct clk_hw *hw, unsigned long rate,
					  unsigned long *parent_rate)
{
	return clamp(rate,
		     HDMI_8974_VCO_MIN_FREQ / HDMI_8974_COMMON_DIV / 6,
		     HDMI_8974_VCO_MAX_FREQ / HDMI_8974_COMMON_DIV / 1);
}

static const struct clk_ops qcom_hdmi_8974_pll_ops = {
	.recalc_rate = qcom_hdmi_8974_pll_recalc_rate,
	.round_rate = qcom_hdmi_8974_pll_round_rate,
};

static int qcom_hdmi_msm8974_phy_find_div(unsigned int pixclk)
{
	int i;
	unsigned int min_freq = HDMI_8974_VCO_MIN_FREQ / HDMI_8974_COMMON_DIV / 1000;

	if (pixclk > HDMI_8974_VCO_MAX_FREQ / HDMI_8974_COMMON_DIV / 1000)
		return -E2BIG;

	for (i = 0; i < ARRAY_SIZE(qcom_hdmi_8974_divs); i++) {
		if (pixclk >= min_freq / qcom_hdmi_8974_divs[i])
			return i;
	}

	return -EINVAL;
}

static int qcom_hdmi_msm8974_phy_pll_set_rate(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	unsigned int pixclk = hdmi_phy->hdmi_opts.pixel_clk_rate;
	unsigned long vco_rate;
	unsigned int div;
	int div_idx = 0;

	div_idx = qcom_hdmi_msm8974_phy_find_div(pixclk);
	if (WARN_ON(div_idx < 0))
		return div_idx;

	div = qcom_hdmi_8974_divs[div_idx];
	vco_rate = pixclk * HDMI_8974_COMMON_DIV * div;

	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_GLB_CFG, 0x81);

	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_GLB_CFG, 0x01);
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_VCOLPF_CFG, 0x19);
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_LPFR_CFG, 0x0e);
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_LPFC1_CFG, 0x20);
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_LPFC2_CFG, 0x0d);

	qcom_uniphy_setup(hdmi_phy->pll_reg, 19200, true, true, true, 1, vco_rate);

	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_LKDET_CFG0, 0x10);
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_LKDET_CFG1, 0x1a);
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_LKDET_CFG2, 0x05);

	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_POSTDIV1_CFG, div_idx);

	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_POSTDIV2_CFG, 0x00);
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_POSTDIV3_CFG, 0x00);
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_CAL_CFG2, 0x01);

	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_PD_CTRL0, 0x1f);
	udelay(50);

	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_GLB_CFG, 0x0f);

	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_PD_CTRL1, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG2, 0x10);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG0, 0xdb);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG1, 0x43);
	if (pixclk == 297000) {
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG2, 0x06);
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG3, 0x03);
	} else if (pixclk == 268500) {
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG2, 0x05);
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG3, 0x00);
	} else {
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG2, 0x02);
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG3, 0x00);
	}

	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_VREG_CFG, 0x04);

	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_DCC_CFG0, 0xd0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_DCC_CFG1, 0x1a);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_TXCAL_CFG0, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_TXCAL_CFG1, 0x00);
	if (pixclk == 268500) {
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_TXCAL_CFG2, 0x11);
	} else {
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_TXCAL_CFG2, 0x02);
	}
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_TXCAL_CFG3, 0x05);
	udelay(200);

	return 0;
}

static int qcom_hdmi_msm8974_phy_pll_enable(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	int ret;
	unsigned long status;

	/* Global enable */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_GLB_CFG, 0x81);

	/* Power up power gen */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_PD_CTRL0, 0x00);
	udelay(350);

	/* PLL power up */
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_GLB_CFG, 0x01);
	udelay(5);

	/* Power up PLL LDO */
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_GLB_CFG, 0x03);
	udelay(350);

	/* PLL power up */
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_GLB_CFG, 0x0f);
	udelay(350);

	/* Poll for PLL ready status */
	ret = readl_poll_timeout(hdmi_phy->pll_reg + UNIPHY_PLL_STATUS,
				 status, status & BIT(0),
				 100, 2000);
	if (ret) {
		dev_warn(hdmi_phy->dev, "HDMI PLL not ready\n");
		goto err;
	}

	udelay(350);

	/* Poll for PHY ready status */
	ret = readl_poll_timeout(hdmi_phy->phy_reg + REG_HDMI_8x74_STATUS,
				 status, status & BIT(0),
				 100, 2000);
	if (ret) {
		dev_warn(hdmi_phy->dev, "HDMI PHY not ready\n");
		goto err;
	}

	return 0;

err:
	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_GLB_CFG, 0);
	udelay(5);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_GLB_CFG, 0);

	return ret;
}

static int qcom_hdmi_msm8974_phy_power_on(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	int ret;

	ret = qcom_hdmi_msm8974_phy_pll_set_rate(hdmi_phy);
	if (ret)
		return ret;

	ret = qcom_hdmi_msm8974_phy_pll_enable(hdmi_phy);
	if (ret)
		return ret;

	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG0,   0x1b);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_ANA_CFG1,   0xf2);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_BIST_CFG0,  0x0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_BIST_PATN0, 0x0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_BIST_PATN1, 0x0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_BIST_PATN2, 0x0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_BIST_PATN3, 0x0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_PD_CTRL1,   0x20);

	return 0;
}

static int qcom_hdmi_msm8974_phy_power_off(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_PD_CTRL0, 0x7f);

	hdmi_pll_write(hdmi_phy, UNIPHY_PLL_GLB_CFG, 0);
	udelay(5);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x74_GLB_CFG, 0);

	return 0;
}

const struct clk_parent_data msm8974_hdmi_pll_parent = {
	.fw_name = "xo", .name = "xo_board",
};

const struct qcom_hdmi_preqmp_cfg msm8974_hdmi_phy_cfg = {
	.clk_names = { "iface", "alt_iface" },
	.num_clks = 2,

	.reg_names = { "vddio", "core-vdda" },
	.reg_init_load = { 100000, 10000 },
	.num_regs = 2,

	.power_on = qcom_hdmi_msm8974_phy_power_on,
	.power_off = qcom_hdmi_msm8974_phy_power_off,

	.pll_ops = &qcom_hdmi_8974_pll_ops,
	.pll_parent = &msm8974_hdmi_pll_parent,
};
