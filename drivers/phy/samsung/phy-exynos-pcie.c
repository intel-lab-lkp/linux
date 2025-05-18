// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos SoC series PCIe PHY driver
 *
 * Phy provider for PCIe controller on Exynos SoC series
 *
 * Copyright (C) 2017-2020 Samsung Electronics Co., Ltd.
 * Jaehoon Chung <jh80.chung@samsung.com>
 */

#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regmap.h>

#define PCIE_PHY_OFFSET(x)		((x) * 0x4)

/* Sysreg FSYS register offsets and bits for Exynos5433 */
#define PCIE_EXYNOS5433_PHY_MAC_RESET		0x0208
#define PCIE_MAC_RESET_MASK			0xFF
#define PCIE_MAC_RESET				BIT(4)
#define PCIE_EXYNOS5433_PHY_L1SUB_CM_CON	0x1010
#define PCIE_REFCLK_GATING_EN			BIT(0)
#define PCIE_EXYNOS5433_PHY_COMMON_RESET	0x1020
#define PCIE_PHY_RESET				BIT(0)
#define PCIE_EXYNOS5433_PHY_GLOBAL_RESET	0x1040
#define PCIE_GLOBAL_RESET			BIT(0)
#define PCIE_REFCLK				BIT(1)
#define PCIE_REFCLK_MASK			0x16
#define PCIE_APP_REQ_EXIT_L1_MODE		BIT(5)

/* PMU PCIE PHY isolation control */
#define EXYNOS5433_PMU_PCIE_PHY_OFFSET		0x730

/* FSD: PCIe PHY common registers */
#define FSD_PCIE_PHY_TRSV_CMN_REG03	0x000c
#define FSD_PCIE_PHY_TRSV_CMN_REG01E	0x0078
#define FSD_PCIE_PHY_TRSV_CMN_REG02D	0x00b4
#define FSD_PCIE_PHY_TRSV_CMN_REG031	0x00c4
#define FSD_PCIE_PHY_TRSV_CMN_REG036	0x00d8
#define FSD_PCIE_PHY_TRSV_CMN_REG05F	0x017c
#define FSD_PCIE_PHY_TRSV_CMN_REG060	0x0180
#define FSD_PCIE_PHY_TRSV_CMN_REG062	0x0188
#define FSD_PCIE_PHY_TRSV_CMN_REG061	0x0184
#define FSD_PCIE_PHY_AGG_BIF_RESET	0x0200
#define FSD_PCIE_PHY_AGG_BIF_CLOCK	0x0208
#define FSD_PCIE_PHY_CMN_RESET		0x0228

