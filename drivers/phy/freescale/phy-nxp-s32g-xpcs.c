// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright 2021-2026 NXP
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/units.h>
#include <linux/io.h>
#include <linux/processor.h>
#include <linux/regmap.h>
#include "phy-nxp-s32g-xpcs.h"

#define XPCS_TIMEOUT_MS				300

#define ADDR1_OFS				0x3FC

#define SR_MII_CTRL				0x1F0000
#define   SR_RST				BIT(15)
#define   SS13					BIT(13)
#define   AN_ENABLE				BIT(12)
#define   RESTART_AN				BIT(9)
#define   DUPLEX_MODE				BIT(8)
#define   SS6					BIT(6)
#define SR_MII_STS				0x1F0001
#define   LINK_STS				BIT(2)
#define   AN_ABL				BIT(3)
#define   AN_CMPL				BIT(5)
#define SR_MII_DEV_ID1				0x1F0002
#define SR_MII_DEV_ID2				0x1F0003
#define SR_MII_EXT_STS				0x1F000F
#define   CAP_1G_T_FD				BIT(13)
#define   CAP_1G_T_HD				BIT(12)
#define VR_MII_DIG_CTRL1			0x1F8000
#define   BYP_PWRUP				BIT(1)
#define   EN_2_5G_MODE				BIT(2)
#define   CL37_TMR_OVRRIDE			BIT(3)
#define   INIT					BIT(8)
#define   MAC_AUTO_SW				BIT(9)
#define   CS_EN					BIT(10)
#define   PWRSV					BIT(11)
#define   EN_VSMMD1				BIT(13)
#define   R2TLBE				BIT(14)
#define   VR_RST				BIT(15)
#define VR_MII_AN_CTRL				0x1F8001
#define   MII_AN_INTR_EN			BIT(0)
#define   PCS_MODE_MASK				GENMASK(2, 1)
#define    PCS_MODE_SGMII			2
#define   MII_CTRL				BIT(8)
#define VR_MII_AN_INTR_STS			0x1F8002
#define  CL37_ANCMPLT_INTR			BIT(0)
#define  CL37_ANSGM_STS_DUPLEX			BIT(1)
#define  CL37_ANSGM_STS_SPEED_MASK		GENMASK(3, 2)
#define   CL37_ANSGM_10MBPS			0
#define   CL37_ANSGM_100MBPS			1
#define   CL37_ANSGM_1000MBPS			2
#define  CL37_ANSGM_STS_LINK			BIT(4)
#define VR_MII_DBG_CTRL				0x1F8005
#define   SUPPRESS_LOS_DET			BIT(4)
#define   RX_DT_EN_CTL				BIT(6)
#define VR_MII_LINK_TIMER_CTRL			0x1F800A
#define VR_MII_DIG_STS				0x1F8010
#define   PSEQ_STATE_MASK			GENMASK(4, 2)
#define     POWER_GOOD_STATE			0x4
#define VR_MII_GEN5_12G_16G_TX_GENCTRL1		0x1F8031
#define   VBOOST_EN_0				BIT(4)
#define   TX_CLK_RDY_0				BIT(12)
#define	VR_MII_GEN5_12G_16G_TX_GENCTRL2		0x1F8032
#define	  TX_REQ_0				BIT(0)
#define VR_MII_GEN5_12G_16G_TX_RATE_CTRL	0x1F8034
#define   TX0_RATE_MASK				GENMASK(2, 0)
#define     TX0_BAUD_DIV_1			0
#define     TX0_BAUD_DIV_4			2
#define VR_MII_GEN5_12G_16G_TX_EQ_CTRL0		0x1F8036
#define   TX_EQ_MAIN_MASK			GENMASK(13, 8)
#define VR_MII_GEN5_12G_16G_TX_EQ_CTRL1		0x1F8037
#define   TX_EQ_OVR_RIDE			BIT(6)
#define VR_MII_CONSUMER_10G_TX_TERM_CTRL	0x1F803C
#define   TX0_TERM_MASK				GENMASK(2, 0)
#define VR_MII_GEN5_12G_16G_RX_GENCTRL1		0x1F8051
#define   RX_RST_0				BIT(4)
#define VR_MII_GEN5_12G_16G_RX_GENCTRL2		0x1F8052
#define   RX_REQ_0				BIT(0)
#define VR_MII_GEN5_12G_16G_RX_RATE_CTRL	0x1F8054
#define   RX0_RATE_MASK				GENMASK(1, 0)
#define     RX0_BAUD_DIV_2			0x1
#define     RX0_BAUD_DIV_8			0x3
#define VR_MII_GEN5_12G_16G_CDR_CTRL		0x1F8056
#define   CDR_SSC_EN_0				BIT(4)
#define   VCO_LOW_FREQ_0			BIT(8)
#define VR_MII_GEN5_12G_16G_MPLL_CMN_CTRL	0x1F8070
#define   MPLLB_SEL_0				BIT(4)
#define VR_MII_GEN5_12G_16G_MPLLA_CTRL0		0x1F8071
#define   MPLLA_CAL_DISABLE			BIT(15)
#define   MLLA_MULTIPLIER_MASK			GENMASK(7, 0)
#define VR_MII_GEN5_12G_MPLLA_CTRL1		0x1F8072
#define   MPLLA_FRACN_CTRL_MASK			GENMASK(15, 5)
#define VR_MII_GEN5_12G_16G_MPLLA_CTRL2		0x1F8073
#define   MPLLA_TX_CLK_DIV_MASK			GENMASK(13, 11)
#define   MPLLA_DIV10_CLK_EN			BIT(9)
#define VR_MII_GEN5_12G_16G_MPLLB_CTRL0		0x1F8074
#define   MPLLB_CAL_DISABLE			BIT(15)
#define   MLLB_MULTIPLIER_OFF			0
#define   MLLB_MULTIPLIER_MASK			0xFF
#define VR_MII_GEN5_12G_MPLLB_CTRL1		0x1F8075
#define   MPLLB_FRACN_CTRL_MASK			GENMASK(15, 5)
#define VR_MII_GEN5_12G_16G_MPLLB_CTRL2		0x1F8076
#define   MPLLB_TX_CLK_DIV_MASK			GENMASK(13, 11)
#define   MPLLB_DIV10_CLK_EN			BIT(9)
#define VR_MII_RX_LSTS				0x1F8020
#define   RX_VALID_0				BIT(12)
#define VR_MII_GEN5_12G_MPLLA_CTRL3		0x1F8077
#define   MPLLA_BANDWIDTH_MASK			GENMASK(15, 0)
#define VR_MII_GEN5_12G_MPLLB_CTRL3		0x1F8078
#define   MPLLB_BANDWIDTH_MASK			GENMASK(15, 0)
#define VR_MII_GEN5_12G_16G_MISC_CTRL0		0x1F8090
#define   PLL_CTRL				BIT(15)
#define VR_MII_GEN5_12G_16G_REF_CLK_CTRL	0x1F8091
#define   REF_CLK_EN				BIT(0)
#define   REF_USE_PAD				BIT(1)
#define   REF_CLK_DIV2				BIT(2)
#define   REF_RANGE_MASK			GENMASK(5, 3)
#define     RANGE_26_53_MHZ			0x1
#define     RANGE_52_78_MHZ			0x2
#define     RANGE_78_104_MHZ			0x3
#define   REF_MPLLA_DIV2			BIT(6)
#define   REF_MPLLB_DIV2			BIT(7)
#define   REF_RPT_CLK_EN			BIT(8)
#define VR_MII_GEN5_12G_16G_VCO_CAL_LD0		0x1F8092
#define   VCO_LD_VAL_0_MASK			GENMASK(12, 0)
#define VR_MII_GEN5_12G_VCO_CAL_REF0		0x1F8096
#define   VCO_REF_LD_0_MASK			GENMASK(5, 0)

