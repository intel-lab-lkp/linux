// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 * Copyright (c) 2023, Linaro Ltd.
 */

#include <linux/delay.h>
#include <linux/iopoll.h>

#include "phy-qcom-hdmi-preqmp.h"

#define REG_HDMI_8960_PHY_REG0					0x00000000

#define REG_HDMI_8960_PHY_REG1					0x00000004

#define REG_HDMI_8960_PHY_REG2					0x00000008

#define REG_HDMI_8960_PHY_REG3					0x0000000c

#define REG_HDMI_8960_PHY_REG4					0x00000010

#define REG_HDMI_8960_PHY_REG5					0x00000014

#define REG_HDMI_8960_PHY_REG6					0x00000018

#define REG_HDMI_8960_PHY_REG7					0x0000001c

#define REG_HDMI_8960_PHY_REG8					0x00000020

#define REG_HDMI_8960_PHY_REG9					0x00000024

#define REG_HDMI_8960_PHY_REG10					0x00000028

#define REG_HDMI_8960_PHY_REG11					0x0000002c

#define REG_HDMI_8960_PHY_REG12					0x00000030
#define HDMI_8960_PHY_REG12_SW_RESET				0x00000020
#define HDMI_8960_PHY_REG12_PWRDN_B				0x00000080

#define REG_HDMI_8960_PHY_REG_BIST_CFG				0x00000034

#define REG_HDMI_8960_PHY_DEBUG_BUS_SEL				0x00000038

#define REG_HDMI_8960_PHY_REG_MISC0				0x0000003c

#define REG_HDMI_8960_PHY_REG13					0x00000040

#define REG_HDMI_8960_PHY_REG14					0x00000044

#define REG_HDMI_8960_PHY_REG15					0x00000048

#define REG_HDMI_8960_PHY_PLL_REFCLK_CFG			0x00000000

#define REG_HDMI_8960_PHY_PLL_CHRG_PUMP_CFG			0x00000004

#define REG_HDMI_8960_PHY_PLL_LOOP_FLT_CFG0			0x00000008

#define REG_HDMI_8960_PHY_PLL_LOOP_FLT_CFG1			0x0000000c

#define REG_HDMI_8960_PHY_PLL_IDAC_ADJ_CFG			0x00000010

#define REG_HDMI_8960_PHY_PLL_I_VI_KVCO_CFG			0x00000014

#define REG_HDMI_8960_PHY_PLL_PWRDN_B				0x00000018
#define HDMI_8960_PHY_PLL_PWRDN_B_PD_PLL			0x00000002
#define HDMI_8960_PHY_PLL_PWRDN_B_PLL_PWRDN_B			0x00000008

#define REG_HDMI_8960_PHY_PLL_SDM_CFG0				0x0000001c

#define REG_HDMI_8960_PHY_PLL_SDM_CFG1				0x00000020

#define REG_HDMI_8960_PHY_PLL_SDM_CFG2				0x00000024

#define REG_HDMI_8960_PHY_PLL_SDM_CFG3				0x00000028

#define REG_HDMI_8960_PHY_PLL_SDM_CFG4				0x0000002c

#define REG_HDMI_8960_PHY_PLL_SSC_CFG0				0x00000030

#define REG_HDMI_8960_PHY_PLL_SSC_CFG1				0x00000034

#define REG_HDMI_8960_PHY_PLL_SSC_CFG2				0x00000038

#define REG_HDMI_8960_PHY_PLL_SSC_CFG3				0x0000003c

#define REG_HDMI_8960_PHY_PLL_LOCKDET_CFG0			0x00000040

#define REG_HDMI_8960_PHY_PLL_LOCKDET_CFG1			0x00000044

#define REG_HDMI_8960_PHY_PLL_LOCKDET_CFG2			0x00000048

#define REG_HDMI_8960_PHY_PLL_VCOCAL_CFG0			0x0000004c

#define REG_HDMI_8960_PHY_PLL_VCOCAL_CFG1			0x00000050

#define REG_HDMI_8960_PHY_PLL_VCOCAL_CFG2			0x00000054

#define REG_HDMI_8960_PHY_PLL_VCOCAL_CFG3			0x00000058

#define REG_HDMI_8960_PHY_PLL_VCOCAL_CFG4			0x0000005c

#define REG_HDMI_8960_PHY_PLL_VCOCAL_CFG5			0x00000060