/* FSD: PCIe PHY lane registers */
#define FSD_PCIE_PHY_LANE_OFFSET	0x400
#define FSD_PCIE_PHY_TRSV_REG001_LN_N	0x404
#define FSD_PCIE_PHY_TRSV_REG002_LN_N	0x408
#define FSD_PCIE_PHY_TRSV_REG005_LN_N	0x414
#define FSD_PCIE_PHY_TRSV_REG006_LN_N	0x418
#define FSD_PCIE_PHY_TRSV_REG007_LN_N	0x41c
#define FSD_PCIE_PHY_TRSV_REG009_LN_N	0x424
#define FSD_PCIE_PHY_TRSV_REG00A_LN_N	0x428
#define FSD_PCIE_PHY_TRSV_REG00C_LN_N	0x430
#define FSD_PCIE_PHY_TRSV_REG012_LN_N	0x448
#define FSD_PCIE_PHY_TRSV_REG013_LN_N	0x44c
#define FSD_PCIE_PHY_TRSV_REG014_LN_N	0x450
#define FSD_PCIE_PHY_TRSV_REG015_LN_N	0x454
#define FSD_PCIE_PHY_TRSV_REG016_LN_N	0x458
#define FSD_PCIE_PHY_TRSV_REG018_LN_N	0x460
#define FSD_PCIE_PHY_TRSV_REG020_LN_N	0x480
#define FSD_PCIE_PHY_TRSV_REG026_LN_N	0x498
#define FSD_PCIE_PHY_TRSV_REG029_LN_N	0x4a4
#define FSD_PCIE_PHY_TRSV_REG031_LN_N	0x4c4
#define FSD_PCIE_PHY_TRSV_REG036_LN_N	0x4d8
#define FSD_PCIE_PHY_TRSV_REG039_LN_N	0x4e4
#define FSD_PCIE_PHY_TRSV_REG03B_LN_N	0x4ec
#define FSD_PCIE_PHY_TRSV_REG03C_LN_N	0x4f0
#define FSD_PCIE_PHY_TRSV_REG03E_LN_N	0x4f8
#define FSD_PCIE_PHY_TRSV_REG03F_LN_N	0x4fc
#define FSD_PCIE_PHY_TRSV_REG043_LN_N	0x50c
#define FSD_PCIE_PHY_TRSV_REG044_LN_N	0x510
#define FSD_PCIE_PHY_TRSV_REG046_LN_N	0x518
#define FSD_PCIE_PHY_TRSV_REG048_LN_N	0x520
#define FSD_PCIE_PHY_TRSV_REG049_LN_N	0x524
#define FSD_PCIE_PHY_TRSV_REG04E_LN_N	0x538
#define FSD_PCIE_PHY_TRSV_REG052_LN_N	0x548
#define FSD_PCIE_PHY_TRSV_REG068_LN_N	0x5a0
#define FSD_PCIE_PHY_TRSV_REG069_LN_N	0x5a4
#define FSD_PCIE_PHY_TRSV_REG06A_LN_N	0x5a8
#define FSD_PCIE_PHY_TRSV_REG06B_LN_N	0x5ac
#define FSD_PCIE_PHY_TRSV_REG07B_LN_N	0x5ec
#define FSD_PCIE_PHY_TRSV_REG083_LN_N	0x60c
#define FSD_PCIE_PHY_TRSV_REG084_LN_N	0x610
#define FSD_PCIE_PHY_TRSV_REG086_LN_N	0x618
#define FSD_PCIE_PHY_TRSV_REG087_LN_N	0x61c
#define FSD_PCIE_PHY_TRSV_REG08B_LN_N	0x62c
#define FSD_PCIE_PHY_TRSV_REG09C_LN_N	0x670
#define FSD_PCIE_PHY_TRSV_REG09D_LN_N	0x674
#define FSD_PCIE_PHY_TRSV_REG09E_LN_N	0x678
#define FSD_PCIE_PHY_TRSV_REG09F_LN_N	0x67c
#define FSD_PCIE_PHY_TRSV_REG0A2_LN_N	0x688
#define FSD_PCIE_PHY_TRSV_REG0A4_LN_N	0x690
#define FSD_PCIE_PHY_TRSV_REG0CE_LN_N	0x738
#define FSD_PCIE_PHY_TRSV_REG0FC_LN_N	0x7f0
#define FSD_PCIE_PHY_TRSV_REG0FD_LN_N	0x7f4
#define FSD_PCIE_PHY_TRSV_REG0FE_LN_N	0x7f8
#define FSD_PCIE_PHY_TRSV_REG0CE_LN_1	0xb38
#define FSD_PCIE_PHY_TRSV_REG0CE_LN_2	0xf38
#define FSD_PCIE_PHY_TRSV_REG0CE_LN_3	0x1338

/* FSD: PCIe PCS registers */
#define FSD_PCIE_PCS_BRF_0		0x0004
#define FSD_PCIE_PCS_BRF_1		0x0804
#define FSD_PCIE_PCS_CLK		0x0180

/* FSD: PCIe SYSREG registers */
#define FSD_PCIE_SYSREG_PHY_0_CON_MASK			0x3ff
#define FSD_PCIE_SYSREG_PHY_0_CON			0x042C
#define FSD_PCIE_SYSREG_PHY_0_REF_SEL_MASK		0x3
#define FSD_PCIE_SYSREG_PHY_0_REF_SEL			(0x2 << 0)
#define FSD_PCIE_SYSREG_PHY_0_SSC_EN_MASK		0x8
#define FSD_PCIE_SYSREG_PHY_0_SSC_EN			BIT(3)
#define FSD_PCIE_SYSREG_PHY_0_AUX_EN_MASK		0x10
#define FSD_PCIE_SYSREG_PHY_0_AUX_EN			BIT(4)
#define FSD_PCIE_SYSREG_PHY_0_CMN_RSTN_MASK		0x100
#define FSD_PCIE_SYSREG_PHY_0_CMN_RSTN			BIT(8)
#define FSD_PCIE_SYSREG_PHY_0_INIT_RSTN_MASK		0x200
#define FSD_PCIE_SYSREG_PHY_0_INIT_RSTN			BIT(9)

