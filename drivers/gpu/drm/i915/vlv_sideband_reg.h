/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _VLV_SIDEBAND_REG_H_
#define _VLV_SIDEBAND_REG_H_

#include "i915_reg_defs.h"
#include "display/intel_display_reg_defs.h"

/*
 * IOSF sideband
 */
#define VLV_IOSF_DOORBELL_REQ			_MMIO(VLV_DISPLAY_BASE + 0x2100)
#define   IOSF_DEVFN_SHIFT			24
#define   IOSF_OPCODE_SHIFT			16
#define   IOSF_PORT_SHIFT			8
#define   IOSF_BYTE_ENABLES_SHIFT		4
#define   IOSF_BAR_SHIFT			1
#define   IOSF_SB_BUSY				(1 << 0)
#define   IOSF_PORT_BUNIT			0x03
#define   IOSF_PORT_PUNIT			0x04
#define   IOSF_PORT_NC				0x11
#define   IOSF_PORT_DPIO			0x12
#define   IOSF_PORT_GPIO_NC			0x13
#define   IOSF_PORT_CCK				0x14
#define   IOSF_PORT_DPIO_2			0x1a
#define   IOSF_PORT_FLISDSI			0x1b
#define   IOSF_PORT_GPIO_SC			0x48
#define   IOSF_PORT_GPIO_SUS			0xa8
#define   IOSF_PORT_CCU				0xa9
#define   CHV_IOSF_PORT_GPIO_N			0x13
#define   CHV_IOSF_PORT_GPIO_SE			0x48
#define   CHV_IOSF_PORT_GPIO_E			0xa8
#define   CHV_IOSF_PORT_GPIO_SW			0xb2
#define VLV_IOSF_DATA				_MMIO(VLV_DISPLAY_BASE + 0x2104)
#define VLV_IOSF_ADDR				_MMIO(VLV_DISPLAY_BASE + 0x2108)

/* DPIO registers */
#define DPIO_DEVFN			0

/* See configdb bunit SB addr map */
#define BUNIT_REG_BISOC				0x11

/* PUNIT_REG_*SSPM0 */
#define   _SSPM0_SSC(val)			((val) << 0)
#define   SSPM0_SSC_MASK			_SSPM0_SSC(0x3)
#define   SSPM0_SSC_PWR_ON			_SSPM0_SSC(0x0)
#define   SSPM0_SSC_CLK_GATE			_SSPM0_SSC(0x1)
#define   SSPM0_SSC_RESET			_SSPM0_SSC(0x2)
#define   SSPM0_SSC_PWR_GATE			_SSPM0_SSC(0x3)
#define   _SSPM0_SSS(val)			((val) << 24)
#define   SSPM0_SSS_MASK			_SSPM0_SSS(0x3)
#define   SSPM0_SSS_PWR_ON			_SSPM0_SSS(0x0)
#define   SSPM0_SSS_CLK_GATE			_SSPM0_SSS(0x1)
#define   SSPM0_SSS_RESET			_SSPM0_SSS(0x2)
#define   SSPM0_SSS_PWR_GATE			_SSPM0_SSS(0x3)

/* PUNIT_REG_*SSPM1 */
#define   SSPM1_FREQSTAT_SHIFT			24
#define   SSPM1_FREQSTAT_MASK			(0x1f << SSPM1_FREQSTAT_SHIFT)
#define   SSPM1_FREQGUAR_SHIFT			8
#define   SSPM1_FREQGUAR_MASK			(0x1f << SSPM1_FREQGUAR_SHIFT)
#define   SSPM1_FREQ_SHIFT			0
#define   SSPM1_FREQ_MASK			(0x1f << SSPM1_FREQ_SHIFT)

#define PUNIT_REG_VEDSSPM0			0x32
#define PUNIT_REG_VEDSSPM1			0x33

#define PUNIT_REG_DSPSSPM			0x36
#define   DSPFREQSTAT_SHIFT_CHV			24
#define   DSPFREQSTAT_MASK_CHV			(0x1f << DSPFREQSTAT_SHIFT_CHV)
#define   DSPFREQGUAR_SHIFT_CHV			8
#define   DSPFREQGUAR_MASK_CHV			(0x1f << DSPFREQGUAR_SHIFT_CHV)
#define   DSPFREQSTAT_SHIFT			30
#define   DSPFREQSTAT_MASK			(0x3 << DSPFREQSTAT_SHIFT)
#define   DSPFREQGUAR_SHIFT			14
#define   DSPFREQGUAR_MASK			(0x3 << DSPFREQGUAR_SHIFT)
#define   DSP_MAXFIFO_PM5_STATUS		(1 << 22) /* chv */
#define   DSP_AUTO_CDCLK_GATE_DISABLE		(1 << 7) /* chv */
#define   DSP_MAXFIFO_PM5_ENABLE		(1 << 6) /* chv */
#define   _DP_SSC(val, pipe)			((val) << (2 * (pipe)))
#define   DP_SSC_MASK(pipe)			_DP_SSC(0x3, (pipe))
#define   DP_SSC_PWR_ON(pipe)			_DP_SSC(0x0, (pipe))
#define   DP_SSC_CLK_GATE(pipe)			_DP_SSC(0x1, (pipe))
#define   DP_SSC_RESET(pipe)			_DP_SSC(0x2, (pipe))
#define   DP_SSC_PWR_GATE(pipe)			_DP_SSC(0x3, (pipe))
#define   _DP_SSS(val, pipe)			((val) << (2 * (pipe) + 16))
#define   DP_SSS_MASK(pipe)			_DP_SSS(0x3, (pipe))
#define   DP_SSS_PWR_ON(pipe)			_DP_SSS(0x0, (pipe))
#define   DP_SSS_CLK_GATE(pipe)			_DP_SSS(0x1, (pipe))
#define   DP_SSS_RESET(pipe)			_DP_SSS(0x2, (pipe))
#define   DP_SSS_PWR_GATE(pipe)			_DP_SSS(0x3, (pipe))

