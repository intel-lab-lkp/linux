// SPDX-License-Identifier: GPL-2.0
/**
 * SerDes driver for S32G SoCs
 *
 * Copyright 2021-2026 NXP
 */

#include <dt-bindings/phy/phy.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/pcs/pcs-nxp-xpcs.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/processor.h>
#include <linux/reset.h>
#include <linux/units.h>

#include "phy-nxp-s32g-xpcs.h"

#define S32G_SERDES_XPCS_MAX			2
#define S32G_SERDES_MODE_MAX			5

#define EXTERNAL_CLK_NAME			"ext"
#define INTERNAL_CLK_NAME			"ref"

/* Serdes Sub system registers */

#define S32G_PCIE_PHY_GEN_CTRL			0x0
#define  REF_USE_PAD				BIT(17)
#define  RX_SRIS_MODE				BIT(9)

#define S32G_PCIE_PHY_MPLLA_CTRL		0x10
#define  MPLL_STATE				BIT(30)

#define S32G_PCIE_PHY_MPLLB_CTRL		0x14
#define  MPLLB_SSC_EN				BIT(1)

#define S32G_PCIE_PHY_EXT_CTRL_SEL		0x18
#define  EXT_PHY_CTRL_SEL			BIT(0)

#define S32G_PCIE_PHY_EXT_BS_CTRL		0x1C
#define  EXT_BS_TX_LOWSWING			BIT(6)
#define  EXT_BS_RX_BIGSWING			BIT(5)
#define  EXT_BS_RX_LEVEL_MASK			GENMASK(4, 0)

#define S32G_PCIE_PHY_REF_CLK_CTRL		0x20
#define  EXT_REF_RANGE_MASK			GENMASK(5, 3)
#define  REF_CLK_DIV2_EN			BIT(2)
#define  REF_CLK_MPLLB_DIV2_EN			BIT(1)

#define S32G_PCIE_PHY_EXT_MPLLA_CTRL_1		0x30
#define  EXT_MPLLA_BANDWIDTH_MASK		GENMASK(15, 0)

#define S32G_PCIE_PHY_EXT_MPLLB_CTRL_1		0x40
#define  EXT_MPLLB_DIV_MULTIPLIER_MASK		GENMASK(31, 24)
#define  EXT_MPLLB_DIV_CLK_EN			BIT(19)
#define  EXT_MPLLB_DIV8_CLK_EN			BIT(18)
#define  EXT_MPLLB_DIV10_CLK_EN			BIT(16)
#define  EXT_MPLLB_BANDWIDTH_MASK		GENMASK(15, 0)

#define S32G_PCIE_PHY_EXT_MPLLB_CTRL_2		0x44
#define  EXT_MPLLB_FRACN_CTRL_MASK		GENMASK(22, 12)
#define  MPLLB_MULTIPLIER_MASK			GENMASK(8, 0)

#define S32G_PCIE_PHY_EXT_MPLLB_CTRL_3		0x48
#define  EXT_MPLLB_WORD_DIV2_EN			BIT(31)
#define  EXT_MPLLB_TX_CLK_DIV_MASK		GENMASK(30, 28)

#define S32G_PCIE_PHY_EXT_MISC_CTRL_1		0xA0
#define  EXT_RX_LOS_THRESHOLD_MASK		GENMASK(7, 1)
#define  EXT_RX_VREF_CTRL_MASK			GENMASK(28, 24)

#define S32G_PCIE_PHY_EXT_MISC_CTRL_2		0xA4
#define  EXT_TX_VBOOST_LVL_MASK			GENMASK(18, 16)
#define  EXT_TX_TERM_CTRL_MASK			GENMASK(26, 24)

#define S32G_PCIE_PHY_XPCS1_RX_OVRD_CTRL	0xD0
#define  XPCS1_RX_VCO_LD_VAL_MASK		GENMASK(28, 16)
#define  XPCS1_RX_REF_LD_VAL_MASK		GENMASK(14, 8)

#define S32G_SS_RW_REG_0			0xF0
#define  SUBMODE_MASK				GENMASK(3, 0)
#define  CLKEN_MASK				BIT(23)
#define  PHY0_CR_PARA_SEL			BIT(9)

