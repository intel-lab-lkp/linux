// SPDX-License-Identifier: GPL-2.0
/*
 * V4L2 Support for the os05b10
 *
 * Copyright (C) 2025 Silicon Signals Pvt. Ltd.
 *
 * Inspired from imx219, ov2735 camera drivers.
 */

#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device/devres.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/time.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#define OS05B10_XCLK_FREQ		(24 * HZ_PER_MHZ)

#define OS05B10_REG_CHIP_ID		CCI_REG24(0x300a)
#define OS05B10_CHIP_ID			0x530641

#define OS05B10_REG_CTRL_MODE		CCI_REG8(0x0100)
#define OS05B10_MODE_STANDBY		0x00
#define OS05B10_MODE_STREAMING		0x01

#define OS05B10_REG_PLL_CTRL_01		CCI_REG8(0x0301)
#define OS05B10_REG_PLL_CTRL_03		CCI_REG8(0x0303)
#define OS05B10_REG_PLL_CTRL_05		CCI_REG8(0x0305)
#define OS05B10_REG_PLL_CTRL_06		CCI_REG8(0x0306)
#define OS05B10_REG_PLL_CTRL_25		CCI_REG8(0x0325)

#define OS05B10_REG_MIPI_SC_CTRL	CCI_REG8(0x3016)
#define OS05B10_4_LANE_MODE		0x72
#define OS05B10_2_LANE_MODE		0x32

#define OS05B10_REG_MIPI_SC_CTRL_1	CCI_REG8(0x3022)
#define OS05B10_10BIT_MODE		0x01
#define OS05B10_12BIT_MODE		0x61

#define OS05B10_REG_EXPOSURE		CCI_REG24(0x3500)
#define OS05B10_EXPOSURE_MIN		2
#define OS05B10_EXPOSURE_STEP		1
#define OS05B10_EXPOSURE_MARGIN		8

#define OS05B10_REG_ANALOG_GAIN		CCI_REG16(0x3508)
#define OS05B10_ANALOG_GAIN_MIN		0x80
#define OS05B10_ANALOG_GAIN_MAX		0x7C0
#define OS05B10_ANALOG_GAIN_STEP	1
#define OS05B10_ANALOG_GAIN_DEFAULT	0x80

#define OS05B10_REG_DIGITAL_GAIN	CCI_REG16(0x350a)
#define OS05B10_DIGITAL_GAIN_MIN	0x400
#define OS05B10_DIGITAL_GAIN_MAX	0x3fff
#define OS05B10_DIGITAL_GAIN_STEP	16
#define OS05B10_DIGITAL_GAIN_DEFAULT	0x400

#define OS05B10_REG_ANALOG_GAIN_SHORT	CCI_REG16(0x350c)
#define OS05B10_REG_DIGITAL_GAIN_SHORT	CCI_REG16(0x350e)
#define OS05B10_REG_EXPOSURE_SHORT	CCI_REG24(0x3510)

#define OS05B10_REG_X_ADDR_START	CCI_REG16(0x3800)
#define OS05B10_REG_Y_ADDR_START	CCI_REG16(0x3802)
#define OS05B10_REG_X_ADDR_END		CCI_REG16(0x3804)
#define OS05B10_REG_Y_ADDR_END		CCI_REG16(0x3806)
#define OS05B10_REG_X_OUTPUT_SIZE	CCI_REG16(0x3808)
#define OS05B10_REG_Y_OUTPUT_SIZE	CCI_REG16(0x380a)

#define OS05B10_REG_HTS			CCI_REG16(0x380c)

#define OS05B10_REG_VTS			CCI_REG16(0x380e)
#define OS05B10_VTS_MAX			0x7fff

#define OS05B10_REG_ISP_X_WIN		CCI_REG16(0x3810)
#define OS05B10_REG_ISP_Y_WIN		CCI_REG16(0x3812)
#define OS05B10_REG_X_INC_ODD		CCI_REG8(0x3814)
#define OS05B10_REG_X_INC_EVEN		CCI_REG8(0x3815)
#define OS05B10_REG_Y_INC_ODD		CCI_REG8(0x3816)
#define OS05B10_REG_Y_INC_EVEN		CCI_REG8(0x3817)

#define OS05B10_REG_FORMAT1		CCI_REG8(0x3820)
#define OS05B10_MIRROR			BIT(3)
#define OS05B10_FLIP			GENMASK(5, 4)

#define OS05B10_REG_ANALOG_FLIP         CCI_REG8(0x3716)
#define OS05B10_FLIP_ENABLE             0x04
#define OS05B10_FLIP_DISABLE            0x24

#define OS05B10_REG_FORMAT2		CCI_REG8(0x3821)
#define OS05B10_HDR_ENABLE		0x04

#define OS05B10_REG_PRE_ISP_20_0	CCI_REG8(0x5080)
#define OS05B10_DISABLED		0x00
#define OS05B10_COLOR_BAR_1		0x80
#define OS05B10_COLOR_BAR_2		0x84
#define OS05B10_COLOR_BAR_3		0x88
#define OS05B10_COLOR_BAR_4		0x8c
#define OS05B10_COLOR_SQUARE		0x82
#define OS05B10_BW_SQUARE		0x92
#define OS05B10_TRANSPARENT_EFFECT	0xa0
#define OS05B10_ROLLING_BAR_EFFECT	0xc0

#define OS05B10_LINK_FREQ_600MHZ	(600 * HZ_PER_MHZ)

static const struct v4l2_rect os05b10_native_area = {
	.top = 0,
	.left = 0,
	.width = 2608,
	.height = 1960,
};

static const struct v4l2_rect os05b10_active_area = {
	.top = 8,
	.left = 8,
	.width = 2592,
	.height = 1944,
};

static const char * const os05b10_supply_name[] = {
	"avdd",		/* Analog supply */
	"dovdd",	/* Digital IO */
	"dvdd",		/* Digital core */
};

