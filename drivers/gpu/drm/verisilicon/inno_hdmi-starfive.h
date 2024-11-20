/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) StarFive Technology Co., Ltd.
 */

#ifndef __STARFIVE_HDMI_H__
#define __STARFIVE_HDMI_H__

#include <linux/bitfield.h>
#include <linux/bits.h>

#define HDMI_SYNC			0xce

#define UPDATE(x, h, l)			FIELD_PREP(GENMASK(h, l), x)
#define HDMI_SYS_CTRL			0x00
#define m_RST_ANALOG			(1 << 6)
#define v_RST_ANALOG			(0 << 6)
#define v_NOT_RST_ANALOG		(1 << 6)
#define m_RST_DIGITAL			(1 << 5)
#define v_RST_DIGITAL			(0 << 5)
#define v_NOT_RST_DIGITAL		(1 << 5)
#define m_REG_CLK_INV			(1 << 4)
#define v_REG_CLK_NOT_INV		(0 << 4)
#define v_REG_CLK_INV			(1 << 4)
#define m_VCLK_INV			(1 << 3)
#define v_VCLK_NOT_INV			(0 << 3)
#define v_VCLK_INV			(1 << 3)
#define m_REG_CLK_SOURCE		(1 << 2)
#define v_REG_CLK_SOURCE_TMDS		(0 << 2)
#define v_REG_CLK_SOURCE_SYS		(1 << 2)
#define m_POWER				(1 << 1)
#define v_PWR_ON			(0 << 1)
#define v_PWR_OFF			(1 << 1)
#define m_INT_POL			(1 << 0)
#define v_INT_POL_HIGH			1
#define v_INT_POL_LOW			0

#define HDMI_VIDEO_TIMING_CTL		0x08
#define v_VSYNC_POLARITY_SF(n)		((n) << 3)
#define v_HSYNC_POLARITY_SF(n)		((n) << 2)
#define v_INETLACE(n)			((n) << 1)
#define v_EXTERANL_VIDEO(n)		((n) << 0)

#define HDMI_VIDEO_EXT_HTOTAL_L		0x09
#define HDMI_VIDEO_EXT_HTOTAL_H		0x0a
#define HDMI_VIDEO_EXT_HBLANK_L		0x0b
#define HDMI_VIDEO_EXT_HBLANK_H		0x0c
#define HDMI_VIDEO_EXT_HDELAY_L		0x0d
#define HDMI_VIDEO_EXT_HDELAY_H		0x0e
#define HDMI_VIDEO_EXT_HDURATION_L	0x0f
#define HDMI_VIDEO_EXT_HDURATION_H	0x10
#define HDMI_VIDEO_EXT_VTOTAL_L		0x11
#define HDMI_VIDEO_EXT_VTOTAL_H		0x12
#define HDMI_VIDEO_EXT_VBLANK		0x13
#define HDMI_VIDEO_EXT_VDELAY		0x14
#define HDMI_VIDEO_EXT_VDURATION	0x15

/* REG: 0x1a0 */
#define STF_INNO_PRE_PLL_CONTROL			0x1a0
#define STF_INNO_PCLK_VCO_DIV_5_MASK			BIT(1)
#define STF_INNO_PCLK_VCO_DIV_5(x)			UPDATE(x, 1, 1)
#define STF_INNO_PRE_PLL_POWER_DOWN			BIT(0)

/* REG: 0x1a1 */
#define STF_INNO_PRE_PLL_DIV_1				0x1a1
#define STF_INNO_PRE_PLL_PRE_DIV_MASK			GENMASK(5, 0)
#define STF_INNO_PRE_PLL_PRE_DIV(x)			UPDATE(x, 5, 0)

/* REG: 0x1a2 */
#define STF_INNO_PRE_PLL_DIV_2					0x1a2
#define STF_INNO_SPREAD_SPECTRUM_MOD_DOWN		BIT(7)
#define STF_INNO_SPREAD_SPECTRUM_MOD_DISABLE		BIT(6)
#define STF_INNO_PRE_PLL_FRAC_DIV_DISABLE		UPDATE(3, 5, 4)
#define STF_INNO_PRE_PLL_FB_DIV_11_8_MASK		GENMASK(3, 0)
#define STF_INNO_PRE_PLL_FB_DIV_11_8(x)			UPDATE((x) >> 8, 3, 0)

