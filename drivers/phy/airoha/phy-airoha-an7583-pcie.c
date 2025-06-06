// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Christian Marangi <ansuelsmth@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "phy-airoha-an7583-pcie-regs.h"

#define FREQ_LOCK_MAX_ATTEMPT	50

/* PCIe-PHY initialization time in ms needed by the hw to complete */
#define PHY_HW_INIT_TIME_MS	30

enum an7583_pcie_port_gen {
	PCIE_PORT_GEN1 = 1,
	PCIE_PORT_GEN2,
	PCIE_PORT_GEN3,
};

/**
 * struct an7583_pcie_phy - PCIe phy driver main structure
 * @dev: pointer to device
 * @phy: pointer to generic phy
 * @g3_ana: IO mapped register base address of G3 Analog
 * @g3_pma: IO mapped register base address of G3 PMA
 * @qp_ana: IO mapped register base address of QPhy Analogc
 * @qp_pma: IO mapped register base address of QPhy PMA
 * @qp_dig: IO mapped register base address of QPhy Diagnostic
 * @xr_dtime: IO mapped register base address of Tx-Rx detection time
 * @rx_aeq: IO mapped register base address of Rx AEQ training
 */
struct an7583_pcie_phy {
	struct device *dev;
	struct phy *phy;

	void __iomem *g3_ana;
	void __iomem *g3_pma;

	void __iomem *qp_ana;
	void __iomem *qp_pma;
	void __iomem *qp_dig;

	void __iomem *xr_dtime;
	void __iomem *rx_aeq;
};

static void airoha_phy_clear_bits(void __iomem *reg, u32 mask)
{
	u32 val = readl(reg) & ~mask;

	writel(val, reg);
}

static void airoha_phy_set_bits(void __iomem *reg, u32 mask)
{
	u32 val = readl(reg) | mask;

	writel(val, reg);
}

static void airoha_phy_update_bits(void __iomem *reg, u32 mask, u32 val)
{
	u32 tmp = readl(reg);

	tmp &= ~mask;
	tmp |= val & mask;
	writel(tmp, reg);
}

#define airoha_phy_update_field(reg, mask, val)					\
	do {									\
		BUILD_BUG_ON_MSG(!__builtin_constant_p((mask)),			\
				 "mask is not constant");			\
		airoha_phy_update_bits((reg), (mask),				\
				       FIELD_PREP((mask), (val)));		\
	} while (0)

#define airoha_g3_ana_clear_bits(pcie_phy, reg, mask)				\
	airoha_phy_clear_bits((pcie_phy)->g3_ana + (reg), (mask))
#define airoha_g3_ana_set_bits(pcie_phy, reg, mask)				\
	airoha_phy_set_bits((pcie_phy)->g3_ana + (reg), (mask))
#define airoha_g3_ana_update_field(pcie_phy, reg, mask, val)			\
	airoha_phy_update_field((pcie_phy)->g3_ana + (reg), (mask), (val))

#define airoha_g3_pma_write(pcie_phy, reg, val)				\
	writel((val), (pcie_phy)->g3_pma + (reg))
#define airoha_g3_pma_clear_bits(pcie_phy, reg, mask)				\
	airoha_phy_clear_bits((pcie_phy)->g3_pma + (reg), (mask))
#define airoha_g3_pma_set_bits(pcie_phy, reg, mask)				\
	airoha_phy_set_bits((pcie_phy)->g3_pma + (reg), (mask))
#define airoha_g3_pma_update_field(pcie_phy, reg, mask, val)			\
	airoha_phy_update_field((pcie_phy)->g3_pma + (reg), (mask), (val))

#define airoha_qphy_ana_clear_bits(pcie_phy, reg, mask)				\
	airoha_phy_clear_bits((pcie_phy)->qp_ana + (reg), (mask))
#define airoha_qphy_ana_set_bits(pcie_phy, reg, mask)				\
	airoha_phy_set_bits((pcie_phy)->qp_ana + (reg), (mask))
#define airoha_qphy_ana_update_field(pcie_phy, reg, mask, val)			\
	airoha_phy_update_field((pcie_phy)->qp_ana + (reg), (mask), (val))

#define airoha_qphy_pma_read(pcie_phy, reg)					\
	readl((pcie_phy)->qp_pma + (reg))
#define airoha_qphy_pma_clear_bits(pcie_phy, reg, mask)				\
	airoha_phy_clear_bits((pcie_phy)->qp_pma + (reg), (mask))
#define airoha_qphy_pma_set_bits(pcie_phy, reg, mask)				\
	airoha_phy_set_bits((pcie_phy)->qp_pma + (reg), (mask))
#define airoha_qphy_pma_update_field(pcie_phy, reg, mask, val)			\
	airoha_phy_update_field((pcie_phy)->qp_pma + (reg), (mask), (val))

#define airoha_qphy_dig_clear_bits(pcie_phy, reg, mask)				\
	airoha_phy_clear_bits((pcie_phy)->qp_dig + (reg), (mask))
#define airoha_qphy_dig_set_bits(pcie_phy, reg, mask)				\
	airoha_phy_set_bits((pcie_phy)->qp_dig + (reg), (mask))
#define airoha_qphy_dig_update_field(pcie_phy, reg, mask, val)			\
	airoha_phy_update_field((pcie_phy)->qp_dig + (reg), (mask), (val))

static void an7583_pcie_phy_init_default(struct an7583_pcie_phy *pcie_phy)
{
	/* Load E-Fuse */
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CMN_EN,
				   G3_ANA_PXP_CMN_TRIM, 0x10);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_TX_TERM_SEL,
			       G3_PMA_FORCE_SEL_DA_PXP_TX_TERM_SEL);
	airoha_g3_pma_update_field(pcie_phy, REG_FORCE_DA_PXP_TX_TERM_SEL,
				   G3_PMA_FORCE_DA_PXP_TX_TERM_SEL, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_SIGDET_NOVTH,
				   G3_ANA_PXP_RX_FE_50OHMS_SEL, 0x1);

	/* Default Init */
	airoha_g3_pma_set_bits(pcie_phy, REG_ADD_DIG_RESERVE_12,
			       G3_PMA_DIG_RESERVE_12_8_8);
	airoha_g3_pma_clear_bits(pcie_phy, REG_ADD_DIG_RESERVE_27,
				 G3_PMA_DIG_RESERVE_27_16_16);
	airoha_g3_pma_write(pcie_phy, REG_ADD_DIG_RESERVE_34, 0xcccbcccb);
	airoha_g3_pma_write(pcie_phy, REG_ADD_DIG_RESERVE_35, 0xcccb);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_CMN_EN,
			       G3_ANA_PXP_CMN_EN);
}

