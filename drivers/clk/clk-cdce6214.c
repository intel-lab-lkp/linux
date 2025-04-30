// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the TI CDCE6214 clock generator
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 * Copyright (c) 2025 Sascha Hauer <s.hauer@pengutronix.de>
 */

#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <dt-bindings/clock/ti,cdce6214.h>

#define RO_I2C_A0			BIT(15)
#define RO_PDN_INPUT_SEL		BIT(14)
#define RO_GPIO4_DIR_SEL		BIT(13)
#define RO_GPIO1_DIR_SEL		BIT(12)
#define RO_ZDM_CLOCKSEL			BIT(10)
#define RO_ZDM_EN			BIT(8)
#define RO_SYNC				BIT(5)
#define RO_RECAL			BIT(4)
#define RO_RESETN_SOFT			BIT(3)
#define RO_SWRST			BIT(2)
#define RO_POWERDOWN			BIT(1)
#define RO_MODE				BIT(0)

#define R1_GPIO4_INPUT_SEL		GENMASK(15, 12)
#define R1_GPIO3_INPUT_SEL		GENMASK(11, 8)
#define R1_GPIO2_INPUT_SEL		GENMASK(7, 4)
#define R1_GPIO1_INPUT_SEL		GENMASK(3, 0)

#define R2_GPIO4_OUTPUT_SEL		GENMASK(9, 6)
#define R2_GPIO1_OUTPUT_SEL		GENMASK(5, 2)
#define R2_REFSEL_SW			GENMASK(1, 0)

#define R3_DISABLE_CRC			BIT(13)
#define R3_UPDATE_CRC			BIT(12)
#define R3_NVMCOMMIT			BIT(11)
#define R3_REGCOMMIT			BIT(10)
#define R3_REGCOMMIT_PAGE		BIT(9)
#define R3_FREQ_DEC_REG			BIT(6)
#define R3_FREQ_INC_REG			BIT(5)
#define R3_FREQ_INC_DEC_REG_MODE	BIT(4)
#define R3_FREQ_INC_DEC_EN		BIT(3)

#define R4_CH4_PD			BIT(7)
#define R4_CH3_PD			BIT(6)
#define R4_CH2_PD			BIT(5)
#define R4_CH1_PD			BIT(4)
#define R4_POST_EE_DLY			GENMASK(3, 0)

#define R5_PLL_VCOBUFF_LDO_PD		BIT(8)
#define R5_PLL_VCO_LDO_PD		BIT(7)
#define R5_PLL_VCO_BUFF_PD		BIT(6)
#define R5_PLL_CP_LDO_PD		BIT(5)
#define R5_PLL_LOCKDET_PD		BIT(4)
#define R5_PLL_PSB_PD			BIT(3)
#define R5_PLL_PSA_PD			BIT(2)
#define R5_PLL_PFD_PD			BIT(1)

#define R7_NVMCRCERR			BIT(5)
#define R7_LOCK_DET_S			BIT(1)
#define R7_LOCK_DET			BIT(0)

#define R9_NVMLCRC			GENMASK(15, 0)

#define R10_NVMSCRC			GENMASK(15, 0)

#define R11_NVM_RD_ADDR			GENMASK(5, 0)

#define R12_NVM_RD_DATA			GENMASK(15, 0)

#define R13_NVM_WR_ADDR			GENMASK(5, 0)

#define R14_NVM_WR_DATA			GENMASK(15, 0)

#define R15_EE_LOCK			GENMASK(15, 12)
#define R15_CAL_MUTE			BIT(5)

#define R24_IP_PRIREF_BUF_SEL		BIT(15)
#define R24_IP_XO_CLOAD			GENMASK(12, 8)
#define R24_IP_BIAS_SEL_XO		GENMASK(5, 2)
#define R24_IP_SECREF_BUF_SEL		GENMASK(1, 0)
#define R24_IP_SECREF_BUF_SEL_XTAL	0
#define R24_IP_SECREF_BUF_SEL_LVCMOS	1
#define R24_IP_SECREF_BUF_SEL_DIFF	2

#define R25_IP_REF_TO_OUT4_EN		BIT(14)
#define R25_IP_REF_TO_OUT3_EN		BIT(13)
#define R25_IP_REF_TO_OUT2_EN		BIT(12)
#define R25_IP_REF_TO_OUT1_EN		BIT(11)
#define R25_IP_BYP_OUT0_EN		BIT(10)
#define R25_REF_CH_MUX			BIT(9)
#define R25_IP_RDIV			GENMASK(7, 0)

#define R27_MASH_ORDER			GENMASK(1, 0)

#define R30_PLL_NDIV			GENMASK(14, 0)

#define R31_PLL_NUM_15_0		GENMASK(15, 0)

#define R32_PLL_NUM_23_16		GENMASK(7, 0)

#define R33_PLL_DEN_15_0		GENMASK(15, 0)

#define R34_PLL_DEN_23_16		GENMASK(7, 0)

#define R41_SSC_EN			BIT(15)

#define R42_SSC_TYPE			BIT(5)
#define R42_SSC_SEL			GENMASK(3, 1)

#define R43_FREQ_INC_DEC_DELTA		GENMASK(15, 0)

#define R47_PLL_CP_DN			GENMASK(12, 7)
#define R47_PLL_PSB			GENMASK(6, 5)
#define R47_PLL_PSA			GENMASK(4, 3)

#define R48_PLL_LF_RES			GENMASK(14, 11)
#define R48_PLL_CP_UP			GENMASK(5, 0)

#define R49_PLL_LF_ZCAP			GENMASK(4, 0)

#define R50_PLL_LOCKDET_WINDOW		GENMASK(10, 8)

