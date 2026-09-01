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
#include <linux/regmap.h>
#include <linux/slab.h>

#include "phy-an7583-pcie-regs.h"

#define FREQ_LOCK_MAX_ATTEMPT	50

/* PCIe-PHY initialization time in ms needed by the hw to complete */
#define PHY_HW_INIT_TIME_MS	30

struct an7583_pcie_phy {
	struct device *dev;
	struct phy *phy;
	struct phy *qp_phy;

	struct regmap *g3_ana;
	struct regmap *g3_pma;

	struct regmap  *xr_dtime;
	struct regmap  *rx_aeq;
};

static void an7583_pcie_phy_init_default(struct an7583_pcie_phy *pcie_phy)
{
	/* Load E-Fuse */
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CMN_EN,
			   G3_ANA_PXP_CMN_TRIM,
			   FIELD_PREP(G3_ANA_PXP_CMN_TRIM, 0x10));
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TX_TERM_SEL,
			G3_PMA_FORCE_SEL_DA_PXP_TX_TERM_SEL);
	regmap_update_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TX_TERM_SEL,
			   G3_PMA_FORCE_DA_PXP_TX_TERM_SEL,
			   FIELD_PREP(G3_PMA_FORCE_DA_PXP_TX_TERM_SEL, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_SIGDET_NOVTH,
			   G3_ANA_PXP_RX_FE_50OHMS_SEL,
			   FIELD_PREP(G3_ANA_PXP_RX_FE_50OHMS_SEL, 0x1));

	/* Default Init */
	regmap_set_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_12,
			G3_PMA_DIG_RESERVE_12_8_8);
	regmap_clear_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_27,
			  G3_PMA_DIG_RESERVE_27_16_16);
	regmap_write(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_34, 0xcccbcccb);
	regmap_write(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_35, 0xcccb);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_CMN_EN,
			G3_ANA_PXP_CMN_EN);
}

static void an7583_pcie_phy_init_clk_out(struct an7583_pcie_phy *pcie_phy)
{
	regmap_update_bits(pcie_phy->g3_ana, REG_PCIE_CLKTX0_AMP,
			   G3_ANA_PCIE_CLKTX0_AMP,
			   FIELD_PREP(G3_ANA_PCIE_CLKTX0_AMP, 0x5));
	regmap_update_bits(pcie_phy->g3_ana, REG_PCIE_CLKTX0_AMP,
			   G3_ANA_PCIE_CLKTX0_OFFSET,
			   FIELD_PREP(G3_ANA_PCIE_CLKTX0_OFFSET, 0x2));
	regmap_clear_bits(pcie_phy->g3_ana, REG_PCIE_CLKTX0_AMP,
			  G3_ANA_PXP_PCIE_CLKTX0_HZ);
	regmap_update_bits(pcie_phy->g3_ana, REG_PCIE_CLKTX0_AMP,
			   G3_ANA_PXP_PCIE_CLKTX0_IMP_SEL,
			   FIELD_PREP(G3_ANA_PXP_PCIE_CLKTX0_IMP_SEL, 0x12));
	regmap_update_bits(pcie_phy->g3_ana, REG_PCIE_CLKTX0_AMP,
			   G3_ANA_PCIE_CLKTX0_SR,
			   FIELD_PREP(G3_ANA_PCIE_CLKTX0_SR, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_PLL_MONCLK_SEL,
			   G3_ANA_PXP_PLL_CMN_RESERVE0,
			   FIELD_PREP(G3_ANA_PXP_PLL_CMN_RESERVE0, 0xd));
}

static void an7583_pcie_phy_init_ana(struct an7583_pcie_phy *pcie_phy)
{
	regmap_set_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			G3_PMA_SW_XFI_RXPCS_RST_N);
	regmap_set_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			G3_PMA_SW_REF_RST_N);
	regmap_set_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			G3_PMA_SW_RX_RST_N);
	regmap_set_bits(pcie_phy->g3_pma, REG_SS_TX_RST_B,
			G3_PMA_TXCALIB_RST_B);
	regmap_set_bits(pcie_phy->g3_pma, REG_SS_TX_RST_B,
			G3_PMA_TX_TOP_RST_B);
}

static void an7583_pcie_phy_init_rx(struct an7583_pcie_phy *pcie_phy)
{
	regmap_write(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_30, 0x2a00090b);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_MONPR_EN,
			G3_ANA_PXP_CDR_PR_XFICK_EN);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_CDR_PD_PICAL_CKD8_INV,
			  G3_ANA_PXP_CDR_PD_EDGE_DIS);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_PHYCK_DIV,
			   G3_ANA_PXP_RX_PHYCK_SEL,
			   FIELD_PREP(G3_ANA_PXP_RX_PHYCK_SEL, 0x1));
}

