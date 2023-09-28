// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Linaro Ltd.
 */

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/phy/phy.h>

#include "phy-qcom-qmp-hdmi.h"
#include "phy-qcom-qmp-qserdes-com.h"
#include "phy-qcom-qmp-qserdes-txrx.h"

#define HDMI_VCO_MAX_FREQ			12000000000UL
#define HDMI_VCO_MIN_FREQ			8000000000UL

#define HDMI_PCLK_MAX_FREQ			600000000UL
#define HDMI_PCLK_MIN_FREQ			25000000UL

#define HDMI_HIGH_FREQ_BIT_CLK_THRESHOLD	3400000000UL
#define HDMI_DIG_FREQ_BIT_CLK_THRESHOLD		1500000000UL
#define HDMI_MID_FREQ_BIT_CLK_THRESHOLD		750000000UL
#define HDMI_DEFAULT_REF_CLOCK			19200000
#define HDMI_PLL_CMP_CNT			1024

#define HDMI_PLL_POLL_MAX_READS			100
#define HDMI_PLL_POLL_TIMEOUT_US		150

#define REG_HDMI_8996_PHY_CFG					0x00000000
#define REG_HDMI_8996_PHY_PD_CTL				0x00000004
#define REG_HDMI_8996_PHY_MODE					0x00000008
#define REG_HDMI_8996_PHY_MISR_CLEAR				0x0000000c
#define REG_HDMI_8996_PHY_TX0_TX1_BIST_CFG0			0x00000010
#define REG_HDMI_8996_PHY_TX0_TX1_BIST_CFG1			0x00000014
#define REG_HDMI_8996_PHY_TX0_TX1_PRBS_SEED_BYTE0		0x00000018
#define REG_HDMI_8996_PHY_TX0_TX1_PRBS_SEED_BYTE1		0x0000001c
#define REG_HDMI_8996_PHY_TX0_TX1_BIST_PATTERN0			0x00000020
#define REG_HDMI_8996_PHY_TX0_TX1_BIST_PATTERN1			0x00000024
#define REG_HDMI_8996_PHY_TX2_TX3_BIST_CFG0			0x00000028
#define REG_HDMI_8996_PHY_TX2_TX3_BIST_CFG1			0x0000002c
#define REG_HDMI_8996_PHY_TX2_TX3_PRBS_SEED_BYTE0		0x00000030
#define REG_HDMI_8996_PHY_TX2_TX3_PRBS_SEED_BYTE1		0x00000034
#define REG_HDMI_8996_PHY_TX2_TX3_BIST_PATTERN0			0x00000038
#define REG_HDMI_8996_PHY_TX2_TX3_BIST_PATTERN1			0x0000003c
#define REG_HDMI_8996_PHY_DEBUG_BUS_SEL				0x00000040
#define REG_HDMI_8996_PHY_TXCAL_CFG0				0x00000044
#define REG_HDMI_8996_PHY_TXCAL_CFG1				0x00000048
#define REG_HDMI_8996_PHY_TX0_TX1_LANE_CTL			0x0000004c
#define REG_HDMI_8996_PHY_TX2_TX3_LANE_CTL			0x00000050
#define REG_HDMI_8996_PHY_LANE_BIST_CONFIG			0x00000054
#define REG_HDMI_8996_PHY_CLOCK					0x00000058
#define REG_HDMI_8996_PHY_MISC1					0x0000005c
#define REG_HDMI_8996_PHY_MISC2					0x00000060
#define REG_HDMI_8996_PHY_TX0_TX1_BIST_STATUS0			0x00000064
#define REG_HDMI_8996_PHY_TX0_TX1_BIST_STATUS1			0x00000068
#define REG_HDMI_8996_PHY_TX0_TX1_BIST_STATUS2			0x0000006c
#define REG_HDMI_8996_PHY_TX2_TX3_BIST_STATUS0			0x00000070
#define REG_HDMI_8996_PHY_TX2_TX3_BIST_STATUS1			0x00000074
#define REG_HDMI_8996_PHY_TX2_TX3_BIST_STATUS2			0x00000078
#define REG_HDMI_8996_PHY_PRE_MISR_STATUS0			0x0000007c
#define REG_HDMI_8996_PHY_PRE_MISR_STATUS1			0x00000080
#define REG_HDMI_8996_PHY_PRE_MISR_STATUS2			0x00000084
#define REG_HDMI_8996_PHY_PRE_MISR_STATUS3			0x00000088
#define REG_HDMI_8996_PHY_POST_MISR_STATUS0			0x0000008c
#define REG_HDMI_8996_PHY_POST_MISR_STATUS1			0x00000090
#define REG_HDMI_8996_PHY_POST_MISR_STATUS2			0x00000094
#define REG_HDMI_8996_PHY_POST_MISR_STATUS3			0x00000098
#define REG_HDMI_8996_PHY_STATUS				0x0000009c
#define REG_HDMI_8996_PHY_MISC3_STATUS				0x000000a0
#define REG_HDMI_8996_PHY_MISC4_STATUS				0x000000a4
#define REG_HDMI_8996_PHY_DEBUG_BUS0				0x000000a8
#define REG_HDMI_8996_PHY_DEBUG_BUS1				0x000000ac
#define REG_HDMI_8996_PHY_DEBUG_BUS2				0x000000b0
#define REG_HDMI_8996_PHY_DEBUG_BUS3				0x000000b4
#define REG_HDMI_8996_PHY_PHY_REVISION_ID0			0x000000b8
#define REG_HDMI_8996_PHY_PHY_REVISION_ID1			0x000000bc
#define REG_HDMI_8996_PHY_PHY_REVISION_ID2			0x000000c0
#define REG_HDMI_8996_PHY_PHY_REVISION_ID3			0x000000c4

