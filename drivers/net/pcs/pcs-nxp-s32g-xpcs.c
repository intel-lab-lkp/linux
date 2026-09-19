// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2021-2026 NXP
 *
 * S32G SerDes XPCS platform glue for the NXP SerDes xPCS shared core.
 *
 * This module no longer implements its own phylink PCS. It provides the
 * S32G specific parts the shared core (pcs-nxp-serdes-xpcs.c) consumes:
 *   - the indirect-MMIO regmap transport. The S32G flat register address
 *     0x1fXXXX is already identical to the shared core's regmap key
 *     "(devad << 16) | reg" (devad 0x1f == MDIO_MMD_VEND2 for the VR_MII
 *     page, devad 0x1f reg 0x0000 == SR_MII_CTRL/MII_BMCR), so the same
 *     regmap serves both the S32G PMA sequences below and the shared core.
 *   - the S32G PMA/PLL bring-up sequences, exported to the SerDes PHY
 *     driver which drives the ordered dual-XPCS bring-up barrier.
 *   - a struct nxp_serdes_xpcs_desc describing the S32G revision, with a
 *     compat[] table (C37 SGMII + 2500BASE-X) and two pma_config() hooks
 *     (SGMII link-timer / CL37 override, and the 1G<->2.5G PLL switch).
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pcs/pcs-nxp-s32g-xpcs.h>
#include <linux/pcs/pcs-nxp-serdes-xpcs.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/regmap.h>
#include <linux/units.h>

enum s32g_xpcs_pll {
	S32G_XPCS_PLLA,	/* Slow PLL */
	S32G_XPCS_PLLB,	/* Fast PLL */
};

struct s32g_xpcs {
	void __iomem *base;
	struct device *dev;
	unsigned char id;
	struct regmap *regmap;
	enum s32g_xpcs_pll ref;
	bool ext_clk;
	bool mhz125;
	enum s32g_xpcs_shared pcie_shared;
	/* phylink PCS owned by the shared core, created in s32g_xpcs_create() */
	struct phylink_pcs *pcs;
};

#define XPCS_TIMEOUT_US				(300 * USEC_PER_MSEC)
#define XPCS_POLL_SLEEP_US			100

#define ADDR1_OFS				0x3fc