static const struct cci_reg_sequence os05b10_common_regs[] = {
	{ OS05B10_REG_PLL_CTRL_01,		0x44 },
	{ OS05B10_REG_PLL_CTRL_03,		0x02 },
	{ OS05B10_REG_PLL_CTRL_05,		0x32 },
	{ OS05B10_REG_PLL_CTRL_06,		0x00 },
	{ OS05B10_REG_PLL_CTRL_25,		0x3b },
	{ OS05B10_REG_MIPI_SC_CTRL,		0x72 },
	{ OS05B10_REG_ANALOG_GAIN_SHORT,	0x0080 },
	{ OS05B10_REG_DIGITAL_GAIN_SHORT,	0x0400 },
	{ OS05B10_REG_EXPOSURE_SHORT,		0x000020 },
	{ CCI_REG8(0x3002), 0x21 },
	{ CCI_REG8(0x301e), 0xb4 },
	{ CCI_REG8(0x301f), 0xd0 },
	{ CCI_REG8(0x3021), 0x03 },
	{ CCI_REG8(0x3107), 0xa1 },
	{ CCI_REG8(0x3108), 0x7d },
	{ CCI_REG8(0x3109), 0xfc },
	{ CCI_REG8(0x3503), 0x88 },
	{ CCI_REG8(0x3600), 0x4d },
	{ CCI_REG8(0x3601), 0x08 },
	{ CCI_REG8(0x3611), 0x24 },
	{ CCI_REG8(0x3614), 0x4c },
	{ CCI_REG8(0x3632), 0x80 },
	{ CCI_REG8(0x3633), 0x00 },
	{ CCI_REG8(0x3636), 0xcc },
	{ CCI_REG8(0x3637), 0x27 },
	{ CCI_REG8(0x3660), 0x00 },
	{ CCI_REG8(0x3665), 0x00 },
	{ CCI_REG8(0x3666), 0x00 },
	{ CCI_REG8(0x366a), 0x14 },
	{ CCI_REG8(0x3670), 0x0b },
	{ CCI_REG8(0x3671), 0x0b },
	{ CCI_REG8(0x3672), 0x0b },
	{ CCI_REG8(0x3673), 0x0b },
	{ CCI_REG8(0x3678), 0x2b },
	{ CCI_REG8(0x367a), 0x11 },
	{ CCI_REG8(0x367b), 0x11 },
	{ CCI_REG8(0x367c), 0x11 },
	{ CCI_REG8(0x367d), 0x11 },
	{ CCI_REG8(0x3681), 0xff },
	{ CCI_REG8(0x3682), 0x86 },
	{ CCI_REG8(0x3683), 0x44 },
	{ CCI_REG8(0x3684), 0x24 },
	{ CCI_REG8(0x3685), 0x00 },
	{ CCI_REG8(0x368a), 0x00 },
	{ CCI_REG8(0x368d), 0x2b },
	{ CCI_REG8(0x368e), 0x2b },
	{ CCI_REG8(0x3690), 0x00 },
	{ CCI_REG8(0x3691), 0x0b },
	{ CCI_REG8(0x3692), 0x0b },
	{ CCI_REG8(0x3693), 0x0b },
	{ CCI_REG8(0x3694), 0x0b },
	{ CCI_REG8(0x369d), 0x68 },
	{ CCI_REG8(0x369e), 0x34 },
	{ CCI_REG8(0x369f), 0x1b },
	{ CCI_REG8(0x36a0), 0x0f },
	{ CCI_REG8(0x36a1), 0x77 },
	{ CCI_REG8(0x36b0), 0x30 },
	{ CCI_REG8(0x36b2), 0x00 },
	{ CCI_REG8(0x36b3), 0x00 },
	{ CCI_REG8(0x36b4), 0x00 },
	{ CCI_REG8(0x36b5), 0x00 },
	{ CCI_REG8(0x36b6), 0x00 },
	{ CCI_REG8(0x36b7), 0x00 },
	{ CCI_REG8(0x36b8), 0x00 },
	{ CCI_REG8(0x36b9), 0x00 },
	{ CCI_REG8(0x36ba), 0x00 },
	{ CCI_REG8(0x36bb), 0x00 },
	{ CCI_REG8(0x36bc), 0x00 },
	{ CCI_REG8(0x36bd), 0x00 },
	{ CCI_REG8(0x36be), 0x00 },
	{ CCI_REG8(0x36bf), 0x00 },
	{ CCI_REG8(0x36c0), 0x01 },
	{ CCI_REG8(0x36c1), 0x00 },
	{ CCI_REG8(0x36c2), 0x00 },
	{ CCI_REG8(0x36c3), 0x00 },
	{ CCI_REG8(0x36c4), 0x00 },
	{ CCI_REG8(0x36c5), 0x00 },
	{ CCI_REG8(0x36c6), 0x00 },
	{ CCI_REG8(0x36c7), 0x00 },
	{ CCI_REG8(0x36c8), 0x00 },
	{ CCI_REG8(0x36c9), 0x00 },
	{ CCI_REG8(0x36ca), 0x0e },
	{ CCI_REG8(0x36cb), 0x0e },
	{ CCI_REG8(0x36cc), 0x0e },
	{ CCI_REG8(0x36cd), 0x0e },
	{ CCI_REG8(0x36ce), 0x0c },
	{ CCI_REG8(0x36cf), 0x0c },
	{ CCI_REG8(0x36d0), 0x0c },
	{ CCI_REG8(0x36d1), 0x0c },
	{ CCI_REG8(0x36d2), 0x00 },
	{ CCI_REG8(0x36d3), 0x08 },
	{ CCI_REG8(0x36d4), 0x10 },
	{ CCI_REG8(0x36d5), 0x10 },
	{ CCI_REG8(0x36d6), 0x00 },
	{ CCI_REG8(0x36d7), 0x08 },
	{ CCI_REG8(0x36d8), 0x10 },
	{ CCI_REG8(0x36d9), 0x10 },
	{ CCI_REG8(0x3701), 0x1d },
	{ CCI_REG8(0x3703), 0x2a },
	{ CCI_REG8(0x3704), 0x05 },
	{ CCI_REG8(0x3709), 0x57 },
	{ CCI_REG8(0x370b), 0x63 },
	{ CCI_REG8(0x3706), 0x28 },
	{ CCI_REG8(0x370a), 0x00 },
	{ CCI_REG8(0x370b), 0x63 },
	{ CCI_REG8(0x370e), 0x0c },
	{ CCI_REG8(0x370f), 0x1c },
	{ CCI_REG8(0x3710), 0x00 },
	{ CCI_REG8(0x3713), 0x00 },
	{ CCI_REG8(0x371a), 0x1e },
	{ CCI_REG8(0x3724), 0x09 },
	{ CCI_REG8(0x3725), 0xb2 },
	{ CCI_REG8(0x372b), 0x54 },
	{ CCI_REG8(0x3730), 0xe1 },
	{ CCI_REG8(0x3735), 0x80 },
	{ CCI_REG8(0x3739), 0x10 },
	{ CCI_REG8(0x3740), 0x28 },
	{ CCI_REG8(0x3741), 0x21 },
	{ CCI_REG8(0x3742), 0x21 },
	{ CCI_REG8(0x3743), 0x21 },
	{ CCI_REG8(0x3744), 0x63 },
	{ CCI_REG8(0x3745), 0x5a },
	{ CCI_REG8(0x3746), 0x5a },
	{ CCI_REG8(0x3747), 0x5a },
	{ CCI_REG8(0x3748), 0x00 },
	{ CCI_REG8(0x3749), 0x00 },
	{ CCI_REG8(0x374a), 0x00 },
	{ CCI_REG8(0x374b), 0x00 },
	{ CCI_REG8(0x3756), 0x00 },
	{ CCI_REG8(0x3757), 0x0e },
	{ CCI_REG8(0x375d), 0x84 },
	{ CCI_REG8(0x3760), 0x11 },
	{ CCI_REG8(0x3767), 0x08 },
	{ CCI_REG8(0x376f), 0x42 },
	{ CCI_REG8(0x3771), 0x00 },
	{ CCI_REG8(0x3773), 0x01 },
	{ CCI_REG8(0x3774), 0x02 },
	{ CCI_REG8(0x3775), 0x12 },
	{ CCI_REG8(0x3776), 0x02 },
	{ CCI_REG8(0x377b), 0x40 },
	{ CCI_REG8(0x377c), 0x00 },
	{ CCI_REG8(0x377d), 0x0c },
	{ CCI_REG8(0x3782), 0x02 },
	{ CCI_REG8(0x3787), 0x24 },
	{ CCI_REG8(0x378a), 0x01 },
	{ CCI_REG8(0x378d), 0x00 },
	{ CCI_REG8(0x3790), 0x1f },
	{ CCI_REG8(0x3791), 0x58 },
	{ CCI_REG8(0x3795), 0x24 },
	{ CCI_REG8(0x3796), 0x01 },
	{ CCI_REG8(0x3798), 0x40 },
	{ CCI_REG8(0x379c), 0x00 },
	{ CCI_REG8(0x379d), 0x00 },
	{ CCI_REG8(0x379e), 0x00 },
	{ CCI_REG8(0x379f), 0x01 },
	{ CCI_REG8(0x37a1), 0x10 },
	{ CCI_REG8(0x37a6), 0x00 },
	{ CCI_REG8(0x37ab), 0x0e },
	{ CCI_REG8(0x37ac), 0xa0 },
	{ CCI_REG8(0x37be), 0x0a },
	{ CCI_REG8(0x37bb), 0x02 },
	{ CCI_REG8(0x37c4), 0x11 },
	{ CCI_REG8(0x37c5), 0x80 },
	{ CCI_REG8(0x37c6), 0x14 },
	{ CCI_REG8(0x37c7), 0x08 },
	{ CCI_REG8(0x37c8), 0x42 },
	{ CCI_REG8(0x37cd), 0x17 },
	{ CCI_REG8(0x37ce), 0x01 },
	{ CCI_REG8(0x37d8), 0x02 },
	{ CCI_REG8(0x37dc), 0x01 },
	{ CCI_REG8(0x37e0), 0x0c },
	{ CCI_REG8(0x37e1), 0x20 },
	{ CCI_REG8(0x37e2), 0x10 },
	{ CCI_REG8(0x37e3), 0x04 },
	{ CCI_REG8(0x37e4), 0x28 },
	{ CCI_REG8(0x37e5), 0x02 },
	{ CCI_REG8(0x37ef), 0x00 },
	{ CCI_REG8(0x37f4), 0x00 },
	{ CCI_REG8(0x37f5), 0x00 },
	{ CCI_REG8(0x37f6), 0x00 },
	{ CCI_REG8(0x37f7), 0x00 },
	{ CCI_REG8(0x3818), 0x00 },
	{ CCI_REG8(0x3819), 0x00 },
	{ CCI_REG8(0x381a), 0x00 },
	{ CCI_REG8(0x381b), 0x01 },
	{ CCI_REG8(0x3822), 0x12 },
	{ CCI_REG8(0x3823), 0x08 },
	{ CCI_REG8(0x3824), 0x00 },
	{ CCI_REG8(0x3825), 0x20 },
	{ CCI_REG8(0x3826), 0x00 },
	{ CCI_REG8(0x3827), 0x08 },
	{ CCI_REG8(0x3829), 0x03 },
	{ CCI_REG8(0x382a), 0x00 },
	{ CCI_REG8(0x382b), 0x00 },
	{ CCI_REG8(0x3838), 0x00 },
	{ CCI_REG8(0x3839), 0x00 },
	{ CCI_REG8(0x383a), 0x00 },
	{ CCI_REG8(0x383b), 0x00 },
	{ CCI_REG8(0x383d), 0x01 },
	{ CCI_REG8(0x383e), 0x00 },
	{ CCI_REG8(0x383f), 0x00 },
	{ CCI_REG8(0x3843), 0x00 },
	{ CCI_REG8(0x3880), 0x16 },
	{ CCI_REG8(0x3881), 0x00 },
	{ CCI_REG8(0x3882), 0x08 },
	{ CCI_REG8(0x389a), 0x00 },
	{ CCI_REG8(0x389b), 0x00 },
	{ CCI_REG8(0x38a2), 0x02 },
	{ CCI_REG8(0x38a3), 0x02 },
	{ CCI_REG8(0x38a4), 0x02 },
	{ CCI_REG8(0x38a5), 0x02 },
	{ CCI_REG8(0x38a7), 0x04 },
	{ CCI_REG8(0x38b8), 0x02 },
	{ CCI_REG8(0x3c80), 0x3e },
	{ CCI_REG8(0x3c87), 0x02 },
	{ CCI_REG8(0x389c), 0x00 },
	{ CCI_REG8(0x3ca2), 0x0c },
	{ CCI_REG8(0x3d85), 0x1b },
	{ CCI_REG8(0x3d8d), 0xe2 },
	{ CCI_REG8(0x3f00), 0xcb },
	{ CCI_REG8(0x3f9e), 0x07 },
	{ CCI_REG8(0x3f9f), 0x04 },
	{ CCI_REG8(0x4000), 0xf3 },
	{ CCI_REG8(0x4002), 0x00 },
	{ CCI_REG8(0x4003), 0x40 },
	{ CCI_REG8(0x400b), 0x00 },
	{ CCI_REG8(0x4040), 0x00 },
	{ CCI_REG8(0x4090), 0x14 },
	{ CCI_REG8(0x40b0), 0x01 },
	{ CCI_REG8(0x40b1), 0x01 },
	{ CCI_REG8(0x40b2), 0x30 },
	{ CCI_REG8(0x40b3), 0x04 },
	{ CCI_REG8(0x40b4), 0xe8 },
	{ CCI_REG8(0x40b5), 0x01 },
	{ CCI_REG8(0x40b7), 0x07 },
	{ CCI_REG8(0x40b8), 0xff },
	{ CCI_REG8(0x40b9), 0x00 },
	{ CCI_REG8(0x40ba), 0x00 },
	{ CCI_REG8(0x4300), 0xff },
	{ CCI_REG8(0x4301), 0x00 },
	{ CCI_REG8(0x4302), 0x0f },
	{ CCI_REG8(0x4303), 0x20 },
	{ CCI_REG8(0x4304), 0x20 },
	{ CCI_REG8(0x4305), 0x83 },
	{ CCI_REG8(0x4306), 0x21 },
	{ CCI_REG8(0x430d), 0x00 },
	{ CCI_REG8(0x4506), 0x00 },
	{ CCI_REG8(0x4507), 0x60 },
	{ CCI_REG8(0x4803), 0x00 },
	{ CCI_REG8(0x4809), 0x8e },
	{ CCI_REG8(0x480e), 0x00 },
	{ CCI_REG8(0x4813), 0x00 },
	{ CCI_REG8(0x4814), 0x2a },
	{ CCI_REG8(0x481b), 0x40 },
	{ CCI_REG8(0x481f), 0x30 },
	{ CCI_REG8(0x4825), 0x34 },
	{ CCI_REG8(0x4829), 0x64 },
	{ CCI_REG8(0x484b), 0x07 },
	{ CCI_REG8(0x4883), 0x36 },
	{ CCI_REG8(0x4885), 0x03 },
	{ CCI_REG8(0x488b), 0x00 },
	{ CCI_REG8(0x4d06), 0x01 },
	{ CCI_REG8(0x4e00), 0x2a },
	{ CCI_REG8(0x4e0d), 0x00 },
	{ CCI_REG8(0x5000), 0xf9 },
	{ CCI_REG8(0x5001), 0x09 },
	{ CCI_REG8(0x5004), 0x00 },
	{ CCI_REG8(0x5005), 0x0e },
	{ CCI_REG8(0x5036), 0x00 },
	{ CCI_REG8(0x5082), 0x00 },
	{ CCI_REG8(0x5180), 0x00 },
	{ CCI_REG8(0x5181), 0x10 },
	{ CCI_REG8(0x5182), 0x01 },
	{ CCI_REG8(0x5183), 0xdf },
	{ CCI_REG8(0x5184), 0x02 },
	{ CCI_REG8(0x5185), 0x6c },
	{ CCI_REG8(0x5189), 0x48 },
	{ CCI_REG8(0x520a), 0x03 },
	{ CCI_REG8(0x520b), 0x0f },
	{ CCI_REG8(0x520c), 0x3f },
	{ CCI_REG8(0x580b), 0x03 },
	{ CCI_REG8(0x580d), 0x00 },
	{ CCI_REG8(0x580f), 0x00 },
	{ CCI_REG8(0x5820), 0x00 },
	{ CCI_REG8(0x5821), 0x00 },
	{ CCI_REG8(0x3222), 0x03 },
};