#define phylink_pcs_to_s32g_xpcs(pl_pcs) \
	container_of((pl_pcs), struct s32g_xpcs, pcs)

typedef bool (*xpcs_poll_func_t)(struct s32g_xpcs *);

/*
 * XPCS registers can't be access directly and an indirect address method
 * must be used instead.
 */

static const struct regmap_range s32g_xpcs_wr_ranges[] = {
	regmap_reg_range(0x1F0000, 0x1F0000),
	regmap_reg_range(0x1F0004, 0x1F0004),
	regmap_reg_range(0x1F8000, 0x1F8003),
	regmap_reg_range(0x1F8005, 0x1F8005),
	regmap_reg_range(0x1F800A, 0x1F800A),
	regmap_reg_range(0x1F8012, 0x1F8012),
	regmap_reg_range(0x1F8015, 0x1F8015),
	regmap_reg_range(0x1F8030, 0x1F8037),
	regmap_reg_range(0x1F803C, 0x1F803E),
	regmap_reg_range(0x1F8050, 0x1F8058),
	regmap_reg_range(0x1F805C, 0x1F805E),
	regmap_reg_range(0x1F8064, 0x1F8064),
	regmap_reg_range(0x1F806B, 0x1F806B),
	regmap_reg_range(0x1F8070, 0x1F8078),
	regmap_reg_range(0x1F8090, 0x1F8092),
	regmap_reg_range(0x1F8096, 0x1F8096),
	regmap_reg_range(0x1F8099, 0x1F8099),
	regmap_reg_range(0x1F80A0, 0x1F80A2),
	regmap_reg_range(0x1F80E1, 0x1F80E1),
};

