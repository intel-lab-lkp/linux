// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Samsung Electronics Co., Ltd.
 * Author: Raghav Sharma <raghav.s@samsung.com>
 *
 * Common Clock Framework support for Exynos 8855 SoC.
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/samsung,exynos8855.h>

#include "clk.h"
#include "clk-exynos-arm64.h"

/* NOTE: Must be equal to the last clock ID increased by one */
#define CLKS_NR_TOP                     (CLKCMU_DOUT_USB_USB20DRD + 1)
#define CLKS_NR_PERIC                   (CLK_GOUT_USI_PERIC_IPCLKPORT_PCLK + 1)
#define CLKS_NR_PERIS                   (CLK_GOUT_WDT1_PERIS_IPCLKPORT_PCLK + 1)
#define CLKS_NR_HSI                     (CLK_GOUT_SYSREG_HSI_IPCLKPORT_PCLK  + 1)
#define CLKS_NR_USB                     (CLK_GOUT_SYSREG_USB_IPCLKPORT_PCLK + 1)

/* ---- CMU_TOP --------------------------------------------------------- */

/* Register Offset definitions for CMU_TOP (0x13900000) */
/* PLL */
#define PLL_LOCKTIME_PLL_SHARED0		             0x8
#define PLL_LOCKTIME_PLL_SHARED1		             0x10
#define PLL_LOCKTIME_PLL_SHARED2		             0x18
#define PLL_LOCKTIME_PLL_MMC                                 0x20
#define PLL_LOCKTIME_PLL_SHARED3		             0x28
#define PLL_LOCKTIME_PLL_SHARED4		             0x38
#define PLL_CON0_PLL_MMC			             0x1c0
#define PLL_CON3_PLL_MMC			             0x1cc
#define PLL_CON0_PLL_SHARED0			             0x100
#define PLL_CON3_PLL_SHARED0			             0x10c
#define PLL_CON0_PLL_SHARED1			             0x140
#define PLL_CON3_PLL_SHARED1			             0x14c
#define PLL_CON0_PLL_SHARED2			             0x180
#define PLL_CON3_PLL_SHARED2			             0x18c
#define PLL_CON0_PLL_SHARED3			             0x200
#define PLL_CON3_PLL_SHARED3			             0x20c
#define PLL_CON0_PLL_SHARED4			             0x280
#define PLL_CON3_PLL_SHARED4			             0x28c

/* MUX */
#define CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_NOC                 0x1078
#define CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_MMC_CARD            0x107c
#define CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_IP                  0x1080

/* DIV */
#define CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_NOC                 0x1878
#define CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_MMC_CARD            0x187c
#define CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_IP                  0x1880
#define CMU_TOP_CLK_CON_MUX_CLKCMU_PERIS_GIC                 0x10c4
#define CMU_TOP_CLK_CON_MUX_CLKCMU_PERIS_NOC                 0x10c8
#define CMU_TOP_CLK_CON_DIV_CLKCMU_PERIS_GIC                 0x18c0
#define CMU_TOP_CLK_CON_DIV_CLKCMU_PERIS_NOC                 0x18c4
#define CMU_TOP_CLK_CON_MUX_CLKCMU_HSI_NOC                   0x1050
#define CMU_TOP_CLK_CON_MUX_CLKCMU_HSI_UFS_EMBD              0x1054
#define CMU_TOP_CLK_CON_DIV_CLKCMU_HSI_NOC                   0x1850
#define CMU_TOP_CLK_CON_DIV_CLKCMU_HSI_UFS_EMBD              0x1854
#define CMU_TOP_CLK_CON_MUX_CLKCMU_USB_NOC                   0x1088
#define CMU_TOP_CLK_CON_MUX_CLKCMU_USB_USB20DRD              0x108c
#define CMU_TOP_CLK_CON_DIV_CLKCMU_USB_NOC                   0x1888
#define CMU_TOP_CLK_CON_DIV_CLKCMU_USB_USB20DRD              0x188c

static const unsigned long top_clk_regs[] __initconst = {
	PLL_LOCKTIME_PLL_MMC,
	PLL_LOCKTIME_PLL_SHARED0,
	PLL_LOCKTIME_PLL_SHARED1,
	PLL_LOCKTIME_PLL_SHARED2,
	PLL_LOCKTIME_PLL_SHARED3,
	PLL_LOCKTIME_PLL_SHARED4,
	PLL_CON0_PLL_MMC,
	PLL_CON3_PLL_MMC,
	PLL_CON0_PLL_SHARED0,
	PLL_CON3_PLL_SHARED0,
	PLL_CON0_PLL_SHARED1,
	PLL_CON3_PLL_SHARED1,
	PLL_CON0_PLL_SHARED2,
	PLL_CON3_PLL_SHARED2,
	PLL_CON0_PLL_SHARED3,
	PLL_CON3_PLL_SHARED3,
	PLL_CON0_PLL_SHARED4,
	PLL_CON3_PLL_SHARED4,
	CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_NOC,
	CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_MMC_CARD,
	CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_IP,
	CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_NOC,
	CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_MMC_CARD,
	CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_IP,
	CMU_TOP_CLK_CON_MUX_CLKCMU_PERIS_GIC,
	CMU_TOP_CLK_CON_MUX_CLKCMU_PERIS_NOC,
	CMU_TOP_CLK_CON_DIV_CLKCMU_PERIS_GIC,
	CMU_TOP_CLK_CON_DIV_CLKCMU_PERIS_NOC,
	CMU_TOP_CLK_CON_MUX_CLKCMU_HSI_NOC,
	CMU_TOP_CLK_CON_MUX_CLKCMU_HSI_UFS_EMBD,
	CMU_TOP_CLK_CON_DIV_CLKCMU_HSI_NOC,
	CMU_TOP_CLK_CON_DIV_CLKCMU_HSI_UFS_EMBD,
	CMU_TOP_CLK_CON_MUX_CLKCMU_USB_NOC,
	CMU_TOP_CLK_CON_MUX_CLKCMU_USB_USB20DRD,
	CMU_TOP_CLK_CON_DIV_CLKCMU_USB_NOC,
	CMU_TOP_CLK_CON_DIV_CLKCMU_USB_USB20DRD,
};