struct qmp_hdmi_8996_post_divider {
	u64 vco_freq;
	int hsclk_divsel;
	int vco_ratio;
	int tx_band_sel;
};

static inline u32 qmp_hdmi_8996_pll_get_pll_cmp(u64 fdata, unsigned long ref_clk)
{
	u64 dividend = HDMI_PLL_CMP_CNT * fdata;
	u32 divisor = ref_clk * 10;
	u32 rem;

	rem = do_div(dividend, divisor);
	if (rem > (divisor >> 1))
		dividend++;

	return dividend - 1;
}

static int qmp_hdmi_8996_pll_get_post_div(struct qmp_hdmi_8996_post_divider *pd, u64 bclk)
{
	int ratio[] = { 2, 3, 4, 5, 6, 9, 10, 12, 14, 15, 20, 21, 25, 28, 35 };
	int hs_divsel[] = { 0, 4, 8, 12, 1, 5, 2, 9, 3, 13, 10, 7, 14, 11, 15 };
	int tx_band_sel[] = { 0, 1, 2, 3 };
	u64 vco_freq[60];
	u64 vco, vco_optimal;
	int half_rate_mode = 0;
	int vco_optimal_index, vco_freq_index;
	int i, j;

retry:
	vco_optimal = HDMI_VCO_MAX_FREQ;
	vco_optimal_index = -1;
	vco_freq_index = 0;
	for (i = 0; i < 15; i++) {
		for (j = 0; j < 4; j++) {
			u32 ratio_mult = ratio[i] << tx_band_sel[j];

			vco = bclk >> half_rate_mode;
			vco *= ratio_mult;
			vco_freq[vco_freq_index++] = vco;
		}
	}

	for (i = 0; i < 60; i++) {
		u64 vco_tmp = vco_freq[i];

		if ((vco_tmp >= HDMI_VCO_MIN_FREQ) &&
		    (vco_tmp <= vco_optimal)) {
			vco_optimal = vco_tmp;
			vco_optimal_index = i;
		}
	}

	if (vco_optimal_index == -1) {
		if (!half_rate_mode) {
			half_rate_mode = 1;
			goto retry;
		}

		return -EINVAL;
	}

	pd->vco_freq = vco_optimal;
	pd->tx_band_sel = tx_band_sel[vco_optimal_index % 4];
	pd->vco_ratio = ratio[vco_optimal_index / 4];
	pd->hsclk_divsel = hs_divsel[vco_optimal_index / 4];

	return 0;
}