static const struct regmap_access_table s32g_xpcs_wr_table = {
	.yes_ranges = s32g_xpcs_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(s32g_xpcs_wr_ranges),
};

static const struct regmap_range s32g_xpcs_rd_ranges[] = {
	regmap_reg_range(0x1F0000, 0x1F0006),
	regmap_reg_range(0x1F000F, 0x1F000F),
	regmap_reg_range(0x1F0708, 0x1F0710),
	regmap_reg_range(0x1F8000, 0x1F8003),
	regmap_reg_range(0x1F8005, 0x1F8005),
	regmap_reg_range(0x1F800A, 0x1F800A),
	regmap_reg_range(0x1F8010, 0x1F8012),
	regmap_reg_range(0x1F8015, 0x1F8015),
	regmap_reg_range(0x1F8018, 0x1F8018),
	regmap_reg_range(0x1F8020, 0x1F8020),
	regmap_reg_range(0x1F8030, 0x1F8037),
	regmap_reg_range(0x1F803C, 0x1F803C),
	regmap_reg_range(0x1F8040, 0x1F8040),
	regmap_reg_range(0x1F8050, 0x1F8058),
	regmap_reg_range(0x1F805C, 0x1F805E),
	regmap_reg_range(0x1F8060, 0x1F8060),
	regmap_reg_range(0x1F8064, 0x1F8064),
	regmap_reg_range(0x1F806B, 0x1F806B),
	regmap_reg_range(0x1F8070, 0x1F8078),
	regmap_reg_range(0x1F8090, 0x1F8092),
	regmap_reg_range(0x1F8096, 0x1F8096),
	regmap_reg_range(0x1F8098, 0x1F8099),
	regmap_reg_range(0x1F80A0, 0x1F80A2),
	regmap_reg_range(0x1F80E1, 0x1F80E1),
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

static const struct regmap_config s32g_xpcs_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_read = s32g_xpcs_regmap_reg_read,
	.reg_write = s32g_xpcs_regmap_reg_write,
	.wr_table = &s32g_xpcs_wr_table,
	.rd_table = &s32g_xpcs_rd_table,
	.max_register = 0x1F80E1,
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

static unsigned int s32g_xpcs_get_an(struct s32g_xpcs *xpcs)
{
	unsigned int val = s32g_xpcs_read(xpcs, VR_MII_AN_INTR_STS);

	return !!(val & CL37_ANCMPLT_INTR);
};

static int s32g_xpcs_wait_an_done(struct s32g_xpcs *xpcs)
{
	unsigned int val;

	return read_poll_timeout(s32g_xpcs_get_an, val,
				 !!(val & CL37_ANCMPLT_INTR),
				 0,
				 XPCS_TIMEOUT_MS, false, xpcs);
};

static bool s32g_xpcs_poll_timeout(struct s32g_xpcs *xpcs, xpcs_poll_func_t func,
				   ktime_t timeout)
{
	ktime_t cur = ktime_get();

	return func(xpcs) || ktime_after(cur, timeout);
}

static int s32g_xpcs_wait(struct s32g_xpcs *xpcs, xpcs_poll_func_t func)
{
	ktime_t timeout = ktime_add_ms(ktime_get(), XPCS_TIMEOUT_MS);

	spin_until_cond(s32g_xpcs_poll_timeout(xpcs, func, timeout));
	if (!func(xpcs))
		return -ETIMEDOUT;

	return 0;
}

static int s32g_xpcs_wait_bits(struct s32g_xpcs *xpcs, unsigned int reg,
			       unsigned int mask, unsigned int bits)
{
	ktime_t cur;
	ktime_t timeout = ktime_add_ms(ktime_get(), XPCS_TIMEOUT_MS);

	spin_until_cond((cur = ktime_get(),
			 (s32g_xpcs_read(xpcs, reg) & mask) == bits ||
			 ktime_after(cur, timeout)));
	if ((s32g_xpcs_read(xpcs, reg) & mask) != bits)
		return -ETIMEDOUT;

	return 0;
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
				 0,
				 XPCS_TIMEOUT_MS, false, xpcs);
}