#define R51_PLL_PFD_DLY_EN		BIT(10)
#define R51_PLL_PFD_CTRL		BIT(6)

#define R52_PLL_NCTRL_EN		BIT(6)
#define R52_PLL_CP_EN			BIT(3)

#define R55_PLL_LF_3_PCTRIM		GENMASK(9, 8)
#define R55_PLL_LF_3_PRTRIM		GENMASK(7, 6)

#define R56_CH1_MUX			GENMASK(15, 14)
#define R56_CH1_DIV			GENMASK(13, 0)

#define R57_CH1_LPHCSL_EN		BIT(14)
#define R57_CH1_1P8VDET			BIT(12)
#define R57_CH1_GLITCHLESS_EN		BIT(9)
#define R57_CH1_SYNC_DELAY		GENMASK(8, 4)
#define R57_CH1_SYNC_EN			BIT(3)
#define R57_CH1_MUTE_SEL		BIT(1)
#define R57_CH1_MUTE			BIT(0)

#define R59_CH1_LVDS_EN			BIT(15)
#define R59_CH1_CMOSN_EN		BIT(14)
#define R59_CH1_CMOSP_EN		BIT(13)
#define R59_CH1_CMOSN_POL		BIT(12)
#define R59_CH1_CMOSP_POL		BIT(11)

#define R60_CH1_DIFFBUF_IBIAS_TRIM	GENMASK(15, 12)
#define R60_CH1_LVDS_CMTRIM_INC		GENMASK(11, 10)
#define R60_CH1_LVDS_CMTRIM_DEC		GENMASK(5, 4)
#define R60_CH1_CMOS_SLEW_RATE_CTRL	GENMASK(3, 0)

#define R62_CH2_MUX			GENMASK(15, 14)
#define R62_CH2_DIV			GENMASK(13, 0)

#define R63_CH2_LPHCSL_EN		BIT(13)
#define R63_CH2_1P8VDET			BIT(12)
#define R63_CH2_GLITCHLESS_EN		BIT(9)
#define R63_CH2_SYNC_DELAY		GENMASK(8, 4)
#define R63_CH2_SYNC_EN			BIT(3)
#define R63_CH2_MUTE_SEL		BIT(1)
#define R63_CH2_MUTE			BIT(0)

#define R65_CH2_LVDS_CMTRIM_DEC		GENMASK(14, 13)
#define R65_CH2_LVDS_EN			BIT(11)

#define R66_CH2_LVDS_CMTRIM_IN		GENMASK(5, 4)
#define R66_CH2_DIFFBUF_IBIAS_TRIM	GENMASK(3, 0)

#define R67_CH3_MUX			GENMASK(15, 14)
#define R67_CH3_DIV			GENMASK(13, 0)

#define R68_CH3_LPHCSL_EN		BIT(13)
#define R68_CH3_1P8VDET			BIT(12)
#define R68_CH3_GLITCHLESS_EN		BIT(9)
#define R68_CH3_SYNC_DELAY		GENMASK(8, 4)
#define R68_CH3_SYNC_EN			BIT(3)
#define R68_CH3_MUTE_SEL		BIT(1)
#define R68_CH3_MUTE			BIT(0)

#define R70_CH3_LVDS_EN			BIT(11)

#define R71_CH3_LVDS_CMTRIM_DEC		GENMASK(10, 9)
#define R71_CH3_LVDS_CMTRIM_INC		GENMASK(5, 4)
#define R71_CH3_DIFFBUF_IBIAS_TR	GENMASK(3, 0)

#define R72_CH4_MUX			GENMASK(15, 14)
#define R72_CH4_DIV			GENMASK(13, 0)

#define R73_CH4_LPHCSL_EN		BIT(13)
#define R73_CH4_1P8VDET			BIT(12)
#define R73_CH4_GLITCHLESS_EN		BIT(9)
#define R73_CH4_SYNC_DELAY		GENMASK(8, 4)
#define R73_CH4_SYNC_EN			BIT(3)
#define R73_CH4_MUTE_SEL		BIT(1)
#define R73_CH4_MUTE			BIT(0)

#define R75_CH4_LVDS_EN			BIT(15)
#define R75_CH4_CMOSP_EN		BIT(14)
#define R75_CH4_CMOSN_EN		BIT(13)
#define R75_CH4_CMOSP_POL		BIT(12)
#define R75_CH4_CMOSN_POL		BIT(11)

#define R76_CH4_DIFFBUF_IBIAS_TRIM	GENMASK(9, 6)
#define R76_CH4_LVDS_CMTRIM_IN		GENMASK(5, 4)
#define R76_CH4_CMOS_SLEW_RATE_CTRL	GENMASK(3, 0)

#define R77_CH4_LVDS_CMTRIM_DEC		GENMASK(1, 0)

#define R78_CH0_EN			BIT(12)

#define R79_SAFETY_1P8V_MODE		BIT(9)
#define R79_CH0_CMOS_SLEW_RATE_CTRL	GENMASK(3, 0)

#define R81_PLL_LOCK_MASK		BIT(3)

#define CDCE6214_VCO_MIN 2335000000
#define CDCE6214_VCO_MAX 2625000000
#define CDCE6214_DENOM_DEFAULT 0x1000000

#define CDCE6214_CLKIN_FMT_CMOS		0
#define CDCE6214_CLKIN_FMT_XTAL		1
#define CDCE6214_CLKIN_FMT_DIFF		2

#define CDCE6214_CLKOUT_FMT_CMOS	0
#define CDCE6214_CLKOUT_FMT_LVDS	1
#define CDCE6214_CLKOUT_FMT_LPHCSL	2

#define CDCE6214_CMOS_MODE_DISABLED	0
#define CDCE6214_CMOS_MODE_HIGH		1
#define CDCE6214_CMOS_MODE_LOW		2