static const struct samsung_pll_clock top_pll_clks[] __initconst = {
	PLL(pll_4313, FOUT_SHARED0_PLL, "fout_shared0_pll", "oscclk",
	    PLL_LOCKTIME_PLL_SHARED0, PLL_CON3_PLL_SHARED0, NULL),
	PLL(pll_4313, FOUT_SHARED1_PLL, "fout_shared1_pll", "oscclk",
	    PLL_LOCKTIME_PLL_SHARED1, PLL_CON3_PLL_SHARED1, NULL),
	PLL(pll_4313, FOUT_SHARED2_PLL, "fout_shared2_pll", "oscclk",
	    PLL_LOCKTIME_PLL_SHARED2, PLL_CON3_PLL_SHARED2, NULL),
	PLL(pll_4313, FOUT_SHARED3_PLL, "fout_shared3_pll", "oscclk",
	    PLL_LOCKTIME_PLL_SHARED3, PLL_CON3_PLL_SHARED3, NULL),
	PLL(pll_4313, FOUT_SHARED4_PLL, "fout_shared4_pll", "oscclk",
	    PLL_LOCKTIME_PLL_SHARED4, PLL_CON3_PLL_SHARED4, NULL),
	PLL(pll_4313, FOUT_MMC_PLL, "fout_mmc_pll", "oscclk",
	    PLL_LOCKTIME_PLL_MMC, PLL_CON3_PLL_MMC, NULL),
};

/* List of parent clocks for Muxes in CMU_TOP */
PNAME(mout_shared0_pll_p) = { "oscclk", "fout_shared0_pll" };
PNAME(mout_shared1_pll_p) = { "oscclk", "fout_shared1_pll" };
PNAME(mout_shared2_pll_p) = { "oscclk", "fout_shared2_pll" };
PNAME(mout_shared3_pll_p) = { "oscclk", "fout_shared3_pll" };
PNAME(mout_shared4_pll_p) = { "oscclk", "fout_shared4_pll" };
PNAME(mout_mmc_pll_p) = { "oscclk", "fout_mmc_pll" };

PNAME(mout_clkcmu_peric_noc_p)            = { "dout_shared0_div2", "dout_shared0_div3",
						"dout_shared1_div2", "dout_shared1_div3",
						"dout_shared2_div2", "dout_shared2_div3",
						"fout_mmc_pll_div2", "dout_shared3_div3"};
PNAME(mout_clkcmu_peric_mmc_card_p)       = { "oscclk", "dout_shared0_div2",
						"fout_mmc_pll_div1", "dout_shared1_div2",
						"dout_shared2_div2", "dout_shared2_div3",
						"dout_shared3_div2", "dout_shared3_div3"};
PNAME(mout_clkcmu_peric_ip_p)            = { "dout_shared0_div4", "dout_shared1_div4"};
PNAME(mout_clkcmu_peris_gic_p)            = { "dout_shared0_div2", "dout_shared0_div3",
						"dout_shared1_div2", "dout_shared1_div3",
						"dout_shared1_div4", "dout_shared2_div2",
						"fout_mmc_pll_clkout_div2", "dout_shared3_div3"};
PNAME(mout_clkcmu_peris_noc_p)            = { "dout_shared0_div2", "dout_shared0_div3",
						"dout_shared1_div2", "dout_shared1_div3",
						"dout_shared1_div4", "dout_shared2_div2",
						"fout_mmc_pll_div2", "dout_shared3_div3"};
PNAME(mout_clkcmu_hsi_noc_p)            = { "dout_shared0_div2", "dout_shared0_div3",
						"dout_shared1_div2", "dout_shared1_div3",
						"dout_shared2_div2", "dout_shared2_div3",
						"dout_shared3_div2", "dout_shared3_div3"};
PNAME(mout_clkcmu_hsi_ufs_embd_p)            = { "dout_shared0_div2", "dout_shared0_div3",
						"dout_shared1_div2", "dout_shared1_div3",
						"dout_shared2_div2", "dout_shared2_div3",
						"dout_shared3_div2", "dout_shared3_div3"};
PNAME(mout_clkcmu_usb_noc_p)            = { "dout_shared0_div2", "dout_shared0_div3",
						"dout_shared0_div4", "dout_shared1_div3",
						"dout_shared2_div2", "dout_shared2_div3",
						"dout_shared3_div2", "dout_shared3_div3"};
PNAME(mout_clkcmu_usb_usb20drd_p)            = { "dout_shared0_div2", "dout_shared0_div3",
						"dout_shared0_div4", "dout_shared1_div3",
						"dout_shared2_div2", "dout_shared2_div3",
						"dout_shared3_div2", "dout_shared3_div3"};