static void an7583_pcie_phy_init_clk_out(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_ana_update_field(pcie_phy, REG_PCIE_CLKTX0_AMP,
				   G3_ANA_PCIE_CLKTX0_AMP, 0x5);
	airoha_g3_ana_update_field(pcie_phy, REG_PCIE_CLKTX0_AMP,
				   G3_ANA_PCIE_CLKTX0_OFFSET, 0x2);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PCIE_CLKTX0_AMP,
				 G3_ANA_PXP_PCIE_CLKTX0_HZ);
	airoha_g3_ana_update_field(pcie_phy, REG_PCIE_CLKTX0_AMP,
				   G3_ANA_PXP_PCIE_CLKTX0_IMP_SEL, 0x12);
	airoha_g3_ana_update_field(pcie_phy, REG_PCIE_CLKTX0_AMP,
				   G3_ANA_PCIE_CLKTX0_SR, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_PLL_MONCLK_SEL,
				   G3_ANA_PXP_PLL_CMN_RESERVE0, 0xD);
}

static void an7583_pcie_phy_init_ana(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_pma_set_bits(pcie_phy, REG_SW_RST_SET,
			       G3_PMA_SW_XFI_RXPCS_RST_N);
	airoha_g3_pma_set_bits(pcie_phy, REG_SW_RST_SET,
			       G3_PMA_SW_REF_RST_N);
	airoha_g3_pma_set_bits(pcie_phy, REG_SW_RST_SET,
			       G3_PMA_SW_RX_RST_N);
	airoha_g3_pma_set_bits(pcie_phy, REG_SS_TX_RST_B,
			       G3_PMA_TXCALIB_RST_B);
	airoha_g3_pma_set_bits(pcie_phy, REG_SS_TX_RST_B,
			       G3_PMA_TX_TOP_RST_B);
}

static void an7583_pcie_phy_init_rx(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_pma_write(pcie_phy, REG_ADD_DIG_RESERVE_30, 0x2a00090b);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_CDR_PR_MONPR_EN,
			       G3_ANA_PXP_CDR_PR_XFICK_EN);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_CDR_PD_PICAL_CKD8_INV,
				 G3_ANA_PXP_CDR_PD_EDGE_DIS);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_PHYCK_DIV,
				   G3_ANA_PXP_RX_PHYCK_SEL, 0x1);
}

static void an7583_pcie_phy_init_jcpll(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			       G3_PMA_FORCE_SEL_DA_PXP_JCPLL_EN);
	airoha_g3_pma_clear_bits(pcie_phy, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
				 G3_PMA_FORCE_DA_PXP_JCPLL_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SPARE_H,
				   G3_ANA_PXP_JCPLL_SPARE_L, 0x20);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_RST_DLY,
			       G3_ANA_PXP_JCPLL_PLL_RSTB);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SSC_DELTA,
				   G3_ANA_PXP_JCPLL_SSC_DELTA, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SSC_TRI_EN,
				   G3_ANA_PXP_JCPLL_SSC_DELTA1, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SSC_DELTA,
				   G3_ANA_PXP_JCPLL_SSC_PERIOD, 0x0);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_JCPLL_VCO_TCLVAR,
				 G3_ANA_PXP_JCPLL_SSC_PHASE_INI);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_JCPLL_SSC_TRI_EN,
				 G3_ANA_PXP_JCPLL_SSC_TRI_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_LPF_BR,
				   G3_ANA_PXP_JCPLL_LPF_BR, 0xA);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_LPF_BR,
				   G3_ANA_PXP_JCPLL_LPF_BP, 0xC);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_LPF_BR,
				   G3_ANA_PXP_JCPLL_LPF_BC, 0x1F);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_LPF_BWC,
				   G3_ANA_PXP_JCPLL_LPF_BWC, 0x1E);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_LPF_BR,
				   G3_ANA_PXP_JCPLL_LPF_BWR, 0xA);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_MMD_PREDIV_MODE,
				   G3_ANA_PXP_JCPLL_MMD_PREDIV_MODE, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_MONCK_EN,
				   G3_ANA_PXP_JCPLL_REFIN_DIV, 0x0);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_RX_FE_VOS,
			       G3_PMA_FORCE_SEL_DA_PXP_JCPLL_SDM_PCW);
	airoha_g3_pma_update_field(pcie_phy, REG_FORCE_DA_PXP_JCPLL_SDM_PCW,
				   G3_PMA_FORCE_DA_PXP_JCPLL_SDM_PCW, 0x50000000);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_MMD_PREDIV_MODE,
			       G3_ANA_PXP_JCPLL_POSTDIV_D5);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_MMD_PREDIV_MODE,
			       G3_ANA_PXP_JCPLL_POSTDIV_D2);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_RST_DLY,
				   G3_ANA_PXP_JCPLL_RST_DLY, 0x4);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_RST_DLY,
				   G3_ANA_PXP_JCPLL_SDM_DI_LS, 0x0);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_JCPLL_FREQ_MEAS_EN,
				 G3_ANA_PXP_JCPLL_VCO_KBAND_MEAS_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_IB_EXT_EN,
				   G3_ANA_PXP_JCPLL_CHP_IOFST, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_IB_EXT_EN,
				   G3_ANA_PXP_JCPLL_CHP_IBIAS, 0xC);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_MMD_PREDIV_MODE,
				   G3_ANA_PXP_JCPLL_MMD_PREDIV_MODE, 0x1);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_VCODIV,
			       G3_ANA_PXP_JCPLL_VCO_HALFLSB_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_VCODIV,
				   G3_ANA_PXP_JCPLL_VCO_CFIX, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_VCODIV,
				   G3_ANA_PXP_JCPLL_VCO_SCAPWR, 0x4);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_JCPLL_IB_EXT_EN,
				 G3_ANA_PXP_JCPLL_LPF_SHCK_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_KBAND_KFC,
			       G3_ANA_PXP_JCPLL_POSTDIV_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_KBAND_KFC,
				   G3_ANA_PXP_JCPLL_KBAND_KFC, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_KBAND_KFC,
				   G3_ANA_PXP_JCPLL_KBAND_KF, 0x3);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_KBAND_KFC,
				   G3_ANA_PXP_JCPLL_KBAND_KS, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_LPF_BWC,
				   G3_ANA_PXP_JCPLL_KBAND_DIV, 0x1);
	airoha_g3_pma_set_bits(pcie_phy, REG_SCAN_MODE,
			       G3_PMA_FORCE_SEL_DA_PXP_JCPLL_KBAND_LOAD_EN);
	airoha_g3_pma_clear_bits(pcie_phy, REG_SCAN_MODE,
				 G3_PMA_FORCE_DA_PXP_JCPLL_KBAND_LOAD_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_LPF_BWC,
				   G3_ANA_PXP_JCPLL_KBAND_CODE, 0xE4);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_SDM_HREN,
			       G3_ANA_PXP_JCPLL_TCL_AMP_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_TCL_CMP_EN,
			       G3_ANA_PXP_JCPLL_TCL_LPF_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SPARE_H,
				   G3_ANA_PXP_JCPLL_TCL_KBAND_VREF, 0xF);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SDM_HREN,
				   G3_ANA_PXP_JCPLL_TCL_AMP_GAIN, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SDM_HREN,
				   G3_ANA_PXP_JCPLL_TCL_AMP_VREF, 0x5);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_TCL_CMP_EN,
				   G3_ANA_PXP_JCPLL_TCL_LPF_BW, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_VCO_TCLVAR,
				   G3_ANA_PXP_JCPLL_VCO_TCLVAR, 0x3);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			       G3_PMA_FORCE_SEL_DA_PXP_JCPLL_CKOUT_EN);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			       G3_PMA_FORCE_DA_PXP_JCPLL_CKOUT_EN);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			       G3_PMA_FORCE_SEL_DA_PXP_JCPLL_EN);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			       G3_PMA_FORCE_DA_PXP_JCPLL_EN);
}