#define SR_MII_CTRL				0x1f0000
#define   SS13					BIT(13)
#define   AN_ENABLE				BIT(12)
#define   RESTART_AN				BIT(9)
#define   DUPLEX_MODE				BIT(8)
#define   SS6					BIT(6)
#define SR_MII_STS				0x1f0001
#define   LINK_STS				BIT(2)
#define VR_MII_DIG_CTRL1			0x1f8000
#define   BYP_PWRUP				BIT(1)
#define   EN_2_5G_MODE				BIT(2)
#define   CL37_TMR_OVRRIDE			BIT(3)
#define   INIT					BIT(8)
#define   MAC_AUTO_SW				BIT(9)
#define   VR_RST				BIT(15)
#define VR_MII_AN_CTRL				0x1f8001
#define   MII_AN_INTR_EN			BIT(0)
#define   PCS_MODE_MASK				GENMASK(2, 1)
#define    PCS_MODE_SGMII			2
#define   MII_CTRL				BIT(8)
#define VR_MII_AN_INTR_STS			0x1f8002
#define  CL37_ANCMPLT_INTR			BIT(0)
#define  CL37_ANSGM_STS_DUPLEX			BIT(1)
#define  CL37_ANSGM_STS_SPEED_MASK		GENMASK(3, 2)
#define   CL37_ANSGM_10MBPS			0
#define   CL37_ANSGM_100MBPS			1
#define   CL37_ANSGM_1000MBPS			2
#define  CL37_ANSGM_STS_LINK			BIT(4)
#define VR_MII_DBG_CTRL				0x1f8005
#define   SUPPRESS_LOS_DET			BIT(4)
#define   RX_DT_EN_CTL				BIT(6)
#define VR_MII_LINK_TIMER_CTRL			0x1f800a
#define VR_MII_DIG_STS				0x1f8010
#define   PSEQ_STATE_MASK			GENMASK(4, 2)
#define     POWER_GOOD_STATE			0x4
#define VR_MII_GEN5_12G_16G_TX_GENCTRL1		0x1f8031
#define   TX_CLK_RDY_0				BIT(12)
#define	VR_MII_GEN5_12G_16G_TX_GENCTRL2		0x1f8032
#define	  TX_REQ_0				BIT(0)
#define VR_MII_GEN5_12G_16G_TX_RATE_CTRL	0x1f8034
#define   TX0_RATE_MASK				GENMASK(2, 0)
#define     TX0_BAUD_DIV_1			0
#define     TX0_BAUD_DIV_4			2
#define VR_MII_GEN5_12G_16G_TX_EQ_CTRL0		0x1f8036
#define   TX_EQ_MAIN_MASK			GENMASK(13, 8)
#define VR_MII_GEN5_12G_16G_TX_EQ_CTRL1		0x1f8037
#define   TX_EQ_OVR_RIDE			BIT(6)
#define VR_MII_CONSUMER_10G_TX_TERM_CTRL	0x1f803c
#define   TX0_TERM_MASK				GENMASK(2, 0)
#define VR_MII_GEN5_12G_16G_RX_GENCTRL1		0x1f8051
#define   RX_RST_0				BIT(4)
#define VR_MII_GEN5_12G_16G_RX_GENCTRL2		0x1f8052
#define   RX_REQ_0				BIT(0)
#define VR_MII_GEN5_12G_16G_RX_RATE_CTRL	0x1f8054
#define   RX0_RATE_MASK				GENMASK(1, 0)
#define     RX0_BAUD_DIV_2			0x1
#define     RX0_BAUD_DIV_8			0x3
#define VR_MII_GEN5_12G_16G_CDR_CTRL		0x1f8056
#define   VCO_LOW_FREQ_0			BIT(8)
#define VR_MII_GEN5_12G_16G_MPLL_CMN_CTRL	0x1f8070
#define   MPLLB_SEL_0				BIT(4)
#define VR_MII_GEN5_12G_16G_MPLLA_CTRL0		0x1f8071
#define   MPLLA_CAL_DISABLE			BIT(15)
#define   MLLA_MULTIPLIER_MASK			GENMASK(7, 0)
#define VR_MII_GEN5_12G_MPLLA_CTRL1		0x1f8072
#define   MPLLA_FRACN_CTRL_MASK			GENMASK(15, 5)
#define VR_MII_GEN5_12G_16G_MPLLA_CTRL2		0x1f8073
#define   MPLLA_TX_CLK_DIV_MASK			GENMASK(13, 11)
#define   MPLLA_DIV10_CLK_EN			BIT(9)
#define VR_MII_GEN5_12G_16G_MPLLB_CTRL0		0x1f8074
#define   MPLLB_CAL_DISABLE			BIT(15)
#define   MLLB_MULTIPLIER_MASK			GENMASK(7, 0)
#define VR_MII_GEN5_12G_MPLLB_CTRL1		0x1f8075
#define   MPLLB_FRACN_CTRL_MASK			GENMASK(15, 5)
#define VR_MII_GEN5_12G_16G_MPLLB_CTRL2		0x1f8076
#define   MPLLB_TX_CLK_DIV_MASK			GENMASK(13, 11)
#define   MPLLB_DIV10_CLK_EN			BIT(9)
#define VR_MII_GEN5_12G_MPLLA_CTRL3		0x1f8077
#define   MPLLA_BANDWIDTH_MASK			GENMASK(15, 0)
#define VR_MII_GEN5_12G_MPLLB_CTRL3		0x1f8078
#define   MPLLB_BANDWIDTH_MASK			GENMASK(15, 0)
#define VR_MII_GEN5_12G_16G_MISC_CTRL0		0x1f8090
#define   PLL_CTRL				BIT(15)
#define VR_MII_GEN5_12G_16G_REF_CLK_CTRL	0x1f8091
#define   REF_USE_PAD				BIT(1)
#define   REF_CLK_DIV2				BIT(2)
#define   REF_RANGE_MASK			GENMASK(5, 3)
#define     RANGE_26_53_MHZ			0x1
#define     RANGE_52_78_MHZ			0x2
#define   REF_MPLLA_DIV2			BIT(6)
#define   REF_MPLLB_DIV2			BIT(7)
#define VR_MII_GEN5_12G_16G_VCO_CAL_LD0		0x1f8092
#define   VCO_LD_VAL_0_MASK			GENMASK(12, 0)
#define VR_MII_GEN5_12G_VCO_CAL_REF0		0x1f8096
#define   VCO_REF_LD_0_MASK			GENMASK(5, 0)

typedef bool (*xpcs_poll_func_t)(struct s32g_xpcs *);

/*
 * XPCS registers can't be accessed directly and an indirect address method
 * must be used instead. The 32-bit "reg" is the flat XPCS address and is
 * identical to the shared core's "(devad << 16) | reg" key.
 */

static const struct regmap_range s32g_xpcs_wr_ranges[] = {
	regmap_reg_range(0x1f0000, 0x1f0000),
	regmap_reg_range(0x1f0004, 0x1f0004),
	regmap_reg_range(0x1f8000, 0x1f8003),
	regmap_reg_range(0x1f8005, 0x1f8005),
	regmap_reg_range(0x1f800a, 0x1f800a),
	regmap_reg_range(0x1f8012, 0x1f8012),
	regmap_reg_range(0x1f8015, 0x1f8015),
	regmap_reg_range(0x1f8030, 0x1f8037),
	regmap_reg_range(0x1f803c, 0x1f803e),
	regmap_reg_range(0x1f8050, 0x1f8058),
	regmap_reg_range(0x1f805c, 0x1f805e),
	regmap_reg_range(0x1f8064, 0x1f8064),
	regmap_reg_range(0x1f806b, 0x1f806b),
	regmap_reg_range(0x1f8070, 0x1f8078),
	regmap_reg_range(0x1f8090, 0x1f8092),
	regmap_reg_range(0x1f8096, 0x1f8096),
	regmap_reg_range(0x1f8099, 0x1f8099),
	regmap_reg_range(0x1f80a0, 0x1f80a2),
	regmap_reg_range(0x1f80e1, 0x1f80e1),
};