static const struct cci_reg_sequence mode_2592_1944_regs[] = {
	{ OS05B10_REG_X_ADDR_START,	0x0130 },
	{ OS05B10_REG_Y_ADDR_START,	0x0000 },
	{ OS05B10_REG_X_ADDR_END,	0x0b5f },
	{ OS05B10_REG_Y_ADDR_END,	0x07a7 },
	{ OS05B10_REG_X_OUTPUT_SIZE,	0x0a20 },
	{ OS05B10_REG_Y_OUTPUT_SIZE,	0x0798 },
	{ OS05B10_REG_HTS,		0x06d0 },
	{ OS05B10_REG_ISP_X_WIN,	0x0008 },
	{ OS05B10_REG_ISP_Y_WIN,	0x0008 },
	{ OS05B10_REG_X_INC_ODD,	0x01 },
	{ OS05B10_REG_X_INC_EVEN,	0x01 },
	{ OS05B10_REG_Y_INC_ODD,	0x01 },
	{ OS05B10_REG_Y_INC_EVEN,	0x01 },
	{ OS05B10_REG_FORMAT1,		0x88 },
	{ OS05B10_REG_FORMAT2,		0x00 },
	{ CCI_REG8(0x3610), 0x87 },
	{ CCI_REG8(0x3620), 0x0c },
	{ CCI_REG8(0x3662), 0x10 },
	{ CCI_REG8(0x3714), 0x24 },
	{ CCI_REG8(0x373f), 0xb0 },
	{ CCI_REG8(0x37bf), 0x05 },
	{ CCI_REG8(0x37c2), 0x04 },
	{ CCI_REG8(0x37d9), 0x08 },
	{ CCI_REG8(0x3832), 0x08 },
	{ CCI_REG8(0x3c86), 0x01 },
	{ CCI_REG8(0x3d8c), 0x01 },
	{ CCI_REG8(0x3f03), 0x08 },
	{ CCI_REG8(0x4008), 0x02 },
	{ CCI_REG8(0x4009), 0x0d },
	{ CCI_REG8(0x400a), 0x01 },
	{ CCI_REG8(0x4041), 0x07 },
	{ CCI_REG8(0x4505), 0xc4 },
	{ CCI_REG8(0x4837), 0x12 },
};