/* PCIe phy subsystem registers */

#define S32G_PHY_REG_ADDR			0x0
#define  PHY_REG_EN				BIT(31)

#define S32G_PHY_REG_DATA			0x4

#define S32G_PHY_RST_CTRL			0x8
#define  WARM_RST				BIT(1)

#define RAWLANE0_DIG_PCS_XF_RX_EQ_DELTA_IQ_OVRD_IN	0x3019
#define RAWLANE1_DIG_PCS_XF_RX_EQ_DELTA_IQ_OVRD_IN	0x3119

/*
 * Until now, there is no generic way to describe and set PCIe clock mode.
 * PCIe controller uses the default CRNS = 0 mode.
 */
enum pcie_phy_mode {
	CRNS = 0, /* Common Reference Clock, No Spread Spectrum */
	CRSS = 1, /* Common Reference Clock, Spread Spectrum */
	SRNS = 2, /* Separate Reference Clock, No Spread Spectrum */
	SRIS = 3  /* Separate Reference Clock, Spread Spectrum */
};

struct s32g_serdes_ctrl {
	void __iomem *ss_base;
	struct reset_control *rst;
	struct clk_bulk_data *clks;
	int nclks;
	u32 ss_mode;
	unsigned long ref_clk_rate;
	bool ext_clk;
};

struct s32g_pcie_ctrl {
	void __iomem *phy_base;
	struct reset_control *rst;
	struct phy *phy;
	enum pcie_phy_mode phy_mode;
	bool powered_on;
};

struct s32g_xpcs_ctrl {
	struct s32g_xpcs *phys[2];
	void __iomem *base0, *base1;
};

struct s32g_serdes {
	struct s32g_serdes_ctrl ctrl;
	struct s32g_pcie_ctrl pcie;
	struct s32g_xpcs_ctrl xpcs;
	struct device *dev;
	u8 lanes_status;
};

/* PCIe phy subsystem */

#define S32G_SERDES_PCIE_FREQ			(100 * HZ_PER_MHZ)

static void s32g_pcie_phy_reset(struct s32g_serdes *serdes)
{
	u32 val;

	val = readl(serdes->pcie.phy_base + S32G_PHY_RST_CTRL);
	writel(val | WARM_RST, serdes->pcie.phy_base + S32G_PHY_RST_CTRL);
	usleep_range(1000, 1100);
	writel(val, serdes->pcie.phy_base + S32G_PHY_RST_CTRL);
}

static int s32g_pcie_check_clk(struct s32g_serdes *serdes)
{
	struct s32g_serdes_ctrl *sctrl = &serdes->ctrl;
	unsigned long rate = sctrl->ref_clk_rate;

	if (rate != S32G_SERDES_PCIE_FREQ) {
		dev_err(serdes->dev, "PCIe PHY cannot operate at %lu HZ\n", rate);
		return -EINVAL;
	}

	return 0;
}

static bool s32g_pcie_phy_is_locked(struct s32g_serdes *serdes)
{
	u32 mplla = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_MPLLA_CTRL);
	const u32 mask = MPLL_STATE;

	return (mplla & mask) == mask;
}

/* Serdes RFM says between 3.4 and 5.2ms depending of pll */
#define S32G_SERDES_LOCK_TIMEOUT_MS		6

static int s32g_pcie_phy_power_on_common(struct s32g_serdes *serdes)
{
	struct s32g_serdes_ctrl *sctrl = &serdes->ctrl;
	struct s32g_pcie_ctrl *pcie = &serdes->pcie;
	u32 reg;
	int val, ret = 0;

	ret = s32g_pcie_check_clk(serdes);
	if (ret)
		return ret;

	reg = readl(sctrl->ss_base + S32G_PCIE_PHY_GEN_CTRL);

	/* if PCIE PHY is in SRIS mode */
	if (pcie->phy_mode == SRIS)
		reg |= RX_SRIS_MODE;

	if (sctrl->ext_clk)
		reg |= REF_USE_PAD;
	else
		reg &= ~REF_USE_PAD;

	writel(reg, sctrl->ss_base + S32G_PCIE_PHY_GEN_CTRL);

	/* Monitor Serdes MPLL state */
	ret = read_poll_timeout(s32g_pcie_phy_is_locked, val,
				(val),
				0,
				S32G_SERDES_LOCK_TIMEOUT_MS, false, serdes);
	if (ret) {
		dev_err(serdes->dev, "Failed to lock PCIE phy\n");
		return -ETIMEDOUT;
	}

	/* Set PHY register access to CR interface */
	reg = readl(sctrl->ss_base + S32G_SS_RW_REG_0);
	reg |=  PHY0_CR_PARA_SEL;
	writel(reg, sctrl->ss_base + S32G_SS_RW_REG_0);

	return ret;
}