static const struct samsung_mux_clock top_mux_clks[] __initconst = {
	MUX(MOUT_SHARED0_PLL, "mout_shared0_pll", mout_shared0_pll_p,
	    PLL_CON0_PLL_SHARED0, 4, 1),
	MUX(MOUT_SHARED1_PLL, "mout_shared1_pll", mout_shared1_pll_p,
	    PLL_CON0_PLL_SHARED1, 4, 1),
	MUX(MOUT_SHARED2_PLL, "mout_shared2_pll", mout_shared2_pll_p,
	    PLL_CON0_PLL_SHARED2, 4, 1),
	MUX(MOUT_SHARED3_PLL, "mout_shared3_pll", mout_shared3_pll_p,
	    PLL_CON0_PLL_SHARED3, 4, 1),
	MUX(MOUT_SHARED4_PLL, "mout_shared4_pll", mout_shared4_pll_p,
	    PLL_CON0_PLL_SHARED4, 4, 1),
	MUX(MOUT_MMC_PLL, "mout_mmc_pll", mout_mmc_pll_p,
	    PLL_CON0_PLL_MMC, 4, 1),
	MUX(CLKCMU_MOUT_PERIC_NOC, "mout_clkcmu_peric_noc",
	    mout_clkcmu_peric_noc_p, CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_NOC, 0, 3),
	MUX(CLKCMU_MOUT_PERIC_MMC_CARD, "mout_clkcmu_peric_mmc_card",
	    mout_clkcmu_peric_mmc_card_p, CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_MMC_CARD, 0, 3),
	MUX(CLKCMU_MOUT_PERIC_IP, "mout_clkcmu_peric_ip",
	    mout_clkcmu_peric_ip_p, CMU_TOP_CLK_CON_MUX_CLKCMU_PERIC_IP, 0, 1),
	MUX(CLKCMU_MOUT_PERIS_GIC, "mout_clkcmu_peris_gic",
	    mout_clkcmu_peris_gic_p, CMU_TOP_CLK_CON_MUX_CLKCMU_PERIS_GIC, 0, 3),
	MUX(CLKCMU_MOUT_PERIS_NOC, "mout_clkcmu_peris_noc",
	    mout_clkcmu_peris_noc_p, CMU_TOP_CLK_CON_MUX_CLKCMU_PERIS_NOC, 0, 3),
	MUX(CLKCMU_MOUT_HSI_NOC, "mout_clkcmu_hsi_noc",
	    mout_clkcmu_hsi_noc_p, CMU_TOP_CLK_CON_MUX_CLKCMU_HSI_NOC, 0, 3),
	MUX(CLKCMU_MOUT_HSI_UFS_EMBD, "mout_clkcmu_hsi_ufs_embd",
	    mout_clkcmu_hsi_ufs_embd_p, CMU_TOP_CLK_CON_MUX_CLKCMU_HSI_UFS_EMBD, 0, 3),
	MUX(CLKCMU_MOUT_USB_NOC, "mout_clkcmu_usb_noc",
	    mout_clkcmu_usb_noc_p, CMU_TOP_CLK_CON_MUX_CLKCMU_USB_NOC, 0, 3),
	MUX(CLKCMU_MOUT_USB_USB20DRD, "mout_clkcmu_usb_usb20drd",
	    mout_clkcmu_usb_usb20drd_p, CMU_TOP_CLK_CON_MUX_CLKCMU_USB_USB20DRD, 0, 3),
};

static const struct samsung_div_clock top_div_clks[] __initconst = {
	DIV(CLKCMU_DOUT_PERIC_NOC, "dout_clkcmu_peric_noc",
	    "mout_clkcmu_peric_noc", CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_NOC,
	    0, 4),
	DIV(CLKCMU_DOUT_PERIC_MMC_CARD, "dout_clkcmu_peric_mmc_card",
	    "mout_clkcmu_peric_mmc_card", CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_MMC_CARD,
	    0, 10),
	DIV(CLKCMU_DOUT_PERIC_IP, "dout_clkcmu_peric_ip",
	    "mout_clkcmu_peric_ip", CMU_TOP_CLK_CON_DIV_CLKCMU_PERIC_IP,
	    0, 4),
	DIV(CLKCMU_DOUT_PERIS_GIC, "dout_clkcmu_peris_gic",
	    "mout_clkcmu_peris_gic", CMU_TOP_CLK_CON_DIV_CLKCMU_PERIS_GIC,
	    0, 4),
	DIV(CLKCMU_DOUT_PERIS_NOC, "dout_clkcmu_peris_noc",
	    "mout_clkcmu_peris_noc", CMU_TOP_CLK_CON_DIV_CLKCMU_PERIS_NOC,
	    0, 4),
	DIV(CLKCMU_DOUT_HSI_NOC, "dout_clkcmu_hsi_noc",
	    "mout_clkcmu_hsi_noc", CMU_TOP_CLK_CON_DIV_CLKCMU_HSI_NOC,
	    0, 4),
	DIV(CLKCMU_DOUT_HSI_UFS_EMBD, "dout_clkcmu_hsi_ufs_embd",
	    "mout_clkcmu_hsi_ufs_embd", CMU_TOP_CLK_CON_DIV_CLKCMU_HSI_UFS_EMBD,
	    0, 4),
	DIV(CLKCMU_DOUT_USB_NOC, "dout_clkcmu_usb_noc",
	    "mout_clkcmu_usb_noc", CMU_TOP_CLK_CON_DIV_CLKCMU_USB_NOC,
	    0, 4),
	DIV(CLKCMU_DOUT_USB_USB20DRD, "dout_clkcmu_usb_usb20drd",
	    "mout_clkcmu_usb_usb20drd", CMU_TOP_CLK_CON_DIV_CLKCMU_USB_USB20DRD,
	    0, 5),
};