static const struct cci_reg_sequence mode_1920_1080_regs[] = {
	{ OS05B10_REG_X_ADDR_START,	0x0280 },
	{ OS05B10_REG_Y_ADDR_START,	0x01b4 },
	{ OS05B10_REG_X_ADDR_END,	0x0a0f },
	{ OS05B10_REG_Y_ADDR_END,	0x05f3 },
	{ OS05B10_REG_X_OUTPUT_SIZE,	0x0780 },
	{ OS05B10_REG_Y_OUTPUT_SIZE,	0x0438 },
	{ OS05B10_REG_HTS,		0x06d0 },
	{ OS05B10_REG_ISP_X_WIN,	0x0008 },
	{ OS05B10_REG_ISP_Y_WIN,	0x0008 },
	{ OS05B10_REG_X_INC_ODD,	0x01 },
	{ OS05B10_REG_X_INC_EVEN,	0x01 },
	{ OS05B10_REG_Y_INC_ODD,	0x01 },
	{ OS05B10_REG_Y_INC_EVEN,	0x01 },
	{ OS05B10_REG_FORMAT1,		0x88 },
	{ OS05B10_REG_FORMAT2,		0x00 },
	{ CCI_REG8(0x3610), 0x87 },
	{ CCI_REG8(0x3620), 0x0c },
	{ CCI_REG8(0x3662), 0x10 },
	{ CCI_REG8(0x3714), 0x24 },
	{ CCI_REG8(0x373f), 0xb0 },
	{ CCI_REG8(0x37bf), 0x05 },
	{ CCI_REG8(0x37c2), 0x04 },
	{ CCI_REG8(0x37d9), 0x08 },
	{ CCI_REG8(0x3832), 0x08 },
	{ CCI_REG8(0x3c86), 0x03 },
	{ CCI_REG8(0x3d8c), 0x71 },
	{ CCI_REG8(0x3f03), 0x08 },
	{ CCI_REG8(0x4008), 0x02 },
	{ CCI_REG8(0x4009), 0x0d },
	{ CCI_REG8(0x400a), 0x02 },
	{ CCI_REG8(0x4041), 0x07 },
	{ CCI_REG8(0x4505), 0xc4 },
	{ CCI_REG8(0x4837), 0x0d },
};