static const struct regmap_access_table s32g_xpcs_wr_table = {
	.yes_ranges = s32g_xpcs_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g_xpcs_wr_ranges),
};

static const struct regmap_range s32g_xpcs_rd_ranges[] = {
	regmap_reg_range(0x1f0000, 0x1f0006),
	regmap_reg_range(0x1f000f, 0x1f000f),
	regmap_reg_range(0x1f0708, 0x1f0710),
	regmap_reg_range(0x1f8000, 0x1f8003),
	regmap_reg_range(0x1f8005, 0x1f8005),
	regmap_reg_range(0x1f800a, 0x1f800a),
	regmap_reg_range(0x1f8010, 0x1f8012),
	regmap_reg_range(0x1f8015, 0x1f8015),
	regmap_reg_range(0x1f8018, 0x1f8018),
	regmap_reg_range(0x1f8020, 0x1f8020),
	regmap_reg_range(0x1f8030, 0x1f8037),
	regmap_reg_range(0x1f803c, 0x1f803c),
	regmap_reg_range(0x1f8040, 0x1f8040),
	regmap_reg_range(0x1f8050, 0x1f8058),
	regmap_reg_range(0x1f805c, 0x1f805e),
	regmap_reg_range(0x1f8060, 0x1f8060),
	regmap_reg_range(0x1f8064, 0x1f8064),
	regmap_reg_range(0x1f806b, 0x1f806b),
	regmap_reg_range(0x1f8070, 0x1f8078),
	regmap_reg_range(0x1f8090, 0x1f8092),
	regmap_reg_range(0x1f8096, 0x1f8096),
	regmap_reg_range(0x1f8098, 0x1f8099),
	regmap_reg_range(0x1f80a0, 0x1f80a2),
	regmap_reg_range(0x1f80e1, 0x1f80e1),
};

static const struct regmap_access_table s32g_xpcs_rd_table = {
	.yes_ranges = s32g_xpcs_rd_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g_xpcs_rd_ranges),
};

static int s32g_xpcs_regmap_reg_read(void *context, unsigned int reg,
				     unsigned int *result)
{
	struct s32g_xpcs *xpcs = context;
	u16 ofsleft = (reg >> 8) & 0xffffU;
	u16 ofsright = (reg & 0xffU);

	writew(ofsleft, xpcs->base + ADDR1_OFS);
	*result = readw(xpcs->base + (ofsright * 4));

	return 0;
}

static int s32g_xpcs_regmap_reg_write(void *context, unsigned int reg,
				      unsigned int val)
{
	struct s32g_xpcs *xpcs = context;
	u16 ofsleft = (reg >> 8) & 0xffffU;
	u16 ofsright = (reg & 0xffU);

	writew(ofsleft, xpcs->base + ADDR1_OFS);
	writew(val, xpcs->base + (ofsright * 4));

	return 0;
}

static const struct regmap_config s32g_xpcs0_regmap_config = {
	.reg_bits = 32,
	.val_bits = 16,
	.reg_read = s32g_xpcs_regmap_reg_read,
	.reg_write = s32g_xpcs_regmap_reg_write,
	.wr_table = &s32g_xpcs_wr_table,
	.rd_table = &s32g_xpcs_rd_table,
	.max_register = 0x1f80e1,
	.name = "xpcs0",
};

static const struct regmap_config s32g_xpcs1_regmap_config = {
	.reg_bits = 32,
	.val_bits = 16,
	.reg_read = s32g_xpcs_regmap_reg_read,
	.reg_write = s32g_xpcs_regmap_reg_write,
	.wr_table = &s32g_xpcs_wr_table,
	.rd_table = &s32g_xpcs_rd_table,
	.max_register = 0x1f80e1,
	.name = "xpcs1",
};

static void s32g_xpcs_write_bits(struct s32g_xpcs *xpcs, unsigned int reg,
				 unsigned int mask, unsigned int value)
{
	int ret = regmap_write_bits(xpcs->regmap, reg, mask, value);

	if (ret)
		dev_err(xpcs->dev, "Failed to write bits of XPCS reg: 0x%x\n", reg);
}

static void s32g_xpcs_write(struct s32g_xpcs *xpcs, unsigned int reg,
			    unsigned int value)
{
	int ret = regmap_write(xpcs->regmap, reg, value);

	if (ret)
		dev_err(xpcs->dev, "Failed to write XPCS reg: 0x%x\n", reg);
}

static unsigned int s32g_xpcs_read(struct s32g_xpcs *xpcs, unsigned int reg)
{
	unsigned int val = 0;
	int ret;

	ret = regmap_read(xpcs->regmap, reg, &val);
	if (ret)
		dev_err(xpcs->dev, "Failed to read XPCS reg: 0x%x\n", reg);

	return val;
}

/*
 * Internal XPCS function
 */

static int s32g_xpcs_wait(struct s32g_xpcs *xpcs, xpcs_poll_func_t func)
{
	bool val;

	return read_poll_timeout(func, val, val, XPCS_POLL_SLEEP_US,
				 XPCS_TIMEOUT_US, false, xpcs);
}