static const struct samsung_fixed_factor_clock top_fixed_factor_clks[] __initconst = {
	FFACTOR(DOUT_SHARED0_DIV1, "dout_shared0_div1",
		"mout_shared0_pll", 1, 1, 0),
	FFACTOR(DOUT_SHARED0_DIV2, "dout_shared0_div2",
		"mout_shared0_pll", 1, 2, 0),
	FFACTOR(DOUT_SHARED0_DIV3, "dout_shared0_div3",
		"mout_shared0_pll", 1, 3, 0),
	FFACTOR(DOUT_SHARED0_DIV4, "dout_shared0_div4",
		"mout_shared0_pll", 1, 4, 0),
	FFACTOR(DOUT_SHARED1_DIV1, "dout_shared1_div1",
		"mout_shared1_pll", 1, 1, 0),
	FFACTOR(DOUT_SHARED1_DIV2, "dout_shared1_div2",
		"mout_shared1_pll", 1, 2, 0),
	FFACTOR(DOUT_SHARED1_DIV3, "dout_shared1_div3",
		"mout_shared1_pll", 1, 3, 0),
	FFACTOR(DOUT_SHARED1_DIV4, "dout_shared1_div4",
		"mout_shared1_pll", 1, 4, 0),
	FFACTOR(DOUT_SHARED2_DIV1, "dout_shared2_div1",
		"mout_shared2_pll", 1, 1, 0),
	FFACTOR(DOUT_SHARED2_DIV2, "dout_shared2_div2",
		"mout_shared2_pll", 1, 2, 0),
	FFACTOR(DOUT_SHARED2_DIV3, "dout_shared2_div3",
		"mout_shared2_pll", 1, 3, 0),
	FFACTOR(DOUT_SHARED2_DIV4, "dout_shared2_div4",
		"mout_shared2_pll", 1, 4, 0),
	FFACTOR(DOUT_SHARED3_DIV1, "dout_shared3_div1",
		"mout_shared3_pll", 1, 1, 0),
	FFACTOR(DOUT_SHARED3_DIV2, "dout_shared3_div2",
		"mout_shared3_pll", 1, 2, 0),
	FFACTOR(DOUT_SHARED3_DIV3, "dout_shared3_div3",
		"mout_shared3_pll", 1, 3, 0),
	FFACTOR(DOUT_SHARED3_DIV4, "dout_shared3_div4",
		"mout_shared3_pll", 1, 4, 0),
	FFACTOR(DOUT_SHARED4_DIV1, "dout_shared4_div1",
		"mout_shared4_pll", 1, 1, 0),
	FFACTOR(DOUT_SHARED4_DIV2, "dout_shared4_div2",
		"mout_shared4_pll", 1, 2, 0),
	FFACTOR(DOUT_SHARED4_DIV3, "dout_shared4_div3",
		"mout_shared4_pll", 1, 3, 0),
	FFACTOR(DOUT_SHARED4_DIV4, "dout_shared4_div4",
		"mout_shared4_pll", 1, 4, 0),
	FFACTOR(FOUT_MMC_PLL_DIV1, "fout_mmc_pll_div1",
		"mout_mmc_pll", 1, 1, 0),
	FFACTOR(FOUT_MMC_PLL_DIV2, "fout_mmc_pll_div2",
		"mout_mmc_pll", 1, 2, 0),
	FFACTOR(FOUT_MMC_PLL_CLKOUT_DIV2, "fout_mmc_pll_clkout_div2",
		"mout_mmc_pll", 1, 2, 0),
};

static const struct samsung_fixed_rate_clock top_fixed_rate_clks[] __initconst = {
	FRATE(0, "oscclk_usb_link", NULL, 0, 19200000),
};

static const struct samsung_cmu_info top_cmu_info __initconst = {
	.pll_clks		= top_pll_clks,
	.nr_pll_clks		= ARRAY_SIZE(top_pll_clks),
	.mux_clks               = top_mux_clks,
	.nr_mux_clks            = ARRAY_SIZE(top_mux_clks),
	.div_clks               = top_div_clks,
	.nr_div_clks            = ARRAY_SIZE(top_div_clks),
	.fixed_factor_clks	= top_fixed_factor_clks,
	.nr_fixed_factor_clks	= ARRAY_SIZE(top_fixed_factor_clks),
	.fixed_clks             = top_fixed_rate_clks,
	.nr_fixed_clks          = ARRAY_SIZE(top_fixed_rate_clks),
	.nr_clk_ids             = CLKS_NR_TOP,
	.clk_regs               = top_clk_regs,
	.nr_clk_regs            = ARRAY_SIZE(top_clk_regs),
};

static void __init exynos8855_cmu_top_init(struct device_node *np)
{
	exynos_arm64_register_cmu(NULL, np, &top_cmu_info);
}

/* Register CMU_TOP early, as it's a dependency for other early domains */
CLK_OF_DECLARE(exynos8855_cmu_top, "samsung,exynos8855-cmu-top",
	       exynos8855_cmu_top_init);

/* ---- CMU_PERIC --------------------------------------------------------- */

/* Register Offset definitions for CMU_PERIC (0x15400000) */
#define CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_IP_USER            0x600
#define CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_MMC_CARD_USER      0x610
#define CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_NOC_USER           0x620
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_I2C                    0x1000
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_UART_DBG               0x1004
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI00                  0x1008
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI01                  0x100c
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI02                  0x1010
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI03                  0x1014
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI04                  0x1018
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI09_USI_OIS          0x101c
#define CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI10_USI_OIS          0x1020
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_NOCP                   0x1800
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_UART_DBG               0x1804
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI00_USI              0x1808
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI01_USI              0x180c
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI02_USI              0x1810
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI03_USI              0x1814
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI04_USI              0x1818
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI09_USI_OIS          0x181c
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI10_USI_OIS          0x1820
#define CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI_I2C                0x1824
#define CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_CMU_PERIC_IPCLKPORT_PCLK    0x2004
#define CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_SYSREG_PERIC_IPCLKPORT_PCLK 0x2060
#define CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_UART_DBG_IPCLKPORT_PCLK     0x2064
#define CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_USI00_USI_IPCLKPORT_PCLK    0x206c

static const unsigned long peric_clk_regs[] __initconst = {
	CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_IP_USER,
	CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_MMC_CARD_USER,
	CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_NOC_USER,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_I2C,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_UART_DBG,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI00,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI01,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI02,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI03,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI04,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI09_USI_OIS,
	CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI10_USI_OIS,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_NOCP,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_UART_DBG,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI00_USI,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI01_USI,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI02_USI,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI03_USI,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI04_USI,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI09_USI_OIS,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI10_USI_OIS,
	CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI_I2C,
	CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_CMU_PERIC_IPCLKPORT_PCLK,
	CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_SYSREG_PERIC_IPCLKPORT_PCLK,
	CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_UART_DBG_IPCLKPORT_PCLK,
	CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_USI00_USI_IPCLKPORT_PCLK,
};

