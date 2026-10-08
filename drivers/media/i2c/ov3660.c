// SPDX-License-Identifier: GPL-2.0
/*
 * OmniVision OV3660 3 megapixel image sensor driver, parallel (DVP) output
 *
 * Copyright (C) 2026 Nguyen Minh Tien <tien.nguyenminh@embeddedlinux.blog>
 *
 * Register descriptions are from the OV3660 datasheet (CSP3, version 1.3).
 * The datasheet lists the analog and sensor control blocks (0x3600-0x3634,
 * 0x3700-0x373c) and some VFIFO and DVP registers as reserved. Their values
 * below, and the colour tuning (AWB, colour matrix, lens correction), are
 * taken from Amlogic's GPL vendor driver for this sensor.
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/media-bus-format.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/time64.h>
#include <linux/types.h>
#include <linux/units.h>
#include <media/media-entity.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define OV3660_XCLK_MIN			(6 * HZ_PER_MHZ)
#define OV3660_XCLK_MAX			(27 * HZ_PER_MHZ)

#define OV3660_REG_SYS_RESET00		CCI_REG8(0x3000)
#define OV3660_REG_CLOCK_ENABLE00	CCI_REG8(0x3004)
#define OV3660_REG_SYS_CTRL0		CCI_REG8(0x3008)
#define OV3660_SYS_CTRL0_SW_RST		BIT(7)
#define OV3660_SYS_CTRL0_SW_PWDN	BIT(6)
#define OV3660_SYS_CTRL0_DEFAULT	0x02
#define OV3660_REG_CHIP_ID		CCI_REG16(0x300a)
#define OV3660_CHIP_ID			0x3660
#define OV3660_REG_PAD_OUTPUT_EN01	CCI_REG8(0x3017)
#define OV3660_REG_PAD_OUTPUT_EN02	CCI_REG8(0x3018)
#define OV3660_REG_PAD_CONTROL		CCI_REG8(0x302c)
#define OV3660_REG_PLLS_CTRL0		CCI_REG8(0x303a)
#define OV3660_REG_PLLS_CTRL1		CCI_REG8(0x303b)
#define OV3660_REG_PLLS_CTRL2		CCI_REG8(0x303c)
#define OV3660_REG_PLLS_CTRL3		CCI_REG8(0x303d)
#define OV3660_PLLS_CTRL3_PREDIV	GENMASK(5, 4)
#define OV3660_REG_SCCB_SYS_CTRL1	CCI_REG8(0x3103)
#define OV3660_REG_SYS_ROOT_DIVIDER	CCI_REG8(0x3108)
#define OV3660_REG_AWB_MANUAL		CCI_REG8(0x3406)
#define OV3660_AWB_MANUAL_EN		BIT(0)
#define OV3660_REG_EXPOSURE		CCI_REG24(0x3500) /* [19:0], 1/16 row */
#define OV3660_REG_AEC_PK_MANUAL	CCI_REG8(0x3503)
#define OV3660_AEC_MANUAL		BIT(0)
#define OV3660_AGC_MANUAL		BIT(1)
#define OV3660_REG_GAIN			CCI_REG16(0x350a) /* [9:0], 1/16 */
#define OV3660_REG_X_ADDR_ST		CCI_REG16(0x3800)
#define OV3660_REG_Y_ADDR_ST		CCI_REG16(0x3802)
#define OV3660_REG_X_ADDR_END		CCI_REG16(0x3804)
#define OV3660_REG_Y_ADDR_END		CCI_REG16(0x3806)
#define OV3660_REG_X_OUTPUT_SIZE	CCI_REG16(0x3808)
#define OV3660_REG_Y_OUTPUT_SIZE	CCI_REG16(0x380a)
#define OV3660_REG_HTS			CCI_REG16(0x380c)
#define OV3660_REG_VTS			CCI_REG16(0x380e)
#define OV3660_REG_X_OFFSET		CCI_REG16(0x3810)
#define OV3660_REG_Y_OFFSET		CCI_REG16(0x3812)
#define OV3660_REG_X_INC		CCI_REG8(0x3814)
#define OV3660_REG_Y_INC		CCI_REG8(0x3815)
#define OV3660_REG_TIMING_TC_REG20	CCI_REG8(0x3820)
#define OV3660_REG_TIMING_TC_REG21	CCI_REG8(0x3821)
#define OV3660_TIMING_TC_FLIP		(BIT(2) | BIT(1)) /* ISP and sensor */
#define OV3660_TIMING_TC_MIRROR		(BIT(2) | BIT(1)) /* ISP and sensor */
#define OV3660_TIMING_TC_BINNING	BIT(0)
#define OV3660_REG_PCLK_RATIO		CCI_REG8(0x3824)
#define OV3660_REG_AEC_CTRL00		CCI_REG8(0x3a00)
#define OV3660_AEC_CTRL00_BAND		BIT(5)
#define OV3660_REG_AEC_B50_STEP		CCI_REG16(0x3a08)
#define OV3660_REG_AEC_B60_STEP		CCI_REG16(0x3a0a)
#define OV3660_REG_AEC_CTRL0D		CCI_REG8(0x3a0d) /* max 60 Hz bands */
#define OV3660_REG_AEC_CTRL0E		CCI_REG8(0x3a0e) /* max 50 Hz bands */
#define OV3660_REG_5060HZ_CTRL00	CCI_REG8(0x3c00)
#define OV3660_5060HZ_CTRL00_BAND50	BIT(2)
#define OV3660_REG_5060HZ_CTRL01	CCI_REG8(0x3c01)
#define OV3660_REG_FORMAT_CTRL00	CCI_REG8(0x4300)
#define OV3660_REG_4514			CCI_REG8(0x4514) /* flip and binning */
#define OV3660_REG_4520			CCI_REG8(0x4520) /* binning */
#define OV3660_REG_VFIFO_CTRL0C		CCI_REG8(0x460c) /* [1] manual PCLK */
#define OV3660_REG_POLARITY_CTRL00	CCI_REG8(0x4740)
#define OV3660_REG_ISP_CTRL01		CCI_REG8(0x5001)
#define OV3660_ISP_CTRL01_SCALE		BIT(5)
#define OV3660_REG_FORMAT_MUX		CCI_REG8(0x501f)
#define OV3660_REG_PRE_ISP_TEST		CCI_REG8(0x503d)

/* The pixel array, and the active area within it (datasheet section 2.2) */
#define OV3660_NATIVE_WIDTH		2080
#define OV3660_NATIVE_HEIGHT		1548
#define OV3660_ACTIVE_LEFT		16
#define OV3660_ACTIVE_TOP		6
#define OV3660_ACTIVE_WIDTH		2048
#define OV3660_ACTIVE_HEIGHT		1536