/* REG: 0x1a3 */
#define STF_INNO_PRE_PLL_DIV_3				0x1a3
#define STF_INNO_PRE_PLL_FB_DIV_7_0(x)			UPDATE(x, 7, 0)

/* REG: 0x1a4*/
#define STF_INNO_PRE_PLL_DIV_4				0x1a4
#define STF_INNO_PRE_PLL_TMDSCLK_DIV_C_MASK		GENMASK(1, 0)
#define STF_INNO_PRE_PLL_TMDSCLK_DIV_C(x)		UPDATE(x, 1, 0)
#define STF_INNO_PRE_PLL_TMDSCLK_DIV_B_MASK		GENMASK(3, 2)
#define STF_INNO_PRE_PLL_TMDSCLK_DIV_B(x)		UPDATE(x, 3, 2)
#define STF_INNO_PRE_PLL_TMDSCLK_DIV_A_MASK		GENMASK(5, 4)
#define STF_INNO_PRE_PLL_TMDSCLK_DIV_A(x)		UPDATE(x, 5, 4)

/* REG: 0x1a5 */
#define STF_INNO_PRE_PLL_DIV_5				0x1a5
#define STF_INNO_PRE_PLL_PCLK_DIV_B_SHIFT		5
#define STF_INNO_PRE_PLL_PCLK_DIV_B_MASK		GENMASK(6, 5)
#define STF_INNO_PRE_PLL_PCLK_DIV_B(x)			UPDATE(x, 6, 5)
#define STF_INNO_PRE_PLL_PCLK_DIV_A_MASK		GENMASK(4, 0)
#define STF_INNO_PRE_PLL_PCLK_DIV_A(x)			UPDATE(x, 4, 0)

/* REG: 0x1a6 */
#define STF_INNO_PRE_PLL_DIV_6				0x1a6
#define STF_INNO_PRE_PLL_PCLK_DIV_C_SHIFT		5
#define STF_INNO_PRE_PLL_PCLK_DIV_C_MASK		GENMASK(6, 5)
#define STF_INNO_PRE_PLL_PCLK_DIV_C(x)			UPDATE(x, 6, 5)
#define STF_INNO_PRE_PLL_PCLK_DIV_D_MASK		GENMASK(4, 0)
#define STF_INNO_PRE_PLL_PCLK_DIV_D(x)			UPDATE(x, 4, 0)

/* REG: 0x1a9 */
#define STF_INNO_PRE_PLL_LOCK_STATUS			0x1a9

/* REG: 0x1aa */
#define STF_INNO_POST_PLL_DIV_1				0x1aa
#define STF_INNO_POST_PLL_POST_DIV_ENABLE		GENMASK(3, 2)
#define STF_INNO_POST_PLL_REFCLK_SEL_TMDS		BIT(1)
#define STF_INNO_POST_PLL_POWER_DOWN			BIT(0)
#define STF_INNO_POST_PLL_FB_DIV_8(x)			UPDATE(((x) >> 8) << 4, 4, 4)

/* REG:0x1ab */
#define STF_INNO_POST_PLL_DIV_2				0x1ab
#define STF_INNO_POST_PLL_Pre_DIV_MASK			GENMASK(5, 0)
#define STF_INNO_POST_PLL_PRE_DIV(x)			UPDATE(x, 5, 0)

/* REG: 0x1ac */
#define STF_INNO_POST_PLL_DIV_3				0x1ac
#define STF_INNO_POST_PLL_FB_DIV_7_0(x)			UPDATE(x, 7, 0)