static int s32g_xpcs_wait_bits(struct s32g_xpcs *xpcs, unsigned int reg,
			       unsigned int mask, unsigned int bits)
{
	unsigned int val;

	return read_poll_timeout(s32g_xpcs_read, val, (val & mask) == bits,
				 XPCS_POLL_SLEEP_US, XPCS_TIMEOUT_US, false,
				 xpcs, reg);
}

static unsigned int s32g_xpcs_digital_status(struct s32g_xpcs *xpcs)
{
	return s32g_xpcs_read(xpcs, VR_MII_DIG_STS);
}

static int s32g_xpcs_wait_power_good_state(struct s32g_xpcs *xpcs)
{
	unsigned int val;

	return read_poll_timeout(s32g_xpcs_digital_status, val,
				 FIELD_GET(PSEQ_STATE_MASK, val) == POWER_GOOD_STATE,
				 XPCS_POLL_SLEEP_US,
				 XPCS_TIMEOUT_US, false, xpcs);
}

void s32g_xpcs_vreset(struct s32g_xpcs *xpcs)
{
	/* Step 19 */
	s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, VR_RST, VR_RST);
}
EXPORT_SYMBOL_GPL(s32g_xpcs_vreset);

static bool s32g_xpcs_is_not_in_reset(struct s32g_xpcs *xpcs)
{
	unsigned int val;

	val = s32g_xpcs_read(xpcs, VR_MII_DIG_CTRL1);

	return !(val & VR_RST);
}

int s32g_xpcs_wait_vreset(struct s32g_xpcs *xpcs)
{
	int ret;

	/* Step 20 */
	ret = s32g_xpcs_wait(xpcs, s32g_xpcs_is_not_in_reset);
	if (ret)
		dev_err(xpcs->dev, "XPCS%d is in reset\n", xpcs->id);

	return ret;
}
EXPORT_SYMBOL_GPL(s32g_xpcs_wait_vreset);

int s32g_xpcs_reset_rx(struct s32g_xpcs *xpcs)
{
	int ret = 0;

	/*
	 * On the shared 1G combo lane the PMA power-up is gated by the PCIe
	 * side and BYP_PWRUP stays set, so the power sequencer never reaches
	 * POWER_GOOD on its own and the wait would always time out.
	 */
	if (xpcs->pcie_shared != S32G_PCIE_XPCS_1G) {
		ret = s32g_xpcs_wait_power_good_state(xpcs);
		if (ret) {
			dev_err(xpcs->dev, "Failed to enter in PGOOD state after vendor reset\n");
			return ret;
		}
	}

	/* Step 21 */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_RX_GENCTRL1,
			     RX_RST_0, RX_RST_0);

	/* Step 22 */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_RX_GENCTRL1,
			     RX_RST_0, 0);

	/* Step 23 */
	/* Wait until SR_MII_STS[LINK_STS] = 1 */

	return ret;
}
EXPORT_SYMBOL_GPL(s32g_xpcs_reset_rx);

static int s32g_xpcs_ref_clk_sel(struct s32g_xpcs *xpcs,
				 enum s32g_xpcs_pll ref_pll)
{
	switch (ref_pll) {
	case S32G_XPCS_PLLA:
		s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MPLL_CMN_CTRL,
				     MPLLB_SEL_0, 0);
		xpcs->ref = S32G_XPCS_PLLA;
		break;
	case S32G_XPCS_PLLB:
		s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MPLL_CMN_CTRL,
				     MPLLB_SEL_0, MPLLB_SEL_0);
		xpcs->ref = S32G_XPCS_PLLB;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void s32g_xpcs_electrical_configure(struct s32g_xpcs *xpcs)
{
	/* Step 2 */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_EQ_CTRL0,
			     TX_EQ_MAIN_MASK, FIELD_PREP(TX_EQ_MAIN_MASK, 0xc));

	/* Step 3 */
	s32g_xpcs_write_bits(xpcs, VR_MII_CONSUMER_10G_TX_TERM_CTRL,
			     TX0_TERM_MASK, FIELD_PREP(TX0_TERM_MASK, 0x4));
}