static void an7583_pcie_phy_init_jcpll(struct an7583_pcie_phy *pcie_phy)
{
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			G3_PMA_FORCE_SEL_DA_PXP_JCPLL_EN);
	regmap_clear_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			  G3_PMA_FORCE_DA_PXP_JCPLL_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SPARE_H,
			   G3_ANA_PXP_JCPLL_SPARE_L,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_SPARE_L, 0x20));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_RST_DLY,
			G3_ANA_PXP_JCPLL_PLL_RSTB);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SSC_DELTA,
			   G3_ANA_PXP_JCPLL_SSC_DELTA,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_SSC_DELTA, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SSC_TRI_EN,
			   G3_ANA_PXP_JCPLL_SSC_DELTA1,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_SSC_DELTA1, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SSC_DELTA,
			   G3_ANA_PXP_JCPLL_SSC_PERIOD,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_SSC_PERIOD, 0x0));
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_VCO_TCLVAR,
			  G3_ANA_PXP_JCPLL_SSC_PHASE_INI);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SSC_TRI_EN,
			  G3_ANA_PXP_JCPLL_SSC_TRI_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_LPF_BR,
			   G3_ANA_PXP_JCPLL_LPF_BR,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_LPF_BR, 0xA));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_LPF_BR,
			   G3_ANA_PXP_JCPLL_LPF_BP,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_LPF_BP, 0xC));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_LPF_BR,
			   G3_ANA_PXP_JCPLL_LPF_BC,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_LPF_BC, 0x1F));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_LPF_BWC,
			   G3_ANA_PXP_JCPLL_LPF_BWC,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_LPF_BWC, 0x1E));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_LPF_BR,
			   G3_ANA_PXP_JCPLL_LPF_BWR,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_LPF_BWR, 0xA));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_MMD_PREDIV_MODE,
			   G3_ANA_PXP_JCPLL_MMD_PREDIV_MODE,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_MMD_PREDIV_MODE, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_MONCK_EN,
			   G3_ANA_PXP_JCPLL_REFIN_DIV,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_REFIN_DIV, 0x0));
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_RX_FE_VOS,
			G3_PMA_FORCE_SEL_DA_PXP_JCPLL_SDM_PCW);
	regmap_update_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_JCPLL_SDM_PCW,
			   G3_PMA_FORCE_DA_PXP_JCPLL_SDM_PCW,
			   FIELD_PREP(G3_PMA_FORCE_DA_PXP_JCPLL_SDM_PCW, 0x50000000));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_MMD_PREDIV_MODE,
			G3_ANA_PXP_JCPLL_POSTDIV_D5);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_MMD_PREDIV_MODE,
			G3_ANA_PXP_JCPLL_POSTDIV_D2);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_RST_DLY,
			   G3_ANA_PXP_JCPLL_RST_DLY,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_RST_DLY, 0x4));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_RST_DLY,
			   G3_ANA_PXP_JCPLL_SDM_DI_LS,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_SDM_DI_LS, 0x0));
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_FREQ_MEAS_EN,
			  G3_ANA_PXP_JCPLL_VCO_KBAND_MEAS_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_IB_EXT_EN,
			   G3_ANA_PXP_JCPLL_CHP_IOFST,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_CHP_IOFST, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_IB_EXT_EN,
			   G3_ANA_PXP_JCPLL_CHP_IBIAS,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_CHP_IBIAS, 0xC));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_MMD_PREDIV_MODE,
			   G3_ANA_PXP_JCPLL_MMD_PREDIV_MODE,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_MMD_PREDIV_MODE, 0x1));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_VCODIV,
			G3_ANA_PXP_JCPLL_VCO_HALFLSB_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_VCODIV,
			   G3_ANA_PXP_JCPLL_VCO_CFIX,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_VCO_CFIX, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_VCODIV,
			   G3_ANA_PXP_JCPLL_VCO_SCAPWR,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_VCO_SCAPWR, 0x4));
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_IB_EXT_EN,
			  G3_ANA_PXP_JCPLL_LPF_SHCK_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_KBAND_KFC,
			G3_ANA_PXP_JCPLL_POSTDIV_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_KBAND_KFC,
			   G3_ANA_PXP_JCPLL_KBAND_KFC,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_KBAND_KFC, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_KBAND_KFC,
			   G3_ANA_PXP_JCPLL_KBAND_KF,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_KBAND_KF, 0x3));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_KBAND_KFC,
			   G3_ANA_PXP_JCPLL_KBAND_KS,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_KBAND_KS, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_LPF_BWC,
			   G3_ANA_PXP_JCPLL_KBAND_DIV,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_KBAND_DIV, 0x1));
	regmap_set_bits(pcie_phy->g3_pma, REG_SCAN_MODE,
			G3_PMA_FORCE_SEL_DA_PXP_JCPLL_KBAND_LOAD_EN);
	regmap_clear_bits(pcie_phy->g3_pma, REG_SCAN_MODE,
			  G3_PMA_FORCE_DA_PXP_JCPLL_KBAND_LOAD_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_LPF_BWC,
			   G3_ANA_PXP_JCPLL_KBAND_CODE,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_KBAND_CODE, 0xE4));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SDM_HREN,
			G3_ANA_PXP_JCPLL_TCL_AMP_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_TCL_CMP_EN,
			G3_ANA_PXP_JCPLL_TCL_LPF_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SPARE_H,
			   G3_ANA_PXP_JCPLL_TCL_KBAND_VREF,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_TCL_KBAND_VREF, 0xF));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SDM_HREN,
			   G3_ANA_PXP_JCPLL_TCL_AMP_GAIN,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_TCL_AMP_GAIN, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SDM_HREN,
			   G3_ANA_PXP_JCPLL_TCL_AMP_VREF,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_TCL_AMP_VREF, 0x5));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_TCL_CMP_EN,
			   G3_ANA_PXP_JCPLL_TCL_LPF_BW,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_TCL_LPF_BW, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_VCO_TCLVAR,
			   G3_ANA_PXP_JCPLL_VCO_TCLVAR,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_VCO_TCLVAR, 0x3));
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			G3_PMA_FORCE_SEL_DA_PXP_JCPLL_CKOUT_EN);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			G3_PMA_FORCE_DA_PXP_JCPLL_CKOUT_EN);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			G3_PMA_FORCE_SEL_DA_PXP_JCPLL_EN);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			G3_PMA_FORCE_DA_PXP_JCPLL_EN);
}

