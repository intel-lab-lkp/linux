// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the OV5645 camera sensor.
 *
 * Copyright (c) 2011-2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2015 By Tech Design S.L. All Rights Reserved.
 * Copyright (C) 2012-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * Based on:
 * - the OV5645 driver from QC msm-3.10 kernel on codeaurora.org:
 *   https://us.codeaurora.org/cgit/quic/la/kernel/msm-3.10/tree/drivers/
 *       media/platform/msm/camera_v2/sensor/ov5645.c?h=LA.BR.1.2.4_rb1.41
 * - the OV5640 driver posted on linux-media:
 *   https://www.mail-archive.com/linux-media%40vger.kernel.org/msg92671.html
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define OV5645_SYSTEM_CTRL0		CCI_REG8(0x3008)
#define		OV5645_SYSTEM_CTRL0_START	0x02
#define		OV5645_SYSTEM_CTRL0_STOP	0x42
#define OV5645_CHIP_ID_HIGH		CCI_REG8(0x300a)
#define		OV5645_CHIP_ID_HIGH_BYTE	0x56
#define OV5645_CHIP_ID_LOW		CCI_REG8(0x300b)
#define		OV5645_CHIP_ID_LOW_BYTE		0x45
#define OV5645_IO_MIPI_CTRL00		CCI_REG8(0x300e)
#define OV5645_PAD_OUTPUT00		CCI_REG8(0x3019)
#define OV5645_AWB_MANUAL_CONTROL	CCI_REG8(0x3406)
#define		OV5645_AWB_MANUAL_ENABLE	BIT(0)
#define OV5645_AEC_PK_MANUAL		CCI_REG8(0x3503)
#define		OV5645_AEC_MANUAL_ENABLE	BIT(0)
#define		OV5645_AGC_MANUAL_ENABLE	BIT(1)
#define OV5645_TIMING_TC_REG20		CCI_REG8(0x3820)
#define		OV5645_SENSOR_VFLIP		BIT(1)
#define		OV5645_ISP_VFLIP		BIT(2)
#define OV5645_TIMING_TC_REG21		CCI_REG8(0x3821)
#define		OV5645_SENSOR_MIRROR		BIT(1)
#define OV5645_MIPI_CTRL00		CCI_REG8(0x4800)
#define OV5645_PRE_ISP_TEST_SETTING_1	CCI_REG8(0x503d)
#define		OV5645_TEST_PATTERN_MASK	0x3
#define		OV5645_SET_TEST_PATTERN(x)	((x) & OV5645_TEST_PATTERN_MASK)
#define		OV5645_TEST_PATTERN_ENABLE	BIT(7)
#define OV5645_SDE_SAT_U		CCI_REG8(0x5583)
#define OV5645_SDE_SAT_V		CCI_REG8(0x5584)

/* regulator supplies */
static const char * const ov5645_supply_name[] = {
	"vdddo", /* Digital I/O (1.8V) supply */
	"vdda",  /* Analog (2.8V) supply */
	"vddd",  /* Digital Core (1.5V) supply */
};

#define OV5645_NUM_SUPPLIES ARRAY_SIZE(ov5645_supply_name)

#define OV5645_PAD_SOURCE	0

struct ov5645_mode_info {
	u32 width;
	u32 height;
	const struct cci_reg_sequence *data;
	u32 data_size;
	u32 pixel_clock;
	u32 link_freq;
};

struct ov5645 {
	struct i2c_client *i2c_client;
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_fwnode_endpoint ep;
	struct v4l2_rect crop;
	struct clk *xclk;

	struct regulator_bulk_data supplies[OV5645_NUM_SUPPLIES];

	const struct ov5645_mode_info *current_mode;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_clock;
	struct v4l2_ctrl *link_freq;

	/* Cached register values */
	u64 aec_pk_manual;
	u64 timing_tc_reg20;
	u64 timing_tc_reg21;

	struct gpio_desc *enable_gpio;
	struct gpio_desc *rst_gpio;
};

static inline struct ov5645 *to_ov5645(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov5645, sd);
}

