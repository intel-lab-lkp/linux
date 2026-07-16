// SPDX-License-Identifier: GPL-2.0
/*
 * Common Clock Framework driver for Exynos9610
 *
 * Copyright (c) 2026, Alexandru Chimac <alex@chimac.ro>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/samsung,exynos9610-cmu.h>

#include "clk.h"
#include "clk-exynos-arm64.h"
#include "clk-pll.h"

#define CLKS_NR_TOP	(CLK_GOUT_CMU_VIPX2_BUS + 1)
#define CLKS_NR_APM	(CLK_GOUT_APM_XIU_DP_ACLK + 1)
#define CLKS_NR_CAM	(CLK_GOUT_CAM_SYSREG_PCLK + 1)
#define CLKS_NR_CMGP	(CLK_GOUT_CMGP_USI_CMGP04_PCLK + 1)
#define CLKS_NR_CORE	(CLK_GOUT_CORE_XIU_D_ACLK + 1)
#define CLKS_NR_CPUCL0	(CLK_GOUT_CPUCL0_SYSREG_PCLK + 1)
#define CLKS_NR_DISPAUD	(CLK_GOUT_DISPAUD_WDT_AUD_PCLK + 1)
#define CLKS_NR_FSYS	(CLK_GOUT_FSYS_XIU_D_ACLK + 1)
#define CLKS_NR_G2D	(CLK_GOUT_G2D_XIU_D_MSCL_ACLK + 1)
#define CLKS_NR_G3D	(CLK_GOUT_G3D_SYSREG_PCLK + 1)
#define CLKS_NR_PERI	(CLK_GOUT_PERI_WDT_CLUSTER1_PCLK + 1)
#define CLKS_NR_USB	(CLK_GOUT_USB_US_D_ACLK + 1)

#define EXYNOS9610_GATE_DBG_OFFSET    0x4000
#define EXYNOS9610_CAM_DRCG_EN_OFFSET 0x1000
#define EXYNOS9610_DRCG_EN_OFFSET     0x104
#define EXYNOS9610_MEMCLK_OFFSET      0x108

/* TOP (0x12100000) */
#define PLL_LOCKTIME_PLL_MMC				0x0000
#define PLL_LOCKTIME_PLL_SHARED0			0x0004
#define PLL_LOCKTIME_PLL_SHARED1			0x0008
#define PLL_CON0_PLL_MMC				0x0100
#define PLL_CON3_PLL_MMC				0x010c
#define PLL_CON0_PLL_SHARED0				0x0120
#define PLL_CON0_PLL_SHARED1				0x0140
#define CMU_CMU_TOP_CONTROLLER_OPTION			0x0800
#define CLK_CON_MUX_MUX_CLKCMU_APM_BUS			0x1000
#define CLK_CON_MUX_MUX_CLKCMU_CAM_BUS			0x1004
#define CLK_CON_MUX_MUX_CLKCMU_CIS_CLK0			0x1008
#define CLK_CON_MUX_MUX_CLKCMU_CIS_CLK1			0x100c
#define CLK_CON_MUX_MUX_CLKCMU_CIS_CLK2			0x1010
#define CLK_CON_MUX_MUX_CLKCMU_CIS_CLK3			0x1014
#define CLK_CON_MUX_MUX_CLKCMU_CORE_BUS			0x1018
#define CLK_CON_MUX_MUX_CLKCMU_CORE_CCI			0x101c
#define CLK_CON_MUX_MUX_CLKCMU_CORE_G3D			0x1020
#define CLK_CON_MUX_MUX_CLKCMU_CPUCL0_DBG		0x1024
#define CLK_CON_MUX_MUX_CLKCMU_CPUCL0_SWITCH		0x1028
#define CLK_CON_MUX_MUX_CLKCMU_CPUCL1_SWITCH		0x102c
#define CLK_CON_MUX_MUX_CLKCMU_DISPAUD_AUD		0x1030
#define CLK_CON_MUX_MUX_CLKCMU_DISPAUD_CPU		0x1034
#define CLK_CON_MUX_MUX_CLKCMU_DISPAUD_DISP		0x1038
#define CLK_CON_MUX_MUX_CLKCMU_FSYS_BUS			0x103c
#define CLK_CON_MUX_MUX_CLKCMU_FSYS_MMC_CARD		0x1040
#define CLK_CON_MUX_MUX_CLKCMU_FSYS_MMC_EMBD		0x1044
#define CLK_CON_MUX_MUX_CLKCMU_FSYS_UFS_EMBD		0x1048
#define CLK_CON_MUX_MUX_CLKCMU_G2D_G2D			0x104c
#define CLK_CON_MUX_MUX_CLKCMU_G2D_MSCL			0x1050
#define CLK_CON_MUX_MUX_CLKCMU_G3D_SWITCH		0x1054
#define CLK_CON_MUX_MUX_CLKCMU_HPM			0x1058
#define CLK_CON_MUX_MUX_CLKCMU_ISP_BUS			0x105c
#define CLK_CON_MUX_MUX_CLKCMU_ISP_GDC			0x1060
#define CLK_CON_MUX_MUX_CLKCMU_ISP_VRA			0x1064
#define CLK_CON_MUX_MUX_CLKCMU_MFC_MFC			0x1068
#define CLK_CON_MUX_MUX_CLKCMU_MFC_WFD			0x106c
#define CLK_CON_MUX_MUX_CLKCMU_MIF_BUSP			0x1070
#define CLK_CON_MUX_MUX_CLKCMU_MIF_SWITCH		0x1074
#define CLK_CON_MUX_MUX_CLKCMU_PERI_BUS			0x1078
#define CLK_CON_MUX_MUX_CLKCMU_PERI_IP			0x107c
#define CLK_CON_MUX_MUX_CLKCMU_PERI_UART		0x1080
#define CLK_CON_MUX_MUX_CLKCMU_USB_BUS			0x1084
#define CLK_CON_MUX_MUX_CLKCMU_USB_DPGTC		0x1088
#define CLK_CON_MUX_MUX_CLKCMU_USB_USB30DRD		0x108c
#define CLK_CON_MUX_MUX_CLKCMU_VIPX1_BUS		0x1090
#define CLK_CON_MUX_MUX_CLKCMU_VIPX2_BUS		0x1094
#define CLK_CON_MUX_MUX_CLK_CMU_CMUREF			0x1098
#define CLK_CON_MUX_MUX_CMU_CMUREF			0x109c
#define CLK_CON_DIV_AP2CP_SHARED0_PLL_CLK		0x1800
#define CLK_CON_DIV_AP2CP_SHARED1_PLL_CLK		0x1804
#define CLK_CON_DIV_CLKCMU_APM_BUS			0x1808
#define CLK_CON_DIV_CLKCMU_CAM_BUS			0x180c
#define CLK_CON_DIV_CLKCMU_CIS_CLK0			0x1810
#define CLK_CON_DIV_CLKCMU_CIS_CLK1			0x1814
#define CLK_CON_DIV_CLKCMU_CIS_CLK2			0x1818
#define CLK_CON_DIV_CLKCMU_CIS_CLK3			0x181c
#define CLK_CON_DIV_CLKCMU_CORE_BUS			0x1820
#define CLK_CON_DIV_CLKCMU_CORE_CCI			0x1824
#define CLK_CON_DIV_CLKCMU_CORE_G3D			0x1828
#define CLK_CON_DIV_CLKCMU_CPUCL0_DBG			0x182c
#define CLK_CON_DIV_CLKCMU_CPUCL0_SWITCH		0x1830
#define CLK_CON_DIV_CLKCMU_CPUCL1_SWITCH		0x1834
#define CLK_CON_DIV_CLKCMU_DISPAUD_AUD			0x1838
#define CLK_CON_DIV_CLKCMU_DISPAUD_CPU			0x183c
#define CLK_CON_DIV_CLKCMU_DISPAUD_DISP			0x1840
#define CLK_CON_DIV_CLKCMU_FSYS_BUS			0x1844
#define CLK_CON_DIV_CLKCMU_FSYS_MMC_CARD		0x1848
#define CLK_CON_DIV_CLKCMU_FSYS_MMC_EMBD		0x184c
#define CLK_CON_DIV_CLKCMU_FSYS_UFS_EMBD		0x1850
#define CLK_CON_DIV_CLKCMU_G2D_G2D			0x1854
#define CLK_CON_DIV_CLKCMU_G2D_MSCL			0x1858
#define CLK_CON_DIV_CLKCMU_G3D_SWITCH			0x185c
#define CLK_CON_DIV_CLKCMU_HPM				0x1860
#define CLK_CON_DIV_CLKCMU_ISP_BUS			0x1864
#define CLK_CON_DIV_CLKCMU_ISP_GDC			0x1868
#define CLK_CON_DIV_CLKCMU_ISP_VRA			0x186c
#define CLK_CON_DIV_CLKCMU_MFC_MFC			0x1870
#define CLK_CON_DIV_CLKCMU_MFC_WFD			0x1874
#define CLK_CON_DIV_CLKCMU_MIF_BUSP			0x1878
#define CLK_CON_DIV_CLKCMU_OTP				0x187c
#define CLK_CON_DIV_CLKCMU_PERI_BUS			0x1880
#define CLK_CON_DIV_CLKCMU_PERI_IP			0x1884
#define CLK_CON_DIV_CLKCMU_PERI_UART			0x1888
#define CLK_CON_DIV_CLKCMU_USB_BUS			0x188c
#define CLK_CON_DIV_CLKCMU_USB_DPGTC			0x1890
#define CLK_CON_DIV_CLKCMU_USB_USB30DRD			0x1894
#define CLK_CON_DIV_CLKCMU_VIPX1_BUS			0x1898
#define CLK_CON_DIV_CLKCMU_VIPX2_BUS			0x189c
#define CLK_CON_DIV_DIV_CLK_CMU_CMUREF			0x18a0
#define CLK_CON_DIV_PLL_MMC_DIV2			0x18a4
#define CLK_CON_DIV_PLL_SHARED0_DIV2			0x18a8
#define CLK_CON_DIV_PLL_SHARED0_DIV3			0x18ac
#define CLK_CON_DIV_PLL_SHARED0_DIV4			0x18b0
#define CLK_CON_DIV_PLL_SHARED1_DIV2			0x18b4
#define CLK_CON_DIV_PLL_SHARED1_DIV3			0x18b8
#define CLK_CON_DIV_PLL_SHARED1_DIV4			0x18bc
#define CLK_CON_GAT_CLKCMU_MIF_SWITCH			0x2000
#define CLK_CON_GAT_CLK_CMU_OTP_CLK			0x2004
#define CLK_CON_GAT_GATE_CLKCMU_APM_BUS			0x2008
#define CLK_CON_GAT_GATE_CLKCMU_CAM_BUS			0x200c
#define CLK_CON_GAT_GATE_CLKCMU_CIS_CLK0		0x2010
#define CLK_CON_GAT_GATE_CLKCMU_CIS_CLK1		0x2014
#define CLK_CON_GAT_GATE_CLKCMU_CIS_CLK2		0x2018
#define CLK_CON_GAT_GATE_CLKCMU_CIS_CLK3		0x201c
#define CLK_CON_GAT_GATE_CLKCMU_CORE_BUS		0x2020
#define CLK_CON_GAT_GATE_CLKCMU_CORE_CCI		0x2024
#define CLK_CON_GAT_GATE_CLKCMU_CORE_G3D		0x2028
#define CLK_CON_GAT_GATE_CLKCMU_CPUCL0_DBG		0x202c
#define CLK_CON_GAT_GATE_CLKCMU_CPUCL0_SWITCH		0x2030
#define CLK_CON_GAT_GATE_CLKCMU_CPUCL1_SWITCH		0x2034
#define CLK_CON_GAT_GATE_CLKCMU_DISPAUD_AUD		0x2038
#define CLK_CON_GAT_GATE_CLKCMU_DISPAUD_CPU		0x203c
#define CLK_CON_GAT_GATE_CLKCMU_DISPAUD_DISP		0x2040
#define CLK_CON_GAT_GATE_CLKCMU_FSYS_BUS		0x2044
#define CLK_CON_GAT_GATE_CLKCMU_FSYS_MMC_CARD		0x2048
#define CLK_CON_GAT_GATE_CLKCMU_FSYS_MMC_EMBD		0x204c
#define CLK_CON_GAT_GATE_CLKCMU_FSYS_UFS_EMBD		0x2050
#define CLK_CON_GAT_GATE_CLKCMU_G2D_G2D			0x2054
#define CLK_CON_GAT_GATE_CLKCMU_G2D_MSCL		0x2058
#define CLK_CON_GAT_GATE_CLKCMU_G3D_SWITCH		0x205c
#define CLK_CON_GAT_GATE_CLKCMU_HPM			0x2060
#define CLK_CON_GAT_GATE_CLKCMU_ISP_BUS			0x2064
#define CLK_CON_GAT_GATE_CLKCMU_ISP_GDC			0x2068
#define CLK_CON_GAT_GATE_CLKCMU_ISP_VRA			0x206c
#define CLK_CON_GAT_GATE_CLKCMU_MFC_MFC			0x2070
#define CLK_CON_GAT_GATE_CLKCMU_MFC_WFD			0x2074
#define CLK_CON_GAT_GATE_CLKCMU_MIF_BUSP		0x2078
#define CLK_CON_GAT_GATE_CLKCMU_MODEM_SHARED0		0x207c
#define CLK_CON_GAT_GATE_CLKCMU_MODEM_SHARED1		0x2080
#define CLK_CON_GAT_GATE_CLKCMU_PERI_BUS		0x2084
#define CLK_CON_GAT_GATE_CLKCMU_PERI_IP			0x2088
#define CLK_CON_GAT_GATE_CLKCMU_PERI_UART		0x208c
#define CLK_CON_GAT_GATE_CLKCMU_USB_BUS			0x2090
#define CLK_CON_GAT_GATE_CLKCMU_USB_DPGTC		0x2094
#define CLK_CON_GAT_GATE_CLKCMU_USB_USB30DRD		0x2098
#define CLK_CON_GAT_GATE_CLKCMU_VIPX1_BUS		0x209c
#define CLK_CON_GAT_GATE_CLKCMU_VIPX2_BUS		0x20a0

static const unsigned long top_clk_regs[] __initconst = {
	PLL_LOCKTIME_PLL_MMC,
	PLL_LOCKTIME_PLL_SHARED0,
	PLL_LOCKTIME_PLL_SHARED1,
	PLL_CON0_PLL_MMC,
	PLL_CON3_PLL_MMC,
	PLL_CON0_PLL_SHARED0,
	PLL_CON0_PLL_SHARED1,
	CMU_CMU_TOP_CONTROLLER_OPTION,
	CLK_CON_MUX_MUX_CLKCMU_APM_BUS,
	CLK_CON_MUX_MUX_CLKCMU_CAM_BUS,
	CLK_CON_MUX_MUX_CLKCMU_CIS_CLK0,
	CLK_CON_MUX_MUX_CLKCMU_CIS_CLK1,
	CLK_CON_MUX_MUX_CLKCMU_CIS_CLK2,
	CLK_CON_MUX_MUX_CLKCMU_CIS_CLK3,
	CLK_CON_MUX_MUX_CLKCMU_CORE_BUS,
	CLK_CON_MUX_MUX_CLKCMU_CORE_CCI,
	CLK_CON_MUX_MUX_CLKCMU_CORE_G3D,
	CLK_CON_MUX_MUX_CLKCMU_CPUCL0_DBG,
	CLK_CON_MUX_MUX_CLKCMU_CPUCL0_SWITCH,
	CLK_CON_MUX_MUX_CLKCMU_CPUCL1_SWITCH,
	CLK_CON_MUX_MUX_CLKCMU_DISPAUD_AUD,
	CLK_CON_MUX_MUX_CLKCMU_DISPAUD_CPU,
	CLK_CON_MUX_MUX_CLKCMU_DISPAUD_DISP,
	CLK_CON_MUX_MUX_CLKCMU_FSYS_BUS,
	CLK_CON_MUX_MUX_CLKCMU_FSYS_MMC_CARD,
	CLK_CON_MUX_MUX_CLKCMU_FSYS_MMC_EMBD,
	CLK_CON_MUX_MUX_CLKCMU_FSYS_UFS_EMBD,
	CLK_CON_MUX_MUX_CLKCMU_G2D_G2D,
	CLK_CON_MUX_MUX_CLKCMU_G2D_MSCL,
	CLK_CON_MUX_MUX_CLKCMU_G3D_SWITCH,
	CLK_CON_MUX_MUX_CLKCMU_HPM,
	CLK_CON_MUX_MUX_CLKCMU_ISP_BUS,
	CLK_CON_MUX_MUX_CLKCMU_ISP_GDC,
	CLK_CON_MUX_MUX_CLKCMU_ISP_VRA,
	CLK_CON_MUX_MUX_CLKCMU_MFC_MFC,
	CLK_CON_MUX_MUX_CLKCMU_MFC_WFD,
	CLK_CON_MUX_MUX_CLKCMU_MIF_BUSP,
	CLK_CON_MUX_MUX_CLKCMU_MIF_SWITCH,
	CLK_CON_MUX_MUX_CLKCMU_PERI_BUS,
	CLK_CON_MUX_MUX_CLKCMU_PERI_IP,
	CLK_CON_MUX_MUX_CLKCMU_PERI_UART,
	CLK_CON_MUX_MUX_CLKCMU_USB_BUS,
	CLK_CON_MUX_MUX_CLKCMU_USB_DPGTC,
	CLK_CON_MUX_MUX_CLKCMU_USB_USB30DRD,
	CLK_CON_MUX_MUX_CLKCMU_VIPX1_BUS,
	CLK_CON_MUX_MUX_CLKCMU_VIPX2_BUS,
	CLK_CON_MUX_MUX_CLK_CMU_CMUREF,
	CLK_CON_MUX_MUX_CMU_CMUREF,
	CLK_CON_DIV_AP2CP_SHARED0_PLL_CLK,
	CLK_CON_DIV_AP2CP_SHARED1_PLL_CLK,
	CLK_CON_DIV_CLKCMU_APM_BUS,
	CLK_CON_DIV_CLKCMU_CAM_BUS,
	CLK_CON_DIV_CLKCMU_CORE_BUS,
	CLK_CON_DIV_CLKCMU_CORE_CCI,
	CLK_CON_DIV_CLKCMU_CORE_G3D,
	CLK_CON_DIV_CLKCMU_CPUCL0_DBG,
	CLK_CON_DIV_CLKCMU_CPUCL0_SWITCH,
	CLK_CON_DIV_CLKCMU_CPUCL1_SWITCH,
	CLK_CON_DIV_CLKCMU_DISPAUD_AUD,
	CLK_CON_DIV_CLKCMU_DISPAUD_CPU,
	CLK_CON_DIV_CLKCMU_DISPAUD_DISP,
	CLK_CON_DIV_CLKCMU_FSYS_BUS,
	CLK_CON_DIV_CLKCMU_FSYS_MMC_CARD,
	CLK_CON_DIV_CLKCMU_FSYS_MMC_EMBD,
	CLK_CON_DIV_CLKCMU_FSYS_UFS_EMBD,
	CLK_CON_DIV_CLKCMU_G2D_G2D,
	CLK_CON_DIV_CLKCMU_G2D_MSCL,
	CLK_CON_DIV_CLKCMU_G3D_SWITCH,
	CLK_CON_DIV_CLKCMU_HPM,
	CLK_CON_DIV_CLKCMU_ISP_BUS,
	CLK_CON_DIV_CLKCMU_ISP_GDC,
	CLK_CON_DIV_CLKCMU_ISP_VRA,
	CLK_CON_DIV_CLKCMU_MFC_MFC,
	CLK_CON_DIV_CLKCMU_MFC_WFD,
	CLK_CON_DIV_CLKCMU_MIF_BUSP,
	CLK_CON_DIV_CLKCMU_OTP,
	CLK_CON_DIV_CLKCMU_PERI_BUS,
	CLK_CON_DIV_CLKCMU_PERI_IP,
	CLK_CON_DIV_CLKCMU_PERI_UART,
	CLK_CON_DIV_CLKCMU_USB_BUS,
	CLK_CON_DIV_CLKCMU_USB_DPGTC,
	CLK_CON_DIV_CLKCMU_USB_USB30DRD,
	CLK_CON_DIV_CLKCMU_VIPX1_BUS,
	CLK_CON_DIV_CLKCMU_VIPX2_BUS,
	CLK_CON_DIV_DIV_CLK_CMU_CMUREF,
	CLK_CON_DIV_PLL_SHARED0_DIV2,
	CLK_CON_DIV_PLL_SHARED0_DIV3,
	CLK_CON_DIV_PLL_SHARED0_DIV4,
	CLK_CON_DIV_PLL_SHARED1_DIV2,
	CLK_CON_DIV_PLL_SHARED1_DIV3,
	CLK_CON_DIV_PLL_SHARED1_DIV4,
	CLK_CON_GAT_CLKCMU_MIF_SWITCH,
	CLK_CON_GAT_CLK_CMU_OTP_CLK,
	CLK_CON_GAT_GATE_CLKCMU_APM_BUS,
	CLK_CON_GAT_GATE_CLKCMU_CAM_BUS,
	CLK_CON_GAT_GATE_CLKCMU_CIS_CLK0,
	CLK_CON_GAT_GATE_CLKCMU_CIS_CLK1,
	CLK_CON_GAT_GATE_CLKCMU_CIS_CLK2,
	CLK_CON_GAT_GATE_CLKCMU_CIS_CLK3,
	CLK_CON_GAT_GATE_CLKCMU_CORE_BUS,
	CLK_CON_GAT_GATE_CLKCMU_CORE_CCI,
	CLK_CON_GAT_GATE_CLKCMU_CORE_G3D,
	CLK_CON_GAT_GATE_CLKCMU_CPUCL0_DBG,
	CLK_CON_GAT_GATE_CLKCMU_CPUCL0_SWITCH,
	CLK_CON_GAT_GATE_CLKCMU_CPUCL1_SWITCH,
	CLK_CON_GAT_GATE_CLKCMU_DISPAUD_AUD,
	CLK_CON_GAT_GATE_CLKCMU_DISPAUD_CPU,
	CLK_CON_GAT_GATE_CLKCMU_DISPAUD_DISP,
	CLK_CON_GAT_GATE_CLKCMU_FSYS_BUS,
	CLK_CON_GAT_GATE_CLKCMU_FSYS_MMC_CARD,
	CLK_CON_GAT_GATE_CLKCMU_FSYS_MMC_EMBD,
	CLK_CON_GAT_GATE_CLKCMU_FSYS_UFS_EMBD,
	CLK_CON_GAT_GATE_CLKCMU_G2D_G2D,
	CLK_CON_GAT_GATE_CLKCMU_G2D_MSCL,
	CLK_CON_GAT_GATE_CLKCMU_G3D_SWITCH,
	CLK_CON_GAT_GATE_CLKCMU_HPM,
	CLK_CON_GAT_GATE_CLKCMU_ISP_BUS,
	CLK_CON_GAT_GATE_CLKCMU_ISP_GDC,
	CLK_CON_GAT_GATE_CLKCMU_ISP_VRA,
	CLK_CON_GAT_GATE_CLKCMU_MFC_MFC,
	CLK_CON_GAT_GATE_CLKCMU_MFC_WFD,
	CLK_CON_GAT_GATE_CLKCMU_MIF_BUSP,
	CLK_CON_GAT_GATE_CLKCMU_MODEM_SHARED0,
	CLK_CON_GAT_GATE_CLKCMU_MODEM_SHARED1,
	CLK_CON_GAT_GATE_CLKCMU_PERI_BUS,
	CLK_CON_GAT_GATE_CLKCMU_PERI_IP,
	CLK_CON_GAT_GATE_CLKCMU_PERI_UART,
	CLK_CON_GAT_GATE_CLKCMU_USB_BUS,
	CLK_CON_GAT_GATE_CLKCMU_USB_DPGTC,
	CLK_CON_GAT_GATE_CLKCMU_USB_USB30DRD,
	CLK_CON_GAT_GATE_CLKCMU_VIPX1_BUS,
	CLK_CON_GAT_GATE_CLKCMU_VIPX2_BUS,
};

static const struct samsung_pll_rate_table pll_shared0_rate_table[] __initconst = {
	PLL_35XX_RATE(26 * MHZ, 1599000000U, 246, 4, 0),
};

static const struct samsung_pll_rate_table pll_shared1_rate_table[] __initconst = {
	PLL_35XX_RATE(26 * MHZ, 1332500000U, 205, 4, 0),
};

static const struct samsung_pll_rate_table pll_mmc_rate_table[] __initconst = {
	PLL_36XX_RATE(26 * MHZ, 799999877, 31, 1, 0, -15124),
};

static const struct samsung_pll_clock top_pll_clks[] __initconst = {
	PLL(pll_1051x, CLK_FOUT_SHARED0_PLL, "fout_shared0_pll", "oscclk",
	    PLL_LOCKTIME_PLL_SHARED0, PLL_CON0_PLL_SHARED0, pll_shared0_rate_table),
	PLL(pll_1051x, CLK_FOUT_SHARED1_PLL, "fout_shared1_pll", "oscclk",
	    PLL_LOCKTIME_PLL_SHARED1, PLL_CON0_PLL_SHARED1, pll_shared1_rate_table),
	PLL(pll_1061x, CLK_FOUT_MMC_PLL, "fout_mmc_pll", "oscclk",
	    PLL_LOCKTIME_PLL_MMC, PLL_CON0_PLL_MMC, pll_mmc_rate_table),
};

static const struct samsung_fixed_rate_clock top_fixed_clks[] __initconst = {
	/* OSCCLK for CMGP */
	FRATE(0, "oscclk_rco_cmgp", NULL, 0, 30000000),
	/* input clock for I2S/PCM */
	FRATE(0, "ioclk_audiocdclk0", NULL, 0, 10000000),
	FRATE(0, "ioclk_audiocdclk1", NULL, 0, 100000000),
	/* for USB audio? */
	FRATE(0, "tick_usb", NULL, 0, 60000000),
};