static void an7583_pcie_phy_txpll(struct an7583_pcie_phy *pcie_phy)
{
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			G3_PMA_FORCE_SEL_DA_PXP_TXPLL_EN);
	regmap_clear_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			  G3_PMA_FORCE_DA_PXP_TXPLL_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_REFIN_INTERNAL,
			G3_ANA_PXP_TXPLL_PLL_RSTB);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SSC_DELTA1,
			   G3_ANA_PXP_TXPLL_SSC_DELTA,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_SSC_DELTA, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SSC_DELTA1,
			   G3_ANA_PXP_TXPLL_SSC_DELTA1,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_SSC_DELTA1, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SSC_PERIOD,
			   G3_ANA_PXP_TXPLL_SSC_PERIOD,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_SSC_PERIOD, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_CHP_IBIAS,
			   G3_ANA_PXP_TXPLL_CHP_IOFST,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_CHP_IOFST, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_CHP_IBIAS,
			   G3_ANA_PXP_TXPLL_CHP_IBIAS,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_CHP_IBIAS, 0x2D));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_REFIN_INTERNAL,
			   G3_ANA_PXP_TXPLL_REFIN_DIV,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_REFIN_DIV, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_LPF_EN,
			   G3_ANA_PXP_TXPLL_VCO_CFIX,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_VCO_CFIX, 0x3));
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_CDR_PR_IDAC,
			G3_PMA_FORCE_SEL_DA_PXP_TXPLL_SDM_PCW);
	regmap_update_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TXPLL_SDM_PCW,
			   G3_PMA_FORCE_DA_PXP_TXPLL_SDM_PCW,
			   FIELD_PREP(G3_PMA_FORCE_DA_PXP_TXPLL_SDM_PCW, 0xC800000));
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SDM_DI_EN,
			  G3_ANA_PXP_TXPLL_SDM_IFM);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SSC_EN,
			  G3_ANA_PXP_TXPLL_SSC_PHASE_INI);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_REFIN_INTERNAL,
			   G3_ANA_PXP_TXPLL_RST_DLY,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_RST_DLY, 0x4));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SDM_DI_EN,
			   G3_ANA_PXP_TXPLL_SDM_DI_LS,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_SDM_DI_LS, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SDM_ORD,
			   G3_ANA_PXP_TXPLL_SDM_ORD,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_SDM_ORD, 0x3));
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_KBAND_VREF,
			  G3_ANA_PXP_TXPLL_VCO_KBAND_MEAS_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SSC_DELTA1,
			   G3_ANA_PXP_TXPLL_SSC_DELTA,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_SSC_DELTA, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SSC_DELTA1,
			   G3_ANA_PXP_TXPLL_SSC_DELTA1,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_SSC_DELTA1, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_LPF_BP,
			   G3_ANA_PXP_TXPLL_LPF_BP,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_LPF_BP, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_CHP_IBIAS,
			   G3_ANA_PXP_TXPLL_LPF_BC,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_LPF_BC, 0x18));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_CHP_IBIAS,
			   G3_ANA_PXP_TXPLL_LPF_BR,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_LPF_BR, 0x5));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_CHP_IBIAS,
			   G3_ANA_PXP_TXPLL_CHP_IOFST,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_CHP_IOFST, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_CHP_IBIAS,
			   G3_ANA_PXP_TXPLL_CHP_IBIAS,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_CHP_IBIAS, 0x2D));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_VTP_EN,
			   G3_ANA_PXP_TXPLL_SPARE_L,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_SPARE_L, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_LPF_BP,
			   G3_ANA_PXP_TXPLL_LPF_BWC,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_LPF_BWC, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_KBAND_KS,
			   G3_ANA_PXP_TXPLL_MMD_PREDIV_MODE,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_MMD_PREDIV_MODE, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_REFIN_INTERNAL,
			   G3_ANA_PXP_TXPLL_REFIN_DIV,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_REFIN_DIV, 0x0));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_VCO_HALFLSB_EN,
			G3_ANA_PXP_TXPLL_VCO_HALFLSB_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_VCO_HALFLSB_EN,
			   G3_ANA_PXP_TXPLL_VCO_SCAPWR,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_VCO_SCAPWR, 0x7));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_LPF_EN,
			   G3_ANA_PXP_TXPLL_VCO_CFIX,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_VCO_CFIX, 0x3));
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_CDR_PR_IDAC,
			G3_PMA_FORCE_SEL_DA_PXP_TXPLL_SDM_PCW);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SSC_EN,
			  G3_ANA_PXP_TXPLL_SSC_PHASE_INI);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_LPF_BP,
			   G3_ANA_PXP_TXPLL_LPF_BWR,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_LPF_BWR, 0x0));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_REFIN_INTERNAL,
			G3_ANA_PXP_TXPLL_REFIN_INTERNAL);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_KBAND_VREF,
			  G3_ANA_PXP_TXPLL_VCO_KBAND_MEAS_EN);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_VTP_EN,
			  G3_ANA_PXP_TXPLL_VTP_EN);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_PHY_CK1_EN,
			  G3_ANA_PXP_TXPLL_PHY_CK1_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_REFIN_INTERNAL,
			G3_ANA_PXP_TXPLL_REFIN_INTERNAL);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SSC_EN,
			  G3_ANA_PXP_TXPLL_SSC_EN);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_FREQ_MEAS_EN,
			  G3_ANA_PXP_TXPLL_LPF_SHCK_EN);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_KBAND_KS,
			  G3_ANA_PXP_TXPLL_POSTDIV_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_KBAND_CODE,
			   G3_ANA_PXP_TXPLL_KBAND_KFC,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_KBAND_KFC, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_KBAND_CODE,
			   G3_ANA_PXP_TXPLL_KBAND_KF,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_KBAND_KF, 0x3));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_KBAND_KS,
			   G3_ANA_PXP_TXPLL_KBAND_KS,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_KBAND_KS, 0x1));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_KBAND_CODE,
			   G3_ANA_PXP_TXPLL_KBAND_DIV,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_KBAND_DIV, 0x4));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_KBAND_CODE,
			   G3_ANA_PXP_TXPLL_KBAND_CODE,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_KBAND_CODE, 0xE4));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_SDM_ORD,
			G3_ANA_PXP_TXPLL_TCL_AMP_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_LPF_EN,
			G3_ANA_PXP_TXPLL_TCL_LPF_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_KBAND_VREF,
			   G3_ANA_PXP_TXPLL_TCL_KBAND_VREF,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_TCL_KBAND_VREF, 0xF));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_AMP_GAIN,
			   G3_ANA_PXP_TXPLL_TCL_AMP_GAIN,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_TCL_AMP_GAIN, 0x3));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_AMP_GAIN,
			   G3_ANA_PXP_TXPLL_TCL_AMP_VREF,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_TCL_AMP_VREF, 0xB));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_TXPLL_TCL_LPF_EN,
			   G3_ANA_PXP_TXPLL_TCL_LPF_BW,
			   FIELD_PREP(G3_ANA_PXP_TXPLL_TCL_LPF_BW, 0x3));
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			G3_PMA_FORCE_SEL_DA_PXP_TXPLL_CKOUT_EN);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			G3_PMA_FORCE_DA_PXP_TXPLL_CKOUT_EN);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			G3_PMA_FORCE_SEL_DA_PXP_TXPLL_EN);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			G3_PMA_FORCE_DA_PXP_TXPLL_EN);
}