static const struct cci_reg_sequence ov5645_global_init_setting[] = {
	{ CCI_REG8(0x3103), 0x11 },
	{ CCI_REG8(0x3008), 0x42 },
	{ CCI_REG8(0x3103), 0x03 },
	{ CCI_REG8(0x3503), 0x07 },
	{ CCI_REG8(0x3002), 0x1c },
	{ CCI_REG8(0x3006), 0xc3 },
	{ CCI_REG8(0x3017), 0x00 },
	{ CCI_REG8(0x3018), 0x00 },
	{ CCI_REG8(0x302e), 0x0b },
	{ CCI_REG8(0x3037), 0x13 },
	{ CCI_REG8(0x3108), 0x01 },
	{ CCI_REG8(0x3611), 0x06 },
	{ CCI_REG8(0x3500), 0x00 },
	{ CCI_REG8(0x3501), 0x01 },
	{ CCI_REG8(0x3502), 0x00 },
	{ CCI_REG8(0x350a), 0x00 },
	{ CCI_REG8(0x350b), 0x3f },
	{ CCI_REG8(0x3620), 0x33 },
	{ CCI_REG8(0x3621), 0xe0 },
	{ CCI_REG8(0x3622), 0x01 },
	{ CCI_REG8(0x3630), 0x2e },
	{ CCI_REG8(0x3631), 0x00 },
	{ CCI_REG8(0x3632), 0x32 },
	{ CCI_REG8(0x3633), 0x52 },
	{ CCI_REG8(0x3634), 0x70 },
	{ CCI_REG8(0x3635), 0x13 },
	{ CCI_REG8(0x3636), 0x03 },
	{ CCI_REG8(0x3703), 0x5a },
	{ CCI_REG8(0x3704), 0xa0 },
	{ CCI_REG8(0x3705), 0x1a },
	{ CCI_REG8(0x3709), 0x12 },
	{ CCI_REG8(0x370b), 0x61 },
	{ CCI_REG8(0x370f), 0x10 },
	{ CCI_REG8(0x3715), 0x78 },
	{ CCI_REG8(0x3717), 0x01 },
	{ CCI_REG8(0x371b), 0x20 },
	{ CCI_REG8(0x3731), 0x12 },
	{ CCI_REG8(0x3901), 0x0a },
	{ CCI_REG8(0x3905), 0x02 },
	{ CCI_REG8(0x3906), 0x10 },
	{ CCI_REG8(0x3719), 0x86 },
	{ CCI_REG8(0x3810), 0x00 },
	{ CCI_REG8(0x3811), 0x10 },
	{ CCI_REG8(0x3812), 0x00 },
	{ CCI_REG8(0x3821), 0x01 },
	{ CCI_REG8(0x3824), 0x01 },
	{ CCI_REG8(0x3826), 0x03 },
	{ CCI_REG8(0x3828), 0x08 },
	{ CCI_REG8(0x3a19), 0xf8 },
	{ CCI_REG8(0x3c01), 0x34 },
	{ CCI_REG8(0x3c04), 0x28 },
	{ CCI_REG8(0x3c05), 0x98 },
	{ CCI_REG8(0x3c07), 0x07 },
	{ CCI_REG8(0x3c09), 0xc2 },
	{ CCI_REG8(0x3c0a), 0x9c },
	{ CCI_REG8(0x3c0b), 0x40 },
	{ CCI_REG8(0x3c01), 0x34 },
	{ CCI_REG8(0x4001), 0x02 },
	{ CCI_REG8(0x4514), 0x00 },
	{ CCI_REG8(0x4520), 0xb0 },
	{ CCI_REG8(0x460b), 0x37 },
	{ CCI_REG8(0x460c), 0x20 },
	{ CCI_REG8(0x4818), 0x01 },
	{ CCI_REG8(0x481d), 0xf0 },
	{ CCI_REG8(0x481f), 0x50 },
	{ CCI_REG8(0x4823), 0x70 },
	{ CCI_REG8(0x4831), 0x14 },
	{ CCI_REG8(0x5000), 0xa7 },
	{ CCI_REG8(0x5001), 0x83 },
	{ CCI_REG8(0x501d), 0x00 },
	{ CCI_REG8(0x501f), 0x00 },
	{ CCI_REG8(0x503d), 0x00 },
	{ CCI_REG8(0x505c), 0x30 },
	{ CCI_REG8(0x5181), 0x59 },
	{ CCI_REG8(0x5183), 0x00 },
	{ CCI_REG8(0x5191), 0xf0 },
	{ CCI_REG8(0x5192), 0x03 },
	{ CCI_REG8(0x5684), 0x10 },
	{ CCI_REG8(0x5685), 0xa0 },
	{ CCI_REG8(0x5686), 0x0c },
	{ CCI_REG8(0x5687), 0x78 },
	{ CCI_REG8(0x5a00), 0x08 },
	{ CCI_REG8(0x5a21), 0x00 },
	{ CCI_REG8(0x5a24), 0x00 },
	{ CCI_REG8(0x3008), 0x02 },
	{ CCI_REG8(0x3503), 0x00 },
	{ CCI_REG8(0x5180), 0xff },
	{ CCI_REG8(0x5181), 0xf2 },
	{ CCI_REG8(0x5182), 0x00 },
	{ CCI_REG8(0x5183), 0x14 },
	{ CCI_REG8(0x5184), 0x25 },
	{ CCI_REG8(0x5185), 0x24 },
	{ CCI_REG8(0x5186), 0x09 },
	{ CCI_REG8(0x5187), 0x09 },
	{ CCI_REG8(0x5188), 0x0a },
	{ CCI_REG8(0x5189), 0x75 },
	{ CCI_REG8(0x518a), 0x52 },
	{ CCI_REG8(0x518b), 0xea },
	{ CCI_REG8(0x518c), 0xa8 },
	{ CCI_REG8(0x518d), 0x42 },
	{ CCI_REG8(0x518e), 0x38 },
	{ CCI_REG8(0x518f), 0x56 },
	{ CCI_REG8(0x5190), 0x42 },
	{ CCI_REG8(0x5191), 0xf8 },
	{ CCI_REG8(0x5192), 0x04 },
	{ CCI_REG8(0x5193), 0x70 },
	{ CCI_REG8(0x5194), 0xf0 },
	{ CCI_REG8(0x5195), 0xf0 },
	{ CCI_REG8(0x5196), 0x03 },
	{ CCI_REG8(0x5197), 0x01 },
	{ CCI_REG8(0x5198), 0x04 },
	{ CCI_REG8(0x5199), 0x12 },
	{ CCI_REG8(0x519a), 0x04 },
	{ CCI_REG8(0x519b), 0x00 },
	{ CCI_REG8(0x519c), 0x06 },
	{ CCI_REG8(0x519d), 0x82 },
	{ CCI_REG8(0x519e), 0x38 },
	{ CCI_REG8(0x5381), 0x1e },
	{ CCI_REG8(0x5382), 0x5b },
	{ CCI_REG8(0x5383), 0x08 },
	{ CCI_REG8(0x5384), 0x0a },
	{ CCI_REG8(0x5385), 0x7e },
	{ CCI_REG8(0x5386), 0x88 },
	{ CCI_REG8(0x5387), 0x7c },
	{ CCI_REG8(0x5388), 0x6c },
	{ CCI_REG8(0x5389), 0x10 },
	{ CCI_REG8(0x538a), 0x01 },
	{ CCI_REG8(0x538b), 0x98 },
	{ CCI_REG8(0x5300), 0x08 },
	{ CCI_REG8(0x5301), 0x30 },
	{ CCI_REG8(0x5302), 0x10 },
	{ CCI_REG8(0x5303), 0x00 },
	{ CCI_REG8(0x5304), 0x08 },
	{ CCI_REG8(0x5305), 0x30 },
	{ CCI_REG8(0x5306), 0x08 },
	{ CCI_REG8(0x5307), 0x16 },
	{ CCI_REG8(0x5309), 0x08 },
	{ CCI_REG8(0x530a), 0x30 },
	{ CCI_REG8(0x530b), 0x04 },
	{ CCI_REG8(0x530c), 0x06 },
	{ CCI_REG8(0x5480), 0x01 },
	{ CCI_REG8(0x5481), 0x08 },
	{ CCI_REG8(0x5482), 0x14 },
	{ CCI_REG8(0x5483), 0x28 },
	{ CCI_REG8(0x5484), 0x51 },
	{ CCI_REG8(0x5485), 0x65 },
	{ CCI_REG8(0x5486), 0x71 },
	{ CCI_REG8(0x5487), 0x7d },
	{ CCI_REG8(0x5488), 0x87 },
	{ CCI_REG8(0x5489), 0x91 },
	{ CCI_REG8(0x548a), 0x9a },
	{ CCI_REG8(0x548b), 0xaa },
	{ CCI_REG8(0x548c), 0xb8 },
	{ CCI_REG8(0x548d), 0xcd },
	{ CCI_REG8(0x548e), 0xdd },
	{ CCI_REG8(0x548f), 0xea },
	{ CCI_REG8(0x5490), 0x1d },
	{ CCI_REG8(0x5580), 0x02 },
	{ CCI_REG8(0x5583), 0x40 },
	{ CCI_REG8(0x5584), 0x10 },
	{ CCI_REG8(0x5589), 0x10 },
	{ CCI_REG8(0x558a), 0x00 },
	{ CCI_REG8(0x558b), 0xf8 },
	{ CCI_REG8(0x5800), 0x3f },
	{ CCI_REG8(0x5801), 0x16 },
	{ CCI_REG8(0x5802), 0x0e },
	{ CCI_REG8(0x5803), 0x0d },
	{ CCI_REG8(0x5804), 0x17 },
	{ CCI_REG8(0x5805), 0x3f },
	{ CCI_REG8(0x5806), 0x0b },
	{ CCI_REG8(0x5807), 0x06 },
	{ CCI_REG8(0x5808), 0x04 },
	{ CCI_REG8(0x5809), 0x04 },
	{ CCI_REG8(0x580a), 0x06 },
	{ CCI_REG8(0x580b), 0x0b },
	{ CCI_REG8(0x580c), 0x09 },
	{ CCI_REG8(0x580d), 0x03 },
	{ CCI_REG8(0x580e), 0x00 },
	{ CCI_REG8(0x580f), 0x00 },
	{ CCI_REG8(0x5810), 0x03 },
	{ CCI_REG8(0x5811), 0x08 },
	{ CCI_REG8(0x5812), 0x0a },
	{ CCI_REG8(0x5813), 0x03 },
	{ CCI_REG8(0x5814), 0x00 },
	{ CCI_REG8(0x5815), 0x00 },
	{ CCI_REG8(0x5816), 0x04 },
	{ CCI_REG8(0x5817), 0x09 },
	{ CCI_REG8(0x5818), 0x0f },
	{ CCI_REG8(0x5819), 0x08 },
	{ CCI_REG8(0x581a), 0x06 },
	{ CCI_REG8(0x581b), 0x06 },
	{ CCI_REG8(0x581c), 0x08 },
	{ CCI_REG8(0x581d), 0x0c },
	{ CCI_REG8(0x581e), 0x3f },
	{ CCI_REG8(0x581f), 0x1e },
	{ CCI_REG8(0x5820), 0x12 },
	{ CCI_REG8(0x5821), 0x13 },
	{ CCI_REG8(0x5822), 0x21 },
	{ CCI_REG8(0x5823), 0x3f },
	{ CCI_REG8(0x5824), 0x68 },
	{ CCI_REG8(0x5825), 0x28 },
	{ CCI_REG8(0x5826), 0x2c },
	{ CCI_REG8(0x5827), 0x28 },
	{ CCI_REG8(0x5828), 0x08 },
	{ CCI_REG8(0x5829), 0x48 },
	{ CCI_REG8(0x582a), 0x64 },
	{ CCI_REG8(0x582b), 0x62 },
	{ CCI_REG8(0x582c), 0x64 },
	{ CCI_REG8(0x582d), 0x28 },
	{ CCI_REG8(0x582e), 0x46 },
	{ CCI_REG8(0x582f), 0x62 },
	{ CCI_REG8(0x5830), 0x60 },
	{ CCI_REG8(0x5831), 0x62 },
	{ CCI_REG8(0x5832), 0x26 },
	{ CCI_REG8(0x5833), 0x48 },
	{ CCI_REG8(0x5834), 0x66 },
	{ CCI_REG8(0x5835), 0x44 },
	{ CCI_REG8(0x5836), 0x64 },
	{ CCI_REG8(0x5837), 0x28 },
	{ CCI_REG8(0x5838), 0x66 },
	{ CCI_REG8(0x5839), 0x48 },
	{ CCI_REG8(0x583a), 0x2c },
	{ CCI_REG8(0x583b), 0x28 },
	{ CCI_REG8(0x583c), 0x26 },
	{ CCI_REG8(0x583d), 0xae },
	{ CCI_REG8(0x5025), 0x00 },
	{ CCI_REG8(0x3a0f), 0x30 },
	{ CCI_REG8(0x3a10), 0x28 },
	{ CCI_REG8(0x3a1b), 0x30 },
	{ CCI_REG8(0x3a1e), 0x26 },
	{ CCI_REG8(0x3a11), 0x60 },
	{ CCI_REG8(0x3a1f), 0x14 },
	{ CCI_REG8(0x0601), 0x02 },
	{ CCI_REG8(0x3008), 0x42 },
	{ CCI_REG8(0x3008), 0x02 },
	{ OV5645_IO_MIPI_CTRL00, 0x40 },
	{ OV5645_MIPI_CTRL00, 0x24 },
	{ OV5645_PAD_OUTPUT00, 0x70 }
};