/* Parent clock list for TOP muxes */
PNAME(mout_pll_shared0_p)	= { "oscclk", "fout_shared0_pll" };
PNAME(mout_pll_shared1_p)	= { "oscclk", "fout_shared1_pll" };
PNAME(mout_pll_mmc_p)		= { "oscclk", "fout_mmc_pll" };
PNAME(mout_cmu_apm_bus_p)	= { "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_cam_bus_p)	= { "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_cis_clk0_p)	= { "oscclk",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_cis_clk1_p)	= { "oscclk",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_cis_clk2_p)	= { "oscclk",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_cis_clk3_p)	= { "oscclk",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_core_bus_p)	= { "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_mmc_div2" };
PNAME(mout_cmu_core_cci_p)	= { "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_mmc_div2" };
PNAME(mout_cmu_core_g3d_p)	= { "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_mmc_div2" };
PNAME(mout_cmu_cpucl0_dbg_p)	= { "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_cpucl0_switch_p)	= { "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3" };
PNAME(mout_cmu_cpucl1_switch_p)	= { "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3" };
PNAME(mout_cmu_dispaud_aud_p)	= { "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_dispaud_cpu_p)	= { "fout_shared1_pll",
				    "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "fout_mmc_pll",
				    "oscclk", "oscclk" };
PNAME(mout_cmu_dispaud_disp_p)	= { "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_fsys_bus_p)	= { "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2" };
PNAME(mout_cmu_fsys_mmc_card_p)	= { "oscclk",
				    "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "fout_mmc_pll",
				    "oscclk", "oscclk" };
PNAME(mout_cmu_fsys_mmc_embd_p)	= { "oscclk",
				    "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "fout_mmc_pll",
				    "oscclk", "oscclk" };
PNAME(mout_cmu_fsys_ufs_embd_p) = { "oscclk",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4",
				    "oscclk" };
PNAME(mout_cmu_g2d_g2d_p)	= { "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_g2d_mscl_p)	= { "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_g3d_switch_p)	= { "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3" };
PNAME(mout_cmu_hpm_p)		= { "oscclk",
				    "dout_cmu_shared0_div2",
				    "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_mmc_div2",
				    "oscclk", "oscclk", "oscclk" };
PNAME(mout_cmu_isp_bus_p)	= { "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_isp_gdc_p)	= { "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_isp_vra_p)	= { "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_mfc_mfc_p)	= { "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_mfc_wfd_p)	= { "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_mif_busp_p)	= { "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4",
				    "dout_cmu_mmc_div2",
				    "oscclk" };
PNAME(mout_cmu_mif_switch_p)	= { "fout_shared0_pll",
				    "fout_shared1_pll",
				    "dout_cmu_shared0_div2",
				    "fout_mmc_pll",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_peri_bus_p)	= { "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_peri_ip_p)	= { "oscclk",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4",
				    "oscclk" };
PNAME(mout_cmu_peri_uart_p)	= { "oscclk",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4",
				    "oscclk" };
PNAME(mout_cmu_usb_bus_p)	= { "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_usb_dpgtc_p)	= { "oscclk",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4",
				    "oscclk" };
PNAME(mout_cmu_usb_usb30drd_p)	= { "oscclk",
				    "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4",
				    "oscclk" };
PNAME(mout_cmu_vipx1_bus_p)	= { "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4" };
PNAME(mout_cmu_vipx2_bus_p)	= { "dout_cmu_shared1_div2",
				    "dout_cmu_shared0_div3",
				    "dout_cmu_shared1_div3",
				    "dout_cmu_shared0_div4" };
PNAME(mout_clk_cmu_cmuref_p)	= { "dout_cmu_shared0_div4",
				    "dout_cmu_shared1_div4" };
PNAME(mout_cmu_cmuref_p)	= { "oscclk",
				    "dout_clk_cmu_cmuref" };

/*
 * Register name to clock name mangling strategy used in this file
 *
 * Replace PLL_CON{0,3}_PLL	   with CLK_MOUT_PLL and mout_pll
 * Replace CLK_CON_MUX_MUX_CLKCMU  with CLK_MOUT_CMU and mout_cmu
 * Replace CLK_CON_DIV_CLKCMU      with CLK_DOUT_CMU_CMU and dout_cmu_cmu
 * Replace CLK_CON_DIV_DIV_CLKCMU  with CLK_DOUT_CMU_CMU and dout_cmu_cmu
 * Replace CLK_CON_DIV_PLL_CLKCMU  with CLK_DOUT_CMU_CMU and dout_cmu_cmu
 * Replace CLK_CON_GAT_CLKCMU      with CLK_GOUT_CMU and gout_cmu
 * Replace CLK_CON_GAT_GATE_CLKCMU with CLK_GOUT_CMU and gout_cmu
 *
 * For gates remove _UID _BLK _IPCLKPORT, _I and _RSTNSYNC
 */

static const struct samsung_mux_clock top_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_SHARED0, "mout_pll_shared0", mout_pll_shared0_p,
	    PLL_CON0_PLL_SHARED0, 4, 1),
	MUX(CLK_MOUT_PLL_SHARED1, "mout_pll_shared1", mout_pll_shared1_p,
	    PLL_CON0_PLL_SHARED1, 4, 1),
	MUX(CLK_MOUT_PLL_MMC, "mout_pll_mmc", mout_pll_mmc_p,
	    PLL_CON0_PLL_MMC, 4, 1),

	MUX(CLK_MOUT_CMU_APM_BUS, "mout_cmu_apm_bus", mout_cmu_apm_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_APM_BUS, 0, 1),
	MUX(CLK_MOUT_CMU_CAM_BUS, "mout_cmu_cam_bus", mout_cmu_cam_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_CAM_BUS, 0, 2),
	MUX(CLK_MOUT_CMU_CIS_CLK0, "mout_cmu_cis_clk0", mout_cmu_cis_clk0_p,
	    CLK_CON_MUX_MUX_CLKCMU_CIS_CLK0, 0, 1),
	MUX(CLK_MOUT_CMU_CIS_CLK1, "mout_cmu_cis_clk1", mout_cmu_cis_clk1_p,
	    CLK_CON_MUX_MUX_CLKCMU_CIS_CLK1, 0, 1),
	MUX(CLK_MOUT_CMU_CIS_CLK2, "mout_cmu_cis_clk2", mout_cmu_cis_clk2_p,
	    CLK_CON_MUX_MUX_CLKCMU_CIS_CLK2, 0, 1),
	MUX(CLK_MOUT_CMU_CIS_CLK3, "mout_cmu_cis_clk3", mout_cmu_cis_clk2_p,
	    CLK_CON_MUX_MUX_CLKCMU_CIS_CLK3, 0, 1),
	MUX(CLK_MOUT_CMU_CORE_BUS, "mout_cmu_core_bus", mout_cmu_core_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_CORE_BUS, 0, 2),
	MUX(CLK_MOUT_CMU_CORE_CCI, "mout_cmu_core_cci", mout_cmu_core_cci_p,
	    CLK_CON_MUX_MUX_CLKCMU_CORE_CCI, 0, 2),
	MUX(CLK_MOUT_CMU_CORE_G3D, "mout_cmu_core_g3d", mout_cmu_core_g3d_p,
	    CLK_CON_MUX_MUX_CLKCMU_CORE_G3D, 0, 2),
	MUX(CLK_MOUT_CMU_CPUCL0_DBG, "mout_cmu_cpucl0_dbg", mout_cmu_cpucl0_dbg_p,
	    CLK_CON_MUX_MUX_CLKCMU_CPUCL0_DBG, 0, 1),
	MUX(CLK_MOUT_CMU_CPUCL0_SWITCH, "mout_cmu_cpucl0_switch", mout_cmu_cpucl0_switch_p,
	    CLK_CON_MUX_MUX_CLKCMU_CPUCL0_SWITCH, 0, 1),
	MUX(CLK_MOUT_CMU_CPUCL1_SWITCH, "mout_cmu_cpucl1_switch", mout_cmu_cpucl1_switch_p,
	    CLK_CON_MUX_MUX_CLKCMU_CPUCL1_SWITCH, 0, 2),
	MUX(CLK_MOUT_CMU_DISPAUD_AUD, "mout_cmu_dispaud_aud", mout_cmu_dispaud_aud_p,
	    CLK_CON_MUX_MUX_CLKCMU_DISPAUD_AUD, 0, 2),
	MUX(CLK_MOUT_CMU_DISPAUD_CPU, "mout_cmu_dispaud_cpu", mout_cmu_dispaud_cpu_p,
	    CLK_CON_MUX_MUX_CLKCMU_DISPAUD_CPU, 0, 3),
	MUX(CLK_MOUT_CMU_DISPAUD_DISP, "mout_cmu_dispaud_disp", mout_cmu_dispaud_disp_p,
	    CLK_CON_MUX_MUX_CLKCMU_DISPAUD_DISP, 0, 2),
	MUX(CLK_MOUT_CMU_FSYS_BUS, "mout_cmu_fsys_bus", mout_cmu_fsys_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_FSYS_BUS, 0, 1),
	MUX(CLK_MOUT_CMU_FSYS_MMC_CARD, "mout_cmu_fsys_mmc_card", mout_cmu_fsys_mmc_card_p,
	    CLK_CON_MUX_MUX_CLKCMU_FSYS_MMC_CARD, 0, 3),
	MUX(CLK_MOUT_CMU_FSYS_MMC_EMBD, "mout_cmu_fsys_mmc_embd", mout_cmu_fsys_mmc_embd_p,
	    CLK_CON_MUX_MUX_CLKCMU_FSYS_MMC_EMBD, 0, 3),
	MUX(CLK_MOUT_CMU_FSYS_UFS_EMBD, "mout_cmu_fsys_ufs_embd", mout_cmu_fsys_ufs_embd_p,
	    CLK_CON_MUX_MUX_CLKCMU_FSYS_UFS_EMBD, 0, 2),
	MUX(CLK_MOUT_CMU_G2D_G2D, "mout_cmu_g2d_g2d", mout_cmu_g2d_g2d_p,
	    CLK_CON_MUX_MUX_CLKCMU_G2D_G2D, 0, 2),
	MUX(CLK_MOUT_CMU_G2D_MSCL, "mout_cmu_g2d_mscl", mout_cmu_g2d_mscl_p,
	    CLK_CON_MUX_MUX_CLKCMU_G2D_MSCL, 0, 2),
	MUX(CLK_MOUT_CMU_G3D_SWITCH, "mout_cmu_g3d_SWITCH", mout_cmu_g3d_switch_p,
	    CLK_CON_MUX_MUX_CLKCMU_G3D_SWITCH, 0, 2),
	MUX(CLK_MOUT_CMU_HPM, "mout_cmu_hpm", mout_cmu_hpm_p,
	    CLK_CON_MUX_MUX_CLKCMU_HPM, 0, 3),
	MUX(CLK_MOUT_CMU_ISP_BUS, "mout_cmu_isp_bus", mout_cmu_isp_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_ISP_BUS, 0, 2),
	MUX(CLK_MOUT_CMU_ISP_GDC, "mout_cmu_isp_gdc", mout_cmu_isp_gdc_p,
	    CLK_CON_MUX_MUX_CLKCMU_ISP_GDC, 0, 2),
	MUX(CLK_MOUT_CMU_ISP_VRA, "mout_cmu_isp_vra", mout_cmu_isp_vra_p,
	    CLK_CON_MUX_MUX_CLKCMU_ISP_VRA, 0, 2),
	MUX(CLK_MOUT_CMU_MFC_MFC, "mout_cmu_mfc_mfc", mout_cmu_mfc_mfc_p,
	    CLK_CON_MUX_MUX_CLKCMU_MFC_MFC, 0, 2),
	MUX(CLK_MOUT_CMU_MFC_WFD, "mout_cmu_mfc_wfd", mout_cmu_mfc_wfd_p,
	    CLK_CON_MUX_MUX_CLKCMU_MFC_WFD, 0, 2),
	MUX(CLK_MOUT_CMU_MIF_BUSP, "mout_cmu_mif_busp", mout_cmu_mif_busp_p,
	    CLK_CON_MUX_MUX_CLKCMU_MIF_BUSP, 0, 2),
	MUX(CLK_MOUT_CMU_MIF_SWITCH, "mout_cmu_mif_switch", mout_cmu_mif_switch_p,
	    CLK_CON_MUX_MUX_CLKCMU_MIF_SWITCH, 0, 3),
	MUX(CLK_MOUT_CMU_PERI_BUS, "mout_cmu_peri_bus", mout_cmu_peri_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_PERI_BUS, 0, 1),
	MUX(CLK_MOUT_CMU_PERI_IP, "mout_cmu_peri_ip", mout_cmu_peri_ip_p,
	    CLK_CON_MUX_MUX_CLKCMU_PERI_IP, 0, 2),
	MUX(CLK_MOUT_CMU_PERI_UART, "mout_cmu_peri_uart", mout_cmu_peri_uart_p,
	    CLK_CON_MUX_MUX_CLKCMU_PERI_UART, 0, 2),
	MUX(CLK_MOUT_CMU_USB_BUS, "mout_cmu_usb_bus", mout_cmu_usb_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_USB_BUS, 0, 2),
	MUX(CLK_MOUT_CMU_USB_DPGTC, "mout_cmu_usb_dpgtc", mout_cmu_usb_dpgtc_p,
	    CLK_CON_MUX_MUX_CLKCMU_USB_DPGTC, 0, 2),
	MUX(CLK_MOUT_CMU_USB_USB30DRD, "mout_cmu_usb_usb30drd", mout_cmu_usb_usb30drd_p,
	    CLK_CON_MUX_MUX_CLKCMU_USB_USB30DRD, 0, 2),
	MUX(CLK_MOUT_CMU_VIPX1_BUS, "mout_cmu_vipx1_bus", mout_cmu_vipx1_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_VIPX1_BUS, 0, 2),
	MUX(CLK_MOUT_CMU_VIPX2_BUS, "mout_cmu_vipx2_bus", mout_cmu_vipx2_bus_p,
	    CLK_CON_MUX_MUX_CLKCMU_VIPX2_BUS, 0, 2),
	MUX(CLK_MOUT_CLK_CMU_CMUREF, "mout_clk_cmu_cmuref", mout_clk_cmu_cmuref_p,
	    CLK_CON_MUX_MUX_CLK_CMU_CMUREF, 0, 1),
	MUX(CLK_MOUT_CMU_CMUREF, "mout_cmu_cmuref", mout_cmu_cmuref_p,
	    CLK_CON_MUX_MUX_CMU_CMUREF, 0, 1),
};

static const struct samsung_div_clock top_div_clks[] __initconst = {
	/* shared0 */
	DIV(CLK_DOUT_CMU_SHARED0_DIV2, "dout_cmu_shared0_div2", "mout_pll_shared0",
	    CLK_CON_DIV_PLL_SHARED0_DIV2, 0, 1),
	DIV(CLK_DOUT_CMU_SHARED0_DIV3, "dout_cmu_shared0_div3", "mout_pll_shared0",
	    CLK_CON_DIV_PLL_SHARED0_DIV3, 0, 2),
	DIV(CLK_DOUT_CMU_SHARED0_DIV4, "dout_cmu_shared0_div4", "dout_cmu_shared0_div2",
	    CLK_CON_DIV_PLL_SHARED0_DIV4, 0, 1),

	/* shared1 */
	DIV(CLK_DOUT_CMU_SHARED1_DIV2, "dout_cmu_shared1_div2", "mout_pll_shared1",
	    CLK_CON_DIV_PLL_SHARED1_DIV2, 0, 1),
	DIV(CLK_DOUT_CMU_SHARED1_DIV3, "dout_cmu_shared1_div3", "mout_pll_shared1",
	    CLK_CON_DIV_PLL_SHARED1_DIV3, 0, 2),
	DIV(CLK_DOUT_CMU_SHARED1_DIV4, "dout_cmu_shared1_div4", "dout_cmu_shared1_div2",
	    CLK_CON_DIV_PLL_SHARED1_DIV4, 0, 1),

	/* mmc */
	DIV(CLK_DOUT_CMU_MMC_DIV2, "dout_cmu_mmc_div2", "mout_pll_mmc",
	    CLK_CON_DIV_PLL_MMC_DIV2, 0, 1),

	DIV(CLK_DOUT_AP2CP_SHARED0_PLL_CLK, "dout_ap2cp_shared0_pll_clk",
	    "gout_cmu_modem_shared0", CLK_CON_DIV_AP2CP_SHARED0_PLL_CLK,
	    0, 4),
	DIV(CLK_DOUT_AP2CP_SHARED1_PLL_CLK, "dout_ap2cp_shared1_pll_clk",
	    "gout_cmu_modem_shared1", CLK_CON_DIV_AP2CP_SHARED1_PLL_CLK,
	    0, 4),
	DIV(CLK_DOUT_CMU_APM_BUS, "dout_cmu_apm_bus", "gout_cmu_apm_bus",
	    CLK_CON_DIV_CLKCMU_APM_BUS, 0, 3),
	DIV(CLK_DOUT_CMU_CAM_BUS, "dout_cmu_cam_bus", "gout_cmu_cam_bus",
	    CLK_CON_DIV_CLKCMU_CAM_BUS, 0, 4),
	DIV(CLK_DOUT_CMU_CIS_CLK0, "dout_cmu_cis_clk0", "gout_cmu_cis_clk0",
	    CLK_CON_DIV_CLKCMU_CIS_CLK0, 0, 5),
	DIV(CLK_DOUT_CMU_CIS_CLK1, "dout_cmu_cis_clk1", "gout_cmu_cis_clk1",
	    CLK_CON_DIV_CLKCMU_CIS_CLK1, 0, 5),
	DIV(CLK_DOUT_CMU_CIS_CLK2, "dout_cmu_cis_clk2", "gout_cmu_cis_clk2",
	    CLK_CON_DIV_CLKCMU_CIS_CLK2, 0, 5),
	DIV(CLK_DOUT_CMU_CIS_CLK3, "dout_cmu_cis_clk3", "gout_cmu_cis_clk3",
	    CLK_CON_DIV_CLKCMU_CIS_CLK3, 0, 5),
	DIV(CLK_DOUT_CMU_CORE_BUS, "dout_cmu_core_bus", "gout_cmu_core_bus",
	    CLK_CON_DIV_CLKCMU_CORE_BUS, 0, 4),
	DIV(CLK_DOUT_CMU_CORE_CCI, "dout_cmu_core_cci", "gout_cmu_core_cci",
	    CLK_CON_DIV_CLKCMU_CORE_BUS, 0, 4),
	DIV(CLK_DOUT_CMU_CORE_G3D, "dout_cmu_core_g3d", "gout_cmu_core_g3d",
	    CLK_CON_DIV_CLKCMU_CORE_G3D, 0, 4),
	DIV(CLK_DOUT_CMU_CPUCL0_DBG, "dout_cmu_cpucl0_dbg", "gout_cmu_cpucl0_dbg",
	    CLK_CON_DIV_CLKCMU_CPUCL0_DBG, 0, 3),
	DIV(CLK_DOUT_CMU_CPUCL0_SWITCH, "dout_cmu_cpucl0_switch", "gout_cmu_cpucl0_switch",
	    CLK_CON_DIV_CLKCMU_CPUCL0_SWITCH, 0, 3),
	DIV(CLK_DOUT_CMU_CPUCL1_SWITCH, "dout_cmu_cpucl1_switch", "gout_cmu_cpucl1_switch",
	    CLK_CON_DIV_CLKCMU_CPUCL1_SWITCH, 0, 3),
	DIV(CLK_DOUT_CMU_DISPAUD_AUD, "dout_cmu_dispaud_aud", "gout_cmu_dispaud_aud",
	    CLK_CON_DIV_CLKCMU_DISPAUD_AUD, 0, 4),
	DIV(CLK_DOUT_CMU_DISPAUD_CPU, "dout_cmu_dispaud_cpu", "gout_cmu_dispaud_cpu",
	    CLK_CON_DIV_CLKCMU_DISPAUD_CPU, 0, 4),
	DIV(CLK_DOUT_CMU_DISPAUD_DISP, "dout_cmu_dispaud_disp", "gout_cmu_dispaud_disp",
	    CLK_CON_DIV_CLKCMU_DISPAUD_DISP, 0, 4),
	DIV(CLK_DOUT_CMU_FSYS_BUS, "dout_cmu_fsys_bus", "gout_cmu_fsys_bus",
	    CLK_CON_DIV_CLKCMU_FSYS_BUS, 0, 4),
	DIV(CLK_DOUT_CMU_FSYS_MMC_CARD, "dout_cmu_fsys_mmc_card", "gout_cmu_fsys_mmc_card",
	    CLK_CON_DIV_CLKCMU_FSYS_MMC_CARD, 0, 9),
	DIV(CLK_DOUT_CMU_FSYS_MMC_EMBD, "dout_cmu_fsys_mmc_embd", "gout_cmu_fsys_mmc_embd",
	    CLK_CON_DIV_CLKCMU_FSYS_MMC_EMBD, 0, 9),
	DIV(CLK_DOUT_CMU_G2D_G2D, "dout_cmu_g2d_g2d", "gout_cmu_g2d_g2d",
	    CLK_CON_DIV_CLKCMU_G2D_G2D, 0, 4),
	DIV(CLK_DOUT_CMU_G2D_MSCL, "dout_cmu_g2d_mscl", "gout_cmu_g2d_mscl",
	    CLK_CON_DIV_CLKCMU_G2D_MSCL, 0, 4),
	DIV(CLK_DOUT_CMU_G3D_SWITCH, "dout_cmu_g3d_switch", "gout_cmu_g3d_switch",
	    CLK_CON_DIV_CLKCMU_G3D_SWITCH, 0, 3),
	DIV(CLK_DOUT_CMU_HPM, "dout_cmu_hpm", "gout_cmu_hpm",
	    CLK_CON_DIV_CLKCMU_HPM, 0, 2),
	DIV(CLK_DOUT_CMU_ISP_BUS, "dout_cmu_isp_bus", "gout_cmu_isp_bus",
	    CLK_CON_DIV_CLKCMU_ISP_BUS, 0, 4),
	DIV(CLK_DOUT_CMU_ISP_GDC, "dout_cmu_isp_gdc", "gout_cmu_isp_gdc",
	    CLK_CON_DIV_CLKCMU_ISP_GDC, 0, 4),
	DIV(CLK_DOUT_CMU_ISP_VRA, "dout_cmu_isp_vra", "gout_cmu_isp_vra",
	    CLK_CON_DIV_CLKCMU_ISP_VRA, 0, 4),
	DIV(CLK_DOUT_CMU_MFD_MFC, "dout_cmu_mfc_mfc", "gout_cmu_mfc_mfc",
	    CLK_CON_DIV_CLKCMU_MFC_MFC, 0, 4),
	DIV(CLK_DOUT_CMU_MFD_WFD, "dout_cmu_mfc_wfd", "gout_cmu_mfc_wfd",
	    CLK_CON_DIV_CLKCMU_MFC_WFD, 0, 4),
	DIV(CLK_DOUT_CMU_MIF_BUSP, "dout_cmu_mif_busp", "gout_cmu_mif_busp",
	    CLK_CON_DIV_CLKCMU_MIF_BUSP, 0, 3),
	DIV(CLK_DOUT_CMU_PERI_BUS, "dout_cmu_peri_bus", "gout_cmu_peri_bus",
	    CLK_CON_DIV_CLKCMU_PERI_BUS, 0, 4),
	DIV(CLK_DOUT_CMU_PERI_IP, "dout_cmu_peri_ip", "gout_cmu_peri_ip",
	    CLK_CON_DIV_CLKCMU_PERI_IP, 0, 4),
	DIV(CLK_DOUT_CMU_PERI_UART, "dout_cmu_peri_uart", "gout_cmu_peri_uart",
	    CLK_CON_DIV_CLKCMU_PERI_UART, 0, 4),
	DIV(CLK_DOUT_CMU_USB_BUS, "dout_cmu_usb_bus", "gout_cmu_usb_bus",
	    CLK_CON_DIV_CLKCMU_USB_BUS, 0, 4),
	DIV(CLK_DOUT_CMU_USB_DPGTC, "dout_cmu_usb_dpgtc", "gout_cmu_usb_dpgtc",
	    CLK_CON_DIV_CLKCMU_USB_DPGTC, 0, 4),
	DIV(CLK_DOUT_CMU_USB_BUS, "dout_cmu_usb_usb30drd", "gout_cmu_usb_usb30drd",
	    CLK_CON_DIV_CLKCMU_USB_USB30DRD, 0, 4),
	DIV(CLK_DOUT_CMU_VIPX1_BUS, "dout_cmu_vipx1_bus", "gout_cmu_vipx1_bus",
	    CLK_CON_DIV_CLKCMU_VIPX1_BUS, 0, 4),
	DIV(CLK_DOUT_CMU_VIPX2_BUS, "dout_cmu_vipx2_bus", "gout_cmu_vipx2_bus",
	    CLK_CON_DIV_CLKCMU_VIPX2_BUS, 0, 4),
	DIV(CLK_DOUT_CLK_CMU_CMUREF, "dout_clk_cmu_cmuref", "mout_clk_cmu_cmuref",
	    CLK_CON_DIV_DIV_CLK_CMU_CMUREF, 0, 2),
};

static const struct samsung_fixed_factor_clock top_ffactor_clks[] __initconst = {
	FFACTOR(CLK_DOUT_CMU_OTP, "dout_cmu_otp", "oscclk", 1, 7, 0),
};

static const struct samsung_gate_clock top_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CMU_MIF_SWITCH, "gout_cmu_mif_switch",
	     "mux_cmu_mif_switch", CLK_CON_GAT_CLKCMU_MIF_SWITCH,
	     21, 0, 0),
	GATE(CLK_GOUT_CLK_CMU_OTP_CLK, "gout_clk_cmu_otp_clk", "dout_cmu_otp",
	     CLK_CON_GAT_CLK_CMU_OTP_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CMU_APM_BUS, "gout_cmu_apm_bus", "mout_cmu_apm_bus",
	     CLK_CON_GAT_GATE_CLKCMU_APM_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CAM_BUS, "gout_cmu_cam_bus", "mout_cmu_cam_bus",
	     CLK_CON_GAT_GATE_CLKCMU_CAM_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CIS_CLK0, "gout_cmu_cis_clk0", "mout_cmu_cis_clk0",
	     CLK_CON_GAT_GATE_CLKCMU_CIS_CLK0, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CIS_CLK1, "gout_cmu_cis_clk1", "mout_cmu_cis_clk1",
	     CLK_CON_GAT_GATE_CLKCMU_CIS_CLK1, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CIS_CLK2, "gout_cmu_cis_clk2", "mout_cmu_cis_clk2",
	     CLK_CON_GAT_GATE_CLKCMU_CIS_CLK2, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CIS_CLK3, "gout_cmu_cis_clk3", "mout_cmu_cis_clk3",
	     CLK_CON_GAT_GATE_CLKCMU_CIS_CLK3, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CORE_BUS, "gout_cmu_core_bus", "mout_cmu_core_bus",
	     CLK_CON_GAT_GATE_CLKCMU_CORE_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CORE_CCI, "gout_cmu_core_cci", "mout_cmu_core_cci",
	     CLK_CON_GAT_GATE_CLKCMU_CORE_CCI, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CORE_G3D, "gout_cmu_core_g3d", "mout_cmu_core_g3d",
	     CLK_CON_GAT_GATE_CLKCMU_CORE_G3D, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CPUCL0_DBG, "gout_cmu_cpucl0_dbg", "mout_cmu_cpucl0_dbg",
	     CLK_CON_GAT_GATE_CLKCMU_CPUCL0_DBG, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CPUCL0_SWITCH, "gout_cmu_cpucl0_switch", "mout_cmu_cpucl0_switch",
	     CLK_CON_GAT_GATE_CLKCMU_CPUCL0_SWITCH, 21, 0, 0),
	GATE(CLK_GOUT_CMU_CPUCL1_SWITCH, "gout_cmu_cpucl1_switch", "mout_cmu_cpucl1_switch",
	     CLK_CON_GAT_GATE_CLKCMU_CPUCL1_SWITCH, 21, 0, 0),
	GATE(CLK_GOUT_CMU_DISPAUD_AUD, "gout_cmu_dispaud_aud", "mout_cmu_dispaud_aud",
	     CLK_CON_GAT_GATE_CLKCMU_DISPAUD_AUD, 21, 0, 0),
	GATE(CLK_GOUT_CMU_DISPAUD_CPU, "gout_cmu_dispaud_cpu", "mout_cmu_dispaud_cpu",
	     CLK_CON_GAT_GATE_CLKCMU_DISPAUD_CPU, 21, 0, 0),
	GATE(CLK_GOUT_CMU_DISPAUD_DISP, "gout_cmu_dispaud_disp", "mout_cmu_dispaud_disp",
	     CLK_CON_GAT_GATE_CLKCMU_DISPAUD_DISP, 21, 0, 0),
	GATE(CLK_GOUT_CMU_FSYS_BUS, "gout_cmu_fsys_bus", "mout_cmu_fsys_bus",
	     CLK_CON_GAT_GATE_CLKCMU_FSYS_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CMU_FSYS_MMC_CARD, "gout_cmu_fsys_mmc_card", "mout_cmu_fsys_mmc_card",
	     CLK_CON_GAT_GATE_CLKCMU_FSYS_MMC_CARD, 21, 0, 0),
	GATE(CLK_GOUT_CMU_FSYS_MMC_EMBD, "gout_cmu_fsys_mmc_embd", "mout_cmu_fsys_mmc_embd",
	     CLK_CON_GAT_GATE_CLKCMU_FSYS_MMC_EMBD, 21, 0, 0),
	GATE(CLK_GOUT_CMU_FSYS_UFS_EMBD, "gout_cmu_fsys_ufs_embd", "mout_cmu_fsys_ufs_embd",
	     CLK_CON_GAT_GATE_CLKCMU_FSYS_UFS_EMBD, 21, 0, 0),
	GATE(CLK_GOUT_CMU_G2D_G2D, "gout_cmu_g2d_g2d", "mout_cmu_g2d_g2d",
	     CLK_CON_GAT_GATE_CLKCMU_G2D_G2D, 21, 0, 0),
	GATE(CLK_GOUT_CMU_G2D_MSCL, "gout_cmu_g2d_mscl", "mout_cmu_g2d_mscl",
	     CLK_CON_GAT_GATE_CLKCMU_G2D_MSCL, 21, 0, 0),
	GATE(CLK_GOUT_CMU_G3D_SWITCH, "gout_cmu_g3d_switch", "mout_cmu_g3d_switch",
	     CLK_CON_GAT_GATE_CLKCMU_G3D_SWITCH, 21, 0, 0),
	GATE(CLK_GOUT_CMU_HPM, "gout_cmu_hpm", "mout_cmu_hpm",
	     CLK_CON_GAT_GATE_CLKCMU_HPM, 21, 0, 0),
	GATE(CLK_GOUT_CMU_ISP_BUS, "gout_cmu_isp_bus", "mout_cmu_isp_bus",
	     CLK_CON_GAT_GATE_CLKCMU_ISP_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CMU_ISP_GDC, "gout_cmu_isp_gdc", "mout_cmu_isp_gdc",
	     CLK_CON_GAT_GATE_CLKCMU_ISP_GDC, 21, 0, 0),
	GATE(CLK_GOUT_CMU_ISP_VRA, "gout_cmu_isp_vra", "mout_cmu_isp_vra",
	     CLK_CON_GAT_GATE_CLKCMU_ISP_VRA, 21, 0, 0),
	GATE(CLK_GOUT_CMU_MFC_MFC, "gout_cmu_mfc_mfc", "mout_cmu_mfc_mfc",
	     CLK_CON_GAT_GATE_CLKCMU_MFC_MFC, 21, 0, 0),
	GATE(CLK_GOUT_CMU_MFC_WFD, "gout_cmu_mfc_wfd", "mout_cmu_mfc_wfd",
	     CLK_CON_GAT_GATE_CLKCMU_MFC_WFD, 21, 0, 0),
	GATE(CLK_GOUT_CMU_MIF_BUSP, "gout_cmu_mif_busp", "mout_cmu_mif_busp",
	     CLK_CON_GAT_GATE_CLKCMU_MIF_BUSP, 21, 0, 0),
	GATE(CLK_GOUT_CMU_MODEM_SHARED0, "gout_cmu_modem_shared0", "dout_cmu_shared0_div2",
	     CLK_CON_GAT_GATE_CLKCMU_MODEM_SHARED0, 21, 0, 0),
	GATE(CLK_GOUT_CMU_MODEM_SHARED1, "gout_cmu_modem_shared1", "dout_cmu_shared1_div2",
	     CLK_CON_GAT_GATE_CLKCMU_MODEM_SHARED1, 21, 0, 0),
	GATE(CLK_GOUT_CMU_PERI_BUS, "gout_cmu_peri_bus", "mout_cmu_peri_bus",
	     CLK_CON_GAT_GATE_CLKCMU_PERI_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CMU_PERI_IP, "gout_cmu_peri_ip", "mout_cmu_peri_ip",
	     CLK_CON_GAT_GATE_CLKCMU_PERI_IP, 21, 0, 0),
	GATE(CLK_GOUT_CMU_PERI_UART, "gout_cmu_peri_uart", "mout_cmu_peri_uart",
	     CLK_CON_GAT_GATE_CLKCMU_PERI_UART, 21, 0, 0),
	GATE(CLK_GOUT_CMU_USB_BUS, "gout_cmu_usb_bus", "mout_cmu_usb_bus",
	     CLK_CON_GAT_GATE_CLKCMU_USB_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CMU_USB_DPGTC, "gout_cmu_usb_dpgtc", "mout_cmu_usb_dpgtc",
	     CLK_CON_GAT_GATE_CLKCMU_USB_DPGTC, 21, 0, 0),
	GATE(CLK_GOUT_CMU_USB_USB30DRD, "gout_cmu_usb_usb30drd", "mout_cmu_usb_usb30drd",
	     CLK_CON_GAT_GATE_CLKCMU_USB_USB30DRD, 21, 0, 0),
	GATE(CLK_GOUT_CMU_VIPX1_BUS, "gout_cmu_vipx1_bus", "mout_cmu_vipx1_bus",
	     CLK_CON_GAT_GATE_CLKCMU_VIPX1_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CMU_VIPX2_BUS, "gout_cmu_vipx2_bus", "mout_cmu_vipx2_bus",
	     CLK_CON_GAT_GATE_CLKCMU_VIPX2_BUS, 21, 0, 0),
};

static const struct samsung_cmu_info top_cmu_info __initconst = {
	.pll_clks		= top_pll_clks,
	.nr_pll_clks		= ARRAY_SIZE(top_pll_clks),
	.mux_clks		= top_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(top_mux_clks),
	.div_clks		= top_div_clks,
	.nr_div_clks		= ARRAY_SIZE(top_div_clks),
	.fixed_factor_clks	= top_ffactor_clks,
	.nr_fixed_factor_clks	= ARRAY_SIZE(top_ffactor_clks),
	.gate_clks		= top_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(top_gate_clks),
	.fixed_clks		= top_fixed_clks,
	.nr_fixed_clks		= ARRAY_SIZE(top_fixed_clks),
	.nr_clk_ids		= CLKS_NR_TOP,
	.clk_regs		= top_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(top_clk_regs),
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_TOP_CONTROLLER_OPTION,
};

static void __init exynos9610_cmu_top_init(struct device_node *np)
{
	exynos_arm64_register_cmu(NULL, np, &top_cmu_info);
}

/* Register CMU_TOP early, as it's a dependency for other early domains */
CLK_OF_DECLARE(exynos9610_cmu_top, "samsung,exynos9610-cmu-top",
	       exynos9610_cmu_top_init);

static const unsigned long drcg_memclk_sysreg[] __initconst = {
	EXYNOS9610_DRCG_EN_OFFSET,
	EXYNOS9610_MEMCLK_OFFSET,
};

static const unsigned long drcg_cam_memclk_sysreg[] __initconst = {
	EXYNOS9610_MEMCLK_OFFSET,
	EXYNOS9610_CAM_DRCG_EN_OFFSET,
};