static const char * const clk_names[] = {
	[CDCE6214_CLK_PRIREF] = "priref",
	[CDCE6214_CLK_SECREF] = "secref",
	[CDCE6214_CLK_OUT0] = "out0",
	[CDCE6214_CLK_OUT1] = "out1",
	[CDCE6214_CLK_OUT2] = "out2",
	[CDCE6214_CLK_OUT3] = "out3",
	[CDCE6214_CLK_OUT4] = "out4",
	[CDCE6214_CLK_PLL] = "pll",
	[CDCE6214_CLK_PSA] = "psa",
	[CDCE6214_CLK_PSB] = "psb",
};

static const char * const clkkin_fmt_names[] = {
	[CDCE6214_CLKIN_FMT_CMOS] = "cmos",
	[CDCE6214_CLKIN_FMT_XTAL] = "xtal",
	[CDCE6214_CLKIN_FMT_DIFF] = "differential",
};

static const char * const clkkout_fmt_names[] = {
	[CDCE6214_CLKOUT_FMT_CMOS] = "cmos",
	[CDCE6214_CLKOUT_FMT_LVDS] = "lvds",
	[CDCE6214_CLKOUT_FMT_LPHCSL] = "lp-hcsl",
};

static const char * const cmos_mode_names[] = {
	[CDCE6214_CMOS_MODE_DISABLED] = "disabled",
	[CDCE6214_CMOS_MODE_HIGH] = "high",
	[CDCE6214_CMOS_MODE_LOW] = "low",
};

#define CDCE6214_NUM_CLOCKS	ARRAY_SIZE(clk_names)

struct cdce6214;

struct cdce6214_clock {
	struct clk_hw hw;
	struct cdce6214 *priv;
	int index;
};

struct cdce6214 {
	struct i2c_client *client;
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct cdce6214_clock clk[CDCE6214_NUM_CLOCKS];
};

static inline struct cdce6214_clock *hw_to_cdce6214_clk(struct clk_hw *hw)
{
	return container_of(hw, struct cdce6214_clock, hw);
}

static struct clk_hw *cdce6214_of_clk_get(struct of_phandle_args *clkspec,
					  void *data)
{
	struct cdce6214 *priv = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= CDCE6214_NUM_CLOCKS)
		return ERR_PTR(-EINVAL);
	if (idx <= CDCE6214_CLK_SECREF)
		return ERR_PTR(-EINVAL);

	return &priv->clk[idx].hw;
}

static const struct regmap_config cdce6214_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 1,
	.max_register = 0x0055,
};

static int cdce6214_configure(struct cdce6214 *priv)
{
	regmap_update_bits(priv->regmap, 2, R2_REFSEL_SW,
			   FIELD_PREP(R2_REFSEL_SW, 2));

	return 0;
}

static unsigned long cdce6214_clk_out0_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int val, div;

	regmap_read(priv->regmap, 25, &val);

	div = FIELD_GET(R25_IP_RDIV, val);

	if (!div)
		return parent_rate * 2;

	return DIV_ROUND_UP_ULL((u64)parent_rate, div);
}

static long cdce6214_clk_out0_round_rate(struct clk_hw *hw, unsigned long rate,
					 unsigned long *best_parent_rate)
{
	unsigned int div;

	if (rate >= *best_parent_rate)
		return *best_parent_rate * 2;

	div = DIV_ROUND_CLOSEST(*best_parent_rate, rate);

	return DIV_ROUND_UP_ULL((u64)*best_parent_rate, div);
}

static int cdce6214_clk_out0_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int div;

	if (rate >= parent_rate) {
		regmap_update_bits(priv->regmap, 25, R25_IP_RDIV, FIELD_PREP(R25_IP_RDIV, 0));
		return 0;
	}

	div = DIV_ROUND_CLOSEST(parent_rate, rate);
	if (div > R25_IP_RDIV)
		div = R25_IP_RDIV;

	regmap_update_bits(priv->regmap, 25, R25_IP_RDIV, FIELD_PREP(R25_IP_RDIV, div));

	return 0;
}

static u8 cdce6214_clk_out0_get_parent(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int val, idx;

	regmap_read(priv->regmap, 2, &val);

	idx = FIELD_GET(R2_REFSEL_SW, val);

	switch (idx) {
	case 0:
	case 1:
		idx = 0;
		break;
	case 2:
		idx = 1;
		break;
	case 3:
		idx = 0;
		break;
	};

	return idx;
}

static int cdce6214_clk_out0_set_parent(struct clk_hw *hw, u8 index)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	regmap_update_bits(priv->regmap, 25, R25_REF_CH_MUX, FIELD_PREP(R25_REF_CH_MUX, index));

	return 0;
}

static const struct clk_ops cdce6214_clk_out0_ops = {
	.recalc_rate = cdce6214_clk_out0_recalc_rate,
	.round_rate = cdce6214_clk_out0_round_rate,
	.set_rate = cdce6214_clk_out0_set_rate,
	.get_parent = cdce6214_clk_out0_get_parent,
	.set_parent = cdce6214_clk_out0_set_parent,
};

static int cdce6214_clk_out_ldo(struct clk_hw *hw, int enable)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int mask, val;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		mask = R4_CH1_PD;
		break;
	case CDCE6214_CLK_OUT2:
		mask = R4_CH2_PD;
		break;
	case CDCE6214_CLK_OUT3:
		mask = R4_CH3_PD;
		break;
	case CDCE6214_CLK_OUT4:
		mask = R4_CH4_PD;
		break;
	default:
		return -EINVAL;
	};

	if (enable > 0) {
		regmap_clear_bits(priv->regmap, 4, mask);
	} else if (!enable) {
		regmap_set_bits(priv->regmap, 4, mask);
	} else {
		regmap_read(priv->regmap, 4, &val);
		return !(val & mask);
	}

	return 0;
}

