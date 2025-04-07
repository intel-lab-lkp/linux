// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 * Author: jitao.shi <jitao.shi@mediatek.com>
 */

#include "phy-mtk-io.h"
#include "phy-mtk-mipi-dsi.h"

#define MIPITX_LANE_CON		0x0004
#define RG_DSI_CPHY_T1DRV_EN		BIT(0)
#define RG_DSI_ANA_CK_SEL		BIT(1)
#define RG_DSI_PHY_CK_SEL		BIT(2)
#define RG_DSI_CPHY_EN			BIT(3)
#define RG_DSI_PHYCK_INV_EN		BIT(4)
#define RG_DSI_PWR04_EN			BIT(5)
#define RG_DSI_BG_LPF_EN		BIT(6)
#define RG_DSI_BG_CORE_EN		BIT(7)
#define RG_DSI_PAD_TIEL_SEL		BIT(8)

#define MIPITX_VOLTAGE_SEL	0x0008
#define RG_DSI_HSTX_LDO_REF_SEL		GENMASK(9, 6)
#define RG_DSI_PRD_REF_SEL		GENMASK(5, 0)
#define RG_DSI_PRD_REF_MINI		0
#define RG_DSI_PRD_REF_DEF		4
#define RG_DSI_PRD_REF_MAX		7

#define MIPITX_PRESERVED	0x000c

#define MIPITX_PLL_PWR		0x0028
#define MIPITX_PLL_CON0		0x002c
#define MIPITX_PLL_CON1		0x0030
#define MIPITX_PLL_CON2		0x0034
#define MIPITX_PLL_CON3		0x0038
#define MIPITX_PLL_CON4		0x003c
#define RG_DSI_PLL_IBIAS		GENMASK(11, 10)

#define MIPITX_PHY_SEL0		0x0040

#define MIPITX_D2P_RTCODE	0x0100

#define MIPITX_D2_SW_CTL_EN	0x015c
#define MIPITX_D0_SW_CTL_EN	0x025c
#define DSI_CK_CKMODE_EN		BIT(0)
#define MIPITX_CK_SW_CTL_EN	0x035c
#define MIPITX_D1_SW_CTL_EN	0x045c
#define MIPITX_D3_SW_CTL_EN	0x055c
#define DSI_SW_CTL_EN			BIT(0)
#define AD_DSI_PLL_SDM_PWR_ON		BIT(0)
#define AD_DSI_PLL_SDM_ISO_EN		BIT(1)

#define MIPITX_D2_CKMODE_EN	0x0120
#define DSI_D2_CKMODE_EN		BIT(0)
#define MIPITX_D0_CKMODE_EN	0x0220
#define DSI_D0_CKMODE_EN		BIT(0)
#define MIPITX_CK_CKMODE_EN	0x0320
#define MIPITX_D1_CKMODE_EN	0x0420
#define DSI_D1_CKMODE_EN		BIT(0)
#define MIPITX_D3_CKMODE_EN	0x0520
#define DSI_D3_CKMODE_EN		BIT(0)

#define RG_DSI_PLL_EN			BIT(0)
#define RG_DSI_PLL_POSDIV		GENMASK(10, 8)
#define RG_DSI_PLL_DIV3_EN		BIT(28)
#define RG_DSI_PLL_FBSEL		BIT(13)

#define DSI_PHY_LANE_SWAP	0x0618
#define LANE_D0_SEL			0
#define LANE_D1_SEL			4
#define LANE_D2_SEL			8
#define LANE_D3_SEL			12
#define LANE_C0_SEL			16
#define LANE_C1_SEL			20

static int mtk_mipi_tx_pll_enable(struct clk_hw *hw)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = mipi_tx->regs;
	unsigned int txdiv, txdiv0;
	u32 pcw = 0;

	/* Select different voltage when different data rate */
	if (mipi_tx->data_rate < 2500000000) {
		mtk_phy_update_field(base + MIPITX_VOLTAGE_SEL,
				     RG_DSI_PRD_REF_SEL, RG_DSI_PRD_REF_MINI);
		writel(0xffff00f0, base +  MIPITX_PRESERVED);
	} else {
		mtk_phy_update_field(base + MIPITX_VOLTAGE_SEL,
				     RG_DSI_PRD_REF_SEL, RG_DSI_PRD_REF_DEF);
		writel(0xffff0030, base +  MIPITX_PRESERVED);
	}

	dev_dbg(mipi_tx->dev, "enable: %u bps\n", mipi_tx->data_rate);

	if (mipi_tx->data_rate >= 2000000000) {
		txdiv = 1;
		txdiv0 = 0;
	} else if (mipi_tx->data_rate >= 1000000000) {
		txdiv = 2;
		txdiv0 = 1;
	} else if (mipi_tx->data_rate >= 500000000) {
		txdiv = 4;
		txdiv0 = 2;
	} else if (mipi_tx->data_rate > 250000000) {
		txdiv = 8;
		txdiv0 = 3;
	} else if (mipi_tx->data_rate >= 125000000) {
		txdiv = 16;
		txdiv0 = 4;
	} else {
		return -EINVAL;
	}

	mtk_phy_set_bits(base + MIPITX_PLL_PWR, AD_DSI_PLL_SDM_PWR_ON);
	mtk_phy_clear_bits(base + MIPITX_PLL_CON1, RG_DSI_PLL_EN);
	udelay(40);
	mtk_phy_clear_bits(base + MIPITX_PLL_PWR, AD_DSI_PLL_SDM_ISO_EN);
	pcw = div_u64(((u64)(mipi_tx->data_rate / 2) * txdiv) << 24, 26000000);
	writel(pcw, base + MIPITX_PLL_CON0);
	mtk_phy_update_field(base + MIPITX_PLL_CON1, RG_DSI_PLL_POSDIV, txdiv0);
	udelay(40);
	mtk_phy_set_bits(base + MIPITX_PLL_CON1, RG_DSI_PLL_EN);
	udelay(40);

	return 0;
}