/* CMU_CPUCL0 */
#define PLL_LOCKTIME_PLL_CPUCL0					0x0000
#define PLL_CON0_MUX_CLKCMU_CPUCL0_DBG_USER			0x0100
#define PLL_CON2_MUX_CLKCMU_CPUCL0_DBG_USER			0x0108
#define PLL_CON0_MUX_CLKCMU_CPUCL0_SWITCH_USER			0x0120
#define PLL_CON2_MUX_CLKCMU_CPUCL0_SWITCH_USER			0x0128
#define PLL_CON0_PLL_CPUCL0					0x0140
#define CMU_CMU_CPUCL0_CONTROLLER_OPTION			0x0800
#define CMU_EMBEDDED_CMU_CPUCL0_CONTROLLER_OPTION		0x0804
#define CLK_CON_MUX_MUX_CLK_CPUCL0_PLL				0x1000
#define CLK_CON_DIV_DIV_CLK_CLUSTER0_ACLK			0x1800
#define CLK_CON_DIV_DIV_CLK_CLUSTER0_CNTCLK			0x1804
#define CLK_CON_DIV_DIV_CLK_CLUSTER0_PCLKDBG			0x1808
#define CLK_CON_DIV_DIV_CLK_CPUCL0_CMUREF			0x180c
#define CLK_CON_DIV_DIV_CLK_CPUCL0_CPU				0x1810
#define CLK_CON_DIV_DIV_CLK_CPUCL0_PCLK				0x1814
#define CLK_CON_GAT_CLK_CPUCL0_CMU_PCLK				0x2000
#define CLK_CON_GAT_CLK_CPUCL0_HPM_TARGETCLK_C			0x2004
#define CLK_CON_GAT_CLK_CPUCL0_OSCCLK_CLK			0x2008
#define CLK_CON_GAT_GATE_CLK_CLUSTER0_CPU			0x2010
#define CLK_CON_GAT_GOUT_CPUCL0_ADM_APB_G_CSSYS_CORE_PCLKM	0x2014
#define CLK_CON_GAT_GOUT_CPUCL0_ADS_AHB_G_CSSYS_FSYS_HCLKS	0x2018
#define CLK_CON_GAT_GOUT_CPUCL0_ADS_APB_G_CSSYS_CPUCL1_PCLKS	0x201c
#define CLK_CON_GAT_GOUT_CPUCL0_ADS_APB_G_P8Q_PCLKS		0x2020
#define CLK_CON_GAT_GOUT_CPUCL0_AD_APB_P_DUMP_PC_CPUCL0_PCLKM	0x2024
#define CLK_CON_GAT_GOUT_CPUCL0_AD_APB_P_DUMP_PC_CPUCL1_PCLKM	0x2028
#define CLK_CON_GAT_GOUT_CPUCL0_BUSIF_HPMCPUCL0_PCLK		0x202c
#define CLK_CON_GAT_GOUT_CPUCL0_CSSYS_DBG_PCLKDBG		0x2030
#define CLK_CON_GAT_GOUT_CPUCL0_DUMP_PC_CPUCL0_PCLK		0x2034
#define CLK_CON_GAT_GOUT_CPUCL0_DUMP_PC_CPUCL1_PCLK		0x2038
#define CLK_CON_GAT_GOUT_CPUCL0_LHM_AXI_P_CPUCL0_CLK		0x203c
#define CLK_CON_GAT_GOUT_CPUCL0_LHS_AXI_D_CSSYS_CLK		0x2040
#define CLK_CON_GAT_GOUT_CPUCL0_DBG_CLK				0x2044
#define CLK_CON_GAT_GOUT_CPUCL0_PCLK_CLK			0x2048
#define CLK_CON_GAT_GOUT_CPUCL0_SECJTAG_CLK			0x204c
#define CLK_CON_GAT_GOUT_CPUCL0_SYSREG_PCLK			0x2050

static const unsigned long cpucl0_clk_regs[] __initconst = {
	PLL_LOCKTIME_PLL_CPUCL0,
	PLL_CON0_MUX_CLKCMU_CPUCL0_DBG_USER,
	PLL_CON2_MUX_CLKCMU_CPUCL0_DBG_USER,
	PLL_CON0_MUX_CLKCMU_CPUCL0_SWITCH_USER,
	PLL_CON2_MUX_CLKCMU_CPUCL0_SWITCH_USER,
	PLL_CON0_PLL_CPUCL0,
	CMU_CMU_CPUCL0_CONTROLLER_OPTION,
	CMU_EMBEDDED_CMU_CPUCL0_CONTROLLER_OPTION,
	CLK_CON_MUX_MUX_CLK_CPUCL0_PLL,
	CLK_CON_DIV_DIV_CLK_CLUSTER0_ACLK,
	CLK_CON_DIV_DIV_CLK_CLUSTER0_CNTCLK,
	CLK_CON_DIV_DIV_CLK_CLUSTER0_PCLKDBG,
	CLK_CON_DIV_DIV_CLK_CPUCL0_CMUREF,
	CLK_CON_DIV_DIV_CLK_CPUCL0_CPU,
	CLK_CON_DIV_DIV_CLK_CPUCL0_PCLK,
	CLK_CON_GAT_CLK_CPUCL0_CMU_PCLK,
	CLK_CON_GAT_CLK_CPUCL0_HPM_TARGETCLK_C,
	CLK_CON_GAT_CLK_CPUCL0_OSCCLK_CLK,
	CLK_CON_GAT_GATE_CLK_CLUSTER0_CPU,
	CLK_CON_GAT_GOUT_CPUCL0_ADM_APB_G_CSSYS_CORE_PCLKM,
	CLK_CON_GAT_GOUT_CPUCL0_ADS_AHB_G_CSSYS_FSYS_HCLKS,
	CLK_CON_GAT_GOUT_CPUCL0_ADS_APB_G_CSSYS_CPUCL1_PCLKS,
	CLK_CON_GAT_GOUT_CPUCL0_ADS_APB_G_P8Q_PCLKS,
	CLK_CON_GAT_GOUT_CPUCL0_AD_APB_P_DUMP_PC_CPUCL0_PCLKM,
	CLK_CON_GAT_GOUT_CPUCL0_AD_APB_P_DUMP_PC_CPUCL1_PCLKM,
	CLK_CON_GAT_GOUT_CPUCL0_BUSIF_HPMCPUCL0_PCLK,
	CLK_CON_GAT_GOUT_CPUCL0_CSSYS_DBG_PCLKDBG,
	CLK_CON_GAT_GOUT_CPUCL0_DUMP_PC_CPUCL0_PCLK,
	CLK_CON_GAT_GOUT_CPUCL0_DUMP_PC_CPUCL1_PCLK,
	CLK_CON_GAT_GOUT_CPUCL0_LHM_AXI_P_CPUCL0_CLK,
	CLK_CON_GAT_GOUT_CPUCL0_LHS_AXI_D_CSSYS_CLK,
	CLK_CON_GAT_GOUT_CPUCL0_DBG_CLK,
	CLK_CON_GAT_GOUT_CPUCL0_PCLK_CLK,
	CLK_CON_GAT_GOUT_CPUCL0_SECJTAG_CLK,
	CLK_CON_GAT_GOUT_CPUCL0_SYSREG_PCLK,
};

static const struct samsung_pll_rate_table pll_cpucl0_rate_table[] __initconst = {
	PLL_35XX_RATE(26 * MHZ, 1049750000U, 323, 4, 1),
	PLL_35XX_RATE(26 * MHZ, 1449500000U, 223, 4, 0),
	PLL_35XX_RATE(26 * MHZ, 1850333333U, 427, 6, 0),
	PLL_35XX_RATE(26 * MHZ, 300083333U, 277, 6, 2),
	PLL_35XX_RATE(26 * MHZ, 600166666U, 277, 6, 1),
};

static const struct samsung_pll_clock cpucl0_pll_clks[] __initconst = {
	PLL(pll_1051x, CLK_FOUT_CPUCL0_PLL, "fout_cpucl0_pll", "oscclk",
	    PLL_LOCKTIME_PLL_CPUCL0, PLL_CON0_PLL_CPUCL0, pll_cpucl0_rate_table),
};

PNAME(mout_pll_cpucl0_dbg_user_p)	= { "oscclk",
					    "dout_cmu_cpucl0_dbg" };
PNAME(mout_pll_cpucl0_switch_user_p)	= { "oscclk",
					    "dout_cmu_cpucl0_switch" };
PNAME(mout_clk_cpucl0_pll_p)		= { "fout_cpucl0_pll",
					    "mout_pll_cpucl0_switch_user" };

static const struct samsung_mux_clock cpucl0_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_CPUCL0_DBG_USER, "mout_pll_cpucl0_dbg_user", mout_pll_cpucl0_dbg_user_p,
	    PLL_CON0_MUX_CLKCMU_CPUCL0_DBG_USER, 4, 1),
	MUX(CLK_MOUT_PLL_CPUCL0_SWITCH_USER, "mout_pll_cpucl0_switch_user",
	    mout_pll_cpucl0_switch_user_p, PLL_CON0_MUX_CLKCMU_CPUCL0_SWITCH_USER, 4, 1),
	MUX(CLK_MOUT_CLK_CPUCL0_PLL, "mout_clk_cpucl0_pll", mout_clk_cpucl0_pll_p,
	    CLK_CON_MUX_MUX_CLK_CPUCL0_PLL, 0, 1),
};

static const struct samsung_div_clock cpucl0_div_clks[] __initconst = {
	DIV(CLK_DOUT_CLK_CLUSTER0_ACLK, "dout_clk_cluster0_aclk", "gout_clk_cluster0_cpu",
	    CLK_CON_DIV_DIV_CLK_CLUSTER0_ACLK, 0, 4),
	DIV(CLK_DOUT_CLK_CLUSTER0_CNTCLK, "dout_clk_cluster0_cntclk", "gout_clk_cluster0_cpu",
	    CLK_CON_DIV_DIV_CLK_CLUSTER0_CNTCLK, 0, 4),
	DIV(CLK_DOUT_CLK_CLUSTER0_PCLKDBG, "dout_clk_cluster0_pclkdbg", "gout_clk_cluster0_cpu",
	    CLK_CON_DIV_DIV_CLK_CLUSTER0_PCLKDBG, 0, 4),
	DIV(CLK_DOUT_CLK_CPUCL0_CMUREF, "dout_clk_cpucl0_cmuref", "dout_clk_cpucl0_cpu",
	    CLK_CON_DIV_DIV_CLK_CPUCL0_CMUREF, 0, 3),
	DIV(CLK_DOUT_CLK_CPUCL0_CPU, "dout_clk_cpucl0_cpu", "mout_clk_cpucl0_pll",
	    CLK_CON_DIV_DIV_CLK_CPUCL0_CPU, 0, 0),
	DIV(CLK_DOUT_CLK_CPUCL0_PCLK, "dout_clk_cpucl0_pclk", "dout_clk_cpucl0_cpu",
	    CLK_CON_DIV_DIV_CLK_CPUCL0_PCLK, 0, 4),
};

static const struct samsung_gate_clock cpucl0_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CLK_CPUCL0_CMU_PCLK, "gout_clk_cpucl0_cmu_pclk",
	     "dout_clk_cpucl0_pclk", CLK_CON_GAT_CLK_CPUCL0_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_CPUCL0_HPM_TARGETCLK_C, "gout_clk_cpucl0_hpm_targetclk_c",
	     "dout_cmu_hpm", CLK_CON_GAT_CLK_CPUCL0_HPM_TARGETCLK_C, 21, 0, 0),
	GATE(CLK_GOUT_CLK_CPUCL0_OSCCLK_CLK, "gout_clk_cpucl0_oscclk_clk",
	     "oscclk", CLK_CON_GAT_CLK_CPUCL0_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_CLUSTER0_CPU, "gout_clk_cluster0_cpu",
	     "dout_clk_cpucl0_cpu", CLK_CON_GAT_GATE_CLK_CLUSTER0_CPU, 21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_ADM_APB_G_CSSYS_CORE_PCLKM, "gout_cpucl0_adm_apb_g_cssys_core_pclkm",
	     "mout_pll_cpucl0_dbg_user", CLK_CON_GAT_GOUT_CPUCL0_ADM_APB_G_CSSYS_CORE_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_ADS_AHB_G_CSSYS_FSYS_HCLKS, "gout_cpucl0_ads_ahb_g_cssys_fsys_hclks",
	     "mout_pll_cpucl0_dbg_user", CLK_CON_GAT_GOUT_CPUCL0_ADS_AHB_G_CSSYS_FSYS_HCLKS,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_ADS_APB_G_CSSYS_CPUCL1_PCLKS,
	     "gout_cpucl0_ads_apb_g_cssys_cpucl1_pclks", "mout_pll_cpucl0_dbg_user",
	     CLK_CON_GAT_GOUT_CPUCL0_ADS_APB_G_CSSYS_CPUCL1_PCLKS, 21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_ADS_APB_G_P8Q_PCLKS, "gout_cpucl0_ads_apb_g_p8q_pclks",
	     "mout_pll_cpucl0_dbg_user", CLK_CON_GAT_GOUT_CPUCL0_ADS_APB_G_P8Q_PCLKS,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_AD_APB_P_DUMP_PC_CPUCL0_PCLKM,
	     "gout_cpucl0_ad_apb_p_dump_pc_cpucl0_pclkm", "mout_pll_cpucl0_dbg_user",
	     CLK_CON_GAT_GOUT_CPUCL0_AD_APB_P_DUMP_PC_CPUCL0_PCLKM, 21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_AD_APB_P_DUMP_PC_CPUCL1_PCLKM,
	     "gout_cpucl0_ad_apb_p_dump_pc_cpucl1_pclkm", "mout_pll_cpucl0_dbg_user",
	     CLK_CON_GAT_GOUT_CPUCL0_AD_APB_P_DUMP_PC_CPUCL1_PCLKM, 21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_BUSIF_HPMCPUCL0_PCLK, "gout_cpucl0_busif_hpmcpucl0_pclk",
	     "dout_clk_cpucl0_pclk", CLK_CON_GAT_GOUT_CPUCL0_BUSIF_HPMCPUCL0_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_CSSYS_DBG_PCLKDBG, "gout_cpucl0_cssys_dbg_pclkdbg",
	     "mout_pll_cpucl0_dbg_user", CLK_CON_GAT_GOUT_CPUCL0_CSSYS_DBG_PCLKDBG,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_DUMP_PC_CPUCL0_PCLK, "gout_cpucl0_dump_pc_cpucl0_pclk",
	     "mout_pll_cpucl0_dbg_user", CLK_CON_GAT_GOUT_CPUCL0_DUMP_PC_CPUCL0_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_DUMP_PC_CPUCL1_PCLK, "gout_cpucl0_dump_pc_cpucl1_pclk",
	     "mout_pll_cpucl0_dbg_user", CLK_CON_GAT_GOUT_CPUCL0_DUMP_PC_CPUCL1_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_LHM_AXI_P_CPUCL0_CLK, "gout_cpucl0_lhm_axi_p_cpucl0_clk",
	     "dout_clk_cpucl0_pclk", CLK_CON_GAT_GOUT_CPUCL0_LHM_AXI_P_CPUCL0_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_LHS_AXI_D_CSSYS_CLK, "gout_cpucl0_lhs_axi_d_cssys_clk",
	     "mout_pll_cpucl0_dbg_user", CLK_CON_GAT_GOUT_CPUCL0_LHS_AXI_D_CSSYS_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_DBG_CLK, "gout_cpucl0_dbg_clk",
	     "mout_pll_cpucl0_dbg_user", CLK_CON_GAT_GOUT_CPUCL0_DBG_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_PCLK_CLK, "gout_cpucl0_pclk_clk",
	     "dout_clk_cpucl0_pclk", CLK_CON_GAT_GOUT_CPUCL0_PCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_SECJTAG_CLK, "gout_cpucl0_secjtag_clk", "mout_pll_cpucl0_dbg_user",
	     CLK_CON_GAT_GOUT_CPUCL0_SECJTAG_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CPUCL0_SYSREG_PCLK, "gout_cpucl0_sysreg_pclk",
	     "dout_clk_cpucl0_pclk", CLK_CON_GAT_GOUT_CPUCL0_SYSREG_PCLK, 21, 0, 0),
};

static const struct samsung_cmu_info cpucl0_cmu_info __initconst = {
	.pll_clks		= cpucl0_pll_clks,
	.nr_pll_clks		= ARRAY_SIZE(cpucl0_pll_clks),
	.mux_clks		= cpucl0_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(cpucl0_mux_clks),
	.div_clks		= cpucl0_div_clks,
	.nr_div_clks		= ARRAY_SIZE(cpucl0_div_clks),
	.gate_clks		= cpucl0_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(cpucl0_gate_clks),
	.nr_clk_ids		= CLKS_NR_CPUCL0,
	.clk_regs		= cpucl0_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(cpucl0_clk_regs),
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_CPUCL0_CONTROLLER_OPTION,
};

static void __init exynos9610_cmu_cpucl0_init(struct device_node *np)
{
	exynos_arm64_register_cmu(NULL, np, &cpucl0_cmu_info);
}

CLK_OF_DECLARE(exynos9610_cmu_cpucl0, "samsung,exynos9610-cmu-cpucl0",
	       exynos9610_cmu_cpucl0_init);

/* CMU_PERI */
#define PLL_CON0_MUX_CLKCMU_PERI_BUS_USER			0x0100
#define PLL_CON2_MUX_CLKCMU_PERI_BUS_USER			0x0108
#define PLL_CON0_MUX_CLKCMU_PERI_IP_USER			0x0120
#define PLL_CON2_MUX_CLKCMU_PERI_IP_USER			0x0128
#define PLL_CON0_MUX_CLKCMU_PERI_UART_USER			0x0140
#define PLL_CON2_MUX_CLKCMU_PERI_UART_USER			0x0148
#define CMU_CMU_PERI_CONTROLLER_OPTION				0x0800
#define CLK_CON_DIV_DIV_CLK_PERI_I2C				0x1800
#define CLK_CON_DIV_DIV_CLK_PERI_SPI0				0x1804
#define CLK_CON_DIV_DIV_CLK_PERI_SPI1				0x1808
#define CLK_CON_DIV_DIV_CLK_PERI_SPI2				0x180c
#define CLK_CON_DIV_DIV_CLK_PERI_USI_I2C			0x1814
#define CLK_CON_DIV_DIV_CLK_PERI_USI_USI			0x1818
#define CLK_CON_GAT_CLK_PERI_CMU_PCLK				0x2000
#define CLK_CON_GAT_CLK_PERI_OSCCLK_CLK				0x2004
#define CLK_CON_GAT_GATE_CLK_PERI_I2C				0x2008
#define CLK_CON_GAT_GATE_CLK_PERI_SPI0				0x200c
#define CLK_CON_GAT_GATE_CLK_PERI_SPI1				0x2010
#define CLK_CON_GAT_GATE_CLK_PERI_SPI2				0x2014
#define CLK_CON_GAT_GATE_CLK_PERI_USI_I2C			0x2018
#define CLK_CON_GAT_GATE_CLK_PERI_USI_USI			0x201c
#define CLK_CON_GAT_GOUT_PERI_AXI2AHB_MSD32_ACLK		0x2020
#define CLK_CON_GAT_GOUT_PERI_BUSIF_TMU_PCLK			0x2024
#define CLK_CON_GAT_GOUT_PERI_CAMI2C_0_IPCLK			0x2028
#define CLK_CON_GAT_GOUT_PERI_CAMI2C_0_PCLK			0x202c
#define CLK_CON_GAT_GOUT_PERI_CAMI2C_1_IPCLK			0x2030
#define CLK_CON_GAT_GOUT_PERI_CAMI2C_1_PCLK			0x2034
#define CLK_CON_GAT_GOUT_PERI_CAMI2C_2_IPCLK			0x2038
#define CLK_CON_GAT_GOUT_PERI_CAMI2C_2_PCLK			0x203c
#define CLK_CON_GAT_GOUT_PERI_CAMI2C_3_IPCLK			0x2040
#define CLK_CON_GAT_GOUT_PERI_CAMI2C_3_PCLK			0x2044
#define CLK_CON_GAT_GOUT_PERI_I2C_0_PCLK			0x204c
#define CLK_CON_GAT_GOUT_PERI_I2C_1_PCLK			0x2050
#define CLK_CON_GAT_GOUT_PERI_I2C_2_PCLK			0x2054
#define CLK_CON_GAT_GOUT_PERI_I2C_3_PCLK			0x2058
#define CLK_CON_GAT_GOUT_PERI_I2C_4_PCLK			0x205c
#define CLK_CON_GAT_GOUT_PERI_I2C_5_PCLK			0x2060
#define CLK_CON_GAT_GOUT_PERI_I2C_6_PCLK			0x2064
#define CLK_CON_GAT_GOUT_PERI_GPIO_PCLK				0x2048
#define CLK_CON_GAT_GOUT_PERI_LHM_AXI_P_CLK			0x2068
#define CLK_CON_GAT_GOUT_PERI_MCT_PCLK				0x206c
#define CLK_CON_GAT_GOUT_PERI_OTP_CON_TOP_PCLK			0x2070
#define CLK_CON_GAT_GOUT_PERI_PWM_MOTOR_PCLK_S0			0x2074
#define CLK_CON_GAT_GOUT_PERI_BUS_CLK				0x2078
#define CLK_CON_GAT_GOUT_PERI_I2C_CLK				0x207c
#define CLK_CON_GAT_GOUT_PERI_SPI_0_CLK				0x2080
#define CLK_CON_GAT_GOUT_PERI_SPI_1_CLK				0x2084
#define CLK_CON_GAT_GOUT_PERI_SPI_2_CLK				0x2088
#define CLK_CON_GAT_GOUT_PERI_UART_CLK				0x208c
#define CLK_CON_GAT_GOUT_PERI_USI00_I2C_CLK			0x2090
#define CLK_CON_GAT_GOUT_PERI_USI00_USI_CLK			0x2094
#define CLK_CON_GAT_GOUT_PERI_SPI_0_IPCLK			0x2098
#define CLK_CON_GAT_GOUT_PERI_SPI_0_PCLK			0x209c
#define CLK_CON_GAT_GOUT_PERI_SPI_1_IPCLK			0x20a0
#define CLK_CON_GAT_GOUT_PERI_SPI_1_PCLK			0x20a4
#define CLK_CON_GAT_GOUT_PERI_SPI_2_IPCLK			0x20a8
#define CLK_CON_GAT_GOUT_PERI_SPI_2_PCLK			0x20ac
#define CLK_CON_GAT_GOUT_PERI_SYSREG_PCLK			0x20b0
#define CLK_CON_GAT_GOUT_PERI_UART_IPCLK			0x20b4
#define CLK_CON_GAT_GOUT_PERI_UART_PCLK				0x20b8
#define CLK_CON_GAT_GOUT_PERI_USI00_I2C_IPCLK			0x20bc
#define CLK_CON_GAT_GOUT_PERI_USI00_I2C_PCLK			0x20c0
#define CLK_CON_GAT_GOUT_PERI_USI00_USI_IPCLK			0x20c4
#define CLK_CON_GAT_GOUT_PERI_USI00_USI_PCLK			0x20c8
#define CLK_CON_GAT_GOUT_PERI_WDT_CLUSTER0_PCLK			0x20cc
#define CLK_CON_GAT_GOUT_PERI_WDT_CLUSTER1_PCLK			0x20d0

static const unsigned long peri_clk_regs[] __initconst = {
	PLL_CON0_MUX_CLKCMU_PERI_BUS_USER,
	PLL_CON2_MUX_CLKCMU_PERI_BUS_USER,
	PLL_CON0_MUX_CLKCMU_PERI_IP_USER,
	PLL_CON2_MUX_CLKCMU_PERI_IP_USER,
	PLL_CON0_MUX_CLKCMU_PERI_UART_USER,
	PLL_CON2_MUX_CLKCMU_PERI_UART_USER,
	CMU_CMU_PERI_CONTROLLER_OPTION,
	CLK_CON_DIV_DIV_CLK_PERI_I2C,
	CLK_CON_DIV_DIV_CLK_PERI_SPI0,
	CLK_CON_DIV_DIV_CLK_PERI_SPI1,
	CLK_CON_DIV_DIV_CLK_PERI_SPI2,
	CLK_CON_DIV_DIV_CLK_PERI_USI_I2C,
	CLK_CON_DIV_DIV_CLK_PERI_USI_USI,
	CLK_CON_GAT_CLK_PERI_CMU_PCLK,
	CLK_CON_GAT_CLK_PERI_OSCCLK_CLK,
	CLK_CON_GAT_GATE_CLK_PERI_I2C,
	CLK_CON_GAT_GATE_CLK_PERI_SPI0,
	CLK_CON_GAT_GATE_CLK_PERI_SPI1,
	CLK_CON_GAT_GATE_CLK_PERI_SPI2,
	CLK_CON_GAT_GATE_CLK_PERI_USI_I2C,
	CLK_CON_GAT_GATE_CLK_PERI_USI_USI,
	CLK_CON_GAT_GOUT_PERI_AXI2AHB_MSD32_ACLK,
	CLK_CON_GAT_GOUT_PERI_BUSIF_TMU_PCLK,
	CLK_CON_GAT_GOUT_PERI_CAMI2C_0_IPCLK,
	CLK_CON_GAT_GOUT_PERI_CAMI2C_0_PCLK,
	CLK_CON_GAT_GOUT_PERI_CAMI2C_1_IPCLK,
	CLK_CON_GAT_GOUT_PERI_CAMI2C_1_PCLK,
	CLK_CON_GAT_GOUT_PERI_CAMI2C_2_IPCLK,
	CLK_CON_GAT_GOUT_PERI_CAMI2C_2_PCLK,
	CLK_CON_GAT_GOUT_PERI_CAMI2C_3_IPCLK,
	CLK_CON_GAT_GOUT_PERI_CAMI2C_3_PCLK,
	CLK_CON_GAT_GOUT_PERI_I2C_0_PCLK,
	CLK_CON_GAT_GOUT_PERI_I2C_1_PCLK,
	CLK_CON_GAT_GOUT_PERI_I2C_2_PCLK,
	CLK_CON_GAT_GOUT_PERI_I2C_3_PCLK,
	CLK_CON_GAT_GOUT_PERI_I2C_4_PCLK,
	CLK_CON_GAT_GOUT_PERI_I2C_5_PCLK,
	CLK_CON_GAT_GOUT_PERI_I2C_6_PCLK,
	CLK_CON_GAT_GOUT_PERI_GPIO_PCLK,
	CLK_CON_GAT_GOUT_PERI_LHM_AXI_P_CLK,
	CLK_CON_GAT_GOUT_PERI_MCT_PCLK,
	CLK_CON_GAT_GOUT_PERI_OTP_CON_TOP_PCLK,
	CLK_CON_GAT_GOUT_PERI_PWM_MOTOR_PCLK_S0,
	CLK_CON_GAT_GOUT_PERI_BUS_CLK,
	CLK_CON_GAT_GOUT_PERI_I2C_CLK,
	CLK_CON_GAT_GOUT_PERI_SPI_0_CLK,
	CLK_CON_GAT_GOUT_PERI_SPI_1_CLK,
	CLK_CON_GAT_GOUT_PERI_SPI_2_CLK,
	CLK_CON_GAT_GOUT_PERI_UART_CLK,
	CLK_CON_GAT_GOUT_PERI_USI00_I2C_CLK,
	CLK_CON_GAT_GOUT_PERI_USI00_USI_CLK,
	CLK_CON_GAT_GOUT_PERI_SPI_0_IPCLK,
	CLK_CON_GAT_GOUT_PERI_SPI_0_PCLK,
	CLK_CON_GAT_GOUT_PERI_SPI_1_IPCLK,
	CLK_CON_GAT_GOUT_PERI_SPI_1_PCLK,
	CLK_CON_GAT_GOUT_PERI_SPI_2_IPCLK,
	CLK_CON_GAT_GOUT_PERI_SPI_2_PCLK,
	CLK_CON_GAT_GOUT_PERI_SYSREG_PCLK,
	CLK_CON_GAT_GOUT_PERI_UART_IPCLK,
	CLK_CON_GAT_GOUT_PERI_UART_PCLK,
	CLK_CON_GAT_GOUT_PERI_USI00_I2C_IPCLK,
	CLK_CON_GAT_GOUT_PERI_USI00_I2C_PCLK,
	CLK_CON_GAT_GOUT_PERI_USI00_USI_IPCLK,
	CLK_CON_GAT_GOUT_PERI_USI00_USI_PCLK,
	CLK_CON_GAT_GOUT_PERI_WDT_CLUSTER0_PCLK,
	CLK_CON_GAT_GOUT_PERI_WDT_CLUSTER1_PCLK,
};

PNAME(mout_pll_peri_bus_user_p)		= { "oscclk", "dout_cmu_peri_bus" };
PNAME(mout_pll_peri_ip_user_p)		= { "oscclk", "dout_cmu_peri_ip" };
PNAME(mout_pll_peri_uart_user_p)	= { "oscclk", "dout_cmu_peri_uart" };

static const struct samsung_mux_clock peri_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_PERI_BUS_USER, "mout_pll_peri_bus_user", mout_pll_peri_bus_user_p,
	    PLL_CON0_MUX_CLKCMU_PERI_BUS_USER, 4, 1),
	MUX(CLK_MOUT_PLL_PERI_IP_USER, "mout_pll_peri_ip_user", mout_pll_peri_ip_user_p,
	    PLL_CON0_MUX_CLKCMU_PERI_IP_USER, 4, 1),
	MUX(CLK_MOUT_PLL_PERI_UART_USER, "mout_pll_peri_uart_user", mout_pll_peri_uart_user_p,
	    PLL_CON0_MUX_CLKCMU_PERI_UART_USER, 4, 1),
};

static const struct samsung_div_clock peri_div_clks[] __initconst = {
	DIV(CLK_DOUT_CLK_PERI_I2C, "dout_clk_peri_i2c", "gout_clk_peri_i2c",
	    CLK_CON_DIV_DIV_CLK_PERI_I2C, 0, 8),
	DIV(CLK_DOUT_CLK_PERI_SPI0, "dout_clk_peri_spi0", "gout_clk_peri_spi0",
	    CLK_CON_DIV_DIV_CLK_PERI_SPI0, 0, 8),
	DIV(CLK_DOUT_CLK_PERI_SPI1, "dout_clk_peri_spi1", "gout_clk_peri_spi1",
	    CLK_CON_DIV_DIV_CLK_PERI_SPI1, 0, 8),
	DIV(CLK_DOUT_CLK_PERI_SPI2, "dout_clk_peri_spi2", "gout_clk_peri_spi2",
	    CLK_CON_DIV_DIV_CLK_PERI_SPI2, 0, 8),
	DIV(CLK_DOUT_CLK_PERI_USI_I2C, "dout_clk_peri_usi_i2c", "gout_clk_peri_usi_i2c",
	    CLK_CON_DIV_DIV_CLK_PERI_USI_I2C, 0, 4),
	DIV(CLK_DOUT_CLK_PERI_USI_USI, "dout_clk_peri_usi_usi", "gout_clk_peri_usi_usi",
	    CLK_CON_DIV_DIV_CLK_PERI_USI_USI, 0, 8),
};