/* List of parent clocks for Muxes in CMU_PERIC */
PNAME(mout_clkcmu_peric_ip_user_p) = { "oscclk", "dout_clkcmu_peric_ip" };
PNAME(mout_clkcmu_peric_mmc_card_user_p) = { "oscclk", "dout_clkcmu_peric_mmc_card" };
PNAME(mout_clkcmu_peric_noc_user_p) = { "oscclk", "dout_clkcmu_peric_noc" };
PNAME(mout_clkcmu_peric_i2c_p) = { "oscclk", "mout_clkcmu_peric_ip_user" };
PNAME(mout_clkcmu_peric_uart_dbg_p) = { "oscclk", "mout_clkcmu_peric_ip_user" };
PNAME(mout_clkcmu_peric_usi0n_p) = { "oscclk", "mout_clkcmu_peric_ip_user" };
PNAME(mout_clkcmu_peric_usi09_usi_ois_p) = { "oscclk", "mout_clkcmu_peric_ip_user" };
PNAME(mout_clkcmu_peric_usi10_usi_ois_p) = { "oscclk", "mout_clkcmu_peric_ip_user" };

static const struct samsung_mux_clock peric_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PERIC_IP_USER, "mout_clkcmu_peric_ip_user",
	    mout_clkcmu_peric_ip_user_p, CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_IP_USER, 4, 1),
	MUX(CLK_MOUT_PERIC_MMC_CARD_USER, "mout_clkcmu_peric_mmc_card_user",
	    mout_clkcmu_peric_mmc_card_user_p, CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_MMC_CARD_USER,
		4, 1),
	MUX(CLK_MOUT_PERIC_NOC_USER, "mout_clkcmu_peric_noc_user",
	    mout_clkcmu_peric_noc_user_p, CMU_PERIC_PLL_CON0_MUX_CLKCMU_PERIC_NOC_USER, 4, 1),
	MUX(CLK_MOUT_PERIC_I2C, "mout_clkcmu_peric_i2c",
	    mout_clkcmu_peric_i2c_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_I2C, 0, 1),
	MUX(CLK_MOUT_PERIC_UART_DBG, "mout_clkcmu_peric_uart_dbg",
	    mout_clkcmu_peric_uart_dbg_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_UART_DBG, 0, 1),
	MUX(CLK_MOUT_PERIC_USI00, "mout_clkcmu_peric_usi00",
	    mout_clkcmu_peric_usi0n_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI00, 0, 1),
	MUX(CLK_MOUT_PERIC_USI01, "mout_clkcmu_peric_usi01",
	    mout_clkcmu_peric_usi0n_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI01, 0, 1),
	MUX(CLK_MOUT_PERIC_USI02, "mout_clkcmu_peric_usi02",
	    mout_clkcmu_peric_usi0n_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI02, 0, 1),
	MUX(CLK_MOUT_PERIC_USI03, "mout_clkcmu_peric_usi03",
	    mout_clkcmu_peric_usi0n_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI03, 0, 1),
	MUX(CLK_MOUT_PERIC_USI04, "mout_clkcmu_peric_usi04",
	    mout_clkcmu_peric_usi0n_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI04, 0, 1),
	MUX(CLK_MOUT_PERIC_USI09_USI_OIS, "mout_clkcmu_peric_usi09_usi_ois",
	    mout_clkcmu_peric_usi09_usi_ois_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI09_USI_OIS, 0, 1),
	MUX(CLK_MOUT_PERIC_USI10_USI_OIS, "mout_clkcmu_peric_usi10_usi_ois",
	    mout_clkcmu_peric_usi10_usi_ois_p, CMU_PERIC_CLK_CON_MUX_CLK_PERIC_USI10_USI_OIS, 0, 1),
};

static const struct samsung_div_clock peric_div_clks[] __initconst = {
	DIV(CLK_DOUT_PERIC_NOCP, "dout_clkcmu_peric_nocp",
	    "mout_clkcmu_peric_noc_user", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_NOCP,
	    0, 4),
	DIV(CLK_DOUT_PERIC_UART_DBG, "dout_clkcmu_peric_uart_dbg",
	    "mout_clkcmu_peric_uart_dbg", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_UART_DBG,
	    0, 4),
	DIV(CLK_DOUT_PERIC_USI00_USI, "dout_clkcmu_peric_usi00_usi",
	    "mout_clkcmu_peric_usi00", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI00_USI,
	    0, 4),
	DIV(CLK_DOUT_PERIC_USI01_USI, "dout_clkcmu_peric_usi01_usi",
	    "mout_clkcmu_peric_usi01", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI01_USI,
	    0, 4),
	DIV(CLK_DOUT_PERIC_USI02_USI, "dout_clkcmu_peric_usi02_usi",
	    "mout_clkcmu_peric_usi02", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI02_USI,
	    0, 4),
	DIV(CLK_DOUT_PERIC_USI03_USI, "dout_clkcmu_peric_usi03_usi",
	    "mout_clkcmu_peric_usi03", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI03_USI,
	    0, 4),
	DIV(CLK_DOUT_PERIC_USI04_USI, "dout_clkcmu_peric_usi04_usi",
	    "mout_clkcmu_peric_usi04", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI04_USI,
	    0, 4),
	DIV(CLK_DOUT_PERIC_USI09_USI_OIS, "dout_clkcmu_peric_usi09_usi_ois",
	    "mout_clkcmu_peric_usi09_usi_ois", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI09_USI_OIS,
	    0, 4),
	DIV(CLK_DOUT_PERIC_USI10_USI_OIS, "dout_clkcmu_peric_usi10_usi_ois",
	    "mout_clkcmu_peric_usi10_usi_ois", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI10_USI_OIS,
	    0, 4),
	DIV(CLK_DOUT_PERIC_USI_I2C, "dout_clkcmu_peric_usi_i2c",
	    "mout_clkcmu_peric_i2c", CMU_PERIC_CLK_CON_DIV_CLK_PERIC_USI_I2C,
	    0, 4),
};