static void an7583_pcie_phy_init_ssc_jcpll(struct an7583_pcie_phy *pcie_phy)
{
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SSC_DELTA,
			   G3_ANA_PXP_JCPLL_SSC_DELTA,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_SSC_DELTA, 0x106));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SSC_TRI_EN,
			   G3_ANA_PXP_JCPLL_SSC_DELTA1,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_SSC_DELTA1, 0x106));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SSC_DELTA,
			   G3_ANA_PXP_JCPLL_SSC_PERIOD,
			   FIELD_PREP(G3_ANA_PXP_JCPLL_SSC_PERIOD, 0x31B));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_VCO_TCLVAR,
			G3_ANA_PXP_JCPLL_SSC_PHASE_INI);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_VCO_TCLVAR,
			G3_ANA_PXP_JCPLL_SSC_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SDM_IFM,
			G3_ANA_PXP_JCPLL_SDM_IFM);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SDM_HREN,
			G3_ANA_PXP_JCPLL_SDM_HREN);
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_RST_DLY,
			  G3_ANA_PXP_JCPLL_SDM_DI_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_JCPLL_SSC_TRI_EN,
			G3_ANA_PXP_JCPLL_SSC_TRI_EN);
}

static void
an7583_pcie_phy_set_rxlan0_signal_detect(struct an7583_pcie_phy *pcie_phy)
{
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_TDC_REF_SEL,
			G3_ANA_PXP_CDR_PR_LDO_FORCE_ON);
	usleep_range(10, 110);
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_32,
			   G3_PMA_DIG_RESERVE_32_31_16,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_32_31_16, 0x18B0));
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_33,
			   G3_PMA_DIG_RESERVE_33_15_0,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_33_15_0, 0x18B0));
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_33,
			   G3_PMA_DIG_RESERVE_33_31_16,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_33_31_16, 0x1030));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_SIGDET_NOVTH,
			   G3_ANA_PXP_RX_SIGDET_PEAK_1_0,
			   FIELD_PREP(G3_ANA_PXP_RX_SIGDET_PEAK_1_0, 0x2));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_SIGDET_NOVTH,
			   G3_ANA_PXP_RX_SIGDET_VTH_SEL_4_0,
			   FIELD_PREP(G3_ANA_PXP_RX_SIGDET_VTH_SEL_4_0, 0x5));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_REV_0,
			   G3_ANA_PXP_RX_REV_1_3_2,
			   FIELD_PREP(G3_ANA_PXP_RX_REV_1_3_2, 0x2));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_DAC_RANGE,
			   G3_ANA_PXP_RX_SIGDET_LPF_CTRL,
			   FIELD_PREP(G3_ANA_PXP_RX_SIGDET_LPF_CTRL, 0x1));
	regmap_update_bits(pcie_phy->g3_pma, REG_SS_RX_CAL_2,
			   G3_PMA_CAL_OUT_OS,
			   FIELD_PREP(G3_PMA_CAL_OUT_OS, 0x0));
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_RX_FE_VCM_GEN_PWDB,
			G3_ANA_PXP_RX_FE_VCM_GEN_PWDB);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_RX_FE_GAIN_CTRL,
			G3_PMA_FORCE_SEL_DA_PXP_RX_FE_GAIN_CTRL);
	regmap_update_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_RX_FE_GAIN_CTRL,
			   G3_PMA_FORCE_DA_PXP_RX_FE_GAIN_CTRL,
			   FIELD_PREP(G3_PMA_FORCE_DA_PXP_RX_FE_GAIN_CTRL, 0x3));
	regmap_update_bits(pcie_phy->g3_pma, REG_RX_FORCE_MODE_0,
			   G3_PMA_FORCE_DA_XPON_RX_FE_GAIN_CTRL,
			   FIELD_PREP(G3_PMA_FORCE_DA_XPON_RX_FE_GAIN_CTRL, 0x1));
	regmap_update_bits(pcie_phy->g3_pma, REG_SS_RX_SIGDET_0,
			   G3_PMA_SIGDET_WIN_NONVLD_TIMES,
			   FIELD_PREP(G3_PMA_SIGDET_WIN_NONVLD_TIMES, 0x3));
	regmap_clear_bits(pcie_phy->g3_pma, REG_RX_CTRL_SEQUENCE_DISB_CTRL_1,
			  G3_PMA_DISB_RX_SDCAL_EN);
	regmap_set_bits(pcie_phy->g3_pma, REG_RX_CTRL_SEQUENCE_FORCE_CTRL_1,
			G3_PMA_FORCE_RX_SDCAL_EN);
	usleep_range(100, 200);
	regmap_clear_bits(pcie_phy->g3_pma, REG_RX_CTRL_SEQUENCE_FORCE_CTRL_1,
			  G3_PMA_FORCE_RX_SDCAL_EN);
}