static const struct samsung_gate_clock peri_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CLK_PERI_I2C, "gout_clk_peri_i2c", "mout_pll_peri_ip_user",
	     CLK_CON_GAT_GATE_CLK_PERI_I2C, 21, 0, 0),
	GATE(CLK_GOUT_CLK_PERI_SPI0, "gout_clk_peri_spi0", "mout_pll_peri_ip_user",
	     CLK_CON_GAT_GATE_CLK_PERI_SPI0, 21, 0, 0),
	GATE(CLK_GOUT_CLK_PERI_SPI1, "gout_clk_peri_spi1", "mout_pll_peri_ip_user",
	     CLK_CON_GAT_GATE_CLK_PERI_SPI1, 21, 0, 0),
	GATE(CLK_GOUT_CLK_PERI_SPI2, "gout_clk_peri_spi2", "mout_pll_peri_ip_user",
	     CLK_CON_GAT_GATE_CLK_PERI_SPI2, 21, 0, 0),
	GATE(CLK_GOUT_CLK_PERI_USI_I2C, "gout_clk_peri_usi_i2c", "mout_pll_peri_ip_user",
	     CLK_CON_GAT_GATE_CLK_PERI_USI_I2C, 21, 0, 0),
	GATE(CLK_GOUT_CLK_PERI_USI_USI, "gout_clk_peri_usi_usi", "mout_pll_peri_ip_user",
	     CLK_CON_GAT_GATE_CLK_PERI_USI_USI, 21, 0, 0),
	GATE(CLK_GOUT_PERI_AXI2AHB_MSD32_ACLK, "gout_peri_axi2ahb_msd32_aclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_AXI2AHB_MSD32_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_BUSIF_TMU_PCLK, "gout_peri_busif_tmu_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_BUSIF_TMU_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_CAMI2C_0_IPCLK, "gout_peri_cami2c_0_ipclk",
	     "dout_clk_peri_i2c", CLK_CON_GAT_GOUT_PERI_CAMI2C_0_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_CAMI2C_0_PCLK, "gout_peri_cami2c_0_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_CAMI2C_0_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_CAMI2C_1_IPCLK, "gout_peri_cami2c_1_ipclk",
	     "dout_clk_peri_i2c", CLK_CON_GAT_GOUT_PERI_CAMI2C_1_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_CAMI2C_1_PCLK, "gout_peri_cami2c_1_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_CAMI2C_1_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_CAMI2C_2_IPCLK, "gout_peri_cami2c_2_ipclk",
	     "dout_clk_peri_i2c", CLK_CON_GAT_GOUT_PERI_CAMI2C_2_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_CAMI2C_2_PCLK, "gout_peri_cami2c_2_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_CAMI2C_2_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_CAMI2C_3_IPCLK, "gout_peri_cami2c_3_ipclk",
	     "dout_clk_peri_i2c", CLK_CON_GAT_GOUT_PERI_CAMI2C_3_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_CAMI2C_3_PCLK, "gout_peri_cami2c_3_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_CAMI2C_3_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_I2C_0_PCLK, "gout_peri_i2c_0_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_I2C_0_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_I2C_1_PCLK, "gout_peri_i2c_1_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_I2C_1_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_I2C_2_PCLK, "gout_peri_i2c_2_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_I2C_2_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_I2C_3_PCLK, "gout_peri_i2c_3_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_I2C_3_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_I2C_4_PCLK, "gout_peri_i2c_4_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_I2C_4_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_I2C_5_PCLK, "gout_peri_i2c_5_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_I2C_5_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_I2C_6_PCLK, "gout_peri_i2c_6_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_I2C_6_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_GPIO_PCLK, "gout_peri_gpio_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_GPIO_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_LHM_AXI_P_CLK, "gout_peri_lhm_axi_p_clk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_LHM_AXI_P_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_MCT_PCLK, "gout_peri_mct_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_MCT_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_OTP_CON_TOP_PCLK, "gout_peri_otp_con_top_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_OTP_CON_TOP_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_PWM_MOTOR_PCLK_S0, "gout_peri_pwm_motor_pclk_s0",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_PWM_MOTOR_PCLK_S0,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_BUS_CLK, "gout_peri_bus_clk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_BUS_CLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_I2C_CLK, "gout_peri_i2c_clk", "dout_clk_peri_i2c",
	     CLK_CON_GAT_GOUT_PERI_I2C_CLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_0_CLK, "gout_peri_spi_0_clk", "dout_clk_peri_spi0",
	     CLK_CON_GAT_GOUT_PERI_SPI_0_CLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_1_CLK, "gout_peri_spi_1_clk", "dout_clk_peri_spi1",
	     CLK_CON_GAT_GOUT_PERI_SPI_1_CLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_2_CLK, "gout_peri_spi_2_clk", "dout_clk_peri_spi2",
	     CLK_CON_GAT_GOUT_PERI_SPI_2_CLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_UART_CLK, "gout_peri_uart_clk", "mout_pll_peri_uart_user",
	     CLK_CON_GAT_GOUT_PERI_UART_CLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_USI00_I2C_CLK, "gout_peri_usi00_i2c_clk",
	     "dout_clk_peri_usi_i2c", CLK_CON_GAT_GOUT_PERI_USI00_I2C_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_USI00_USI_CLK, "gout_peri_usi00_usi_clk",
	     "dout_clk_peri_usi_usi", CLK_CON_GAT_GOUT_PERI_USI00_USI_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_0_PCLK, "gout_peri_spi_0_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_SPI_0_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_0_IPCLK, "gout_peri_spi_0_ipclk", "dout_clk_peri_spi0",
	     CLK_CON_GAT_GOUT_PERI_SPI_0_IPCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_1_PCLK, "gout_peri_spi_1_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_SPI_1_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_1_IPCLK, "gout_peri_spi_1_ipclk", "dout_clk_peri_spi1",
	     CLK_CON_GAT_GOUT_PERI_SPI_1_IPCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_2_PCLK, "gout_peri_spi_2_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_SPI_2_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SPI_2_IPCLK, "gout_peri_spi_2_ipclk", "dout_clk_peri_spi2",
	     CLK_CON_GAT_GOUT_PERI_SPI_2_IPCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_SYSREG_PCLK, "gout_peri_sysreg_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_SYSREG_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_UART_IPCLK, "gout_peri_uart_ipclk", "mout_pll_peri_uart_user",
	     CLK_CON_GAT_GOUT_PERI_UART_IPCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_UART_PCLK, "gout_peri_uart_pclk", "mout_pll_peri_bus_user",
	     CLK_CON_GAT_GOUT_PERI_UART_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_PERI_USI00_I2C_IPCLK, "gout_peri_usi00_i2c_ipclk",
	     "dout_clk_peri_usi_i2c", CLK_CON_GAT_GOUT_PERI_USI00_I2C_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_USI00_I2C_PCLK, "gout_peri_usi00_i2c_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_USI00_I2C_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_USI00_USI_IPCLK, "gout_peri_usi00_usi_ipclk",
	     "dout_clk_peri_usi_usi", CLK_CON_GAT_GOUT_PERI_USI00_USI_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_USI00_USI_PCLK, "gout_peri_usi00_usi_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_USI00_USI_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_WDT_CLUSTER0_PCLK, "gout_peri_wdt_cluster0_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_WDT_CLUSTER0_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_PERI_WDT_CLUSTER1_PCLK, "gout_peri_wdt_cluster1_pclk",
	     "mout_pll_peri_bus_user", CLK_CON_GAT_GOUT_PERI_WDT_CLUSTER1_PCLK,
	     21, 0, 0),
};

static const struct samsung_cmu_info peri_cmu_info __initconst = {
	.mux_clks		= peri_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(peri_mux_clks),
	.div_clks		= peri_div_clks,
	.nr_div_clks		= ARRAY_SIZE(peri_div_clks),
	.gate_clks		= peri_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(peri_gate_clks),
	.clk_regs		= peri_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(peri_clk_regs),
	.nr_clk_ids		= CLKS_NR_PERI,
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_PERI_CONTROLLER_OPTION,
};

static void __init exynos9610_cmu_peri_init(struct device_node *np)
{
	exynos_arm64_register_cmu(NULL, np, &peri_cmu_info);
}

CLK_OF_DECLARE(exynos9610_cmu_peri, "samsung,exynos9610-cmu-peri",
	       exynos9610_cmu_peri_init);

/* CMU_APM */
#define PLL_CON0_MUX_CLKCMU_APM_BUS_USER		0x0100
#define PLL_CON2_MUX_CLKCMU_APM_BUS_USER		0x0108
#define PLL_CON0_MUX_DLL_USER				0x0120
#define PLL_CON2_MUX_DLL_USER				0x0128
#define CMU_CMU_APM_CONTROLLER_OPTION			0x0800
#define CLK_CON_MUX_CLKCMU_SHUB_BUS			0x1000
#define CLK_CON_MUX_CLK_APM_BUS				0x1004
#define CLK_CON_DIV_CLKCMU_SHUB_BUS			0x1800
#define CLK_CON_DIV_CLK_APM_BUS				0x1804
#define CLK_CON_GAT_CLKCMU_CMGP_BUS			0x2000
#define CLK_CON_GAT_CLK_APM_CMU_PCLK			0x2004
#define CLK_CON_GAT_CLK_APM_OSCCLK_CLK			0x2008
#define CLK_CON_GAT_CLK_APM_OSCCLK_RCO_CLK		0x200c
#define CLK_CON_GAT_GATE_CLKCMU_SHUB_BUS		0x2010
#define CLK_CON_GAT_GOUT_APM_APBIF_GPIO_ALIVE_PCLK	0x2014
#define CLK_CON_GAT_GOUT_APM_APBIF_PMU_ALIVE_PCLK	0x2018
#define CLK_CON_GAT_GOUT_APM_APBIF_RTC_ALIVE_PCLK	0x201c
#define CLK_CON_GAT_GOUT_APM_APBIF_TOP_RTC_ALIVE_PCLK	0x2020
#define CLK_CON_GAT_GOUT_APM_GREBEINTEGRATION_HCLK	0x2024
#define CLK_CON_GAT_GOUT_APM_INTMEM_ACLK		0x2028
#define CLK_CON_GAT_GOUT_APM_INTMEM_PCLK		0x202c
#define CLK_CON_GAT_GOUT_APM_LHM_AXI_P_GNSS_CLK		0x2030
#define CLK_CON_GAT_GOUT_APM_LHM_AXI_P_CLK		0x2034
#define CLK_CON_GAT_GOUT_APM_LHM_AXI_P_MODEM_CLK	0x2038
#define CLK_CON_GAT_GOUT_APM_LHM_AXI_P_SHUB_CLK		0x203c
#define CLK_CON_GAT_GOUT_APM_LHM_AXI_P_WLBT_CLK		0x2040
#define CLK_CON_GAT_GOUT_APM_LHS_AXI_D_CLK		0x2044
#define CLK_CON_GAT_GOUT_APM_LHS_AXI_LP_SHUB_CLK	0x2048
#define CLK_CON_GAT_GOUT_APM_MAILBOX_AP2CP_PCLK		0x204c
#define CLK_CON_GAT_GOUT_APM_MAILBOX_AP2CP_S_PCLK	0x2050
#define CLK_CON_GAT_GOUT_APM_MAILBOX_AP2GNSS_PCLK	0x2054
#define CLK_CON_GAT_GOUT_APM_MAILBOX_AP2SHUB_PCLK	0x2058
#define CLK_CON_GAT_GOUT_APM_MAILBOX_AP2WLBT_PCLK	0x205c
#define CLK_CON_GAT_GOUT_APM_MAILBOX_APM2AP_PCLK	0x2060
#define CLK_CON_GAT_GOUT_APM_MAILBOX_APM2CP_PCLK	0x2064
#define CLK_CON_GAT_GOUT_APM_MAILBOX_APM2GNSS_PCLK	0x2068
#define CLK_CON_GAT_GOUT_APM_MAILBOX_APM2SHUB_PCLK	0x206c
#define CLK_CON_GAT_GOUT_APM_MAILBOX_APM2WLBT_PCLK	0x2070
#define CLK_CON_GAT_GOUT_APM_MAILBOX_CP2GNSS_PCLK	0x2074
#define CLK_CON_GAT_GOUT_APM_MAILBOX_CP2SHUB_PCLK	0x2078
#define CLK_CON_GAT_GOUT_APM_MAILBOX_CP2WLBT_PCLK	0x207c
#define CLK_CON_GAT_GOUT_APM_MAILBOX_SHUB2GNSS_PCLK	0x2080
#define CLK_CON_GAT_GOUT_APM_MAILBOX_SHUB2WLBT_PCLK	0x2084
#define CLK_CON_GAT_GOUT_APM_MAILBOX_WLBT2ABOX_PCLK	0x2088
#define CLK_CON_GAT_GOUT_APM_MAILBOX_WLBT2GNSS_PCLK	0x208c
#define CLK_CON_GAT_GOUT_APM_PEM_CLK			0x2090
#define CLK_CON_GAT_GOUT_APM_PGEN_LITE_CLK		0x2094
#define CLK_CON_GAT_GOUT_APM_PMU_INTR_GEN_PCLK		0x2098
#define CLK_CON_GAT_GOUT_APM_BUS_CLK			0x209c
#define CLK_CON_GAT_GOUT_APM_GREBE_CLK			0x20a0
#define CLK_CON_GAT_GOUT_APM_SPEEDY_PCLK		0x20a4
#define CLK_CON_GAT_GOUT_APM_SYSREG_PCLK		0x20a8
#define CLK_CON_GAT_GOUT_APM_WDT_PCLK			0x20ac
#define CLK_CON_GAT_GOUT_APM_XIU_DP_ACLK		0x20b0

static const unsigned long apm_clk_regs[] __initconst = {
	PLL_CON0_MUX_CLKCMU_APM_BUS_USER,
	PLL_CON2_MUX_CLKCMU_APM_BUS_USER,
	PLL_CON0_MUX_DLL_USER,
	PLL_CON2_MUX_DLL_USER,
	CMU_CMU_APM_CONTROLLER_OPTION,
	CLK_CON_MUX_CLKCMU_SHUB_BUS,
	CLK_CON_MUX_CLK_APM_BUS,
	CLK_CON_DIV_CLKCMU_SHUB_BUS,
	CLK_CON_DIV_CLK_APM_BUS,
	CLK_CON_GAT_CLKCMU_CMGP_BUS,
	CLK_CON_GAT_CLK_APM_CMU_PCLK,
	CLK_CON_GAT_CLK_APM_OSCCLK_CLK,
	CLK_CON_GAT_CLK_APM_OSCCLK_RCO_CLK,
	CLK_CON_GAT_GATE_CLKCMU_SHUB_BUS,
	CLK_CON_GAT_GOUT_APM_APBIF_GPIO_ALIVE_PCLK,
	CLK_CON_GAT_GOUT_APM_APBIF_PMU_ALIVE_PCLK,
	CLK_CON_GAT_GOUT_APM_APBIF_RTC_ALIVE_PCLK,
	CLK_CON_GAT_GOUT_APM_APBIF_TOP_RTC_ALIVE_PCLK,
	CLK_CON_GAT_GOUT_APM_GREBEINTEGRATION_HCLK,
	CLK_CON_GAT_GOUT_APM_INTMEM_ACLK,
	CLK_CON_GAT_GOUT_APM_INTMEM_PCLK,
	CLK_CON_GAT_GOUT_APM_LHM_AXI_P_GNSS_CLK,
	CLK_CON_GAT_GOUT_APM_LHM_AXI_P_CLK,
	CLK_CON_GAT_GOUT_APM_LHM_AXI_P_MODEM_CLK,
	CLK_CON_GAT_GOUT_APM_LHM_AXI_P_SHUB_CLK,
	CLK_CON_GAT_GOUT_APM_LHM_AXI_P_WLBT_CLK,
	CLK_CON_GAT_GOUT_APM_LHS_AXI_D_CLK,
	CLK_CON_GAT_GOUT_APM_LHS_AXI_LP_SHUB_CLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_AP2CP_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_AP2CP_S_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_AP2GNSS_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_AP2SHUB_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_AP2WLBT_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_APM2AP_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_APM2CP_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_APM2GNSS_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_APM2SHUB_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_APM2WLBT_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_CP2GNSS_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_CP2SHUB_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_CP2WLBT_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_SHUB2GNSS_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_SHUB2WLBT_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_WLBT2ABOX_PCLK,
	CLK_CON_GAT_GOUT_APM_MAILBOX_WLBT2GNSS_PCLK,
	CLK_CON_GAT_GOUT_APM_PEM_CLK,
	CLK_CON_GAT_GOUT_APM_PGEN_LITE_CLK,
	CLK_CON_GAT_GOUT_APM_PMU_INTR_GEN_PCLK,
	CLK_CON_GAT_GOUT_APM_BUS_CLK,
	CLK_CON_GAT_GOUT_APM_GREBE_CLK,
	CLK_CON_GAT_GOUT_APM_SPEEDY_PCLK,
	CLK_CON_GAT_GOUT_APM_SYSREG_PCLK,
	CLK_CON_GAT_GOUT_APM_WDT_PCLK,
	CLK_CON_GAT_GOUT_APM_XIU_DP_ACLK,
};

PNAME(mout_pll_apm_bus_user_p)		= { "oscclk", "dout_cmu_apm_bus" };
PNAME(mout_pll_dll_user_p)		= { "oscclk", "dll_dco" };
PNAME(mout_cmu_shub_bus_p)		= { "oscclk", "mout_pll_dll_user" };
PNAME(mout_clk_apm_bus_p)		= { "mout_pll_apm_bus_user",
					    "dll_dco" };

static const struct samsung_mux_clock apm_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_APM_BUS_USER, "mout_pll_apm_bus_user", mout_pll_apm_bus_user_p,
	    PLL_CON0_MUX_CLKCMU_APM_BUS_USER, 4, 1),
	MUX(CLK_MOUT_PLL_DLL_USER, "mout_pll_dll_user", mout_pll_dll_user_p,
	    PLL_CON0_MUX_DLL_USER, 4, 1),
	MUX(CLK_MOUT_CMU_SHUB_BUS, "mout_cmu_shub_bus", mout_cmu_shub_bus_p,
	    CLK_CON_MUX_CLKCMU_SHUB_BUS, 0, 1),
	MUX(CLK_MOUT_CLK_APM_BUS, "mout_clk_apm_bus", mout_clk_apm_bus_p,
	    CLK_CON_MUX_CLK_APM_BUS, 0, 1),
};

static const struct samsung_div_clock apm_div_clks[] __initconst = {
	DIV(CLK_DOUT_CMU_SHUB_BUS, "dout_cmu_shub_bus", "gout_cmu_shub_bus",
	    CLK_CON_DIV_CLKCMU_SHUB_BUS, 0, 3),
	DIV(CLK_DOUT_CLK_APM_BUS, "dout_clk_apm_bus", "mout_clk_apm_bus",
	    CLK_CON_DIV_CLK_APM_BUS, 0, 3),
};

static const struct samsung_gate_clock apm_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CMU_CMGP_BUS, "gout_cmu_cmgp_bus", "dout_clk_apm_bus",
	     CLK_CON_GAT_CLKCMU_CMGP_BUS, 21, 0, 0),
	GATE(CLK_GOUT_CLK_APM_CMU_PCLK, "gout_clk_apm_cmu_pclk", "dout_clk_apm_bus",
	     CLK_CON_GAT_CLK_APM_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_APM_OSCCLK_CLK, "gat_clk_apm_oscclk_clk", "oscclk",
	     CLK_CON_GAT_CLK_APM_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_APM_OSCCLK_RCO_CLK, "gat_clk_apm_oscclk_rco_clk", "oscclk",
	     CLK_CON_GAT_CLK_APM_OSCCLK_RCO_CLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_APBIF_GPIO_ALIVE_PCLK, "gout_apm_apbif_gpio_alive_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_APBIF_GPIO_ALIVE_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_APBIF_PMU_ALIVE_PCLK, "gout_apm_apbif_pmu_alive_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_APBIF_PMU_ALIVE_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_APBIF_RTC_ALIVE_PCLK, "gout_apm_apbif_rtc_alive_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_APBIF_RTC_ALIVE_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_APBIF_TOP_RTC_ALIVE_PCLK, "gout_apm_apbif_top_rtc_alive_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_APBIF_TOP_RTC_ALIVE_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_GREBEINTEGRATION_HCLK, "gout_apm_grebeintegration_hclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_GREBEINTEGRATION_HCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_INTMEM_ACLK, "gout_apm_intmem_aclk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_INTMEM_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_INTMEM_PCLK, "gout_apm_intmem_pclk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_INTMEM_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_LHM_AXI_P_GNSS_CLK, "gout_apm_lhm_axi_p_gnss_clk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_LHM_AXI_P_GNSS_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_LHM_AXI_P_CLK, "gout_apm_lhm_axi_p_clk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_LHM_AXI_P_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_LHM_AXI_P_MODEM_CLK, "gout_apm_lhm_axi_p_modem_clk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_LHM_AXI_P_MODEM_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_LHM_AXI_P_SHUB_CLK, "gout_apm_lhm_axi_p_shub_clk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_LHM_AXI_P_SHUB_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_LHM_AXI_P_WLBT_CLK, "gout_apm_lhm_axi_p_wlbt_clk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_LHM_AXI_P_WLBT_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_LHS_AXI_D_CLK, "gout_apm_lhs_axi_d_clk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_LHS_AXI_D_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_LHS_AXI_LP_SHUB_CLK, "gout_apm_lhs_axi_lp_shub_clk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_LHS_AXI_LP_SHUB_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_AP2CP_PCLK, "gout_apm_mailbox_ap2cp_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_AP2CP_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_AP2CP_S_PCLK, "gout_apm_mailbox_ap2cp_s_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_AP2CP_S_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_AP2GNSS_PCLK, "gout_apm_mailbox_ap2gnss_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_AP2GNSS_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_AP2SHUB_PCLK, "gout_apm_mailbox_ap2shub_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_AP2SHUB_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_AP2WLBT_PCLK, "gout_apm_mailbox_ap2wlbt_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_AP2WLBT_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_APM2AP_PCLK, "gout_apm_mailbox_apm2ap_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_APM2AP_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_APM2CP_PCLK, "gout_apm_mailbox_apm2cp_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_APM2CP_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_APM2GNSS_PCLK, "gout_apm_mailbox_apm2gnss_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_APM2GNSS_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_APM2SHUB_PCLK, "gout_apm_mailbox_apm2shub_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_APM2SHUB_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_APM2WLBT_PCLK, "gout_apm_mailbox_apm2wlbt_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_APM2WLBT_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_CP2GNSS_PCLK, "gout_apm_mailbox_cp2gnss_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_CP2GNSS_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_CP2SHUB_PCLK, "gout_apm_mailbox_cp2shub_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_CP2SHUB_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_CP2WLBT_PCLK, "gout_apm_mailbox_cp2wlbt_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_CP2WLBT_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_SHUB2GNSS_PCLK, "gout_apm_mailbox_shub2gnss_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_SHUB2GNSS_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_SHUB2WLBT_PCLK, "gout_apm_mailbox_shub2wlbt_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_SHUB2WLBT_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_WLBT2ABOX_PCLK, "gout_apm_mailbox_wlbt2abox_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_WLBT2ABOX_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_MAILBOX_WLBT2GNSS_PCLK, "gout_apm_mailbox_wlbt2gnss_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_MAILBOX_WLBT2GNSS_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_APM_PEM_CLK, "gout_apm_pem_clk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_PEM_CLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_PGEN_LITE_CLK, "gout_apm_pgen_lite_clk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_PGEN_LITE_CLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_PMU_INTR_GEN_PCLK, "gout_apm_pmu_intr_gen_pclk",
	     "dout_clk_apm_bus", CLK_CON_GAT_GOUT_APM_PMU_INTR_GEN_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_BUS_CLK, "gout_apm_bus_clk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_BUS_CLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_GREBE_CLK, "gout_apm_grebe_clk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_GREBE_CLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_SPEEDY_PCLK, "gout_apm_speedy_pclk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_SPEEDY_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_SYSREG_PCLK, "gout_apm_sysreg_pclk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_SYSREG_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_WDT_PCLK, "gout_apm_wdt_pclk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_WDT_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_APM_XIU_DP_ACLK, "gout_apm_xiu_dp_aclk", "dout_clk_apm_bus",
	     CLK_CON_GAT_GOUT_APM_XIU_DP_ACLK, 21, 0, 0),
};

static const struct samsung_cmu_info apm_cmu_info __initconst = {
	.mux_clks		= apm_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(apm_mux_clks),
	.div_clks		= apm_div_clks,
	.nr_div_clks		= ARRAY_SIZE(apm_div_clks),
	.gate_clks		= apm_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(apm_gate_clks),
	.clk_regs		= apm_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(apm_clk_regs),
	.nr_clk_ids		= CLKS_NR_APM,
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_APM_CONTROLLER_OPTION,
};

static void __init exynos9610_cmu_apm_init(struct device_node *np)
{
	exynos_arm64_register_cmu(NULL, np, &apm_cmu_info);
}

/* Register APM early, as it's a dependency of CMU_CMGP */
CLK_OF_DECLARE(exynos9610_cmu_apm, "samsung,exynos9610-cmu-apm",
	       exynos9610_cmu_apm_init);

/* CMU_CAM */
#define PLL_CON0_MUX_CLKCMU_CAM_BUS_USER			0x0100
#define PLL_CON2_MUX_CLKCMU_CAM_BUS_USER			0x0108
#define CMU_CMU_CAM_CONTROLLER_OPTION				0x0800
#define CLK_CON_DIV_DIV_CLK_CAM_BUSP				0x1800
#define CLK_CON_GAT_CLK_CAM_CMU_PCLK				0x2000
#define CLK_CON_GAT_CLK_CAM_OSCCLK_CLK				0x2004
#define CLK_CON_GAT_GOUT_CAM_BUSD				0x2008
#define CLK_CON_GAT_GOUT_CAM_BTM_ACLK				0x200c
#define CLK_CON_GAT_GOUT_CAM_BTM_PCLK				0x2010
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_3AA			0x2014
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS0		0x2018
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS1		0x201c
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS2		0x2020
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS3		0x2024
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_DMA			0x2028
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS0		0x202c
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS1		0x2030
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS2		0x2034
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS3		0x2038
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_PAFSTAT_CORE		0x203c
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_PPMU_CAM		0x2040
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_RDMA			0x2044
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_SMMU_CAM		0x2048
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_XIU_D_CAM		0x204c
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_PCLK_PGEN_LITE_CAM0	0x2050
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_PCLK_PGEN_LITE_CAM1	0x2054
#define CLK_CON_GAT_GOUT_CAM_IS6P10P0_PCLK_PPMU_CAM		0x2058
#define CLK_CON_GAT_GOUT_CAM_LHM_AXI_P_CLK			0x205c
#define CLK_CON_GAT_GOUT_CAM_LHS_ACEL_D_CLK			0x2060
#define CLK_CON_GAT_GOUT_CAM_LHS_ATB_CAMISP_CLK			0x2064
#define CLK_CON_GAT_GOUT_CAM_BUSD_CLK				0x2068
#define CLK_CON_GAT_GOUT_CAM_BUSP_CLK				0x206c
#define CLK_CON_GAT_GOUT_CAM_SYSREG_PCLK			0x2070

static const unsigned long cam_clk_regs[] __initconst = {
	PLL_CON0_MUX_CLKCMU_CAM_BUS_USER,
	PLL_CON2_MUX_CLKCMU_CAM_BUS_USER,
	CMU_CMU_CAM_CONTROLLER_OPTION,
	CLK_CON_DIV_DIV_CLK_CAM_BUSP,
	CLK_CON_GAT_CLK_CAM_CMU_PCLK,
	CLK_CON_GAT_CLK_CAM_OSCCLK_CLK,
	CLK_CON_GAT_GOUT_CAM_BUSD,
	CLK_CON_GAT_GOUT_CAM_BTM_ACLK,
	CLK_CON_GAT_GOUT_CAM_BTM_PCLK,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_3AA,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS0,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS1,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS2,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS3,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_DMA,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS0,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS1,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS2,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS3,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_PAFSTAT_CORE,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_PPMU_CAM,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_RDMA,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_SMMU_CAM,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_XIU_D_CAM,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_PCLK_PGEN_LITE_CAM0,
	CLK_CON_GAT_GOUT_CAM_IS6P10P0_PCLK_PPMU_CAM,
	CLK_CON_GAT_GOUT_CAM_LHM_AXI_P_CLK,
	CLK_CON_GAT_GOUT_CAM_LHS_ATB_CAMISP_CLK,
	CLK_CON_GAT_GOUT_CAM_BUSD_CLK,
	CLK_CON_GAT_GOUT_CAM_BUSP_CLK,
	CLK_CON_GAT_GOUT_CAM_SYSREG_PCLK,
};

PNAME(mout_pll_cam_bus_user_p) = { "oscclk", "dout_cmu_cam_bus" };

static const struct samsung_mux_clock cam_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_CAM_BUS_USER, "mout_pll_cam_bus_user", mout_pll_cam_bus_user_p,
	    PLL_CON0_MUX_CLKCMU_CAM_BUS_USER, 4, 1),
};

static const struct samsung_div_clock cam_div_clks[] __initconst = {
	DIV(CLK_DIV_CLK_CAM_BUSP, "dout_clk_cam_busp", "mout_pll_cam_bus_user",
	    CLK_CON_DIV_DIV_CLK_CAM_BUSP, 0, 2),
};

