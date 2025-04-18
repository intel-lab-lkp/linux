// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019-2022 MediaTek Inc.
 * Copyright (c) 2022 BayLibre
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define PHYD_OFFSET			0x0000
#define PHYD_DIG_LAN0_OFFSET		0x1000
#define PHYD_DIG_LAN1_OFFSET		0x1100
#define PHYD_DIG_LAN2_OFFSET		0x1200
#define PHYD_DIG_LAN3_OFFSET		0x1300
#define PHYD_DIG_GLB_OFFSET		0x1400

#define DP_PHY_DIG_PLL_CTL_0		(PHYD_DIG_GLB_OFFSET + 0x10)
#define FORCE_PWORE_STATE_FLDMASK		GENMASK(2, 0)
#define FORCE_PWORE_STATE_VALUE			0x7

#define IPMUX_CONTROL			(PHYD_DIG_GLB_OFFSET + 0x98)
#define EDPTX_DSI_PHYD_SEL_FLDMASK		0x1
#define EDPTX_DSI_PHYD_SEL_FLDMASK_POS		0

#define DP_PHY_DIG_TX_CTL_0		(PHYD_DIG_GLB_OFFSET + 0x74)
#define TX_LN_EN_FLDMASK			0xf

#define mtk_edp_PHY_DIG_PLL_CTL_1	(PHYD_DIG_GLB_OFFSET + 0x14)
#define TPLL_SSC_EN				BIT(8)

#define mtk_edp_PHY_DIG_BIT_RATE		(PHYD_DIG_GLB_OFFSET + 0x3C)
#define BIT_RATE_RBR				0x1
#define BIT_RATE_HBR				0x4
#define BIT_RATE_HBR2				0x7
#define BIT_RATE_HBR3				0x9

#define mtk_edp_PHY_DIG_SW_RST		(PHYD_DIG_GLB_OFFSET + 0x38)
#define DP_GLB_SW_RST_PHYD			BIT(0)
#define DP_GLB_SW_RST_PHYD_MASK			BIT(0)

#define DRIVING_FORCE			0x30
#define EDP_TX_LN_VOLT_SWING_VAL_FLDMASK	0x6
#define EDP_TX_LN_VOLT_SWING_VAL_FLDMASK_POS	1
#define EDP_TX_LN_PRE_EMPH_VAL_FLDMASK		0x18
#define EDP_TX_LN_PRE_EMPH_VAL_FLDMASK_POS	3

struct mtk_edp_phy {
	struct regmap *regs;
};

enum DPTX_LANE_NUM {
	DPTX_LANE0 = 0x0,
	DPTX_LANE1 = 0x1,
	DPTX_LANE2 = 0x2,
	DPTX_LANE3 = 0x3,
	DPTX_LANE_MAX,
};

enum DPTX_LANE_COUNT {
	DPTX_LANE_COUNT1 = 0x1,
	DPTX_LANE_COUNT2 = 0x2,
	DPTX_LANE_COUNT4 = 0x4,
};

static void mtk_edptx_phyd_reset_swing_pre(struct mtk_edp_phy *edp_phy)
{
	regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN0_OFFSET + DRIVING_FORCE,
			   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
			   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK, 0x0);
	regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN1_OFFSET + DRIVING_FORCE,
			   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
			   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK, 0x0);
	regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN2_OFFSET + DRIVING_FORCE,
			   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
			   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK, 0x0);
	regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN3_OFFSET + DRIVING_FORCE,
			   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
			   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK, 0x0);
}

static int mtk_edp_phy_init(struct phy *phy)
{
	struct mtk_edp_phy *edp_phy = phy_get_drvdata(phy);

	regmap_update_bits(edp_phy->regs, IPMUX_CONTROL, 0,
			   EDPTX_DSI_PHYD_SEL_FLDMASK);

	regmap_update_bits(edp_phy->regs, DP_PHY_DIG_PLL_CTL_0,
			   FORCE_PWORE_STATE_VALUE,
			   FORCE_PWORE_STATE_FLDMASK);

	return 0;
}