#define PUNIT_REG_ISPSSPM0			0x39
#define PUNIT_REG_ISPSSPM1			0x3a

#define PUNIT_REG_PWRGT_CTRL			0x60
#define PUNIT_REG_PWRGT_STATUS			0x61
#define   PUNIT_PWRGT_MASK(pw_idx)		(3 << ((pw_idx) * 2))
#define   PUNIT_PWRGT_PWR_ON(pw_idx)		(0 << ((pw_idx) * 2))
#define   PUNIT_PWRGT_CLK_GATE(pw_idx)		(1 << ((pw_idx) * 2))
#define   PUNIT_PWRGT_RESET(pw_idx)		(2 << ((pw_idx) * 2))
#define   PUNIT_PWRGT_PWR_GATE(pw_idx)		(3 << ((pw_idx) * 2))

#define PUNIT_PWGT_IDX_RENDER			0
#define PUNIT_PWGT_IDX_MEDIA			1
#define PUNIT_PWGT_IDX_DISP2D			3
#define PUNIT_PWGT_IDX_DPIO_CMN_BC		5
#define PUNIT_PWGT_IDX_DPIO_TX_B_LANES_01	6
#define PUNIT_PWGT_IDX_DPIO_TX_B_LANES_23	7
#define PUNIT_PWGT_IDX_DPIO_TX_C_LANES_01	8
#define PUNIT_PWGT_IDX_DPIO_TX_C_LANES_23	9
#define PUNIT_PWGT_IDX_DPIO_RX0			10
#define PUNIT_PWGT_IDX_DPIO_RX1			11
#define PUNIT_PWGT_IDX_DPIO_CMN_D		12

#define PUNIT_REG_GPU_LFM			0xd3
#define PUNIT_REG_GPU_FREQ_REQ			0xd4
#define PUNIT_REG_GPU_FREQ_STS			0xd8
#define   GPLLENABLE				(1 << 4)
#define   GENFREQSTATUS				(1 << 0)
#define PUNIT_REG_MEDIA_TURBO_FREQ_REQ		0xdc
#define PUNIT_REG_CZ_TIMESTAMP			0xce

#define PUNIT_FUSE_BUS2				0xf6 /* bits 47:40 */
#define PUNIT_FUSE_BUS1				0xf5 /* bits 55:48 */

#define FB_GFX_FMAX_AT_VMAX_FUSE		0x136
#define FB_GFX_FREQ_FUSE_MASK			0xff
#define FB_GFX_FMAX_AT_VMAX_2SS4EU_FUSE_SHIFT	24
#define FB_GFX_FMAX_AT_VMAX_2SS6EU_FUSE_SHIFT	16
#define FB_GFX_FMAX_AT_VMAX_2SS8EU_FUSE_SHIFT	8

#define FB_GFX_FMIN_AT_VMIN_FUSE		0x137
#define FB_GFX_FMIN_AT_VMIN_FUSE_SHIFT		8

#define PUNIT_REG_DDR_SETUP2			0x139
#define   FORCE_DDR_FREQ_REQ_ACK		(1 << 8)
#define   FORCE_DDR_LOW_FREQ			(1 << 1)
#define   FORCE_DDR_HIGH_FREQ			(1 << 0)

#define PUNIT_GPU_STATUS_REG			0xdb
#define PUNIT_GPU_STATUS_MAX_FREQ_SHIFT	16
#define PUNIT_GPU_STATUS_MAX_FREQ_MASK		0xff
#define PUNIT_GPU_STATIS_GFX_MIN_FREQ_SHIFT	8
#define PUNIT_GPU_STATUS_GFX_MIN_FREQ_MASK	0xff

#define PUNIT_GPU_DUTYCYCLE_REG		0xdf
#define PUNIT_GPU_DUTYCYCLE_RPE_FREQ_SHIFT	8
#define PUNIT_GPU_DUTYCYCLE_RPE_FREQ_MASK	0xff