static const struct samsung_gate_clock cam_gate_clks[] __initconst = {
	GATE(CLK_GAT_CLK_CAM_CMU_PCLK, "gat_clk_cam_cmu_pclk", "dout_clk_cam_busp",
	     CLK_CON_GAT_CLK_CAM_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GAT_CLK_CAM_OSCCLK_CLK, "gat_clk_cam_oscclk_clk", "oscclk",
	     CLK_CON_GAT_CLK_CAM_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CAM_BUSD, "gout_cam_busd", "mout_pll_cam_bus_user",
	     CLK_CON_GAT_GOUT_CAM_BUSD, 21, 0, 0),
	GATE(CLK_GOUT_CAM_BTM_ACLK, "gout_cam_btm_aclk", "mout_pll_cam_bus_user",
	     CLK_CON_GAT_GOUT_CAM_BTM_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_CAM_BTM_PCLK, "gout_cam_btm_pclk", "dout_clk_cam_busp",
	     CLK_CON_GAT_GOUT_CAM_BTM_PCLK, 21, 0, 0),

	GATE(CLK_GOUT_CAM_LHS_ATB_CAMISP_CLK, "gout_cam_lhs_atb_camisp_clk",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_LHS_ATB_CAMISP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_3AA, "gout_cam_is6p10p0_aclk_3aa",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_3AA,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_CSIS0, "gout_cam_is6p10p0_aclk_csis0",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS0,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_CSIS1, "gout_cam_is6p10p0_aclk_csis1",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS1,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_CSIS2, "gout_cam_is6p10p0_aclk_csis2",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS2,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_CSIS3, "gout_cam_is6p10p0_aclk_csis3",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_CSIS3,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_RDMA, "gout_cam_is6p10p0_aclk_rdma",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_RDMA,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS0, "gout_cam_is6p10p0_aclk_glue_csis0",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS0,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS1, "gout_cam_is6p10p0_aclk_glue_csis1",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS1,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS2, "gout_cam_is6p10p0_aclk_glue_csis2",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS2,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS3, "gout_cam_is6p10p0_aclk_glue_csis3",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_GLUE_CSIS3,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_PAFSTAT_CORE, "gout_cam_is6p10p0_aclk_pafstat_core",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_PAFSTAT_CORE,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_PPMU_CAM, "gout_cam_is6p10p0_aclk_ppmu_cam",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_PPMU_CAM,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_DMA, "gout_cam_is6p10p0_aclk_dma",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_DMA,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_SMMU_CAM, "gout_cam_is6p10p0_aclk_smmu_cam",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_SMMU_CAM,
	     21, CLK_IGNORE_UNUSED, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_ACLK_XIU_D_CAM, "gout_cam_is6p10p0_aclk_xiu_d_cam",
	     "mout_pll_cam_bus_user", CLK_CON_GAT_GOUT_CAM_IS6P10P0_ACLK_XIU_D_CAM,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_PCLK_PGEN_LITE_CAM0, "gout_cam_is6p10p0_pclk_pgen_lite_cam0",
	     "dout_clk_cam_busp", CLK_CON_GAT_GOUT_CAM_IS6P10P0_PCLK_PGEN_LITE_CAM0,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_PCLK_PGEN_LITE_CAM1, "gout_cam_is6p10p0_pclk_pgen_lite_cam1",
	     "dout_clk_cam_busp", CLK_CON_GAT_GOUT_CAM_IS6P10P0_PCLK_PGEN_LITE_CAM1,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_IS6P10P0_PCLK_PPMU_CAM, "gout_cam_is6p10p0_pclk_ppmu_cam",
	     "dout_clk_cam_busp", CLK_CON_GAT_GOUT_CAM_IS6P10P0_PCLK_PPMU_CAM,
	     21, 0, 0),
	GATE(CLK_GOUT_CAM_LHM_AXI_P_CLK, "gout_cam_lhm_axi_p_clk", "dout_clk_cam_busp",
	     CLK_CON_GAT_GOUT_CAM_LHM_AXI_P_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CAM_LHS_ACEL_D_CLK, "gout_cam_lhs_acel_d_clk", "mout_pll_cam_bus_user",
	     CLK_CON_GAT_GOUT_CAM_LHS_ACEL_D_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CAM_BUSD_CLK, "gout_cam_busd_clk", "mout_pll_cam_bus_user",
	     CLK_CON_GAT_GOUT_CAM_BUSD_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CAM_BUSP_CLK, "gout_cam_busp_clk", "dout_clk_cam_busp",
	     CLK_CON_GAT_GOUT_CAM_BUSP_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CAM_SYSREG_PCLK, "gout_cam_sysreg_pclk", "dout_clk_cam_busp",
	     CLK_CON_GAT_GOUT_CAM_SYSREG_PCLK, 21, 0, 0),
};

static const struct samsung_cmu_info cam_cmu_info __initconst = {
	.mux_clks		= cam_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(cam_mux_clks),
	.div_clks		= cam_div_clks,
	.nr_div_clks		= ARRAY_SIZE(cam_div_clks),
	.gate_clks		= cam_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(cam_gate_clks),
	.clk_regs		= cam_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(cam_clk_regs),
	.nr_clk_ids		= CLKS_NR_CAM,
	.sysreg_clk_regs	= drcg_cam_memclk_sysreg,
	.nr_sysreg_clk_regs	= ARRAY_SIZE(drcg_cam_memclk_sysreg),
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.drcg_offset		= EXYNOS9610_CAM_DRCG_EN_OFFSET,
	.memclk_offset		= EXYNOS9610_MEMCLK_OFFSET,
	.option_offset		= CMU_CMU_CAM_CONTROLLER_OPTION,
};

/* CMU_CMGP */
#define CMU_CMU_CMGP_CONTROLLER_OPTION			0x0800
#define CLK_CON_MUX_MUX_CLK_CMGP_ADC			0x1000
#define CLK_CON_MUX_MUX_CLK_CMGP_I2C			0x1004
#define CLK_CON_MUX_MUX_CLK_CMGP_USI00			0x1008
#define CLK_CON_MUX_MUX_CLK_CMGP_USI01			0x100c
#define CLK_CON_MUX_MUX_CLK_CMGP_USI02			0x1010
#define CLK_CON_MUX_MUX_CLK_CMGP_USI03			0x1014
#define CLK_CON_MUX_MUX_CLK_CMGP_USI04			0x1018
#define CLK_CON_DIV_DIV_CLK_CMGP_ADC			0x1800
#define CLK_CON_DIV_DIV_CLK_CMGP_I2C			0x1804
#define CLK_CON_DIV_DIV_CLK_CMGP_USI00			0x1808
#define CLK_CON_DIV_DIV_CLK_CMGP_USI01			0x180c
#define CLK_CON_DIV_DIV_CLK_CMGP_USI02			0x1810
#define CLK_CON_DIV_DIV_CLK_CMGP_USI03			0x1814
#define CLK_CON_DIV_DIV_CLK_CMGP_USI04			0x1818
#define CLK_CON_GAT_CMGP_CMU_PCLK			0x2004
#define CLK_CON_GAT_CLK_CMGP_OSCCLK_RCO_CLK		0x2008
#define CLK_CON_GAT_GOUT_CMGP_ADC_PCLK_S0		0x200c
#define CLK_CON_GAT_GOUT_CMGP_ADC_PCLK_S1		0x2010
#define CLK_CON_GAT_GOUT_CMGP_GPIO_PCLK			0x2014
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP00_IPCLK		0x2018
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP00_PCLK		0x201c
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP01_IPCLK		0x2020
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP01_PCLK		0x2024
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP02_IPCLK		0x2028
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP02_PCLK		0x202c
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP03_IPCLK		0x2030
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP03_PCLK		0x2034
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP04_IPCLK		0x2038
#define CLK_CON_GAT_GOUT_CMGP_I2C_CMGP04_PCLK		0x203c
#define CLK_CON_GAT_GOUT_CMGP_BUS_CLK			0x2040
#define CLK_CON_GAT_GOUT_CMGP_I2C_CLK			0x2044
#define CLK_CON_GAT_GOUT_CMGP_USI00_CLK			0x2048
#define CLK_CON_GAT_GOUT_CMGP_USI01_CLK			0x204c
#define CLK_CON_GAT_GOUT_CMGP_USI02_CLK			0x2050
#define CLK_CON_GAT_GOUT_CMGP_USI03_CLK			0x2054
#define CLK_CON_GAT_GOUT_CMGP_USI04_CLK			0x2058
#define CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2CP_PCLK	0x205c
#define CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2GNSS_PCLK	0x2060
#define CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2PMU_AP_PCLK	0x2064
#define CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2PMU_SHUB_PCLK	0x2068
#define CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2SHUB_PCLK	0x206c
#define CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2WLBT_PCLK	0x2070
#define CLK_CON_GAT_GOUT_CMGP_SYSREG_PCLK		0x2074
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP00_IPCLK		0x2078
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP00_PCLK		0x207c
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP01_IPCLK		0x2080
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP01_PCLK		0x2084
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP02_IPCLK		0x2088
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP02_PCLK		0x208c
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP03_IPCLK		0x2090
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP03_PCLK		0x2094
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP04_IPCLK		0x2098
#define CLK_CON_GAT_GOUT_CMGP_USI_CMGP04_PCLK		0x209c

static const unsigned long cmgp_clk_regs[] __initconst = {
	CMU_CMU_CMGP_CONTROLLER_OPTION,
	CLK_CON_MUX_MUX_CLK_CMGP_ADC,
	CLK_CON_MUX_MUX_CLK_CMGP_I2C,
	CLK_CON_MUX_MUX_CLK_CMGP_USI00,
	CLK_CON_MUX_MUX_CLK_CMGP_USI01,
	CLK_CON_MUX_MUX_CLK_CMGP_USI02,
	CLK_CON_MUX_MUX_CLK_CMGP_USI03,
	CLK_CON_MUX_MUX_CLK_CMGP_USI04,
	CLK_CON_DIV_DIV_CLK_CMGP_ADC,
	CLK_CON_DIV_DIV_CLK_CMGP_I2C,
	CLK_CON_DIV_DIV_CLK_CMGP_USI00,
	CLK_CON_DIV_DIV_CLK_CMGP_USI01,
	CLK_CON_DIV_DIV_CLK_CMGP_USI02,
	CLK_CON_DIV_DIV_CLK_CMGP_USI03,
	CLK_CON_DIV_DIV_CLK_CMGP_USI04,
	CLK_CON_GAT_CMGP_CMU_PCLK,
	CLK_CON_GAT_CLK_CMGP_OSCCLK_RCO_CLK,
	CLK_CON_GAT_GOUT_CMGP_ADC_PCLK_S0,
	CLK_CON_GAT_GOUT_CMGP_ADC_PCLK_S1,
	CLK_CON_GAT_GOUT_CMGP_GPIO_PCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP00_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP00_PCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP01_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP01_PCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP02_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP02_PCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP03_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP03_PCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP04_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CMGP04_PCLK,
	CLK_CON_GAT_GOUT_CMGP_BUS_CLK,
	CLK_CON_GAT_GOUT_CMGP_I2C_CLK,
	CLK_CON_GAT_GOUT_CMGP_USI00_CLK,
	CLK_CON_GAT_GOUT_CMGP_USI01_CLK,
	CLK_CON_GAT_GOUT_CMGP_USI02_CLK,
	CLK_CON_GAT_GOUT_CMGP_USI03_CLK,
	CLK_CON_GAT_GOUT_CMGP_USI04_CLK,
	CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2CP_PCLK,
	CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2GNSS_PCLK,
	CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2PMU_AP_PCLK,
	CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2PMU_SHUB_PCLK,
	CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2SHUB_PCLK,
	CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2WLBT_PCLK,
	CLK_CON_GAT_GOUT_CMGP_SYSREG_PCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP00_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP00_PCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP01_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP01_PCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP02_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP02_PCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP03_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP03_PCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP04_IPCLK,
	CLK_CON_GAT_GOUT_CMGP_USI_CMGP04_PCLK,
};

PNAME(mout_clk_cmgp_adc_p)	= { "oscclk", "dout_clk_cmgp_adc" };
PNAME(mout_clk_cmgp_bus_user_p)	= { "oscclk_rco",
				    "gout_cmu_cmgp_bus" };

static const struct samsung_mux_clock cmgp_mux_clks[] __initconst = {
	MUX(CLK_MOUT_CLK_CMGP_ADC, "mout_clk_cmgp_adc", mout_clk_cmgp_adc_p,
	    CLK_CON_MUX_MUX_CLK_CMGP_ADC, 0, 1),
	MUX(CLK_MOUT_CLK_CMGP_I2C, "mout_clk_cmgp_i2c", mout_clk_cmgp_bus_user_p,
	    CLK_CON_MUX_MUX_CLK_CMGP_I2C, 0, 1),
	MUX(CLK_MOUT_CLK_CMGP_USI00, "mout_clk_cmgp_usi00", mout_clk_cmgp_bus_user_p,
	    CLK_CON_MUX_MUX_CLK_CMGP_USI00, 0, 1),
	MUX(CLK_MOUT_CLK_CMGP_USI01, "mout_clk_cmgp_usi01", mout_clk_cmgp_bus_user_p,
	    CLK_CON_MUX_MUX_CLK_CMGP_USI01, 0, 1),
	MUX(CLK_MOUT_CLK_CMGP_USI02, "mout_clk_cmgp_usi02", mout_clk_cmgp_bus_user_p,
	    CLK_CON_MUX_MUX_CLK_CMGP_USI02, 0, 1),
	MUX(CLK_MOUT_CLK_CMGP_USI03, "mout_clk_cmgp_usi03", mout_clk_cmgp_bus_user_p,
	    CLK_CON_MUX_MUX_CLK_CMGP_USI03, 0, 1),
	MUX(CLK_MOUT_CLK_CMGP_USI04, "mout_clk_cmgp_usi04", mout_clk_cmgp_bus_user_p,
	    CLK_CON_MUX_MUX_CLK_CMGP_USI04, 0, 1),
};

static const struct samsung_div_clock cmgp_div_clks[] __initconst = {
	DIV(CLK_DOUT_CLK_CMGP_ADC, "dout_clk_cmgp_adc", "gout_cmu_cmgp_bus",
	    CLK_CON_DIV_DIV_CLK_CMGP_ADC, 0, 4),
	DIV(CLK_DOUT_CLK_CMGP_I2C, "dout_clk_cmgp_i2c", "mout_clk_cmgp_i2c",
	    CLK_CON_DIV_DIV_CLK_CMGP_I2C, 0, 4),
	DIV(CLK_DOUT_CLK_CMGP_USI00, "dout_clk_cmgp_usi00", "mout_clk_cmgp_usi00",
	    CLK_CON_DIV_DIV_CLK_CMGP_USI00, 0, 4),
	DIV(CLK_DOUT_CLK_CMGP_USI01, "dout_clk_cmgp_usi01", "mout_clk_cmgp_usi01",
	    CLK_CON_DIV_DIV_CLK_CMGP_USI01, 0, 4),
	DIV(CLK_DOUT_CLK_CMGP_USI02, "dout_clk_cmgp_usi02", "mout_clk_cmgp_usi02",
	    CLK_CON_DIV_DIV_CLK_CMGP_USI02, 0, 4),
	DIV(CLK_DOUT_CLK_CMGP_USI03, "dout_clk_cmgp_usi03", "mout_clk_cmgp_usi03",
	    CLK_CON_DIV_DIV_CLK_CMGP_USI03, 0, 4),
	DIV(CLK_DOUT_CLK_CMGP_USI04, "dout_clk_cmgp_usi04", "mout_clk_cmgp_usi04",
	    CLK_CON_DIV_DIV_CLK_CMGP_USI04, 0, 4),
};

static const struct samsung_gate_clock cmgp_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CMGP_CMU_PCLK, "gout_cmgp_cmu_pclk", "gout_cmu_cmgp_bus",
	     CLK_CON_GAT_CMGP_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_CMGP_OSCCLK_RCO_CLK, "gout_clk_cmgp_oscclk_rco_clk",
	     "oscclk_rco", CLK_CON_GAT_CLK_CMGP_OSCCLK_RCO_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CMGP_ADC_PCLK_S0, "gout_clk_cmgp_adc_pclk_s0",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_ADC_PCLK_S0,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_ADC_PCLK_S1, "gout_clk_cmgp_adc_pclk_s1",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_ADC_PCLK_S1,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_GPIO_PCLK, "gout_clk_cmgp_gpio_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_GPIO_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP00_IPCLK, "gout_clk_cmgp_i2c_cmgp00_ipclk",
	     "mout_clk_cmgp_i2c", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP00_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP00_PCLK, "gout_clk_cmgp_i2c_cmgp00_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP00_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP01_IPCLK, "gout_clk_cmgp_i2c_cmgp01_ipclk",
	     "mout_clk_cmgp_i2c", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP01_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP01_PCLK, "gout_clk_cmgp_i2c_cmgp01_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP01_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP02_IPCLK, "gout_clk_cmgp_i2c_cmgp02_ipclk",
	     "mout_clk_cmgp_i2c", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP02_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP02_PCLK, "gout_clk_cmgp_i2c_cmgp02_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP02_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP03_IPCLK, "gout_clk_cmgp_i2c_cmgp03_ipclk",
	     "mout_clk_cmgp_i2c", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP03_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP03_PCLK, "gout_clk_cmgp_i2c_cmgp03_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP03_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP04_IPCLK, "gout_clk_cmgp_i2c_cmgp04_ipclk",
	     "mout_clk_cmgp_i2c", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP04_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CMGP04_PCLK, "gout_clk_cmgp_i2c_cmgp04_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_I2C_CMGP04_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_BUS_CLK, "gout_cmgp_bus_clk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_BUS_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_I2C_CLK, "gout_cmgp_i2c_clk",
	     "dout_clk_cmgp_i2c", CLK_CON_GAT_GOUT_CMGP_I2C_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI00_CLK, "gout_cmgp_usi00_clk",
	     "mout_clk_cmgp_usi00", CLK_CON_GAT_GOUT_CMGP_USI00_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI01_CLK, "gout_cmgp_usi01_clk",
	     "mout_clk_cmgp_usi01", CLK_CON_GAT_GOUT_CMGP_USI01_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI02_CLK, "gout_cmgp_usi02_clk",
	     "mout_clk_cmgp_usi02", CLK_CON_GAT_GOUT_CMGP_USI02_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI03_CLK, "gout_cmgp_usi03_clk",
	     "mout_clk_cmgp_usi03", CLK_CON_GAT_GOUT_CMGP_USI03_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI04_CLK, "gout_cmgp_usi04_clk",
	     "mout_clk_cmgp_usi04", CLK_CON_GAT_GOUT_CMGP_USI04_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_SYSREG_CMGP2CP_PCLK,
	     "gout_cmgp_sysreg_cmgp2cp_pclk", "gout_cmu_cmgp_bus",
	     CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2CP_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CMGP_SYSREG_CMGP2GNSS_PCLK,
	     "gout_cmgp_sysreg_cmgp2gnss_pclk", "gout_cmu_cmgp_bus",
	     CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2GNSS_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CMGP_SYSREG_CMGP2PMU_AP_PCLK,
	     "gout_cmgp_sysreg_cmgp2pmu_ap_pclk", "gout_cmu_cmgp_bus",
	     CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2PMU_AP_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CMGP_SYSREG_CMGP2PMU_SHUB_PCLK,
	     "gout_cmgp_sysreg_cmgp2pmu_shub_pclk", "gout_cmu_cmgp_bus",
	     CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2PMU_SHUB_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CMGP_SYSREG_CMGP2SHUB_PCLK,
	     "gout_cmgp_sysreg_cmgp2shub_pclk", "gout_cmu_cmgp_bus",
	     CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2SHUB_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CMGP_SYSREG_CMGP2WLBT_PCLK,
	     "gout_cmgp_sysreg_cmgp2wlbt_pclk", "gout_cmu_cmgp_bus",
	     CLK_CON_GAT_GOUT_CMGP_SYSREG_CMGP2WLBT_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CMGP_SYSREG_PCLK, "gout_cmgp_sysreg_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_SYSREG_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP00_IPCLK, "gout_clk_cmgp_usi_cmgp00_ipclk",
	     "mout_clk_cmgp_usi00", CLK_CON_GAT_GOUT_CMGP_USI_CMGP00_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP00_PCLK, "gout_clk_cmgp_usi_cmgp00_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_USI_CMGP00_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP01_IPCLK, "gout_clk_cmgp_usi_cmgp01_ipclk",
	     "mout_clk_cmgp_usi01", CLK_CON_GAT_GOUT_CMGP_USI_CMGP01_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP01_PCLK, "gout_clk_cmgp_usi_cmgp01_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_USI_CMGP01_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP02_IPCLK, "gout_clk_cmgp_usi_cmgp02_ipclk",
	     "mout_clk_cmgp_usi02", CLK_CON_GAT_GOUT_CMGP_USI_CMGP02_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP02_PCLK, "gout_clk_cmgp_usi_cmgp02_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_USI_CMGP02_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP03_IPCLK, "gout_clk_cmgp_usi_cmgp03_ipclk",
	     "mout_clk_cmgp_usi03", CLK_CON_GAT_GOUT_CMGP_USI_CMGP03_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP03_PCLK, "gout_clk_cmgp_usi_cmgp03_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_USI_CMGP03_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP04_IPCLK, "gout_clk_cmgp_usi_cmgp04_ipclk",
	     "mout_clk_cmgp_usi04", CLK_CON_GAT_GOUT_CMGP_USI_CMGP04_IPCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CMGP_USI_CMGP04_PCLK, "gout_clk_cmgp_usi_cmgp04_pclk",
	     "gout_cmu_cmgp_bus", CLK_CON_GAT_GOUT_CMGP_USI_CMGP04_IPCLK,
	     21, 0, 0),
};

static const struct samsung_cmu_info cmgp_cmu_info __initconst = {
	.mux_clks		= cmgp_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(cmgp_mux_clks),
	.div_clks		= cmgp_div_clks,
	.nr_div_clks		= ARRAY_SIZE(cmgp_div_clks),
	.gate_clks		= cmgp_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(cmgp_gate_clks),
	.clk_regs		= cmgp_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(cmgp_clk_regs),
	.nr_clk_ids		= CLKS_NR_CMGP,
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_CMGP_CONTROLLER_OPTION,
};

/* CMU_CORE */
#define PLL_CON0_MUX_CLKCMU_CORE_BUS_USER			0x0100
#define PLL_CON2_MUX_CLKCMU_CORE_BUS_USER			0x0108
#define PLL_CON0_MUX_CLKCMU_CORE_CCI_USER			0x0120
#define PLL_CON2_MUX_CLKCMU_CORE_CCI_USER			0x0128
#define PLL_CON0_MUX_CLKCMU_CORE_G3D_USER			0x0140
#define PLL_CON2_MUX_CLKCMU_CORE_G3D_USER			0x0148
#define CMU_CMU_CORE_CONTROLLER_OPTION				0x0800
#define CLK_CON_MUX_MUX_CLK_CORE_GIC				0x1000
#define CLK_CON_DIV_DIV_CLK_CORE_BUSP				0x1800
#define CLK_CON_GAT_CLK_CORE_CMU_PCLK				0x2000
#define CLK_CON_GAT_GOUT_CORE_AD_APB_CCI_550_PCLKM		0x2004
#define CLK_CON_GAT_GOUT_CORE_AD_APB_DIT_PCLKM			0x2008
#define CLK_CON_GAT_GOUT_CORE_AD_APB_PDMA0_PCLKM		0x200c
#define CLK_CON_GAT_GOUT_CORE_AD_APB_PGEN_PDMA_PCLKM		0x2010
#define CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_MEM0_PCLKM		0x2014
#define CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_MEM1_PCLKM		0x2018
#define CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_PERI_PCLKM		0x201c
#define CLK_CON_GAT_GOUT_CORE_AD_APB_SPDMA_PCLKM		0x2020
#define CLK_CON_GAT_GOUT_CORE_AD_AXI_GIC_ACLKM			0x2024
#define CLK_CON_GAT_GOUT_CORE_ASYNCSFR_WR_DMC0_PCLK		0x2028
#define CLK_CON_GAT_GOUT_CORE_ASYNCSFR_WR_DMC1_PCLK		0x202c
#define CLK_CON_GAT_GOUT_CORE_AXI_US_A40_64TO128_DIT_ACLK	0x2030
#define CLK_CON_GAT_GOUT_CORE_BAAW_P_GNSS_PCLK			0x2034
#define CLK_CON_GAT_GOUT_CORE_BAAW_P_MODEM_PCLK			0x2038
#define CLK_CON_GAT_GOUT_CORE_BAAW_P_SHUB_PCLK			0x203c
#define CLK_CON_GAT_GOUT_CORE_BAAW_P_WLBT_PCLK			0x2040
#define CLK_CON_GAT_GOUT_CORE_CCI_550_ACLK			0x2044
#define CLK_CON_GAT_GOUT_CORE_DIT_ICLKL2A			0x2048
#define CLK_CON_GAT_GOUT_CORE_GIC400_AIHWACG_CLK		0x204c
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D0_ISP_CLK		0x2050
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D0_MFC_CLK		0x2054
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D1_ISP_CLK		0x2058
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D1_MFC_CLK		0x205c
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_CAM_CLK		0x2060
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_DPU_CLK		0x2064
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_FSYS_CLK		0x2068
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_G2D_CLK		0x206c
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_USB_CLK		0x2070
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_VIPX1_CLK		0x2074
#define CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_VIPX2_CLK		0x2078
#define CLK_CON_GAT_GOUT_CORE_LHM_ACE_D_CPUCL0_CLK		0x207c
#define CLK_CON_GAT_GOUT_CORE_LHM_ACE_D_CPUCL1_CLK		0x2080
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D0_MODEM_CLK		0x2084
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D1_MODEM_CLK		0x2088
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_ABOX_CLK		0x208c
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_APM_CLK			0x2090
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_CSSYS_CLK		0x2094
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_G3D_CLK			0x2098
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_GNSS_CLK		0x209c
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_SHUB_CLK		0x20a0
#define CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_WLBT_CLK		0x20a4
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_CPU_CLK		0x20a8
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_CP_CLK		0x20ac
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_NRT_CLK		0x20b0
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_RT_CLK		0x20b4
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_CPU_CLK		0x20b8
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_CP_CLK		0x20bc
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_NRT_CLK		0x20c0
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_RT_CLK		0x20c4
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_APM_CLK			0x20c8
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CAM_CLK			0x20cc
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CPUCL0_CLK		0x20d0
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CPUCL1_CLK		0x20d4
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_DISPAUD_CLK		0x20d8
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_FSYS_CLK		0x20dc
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_G2D_CLK			0x20e0
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_G3D_CLK			0x20e4
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_GNSS_CLK		0x20e8
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_ISP_CLK			0x20ec
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MFC_CLK			0x20f0
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MIF0_CLK		0x20f4
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MIF1_CLK		0x20f8
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MODEM_CLK		0x20fc
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_PERI_CLK		0x2100
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_SHUB_CLK		0x2104
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_USB_CLK			0x2108
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_VIPX1_CLK		0x210c
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_VIPX2_CLK		0x2110
#define CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_WLBT_CLK		0x2114
#define CLK_CON_GAT_GOUT_CORE_PDMA_CORE_ACLK_PDMA0		0x2118
#define CLK_CON_GAT_GOUT_CORE_PGEN_LITE_SIREX_CLK		0x211c
#define CLK_CON_GAT_GOUT_CORE_PGEN_PDMA_CLK			0x2120
#define CLK_CON_GAT_GOUT_CORE_PPCFW_G3D_ACLK			0x2124
#define CLK_CON_GAT_GOUT_CORE_PPCFW_G3D_PCLK			0x2128
#define CLK_CON_GAT_GOUT_CORE_PPFW_CORE_MEM0_CLK		0x212c
#define CLK_CON_GAT_GOUT_CORE_PPFW_CORE_MEM1_CLK		0x2130
#define CLK_CON_GAT_GOUT_CORE_PPFW_CORE_PERI_CLK		0x2134
#define CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL0_ACLK		0x2138
#define CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL0_PCLK		0x213c
#define CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL1_ACLK		0x2140
#define CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL1_PCLK		0x2144
#define CLK_CON_GAT_GOUT_CORE_BUSD_CLK				0x2148
#define CLK_CON_GAT_GOUT_CORE_BUSP_G3D_OCC_CLK			0x214c
#define CLK_CON_GAT_GOUT_CORE_BUSP_CLK				0x2150
#define CLK_CON_GAT_GOUT_CORE_BUSP_OCC_CLK			0x2154
#define CLK_CON_GAT_GOUT_CORE_CCI_CLK				0x2158
#define CLK_CON_GAT_GOUT_CORE_CCI_OCC_CLK			0x215c
#define CLK_CON_GAT_GOUT_CORE_G3D_CLK				0x2160
#define CLK_CON_GAT_GOUT_CORE_G3D_OCC_CLK			0x2164
#define CLK_CON_GAT_GOUT_CORE_GIC_CLK				0x2168
#define CLK_CON_GAT_GOUT_CORE_OSCCLK_CLK			0x216c
#define CLK_CON_GAT_GOUT_CORE_SFR_APBIF_CMU_TOPC_PCLK		0x2170
#define CLK_CON_GAT_GOUT_CORE_SIREX_ACLK			0x2174
#define CLK_CON_GAT_GOUT_CORE_SIREX_PCLK			0x2178
#define CLK_CON_GAT_GOUT_CORE_SPDMA_CORE_ACLK_PDMA1		0x217c
#define CLK_CON_GAT_GOUT_CORE_SYSREG_PCLK			0x2180
#define CLK_CON_GAT_GOUT_CORE_TREX_D_ACLK			0x2184
#define CLK_CON_GAT_GOUT_CORE_TREX_D_CCLK			0x2188
#define CLK_CON_GAT_GOUT_CORE_TREX_D_GCLK			0x218c
#define CLK_CON_GAT_GOUT_CORE_TREX_D_PCLK			0x2190
#define CLK_CON_GAT_GOUT_CORE_TREX_D_NRT_ACLK			0x2194
#define CLK_CON_GAT_GOUT_CORE_TREX_D_NRT_PCLK			0x2198
#define CLK_CON_GAT_GOUT_CORE_TREX_P_ACLK_P_CORE		0x219c
#define CLK_CON_GAT_GOUT_CORE_TREX_P_CCLK_P_CORE		0x21a0
#define CLK_CON_GAT_GOUT_CORE_TREX_P_PCLK			0x21a4
#define CLK_CON_GAT_GOUT_CORE_TREX_P_PCLK_P_CORE		0x21a8
#define CLK_CON_GAT_GOUT_CORE_XIU_D_ACLK			0x21ac