#define OV3660_VTS_MAX			0xffff
#define OV3660_EXPOSURE_MARGIN		4

/*
 * Clocks. The PLL (datasheet section 2.9) divides XVCLK by 1, 1.5, 2 or 3
 * into REFIN, which may run at 4 to 13.5 MHz, and multiplies that by up to
 * 31 into 216 MHz, the most it may output. Everything below derives from
 * those 216 MHz, so an XVCLK that can't give them exactly (19.2 or 26 MHz,
 * for example) is refused. REFIN is then 216 MHz / multiplier: multipliers
 * of 16 to 31 keep it in range.
 *
 * 0x3108 divides the PLL output into SCLK, the sensor and ISP clock
 * ([1:0]), and SCLK2x ([3:2]). In manual mode the DVP PCLK is SCLK2x
 * divided by 0x3824: measured, the datasheet doesn't say which clock
 * 0x3824 divides. A divider that makes PCLK too slow for the line stops
 * the frames.
 *
 * 0x3108 comes up as 0x01 on this part, not the datasheet's 0x16, and
 * runs SCLK at 108 MHz: the test pattern comes out at that rate, but the
 * pixel array stops after one frame. 0x16 gives SCLK 54 MHz, the
 * datasheet's rate for 3 Mpixel at 15 fps. HTS counts SCLK cycles, so
 * SCLK is the pixel rate of the timing model.
 *
 * Full-width lines need PCLK = SCLK2x = 108 MHz, the binned ones fit in
 * 54 MHz. These are the link frequencies: a mode is offered only if the
 * firmware lists its pixel clock.
 */
#define OV3660_PLL_RATE			(216 * HZ_PER_MHZ)
#define OV3660_PLL_MULT_MIN		16
#define OV3660_PLL_MULT_MAX		31
#define OV3660_SYS_ROOT_DIV		0x16	/* SCLK /4, SCLK2x /2 */
#define OV3660_PIXEL_RATE		(OV3660_PLL_RATE / 4)	/* SCLK */
#define OV3660_SCLK2X			(OV3660_PLL_RATE / 2)

static const s64 ov3660_link_freqs[] = {
	108 * HZ_PER_MHZ,
	54 * HZ_PER_MHZ,
};

struct ov3660_mode {
	u32 width;
	u32 height;
	/* array window and ISP offsets, datasheet section 3.2 */
	u16 x_start;
	u16 y_start;
	u16 x_end;
	u16 y_end;
	u16 x_offset;
	u16 y_offset;
	u16 hts;
	u16 vts;		/* the minimum */
	bool binning;		/* 2x2: the window above is read at half size */
	bool scale;		/* the ISP scales to the output size */
	u8 link_freq_index;	/* DVP PCLK, in ov3660_link_freqs[] */
	struct v4l2_rect crop;	/* the part of the array the image shows */
};

/*
 * The full array runs at 15 fps, the 16:9 crop at 18.8 fps and the 2x2
 * binned array at 30 fps. The active area is 2048x1536 at (16, 6); binning
 * moves in steps of four rows, so the binned window starts two rows higher.
 */
static const struct ov3660_mode ov3660_modes[] = {
	{
		.width = 2048, .height = 1536,
		.x_start = 0, .y_start = 0, .x_end = 2079, .y_end = 1547,
		.x_offset = 16, .y_offset = 6,
		.hts = 2300, .vts = 1564,
		.link_freq_index = 0,
		.crop = { 16, 6, 2048, 1536 },
	}, {
		.width = 1920, .height = 1080,
		.x_start = 64, .y_start = 242, .x_end = 2015, .y_end = 1333,
		.x_offset = 16, .y_offset = 6,
		.hts = 2172, .vts = 1322,
		.link_freq_index = 0,
		.crop = { 80, 248, 1920, 1080 },
	}, {
		.width = 1280, .height = 720,
		.x_start = 64, .y_start = 242, .x_end = 2015, .y_end = 1333,
		.x_offset = 16, .y_offset = 6,
		.hts = 2172, .vts = 1322,
		.scale = true,
		.link_freq_index = 0,
		.crop = { 80, 248, 1920, 1080 },
	}, {
		.width = 1024, .height = 768,
		.x_start = 0, .y_start = 0, .x_end = 2079, .y_end = 1547,
		.x_offset = 8, .y_offset = 2,
		.hts = 2300, .vts = 783,
		.binning = true,
		.link_freq_index = 1,
		.crop = { 16, 4, 2048, 1536 },
	}, {
		.width = 640, .height = 480,
		.x_start = 0, .y_start = 0, .x_end = 2079, .y_end = 1547,
		.x_offset = 8, .y_offset = 2,
		.hts = 2300, .vts = 783,
		.binning = true, .scale = true,
		.link_freq_index = 1,
		.crop = { 16, 4, 2048, 1536 },
	}, {
		.width = 320, .height = 240,
		.x_start = 0, .y_start = 0, .x_end = 2079, .y_end = 1547,
		.x_offset = 8, .y_offset = 2,
		.hts = 2300, .vts = 783,
		.binning = true, .scale = true,
		.link_freq_index = 1,
		.crop = { 16, 4, 2048, 1536 },
	},
};

struct ov3660_format {
	u32 code;
	u8 format_ctrl00;	/* 0x4300 */
	u8 format_mux;		/* 0x501f */
};

static const struct ov3660_format ov3660_formats[] = {
	{ MEDIA_BUS_FMT_YUYV8_2X8, 0x30, 0x00 },
	{ MEDIA_BUS_FMT_UYVY8_2X8, 0x32, 0x00 },
	{ MEDIA_BUS_FMT_YVYU8_2X8, 0x31, 0x00 },
	{ MEDIA_BUS_FMT_VYUY8_2X8, 0x33, 0x00 },
	{ MEDIA_BUS_FMT_RGB565_2X8_LE, 0x6f, 0x01 },
	{ MEDIA_BUS_FMT_RGB565_2X8_BE, 0x61, 0x01 },
};

static const char * const ov3660_supply_names[] = {
	"dovdd",	/* digital I/O, 1.8 or 2.8 V */
	"avdd",		/* analog, 2.8 V */
	"dvdd",		/* digital core, 1.5 V, optional (internal LDO) */
};

static const char * const ov3660_test_pattern_menu[] = {
	"Disabled",
	"Color bars",
	"Color bars w/ rolling bar",
	"Gradual change vertical",
	"Gradual change horizontal",
	"Square",
	"Random data",
	"Black",
};