static const struct samsung_gate_clock peric_gate_clks[] __initconst = {
	/* System will hang if this critical clock is gated */
	GATE(CLK_GOUT_CMU_PERIC_IPCLKPORT_PCLK, "gout_cmu_peric_ipclkport_pclk",
	     "dout_clkcmu_peric_nocp",
	     CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_CMU_PERIC_IPCLKPORT_PCLK,
	     21, CLK_IS_CRITICAL, 0),
	GATE(CLK_GOUT_SYSREG_PERIC_IPCLKPORT_PCLK, "gout_sysreg_peric_ipclkport_pclk",
	     "dout_clkcmu_peric_nocp",
	     CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_SYSREG_PERIC_IPCLKPORT_PCLK,
	     21, 0, 0),
	/* System will hang if this critical clock is gated */
	GATE(CLK_GOUT_UART_DBG_PERIC_IPCLKPORT_PCLK, "gout_uart_dbg_peric_ipclkport_pclk",
	     "dout_clkcmu_peric_uart_dbg",
	     CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_UART_DBG_IPCLKPORT_PCLK,
	     21, CLK_IS_CRITICAL, 0),
	GATE(CLK_GOUT_USI_PERIC_IPCLKPORT_PCLK, "gout_usi_peric_ipclkport_pclk",
	     "dout_clkcmu_peric_nocp",
	     CMU_PERIC_CLK_CON_GAT_BLK_PERIC_UID_USI00_USI_IPCLKPORT_PCLK,
	     21, 0, 0),
};

static const struct samsung_cmu_info peric_cmu_info __initconst = {
	.mux_clks               = peric_mux_clks,
	.nr_mux_clks            = ARRAY_SIZE(peric_mux_clks),
	.div_clks               = peric_div_clks,
	.nr_div_clks            = ARRAY_SIZE(peric_div_clks),
	.gate_clks              = peric_gate_clks,
	.nr_gate_clks           = ARRAY_SIZE(peric_gate_clks),
	.nr_clk_ids             = CLKS_NR_PERIC,
	.clk_regs               = peric_clk_regs,
	.nr_clk_regs            = ARRAY_SIZE(peric_clk_regs),
	.clk_name               = "bus",
};

/* ---- CMU_PERIS --------------------------------------------------------- */

/* Register Offset definitions for CMU_PERIS (0x10030000) */
#define CMU_PERIS_PLL_CON0_MUX_CLKCMU_PERIS_NOC_USER           0x600
#define CMU_PERIS_CLK_CON_MUX_CLK_PERIS_GIC                    0x1000
#define CMU_PERIS_PLL_CON0_MUX_CLK_PERIS_GIC_USER              0x610
#define CMU_PERIS_CLK_CON_DIV_CLK_PERIS_NOCP                   0x1800
#define CMU_PERIS_CLK_CON_DIV_CLK_PERIS_OTP                    0x1804
#define CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_CMU_PERIS_IPCLKPORT_PCLK     0x201c
#define CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_SYSREG_PERIS_IPCLKPORT_PCLK  0x2040
#define CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_WDT0_IPCLKPORT_PCLK          0x2048
#define CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_WDT1_IPCLKPORT_PCLK          0x204c

static const unsigned long peris_clk_regs[] __initconst = {
	CMU_PERIS_PLL_CON0_MUX_CLKCMU_PERIS_NOC_USER,
	CMU_PERIS_CLK_CON_MUX_CLK_PERIS_GIC,
	CMU_PERIS_PLL_CON0_MUX_CLK_PERIS_GIC_USER,
	CMU_PERIS_CLK_CON_DIV_CLK_PERIS_NOCP,
	CMU_PERIS_CLK_CON_DIV_CLK_PERIS_OTP,
	CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_CMU_PERIS_IPCLKPORT_PCLK,
	CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_SYSREG_PERIS_IPCLKPORT_PCLK,
	CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_WDT0_IPCLKPORT_PCLK,
	CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_WDT1_IPCLKPORT_PCLK,
};

/* List of parent clocks for Muxes in CMU_PERIS */
PNAME(mout_clkcmu_peris_noc_user_p) = { "oscclk", "dout_clkcmu_peris_noc" };
PNAME(mout_clk_peris_gic_p) = { "oscclk", "mout_clkcmu_peris_gic_user" };
PNAME(mout_clkcmu_peris_gic_user_p) = { "oscclk", "dout_clkcmu_peris_gic" };

static const struct samsung_mux_clock peris_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PERIS_NOC_USER, "mout_clkcmu_peris_noc_user",
	    mout_clkcmu_peris_noc_user_p, CMU_PERIS_PLL_CON0_MUX_CLKCMU_PERIS_NOC_USER, 4, 1),
	MUX(CLK_MOUT_PERIS_GIC, "mout_clk_peris_gic",
	    mout_clk_peris_gic_p, CMU_PERIS_CLK_CON_MUX_CLK_PERIS_GIC, 0, 1),
	MUX(CLK_MOUT_PERIS_GIC_USER, "mout_clkcmu_peris_gic_user",
	    mout_clkcmu_peris_gic_user_p, CMU_PERIS_PLL_CON0_MUX_CLK_PERIS_GIC_USER, 4, 1),
};

static const struct samsung_div_clock peris_div_clks[] __initconst = {
	DIV(CLK_DOUT_PERIS_NOCP, "dout_clkcmu_peris_nocp",
	    "mout_clkcmu_peris_noc_user", CMU_PERIS_CLK_CON_DIV_CLK_PERIS_NOCP,
	    0, 4),
	DIV(CLK_DOUT_PERIS_OTP, "dout_clkcmu_peris_otp",
	    "oscclk", CMU_PERIS_CLK_CON_DIV_CLK_PERIS_OTP,
	    0, 4),
};