static int qmp_hdmi_8996_phy_set_rate(struct qmp_hdmi_phy *hdmi_phy)
{
	unsigned long parent_rate = HDMI_DEFAULT_REF_CLOCK;
	unsigned long rate = hdmi_phy->hdmi_opts.pixel_clk_rate * 1000;
	struct qmp_hdmi_8996_post_divider pd;
	bool gen_ssc = false;
	u64 bclk;
	u64 dec_start;
	u64 frac_start;
	u64 fdata;
	u32 pll_divisor;
	u32 rem;
	u32 integloop_gain;
	u32 pll_cmp;
	int i, ret;

	bclk = ((u64)rate) * 10;
	ret = qmp_hdmi_8996_pll_get_post_div(&pd, bclk);
	if (ret) {
		dev_err(hdmi_phy->dev, "PLL calculation failed\n");
		return ret;
	}

	dec_start = pd.vco_freq;
	pll_divisor = 4 * parent_rate;
	do_div(dec_start, pll_divisor);

	frac_start = pd.vco_freq * (1 << 20);

	rem = do_div(frac_start, pll_divisor);
	frac_start -= dec_start * (1 << 20);
	if (rem > (pll_divisor >> 1))
		frac_start++;

	fdata = pd.vco_freq;
	do_div(fdata, pd.vco_ratio);

	pll_cmp = qmp_hdmi_8996_pll_get_pll_cmp(fdata, parent_rate);

	/* Initially shut down PHY */
	dev_dbg(hdmi_phy->dev, "Disabling PHY");
	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_PD_CTL, 0x0);
	udelay(500);

	/* Power up sequence */
	hdmi_pll_write(hdmi_phy, QSERDES_COM_BG_CTRL, 0x04);

	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_PD_CTL, 0x1);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_RESETSM_CNTRL, 0x20);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_TX0_TX1_LANE_CTL, 0x0f);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_TX2_TX3_LANE_CTL, 0x0f);

	hdmi_tx_chan_write(hdmi_phy, 0, QSERDES_TX_LANE_MODE, 0x43);
	hdmi_tx_chan_write(hdmi_phy, 2, QSERDES_TX_LANE_MODE, 0x43);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_SYSCLK_BUF_ENABLE, 0x1e);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_BIAS_EN_CLKBUFLR_EN, 0x07);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_SYSCLK_EN_SEL, 0x37);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_SYS_CLK_CTRL, 0x02);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_CLK_ENABLE1, 0x0e);

	if (frac_start != 0 || gen_ssc) {
		hdmi_pll_write(hdmi_phy, QSERDES_COM_PLL_CCTRL_MODE0, 0x28);
		hdmi_pll_write(hdmi_phy, QSERDES_COM_PLL_RCTRL_MODE0, 0x16);
		hdmi_pll_write(hdmi_phy, QSERDES_COM_CP_CTRL_MODE0,
			       11000000 / (parent_rate/ 20));
		integloop_gain = (64 * parent_rate) / HDMI_DEFAULT_REF_CLOCK;
	} else {
		hdmi_pll_write(hdmi_phy, QSERDES_COM_PLL_CCTRL_MODE0, 0x01);
		hdmi_pll_write(hdmi_phy, QSERDES_COM_PLL_RCTRL_MODE0, 0x10);
		hdmi_pll_write(hdmi_phy, QSERDES_COM_CP_CTRL_MODE0, 0x23);
		integloop_gain = (1022 * parent_rate) / (100 * 1000 * 1000);
	}

	/* Bypass VCO calibration */
	if (bclk > HDMI_DIG_FREQ_BIT_CLK_THRESHOLD) {
		hdmi_pll_write(hdmi_phy, QSERDES_COM_SVS_MODE_CLK_SEL, 1);
		integloop_gain <<= 1;
	} else {
		hdmi_pll_write(hdmi_phy, QSERDES_COM_SVS_MODE_CLK_SEL, 2);
		integloop_gain <<= 2;
	}

	integloop_gain = min_t(u32, integloop_gain, 2046);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_BG_TRIM, 0x0f);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_PLL_IVCO, 0x0f);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_VCO_TUNE_CTRL, 0);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_BG_CTRL, 0x06);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_CLK_SELECT, 0x30);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_HSCLK_SEL, 0x20 | pd.hsclk_divsel);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_LOCK_CMP_EN, 0x0);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_DEC_START_MODE0, dec_start);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_DIV_FRAC_START1_MODE0,
		       frac_start & 0xff);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_DIV_FRAC_START2_MODE0,
		       (frac_start >> 8) & 0xff);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_DIV_FRAC_START3_MODE0,
		       (frac_start >> 16) & 0xf);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_INTEGLOOP_GAIN0_MODE0,
		       integloop_gain & 0xff);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_INTEGLOOP_GAIN1_MODE0,
		       (integloop_gain >> 8) & 0xff);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_LOCK_CMP1_MODE0,
		       pll_cmp & 0xff);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_LOCK_CMP2_MODE0,
		       (pll_cmp >> 8) & 0xff);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_LOCK_CMP3_MODE0,
		       (pll_cmp >> 16) & 0x3);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_VCO_TUNE_MAP, 0x00);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_CORE_CLK_EN, 0x2c);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_CORECLK_DIV, 5);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_CMN_CONFIG, 0x02);

	hdmi_pll_write(hdmi_phy, QSERDES_COM_RESCODE_DIV_NUM, 0x15);

	/* TX lanes setup (TX 0/1/2/3) */
	for (i = 0; i < HDMI_NUM_TX_CHANNEL; i++) {
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_CLKBUF_ENABLE, 0x03);
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TX_BAND, pd.tx_band_sel + 4);
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_RESET_TSYNC_EN, 0x03);
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_VMODE_CTRL1, 0x00);
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TX_DRV_LVL_OFFSET, 0x00);
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_RES_CODE_LANE_OFFSET, 0x00);
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TRAN_DRVR_EMP_EN, 0x03);
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_PARRATE_REC_DETECT_IDLE_EN, 0x40);
		hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_HP_PD_ENABLES,
				   i != 3 ? 0xc : 0x3);
	}

	if (bclk > HDMI_HIGH_FREQ_BIT_CLK_THRESHOLD) {
		for (i = 0; i < HDMI_NUM_TX_CHANNEL; i++) {
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TX_DRV_LVL,
					   i != 3 ? 0x25 : 0x22);
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TX_EMP_POST1_LVL,
					   i != 3 ? 0x23 : 0x27);
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_VMODE_CTRL2,
					   i != 3 ? 0x0d : 0x00);
		}
	} else if (bclk > HDMI_MID_FREQ_BIT_CLK_THRESHOLD) {
		for (i = 0; i < HDMI_NUM_TX_CHANNEL; i++) {
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TX_DRV_LVL, 0x25);
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TX_EMP_POST1_LVL, 0x23);
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_VMODE_CTRL2,
					   i != 3 ? 0x0d : 0x00);
		}
	} else {
		for (i = 0; i < HDMI_NUM_TX_CHANNEL; i++) {
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TX_DRV_LVL, 0x20);
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_TX_EMP_POST1_LVL, 0x20);
			hdmi_tx_chan_write(hdmi_phy, i, QSERDES_TX_VMODE_CTRL2, 0x0e);
		}
	}

	if (bclk > HDMI_HIGH_FREQ_BIT_CLK_THRESHOLD)
		hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_MODE, 0x10);
	else
		hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_MODE, 0x00);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_PD_CTL, 0x1f);

	return 0;
}