#define REG_HDMI_8960_PHY_PLL_VCOCAL_CFG6			0x00000064

#define REG_HDMI_8960_PHY_PLL_VCOCAL_CFG7			0x00000068

#define REG_HDMI_8960_PHY_PLL_DEBUG_SEL				0x0000006c

#define REG_HDMI_8960_PHY_PLL_MISC0				0x00000070

#define REG_HDMI_8960_PHY_PLL_MISC1				0x00000074

#define REG_HDMI_8960_PHY_PLL_MISC2				0x00000078

#define REG_HDMI_8960_PHY_PLL_MISC3				0x0000007c

#define REG_HDMI_8960_PHY_PLL_MISC4				0x00000080

#define REG_HDMI_8960_PHY_PLL_MISC5				0x00000084

#define REG_HDMI_8960_PHY_PLL_MISC6				0x00000088

#define REG_HDMI_8960_PHY_PLL_DEBUG_BUS0			0x0000008c

#define REG_HDMI_8960_PHY_PLL_DEBUG_BUS1			0x00000090

#define REG_HDMI_8960_PHY_PLL_DEBUG_BUS2			0x00000094

#define REG_HDMI_8960_PHY_PLL_STATUS0				0x00000098
#define HDMI_8960_PHY_PLL_STATUS0_PLL_LOCK			0x00000001

#define REG_HDMI_8960_PHY_PLL_STATUS1				0x0000009c

#define HDMI_8974_VCO_MAX_FREQ 1125000000UL
#define HDMI_8974_VCO_MIN_FREQ 540000000UL

#define HDMI_8974_COMMON_DIV 5

static unsigned long qcom_28lpm_recalc(struct qcom_hdmi_preqmp_phy *hdmi_phy, unsigned long parent_rate)
{
	unsigned long rate;
	u32 refclk_cfg;
	u32 dc_offset;
	u64 fraq_n;
	u32 val;

	refclk_cfg = hdmi_pll_read(hdmi_phy, REG_HDMI_8960_PHY_PLL_REFCLK_CFG);
	if (refclk_cfg & BIT(1))
		parent_rate /= 2;
	if (refclk_cfg & BIT(3))
		parent_rate *= 2;

	val = hdmi_pll_read(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG0);
	if (val & 0x40) {
		dc_offset = val & 0x3f;
		fraq_n = 0;
	} else {
		dc_offset = hdmi_pll_read(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG1) & 0x3f;
		fraq_n = hdmi_pll_read(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG2) |
			(hdmi_pll_read(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG3) << 8);
	}

	rate = (dc_offset + 1) * parent_rate;
	rate += mult_frac(fraq_n, parent_rate, 0x10000);

	return rate;
}

static int qcom_28lpm_set_rate(struct qcom_hdmi_preqmp_phy *hdmi_phy, unsigned long parent_rate,
			       unsigned long vco_freq, u32 div_idx)
{
	unsigned int pixclk = hdmi_phy->hdmi_opts.pixel_clk_rate;
        unsigned int int_ref_freq;
	unsigned int div;
	unsigned int dc_offset;
	unsigned int sdm_freq_seed;
	unsigned int val;
	bool sdm_mode = false;
        u32 refclk_cfg;
        u32 lf_cfg0;
        u32 lf_cfg1;

	dev_dbg(hdmi_phy->dev, "rate=%u, div = %d, vco = %lu", pixclk, div, vco_freq);

	if (vco_freq % (parent_rate / 2) == 0) {
                refclk_cfg = 0x2;
                int_ref_freq = parent_rate / 2;
        } else {
                refclk_cfg = 0x8;
                int_ref_freq = parent_rate * 2;
                sdm_mode = true;
        }

	dc_offset = vco_freq / int_ref_freq - 1;
	sdm_freq_seed = vco_freq - (dc_offset + 1) * int_ref_freq;
	sdm_freq_seed = mult_frac(sdm_freq_seed, 0x10000, int_ref_freq);

	val = (div_idx << 4) | refclk_cfg;
	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_REFCLK_CFG, val);

	lf_cfg0 = dc_offset >= 30 ? 0 : (dc_offset >= 16 ? 0x10 : 0x20);
	lf_cfg0 += sdm_mode ? 0 : 1;

	/* XXX: 0xc3 instead of 0x33 for qcs404 */
	lf_cfg1 = dc_offset >= 30 ? 0x33 : (dc_offset >= 16 ? 0xbb : 0xf9);

	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_LOOP_FLT_CFG0, lf_cfg0);
	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_LOOP_FLT_CFG1, lf_cfg1);

	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG0,
		       (sdm_mode ? 0 : 0x40) | dc_offset);
	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG1,
		       0x40 | dc_offset);

	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG2,
		       sdm_freq_seed & 0xff);

	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG3,
		       (sdm_freq_seed >> 8) & 0xff);

	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_SDM_CFG4,
		       sdm_freq_seed >> 16);

	vco_freq /= 1000;
	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_VCOCAL_CFG0, vco_freq & 0xff);
	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_VCOCAL_CFG1, vco_freq >> 8);

	hdmi_pll_write(hdmi_phy, REG_HDMI_8960_PHY_PLL_VCOCAL_CFG2, 0x3b);

	return 0;
}

