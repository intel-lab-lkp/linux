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
#define FSD_PCIE_LANE_OFFSET		0x0400
#define FSD_PCIE_NUM_LANES		0x4

#define FSD_PCIE_PHY_TRSV_REG001_LN_N	0x0404
#define FSD_PCIE_PHY_TRSV_REG002_LN_N	0x0408
#define FSD_PCIE_PHY_TRSV_REG005_LN_N	0x0414
#define FSD_PCIE_PHY_TRSV_REG006_LN_N	0x0418
#define FSD_PCIE_PHY_TRSV_REG007_LN_N	0x041c
#define FSD_PCIE_PHY_TRSV_REG009_LN_N	0x0424
#define FSD_PCIE_PHY_TRSV_REG00A_LN_N	0x0428
#define FSD_PCIE_PHY_TRSV_REG00C_LN_N	0x0430
#define FSD_PCIE_PHY_TRSV_REG012_LN_N	0x0448
#define FSD_PCIE_PHY_TRSV_REG013_LN_N	0x044c
#define FSD_PCIE_PHY_TRSV_REG014_LN_N	0x0450
#define FSD_PCIE_PHY_TRSV_REG015_LN_N	0x0454
#define FSD_PCIE_PHY_TRSV_REG016_LN_N	0x0458
#define FSD_PCIE_PHY_TRSV_REG018_LN_N	0x0460
#define FSD_PCIE_PHY_TRSV_REG020_LN_N	0x0480
#define FSD_PCIE_PHY_TRSV_REG026_LN_N	0x0498
#define FSD_PCIE_PHY_TRSV_REG029_LN_N	0x04a4
#define FSD_PCIE_PHY_TRSV_REG031_LN_N	0x04c4
#define FSD_PCIE_PHY_TRSV_REG036_LN_N	0x04d8
#define FSD_PCIE_PHY_TRSV_REG039_LN_N	0x04e4
#define FSD_PCIE_PHY_TRSV_REG03B_LN_N	0x04ec
#define FSD_PCIE_PHY_TRSV_REG03C_LN_N	0x04f0
#define FSD_PCIE_PHY_TRSV_REG03E_LN_N	0x04f8
#define FSD_PCIE_PHY_TRSV_REG03F_LN_N	0x04fc
#define FSD_PCIE_PHY_TRSV_REG043_LN_N	0x050c
#define FSD_PCIE_PHY_TRSV_REG044_LN_N	0x0510
#define FSD_PCIE_PHY_TRSV_REG046_LN_N	0x0518
#define FSD_PCIE_PHY_TRSV_REG048_LN_N	0x0520
#define FSD_PCIE_PHY_TRSV_REG049_LN_N	0x0524
#define FSD_PCIE_PHY_TRSV_REG04E_LN_N	0x0538
#define FSD_PCIE_PHY_TRSV_REG052_LN_N	0x0548
#define FSD_PCIE_PHY_TRSV_REG068_LN_N	0x05a0
#define FSD_PCIE_PHY_TRSV_REG069_LN_N	0x05a4
#define FSD_PCIE_PHY_TRSV_REG06A_LN_N	0x05a8
#define FSD_PCIE_PHY_TRSV_REG06B_LN_N	0x05ac
#define FSD_PCIE_PHY_TRSV_REG07B_LN_N	0x05ec
#define FSD_PCIE_PHY_TRSV_REG083_LN_N	0x060c
#define FSD_PCIE_PHY_TRSV_REG084_LN_N	0x0610
#define FSD_PCIE_PHY_TRSV_REG086_LN_N	0x0618
#define FSD_PCIE_PHY_TRSV_REG087_LN_N	0x061c
#define FSD_PCIE_PHY_TRSV_REG08B_LN_N	0x062c
#define FSD_PCIE_PHY_TRSV_REG09C_LN_N	0x0670
#define FSD_PCIE_PHY_TRSV_REG09D_LN_N	0x0674
#define FSD_PCIE_PHY_TRSV_REG09E_LN_N	0x0678
#define FSD_PCIE_PHY_TRSV_REG09F_LN_N	0x067c
#define FSD_PCIE_PHY_TRSV_REG0A2_LN_N	0x0688
#define FSD_PCIE_PHY_TRSV_REG0A4_LN_N	0x0690
#define FSD_PCIE_PHY_TRSV_REG0CE_LN_N	0x0738
#define FSD_PCIE_PHY_TRSV_REG0FC_LN_N	0x07f0
#define FSD_PCIE_PHY_TRSV_REG0FD_LN_N	0x07f4
#define FSD_PCIE_PHY_TRSV_REG0FE_LN_N	0x07f8
#define FSD_PCIE_PHY_TRSV_REG0CE_LN_1	0x0b38
#define FSD_PCIE_PHY_TRSV_REG0CE_LN_2	0x0f38
#define FSD_PCIE_PHY_TRSV_REG0CE_LN_3	0x1338

/* FSD: PCIe PCS registers */
#define FSD_PCIE_PCS_BRF_0		0x0004
#define FSD_PCIE_PCS_BRF_1		0x0804
#define FSD_PCIE_PCS_CLK		0x0180