#define IOSF_NC_FB_GFX_FREQ_FUSE		0x1c
#define   FB_GFX_MAX_FREQ_FUSE_SHIFT		3
#define   FB_GFX_MAX_FREQ_FUSE_MASK		0x000007f8
#define   FB_GFX_FGUARANTEED_FREQ_FUSE_SHIFT	11
#define   FB_GFX_FGUARANTEED_FREQ_FUSE_MASK	0x0007f800
#define IOSF_NC_FB_GFX_FMAX_FUSE_HI		0x34
#define   FB_FMAX_VMIN_FREQ_HI_MASK		0x00000007
#define IOSF_NC_FB_GFX_FMAX_FUSE_LO		0x30
#define   FB_FMAX_VMIN_FREQ_LO_SHIFT		27
#define   FB_FMAX_VMIN_FREQ_LO_MASK		0xf8000000

#define VLV_TURBO_SOC_OVERRIDE		0x04
#define   VLV_OVERRIDE_EN		1
#define   VLV_SOC_TDP_EN		(1 << 1)
#define   VLV_BIAS_CPU_125_SOC_875	(6 << 2)
#define   CHV_BIAS_CPU_50_SOC_50	(3 << 2)

/* vlv2 north clock has */
#define CCK_FUSE_REG				0x8
#define  CCK_FUSE_HPLL_FREQ_MASK		0x3
#define CCK_REG_DSI_PLL_FUSE			0x44
#define CCK_REG_DSI_PLL_CONTROL			0x48
#define  DSI_PLL_VCO_EN				(1 << 31)
#define  DSI_PLL_LDO_GATE			(1 << 30)
#define  DSI_PLL_P1_POST_DIV_SHIFT		17
#define  DSI_PLL_P1_POST_DIV_MASK		(0x1ff << 17)
#define  DSI_PLL_P2_MUX_DSI0_DIV2		(1 << 13)
#define  DSI_PLL_P3_MUX_DSI1_DIV2		(1 << 12)
#define  DSI_PLL_MUX_MASK			(3 << 9)
#define  DSI_PLL_MUX_DSI0_DSIPLL		(0 << 10)
#define  DSI_PLL_MUX_DSI0_CCK			(1 << 10)
#define  DSI_PLL_MUX_DSI1_DSIPLL		(0 << 9)
#define  DSI_PLL_MUX_DSI1_CCK			(1 << 9)
#define  DSI_PLL_CLK_GATE_MASK			(0xf << 5)
#define  DSI_PLL_CLK_GATE_DSI0_DSIPLL		(1 << 8)
#define  DSI_PLL_CLK_GATE_DSI1_DSIPLL		(1 << 7)
#define  DSI_PLL_CLK_GATE_DSI0_CCK		(1 << 6)
#define  DSI_PLL_CLK_GATE_DSI1_CCK		(1 << 5)
#define  DSI_PLL_LOCK				(1 << 0)
#define CCK_REG_DSI_PLL_DIVIDER			0x4c
#define  DSI_PLL_LFSR				(1 << 31)
#define  DSI_PLL_FRACTION_EN			(1 << 30)
#define  DSI_PLL_FRAC_COUNTER_SHIFT		27
#define  DSI_PLL_FRAC_COUNTER_MASK		(7 << 27)
#define  DSI_PLL_USYNC_CNT_SHIFT		18
#define  DSI_PLL_USYNC_CNT_MASK			(0x1ff << 18)
#define  DSI_PLL_N1_DIV_SHIFT			16
#define  DSI_PLL_N1_DIV_MASK			(3 << 16)
#define  DSI_PLL_M1_DIV_SHIFT			0
#define  DSI_PLL_M1_DIV_MASK			(0x1ff << 0)
#define CCK_CZ_CLOCK_CONTROL			0x62
#define CCK_GPLL_CLOCK_CONTROL			0x67
#define CCK_DISPLAY_CLOCK_CONTROL		0x6b
#define CCK_DISPLAY_REF_CLOCK_CONTROL		0x6c
#define  CCK_TRUNK_FORCE_ON			(1 << 17)
#define  CCK_TRUNK_FORCE_OFF			(1 << 16)
#define  CCK_FREQUENCY_STATUS			(0x1f << 8)
#define  CCK_FREQUENCY_STATUS_SHIFT		8
#define  CCK_FREQUENCY_VALUES			(0x1f << 0)

/*
 * Per pipe/PLL DPIO regs
 */