static void s32g_pcie_phy_write(struct s32g_serdes *serdes, u32 reg, u32 val)
{
	writel(PHY_REG_EN, serdes->pcie.phy_base + S32G_PHY_REG_ADDR);
	writel(reg | PHY_REG_EN, serdes->pcie.phy_base + S32G_PHY_REG_ADDR);
	usleep_range(100, 110);
	writel(val, serdes->pcie.phy_base + S32G_PHY_REG_DATA);
	usleep_range(100, 110);
	writel(0, serdes->pcie.phy_base + S32G_PHY_REG_ADDR);
}

static int s32g_pcie_phy_power_on(struct s32g_serdes *serdes)
{
	struct s32g_pcie_ctrl *pcie = &serdes->pcie;
	struct s32g_serdes_ctrl *ctrl = &serdes->ctrl;
	u32 iq_ovrd_in;
	int ret = 0;

	ret = s32g_pcie_phy_power_on_common(serdes);
	if (ret)
		return ret;

	/* RX_EQ_DELTA_IQ_OVRD enable and override value for PCIe lanes */
	iq_ovrd_in = RAWLANE0_DIG_PCS_XF_RX_EQ_DELTA_IQ_OVRD_IN;

	s32g_pcie_phy_write(serdes, iq_ovrd_in, 0x3);
	s32g_pcie_phy_write(serdes, iq_ovrd_in, 0x13);

	if (ctrl->ss_mode == 0) {
		iq_ovrd_in = RAWLANE1_DIG_PCS_XF_RX_EQ_DELTA_IQ_OVRD_IN;

		s32g_pcie_phy_write(serdes, iq_ovrd_in, 0x3);
		s32g_pcie_phy_write(serdes, iq_ovrd_in, 0x13);
	}

	pcie->powered_on = true;

	return 0;
}

/* PCIe phy ops function */

static int s32g_serdes_phy_power_on(struct phy *p)
{
	struct s32g_serdes *serdes = phy_get_drvdata(p);

	return s32g_pcie_phy_power_on(serdes);
}

static inline bool is_pcie_phy_mode_valid(int mode)
{
	switch (mode) {
	case CRNS:
	case CRSS:
	case SRNS:
	case SRIS:
		return true;
	default:
		return false;
	}
}

static int s32g_serdes_phy_set_mode_ext(struct phy *p,
					enum phy_mode mode, int submode)
{
	struct s32g_serdes *serdes = phy_get_drvdata(p);

	if (mode == PHY_MODE_PCIE)
		return -EINVAL;

	if (!is_pcie_phy_mode_valid(submode))
		return -EINVAL;

	/*
	 * Do not configure SRIS or CRSS PHY MODE in conjunction
	 * with any SGMII mode on the same SerDes subsystem
	 */
	if ((submode == CRSS || submode == SRIS) &&
	    serdes->ctrl.ss_mode != 0)
		return -EINVAL;

	/*
	 * Internal reference clock cannot be used with either Common clock
	 * or Spread spectrum, leaving only SRNSS
	 */
	if (submode != SRNS &&  !serdes->ctrl.ext_clk)
		return -EINVAL;

	serdes->pcie.phy_mode = submode;

	return 0;
}

static const struct phy_ops serdes_pcie_ops = {
	.power_on	= s32g_serdes_phy_power_on,
	.set_mode	= s32g_serdes_phy_set_mode_ext,
};