static void an7583_pcie_phy_txpll(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			       G3_PMA_FORCE_SEL_DA_PXP_TXPLL_EN);
	airoha_g3_pma_clear_bits(pcie_phy, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
				 G3_PMA_FORCE_DA_PXP_TXPLL_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_TXPLL_REFIN_INTERNAL,
			       G3_ANA_PXP_TXPLL_PLL_RSTB);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_SSC_DELTA1,
				   G3_ANA_PXP_TXPLL_SSC_DELTA, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_SSC_DELTA1,
				   G3_ANA_PXP_TXPLL_SSC_DELTA1, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_SSC_PERIOD,
				   G3_ANA_PXP_TXPLL_SSC_PERIOD, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_CHP_IBIAS,
				   G3_ANA_PXP_TXPLL_CHP_IOFST, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_CHP_IBIAS,
				   G3_ANA_PXP_TXPLL_CHP_IBIAS, 0x2D);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_REFIN_INTERNAL,
				   G3_ANA_PXP_TXPLL_REFIN_DIV, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_TCL_LPF_EN,
				   G3_ANA_PXP_TXPLL_VCO_CFIX, 0x3);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_CDR_PR_IDAC,
			       G3_PMA_FORCE_SEL_DA_PXP_TXPLL_SDM_PCW);
	airoha_g3_pma_update_field(pcie_phy, REG_FORCE_DA_PXP_TXPLL_SDM_PCW,
				   G3_PMA_FORCE_DA_PXP_TXPLL_SDM_PCW, 0xC800000);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_SDM_DI_EN,
				 G3_ANA_PXP_TXPLL_SDM_IFM);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_SSC_EN,
				 G3_ANA_PXP_TXPLL_SSC_PHASE_INI);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_REFIN_INTERNAL,
				   G3_ANA_PXP_TXPLL_RST_DLY, 0x4);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_SDM_DI_EN,
				   G3_ANA_PXP_TXPLL_SDM_DI_LS, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_SDM_ORD,
				   G3_ANA_PXP_TXPLL_SDM_ORD, 0x3);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_TCL_KBAND_VREF,
				 G3_ANA_PXP_TXPLL_VCO_KBAND_MEAS_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_SSC_DELTA1,
				   G3_ANA_PXP_TXPLL_SSC_DELTA, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_SSC_DELTA1,
				   G3_ANA_PXP_TXPLL_SSC_DELTA1, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_LPF_BP,
				   G3_ANA_PXP_TXPLL_LPF_BP, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_CHP_IBIAS,
				   G3_ANA_PXP_TXPLL_LPF_BC, 0x18);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_CHP_IBIAS,
				   G3_ANA_PXP_TXPLL_LPF_BR, 0x5);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_CHP_IBIAS,
				   G3_ANA_PXP_TXPLL_CHP_IOFST, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_CHP_IBIAS,
				   G3_ANA_PXP_TXPLL_CHP_IBIAS, 0x2D);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_TCL_VTP_EN,
				   G3_ANA_PXP_TXPLL_SPARE_L, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_LPF_BP,
				   G3_ANA_PXP_TXPLL_LPF_BWC, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_KBAND_KS,
				   G3_ANA_PXP_TXPLL_MMD_PREDIV_MODE, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_REFIN_INTERNAL,
				   G3_ANA_PXP_TXPLL_REFIN_DIV, 0x0);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_TXPLL_VCO_HALFLSB_EN,
			       G3_ANA_PXP_TXPLL_VCO_HALFLSB_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_VCO_HALFLSB_EN,
				   G3_ANA_PXP_TXPLL_VCO_SCAPWR, 0x7);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_TCL_LPF_EN,
				   G3_ANA_PXP_TXPLL_VCO_CFIX, 0x3);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_CDR_PR_IDAC,
			       G3_PMA_FORCE_SEL_DA_PXP_TXPLL_SDM_PCW);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_SSC_EN,
				 G3_ANA_PXP_TXPLL_SSC_PHASE_INI);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_LPF_BP,
				   G3_ANA_PXP_TXPLL_LPF_BWR, 0x0);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_TXPLL_REFIN_INTERNAL,
			       G3_ANA_PXP_TXPLL_REFIN_INTERNAL);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_TCL_KBAND_VREF,
				 G3_ANA_PXP_TXPLL_VCO_KBAND_MEAS_EN);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_VTP_EN,
				 G3_ANA_PXP_TXPLL_VTP_EN);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_PHY_CK1_EN,
				 G3_ANA_PXP_TXPLL_PHY_CK1_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_TXPLL_REFIN_INTERNAL,
			       G3_ANA_PXP_TXPLL_REFIN_INTERNAL);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_SSC_EN,
				 G3_ANA_PXP_TXPLL_SSC_EN);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_JCPLL_FREQ_MEAS_EN,
				 G3_ANA_PXP_TXPLL_LPF_SHCK_EN);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_TXPLL_KBAND_KS,
				 G3_ANA_PXP_TXPLL_POSTDIV_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_KBAND_CODE,
				   G3_ANA_PXP_TXPLL_KBAND_KFC, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_KBAND_CODE,
				   G3_ANA_PXP_TXPLL_KBAND_KF, 0x3);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_KBAND_KS,
				   G3_ANA_PXP_TXPLL_KBAND_KS, 0x1);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_KBAND_CODE,
				   G3_ANA_PXP_TXPLL_KBAND_DIV, 0x4);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_KBAND_CODE,
				   G3_ANA_PXP_TXPLL_KBAND_CODE, 0xE4);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_TXPLL_SDM_ORD,
			       G3_ANA_PXP_TXPLL_TCL_AMP_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_TXPLL_TCL_LPF_EN,
			       G3_ANA_PXP_TXPLL_TCL_LPF_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_TCL_KBAND_VREF,
				   G3_ANA_PXP_TXPLL_TCL_KBAND_VREF, 0xF);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_TCL_AMP_GAIN,
				   G3_ANA_PXP_TXPLL_TCL_AMP_GAIN, 0x3);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_TCL_AMP_GAIN,
				   G3_ANA_PXP_TXPLL_TCL_AMP_VREF, 0xB);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_TXPLL_TCL_LPF_EN,
				   G3_ANA_PXP_TXPLL_TCL_LPF_BW, 0x3);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			       G3_PMA_FORCE_SEL_DA_PXP_TXPLL_CKOUT_EN);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			       G3_PMA_FORCE_DA_PXP_TXPLL_CKOUT_EN);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			       G3_PMA_FORCE_SEL_DA_PXP_TXPLL_EN);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			       G3_PMA_FORCE_DA_PXP_TXPLL_EN);
}