static const struct cci_reg_sequence mode_1280_720_regs[] = {
	{ OS05B10_REG_X_ADDR_START,	0x0140 },
	{ OS05B10_REG_Y_ADDR_START,	0x00fc },
	{ OS05B10_REG_X_ADDR_END,	0x0b4f },
	{ OS05B10_REG_Y_ADDR_END,	0x06ab },
	{ OS05B10_REG_X_OUTPUT_SIZE,	0x0500 },
	{ OS05B10_REG_Y_OUTPUT_SIZE,	0x02d0 },
	{ OS05B10_REG_HTS,		0x0368 },
	{ OS05B10_REG_ISP_X_WIN,	0x0004 },
	{ OS05B10_REG_ISP_Y_WIN,	0x0004 },
	{ OS05B10_REG_X_INC_ODD,	0x03 },
	{ OS05B10_REG_X_INC_EVEN,	0x01 },
	{ OS05B10_REG_Y_INC_ODD,	0x03 },
	{ OS05B10_REG_Y_INC_EVEN,	0x01 },
	{ OS05B10_REG_FORMAT1,		0x8b },
	{ OS05B10_REG_FORMAT2,		0x00 },
	{ CCI_REG8(0x3610), 0x57 },
	{ CCI_REG8(0x3620), 0x01 },
	{ CCI_REG8(0x3662), 0x08 },
	{ CCI_REG8(0x3714), 0x28 },
	{ CCI_REG8(0x373f), 0xa0 },
	{ CCI_REG8(0x37bf), 0x05 },
	{ CCI_REG8(0x37c2), 0x14 },
	{ CCI_REG8(0x37d9), 0x04 },
	{ CCI_REG8(0x3832), 0x00 },
	{ CCI_REG8(0x3c86), 0x03 },
	{ CCI_REG8(0x3d8c), 0x71 },
	{ CCI_REG8(0x3f03), 0x1d },
	{ CCI_REG8(0x4008), 0x01 },
	{ CCI_REG8(0x4009), 0x06 },
	{ CCI_REG8(0x400a), 0x02 },
	{ CCI_REG8(0x4041), 0x03 },
	{ CCI_REG8(0x4505), 0xe4 },
	{ CCI_REG8(0x4837), 0x0d },
};

struct os05b10 {
	struct device *dev;
	struct regmap *cci;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *xclk;
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(os05b10_supply_name)];

	/* V4L2 Controls */
	struct v4l2_ctrl_handler handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *gain;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;

	u32 link_freq_index;
	u32 data_lanes;
};

struct os05b10_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

struct os05b10_mode {
	u32 width;
	u32 height;
	u32 vts;
	u32 hts;
	u32 exp;
	u8 bpp;
	struct os05b10_reg_list reg_list;
};

static const struct os05b10_mode supported_modes_12bit[] = {
	{
		.width = 2592,
		.height = 1944,
		.vts = 2007,
		.hts = 1744,
		.exp = 1900,
		.bpp = 12,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2592_1944_regs),
			.regs = mode_2592_1944_regs,
		},
	},
	{	/* 40 fps */
		.width = 1920,
		.height = 1080,
		.vts = 1504,
		.hts = 1744,
		.exp = 1472,
		.bpp = 12,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1920_1080_regs),
			.regs = mode_1920_1080_regs,
		},
	},
	{	/* 2x2 binning 120 fps */
		.width = 1280,
		.height = 720,
		.vts = 1003,
		.hts = 872,
		.exp = 970,
		.bpp = 12,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1280_720_regs),
			.regs = mode_1280_720_regs,
		},
	},
};

static const struct os05b10_mode supported_modes_10bit[] = {
	{
		.width = 2592,
		.height = 1944,
		.vts = 2006,
		.hts = 1744,
		.exp = 1944,
		.bpp = 10,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2592_1944_regs),
			.regs = mode_2592_1944_regs,
		},
	},
	{	/* 40 fps */
		.width = 1920,
		.height = 1080,
		.vts = 1504,
		.hts = 1744,
		.exp = 1472,
		.bpp = 10,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1920_1080_regs),
			.regs = mode_1920_1080_regs,
		},
	},
	{	/* 2x2 binning 120 fps */
		.width = 1280,
		.height = 720,
		.vts = 1003,
		.hts = 872,
		.exp = 970,
		.bpp = 10,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1280_720_regs),
			.regs = mode_1280_720_regs,
		},
	},
};

static const s64 link_frequencies[] = {
	OS05B10_LINK_FREQ_600MHZ,
};

static const u32 os05b10_mbus_codes[] = {
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SBGGR12_1X12,
};

static const char * const os05b10_test_pattern_menu[] = {
	"Disabled",
	"colour bar type 1",
	"colour bar type 2",
	"colour bar type 3",
	"colour bar type 4",
	"color square",
	"black-white square",
	"transparent effect",
	"rolling bar effect",
};

static const int os05b10_tp_val[] = {
	OS05B10_DISABLED,
	OS05B10_COLOR_BAR_1,
	OS05B10_COLOR_BAR_2,
	OS05B10_COLOR_BAR_3,
	OS05B10_COLOR_BAR_4,
	OS05B10_COLOR_SQUARE,
	OS05B10_BW_SQUARE,
	OS05B10_TRANSPARENT_EFFECT,
	OS05B10_ROLLING_BAR_EFFECT,
};

static inline struct os05b10 *to_os05b10(struct v4l2_subdev *sd)
{
	return container_of_const(sd, struct os05b10, sd);
};

static u32 os05b10_get_format_code(struct os05b10 *os05b10, u8 bpp)
{
	static const u32 codes_12[2][2] = {
		{ MEDIA_BUS_FMT_SBGGR12_1X12, MEDIA_BUS_FMT_SGBRG12_1X12, },
		{ MEDIA_BUS_FMT_SGRBG12_1X12, MEDIA_BUS_FMT_SRGGB12_1X12, },
	};

	static const u32 codes_10[2][2] = {
		{ MEDIA_BUS_FMT_SBGGR10_1X10, MEDIA_BUS_FMT_SGBRG10_1X10, },
		{ MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SRGGB10_1X10, },
	};

	const u32 (*codes)[2] = (bpp == 12) ? codes_12 : codes_10;

	u32 code = codes[os05b10->vflip->val][os05b10->hflip->val];

	return code;
}

static int os05b10_update_test_pattern(struct os05b10 *os05b10, u32 pattern)
{
	if (pattern >= ARRAY_SIZE(os05b10_test_pattern_menu))
		return -EINVAL;

	return cci_write(os05b10->cci, OS05B10_REG_PRE_ISP_20_0,
			 os05b10_tp_val[pattern], NULL);
}