static const u8 ov3660_test_pattern_val[] = {
	0x00, 0x80, 0xc0, 0x84, 0x88, 0x82, 0x81, 0x83,
};

static_assert(ARRAY_SIZE(ov3660_test_pattern_val) ==
	      ARRAY_SIZE(ov3660_test_pattern_menu));

/*
 * Written once after power-up, after the clocks (ov3660_init_sensor()),
 * with the sensor in software standby.
 */
static const struct cci_reg_sequence ov3660_init_regs[] = {
	{ OV3660_REG_PAD_OUTPUT_EN01, 0x7f },	/* VSYNC, HREF, PCLK, D[9:6] */
	{ OV3660_REG_PAD_OUTPUT_EN02, 0xfc },	/* D[5:0] */
	{ OV3660_REG_PAD_CONTROL, 0x43 },	/* 2x drive */
	/* analog and sensor control (reserved) */
	{ CCI_REG8(0x3032), 0x00 }, { CCI_REG8(0x3614), 0x80 },
	{ CCI_REG8(0x3618), 0x00 }, { CCI_REG8(0x3619), 0x75 },
	{ CCI_REG8(0x3622), 0x80 }, { CCI_REG8(0x3623), 0x00 },
	{ CCI_REG8(0x3624), 0x03 }, { CCI_REG8(0x3630), 0x52 },
	{ CCI_REG8(0x3632), 0x07 }, { CCI_REG8(0x3633), 0xd2 },
	{ CCI_REG8(0x3704), 0x80 }, { CCI_REG8(0x3708), 0x66 },
	{ CCI_REG8(0x3709), 0x12 }, { CCI_REG8(0x370b), 0x12 },
	{ CCI_REG8(0x3717), 0x00 }, { CCI_REG8(0x371b), 0x60 },
	{ CCI_REG8(0x371c), 0x00 }, { CCI_REG8(0x3901), 0x13 },
	{ CCI_REG8(0x3600), 0x08 }, { CCI_REG8(0x3620), 0x43 },
	{ CCI_REG8(0x3702), 0x20 }, { CCI_REG8(0x3739), 0x48 },
	{ CCI_REG8(0x3730), 0x20 }, { CCI_REG8(0x370c), 0x0c },
	/* AEC gain ceiling 15.5x */
	{ CCI_REG16(0x3a18), 0x00f8 },
	/* hold the compression block in reset, gate its clock */
	{ OV3660_REG_SYS_RESET00, 0x10 }, { OV3660_REG_CLOCK_ENABLE00, 0xef },
	/* temperature sensor (reserved) */
	{ CCI_REG8(0x6700), 0x05 }, { CCI_REG8(0x6701), 0x19 },
	{ CCI_REG8(0x6702), 0xfd }, { CCI_REG8(0x6703), 0xd1 },
	{ CCI_REG8(0x6704), 0xff }, { CCI_REG8(0x6705), 0xff },
	/*
	 * Banding filter on, manual 50/60 Hz selection; the band steps follow
	 * the mode. Night mode off: the frame length stays as set.
	 */
	{ OV3660_REG_5060HZ_CTRL01, 0x80 },
	{ OV3660_REG_AEC_CTRL00, 0x3a },
	{ CCI_REG16(0x3a02), 0x0930 }, { CCI_REG16(0x3a14), 0x0930 },
	/* VFIFO and DVP (reserved) */
	{ CCI_REG8(0x440e), 0x08 }, { CCI_REG8(0x460b), 0x37 },
	{ CCI_REG8(0x4713), 0x02 }, { CCI_REG8(0x471c), 0xd0 },
	{ CCI_REG8(0x5086), 0x00 }, { CCI_REG8(0x5002), 0x00 },
	{ OV3660_REG_FORMAT_MUX, 0x00 },
	/* AWB */
	{ CCI_REG8(0x5180), 0xff }, { CCI_REG8(0x5181), 0xf2 },
	{ CCI_REG8(0x5182), 0x00 }, { CCI_REG8(0x5183), 0x14 },
	{ CCI_REG8(0x5184), 0x25 }, { CCI_REG8(0x5185), 0x24 },
	{ CCI_REG8(0x5186), 0x16 }, { CCI_REG8(0x5187), 0x16 },
	{ CCI_REG8(0x5188), 0x16 }, { CCI_REG8(0x5189), 0x68 },
	{ CCI_REG8(0x518a), 0x60 }, { CCI_REG8(0x518b), 0xe0 },
	{ CCI_REG8(0x518c), 0xb2 }, { CCI_REG8(0x518d), 0x42 },
	{ CCI_REG8(0x518e), 0x15 }, { CCI_REG8(0x518f), 0x86 },
	{ CCI_REG8(0x5190), 0x56 }, { CCI_REG8(0x5191), 0xf8 },
	{ CCI_REG8(0x5192), 0x04 }, { CCI_REG8(0x5193), 0x70 },
	{ CCI_REG8(0x5194), 0xf0 }, { CCI_REG8(0x5195), 0xf0 },
	{ CCI_REG8(0x5196), 0x03 }, { CCI_REG8(0x5197), 0x01 },
	{ CCI_REG8(0x5198), 0x04 }, { CCI_REG8(0x5199), 0x12 },
	{ CCI_REG8(0x519a), 0x04 }, { CCI_REG8(0x519b), 0x00 },
	{ CCI_REG8(0x519c), 0x06 }, { CCI_REG8(0x519d), 0x82 },
	{ CCI_REG8(0x519e), 0x38 },
	/* colour matrix */
	{ CCI_REG8(0x5381), 0x1c }, { CCI_REG8(0x5382), 0x5a },
	{ CCI_REG8(0x5383), 0x12 }, { CCI_REG8(0x5384), 0x07 },
	{ CCI_REG8(0x5385), 0x73 }, { CCI_REG8(0x5386), 0x7a },
	{ CCI_REG8(0x5387), 0x7a }, { CCI_REG8(0x5388), 0x5e },
	{ CCI_REG8(0x5389), 0x1c }, { CCI_REG8(0x538a), 0x01 },
	{ CCI_REG8(0x538b), 0x98 },
	/* ISP: LENC, gamma, black/white pixel cancel, interpolation */
	{ CCI_REG8(0x5000), 0xa7 },
	/* lens correction */
	{ CCI_REG8(0x5800), 0x0c }, { CCI_REG8(0x5801), 0x09 },
	{ CCI_REG8(0x5802), 0x0c }, { CCI_REG8(0x5803), 0x0c },
	{ CCI_REG8(0x5804), 0x0d }, { CCI_REG8(0x5805), 0x17 },
	{ CCI_REG8(0x5806), 0x06 }, { CCI_REG8(0x5807), 0x05 },
	{ CCI_REG8(0x5808), 0x04 }, { CCI_REG8(0x5809), 0x06 },
	{ CCI_REG8(0x580a), 0x09 }, { CCI_REG8(0x580b), 0x0e },
	{ CCI_REG8(0x580c), 0x05 }, { CCI_REG8(0x580d), 0x01 },
	{ CCI_REG8(0x580e), 0x00 }, { CCI_REG8(0x580f), 0x01 },
	{ CCI_REG8(0x5810), 0x05 }, { CCI_REG8(0x5811), 0x0d },
	{ CCI_REG8(0x5812), 0x05 }, { CCI_REG8(0x5813), 0x01 },
	{ CCI_REG8(0x5814), 0x00 }, { CCI_REG8(0x5815), 0x01 },
	{ CCI_REG8(0x5816), 0x05 }, { CCI_REG8(0x5817), 0x0d },
	{ CCI_REG8(0x5818), 0x08 }, { CCI_REG8(0x5819), 0x06 },
	{ CCI_REG8(0x581a), 0x05 }, { CCI_REG8(0x581b), 0x07 },
	{ CCI_REG8(0x581c), 0x0b }, { CCI_REG8(0x581d), 0x0d },
	{ CCI_REG8(0x581e), 0x12 }, { CCI_REG8(0x581f), 0x0d },
	{ CCI_REG8(0x5820), 0x0e }, { CCI_REG8(0x5821), 0x10 },
	{ CCI_REG8(0x5822), 0x10 }, { CCI_REG8(0x5823), 0x1e },
	{ CCI_REG8(0x5824), 0x53 }, { CCI_REG8(0x5825), 0x15 },
	{ CCI_REG8(0x5826), 0x05 }, { CCI_REG8(0x5827), 0x14 },
	{ CCI_REG8(0x5828), 0x54 }, { CCI_REG8(0x5829), 0x25 },
	{ CCI_REG8(0x582a), 0x33 }, { CCI_REG8(0x582b), 0x33 },
	{ CCI_REG8(0x582c), 0x34 }, { CCI_REG8(0x582d), 0x16 },
	{ CCI_REG8(0x582e), 0x24 }, { CCI_REG8(0x582f), 0x41 },
	{ CCI_REG8(0x5830), 0x50 }, { CCI_REG8(0x5831), 0x42 },
	{ CCI_REG8(0x5832), 0x15 }, { CCI_REG8(0x5833), 0x25 },
	{ CCI_REG8(0x5834), 0x34 }, { CCI_REG8(0x5835), 0x33 },
	{ CCI_REG8(0x5836), 0x24 }, { CCI_REG8(0x5837), 0x26 },
	{ CCI_REG8(0x5838), 0x54 }, { CCI_REG8(0x5839), 0x25 },
	{ CCI_REG8(0x583a), 0x15 }, { CCI_REG8(0x583b), 0x25 },
	{ CCI_REG8(0x583c), 0x53 }, { CCI_REG8(0x583d), 0xcf },
	/* AEC target window */
	{ CCI_REG8(0x3a0f), 0x30 }, { CCI_REG8(0x3a10), 0x28 },
	{ CCI_REG8(0x3a1b), 0x30 }, { CCI_REG8(0x3a1e), 0x28 },
	{ CCI_REG8(0x3a11), 0x60 }, { CCI_REG8(0x3a1f), 0x14 },
	/* sharpness and de-noise */
	{ CCI_REG8(0x5302), 0x28 }, { CCI_REG8(0x5303), 0x20 },
	{ CCI_REG8(0x5306), 0x1c }, { CCI_REG8(0x5307), 0x28 },
	/* black level calibration: auto, redone when the format changes */
	{ CCI_REG8(0x4002), 0xc5 }, { CCI_REG8(0x4003), 0x81 },
	{ CCI_REG8(0x4005), 0x12 },
	/* AEC average weights: all zones equal */
	{ CCI_REG32(0x5688), 0x11111111 }, { CCI_REG32(0x568c), 0x11111111 },
	/* special digital effects: saturation and contrast, no effect */
	{ CCI_REG8(0x5580), 0x06 }, { CCI_REG8(0x5588), 0x00 },
	{ CCI_REG8(0x5583), 0x40 }, { CCI_REG8(0x5584), 0x2c },
	/* SDE, colour matrix, AWB; scaling is per mode */
	{ OV3660_REG_ISP_CTRL01, 0x83 },
	/* manual DVP PCLK divider, set per mode */
	{ OV3660_REG_VFIFO_CTRL0C, 0x22 },
};