static const struct cci_reg_sequence ov5645_setting_sxga[] = {
	{ CCI_REG8(0x3612), 0xa9 },
	{ CCI_REG8(0x3614), 0x50 },
	{ CCI_REG8(0x3618), 0x00 },
	{ CCI_REG8(0x3034), 0x18 },
	{ CCI_REG8(0x3035), 0x21 },
	{ CCI_REG8(0x3036), 0x70 },
	{ CCI_REG8(0x3600), 0x09 },
	{ CCI_REG8(0x3601), 0x43 },
	{ CCI_REG8(0x3708), 0x66 },
	{ CCI_REG8(0x370c), 0xc3 },
	{ CCI_REG8(0x3800), 0x00 },
	{ CCI_REG8(0x3801), 0x00 },
	{ CCI_REG8(0x3802), 0x00 },
	{ CCI_REG8(0x3803), 0x06 },
	{ CCI_REG8(0x3804), 0x0a },
	{ CCI_REG8(0x3805), 0x3f },
	{ CCI_REG8(0x3806), 0x07 },
	{ CCI_REG8(0x3807), 0x9d },
	{ CCI_REG8(0x3808), 0x05 },
	{ CCI_REG8(0x3809), 0x00 },
	{ CCI_REG8(0x380a), 0x03 },
	{ CCI_REG8(0x380b), 0xc0 },
	{ CCI_REG8(0x380c), 0x07 },
	{ CCI_REG8(0x380d), 0x68 },
	{ CCI_REG8(0x380e), 0x03 },
	{ CCI_REG8(0x380f), 0xd8 },
	{ CCI_REG8(0x3813), 0x06 },
	{ CCI_REG8(0x3814), 0x31 },
	{ CCI_REG8(0x3815), 0x31 },
	{ CCI_REG8(0x3820), 0x47 },
	{ CCI_REG8(0x3a02), 0x03 },
	{ CCI_REG8(0x3a03), 0xd8 },
	{ CCI_REG8(0x3a08), 0x01 },
	{ CCI_REG8(0x3a09), 0xf8 },
	{ CCI_REG8(0x3a0a), 0x01 },
	{ CCI_REG8(0x3a0b), 0xa4 },
	{ CCI_REG8(0x3a0e), 0x02 },
	{ CCI_REG8(0x3a0d), 0x02 },
	{ CCI_REG8(0x3a14), 0x03 },
	{ CCI_REG8(0x3a15), 0xd8 },
	{ CCI_REG8(0x3a18), 0x00 },
	{ CCI_REG8(0x4004), 0x02 },
	{ CCI_REG8(0x4005), 0x18 },
	{ CCI_REG8(0x4300), 0x32 },
	{ CCI_REG8(0x4202), 0x00 }
};