#define _VLV_PLL_DW3_CH0		0x800c
#define   DPIO_POST_DIV_SHIFT		(28) /* 3 bits */
#define   DPIO_POST_DIV_DAC		0
#define   DPIO_POST_DIV_HDMIDP		1 /* DAC 225-400M rate */
#define   DPIO_POST_DIV_LVDS1		2
#define   DPIO_POST_DIV_LVDS2		3
#define   DPIO_K_SHIFT			(24) /* 4 bits */
#define   DPIO_P1_SHIFT			(21) /* 3 bits */
#define   DPIO_P2_SHIFT			(16) /* 5 bits */
#define   DPIO_N_SHIFT			(12) /* 4 bits */
#define   DPIO_ENABLE_CALIBRATION	(1 << 11)
#define   DPIO_M1DIV_SHIFT		(8) /* 3 bits */
#define   DPIO_M2DIV_MASK		0xff
#define _VLV_PLL_DW3_CH1		0x802c
#define VLV_PLL_DW3(ch) _PIPE(ch, _VLV_PLL_DW3_CH0, _VLV_PLL_DW3_CH1)

#define _VLV_PLL_DW5_CH0		0x8014
#define   DPIO_REFSEL_OVERRIDE		27
#define   DPIO_PLL_MODESEL_SHIFT	24 /* 3 bits */
#define   DPIO_BIAS_CURRENT_CTL_SHIFT	21 /* 3 bits, always 0x7 */
#define   DPIO_PLL_REFCLK_SEL_SHIFT	16 /* 2 bits */
#define   DPIO_PLL_REFCLK_SEL_MASK	3
#define   DPIO_DRIVER_CTL_SHIFT		12 /* always set to 0x8 */
#define   DPIO_CLK_BIAS_CTL_SHIFT	8 /* always set to 0x5 */
#define _VLV_PLL_DW5_CH1		0x8034
#define VLV_PLL_DW5(ch) _PIPE(ch, _VLV_PLL_DW5_CH0, _VLV_PLL_DW5_CH1)

#define _VLV_PLL_DW7_CH0		0x801c
#define _VLV_PLL_DW7_CH1		0x803c
#define VLV_PLL_DW7(ch) _PIPE(ch, _VLV_PLL_DW7_CH0, _VLV_PLL_DW7_CH1)

#define _VLV_PLL_DW8_CH0		0x8040
#define _VLV_PLL_DW8_CH1		0x8060
#define VLV_PLL_DW8(ch) _PIPE(ch, _VLV_PLL_DW8_CH0, _VLV_PLL_DW8_CH1)

#define VLV_PLL_DW9_BCAST		0xc044
#define _VLV_PLL_DW9_CH0		0x8044
#define _VLV_PLL_DW9_CH1		0x8064
#define VLV_PLL_DW9(ch) _PIPE(ch, _VLV_PLL_DW9_CH0, _VLV_PLL_DW9_CH1)

#define _VLV_PLL_DW10_CH0		0x8048
#define _VLV_PLL_DW10_CH1		0x8068
#define VLV_PLL_DW10(ch) _PIPE(ch, _VLV_PLL_DW10_CH0, _VLV_PLL_DW10_CH1)

#define _VLV_PLL_DW11_CH0		0x804c
#define _VLV_PLL_DW11_CH1		0x806c
#define VLV_PLL_DW11(ch) _PIPE(ch, _VLV_PLL_DW11_CH0, _VLV_PLL_DW11_CH1)

/* Spec for ref block start counts at DW10 */
#define VLV_REF_DW13			0x80ac

#define VLV_CMN_DW0			0x8100

/*
 * Per DDI channel DPIO regs
 */

#define _VLV_PCS_DW0_CH0		0x8200
#define _VLV_PCS_DW0_CH1		0x8400
#define   DPIO_PCS_TX_LANE2_RESET	(1 << 16)
#define   DPIO_PCS_TX_LANE1_RESET	(1 << 7)
#define   DPIO_LEFT_TXFIFO_RST_MASTER2	(1 << 4)
#define   DPIO_RIGHT_TXFIFO_RST_MASTER2	(1 << 3)
#define VLV_PCS_DW0(ch) _PORT(ch, _VLV_PCS_DW0_CH0, _VLV_PCS_DW0_CH1)

#define _VLV_PCS01_DW0_CH0		0x200
#define _VLV_PCS23_DW0_CH0		0x400
#define _VLV_PCS01_DW0_CH1		0x2600
#define _VLV_PCS23_DW0_CH1		0x2800
#define VLV_PCS01_DW0(ch) _PORT(ch, _VLV_PCS01_DW0_CH0, _VLV_PCS01_DW0_CH1)
#define VLV_PCS23_DW0(ch) _PORT(ch, _VLV_PCS23_DW0_CH0, _VLV_PCS23_DW0_CH1)

#define _VLV_PCS_DW1_CH0		0x8204
#define _VLV_PCS_DW1_CH1		0x8404
#define   CHV_PCS_REQ_SOFTRESET_EN	(1 << 23)
#define   DPIO_PCS_CLK_CRI_RXEB_EIOS_EN	(1 << 22)
#define   DPIO_PCS_CLK_CRI_RXDIGFILTSG_EN (1 << 21)
#define   DPIO_PCS_CLK_DATAWIDTH_SHIFT	(6)
#define   DPIO_PCS_CLK_SOFT_RESET	(1 << 5)
#define VLV_PCS_DW1(ch) _PORT(ch, _VLV_PCS_DW1_CH0, _VLV_PCS_DW1_CH1)