struct ov3660 {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;
	unsigned int bus_flags;
	struct clk *xclk;
	u8 pll_prediv;		/* 0x303d[5:4] */
	u8 pll_mult;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ov3660_supply_names)];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwdn_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct {
		struct v4l2_ctrl *auto_exp;
		struct v4l2_ctrl *exposure;
	};
	struct {
		struct v4l2_ctrl *auto_gain;
		struct v4l2_ctrl *gain;
	};
	struct {
		struct v4l2_ctrl *hflip;
		struct v4l2_ctrl *vflip;
	};
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *link_freq;

	/* The link frequencies the firmware lists, in ov3660_link_freqs[] */
	unsigned long link_freq_bitmap;
};

static inline struct ov3660 *to_ov3660(struct v4l2_subdev *sd)
{
	return container_of_const(sd, struct ov3660, sd);
}

static const struct ov3660_format *ov3660_find_format(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ov3660_formats); i++)
		if (ov3660_formats[i].code == code)
			return &ov3660_formats[i];

	return NULL;
}

/* A mode is offered only if the firmware lists its pixel clock */
static bool ov3660_mode_allowed(const void *array, size_t index,
				const void *context)
{
	const struct ov3660_mode *mode = array;
	const struct ov3660 *sensor = context;

	return sensor->link_freq_bitmap & BIT(mode->link_freq_index);
}

static const struct ov3660_mode *
ov3660_find_mode(const struct ov3660 *sensor, u32 width, u32 height)
{
	return v4l2_find_nearest_size_conditional(ov3660_modes,
						  ARRAY_SIZE(ov3660_modes),
						  width, height, width, height,
						  ov3660_mode_allowed, sensor);
}

/* VGA, or the nearest mode the link frequencies allow */
static const struct ov3660_mode *
ov3660_default_mode(const struct ov3660 *sensor)
{
	return ov3660_find_mode(sensor, 640, 480);
}