static const struct samsung_gate_clock peris_gate_clks[] __initconst = {
	/* System will hang if this critical clock is gated */
	GATE(CLK_GOUT_CMU_PERIS_IPCLKPORT_PCLK, "gout_cmu_peris_ipclkport_pclk",
	     "dout_clkcmu_peris_nocp",
	     CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_CMU_PERIS_IPCLKPORT_PCLK,
	     21, CLK_IS_CRITICAL, 0),
	GATE(CLK_GOUT_SYSREG_PERIS_IPCLKPORT_PCLK, "gout_sysreg_peris_ipclkport_pclk",
	     "dout_clkcmu_peris_nocp",
	     CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_SYSREG_PERIS_IPCLKPORT_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_WDT0_PERIS_IPCLKPORT_PCLK, "gout_wdt0_peris_ipclkport_pclk",
	     "dout_clkcmu_peris_nocp",
	     CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_WDT0_IPCLKPORT_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_WDT1_PERIS_IPCLKPORT_PCLK, "gout_wdt1_peris_ipclkport_pclk",
	     "dout_clkcmu_peris_nocp",
	     CMU_PERIS_CLK_CON_GAT_BLK_PERIS_UID_WDT1_IPCLKPORT_PCLK,
	     21, 0, 0),
};

static const struct samsung_cmu_info peris_cmu_info __initconst = {
	.mux_clks               = peris_mux_clks,
	.nr_mux_clks            = ARRAY_SIZE(peris_mux_clks),
	.div_clks               = peris_div_clks,
	.nr_div_clks            = ARRAY_SIZE(peris_div_clks),
	.gate_clks              = peris_gate_clks,
	.nr_gate_clks           = ARRAY_SIZE(peris_gate_clks),
	.nr_clk_ids             = CLKS_NR_PERIS,
	.clk_regs               = peris_clk_regs,
	.nr_clk_regs            = ARRAY_SIZE(peris_clk_regs),
	.clk_name               = "bus",
};

/* ---- CMU_HSI --------------------------------------------------------- */

/* Register Offset definitions for CMU_HSI (0x17000000) */
#define CMU_HSI_PLL_CON0_MUX_CLKCMU_HSI_NOC_USER               0x600
#define CMU_HSI_PLL_CON0_MUX_CLKCMU_HSI_UFS_EMBD_USER          0x610
#define CMU_HSI_CLK_CON_GAT_BLK_HSI_UID_CMU_HSI_IPCLKPORT_PCLK        0x2000
#define CMU_HSI_CLK_CON_GAT_BLK_HSI_UID_SYSREG_HSI_IPCLKPORT_PCLK     0x2038

static const unsigned long hsi_clk_regs[] __initconst = {
	CMU_HSI_PLL_CON0_MUX_CLKCMU_HSI_NOC_USER,
	CMU_HSI_PLL_CON0_MUX_CLKCMU_HSI_UFS_EMBD_USER,
	CMU_HSI_CLK_CON_GAT_BLK_HSI_UID_CMU_HSI_IPCLKPORT_PCLK,
	CMU_HSI_CLK_CON_GAT_BLK_HSI_UID_SYSREG_HSI_IPCLKPORT_PCLK,
};

/* List of parent clocks for Muxes in CMU_HSI */
PNAME(mout_clkcmu_hsi_noc_user_p) = { "oscclk", "dout_clkcmu_hsi_noc" };
PNAME(mout_clkcmu_hsi_ufs_embd_user_p) = { "oscclk", "dout_clkcmu_hsi_ufs_embd" };

static const struct samsung_mux_clock hsi_mux_clks[] __initconst = {
	MUX(CLK_MOUT_HSI_NOC_USER, "mout_clkcmu_hsi_noc_user",
	    mout_clkcmu_hsi_noc_user_p, CMU_HSI_PLL_CON0_MUX_CLKCMU_HSI_NOC_USER, 4, 1),
	MUX(CLK_MOUT_HSI_UFS_EMBD_USER, "mout_clkcmu_hsi_ufs_embd_user",
	    mout_clkcmu_hsi_ufs_embd_user_p, CMU_HSI_PLL_CON0_MUX_CLKCMU_HSI_UFS_EMBD_USER, 4, 1),
};

static const struct samsung_gate_clock hsi_gate_clks[] __initconst = {
	/* System will hang if this critical clock is gated */
	GATE(CLK_GOUT_CMU_HSI_IPCLKPORT_PCLK, "gout_cmu_hsi_ipclkport_pclk",
	     "mout_clkcmu_hsi_noc_user",
	     CMU_HSI_CLK_CON_GAT_BLK_HSI_UID_CMU_HSI_IPCLKPORT_PCLK,
	     21, CLK_IS_CRITICAL, 0),
	GATE(CLK_GOUT_SYSREG_HSI_IPCLKPORT_PCLK, "gout_sysreg_hsi_ipclkport_pclk",
	     "mout_clkcmu_hsi_noc_user",
	     CMU_HSI_CLK_CON_GAT_BLK_HSI_UID_SYSREG_HSI_IPCLKPORT_PCLK,
	     21, 0, 0),
};

static const struct samsung_cmu_info hsi_cmu_info __initconst = {
	.mux_clks               = hsi_mux_clks,
	.nr_mux_clks            = ARRAY_SIZE(hsi_mux_clks),
	.gate_clks              = hsi_gate_clks,
	.nr_gate_clks           = ARRAY_SIZE(hsi_gate_clks),
	.nr_clk_ids             = CLKS_NR_HSI,
	.clk_regs               = hsi_clk_regs,
	.nr_clk_regs            = ARRAY_SIZE(hsi_clk_regs),
	.clk_name               = "bus",
};

/* ---- CMU_USB --------------------------------------------------------- */