#define FSD_PCIE_SYSREG_PHY_1_CON_MASK			0x1ff
#define FSD_PCIE_SYSREG_PHY_1_CON			0x0500
#define FSD_PCIE_SYSREG_PHY_1_REF_SEL_MASK		0x30
#define FSD_PCIE_SYSREG_PHY_1_REF_SEL			(0x2 << 4)
#define FSD_PCIE_SYSREG_PHY_1_SSC_EN_MASK		0x80
#define FSD_PCIE_SYSREG_PHY_1_SSC_EN			BIT(7)
#define FSD_PCIE_SYSREG_PHY_1_AUX_EN_MASK		0x1
#define FSD_PCIE_SYSREG_PHY_1_AUX_EN			BIT(0)
#define FSD_PCIE_SYSREG_PHY_1_CMN_RSTN_MASK		0x2
#define FSD_PCIE_SYSREG_PHY_1_CMN_RSTN			BIT(1)
#define FSD_PCIE_SYSREG_PHY_1_INIT_RSTN_MASK		0x8
#define FSD_PCIE_SYSREG_PHY_1_INIT_RSTN			BIT(3)

/* For Exynos pcie phy */
struct exynos_pcie_phy {
	void __iomem *base;
	void __iomem *pcs_base;
	struct regmap *pmureg;
	struct regmap *fsysreg;
	int phy_id;
	const struct samsung_drv_data *drv_data;
};

struct samsung_drv_data {
	const struct phy_ops *phy_ops;
};

static void exynos_pcie_phy_writel(void __iomem *base, u32 val, u32 offset)
{
	writel(val, base + offset);
}