static const struct cci_reg_sequence ov5645_setting_1080p[] = {
	{ CCI_REG8(0x3612), 0xab },
	{ CCI_REG8(0x3614), 0x50 },
	{ CCI_REG8(0x3618), 0x04 },
	{ CCI_REG8(0x3034), 0x18 },
	{ CCI_REG8(0x3035), 0x11 },
	{ CCI_REG8(0x3036), 0x54 },
	{ CCI_REG8(0x3600), 0x08 },
	{ CCI_REG8(0x3601), 0x33 },
	{ CCI_REG8(0x3708), 0x63 },
	{ CCI_REG8(0x370c), 0xc0 },
	{ CCI_REG8(0x3800), 0x01 },
	{ CCI_REG8(0x3801), 0x50 },
	{ CCI_REG8(0x3802), 0x01 },
	{ CCI_REG8(0x3803), 0xb2 },
	{ CCI_REG8(0x3804), 0x08 },
	{ CCI_REG8(0x3805), 0xef },
	{ CCI_REG8(0x3806), 0x05 },
	{ CCI_REG8(0x3807), 0xf1 },
	{ CCI_REG8(0x3808), 0x07 },
	{ CCI_REG8(0x3809), 0x80 },
	{ CCI_REG8(0x380a), 0x04 },
	{ CCI_REG8(0x380b), 0x38 },
	{ CCI_REG8(0x380c), 0x09 },
	{ CCI_REG8(0x380d), 0xc4 },
	{ CCI_REG8(0x380e), 0x04 },
	{ CCI_REG8(0x380f), 0x60 },
	{ CCI_REG8(0x3813), 0x04 },
	{ CCI_REG8(0x3814), 0x11 },
	{ CCI_REG8(0x3815), 0x11 },
	{ CCI_REG8(0x3820), 0x47 },
	{ CCI_REG8(0x4514), 0x88 },
	{ CCI_REG8(0x3a02), 0x04 },
	{ CCI_REG8(0x3a03), 0x60 },
	{ CCI_REG8(0x3a08), 0x01 },
	{ CCI_REG8(0x3a09), 0x50 },
	{ CCI_REG8(0x3a0a), 0x01 },
	{ CCI_REG8(0x3a0b), 0x18 },
	{ CCI_REG8(0x3a0e), 0x03 },
	{ CCI_REG8(0x3a0d), 0x04 },
	{ CCI_REG8(0x3a14), 0x04 },
	{ CCI_REG8(0x3a15), 0x60 },
	{ CCI_REG8(0x3a18), 0x00 },
	{ CCI_REG8(0x4004), 0x06 },
	{ CCI_REG8(0x4005), 0x18 },
	{ CCI_REG8(0x4300), 0x32 },
	{ CCI_REG8(0x4202), 0x00 },
	{ CCI_REG8(0x4837), 0x0b }
};