static int s32g_xpcs_vco_cfg(struct s32g_xpcs *xpcs, enum s32g_xpcs_pll vco_pll)
{
	unsigned int vco_ld = 0;
	unsigned int vco_ref = 0;
	unsigned int rx_baud = 0;
	unsigned int tx_baud = 0;

	switch (vco_pll) {
	case S32G_XPCS_PLLA:
		if (xpcs->mhz125) {
			vco_ld = FIELD_PREP(VCO_LD_VAL_0_MASK, 1360);
			vco_ref = FIELD_PREP(VCO_REF_LD_0_MASK, 17);
		} else {
			vco_ld = FIELD_PREP(VCO_LD_VAL_0_MASK, 1350);
			vco_ref = FIELD_PREP(VCO_REF_LD_0_MASK, 27);
		}

		rx_baud = FIELD_PREP(RX0_RATE_MASK, RX0_BAUD_DIV_8);
		tx_baud = FIELD_PREP(TX0_RATE_MASK, TX0_BAUD_DIV_4);
		break;
	case S32G_XPCS_PLLB:
		if (xpcs->mhz125) {
			vco_ld = FIELD_PREP(VCO_LD_VAL_0_MASK, 1350);
			vco_ref = FIELD_PREP(VCO_REF_LD_0_MASK, 27);
		} else {
			vco_ld = FIELD_PREP(VCO_LD_VAL_0_MASK, 1344);
			vco_ref = FIELD_PREP(VCO_REF_LD_0_MASK, 43);
		}

		rx_baud = FIELD_PREP(RX0_RATE_MASK, RX0_BAUD_DIV_2);
		tx_baud = FIELD_PREP(TX0_RATE_MASK, TX0_BAUD_DIV_1);
		break;
	default:
		return -EINVAL;
	}

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_VCO_CAL_LD0,
			     VCO_LD_VAL_0_MASK, vco_ld);

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_VCO_CAL_REF0,
			     VCO_REF_LD_0_MASK, vco_ref);

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_RATE_CTRL,
			     TX0_RATE_MASK, tx_baud);
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_RX_RATE_CTRL,
			     RX0_RATE_MASK, rx_baud);

	if (vco_pll == S32G_XPCS_PLLB) {
		s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_CDR_CTRL,
				     VCO_LOW_FREQ_0, VCO_LOW_FREQ_0);
	} else {
		s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_CDR_CTRL,
				     VCO_LOW_FREQ_0, 0);
	}

	return 0;
}

static void s32g_xpcs_init_mplla(struct s32g_xpcs *xpcs)
{
	unsigned int val;

	/* Step 7 */
	val = 0;
	if (xpcs->ext_clk)
		val |= REF_USE_PAD;

	if (xpcs->mhz125) {
		val |= REF_MPLLA_DIV2;
		val |= REF_CLK_DIV2;
		val |= FIELD_PREP(REF_RANGE_MASK, RANGE_52_78_MHZ);
	} else {
		/*
		 * A non-125 MHz reference is 100 MHz, which falls in the
		 * 26-53 MHz range after the internal divider, so select
		 * RANGE_26_53_MHZ.
		 */
		val |= FIELD_PREP(REF_RANGE_MASK, RANGE_26_53_MHZ);
	}

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_REF_CLK_CTRL,
			     REF_MPLLA_DIV2 | REF_USE_PAD | REF_RANGE_MASK |
			     REF_CLK_DIV2, val);

	/* Step 8 */
	if (xpcs->mhz125)
		val = FIELD_PREP(MLLA_MULTIPLIER_MASK, 80);
	else
		val = FIELD_PREP(MLLA_MULTIPLIER_MASK, 25);

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MPLLA_CTRL0,
			     MPLLA_CAL_DISABLE | MLLA_MULTIPLIER_MASK,
			     val);

	/* Step 9 */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_MPLLA_CTRL1,
			     MPLLA_FRACN_CTRL_MASK, 0);

	/* Step 10 */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MPLLA_CTRL2,
			     MPLLA_TX_CLK_DIV_MASK | MPLLA_DIV10_CLK_EN,
			     FIELD_PREP(MPLLA_TX_CLK_DIV_MASK, 1) | MPLLA_DIV10_CLK_EN);

	/* Step 11 */
	if (xpcs->mhz125)
		val = FIELD_PREP(MPLLA_BANDWIDTH_MASK, 43);
	else
		val = FIELD_PREP(MPLLA_BANDWIDTH_MASK, 357);

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_MPLLA_CTRL3,
			     MPLLA_BANDWIDTH_MASK, val);
}

static void s32g_xpcs_init_mpllb(struct s32g_xpcs *xpcs)
{
	unsigned int val;

	/* Step 7 */
	val = 0;
	if (xpcs->ext_clk)
		val |= REF_USE_PAD;

	if (xpcs->mhz125) {
		val |= REF_MPLLB_DIV2;
		val |= REF_CLK_DIV2;
		val |= FIELD_PREP(REF_RANGE_MASK, RANGE_52_78_MHZ);
	} else {
		/*
		 * REF_RANGE selects the reference-clock frequency band and is
		 * shared by both PLLs. For a 100 MHz reference this is the
		 * 26-53 MHz range after the internal divider: RANGE_26_53_MHZ.
		 */
		val |= FIELD_PREP(REF_RANGE_MASK, RANGE_26_53_MHZ);
	}

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_REF_CLK_CTRL,
			     REF_MPLLB_DIV2 | REF_USE_PAD | REF_RANGE_MASK |
			     REF_CLK_DIV2, val);

	/* Step 8 */
	if (xpcs->mhz125)
		val = FIELD_PREP(MLLB_MULTIPLIER_MASK, 125);
	else
		val = FIELD_PREP(MLLB_MULTIPLIER_MASK, 39);

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MPLLB_CTRL0,
			     MPLLB_CAL_DISABLE | MLLB_MULTIPLIER_MASK,
			     val);

	/* Step 9 */
	if (xpcs->mhz125)
		val = FIELD_PREP(MPLLB_FRACN_CTRL_MASK, 0);
	else
		val = FIELD_PREP(MPLLB_FRACN_CTRL_MASK, 1044);

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_MPLLB_CTRL1,
			     MPLLB_FRACN_CTRL_MASK, val);

	/* Step 10 */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MPLLB_CTRL2,
			     MPLLB_TX_CLK_DIV_MASK | MPLLB_DIV10_CLK_EN,
			     FIELD_PREP(MPLLB_TX_CLK_DIV_MASK, 5) | MPLLB_DIV10_CLK_EN);

	/* Step 11 */
	if (xpcs->mhz125)
		val = FIELD_PREP(MPLLB_BANDWIDTH_MASK, 68);
	else
		val = FIELD_PREP(MPLLB_BANDWIDTH_MASK, 102);

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_MPLLB_CTRL3,
			     MPLLB_BANDWIDTH_MASK, val);
}

