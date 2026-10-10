// SPDX-License-Identifier: GPL-2.0
#include "phy-mtk-hdmi.h"
#include "phy-mtk-io.h"

#define HDMI_CON0	0x00
#define RG_HDMITX_DRV_IBIAS		GENMASK(5, 0)
#define RG_HDMITX_EN_SER		GENMASK(15, 12)
#define RG_HDMITX_EN_SLDO		GENMASK(19, 16)
#define RG_HDMITX_EN_PRED		GENMASK(23, 20)
#define RG_HDMITX_EN_IMP		GENMASK(27, 24)
#define RG_HDMITX_EN_DRV		GENMASK(31, 28)

#define HDMI_CON1	0x04
#define RG_HDMITX_PRED_IBIAS		GENMASK(21, 18)
#define RG_HDMITX_PRED_IMP		BIT(22)
#define RG_HDMITX_DRV_IMP		GENMASK(31, 26)

#define HDMI_CON2	0x08
#define RG_HDMITX_EN_TX_CKLDO		BIT(0)
#define RG_HDMITX_EN_TX_POSDIV		BIT(1)
#define RG_HDMITX_TX_POSDIV		GENMASK(4, 3)
#define RG_HDMITX_EN_MBIAS		BIT(6)
#define RG_HDMITX_MBIAS_LPF_EN		BIT(7)

#define HDMI_CON4	0x10
#define RG_HDMITX_D0_IMP		GENMASK(7, 0)
#define RG_HDMITX_D1_IMP		GENMASK(15, 8)
#define RG_HDMITX_D2_IMP		GENMASK(23, 16)

#define HDMI_CON6	0x18
#define RG_HTPLL_BR			GENMASK(1, 0)
#define RG_HTPLL_BC			GENMASK(3, 2)
#define RG_HTPLL_BP			GENMASK(7, 4)
#define RG_HTPLL_IR			GENMASK(11, 8)
#define RG_HTPLL_IC			GENMASK(15, 12)
#define RG_HTPLL_POSDIV			GENMASK(17, 16)
#define RG_HTPLL_PREDIV			GENMASK(19, 18)
#define RG_HTPLL_FBKSEL			GENMASK(21, 20)
#define RG_HTPLL_RLH_EN			BIT(22)
#define RG_HTPLL_FBKDIV			GENMASK(30, 24)
#define RG_HTPLL_EN			BIT(31)

#define HDMI_CON7	0x1c
#define RG_HTPLL_AUTOK_EN		BIT(23)
#define RG_HTPLL_DIVEN			GENMASK(30, 28)

static int mtk_hdmi_pll_prepare(struct clk_hw *hw)
{
	struct mtk_hdmi_phy *hdmi_phy = to_mtk_hdmi_phy(hw);
	void __iomem *base = hdmi_phy->regs;

	mtk_phy_set_bits(base + HDMI_CON7, RG_HTPLL_AUTOK_EN);
	mtk_phy_clear_bits(base + HDMI_CON6, RG_HTPLL_RLH_EN);
	mtk_phy_set_bits(base + HDMI_CON6, RG_HTPLL_POSDIV);
	mtk_phy_set_bits(base + HDMI_CON2, RG_HDMITX_EN_MBIAS);
	usleep_range(80, 100);
	mtk_phy_set_bits(base + HDMI_CON6, RG_HTPLL_EN);
	mtk_phy_set_bits(base + HDMI_CON2, RG_HDMITX_EN_TX_CKLDO);
	mtk_phy_set_bits(base + HDMI_CON0, RG_HDMITX_EN_SLDO);
	usleep_range(80, 100);
	mtk_phy_set_bits(base + HDMI_CON2, RG_HDMITX_MBIAS_LPF_EN);
	mtk_phy_set_bits(base + HDMI_CON2, RG_HDMITX_EN_TX_POSDIV);

	return 0;
}

static void mtk_hdmi_pll_unprepare(struct clk_hw *hw)
{
	struct mtk_hdmi_phy *hdmi_phy = to_mtk_hdmi_phy(hw);
	void __iomem *base = hdmi_phy->regs;

	mtk_phy_clear_bits(base + HDMI_CON2, RG_HDMITX_EN_TX_POSDIV);
	mtk_phy_clear_bits(base + HDMI_CON2, RG_HDMITX_MBIAS_LPF_EN);
	usleep_range(80, 100);
	mtk_phy_clear_bits(base + HDMI_CON0, RG_HDMITX_EN_SLDO);
	mtk_phy_clear_bits(base + HDMI_CON2, RG_HDMITX_EN_TX_CKLDO);
	mtk_phy_clear_bits(base + HDMI_CON6, RG_HTPLL_EN);
	usleep_range(80, 100);
	mtk_phy_clear_bits(base + HDMI_CON2, RG_HDMITX_EN_MBIAS);
	mtk_phy_clear_bits(base + HDMI_CON6, RG_HTPLL_POSDIV);
	mtk_phy_clear_bits(base + HDMI_CON6, RG_HTPLL_RLH_EN);
	mtk_phy_clear_bits(base + HDMI_CON7, RG_HTPLL_AUTOK_EN);
	usleep_range(80, 100);
}

static int mtk_hdmi_pll_determine_rate(struct clk_hw *hw,
				       struct clk_rate_request *req)
{
	struct mtk_hdmi_phy *hdmi_phy = to_mtk_hdmi_phy(hw);

	hdmi_phy->pll_rate = req->rate;

	return 0;
}