static const struct cci_reg_sequence ov5645_setting_full[] = {
	{ CCI_REG8(0x3612), 0xab },
	{ CCI_REG8(0x3614), 0x50 },
	{ CCI_REG8(0x3618), 0x04 },
	{ CCI_REG8(0x3034), 0x18 },
	{ CCI_REG8(0x3035), 0x11 },
	{ CCI_REG8(0x3036), 0x54 },
	{ CCI_REG8(0x3600), 0x08 },
	{ CCI_REG8(0x3601), 0x33 },
	{ CCI_REG8(0x3708), 0x63 },
	{ CCI_REG8(0x370c), 0xc0 },
	{ CCI_REG8(0x3800), 0x00 },
	{ CCI_REG8(0x3801), 0x00 },
	{ CCI_REG8(0x3802), 0x00 },
	{ CCI_REG8(0x3803), 0x00 },
	{ CCI_REG8(0x3804), 0x0a },
	{ CCI_REG8(0x3805), 0x3f },
	{ CCI_REG8(0x3806), 0x07 },
	{ CCI_REG8(0x3807), 0x9f },
	{ CCI_REG8(0x3808), 0x0a },
	{ CCI_REG8(0x3809), 0x20 },
	{ CCI_REG8(0x380a), 0x07 },
	{ CCI_REG8(0x380b), 0x98 },
	{ CCI_REG8(0x380c), 0x0b },
	{ CCI_REG8(0x380d), 0x1c },
	{ CCI_REG8(0x380e), 0x07 },
	{ CCI_REG8(0x380f), 0xb0 },
	{ CCI_REG8(0x3813), 0x06 },
	{ CCI_REG8(0x3814), 0x11 },
	{ CCI_REG8(0x3815), 0x11 },
	{ CCI_REG8(0x3820), 0x47 },
	{ CCI_REG8(0x4514), 0x88 },
	{ CCI_REG8(0x3a02), 0x07 },
	{ CCI_REG8(0x3a03), 0xb0 },
	{ CCI_REG8(0x3a08), 0x01 },
	{ CCI_REG8(0x3a09), 0x27 },
	{ CCI_REG8(0x3a0a), 0x00 },
	{ CCI_REG8(0x3a0b), 0xf6 },
	{ CCI_REG8(0x3a0e), 0x06 },
	{ CCI_REG8(0x3a0d), 0x08 },
	{ CCI_REG8(0x3a14), 0x07 },
	{ CCI_REG8(0x3a15), 0xb0 },
	{ CCI_REG8(0x3a18), 0x01 },
	{ CCI_REG8(0x4004), 0x06 },
	{ CCI_REG8(0x4005), 0x18 },
	{ CCI_REG8(0x4300), 0x32 },
	{ CCI_REG8(0x4837), 0x0b },
	{ CCI_REG8(0x4202), 0x00 }
};

static const s64 link_freq[] = {
	224000000,
	336000000
};

static const struct ov5645_mode_info ov5645_mode_info_data[] = {
	{
		.width = 1280,
		.height = 960,
		.data = ov5645_setting_sxga,
		.data_size = ARRAY_SIZE(ov5645_setting_sxga),
		.pixel_clock = 112000000,
		.link_freq = 0 /* an index in link_freq[] */
	},
	{
		.width = 1920,
		.height = 1080,
		.data = ov5645_setting_1080p,
		.data_size = ARRAY_SIZE(ov5645_setting_1080p),
		.pixel_clock = 168000000,
		.link_freq = 1 /* an index in link_freq[] */
	},
	{
		.width = 2592,
		.height = 1944,
		.data = ov5645_setting_full,
		.data_size = ARRAY_SIZE(ov5645_setting_full),
		.pixel_clock = 168000000,
		.link_freq = 1 /* an index in link_freq[] */
	},
};

static int ov5645_set_aec_mode(struct ov5645 *ov5645, u32 mode)
{
	u8 val = ov5645->aec_pk_manual;
	int ret;

	if (mode == V4L2_EXPOSURE_AUTO)
		val &= ~OV5645_AEC_MANUAL_ENABLE;
	else /* V4L2_EXPOSURE_MANUAL */
		val |= OV5645_AEC_MANUAL_ENABLE;

	ret = cci_write(ov5645->regmap, OV5645_AEC_PK_MANUAL, val, NULL);
	if (!ret)
		ov5645->aec_pk_manual = val;

	return ret;
}

static int ov5645_set_agc_mode(struct ov5645 *ov5645, u32 enable)
{
	u8 val = ov5645->aec_pk_manual;
	int ret;

	if (enable)
		val &= ~OV5645_AGC_MANUAL_ENABLE;
	else
		val |= OV5645_AGC_MANUAL_ENABLE;

	ret = cci_write(ov5645->regmap, OV5645_AEC_PK_MANUAL, val, NULL);
	if (!ret)
		ov5645->aec_pk_manual = val;

	return ret;
}

static int ov5645_set_register_array(struct ov5645 *ov5645,
				     const struct cci_reg_sequence *settings,
				     unsigned int num_settings)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_settings; ++i, ++settings) {
		ret = cci_write(ov5645->regmap, settings->reg, settings->val, NULL);
		if (ret < 0)
			return ret;

		if (settings->reg == OV5645_SYSTEM_CTRL0 &&
		    settings->val == OV5645_SYSTEM_CTRL0_START)
			usleep_range(1000, 2000);
	}

	return 0;
}

static void __ov5645_set_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov5645 *ov5645 = to_ov5645(sd);

	cci_write(ov5645->regmap, OV5645_IO_MIPI_CTRL00, 0x58, NULL);
	gpiod_set_value_cansleep(ov5645->rst_gpio, 1);
	gpiod_set_value_cansleep(ov5645->enable_gpio, 0);
	regulator_bulk_disable(OV5645_NUM_SUPPLIES, ov5645->supplies);
}

static int ov5645_set_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov5645 *ov5645 = to_ov5645(sd);

	__ov5645_set_power_off(dev);
	clk_disable_unprepare(ov5645->xclk);

	return 0;
}