static inline void get_mode_table(unsigned int code,
				  const struct os05b10_mode **mode_list,
				  unsigned int *num_modes)
{
	switch (code) {
	case MEDIA_BUS_FMT_SBGGR12_1X12:
		*mode_list = supported_modes_12bit;
		*num_modes = ARRAY_SIZE(supported_modes_12bit);
		break;

	case MEDIA_BUS_FMT_SBGGR10_1X10:
		*mode_list = supported_modes_10bit;
		*num_modes = ARRAY_SIZE(supported_modes_10bit);
		break;
	default:
		*mode_list = NULL;
		*num_modes = 0;
		break;
	}
}

static int os05b10_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct os05b10 *os05b10 = container_of_const(ctrl->handler,
						     struct os05b10, handler);
	const struct os05b10_mode *mode_list;
	const struct os05b10_mode *mode;
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *fmt;
	unsigned int num_modes;
	int vmax, ret;

	state = v4l2_subdev_get_locked_active_state(&os05b10->sd);
	fmt = v4l2_subdev_state_get_format(state, 0);

	get_mode_table(fmt->code, &mode_list, &num_modes);
	mode = v4l2_find_nearest_size(mode_list, num_modes, width, height,
				      fmt->width, fmt->height);

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Honour the VBLANK limits when setting exposure. */
		s64 max = fmt->height + ctrl->val - OS05B10_EXPOSURE_MARGIN;
		ret = __v4l2_ctrl_modify_range(os05b10->exposure,
					       os05b10->exposure->minimum, max,
					       os05b10->exposure->step,
					       mode->exp);
		if (ret)
			return ret;
	}

	if (pm_runtime_get_if_active(os05b10->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		vmax = fmt->height + ctrl->val;
		ret = cci_write(os05b10->cci, OS05B10_REG_VTS, vmax, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(os05b10->cci, OS05B10_REG_ANALOG_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(os05b10->cci, OS05B10_REG_DIGITAL_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(os05b10->cci, OS05B10_REG_EXPOSURE,
				ctrl->val, NULL);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = cci_update_bits(os05b10->cci, OS05B10_REG_FORMAT1,
				      GENMASK(5, 3),
				      (!os05b10->hflip->val) << 3 |
				      os05b10->vflip->val << 5 |
				      os05b10->vflip->val << 4, NULL);
		if (ret)
			return ret;

		ret = cci_write(os05b10->cci, OS05B10_REG_ANALOG_FLIP,
				(os05b10->vflip->val == 1) ?
				OS05B10_FLIP_ENABLE : OS05B10_FLIP_DISABLE,
				NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = os05b10_update_test_pattern(os05b10, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(os05b10->dev);

	return ret;
}

static int os05b10_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct os05b10 *os05b10 = to_os05b10(sd);

	if (code->index >= ARRAY_SIZE(os05b10_mbus_codes))
		return -EINVAL;

	code->code = os05b10_get_format_code(os05b10,
					     (code->index == 1) ? 12 : 10);
	return 0;
}

static u64 os05b10_pixel_rate(struct os05b10 *os05b10,
			      const struct os05b10_mode *mode)
{
	u64 link_freq = link_frequencies[os05b10->link_freq_index];
	u64 pixel_rate = div_u64(link_freq * 2 * os05b10->data_lanes, mode->bpp);

	dev_dbg(os05b10->dev,
		"link_freq=%llu bpp=%u lanes=%u pixel_rate=%llu\n",
		link_freq, mode->bpp, os05b10->data_lanes, pixel_rate);

	return pixel_rate;
}

static int os05b10_set_framing_limits(struct os05b10 *os05b10,
				      const struct os05b10_mode *mode)
{
	u64 pixel_rate = os05b10_pixel_rate(os05b10, mode);
	u32 hblank, vblank, vblank_max;
	int ret;

	ret = __v4l2_ctrl_modify_range(os05b10->pixel_rate, pixel_rate,
				       pixel_rate, 1, pixel_rate);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl_int64(os05b10->pixel_rate, pixel_rate);
	if (ret)
		return ret;

	hblank = mode->hts - mode->width;
	ret = __v4l2_ctrl_modify_range(os05b10->hblank, hblank, hblank, 1,
				       hblank);
	if (ret)
		return ret;

	vblank = mode->vts - mode->height;
	vblank_max = OS05B10_VTS_MAX - mode->height;
	ret = __v4l2_ctrl_modify_range(os05b10->vblank, 0, vblank_max, 1,
				       vblank);
	if (ret)
		return ret;

	return __v4l2_ctrl_s_ctrl(os05b10->vblank, vblank);
}

static int os05b10_set_pad_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_format *fmt)
{
	struct os05b10 *os05b10 = to_os05b10(sd);
	const struct os05b10_mode *mode_list;
	struct v4l2_mbus_framefmt *format;
	const struct os05b10_mode *mode;
	unsigned int num_modes;
	int ret;

	get_mode_table(fmt->format.code, &mode_list, &num_modes);
	mode = v4l2_find_nearest_size(mode_list, num_modes, width, height,
				      fmt->format.width, fmt->format.height);

	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	format = v4l2_subdev_state_get_format(sd_state, 0);

	*format = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ret = os05b10_set_framing_limits(os05b10, mode);
		if (ret)
			return ret;
	}

	return 0;
}

static int os05b10_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = os05b10_native_area;
		return 0;
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = os05b10_active_area;
		return 0;
	default:
		return -EINVAL;
	}
}

static int os05b10_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	const struct os05b10_mode *mode_list;
	unsigned int num_modes;

	get_mode_table(fse->code, &mode_list, &num_modes);

	if (fse->index >= num_modes)
		return -EINVAL;

	fse->min_width = mode_list[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = mode_list[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int os05b10_enable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct os05b10 *os05b10 = to_os05b10(sd);
	const struct os05b10_reg_list *reg_list;
	const struct v4l2_mbus_framefmt *fmt;
	const struct os05b10_mode *mode_list;
	const struct os05b10_mode *mode;
	unsigned int num_modes;
	int ret;

	fmt = v4l2_subdev_state_get_format(state, 0);
	get_mode_table(fmt->code, &mode_list, &num_modes);
	mode = v4l2_find_nearest_size(mode_list, num_modes, width, height,
				      fmt->width, fmt->height);

	ret = pm_runtime_resume_and_get(os05b10->dev);
	if (ret < 0)
		return ret;

	/* Write common registers */
	ret = cci_multi_reg_write(os05b10->cci, os05b10_common_regs,
				  ARRAY_SIZE(os05b10_common_regs), NULL);
	if (ret) {
		dev_err(os05b10->dev, "failed to write common registers\n");
		goto err_rpm_put;
	}

	ret = cci_write(os05b10->cci, OS05B10_REG_MIPI_SC_CTRL_1,
			(mode->bpp == 12) ? OS05B10_12BIT_MODE :
			OS05B10_10BIT_MODE, NULL);
	if (ret) {
		dev_err(os05b10->dev, "failed to write pixel bit registers\n");
		goto err_rpm_put;
	}

	/* Write sensor mode registers */
	reg_list = &mode->reg_list;
	ret = cci_multi_reg_write(os05b10->cci, reg_list->regs,
				  reg_list->num_of_regs, NULL);
	if (ret) {
		dev_err(os05b10->dev, "fail to write initial registers\n");
		goto err_rpm_put;
	}

	/* Apply customized user controls */
	ret = __v4l2_ctrl_handler_setup(os05b10->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	/* Stream ON */
	ret = cci_write(os05b10->cci, OS05B10_REG_CTRL_MODE,
			OS05B10_MODE_STREAMING, NULL);
	if (ret)
		goto err_rpm_put;

	__v4l2_ctrl_grab(os05b10->vflip, true);
	__v4l2_ctrl_grab(os05b10->hflip, true);

	return 0;

err_rpm_put:
	pm_runtime_put(os05b10->dev);

	return ret;
}

static int os05b10_disable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   u32 pad, u64 streams_mask)
{
	struct os05b10 *os05b10 = to_os05b10(sd);
	int ret;

	ret = cci_write(os05b10->cci, OS05B10_REG_CTRL_MODE,
			OS05B10_MODE_STANDBY, NULL);
	if (ret)
		dev_err(os05b10->dev, "failed to set stream off\n");

	__v4l2_ctrl_grab(os05b10->vflip, false);
	__v4l2_ctrl_grab(os05b10->hflip, false);

	pm_runtime_put(os05b10->dev);

	return 0;
}

static int os05b10_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct os05b10 *os05b10 = to_os05b10(sd);
	struct v4l2_mbus_framefmt *format;
	const struct os05b10_mode *mode;

	/* Initialize try_fmt */
	format = v4l2_subdev_state_get_format(state, 0);

	mode = &supported_modes_10bit[0];
	format->code = os05b10_get_format_code(os05b10, 10);

	/* Update image pad formate */
	format->width = mode->width;
	format->height = mode->height;
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	return 0;
}

static const struct v4l2_subdev_video_ops os05b10_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops os05b10_pad_ops = {
	.enum_mbus_code = os05b10_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = os05b10_set_pad_format,
	.get_selection = os05b10_get_selection,
	.enum_frame_size = os05b10_enum_frame_size,
	.enable_streams = os05b10_enable_streams,
	.disable_streams = os05b10_disable_streams,
};

static const struct v4l2_subdev_internal_ops os05b10_internal_ops = {
	.init_state = os05b10_init_state,
};

static const struct v4l2_subdev_ops os05b10_subdev_ops = {
	.video = &os05b10_video_ops,
	.pad = &os05b10_pad_ops,
};

static const struct v4l2_ctrl_ops os05b10_ctrl_ops = {
	.s_ctrl = os05b10_set_ctrl,
};

static int os05b10_identify_module(struct os05b10 *os05b10)
{
	int ret;
	u64 val;

	ret = cci_read(os05b10->cci, OS05B10_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(os05b10->dev, ret,
				     "failed to read chip id %x\n",
				     OS05B10_CHIP_ID);

	if (val != OS05B10_CHIP_ID)
		return dev_err_probe(os05b10->dev, -ENODEV,
				     "chip id mismatch: %x!=%llx\n",
				     OS05B10_CHIP_ID, val);

	return 0;
}

static int os05b10_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct os05b10 *os05b10 = to_os05b10(sd);
	unsigned long delay_us;
	int ret;

	/* Enable power rails */
	ret = regulator_bulk_enable(ARRAY_SIZE(os05b10_supply_name),
				    os05b10->supplies);
	if (ret) {
		dev_err(os05b10->dev, "failed to enable regulators\n");
		return ret;
	}

	/* Enable xclk */
	ret = clk_prepare_enable(os05b10->xclk);
	if (ret) {
		dev_err(os05b10->dev, "failed to enable clock\n");
		goto err_regulator_off;
	}

	gpiod_set_value_cansleep(os05b10->reset_gpio, 0);

	/* Delay T1 */
	fsleep(5 * USEC_PER_MSEC);

	/* Delay T2 (8192 cycles before SCCB/I2C access) */
	delay_us = DIV_ROUND_UP(8192, OS05B10_XCLK_FREQ / 1000 / 1000);
	usleep_range(delay_us, delay_us * 2);

	return 0;

err_regulator_off:
	regulator_bulk_disable(ARRAY_SIZE(os05b10_supply_name),
			       os05b10->supplies);

	return ret;
}

static int os05b10_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct os05b10 *os05b10 = to_os05b10(sd);

	gpiod_set_value_cansleep(os05b10->reset_gpio, 1);

	regulator_bulk_disable(ARRAY_SIZE(os05b10_supply_name),
			       os05b10->supplies);
	clk_disable_unprepare(os05b10->xclk);

	return 0;
}