#define _VLV_PCS01_DW1_CH0		0x204
#define _VLV_PCS23_DW1_CH0		0x404
#define _VLV_PCS01_DW1_CH1		0x2604
#define _VLV_PCS23_DW1_CH1		0x2804
#define VLV_PCS01_DW1(ch) _PORT(ch, _VLV_PCS01_DW1_CH0, _VLV_PCS01_DW1_CH1)
#define VLV_PCS23_DW1(ch) _PORT(ch, _VLV_PCS23_DW1_CH0, _VLV_PCS23_DW1_CH1)

#define _VLV_PCS_DW8_CH0		0x8220
#define _VLV_PCS_DW8_CH1		0x8420
#define   CHV_PCS_USEDCLKCHANNEL_OVRRIDE	(1 << 20)
#define   CHV_PCS_USEDCLKCHANNEL		(1 << 21)
#define VLV_PCS_DW8(ch) _PORT(ch, _VLV_PCS_DW8_CH0, _VLV_PCS_DW8_CH1)

#define _VLV_PCS01_DW8_CH0		0x0220
#define _VLV_PCS23_DW8_CH0		0x0420
#define _VLV_PCS01_DW8_CH1		0x2620
#define _VLV_PCS23_DW8_CH1		0x2820
#define VLV_PCS01_DW8(port) _PORT(port, _VLV_PCS01_DW8_CH0, _VLV_PCS01_DW8_CH1)
#define VLV_PCS23_DW8(port) _PORT(port, _VLV_PCS23_DW8_CH0, _VLV_PCS23_DW8_CH1)

#define _VLV_PCS_DW9_CH0		0x8224
#define _VLV_PCS_DW9_CH1		0x8424
#define   DPIO_PCS_TX2MARGIN_MASK	(0x7 << 13)
#define   DPIO_PCS_TX2MARGIN_000	(0 << 13)
#define   DPIO_PCS_TX2MARGIN_101	(1 << 13)
#define   DPIO_PCS_TX1MARGIN_MASK	(0x7 << 10)
#define   DPIO_PCS_TX1MARGIN_000	(0 << 10)
#define   DPIO_PCS_TX1MARGIN_101	(1 << 10)
#define	VLV_PCS_DW9(ch) _PORT(ch, _VLV_PCS_DW9_CH0, _VLV_PCS_DW9_CH1)

#define _VLV_PCS01_DW9_CH0		0x224
#define _VLV_PCS23_DW9_CH0		0x424
#define _VLV_PCS01_DW9_CH1		0x2624
#define _VLV_PCS23_DW9_CH1		0x2824
#define VLV_PCS01_DW9(ch) _PORT(ch, _VLV_PCS01_DW9_CH0, _VLV_PCS01_DW9_CH1)
#define VLV_PCS23_DW9(ch) _PORT(ch, _VLV_PCS23_DW9_CH0, _VLV_PCS23_DW9_CH1)

#define _CHV_PCS_DW10_CH0		0x8228
#define _CHV_PCS_DW10_CH1		0x8428
#define   DPIO_PCS_SWING_CALC_TX0_TX2	(1 << 30)
#define   DPIO_PCS_SWING_CALC_TX1_TX3	(1 << 31)
#define   DPIO_PCS_TX2DEEMP_MASK	(0xf << 24)
#define   DPIO_PCS_TX2DEEMP_9P5		(0 << 24)
#define   DPIO_PCS_TX2DEEMP_6P0		(2 << 24)
#define   DPIO_PCS_TX1DEEMP_MASK	(0xf << 16)
#define   DPIO_PCS_TX1DEEMP_9P5		(0 << 16)
#define   DPIO_PCS_TX1DEEMP_6P0		(2 << 16)
#define CHV_PCS_DW10(ch) _PORT(ch, _CHV_PCS_DW10_CH0, _CHV_PCS_DW10_CH1)

#define _VLV_PCS01_DW10_CH0		0x0228
#define _VLV_PCS23_DW10_CH0		0x0428
#define _VLV_PCS01_DW10_CH1		0x2628
#define _VLV_PCS23_DW10_CH1		0x2828
#define VLV_PCS01_DW10(port) _PORT(port, _VLV_PCS01_DW10_CH0, _VLV_PCS01_DW10_CH1)
#define VLV_PCS23_DW10(port) _PORT(port, _VLV_PCS23_DW10_CH0, _VLV_PCS23_DW10_CH1)

#define _VLV_PCS_DW11_CH0		0x822c
#define _VLV_PCS_DW11_CH1		0x842c
#define   DPIO_TX2_STAGGER_MASK(x)	((x) << 24)
#define   DPIO_LANEDESKEW_STRAP_OVRD	(1 << 3)
#define   DPIO_LEFT_TXFIFO_RST_MASTER	(1 << 1)
#define   DPIO_RIGHT_TXFIFO_RST_MASTER	(1 << 0)
#define VLV_PCS_DW11(ch) _PORT(ch, _VLV_PCS_DW11_CH0, _VLV_PCS_DW11_CH1)