int s32g_xpcs_vreset(struct s32g_xpcs *xpcs)
{
	if (!xpcs)
		return -EINVAL;

	/* Step 19 */
	s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, VR_RST, VR_RST);

	return 0;
}

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

int s32g_xpcs_reset_rx(struct s32g_xpcs *xpcs)
{
	int ret = 0;

	ret = s32g_xpcs_wait_power_good_state(xpcs);
	if (ret) {
		dev_err(xpcs->dev, "Failed to enter in PGOOD state after vendor reset\n");
		return ret;
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

static int s32g_xpcs_ref_clk_sel(struct s32g_xpcs *xpcs,
				 enum s32g_xpcs_pll ref_pll)
{
	switch (ref_pll) {
	case XPCS_PLLA:
		s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MPLL_CMN_CTRL,
				     MPLLB_SEL_0, 0);
		xpcs->ref = XPCS_PLLA;
		break;
	case XPCS_PLLB:
		s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_MPLL_CMN_CTRL,
				     MPLLB_SEL_0, MPLLB_SEL_0);
		xpcs->ref = XPCS_PLLB;
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
			     TX_EQ_MAIN_MASK, FIELD_PREP(TX_EQ_MAIN_MASK, 0xC));

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
	case XPCS_PLLA:
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
	case XPCS_PLLB:
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

	if (vco_pll == XPCS_PLLB) {
		s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_CDR_CTRL,
				     VCO_LOW_FREQ_0, VCO_LOW_FREQ_0);
	} else {
		s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_CDR_CTRL,
				     VCO_LOW_FREQ_0, 0);
	}

	return 0;
}