/* The mode of the active state; its lock is the control handler lock. */
static const struct ov3660_mode *ov3660_active_mode(struct ov3660 *sensor)
{
	struct v4l2_subdev_state *state =
		v4l2_subdev_get_locked_active_state(&sensor->sd);
	const struct v4l2_mbus_framefmt *fmt =
		v4l2_subdev_state_get_format(state, 0);

	return ov3660_find_mode(sensor, fmt->width, fmt->height);
}

/*
 * Mirror, flip and binning share registers, so they are set together.
 * The sensor is back side illuminated: read out as is, the image is
 * mirrored, so the mirror bits are set for an unmirrored picture.
 */
static int ov3660_set_image_options(struct ov3660 *sensor,
				    const struct ov3660_mode *mode)
{
	bool vflip = sensor->vflip->val;
	bool mirror = !sensor->hflip->val;
	u8 reg20, reg21, reg4514;
	int ret = 0;

	/* REG20 bit 6 is reserved and set at reset; binning clears it */
	reg20 = mode->binning ? OV3660_TIMING_TC_BINNING : BIT(6);
	reg21 = mode->binning ? OV3660_TIMING_TC_BINNING : 0;
	if (vflip)
		reg20 |= OV3660_TIMING_TC_FLIP;
	if (mirror)
		reg21 |= OV3660_TIMING_TC_MIRROR;

	/*
	 * 0x4514 follows the readout direction. The notes of datasheet table
	 * 3-1 give 0x88 for a full-size flip and 0xbb for a full-size mirror
	 * and flip and a binned mirror; the other cases were checked on the
	 * colours of the image.
	 */
	if (!mode->binning)
		reg4514 = mirror ? 0xbb : 0x88;
	else
		reg4514 = (mirror != vflip) ? 0xbb : 0xaa;

	cci_write(sensor->regmap, OV3660_REG_TIMING_TC_REG20, reg20, &ret);
	cci_write(sensor->regmap, OV3660_REG_TIMING_TC_REG21, reg21, &ret);
	cci_write(sensor->regmap, OV3660_REG_4514, reg4514, &ret);

	return ret;
}

/*
 * The frame length and what depends on it: the banding filter steps (rows
 * per half mains period) and the number of bands that fit in a frame.
 */
static int ov3660_set_vts(struct ov3660 *sensor,
			  const struct ov3660_mode *mode, u32 vts)
{
	u32 b50 = OV3660_PIXEL_RATE / (mode->hts * 100);
	u32 b60 = OV3660_PIXEL_RATE / (mode->hts * 120);
	int ret = 0;

	cci_write(sensor->regmap, OV3660_REG_VTS, vts, &ret);
	cci_write(sensor->regmap, OV3660_REG_AEC_B50_STEP, b50, &ret);
	cci_write(sensor->regmap, OV3660_REG_AEC_B60_STEP, b60, &ret);
	cci_write(sensor->regmap, OV3660_REG_AEC_CTRL0E,
		  min((vts - OV3660_EXPOSURE_MARGIN) / b50, 0x3fU), &ret);
	cci_write(sensor->regmap, OV3660_REG_AEC_CTRL0D,
		  min((vts - OV3660_EXPOSURE_MARGIN) / b60, 0x3fU), &ret);

	return ret;
}

static int ov3660_set_mode(struct ov3660 *sensor,
			   const struct ov3660_mode *mode, u32 code)
{
	const struct ov3660_format *format = ov3660_find_format(code);
	struct regmap *map = sensor->regmap;
	u8 pol = 0;
	int ret = 0;

	cci_write(map, OV3660_REG_PCLK_RATIO,
		  OV3660_SCLK2X / (u32)ov3660_link_freqs[mode->link_freq_index],
		  &ret);

	cci_write(map, OV3660_REG_X_ADDR_ST, mode->x_start, &ret);
	cci_write(map, OV3660_REG_Y_ADDR_ST, mode->y_start, &ret);
	cci_write(map, OV3660_REG_X_ADDR_END, mode->x_end, &ret);
	cci_write(map, OV3660_REG_Y_ADDR_END, mode->y_end, &ret);
	cci_write(map, OV3660_REG_X_OUTPUT_SIZE, mode->width, &ret);
	cci_write(map, OV3660_REG_Y_OUTPUT_SIZE, mode->height, &ret);
	cci_write(map, OV3660_REG_HTS, mode->hts, &ret);
	cci_write(map, OV3660_REG_X_OFFSET, mode->x_offset, &ret);
	cci_write(map, OV3660_REG_Y_OFFSET, mode->y_offset, &ret);

	/*
	 * Binning: the subsampling increments and a reserved register that
	 * also sets how the full array is read out (other values shift or
	 * split the lines). With the full array, the green and the red/blue
	 * pixels end up with slightly different black levels: dark areas get
	 * a magenta cast at high gain, green when flipped.
	 */
	cci_write(map, OV3660_REG_X_INC, mode->binning ? 0x31 : 0x11, &ret);
	cci_write(map, OV3660_REG_Y_INC, mode->binning ? 0x31 : 0x11, &ret);
	cci_write(map, OV3660_REG_4520, mode->binning ? 0x0b : 0xb0, &ret);
	cci_update_bits(map, OV3660_REG_ISP_CTRL01, OV3660_ISP_CTRL01_SCALE,
			mode->scale ? OV3660_ISP_CTRL01_SCALE : 0, &ret);

	cci_write(map, OV3660_REG_FORMAT_CTRL00, format->format_ctrl00, &ret);
	cci_write(map, OV3660_REG_FORMAT_MUX, format->format_mux, &ret);

	/*
	 * Bus polarity. As on the OV5640, bit 0 is set for an active low
	 * VSYNC, the opposite of what the datasheet says.
	 */
	if (sensor->bus_flags & V4L2_MBUS_PCLK_SAMPLE_RISING)
		pol |= BIT(5);
	if (sensor->bus_flags & V4L2_MBUS_HSYNC_ACTIVE_HIGH)
		pol |= BIT(1);
	if (sensor->bus_flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		pol |= BIT(0);
	cci_write(map, OV3660_REG_POLARITY_CTRL00, pol, &ret);

	return ret;
}

/* Software standby, the clocks, then the rest of the setup */
static int ov3660_init_sensor(struct ov3660 *sensor)
{
	struct regmap *map = sensor->regmap;
	int ret = 0;

	cci_write(map, OV3660_REG_SYS_CTRL0,
		  OV3660_SYS_CTRL0_DEFAULT | OV3660_SYS_CTRL0_SW_PWDN, &ret);
	/* PLL input: the pre-divider */
	cci_write(map, OV3660_REG_SCCB_SYS_CTRL1, 0x13, &ret);
	cci_write(map, OV3660_REG_SYS_ROOT_DIVIDER, OV3660_SYS_ROOT_DIV, &ret);
	/* PLL: XVCLK / pre-divider x multiplier, no other divider, no bypass */
	cci_write(map, OV3660_REG_PLLS_CTRL0, 0x00, &ret);
	cci_write(map, OV3660_REG_PLLS_CTRL1, sensor->pll_mult, &ret);
	cci_write(map, OV3660_REG_PLLS_CTRL2, 0x11, &ret);
	cci_write(map, OV3660_REG_PLLS_CTRL3,
		  FIELD_PREP(OV3660_PLLS_CTRL3_PREDIV, sensor->pll_prediv),
		  &ret);
	cci_multi_reg_write(map, ov3660_init_regs, ARRAY_SIZE(ov3660_init_regs),
			    &ret);

	return ret;
}

static int ov3660_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov3660 *sensor = to_ov3660(sd);
	unsigned int i;
	int ret;

	/* Datasheet section 2.4: DOVDD, then AVDD, then DVDD (array order) */
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		ret = regulator_enable(sensor->supplies[i].consumer);
		if (ret)
			goto err_regulators;
	}

	ret = clk_prepare_enable(sensor->xclk);
	if (ret)
		goto err_regulators;

	/*
	 * Datasheet section 2.4: PWDN low 5 ms after the supplies are
	 * stable, RESETB high 1 ms later, then 20 ms to the first SCCB access.
	 * Both are asserted while the sensor is off.
	 */
	fsleep(5 * USEC_PER_MSEC);
	gpiod_set_value_cansleep(sensor->pwdn_gpio, 0);
	fsleep(1 * USEC_PER_MSEC);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	fsleep(20 * USEC_PER_MSEC);

	/* About 2 ms to settle after a software reset (section 2.5) */
	ret = cci_write(sensor->regmap, OV3660_REG_SYS_CTRL0,
			OV3660_SYS_CTRL0_SW_RST, NULL);
	if (ret)
		goto err_clk;
	fsleep(3 * USEC_PER_MSEC);

	ret = ov3660_init_sensor(sensor);
	if (ret)
		goto err_clk;

	return 0;

