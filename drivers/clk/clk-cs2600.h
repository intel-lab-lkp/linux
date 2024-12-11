/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CS2600 clock driver
 *
 * Copyright (C) 2023-2024 Cirrus Logic, Inc. and
 *                         Cirrus Logic International Semiconductor Ltd.
 */

#ifndef _CS2600_H
#define _CS2600_H

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/regmap.h>

/* Registers */

#define CS2600_PLL_CFG1			0x0002
#define CS2600_PLL_CFG2			0x0004

#define CS2600_RATIO1_1			0x0006
#define CS2600_RATIO1_2			0x0008
#define CS2600_RATIO2_1			0x000A
#define CS2600_RATIO2_2			0x000C

#define CS2600_PLL_CFG3			0x0016
#define CS2600_SW_RESET			0x0058
#define CS2600_OUTPUT_CFG1		0x0100
#define CS2600_OUTPUT_CFG2		0x0102
#define CS2600_PHASE_ALIGNMENT_CFG1	0x0108

#define CS2600_DEVICE_ID1		0x0110
#define CS2600_DEVICE_ID2		0x0112

#define CS2600_UNLOCK_INDICATORS	0x0114
#define CS2600_ERROR_STS		0x0116

#define CS2600_MAX_REGISTER		CS2600_ERROR_STS
#define CS2600_OUT_CLK_MAX		4
#define CS2600_INTERNAL_OSC_RATE	12000000
#define CS2600_DEVICE_ID_VALUE		0x2600

/* Register Fields */

/* PLL_CFG1 */
#define CS2600_RATIO_MOD_MASK		GENMASK(15, 13)
#define CS2600_S_RATIO_SEL_MASK		GENMASK(12, 11)
#define CS2600_PLL_EN1			BIT(8)
#define CS2600_AUX1_OUT_DIS		BIT(1)
#define CS2600_CLK_OUT_DIS		BIT(0)

#define CS2600_S_RATIO_SEL(x)		(((x) - 1) << 11)

/* PLL_CFG2 */
#define CS2600_FREEZE_EN		BIT(11)
#define CS2600_PLL_EN2			BIT(8)
#define CS2600_M_RATIO_SEL_MASK		GENMASK(2, 1)
#define CS2600_PLL_MODE_SEL		BIT(0)

#define CS2600_M_RATIO_SEL(x)		(((x) - 1) << 1)

/* PLL_CFG3 */
#define CS2600_OUT_GATE_TYPE_MASK	GENMASK(14, 13)
#define CS2600_OUT_GATE			BIT(12)
#define CS2600_RATIO_CFG		BIT(11)
#define CS2600_AUX_OUT_CFG		BIT(6)
#define CS2600_REF_CLK_IN_DIV_MASK	GENMASK(4, 3)
#define CS2600_REF_CLK_IN_DIV(x)	(x << 3)
#define CS2600_SYSCLK_SRC_MASK		GENMASK(2, 1)
#define CS2600_SYSCLK_SRC_OSC		(1 << 1)
#define CS2600_SYSCLK_SRC_REFCLK	(2 << 1)

/* SW_RESET */
#define CS2600_SW_RST_VAL		0x5A

/* OUTPUT_CFG1 */
#define	CS2600_BCLK_OUT_DIS		BIT(6)
#define CS2600_FSYNC_DUTY_CYCLE_MASK	GENMASK(4, 2)
#define	CS2600_FSYNC_OUT_DIS		BIT(0)
#define CS2600_BCLK_DIV_MASK		GENMASK(15, 12)
#define CS2600_BCLK_DIV(x)		(((x) & 0xF) << 12)
#define CS2600_FSYNC_DIV_MASK		GENMASK(11, 8)
#define CS2600_FSYNC_DIV(x)		(((x) & 0xF) << 8)

/* PHASE_ALIGNMENT_CFG1 */
#define CS2600_PHASE_ALIGN_EN		BIT(15)
#define CS2600_PHASE_ALIGN_STB_EN	BIT(7)