/* FSD: PCIe SYSREG registers */
#define FSD_PCIE_SYSREG_PHY_0_CON			0x042c
#define FSD_PCIE_SYSREG_PHY_0_CON_MASK			0x03ff
#define FSD_PCIE_SYSREG_PHY_0_REF_SEL			(0x2 << 0)
#define FSD_PCIE_SYSREG_PHY_0_REF_SEL_MASK		0x3
#define FSD_PCIE_SYSREG_PHY_0_AUX_EN			BIT(4)
#define FSD_PCIE_SYSREG_PHY_0_CMN_RSTN			BIT(8)
#define FSD_PCIE_SYSREG_PHY_0_INIT_RSTN			BIT(9)

#define FSD_PCIE_SYSREG_PHY_1_CON			0x0500
#define FSD_PCIE_SYSREG_PHY_1_CON_MASK			0x01ff
#define FSD_PCIE_SYSREG_PHY_1_REF_SEL			(0x2 << 4)
#define FSD_PCIE_SYSREG_PHY_1_REF_SEL_MASK		0x30
#define FSD_PCIE_SYSREG_PHY_1_AUX_EN			BIT(0)
#define FSD_PCIE_SYSREG_PHY_1_CMN_RSTN			BIT(1)
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

static void fsd_pcie_phy_writel(struct exynos_pcie_phy *phy_ctrl, u32 offset, u32 val)
{
	void __iomem *phy_base = phy_ctrl->base;
	u32 i;

	for (i = 0; i < FSD_PCIE_NUM_LANES; i++)
		writel(val, phy_base + (offset + i * FSD_PCIE_LANE_OFFSET));
}

struct fsd_pcie_phy_pdata {
	u32 phy_con_offset;
	u32 phy_con_mask;
	u32 phy_ref_sel;
	u32 phy_ref_sel_mask;
	u32 phy_aux_en;
	u32 phy_cmn_rstn;
	u32 phy_init_rstn;
};

static const struct fsd_pcie_phy_pdata fsd_phy_con[] = {
{
	.phy_con_offset		= FSD_PCIE_SYSREG_PHY_0_CON,
	.phy_con_mask		= FSD_PCIE_SYSREG_PHY_0_CON_MASK,
	.phy_ref_sel		= FSD_PCIE_SYSREG_PHY_0_REF_SEL,
	.phy_ref_sel_mask	= FSD_PCIE_SYSREG_PHY_0_REF_SEL_MASK,
	.phy_aux_en		= FSD_PCIE_SYSREG_PHY_0_AUX_EN,
	.phy_cmn_rstn		= FSD_PCIE_SYSREG_PHY_0_CMN_RSTN,
	.phy_init_rstn		= FSD_PCIE_SYSREG_PHY_0_INIT_RSTN,
	},
	{
	.phy_con_offset		= FSD_PCIE_SYSREG_PHY_1_CON,
	.phy_con_mask		= FSD_PCIE_SYSREG_PHY_1_CON_MASK,
	.phy_ref_sel		= FSD_PCIE_SYSREG_PHY_1_REF_SEL,
	.phy_ref_sel_mask	= FSD_PCIE_SYSREG_PHY_1_REF_SEL_MASK,
	.phy_aux_en		= FSD_PCIE_SYSREG_PHY_1_AUX_EN,
	.phy_cmn_rstn		= FSD_PCIE_SYSREG_PHY_1_CMN_RSTN,
	.phy_init_rstn		= FSD_PCIE_SYSREG_PHY_1_INIT_RSTN,
	},
	{ },
};

static int fsd_pcie_phy_reset(struct phy *phy)
{
	struct exynos_pcie_phy *phy_ctrl = phy_get_drvdata(phy);
	const struct fsd_pcie_phy_pdata *pdata = &fsd_phy_con[phy_ctrl->phy_id];

	writel(0x1, phy_ctrl->pcs_base + FSD_PCIE_PCS_CLK);

	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con_offset,
			pdata->phy_con_mask, 0x0);
	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con_offset,
			pdata->phy_aux_en, pdata->phy_aux_en);
	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con_offset,
			pdata->phy_ref_sel_mask, pdata->phy_ref_sel);
	/* perform init reset release */
	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con_offset,
			pdata->phy_init_rstn, pdata->phy_init_rstn);

	return 0;
}