static int s32g_xpcs_init_mplla(struct s32g_xpcs *xpcs)
{
	unsigned int val;

	if (!xpcs)
		return -EINVAL;

	/* Step 7 */
	val = 0;
	if (xpcs->ext_clk)
		val |= REF_USE_PAD;

	if (xpcs->mhz125) {
		val |= REF_MPLLA_DIV2;
		val |= REF_CLK_DIV2;
		val |= FIELD_PREP(REF_RANGE_MASK, RANGE_52_78_MHZ);
	} else {
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

	return 0;
}

static int s32g_xpcs_init_mpllb(struct s32g_xpcs *xpcs)
{
	unsigned int val;

	if (!xpcs)
		return -EINVAL;

	/* Step 7 */
	val = 0;
	if (xpcs->ext_clk)
		val |= REF_USE_PAD;

	if (xpcs->mhz125) {
		val |= REF_MPLLB_DIV2;
		val |= REF_CLK_DIV2;
		val |= FIELD_PREP(REF_RANGE_MASK, RANGE_52_78_MHZ);
	} else {
		val |= FIELD_PREP(REF_RANGE_MASK, RANGE_26_53_MHZ);
	}

	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_REF_CLK_CTRL,
			     REF_MPLLB_DIV2 | REF_USE_PAD | REF_RANGE_MASK |
			     REF_CLK_DIV2, val);

	/* Step 8 */
	if (xpcs->mhz125)
		val = 125 << MLLB_MULTIPLIER_OFF;
	else
		val = 39 << MLLB_MULTIPLIER_OFF;

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

	return 0;
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
	if (target_pll == XPCS_PLLA) {
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, EN_2_5G_MODE, 0);
		s32g_xpcs_vco_cfg(xpcs, XPCS_PLLA);
	} else {
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1,
				     EN_2_5G_MODE, EN_2_5G_MODE);
		s32g_xpcs_vco_cfg(xpcs, XPCS_PLLB);
	}

	/* Signal that clock are not available */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_GENCTRL1,
			     TX_CLK_RDY_0, 0);

	/* Select PLL reference */
	if (target_pll == XPCS_PLLA)
		s32g_xpcs_ref_clk_sel(xpcs, XPCS_PLLA);
	else
		s32g_xpcs_ref_clk_sel(xpcs, XPCS_PLLB);

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
 * Note: This function should be compatible with phylink.
 * That means it should only modify link, duplex, speed
 * an_complete, pause.
 */
static int s32g_xpcs_get_state(struct s32g_xpcs *xpcs,
			       struct phylink_link_state *state)
{
	struct device *dev = xpcs->dev;
	unsigned int mii_ctrl, val, ss;
	bool ss6, ss13, an_enabled, intr_en;

	mii_ctrl = s32g_xpcs_read(xpcs, SR_MII_CTRL);
	an_enabled = !!(mii_ctrl & AN_ENABLE);
	intr_en = !!(s32g_xpcs_read(xpcs, VR_MII_AN_CTRL) & MII_AN_INTR_EN);

	/* Check this important condition */
	if (an_enabled && !intr_en) {
		dev_err(dev, "Invalid SGMII AN config interrupt is disabled\n");
		return -EINVAL;
	}

	if (an_enabled) {
		/* MLO_AN_INBAND */
		state->speed = SPEED_UNKNOWN;
		state->link = 0;
		state->duplex =  DUPLEX_UNKNOWN;
		state->an_complete = 0;
		state->pause = MLO_PAUSE_NONE;
		val = s32g_xpcs_read(xpcs, VR_MII_AN_INTR_STS);

		/* Interrupt is raised with each SGMII AN that is in cases
		 * Link down - Every SGMII link timer expire
		 * Link up - Once before link goes up
		 * So either linkup or raised interrupt mean AN was completed
		 */
		if ((val & CL37_ANCMPLT_INTR) || (val & CL37_ANSGM_STS_LINK)) {
			state->an_complete = 1;
			if (val & CL37_ANSGM_STS_LINK)
				state->link = 1;
			else
				return 0;
			if (val & CL37_ANSGM_STS_DUPLEX)
				state->duplex = DUPLEX_FULL;
			else
				state->duplex = DUPLEX_HALF;
			ss = FIELD_GET(CL37_ANSGM_STS_SPEED_MASK, val);
		} else {
			return 0;
		}

	} else {
		/* MLO_AN_FIXED, MLO_AN_PHY */
		val = s32g_xpcs_read(xpcs, SR_MII_STS);
		state->link = !!(val & LINK_STS);
		state->an_complete = 0;
		state->pause = MLO_PAUSE_NONE;

		if (mii_ctrl & DUPLEX_MODE)
			state->duplex = DUPLEX_FULL;
		else
			state->duplex = DUPLEX_HALF;

		/*
		 * Build similar value as CL37_ANSGM_STS_SPEED with
		 * SS6 and SS13 of SR_MII_CTRL:
		 *   - 0 for 10 Mbps
		 *   - 1 for 100 Mbps
		 *   - 2 for 1000 Mbps
		 */
		ss6 = !!(mii_ctrl & SS6);
		ss13 = !!(mii_ctrl & SS13);
		ss = ss6 << 1 | ss13;
	}