static void s32g_serdes_pma_high_freq_recovery(struct s32g_xpcs *xpcs)
{
	/* PCS signal protection, PLL railout recovery */
	s32g_xpcs_write_bits(xpcs, VR_MII_DBG_CTRL, SUPPRESS_LOS_DET | RX_DT_EN_CTL,
			     SUPPRESS_LOS_DET | RX_DT_EN_CTL);
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MISC_CTRL0,
			     PLL_CTRL, PLL_CTRL);
}

static void s32g_serdes_pma_configure_tx_eq_post(struct s32g_xpcs *xpcs)
{
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_EQ_CTRL1,
			     TX_EQ_OVR_RIDE, TX_EQ_OVR_RIDE);
}

static int s32g_serdes_bifurcation_pll_transit(struct s32g_xpcs *xpcs,
					       enum s32g_xpcs_pll target_pll)
{
	int ret = 0;
	struct device *dev = xpcs->dev;

	/* Configure XPCS speed and VCO */
	if (target_pll == S32G_XPCS_PLLA) {
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, EN_2_5G_MODE, 0);
		s32g_xpcs_vco_cfg(xpcs, S32G_XPCS_PLLA);
	} else {
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1,
				     EN_2_5G_MODE, EN_2_5G_MODE);
		s32g_xpcs_vco_cfg(xpcs, S32G_XPCS_PLLB);
	}

	/* Signal that clock are not available */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_GENCTRL1,
			     TX_CLK_RDY_0, 0);

	/* Select PLL reference */
	if (target_pll == S32G_XPCS_PLLA)
		s32g_xpcs_ref_clk_sel(xpcs, S32G_XPCS_PLLA);
	else
		s32g_xpcs_ref_clk_sel(xpcs, S32G_XPCS_PLLB);

	/* Initiate transmitter TX reconfiguration request */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_GENCTRL2,
			     TX_REQ_0, TX_REQ_0);

	/* Wait for transmitter to reconfigure */
	ret = s32g_xpcs_wait_bits(xpcs, VR_MII_GEN5_12G_16G_TX_GENCTRL2,
				  TX_REQ_0, 0);
	if (ret) {
		dev_err(dev, "Switch to TX_REQ_0 failed\n");
		return ret;
	}

	/* Initiate transmitter RX reconfiguration request */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_RX_GENCTRL2,
			     RX_REQ_0, RX_REQ_0);

	/* Wait for receiver to reconfigure */
	ret = s32g_xpcs_wait_bits(xpcs, VR_MII_GEN5_12G_16G_RX_GENCTRL2,
				  RX_REQ_0, 0);
	if (ret) {
		dev_err(dev, "Switch to RX_REQ_0 failed\n");
		return ret;
	}

	/* Signal that clock are available */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_GENCTRL1,
			     TX_CLK_RDY_0, TX_CLK_RDY_0);

	/* Flush internal logic */
	s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, INIT, INIT);

	/* Wait for init */
	ret = s32g_xpcs_wait_bits(xpcs, VR_MII_DIG_CTRL1, INIT, 0);
	if (ret) {
		dev_err(dev, "XPCS INIT failed\n");
		return ret;
	}

	return ret;
}

/*
 * PMA hooks, run from the shared core's pcs_config() after the generic C37
 * SGMII / 2500BASE-X setup. The S32G context hangs off the shared
 * nxp_serdes_xpcs priv pointer, set in s32g_xpcs_create().
 */

static int s32g_xpcs_pma_sgmii(struct nxp_serdes_xpcs *nxpcs)
{
	struct s32g_xpcs *xpcs = nxp_serdes_xpcs_get_drvdata(nxpcs);
	int ret;

	/*
	 * The shared core sets PCS_MODE_SGMII, TX_CONFIG and MAC_AUTO_SW. The
	 * link timer differs on the PCIe-shared 2.5G lane, where
	 * CL37_TMR_OVRRIDE must be set for the override value to take effect.
	 */
	if (xpcs->pcie_shared == S32G_PCIE_XPCS_2G5) {
		s32g_xpcs_write(xpcs, VR_MII_LINK_TIMER_CTRL, 0x2faf);
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1,
				     MAC_AUTO_SW, MAC_AUTO_SW);
	} else {
		s32g_xpcs_write(xpcs, VR_MII_LINK_TIMER_CTRL, 0x7a1);

		/*
		 * Clearing EN_2_5G_MODE does not reprogram the PMA line rate:
		 * the VCO load, the baud dividers and MPLLB_SEL stay at 2.5G if
		 * the lane last came up as 2500BASE-X. A 2.5G to 1G
		 * renegotiation then links at 1000 Mbps while the PMA keeps
		 * clocking at 3.125 Gbaud and no traffic passes, so run the
		 * full transition back to PLLA. Idempotent on a fresh 1G lane.
		 */
		ret = s32g_serdes_bifurcation_pll_transit(xpcs,
							  S32G_XPCS_PLLA);
		if (ret)
			return ret;
	}

	s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1,
			     CL37_TMR_OVRRIDE, CL37_TMR_OVRRIDE);

	return 0;
}