#define _VLV_PCS01_DW11_CH0		0x022c
#define _VLV_PCS23_DW11_CH0		0x042c
#define _VLV_PCS01_DW11_CH1		0x262c
#define _VLV_PCS23_DW11_CH1		0x282c
#define VLV_PCS01_DW11(ch) _PORT(ch, _VLV_PCS01_DW11_CH0, _VLV_PCS01_DW11_CH1)
#define VLV_PCS23_DW11(ch) _PORT(ch, _VLV_PCS23_DW11_CH0, _VLV_PCS23_DW11_CH1)

#define _VLV_PCS01_DW12_CH0		0x0230
#define _VLV_PCS23_DW12_CH0		0x0430
#define _VLV_PCS01_DW12_CH1		0x2630
#define _VLV_PCS23_DW12_CH1		0x2830
#define VLV_PCS01_DW12(ch) _PORT(ch, _VLV_PCS01_DW12_CH0, _VLV_PCS01_DW12_CH1)
#define VLV_PCS23_DW12(ch) _PORT(ch, _VLV_PCS23_DW12_CH0, _VLV_PCS23_DW12_CH1)

#define _VLV_PCS_DW12_CH0		0x8230
#define _VLV_PCS_DW12_CH1		0x8430
#define   DPIO_TX2_STAGGER_MULT(x)	((x) << 20)
#define   DPIO_TX1_STAGGER_MULT(x)	((x) << 16)
#define   DPIO_TX1_STAGGER_MASK(x)	((x) << 8)
#define   DPIO_LANESTAGGER_STRAP_OVRD	(1 << 6)
#define   DPIO_LANESTAGGER_STRAP(x)	((x) << 0)
#define VLV_PCS_DW12(ch) _PORT(ch, _VLV_PCS_DW12_CH0, _VLV_PCS_DW12_CH1)

#define _VLV_PCS_DW14_CH0		0x8238
#define _VLV_PCS_DW14_CH1		0x8438
#define	VLV_PCS_DW14(ch) _PORT(ch, _VLV_PCS_DW14_CH0, _VLV_PCS_DW14_CH1)

#define _VLV_PCS_DW23_CH0		0x825c
#define _VLV_PCS_DW23_CH1		0x845c
#define VLV_PCS_DW23(ch) _PORT(ch, _VLV_PCS_DW23_CH0, _VLV_PCS_DW23_CH1)

#define _VLV_TX_DW2_CH0			0x8288
#define _VLV_TX_DW2_CH1			0x8488
#define   DPIO_SWING_MARGIN000_SHIFT	16
#define   DPIO_SWING_MARGIN000_MASK	(0xff << DPIO_SWING_MARGIN000_SHIFT)
#define   DPIO_UNIQ_TRANS_SCALE_SHIFT	8
#define VLV_TX_DW2(ch) _PORT(ch, _VLV_TX_DW2_CH0, _VLV_TX_DW2_CH1)

#define _VLV_TX_DW3_CH0			0x828c
#define _VLV_TX_DW3_CH1			0x848c
/* The following bit for CHV phy */
#define   DPIO_TX_UNIQ_TRANS_SCALE_EN	(1 << 27)
#define   DPIO_SWING_MARGIN101_SHIFT	16
#define   DPIO_SWING_MARGIN101_MASK	(0xff << DPIO_SWING_MARGIN101_SHIFT)
#define VLV_TX_DW3(ch) _PORT(ch, _VLV_TX_DW3_CH0, _VLV_TX_DW3_CH1)

#define _VLV_TX_DW4_CH0			0x8290
#define _VLV_TX_DW4_CH1			0x8490
#define   DPIO_SWING_DEEMPH9P5_SHIFT	24
#define   DPIO_SWING_DEEMPH9P5_MASK	(0xff << DPIO_SWING_DEEMPH9P5_SHIFT)
#define   DPIO_SWING_DEEMPH6P0_SHIFT	16
#define   DPIO_SWING_DEEMPH6P0_MASK	(0xff << DPIO_SWING_DEEMPH6P0_SHIFT)
#define VLV_TX_DW4(ch) _PORT(ch, _VLV_TX_DW4_CH0, _VLV_TX_DW4_CH1)

#define _VLV_TX3_DW4_CH0		0x690
#define _VLV_TX3_DW4_CH1		0x2a90
#define VLV_TX3_DW4(ch) _PORT(ch, _VLV_TX3_DW4_CH0, _VLV_TX3_DW4_CH1)

#define _VLV_TX_DW5_CH0			0x8294
#define _VLV_TX_DW5_CH1			0x8494
#define   DPIO_TX_OCALINIT_EN		(1 << 31)
#define VLV_TX_DW5(ch) _PORT(ch, _VLV_TX_DW5_CH0, _VLV_TX_DW5_CH1)