/* Exynos5433 specific functions */
static int exynos5433_pcie_phy_init(struct phy *phy)
{
	struct exynos_pcie_phy *ep = phy_get_drvdata(phy);

	regmap_update_bits(ep->pmureg, EXYNOS5433_PMU_PCIE_PHY_OFFSET,
			   BIT(0), 1);
	regmap_update_bits(ep->fsysreg, PCIE_EXYNOS5433_PHY_GLOBAL_RESET,
			   PCIE_APP_REQ_EXIT_L1_MODE, 0);
	regmap_update_bits(ep->fsysreg, PCIE_EXYNOS5433_PHY_L1SUB_CM_CON,
			   PCIE_REFCLK_GATING_EN, 0);

	regmap_update_bits(ep->fsysreg,	PCIE_EXYNOS5433_PHY_COMMON_RESET,
			   PCIE_PHY_RESET, 1);
	regmap_update_bits(ep->fsysreg, PCIE_EXYNOS5433_PHY_MAC_RESET,
			   PCIE_MAC_RESET, 0);

	/* PHY refclk 24MHz */
	regmap_update_bits(ep->fsysreg, PCIE_EXYNOS5433_PHY_GLOBAL_RESET,
			   PCIE_REFCLK_MASK, PCIE_REFCLK);
	regmap_update_bits(ep->fsysreg, PCIE_EXYNOS5433_PHY_GLOBAL_RESET,
			   PCIE_GLOBAL_RESET, 0);


	exynos_pcie_phy_writel(ep->base, 0x11, PCIE_PHY_OFFSET(0x3));

	/* band gap reference on */
	exynos_pcie_phy_writel(ep->base, 0, PCIE_PHY_OFFSET(0x20));
	exynos_pcie_phy_writel(ep->base, 0, PCIE_PHY_OFFSET(0x4b));

	/* jitter tuning */
	exynos_pcie_phy_writel(ep->base, 0x34, PCIE_PHY_OFFSET(0x4));
	exynos_pcie_phy_writel(ep->base, 0x02, PCIE_PHY_OFFSET(0x7));
	exynos_pcie_phy_writel(ep->base, 0x41, PCIE_PHY_OFFSET(0x21));
	exynos_pcie_phy_writel(ep->base, 0x7F, PCIE_PHY_OFFSET(0x14));
	exynos_pcie_phy_writel(ep->base, 0xC0, PCIE_PHY_OFFSET(0x15));
	exynos_pcie_phy_writel(ep->base, 0x61, PCIE_PHY_OFFSET(0x36));

	/* D0 uninit.. */
	exynos_pcie_phy_writel(ep->base, 0x44, PCIE_PHY_OFFSET(0x3D));

	/* 24MHz */
	exynos_pcie_phy_writel(ep->base, 0x94, PCIE_PHY_OFFSET(0x8));
	exynos_pcie_phy_writel(ep->base, 0xA7, PCIE_PHY_OFFSET(0x9));
	exynos_pcie_phy_writel(ep->base, 0x93, PCIE_PHY_OFFSET(0xA));
	exynos_pcie_phy_writel(ep->base, 0x6B, PCIE_PHY_OFFSET(0xC));
	exynos_pcie_phy_writel(ep->base, 0xA5, PCIE_PHY_OFFSET(0xF));
	exynos_pcie_phy_writel(ep->base, 0x34, PCIE_PHY_OFFSET(0x16));
	exynos_pcie_phy_writel(ep->base, 0xA3, PCIE_PHY_OFFSET(0x17));
	exynos_pcie_phy_writel(ep->base, 0xA7, PCIE_PHY_OFFSET(0x1A));
	exynos_pcie_phy_writel(ep->base, 0x71, PCIE_PHY_OFFSET(0x23));
	exynos_pcie_phy_writel(ep->base, 0x4C, PCIE_PHY_OFFSET(0x24));

	exynos_pcie_phy_writel(ep->base, 0x0E, PCIE_PHY_OFFSET(0x26));
	exynos_pcie_phy_writel(ep->base, 0x14, PCIE_PHY_OFFSET(0x7));
	exynos_pcie_phy_writel(ep->base, 0x48, PCIE_PHY_OFFSET(0x43));
	exynos_pcie_phy_writel(ep->base, 0x44, PCIE_PHY_OFFSET(0x44));
	exynos_pcie_phy_writel(ep->base, 0x03, PCIE_PHY_OFFSET(0x45));
	exynos_pcie_phy_writel(ep->base, 0xA7, PCIE_PHY_OFFSET(0x48));
	exynos_pcie_phy_writel(ep->base, 0x13, PCIE_PHY_OFFSET(0x54));
	exynos_pcie_phy_writel(ep->base, 0x04, PCIE_PHY_OFFSET(0x31));
	exynos_pcie_phy_writel(ep->base, 0, PCIE_PHY_OFFSET(0x32));

	regmap_update_bits(ep->fsysreg, PCIE_EXYNOS5433_PHY_COMMON_RESET,
			   PCIE_PHY_RESET, 0);
	regmap_update_bits(ep->fsysreg, PCIE_EXYNOS5433_PHY_MAC_RESET,
			   PCIE_MAC_RESET_MASK, PCIE_MAC_RESET);
	return 0;
}

static int exynos5433_pcie_phy_exit(struct phy *phy)
{
	struct exynos_pcie_phy *ep = phy_get_drvdata(phy);

	regmap_update_bits(ep->fsysreg, PCIE_EXYNOS5433_PHY_L1SUB_CM_CON,
			   PCIE_REFCLK_GATING_EN, PCIE_REFCLK_GATING_EN);
	regmap_update_bits(ep->pmureg, EXYNOS5433_PMU_PCIE_PHY_OFFSET,
			   BIT(0), 0);
	return 0;
}

static const struct phy_ops exynos5433_phy_ops = {
	.init		= exynos5433_pcie_phy_init,
	.exit		= exynos5433_pcie_phy_exit,
	.owner		= THIS_MODULE,
};