static int s32g_xpcs_pma_2500basex(struct nxp_serdes_xpcs *nxpcs)
{
	struct s32g_xpcs *xpcs = nxp_serdes_xpcs_get_drvdata(nxpcs);

	/*
	 * The shared core has set EN_2_5G_MODE (DW_VR_MII_DIG_CTRL1_2G5_EN)
	 * via nxp_serdes_xpcs_config_2500basex(). S32G additionally needs the
	 * PMA switched from the slow PLLA to the fast PLLB and re-initialised.
	 */
	return s32g_serdes_bifurcation_pll_transit(xpcs, S32G_XPCS_PLLB);
}

static int s32g_xpcs_pma_link_up_sgmii(struct nxp_serdes_xpcs *nxpcs,
				       int speed, int duplex)
{
	struct s32g_xpcs *xpcs = nxp_serdes_xpcs_get_drvdata(nxpcs);

	/*
	 * A 2.5G SGMII fixed-link keeps interface == SGMII and never reaches
	 * the 2500BASE-X hook, so the PMA would stay on the 1G-class PLLA that
	 * pma_sgmii() programmed at config time. Move to PLLB (which also sets
	 * EN_2_5G_MODE) once the link resolves to 2.5G; below that the
	 * config-time PLLA setting is already right.
	 */
	if (speed != SPEED_2500)
		return 0;

	return s32g_serdes_bifurcation_pll_transit(xpcs, S32G_XPCS_PLLB);
}

static const struct nxp_serdes_xpcs_compat s32g_xpcs_compat[] = {
	{
		.interface = PHY_INTERFACE_MODE_SGMII,
		.supported = nxp_serdes_xpcs_sgmii_features,
		.an_mode = NXP_SERDES_AN_C37_SGMII,
		.pma_config = s32g_xpcs_pma_sgmii,
		.pma_link_up = s32g_xpcs_pma_link_up_sgmii,
	},
	{
		.interface = PHY_INTERFACE_MODE_2500BASEX,
		.supported = nxp_serdes_xpcs_2500basex_features,
		.an_mode = NXP_SERDES_2500BASEX,
		.pma_config = s32g_xpcs_pma_2500basex,
	},
	{ }
};

static const struct nxp_serdes_xpcs_desc s32g_xpcs_desc = {
	.name = "s32g-serdes-xpcs",
	.compat = s32g_xpcs_compat,
	/*
	 * S32G runs its own vendor reset (VR_RST) as part of the ordered
	 * dual-XPCS bring-up barrier in the SerDes PHY driver, so the shared
	 * core must not issue the generic BMCR soft reset. S32G3 does not
	 * implement the EEE VR_MII_EEE_MCTRL0/1 registers either.
	 */
	.quirks = NXP_SERDES_QUIRK_NO_SOFT_RESET | NXP_SERDES_QUIRK_NO_EEE,
	.reset = NULL,
};

/*
 * Serdes functions for initializing/configuring/releasing the xpcs
 */

int s32g_xpcs_init_plls(struct s32g_xpcs *xpcs)
{
	int ret;

	if (!xpcs->ext_clk) {
		/* Step 1 */
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, BYP_PWRUP, BYP_PWRUP);
	} else if (xpcs->pcie_shared == S32G_NOT_SHARED) {
		ret = s32g_xpcs_wait_power_good_state(xpcs);
		if (ret)
			return ret;
	} else if (xpcs->pcie_shared == S32G_PCIE_XPCS_2G5) {
		ret = s32g_xpcs_wait_power_good_state(xpcs);
		if (ret)
			return ret;
		/* Configure equalization */
		s32g_serdes_pma_configure_tx_eq_post(xpcs);
		s32g_xpcs_electrical_configure(xpcs);

		/* Enable receiver recover */
		s32g_serdes_pma_high_freq_recovery(xpcs);
		return 0;
	}

	s32g_xpcs_electrical_configure(xpcs);

	s32g_xpcs_ref_clk_sel(xpcs, S32G_XPCS_PLLA);
	s32g_xpcs_init_mplla(xpcs);
	s32g_xpcs_init_mpllb(xpcs);
	s32g_xpcs_vco_cfg(xpcs, S32G_XPCS_PLLA);

	/*
	 * Step 18: for an internal reference BYP_PWRUP was set in Step 1 to
	 * bypass the power-up sequencer; clear it so the PMA powers up. The
	 * shared 1G lane keeps it set - there the PCIe side drives power-up
	 * and clearing it stalls the XPCS bring-up.
	 */
	if (!xpcs->ext_clk && xpcs->pcie_shared != S32G_PCIE_XPCS_1G)
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, BYP_PWRUP, 0);

	/* Will be cleared by Step 19 Vreset */
	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, AN_ENABLE, 0);
	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, DUPLEX_MODE, DUPLEX_MODE);

	return 0;
}
EXPORT_SYMBOL_GPL(s32g_xpcs_init_plls);