static void an7583_pcie_phy_init_ssc_jcpll(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SSC_DELTA,
				   G3_ANA_PXP_JCPLL_SSC_DELTA, 0x106);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SSC_TRI_EN,
				   G3_ANA_PXP_JCPLL_SSC_DELTA1, 0x106);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_JCPLL_SSC_DELTA,
				   G3_ANA_PXP_JCPLL_SSC_PERIOD, 0x31B);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_VCO_TCLVAR,
			       G3_ANA_PXP_JCPLL_SSC_PHASE_INI);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_VCO_TCLVAR,
			       G3_ANA_PXP_JCPLL_SSC_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_SDM_IFM,
			       G3_ANA_PXP_JCPLL_SDM_IFM);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_SDM_HREN,
			       G3_ANA_PXP_JCPLL_SDM_HREN);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_JCPLL_RST_DLY,
				 G3_ANA_PXP_JCPLL_SDM_DI_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_JCPLL_SSC_TRI_EN,
			       G3_ANA_PXP_JCPLL_SSC_TRI_EN);
}

static void
an7583_pcie_phy_set_rxlan0_signal_detect(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_CDR_PR_TDC_REF_SEL,
			       G3_ANA_PXP_CDR_PR_LDO_FORCE_ON);
	usleep_range(10, 110);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_32,
				   G3_PMA_DIG_RESERVE_32_31_16, 0x18B0);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_33,
				   G3_PMA_DIG_RESERVE_33_15_0, 0x18B0);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_33,
				   G3_PMA_DIG_RESERVE_33_31_16, 0x1030);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_SIGDET_NOVTH,
				   G3_ANA_PXP_RX_SIGDET_PEAK_1_0, 0x2);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_SIGDET_NOVTH,
				   G3_ANA_PXP_RX_SIGDET_VTH_SEL_4_0, 0x5);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_REV_0,
				   G3_ANA_PXP_RX_REV_1_3_2, 0x2);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_DAC_RANGE,
				   G3_ANA_PXP_RX_SIGDET_LPF_CTRL, 0x1);
	airoha_g3_pma_update_field(pcie_phy, REG_SS_RX_CAL_2,
				   G3_PMA_CAL_OUT_OS, 0x0);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_RX_FE_VCM_GEN_PWDB,
			       G3_ANA_PXP_RX_FE_VCM_GEN_PWDB);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_RX_FE_GAIN_CTRL,
			       G3_PMA_FORCE_SEL_DA_PXP_RX_FE_GAIN_CTRL);
	airoha_g3_pma_update_field(pcie_phy, REG_FORCE_DA_PXP_RX_FE_GAIN_CTRL,
				   G3_PMA_FORCE_DA_PXP_RX_FE_GAIN_CTRL, 0x3);
	airoha_g3_pma_update_field(pcie_phy, REG_RX_FORCE_MODE_0,
				   G3_PMA_FORCE_DA_XPON_RX_FE_GAIN_CTRL, 0x1);
	airoha_g3_pma_update_field(pcie_phy, REG_SS_RX_SIGDET_0,
				   G3_PMA_SIGDET_WIN_NONVLD_TIMES, 0x3);
	airoha_g3_pma_clear_bits(pcie_phy, REG_RX_CTRL_SEQUENCE_DISB_CTRL_1,
				 G3_PMA_DISB_RX_SDCAL_EN);
	airoha_g3_pma_set_bits(pcie_phy, REG_RX_CTRL_SEQUENCE_FORCE_CTRL_1,
			       G3_PMA_FORCE_RX_SDCAL_EN);
	usleep_range(100, 200);
	airoha_g3_pma_clear_bits(pcie_phy, REG_RX_CTRL_SEQUENCE_FORCE_CTRL_1,
				 G3_PMA_FORCE_RX_SDCAL_EN);
}

static void an7583_pcie_phy_set_rxflow(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_RX_SCAN_RST_B,
			       G3_PMA_FORCE_SEL_DA_PXP_RX_SIGDET_PWDB);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_RX_SCAN_RST_B,
			       G3_PMA_FORCE_DA_PXP_RX_SIGDET_PWDB);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_CDR_PD_PWDB,
			       G3_PMA_FORCE_SEL_DA_PXP_CDR_PD_PWDB);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_CDR_PD_PWDB,
			       G3_PMA_FORCE_DA_PXP_CDR_PD_PWDB);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_RX_FE_PWDB,
			       G3_PMA_FORCE_SEL_DA_PXP_RX_FE_PWDB);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_RX_FE_PWDB,
			       G3_PMA_FORCE_DA_PXP_RX_FE_PWDB);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_RX_PHYCK_DIV,
			       G3_ANA_PXP_RX_TDC_CK_SEL);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_RX_PHYCK_DIV,
			       G3_ANA_PXP_RX_PHYCK_RSTB);
	airoha_g3_pma_set_bits(pcie_phy, REG_SW_RST_SET,
			       G3_PMA_SW_TX_FIFO_RST_N);
	airoha_g3_pma_set_bits(pcie_phy, REG_SW_RST_SET,
			       G3_PMA_SW_ALLPCS_RST_N);
	airoha_g3_pma_set_bits(pcie_phy, REG_SW_RST_SET,
			       G3_PMA_SW_PMA_RST_N);
	airoha_g3_pma_set_bits(pcie_phy, REG_SW_RST_SET,
			       G3_PMA_SW_TX_RST_N);
	airoha_g3_pma_set_bits(pcie_phy, REG_SW_RST_SET,
			       G3_PMA_SW_RX_FIFO_RST_N);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_RX_FE_EQ_HZEN,
			       G3_ANA_PXP_RX_FE_VB_EQ3_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_RX_FE_EQ_HZEN,
			       G3_ANA_PXP_RX_FE_VB_EQ2_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_RX_FE_EQ_HZEN,
			       G3_ANA_PXP_RX_FE_VB_EQ1_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_REV_0,
				   G3_ANA_PXP_RX_REV_1_6_4, 0x4);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_REV_0,
				   G3_ANA_PXP_RX_REV_1_10_8, 0x4);
}