/* Register Offset definitions for CMU_USB (0x13000000) */
#define CMU_USB_PLL_CON0_MUX_CLKCMU_USB_NOC_USER                   0x600
#define CMU_USB_PLL_CON0_MUX_CLKCMU_USB_USB20DRD_USER              0x610
#define CMU_USB_CLK_CON_MUX_CLK_USB_USB20DRD                       0x1000
#define CMU_USB_CLK_CON_DIV_CLK_USB_NOC_DIV3                       0x1800
#define CMU_USB_CLK_CON_GAT_BLK_USB_UID_CMU_USB_IPCLKPORT_PCLK     0x2000
#define CMU_USB_CLK_CON_GAT_BLK_USB_UID_SYSREG_USB_IPCLKPORT_PCLK  0x2030

static const unsigned long usb_clk_regs[] __initconst = {
	CMU_USB_PLL_CON0_MUX_CLKCMU_USB_NOC_USER,
	CMU_USB_PLL_CON0_MUX_CLKCMU_USB_USB20DRD_USER,
	CMU_USB_CLK_CON_MUX_CLK_USB_USB20DRD,
	CMU_USB_CLK_CON_DIV_CLK_USB_NOC_DIV3,
	CMU_USB_CLK_CON_GAT_BLK_USB_UID_CMU_USB_IPCLKPORT_PCLK,
	CMU_USB_CLK_CON_GAT_BLK_USB_UID_SYSREG_USB_IPCLKPORT_PCLK,
};

/* List of parent clocks for Muxes in CMU_USB */
PNAME(mout_clkcmu_usb_noc_user_p) = { "oscclk", "dout_clkcmu_usb_noc" };
PNAME(mout_clkcmu_usb_usb20drd_user_p) = { "oscclk", "dout_clkcmu_usb_usb20drd" };
PNAME(mout_clk_usb_usb20drd_p) = { "oscclk_usb_link", "mout_clkcmu_usb_usb20drd_user" };

static const struct samsung_mux_clock usb_mux_clks[] __initconst = {
	MUX(CLK_MOUT_USB_NOC_USER, "mout_clkcmu_usb_noc_user",
	    mout_clkcmu_usb_noc_user_p, CMU_USB_PLL_CON0_MUX_CLKCMU_USB_NOC_USER, 4, 1),
	MUX(CLK_MOUT_USB_USB20DRD_USER, "mout_clkcmu_usb_usb20drd_user",
	    mout_clkcmu_usb_usb20drd_user_p, CMU_USB_PLL_CON0_MUX_CLKCMU_USB_USB20DRD_USER, 4, 1),
	MUX(CLK_MOUT_USB_USB20DRD, "mout_clk_usb_usb20drd",
	    mout_clk_usb_usb20drd_p, CMU_USB_CLK_CON_MUX_CLK_USB_USB20DRD, 0, 1),
};

static const struct samsung_div_clock usb_div_clks[] __initconst = {
	DIV(CLK_DOUT_USB_NOC_DIV3, "dout_clkcmu_usb_noc_div3",
	    "mout_clkcmu_usb_noc_user", CMU_USB_CLK_CON_DIV_CLK_USB_NOC_DIV3,
	    0, 4),
};

static const struct samsung_gate_clock usb_gate_clks[] __initconst = {
	/* System will hang if this critical clock is gated */
	GATE(CLK_GOUT_CMU_USB_IPCLKPORT_PCLK, "gout_cmu_usb_ipclkport_pclk",
	     "mout_clkcmu_usb_noc_user",
	     CMU_USB_CLK_CON_GAT_BLK_USB_UID_CMU_USB_IPCLKPORT_PCLK,
	     21, CLK_IS_CRITICAL, 0),
	GATE(CLK_GOUT_SYSREG_USB_IPCLKPORT_PCLK, "gout_sysreg_usb_ipclkport_pclk",
	     "mout_clkcmu_usb_noc_user",
	     CMU_USB_CLK_CON_GAT_BLK_USB_UID_SYSREG_USB_IPCLKPORT_PCLK,
	     21, 0, 0),
};

static const struct samsung_cmu_info usb_cmu_info __initconst = {
	.mux_clks               = usb_mux_clks,
	.nr_mux_clks            = ARRAY_SIZE(usb_mux_clks),
	.div_clks               = usb_div_clks,
	.nr_div_clks            = ARRAY_SIZE(usb_div_clks),
	.gate_clks              = usb_gate_clks,
	.nr_gate_clks           = ARRAY_SIZE(usb_gate_clks),
	.nr_clk_ids             = CLKS_NR_USB,
	.clk_regs               = usb_clk_regs,
	.nr_clk_regs            = ARRAY_SIZE(usb_clk_regs),
	.clk_name               = "bus",
};

static int __init exynos8855_cmu_probe(struct platform_device *pdev)
{
	const struct samsung_cmu_info *info;
	struct device *dev = &pdev->dev;

	info = of_device_get_match_data(dev);
	exynos_arm64_register_cmu(dev, dev->of_node, info);

	return 0;
}

static const struct of_device_id exynos8855_cmu_of_match[] = {
	{
		.compatible = "samsung,exynos8855-cmu-hsi",
		.data = &hsi_cmu_info,
	}, {
		.compatible = "samsung,exynos8855-cmu-peric",
		.data = &peric_cmu_info,
	}, {
		.compatible = "samsung,exynos8855-cmu-peris",
		.data = &peris_cmu_info,
	}, {
		.compatible = "samsung,exynos8855-cmu-top",
		.data = &top_cmu_info,
	}, {
		.compatible = "samsung,exynos8855-cmu-usb",
		.data = &usb_cmu_info,
	},
	{ }
};

static struct platform_driver exynos8855_cmu_driver __refdata = {
	.driver = {
		.name = "exynos8855-cmu",
		.of_match_table = exynos8855_cmu_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = exynos8855_cmu_probe,
};

static int __init exynos8855_cmu_init(void)
{
	return platform_driver_register(&exynos8855_cmu_driver);
}
core_initcall(exynos8855_cmu_init);