static const unsigned int qcom_hdmi_8974_divs[] = {1, 2, 4, 6};

static unsigned long qcom_hdmi_8960_pll_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct qcom_hdmi_preqmp_phy *hdmi_phy = hw_clk_to_phy(hw);
	u32 div_idx = hdmi_pll_read(hdmi_phy, REG_HDMI_8960_PHY_PLL_REFCLK_CFG);
	unsigned long rate = qcom_28lpm_recalc(hdmi_phy, parent_rate);

	return rate / HDMI_8974_COMMON_DIV / qcom_hdmi_8974_divs[div_idx >> 4];
}

static long qcom_hdmi_8960_pll_round_rate(struct clk_hw *hw, unsigned long rate,
					  unsigned long *parent_rate)
{
	return clamp(rate,
		     HDMI_8974_VCO_MIN_FREQ / HDMI_8974_COMMON_DIV / 6,
		     HDMI_8974_VCO_MAX_FREQ / HDMI_8974_COMMON_DIV / 1);
}

static const struct clk_ops qcom_hdmi_8960_pll_ops = {
	.recalc_rate = qcom_hdmi_8960_pll_recalc_rate,
	.round_rate = qcom_hdmi_8960_pll_round_rate,
};

static int qcom_hdmi_msm8960_phy_pll_enable(struct qcom_hdmi_preqmp_phy *phy)
{
	int pll_lock_retry = 10;
	unsigned int val;
	int ret;

	/* Assert PLL S/W reset */
	hdmi_pll_write(phy, REG_HDMI_8960_PHY_PLL_LOCKDET_CFG2, 0x8d);
	hdmi_pll_write(phy, REG_HDMI_8960_PHY_PLL_LOCKDET_CFG0, 0x10);
	hdmi_pll_write(phy, REG_HDMI_8960_PHY_PLL_LOCKDET_CFG1, 0x1a);

	/* Wait for a short time before de-asserting
	 * to allow the hardware to complete its job.
	 * This much of delay should be fine for hardware
	 * to assert and de-assert.
	 */
	udelay(10);

	/* De-assert PLL S/W reset */
	hdmi_pll_write(phy, REG_HDMI_8960_PHY_PLL_LOCKDET_CFG2, 0x0d);

	val = hdmi_phy_read(phy, REG_HDMI_8960_PHY_REG12);
	val |= HDMI_8960_PHY_REG12_SW_RESET;
	/* Assert PHY S/W reset */
	hdmi_phy_write(phy, REG_HDMI_8960_PHY_REG12, val);
	val &= ~HDMI_8960_PHY_REG12_SW_RESET;
	/*
	 * Wait for a short time before de-asserting to allow the hardware to
	 * complete its job. This much of delay should be fine for hardware to
	 * assert and de-assert.
	 */
	udelay(10);
	/* De-assert PHY S/W reset */
	hdmi_phy_write(phy, REG_HDMI_8960_PHY_REG12, val);
	hdmi_phy_write(phy, REG_HDMI_8960_PHY_REG2,  0x3f);

	val = hdmi_phy_read(phy, REG_HDMI_8960_PHY_REG12);
	val |= HDMI_8960_PHY_REG12_PWRDN_B;
	hdmi_phy_write(phy, REG_HDMI_8960_PHY_REG12, val);
	/* Wait 10 us for enabling global power for PHY */
	mb();
	udelay(10);

	val = hdmi_pll_read(phy, REG_HDMI_8960_PHY_PLL_PWRDN_B);
	val |= HDMI_8960_PHY_PLL_PWRDN_B_PLL_PWRDN_B;
	val &= ~HDMI_8960_PHY_PLL_PWRDN_B_PD_PLL;
	hdmi_pll_write(phy, REG_HDMI_8960_PHY_PLL_PWRDN_B, val);
	hdmi_phy_write(phy, REG_HDMI_8960_PHY_REG2, 0x80);

	while (--pll_lock_retry > 0) {
		ret = readl_poll_timeout(phy->pll_reg + REG_HDMI_8960_PHY_PLL_STATUS0,
					 val, val & HDMI_8960_PHY_PLL_STATUS0_PLL_LOCK,
					 1, 1000);
		if (!ret)
			break;

		/*
		 * PLL has still not locked.
		 * Do a software reset and try again
		 * Assert PLL S/W reset first
		 */
		hdmi_pll_write(phy, REG_HDMI_8960_PHY_PLL_LOCKDET_CFG2, 0x8d);
		udelay(10);
		hdmi_pll_write(phy, REG_HDMI_8960_PHY_PLL_LOCKDET_CFG2, 0x0d);

		/*
		 * Wait for a short duration for the PLL calibration
		 * before checking if the PLL gets locked
		 */
		udelay(350);
	}

	return ret;
}

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