static void an7583_pcie_phy_set_pr(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CDR_PR_VREG_IBAND_VAL,
				   G3_ANA_PXP_CDR_PR_VREG_IBAND_VAL_2_0, 0x5);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CDR_PR_VREG_IBAND_VAL,
				   G3_ANA_PXP_CDR_PR_VREG_CKBUF_VAL_2_0, 0x5);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CDR_PR_CKREF_DIV,
				   G3_ANA_PXP_CDR_PR_CKREF_DIV_1_0, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CDR_PR_TDC_REF_SEL,
				   G3_ANA_PXP_CDR_PR_CKREF_DIV1_1_0, 0x0);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CDR_LPF_RATIO,
				   G3_ANA_PXP_CDR_LPF_TOP_LIM, 0x20000);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CDR_PR_BETA_DAC,
				   G3_ANA_PXP_CDR_PR_BETA_SEL, 0x2);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CDR_PR_BETA_DAC,
				   G3_ANA_PXP_CDR_PR_KBAND_DIV, 0x4);
}

static void an7583_pcie_phy_set_txflow(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_TX_CKLDO_EN,
			       G3_ANA_PXP_TX_CKLDO_EN);
	airoha_g3_ana_set_bits(pcie_phy, REG_PXP_TX_CKLDO_EN,
			       G3_ANA_PXP_TX_DMEDGEGEN_EN);
}

static void an7583_pcie_phy_set_rx_mode(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_pma_write(pcie_phy, REG_ADD_DIG_RESERVE_40, 0x804000);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_31,
				   G3_PMA_DIG_RESERVE_31_4_0, 0x5);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_31,
				   G3_PMA_DIG_RESERVE_31_12_8, 0x5);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_31,
				   G3_PMA_DIG_RESERVE_31_20_16, 0x5);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_43,
				   G3_PMA_DIG_RESERVE_43_10_8, 0x7);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_43,
				   G3_PMA_DIG_RESERVE_43_14_12, 0x7);
	airoha_g3_pma_update_field(pcie_phy, REG_ADD_DIG_RESERVE_43,
				   G3_PMA_DIG_RESERVE_43_18_16, 0x7);
	airoha_g3_ana_clear_bits(pcie_phy, REG_PXP_CDR_PR_MONCK_EN,
				 G3_ANA_PXP_CDR_PR_MONCK_EN);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_CDR_PR_MONCK_EN,
				   G3_ANA_PXP_CDR_PR_RESERVE0, 0x2);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_OSCAL_CTLE2IOS,
				   G3_ANA_PXP_RX_OSCAL_VGA1IOS, 0x19);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_OSCAL_CTLE2IOS,
				   G3_ANA_PXP_RX_OSCAL_VGA1VOS, 0x19);
	airoha_g3_ana_update_field(pcie_phy, REG_PXP_RX_OSCAL_VGA2IOS,
				   G3_ANA_PXP_RX_OSCAL_VGA2IOS, 0x14);
}

static void an7583_pcie_phy_set_eye_scan(struct an7583_pcie_phy *pcie_phy)
{
	airoha_g3_pma_update_field(pcie_phy, REG_FORCE_DA_PXP_CDR_PR_FLL_COR,
				   G3_PMA_FORCE_DA_PXP_RX_DAC_EYE, 0x0);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_CDR_PR_FLL_COR,
			       G3_PMA_FORCE_SEL_DA_PXP_RX_DAC_EYE);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_CDR_PR_PIEYE_PWDB,
			       G3_PMA_FORCE_DA_PXP_CDR_PR_PIEYE_PWDB);
	airoha_g3_pma_set_bits(pcie_phy, REG_FORCE_DA_PXP_CDR_PR_PIEYE_PWDB,
			       G3_PMA_FORCE_SEL_DA_PXP_CDR_PR_PIEYE_PWDB);
	airoha_g3_pma_clear_bits(pcie_phy, REG_SS_DA_XPON_PWDB_0,
				 G3_PMA_DA_XPON_CDR_PR_PWDB);
}

static void an7583_pcie_phy_tx_pll_disable(struct an7583_pcie_phy *pcie_phy)
{
	/* TX Force Disable */
	airoha_qphy_pma_clear_bits(pcie_phy, REG_INTF_CTRL_6,
				   QP_PMA_DA_QP_TX_DATA_EN_FORCE);
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_6,
				 QP_PMA_DA_QP_TX_DATA_EN_SEL);

	/* PLL Force Unstable (For PMA) */
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CK_CTRL_1,
				 QP_PMA_PLL_FORCE_UNSTABLE);

	/* PLL Disable */
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CTRL_0,
				   QP_PMA_PHYA_AUTO_INIT);
	usleep_range(1000, 2000);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CTRL_0,
				 QP_PMA_PHYA_AUTO_INIT);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_INTF_CTRL_7,
				   QP_PMA_DA_QP_PLL_EN_FORCE);
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_7,
				 QP_PMA_DA_QP_PLL_EN_SEL);
}

static void an7583_pcie_phy_tx_pll_enable(struct an7583_pcie_phy *pcie_phy)
{
	/* PLL Reset to default */
	airoha_qphy_ana_clear_bits(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				   QP_ANA_QP_PLL_MONVC_EN);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				     QP_ANA_QP_PLL_MON_LDO_SEL, 0x0);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_INTF_CTRL_7,
				   QP_PMA_DA_QP_PLL_EN_SEL);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_INTF_CTRL_7,
				   QP_PMA_DA_QP_PLL_EN_FORCE);

	/* PLL Auto Mode (For PMA) */
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CK_CTRL_1,
				 QP_PMA_PLL_FORCE_UNSTABLE);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CK_CTRL_1,
				 QP_PMA_PLL_FORCE_STABLE);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CK_CTRL_1,
				   QP_PMA_PLL_FORCE_UNSTABLE);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CK_CTRL_1,
				   QP_PMA_PLL_FORCE_STABLE);

	/* TX Auto Mode */
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_6,
				 QP_PMA_DA_QP_TX_DATA_EN_FORCE);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_INTF_CTRL_6,
				   QP_PMA_DA_QP_TX_DATA_EN_SEL);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_INTF_CTRL_6,
				   QP_PMA_DA_QP_TX_DATA_EN_FORCE);
}