void s32g_xpcs_disable_an(struct s32g_xpcs *xpcs)
{
	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, DUPLEX_MODE, DUPLEX_MODE);
	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, AN_ENABLE, 0);
}
EXPORT_SYMBOL_GPL(s32g_xpcs_disable_an);

static void s32g_xpcs_pcs_destroy(void *pcs)
{
	nxp_serdes_xpcs_destroy(pcs);
}

/**
 * s32g_xpcs_create() - allocate and set up one XPCS instance
 * @dev: the SerDes device the XPCS belongs to; owns the allocation
 * @id: XPCS instance number within the SerDes subsystem (0 or 1)
 * @base: mapped register block of that instance
 * @ext_clk: reference is taken from the external pad rather than internal
 * @rate: reference clock rate, 100 or 125 MHz
 * @pcie_shared: whether the lane shares the combo PHY with PCIe
 *
 * The instance is devm-allocated against @dev, so it lives as long as the
 * SerDes device and must not be freed by the caller. The returned handle
 * is opaque; use s32g_xpcs_pcs() to obtain the phylink PCS created on top
 * of the NXP SerDes xPCS shared core.
 *
 * Return: the XPCS handle, or an ERR_PTR() on failure.
 */
struct s32g_xpcs *s32g_xpcs_create(struct device *dev, unsigned char id,
				   void __iomem *base, bool ext_clk,
				   unsigned long rate,
				   enum s32g_xpcs_shared pcie_shared)
{
	const struct regmap_config *conf;
	struct nxp_serdes_xpcs *nxpcs;
	struct s32g_xpcs *xpcs;
	struct phylink_pcs *pcs;
	int ret;

	if (rate != (125 * HZ_PER_MHZ) && rate != (100 * HZ_PER_MHZ)) {
		dev_err(dev, "XPCS cannot operate @%lu HZ\n", rate);
		return ERR_PTR(-EINVAL);
	}

	xpcs = devm_kzalloc(dev, sizeof(*xpcs), GFP_KERNEL);
	if (!xpcs)
		return ERR_PTR(-ENOMEM);

	xpcs->base = base;
	xpcs->ext_clk = ext_clk;
	xpcs->id = id;
	xpcs->dev = dev;
	xpcs->pcie_shared = pcie_shared;

	if (rate == (125 * HZ_PER_MHZ))
		xpcs->mhz125 = true;
	else
		xpcs->mhz125 = false;

	if (!id)
		conf = &s32g_xpcs0_regmap_config;
	else
		conf = &s32g_xpcs1_regmap_config;

	xpcs->regmap = devm_regmap_init(dev, NULL, xpcs, conf);
	if (IS_ERR(xpcs->regmap))
		return ERR_PTR(dev_err_probe(dev, PTR_ERR(xpcs->regmap),
					     "Failed to init register map\n"));

	/*
	 * Create the phylink PCS on the shared core, handing it the S32G
	 * regmap as the register transport. SGMII is the initial interface;
	 * phylink re-runs pcs_config() with the real interface at link time.
	 */
	pcs = nxp_serdes_xpcs_create(dev, xpcs->regmap, NULL, &s32g_xpcs_desc,
				     id, PHY_INTERFACE_MODE_SGMII);
	if (IS_ERR(pcs))
		return ERR_PTR(dev_err_probe(dev, PTR_ERR(pcs),
					     "Failed to create SerDes xPCS\n"));

	ret = devm_add_action_or_reset(dev, s32g_xpcs_pcs_destroy, pcs);
	if (ret)
		return ERR_PTR(ret);

	/* Let the S32G PMA hooks recover their context from the shared PCS. */
	nxpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);
	nxp_serdes_xpcs_set_drvdata(nxpcs, xpcs);

	xpcs->pcs = pcs;

	return xpcs;
}
EXPORT_SYMBOL_GPL(s32g_xpcs_create);

/**
 * s32g_xpcs_pcs() - the phylink PCS of an XPCS instance
 * @xpcs: handle from s32g_xpcs_create()
 *
 * Return: the phylink_pcs, owned by the SerDes device.
 */
struct phylink_pcs *s32g_xpcs_pcs(struct s32g_xpcs *xpcs)
{
	return xpcs->pcs;
}
EXPORT_SYMBOL_GPL(s32g_xpcs_pcs);

MODULE_DESCRIPTION("NXP S32G SerDes XPCS driver");
MODULE_AUTHOR("Jan Petrous (OSS) <jan.petrous@oss.nxp.com>");
MODULE_LICENSE("GPL");