static int qmp_hdmi_8996_phy_power_on(struct phy *phy)
{
	struct qmp_hdmi_phy *hdmi_phy = phy_get_drvdata(phy);
	u32 status;
	int i, ret = 0;

	ret = qmp_hdmi_8996_phy_set_rate(hdmi_phy);
	if (ret) {
		dev_err(hdmi_phy->dev, "Setting pixel clock rate failed\n");
		return ret;
	}

	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_CFG, 0x1);
	udelay(100);

	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_CFG, 0x19);
	udelay(100);

	ret = readl_poll_timeout(hdmi_phy->serdes + QSERDES_COM_C_READY_STATUS,
				 status, status & BIT(0),
				 HDMI_PLL_POLL_TIMEOUT_US,
				 HDMI_PLL_POLL_MAX_READS * HDMI_PLL_POLL_TIMEOUT_US);

	if (ret) {
		dev_warn(hdmi_phy->dev, "HDMI PLL is not locked\n");
		return ret;
	}

	for (i = 0; i < HDMI_NUM_TX_CHANNEL; i++)
		hdmi_tx_chan_write(hdmi_phy, i,
				   QSERDES_TX_HIGHZ_TRANSCEIVEREN_BIAS_DRVR_EN,
				   0x6f);

	/* Disable SSC */
	hdmi_pll_write(hdmi_phy, QSERDES_COM_SSC_PER1, 0x0);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_SSC_PER2, 0x0);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_SSC_STEP_SIZE1, 0x0);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_SSC_STEP_SIZE2, 0x0);
	hdmi_pll_write(hdmi_phy, QSERDES_COM_SSC_EN_CENTER, 0x2);

	ret = readl_poll_timeout(hdmi_phy->phy_reg + REG_HDMI_8996_PHY_STATUS,
				 status, status & BIT(0),
				 HDMI_PLL_POLL_TIMEOUT_US,
				 HDMI_PLL_POLL_MAX_READS * HDMI_PLL_POLL_TIMEOUT_US);
	if (ret) {
		dev_warn(hdmi_phy->dev, "HDMI PLL is not locked\n");
		return ret;
	}

	/* Restart the retiming buffer */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_CFG, 0x18);
	udelay(1);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_CFG, 0x19);

	return 0;
}