static int ov5645_set_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov5645 *ov5645 = to_ov5645(sd);
	int ret;

	ret = regulator_bulk_enable(OV5645_NUM_SUPPLIES, ov5645->supplies);
	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(ov5645->xclk);
	if (ret < 0) {
		dev_err(ov5645->dev, "clk prepare enable failed\n");
		regulator_bulk_disable(OV5645_NUM_SUPPLIES, ov5645->supplies);
		return ret;
	}

	usleep_range(5000, 15000);
	gpiod_set_value_cansleep(ov5645->enable_gpio, 1);

	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ov5645->rst_gpio, 0);

	msleep(20);

	ret = ov5645_set_register_array(ov5645, ov5645_global_init_setting,
					ARRAY_SIZE(ov5645_global_init_setting));
	if (ret < 0) {
		dev_err(ov5645->dev, "could not set init registers\n");
		goto exit;
	}

	usleep_range(500, 1000);

	return 0;

exit:
	__ov5645_set_power_off(dev);
	clk_disable_unprepare(ov5645->xclk);
	return ret;
}

static int ov5645_set_saturation(struct ov5645 *ov5645, s32 value)
{
	u32 reg_value = (value * 0x10) + 0x40;
	int ret;

	ret = cci_write(ov5645->regmap, OV5645_SDE_SAT_U, reg_value, NULL);
	if (ret < 0)
		return ret;

	return cci_write(ov5645->regmap, OV5645_SDE_SAT_V, reg_value, NULL);
}

static int ov5645_set_hflip(struct ov5645 *ov5645, s32 value)
{
	u8 val = ov5645->timing_tc_reg21;
	int ret;

	if (value == 0)
		val &= ~(OV5645_SENSOR_MIRROR);
	else
		val |= (OV5645_SENSOR_MIRROR);

	ret = cci_write(ov5645->regmap, OV5645_TIMING_TC_REG21, val, NULL);
	if (!ret)
		ov5645->timing_tc_reg21 = val;

	return ret;
}

static int ov5645_set_vflip(struct ov5645 *ov5645, s32 value)
{
	u8 val = ov5645->timing_tc_reg20;
	int ret;

	if (value == 0)
		val |= (OV5645_SENSOR_VFLIP | OV5645_ISP_VFLIP);
	else
		val &= ~(OV5645_SENSOR_VFLIP | OV5645_ISP_VFLIP);

	ret = cci_write(ov5645->regmap, OV5645_TIMING_TC_REG20, val, NULL);
	if (!ret)
		ov5645->timing_tc_reg20 = val;

	return ret;
}

static int ov5645_set_test_pattern(struct ov5645 *ov5645, s32 value)
{
	u8 val = 0;

	if (value) {
		val = OV5645_SET_TEST_PATTERN(value - 1);
		val |= OV5645_TEST_PATTERN_ENABLE;
	}

	return cci_write(ov5645->regmap, OV5645_PRE_ISP_TEST_SETTING_1, val, NULL);
}

static const char * const ov5645_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bars",
	"Pseudo-Random Data",
	"Color Square",
	"Black Image",
};

static int ov5645_set_awb(struct ov5645 *ov5645, s32 enable_auto)
{
	u8 val = 0;

	if (!enable_auto)
		val = OV5645_AWB_MANUAL_ENABLE;

	return cci_write(ov5645->regmap, OV5645_AWB_MANUAL_CONTROL, val, NULL);
}