static int cdce6214_clk_out_prepare(struct clk_hw *hw)
{
	return cdce6214_clk_out_ldo(hw, 1);
}

static void cdce6214_clk_out_unprepare(struct clk_hw *hw)
{
	cdce6214_clk_out_ldo(hw, 0);
}

static int cdce6214_clk_out_is_prepared(struct clk_hw *hw)
{
	return cdce6214_clk_out_ldo(hw, -1);
}

static unsigned long cdce6214_clk_out_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int val, div;
	unsigned long r;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		regmap_read(priv->regmap, 56, &val);
		div = FIELD_GET(R56_CH1_DIV, val);
		break;
	case CDCE6214_CLK_OUT2:
		regmap_read(priv->regmap, 62, &val);
		div = FIELD_GET(R62_CH2_DIV, val);
		break;
	case CDCE6214_CLK_OUT3:
		regmap_read(priv->regmap, 67, &val);
		div = FIELD_GET(R67_CH3_DIV, val);
		break;
	case CDCE6214_CLK_OUT4:
		regmap_read(priv->regmap, 72, &val);
		div = FIELD_GET(R72_CH4_DIV, val);
		break;
	};

	if (!div)
		div = 1;

	r = DIV_ROUND_UP_ULL((u64)parent_rate, div);

	return r;
}

static int cdce6214_get_out_div(unsigned long rate, unsigned long parent_rate)
{
	unsigned int div = DIV_ROUND_CLOSEST(parent_rate, rate);

	if (div < 1)
		div = 1;

	if (div > R72_CH4_DIV)
		div = R72_CH4_DIV;

	return div;
}

static long cdce6214_clk_out_round_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long *best_parent_rate)
{
	unsigned int div = cdce6214_get_out_div(rate, *best_parent_rate);

	return DIV_ROUND_UP_ULL((u64)*best_parent_rate, div);
}

static int cdce6214_clk_out_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	unsigned int div = cdce6214_get_out_div(rate, parent_rate);
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		regmap_update_bits(priv->regmap, 56, R56_CH1_DIV,
				   FIELD_PREP(R56_CH1_DIV, div));
		break;
	case CDCE6214_CLK_OUT2:
		regmap_update_bits(priv->regmap, 62, R62_CH2_DIV,
				   FIELD_PREP(R62_CH2_DIV, div));
		break;
	case CDCE6214_CLK_OUT3:
		regmap_update_bits(priv->regmap, 67, R67_CH3_DIV,
				   FIELD_PREP(R67_CH3_DIV, div));
		break;
	case CDCE6214_CLK_OUT4:
		regmap_update_bits(priv->regmap, 72, R72_CH4_DIV,
				   FIELD_PREP(R72_CH4_DIV, div));
		break;
	};

	return 0;
}

static u8 cdce6214_clk_out_get_parent(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int val, idx;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		regmap_read(priv->regmap, 56, &val);
		idx = FIELD_GET(R56_CH1_MUX, val);
		break;
	case CDCE6214_CLK_OUT2:
		regmap_read(priv->regmap, 62, &val);
		idx = FIELD_GET(R62_CH2_MUX, val);
		break;
	case CDCE6214_CLK_OUT3:
		regmap_read(priv->regmap, 67, &val);
		idx = FIELD_GET(R67_CH3_MUX, val);
		break;
	case CDCE6214_CLK_OUT4:
		regmap_read(priv->regmap, 72, &val);
		idx = FIELD_GET(R72_CH4_MUX, val);
		break;
	};

	return idx;
}

static int cdce6214_clk_out_set_parent(struct clk_hw *hw, u8 index)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	switch (clock->index) {
	case CDCE6214_CLK_OUT1:
		regmap_update_bits(priv->regmap, 56, R56_CH1_MUX, FIELD_PREP(R56_CH1_MUX, index));
		break;
	case CDCE6214_CLK_OUT2:
		regmap_update_bits(priv->regmap, 62, R62_CH2_MUX, FIELD_PREP(R62_CH2_MUX, index));
		break;
	case CDCE6214_CLK_OUT3:
		regmap_update_bits(priv->regmap, 67, R67_CH3_MUX, FIELD_PREP(R67_CH3_MUX, index));
		break;
	case CDCE6214_CLK_OUT4:
		regmap_update_bits(priv->regmap, 72, R72_CH4_MUX, FIELD_PREP(R72_CH4_MUX, index));
		break;
	};

	return 0;
}

static const struct clk_ops cdce6214_clk_out_ops = {
	.prepare = cdce6214_clk_out_prepare,
	.unprepare = cdce6214_clk_out_unprepare,
	.is_prepared = cdce6214_clk_out_is_prepared,
	.recalc_rate = cdce6214_clk_out_recalc_rate,
	.round_rate = cdce6214_clk_out_round_rate,
	.set_rate = cdce6214_clk_out_set_rate,
	.get_parent = cdce6214_clk_out_get_parent,
	.set_parent = cdce6214_clk_out_set_parent,
};

static int pll_calc_values(unsigned long parent_rate, unsigned long out,
			   unsigned long *ndiv, unsigned long *num, unsigned long *den)
{
	u64 a;

	if (out < CDCE6214_VCO_MIN || out > CDCE6214_VCO_MAX)
		return -EINVAL;

	*den = 10000000;
	*ndiv = out / parent_rate;
	a = (out % parent_rate);
	a *= *den;
	do_div(a, parent_rate);
	*num = a;

	return 0;
}