static void fsd_pcie_phy1_init(struct exynos_pcie_phy *phy_ctrl)
{
	void __iomem *pbase = phy_ctrl->base;

	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG07B_LN_N, 0x20);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG052_LN_N, 0x00);
	writel(0xaa, pbase + FSD_PCIE_PHY_TRSV_CMN_REG01E);
	writel(0x28, pbase + FSD_PCIE_PHY_TRSV_CMN_REG02D);
	writel(0x28, pbase + FSD_PCIE_PHY_TRSV_CMN_REG031);
	writel(0x21, pbase + FSD_PCIE_PHY_TRSV_CMN_REG036);
	writel(0x12, pbase + FSD_PCIE_PHY_TRSV_CMN_REG05F);
	writel(0x23, pbase + FSD_PCIE_PHY_TRSV_CMN_REG060);
	writel(0x0, pbase + FSD_PCIE_PHY_TRSV_CMN_REG061);
	writel(0x0, pbase + FSD_PCIE_PHY_TRSV_CMN_REG062);
	writel(0x15, pbase + FSD_PCIE_PHY_TRSV_CMN_REG03);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG039_LN_N, 0xf);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG03B_LN_N, 0x13);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG03C_LN_N, 0x66);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG044_LN_N, 0x57);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG03E_LN_N, 0x10);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG03F_LN_N, 0x44);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG043_LN_N, 0x11);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG046_LN_N, 0xef);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG048_LN_N, 0x06);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG049_LN_N, 0xaf);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG04E_LN_N, 0x28);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG068_LN_N, 0x1f);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG069_LN_N, 0xc);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG06A_LN_N, 0x8);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG06B_LN_N, 0x78);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG083_LN_N, 0xa);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG084_LN_N, 0x80);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG087_LN_N, 0x30);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG08B_LN_N, 0xa0);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG09C_LN_N, 0xf7);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG09E_LN_N, 0x33);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG0A2_LN_N, 0xfa);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG0A4_LN_N, 0xf2);
	writel(0x8, pbase + FSD_PCIE_PHY_TRSV_REG0CE_LN_N);
	writel(0x9, pbase + FSD_PCIE_PHY_TRSV_REG0CE_LN_1);
	writel(0x9, pbase + FSD_PCIE_PHY_TRSV_REG0CE_LN_2);
	writel(0x9, pbase + FSD_PCIE_PHY_TRSV_REG0CE_LN_3);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG0FE_LN_N, 0x33);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG001_LN_N, 0x3f);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG005_LN_N, 0x2b);
}

static void fsd_pcie_phy0_init(struct exynos_pcie_phy *phy_ctrl)
{
	void __iomem *pbase = phy_ctrl->base;

	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG07B_LN_N, 0x20);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG052_LN_N, 0x00);
	writel(0x11, pbase + FSD_PCIE_PHY_TRSV_CMN_REG05F);
	writel(0x23, pbase + FSD_PCIE_PHY_TRSV_CMN_REG060);
	writel(0x0, pbase + FSD_PCIE_PHY_TRSV_CMN_REG062);
	writel(0x15, pbase + FSD_PCIE_PHY_TRSV_CMN_REG03);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG0CE_LN_N, 0x8);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG039_LN_N, 0xf);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG03B_LN_N, 0x13);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG03C_LN_N, 0xf6);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG044_LN_N, 0x57);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG03E_LN_N, 0x10);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG03F_LN_N, 0x04);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG043_LN_N, 0x11);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG049_LN_N, 0x6f);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG04E_LN_N, 0x18);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG068_LN_N, 0x1f);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG069_LN_N, 0xc);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG06B_LN_N, 0x78);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG083_LN_N, 0xa);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG084_LN_N, 0x80);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG086_LN_N, 0xff);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG087_LN_N, 0x3c);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG09D_LN_N, 0x7c);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG09E_LN_N, 0x33);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG09F_LN_N, 0x33);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG001_LN_N, 0x3f);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG002_LN_N, 0x1c);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG005_LN_N, 0x2b);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG006_LN_N, 0x3);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG007_LN_N, 0x0c);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG009_LN_N, 0x10);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG00A_LN_N, 0x1);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG00C_LN_N, 0x93);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG012_LN_N, 0x1);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG013_LN_N, 0x0);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG014_LN_N, 0x70);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG015_LN_N, 0x0);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG016_LN_N, 0x70);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG0FC_LN_N, 0x80);
	fsd_pcie_phy_writel(phy_ctrl, FSD_PCIE_PHY_TRSV_REG0FD_LN_N, 0x0);
}

static int fsd_pcie_phy_init(struct phy *phy)
{
	struct exynos_pcie_phy *phy_ctrl = phy_get_drvdata(phy);
	void __iomem *phy_base = phy_ctrl->base;
	const struct fsd_pcie_phy_pdata *pdata = &fsd_phy_con[phy_ctrl->phy_id];

	fsd_pcie_phy_reset(phy);

	if (phy_ctrl->phy_id == 1)
		writel(0x2, phy_base + FSD_PCIE_PHY_CMN_RESET);

	writel(0x00, phy_ctrl->pcs_base + FSD_PCIE_PCS_BRF_0);
	writel(0x00, phy_ctrl->pcs_base + FSD_PCIE_PCS_BRF_1);
	writel(0x00, phy_base + FSD_PCIE_PHY_AGG_BIF_RESET);
	writel(0x00, phy_base + FSD_PCIE_PHY_AGG_BIF_CLOCK);

	if (phy_ctrl->phy_id == 1) {
		fsd_pcie_phy1_init(phy_ctrl);
		writel(0x3, phy_base + FSD_PCIE_PHY_CMN_RESET);
	} else {
		fsd_pcie_phy0_init(phy_ctrl);
	}

	regmap_update_bits(phy_ctrl->fsysreg, pdata->phy_con_offset,
			pdata->phy_cmn_rstn, pdata->phy_cmn_rstn);

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