static void an7583_pcie_phy_set_rxflow(struct an7583_pcie_phy *pcie_phy)
{
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_RX_SCAN_RST_B,
			G3_PMA_FORCE_SEL_DA_PXP_RX_SIGDET_PWDB);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_RX_SCAN_RST_B,
			G3_PMA_FORCE_DA_PXP_RX_SIGDET_PWDB);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_CDR_PD_PWDB,
			G3_PMA_FORCE_SEL_DA_PXP_CDR_PD_PWDB);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_CDR_PD_PWDB,
			G3_PMA_FORCE_DA_PXP_CDR_PD_PWDB);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_RX_FE_PWDB,
			G3_PMA_FORCE_SEL_DA_PXP_RX_FE_PWDB);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_RX_FE_PWDB,
			G3_PMA_FORCE_DA_PXP_RX_FE_PWDB);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_RX_PHYCK_DIV,
			G3_ANA_PXP_RX_TDC_CK_SEL);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_RX_PHYCK_DIV,
			G3_ANA_PXP_RX_PHYCK_RSTB);
	regmap_set_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			G3_PMA_SW_TX_FIFO_RST_N);
	regmap_set_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			G3_PMA_SW_ALLPCS_RST_N);
	regmap_set_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			G3_PMA_SW_PMA_RST_N);
	regmap_set_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			G3_PMA_SW_TX_RST_N);
	regmap_set_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			G3_PMA_SW_RX_FIFO_RST_N);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_RX_FE_EQ_HZEN,
			G3_ANA_PXP_RX_FE_VB_EQ3_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_RX_FE_EQ_HZEN,
			G3_ANA_PXP_RX_FE_VB_EQ2_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_RX_FE_EQ_HZEN,
			G3_ANA_PXP_RX_FE_VB_EQ1_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_REV_0,
			   G3_ANA_PXP_RX_REV_1_6_4,
			   FIELD_PREP(G3_ANA_PXP_RX_REV_1_6_4, 0x4));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_REV_0,
			   G3_ANA_PXP_RX_REV_1_10_8,
			   FIELD_PREP(G3_ANA_PXP_RX_REV_1_10_8, 0x4));
}