static unsigned long cdce6214_clk_pll_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned long ndiv, num, den;
	unsigned int val;

	regmap_read(priv->regmap, 30, &val);
	ndiv = FIELD_GET(R30_PLL_NDIV, val);

	regmap_read(priv->regmap, 31, &val);
	num = FIELD_GET(R31_PLL_NUM_15_0, val);

	regmap_read(priv->regmap, 32, &val);
	num |= FIELD_GET(R32_PLL_NUM_23_16, val) << 16;

	regmap_read(priv->regmap, 33, &val);
	den = FIELD_GET(R33_PLL_DEN_15_0, val);

	regmap_read(priv->regmap, 34, &val);
	den |= FIELD_GET(R34_PLL_DEN_23_16, val) << 16;

	if (!den)
		den = CDCE6214_DENOM_DEFAULT;

	return parent_rate * ndiv + DIV_ROUND_CLOSEST(parent_rate * num, den);
}

static long cdce6214_clk_pll_round_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long *best_parent_rate)
{
	if (rate < CDCE6214_VCO_MIN)
		rate = CDCE6214_VCO_MIN;
	if (rate > CDCE6214_VCO_MAX)
		rate = CDCE6214_VCO_MAX;
	if (rate < *best_parent_rate * 24)
		return -EINVAL;

	return rate;
}

static bool cdce6214_pll_locked(struct cdce6214 *priv)
{
	unsigned int val;

	regmap_read(priv->regmap, 7, &val);

	return val & R7_LOCK_DET;
}

static int cdce6214_wait_pll_lock(struct cdce6214 *priv)
{
	unsigned int val;
	int ret;

	ret = regmap_read_poll_timeout(priv->regmap, 7, val,
				       val & R7_LOCK_DET, 0, 1000);
	if (ret)
		dev_err(priv->dev, "Timeout waiting for PLL lock\n");

	return ret;
}

#define R5_PLL_POWER_BITS (R5_PLL_VCOBUFF_LDO_PD | \
			   R5_PLL_VCO_LDO_PD | \
			   R5_PLL_VCO_BUFF_PD)

static int cdce6214_clk_pll_prepare(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	regmap_clear_bits(priv->regmap, 5, R5_PLL_POWER_BITS);

	regmap_set_bits(priv->regmap, 0, RO_RECAL);

	return cdce6214_wait_pll_lock(priv);
}

static void cdce6214_clk_pll_unprepare(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	regmap_set_bits(priv->regmap, 5, R5_PLL_POWER_BITS);
}

static bool cdce6214_clk_pll_powered(struct cdce6214 *priv)
{
	unsigned int val;

	regmap_read(priv->regmap, 5, &val);

	return (val & R5_PLL_POWER_BITS) == 0;
}

static int cdce6214_clk_pll_is_prepared(struct clk_hw *hw)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	return cdce6214_pll_locked(priv);
}

static int cdce6214_clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned long ndiv, num, den;
	int ret;

	ret = pll_calc_values(parent_rate, rate, &ndiv, &num, &den);
	if (ret < 0)
		return ret;

	if (den == CDCE6214_DENOM_DEFAULT)
		den = 0;

	regmap_update_bits(priv->regmap, 34, R34_PLL_DEN_23_16,
			   FIELD_PREP(R34_PLL_DEN_23_16, den >> 16));
	regmap_update_bits(priv->regmap, 33, R33_PLL_DEN_15_0,
			   FIELD_PREP(R33_PLL_DEN_15_0, den & 0xffff));
	regmap_update_bits(priv->regmap, 32, R32_PLL_NUM_23_16,
			   FIELD_PREP(R32_PLL_NUM_23_16, num >> 16));
	regmap_update_bits(priv->regmap, 31, R31_PLL_NUM_15_0,
			   FIELD_PREP(R31_PLL_NUM_15_0, num & 0xffff));
	regmap_update_bits(priv->regmap, 30, R30_PLL_NDIV,
			   FIELD_PREP(R30_PLL_NDIV, ndiv));

	regmap_update_bits(priv->regmap, 3, R3_FREQ_INC_DEC_REG_MODE | R3_FREQ_INC_DEC_EN,
			   R3_FREQ_INC_DEC_REG_MODE | R3_FREQ_INC_DEC_EN);

	if (cdce6214_clk_pll_powered(priv)) {
		regmap_set_bits(priv->regmap, 0, RO_RECAL);
		ret = cdce6214_wait_pll_lock(priv);
	}

	return ret;
}

static const struct clk_ops cdce6214_clk_pll_ops = {
	.prepare = cdce6214_clk_pll_prepare,
	.unprepare = cdce6214_clk_pll_unprepare,
	.is_prepared = cdce6214_clk_pll_is_prepared,
	.recalc_rate = cdce6214_clk_pll_recalc_rate,
	.round_rate = cdce6214_clk_pll_round_rate,
	.set_rate = cdce6214_clk_pll_set_rate,
};

static int cdce6214_clk_psx_ldo(struct clk_hw *hw, int enable)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int mask, val;

	switch (clock->index) {
	case CDCE6214_CLK_PSA:
		mask = R5_PLL_PSA_PD;
		break;
	case CDCE6214_CLK_PSB:
		mask = R5_PLL_PSB_PD;
		break;
	default:
		return -EINVAL;
	};

	if (enable > 0) {
		regmap_clear_bits(priv->regmap, 5, mask);
	} else if (!enable) {
		regmap_set_bits(priv->regmap, 5, mask);
	} else {
		regmap_read(priv->regmap, 5, &val);

		return !(val & mask);
	}

	return 0;
}

static int cdce6214_clk_psx_prepare(struct clk_hw *hw)
{
	return cdce6214_clk_psx_ldo(hw, 1);
}

static void cdce6214_clk_psx_unprepare(struct clk_hw *hw)
{
	cdce6214_clk_psx_ldo(hw, 0);
}