struct fsd_pcie_phy_pdata {
	u32 phy_con_mask;
	u32 phy_con;
	u32 phy_ref_sel_mask;
	u32 phy_ref_sel;
	u32 phy_ssc_en_mask;
	u32 phy_ssc_en;
	u32 phy_aux_en_mask;
	u32 phy_aux_en;
	u32 phy_cmn_rstn_mask;
	u32 phy_cmn_rstn;
	u32 phy_init_rstn_mask;
	u32 phy_init_rstn;
	u32 num_lanes;
	u32 lane_offset;
};

struct fsd_pcie_phy_pdata fsd_phy_con[] = {
	{
	.phy_con		= FSD_PCIE_SYSREG_PHY_0_CON,
	.phy_con_mask		= FSD_PCIE_SYSREG_PHY_0_CON_MASK,
	.phy_ref_sel_mask	= FSD_PCIE_SYSREG_PHY_0_REF_SEL_MASK,
	.phy_ref_sel		= FSD_PCIE_SYSREG_PHY_0_REF_SEL,
	.phy_ssc_en_mask	= FSD_PCIE_SYSREG_PHY_0_SSC_EN_MASK,
	.phy_ssc_en		= FSD_PCIE_SYSREG_PHY_0_SSC_EN,
	.phy_aux_en_mask	= FSD_PCIE_SYSREG_PHY_0_AUX_EN_MASK,
	.phy_aux_en		= FSD_PCIE_SYSREG_PHY_0_AUX_EN,
	.phy_cmn_rstn_mask	= FSD_PCIE_SYSREG_PHY_0_CMN_RSTN_MASK,
	.phy_cmn_rstn		= FSD_PCIE_SYSREG_PHY_0_CMN_RSTN,
	.phy_init_rstn_mask	= FSD_PCIE_SYSREG_PHY_0_INIT_RSTN_MASK,
	.phy_init_rstn		= FSD_PCIE_SYSREG_PHY_0_INIT_RSTN,
	.num_lanes		= 0x4,
	.lane_offset		= FSD_PCIE_PHY_LANE_OFFSET,
	},
	{
	.phy_con		= FSD_PCIE_SYSREG_PHY_1_CON,
	.phy_con_mask		= FSD_PCIE_SYSREG_PHY_1_CON_MASK,
	.phy_ref_sel_mask	= FSD_PCIE_SYSREG_PHY_1_REF_SEL_MASK,
	.phy_ref_sel		= FSD_PCIE_SYSREG_PHY_1_REF_SEL,
	.phy_ssc_en_mask	= FSD_PCIE_SYSREG_PHY_1_SSC_EN_MASK,
	.phy_ssc_en		= FSD_PCIE_SYSREG_PHY_1_SSC_EN,
	.phy_aux_en_mask	= FSD_PCIE_SYSREG_PHY_1_AUX_EN_MASK,
	.phy_aux_en		= FSD_PCIE_SYSREG_PHY_1_AUX_EN,
	.phy_cmn_rstn_mask	= FSD_PCIE_SYSREG_PHY_1_CMN_RSTN_MASK,
	.phy_cmn_rstn		= FSD_PCIE_SYSREG_PHY_1_CMN_RSTN,
	.phy_init_rstn_mask	= FSD_PCIE_SYSREG_PHY_1_INIT_RSTN_MASK,
	.phy_init_rstn		= FSD_PCIE_SYSREG_PHY_1_INIT_RSTN,
	.num_lanes		= 0x4,
	.lane_offset		= FSD_PCIE_PHY_LANE_OFFSET,
	},
	{ },
};

struct fsd_pcie_phy_setting {
	u32 addr;
	u32 val;
	bool is_cmn_reg;
};