static int ov5645_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5645 *ov5645 = container_of(ctrl->handler,
					     struct ov5645, ctrls);
	int ret;

	if (!pm_runtime_get_if_in_use(ov5645->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_SATURATION:
		ret = ov5645_set_saturation(ov5645, ctrl->val);
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		ret = ov5645_set_awb(ov5645, ctrl->val);
		break;
	case V4L2_CID_AUTOGAIN:
		ret = ov5645_set_agc_mode(ov5645, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		ret = ov5645_set_aec_mode(ov5645, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov5645_set_test_pattern(ov5645, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		ret = ov5645_set_hflip(ov5645, ctrl->val);
		break;
	case V4L2_CID_VFLIP:
		ret = ov5645_set_vflip(ov5645, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(ov5645->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov5645_ctrl_ops = {
	.s_ctrl = ov5645_s_ctrl,
};

static int ov5645_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_mbus_frame_desc *fd)
{
	struct v4l2_subdev_state *state;
	u32 code;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	code = v4l2_subdev_state_get_format(state, OV5645_PAD_SOURCE, 0)->code;
	v4l2_subdev_unlock_state(state);

	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;

	memset(fd->entry, 0, sizeof(fd->entry));

	fd->entry[0].pixelcode = code;
	fd->entry[0].stream = 0;
	fd->entry[0].bus.csi2.vc = 0;
	fd->entry[0].bus.csi2.dt = MIPI_CSI2_DT_YUV422_8B;

	return 0;
}

static int ov5645_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_UYVY8_1X16;

	return 0;
}

static int ov5645_enum_frame_size(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->code != MEDIA_BUS_FMT_UYVY8_1X16)
		return -EINVAL;

	if (fse->index >= ARRAY_SIZE(ov5645_mode_info_data))
		return -EINVAL;

	fse->min_width = ov5645_mode_info_data[fse->index].width;
	fse->max_width = ov5645_mode_info_data[fse->index].width;
	fse->min_height = ov5645_mode_info_data[fse->index].height;
	fse->max_height = ov5645_mode_info_data[fse->index].height;

	return 0;
}

static int ov5645_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *format)
{
	struct ov5645 *ov5645 = to_ov5645(sd);
	struct v4l2_mbus_framefmt *__format;
	struct v4l2_rect *__crop;
	const struct ov5645_mode_info *new_mode;
	int ret;

	__crop = v4l2_subdev_state_get_crop(sd_state, 0);
	new_mode = v4l2_find_nearest_size(ov5645_mode_info_data,
					  ARRAY_SIZE(ov5645_mode_info_data),
					  width, height, format->format.width,
					  format->format.height);

	__crop->width = new_mode->width;
	__crop->height = new_mode->height;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ret = __v4l2_ctrl_s_ctrl_int64(ov5645->pixel_clock,
					       new_mode->pixel_clock);
		if (ret < 0)
			return ret;

		ret = __v4l2_ctrl_s_ctrl(ov5645->link_freq,
					 new_mode->link_freq);
		if (ret < 0)
			return ret;

		ov5645->current_mode = new_mode;
	}

	__format = v4l2_subdev_state_get_format(sd_state, 0);
	__format->width = __crop->width;
	__format->height = __crop->height;
	__format->code = MEDIA_BUS_FMT_UYVY8_1X16;
	__format->field = V4L2_FIELD_NONE;
	__format->colorspace = V4L2_COLORSPACE_SRGB;

	format->format = *__format;

	return 0;
}

static int ov5645_init_state(struct v4l2_subdev *subdev,
			     struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = OV5645_PAD_SOURCE,
		.format = {
			.code = MEDIA_BUS_FMT_UYVY8_1X16,
			.width = ov5645_mode_info_data[1].width,
			.height = ov5645_mode_info_data[1].height,
		},
	};

	ov5645_set_format(subdev, sd_state, &fmt);

	return 0;
}

static int ov5645_get_selection(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_selection *sel)
{
	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	sel->r = *v4l2_subdev_state_get_crop(sd_state, 0);
	return 0;
}

static int ov5645_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct ov5645 *ov5645 = to_ov5645(sd);
	int ret;

	ret = pm_runtime_resume_and_get(ov5645->dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(ov5645->regmap, ov5645->current_mode->data,
				  ov5645->current_mode->data_size, NULL);
	if (ret < 0) {
		dev_err(ov5645->dev, "could not set mode %dx%d\n",
			ov5645->current_mode->width,
			ov5645->current_mode->height);
		goto err_rpm_put;
	}
	ret = __v4l2_ctrl_handler_setup(&ov5645->ctrls);
	if (ret < 0) {
		dev_err(ov5645->dev, "could not sync v4l2 controls\n");
		goto err_rpm_put;
	}

	ret = cci_write(ov5645->regmap, OV5645_IO_MIPI_CTRL00, 0x45, NULL);
	if (ret < 0)
		goto err_rpm_put;

	ret = cci_write(ov5645->regmap, OV5645_SYSTEM_CTRL0,
			OV5645_SYSTEM_CTRL0_START, NULL);
	if (ret < 0)
		goto err_rpm_put;

	return 0;

err_rpm_put:
	pm_runtime_put_sync(ov5645->dev);
	return ret;
}

static int ov5645_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct ov5645 *ov5645 = to_ov5645(sd);
	int ret;

	ret = cci_write(ov5645->regmap, OV5645_IO_MIPI_CTRL00, 0x40, NULL);
	if (ret < 0)
		goto rpm_put;

	ret = cci_write(ov5645->regmap, OV5645_SYSTEM_CTRL0,
			OV5645_SYSTEM_CTRL0_STOP, NULL);

rpm_put:
	pm_runtime_put_autosuspend(ov5645->dev);

	return ret;
}

static const struct v4l2_subdev_video_ops ov5645_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops ov5645_subdev_pad_ops = {
	.get_frame_desc = ov5645_get_frame_desc,
	.enum_mbus_code = ov5645_enum_mbus_code,
	.enum_frame_size = ov5645_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ov5645_set_format,
	.get_selection = ov5645_get_selection,
	.enable_streams = ov5645_enable_streams,
	.disable_streams = ov5645_disable_streams,
};

static const struct v4l2_subdev_ops ov5645_subdev_ops = {
	.video = &ov5645_video_ops,
	.pad = &ov5645_subdev_pad_ops,
};

static const struct v4l2_subdev_internal_ops ov5645_internal_ops = {
	.init_state = ov5645_init_state,
};