/* OUTPUT_CFG2 */
#define CS2600_AUX1OUT_SEL		GENMASK(12, 10)
#define CS2600_AUXOUT1_SRC(x)		(x << 10)
#define CS2600_AUXOUT1_MAX		0x7

/* DEVICE_ID2 */
#define CS2600_AREVID_MASK		GENMASK(7, 4)
#define CS2600_MTLRVID_MASK		GENMASK(3, 0)

/* UNLOCK_INDICATORS */
#define CS2600_P_UNLOCK_STICKY		BIT(3)
#define CS2600_P_UNLOCK			BIT(2)
#define CS2600_F_UNLOCK_STICKY		BIT(1)
#define CS2600_F_UNLOCK			BIT(0)

/* ERROR_STS */
#define CS2600_ERR_DEV_DEFECT		BIT(7) /* Device defective */
#define CS2600_ERR_OTP_CORRUPT		BIT(6)
#define CS2600_ERR_REG_CFG		BIT(5) /* Invalid register config */
#define CS2600_ERR_PLL_DISABLED		BIT(4)
#define CS2600_ERR_HW_CFG		BIT(3) /* Invalid HW Config */
#define CS2600_ERR_REFCLK_MISSING	BIT(2)
#define CS2600_ERR_CLKIN_UNSTABLE	BIT(1)
#define CS2600_ERR_CLKIN_MISSING	BIT(0)

#define CS2600_PLL_OUT			0
#define CS2600_CLK_OUT			1
#define CS2600_BCLK_OUT			2
#define CS2600_FSYNC_OUT		3

#define CS2600_LOCK_ATTEMPTS_MAX	100
#define CS2600_CLEAR_INDICATORS		0xA

#define CS2600_REFCLK_INPUT		0
#define CS2600_CLK_IN_INPUT		1

#define CS2600_12_20_SHIFT		20
#define CS2600_20_12_SHIFT		12
#define CS2600_12_20_PPM		1
#define CS2600_20_12_PPM		224
#define CS2600_PLL_MULTIPLY_MAX		1048576
#define CS2600_HI_RES_MAX		4096

#define CS2600_RATIO_SLOT_MAX		2

#define CS2600_CLK_OUT_MIN		6000000
#define CS2600_CLK_OUT_MAX		75000000
#define CS2600_BCLK_OUT_MIN		(CS2600_CLK_OUT_MIN / 48)
#define CS2600_BCLK_OUT_MAX		CS2600_CLK_OUT_MAX
#define CS2600_FSYNC_OUT_MIN		(CS2600_CLK_OUT_MIN / 1536)
#define CS2600_FSYNC_OUT_MAX		(CS2600_CLK_OUT_MAX / 16)

#define CS2600_AUX_OUT_FREQ_UNLOCK	0
#define CS2600_AUX_OUT_FREQ_UNLOCK_VAL  0x3
#define CS2600_AUX_OUT_PHASE_UNLOCK	1
#define CS2600_AUX_OUT_PHASE_UNLOCK_VAL	0x4
#define CS2600_AUX_OUT_NO_CLKIN		2
#define CS2600_AUX_OUT_NO_CLKIN_VAL	0x7

struct cs2600;

struct cs2600_clk_hw {
	struct clk_hw hw;
	struct clk_init_data init;
	struct cs2600 *priv;
};

enum cs2600_mode {
	CS2600_MANUAL_MODE = 0,
	CS2600_SMART_MODE = 1,
	CS2600_SMART_CLKIN_ONLY_MODE = 2,
};

/* CS2600 private data */
struct cs2600 {
	struct device *dev;
	struct regmap *regmap;
	struct cs2600_clk_hw hw[CS2600_OUT_CLK_MAX];

	struct clk *clk_in;
	struct clk *ref_clk;

	enum cs2600_mode mode;
	unsigned long refclk_rate;
};

#endif