#define _VLV_TX_DW11_CH0		0x82ac
#define _VLV_TX_DW11_CH1		0x84ac
#define VLV_TX_DW11(ch) _PORT(ch, _VLV_TX_DW11_CH0, _VLV_TX_DW11_CH1)

#define _VLV_TX_DW14_CH0		0x82b8
#define _VLV_TX_DW14_CH1		0x84b8
#define VLV_TX_DW14(ch) _PORT(ch, _VLV_TX_DW14_CH0, _VLV_TX_DW14_CH1)

/* CHV dpPhy registers */
#define _CHV_PLL_DW0_CH0		0x8000
#define _CHV_PLL_DW0_CH1		0x8180
#define CHV_PLL_DW0(ch) _PIPE(ch, _CHV_PLL_DW0_CH0, _CHV_PLL_DW0_CH1)

#define _CHV_PLL_DW1_CH0		0x8004
#define _CHV_PLL_DW1_CH1		0x8184
#define   DPIO_CHV_N_DIV_SHIFT		8
#define   DPIO_CHV_M1_DIV_BY_2		(0 << 0)
#define CHV_PLL_DW1(ch) _PIPE(ch, _CHV_PLL_DW1_CH0, _CHV_PLL_DW1_CH1)

#define _CHV_PLL_DW2_CH0		0x8008
#define _CHV_PLL_DW2_CH1		0x8188
#define CHV_PLL_DW2(ch) _PIPE(ch, _CHV_PLL_DW2_CH0, _CHV_PLL_DW2_CH1)

#define _CHV_PLL_DW3_CH0		0x800c
#define _CHV_PLL_DW3_CH1		0x818c
#define  DPIO_CHV_FRAC_DIV_EN		(1 << 16)
#define  DPIO_CHV_FIRST_MOD		(0 << 8)
#define  DPIO_CHV_SECOND_MOD		(1 << 8)
#define  DPIO_CHV_FEEDFWD_GAIN_SHIFT	0
#define  DPIO_CHV_FEEDFWD_GAIN_MASK		(0xF << 0)
#define CHV_PLL_DW3(ch) _PIPE(ch, _CHV_PLL_DW3_CH0, _CHV_PLL_DW3_CH1)

#define _CHV_PLL_DW6_CH0		0x8018
#define _CHV_PLL_DW6_CH1		0x8198
#define   DPIO_CHV_GAIN_CTRL_SHIFT	16
#define	  DPIO_CHV_INT_COEFF_SHIFT	8
#define   DPIO_CHV_PROP_COEFF_SHIFT	0
#define CHV_PLL_DW6(ch) _PIPE(ch, _CHV_PLL_DW6_CH0, _CHV_PLL_DW6_CH1)

#define _CHV_PLL_DW8_CH0		0x8020
#define _CHV_PLL_DW8_CH1		0x81A0
#define   DPIO_CHV_TDC_TARGET_CNT_SHIFT 0
#define   DPIO_CHV_TDC_TARGET_CNT_MASK  (0x3FF << 0)
#define CHV_PLL_DW8(ch) _PIPE(ch, _CHV_PLL_DW8_CH0, _CHV_PLL_DW8_CH1)

#define _CHV_PLL_DW9_CH0		0x8024
#define _CHV_PLL_DW9_CH1		0x81A4
#define  DPIO_CHV_INT_LOCK_THRESHOLD_SHIFT		1 /* 3 bits */
#define  DPIO_CHV_INT_LOCK_THRESHOLD_MASK		(7 << 1)
#define  DPIO_CHV_INT_LOCK_THRESHOLD_SEL_COARSE	1 /* 1: coarse & 0 : fine  */
#define CHV_PLL_DW9(ch) _PIPE(ch, _CHV_PLL_DW9_CH0, _CHV_PLL_DW9_CH1)

#define _CHV_CMN_DW0_CH0               0x8100
#define   DPIO_ALLDL_POWERDOWN_SHIFT_CH0	19
#define   DPIO_ANYDL_POWERDOWN_SHIFT_CH0	18
#define   DPIO_ALLDL_POWERDOWN			(1 << 1)
#define   DPIO_ANYDL_POWERDOWN			(1 << 0)

#define _CHV_CMN_DW5_CH0               0x8114
#define   CHV_BUFRIGHTENA1_DISABLE	(0 << 20)
#define   CHV_BUFRIGHTENA1_NORMAL	(1 << 20)
#define   CHV_BUFRIGHTENA1_FORCE	(3 << 20)
#define   CHV_BUFRIGHTENA1_MASK		(3 << 20)
#define   CHV_BUFLEFTENA1_DISABLE	(0 << 22)
#define   CHV_BUFLEFTENA1_NORMAL	(1 << 22)
#define   CHV_BUFLEFTENA1_FORCE		(3 << 22)
#define   CHV_BUFLEFTENA1_MASK		(3 << 22)