static int cdce6214_clk_psx_is_prepared(struct clk_hw *hw)
{
	return cdce6214_clk_psx_ldo(hw, -1);
}

static unsigned long cdce6214_clk_psx_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;
	unsigned int psx[] = { 4, 5, 6, 6 };
	unsigned int val, div;

	regmap_read(priv->regmap, 47, &val);

	switch (clock->index) {
	case CDCE6214_CLK_PSA:
		div = psx[FIELD_GET(R47_PLL_PSA, val)];
		break;
	case CDCE6214_CLK_PSB:
		div = psx[FIELD_GET(R47_PLL_PSB, val)];
		break;
	};

	return DIV_ROUND_UP_ULL((u64)parent_rate, div);
}

static int cdce6214_get_psx_div(unsigned long rate, unsigned long parent_rate)
{
	unsigned int div = DIV_ROUND_CLOSEST(parent_rate, rate);

	if (div < 4)
		div = 4;

	if (div > 6)
		div = 6;

	return div;
}

static long cdce6214_clk_psx_round_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long *best_parent_rate)
{
	unsigned int div = cdce6214_get_psx_div(rate, *best_parent_rate);

	return DIV_ROUND_UP_ULL((u64)*best_parent_rate, div);
}

static int cdce6214_clk_psx_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	unsigned int div = cdce6214_get_psx_div(rate, parent_rate);
	struct cdce6214_clock *clock = hw_to_cdce6214_clk(hw);
	struct cdce6214 *priv = clock->priv;

	switch (clock->index) {
	case CDCE6214_CLK_PSA:
		regmap_update_bits(priv->regmap, 47, R47_PLL_PSA,
				   FIELD_PREP(R47_PLL_PSA, div));
		break;
	case CDCE6214_CLK_PSB:
		regmap_update_bits(priv->regmap, 47, R47_PLL_PSB,
				   FIELD_PREP(R47_PLL_PSB, div));
		break;
	};

	return 0;
}

static const struct clk_ops cdce6214_clk_psx_ops = {
	.prepare = cdce6214_clk_psx_prepare,
	.unprepare = cdce6214_clk_psx_unprepare,
	.is_prepared = cdce6214_clk_psx_is_prepared,
	.recalc_rate = cdce6214_clk_psx_recalc_rate,
	.round_rate = cdce6214_clk_psx_round_rate,
	.set_rate = cdce6214_clk_psx_set_rate,
};

static int cdce6214_clk_register(struct cdce6214 *priv)
{
	struct clk_init_data init[CDCE6214_NUM_CLOCKS] = { 0 };
	struct clk_parent_data pdata_out0[2] = {};
	struct clk_parent_data pdata_out[4] = {};
	struct clk_parent_data pdata_pll = {};
	struct clk_parent_data pdata_psx = {};
	int i, ret;

	pdata_out0[0].fw_name = "priref";
	pdata_out0[1].fw_name = "secref";

	init[CDCE6214_CLK_OUT0].ops = &cdce6214_clk_out0_ops;
	init[CDCE6214_CLK_OUT0].num_parents = 2;
	init[CDCE6214_CLK_OUT0].parent_data = pdata_out0;
	init[CDCE6214_CLK_OUT0].flags = CLK_SET_RATE_NO_REPARENT;

	pdata_out[0].hw = &priv->clk[CDCE6214_CLK_PSA].hw;
	pdata_out[1].hw = &priv->clk[CDCE6214_CLK_PSB].hw;
	pdata_out[3].hw = &priv->clk[CDCE6214_CLK_OUT0].hw;

	for (i = CDCE6214_CLK_OUT1; i <= CDCE6214_CLK_OUT4; i++) {
		init[i].ops = &cdce6214_clk_out_ops;
		init[i].num_parents = 4;
		init[i].parent_data = pdata_out;
		init[i].flags = CLK_SET_RATE_NO_REPARENT;
	}

	init[CDCE6214_CLK_PLL].ops = &cdce6214_clk_pll_ops;
	init[CDCE6214_CLK_PLL].num_parents = 1;
	pdata_pll.hw = &priv->clk[CDCE6214_CLK_OUT0].hw;
	init[CDCE6214_CLK_PLL].parent_data = &pdata_pll;

	pdata_psx.hw = &priv->clk[CDCE6214_CLK_PLL].hw;
	for (i = CDCE6214_CLK_PSA; i <= CDCE6214_CLK_PSB; i++) {
		init[i].ops = &cdce6214_clk_psx_ops;
		init[i].num_parents = 1;
		init[i].parent_data = &pdata_psx;
	}

	for (i = 0; i < CDCE6214_NUM_CLOCKS; i++) {
		struct cdce6214_clock *clk = &priv->clk[i];
		char name[128];

		if (!init[i].ops)
			continue;

		snprintf(name, sizeof(name), "%s_%s", dev_name(priv->dev), clk_names[i]);
		init[i].name = name;
		clk->hw.init = &init[i];
		clk->priv = priv;
		clk->index = i;
		ret = devm_clk_hw_register(priv->dev, &clk->hw);
		if (ret)
			return ret;
	}

	return 0;
}