static struct phy *s32g_serdes_phy_xlate(struct device *dev,
					 const struct of_phandle_args *args)
{
	struct s32g_serdes *serdes;
	struct phy *phy;

	serdes = dev_get_drvdata(dev);
	if (!serdes)
		return ERR_PTR(-EINVAL);

	phy = serdes->pcie.phy;

	return phy;
}

/* XPCS subsystem */

static int s32g_serdes_xpcs_init(struct s32g_serdes *serdes, int id)
{
	struct s32g_serdes_ctrl *ctrl = &serdes->ctrl;
	struct s32g_xpcs_ctrl *xpcs = &serdes->xpcs;
	enum pcie_xpcs_mode shared = NOT_SHARED;
	unsigned long rate = ctrl->ref_clk_rate;
	struct device *dev = serdes->dev;
	void __iomem *base;

	if (!id)
		base = xpcs->base0;
	else
		base = xpcs->base1;

	if (ctrl->ss_mode == 1 || ctrl->ss_mode == 2)
		shared = PCIE_XPCS_1G;
	else if (ctrl->ss_mode == 5)
		shared = PCIE_XPCS_2G5;

	return s32g_xpcs_init(xpcs->phys[id], dev, id, base,
			       ctrl->ext_clk, rate, shared);
}

static void s32g_serdes_prepare_pma_mode5(struct s32g_serdes *serdes)
{
	u32 val;
	/* Configure TX_VBOOST_LVL and TX_TERM_CTRL */
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MISC_CTRL_2);
	val &= ~(EXT_TX_VBOOST_LVL_MASK | EXT_TX_TERM_CTRL_MASK);
	val |= FIELD_PREP(EXT_TX_VBOOST_LVL_MASK, 0x3) |
		FIELD_PREP(EXT_TX_TERM_CTRL_MASK, 0x4);
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MISC_CTRL_2);

	/* Enable phy external control */
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_CTRL_SEL);
	val |= EXT_PHY_CTRL_SEL;
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_CTRL_SEL);

	/* Configure ref range, disable PLLB/ref div2 */
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_REF_CLK_CTRL);
	val &= ~(REF_CLK_DIV2_EN | REF_CLK_MPLLB_DIV2_EN | EXT_REF_RANGE_MASK);
	val |= FIELD_PREP(EXT_REF_RANGE_MASK, 0x3);
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_REF_CLK_CTRL);

	/* Configure multiplier */
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MPLLB_CTRL_2);
	val &= ~(MPLLB_MULTIPLIER_MASK | EXT_MPLLB_FRACN_CTRL_MASK | BIT(24) | BIT(28));
	val |= FIELD_PREP(MPLLB_MULTIPLIER_MASK, 0x27U) |
		FIELD_PREP(EXT_MPLLB_FRACN_CTRL_MASK, 0x414);
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MPLLB_CTRL_2);

	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_MPLLB_CTRL);
	val &= ~MPLLB_SSC_EN;
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_MPLLB_CTRL);

	/* Configure tx lane division, disable word clock div2*/
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MPLLB_CTRL_3);
	val &= ~(EXT_MPLLB_WORD_DIV2_EN | EXT_MPLLB_TX_CLK_DIV_MASK);
	val |= FIELD_PREP(EXT_MPLLB_TX_CLK_DIV_MASK, 0x5);
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MPLLB_CTRL_3);

	/* Configure bandwidth for filtering and div10*/
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MPLLB_CTRL_1);
	val &= ~(EXT_MPLLB_BANDWIDTH_MASK | EXT_MPLLB_DIV_CLK_EN |
		 EXT_MPLLB_DIV8_CLK_EN | EXT_MPLLB_DIV_MULTIPLIER_MASK);
	val |= FIELD_PREP(EXT_MPLLB_BANDWIDTH_MASK, 0x5f) | EXT_MPLLB_DIV10_CLK_EN;
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MPLLB_CTRL_1);

	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MPLLA_CTRL_1);
	val &= ~(EXT_MPLLA_BANDWIDTH_MASK);
	val |= FIELD_PREP(EXT_MPLLA_BANDWIDTH_MASK, 0xc5);
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MPLLA_CTRL_1);

	/* Configure VCO */
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_XPCS1_RX_OVRD_CTRL);
	val &= ~(XPCS1_RX_VCO_LD_VAL_MASK | XPCS1_RX_REF_LD_VAL_MASK);
	val |= FIELD_PREP(XPCS1_RX_VCO_LD_VAL_MASK, 0x540) |
		FIELD_PREP(XPCS1_RX_REF_LD_VAL_MASK, 0x2b);
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_XPCS1_RX_OVRD_CTRL);

	/* Boundary scan control */
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_BS_CTRL);
	val &= ~(EXT_BS_RX_LEVEL_MASK | EXT_BS_TX_LOWSWING);
	val |= FIELD_PREP(EXT_BS_RX_LEVEL_MASK, 0xB) | EXT_BS_RX_BIGSWING;
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_BS_CTRL);

	/* Rx loss threshold */
	val = readl(serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MISC_CTRL_1);
	val &= ~(EXT_RX_LOS_THRESHOLD_MASK | EXT_RX_VREF_CTRL_MASK);
	val |= FIELD_PREP(EXT_RX_LOS_THRESHOLD_MASK, 0x3U) |
		FIELD_PREP(EXT_RX_VREF_CTRL_MASK, 0x11U);
	writel(val, serdes->ctrl.ss_base + S32G_PCIE_PHY_EXT_MISC_CTRL_1);
}