struct fsd_pcie_phy_setting fsd_pcie_phy0_setting[] = {
	{FSD_PCIE_PHY_TRSV_REG07B_LN_N, 0x20, false},
	{FSD_PCIE_PHY_TRSV_REG052_LN_N, 0x00, false},
	{FSD_PCIE_PHY_TRSV_CMN_REG05F, 0x11, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG060, 0x23, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG062, 0x0, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG03, 0x15, true},
	{FSD_PCIE_PHY_TRSV_REG0CE_LN_N, 0x8, false},
	{FSD_PCIE_PHY_TRSV_REG039_LN_N, 0xf, false},
	{FSD_PCIE_PHY_TRSV_REG03B_LN_N, 0x13, false},
	{FSD_PCIE_PHY_TRSV_REG03C_LN_N, 0xf6, false},
	{FSD_PCIE_PHY_TRSV_REG044_LN_N, 0x57, false},
	{FSD_PCIE_PHY_TRSV_REG03E_LN_N, 0x10, false},
	{FSD_PCIE_PHY_TRSV_REG03F_LN_N, 0x04, false},
	{FSD_PCIE_PHY_TRSV_REG043_LN_N, 0x11, false},
	{FSD_PCIE_PHY_TRSV_REG049_LN_N, 0x6f, false},
	{FSD_PCIE_PHY_TRSV_REG04E_LN_N, 0x18, false},
	{FSD_PCIE_PHY_TRSV_REG068_LN_N, 0x1f, false},
	{FSD_PCIE_PHY_TRSV_REG069_LN_N, 0xc, false},
	{FSD_PCIE_PHY_TRSV_REG06B_LN_N, 0x78, false},
	{FSD_PCIE_PHY_TRSV_REG083_LN_N, 0xa, false},
	{FSD_PCIE_PHY_TRSV_REG084_LN_N, 0x80, false},
	{FSD_PCIE_PHY_TRSV_REG086_LN_N, 0xff, false},
	{FSD_PCIE_PHY_TRSV_REG087_LN_N, 0x3c, false},
	{FSD_PCIE_PHY_TRSV_REG09D_LN_N, 0x7c, false},
	{FSD_PCIE_PHY_TRSV_REG09E_LN_N, 0x33, false},
	{FSD_PCIE_PHY_TRSV_REG09F_LN_N, 0x33, false},
	{FSD_PCIE_PHY_TRSV_REG001_LN_N, 0x3f, false},
	{FSD_PCIE_PHY_TRSV_REG002_LN_N, 0x1c, false},
	{FSD_PCIE_PHY_TRSV_REG005_LN_N, 0x2b, false},
	{FSD_PCIE_PHY_TRSV_REG006_LN_N, 0x3, false},
	{FSD_PCIE_PHY_TRSV_REG007_LN_N, 0x0c, false},
	{FSD_PCIE_PHY_TRSV_REG009_LN_N, 0x10, false},
	{FSD_PCIE_PHY_TRSV_REG00A_LN_N, 0x1, false},
	{FSD_PCIE_PHY_TRSV_REG00C_LN_N, 0x93, false},
	{FSD_PCIE_PHY_TRSV_REG012_LN_N, 0x1, false},
	{FSD_PCIE_PHY_TRSV_REG013_LN_N, 0x0, false},
	{FSD_PCIE_PHY_TRSV_REG014_LN_N, 0x70, false},
	{FSD_PCIE_PHY_TRSV_REG015_LN_N, 0x0, false},
	{FSD_PCIE_PHY_TRSV_REG016_LN_N, 0x70, false},
	{FSD_PCIE_PHY_TRSV_REG0FC_LN_N, 0x80, false},
	{FSD_PCIE_PHY_TRSV_REG0FD_LN_N, 0x0, false},
};