static void an7583_pcie_phy_set_pr(struct an7583_pcie_phy *pcie_phy)
{
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_VREG_IBAND_VAL,
			   G3_ANA_PXP_CDR_PR_VREG_IBAND_VAL_2_0,
			   FIELD_PREP(G3_ANA_PXP_CDR_PR_VREG_IBAND_VAL_2_0, 0x5));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_VREG_IBAND_VAL,
			   G3_ANA_PXP_CDR_PR_VREG_CKBUF_VAL_2_0,
			   FIELD_PREP(G3_ANA_PXP_CDR_PR_VREG_CKBUF_VAL_2_0, 0x5));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_CKREF_DIV,
			   G3_ANA_PXP_CDR_PR_CKREF_DIV_1_0,
			   FIELD_PREP(G3_ANA_PXP_CDR_PR_CKREF_DIV_1_0, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_TDC_REF_SEL,
			   G3_ANA_PXP_CDR_PR_CKREF_DIV1_1_0,
			   FIELD_PREP(G3_ANA_PXP_CDR_PR_CKREF_DIV1_1_0, 0x0));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CDR_LPF_RATIO,
			   G3_ANA_PXP_CDR_LPF_TOP_LIM,
			   FIELD_PREP(G3_ANA_PXP_CDR_LPF_TOP_LIM, 0x20000));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_BETA_DAC,
			   G3_ANA_PXP_CDR_PR_BETA_SEL,
			   FIELD_PREP(G3_ANA_PXP_CDR_PR_BETA_SEL, 0x2));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_BETA_DAC,
			   G3_ANA_PXP_CDR_PR_KBAND_DIV,
			   FIELD_PREP(G3_ANA_PXP_CDR_PR_KBAND_DIV, 0x4));
}

static void an7583_pcie_phy_set_txflow(struct an7583_pcie_phy *pcie_phy)
{
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_TX_CKLDO_EN,
			G3_ANA_PXP_TX_CKLDO_EN);
	regmap_set_bits(pcie_phy->g3_ana, REG_PXP_TX_CKLDO_EN,
			G3_ANA_PXP_TX_DMEDGEGEN_EN);
}