static int mtk_hdmi_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	struct mtk_hdmi_phy *hdmi_phy = to_mtk_hdmi_phy(hw);
	void __iomem *base = hdmi_phy->regs;
	u32 pos_div;
	u32 pre_imp_en = 0x0;
	u32 pre_ibias = 0xd;
	u32 imp_en = 0x0;
	u32 imp_clk = 0x1c;
	u32 imp_d0 = 0x1c;
	u32 imp_d1 = 0x1c;
	u32 imp_d2 = 0x1c;
	u32 drv_ibias = 0xa;

	if (rate <= 27000000)
		pos_div = 3;
	else if (rate <= 74250000)
		pos_div = 2;
	else
		pos_div = 1;

	mtk_phy_set_bits(base + HDMI_CON6, RG_HTPLL_PREDIV);
	mtk_phy_set_bits(base + HDMI_CON6, RG_HTPLL_POSDIV);
	mtk_phy_update_bits(base + HDMI_CON6,
			    RG_HTPLL_IC | RG_HTPLL_IR,
			    FIELD_PREP(RG_HTPLL_IC, 0x1) |
			    FIELD_PREP(RG_HTPLL_IR, 0x1));
	mtk_phy_update_field(base + HDMI_CON2, RG_HDMITX_TX_POSDIV, pos_div);
	mtk_phy_update_field(base + HDMI_CON6, RG_HTPLL_FBKSEL, 1);
	mtk_phy_update_field(base + HDMI_CON6, RG_HTPLL_FBKDIV, 19);
	mtk_phy_update_field(base + HDMI_CON7, RG_HTPLL_DIVEN, 0x2);
	mtk_phy_update_bits(base + HDMI_CON6,
			    RG_HTPLL_BP | RG_HTPLL_BC | RG_HTPLL_BR,
			    FIELD_PREP(RG_HTPLL_BP, 0xc) |
			    FIELD_PREP(RG_HTPLL_BC, 0x2) |
			    FIELD_PREP(RG_HTPLL_BR, 0x1));

	mtk_phy_update_field(base + HDMI_CON1, RG_HDMITX_PRED_IMP, pre_imp_en);
	mtk_phy_update_field(base + HDMI_CON1, RG_HDMITX_PRED_IBIAS, pre_ibias);
	mtk_phy_update_field(base + HDMI_CON0, RG_HDMITX_EN_IMP, imp_en);
	mtk_phy_update_field(base + HDMI_CON1, RG_HDMITX_DRV_IMP, imp_clk);
	mtk_phy_update_bits(base + HDMI_CON4,
			    RG_HDMITX_D0_IMP | RG_HDMITX_D1_IMP | RG_HDMITX_D2_IMP,
			    FIELD_PREP(RG_HDMITX_D0_IMP, imp_d0) |
			    FIELD_PREP(RG_HDMITX_D1_IMP, imp_d1) |
			    FIELD_PREP(RG_HDMITX_D2_IMP, imp_d2));
	mtk_phy_update_field(base + HDMI_CON0, RG_HDMITX_DRV_IBIAS, drv_ibias);

	return 0;
}

static unsigned long mtk_hdmi_pll_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct mtk_hdmi_phy *hdmi_phy = to_mtk_hdmi_phy(hw);

	unsigned long out_rate, val;
	u32 tmp;

	tmp = readl(hdmi_phy->regs + HDMI_CON6);
	val = FIELD_GET(RG_HTPLL_PREDIV, tmp);
	switch (val) {
	case 0x00:
		out_rate = parent_rate;
		break;
	case 0x01:
		out_rate = parent_rate / 2;
		break;
	default:
		out_rate = parent_rate / 4;
		break;
	}

	val = FIELD_GET(RG_HTPLL_FBKDIV, tmp);
	out_rate *= (val + 1) * 2;

	tmp = readl(hdmi_phy->regs + HDMI_CON2);
	val = FIELD_GET(RG_HDMITX_TX_POSDIV, tmp);
	out_rate >>= val;

	if (tmp & RG_HDMITX_EN_TX_POSDIV)
		out_rate /= 5;

	return out_rate;
}

static const struct clk_ops mtk_hdmi_phy_pll_ops = {
	.prepare = mtk_hdmi_pll_prepare,
	.unprepare = mtk_hdmi_pll_unprepare,
	.set_rate = mtk_hdmi_pll_set_rate,
	.determine_rate = mtk_hdmi_pll_determine_rate,
	.recalc_rate = mtk_hdmi_pll_recalc_rate,
};

static void mtk_hdmi_phy_enable_tmds(struct mtk_hdmi_phy *hdmi_phy)
{
	void __iomem *base = hdmi_phy->regs;

	mtk_phy_set_bits(base + HDMI_CON0,
			 RG_HDMITX_EN_DRV | RG_HDMITX_EN_PRED | RG_HDMITX_EN_SER);
	usleep_range(80, 100);
}

static void mtk_hdmi_phy_disable_tmds(struct mtk_hdmi_phy *hdmi_phy)
{
	void __iomem *base = hdmi_phy->regs;

	mtk_phy_clear_bits(base + HDMI_CON0,
			   RG_HDMITX_EN_DRV | RG_HDMITX_EN_PRED | RG_HDMITX_EN_SER);
	usleep_range(80, 100);
}

struct mtk_hdmi_phy_conf mtk_hdmi_phy_8167_conf = {
	.flags = CLK_SET_RATE_PARENT | CLK_SET_RATE_GATE,
	.hdmi_phy_clk_ops = &mtk_hdmi_phy_pll_ops,
	.hdmi_phy_enable_tmds = mtk_hdmi_phy_enable_tmds,
	.hdmi_phy_disable_tmds = mtk_hdmi_phy_disable_tmds,
};

MODULE_DESCRIPTION("MediaTek HDMI PHY Driver");
MODULE_LICENSE("GPL");