static int os05b10_parse_endpoint(struct os05b10 *os05b10)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	unsigned long link_freq_bitmap;
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(os05b10->dev), 0, 0, 0);
	if (!ep) {
		dev_err(os05b10->dev, "Failed to get next endpoint\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != 4) {
		ret = dev_err_probe(os05b10->dev, -EINVAL,
				    "only 4 data lanes are supported\n");
		goto error_out;
	}

	os05b10->data_lanes = bus_cfg.bus.mipi_csi2.num_data_lanes;

	ret = v4l2_link_freq_to_bitmap(os05b10->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       link_frequencies,
				       ARRAY_SIZE(link_frequencies),
				       &link_freq_bitmap);
	if (ret) {
		dev_err(os05b10->dev, "only 600MHz frequency is available\n");
		goto error_out;
	}

	os05b10->link_freq_index = __ffs(link_freq_bitmap);

error_out:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}


static int os05b10_init_controls(struct os05b10 *os05b10)
{
	const struct os05b10_mode *mode = &supported_modes_10bit[0];
	u64 hblank_def, vblank_def, exp_max, pixel_rate;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	int ret;

	ctrl_hdlr = &os05b10->handler;
	v4l2_ctrl_handler_init(ctrl_hdlr, 12);

	pixel_rate = os05b10_pixel_rate(os05b10, mode);
	os05b10->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &os05b10_ctrl_ops,
						V4L2_CID_PIXEL_RATE, pixel_rate,
						pixel_rate, 1, pixel_rate);

	os05b10->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &os05b10_ctrl_ops,
						    V4L2_CID_LINK_FREQ,
						    ARRAY_SIZE(link_frequencies) - 1,
						    os05b10->link_freq_index,
						    link_frequencies);

	if (os05b10->link_freq)
		os05b10->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hblank_def = mode->hts - mode->width;
	os05b10->hblank = v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_HBLANK,
					    hblank_def, hblank_def,
					    1, hblank_def);
	if (os05b10->hblank)
		os05b10->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts - mode->height;
	os05b10->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &os05b10_ctrl_ops,
					    V4L2_CID_VBLANK, vblank_def,
					    OS05B10_VTS_MAX - mode->height,
					    1, vblank_def);

	exp_max = mode->vts - OS05B10_EXPOSURE_MARGIN;
	os05b10->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &os05b10_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      OS05B10_EXPOSURE_MIN,
					      exp_max, OS05B10_EXPOSURE_STEP,
					      mode->exp);

	os05b10->gain = v4l2_ctrl_new_std(ctrl_hdlr, &os05b10_ctrl_ops,
					  V4L2_CID_ANALOGUE_GAIN,
					  OS05B10_ANALOG_GAIN_MIN,
					  OS05B10_ANALOG_GAIN_MAX,
					  OS05B10_ANALOG_GAIN_STEP,
					  OS05B10_ANALOG_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &os05b10_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  OS05B10_DIGITAL_GAIN_MIN, OS05B10_DIGITAL_GAIN_MAX,
			  OS05B10_DIGITAL_GAIN_STEP, OS05B10_DIGITAL_GAIN_DEFAULT);

	os05b10->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &os05b10_ctrl_ops,
					   V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (os05b10->hflip)
		os05b10->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	os05b10->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &os05b10_ctrl_ops,
					   V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (os05b10->vflip)
		os05b10->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &os05b10_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(os05b10_test_pattern_menu) - 1,
				     0, 0, os05b10_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(os05b10->dev, "control init failed (%d)\n", ret);
		goto error;
	}

	ret = v4l2_fwnode_device_parse(os05b10->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &os05b10_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	os05b10->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int os05b10_probe(struct i2c_client *client)
{
	struct os05b10 *os05b10;
	unsigned int xclk_freq;
	int ret;

	os05b10 = devm_kzalloc(&client->dev, sizeof(*os05b10), GFP_KERNEL);
	if (!os05b10)
		return -ENOMEM;

	os05b10->client = client;
	os05b10->dev = &client->dev;

	v4l2_i2c_subdev_init(&os05b10->sd, client, &os05b10_subdev_ops);

	os05b10->cci = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(os05b10->cci))
		return dev_err_probe(os05b10->dev, PTR_ERR(os05b10->cci),
				     "failed to initialize CCI\n");

	os05b10->xclk = devm_v4l2_sensor_clk_get(os05b10->dev, NULL);
	if (IS_ERR(os05b10->xclk))
		return dev_err_probe(os05b10->dev, PTR_ERR(os05b10->xclk),
				     "failed to get xclk\n");

	xclk_freq = clk_get_rate(os05b10->xclk);
	if (xclk_freq != OS05B10_XCLK_FREQ)
		return dev_err_probe(os05b10->dev, -EINVAL,
				     "xclk frequency not supported: %d Hz\n",
				     xclk_freq);

	for (unsigned int i = 0; i < ARRAY_SIZE(os05b10_supply_name); i++)
		os05b10->supplies[i].supply = os05b10_supply_name[i];

	ret = devm_regulator_bulk_get(os05b10->dev,
				      ARRAY_SIZE(os05b10_supply_name),
				      os05b10->supplies);
	if (ret)
		return dev_err_probe(os05b10->dev, ret,
				     "failed to get regulators\n");

	ret = os05b10_parse_endpoint(os05b10);
	if (ret)
		return dev_err_probe(os05b10->dev, ret,
				     "failed to parse endpoint configuration\n");

	os05b10->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(os05b10->reset_gpio))
		return dev_err_probe(os05b10->dev, PTR_ERR(os05b10->reset_gpio),
				     "failed to get reset GPIO\n");

	ret = os05b10_power_on(os05b10->dev);
	if (ret)
		return ret;

	ret = os05b10_identify_module(os05b10);
	if (ret)
		goto error_power_off;

	/* This needs the pm runtime to be registered. */
	ret = os05b10_init_controls(os05b10);
	if (ret)
		goto error_power_off;

	/* Initialize subdev */
	os05b10->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	os05b10->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	os05b10->sd.internal_ops = &os05b10_internal_ops;
	os05b10->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&os05b10->sd.entity, 1, &os05b10->pad);
	if (ret) {
		dev_err_probe(os05b10->dev, ret,
			      "failed to init entity pads\n");
		goto error_handler_free;
	}

	os05b10->sd.state_lock = os05b10->handler.lock;
	ret = v4l2_subdev_init_finalize(&os05b10->sd);
	if (ret < 0) {
		dev_err_probe(os05b10->dev, ret, "subdev init error\n");
		goto error_media_entity;
	}

	pm_runtime_set_active(os05b10->dev);
	pm_runtime_enable(os05b10->dev);

	ret = v4l2_async_register_subdev_sensor(&os05b10->sd);
	if (ret < 0) {
		dev_err_probe(os05b10->dev, ret,
			      "failed to register os05b10 sub-device\n");
		goto error_subdev_cleanup;
	}

	pm_runtime_idle(os05b10->dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&os05b10->sd);
	pm_runtime_disable(os05b10->dev);
	pm_runtime_set_suspended(os05b10->dev);