	switch (ss) {
	case CL37_ANSGM_10MBPS:
		state->speed = SPEED_10;
		break;
	case CL37_ANSGM_100MBPS:
		state->speed = SPEED_100;
		break;
	case CL37_ANSGM_1000MBPS:
		state->speed = SPEED_1000;
		break;
	default:
		dev_err(dev, "Failed to interpret the value of SR_MII_CTRL\n");
		break;
	}

	val = s32g_xpcs_read(xpcs, VR_MII_DIG_CTRL1);
	if ((val & EN_2_5G_MODE) && state->speed == SPEED_1000)
		state->speed = SPEED_2500;

	/* Cover SGMII AN inability to distigunish between 1G and 2.5G */
	if ((val & EN_2_5G_MODE) &&
	    state->speed != SPEED_2500 && an_enabled) {
		dev_err(dev, "Speed not supported in SGMII AN mode\n");
		return -EINVAL;
	}

	return 0;
}

static int s32g_xpcs_config_an(struct s32g_xpcs *xpcs,
			       const struct phylink_link_state state)
{
	bool an_enabled = false;

	an_enabled = linkmode_test_bit(ETHTOOL_LINK_MODE_Autoneg_BIT,
				       state.advertising);
	if (!an_enabled)
		return 0;

	s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1,
			     CL37_TMR_OVRRIDE, CL37_TMR_OVRRIDE);

	s32g_xpcs_write_bits(xpcs, VR_MII_AN_CTRL,
			     PCS_MODE_MASK | MII_AN_INTR_EN,
			     FIELD_PREP(PCS_MODE_MASK, PCS_MODE_SGMII) | MII_AN_INTR_EN);
	/* Enable SGMII AN */
	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, AN_ENABLE, AN_ENABLE);
	/* Enable SGMII AUTO SW */
	s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1,
			     MAC_AUTO_SW, MAC_AUTO_SW);

	return 0;
}