static int ov5645_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	u64 chip_id_high, chip_id_low;
	struct device_node *endpoint;
	struct ov5645 *ov5645;
	unsigned int i;
	u32 xclk_freq;
	int ret;

	ov5645 = devm_kzalloc(dev, sizeof(struct ov5645), GFP_KERNEL);
	if (!ov5645)
		return -ENOMEM;

	ov5645->i2c_client = client;
	ov5645->dev = dev;

	endpoint = of_graph_get_endpoint_by_regs(dev->of_node, 0, -1);
	if (!endpoint)
		return dev_err_probe(dev, -EINVAL,
				     "endpoint node not found\n");

	ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(endpoint),
					 &ov5645->ep);

	of_node_put(endpoint);

	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "parsing endpoint node failed\n");

	if (ov5645->ep.bus_type != V4L2_MBUS_CSI2_DPHY)
		return dev_err_probe(dev, -EINVAL,
				     "invalid bus type, must be CSI2\n");

	ov5645->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ov5645->regmap))
		return dev_err_probe(ov5645->dev, PTR_ERR(ov5645->regmap),
				     "Failed to init CCI\n");

	/* get system clock (xclk) */
	ov5645->xclk = devm_v4l2_sensor_clk_get_legacy(dev, NULL, false, 0);
	if (IS_ERR(ov5645->xclk))
		return dev_err_probe(dev, PTR_ERR(ov5645->xclk),
				     "could not get xclk");

	/* external clock must be 24MHz, allow 1% tolerance */
	xclk_freq = clk_get_rate(ov5645->xclk);
	if (xclk_freq < 23760000 || xclk_freq > 24240000)
		return dev_err_probe(dev, -EINVAL,
				     "unsupported xclk frequency %u\n",
				     xclk_freq);

	for (i = 0; i < OV5645_NUM_SUPPLIES; i++)
		ov5645->supplies[i].supply = ov5645_supply_name[i];

	ret = devm_regulator_bulk_get(dev, OV5645_NUM_SUPPLIES,
				      ov5645->supplies);
	if (ret < 0)
		return ret;

	ov5645->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(ov5645->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ov5645->enable_gpio),
				     "cannot get enable gpio\n");

	ov5645->rst_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ov5645->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(ov5645->rst_gpio),
				     "cannot get reset gpio\n");

	v4l2_ctrl_handler_init(&ov5645->ctrls, 9);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_SATURATION, -4, 4, 1, 0);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_AUTOGAIN, 0, 1, 1, 1);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	v4l2_ctrl_new_std_menu(&ov5645->ctrls, &ov5645_ctrl_ops,
			       V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL,
			       0, V4L2_EXPOSURE_AUTO);
	v4l2_ctrl_new_std_menu_items(&ov5645->ctrls, &ov5645_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov5645_test_pattern_menu) - 1,
				     0, 0, ov5645_test_pattern_menu);
	ov5645->pixel_clock = v4l2_ctrl_new_std(&ov5645->ctrls,
						&ov5645_ctrl_ops,
						V4L2_CID_PIXEL_RATE,
						1, INT_MAX, 1, 1);
	ov5645->link_freq = v4l2_ctrl_new_int_menu(&ov5645->ctrls,
						   &ov5645_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(link_freq) - 1,
						   0, link_freq);
	if (ov5645->link_freq)
		ov5645->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ov5645->sd.ctrl_handler = &ov5645->ctrls;

	if (ov5645->ctrls.error) {
		ret = ov5645->ctrls.error;
		dev_err_probe(dev, ret, "failed to add controls\n");
		goto free_ctrl;
	}

	v4l2_i2c_subdev_init(&ov5645->sd, client, &ov5645_subdev_ops);
	ov5645->sd.internal_ops = &ov5645_internal_ops;
	ov5645->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov5645->pad.flags = MEDIA_PAD_FL_SOURCE;
	ov5645->sd.dev = dev;
	ov5645->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&ov5645->sd.entity, 1, &ov5645->pad);
	if (ret < 0) {
		dev_err_probe(dev, ret, "could not register media entity\n");
		goto free_ctrl;
	}

	ret = ov5645_set_power_on(dev);
	if (ret)
		goto free_entity;

	ret = cci_read(ov5645->regmap, OV5645_CHIP_ID_HIGH, &chip_id_high, NULL);
	if (ret < 0 || chip_id_high != OV5645_CHIP_ID_HIGH_BYTE) {
		ret = -ENODEV;
		dev_err_probe(dev, ret, "could not read ID high\n");
		goto power_down;
	}
	ret = cci_read(ov5645->regmap, OV5645_CHIP_ID_LOW, &chip_id_low, NULL);
	if (ret < 0 || chip_id_low != OV5645_CHIP_ID_LOW_BYTE) {
		ret = -ENODEV;
		dev_err_probe(dev, ret, "could not read ID low\n");
		goto power_down;
	}

	dev_info(dev, "OV5645 detected at address 0x%02x\n", client->addr);

	ret = cci_read(ov5645->regmap, OV5645_AEC_PK_MANUAL, &ov5645->aec_pk_manual, NULL);
	if (ret < 0) {
		ret = -ENODEV;
		dev_err_probe(dev, ret, "could not read AEC/AGC mode\n");
		goto power_down;
	}

	ret = cci_read(ov5645->regmap, OV5645_TIMING_TC_REG20, &ov5645->timing_tc_reg20, NULL);
	if (ret < 0) {
		ret = -ENODEV;
		dev_err_probe(dev, ret, "could not read vflip value\n");
		goto power_down;
	}

	ret = cci_read(ov5645->regmap, OV5645_TIMING_TC_REG21, &ov5645->timing_tc_reg21, NULL);
	if (ret < 0) {
		ret = -ENODEV;
		dev_err_probe(dev, ret, "could not read hflip value\n");
		goto power_down;
	}

	ov5645->sd.state_lock = ov5645->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&ov5645->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "subdev init error\n");
		goto power_down;
	}

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&ov5645->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "could not register v4l2 device\n");
		goto err_pm_runtime;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;

err_pm_runtime:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	v4l2_subdev_cleanup(&ov5645->sd);
power_down:
	ov5645_set_power_off(dev);
free_entity:
	media_entity_cleanup(&ov5645->sd.entity);
free_ctrl:
	v4l2_ctrl_handler_free(&ov5645->ctrls);

	return ret;
}

static void ov5645_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5645 *ov5645 = to_ov5645(sd);

	v4l2_async_unregister_subdev(&ov5645->sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&ov5645->sd.entity);
	v4l2_ctrl_handler_free(&ov5645->ctrls);
	pm_runtime_disable(ov5645->dev);
	if (!pm_runtime_status_suspended(ov5645->dev))
		ov5645_set_power_off(ov5645->dev);
	pm_runtime_set_suspended(ov5645->dev);
}

static const struct i2c_device_id ov5645_id[] = {
	{ "ov5645" },
	{}
};
MODULE_DEVICE_TABLE(i2c, ov5645_id);

static const struct of_device_id ov5645_of_match[] = {
	{ .compatible = "ovti,ov5645" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov5645_of_match);

static const struct dev_pm_ops ov5645_pm_ops = {
	SET_RUNTIME_PM_OPS(ov5645_set_power_off, ov5645_set_power_on, NULL)
};

static struct i2c_driver ov5645_i2c_driver = {
	.driver = {
		.of_match_table = ov5645_of_match,
		.name  = "ov5645",
		.pm = &ov5645_pm_ops,
	},
	.probe = ov5645_probe,
	.remove = ov5645_remove,
	.id_table = ov5645_id,
};

module_i2c_driver(ov5645_i2c_driver);

MODULE_DESCRIPTION("Omnivision OV5645 Camera Driver");
MODULE_AUTHOR("Todor Tomov <todor.tomov@linaro.org>");
MODULE_LICENSE("GPL v2");