err_clk:
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);
	clk_disable_unprepare(sensor->xclk);
err_regulators:
	while (i--)
		regulator_disable(sensor->supplies[i].consumer);
	return ret;
}

static int ov3660_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov3660 *sensor = to_ov3660(sd);

	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);
	clk_disable_unprepare(sensor->xclk);
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);

	return 0;
}

/* ---- controls ---- */

static int ov3660_update_exposure_range(struct ov3660 *sensor, u32 vts)
{
	s64 max = vts - OV3660_EXPOSURE_MARGIN;

	return __v4l2_ctrl_modify_range(sensor->exposure,
					sensor->exposure->minimum, max,
					sensor->exposure->step,
					min(sensor->exposure->default_value,
					    max));
}

/* The banding filter, and the 50 or 60 Hz band in manual mode */
static int ov3660_set_power_line(struct ov3660 *sensor, s32 val)
{
	bool band50 = val == V4L2_CID_POWER_LINE_FREQUENCY_50HZ;
	int ret = 0;

	cci_update_bits(sensor->regmap, OV3660_REG_AEC_CTRL00,
			OV3660_AEC_CTRL00_BAND,
			val ? OV3660_AEC_CTRL00_BAND : 0, &ret);
	cci_update_bits(sensor->regmap, OV3660_REG_5060HZ_CTRL00,
			OV3660_5060HZ_CTRL00_BAND50,
			band50 ? OV3660_5060HZ_CTRL00_BAND50 : 0, &ret);

	return ret;
}