static const unsigned long core_clk_regs[] __initconst = {
	PLL_CON0_MUX_CLKCMU_CORE_BUS_USER,
	PLL_CON2_MUX_CLKCMU_CORE_BUS_USER,
	PLL_CON0_MUX_CLKCMU_CORE_CCI_USER,
	PLL_CON2_MUX_CLKCMU_CORE_CCI_USER,
	PLL_CON0_MUX_CLKCMU_CORE_G3D_USER,
	PLL_CON2_MUX_CLKCMU_CORE_G3D_USER,
	CMU_CMU_CORE_CONTROLLER_OPTION,
	CLK_CON_MUX_MUX_CLK_CORE_GIC,
	CLK_CON_DIV_DIV_CLK_CORE_BUSP,
	CLK_CON_GAT_CLK_CORE_CMU_PCLK,
	CLK_CON_GAT_GOUT_CORE_AD_APB_CCI_550_PCLKM,
	CLK_CON_GAT_GOUT_CORE_AD_APB_DIT_PCLKM,
	CLK_CON_GAT_GOUT_CORE_AD_APB_PDMA0_PCLKM,
	CLK_CON_GAT_GOUT_CORE_AD_APB_PGEN_PDMA_PCLKM,
	CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_MEM0_PCLKM,
	CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_MEM1_PCLKM,
	CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_PERI_PCLKM,
	CLK_CON_GAT_GOUT_CORE_AD_APB_SPDMA_PCLKM,
	CLK_CON_GAT_GOUT_CORE_AD_AXI_GIC_ACLKM,
	CLK_CON_GAT_GOUT_CORE_ASYNCSFR_WR_DMC0_PCLK,
	CLK_CON_GAT_GOUT_CORE_ASYNCSFR_WR_DMC1_PCLK,
	CLK_CON_GAT_GOUT_CORE_AXI_US_A40_64TO128_DIT_ACLK,
	CLK_CON_GAT_GOUT_CORE_BAAW_P_GNSS_PCLK,
	CLK_CON_GAT_GOUT_CORE_BAAW_P_MODEM_PCLK,
	CLK_CON_GAT_GOUT_CORE_BAAW_P_SHUB_PCLK,
	CLK_CON_GAT_GOUT_CORE_BAAW_P_WLBT_PCLK,
	CLK_CON_GAT_GOUT_CORE_CCI_550_ACLK,
	CLK_CON_GAT_GOUT_CORE_DIT_ICLKL2A,
	CLK_CON_GAT_GOUT_CORE_GIC400_AIHWACG_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D0_ISP_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D0_MFC_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D1_ISP_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D1_MFC_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_CAM_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_DPU_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_FSYS_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_G2D_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_USB_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_VIPX1_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_VIPX2_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACE_D_CPUCL0_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_ACE_D_CPUCL1_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D0_MODEM_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D1_MODEM_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_ABOX_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_APM_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_CSSYS_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_G3D_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_GNSS_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_SHUB_CLK,
	CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_WLBT_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_CPU_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_CP_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_NRT_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_RT_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_CPU_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_CP_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_NRT_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_RT_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_APM_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CAM_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CPUCL0_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CPUCL1_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_DISPAUD_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_FSYS_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_G2D_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_G3D_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_GNSS_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_ISP_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MFC_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MIF0_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MIF1_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MODEM_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_PERI_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_SHUB_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_USB_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_VIPX1_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_VIPX2_CLK,
	CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_WLBT_CLK,
	CLK_CON_GAT_GOUT_CORE_PDMA_CORE_ACLK_PDMA0,
	CLK_CON_GAT_GOUT_CORE_PGEN_LITE_SIREX_CLK,
	CLK_CON_GAT_GOUT_CORE_PGEN_PDMA_CLK,
	CLK_CON_GAT_GOUT_CORE_PPCFW_G3D_ACLK,
	CLK_CON_GAT_GOUT_CORE_PPCFW_G3D_PCLK,
	CLK_CON_GAT_GOUT_CORE_PPFW_CORE_MEM0_CLK,
	CLK_CON_GAT_GOUT_CORE_PPFW_CORE_MEM1_CLK,
	CLK_CON_GAT_GOUT_CORE_PPFW_CORE_PERI_CLK,
	CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL0_ACLK,
	CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL0_PCLK,
	CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL1_ACLK,
	CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL1_PCLK,
	CLK_CON_GAT_GOUT_CORE_BUSD_CLK,
	CLK_CON_GAT_GOUT_CORE_BUSP_G3D_OCC_CLK,
	CLK_CON_GAT_GOUT_CORE_BUSP_CLK,
	CLK_CON_GAT_GOUT_CORE_BUSP_OCC_CLK,
	CLK_CON_GAT_GOUT_CORE_CCI_CLK,
	CLK_CON_GAT_GOUT_CORE_CCI_OCC_CLK,
	CLK_CON_GAT_GOUT_CORE_G3D_CLK,
	CLK_CON_GAT_GOUT_CORE_G3D_OCC_CLK,
	CLK_CON_GAT_GOUT_CORE_GIC_CLK,
	CLK_CON_GAT_GOUT_CORE_OSCCLK_CLK,
	CLK_CON_GAT_GOUT_CORE_SFR_APBIF_CMU_TOPC_PCLK,
	CLK_CON_GAT_GOUT_CORE_SIREX_ACLK,
	CLK_CON_GAT_GOUT_CORE_SIREX_PCLK,
	CLK_CON_GAT_GOUT_CORE_SPDMA_CORE_ACLK_PDMA1,
	CLK_CON_GAT_GOUT_CORE_SYSREG_PCLK,
	CLK_CON_GAT_GOUT_CORE_TREX_D_ACLK,
	CLK_CON_GAT_GOUT_CORE_TREX_D_CCLK,
	CLK_CON_GAT_GOUT_CORE_TREX_D_GCLK,
	CLK_CON_GAT_GOUT_CORE_TREX_D_PCLK,
	CLK_CON_GAT_GOUT_CORE_TREX_D_NRT_ACLK,
	CLK_CON_GAT_GOUT_CORE_TREX_D_NRT_PCLK,
	CLK_CON_GAT_GOUT_CORE_TREX_P_ACLK_P_CORE,
	CLK_CON_GAT_GOUT_CORE_TREX_P_CCLK_P_CORE,
	CLK_CON_GAT_GOUT_CORE_TREX_P_PCLK,
	CLK_CON_GAT_GOUT_CORE_TREX_P_PCLK_P_CORE,
	CLK_CON_GAT_GOUT_CORE_XIU_D_ACLK,
};

PNAME(mout_pll_core_bus_user_p)		= { "oscclk",
					    "dout_cmu_core_bus" };
PNAME(mout_pll_core_cci_user_p)	=	 { "oscclk",
					    "dout_cmu_core_cci" };
PNAME(mout_pll_core_g3d_user_p)	=	 { "oscclk",
					    "dout_cmu_core_g3d" };
PNAME(mout_clk_core_gic_p)		= { "oscclk",
					    "dout_clk_core_busp" };

static const struct samsung_mux_clock core_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_CORE_BUS_USER, "mout_pll_core_bus_user", mout_pll_core_bus_user_p,
	    PLL_CON0_MUX_CLKCMU_CORE_BUS_USER, 4, 1),
	MUX(CLK_MOUT_PLL_CORE_CCI_USER, "mout_pll_core_cci_user", mout_pll_core_cci_user_p,
	    PLL_CON0_MUX_CLKCMU_CORE_CCI_USER, 4, 1),
	MUX(CLK_MOUT_PLL_CORE_G3D_USER, "mout_pll_core_g3d_user", mout_pll_core_g3d_user_p,
	    PLL_CON0_MUX_CLKCMU_CORE_G3D_USER, 4, 1),
	MUX(CLK_MOUT_CLK_CORE_GIC, "mout_clk_core_gic", mout_clk_core_gic_p,
	    CLK_CON_MUX_MUX_CLK_CORE_GIC, 0, 1),
};

static const struct samsung_div_clock core_div_clks[] __initconst = {
	DIV(CLK_DOUT_CLK_CORE_BUSP, "dout_clk_core_busp", "mout_pll_core_bus_user",
	    CLK_CON_DIV_DIV_CLK_CORE_BUSP, 0, 2),
};

static const struct samsung_gate_clock core_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CLK_CORE_CMU_PCLK, "gout_clk_core_cmu_pclk", "dout_clk_core_busp",
	     CLK_CON_GAT_CLK_CORE_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_APB_CCI_550_PCLKM, "gout_core_ad_apb_cci_550_pclkm",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_AD_APB_CCI_550_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_APB_DIT_PCLKM, "gout_core_ad_apb_dit_pclkm",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_AD_APB_DIT_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_APB_PDMA0_PCLKM, "gout_core_ad_apb_pdma0_pclkm",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_AD_APB_PDMA0_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_APB_PGEN_PDMA_PCLKM, "gout_core_ad_apb_pgen_pdma_pclkm",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_AD_APB_PGEN_PDMA_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_APB_PPFW_MEM0_PCLKM, "gout_core_ad_apb_ppfw_mem0_pclkm",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_MEM0_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_APB_PPFW_MEM1_PCLKM, "gout_core_ad_apb_ppfw_mem1_pclkm",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_MEM1_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_APB_PPFW_PERI_PCLKM, "gout_core_ad_apb_ppfw_peri_pclkm",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_AD_APB_PPFW_PERI_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_APB_SPDMA_PCLKM, "gout_core_ad_apb_spdma_pclkm",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_AD_APB_SPDMA_PCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AD_AXI_GIC_ACLKM, "gout_core_ad_axi_gic_aclkm",
	     "mout_clk_core_gic", CLK_CON_GAT_GOUT_CORE_AD_AXI_GIC_ACLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_ASYNCSFR_WR_DMC0_PCLK, "gout_core_asyncsfr_wr_dmc0_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_ASYNCSFR_WR_DMC0_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_ASYNCSFR_WR_DMC1_PCLK, "gout_core_asyncsfr_wr_dmc1_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_ASYNCSFR_WR_DMC1_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_AXI_US_A40_64TO128_DIT_ACLK,
	     "gout_core_axi_us_a40_64to128_dit_aclk", "mout_pll_core_bus_user",
	     CLK_CON_GAT_GOUT_CORE_AXI_US_A40_64TO128_DIT_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_BAAW_P_GNSS_PCLK, "gout_core_baaw_p_gnss_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_BAAW_P_GNSS_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_BAAW_P_MODEM_PCLK, "gout_core_baaw_p_modem_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_BAAW_P_MODEM_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_BAAW_P_SHUB_PCLK, "gout_core_baaw_p_shub_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_BAAW_P_SHUB_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_BAAW_P_WLBT_PCLK, "gout_core_baaw_p_wlbt_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_BAAW_P_WLBT_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_CCI_550_ACLK, "gout_core_cci_550_aclk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_CCI_550_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_DIT_ICLKL2A, "gout_core_dit_iclkl2a",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_DIT_ICLKL2A,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_GIC400_AIHWACG_CLK, "gout_core_gic400_aihwacg_clk",
	     "mout_clk_core_gic", CLK_CON_GAT_GOUT_CORE_GIC400_AIHWACG_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D0_ISP_CLK, "gout_core_lhm_acel_d0_isp_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D0_ISP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D0_MFC_CLK, "gout_core_lhm_acel_d0_mfc_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D0_MFC_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D1_ISP_CLK, "gout_core_lhm_acel_d1_isp_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D1_ISP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D1_MFC_CLK, "gout_core_lhm_acel_d1_mfc_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D1_MFC_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D_CAM_CLK, "gout_core_lhm_acel_d_cam_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_CAM_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D_DPU_CLK, "gout_core_lhm_acel_d_dpu_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_DPU_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D_FSYS_CLK, "gout_core_lhm_acel_d_fsys_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_FSYS_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D_G2D_CLK, "gout_core_lhm_acel_d_g2d_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_G2D_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D_USB_CLK, "gout_core_lhm_acel_d_usb_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_USB_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D_VIPX1_CLK, "gout_core_lhm_acel_d_vipx1_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_VIPX1_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACEL_D_VIPX2_CLK, "gout_core_lhm_acel_d_vipx2_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_ACEL_D_VIPX2_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACE_D_CPUCL0_CLK, "gout_core_lhm_ace_d_cpucl0_clk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_LHM_ACE_D_CPUCL0_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_ACE_D_CPUCL1_CLK, "gout_core_lhm_ace_d_cpucl1_clk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_LHM_ACE_D_CPUCL1_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D0_MODEM_CLK, "gout_core_lhm_axi_d0_modem_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D0_MODEM_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D1_MODEM_CLK, "gout_core_lhm_axi_d1_modem_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D1_MODEM_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D_ABOX_CLK, "gout_core_lhm_axi_d_abox_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_ABOX_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D_APM_CLK, "gout_core_lhm_axi_d_apm_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_APM_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D_CSSYS_CLK, "gout_core_lhm_axi_d_cssys_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_CSSYS_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D_G3D_CLK, "gout_core_lhm_axi_d_g3d_clk",
	     "mout_pll_core_g3d_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_G3D_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D_GNSS_CLK, "gout_core_lhm_axi_d_gnss_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_GNSS_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D_SHUB_CLK, "gout_core_lhm_axi_d_shub_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_SHUB_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHM_AXI_D_WLBT_CLK, "gout_core_lhm_axi_d_wlbt_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHM_AXI_D_WLBT_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_D0_MIF_CPU_CLK, "gout_core_lhs_axi_d0_mif_cpu_clk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_CPU_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_D0_MIF_CP_CLK, "gout_core_lhs_axi_d0_mif_cp_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_CP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_D0_MIF_NRT_CLK, "gout_core_lhs_axi_d0_mif_nrt_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_NRT_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_D0_MIF_RT_CLK, "gout_core_lhs_axi_d0_mif_rt_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHS_AXI_D0_MIF_RT_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_D1_MIF_CPU_CLK, "gout_core_lhs_axi_d1_mif_cpu_clk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_CPU_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_D1_MIF_CP_CLK, "gout_core_lhs_axi_d1_mif_cp_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_CP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_D1_MIF_NRT_CLK, "gout_core_lhs_axi_d1_mif_nrt_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_NRT_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_D1_MIF_RT_CLK, "gout_core_lhs_axi_d1_mif_rt_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_LHS_AXI_D1_MIF_RT_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_APM_CLK, "gout_core_lhs_axi_p_apm_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_APM_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_CAM_CLK, "gout_core_lhs_axi_p_cam_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CAM_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_CPUCL0_CLK, "gout_core_lhs_axi_p_cpucl0_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CPUCL0_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_CPUCL1_CLK, "gout_core_lhs_axi_p_cpucl1_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_CPUCL1_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_DISPAUD_CLK, "gout_core_lhs_axi_p_dispaud_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_DISPAUD_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_FSYS_CLK, "gout_core_lhs_axi_p_fsys_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_FSYS_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_G2D_CLK, "gout_core_lhs_axi_p_g2d_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_G2D_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_G3D_CLK, "gout_core_lhs_axi_p_g3d_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_G3D_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_GNSS_CLK, "gout_core_lhs_axi_p_gnss_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_GNSS_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_ISP_CLK, "gout_core_lhs_axi_p_isp_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_ISP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_MFC_CLK, "gout_core_lhs_axi_p_mfc_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MFC_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_MIF0_CLK, "gout_core_lhs_axi_p_mif0_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MIF0_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_MIF1_CLK, "gout_core_lhs_axi_p_mif1_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MIF1_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_MODEM_CLK, "gout_core_lhs_axi_p_modem_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_MODEM_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_PERI_CLK, "gout_core_lhs_axi_p_peri_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_PERI_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_SHUB_CLK, "gout_core_lhs_axi_p_shub_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_SHUB_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_USB_CLK, "gout_core_lhs_axi_p_usb_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_USB_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_VIPX1_CLK, "gout_core_lhs_axi_p_vipx1_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_VIPX1_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_VIPX2_CLK, "gout_core_lhs_axi_p_vipx2_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_VIPX2_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_LHS_AXI_P_WLBT_CLK, "gout_core_lhs_wlbt_p_apm_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_LHS_AXI_P_WLBT_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PDMA_CORE_ACLK_PDMA0, "gout_core_pdma_core_aclk_pdma0",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_PDMA_CORE_ACLK_PDMA0,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PGEN_LITE_SIREX_CLK, "gout_core_pgen_lite_sirex_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_PGEN_LITE_SIREX_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PGEN_PDMA_CLK, "gout_core_pgen_pdma_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_PGEN_PDMA_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPCFW_G3D_ACLK, "gout_core_ppcfw_g3d_aclk",
	     "mout_pll_core_g3d_user", CLK_CON_GAT_GOUT_CORE_PPCFW_G3D_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPCFW_G3D_PCLK, "gout_core_ppcfw_g3d_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_PPCFW_G3D_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPFW_CORE_MEM0_CLK, "gout_core_ppfw_core_mem0_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_PPFW_CORE_MEM0_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPFW_CORE_MEM1_CLK, "gout_core_ppfw_core_mem1_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_PPFW_CORE_MEM1_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPFW_CORE_PERI_CLK, "gout_core_ppfw_core_peri_clk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_PPFW_CORE_PERI_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPMU_ACE_CPUCL0_ACLK, "gout_core_ppmu_ace_cpucl0_aclk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL0_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPMU_ACE_CPUCL0_PCLK, "gout_core_ppmu_ace_cpucl0_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL0_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPMU_ACE_CPUCL1_ACLK, "gout_core_ppmu_ace_cpucl1_aclk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL1_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_PPMU_ACE_CPUCL1_PCLK, "gout_core_ppmu_ace_cpucl1_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_PPMU_ACE_CPUCL1_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_BUSD_CLK, "gout_core_busd_clk", "mout_pll_core_bus_user",
	     CLK_CON_GAT_GOUT_CORE_BUSD_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_BUSP_G3D_OCC_CLK, "gout_core_busp_g3d_occ_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_BUSP_G3D_OCC_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_BUSP_CLK, "gout_core_busp_clk", "dout_clk_core_busp",
	     CLK_CON_GAT_GOUT_CORE_BUSP_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_BUSP_OCC_CLK, "gout_core_busp_occ_clk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_BUSP_OCC_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_CCI_CLK, "gout_core_cci_clk", "mout_pll_core_cci_user",
	     CLK_CON_GAT_GOUT_CORE_CCI_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_CCI_OCC_CLK, "gout_core_cci_occ_clk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_CCI_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_G3D_CLK, "gout_core_g3d_clk", "mout_pll_core_g3d_user",
	     CLK_CON_GAT_GOUT_CORE_G3D_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_G3D_OCC_CLK, "gout_core_cci_g3d_occ_clk",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_G3D_OCC_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_GIC_CLK, "gout_core_gic_clk", "mout_clk_core_gic",
	     CLK_CON_GAT_GOUT_CORE_GIC_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_OSCCLK_CLK, "gout_core_oscclk_clk", "oscclk",
	     CLK_CON_GAT_GOUT_CORE_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_SFR_APBIF_CMU_TOPC_PCLK, "gout_core_sfr_apbif_cmu_topc_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_SFR_APBIF_CMU_TOPC_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_SIREX_ACLK, "gout_core_sirex_aclk", "mout_pll_core_bus_user",
	     CLK_CON_GAT_GOUT_CORE_SIREX_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_SIREX_PCLK, "gout_core_sirex_pclk", "dout_clk_core_busp",
	     CLK_CON_GAT_GOUT_CORE_SIREX_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_SPDMA_CORE_ACLK_PDMA1, "gout_core_spdma_core_aclk_pdma1",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_SPDMA_CORE_ACLK_PDMA1,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_SYSREG_PCLK, "gout_core_sysreg_pclk", "dout_clk_core_busp",
	     CLK_CON_GAT_GOUT_CORE_SYSREG_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_D_ACLK, "gout_core_trex_d_aclk", "mout_pll_core_bus_user",
	     CLK_CON_GAT_GOUT_CORE_TREX_D_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_D_CCLK, "gout_core_trex_d_cclk", "mout_pll_core_cci_user",
	     CLK_CON_GAT_GOUT_CORE_TREX_D_CCLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_D_GCLK, "gout_core_trex_d_gclk", "mout_pll_core_g3d_user",
	     CLK_CON_GAT_GOUT_CORE_TREX_D_GCLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_D_PCLK, "gout_core_trex_d_pclk", "dout_clk_core_busp",
	     CLK_CON_GAT_GOUT_CORE_TREX_D_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_D_NRT_ACLK, "gout_core_trex_d_nrt_aclk",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_TREX_D_NRT_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_D_NRT_PCLK, "gout_core_trex_d_nrt_pclk",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_TREX_D_NRT_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_P_ACLK_P_CORE, "gout_core_trex_p_aclk_p_core",
	     "mout_pll_core_bus_user", CLK_CON_GAT_GOUT_CORE_TREX_P_ACLK_P_CORE,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_P_CCLK_P_CORE, "gout_core_trex_p_cclk_p_core",
	     "mout_pll_core_cci_user", CLK_CON_GAT_GOUT_CORE_TREX_P_CCLK_P_CORE,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_P_PCLK, "gout_core_trex_p_pclk", "dout_clk_core_busp",
	     CLK_CON_GAT_GOUT_CORE_TREX_P_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CORE_TREX_P_PCLK_P_CORE, "gout_core_trex_p_pclk_p_core",
	     "dout_clk_core_busp", CLK_CON_GAT_GOUT_CORE_TREX_P_PCLK_P_CORE,
	     21, 0, 0),
	GATE(CLK_GOUT_CORE_XIU_D_ACLK, "gout_core_xiu_d_aclk", "mout_pll_core_bus_user",
	     CLK_CON_GAT_GOUT_CORE_XIU_D_ACLK, 21, 0, 0),
};

static const struct samsung_cmu_info core_cmu_info __initconst = {
	.mux_clks		= core_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(core_mux_clks),
	.gate_clks		= core_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(core_gate_clks),
	.clk_regs		= core_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(core_clk_regs),
	.nr_clk_ids		= CLKS_NR_CORE,
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_CORE_CONTROLLER_OPTION,
};

/* CMU_DISPAUD */
#define PLL_LOCKTIME_PLL_AUD					0x0000
#define PLL_CON0_MUX_CLKCMU_DISPAUD_AUD_USER			0x0100
#define PLL_CON2_MUX_CLKCMU_DISPAUD_AUD_USER			0x0108
#define PLL_CON0_MUX_CLKCMU_DISPAUD_CPU_USER			0x0120
#define PLL_CON2_MUX_CLKCMU_DISPAUD_CPU_USER			0x0128
#define PLL_CON0_MUX_CLKCMU_DISPAUD_DISP_USER			0x0140
#define PLL_CON2_MUX_CLKCMU_DISPAUD_DISP_USER			0x0148
#define PLL_CON0_PLL_AUD					0x0160
#define PLL_CON3_PLL_AUD					0x016c
#define CMU_CMU_DISPAUD_CONTROLLER_OPTION			0x0800
#define CLK_CON_MUX_MUX_CLK_AUD_BUS				0x1000
#define CLK_CON_MUX_MUX_CLK_AUD_CPU				0x1004
#define CLK_CON_MUX_MUX_CLK_AUD_CPU_HCH				0x1008
#define CLK_CON_MUX_MUX_CLK_AUD_FM				0x100c
#define CLK_CON_MUX_MUX_CLK_AUD_UAIF0				0x1010
#define CLK_CON_MUX_MUX_CLK_AUD_UAIF1				0x1014
#define CLK_CON_MUX_MUX_CLK_AUD_UAIF2				0x1018
#define CLK_CON_DIV_DIV_CLK_AUD_AUDIF				0x1800
#define CLK_CON_DIV_DIV_CLK_AUD_BUS				0x1808
#define CLK_CON_DIV_DIV_CLK_AUD_CPU				0x180c
#define CLK_CON_DIV_DIV_CLK_AUD_CPU_ACLK			0x1810
#define CLK_CON_DIV_DIV_CLK_AUD_CPU_PCLKDBG			0x1814
#define CLK_CON_DIV_DIV_CLK_AUD_DSIF				0x1818
#define CLK_CON_DIV_DIV_CLK_AUD_FM				0x181c
#define CLK_CON_DIV_DIV_CLK_AUD_FM_SPDY				0x1820
#define CLK_CON_DIV_DIV_CLK_AUD_UAIF0				0x1824
#define CLK_CON_DIV_DIV_CLK_AUD_UAIF1				0x1828
#define CLK_CON_DIV_DIV_CLK_AUD_UAIF2				0x182c
#define CLK_CON_DIV_DIV_CLK_DISPAUD_BUSP			0x1830
#define CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF0			0x2000
#define CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF1			0x2004
#define CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF2			0x2008
#define CLK_CON_GAT_CLK_DISPAUD_CMU_PCLK			0x200c
#define CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF0_CLK		0x2010
#define CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF1_CLK		0x2014
#define CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF2_CLK		0x2018
#define CLK_CON_GAT_CLK_DISPAUD_OSCCLK_CLK			0x201c
#define CLK_CON_GAT_GOUT_DISPAUD_ABOX_ACLK			0x2020
#define CLK_CON_GAT_GOUT_DISPAUD_ABOX_BCLK_DSIF			0x2024
#define CLK_CON_GAT_GOUT_DISPAUD_ABOX_BCLK_SPDY			0x2028
#define CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_ASB			0x202c
#define CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_CA7			0x2030
#define CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_DBG			0x2034
#define CLK_CON_GAT_GOUT_DISPAUD_ABOX_OSC_SPDY			0x2038
#define CLK_CON_GAT_GOUT_DISPAUD_AXI_US_32TO128_ACLK		0x203c
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_DISPAUD_AUD		0x2040
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_DISPAUD_DISP		0x2044
#define CLK_CON_GAT_GOUT_DISPAUD_BTM_ABOX_ACLK			0x2048
#define CLK_CON_GAT_GOUT_DISPAUD_BTM_ABOX_PCLK			0x204c
#define CLK_CON_GAT_GOUT_DISPAUD_BTM_DPU_ACLK			0x2050
#define CLK_CON_GAT_GOUT_DISPAUD_BTM_DPU_PCLK			0x2054
#define CLK_CON_GAT_GOUT_DISPAUD_DFTMUX_AUD_CODEC_MCLK		0x2058
#define CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DECON			0x2060
#define CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DMA			0x2064
#define CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DPP			0x2068
#define CLK_CON_GAT_GOUT_DISPAUD_GPIO_DISPAUD_PCLK		0x206c
#define CLK_CON_GAT_GOUT_DISPAUD_LHM_AXI_P_DISPAUD_CLK		0x2070
#define CLK_CON_GAT_GOUT_DISPAUD_LHS_ACEL_D_DPU_CLK		0x2074
#define CLK_CON_GAT_GOUT_DISPAUD_LHS_AXI_D_ABOX_CLK		0x2078
#define CLK_CON_GAT_GOUT_DISPAUD_PERI_AXI_ASB_ACLKM		0x207c
#define CLK_CON_GAT_GOUT_DISPAUD_PERI_AXI_ASB_PCLK		0x2080
#define CLK_CON_GAT_GOUT_DISPAUD_PPMU_ABOX_ACLK			0x2084
#define CLK_CON_GAT_GOUT_DISPAUD_PPMU_ABOX_PCLK			0x2088
#define CLK_CON_GAT_GOUT_DISPAUD_PPMU_DPU_ACLK			0x208c
#define CLK_CON_GAT_GOUT_DISPAUD_PPMU_DPU_PCLK			0x2090
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_ACLK_CLK		0x2094
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_CLKIN_CLK		0x2098
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_PCLKDBG_CLK	0x209c
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_DSIF_CLK		0x20a0
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CLK			0x20a4
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_BUSP_CLK			0x20a8
#define CLK_CON_GAT_GOUT_DISPAUD_CLK_DISP_CLK			0x20ac
#define CLK_CON_GAT_GOUT_DISPAUD_SMMU_ABOX_CLK			0x20b0
#define CLK_CON_GAT_GOUT_DISPAUD_SMMU_DPU_CLK			0x20b4
#define CLK_CON_GAT_GOUT_DISPAUD_SYSREG_PCLK			0x20b8
#define CLK_CON_GAT_GOUT_DISPAUD_WDT_AUD_PCLK			0x20bc

static const unsigned long dispaud_clk_regs[] __initconst = {
	PLL_LOCKTIME_PLL_AUD,
	PLL_CON0_MUX_CLKCMU_DISPAUD_AUD_USER,
	PLL_CON2_MUX_CLKCMU_DISPAUD_AUD_USER,
	PLL_CON0_MUX_CLKCMU_DISPAUD_CPU_USER,
	PLL_CON2_MUX_CLKCMU_DISPAUD_CPU_USER,
	PLL_CON0_MUX_CLKCMU_DISPAUD_DISP_USER,
	PLL_CON2_MUX_CLKCMU_DISPAUD_DISP_USER,
	PLL_CON0_PLL_AUD,
	PLL_CON3_PLL_AUD,
	CMU_CMU_DISPAUD_CONTROLLER_OPTION,
	CLK_CON_MUX_MUX_CLK_AUD_BUS,
	CLK_CON_MUX_MUX_CLK_AUD_CPU,
	CLK_CON_MUX_MUX_CLK_AUD_CPU_HCH,
	CLK_CON_MUX_MUX_CLK_AUD_FM,
	CLK_CON_MUX_MUX_CLK_AUD_UAIF0,
	CLK_CON_MUX_MUX_CLK_AUD_UAIF1,
	CLK_CON_MUX_MUX_CLK_AUD_UAIF2,
	CLK_CON_DIV_DIV_CLK_AUD_AUDIF,
	CLK_CON_DIV_DIV_CLK_AUD_BUS,
	CLK_CON_DIV_DIV_CLK_AUD_CPU,
	CLK_CON_DIV_DIV_CLK_AUD_CPU_ACLK,
	CLK_CON_DIV_DIV_CLK_AUD_CPU_PCLKDBG,
	CLK_CON_DIV_DIV_CLK_AUD_DSIF,
	CLK_CON_DIV_DIV_CLK_AUD_FM,
	CLK_CON_DIV_DIV_CLK_AUD_FM_SPDY,
	CLK_CON_DIV_DIV_CLK_AUD_UAIF0,
	CLK_CON_DIV_DIV_CLK_AUD_UAIF1,
	CLK_CON_DIV_DIV_CLK_AUD_UAIF2,
	CLK_CON_DIV_DIV_CLK_DISPAUD_BUSP,
	CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF0,
	CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF1,
	CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF2,
	CLK_CON_GAT_CLK_DISPAUD_CMU_PCLK,
	CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF0_CLK,
	CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF1_CLK,
	CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF2_CLK,
	CLK_CON_GAT_CLK_DISPAUD_OSCCLK_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_ABOX_ACLK,
	CLK_CON_GAT_GOUT_DISPAUD_ABOX_BCLK_DSIF,
	CLK_CON_GAT_GOUT_DISPAUD_ABOX_BCLK_SPDY,
	CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_ASB,
	CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_CA7,
	CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_DBG,
	CLK_CON_GAT_GOUT_DISPAUD_ABOX_OSC_SPDY,
	CLK_CON_GAT_GOUT_DISPAUD_AXI_US_32TO128_ACLK,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_DISPAUD_AUD,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_DISPAUD_DISP,
	CLK_CON_GAT_GOUT_DISPAUD_BTM_ABOX_ACLK,
	CLK_CON_GAT_GOUT_DISPAUD_BTM_ABOX_PCLK,
	CLK_CON_GAT_GOUT_DISPAUD_BTM_DPU_ACLK,
	CLK_CON_GAT_GOUT_DISPAUD_BTM_DPU_PCLK,
	CLK_CON_GAT_GOUT_DISPAUD_DFTMUX_AUD_CODEC_MCLK,
	CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DECON,
	CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DMA,
	CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DPP,
	CLK_CON_GAT_GOUT_DISPAUD_GPIO_DISPAUD_PCLK,
	CLK_CON_GAT_GOUT_DISPAUD_LHM_AXI_P_DISPAUD_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_LHS_ACEL_D_DPU_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_LHS_AXI_D_ABOX_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_PERI_AXI_ASB_ACLKM,
	CLK_CON_GAT_GOUT_DISPAUD_PERI_AXI_ASB_PCLK,
	CLK_CON_GAT_GOUT_DISPAUD_PPMU_ABOX_ACLK,
	CLK_CON_GAT_GOUT_DISPAUD_PPMU_ABOX_PCLK,
	CLK_CON_GAT_GOUT_DISPAUD_PPMU_DPU_ACLK,
	CLK_CON_GAT_GOUT_DISPAUD_PPMU_DPU_PCLK,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_ACLK_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_CLKIN_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_PCLKDBG_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_DSIF_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_BUSP_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_CLK_DISP_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_SMMU_ABOX_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_SMMU_DPU_CLK,
	CLK_CON_GAT_GOUT_DISPAUD_SYSREG_PCLK,
	CLK_CON_GAT_GOUT_DISPAUD_WDT_AUD_PCLK,
};