struct fsd_pcie_phy_setting fsd_pcie_phy1_setting[] = {
	{FSD_PCIE_PHY_TRSV_REG07B_LN_N, 0x20, false},
	{FSD_PCIE_PHY_TRSV_REG052_LN_N, 0x00, false},
	{FSD_PCIE_PHY_TRSV_CMN_REG01E, 0xaa, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG02D, 0x28, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG031, 0x28, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG036, 0x21, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG05F, 0x12, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG060, 0x23, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG061, 0x0, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG062, 0x0, true},
	{FSD_PCIE_PHY_TRSV_CMN_REG03, 0x15, true},
	{FSD_PCIE_PHY_TRSV_REG039_LN_N, 0xf, false},
	{FSD_PCIE_PHY_TRSV_REG03B_LN_N, 0x13, false},
	{FSD_PCIE_PHY_TRSV_REG03C_LN_N, 0x66, false},
	{FSD_PCIE_PHY_TRSV_REG044_LN_N, 0x57, false},
	{FSD_PCIE_PHY_TRSV_REG03E_LN_N, 0x10, false},
	{FSD_PCIE_PHY_TRSV_REG03F_LN_N, 0x44, false},
	{FSD_PCIE_PHY_TRSV_REG043_LN_N, 0x11, false},
	{FSD_PCIE_PHY_TRSV_REG046_LN_N, 0xef, false},
	{FSD_PCIE_PHY_TRSV_REG048_LN_N, 0x06, false},
	{FSD_PCIE_PHY_TRSV_REG049_LN_N, 0xaf, false},
	{FSD_PCIE_PHY_TRSV_REG04E_LN_N, 0x28, false},
	{FSD_PCIE_PHY_TRSV_REG068_LN_N, 0x1f, false},
	{FSD_PCIE_PHY_TRSV_REG069_LN_N, 0xc, false},
	{FSD_PCIE_PHY_TRSV_REG06A_LN_N, 0x8, false},
	{FSD_PCIE_PHY_TRSV_REG06B_LN_N, 0x78, false},
	{FSD_PCIE_PHY_TRSV_REG083_LN_N, 0xa, false},
	{FSD_PCIE_PHY_TRSV_REG084_LN_N, 0x80, false},
	{FSD_PCIE_PHY_TRSV_REG087_LN_N, 0x30, false},
	{FSD_PCIE_PHY_TRSV_REG08B_LN_N, 0xa0, false},
	{FSD_PCIE_PHY_TRSV_REG09C_LN_N, 0xf7, false},
	{FSD_PCIE_PHY_TRSV_REG09E_LN_N, 0x33, false},
	{FSD_PCIE_PHY_TRSV_REG0A2_LN_N, 0xfa, false},
	{FSD_PCIE_PHY_TRSV_REG0A4_LN_N, 0xf2, false},
	{FSD_PCIE_PHY_TRSV_REG0CE_LN_N, 0x08, true},
	{FSD_PCIE_PHY_TRSV_REG0CE_LN_1, 0x09, true},
	{FSD_PCIE_PHY_TRSV_REG0CE_LN_2, 0x09, true},
	{FSD_PCIE_PHY_TRSV_REG0CE_LN_3, 0x09, true},
	{FSD_PCIE_PHY_TRSV_REG0FE_LN_N, 0x33, false},
	{FSD_PCIE_PHY_TRSV_REG001_LN_N, 0x3f, false},
	{FSD_PCIE_PHY_TRSV_REG005_LN_N, 0x2b, false},
};

static void fsd_pcie_phy_writel(struct exynos_pcie_phy *phy_ctrl,
							u32 val, u32 offset)
{
	struct fsd_pcie_phy_pdata *pdata = &fsd_phy_con[phy_ctrl->phy_id];
	void __iomem *phy_base = phy_ctrl->base;
	u32 i;

	for (i = 0; i < pdata->num_lanes; i++)
		writel(val, phy_base + (offset + i * pdata->lane_offset));
}

static int fsd_pcie_phy_reset(struct phy *phy)
{
	struct exynos_pcie_phy *phy_ctrl = phy_get_drvdata(phy);
	struct fsd_pcie_phy_pdata *pdata = &fsd_phy_con[phy_ctrl->phy_id];

	writel(0x1, phy_ctrl->pcs_base + FSD_PCIE_PCS_CLK);

	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con, pdata->phy_con_mask,
			   0x0);
	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con, pdata->phy_aux_en_mask,
			   pdata->phy_aux_en);
	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con, pdata->phy_ref_sel_mask,
			   pdata->phy_ref_sel);

	/* perform init reset release */
	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con,
			pdata->phy_init_rstn_mask, pdata->phy_init_rstn);
	return 0;
}