static int s32g_xpcs_config(struct s32g_xpcs *xpcs,
			    const struct phylink_link_state state)
{
	struct device *dev = xpcs->dev;
	unsigned int val = 0, duplex = 0;
	int ret = 0;
	int speed = state.speed;
	bool an_enabled;

	/* Configure adaptive MII width */
	s32g_xpcs_write_bits(xpcs, VR_MII_AN_CTRL, MII_CTRL, 0);

	an_enabled = !!(s32g_xpcs_read(xpcs, SR_MII_CTRL) & AN_ENABLE);

	dev_dbg(dev, "xpcs_%d: speed=%u duplex=%d an=%d\n", xpcs->id,
		speed, state.duplex, an_enabled);

	if (an_enabled) {
		switch (speed) {
		case SPEED_10:
		case SPEED_100:
		case SPEED_1000:
			s32g_xpcs_write(xpcs, VR_MII_LINK_TIMER_CTRL, 0x2faf);
			break;
		case SPEED_2500:
			s32g_xpcs_write(xpcs, VR_MII_LINK_TIMER_CTRL, 0x7a1);
			s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, MAC_AUTO_SW, 0);
			break;
		default:
			dev_err(dev, "Speed not recognized. Can't setup xpcs\n");
			return -EINVAL;
		}

		s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, RESTART_AN, RESTART_AN);

		ret = s32g_xpcs_wait_an_done(xpcs);
		if (ret)
			dev_warn(dev, "AN did not finish for XPCS%d", xpcs->id);

		/* Clear the AN CMPL intr */
		s32g_xpcs_write_bits(xpcs, VR_MII_AN_INTR_STS, CL37_ANCMPLT_INTR, 0);
	} else {
		s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, AN_ENABLE, 0);
		s32g_xpcs_write_bits(xpcs, VR_MII_AN_CTRL, MII_AN_INTR_EN, 0);

		switch (speed) {
		case SPEED_10:
			break;
		case SPEED_100:
			val = SS13;
			break;
		case SPEED_1000:
			val = SS6;
			break;
		case SPEED_2500:
			val = SS6;
			break;
		default:
			dev_err(dev, "Speed not supported\n");
			break;
		}

		if (state.duplex == DUPLEX_FULL)
			duplex = DUPLEX_MODE;

		s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, DUPLEX_MODE, duplex);

		if (speed == SPEED_2500) {
			ret = s32g_serdes_bifurcation_pll_transit(xpcs, XPCS_PLLB);
			if (ret)
				dev_err(dev, "Switch to PLLB failed\n");
		} else {
			ret = s32g_serdes_bifurcation_pll_transit(xpcs, XPCS_PLLA);
			if (ret)
				dev_err(dev, "Switch to PLLA failed\n");
		}

		s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, SS6 | SS13, val);
	}

	return 0;
}

/*
 * phylink_pcs_ops fops
 */

static void s32cc_phylink_pcs_get_state(struct phylink_pcs *pcs, unsigned int neg_mode,
					struct phylink_link_state *state)
{
	struct s32g_xpcs *xpcs = phylink_pcs_to_s32g_xpcs(pcs);

	s32g_xpcs_get_state(xpcs, state);
}

static int s32cc_phylink_pcs_config(struct phylink_pcs *pcs,
				    unsigned int neg_mode,
				    phy_interface_t interface,
				    const unsigned long *advertising,
				    bool permit_pause_to_mac)
{
	struct s32g_xpcs *xpcs = phylink_pcs_to_s32g_xpcs(pcs);
	struct phylink_link_state state  = { 0 };

	if (!(neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED))
		return 0;

	linkmode_copy(state.advertising, advertising);

	return s32g_xpcs_config_an(xpcs, state);
}

static void s32cc_phylink_pcs_restart_an(struct phylink_pcs *pcs)
{
	/* Not yet */
}

static void s32cc_phylink_pcs_link_up(struct phylink_pcs *pcs,
				      unsigned int neg_mode,
				      phy_interface_t interface, int speed,
				      int duplex)
{
	struct s32g_xpcs *xpcs = phylink_pcs_to_s32g_xpcs(pcs);
	struct phylink_link_state state = { 0 };

	state.speed = speed;
	state.duplex = duplex;
	state.an_complete = false;

	s32g_xpcs_config(xpcs, state);
}

static const struct phylink_pcs_ops s32cc_phylink_pcs_ops = {
	.pcs_get_state = s32cc_phylink_pcs_get_state,
	.pcs_config = s32cc_phylink_pcs_config,
	.pcs_an_restart = s32cc_phylink_pcs_restart_an,
	.pcs_link_up = s32cc_phylink_pcs_link_up,
};

/*
 * Serdes functions for initializing/configuring/releasing the xpcs
 */