static const struct samsung_pll_rate_table pll_aud_rate_table[] __initconst = {
	PLL_36XX_RATE(26 * MHZ, 1179648040, 45, 1, 0, 24319),
	PLL_36XX_RATE(26 * MHZ, 1083801605, 42, 1, 0, -20665),
};

static const struct samsung_pll_clock dispaud_pll_clks[] __initconst = {
	PLL(pll_1061x, CLK_FOUT_AUD_PLL, "fout_aud_pll", "oscclk",
	    PLL_LOCKTIME_PLL_AUD, PLL_CON0_PLL_AUD, pll_aud_rate_table),
};

PNAME(mout_pll_dispaud_aud_user_p)	= { "oscclk",
					    "dout_cmu_dispaud_aud" };
PNAME(mout_pll_dispaud_cpu_user_p)	= { "oscclk",
					    "dout_cmu_dispaud_cpu" };
PNAME(mout_pll_dispaud_disp_user_p)	= { "oscclk",
					    "dout_cmu_dispaud_disp" };
PNAME(mout_clk_aud_bus_p)		= { "dout_clk_aud_bus",
					    "mout_pll_dispaud_aud_user" };
PNAME(mout_clk_aud_cpu_p)		= { "dout_clk_aud_cpu",
					    "mout_pll_dispaud_cpu_user" };
PNAME(mout_clk_aud_cpu_hch_p)		= { "mout_clk_aud_cpu", "oscclk" };
PNAME(mout_clk_aud_fm_p)		= { "oscclk",
					    "dout_clk_aud_fm_spdy" };
PNAME(mout_clk_aud_uaif0_p)		= { "dout_clk_aud_uaif0",
					    "ioclk_audiocdclk0" };
PNAME(mout_clk_aud_uaif1_p)		= { "dout_clk_aud_uaif1",
					    "ioclk_audiocdclk1" };
PNAME(mout_clk_aud_uaif2_p)		= { "dout_clk_aud_uaif2",
					    "ioclk_audiocdclk0" };

static const struct samsung_mux_clock dispaud_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_DISPAUD_AUD_USER, "mout_pll_dispaud_aud_user", mout_pll_dispaud_aud_user_p,
	    PLL_CON0_MUX_CLKCMU_DISPAUD_AUD_USER, 4, 1),
	MUX(CLK_MOUT_PLL_DISPAUD_CPU_USER, "mout_pll_dispaud_cpu_user", mout_pll_dispaud_cpu_user_p,
	    PLL_CON0_MUX_CLKCMU_DISPAUD_CPU_USER, 4, 1),
	MUX(CLK_MOUT_PLL_DISPAUD_DISP_USER, "mout_pll_dispaud_disp_user",
	    mout_pll_dispaud_disp_user_p, PLL_CON0_MUX_CLKCMU_DISPAUD_DISP_USER, 4, 1),
	MUX(CLK_MOUT_CLK_AUD_BUS, "mout_clk_aud_bus", mout_clk_aud_bus_p,
	    CLK_CON_MUX_MUX_CLK_AUD_BUS, 0, 1),
	MUX(CLK_MOUT_CLK_AUD_CPU, "mout_clk_aud_cpu", mout_clk_aud_cpu_p,
	    CLK_CON_MUX_MUX_CLK_AUD_CPU, 0, 1),
	MUX(CLK_MOUT_CLK_AUD_CPU_HCH, "mout_clk_aud_cpu_hch", mout_clk_aud_cpu_hch_p,
	    CLK_CON_MUX_MUX_CLK_AUD_CPU_HCH, 0, 1),
	MUX(CLK_MOUT_CLK_AUD_FM, "mout_clk_aud_fm", mout_clk_aud_fm_p,
	    CLK_CON_MUX_MUX_CLK_AUD_FM, 0, 1),
	MUX(CLK_MOUT_CLK_AUD_UAIF0, "mout_clk_aud_uaif0", mout_clk_aud_uaif0_p,
	    CLK_CON_MUX_MUX_CLK_AUD_UAIF0, 0, 1),
	MUX(CLK_MOUT_CLK_AUD_UAIF1, "mout_clk_aud_uaif1", mout_clk_aud_uaif1_p,
	    CLK_CON_MUX_MUX_CLK_AUD_UAIF1, 0, 1),
	MUX(CLK_MOUT_CLK_AUD_UAIF2, "mout_clk_aud_uaif2", mout_clk_aud_uaif2_p,
	    CLK_CON_MUX_MUX_CLK_AUD_UAIF2, 0, 1),
};

static const struct samsung_div_clock dispaud_div_clks[] __initconst = {
	DIV(CLK_DOUT_CLK_AUD_AUDIF, "dout_clk_aud_audif", "fout_aud_pll",
	    CLK_CON_DIV_DIV_CLK_AUD_AUDIF, 0, 9),
	DIV(CLK_DOUT_CLK_AUD_BUS, "dout_clk_aud_bus", "fout_aud_pll",
	    CLK_CON_DIV_DIV_CLK_AUD_BUS, 0, 3),
	DIV(CLK_DOUT_CLK_AUD_CPU, "dout_clk_aud_cpu", "fout_aud_pll",
	    CLK_CON_DIV_DIV_CLK_AUD_CPU, 0, 4),
	DIV(CLK_DOUT_CLK_AUD_CPU_ACLK, "dout_clk_aud_cpu_aclk", "mout_clk_aud_cpu_hch",
	    CLK_CON_DIV_DIV_CLK_AUD_CPU_ACLK, 0, 3),
	DIV(CLK_DOUT_CLK_AUD_CPU_PCLKDBG, "dout_clk_aud_cpu_pclkdbg", "mout_clk_aud_cpu_hch",
	    CLK_CON_DIV_DIV_CLK_AUD_CPU_PCLKDBG, 0, 3),
	DIV(CLK_DOUT_CLK_AUD_DSIF, "dout_clk_aud_dsif", "dout_clk_aud_audif",
	    CLK_CON_DIV_DIV_CLK_AUD_DSIF, 0, 9),
	DIV(CLK_DOUT_CLK_AUD_FM, "dout_clk_aud_fm", "mout_clk_aud_fm",
	    CLK_CON_DIV_DIV_CLK_AUD_FM, 0, 9),
	DIV(CLK_DOUT_CLK_AUD_FM_SPDY, "dout_clk_aud_fm_spdy", "tick_usb",
	    CLK_CON_DIV_DIV_CLK_AUD_FM_SPDY, 0, 9),
	DIV(CLK_DOUT_CLK_AUD_UAIF0, "dout_clk_aud_uaif0", "dout_clk_aud_audif",
	    CLK_CON_DIV_DIV_CLK_AUD_UAIF0, 0, 9),
	DIV(CLK_DOUT_CLK_AUD_UAIF1, "dout_clk_aud_uaif1", "dout_clk_aud_audif",
	    CLK_CON_DIV_DIV_CLK_AUD_UAIF1, 0, 9),
	DIV(CLK_DOUT_CLK_AUD_UAIF2, "dout_clk_aud_uaif2", "dout_clk_aud_audif",
	    CLK_CON_DIV_DIV_CLK_AUD_UAIF2, 0, 9),
	DIV(CLK_DOUT_CLK_DISPAUD_BUSP, "dout_clk_dispaud_busp", "mout_pll_dispaud_disp_user",
	    CLK_CON_DIV_DIV_CLK_DISPAUD_BUSP, 0, 3),
};

static const struct samsung_gate_clock dispaud_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CLK_DISPAUD_ABOX_BCLK_UAIF0, "gout_clk_dispaud_abox_bclk_uaif0",
	     "mout_clk_aud_uaif0", CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF0,
	     21, 0, 0),
	GATE(CLK_GOUT_CLK_DISPAUD_ABOX_BCLK_UAIF1, "gout_clk_dispaud_abox_bclk_uaif1",
	     "mout_clk_aud_uaif1", CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF1,
	     21, 0, 0),
	GATE(CLK_GOUT_CLK_DISPAUD_ABOX_BCLK_UAIF2, "gout_clk_dispaud_abox_bclk_uaif2",
	     "mout_clk_aud_uaif2", CLK_CON_GAT_CLK_DISPAUD_ABOX_BCLK_UAIF2,
	     21, 0, 0),
	GATE(CLK_GOUT_CLK_DISPAUD_CMU_PCLK, "gout_clk_dispaud_cmu_pclk",
	     "dout_clk_dispaud_busp", CLK_CON_GAT_CLK_DISPAUD_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_DISPAUD_CLK_AUD_UAIF0_CLK, "gout_clk_dispaud_clk_aud_uaif0_clk",
	     "mout_clk_aud_uaif0", CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF0_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CLK_DISPAUD_CLK_AUD_UAIF1_CLK, "gout_clk_dispaud_clk_aud_uaif1_clk",
	     "mout_clk_aud_uaif1", CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF1_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CLK_DISPAUD_CLK_AUD_UAIF2_CLK, "gout_clk_dispaud_clk_aud_uaif2_clk",
	     "mout_clk_aud_uaif2", CLK_CON_GAT_CLK_DISPAUD_CLK_AUD_UAIF2_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_CLK_DISPAUD_OSCCLK_CLK, "gout_clk_dispaud_oscclk_clk", "oscclk",
	     CLK_CON_GAT_CLK_DISPAUD_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_ABOX_ACLK, "gout_clk_dispaud_abox_aclk", "mout_clk_aud_bus",
	     CLK_CON_GAT_GOUT_DISPAUD_ABOX_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_ABOX_BCLK_DSIF, "gout_clk_dispaud_abox_bclk_dsif",
	     "dout_clk_aud_dsif", CLK_CON_GAT_GOUT_DISPAUD_ABOX_BCLK_DSIF,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_ABOX_BCLK_SPDY, "gout_clk_dispaud_abox_bclk_spdy",
	     "dout_clk_aud_fm", CLK_CON_GAT_GOUT_DISPAUD_ABOX_BCLK_SPDY,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_ABOX_CCLK_ASB, "gout_clk_dispaud_abox_cclk_asb",
	     "dout_clk_aud_cpu_aclk", CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_ASB,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_ABOX_CCLK_CA7, "gout_clk_dispaud_abox_cclk_ca7",
	     "mout_clk_aud_cpu_hch", CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_CA7,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_ABOX_CCLK_DBG, "gout_clk_dispaud_abox_cclk_dbg",
	     "dout_clk_aud_cpu_pclkdbg", CLK_CON_GAT_GOUT_DISPAUD_ABOX_CCLK_DBG,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_ABOX_OSC_SPDY, "gout_clk_dispaud_abox_osc_spdy",
	     "mout_clk_aud_fm", CLK_CON_GAT_GOUT_DISPAUD_ABOX_OSC_SPDY, 21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_AXI_US_32TO128_ACLK, "gout_dispaud_axi_us_32to128_aclk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_AXI_US_32TO128_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_DISPAUD_AUD, "gout_dispaud_clk_dispaud_aud",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_CLK_DISPAUD_AUD,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_DISPAUD_DISP, "gout_dispaud_clk_dispaud_disp",
	     "mout_pll_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_CLK_DISPAUD_DISP,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_BTM_ABOX_ACLK, "gout_dispaud_btm_abox_aclk", "mout_clk_aud_bus",
	     CLK_CON_GAT_GOUT_DISPAUD_BTM_ABOX_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_BTM_ABOX_PCLK, "gout_dispaud_btm_abox_pclk",
	     "dout_clk_dispaud_busp", CLK_CON_GAT_GOUT_DISPAUD_BTM_ABOX_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_BTM_DPU_ACLK, "gout_dispaud_btm_dpu_aclk",
	     "mout_cmu_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_BTM_DPU_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_BTM_DPU_PCLK, "gout_dispaud_btm_dpu_pclk",
	     "dout_clk_dispaud_busp", CLK_CON_GAT_GOUT_DISPAUD_BTM_DPU_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_DFTMUX_AUD_CODEC_MCLK, "gout_dispaud_dftmux_aud_codec_mclk",
	     "dout_clk_aud_audif", CLK_CON_GAT_GOUT_DISPAUD_DFTMUX_AUD_CODEC_MCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_DPU_ACLK_DECON, "gout_dispaud_dpu_aclk_decon",
	     "mout_pll_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DECON,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_DPU_ACLK_DMA, "gout_dispaud_dpu_aclk_dma",
	     "mout_pll_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DMA,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_DPU_ACLK_DPP, "gout_dispaud_dpu_aclk_dpp",
	     "mout_pll_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_DPU_ACLK_DPP,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_GPIO_DISPAUD_PCLK, "gout_dispaud_gpio_dispaud_pclk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_GPIO_DISPAUD_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_LHM_AXI_P_DISPAUD_CLK, "gout_dispaud_lhm_axi_p_dispaud_clk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_LHM_AXI_P_DISPAUD_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_LHS_ACEL_D_DPU_CLK, "gout_dispaud_lhs_acel_d_dpu_clk",
	     "mout_cmu_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_LHS_ACEL_D_DPU_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_LHS_AXI_D_ABOX_CLK, "gout_dispaud_lhs_axi_d_abox_clk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_LHS_AXI_D_ABOX_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_PERI_AXI_ASB_ACLKM, "gout_dispaud_peri_axi_asb_aclkm",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_PERI_AXI_ASB_ACLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_PERI_AXI_ASB_PCLK, "gout_dispaud_peri_axi_asb_pclk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_PERI_AXI_ASB_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_PPMU_ABOX_ACLK, "gout_dispaud_ppmu_abox_aclk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_PPMU_ABOX_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_PPMU_ABOX_PCLK, "gout_dispaud_ppmu_abox_pclk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_PPMU_ABOX_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_PPMU_DPU_ACLK, "gout_dispaud_ppmu_dpu_aclk",
	     "mout_cmu_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_PPMU_DPU_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_PPMU_DPU_PCLK, "gout_dispaud_ppmu_dpu_pclk",
	     "dout_clk_dispaud_busp", CLK_CON_GAT_GOUT_DISPAUD_PPMU_DPU_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_AUD_CPU_ACLK_CLK,
	     "gout_dispaud_clk_aud_cpu_aclk_clk", "dout_clk_aud_cpu_aclk",
	     CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_ACLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_AUD_CPU_CLKIN_CLK,
	     "gout_dispaud_clk_aud_cpu_clkin_clk", "mout_clk_aud_cpu_hch",
	     CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_CLKIN_CLK, 21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_AUD_CPU_PCLKDBG_CLK,
	     "gout_dispaud_clk_aud_cpu_pclkdbg_clk", "dout_clk_aud_cpu_pclkdbg",
	     CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CPU_PCLKDBG_CLK, 21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_AUD_DSIF_CLK, "gout_dispaud_clk_aud_dsif_clk",
	     "dout_clk_aud_dsif", CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_DSIF_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_AUD_CLK, "gout_dispaud_clk_aud_clk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_CLK_AUD_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_BUSP_CLK, "gout_dispaud_clk_busp_clk",
	     "dout_clk_dispaud_busp", CLK_CON_GAT_GOUT_DISPAUD_CLK_BUSP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_CLK_DISP_CLK, "gout_dispaud_clk_disp_clk",
	     "mout_cmu_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_CLK_DISP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_SMMU_ABOX_CLK, "gout_dispaud_smmu_abox_clk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_SMMU_ABOX_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_SMMU_DPU_CLK, "gout_dispaud_smmu_dpu_clk",
	     "mout_cmu_dispaud_disp_user", CLK_CON_GAT_GOUT_DISPAUD_SMMU_DPU_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_SYSREG_PCLK, "gout_dispaud_sysreg_pclk",
	     "dout_clk_dispaud_busp", CLK_CON_GAT_GOUT_DISPAUD_SYSREG_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_DISPAUD_WDT_AUD_PCLK, "gout_dispaud_wdt_aud_pclk",
	     "mout_clk_aud_bus", CLK_CON_GAT_GOUT_DISPAUD_WDT_AUD_PCLK,
	     21, 0, 0),
};

static const struct samsung_cmu_info dispaud_cmu_info __initconst = {
	.pll_clks		= dispaud_pll_clks,
	.nr_pll_clks		= ARRAY_SIZE(dispaud_pll_clks),
	.mux_clks		= dispaud_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(dispaud_mux_clks),
	.div_clks		= dispaud_div_clks,
	.nr_div_clks		= ARRAY_SIZE(dispaud_div_clks),
	.gate_clks		= dispaud_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(dispaud_gate_clks),
	.nr_clk_ids		= CLKS_NR_DISPAUD,
	.clk_regs		= dispaud_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(dispaud_clk_regs),
	.sysreg_clk_regs	= drcg_cam_memclk_sysreg,
	.nr_sysreg_clk_regs	= ARRAY_SIZE(drcg_cam_memclk_sysreg),
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.drcg_offset		= EXYNOS9610_DRCG_EN_OFFSET,
	.memclk_offset		= EXYNOS9610_MEMCLK_OFFSET,
	.option_offset		= CMU_CMU_DISPAUD_CONTROLLER_OPTION,
};

/* CMU_FSYS */
#define PLL_CON0_MUX_CLKCMU_FSYS_BUS_USER		0x0100
#define PLL_CON2_MUX_CLKCMU_FSYS_BUS_USER		0x0108
#define PLL_CON0_MUX_CLKCMU_FSYS_MMC_CARD_USER		0x0120
#define PLL_CON2_MUX_CLKCMU_FSYS_MMC_CARD_USER		0x0128
#define PLL_CON0_MUX_CLKCMU_FSYS_MMC_EMBD_USER		0x0140
#define PLL_CON2_MUX_CLKCMU_FSYS_MMC_EMBD_USER		0x0148
#define PLL_CON0_MUX_CLKCMU_FSYS_UFS_EMBD_USER		0x0160
#define PLL_CON2_MUX_CLKCMU_FSYS_UFS_EMBD_USER		0x0168
#define CMU_CMU_FSYS_CONTROLLER_OPTION			0x0800
#define CLK_CON_GAT_CLK_FSYS_CMU_PCLK			0x2000
#define CLK_CON_GAT_CLK_FSYS_OSCCLK_CLK			0x2004
#define CLK_CON_GAT_GOUT_FSYS_ADM_AHB_SSS_HCLKM		0x2008
#define CLK_CON_GAT_GOUT_FSYS_BTM_ACLK			0x200c
#define CLK_CON_GAT_GOUT_FSYS_BTM_PCLK			0x2010
#define CLK_CON_GAT_GOUT_FSYS_GPIO_PCLK			0x2014
#define CLK_CON_GAT_GOUT_FSYS_LHM_AXI_P_CLK		0x2018
#define CLK_CON_GAT_GOUT_FSYS_LHS_ACEL_D_CLK		0x201c
#define CLK_CON_GAT_GOUT_FSYS_MMC_CARD_ACLK		0x2020
#define CLK_CON_GAT_GOUT_FSYS_MMC_CARD_SDCLKIN		0x2024
#define CLK_CON_GAT_GOUT_FSYS_MMC_EMBD_ACLK		0x2028
#define CLK_CON_GAT_GOUT_FSYS_MMC_EMBD_SDCLKIN		0x202c
#define CLK_CON_GAT_GOUT_FSYS_PGEN_LITE_CLK		0x2030
#define CLK_CON_GAT_GOUT_FSYS_PPMU_ACLK			0x2034
#define CLK_CON_GAT_GOUT_FSYS_PPMU_PCLK			0x2038
#define CLK_CON_GAT_GOUT_FSYS_BUS_CLK			0x203c
#define CLK_CON_GAT_GOUT_FSYS_RTIC_ACLK			0x2040
#define CLK_CON_GAT_GOUT_FSYS_RTIC_PCLK			0x2044
#define CLK_CON_GAT_GOUT_FSYS_SSS_ACLK			0x2048
#define CLK_CON_GAT_GOUT_FSYS_SSS_PCLK			0x204c
#define CLK_CON_GAT_GOUT_FSYS_SYSREG_PCLK		0x2050
#define CLK_CON_GAT_GOUT_FSYS_UFS_EMBD_ACLK		0x2054
#define CLK_CON_GAT_GOUT_FSYS_UFS_EMBD_CLK_UNIPRO	0x2058
#define CLK_CON_GAT_GOUT_FSYS_UFS_EMBD_FMP_CLK		0x205c
#define CLK_CON_GAT_GOUT_FSYS_XIU_D_ACLK		0x2060

static const unsigned long fsys_clk_regs[] __initconst = {
	PLL_CON0_MUX_CLKCMU_FSYS_BUS_USER,
	PLL_CON2_MUX_CLKCMU_FSYS_BUS_USER,
	PLL_CON0_MUX_CLKCMU_FSYS_MMC_CARD_USER,
	PLL_CON2_MUX_CLKCMU_FSYS_MMC_CARD_USER,
	PLL_CON0_MUX_CLKCMU_FSYS_MMC_EMBD_USER,
	PLL_CON2_MUX_CLKCMU_FSYS_MMC_EMBD_USER,
	PLL_CON0_MUX_CLKCMU_FSYS_UFS_EMBD_USER,
	PLL_CON2_MUX_CLKCMU_FSYS_UFS_EMBD_USER,
	CMU_CMU_FSYS_CONTROLLER_OPTION,
	CLK_CON_GAT_CLK_FSYS_CMU_PCLK,
	CLK_CON_GAT_CLK_FSYS_OSCCLK_CLK,
	CLK_CON_GAT_GOUT_FSYS_ADM_AHB_SSS_HCLKM,
	CLK_CON_GAT_GOUT_FSYS_BTM_ACLK,
	CLK_CON_GAT_GOUT_FSYS_BTM_PCLK,
	CLK_CON_GAT_GOUT_FSYS_GPIO_PCLK,
	CLK_CON_GAT_GOUT_FSYS_LHM_AXI_P_CLK,
	CLK_CON_GAT_GOUT_FSYS_LHS_ACEL_D_CLK,
	CLK_CON_GAT_GOUT_FSYS_MMC_CARD_ACLK,
	CLK_CON_GAT_GOUT_FSYS_MMC_EMBD_ACLK,
	CLK_CON_GAT_GOUT_FSYS_PGEN_LITE_CLK,
	CLK_CON_GAT_GOUT_FSYS_PPMU_ACLK,
	CLK_CON_GAT_GOUT_FSYS_PPMU_PCLK,
	CLK_CON_GAT_GOUT_FSYS_BUS_CLK,
	CLK_CON_GAT_GOUT_FSYS_SYSREG_PCLK,
	CLK_CON_GAT_GOUT_FSYS_UFS_EMBD_ACLK,
	CLK_CON_GAT_GOUT_FSYS_UFS_EMBD_FMP_CLK,
	CLK_CON_GAT_GOUT_FSYS_XIU_D_ACLK,
};

PNAME(mout_pll_fsys_bus_user_p)		= { "oscclk",
					    "dout_cmu_fsys_bus" };
PNAME(mout_pll_fsys_mmc_card_user_p)	= { "oscclk",
					    "dout_cmu_fsys_mmc_card" };
PNAME(mout_pll_fsys_mmc_embd_user_p)	= { "oscclk",
					    "dout_cmu_fsys_mmc_embd" };
PNAME(mout_pll_fsys_ufs_embd_user_p)	= { "oscclk",
					    "dout_cmu_fsys_ufs_embd" };

static const struct samsung_mux_clock fsys_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_FSYS_BUS_USER, "mout_pll_fsys_bus_user", mout_pll_fsys_bus_user_p,
	    PLL_CON0_MUX_CLKCMU_FSYS_BUS_USER, 4, 1),
	MUX(CLK_MOUT_PLL_FSYS_MMC_CARD_USER, "mout_pll_fsys_mmc_card_user",
	    mout_pll_fsys_mmc_card_user_p, PLL_CON0_MUX_CLKCMU_FSYS_MMC_CARD_USER, 4, 1),
	MUX(CLK_MOUT_PLL_FSYS_MMC_EMBD_USER, "mout_pll_fsys_mmc_embd_user",
	    mout_pll_fsys_mmc_embd_user_p, PLL_CON0_MUX_CLKCMU_FSYS_MMC_EMBD_USER, 4, 1),
	MUX(CLK_MOUT_PLL_FSYS_UFS_EMBD_USER, "mout_pll_fsys_ufs_embd_user",
	    mout_pll_fsys_ufs_embd_user_p, PLL_CON0_MUX_CLKCMU_FSYS_UFS_EMBD_USER, 4, 1),
};

static const struct samsung_gate_clock fsys_gate_clks[] __initconst = {
	GATE(CLK_GOUT_FSYS_CMU_PCLK, "gout_fsys_cmu_pclk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_CLK_FSYS_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_OSCCLK_CLK, "gout_fsys_oscclk_clk", "oscclk",
	     CLK_CON_GAT_CLK_FSYS_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_ADM_AHB_SSS_HCLKM, "gout_fsys_adm_ahb_sss_hclkm",
	     "mout_pll_fsys_bus_user", CLK_CON_GAT_GOUT_FSYS_ADM_AHB_SSS_HCLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_BTM_ACLK, "gout_fsys_btm_aclk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_GOUT_FSYS_BTM_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_BTM_PCLK, "gout_fsys_btm_pclk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_GOUT_FSYS_BTM_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_GPIO_PCLK, "gout_fsys_gpio_pclk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_GOUT_FSYS_GPIO_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_LHM_AXI_P_CLK, "gout_fsys_lhm_axi_p_clk",
	     "mout_pll_fsys_bus_user", CLK_CON_GAT_GOUT_FSYS_LHM_AXI_P_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_LHS_ACEL_D_CLK, "gout_fsys_lhs_acel_d_clk",
	     "mout_pll_fsys_bus_user", CLK_CON_GAT_GOUT_FSYS_LHS_ACEL_D_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_MMC_CARD_ACLK, "gout_fsys_mmc_card_aclk",
	     "mout_pll_fsys_bus_user", CLK_CON_GAT_GOUT_FSYS_MMC_CARD_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_MMC_CARD_SDCLKIN, "gout_fsys_mmc_card_sdclkin",
	     "mout_pll_fsys_mmc_card_user", CLK_CON_GAT_GOUT_FSYS_MMC_CARD_SDCLKIN,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_MMC_EMBD_ACLK, "gout_fsys_mmc_embd_aclk",
	     "mout_pll_fsys_bus_user", CLK_CON_GAT_GOUT_FSYS_MMC_EMBD_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_MMC_EMBD_SDCLKIN, "gout_fsys_mmc_embd_sdclkin",
	     "mout_pll_fsys_mmc_embd_user", CLK_CON_GAT_GOUT_FSYS_MMC_EMBD_SDCLKIN,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_PGEN_LITE_CLK, "gout_fsys_pgen_lite_clk",
	     "mout_pll_fsys_bus_user", CLK_CON_GAT_GOUT_FSYS_PGEN_LITE_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_PPMU_ACLK, "gout_fsys_ppmu_aclk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_GOUT_FSYS_PPMU_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_PPMU_PCLK, "gout_fsys_ppmu_pclk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_GOUT_FSYS_PPMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_BUS_CLK, "gout_fsys_bus_clk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_GOUT_FSYS_BUS_CLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_SYSREG_PCLK, "gout_fsys_sysreg_pclk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_GOUT_FSYS_SYSREG_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_FSYS_UFS_EMBD_ACLK, "gout_fsys_ufs_embd_aclk",
	     "mout_pll_fsys_bus_user", CLK_CON_GAT_GOUT_FSYS_UFS_EMBD_ACLK,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_UFS_EMBD_CLK_UNIPRO, "gout_fsys_ufs_embd_clk_unipro",
	     "mout_pll_fsys_ufs_embd_user", CLK_CON_GAT_GOUT_FSYS_UFS_EMBD_CLK_UNIPRO,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_UFS_EMBD_FMP_CLK, "gout_fsys_ufs_embd_fmp_clk",
	     "mout_pll_fsys_bus_user", CLK_CON_GAT_GOUT_FSYS_UFS_EMBD_FMP_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_FSYS_XIU_D_ACLK, "gout_fsys_xiu_d_aclk", "mout_pll_fsys_bus_user",
	     CLK_CON_GAT_GOUT_FSYS_XIU_D_ACLK, 21, 0, 0),
};

static const struct samsung_cmu_info fsys_cmu_info __initconst = {
	.mux_clks		= fsys_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(fsys_mux_clks),
	.gate_clks		= fsys_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(fsys_gate_clks),
	.clk_regs		= fsys_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(fsys_clk_regs),
	.nr_clk_ids		= CLKS_NR_FSYS,
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_FSYS_CONTROLLER_OPTION,
};

/* CMU_G2D */
#define PLL_CON0_MUX_CLKCMU_G2D_G2D_USER	0x0100
#define PLL_CON2_MUX_CLKCMU_G2D_G2D_USER	0x0108
#define PLL_CON0_MUX_CLKCMU_G2D_MSCL_USER	0x0120
#define PLL_CON2_MUX_CLKCMU_G2D_MSCL_USER	0x0128
#define CMU_CMU_G2D_CONTROLLER_OPTION		0x0800
#define CLK_CON_DIV_DIV_CLK_G2D_BUSP		0x1800
#define CLK_CON_GAT_CLK_G2D_CMU_PCLK		0x2000
#define CLK_CON_GAT_CLK_G2D_OSCCLK_CLK		0x2004
#define CLK_CON_GAT_GOUT_G2D_AS_AXI_JPEG_ACLKM	0x2008
#define CLK_CON_GAT_GOUT_G2D_AS_AXI_JPEG_ACLKS	0x200c
#define CLK_CON_GAT_GOUT_G2D_AS_AXI_MSCL_ACLKM	0x2010
#define CLK_CON_GAT_GOUT_G2D_AS_AXI_MSCL_ACLKS	0x2014
#define CLK_CON_GAT_GOUT_G2D_CLK_G2D_G2D	0x2018
#define CLK_CON_GAT_GOUT_G2D_CLK_G2D_MSCL	0x201c
#define CLK_CON_GAT_GOUT_G2D_BTM_ACLK		0x2020
#define CLK_CON_GAT_GOUT_G2D_BTM_PCLK		0x2024
#define CLK_CON_GAT_GOUT_G2D_G2D_ACLK		0x2028
#define CLK_CON_GAT_GOUT_G2D_JPEG_FIMP_CLK	0x202c
#define CLK_CON_GAT_GOUT_G2D_LHM_AXI_P_CLK	0x2030
#define CLK_CON_GAT_GOUT_G2D_LHS_ACEL_D_CLK	0x2034
#define CLK_CON_GAT_GOUT_G2D_MSCL_ACLK		0x2038
#define CLK_CON_GAT_GOUT_G2D_PGEN100_LITE_CLK	0x203c
#define CLK_CON_GAT_GOUT_G2D_PPMU_ACLK		0x2040
#define CLK_CON_GAT_GOUT_G2D_PPMU_PCLK		0x2044
#define CLK_CON_GAT_GOUT_G2D_BUSP_CLK		0x2048
#define CLK_CON_GAT_GOUT_G2D_G2D_CLK		0x204c
#define CLK_CON_GAT_GOUT_G2D_MSCL_CLK		0x2050
#define CLK_CON_GAT_GOUT_G2D_SYSMMU_CLK		0x2054
#define CLK_CON_GAT_GOUT_G2D_SYSREG_PCLK	0x2058
#define CLK_CON_GAT_GOUT_G2D_XIU_D_MSCL_ACLK	0x205c