static void an7583_pcie_phy_set_rx_mode(struct an7583_pcie_phy *pcie_phy)
{
	regmap_write(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_40, 0x804000);
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_31,
			   G3_PMA_DIG_RESERVE_31_4_0,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_31_4_0, 0x5));
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_31,
			   G3_PMA_DIG_RESERVE_31_12_8,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_31_12_8, 0x5));
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_31,
			   G3_PMA_DIG_RESERVE_31_20_16,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_31_20_16, 0x5));
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_43,
			   G3_PMA_DIG_RESERVE_43_10_8,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_43_10_8, 0x7));
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_43,
			   G3_PMA_DIG_RESERVE_43_14_12,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_43_14_12, 0x7));
	regmap_update_bits(pcie_phy->g3_pma, REG_ADD_DIG_RESERVE_43,
			   G3_PMA_DIG_RESERVE_43_18_16,
			   FIELD_PREP(G3_PMA_DIG_RESERVE_43_18_16, 0x7));
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_MONCK_EN,
			  G3_ANA_PXP_CDR_PR_MONCK_EN);
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_CDR_PR_MONCK_EN,
			   G3_ANA_PXP_CDR_PR_RESERVE0,
			   FIELD_PREP(G3_ANA_PXP_CDR_PR_RESERVE0, 0x2));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_OSCAL_CTLE2IOS,
			   G3_ANA_PXP_RX_OSCAL_VGA1IOS,
			   FIELD_PREP(G3_ANA_PXP_RX_OSCAL_VGA1IOS, 0x19));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_OSCAL_CTLE2IOS,
			   G3_ANA_PXP_RX_OSCAL_VGA1VOS,
			   FIELD_PREP(G3_ANA_PXP_RX_OSCAL_VGA1VOS, 0x19));
	regmap_update_bits(pcie_phy->g3_ana, REG_PXP_RX_OSCAL_VGA2IOS,
			   G3_ANA_PXP_RX_OSCAL_VGA2IOS,
			   FIELD_PREP(G3_ANA_PXP_RX_OSCAL_VGA2IOS, 0x14));
}

static void an7583_pcie_phy_set_eye_scan(struct an7583_pcie_phy *pcie_phy)
{
	regmap_update_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_CDR_PR_FLL_COR,
			   G3_PMA_FORCE_DA_PXP_RX_DAC_EYE,
			   FIELD_PREP(G3_PMA_FORCE_DA_PXP_RX_DAC_EYE, 0x0));
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_CDR_PR_FLL_COR,
			G3_PMA_FORCE_SEL_DA_PXP_RX_DAC_EYE);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_CDR_PR_PIEYE_PWDB,
			G3_PMA_FORCE_DA_PXP_CDR_PR_PIEYE_PWDB);
	regmap_set_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_CDR_PR_PIEYE_PWDB,
			G3_PMA_FORCE_SEL_DA_PXP_CDR_PR_PIEYE_PWDB);
	regmap_clear_bits(pcie_phy->g3_pma, REG_SS_DA_XPON_PWDB_0,
			  G3_PMA_DA_XPON_CDR_PR_PWDB);
}

static int an7583_pcie_phy_deinit(struct an7583_pcie_phy *pcie_phy)
{
	/* Disable RX CDR/PR */
	regmap_clear_bits(pcie_phy->g3_pma, REG_SS_DA_XPON_PWDB_0,
			  G3_PMA_DA_XPON_CDR_PR_PWDB);

	/* Disable TX PLL output/enable */
	regmap_clear_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_TXPLL_CKOUT_EN,
			  G3_PMA_FORCE_DA_PXP_TXPLL_CKOUT_EN |
			  G3_PMA_FORCE_SEL_DA_PXP_TXPLL_CKOUT_EN |
			  G3_PMA_FORCE_SEL_DA_PXP_TXPLL_EN |
			  G3_PMA_FORCE_DA_PXP_TXPLL_EN);

	/* Disable JCPLL output/enable */
	regmap_clear_bits(pcie_phy->g3_pma, REG_FORCE_DA_PXP_JCPLL_CKOUT_EN,
			  G3_PMA_FORCE_DA_PXP_JCPLL_CKOUT_EN |
			  G3_PMA_FORCE_SEL_DA_PXP_JCPLL_CKOUT_EN |
			  G3_PMA_FORCE_SEL_DA_PXP_JCPLL_EN |
			  G3_PMA_FORCE_DA_PXP_JCPLL_EN);

	/* Assert PHY resets */
	regmap_clear_bits(pcie_phy->g3_pma, REG_SW_RST_SET,
			  G3_PMA_SW_TX_FIFO_RST_N |
			  G3_PMA_SW_ALLPCS_RST_N |
			  G3_PMA_SW_PMA_RST_N |
			  G3_PMA_SW_TX_RST_N |
			  G3_PMA_SW_RX_FIFO_RST_N |
			  G3_PMA_SW_XFI_RXPCS_RST_N |
			  G3_PMA_SW_REF_RST_N |
			  G3_PMA_SW_RX_RST_N);

	/* Disable TX analog blocks */
	regmap_clear_bits(pcie_phy->g3_ana, REG_PXP_TX_CKLDO_EN,
			  G3_ANA_PXP_TX_CKLDO_EN |
			  G3_ANA_PXP_TX_DMEDGEGEN_EN);

	return 0;
}