int s32g_xpcs_pre_pcie_2g5(struct s32g_xpcs *xpcs)
{
	int ret;

	/* Enable voltage boost */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_GENCTRL1, VBOOST_EN_0,
			     VBOOST_EN_0);

	/* TX rate baud  */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_TX_RATE_CTRL, 0x7, 0x0U);

	/* Rx rate baud/2 */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_RX_RATE_CTRL, 0x3U, 0x1U);

	/* Set low-frequency operating band */
	s32g_xpcs_write_bits(xpcs, VR_MII_GEN5_12G_16G_CDR_CTRL, CDR_SSC_EN_0,
			     VCO_LOW_FREQ_0);

	ret = s32g_serdes_bifurcation_pll_transit(xpcs, XPCS_PLLB);
	if (ret)
		dev_err(xpcs->dev, "Switch to PLLB failed\n");

	return ret;
}

int s32g_xpcs_init_plls(struct s32g_xpcs *xpcs)
{
	int ret;
	struct device *dev = xpcs->dev;

	if (!xpcs->ext_clk) {
		/* Step 1 */
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, BYP_PWRUP, BYP_PWRUP);
	} else if (xpcs->pcie_shared == NOT_SHARED) {
		ret = s32g_xpcs_wait_power_good_state(xpcs);
		if (ret)
			return ret;
	} else if (xpcs->pcie_shared == PCIE_XPCS_2G5) {
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

	s32g_xpcs_ref_clk_sel(xpcs, XPCS_PLLA);
	ret = s32g_xpcs_init_mplla(xpcs);
	if (ret) {
		dev_err(dev, "Failed to initialize PLLA\n");
		return ret;
	}
	ret = s32g_xpcs_init_mpllb(xpcs);
	if (ret) {
		dev_err(dev, "Failed to initialize PLLB\n");
		return ret;
	}
	s32g_xpcs_vco_cfg(xpcs, XPCS_PLLA);

	/* Step 18 */
	if (!xpcs->ext_clk)
		s32g_xpcs_write_bits(xpcs, VR_MII_DIG_CTRL1, BYP_PWRUP, 0);

	/* Will be cleared by Step 19 Vreset ??? */
	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, AN_ENABLE, 0);
	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, DUPLEX_MODE, DUPLEX_MODE);

	return ret;
}

int s32g_xpcs_disable_an(struct s32g_xpcs *xpcs)
{
	int ret;

	ret = (s32g_xpcs_read(xpcs, SR_MII_CTRL) & AN_ENABLE);

	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, DUPLEX_MODE, DUPLEX_MODE);
	s32g_xpcs_write_bits(xpcs, SR_MII_CTRL, AN_ENABLE, 0);

	return ret;
}

int s32g_xpcs_init(struct s32g_xpcs *xpcs, struct device *dev,
		   unsigned char id, void __iomem *base, bool ext_clk,
		   unsigned long rate, enum pcie_xpcs_mode pcie_shared)
{
	struct regmap_config conf;

	if (rate != (125 * HZ_PER_MHZ) && rate != (100 * HZ_PER_MHZ)) {
		dev_err(dev, "XPCS cannot operate @%lu HZ\n", rate);
		return -EINVAL;
	}

	xpcs->base = base;
	xpcs->ext_clk = ext_clk;
	xpcs->id = id;
	xpcs->dev = dev;
	xpcs->pcie_shared = pcie_shared;

	if (rate == (125 * HZ_PER_MHZ))
		xpcs->mhz125 = true;
	else
		xpcs->mhz125 = false;

	conf = s32g_xpcs_regmap_config;

	if (!id)
		conf.name = "xpcs0";
	else
		conf.name = "xpcs1";

	xpcs->regmap = devm_regmap_init(dev, NULL, xpcs, &conf);
	if (IS_ERR(xpcs->regmap))
		return dev_err_probe(dev, PTR_ERR(xpcs->regmap),
				     "Failed to init register amp\n");

	/* Phylink PCS */
	xpcs->pcs.ops = &s32cc_phylink_pcs_ops;
	xpcs->pcs.poll = true;
	__set_bit(PHY_INTERFACE_MODE_SGMII, xpcs->pcs.supported_interfaces);

	return 0;
}