static int fsd_pcie_phy_init(struct phy *phy)
{
	struct fsd_pcie_phy_setting *phy_param = fsd_pcie_phy0_setting;
	struct exynos_pcie_phy *phy_ctrl = phy_get_drvdata(phy);
	struct fsd_pcie_phy_pdata *pdata = &fsd_phy_con[phy_ctrl->phy_id];
	int len = ARRAY_SIZE(fsd_pcie_phy0_setting);
	void __iomem *phy_base = phy_ctrl->base;
	int i;

	fsd_pcie_phy_reset(phy);

	if (phy_ctrl->phy_id == 1) {
		writel(0x2, phy_base + FSD_PCIE_PHY_CMN_RESET);
		phy_param = fsd_pcie_phy1_setting;
		len = ARRAY_SIZE(fsd_pcie_phy1_setting);
	}

	writel(0x00, phy_ctrl->pcs_base + FSD_PCIE_PCS_BRF_0);
	writel(0x00, phy_ctrl->pcs_base + FSD_PCIE_PCS_BRF_1);
	writel(0x00, phy_base + FSD_PCIE_PHY_AGG_BIF_RESET);
	writel(0x00, phy_base + FSD_PCIE_PHY_AGG_BIF_CLOCK);

	for (i = 0; i < len; i++) {
		if (phy_param[i].is_cmn_reg)
			writel(phy_param[i].val, phy_base + phy_param[i].addr);
		else
			fsd_pcie_phy_writel(phy_ctrl, phy_param[i].val, phy_param[i].addr);
	}

	if (phy_ctrl->phy_id == 1)
		writel(0x3, phy_base + FSD_PCIE_PHY_CMN_RESET);

	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con,
			pdata->phy_cmn_rstn_mask, pdata->phy_cmn_rstn);

	return 0;
}

static const struct phy_ops fsd_phy_ops = {
	.init		= fsd_pcie_phy_init,
	.reset		= fsd_pcie_phy_reset,
	.owner		= THIS_MODULE,
};

static const struct samsung_drv_data exynos5433_drv_data = {
	.phy_ops		= &exynos5433_phy_ops,
};

static const struct samsung_drv_data fsd_drv_data = {
	.phy_ops		= &fsd_phy_ops,
};

static const struct of_device_id exynos_pcie_phy_match[] = {
	{
		.compatible = "samsung,exynos5433-pcie-phy",
		.data = &exynos5433_drv_data,
	},
	{
		.compatible = "tesla,fsd-pcie-phy",
		.data = &fsd_drv_data,
	},
	{},
};

static int exynos_pcie_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_pcie_phy *exynos_phy;
	struct phy *generic_phy;
	struct phy_provider *phy_provider;
	const struct samsung_drv_data *drv_data;

	drv_data = of_device_get_match_data(dev);
	if (!drv_data)
		return -ENODEV;

	exynos_phy = devm_kzalloc(dev, sizeof(*exynos_phy), GFP_KERNEL);
	if (!exynos_phy)
		return -ENOMEM;

	exynos_phy->drv_data = drv_data;

	exynos_phy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(exynos_phy->base))
		return PTR_ERR(exynos_phy->base);

	exynos_phy->pmureg = syscon_regmap_lookup_by_phandle(dev->of_node,
							"samsung,pmu-syscon");
	if (IS_ERR(exynos_phy->pmureg)) {
		dev_err(&pdev->dev, "PMU regmap lookup failed.\n");
		return PTR_ERR(exynos_phy->pmureg);
	}

	exynos_phy->fsysreg = syscon_regmap_lookup_by_phandle(dev->of_node,
							 "samsung,fsys-sysreg");
	if (IS_ERR(exynos_phy->fsysreg)) {
		dev_err(&pdev->dev, "FSYS sysreg regmap lookup failed.\n");
		return PTR_ERR(exynos_phy->fsysreg);
	}

	generic_phy = devm_phy_create(dev, dev->of_node, drv_data->phy_ops);
	if (IS_ERR(generic_phy)) {
		dev_err(dev, "failed to create PHY\n");
		return PTR_ERR(generic_phy);
	}

	exynos_phy->pcs_base = devm_platform_ioremap_resource(pdev, 1);
	exynos_phy->phy_id = of_alias_get_id(dev->of_node, "pciephy");

	phy_set_drvdata(generic_phy, exynos_phy);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static struct platform_driver exynos_pcie_phy_driver = {
	.probe	= exynos_pcie_phy_probe,
	.driver = {
		.of_match_table	= exynos_pcie_phy_match,
		.name		= "exynos_pcie_phy",
		.suppress_bind_attrs = true,
	}
};
builtin_platform_driver(exynos_pcie_phy_driver);