static int mtk_edp_phy_configure(struct phy *phy, union phy_configure_opts *opts)
{
	struct mtk_edp_phy *edp_phy = phy_get_drvdata(phy);
	u32 val;

	if (opts->dp.set_rate) {
		switch (opts->dp.link_rate) {
		case 1620:
			val = BIT_RATE_RBR;
			break;
		case 2700:
			val = BIT_RATE_HBR;
			break;
		case 5400:
			val = BIT_RATE_HBR2;
			break;
		case 8100:
			val = BIT_RATE_HBR3;
			break;
		default:
			dev_err(&phy->dev,
				"Implementation error, unknown linkrate %x\n",
				opts->dp.link_rate);
			return -EINVAL;
		}
		regmap_write(edp_phy->regs, mtk_edp_PHY_DIG_BIT_RATE, val);
	}

	if (opts->dp.set_lanes) {
		for (val = 0; val < 4; val++) {
			regmap_update_bits(edp_phy->regs, DP_PHY_DIG_TX_CTL_0,
					   ((1 << (val + 1)) - 1),
					   TX_LN_EN_FLDMASK);
		}
	}

	if (opts->dp.set_voltages) {
		switch (opts->dp.lanes) {
		case DPTX_LANE_COUNT1:
			regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN0_OFFSET +
					   DRIVING_FORCE,
					   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
					   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK,
					   opts->dp.voltage[DPTX_LANE0] << 1 |
					   opts->dp.pre[DPTX_LANE0] << 3);
		break;
		case DPTX_LANE_COUNT2:
			regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN0_OFFSET +
					   DRIVING_FORCE,
					   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
					   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK,
					   opts->dp.voltage[DPTX_LANE0] << 1 |
					   opts->dp.pre[DPTX_LANE0] << 3);
			regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN1_OFFSET +
					   DRIVING_FORCE,
					   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
					   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK,
					   opts->dp.voltage[DPTX_LANE1] << 1 |
					   opts->dp.pre[DPTX_LANE1] << 3);
		break;
		case DPTX_LANE_COUNT4:
			regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN0_OFFSET +
					   DRIVING_FORCE,
					   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
					   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK,
					   opts->dp.voltage[DPTX_LANE0] << 1 |
					   opts->dp.pre[DPTX_LANE0] << 3);
			regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN1_OFFSET +
					   DRIVING_FORCE,
					   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
					   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK,
					   opts->dp.voltage[DPTX_LANE1] << 1 |
					   opts->dp.pre[DPTX_LANE1] << 3);
			regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN2_OFFSET +
					   DRIVING_FORCE,
					   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
					   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK,
					   opts->dp.voltage[DPTX_LANE2] << 1 |
					   opts->dp.pre[DPTX_LANE2] << 3);
			regmap_update_bits(edp_phy->regs, PHYD_DIG_LAN3_OFFSET +
					   DRIVING_FORCE,
					   EDP_TX_LN_VOLT_SWING_VAL_FLDMASK |
					   EDP_TX_LN_PRE_EMPH_VAL_FLDMASK,
					   opts->dp.voltage[DPTX_LANE3] << 1 |
					   opts->dp.pre[DPTX_LANE3] << 3);
		break;
		default:
			dev_err(&phy->dev, "Wrong lanes config: %x\n",
				opts->dp.lanes);
			return -EINVAL;
		}
	}

	regmap_update_bits(edp_phy->regs, mtk_edp_PHY_DIG_PLL_CTL_1,
			   TPLL_SSC_EN, opts->dp.ssc ? 0 : TPLL_SSC_EN);

	return 0;
}

static int mtk_edp_phy_reset(struct phy *phy)
{
	struct mtk_edp_phy *edp_phy = phy_get_drvdata(phy);

	regmap_update_bits(edp_phy->regs, mtk_edp_PHY_DIG_SW_RST,
			   0, DP_GLB_SW_RST_PHYD_MASK);
	usleep_range(50, 200);
	regmap_update_bits(edp_phy->regs, mtk_edp_PHY_DIG_SW_RST,
			   DP_GLB_SW_RST_PHYD, DP_GLB_SW_RST_PHYD_MASK);
	regmap_update_bits(edp_phy->regs, DP_PHY_DIG_TX_CTL_0,
			   0x0, TX_LN_EN_FLDMASK);
	mtk_edptx_phyd_reset_swing_pre(edp_phy);

	return 0;
}

static const struct phy_ops mtk_edp_phy_dev_ops = {
	.init = mtk_edp_phy_init,
	.configure = mtk_edp_phy_configure,
	.reset = mtk_edp_phy_reset,
	.owner = THIS_MODULE,
};

static int mtk_edp_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_edp_phy *edp_phy;
	struct phy *phy;
	struct regmap *regs;

	regs = *(struct regmap **)dev->platform_data;
	if (!regs)
		return dev_err_probe(dev, -EINVAL,
				     "No data passed, requires struct regmap**\n");

	edp_phy = devm_kzalloc(dev, sizeof(*edp_phy), GFP_KERNEL);
	if (!edp_phy)
		return -ENOMEM;

	edp_phy->regs = regs;
	phy = devm_phy_create(dev, NULL, &mtk_edp_phy_dev_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy),
				     "Failed to create DP PHY\n");

	phy_set_drvdata(phy, edp_phy);
	if (!dev->of_node)
		phy_create_lookup(phy, "edp", dev_name(dev));

	return 0;
}

struct platform_driver mtk_edp_phy_driver = {
	.probe = mtk_edp_phy_probe,
	.driver = {
		.name = "mediatek-edp-phy",
	},
};

module_platform_driver(mtk_edp_phy_driver);

MODULE_AUTHOR("Markus Schneider-Pargmann <msp@baylibre.com>");
MODULE_DESCRIPTION("MediaTek DP PHY Driver");
MODULE_LICENSE("GPL");