static void cdce6214_setup_xtal(struct cdce6214 *priv, struct device_node *np)
{
	unsigned short ip_xo_cload[] = {
		/* index is the register value */
		3000, 3200, 3400, 3600, 3800, 4000, 4200, 4400,
		4600, 4800, 5000, 5200, 5400, 5600, 5800, 6000,
		6200, 6400, 6500, 6700, 6900, 7100, 7300, 7500,
		7700, 7900, 8100, 8300, 8500, 8700, 8900, 9000
	};

	unsigned short ip_bias_sel_xo[] = {
		/* index is the register value */
		0, 14, 29, 44,
		59, 148, 295, 443,
		591, 884, 1177, 1468, 1758
	};

	unsigned int cload = 4400; /* reset default */
	unsigned int bias = 295; /* reset default */
	int i;

	of_property_read_u32(np, "ti,xo-cload-femtofarad", &cload);
	of_property_read_u32(np, "ti,xo-bias-current-microampere", &bias);

	for (i = 0; i < ARRAY_SIZE(ip_xo_cload); i++)
		if (cload <= ip_xo_cload[i])
			break;

	if (i >= ARRAY_SIZE(ip_xo_cload)) {
		dev_warn(priv->dev, "ti,xo-cload-femtofarad value %u too high\n",
			 cload);
		i = ARRAY_SIZE(ip_xo_cload) - 1;
	}

	regmap_update_bits(priv->regmap, 2, R24_IP_XO_CLOAD,
			   FIELD_PREP(R24_IP_XO_CLOAD, i));

	for (i = 0; i < ARRAY_SIZE(ip_bias_sel_xo); i++)
		if (bias <= ip_bias_sel_xo[i])
			break;

	if (i >= ARRAY_SIZE(ip_xo_cload)) {
		dev_warn(priv->dev, "ti,xo-bias-current-microampere value %u too high\n",
			 bias);
		i = ARRAY_SIZE(ip_xo_cload) - 1;
	}

	regmap_update_bits(priv->regmap, 2, R24_IP_BIAS_SEL_XO,
			   FIELD_PREP(R24_IP_BIAS_SEL_XO, i));
}

static int cdce6214_get_clkout_fmt(struct cdce6214 *priv, struct device_node *np)
{
	const char *fmt;
	int ret;

	ret = of_property_read_string(np, "ti,clkout-fmt", &fmt);
	if (ret)
		return ret;

	return match_string(clkkout_fmt_names, ARRAY_SIZE(clkkout_fmt_names), fmt);
}

static int cdce6214_get_clkin_fmt(struct cdce6214 *priv, struct device_node *np)
{
	const char *fmt;
	int ret;

	ret = of_property_read_string(np, "ti,clkin-fmt", &fmt);
	if (ret)
		return ret;

	return match_string(clkkin_fmt_names, ARRAY_SIZE(clkkin_fmt_names), fmt);
}

static int cdce6214_get_cmos_mode(struct cdce6214 *priv, struct device_node *np,
				  const char *propname)
{
	const char *fmt;
	int ret;

	ret = of_property_read_string(np, propname, &fmt);
	if (ret)
		return 0;

	return match_string(cmos_mode_names, ARRAY_SIZE(cmos_mode_names), fmt);
}

static int cdce6214_set_cmos_mode(struct cdce6214 *priv, struct device_node *np,
				  unsigned int reg)
{
	int cmosp_mode, cmosn_mode;
	u16 cmode = 0, cmode_mask;

	cmosn_mode = cdce6214_get_cmos_mode(priv, np, "ti,cmosn-mode");
	if (cmosn_mode < 0)
		return cmosn_mode;

	cmosp_mode = cdce6214_get_cmos_mode(priv, np, "ti,cmosp-mode");
	if (cmosp_mode < 0)
		return cmosp_mode;

	switch (cmosp_mode) {
	case CDCE6214_CMOS_MODE_DISABLED:
		break;
	case CDCE6214_CMOS_MODE_HIGH:
		cmode |= R59_CH1_CMOSP_EN | R59_CH1_CMOSP_POL;
		break;
	case CDCE6214_CMOS_MODE_LOW:
		cmode |= R59_CH1_CMOSP_EN;
		break;
	default:
		return -EINVAL;
	}

	switch (cmosn_mode) {
	case CDCE6214_CMOS_MODE_DISABLED:
		break;
	case CDCE6214_CMOS_MODE_HIGH:
		cmode |= R59_CH1_CMOSN_EN | R59_CH1_CMOSN_POL;
		break;
	case CDCE6214_CMOS_MODE_LOW:
		cmode |= R59_CH1_CMOSN_EN;
		break;
	default:
		return -EINVAL;
	}

	cmode_mask = R59_CH1_CMOSP_EN | R59_CH1_CMOSN_EN |
		     R59_CH1_CMOSP_POL | R59_CH1_CMOSN_POL;

	/* Relevant fields are identical for register 59 and 75 */
	regmap_update_bits(priv->regmap, reg, cmode_mask, cmode);

	return 0;
}