static void an7583_pcie_phy_init_pll_kband_flow(struct an7583_pcie_phy *pcie_phy)
{
	airoha_qphy_pma_set_bits(pcie_phy, REG_RX_CTRL_36,
				 QP_PMA_QP_PCIE_USB_SYSTEM);

	/* 50Mhz XTAL */
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_BGR_EN,
				     QP_ANA_QP_BG_DIV, 0x1);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				     QP_ANA_QP_PLL_PREDIV, 0x1);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CTRL_4,
				   QP_PMA_DA_QP_PLL_ICOLP_EN_INTF);
	airoha_qphy_dig_clear_bits(pcie_phy, REG_QP_CK_RST_CTRL_7,
				   QP_DIG_MULTI_PHY_USB5_EN);
	airoha_qphy_dig_clear_bits(pcie_phy, REG_QP_CK_RST_CTRL_7,
				   QP_DIG_MULTI_PHY_USB2P5_EN);
	airoha_qphy_dig_set_bits(pcie_phy, REG_QP_CK_RST_CTRL_7,
				 QP_DIG_MULTI_PHY_USB_MODE_EN);
	airoha_qphy_dig_set_bits(pcie_phy, REG_QP_CK_RST_CTRL_7,
				 QP_DIG_MULTI_PHY_USB5_EN);
	airoha_qphy_dig_set_bits(pcie_phy, REG_QP_CK_RST_CTRL_7,
				 QP_DIG_MULTI_PHY_USB2P5_EN);

	/* PLL RG */
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CTRL_2,
				 QP_PMA_DA_QP_PLL_PCK_SEL_INTF);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_2,
				     QP_PMA_DA_QP_PLL_IR_INTF, 0x4);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_2,
				     QP_PMA_DA_QP_PLL_FBKSEL_INTF, 0x0);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_2,
				     QP_PMA_DA_QP_PLL_KBAND_PREDIV_INTF, 0x0);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_2,
				     QP_PMA_DA_QP_PLL_BC_INTF, 0x3);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_2,
				     QP_PMA_DA_QP_PLL_BPA_INTF, 0x5);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_2,
				     QP_PMA_DA_QP_PLL_BPB_INTF, 0x1);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CTRL_2,
				 QP_PMA_DA_QP_PLL_ICOIQ_EN_INTF);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CTRL_2,
				   QP_PMA_DA_QP_PLL_PHY_CK_EN_INTF);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_4,
				     QP_PMA_DA_QP_PLL_SDM_HREN_INTF, 0x1);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CTRL_2,
				 QP_PMA_DA_QP_PLL_SDM_IFM_INTF);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_3,
				     QP_PMA_DA_QP_PLL_SSC_DELTA_INTF, 0x1FD);
	airoha_qphy_pma_update_field(pcie_phy, REG_PLL_CTRL_3,
				     QP_PMA_DA_QP_PLL_SSC_PERIOD_INTF, 0x19C);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CTRL_1,
				 QP_PMA_QP_PLL_SSC_EN);
	airoha_qphy_pma_update_field(pcie_phy, REG_SS_LCPLL_TDC_PCW_1,
				     QP_PMA_LCPLL_PON_HRDDS_PCW_NCPO_GPON, 0x48000000);
	airoha_qphy_pma_update_field(pcie_phy, REG_SS_LCPLL_PWCTL_SETTING_2,
				     QP_PMA_NCPO_ANA_MSB, 0x1);
	airoha_qphy_pma_update_field(pcie_phy, REG_SS_LCPLL_TDC_FLT_2,
				     QP_PMA_LCPLL_NCPO_VALUE, 0x48000000);

	/* RX RG */
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_CDR_LPF_MJV_LIM,
				     QP_ANA_QP_CDR_LPF_RATIO, 0x1);
	airoha_qphy_ana_set_bits(pcie_phy, REG_QP_RXAFE_RESERVE,
				 QP_ANA_QP_CDR_PD_10B_EN);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_CDR_PR_CKREF_DIV1,
				     QP_ANA_QP_CDR_PR_DAC_BAND, 0xC);
	airoha_qphy_ana_clear_bits(pcie_phy, REG_QP_CDR_FORCE_IBANDLPF_R_OFF,
				   QP_ANA_QP_CDR_PHYCK_RSTB);
	airoha_qphy_ana_clear_bits(pcie_phy, REG_QP_CDR_PR_KBAND_DIV_PCIE,
				   QP_ANA_QP_CDR_PR_XFICK_EN);
	airoha_qphy_ana_set_bits(pcie_phy, REG_QP_CDR_PR_KBAND_DIV_PCIE,
				 QP_ANA_QP_CDR_PR_KBAND_PCIE_MODE);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_CDR_PR_CKREF_DIV1,
				     QP_ANA_QP_CDR_PR_KBAND_DIV, 0x3);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_CDR_PR_KBAND_DIV_PCIE,
				     QP_ANA_QP_CDR_PR_KBAND_DIV_PCIE, 0x19);

	/* Init L0/L0S U0/U1 */
	airoha_qphy_pma_set_bits(pcie_phy, REG_RX_CTRL_46,
				 QP_PMA_QP_PCIE_USB_BYPASS_EQ_P1_TO_P0_EN);
	airoha_qphy_pma_set_bits(pcie_phy, REG_RX_CTRL_46,
				 QP_PMA_REBACK_P0_LCK2REF_EN);
	airoha_qphy_pma_set_bits(pcie_phy, REG_RX_CTRL_11,
				 QP_PMA_QP_FORCE_SIGDET_5G);
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_CTRL_36,
				     QP_PMA_QP_LCK2DATA_DLY_TIME_9_0, 0x2);
	airoha_qphy_ana_clear_bits(pcie_phy, REG_QP_RXAFE_RESERVE,
				   QP_ANA_QP_CDR_PD_EDGE_DIS);

	/* PI Calibration */
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_DLY_0,
				     QP_PMA_QP_RX_PI_CAL_EN_H_DLY, 0x10);

	/* CRS detect and CLK TRX Control */
	airoha_qphy_pma_set_bits(pcie_phy, REG_RX_CTRL_10,
				 QP_PMA_QP_CRSDET_RSTB);
	airoha_qphy_ana_set_bits(pcie_phy, REG_QP_PLL_SDM_ORD,
				 QP_ANA_QP_PLL_SSC_PHASE_INI);
	airoha_qphy_ana_set_bits(pcie_phy, REG_QP_PLL_SDM_ORD,
				 QP_ANA_QP_PLL_SSC_TRI_EN);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CK_CTRL_0,
				   QP_PMA_DA_PCIE_CLKRX_EN_INTF);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CK_CTRL_0,
				 QP_PMA_DA_PCIE_CLKTX_EN_INTF);
	airoha_qphy_ana_clear_bits(pcie_phy, REG_PCIE_CLKDRV_IMPSEL,
				   QP_ANA_PCIE_CLKDRV_HZ);
	airoha_qphy_ana_update_field(pcie_phy, REG_PCIE_CLKDRV_IMPSEL,
				     QP_ANA_PCIE_CLKDRV_AMP, 0x4);
	airoha_qphy_ana_update_field(pcie_phy, REG_PCIE_CLKDRV_IMPSEL,
				     QP_ANA_PCIE_CLKDRV_FORCEIN, 0x1);
	airoha_qphy_ana_update_field(pcie_phy, REG_PCIE_CLKDRV_IMPSEL,
				     QP_ANA_PCIE_CLKDRV_IMPSEL, 0x12);
	airoha_qphy_ana_update_field(pcie_phy, REG_PCIE_CLKDRV_IMPSEL,
				     QP_ANA_PCIE_CLKDRV_RP, 0xC);
	airoha_qphy_pma_update_field(pcie_phy, REG_QP_TX_DA_CTRL_3,
				     QP_PMA_TX_DATA_RATE_SEL, 0x1);

	/* Common Setting */
	airoha_qphy_dig_set_bits(pcie_phy, REG_QP_CK_RST_CTRL_3,
				 QP_DIG_US_CK_DIV_SEL);
	airoha_qphy_dig_set_bits(pcie_phy, REG_QP_CK_RST_CTRL_3,
				 QP_DIG_NS_CK_DIV_SEL);
	airoha_qphy_pma_update_field(pcie_phy, REG_QP_TX_DA_CTRL_0,
				     QP_PMA_RXDET_EN_WINDOW, 0xA);
	airoha_qphy_pma_update_field(pcie_phy, REG_QP_TX_DA_CTRL_0,
				     QP_PMA_RXDET_RD_WAIT_TIMER, 0x4);
	airoha_qphy_ana_clear_bits(pcie_phy, REG_QP_RXLBTX_EN,
				   QP_ANA_QP_TX_RXDET_METHOD);
	airoha_qphy_ana_set_bits(pcie_phy, REG_QP_RXLBTX_EN,
				 QP_ANA_QP_TX_DMEDGEGEN_EN);

	/* PLL auto ON/OFF at L2/U3 state */
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CK_CTRL_2,
				 QP_PMA_PCIE_MODE_PLL_AUTO_EN);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CK_CTRL_2,
				 QP_PMA_PCIE_MODE_PLL_AUTO_ON_EN);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CK_CTRL_2,
				 QP_PMA_PCIE_MODE_PLL_AUTO_OFF_EN);

	/* RX speed up */
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_CTRL_5,
				     QP_PMA_FREDET_CHK_CYCLE, 0x28);
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_CTRL_6,
				     QP_PMA_FREDET_GOLDEN_CYCLE, 0x64);
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_CTRL_7,
				     QP_PMA_FREDET_TOLERATE_CYCLE, 0x2710);
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_CTRL_2,
				     QP_PMA_QP_RX_EQ_EN_H_DLY, 0x9C4);
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_CTRL_45,
				     QP_PMA_QP_EQ_EN_DLY, 0x9C4);
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_CTRL_50,
				     QP_PMA_QP_RX_EQ_EN_H_DLY_SHORT, 0x9C4);
	airoha_qphy_pma_update_field(pcie_phy, REG_RX_CTRL_50,
				     QP_PMA_QP_EQ_EN_DLY_SHORT, 0x9C4);
	airoha_qphy_ana_set_bits(pcie_phy, REG_QP_TX_MODE_16B_EN,
				 QP_ANA_QP_TX_RESERVE_8);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PON_RXFEDIG_CTRL_0,
				   QP_PMA_QP_EQ_RX500M_CK_SEL);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CTRL_0,
				 QP_PMA_PHYA_AUTO_INIT);

	/* Select PLL_EN to Force mode */
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_7,
				 QP_PMA_DA_QP_PLL_EN_FORCE);
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_7,
				 QP_PMA_DA_QP_PLL_EN_SEL);
}