/* REG: 0x1ad */
#define STF_INNO_POST_PLL_DIV_4				0x1ad
#define STF_INNO_POST_PLL_POST_DIV_MASK			GENMASK(2, 0)
#define STF_INNO_POST_PLL_POST_DIV_2			0x0
#define STF_INNO_POST_PLL_POST_DIV_4			0x1
#define STF_INNO_POST_PLL_POST_DIV_8			0x3

/* REG: 0x1af */
#define STF_INNO_POST_PLL_LOCK_STATUS			0x1af

/* REG: 0x1b0 */
#define STF_INNO_BIAS_CONTROL				0x1b0
#define STF_INNO_BIAS_ENABLE				BIT(2)

/* REG: 0x1b2 */
#define STF_INNO_TMDS_CONTROL				0x1b2
#define STF_INNO_TMDS_CLK_DRIVER_EN			BIT(3)
#define STF_INNO_TMDS_D2_DRIVER_EN			BIT(2)
#define STF_INNO_TMDS_D1_DRIVER_EN			BIT(1)
#define STF_INNO_TMDS_D0_DRIVER_EN			BIT(0)
#define STF_INNO_TMDS_DRIVER_ENABLE			(STF_INNO_TMDS_CLK_DRIVER_EN | \
							 STF_INNO_TMDS_D2_DRIVER_EN | \
							 STF_INNO_TMDS_D1_DRIVER_EN | \
							 STF_INNO_TMDS_D0_DRIVER_EN)

/* REG: 0x1b4 */
#define STF_INNO_LDO_CONTROL				0x1b4
#define STF_INNO_LDO_D2_EN				BIT(2)
#define STF_INNO_LDO_D1_EN				BIT(1)
#define STF_INNO_LDO_D0_EN				BIT(0)
#define STF_INNO_LDO_ENABLE				(STF_INNO_LDO_D2_EN | \
							 STF_INNO_LDO_D1_EN | \
							 STF_INNO_LDO_D0_EN)

/* REG: 0x1be */
#define STF_INNO_SERIALIER_CONTROL			0x1be
#define STF_INNO_SERIALIER_D2_EN			BIT(6)
#define STF_INNO_SERIALIER_D1_EN			BIT(5)
#define STF_INNO_SERIALIER_D0_EN			BIT(4)
#define STF_INNO_SERIALIER_EN				BIT(0)

#define STF_INNO_SERIALIER_ENABLE			(STF_INNO_SERIALIER_D2_EN | \
							 STF_INNO_SERIALIER_D1_EN | \
							 STF_INNO_SERIALIER_D0_EN | \
							 STF_INNO_SERIALIER_EN)

/* REG: 0x1cc */
#define STF_INNO_RX_CONTROL				0x1cc
#define STF_INNO_RX_EN					BIT(3)
#define STF_INNO_RX_CHANNEL_2_EN			BIT(2)
#define STF_INNO_RX_CHANNEL_1_EN			BIT(1)
#define STF_INNO_RX_CHANNEL_0_EN			BIT(0)
#define STF_INNO_RX_ENABLE				(STF_INNO_RX_EN | \
							 STF_INNO_RX_CHANNEL_2_EN | \
							 STF_INNO_RX_CHANNEL_1_EN | \
							 STF_INNO_RX_CHANNEL_0_EN)

/* REG: 0x1d1 */
#define STF_INNO_PRE_PLL_FRAC_DIV_H			0x1d1
#define STF_INNO_PRE_PLL_FRAC_DIV_23_16(x)		UPDATE((x) >> 16, 7, 0)
/* REG: 0x1d2 */
#define STF_INNO_PRE_PLL_FRAC_DIV_M			0x1d2
#define STF_INNO_PRE_PLL_FRAC_DIV_15_8(x)		UPDATE((x) >> 8, 7, 0)
/* REG: 0x1d3 */
#define STF_INNO_PRE_PLL_FRAC_DIV_L			0x1d3
#define STF_INNO_PRE_PLL_FRAC_DIV_7_0(x)		UPDATE(x, 7, 0)

#define PIXCLOCK_4K_30FPS				297000000

#endif /* __STARFIVE_HDMI_H__ */