static int ov3660_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov3660 *sensor =
		container_of_const(ctrl->handler, struct ov3660, ctrls);
	u64 val;
	int ret;

	/* the AEC/AGC results are only there while the sensor is powered */
	if (!pm_runtime_get_if_active(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_AUTO:
		ret = cci_read(sensor->regmap, OV3660_REG_EXPOSURE, &val, NULL);
		if (!ret)
			sensor->exposure->val = (val & 0xfffff) >> 4;
		break;
	case V4L2_CID_AUTOGAIN:
		ret = cci_read(sensor->regmap, OV3660_REG_GAIN, &val, NULL);
		if (!ret)
			sensor->gain->val = val & 0x3ff;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(sensor->dev);

	return ret;
}

static int ov3660_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov3660 *sensor =
		container_of_const(ctrl->handler, struct ov3660, ctrls);
	const struct ov3660_mode *mode = ov3660_active_mode(sensor);
	struct regmap *map = sensor->regmap;
	bool manual;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		ret = ov3660_update_exposure_range(sensor,
						   mode->height + ctrl->val);
		if (ret)
			return ret;
	}

	/* written when the stream starts */
	if (!pm_runtime_get_if_active(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_AUTO:
		manual = ctrl->val == V4L2_EXPOSURE_MANUAL;
		cci_update_bits(map, OV3660_REG_AEC_PK_MANUAL,
				OV3660_AEC_MANUAL,
				manual ? OV3660_AEC_MANUAL : 0, &ret);
		if (manual && sensor->exposure->is_new)
			cci_write(map, OV3660_REG_EXPOSURE,
				  sensor->exposure->val << 4, &ret);
		break;
	case V4L2_CID_AUTOGAIN:
		manual = !ctrl->val;
		cci_update_bits(map, OV3660_REG_AEC_PK_MANUAL,
				OV3660_AGC_MANUAL,
				manual ? OV3660_AGC_MANUAL : 0, &ret);
		if (manual && sensor->gain->is_new)
			cci_write(map, OV3660_REG_GAIN, sensor->gain->val,
				  &ret);
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		cci_write(map, OV3660_REG_AWB_MANUAL,
			  ctrl->val ? 0 : OV3660_AWB_MANUAL_EN, &ret);
		break;
	case V4L2_CID_HFLIP:
		ret = ov3660_set_image_options(sensor, mode);
		break;
	case V4L2_CID_VBLANK:
		ret = ov3660_set_vts(sensor, mode, mode->height + ctrl->val);
		break;
	case V4L2_CID_POWER_LINE_FREQUENCY:
		ret = ov3660_set_power_line(sensor, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(map, OV3660_REG_PRE_ISP_TEST,
			  ov3660_test_pattern_val[ctrl->val], &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(sensor->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov3660_ctrl_ops = {
	.g_volatile_ctrl = ov3660_g_volatile_ctrl,
	.s_ctrl = ov3660_s_ctrl,
};

static int ov3660_init_controls(struct ov3660 *sensor)
{
	const struct ov3660_mode *mode = ov3660_default_mode(sensor);
	const struct v4l2_ctrl_ops *ops = &ov3660_ctrl_ops;
	struct v4l2_ctrl_handler *hdl = &sensor->ctrls;
	struct v4l2_fwnode_device_properties props;
	u32 exposure_max = mode->vts - OV3660_EXPOSURE_MARGIN;
	u32 hblank = mode->hts - mode->width;
	u32 vblank = mode->vts - mode->height;
	int ret;

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(hdl, 15);

	sensor->auto_exp = v4l2_ctrl_new_std_menu(hdl, ops,
						  V4L2_CID_EXPOSURE_AUTO,
						  V4L2_EXPOSURE_MANUAL, 0,
						  V4L2_EXPOSURE_AUTO);
	/* in rows */
	sensor->exposure = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,
					     1, exposure_max, 1, 0x200);
	sensor->auto_gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_AUTOGAIN,
					      0, 1, 1, 1);
	/* in 1/16 steps, 1x to 63.9x */
	sensor->gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_GAIN,
					 16, 1023, 1, 16);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	sensor->hflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	sensor->vflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_POWER_LINE_FREQUENCY,
			       V4L2_CID_POWER_LINE_FREQUENCY_60HZ, 0,
			       V4L2_CID_POWER_LINE_FREQUENCY_50HZ);
	v4l2_ctrl_new_std_menu_items(hdl, ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov3660_test_pattern_menu) - 1,
				     0, 0, ov3660_test_pattern_menu);
	v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE, OV3660_PIXEL_RATE,
			  OV3660_PIXEL_RATE, 1, OV3660_PIXEL_RATE);
	sensor->hblank = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK,
					   hblank, hblank, 1, hblank);
	sensor->vblank = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VBLANK, vblank,
					   OV3660_VTS_MAX - mode->height, 1,
					   vblank);
	sensor->link_freq =
		v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(ov3660_link_freqs) - 1,
				       mode->link_freq_index,
				       ov3660_link_freqs);
	v4l2_ctrl_new_fwnode_properties(hdl, ops, &props);

	if (hdl->error)
		return v4l2_ctrl_handler_free(hdl);

	/* Only the link frequencies the firmware allows */
	ret = v4l2_ctrl_modify_range(sensor->link_freq, 0,
				     ARRAY_SIZE(ov3660_link_freqs) - 1,
				     ~sensor->link_freq_bitmap,
				     mode->link_freq_index);
	if (ret)
		goto err;

	sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	sensor->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_auto_cluster(2, &sensor->auto_exp, V4L2_EXPOSURE_MANUAL,
			       true);
	v4l2_ctrl_auto_cluster(2, &sensor->auto_gain, 0, true);
	/* both flips are written together */
	v4l2_ctrl_cluster(2, &sensor->hflip);

	sensor->sd.ctrl_handler = hdl;

	return 0;

err:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

/* ---- pad and video ops ---- */

static void ov3660_fill_fmt(const struct ov3660_mode *mode, u32 code,
			    struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = code;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
}

static int ov3660_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	const struct ov3660_mode *mode = ov3660_default_mode(to_ov3660(sd));

	ov3660_fill_fmt(mode, ov3660_formats[0].code,
			v4l2_subdev_state_get_format(state, 0));
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	return 0;
}

static int ov3660_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(ov3660_formats))
		return -EINVAL;

	code->code = ov3660_formats[code->index].code;

	return 0;
}

static int ov3660_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct ov3660 *sensor = to_ov3660(sd);
	unsigned int n = 0;

	if (!ov3660_find_format(fse->code))
		return -EINVAL;

	for (unsigned int i = 0; i < ARRAY_SIZE(ov3660_modes); i++) {
		if (!ov3660_mode_allowed(&ov3660_modes[i], i, sensor) ||
		    n++ != fse->index)
			continue;

		fse->min_width = ov3660_modes[i].width;
		fse->max_width = fse->min_width;
		fse->min_height = ov3660_modes[i].height;
		fse->max_height = fse->min_height;
		return 0;
	}

	return -EINVAL;
}

static int ov3660_set_fmt(struct v4l2_subdev *sd,
			  const struct v4l2_subdev_client_info *ci,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct ov3660 *sensor = to_ov3660(sd);
	const struct ov3660_mode *mode;
	u32 code = format->format.code;
	u32 hblank, vblank;
	int ret;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	if (!ov3660_find_format(code))
		code = ov3660_formats[0].code;
	mode = ov3660_find_mode(sensor, format->format.width,
				format->format.height);
	ov3660_fill_fmt(mode, code, &format->format);
	*v4l2_subdev_state_get_format(state, 0) = format->format;
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	hblank = mode->hts - mode->width;
	vblank = mode->vts - mode->height;
	ret = __v4l2_ctrl_modify_range(sensor->hblank, hblank, hblank, 1,
				       hblank);
	if (!ret)
		ret = __v4l2_ctrl_modify_range(sensor->vblank, vblank,
					       OV3660_VTS_MAX - mode->height,
					       1, vblank);
	if (!ret)
		ret = __v4l2_ctrl_s_ctrl(sensor->vblank, vblank);
	if (!ret)
		ret = ov3660_update_exposure_range(sensor, mode->vts);
	if (!ret)
		ret = __v4l2_ctrl_s_ctrl(sensor->link_freq,
					 mode->link_freq_index);

	return ret;
}

static int ov3660_get_selection(struct v4l2_subdev *sd,
				const struct v4l2_subdev_client_info *ci,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = OV3660_NATIVE_WIDTH;
		sel->r.height = OV3660_NATIVE_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = OV3660_ACTIVE_LEFT;
		sel->r.top = OV3660_ACTIVE_TOP;
		sel->r.width = OV3660_ACTIVE_WIDTH;
		sel->r.height = OV3660_ACTIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ov3660_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct ov3660 *sensor = to_ov3660(sd);
	const struct v4l2_mbus_framefmt *fmt =
		v4l2_subdev_state_get_format(state, 0);
	int ret;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret)
		return ret;

	ret = ov3660_set_mode(sensor,
			      ov3660_find_mode(sensor, fmt->width, fmt->height),
			      fmt->code);
	if (!ret)
		ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	if (!ret)
		ret = cci_write(sensor->regmap, OV3660_REG_SYS_CTRL0,
				OV3660_SYS_CTRL0_DEFAULT, NULL);
	if (ret)
		pm_runtime_put_autosuspend(sensor->dev);

	return ret;
}