static int cdce6214_parse_subnode(struct cdce6214 *priv, struct device_node *np)
{
	struct regmap *reg = priv->regmap;
	unsigned int idx, val;
	int fmt;
	int ret;

	ret = of_property_read_u32(np, "reg", &idx);
	if (ret) {
		dev_err(priv->dev, "missing reg property in child: %s\n",
			np->full_name);
		return ret;
	}

	if (idx >= CDCE6214_NUM_CLOCKS)
		return -EINVAL;

	switch (idx) {
	case CDCE6214_CLK_OUT1:
		fmt = cdce6214_get_clkout_fmt(priv, np);
		switch (fmt) {
		case CDCE6214_CLKOUT_FMT_CMOS:
			ret = cdce6214_set_cmos_mode(priv, np, 59);
			if (ret)
				return ret;
			regmap_clear_bits(reg, 59, R59_CH1_LVDS_EN);
			regmap_clear_bits(reg, 57, R57_CH1_LPHCSL_EN);
			break;
		case CDCE6214_CLKOUT_FMT_LVDS:
			regmap_clear_bits(reg, 57, R57_CH1_LPHCSL_EN);
			regmap_set_bits(reg, 59, R59_CH1_LVDS_EN);
			break;
		case CDCE6214_CLKOUT_FMT_LPHCSL:
			regmap_clear_bits(reg, 59, R59_CH1_LVDS_EN);
			regmap_set_bits(reg, 57, R57_CH1_LPHCSL_EN);
			break;
		default:
			goto err_illegal_fmt;
		}
		break;
	case CDCE6214_CLK_OUT2:
		fmt = cdce6214_get_clkout_fmt(priv, np);
		switch (fmt) {
		case CDCE6214_CLKOUT_FMT_CMOS:
			goto err_illegal_fmt;
		case CDCE6214_CLKOUT_FMT_LVDS:
			regmap_set_bits(reg, 65, R65_CH2_LVDS_EN);
			regmap_clear_bits(reg, 63, R63_CH2_LPHCSL_EN);
			break;
		case CDCE6214_CLKOUT_FMT_LPHCSL:
			regmap_set_bits(reg, 63, R63_CH2_LPHCSL_EN);
			regmap_clear_bits(reg, 65, R65_CH2_LVDS_EN);
			break;
		default:
			goto err_illegal_fmt;
		}
		break;
	case CDCE6214_CLK_OUT3:
		fmt = cdce6214_get_clkout_fmt(priv, np);
		switch (fmt) {
		case CDCE6214_CLKOUT_FMT_CMOS:
			goto err_illegal_fmt;
		case CDCE6214_CLKOUT_FMT_LVDS:
			regmap_set_bits(reg, 70, R70_CH3_LVDS_EN);
			regmap_clear_bits(reg, 68, R68_CH3_LPHCSL_EN);
			break;
		case CDCE6214_CLKOUT_FMT_LPHCSL:
			regmap_set_bits(reg, 70, R70_CH3_LVDS_EN);
			regmap_clear_bits(reg, 68, R65_CH2_LVDS_EN);
			break;
		}
		break;
	case CDCE6214_CLK_OUT4:
		fmt = cdce6214_get_clkout_fmt(priv, np);
		switch (fmt) {
		case CDCE6214_CLKOUT_FMT_CMOS:
			ret = cdce6214_set_cmos_mode(priv, np, 75);
			if (ret)
				return ret;
			regmap_clear_bits(reg, 75, R75_CH4_LVDS_EN);
			regmap_clear_bits(reg, 73, R73_CH4_LPHCSL_EN);
			break;
		case CDCE6214_CLKOUT_FMT_LVDS:
			regmap_clear_bits(reg, 73, R73_CH4_LPHCSL_EN);
			regmap_set_bits(reg, 75, R75_CH4_LVDS_EN);
			break;
		case CDCE6214_CLKOUT_FMT_LPHCSL:
			regmap_clear_bits(reg, 75, R75_CH4_LVDS_EN);
			regmap_set_bits(reg, 72, R73_CH4_LPHCSL_EN);
			break;
		default:
			goto err_illegal_fmt;
		}
		break;
	case CDCE6214_CLK_PRIREF:
		fmt = cdce6214_get_clkin_fmt(priv, np);
		switch (fmt) {
		case CDCE6214_CLKIN_FMT_CMOS:
			regmap_clear_bits(reg, 24, R24_IP_PRIREF_BUF_SEL);
			break;
		case CDCE6214_CLKIN_FMT_DIFF:
			regmap_set_bits(reg, 24, R24_IP_PRIREF_BUF_SEL);
			break;
		case CDCE6214_CLKIN_FMT_XTAL: /* XTAL not allowed for PRIREF */
		default:
			goto err_illegal_fmt;
		}
		break;
	case CDCE6214_CLK_SECREF:
		fmt = cdce6214_get_clkin_fmt(priv, np);
		switch (fmt) {
		case CDCE6214_CLKIN_FMT_CMOS:
			val = R24_IP_SECREF_BUF_SEL_LVCMOS;
			break;
		case CDCE6214_CLKIN_FMT_XTAL:
			val = R24_IP_SECREF_BUF_SEL_XTAL;
			cdce6214_setup_xtal(priv, np);
			break;
		case CDCE6214_CLKIN_FMT_DIFF:
			val = R24_IP_SECREF_BUF_SEL_DIFF;
			break;
		default:
			goto err_illegal_fmt;
		}

		regmap_update_bits(reg, 24, R24_IP_SECREF_BUF_SEL, val);

		break;
	}

	return 0;

err_illegal_fmt:
	if (fmt < 0)
		dev_err(priv->dev, "%pOF: missing required property\n", np);
	else
		dev_err(priv->dev, "%pOF: illegal format %u\n", np, fmt);

	return -EINVAL;
}

static int cdce6214_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *child;
	struct cdce6214 *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->dev = dev;
	i2c_set_clientdata(client, priv);
	dev_set_drvdata(dev, priv);

	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio)) {
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				     "failed to get reset gpio\n");
	}

	priv->regmap = devm_regmap_init_i2c(client, &cdce6214_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	ret = cdce6214_configure(priv);
	if (ret)
		return ret;

	for_each_child_of_node(dev->of_node, child) {
		ret = cdce6214_parse_subnode(priv, child);
		if (ret)
			return ret;
	}

	ret = cdce6214_clk_register(priv);
	if (ret)
		return ret;

	ret = devm_of_clk_add_hw_provider(dev, cdce6214_of_clk_get, priv);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id cdce6214_ids[] = {
	{
		.compatible = "ti,cdce6214",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, cdce6214_ids);

static struct i2c_driver cdce6214_driver = {
	.driver = {
		.name = "cdce6214",
		.of_match_table = cdce6214_ids,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = cdce6214_probe,
};
module_i2c_driver(cdce6214_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_DESCRIPTION("TI CDCE6214 driver");
MODULE_LICENSE("GPL");