static int qmp_hdmi_8996_phy_power_off(struct phy *phy)
{
	struct qmp_hdmi_phy *hdmi_phy = phy_get_drvdata(phy);

	hdmi_phy_write(hdmi_phy, REG_HDMI_8996_PHY_CFG, 0x6);
	usleep_range(100, 150);

	return 0;
}

static long qmp_hdmi_8996_pll_round_rate(struct clk_hw *hw,
				     unsigned long rate,
				     unsigned long *parent_rate)
{
	return clamp(rate, HDMI_PCLK_MIN_FREQ, HDMI_PCLK_MAX_FREQ);
}

static unsigned long qmp_hdmi_8996_pll_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct qmp_hdmi_phy *phy = hw_clk_to_pll(hw);
	u32 cmp1, cmp2, cmp3, pll_cmp;

	cmp1 = hdmi_pll_read(phy, QSERDES_COM_LOCK_CMP1_MODE0);
	cmp2 = hdmi_pll_read(phy, QSERDES_COM_LOCK_CMP2_MODE0);
	cmp3 = hdmi_pll_read(phy, QSERDES_COM_LOCK_CMP3_MODE0);

	pll_cmp = cmp1 | (cmp2 << 8) | (cmp3 << 16);

	return mult_frac(pll_cmp + 1, parent_rate, HDMI_PLL_CMP_CNT);
}

static int qmp_hdmi_8996_pll_is_enabled(struct clk_hw *hw)
{
	struct qmp_hdmi_phy *phy = hw_clk_to_pll(hw);
	u32 status;
	int pll_locked;

	status = hdmi_pll_read(phy, QSERDES_COM_C_READY_STATUS);
	pll_locked = status & BIT(0);

	return pll_locked;
}

static const struct clk_ops qmp_hdmi_8996_pll_ops = {
	.recalc_rate = qmp_hdmi_8996_pll_recalc_rate,
	.round_rate = qmp_hdmi_8996_pll_round_rate,
	.is_enabled = qmp_hdmi_8996_pll_is_enabled,
};

static const struct phy_ops qmp_hdmi_8996_phy_ops = {
	.init		= qmp_hdmi_phy_init,
	.configure	= qmp_hdmi_phy_configure,
	.power_on	= qmp_hdmi_8996_phy_power_on,
	.power_off	= qmp_hdmi_8996_phy_power_off,
	.exit		= qmp_hdmi_phy_exit,
	.owner		= THIS_MODULE,
};

const struct qmp_hdmi_cfg qmp_hdmi_8996_cfg = {
	.pll_ops = &qmp_hdmi_8996_pll_ops,
	.phy_ops = &qmp_hdmi_8996_phy_ops,
};