static int ov3660_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct ov3660 *sensor = to_ov3660(sd);
	int ret;

	ret = cci_write(sensor->regmap, OV3660_REG_SYS_CTRL0,
			OV3660_SYS_CTRL0_DEFAULT | OV3660_SYS_CTRL0_SW_PWDN,
			NULL);
	pm_runtime_put_autosuspend(sensor->dev);

	return ret;
}

static const struct v4l2_subdev_video_ops ov3660_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops ov3660_pad_ops = {
	.enum_mbus_code = ov3660_enum_mbus_code,
	.enum_frame_size = ov3660_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ov3660_set_fmt,
	.get_selection = ov3660_get_selection,
	.enable_streams = ov3660_enable_streams,
	.disable_streams = ov3660_disable_streams,
};

static const struct v4l2_subdev_ops ov3660_subdev_ops = {
	.video = &ov3660_video_ops,
	.pad = &ov3660_pad_ops,
};

static const struct v4l2_subdev_internal_ops ov3660_internal_ops = {
	.init_state = ov3660_init_state,
};

/* ---- probe ---- */

static int ov3660_parse_endpoint(struct ov3660 *sensor)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_PARALLEL };
	struct fwnode_handle *fwnode;
	int ret;

	fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(sensor->dev), 0, 0,
						 FWNODE_GRAPH_ENDPOINT_NEXT);
	ret = v4l2_fwnode_endpoint_alloc_parse(fwnode, &ep);
	fwnode_handle_put(fwnode);
	if (ret)
		return dev_err_probe(sensor->dev, ret,
				     "a parallel endpoint is needed\n");

	if (ep.bus.parallel.bus_width != 8) {
		ret = dev_err_probe(sensor->dev, -EINVAL,
				    "only an 8-bit bus is supported\n");
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(sensor->dev, ep.link_frequencies,
				       ep.nr_of_link_frequencies,
				       ov3660_link_freqs,
				       ARRAY_SIZE(ov3660_link_freqs),
				       &sensor->link_freq_bitmap);
	if (ret)
		goto out;

	sensor->bus_flags = ep.bus.parallel.flags;

out:
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

/*
 * The PLL pre-divider and multiplier for 216 MHz. The largest pre-divider
 * comes first: for a 24 MHz XVCLK that is the datasheet's default, / 3 x 27.
 */
static int ov3660_calc_pll(struct ov3660 *sensor)
{
	/* 1, 1.5, 2 and 3, doubled; the index is the 0x303d[5:4] value */
	static const u8 prediv_x2[] = { 2, 3, 4, 6 };
	unsigned long xclk = clk_get_rate(sensor->xclk);
	int i;

	if (xclk < OV3660_XCLK_MIN || xclk > OV3660_XCLK_MAX)
		return dev_err_probe(sensor->dev, -EINVAL,
				     "xclk rate %lu Hz out of range\n", xclk);

	for (i = ARRAY_SIZE(prediv_x2) - 1; i >= 0; i--) {
		/* multiplier = 216 MHz / (XVCLK / pre-divider) */
		unsigned long num = OV3660_PLL_RATE * prediv_x2[i];
		unsigned long den = 2 * xclk;
		unsigned long mult = num / den;

		if (num % den || mult < OV3660_PLL_MULT_MIN ||
		    mult > OV3660_PLL_MULT_MAX)
			continue;

		sensor->pll_prediv = i;
		sensor->pll_mult = mult;
		return 0;
	}

	return dev_err_probe(sensor->dev, -EINVAL,
			     "no 216 MHz PLL setting for a %lu Hz xclk\n",
			     xclk);
}

static int ov3660_check_chip_id(struct ov3660 *sensor)
{
	u64 id;
	int ret;

	ret = cci_read(sensor->regmap, OV3660_REG_CHIP_ID, &id, NULL);
	if (ret)
		return dev_err_probe(sensor->dev, ret, "cannot read chip id\n");

	if (id != OV3660_CHIP_ID)
		return dev_err_probe(sensor->dev, -ENODEV,
				     "chip id 0x%04llx, expected 0x%04x\n",
				     id, OV3660_CHIP_ID);

	return 0;
}

static int ov3660_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov3660 *sensor;
	unsigned int i;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = dev;

	ret = ov3660_parse_endpoint(sensor);
	if (ret)
		return ret;

	sensor->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "cannot get xclk\n");
	ret = ov3660_calc_pll(sensor);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(ov3660_supply_names); i++)
		sensor->supplies[i].supply = ov3660_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get supplies\n");

	sensor->pwdn_gpio = devm_gpiod_get_optional(dev, "powerdown",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->pwdn_gpio),
				     "cannot get powerdown gpio\n");
	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "cannot get reset gpio\n");

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "cannot init regmap\n");

	v4l2_i2c_subdev_init(&sensor->sd, client, &ov3660_subdev_ops);
	sensor->sd.internal_ops = &ov3660_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		return ret;

	ret = ov3660_power_on(dev);
	if (ret)
		goto err_entity;

	ret = ov3660_check_chip_id(sensor);
	if (ret)
		goto err_power;

	ret = ov3660_init_controls(sensor);
	if (ret)
		goto err_power;

	sensor->sd.state_lock = sensor->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto err_ctrls;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_pm;

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&sensor->sd);
err_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
err_power:
	ov3660_power_off(dev);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

static void ov3660_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov3660 *sensor = to_ov3660(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);

	pm_runtime_dont_use_autosuspend(sensor->dev);
	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev)) {
		ov3660_power_off(sensor->dev);
		pm_runtime_set_suspended(sensor->dev);
	}
}

static DEFINE_RUNTIME_DEV_PM_OPS(ov3660_pm_ops,
				 ov3660_power_off, ov3660_power_on, NULL);

static const struct of_device_id ov3660_of_match[] = {
	{ .compatible = "ovti,ov3660" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov3660_of_match);

static struct i2c_driver ov3660_i2c_driver = {
	.driver = {
		.name = "ov3660",
		.of_match_table = ov3660_of_match,
		.pm = pm_ptr(&ov3660_pm_ops),
	},
	.probe = ov3660_probe,
	.remove = ov3660_remove,
};
module_i2c_driver(ov3660_i2c_driver);

MODULE_DESCRIPTION("OmniVision OV3660 camera sensor driver");
MODULE_AUTHOR("Nguyen Minh Tien <tien.nguyenminh@embeddedlinux.blog>");
MODULE_LICENSE("GPL");