static bool an7583_pcie_phy_kband_is_calibrated(struct an7583_pcie_phy *pcie_phy)
{
	u32 val, res;

	mdelay(50); /* TODO */

	/* Read PLL KBand Code */
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CTRL_4,
				   QP_PMA_DA_QP_PLL_ICOLP_EN_INTF);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_BGR_EN,
				     QP_ANA_QP_BIAS_V2V_CAL, 0x15);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				     QP_ANA_QP_PLL_LDOLPF_VSEL, 0x1);
	airoha_qphy_ana_set_bits(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				 QP_ANA_QP_PLL_OSCAL_ENB);
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CTRL_2,
				 QP_PMA_DA_QP_PLL_RICO_SEL_INTF);
	airoha_qphy_pma_update_field(pcie_phy, REG_INTF_CTRL_8,
				     QP_PMA_DA_QP_XTAL_EXT_EN_FORCE, 0x0);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_INTF_CTRL_8,
				   QP_PMA_DA_QP_XTAL_EXT_EN_SEL);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CTRL_2,
				   QP_PMA_DA_QP_PLL_ICOIQ_EN_INTF);
	airoha_qphy_ana_clear_bits(pcie_phy, REG_QP_TDC_FT_CK_EN,
				   QP_ANA_QP_PLL_DEBUG_SEL);

	mdelay(5); /* TODO */

	val = airoha_qphy_pma_read(pcie_phy, REG_INTF_STS_9);
	res = FIELD_GET(QP_PMA_ADDR_INTF_STS_PLL_VCOCAL, val) << 4;

	airoha_qphy_ana_clear_bits(pcie_phy, REG_QP_TDC_FT_CK_EN,
				   QP_ANA_QP_PLL_DEBUG_SEL);

	mdelay(5); /* TODO */

	val = airoha_qphy_pma_read(pcie_phy, REG_INTF_STS_9);
	res |= val;

	/*
	 * KBand is calibrated if KBand Code is NOT 0xfff and
	 * KBand Done is set.
	 */
	return res != 0xfff && res & 0x800;
}

static void an7583_pcie_phy_kband_calibrate(struct an7583_pcie_phy *pcie_phy)
{
	/* PLL Disable */
	airoha_qphy_pma_clear_bits(pcie_phy, REG_INTF_CTRL_7,
				   QP_PMA_DA_QP_PLL_EN_FORCE);
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_7,
				 QP_PMA_DA_QP_PLL_EN_SEL);

	/* PLL Config for CPR */
	airoha_qphy_pma_set_bits(pcie_phy, REG_PLL_CTRL_4,
				 QP_PMA_DA_QP_PLL_ICOLP_EN_INTF);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_BGR_EN,
				     QP_ANA_QP_BIAS_V2V_CAL, 0x0);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				     QP_ANA_QP_PLL_LDOLPF_VSEL, 0x3);
	airoha_qphy_ana_clear_bits(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				   QP_ANA_QP_PLL_OSCAL_ENB);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CTRL_2,
				   QP_PMA_DA_QP_PLL_RICO_SEL_INTF);
	airoha_qphy_pma_update_field(pcie_phy, REG_INTF_CTRL_8,
				     QP_PMA_DA_QP_XTAL_EXT_EN_FORCE, 0x3);
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_8,
				 QP_PMA_DA_QP_XTAL_EXT_EN_SEL);
	airoha_qphy_pma_clear_bits(pcie_phy, REG_PLL_CTRL_2,
				   QP_PMA_DA_QP_PLL_ICOIQ_EN_INTF);
	airoha_qphy_ana_set_bits(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				 QP_ANA_QP_PLL_MONVC_EN);
	airoha_qphy_ana_update_field(pcie_phy, REG_QP_PLL_IPLL_DIG_PWR_SEL,
				     QP_ANA_QP_PLL_MON_LDO_SEL, 0x3);

	/* PLL Enable */
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_7,
				 QP_PMA_DA_QP_PLL_EN_FORCE);
	airoha_qphy_pma_set_bits(pcie_phy, REG_INTF_CTRL_7,
				 QP_PMA_DA_QP_PLL_EN_SEL);
}