#define XPCS_DISABLED	-1

static int s32g_serdes_init_clks(struct s32g_serdes *serdes)
{
	struct s32g_serdes_ctrl *ctrl = &serdes->ctrl;
	struct s32g_xpcs_ctrl *xpcs = &serdes->xpcs;
	int ret, order[2], xpcs_id;
	size_t i;

	switch (ctrl->ss_mode) {
	case 0:
		return 0;
	case 1:
		order[0] = 0;
		order[1] = XPCS_DISABLED;
		break;
	case 2:
	case 5:
		order[0] = 1;
		order[1] = XPCS_DISABLED;
		break;
	case 3:
		order[0] = 1;
		order[1] = 0;
		break;
	case 4:
		order[0] = 0;
		order[1] = 1;
		break;
	default:
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		xpcs_id = order[i];

		if (xpcs_id == XPCS_DISABLED)
			continue;

		ret = s32g_xpcs_init_plls(xpcs->phys[xpcs_id]);
		if (ret)
			return ret;
	}

	if (ctrl->ss_mode == 5) {
		s32g_serdes_prepare_pma_mode5(serdes);

		ret = s32g_xpcs_pre_pcie_2g5(xpcs->phys[1]);
		if (ret) {
			dev_err(serdes->dev,
				"Failed to prepare SerDes for PCIE & XPCS @ 2G5 mode\n");
			return ret;
		}

		s32g_pcie_phy_reset(serdes);
	} else {
		for (i = 0; i < ARRAY_SIZE(order); i++) {
			xpcs_id = order[i];

			if (xpcs_id == XPCS_DISABLED)
				continue;

			ret = s32g_xpcs_vreset(xpcs->phys[xpcs_id]);
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		xpcs_id = order[i];

		if (xpcs_id == XPCS_DISABLED)
			continue;

		ret = s32g_xpcs_wait_vreset(xpcs->phys[xpcs_id]);
		if (ret)
			return ret;

		s32g_xpcs_reset_rx(xpcs->phys[xpcs_id]);
		s32g_xpcs_disable_an(xpcs->phys[xpcs_id]);
	}

	return 0;
}

/* Serdes subsystem */

static int s32g_serdes_assert_reset(struct s32g_serdes *serdes)
{
	struct device *dev = serdes->dev;
	int ret;

	ret = reset_control_assert(serdes->pcie.rst);
	if (ret) {
		dev_err(dev, "Failed to assert PCIE reset: %d\n", ret);
		return ret;
	}

	ret = reset_control_assert(serdes->ctrl.rst);
	if (ret) {
		dev_err(dev, "Failed to assert SerDes reset: %d\n", ret);
		return ret;
	}

	return 0;
}

static int s32g_serdes_deassert_reset(struct s32g_serdes *serdes)
{
	struct device *dev = serdes->dev;
	int ret;

	ret = reset_control_deassert(serdes->pcie.rst);
	if (ret) {
		dev_err(dev, "Failed to assert PCIE reset: %d\n", ret);
		return ret;
	}

	ret = reset_control_deassert(serdes->ctrl.rst);
	if (ret) {
		dev_err(dev, "Failed to assert SerDes reset: %d\n", ret);
		return ret;
	}

	return ret;
}

static int s32g_serdes_init(struct s32g_serdes *serdes)
{
	struct s32g_serdes_ctrl *ctrl = &serdes->ctrl;
	u32 reg0;
	int ret;

	ret = clk_bulk_prepare_enable(ctrl->nclks, ctrl->clks);
	if (ret) {
		dev_err(serdes->dev, "Failed to enable SerDes clocks\n");
		return ret;
	}

	/*
	 * We have a tight timing for the init sequence and any delay linked to
	 * printk as an example can fail the init after reset
	 */
	ret = s32g_serdes_assert_reset(serdes);
	if (ret)
		goto disable_clks;

	/* Set serdes mode */
	reg0 = readl(ctrl->ss_base + S32G_SS_RW_REG_0);
	reg0 &= ~SUBMODE_MASK;
	if (ctrl->ss_mode == 5)
		reg0 |= 2;
	else
		reg0 |= ctrl->ss_mode;
	writel(reg0, ctrl->ss_base + S32G_SS_RW_REG_0);

	/* Set Clock source: internal or external */
	reg0 = readl(ctrl->ss_base + S32G_SS_RW_REG_0);
	if (ctrl->ext_clk)
		reg0 &= ~CLKEN_MASK;
	else
		reg0 |= CLKEN_MASK;

	writel(reg0, ctrl->ss_base + S32G_SS_RW_REG_0);

	/* Wait for the selection of working mode (as per the manual specs) */
	usleep_range(100, 110);

	ret = s32g_serdes_deassert_reset(serdes);
	if (ret)
		goto disable_clks;

	dev_info(serdes->dev, "Using mode %d for SerDes subsystem\n",
		 ctrl->ss_mode);

	ret = s32g_serdes_init_clks(serdes);
	if (ret) {
		dev_err(serdes->dev, "XPCS init failed\n");
		goto disable_clks;
	}

	return ret;

disable_clks:
	clk_bulk_disable_unprepare(serdes->ctrl.nclks,
				   serdes->ctrl.clks);

	return ret;
}

static int s32g_serdes_get_ctrl_resources(struct platform_device *pdev, struct s32g_serdes *serdes)
{
	struct s32g_serdes_ctrl *ctrl = &serdes->ctrl;
	struct device *dev = &pdev->dev;
	int ret, idx;

	ret = of_property_read_u32(dev->of_node, "nxp,sys-mode",
				   &ctrl->ss_mode);
	if (ret) {
		dev_err(dev, "Failed to get SerDes subsystem mode\n");
		return -EINVAL;
	}

	if (ctrl->ss_mode > S32G_SERDES_MODE_MAX) {
		dev_err(dev, "Invalid SerDes subsystem mode %u\n",
			ctrl->ss_mode);
		return -EINVAL;
	}

	ctrl->ss_base = devm_platform_ioremap_resource_byname(pdev, "ss_pcie");
	if (IS_ERR(ctrl->ss_base)) {
		dev_err(dev, "Failed to map 'ss_pcie'\n");
		return PTR_ERR(ctrl->ss_base);
	}

	ctrl->rst = devm_reset_control_get(dev, "serdes");
	if (IS_ERR(ctrl->rst))
		return dev_err_probe(dev, PTR_ERR(ctrl->rst),
				     "Failed to get 'serdes' reset control\n");

	ctrl->nclks = devm_clk_bulk_get_all(dev, &ctrl->clks);
	if (ctrl->nclks < 1)
		return dev_err_probe(dev, ctrl->nclks,
				     "Failed to get SerDes clocks\n");

	idx = of_property_match_string(dev->of_node, "clock-names", EXTERNAL_CLK_NAME);
	if (idx < 0)
		idx = of_property_match_string(dev->of_node, "clock-names", INTERNAL_CLK_NAME);
	else
		ctrl->ext_clk = true;

	if (idx < 0) {
		dev_err(dev, "Failed to get Phy reference clock source\n");
		return -EINVAL;
	}

	ctrl->ref_clk_rate = clk_get_rate(ctrl->clks[idx].clk);
	if (!ctrl->ref_clk_rate) {
		dev_err(dev, "Failed to get Phy reference clock rate\n");
		return -EINVAL;
	}

	return ret;
}

static int s32g_serdes_get_pcie_resources(struct platform_device *pdev, struct s32g_serdes *serdes)
{
	struct s32g_pcie_ctrl *pcie = &serdes->pcie;
	struct device *dev = &pdev->dev;

	pcie->phy_base = devm_platform_ioremap_resource_byname(pdev,
							       "pcie_phy");
	if (IS_ERR(pcie->phy_base)) {
		dev_err(dev, "Failed to map 'pcie_phy'\n");
		return PTR_ERR(pcie->phy_base);
	}

	pcie->rst = devm_reset_control_get(dev, "pcie");
	if (IS_ERR(pcie->rst))
		return dev_err_probe(dev, IS_ERR(pcie->rst),
				     "Failed to get 'pcie' reset control\n");

	return 0;
}

static int s32g_serdes_get_xpcs_resources(struct platform_device *pdev, struct s32g_serdes *serdes)
{
	struct s32g_xpcs_ctrl *xpcs = &serdes->xpcs;
	struct device *dev = &pdev->dev;

	xpcs->base0 = devm_platform_ioremap_resource_byname(pdev, "xpcs0");
	if (IS_ERR(xpcs->base0)) {
		dev_err(dev, "Failed to map 'xpcs0'\n");
		return PTR_ERR(xpcs->base0);
	}

	xpcs->base1 = devm_platform_ioremap_resource_byname(pdev, "xpcs1");
	if (IS_ERR(xpcs->base1)) {
		dev_err(dev, "Failed to map 'xpcs1'\n");
		return PTR_ERR(xpcs->base1);
	}

	return 0;
}

static int s32g2_serdes_create_phy(struct s32g_serdes *serdes, struct device_node *child_node)
{
	struct s32g_serdes_ctrl *ctrl = &serdes->ctrl;
	struct phy_provider *phy_provider;
	struct device *dev = serdes->dev;
	int ret, ss_mode = ctrl->ss_mode;
	struct phy *phy;

	if (of_device_is_compatible(child_node, "nxp,s32g2-serdes-pcie-phy")) {
		/* no PCIe phy lane */
		if (ss_mode > 2)
			return 0;

		phy = devm_phy_create(dev, child_node, &serdes_pcie_ops);
		if (IS_ERR(phy))
			return PTR_ERR(phy);

		phy_set_drvdata(phy, serdes);

		phy->attrs.mode = PHY_MODE_PCIE;
		phy->id = 0;
		serdes->pcie.phy = phy;

		phy_provider = devm_of_phy_provider_register(&phy->dev, s32g_serdes_phy_xlate);
		if (IS_ERR(phy_provider))
			return PTR_ERR(phy_provider);

	} else if (of_device_is_compatible(child_node, "nxp,s32g2-serdes-xpcs")) {
		struct s32g_xpcs_ctrl *xpcs_ctrl = &serdes->xpcs;
		struct s32g_xpcs *xpcs;
		int port;

		/* no Ethernet phy lane */
		if (ss_mode == 0)
			return 0;

		/* Get XPCS port number connected to the lane */
		if (of_property_read_u32(child_node, "reg", &port))
			return -EINVAL;

		/* XPCS1 is not used */
		if (ss_mode == 1 && port == 1)
			return -EINVAL;

		/* XPCS0 is not used */
		if (ss_mode == 2 && port == 0)
			return -EINVAL;

		xpcs = devm_kmalloc(dev, sizeof(*xpcs), GFP_KERNEL);
		if (IS_ERR(xpcs)) {
			dev_err(dev, "Failed to allocate xpcs\n");
			return -ENOMEM;
		}

		xpcs_ctrl->phys[port] = xpcs;

		xpcs->an = of_property_read_bool(dev->of_node, "nxp,xpcs_an");

		ret = s32g_serdes_xpcs_init(serdes, port);
		if (ret)
			return ret;

	} else {
		dev_warn(dev, "Skipping unknown child node %pOFn\n", child_node);
	}

	return 0;
}

static int s32g_serdes_parse_lanes(struct device *dev, struct s32g_serdes *serdes)
{
	int ret;

	for_each_available_child_of_node_scoped(dev->of_node, of_port) {
		ret = s32g2_serdes_create_phy(serdes, of_port);
		if (ret)
			break;
	}

	return ret;
}

static int s32g_serdes_probe(struct platform_device *pdev)
{
	struct s32g_serdes *serdes;
	struct device *dev = &pdev->dev;
	int ret;

	serdes = devm_kzalloc(dev, sizeof(*serdes), GFP_KERNEL);
	if (!serdes)
		return -ENOMEM;

	platform_set_drvdata(pdev, serdes);
	serdes->dev = dev;

	ret = s32g_serdes_get_ctrl_resources(pdev, serdes);
	if (ret)
		return ret;

	ret = s32g_serdes_get_pcie_resources(pdev, serdes);
	if (ret)
		return ret;

	ret = s32g_serdes_get_xpcs_resources(pdev, serdes);
	if (ret)
		return ret;

	ret = s32g_serdes_parse_lanes(dev, serdes);
	if (ret)
		return ret;

	ret = s32g_serdes_init(serdes);

	return ret;
}

static int __maybe_unused s32g_serdes_suspend(struct device *device)
{
	struct s32g_serdes *serdes = dev_get_drvdata(device);

	clk_bulk_disable_unprepare(serdes->ctrl.nclks, serdes->ctrl.clks);

	return 0;
}

static int __maybe_unused s32g_serdes_resume(struct device *device)
{
	struct s32g_serdes *serdes = dev_get_drvdata(device);
	struct s32g_pcie_ctrl *pcie = &serdes->pcie;
	int ret;

	ret = s32g_serdes_init(serdes);
	if (ret) {
		dev_err(device, "Failed to initialize\n");
		return ret;
	}

	/* Restore PCIe phy power */
	if (pcie->powered_on) {
		ret = s32g_pcie_phy_power_on(serdes);
		if (ret)
			dev_err(device, "Failed to power-on PCIe phy\n");
	}

	return ret;
}

struct phylink_pcs *s32g_serdes_pcs_create(struct device *dev, struct device_node *np)
{
	struct platform_device *pdev;
	struct device_node *pcs_np;
	struct s32g_serdes *serdes;
	u32 port;

	if (of_property_read_u32(np, "reg", &port))
		return ERR_PTR(-EINVAL);

	if (port > S32G_SERDES_XPCS_MAX)
		return ERR_PTR(-EINVAL);

	/* The PCS pdev is attached to the parent node */
	pcs_np = of_get_parent(np);
	if (!pcs_np)
		return ERR_PTR(-ENODEV);

	if (!of_device_is_available(pcs_np)) {
		of_node_put(pcs_np);
		return ERR_PTR(-ENODEV);
	}

	pdev = of_find_device_by_node(pcs_np);
	of_node_put(pcs_np);
	if (!pdev || !platform_get_drvdata(pdev)) {
		if (pdev)
			put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	serdes = platform_get_drvdata(pdev);

	return &serdes->xpcs.phys[port]->pcs;
}
EXPORT_SYMBOL(s32g_serdes_pcs_create);

static const struct of_device_id s32g_serdes_match[] = {
	{
		.compatible = "nxp,s32g2-serdes",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s32g_serdes_match);

static const struct dev_pm_ops s32g_serdes_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(s32g_serdes_suspend,
				  s32g_serdes_resume)
};

static struct platform_driver s32g_serdes_driver = {
	.probe		= s32g_serdes_probe,
	.driver		= {
		.name	= "phy-s32g-serdes",
		.of_match_table = s32g_serdes_match,
		.pm = &s32g_serdes_pm_ops,
	},
};
module_platform_driver(s32g_serdes_driver);

MODULE_AUTHOR("Ghennadi Procopciuc <ghennadi.procopciuc@nxp.com>");
MODULE_DESCRIPTION("S32CC SerDes driver");
MODULE_LICENSE("GPL");