#define _CHV_CMN_DW13_CH0		0x8134
#define _CHV_CMN_DW0_CH1		0x8080
#define   DPIO_CHV_S1_DIV_SHIFT		21
#define   DPIO_CHV_P1_DIV_SHIFT		13 /* 3 bits */
#define   DPIO_CHV_P2_DIV_SHIFT		8  /* 5 bits */
#define   DPIO_CHV_K_DIV_SHIFT		4
#define   DPIO_PLL_FREQLOCK		(1 << 1)
#define   DPIO_PLL_LOCK			(1 << 0)
#define CHV_CMN_DW13(ch) _PIPE(ch, _CHV_CMN_DW13_CH0, _CHV_CMN_DW0_CH1)

#define _CHV_CMN_DW14_CH0		0x8138
#define _CHV_CMN_DW1_CH1		0x8084
#define   DPIO_AFC_RECAL		(1 << 14)
#define   DPIO_DCLKP_EN			(1 << 13)
#define   CHV_BUFLEFTENA2_DISABLE	(0 << 17) /* CL2 DW1 only */
#define   CHV_BUFLEFTENA2_NORMAL	(1 << 17) /* CL2 DW1 only */
#define   CHV_BUFLEFTENA2_FORCE		(3 << 17) /* CL2 DW1 only */
#define   CHV_BUFLEFTENA2_MASK		(3 << 17) /* CL2 DW1 only */
#define   CHV_BUFRIGHTENA2_DISABLE	(0 << 19) /* CL2 DW1 only */
#define   CHV_BUFRIGHTENA2_NORMAL	(1 << 19) /* CL2 DW1 only */
#define   CHV_BUFRIGHTENA2_FORCE	(3 << 19) /* CL2 DW1 only */
#define   CHV_BUFRIGHTENA2_MASK		(3 << 19) /* CL2 DW1 only */
#define CHV_CMN_DW14(ch) _PIPE(ch, _CHV_CMN_DW14_CH0, _CHV_CMN_DW1_CH1)

#define _CHV_CMN_DW19_CH0		0x814c
#define _CHV_CMN_DW6_CH1		0x8098
#define   DPIO_ALLDL_POWERDOWN_SHIFT_CH1	30 /* CL2 DW6 only */
#define   DPIO_ANYDL_POWERDOWN_SHIFT_CH1	29 /* CL2 DW6 only */
#define   DPIO_DYNPWRDOWNEN_CH1		(1 << 28) /* CL2 DW6 only */
#define   CHV_CMN_USEDCLKCHANNEL	(1 << 13)

#define CHV_CMN_DW19(ch) _PIPE(ch, _CHV_CMN_DW19_CH0, _CHV_CMN_DW6_CH1)

#define CHV_CMN_DW28			0x8170
#define   DPIO_CL1POWERDOWNEN		(1 << 23)
#define   DPIO_DYNPWRDOWNEN_CH0		(1 << 22)
#define   DPIO_SUS_CLK_CONFIG_ON		(0 << 0)
#define   DPIO_SUS_CLK_CONFIG_CLKREQ		(1 << 0)
#define   DPIO_SUS_CLK_CONFIG_GATE		(2 << 0)
#define   DPIO_SUS_CLK_CONFIG_GATE_CLKREQ	(3 << 0)

#define CHV_CMN_DW30			0x8178
#define   DPIO_CL2_LDOFUSE_PWRENB	(1 << 6)
#define   DPIO_LRC_BYPASS		(1 << 3)

#define _TXLANE(ch, lane, offset) ((ch ? 0x2400 : 0) + \
					(lane) * 0x200 + (offset))

#define CHV_TX_DW0(ch, lane) _TXLANE(ch, lane, 0x80)
#define CHV_TX_DW1(ch, lane) _TXLANE(ch, lane, 0x84)
#define CHV_TX_DW2(ch, lane) _TXLANE(ch, lane, 0x88)
#define CHV_TX_DW3(ch, lane) _TXLANE(ch, lane, 0x8c)
#define CHV_TX_DW4(ch, lane) _TXLANE(ch, lane, 0x90)
#define CHV_TX_DW5(ch, lane) _TXLANE(ch, lane, 0x94)
#define CHV_TX_DW6(ch, lane) _TXLANE(ch, lane, 0x98)
#define CHV_TX_DW7(ch, lane) _TXLANE(ch, lane, 0x9c)
#define CHV_TX_DW8(ch, lane) _TXLANE(ch, lane, 0xa0)
#define CHV_TX_DW9(ch, lane) _TXLANE(ch, lane, 0xa4)
#define CHV_TX_DW10(ch, lane) _TXLANE(ch, lane, 0xa8)
#define CHV_TX_DW11(ch, lane) _TXLANE(ch, lane, 0xac)
#define   DPIO_FRC_LATENCY_SHFIT	8
#define CHV_TX_DW14(ch, lane) _TXLANE(ch, lane, 0xb8)
#define   DPIO_UPAR_SHIFT		30

#endif /* _VLV_SIDEBAND_REG_H_ */