static const unsigned long g2d_clk_regs[] __initconst = {
	PLL_CON0_MUX_CLKCMU_G2D_G2D_USER,
	PLL_CON2_MUX_CLKCMU_G2D_G2D_USER,
	PLL_CON0_MUX_CLKCMU_G2D_MSCL_USER,
	PLL_CON2_MUX_CLKCMU_G2D_MSCL_USER,
	CMU_CMU_G2D_CONTROLLER_OPTION,
	CLK_CON_DIV_DIV_CLK_G2D_BUSP,
	CLK_CON_GAT_CLK_G2D_CMU_PCLK,
	CLK_CON_GAT_CLK_G2D_OSCCLK_CLK,
	CLK_CON_GAT_GOUT_G2D_AS_AXI_JPEG_ACLKM,
	CLK_CON_GAT_GOUT_G2D_AS_AXI_JPEG_ACLKS,
	CLK_CON_GAT_GOUT_G2D_AS_AXI_MSCL_ACLKM,
	CLK_CON_GAT_GOUT_G2D_AS_AXI_MSCL_ACLKS,
	CLK_CON_GAT_GOUT_G2D_CLK_G2D_G2D,
	CLK_CON_GAT_GOUT_G2D_CLK_G2D_MSCL,
	CLK_CON_GAT_GOUT_G2D_BTM_ACLK,
	CLK_CON_GAT_GOUT_G2D_BTM_PCLK,
	CLK_CON_GAT_GOUT_G2D_G2D_ACLK,
	CLK_CON_GAT_GOUT_G2D_JPEG_FIMP_CLK,
	CLK_CON_GAT_GOUT_G2D_LHM_AXI_P_CLK,
	CLK_CON_GAT_GOUT_G2D_LHS_ACEL_D_CLK,
	CLK_CON_GAT_GOUT_G2D_MSCL_ACLK,
	CLK_CON_GAT_GOUT_G2D_PGEN100_LITE_CLK,
	CLK_CON_GAT_GOUT_G2D_PPMU_ACLK,
	CLK_CON_GAT_GOUT_G2D_PPMU_PCLK,
	CLK_CON_GAT_GOUT_G2D_BUSP_CLK,
	CLK_CON_GAT_GOUT_G2D_G2D_CLK,
	CLK_CON_GAT_GOUT_G2D_MSCL_CLK,
	CLK_CON_GAT_GOUT_G2D_SYSMMU_CLK,
	CLK_CON_GAT_GOUT_G2D_SYSREG_PCLK,
	CLK_CON_GAT_GOUT_G2D_XIU_D_MSCL_ACLK,
};

PNAME(mout_pll_g2d_g2d_user_p) = { "oscclk", "dout_cmu_g2d_g2d" };
PNAME(mout_pll_g2d_mscl_user_p) = { "oscclk", "dout_cmu_g2d_mscl" };

static const struct samsung_mux_clock g2d_mux_clks[] __initconst = {
	MUX(CLK_MOUT_PLL_G2D_G2D_USER, "mout_pll_g2d_g2d_user", mout_pll_g2d_g2d_user_p,
	    PLL_CON0_MUX_CLKCMU_G2D_G2D_USER, 4, 1),
	MUX(CLK_MOUT_PLL_G2D_MSCL_USER, "mout_pll_g2d_mscl_user", mout_pll_g2d_mscl_user_p,
	    PLL_CON0_MUX_CLKCMU_G2D_MSCL_USER, 4, 1),
};

static const struct samsung_div_clock g2d_div_clks[] __initconst = {
	DIV(CLK_DOUT_CLK_G2D_BUSP, "dout_clk_g2d_busp", "mout_pll_g2d_mscl_user",
	    CLK_CON_DIV_DIV_CLK_G2D_BUSP, 0, 3),
};

static const struct samsung_gate_clock g2d_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CLK_G2D_CMU_PCLK, "gout_clk_g2d_cmu_pclk", "dout_clk_g2d_busp",
	     CLK_CON_GAT_CLK_G2D_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_G2D_OSCCLK_CLK, "gout_clk_g2d_oscclk_clk", "oscclk",
	     CLK_CON_GAT_CLK_G2D_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_AS_AXI_JPEG_ACLKM, "gout_g2d_as_axi_jpeg_aclkm",
	     "mout_pll_g2d_g2d_user", CLK_CON_GAT_GOUT_G2D_AS_AXI_JPEG_ACLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_G2D_AS_AXI_JPEG_ACLKS, "gout_g2d_as_axi_jpeg_aclks",
	     "mout_pll_g2d_mscl_user", CLK_CON_GAT_GOUT_G2D_AS_AXI_JPEG_ACLKS,
	     21, 0, 0),
	GATE(CLK_GOUT_G2D_AS_AXI_MSCL_ACLKM, "gout_g2d_as_axi_mscl_aclkm",
	     "mout_pll_g2d_g2d_user", CLK_CON_GAT_GOUT_G2D_AS_AXI_MSCL_ACLKM,
	     21, 0, 0),
	GATE(CLK_GOUT_G2D_AS_AXI_MSCL_ACLKS, "gout_g2d_as_axi_mscl_aclks",
	     "mout_pll_g2d_mscl_user", CLK_CON_GAT_GOUT_G2D_AS_AXI_MSCL_ACLKS,
	     21, 0, 0),
	GATE(CLK_GOUT_G2D_CLK_G2D_G2D, "gout_g2d_clk_g2d_g2d", "mout_pll_g2d_g2d_user",
	     CLK_CON_GAT_GOUT_G2D_CLK_G2D_G2D, 21, 0, 0),
	GATE(CLK_GOUT_G2D_CLK_G2D_MSCL, "gout_g2d_clk_g2d_mscl", "mout_pll_g2d_mscl_user",
	     CLK_CON_GAT_GOUT_G2D_CLK_G2D_MSCL, 21, 0, 0),
	GATE(CLK_GOUT_G2D_BTM_ACLK, "gout_g2d_btm_aclk", "mout_pll_g2d_g2d_user",
	     CLK_CON_GAT_GOUT_G2D_BTM_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_BTM_PCLK, "gout_g2d_btm_pclk", "dout_clk_g2d_busp",
	     CLK_CON_GAT_GOUT_G2D_BTM_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_G2D_ACLK, "gout_g2d_g2d_aclk", "mout_pll_g2d_g2d_user",
	     CLK_CON_GAT_GOUT_G2D_G2D_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_JPEG_FIMP_CLK, "gout_g2d_jpeg_fimp_clk", "mout_pll_g2d_mscl_user",
	     CLK_CON_GAT_GOUT_G2D_JPEG_FIMP_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_LHM_AXI_P_CLK, "gout_g2d_lhm_axi_p_clk", "dout_clk_g2d_busp",
	     CLK_CON_GAT_GOUT_G2D_LHM_AXI_P_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_LHS_ACEL_D_CLK, "gout_g2d_lhs_acel_d_clk", "mout_pll_g2d_mscl_user",
	     CLK_CON_GAT_GOUT_G2D_LHS_ACEL_D_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_MSCL_ACLK, "gout_g2d_mscl_aclk", "mout_pll_g2d_mscl_user",
	     CLK_CON_GAT_GOUT_G2D_MSCL_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_PGEN100_LITE_CLK, "gout_g2d_pgen100_lite_clk", "dout_clk_g2d_busp",
	     CLK_CON_GAT_GOUT_G2D_PGEN100_LITE_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_PPMU_ACLK, "gout_g2d_ppmu_aclk", "mout_pll_g2d_g2d_user",
	     CLK_CON_GAT_GOUT_G2D_PPMU_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_PPMU_PCLK, "gout_g2d_ppmu_pclk", "dout_clk_g2d_busp",
	     CLK_CON_GAT_GOUT_G2D_PPMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_BUSP_CLK, "gout_g2d_busp_clk", "dout_clk_g2d_busp",
	     CLK_CON_GAT_GOUT_G2D_BUSP_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_SYSMMU_CLK, "gout_g2d_sysmmu_clk", "mout_pll_g2d_g2d_user",
	     CLK_CON_GAT_GOUT_G2D_SYSMMU_CLK, 21, CLK_IGNORE_UNUSED, 0),
	GATE(CLK_GOUT_G2D_SYSREG_PCLK, "gout_g2d_sysreg_pclk", "dout_clk_g2d_busp",
	     CLK_CON_GAT_GOUT_G2D_SYSREG_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_G2D_XIU_D_MSCL_ACLK, "gout_g2d_xiu_d_mscl_aclk", "mout_pll_g2d_g2d_user",
	     CLK_CON_GAT_GOUT_G2D_XIU_D_MSCL_ACLK, 21, 0, 0),
};

static const struct samsung_cmu_info g2d_cmu_info __initconst = {
	.mux_clks		= g2d_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(g2d_mux_clks),
	.div_clks		= g2d_div_clks,
	.nr_div_clks		= ARRAY_SIZE(g2d_div_clks),
	.gate_clks		= g2d_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(g2d_gate_clks),
	.clk_regs		= g2d_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(g2d_clk_regs),
	.nr_clk_ids		= CLKS_NR_G2D,
	.sysreg_clk_regs	= drcg_memclk_sysreg,
	.nr_sysreg_clk_regs	= ARRAY_SIZE(drcg_memclk_sysreg),
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.drcg_offset		= EXYNOS9610_DRCG_EN_OFFSET,
	.memclk_offset		= EXYNOS9610_MEMCLK_OFFSET,
	.option_offset		= CMU_CMU_G2D_CONTROLLER_OPTION,
};

/* CMU_G3D */
#define PLL_LOCKTIME_PLL_G3D			0x0000
#define PLL_CON0_MUX_CLKCMU_G3D_SWITCH_USER	0x0100
#define PLL_CON2_MUX_CLKCMU_G3D_SWITCH_USER	0x0108
#define PLL_CON0_PLL_G3D			0x0120
#define CMU_CMU_G3D_CONTROLLER_OPTION		0x0800
#define CLK_CON_MUX_MUX_CLK_G3D_BUSD		0x1004
#define CLK_CON_DIV_DIV_CLK_G3D_BUSD		0x1800
#define CLK_CON_DIV_DIV_CLK_G3D_BUSP		0x1804
#define CLK_CON_GAT_CLK_G3D_CMU_PCLK		0x2000
#define CLK_CON_GAT_CLK_G3D_G3D_CLK		0x2004
#define CLK_CON_GAT_CLK_G3D_HPM_TARGETCLK_C	0x2008
#define CLK_CON_GAT_CLK_G3D_OSCCLK_CLK		0x200c
#define CLK_CON_GAT_GOUT_G3D_BTM_ACLK		0x2014
#define CLK_CON_GAT_GOUT_G3D_BTM_PCLK		0x2018
#define CLK_CON_GAT_GOUT_G3D_BUSIF_HPMG3D_PCLK	0x201c
#define CLK_CON_GAT_GOUT_G3D_GRAY2BIN_CLK	0x2020
#define CLK_CON_GAT_GOUT_G3D_LHM_AXI_G3DSFR_CLK	0x2024
#define CLK_CON_GAT_GOUT_G3D_LHM_AXI_P_CLK	0x2028
#define CLK_CON_GAT_GOUT_G3D_LHS_AXI_D_CLK	0x202c
#define CLK_CON_GAT_GOUT_G3D_LHS_AXI_G3DSFR_CLK	0x2030
#define CLK_CON_GAT_GOUT_G3D_PGEN_LITE_CLK	0x2034
#define CLK_CON_GAT_GOUT_G3D_BUSD_CLK		0x2038
#define CLK_CON_GAT_GOUT_G3D_BUSP_CLK		0x203c
#define CLK_CON_GAT_GOUT_G3D_SYSREG_PCLK	0x2040

static const unsigned long g3d_clk_regs[] __initconst = {
	PLL_LOCKTIME_PLL_G3D,
	PLL_CON0_MUX_CLKCMU_G3D_SWITCH_USER,
	PLL_CON2_MUX_CLKCMU_G3D_SWITCH_USER,
	PLL_CON0_PLL_G3D,
	CMU_CMU_G3D_CONTROLLER_OPTION,
	CLK_CON_MUX_MUX_CLK_G3D_BUSD,
	CLK_CON_DIV_DIV_CLK_G3D_BUSD,
	CLK_CON_DIV_DIV_CLK_G3D_BUSP,
	CLK_CON_GAT_CLK_G3D_CMU_PCLK,
	CLK_CON_GAT_CLK_G3D_G3D_CLK,
	CLK_CON_GAT_CLK_G3D_HPM_TARGETCLK_C,
	CLK_CON_GAT_CLK_G3D_OSCCLK_CLK,
	CLK_CON_GAT_GOUT_G3D_BTM_ACLK,
	CLK_CON_GAT_GOUT_G3D_BTM_PCLK,
	CLK_CON_GAT_GOUT_G3D_BUSIF_HPMG3D_PCLK,
	CLK_CON_GAT_GOUT_G3D_GRAY2BIN_CLK,
	CLK_CON_GAT_GOUT_G3D_LHM_AXI_G3DSFR_CLK,
	CLK_CON_GAT_GOUT_G3D_LHM_AXI_P_CLK,
	CLK_CON_GAT_GOUT_G3D_LHS_AXI_D_CLK,
	CLK_CON_GAT_GOUT_G3D_LHS_AXI_G3DSFR_CLK,
	CLK_CON_GAT_GOUT_G3D_PGEN_LITE_CLK,
	CLK_CON_GAT_GOUT_G3D_BUSD_CLK,
	CLK_CON_GAT_GOUT_G3D_BUSP_CLK,
	CLK_CON_GAT_GOUT_G3D_SYSREG_PCLK,
};

static const struct samsung_pll_rate_table pll_g3d_rate_table[] __initconst = {
	PLL_35XX_RATE(26 * MHZ, 750000000, 375, 13, 0),
	PLL_35XX_RATE(26 * MHZ, 1000000000, 500, 13, 0),
	PLL_35XX_RATE(26 * MHZ, 1200000000, 600, 13, 0),
	PLL_35XX_RATE(26 * MHZ, 330000000, 660, 13, 2),
	PLL_35XX_RATE(26 * MHZ, 550000000, 550, 13, 1),
};

static const struct samsung_pll_clock g3d_pll_clks[] __initconst = {
	PLL(pll_1052x, CLK_FOUT_G3D_PLL, "fout_g3d_pll", "oscclk",
	    PLL_LOCKTIME_PLL_G3D, PLL_CON0_PLL_G3D,
	    pll_g3d_rate_table),
};

PNAME(mout_g3d_switch_user_p)	= { "oscclk", "dout_cmu_g3d_switch" };
PNAME(mout_clk_g3d_busd_p)	= { "fout_g3d_pll",
				    "mout_g3d_switch_user" };

static const struct samsung_mux_clock g3d_mux_clks[] __initconst = {
	MUX(CLK_MOUT_G3D_SWITCH_USER, "mout_g3d_switch_user", mout_g3d_switch_user_p,
	    PLL_CON0_MUX_CLKCMU_G3D_SWITCH_USER, 4, 1),
	MUX(CLK_MOUT_CLK_G3D_BUSD, "mout_clk_g3d_busd", mout_clk_g3d_busd_p,
	    CLK_CON_MUX_MUX_CLK_G3D_BUSD, 0, 1),
};

static const struct samsung_div_clock g3d_div_clks[] __initconst = {
	DIV(CLK_DOUT_CLK_G3D_BUSD, "dout_clk_g3d_busd", "mout_clk_g3d_busd",
	    CLK_CON_DIV_DIV_CLK_G3D_BUSD, 0, 1),
	DIV(CLK_DOUT_CLK_G3D_BUSP, "dout_clk_g3d_busp", "mout_clk_g3d_busd",
	    CLK_CON_DIV_DIV_CLK_G3D_BUSP, 0, 3),
};

static const struct samsung_gate_clock g3d_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CLK_G3D_CMU_PCLK, "gout_clk_g3d_cmu_pclk", "dout_clk_g3d_busp",
	     CLK_CON_GAT_CLK_G3D_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_G3D_G3D_CLK, "gout_clk_g3d_g3d_clk", "dout_clk_g3d_busd",
	     CLK_CON_GAT_CLK_G3D_G3D_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_G3D_HPM_TARGETCLK_C, "gout_clk_g3d_hpm_targetclk_c",
	     "dout_cmu_hpm", CLK_CON_GAT_CLK_G3D_HPM_TARGETCLK_C, 21, 0, 0),
	GATE(CLK_GOUT_CLK_G3D_OSCCLK_CLK, "gout_clk_g3d_oscclk_clk", "oscclk",
	     CLK_CON_GAT_CLK_G3D_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G3D_BTM_ACLK, "gout_g3d_btm_aclk", "dout_clk_g3d_busd",
	     CLK_CON_GAT_GOUT_G3D_BTM_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_G3D_BTM_PCLK, "gout_g3d_btm_pclk", "dout_clk_g3d_busp",
	     CLK_CON_GAT_GOUT_G3D_BTM_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_G3D_BUSIF_HPMG3D_PCLK, "gout_g3d_busif_hpmg3d_pclk",
	     "dout_clk_g3d_busp", CLK_CON_GAT_GOUT_G3D_BUSIF_HPMG3D_PCLK,
	     21, 0, 0),
	GATE(CLK_GOUT_G3D_GRAY2BIN_CLK, "gout_g3d_gray2bin_clk",
	     "dout_clk_g3d_busd", CLK_CON_GAT_GOUT_G3D_GRAY2BIN_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_G3D_LHM_AXI_G3DSFR_CLK, "gout_g3d_lhm_axi_g3dsfr_clk",
	     "dout_clk_g3d_busd", CLK_CON_GAT_GOUT_G3D_LHM_AXI_G3DSFR_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_G3D_LHM_AXI_P_CLK, "gout_g3d_lhm_axi_p_clk",
	     "dout_clk_g3d_busp", CLK_CON_GAT_GOUT_G3D_LHM_AXI_P_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_G3D_LHS_AXI_D_CLK, "gout_g3d_lhs_axi_d_clk",
	     "dout_clk_g3d_busd", CLK_CON_GAT_GOUT_G3D_LHS_AXI_D_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_G3D_LHS_AXI_G3DSFR_CLK, "gout_g3d_lhs_axi_g3dsfr_clk",
	     "dout_clk_g3d_busp", CLK_CON_GAT_GOUT_G3D_LHS_AXI_G3DSFR_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_G3D_PGEN_LITE_CLK, "gout_g3d_pgen_lite_clk",
	     "dout_clk_g3d_busp", CLK_CON_GAT_GOUT_G3D_PGEN_LITE_CLK,
	     21, 0, 0),
	GATE(CLK_GOUT_G3D_BUSD_CLK, "gout_g3d_busd_clk", "dout_clk_g3d_busd",
	     CLK_CON_GAT_GOUT_G3D_BUSD_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G3D_BUSP_CLK, "gout_g3d_busp_clk", "dout_clk_g3d_busp",
	     CLK_CON_GAT_GOUT_G3D_BUSP_CLK, 21, 0, 0),
	GATE(CLK_GOUT_G3D_SYSREG_PCLK, "gout_g3d_sysreg_pclk", "dout_clk_g3d_busp",
	     CLK_CON_GAT_GOUT_G3D_SYSREG_PCLK, 21, 0, 0),
};

static const struct samsung_cmu_info g3d_cmu_info __initconst = {
	.pll_clks		= g3d_pll_clks,
	.nr_pll_clks		= ARRAY_SIZE(g3d_pll_clks),
	.mux_clks		= g3d_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(g3d_mux_clks),
	.div_clks		= g3d_div_clks,
	.nr_div_clks		= ARRAY_SIZE(g3d_div_clks),
	.gate_clks		= g3d_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(g3d_gate_clks),
	.clk_regs		= g3d_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(g3d_clk_regs),
	.nr_clk_ids		= CLKS_NR_G3D,
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_G3D_CONTROLLER_OPTION,
};

/* CMU_USB */
#define PLL_CON0_MUX_CLKCMU_USB_BUS_USER		0x0100
#define PLL_CON2_MUX_CLKCMU_USB_BUS_USER		0x0108
#define PLL_CON0_MUX_CLKCMU_USB_DPGTC_USER		0x0120
#define PLL_CON2_MUX_CLKCMU_USB_DPGTC_USER		0x0128
#define PLL_CON0_MUX_CLKCMU_USB_USB30DRD_USER		0x0140
#define PLL_CON2_MUX_CLKCMU_USB_USB30DRD_USER		0x0148
#define CMU_CMU_USB_CONTROLLER_OPTION			0x0800
#define CLK_CON_GAT_CLK_USB_OSCCLK_CLK			0x2000
#define CLK_CON_GAT_CLK_USB_CMU_PCLK			0x2004
#define CLK_CON_GOUT_USB_BTM_ACLK			0x2008
#define CLK_CON_GOUT_USB_BTM_PCLK			0x200c
#define CLK_CON_GOUT_USB_DP_LINK_DPTX_LINK_DP_GTC_CLK	0x2010
#define CLK_CON_GOUT_USB_DP_LINK_DPTX_LINK_PCLK		0x2014
#define CLK_CON_GOUT_USB_LHM_AXI_P_CLK			0x2018
#define CLK_CON_GOUT_USB_LHS_ACEL_D_CLK			0x201c
#define CLK_CON_GOUT_USB_PGEN_LITE_CLK			0x2020
#define CLK_CON_GOUT_USB_PPMU_ACLK			0x2024
#define CLK_CON_GOUT_USB_PPMU_PCLK			0x2028
#define CLK_CON_GOUT_USB_BUS_CLK			0x202c
#define CLK_CON_GOUT_USB_SYSREG_PCLK			0x2030
#define CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_20	0x2034
#define CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_30_0	0x2038
#define CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_30_1	0x203c
#define CLK_CON_GOUT_USB_USB30DRD_BUS_CLK_EARLY		0x2040
#define CLK_CON_GOUT_USB_USB30DRD_REF_CLK		0x2044
#define CLK_CON_GOUT_USB_US_D_ACLK			0x2048

static const unsigned long usb_clk_regs[] __initconst = {
	PLL_CON0_MUX_CLKCMU_USB_BUS_USER,
	PLL_CON2_MUX_CLKCMU_USB_BUS_USER,
	PLL_CON0_MUX_CLKCMU_USB_DPGTC_USER,
	PLL_CON2_MUX_CLKCMU_USB_DPGTC_USER,
	PLL_CON0_MUX_CLKCMU_USB_USB30DRD_USER,
	PLL_CON2_MUX_CLKCMU_USB_USB30DRD_USER,
	CMU_CMU_USB_CONTROLLER_OPTION,
	CLK_CON_GAT_CLK_USB_OSCCLK_CLK,
	CLK_CON_GAT_CLK_USB_CMU_PCLK,
	CLK_CON_GOUT_USB_BTM_ACLK,
	CLK_CON_GOUT_USB_BTM_PCLK,
	CLK_CON_GOUT_USB_DP_LINK_DPTX_LINK_DP_GTC_CLK,
	CLK_CON_GOUT_USB_DP_LINK_DPTX_LINK_PCLK,
	CLK_CON_GOUT_USB_LHM_AXI_P_CLK,
	CLK_CON_GOUT_USB_LHS_ACEL_D_CLK,
	CLK_CON_GOUT_USB_PGEN_LITE_CLK,
	CLK_CON_GOUT_USB_PPMU_ACLK,
	CLK_CON_GOUT_USB_PPMU_PCLK,
	CLK_CON_GOUT_USB_BUS_CLK,
	CLK_CON_GOUT_USB_SYSREG_PCLK,
	CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_20,
	CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_30_0,
	CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_30_1,
	CLK_CON_GOUT_USB_USB30DRD_BUS_CLK_EARLY,
	CLK_CON_GOUT_USB_USB30DRD_REF_CLK,
	CLK_CON_GOUT_USB_US_D_ACLK,
};

PNAME(mout_usb_bus_user_p)	= { "oscclk", "dout_cmu_usb_bus" };
PNAME(mout_usb_dpgtc_user_p)	= { "oscclk", "dout_cmu_usb_dpgtc" };
PNAME(mout_usb_usb30drd_user_p)	= { "oscclk", "dout_cmu_usb_usb30drd" };

static const struct samsung_mux_clock usb_mux_clks[] __initconst = {
	MUX(CLK_MOUT_USB_BUS_USER, "mout_usb_bus_user", mout_usb_bus_user_p,
	    PLL_CON0_MUX_CLKCMU_USB_BUS_USER, 4, 1),
	MUX(CLK_MOUT_USB_DPGTC_USER, "mout_usb_dpgtc_user", mout_usb_dpgtc_user_p,
	    PLL_CON0_MUX_CLKCMU_USB_DPGTC_USER, 4, 1),
	MUX(CLK_MOUT_USB_USB30DRD_USER, "mout_usb_usb30drd_user", mout_usb_usb30drd_user_p,
	    PLL_CON0_MUX_CLKCMU_USB_USB30DRD_USER, 4, 1),
};

static const struct samsung_gate_clock usb_gate_clks[] __initconst = {
	GATE(CLK_GOUT_CLK_USB_OSCCLK_CLK, "gout_clk_usb_oscclk_clk", "oscclk",
	     CLK_CON_GAT_CLK_USB_OSCCLK_CLK, 21, 0, 0),
	GATE(CLK_GOUT_CLK_USB_CMU_PCLK, "gout_clk_usb_cmu_pclk", "mout_usb_bus_user",
	     CLK_CON_GAT_CLK_USB_CMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_BTM_ACLK, "gout_usb_btm_aclk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_BTM_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_BTM_PCLK, "gout_usb_btm_pclk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_BTM_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_DP_LINK_DPTX_LINK_DT_GTC_CLK, "gout_usb_dp_link_dptx_link_dt_gtc_clk",
	     "mout_usb_dpgtc_user", CLK_CON_GOUT_USB_DP_LINK_DPTX_LINK_DP_GTC_CLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_DP_LINK_DPTX_LINK_PCLK, "gout_usb_dp_link_dptx_link_pclk",
	     "mout_usb_bus_user", CLK_CON_GOUT_USB_DP_LINK_DPTX_LINK_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_LHM_AXI_P_CLK, "gout_usb_lhm_axi_p_clk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_LHM_AXI_P_CLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_LHS_ACEL_D_CLK, "gout_usb_lhs_acel_d_clk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_LHS_ACEL_D_CLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_PGEN_LITE_CLK, "gout_usb_pgen_lite_clk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_PGEN_LITE_CLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_PPMU_ACLK, "gout_usb_ppmu_aclk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_PPMU_ACLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_PPMU_PCLK, "gout_usb_ppmu_pclk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_PPMU_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_BUS_CLK, "gout_usb_bus_clk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_BUS_CLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_SYSREG_PCLK, "gout_usb_sysreg_pclk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_SYSREG_PCLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_USB30DRD_ACLK_PHYCTRL_20, "gout_usb_usb30drd_aclk_phyctrl_20",
	     "mout_usb_bus_user", CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_20, 21, 0, 0),
	GATE(CLK_GOUT_USB_USB30DRD_ACLK_PHYCTRL_30_0, "gout_usb_usb30drd_aclk_phyctrl_30_0",
	     "mout_usb_bus_user", CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_30_0, 21, 0, 0),
	GATE(CLK_GOUT_USB_USB30DRD_ACLK_PHYCTRL_30_1, "gout_usb_usb30drd_aclk_phyctrl_30_1",
	     "mout_usb_bus_user", CLK_CON_GOUT_USB_USB30DRD_ACLK_PHYCTRL_30_1, 21, 0, 0),
	GATE(CLK_GOUT_USB_USB30DRD_BUS_CLK_EARLY, "gout_usb_usb30drd_bus_clk_early",
	     "mout_usb_bus_user", CLK_CON_GOUT_USB_USB30DRD_BUS_CLK_EARLY, 21, 0, 0),
	GATE(CLK_GOUT_USB_USB30DRD_REF_CLK, "gout_usb_usb30drd_ref_clk",
	     "mout_usb_usb30drd_user", CLK_CON_GOUT_USB_USB30DRD_REF_CLK, 21, 0, 0),
	GATE(CLK_GOUT_USB_US_D_ACLK, "gout_usb_us_d_aclk", "mout_usb_bus_user",
	     CLK_CON_GOUT_USB_US_D_ACLK, 21, 0, 0),
};

static const struct samsung_cmu_info usb_cmu_info __initconst = {
	.mux_clks		= usb_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(usb_mux_clks),
	.gate_clks		= usb_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(usb_gate_clks),
	.clk_regs		= usb_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(usb_clk_regs),
	.nr_clk_ids		= CLKS_NR_USB,
	.auto_clock_gate	= true,
	.gate_dbg_offset	= EXYNOS9610_GATE_DBG_OFFSET,
	.option_offset		= CMU_CMU_USB_CONTROLLER_OPTION,
};

static int __init exynos9610_cmu_probe(struct platform_device *pdev)
{
	const struct samsung_cmu_info *info;
	struct device *dev = &pdev->dev;

	info = of_device_get_match_data(dev);
	exynos_arm64_register_cmu(dev, dev->of_node, info);

	return 0;
}

static const struct of_device_id exynos9610_cmu_of_match[] = {
	{
		.compatible = "samsung,exynos9610-cmu-cam",
		.data = &cam_cmu_info,
	},
	{
		.compatible = "samsung,exynos9610-cmu-cmgp",
		.data = &cmgp_cmu_info,
	},
	{
		.compatible = "samsung,exynos9610-cmu-core",
		.data = &core_cmu_info,
	},
	{
		.compatible = "samsung,exynos9610-cmu-dispaud",
		.data = &dispaud_cmu_info,
	},
	{
		.compatible = "samsung,exynos9610-cmu-fsys",
		.data = &fsys_cmu_info,
	},
	{
		.compatible = "samsung,exynos9610-cmu-g2d",
		.data = &g2d_cmu_info,
	},
	{
		.compatible = "samsung,exynos9610-cmu-g3d",
		.data = &g3d_cmu_info,
	},
	{
		.compatible = "samsung,exynos9610-cmu-usb",
		.data = &usb_cmu_info,
	},
	{ }
};

static struct platform_driver exynos9610_cmu_driver __refdata = {
	.driver = {
		.name = "exynos9610-cmu",
		.of_match_table = exynos9610_cmu_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = exynos9610_cmu_probe,
};

static int __init exynos9610_cmu_init(void)
{
	return platform_driver_register(&exynos9610_cmu_driver);
}
core_initcall(exynos9610_cmu_init);