error_media_entity:
	media_entity_cleanup(&os05b10->sd.entity);

error_handler_free:
	v4l2_ctrl_handler_free(os05b10->sd.ctrl_handler);

error_power_off:
	os05b10_power_off(os05b10->dev);

	return ret;
}

static void os05b10_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct os05b10 *os05b10 = to_os05b10(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(&os05b10->sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(os05b10->sd.ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		os05b10_power_off(&client->dev);
		pm_runtime_set_suspended(&client->dev);
	}
}

static DEFINE_RUNTIME_DEV_PM_OPS(os05b10_pm_ops, os05b10_power_off,
				 os05b10_power_on, NULL);

static const struct of_device_id os05b10_id[] = {
	{ .compatible = "ovti,os05b10" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, os05b10_id);

static struct i2c_driver os05b10_driver = {
	.driver = {
		.name = "os05b10",
		.pm = pm_ptr(&os05b10_pm_ops),
		.of_match_table = os05b10_id,
	},
	.probe = os05b10_probe,
	.remove = os05b10_remove,
};

module_i2c_driver(os05b10_driver);

MODULE_DESCRIPTION("OS05B10 Camera Sensor Driver");
MODULE_AUTHOR("Himanshu Bhavani <himanshu.bhavani@siliconsignals.io>");
MODULE_AUTHOR("Elgin Perumbilly <elgin.perumbilly@siliconsignals.io>");
MODULE_LICENSE("GPL");