static int qcom_hdmi_msm8960_phy_set_rate(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	unsigned int pixclk = hdmi_phy->hdmi_opts.pixel_clk_rate;
	/* XXX: 19.2 for qcs404 */
	unsigned long parent_rate = 27000;
	unsigned long vco_freq;
	int div_idx;
	u32 div;

	div_idx = qcom_hdmi_msm8974_phy_find_div(pixclk);
	if (WARN_ON(div_idx < 0))
		return div_idx;

	div = qcom_hdmi_8974_divs[div_idx];
	vco_freq = pixclk * HDMI_8974_COMMON_DIV * div;

	return qcom_28lpm_set_rate(hdmi_phy, parent_rate, vco_freq, div_idx);
}

static void qcom_hdmi_msm8960_phy_pll_disable(struct qcom_hdmi_preqmp_phy *phy)
{
	unsigned int val;

	val = hdmi_phy_read(phy, REG_HDMI_8960_PHY_REG12);
	val &= ~HDMI_8960_PHY_REG12_PWRDN_B;
	hdmi_phy_write(phy, REG_HDMI_8960_PHY_REG12, val);

	val = hdmi_pll_read(phy, REG_HDMI_8960_PHY_PLL_PWRDN_B);
	val |= HDMI_8960_PHY_REG12_SW_RESET;
	val &= ~HDMI_8960_PHY_REG12_PWRDN_B;
	hdmi_pll_write(phy, REG_HDMI_8960_PHY_PLL_PWRDN_B, val);
	/* Make sure HDMI PHY/PLL are powered down */
	mb();
}

static int qcom_hdmi_msm8960_phy_power_on(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	int ret;

	ret = qcom_hdmi_msm8960_phy_set_rate(hdmi_phy);
	if (ret)
		return ret;

	ret = qcom_hdmi_msm8960_phy_pll_enable(hdmi_phy);
	if (ret)
		return ret;

	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG2, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG0, 0x1b);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG1, 0xf2);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG4, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG5, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG6, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG7, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG8, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG9, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG10, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG11, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG3, 0x20);

	return 0;
}

static int qcom_hdmi_msm8960_phy_power_off(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	hdmi_phy_write(hdmi_phy, REG_HDMI_8960_PHY_REG2, 0x7f);

	qcom_hdmi_msm8960_phy_pll_disable(hdmi_phy);

	return 0;
}

const struct clk_parent_data msm8960_hdmi_pll_parent = {
	.fw_name = "pxo", .name = "pxo_board",
};

const struct qcom_hdmi_preqmp_cfg msm8960_hdmi_phy_cfg = {
	.clk_names = { "slave_iface" },
	.num_clks = 1,

	.reg_names = { "core-vdda" },
	.num_regs = 1,

	.power_on = qcom_hdmi_msm8960_phy_power_on,
	.power_off = qcom_hdmi_msm8960_phy_power_off,

	.pll_ops = &qcom_hdmi_8960_pll_ops,
	.pll_parent = &msm8960_hdmi_pll_parent,
};