static void mtk_mipi_tx_pll_disable(struct clk_hw *hw)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = mipi_tx->regs;

	mtk_phy_clear_bits(base + MIPITX_PLL_CON1, RG_DSI_PLL_EN);

	mtk_phy_set_bits(base + MIPITX_PLL_PWR, AD_DSI_PLL_SDM_ISO_EN);
	mtk_phy_clear_bits(base + MIPITX_PLL_PWR, AD_DSI_PLL_SDM_PWR_ON);
}

static long mtk_mipi_tx_pll_round_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long *prate)
{
	return clamp_val(rate, 125000000, 1600000000);
}

static const struct clk_ops mtk_mipi_tx_pll_ops = {
	.enable = mtk_mipi_tx_pll_enable,
	.disable = mtk_mipi_tx_pll_disable,
	.round_rate = mtk_mipi_tx_pll_round_rate,
	.set_rate = mtk_mipi_tx_pll_set_rate,
	.recalc_rate = mtk_mipi_tx_pll_recalc_rate,
};

static void mtk_mipi_tx_power_on_signal(struct phy *phy)
{
	struct mtk_mipi_tx *mipi_tx = phy_get_drvdata(phy);
	void __iomem *base = mipi_tx->regs;

	/* BG_LPF_EN / BG_CORE_EN */
	writel(RG_DSI_PAD_TIEL_SEL | RG_DSI_BG_CORE_EN, base + MIPITX_LANE_CON);
	/* delay for mipi core enable */
	usleep_range(30, 100);
	writel(RG_DSI_BG_CORE_EN | RG_DSI_BG_LPF_EN, base + MIPITX_LANE_CON);

	/* Switch OFF each Lane */
	mtk_phy_clear_bits(base + MIPITX_D0_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_phy_clear_bits(base + MIPITX_D1_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_phy_clear_bits(base + MIPITX_D2_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_phy_clear_bits(base + MIPITX_D3_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_phy_clear_bits(base + MIPITX_CK_SW_CTL_EN, DSI_SW_CTL_EN);

	/* The mipitx_drive is start from 3000 microamp and the range is
	 * 3000 ~ 6000 microamp. RG_DSI_HSTX_LDO_REF_SEL is the offset from
	 * 3000 microamp, so -3000.
	 */
	mtk_phy_update_field(base + MIPITX_VOLTAGE_SEL, RG_DSI_HSTX_LDO_REF_SEL,
			     (mipi_tx->mipitx_drive - 3000) / 200);

	mtk_phy_set_bits(base + MIPITX_CK_CKMODE_EN, DSI_CK_CKMODE_EN);
}

static void mtk_mipi_tx_power_off_signal(struct phy *phy)
{
	struct mtk_mipi_tx *mipi_tx = phy_get_drvdata(phy);
	void __iomem *base = mipi_tx->regs;

	/* Switch ON each Lane */
	mtk_phy_set_bits(base + MIPITX_D0_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_phy_set_bits(base + MIPITX_D1_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_phy_set_bits(base + MIPITX_D2_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_phy_set_bits(base + MIPITX_D3_SW_CTL_EN, DSI_SW_CTL_EN);
	mtk_phy_set_bits(base + MIPITX_CK_SW_CTL_EN, DSI_SW_CTL_EN);

	writel(RG_DSI_PAD_TIEL_SEL | RG_DSI_BG_CORE_EN, base + MIPITX_LANE_CON);
	writel(RG_DSI_PAD_TIEL_SEL, base + MIPITX_LANE_CON);
}

const struct mtk_mipitx_data mt8196_mipitx_data = {
	.mipi_tx_clk_ops = &mtk_mipi_tx_pll_ops,
	.mipi_tx_enable_signal = mtk_mipi_tx_power_on_signal,
	.mipi_tx_disable_signal = mtk_mipi_tx_power_off_signal,
};