static void an7583_pcie_phy_init_qphy(struct an7583_pcie_phy *pcie_phy)
{
	int i;

	an7583_pcie_phy_tx_pll_disable(pcie_phy);

	an7583_pcie_phy_init_pll_kband_flow(pcie_phy);

	/* Try to calibrate KBand up to FREQ_LOCK_MAX_ATTEMPT times */
	for (i = 0; i < FREQ_LOCK_MAX_ATTEMPT; i++) {
		/* Check if KBand is calibrated */
		if (an7583_pcie_phy_kband_is_calibrated(pcie_phy))
			break;

		/* Trigger KBand calibration */
		an7583_pcie_phy_kband_calibrate(pcie_phy);
	}

	an7583_pcie_phy_tx_pll_enable(pcie_phy);
}

static int an7583_pcie_phy_init(struct phy *phy)
{
	struct an7583_pcie_phy *pcie_phy = phy_get_drvdata(phy);
	u32 val;

	/* Setup Tx-Rx detection time */
	val = FIELD_PREP(PCIE_XTP_RXDET_VCM_OFF_STB_T_SEL, 0x33) |
	      FIELD_PREP(PCIE_XTP_RXDET_EN_STB_T_SEL, 0x1) |
	      FIELD_PREP(PCIE_XTP_RXDET_FINISH_STB_T_SEL, 0x2) |
	      FIELD_PREP(PCIE_XTP_TXPD_TX_DATA_EN_DLY, 0x3) |
	      FIELD_PREP(PCIE_XTP_RXDET_LATCH_STB_T_SEL, 0x1);
	writel(val, pcie_phy->xr_dtime + REG_PCIE_PEXTP_DIG_GLB44);

	/* Setup Rx AEQ training time */
	val = FIELD_PREP(PCIE_XTP_LN_RX_PDOWN_L1P2_EXIT_WAIT, 0x32) |
	      FIELD_PREP(PCIE_XTP_LN_RX_PDOWN_E0_AEQEN_WAIT, 0x5050);
	writel(val, pcie_phy->rx_aeq + REG_PCIE_PEXTP_DIG_LN_RX30);

	an7583_pcie_phy_init_default(pcie_phy);
	an7583_pcie_phy_init_clk_out(pcie_phy);
	an7583_pcie_phy_init_ana(pcie_phy);

	usleep_range(50, 100);

	an7583_pcie_phy_init_rx(pcie_phy);

	/* phase 1, no ssc for K TXPLL */
	an7583_pcie_phy_init_jcpll(pcie_phy);

	usleep_range(200, 300);

	/* TX PLL settings */
	an7583_pcie_phy_txpll(pcie_phy);

	usleep_range(200, 300);

	/* SSC JCPLL setting */
	an7583_pcie_phy_init_ssc_jcpll(pcie_phy);

	usleep_range(30, 130);

	/* Rx lan0 signal detect */
	an7583_pcie_phy_set_rxlan0_signal_detect(pcie_phy);

	/* RX FLOW */
	an7583_pcie_phy_set_rxflow(pcie_phy);

	usleep_range(50, 200);

	an7583_pcie_phy_set_pr(pcie_phy);

	/* TX FLOW */
	an7583_pcie_phy_set_txflow(pcie_phy);

	usleep_range(50, 200);

	/* RX mode setting */
	an7583_pcie_phy_set_rx_mode(pcie_phy);

	an7583_pcie_phy_set_eye_scan(pcie_phy);

	usleep_range(50, 200);

	airoha_g3_pma_set_bits(pcie_phy, REG_SS_DA_XPON_PWDB_0,
			       G3_PMA_DA_XPON_CDR_PR_PWDB);

	usleep_range(100, 200);

	an7583_pcie_phy_init_qphy(pcie_phy);

	/* Wait for the PCIe PHY to complete initialization before returning */
	msleep(PHY_HW_INIT_TIME_MS);

	return 0;
}

static const struct phy_ops an7583_pcie_phy_ops = {
	.init = an7583_pcie_phy_init,
	.owner = THIS_MODULE,
};

static int an7583_pcie_phy_probe(struct platform_device *pdev)
{
	struct an7583_pcie_phy *pcie_phy;
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;

	pcie_phy = devm_kzalloc(dev, sizeof(*pcie_phy), GFP_KERNEL);
	if (!pcie_phy)
		return -ENOMEM;

	pcie_phy->g3_ana = devm_platform_ioremap_resource_byname(pdev, "g3-ana");
	if (IS_ERR(pcie_phy->g3_ana))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->g3_ana),
				     "Failed to map g3 ANA base\n");

	pcie_phy->g3_pma = devm_platform_ioremap_resource_byname(pdev, "g3-pma");
	if (IS_ERR(pcie_phy->g3_pma))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->g3_pma),
				     "Failed to map g3 PMA base\n");

	pcie_phy->qp_ana = devm_platform_ioremap_resource_byname(pdev, "qp-ana");
	if (IS_ERR(pcie_phy->qp_ana))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->qp_ana),
				     "Failed to map QP ANA base\n");

	pcie_phy->qp_pma = devm_platform_ioremap_resource_byname(pdev, "qp-pma");
	if (IS_ERR(pcie_phy->qp_pma))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->qp_pma),
				     "Failed to map QP PMA base\n");

	pcie_phy->qp_dig = devm_platform_ioremap_resource_byname(pdev, "qp-dig");
	if (IS_ERR(pcie_phy->qp_dig))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->qp_dig),
				     "Failed to map QP Dig base\n");

	pcie_phy->phy = devm_phy_create(dev, dev->of_node, &an7583_pcie_phy_ops);
	if (IS_ERR(pcie_phy->phy))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->phy),
				     "Failed to create PCIe phy\n");

	pcie_phy->xr_dtime =
		devm_platform_ioremap_resource_byname(pdev, "xr-dtime");
	if (IS_ERR(pcie_phy->xr_dtime))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->xr_dtime),
				     "Failed to map Tx-Rx dtime base\n");

	pcie_phy->rx_aeq = devm_platform_ioremap_resource_byname(pdev, "rx-aeq");
	if (IS_ERR(pcie_phy->rx_aeq))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->rx_aeq),
				     "Failed to map Rx AEQ base\n");

	pcie_phy->dev = dev;
	phy_set_drvdata(pcie_phy->phy, pcie_phy);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider),
				     "PCIe phy probe failed\n");

	return 0;
}

static const struct of_device_id an7583_pcie_phy_of_match[] = {
	{ .compatible = "airoha,an7583-pcie-phy" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, an7583_pcie_phy_of_match);

static struct platform_driver an7583_pcie_phy_driver = {
	.probe	= an7583_pcie_phy_probe,
	.driver	= {
		.name = "airoha-an7583-pcie-phy",
		.of_match_table = an7583_pcie_phy_of_match,
	},
};
module_platform_driver(an7583_pcie_phy_driver);

MODULE_DESCRIPTION("Airoha AN7583 PCIe PHY driver");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_LICENSE("GPL");