static int an7583_pcie_phy_init(struct phy *phy)
{
	struct an7583_pcie_phy *pcie_phy = phy_get_drvdata(phy);
	u32 val;
	int ret;

	/* Setup Tx-Rx detection time */
	val = FIELD_PREP(PCIE_XTP_RXDET_VCM_OFF_STB_T_SEL, 0x33) |
	      FIELD_PREP(PCIE_XTP_RXDET_EN_STB_T_SEL, 0x1) |
	      FIELD_PREP(PCIE_XTP_RXDET_FINISH_STB_T_SEL, 0x2) |
	      FIELD_PREP(PCIE_XTP_TXPD_TX_DATA_EN_DLY, 0x3) |
	      FIELD_PREP(PCIE_XTP_RXDET_LATCH_STB_T_SEL, 0x1);
	regmap_write(pcie_phy->xr_dtime, REG_PCIE_PEXTP_DIG_GLB44, val);

	/* Setup Rx AEQ training time */
	val = FIELD_PREP(PCIE_XTP_LN_RX_PDOWN_L1P2_EXIT_WAIT, 0x32) |
	      FIELD_PREP(PCIE_XTP_LN_RX_PDOWN_E0_AEQEN_WAIT, 0x5050);
	regmap_write(pcie_phy->rx_aeq, REG_PCIE_PEXTP_DIG_LN_RX30, val);

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

	regmap_set_bits(pcie_phy->g3_pma, REG_SS_DA_XPON_PWDB_0,
			G3_PMA_DA_XPON_CDR_PR_PWDB);

	usleep_range(100, 200);

	ret = phy_init(pcie_phy->qp_phy);
	if (ret) {
		an7583_pcie_phy_deinit(pcie_phy);

		return dev_err_probe(pcie_phy->dev, ret, "failed to initialize QP phy\n");
	}

	/* Wait for the PCIe PHY to complete initialization before returning */
	msleep(PHY_HW_INIT_TIME_MS);

	return 0;
}

static int an7583_pcie_phy_exit(struct phy *phy)
{
	struct an7583_pcie_phy *pcie_phy = phy_get_drvdata(phy);
	int ret;

	ret = an7583_pcie_phy_deinit(pcie_phy);
	if (ret)
		return ret;

	return phy_exit(pcie_phy->qp_phy);
}

static const struct phy_ops an7583_pcie_phy_ops = {
	.init = an7583_pcie_phy_init,
	.exit = an7583_pcie_phy_exit,
	.owner = THIS_MODULE,
};

static const struct regmap_config an7583_pcie_phy_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static struct regmap *an7583_pcie_phy_regmap_init(struct platform_device *pdev,
						  const char *name)
{
	struct regmap_config config = an7583_pcie_phy_regmap_config;
	void __iomem *base;

	config.name = name;

	base = devm_platform_ioremap_resource_byname(pdev, name);
	if (IS_ERR(base))
		return ERR_CAST(base);

	return devm_regmap_init_mmio(&pdev->dev, base, &config);
}

static int an7583_pcie_phy_probe(struct platform_device *pdev)
{
	struct an7583_pcie_phy *pcie_phy;
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;

	pcie_phy = devm_kzalloc(dev, sizeof(*pcie_phy), GFP_KERNEL);
	if (!pcie_phy)
		return -ENOMEM;

	pcie_phy->g3_ana = an7583_pcie_phy_regmap_init(pdev, "g3-ana");
	if (IS_ERR(pcie_phy->g3_ana))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->g3_ana),
				     "Failed to initialize g3 ANA regmap\n");

	pcie_phy->qp_phy = devm_phy_optional_get(dev, NULL);
	if (IS_ERR(pcie_phy->qp_phy))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->qp_phy),
				     "Failed to get QP PCIe phy\n");

	pcie_phy->g3_pma = an7583_pcie_phy_regmap_init(pdev, "g3-pma");
	if (IS_ERR(pcie_phy->g3_pma))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->g3_pma),
				     "Failed to initialize g3 PMA regmap\n");

	pcie_phy->phy = devm_phy_create(dev, dev->of_node, &an7583_pcie_phy_ops);
	if (IS_ERR(pcie_phy->phy))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->phy),
				     "Failed to create PCIe phy\n");

	pcie_phy->xr_dtime = an7583_pcie_phy_regmap_init(pdev, "xr-dtime");
	if (IS_ERR(pcie_phy->xr_dtime))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->xr_dtime),
				     "Failed to initialize Tx-Rx dtime regmap\n");

	pcie_phy->rx_aeq = an7583_pcie_phy_regmap_init(pdev, "rx-aeq");
	if (IS_ERR(pcie_phy->rx_aeq))
		return dev_err_probe(dev, PTR_ERR(pcie_phy->rx_aeq),
				     "Failed to initialize Rx AEQ regmap\n");

	pcie_phy->dev = dev;
	phy_set_drvdata(pcie_phy->phy, pcie_phy);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider),
				     "PCIe phy probe failed\n");

	return 0;
}

static const struct of_device_id an7583_pcie_phy_of_match[] = {
	{ .compatible = "airoha,an7583-pcie-gen3-phy" },
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
