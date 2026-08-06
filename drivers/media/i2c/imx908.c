// SPDX-License-Identifier: GPL-2.0
/*
 * V4L2 driver for Sony IMX908
 *
 * Diagonal 6.42 mm (Type 1/2.8) CMOS image sensor with 8.39 M pixels.
 *
 * Copyright 2026 Sony Semiconductor Solutions Corporation
 *
 */

#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/math.h>		/* DIV_ROUND_UP */
#include <linux/math64.h>	/* DIV64_U64_ROUND_UP */
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-rect.h>
#include <media/v4l2-subdev.h>

/* ---- IMX908 Registers --------- */
#define IMX908_REG_STANDBY	CCI_REG8(0x3000)
#define IMX908_STANDBY_EN	BIT(0)		/* Standby mode */
#define IMX908_STANDBY_CANCEL	0x00		/* Operating mode */
#define IMX908_REG_XMSTA	CCI_REG8(0x3002)	/* [0] Init 1h */
#define IMX908_START_CONTROLLER	0x00	/* Controller mode operation start */
#define IMX908_STOP_CONTROLLER	0x01	/* Controller mode operation stop */
#define IMX908_REG_XMASTER	CCI_REG8(0x3003)	/* [0] Init 0h */
#define IMX908_CONTROLLER_MODE	0x00
#define IMX908_REG_INCK_SEL	CCI_REG8(0x3014)	/* 0:74.25 1:37.125 2:72 3:27 4:24 */
#define IMX908_REG_DATARATE_SEL	CCI_REG8(0x3015)

/* ---- Crop */
#define IMX908_REG_WINMODE	CCI_REG8(0x3018)	/* [3:0] */
#define IMX908_WINMODE_ALLPIX	0x00	/* All-pixel readout mode */
#define IMX908_WINMODE_CROP	0x04	/* Window cropping mode */

/* ---- Flip */
#define IMX908_REG_HREVERSE	CCI_REG8(0x3020)	/* 0:Normal, 1:Inverted */
#define IMX908_REG_VREVERSE	CCI_REG8(0x3021)	/* 0:Normal, 1:Inverted */

/* ---- Internal Analog to Digital conversion bit width */
#define IMX908_REG_ADBIT	CCI_REG8(0x3022)
#define IMX908_ADBIT_10BIT	0x00	/* 10-bit */
#define IMX908_ADBIT_12BIT	0x01	/* 11-bit + digital dither */

#define IMX908_REG_MDBIT	CCI_REG8(0x3023)
#define IMX908_MDBIT_RAW10	0x00
#define IMX908_MDBIT_RAW12	0x01

/* ---- VMAX  Frame length in lines */
#define IMX908_REG_VMAX		CCI_REG24_LE(0x3028)	/* [19:0] */
#define IMX908_VMAX_MAX		0xfffffU
#define IMX908_VMAX_DEFAULT	2250U
/*
 * Window cropping mode restrictions:
 * VMAX >= PIX_VWIDTH + 70
 * VMAX >= 1206
 */
#define IMX908_VMAX_CROP_MIN_MARGIN	70U
#define IMX908_VMAX_CROP_MIN_LIMIT	1206U

/* ---- HMAX Line length in clocks */
#define IMX908_REG_HMAX		CCI_REG16_LE(0x302c)	/* [15:0] */
#define IMX908_HMAX_MAX		0xffffU
#define IMX908_HMAX_DEFAULT	1100U

/* ---- Cropping related */
#define IMX908_REG_PIX_HST	CCI_REG16_LE(0x303c)	/* [12:0] Init 0h */
#define IMX908_REG_PIX_HWIDTH	CCI_REG16_LE(0x303e)	/* [12:0] Init 0F10h */
#define IMX908_REG_LANEMODE	CCI_REG8(0x3040)	/* 1:2-lane, 3:4-lane */
#define IMX908_REG_PIX_VST	CCI_REG16_LE(0x3044)	/* [12:0] Init 0h */
#define IMX908_REG_PIX_VWIDTH	CCI_REG16_LE(0x3046)	/* [12:0] Init 0884h */
#define IMX908_CROP_ALIGN_VSTART	4U
#define IMX908_CROP_ALIGN_VWIDTH	4U
#define IMX908_CROP_ALIGN_HSTART	4U
#define IMX908_CROP_ALIGN_HWIDTH	16U
#define IMX908_CROP_MIN_HEIGHT		1136U
#define IMX908_CROP_MIN_WIDTH		1040U

/* ---- Shutter */
#define IMX908_REG_SHR0		CCI_REG24_LE(0x3050)	/* [19:0] */
#define IMX908_MIN_SHR0		7U

/*
 * Analogue gain control
 * Range is from 0 to 100 (0dB - 30dB) with 0.3dB step size
 * Values from 101 to 240 are valid but correspond to additional digital gain
 * (0.3dB - 42dB) so don't expose it to userspace
 */
#define IMX908_REG_GAIN		CCI_REG16_LE(0x3070)	/* [10:0] */
#define IMX908_ANA_GAIN_MAX	100
#define IMX908_ANA_GAIN_MIN	0
#define IMX908_ANA_GAIN_STEP	1
#define IMX908_ANA_GAIN_DEFAULT	0

/* ---- XVS/XHS output mode */
#define IMX908_XXS_DRV		CCI_REG8(0x30a6)	/* [1:0] XVS_DRV [3:2] XHS_DRV */

#define IMX908_REG_BLKLEVEL	CCI_REG16_LE(0x30dc)	/* [15:0] black level */

/* ---- Test Pattern Generator (TPG) registers */
#define IMX908_REG_TPG_EN	CCI_REG8(0x30e0)
#define IMX908_TPG_EN_BIT	BIT(0)
#define IMX908_REG_TPG_PATSEL	CCI_REG8(0x30e2)	/* [4:0] pattern idx */
#define IMX908_REG_TESTCLKEN	CCI_REG8(0x4900)
#define IMX908_TESTCLKEN_BIT	BIT(3)

#define IMX908_REG_TYPE_ID	CCI_REG16_LE(0x4c0c)
#define IMX908_CHIP_ID		0x038c			/* 908 */

/* ---- IMX908 Common Register List ---- */
static const struct cci_reg_sequence imx908_common_regs[] = {
	{ IMX908_XXS_DRV, IMX908_CONTROLLER_MODE },
	{ CCI_REG8(0x039c), 0x03 },
	{ CCI_REG8(0x3416), 0x20 },
	{ CCI_REG8(0x3417), 0x00 },
	{ CCI_REG8(0x3456), 0xf4 },
	{ CCI_REG8(0x345d), 0x01 },
	{ CCI_REG8(0x3460), 0x00 },
	{ CCI_REG8(0x3461), 0x0b },
	{ CCI_REG8(0x3471), 0x00 },
	{ CCI_REG8(0x3472), 0x23 },
	{ CCI_REG8(0x347b), 0x02 },
	{ CCI_REG8(0x3481), 0x01 },
	{ CCI_REG8(0x380c), 0x00 },
	{ CCI_REG8(0x380f), 0x0c },
	{ CCI_REG8(0x381c), 0x11 },
	{ CCI_REG8(0x3820), 0x22 },
	{ CCI_REG8(0x3824), 0x33 },
	{ CCI_REG8(0x3828), 0x22 },
	{ CCI_REG8(0x382c), 0x33 },
	{ CCI_REG8(0x3830), 0x33 },
	{ CCI_REG8(0x3838), 0x05 },
	{ CCI_REG8(0x383c), 0x07 },
	{ CCI_REG8(0x3840), 0x06 },
	{ CCI_REG8(0x3848), 0x17 },
	{ CCI_REG8(0x384c), 0x0b },
	{ CCI_REG8(0x3850), 0x10 },
	{ CCI_REG8(0x3854), 0x12 },
	{ CCI_REG8(0x385c), 0x20 },
	{ CCI_REG8(0x38c4), 0x64 },
	{ CCI_REG8(0x38c5), 0x64 },
	{ CCI_REG8(0x38c6), 0x64 },
	{ CCI_REG8(0x3c4a), 0x15 },
	{ CCI_REG8(0x3c4c), 0x13 },
	{ CCI_REG8(0x3c4d), 0x13 },
	{ CCI_REG8(0x3c4e), 0x13 },
	{ CCI_REG8(0x3c50), 0x77 },
	{ CCI_REG8(0x3c51), 0x07 },
	{ CCI_REG8(0x3db4), 0x00 },
	{ CCI_REG8(0x4419), 0x06 },
	{ CCI_REG8(0x441c), 0x00 },
	{ CCI_REG8(0x4426), 0x00 },
	{ CCI_REG8(0x4538), 0x20 },
	{ CCI_REG8(0x4539), 0x19 },
	{ CCI_REG8(0x453a), 0x19 },
	{ CCI_REG8(0x453b), 0x19 },
	{ CCI_REG8(0x453c), 0x19 },
	{ CCI_REG8(0x453d), 0x19 },
	{ CCI_REG8(0x453e), 0x19 },
	{ CCI_REG8(0x453f), 0x19 },
	{ CCI_REG8(0x4540), 0x19 },
	{ CCI_REG8(0x4544), 0x11 },
	{ CCI_REG8(0x4545), 0x11 },
	{ CCI_REG8(0x4546), 0x11 },
	{ CCI_REG8(0x463c), 0x20 },
	{ CCI_REG8(0x465e), 0xcf },
	{ CCI_REG8(0x4684), 0x20 },
	{ CCI_REG8(0x46a6), 0xcf },
	{ CCI_REG8(0x46e2), 0xf3 },
};

static const u32 tuning_regs[] = {
	CCI_REG8(0x3d78),
	CCI_REG8(0x3d79),
	CCI_REG8(0x3d80),
	CCI_REG8(0x3d81),
	CCI_REG8(0x3d88),
	CCI_REG8(0x3d89),
	CCI_REG8(0x3d90),
	CCI_REG8(0x3d91),
};

#define IMX908_EXPOSURE_MIN	1
#define IMX908_EXPOSURE_STEP	1

/* ---- Speed of internal clock */
#define IMX908_XHS_HZ		74250000ULL

/* Fixed pixel rate: column ADC reads 8 pixels per 74.25 MHz clock */
#define IMX908_PIX_PER_CLK	8U
#define IMX908_PIXEL_RATE	(IMX908_XHS_HZ * IMX908_PIX_PER_CLK)	/* 594 MHz */

/* ---- Subdev Pads */
#define IMX908_SOURCE_PAD	0

#define IMX908_DEFAULT_MBUS_CODE MEDIA_BUS_FMT_SRGGB10_1X10

/*
 * IMX908 total area includes active area height plus
 *  4 pixels effective pixel ignored area
 * 10 pixels vertical direction effective OB
 * 10 pixels OB side ignored area
 */
static const struct v4l2_rect imx908_total_area = {
	.top = 0,
	.left = 0,
	.width = 3856,
	.height = 2200,
};

static const struct v4l2_rect imx908_active_area = {
	.top = 0,
	.left = 0,
	.width = 3856,
	.height = 2176,
};

/* Recommended 4K recording area centered within the active area */
static const struct v4l2_rect imx908_recording_area = {
	.top = 8,
	.left = 8,
	.width = 3840,
	.height = 2160,
};

static const char *const imx908_supply_names[] = {
	"avdd",  /* Analog (3.3V) supply */
	"dvdd",  /* Digital Core (1.1V) supply */
	"ovdd",  /* IF (1.8V) supply */
};

static const u32 imx908_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10, /* RAW10 */
	MEDIA_BUS_FMT_SRGGB12_1X12, /* RAW12 */
};

enum {
	IMX908_LINK_FREQ_297MHZ,
	IMX908_LINK_FREQ_360MHZ,
	IMX908_LINK_FREQ_445MHZ,
	IMX908_LINK_FREQ_594MHZ,
	IMX908_LINK_FREQ_720MHZ,
	IMX908_LINK_FREQ_891MHZ,
	IMX908_LINK_FREQ_1039MHZ,
	IMX908_LINK_FREQ_1188MHZ,
};

static const s64 imx908_link_freqs[] = {
	[IMX908_LINK_FREQ_297MHZ]  =  297000000LL,
	[IMX908_LINK_FREQ_360MHZ]  =  360000000LL,
	[IMX908_LINK_FREQ_445MHZ]  =  445000000LL,
	[IMX908_LINK_FREQ_594MHZ]  =  594000000LL,
	[IMX908_LINK_FREQ_720MHZ]  =  720000000LL,
	[IMX908_LINK_FREQ_891MHZ]  =  891000000LL,
	[IMX908_LINK_FREQ_1039MHZ] = 1039500000LL,
	[IMX908_LINK_FREQ_1188MHZ] = 1188000000LL,
};

/* DDR lane_rate (Mbps/lane) = 2 x link_freq (Hz) / 1e6 */
static const u8 imx908_datarate_sel[] = {
	[IMX908_LINK_FREQ_297MHZ]  = 0x07, /*  594 Mbps */
	[IMX908_LINK_FREQ_360MHZ]  = 0x06, /*  720 Mbps */
	[IMX908_LINK_FREQ_445MHZ]  = 0x05, /*  891 Mbps */
	[IMX908_LINK_FREQ_594MHZ]  = 0x04, /* 1188 Mbps */
	[IMX908_LINK_FREQ_720MHZ]  = 0x03, /* 1440 Mbps */
	[IMX908_LINK_FREQ_891MHZ]  = 0x02, /* 1782 Mbps */
	[IMX908_LINK_FREQ_1039MHZ] = 0x01, /* 2079 Mbps */
	[IMX908_LINK_FREQ_1188MHZ] = 0x00, /* 2376 Mbps */
};

/* Allowed clock values in Hz */
static const u32 imx908_inck_table[] = {
	74250000,
	37125000,
	72000000,
	27000000,
	24000000,
};

struct imx908 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct device *dev;

	struct regmap *cci;

	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx908_supply_names)];

	u8  inck_sel;

	u8 num_lanes;
	unsigned long link_freq_bitmap;
	unsigned int link_freq_idx;

	/* Cached current sensor timing */
	u16 hmax;	/* clocks per line */
	u32 vmax;	/* lines per frame */

	struct {
		struct v4l2_ctrl_handler handler;

		struct v4l2_ctrl *pixel_rate;	/* fixed, read-only */
		struct v4l2_ctrl *exposure;
		struct v4l2_ctrl *vblank;
		struct v4l2_ctrl *hblank;
		struct v4l2_ctrl *test_pattern;
	} ctrls;
};

static inline struct imx908 *to_imx908(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx908, sd);
}

/* ----------------------- Basic Helper Functions --------------------------- */

static int imx908_link_freq_to_datarate_sel(struct imx908 *imx,
					    u64 link_freq_hz, u8 *sel)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(imx908_link_freqs); i++) {
		if (imx908_link_freqs[i] == link_freq_hz) {
			*sel = imx908_datarate_sel[i];
			return 0;
		}
	}
	return -EINVAL;
}

static bool imx908_mbus_code_supported(struct imx908 *imx, u32 code)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(imx908_mbus_codes); i++) {
		if (imx908_mbus_codes[i] == code)
			return true;
	}

	return false;
}

static u16 imx908_calc_hmax(u32 width, u32 hblank)
{
	/* Fixed pixel rate makes this a divide-by-8 */
	u32 hmax = DIV_ROUND_UP(width + hblank, IMX908_PIX_PER_CLK);

	return min_t(u32, hmax, IMX908_HMAX_MAX);
}

static u32 imx908_calc_vmax(u32 height, u32 vblank)
{
	return min_t(u32, height + vblank, IMX908_VMAX_MAX);
}

/*
 * Array (producer) floor: with the fixed pixel rate the column ADC emits
 * IMX908_PIX_PER_CLK pixels per internal clock, so a line cannot be shorter
 * than ceil(width / 8) internal clocks.
 */
static u32 imx908_calc_array_min_hmax(u32 width)
{
	return DIV_ROUND_UP(width, IMX908_PIX_PER_CLK);
}

/*
 * Link (consumer) floor: the line period must be long enough for the MIPI
 * burst (width * bpp bits) to drain over num_lanes * link_freq * 2 (DDR).
 */
static u32 imx908_calc_link_min_hmax(struct imx908 *imx, u32 width, u8 bpp)
{
	u64 link_hz = imx908_link_freqs[imx->link_freq_idx];
	u64 num = (u64)width * bpp * IMX908_XHS_HZ;
	u64 den = (u64)imx->num_lanes * link_hz * 2; /* DDR */

	/*
	 * den can exceed 32 bits (e.g. 4 lanes * 720 MHz * 2 = 5.76 GHz), so
	 * DIV_ROUND_UP_ULL / do_div would truncate the divisor to u32. Use a
	 * full 64/64 division.
	 */
	return DIV64_U64_ROUND_UP(num, den);
}

/* HMAX must satisfy both the array (producer) and link (consumer) limits */
static u16 imx908_calc_min_hmax(struct imx908 *imx, u32 width, u8 bpp)
{
	return max(imx908_calc_array_min_hmax(width),
		   imx908_calc_link_min_hmax(imx, width, bpp));
}

static u32 imx908_calc_min_vmax(const struct v4l2_rect *crop)
{
	/* In window crop mode constrain VMAX (from datasheet) */
	if (!v4l2_rect_equal(crop, &imx908_active_area))
		return max_t(u32, crop->height + IMX908_VMAX_CROP_MIN_MARGIN,
			   IMX908_VMAX_CROP_MIN_LIMIT);

	return IMX908_VMAX_DEFAULT;
}

static u32 imx908_calc_min_vblank(const struct v4l2_rect *crop)
{
	u32 min_vmax = imx908_calc_min_vmax(crop);

	if (min_vmax <= crop->height)
		return 0;

	return min_vmax - crop->height;
}

static u32 imx908_calc_max_vblank(u32 height)
{
	return IMX908_VMAX_MAX - height;
}

static int imx908_set_exposure_lines(struct imx908 *imx, u32 exposure_lines)
{
	u32 max_lines = imx->vmax - IMX908_MIN_SHR0;

	exposure_lines = clamp_t(u32, exposure_lines, 1, max_lines);

	u32 shr0 = imx->vmax - exposure_lines;

	return cci_write(imx->cci, IMX908_REG_SHR0, shr0, NULL);
}

static u8 imx908_bits_per_pixel(u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		return 12;
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	default:
		return 10;
	}
}

static u32 imx908_hmax_to_hblank(u16 hmax, u32 width)
{
	u32 line_len_pixels = hmax * IMX908_PIX_PER_CLK;

	if (line_len_pixels <= width)
		return 0;

	return line_len_pixels - width;
}

/*
 * Window crop: HMAX is fixed to the pre-crop drive mode (datasheet),
 * so cropping only extends HBLANK. Expose it as a single fixed value to
 * keep the userspace line time (and thus fps) consistent.
 */
static void imx908_update_hblank_crop(struct imx908 *imx, u32 width)
{
	u32 hblank = imx908_hmax_to_hblank(imx->hmax, width);

	__v4l2_ctrl_modify_range(imx->ctrls.hblank, hblank, hblank,
				 IMX908_PIX_PER_CLK, hblank);

	if (imx->ctrls.hblank->val != hblank)
		__v4l2_ctrl_s_ctrl(imx->ctrls.hblank, hblank);
}

static void imx908_update_hblank_limits(struct imx908 *imx, u32 width, u8 bpp)
{
	u16 min_hmax = imx908_calc_min_hmax(imx, width, bpp);
	u32 min_hblank = imx908_hmax_to_hblank(min_hmax, width);
	u32 max_hblank = imx908_hmax_to_hblank(IMX908_HMAX_MAX, width);
	u32 hblank = imx908_hmax_to_hblank(imx->hmax, width);

	hblank = clamp_t(u32, hblank, min_hblank, max_hblank);

	/* step = 8: one HMAX clock == 8 pixels of line length */
	__v4l2_ctrl_modify_range(imx->ctrls.hblank, min_hblank, max_hblank,
				 IMX908_PIX_PER_CLK, hblank);

	if (imx->ctrls.hblank->val != hblank)
		__v4l2_ctrl_s_ctrl(imx->ctrls.hblank, hblank);
}

static void imx908_update_vblank_limits(struct imx908 *imx,
					const struct v4l2_rect *crop)
{
	if (!imx->ctrls.vblank)
		return;

	u32 min_vblank = imx908_calc_min_vblank(crop);
	u32 max_vblank = imx908_calc_max_vblank(crop->height);

	__v4l2_ctrl_modify_range(imx->ctrls.vblank, min_vblank, max_vblank, 1,
				 imx->ctrls.vblank->default_value);
}

/* ------------------ Pattern Generator (TPG) ----------------------- */
static const char * const imx908_tpg_menu[] = {
	"Disabled",
	"All 000h",
	"All FFFh",
	"All 555h",
	"All AAAh",
	"555h / AAAh Toggle",
	"AAAh / 555h Toggle",
	"000h / 555h Toggle",
	"555h / 000h Toggle",
	"000h / FFFh Toggle",
	"FFFh / 000h Toggle",
	"Horiz Color Bars",
	"Vert Color Bars",
};

static int imx908_update_test_pattern(struct imx908 *imx, u32 index)
{
	int ret = 0;

	if (index == 0) {
		/* Picture (not TPG) output setting */
		cci_update_bits(imx->cci, IMX908_REG_TPG_EN, IMX908_TPG_EN_BIT,
				0x00, &ret);

		/* Disable TESTCLKEN (write-only register) */
		cci_write(imx->cci, IMX908_REG_TESTCLKEN, 0x00, &ret);

		return ret;
	}

	cci_write(imx->cci, IMX908_REG_TESTCLKEN, IMX908_TESTCLKEN_BIT, &ret);
	cci_write(imx->cci, IMX908_REG_TPG_PATSEL, index - 1, &ret);
	cci_update_bits(imx->cci, IMX908_REG_TPG_EN, IMX908_TPG_EN_BIT,
			IMX908_TPG_EN_BIT, &ret);

	return ret;
}

/* -------------------- Mode Building ----------------------- */
static void imx908_mode_build(struct imx908 *imx,
			      const struct v4l2_mbus_framefmt *fmt,
			      const struct v4l2_rect *crop)
{
	u8 bpp = imx908_bits_per_pixel(fmt->code);

	/* --- H timing (HMAX) --- */
	if (v4l2_rect_equal(crop, &imx908_active_area)) {
		u16 min_hmax = imx908_calc_min_hmax(imx, fmt->width, bpp);
		u32 min_hblank = imx908_hmax_to_hblank(min_hmax, fmt->width);
		u32 max_hblank = imx908_hmax_to_hblank(IMX908_HMAX_MAX,
						       fmt->width);
		u32 hblank = clamp_t(u32, imx->ctrls.hblank->val, min_hblank,
				     max_hblank);

		imx->hmax = imx908_calc_hmax(fmt->width, hblank);
	} else if (!imx->hmax) {
		/* First crop before any all-pixel config: seed from default */
		u32 hblank = imx908_hmax_to_hblank(IMX908_HMAX_DEFAULT,
						   imx908_active_area.width);

		imx->hmax = imx908_calc_hmax(fmt->width, hblank);
	}
	/* else: crop with HMAX already fixed by the drive mode - keep it */

	/* --- V timing (VMAX) --- */
	u32 min_vblank = imx908_calc_min_vblank(crop);
	u32 max_vblank = imx908_calc_max_vblank(fmt->height);
	u32 vblank = clamp_t(u32, imx->ctrls.vblank->val,
			     min_vblank, max_vblank);

	imx->vmax = imx908_calc_vmax(fmt->height, vblank);
}

static void imx908_update_framing_limits(struct imx908 *imx,
					 struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *fmt;
	const struct v4l2_rect *crop;

	fmt = v4l2_subdev_state_get_format(state, IMX908_SOURCE_PAD);
	crop = v4l2_subdev_state_get_crop(state, IMX908_SOURCE_PAD);
	u8 bpp = imx908_bits_per_pixel(fmt->code);

	/* All-pixel: HBLANK drives HMAX. Crop: HMAX fixed, HBLANK derived. */
	if (v4l2_rect_equal(crop, &imx908_active_area))
		imx908_update_hblank_limits(imx, fmt->width, bpp);
	else
		imx908_update_hblank_crop(imx, crop->width);

	imx908_update_vblank_limits(imx, crop);
	imx908_mode_build(imx, fmt, crop);

	__v4l2_ctrl_modify_range(imx->ctrls.exposure,
				 IMX908_EXPOSURE_MIN,
				 imx->vmax - IMX908_MIN_SHR0,
				 IMX908_EXPOSURE_STEP,
				 imx->ctrls.exposure->default_value);
}

/* ----------------------- HW Programming ---------------------- */

static int imx908_mode_apply(struct imx908 *imx,
			     const struct v4l2_mbus_framefmt *fmt)
{
	int ret = 0;
	u8 adbit, mdbit;

	switch (fmt->code) {
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		adbit = IMX908_ADBIT_12BIT;
		mdbit = IMX908_MDBIT_RAW12;
		break;

	case MEDIA_BUS_FMT_SRGGB10_1X10:
	default:
		adbit = IMX908_ADBIT_10BIT;
		mdbit = IMX908_MDBIT_RAW10;
		break;
	}

	cci_write(imx->cci, IMX908_REG_XMASTER, IMX908_CONTROLLER_MODE, &ret);
	cci_write(imx->cci, IMX908_REG_ADBIT, adbit, &ret);
	cci_write(imx->cci, IMX908_REG_MDBIT, mdbit, &ret);

	/* Tuning registers that need to be changed for RAW10/10-bit */
	u8 value = adbit == IMX908_ADBIT_10BIT &&
		   mdbit == IMX908_MDBIT_RAW10 ? 0x0c : 0x05;

	for (unsigned int i = 0; i < ARRAY_SIZE(tuning_regs); ++i)
		cci_write(imx->cci, tuning_regs[i], value, &ret);

	cci_write(imx->cci, IMX908_REG_INCK_SEL, imx->inck_sel, &ret);
	if (ret) {
		dev_err(imx->dev, "INCK register write failed: %d\n", ret);
		return ret;
	}

	u8 datarate_sel;
	u64 link_freq = imx908_link_freqs[imx->link_freq_idx];

	ret = imx908_link_freq_to_datarate_sel(imx, link_freq, &datarate_sel);
	cci_write(imx->cci, IMX908_REG_DATARATE_SEL, datarate_sel, &ret);
	cci_write(imx->cci, IMX908_REG_LANEMODE, imx->num_lanes - 1, &ret);

	/* Recommended black level offset is 50 in 10-bit, 200 for others */
	u16 blklevel = (mdbit == IMX908_MDBIT_RAW10) ? 50 : 200;

	cci_write(imx->cci, IMX908_REG_BLKLEVEL, blklevel, &ret);

	return ret;
}

static int imx908_program_window(struct imx908 *imx,
				 const struct v4l2_rect *crop)
{
	bool all_pixel_mode = v4l2_rect_equal(crop, &imx908_active_area);
	int ret = 0;

	cci_write(imx->cci, IMX908_REG_WINMODE,
		  all_pixel_mode ? IMX908_WINMODE_ALLPIX : IMX908_WINMODE_CROP,
		  &ret);

	if (!all_pixel_mode) {
		cci_write(imx->cci, IMX908_REG_PIX_HST,    crop->left,   &ret);
		cci_write(imx->cci, IMX908_REG_PIX_HWIDTH, crop->width,  &ret);
		cci_write(imx->cci, IMX908_REG_PIX_VST,    crop->top,    &ret);
		cci_write(imx->cci, IMX908_REG_PIX_VWIDTH, crop->height, &ret);
	}

	return ret;
}

/* ----------------------- Start/Stop streaming ---------------------- */

static int imx908_start_streaming(struct imx908 *imx,
				  struct v4l2_subdev_state *sd_state)
{
	const struct v4l2_rect *crop;
	const struct v4l2_mbus_framefmt *format;
	int ret = 0;

	crop = v4l2_subdev_state_get_crop(sd_state, IMX908_SOURCE_PAD);
	format = v4l2_subdev_state_get_format(sd_state, IMX908_SOURCE_PAD);

	imx908_mode_build(imx, format, crop);

	cci_multi_reg_write(imx->cci, imx908_common_regs,
			    ARRAY_SIZE(imx908_common_regs), &ret);
	if (ret)
		return ret;

	ret = imx908_mode_apply(imx, format);
	if (ret)
		return ret;

	ret = imx908_program_window(imx, crop);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_handler_setup(imx->sd.ctrl_handler);
	if (ret)
		return ret;

	cci_write(imx->cci, IMX908_REG_STANDBY, IMX908_STANDBY_CANCEL, &ret);
	msleep(24); /* Datasheet >=24 ms stabilization after standby cancel */

	cci_write(imx->cci, IMX908_REG_XMSTA, IMX908_START_CONTROLLER, &ret);
	return ret;
}

static int imx908_stop_streaming(struct imx908 *imx)
{
	int ret = 0;

	cci_write(imx->cci, IMX908_REG_STANDBY, IMX908_STANDBY_EN, &ret);
	cci_write(imx->cci, IMX908_REG_XMSTA, IMX908_STOP_CONTROLLER, &ret);
	return ret;
}

/* --------------------------- V4L2 controls ------------------------------ */

static int imx908_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx908 *imx = container_of(ctrl->handler, struct imx908,
					  ctrls.handler);
	struct v4l2_subdev_state *state;
	const struct v4l2_mbus_framefmt *format;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&imx->sd);
	format = v4l2_subdev_state_get_format(state, IMX908_SOURCE_PAD);

	/* Update exposure control limits even if the sensor is not streaming */
	if (ctrl->id == V4L2_CID_VBLANK) {
		const struct v4l2_rect *crop;

		crop = v4l2_subdev_state_get_crop(state, IMX908_SOURCE_PAD);

		u32 min_vblank = imx908_calc_min_vblank(crop);
		u32 max_vblank = imx908_calc_max_vblank(format->height);
		u32 vblank = clamp_t(u32, ctrl->val, min_vblank, max_vblank);

		imx->vmax = imx908_calc_vmax(format->height, vblank);

		__v4l2_ctrl_modify_range(imx->ctrls.exposure,
					 IMX908_EXPOSURE_MIN,
					 imx->vmax - IMX908_MIN_SHR0,
					 IMX908_EXPOSURE_STEP,
					 imx->ctrls.exposure->default_value);
	}

	/* Hardware writes only when powered; cached ctrls applied on resume */
	ret = pm_runtime_get_if_in_use(imx->dev);
	if (ret <= 0)
		return ret;
	ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = imx908_set_exposure_lines(imx, ctrl->val);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(imx->cci, IMX908_REG_GAIN, ctrl->val, &ret);
		break;

	case V4L2_CID_VBLANK:
		ret = cci_write(imx->cci, IMX908_REG_VMAX, imx->vmax, NULL);
		/* SHR0 derived from VMAX, re-apply exposure after changes */
		if (!ret)
			ret = imx908_set_exposure_lines(imx,
							imx->ctrls.exposure->val);

		break;

	case V4L2_CID_HBLANK: {
		const struct v4l2_rect *crop;

		crop = v4l2_subdev_state_get_crop(state, IMX908_SOURCE_PAD);

		/* HBLANK drives HMAX only in all-pixel; crop HMAX is fixed */
		if (v4l2_rect_equal(crop, &imx908_active_area))
			imx->hmax = imx908_calc_hmax(format->width, ctrl->val);

		cci_write(imx->cci, IMX908_REG_HMAX, imx->hmax, &ret);
		break;
	}

	case V4L2_CID_PIXEL_RATE:
	case V4L2_CID_LINK_FREQ:
		break;

	case V4L2_CID_TEST_PATTERN:
		ret = imx908_update_test_pattern(imx, ctrl->val);
		break;

	case V4L2_CID_HFLIP:
		cci_write(imx->cci, IMX908_REG_HREVERSE, ctrl->val, &ret);
		break;

	case V4L2_CID_VFLIP:
		cci_write(imx->cci, IMX908_REG_VREVERSE, ctrl->val, &ret);
		break;

	default:
		dev_warn(imx->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(imx->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx908_ctrl_ops = {
	.s_ctrl = imx908_set_ctrl,
};

/* ------------------------ Pad/video ops ------------------ */

static int imx908_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(imx908_mbus_codes))
		return -EINVAL;
	code->code = imx908_mbus_codes[code->index];
	return 0;
}

static int imx908_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx908 *imx = to_imx908(sd);
	const struct v4l2_rect *crop;

	if (fse->index > 0)
		return -EINVAL;

	if (!imx908_mbus_code_supported(imx, fse->code))
		return -EINVAL;

	crop = v4l2_subdev_state_get_crop(sd_state, IMX908_SOURCE_PAD);

	fse->min_width  = crop->width;
	fse->max_width  = crop->width;
	fse->min_height = crop->height;
	fse->max_height = crop->height;

	return 0;
}

static int imx908_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx908 *imx = to_imx908(sd);
	struct v4l2_mbus_framefmt *format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	format = v4l2_subdev_state_get_format(sd_state, fmt->pad);

	if (imx908_mbus_code_supported(imx, fmt->format.code))
		format->code = fmt->format.code;
	else
		format->code = IMX908_DEFAULT_MBUS_CODE;

	/* Fully define metadata */
	format->field        = V4L2_FIELD_NONE;
	format->colorspace   = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc    = V4L2_YCBCR_ENC_DEFAULT;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func    = V4L2_XFER_FUNC_NONE;

	/* Limits track the real controls only for the ACTIVE state, not TRY */
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		imx908_update_framing_limits(imx, sd_state);

	fmt->format = *format;

	return 0;
}

static int imx908_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	struct v4l2_rect *r = &sel->r;
	struct imx908 *imx = to_imx908(sd);

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	crop = v4l2_subdev_state_get_crop(sd_state, sel->pad);
	format = v4l2_subdev_state_get_format(sd_state, sel->pad);

	/*
	 * Round the crop rectangle size down to the hardware alignment
	 * constraints, then clamp it to the supported size range.
	 */
	r->width = clamp_t(s32, ALIGN_DOWN(r->width, IMX908_CROP_ALIGN_HWIDTH),
			   IMX908_CROP_MIN_WIDTH, imx908_active_area.width);
	r->height = clamp_t(s32, ALIGN_DOWN(r->height, IMX908_CROP_ALIGN_VWIDTH),
			    IMX908_CROP_MIN_HEIGHT, imx908_active_area.height);

	/*
	 * Round and clamp the crop position similarly, with the maximum value
	 * chosen so that the crop rectangle remains fully inside the active
	 * pixel array.
	 */
	r->left = clamp_t(s32, ALIGN_DOWN(r->left, IMX908_CROP_ALIGN_HSTART), 0,
			  imx908_active_area.width - r->width);
	r->top = clamp_t(s32, ALIGN_DOWN(r->top, IMX908_CROP_ALIGN_VSTART), 0,
			 imx908_active_area.height - r->height);

	*crop = *r;

	/* IMX908 has no binning, so the output size matches the crop 1:1 */
	format->width  = crop->width;
	format->height = crop->height;

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		imx908_update_framing_limits(imx, sd_state);

	return 0;
}

static int imx908_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state,
						     IMX908_SOURCE_PAD);
		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = imx908_total_area;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = imx908_recording_area;
		return 0;

	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = imx908_active_area;
		return 0;

	default:
		return -EINVAL;
	}
}

static int imx908_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 u32 pad,
				 u64 streams_mask)
{
	struct imx908 *imx = to_imx908(sd);
	int ret;

	ret = pm_runtime_resume_and_get(imx->dev);
	if (ret)
		return ret;

	ret = imx908_start_streaming(imx, sd_state);
	if (ret) {
		pm_runtime_mark_last_busy(imx->dev);
		pm_runtime_put_autosuspend(imx->dev);
		return ret;
	}

	return 0;
}

static int imx908_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  u32 pad,
				  u64 streams_mask)
{
	struct imx908 *imx = to_imx908(sd);
	int ret;

	ret = imx908_stop_streaming(imx);

	pm_runtime_mark_last_busy(imx->dev);
	pm_runtime_put_autosuspend(imx->dev);

	return ret;
}

static int imx908_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_selection sel = {
		.which  = V4L2_SUBDEV_FORMAT_TRY,
		.pad    = IMX908_SOURCE_PAD,
		.target = V4L2_SEL_TGT_CROP,
		.r      = imx908_active_area,
	};
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad   = IMX908_SOURCE_PAD,
		.format = {
			.code   = IMX908_DEFAULT_MBUS_CODE,
			.width  = imx908_active_area.width,
			.height = imx908_active_area.height,
		},
	};

	imx908_set_selection(sd, sd_state, &sel);
	imx908_set_pad_format(sd, sd_state, &fmt);

	return 0;
}

static const struct v4l2_subdev_core_ops imx908_core_ops = {
	.subscribe_event   = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops imx908_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx908_pad_ops = {
	.enum_mbus_code      = imx908_enum_mbus_code,
	.enum_frame_size     = imx908_enum_frame_size,
	.get_fmt             = v4l2_subdev_get_fmt,
	.set_fmt             = imx908_set_pad_format,
	.get_selection       = imx908_get_selection,
	.set_selection       = imx908_set_selection,
	.enable_streams      = imx908_enable_streams,
	.disable_streams     = imx908_disable_streams,
};

static const struct v4l2_subdev_internal_ops imx908_internal_ops = {
	.init_state = imx908_init_state,
};

static const struct v4l2_subdev_ops imx908_subdev_ops = {
	.core  = &imx908_core_ops,
	.video = &imx908_video_ops,
	.pad   = &imx908_pad_ops,
};

/* ----------------------- Power management ---------------------- */

static int imx908_power_on(struct imx908 *imx)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx908_supply_names),
				    imx->supplies);
	if (ret) {
		dev_err(imx->dev, "failed to enable regulators\n");
		return ret;
	}
	msleep(200); /* IMX908 power ok after 200ms */

	if (imx->reset_gpio) {
		gpiod_set_value_cansleep(imx->reset_gpio, 1); /* XCLR low */
		udelay(1); /* >= 500ns T_low */
		gpiod_set_value_cansleep(imx->reset_gpio, 0); /* Sensor start */
	}

	ret = clk_prepare_enable(imx->xclk);
	if (ret) {
		dev_err(imx->dev, "failed to enable xclk: %d\n", ret);
		goto err_reset;
	}

	/* T_1 >=20us delay before initial SDA/SCL */
	usleep_range(20, 25);

	return 0;

err_reset:
	gpiod_set_value_cansleep(imx->reset_gpio, 1); /* assert reset */
	regulator_bulk_disable(ARRAY_SIZE(imx908_supply_names), imx->supplies);
	return ret;
}

static void imx908_power_off(struct imx908 *imx)
{
	gpiod_set_value_cansleep(imx->reset_gpio, 1);
	clk_disable_unprepare(imx->xclk);
	regulator_bulk_disable(ARRAY_SIZE(imx908_supply_names), imx->supplies);
}

static int imx908_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx908 *imx = to_imx908(sd);

	return imx908_power_on(imx);
}

static int imx908_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx908 *imx = to_imx908(sd);

	imx908_power_off(imx);
	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(imx908_pm_ops,
				 imx908_runtime_suspend,
				 imx908_runtime_resume, NULL);

/* ------------------------------- Probe/remove --------------------------- */

static int imx908_get_inck_sel(struct imx908 *imx, u32 xclk_freq)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(imx908_inck_table); i++) {
		if (imx908_inck_table[i] == xclk_freq) {
			imx->inck_sel = i;
			return 0;
		}
	}

	return dev_err_probe(imx->dev, -EINVAL, "unsupported xclk %u Hz\n",
			     xclk_freq);
}

static int imx908_get_regulators(struct imx908 *imx)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(imx908_supply_names); i++)
		imx->supplies[i].supply = imx908_supply_names[i];

	return devm_regulator_bulk_get(imx->dev,
			ARRAY_SIZE(imx908_supply_names),
			imx->supplies);
}

static int imx908_parse_fwnode(struct imx908 *imx)
{
	struct fwnode_handle *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = 0;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(imx->dev), NULL);
	if (!ep) {
		dev_err(imx->dev, "failed to find endpoint in device tree\n");
		return -ENXIO;
	}

	/* Only data-lanes and link-frequencies are used from the endpoint */
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	imx->num_lanes = bus_cfg.bus.mipi_csi2.num_data_lanes;

	if (imx->num_lanes != 2 && imx->num_lanes != 4) {
		dev_err(imx->dev,
			"only 2 or 4 CSI-2 data lanes are supported (got %u)\n",
			imx->num_lanes);
		ret = -EINVAL;
		goto out_free;
	}

	ret = v4l2_link_freq_to_bitmap(imx->dev,
				       bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx908_link_freqs,
				       ARRAY_SIZE(imx908_link_freqs),
				       &imx->link_freq_bitmap);
	if (ret) {
		dev_err(imx->dev, "failed to parse link frequencies from DT\n");
		goto out_free;
	}

	if (bitmap_empty(&imx->link_freq_bitmap, ARRAY_SIZE(imx908_link_freqs))) {
		dev_err(imx->dev,
			"no common link frequencies between driver and DT\n");
		ret = -EINVAL;
		goto out_free;
	}

	imx->link_freq_idx = __ffs(imx->link_freq_bitmap);
	dev_dbg(imx->dev, "using %u lanes at link freq %llu Hz\n",
		imx->num_lanes, imx908_link_freqs[imx->link_freq_idx]);

out_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int imx908_init_controls(struct imx908 *imx)
{
	struct v4l2_ctrl_handler *hdl = &imx->ctrls.handler;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *link_freq_ctl;
	int ret;

	ret = v4l2_ctrl_handler_init(hdl, 11);
	if (ret)
		return ret;

	imx->ctrls.pixel_rate = v4l2_ctrl_new_std(hdl, &imx908_ctrl_ops,
						  V4L2_CID_PIXEL_RATE,
						  IMX908_PIXEL_RATE,
						  IMX908_PIXEL_RATE, 1,
						  IMX908_PIXEL_RATE);
	if (imx->ctrls.pixel_rate)
		imx->ctrls.pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	link_freq_ctl = v4l2_ctrl_new_int_menu(hdl, &imx908_ctrl_ops,
					       V4L2_CID_LINK_FREQ,
					       ARRAY_SIZE(imx908_link_freqs) - 1,
					       imx->link_freq_idx,
					       imx908_link_freqs);

	if (link_freq_ctl)
		link_freq_ctl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx->hmax = IMX908_HMAX_DEFAULT;
	imx->vmax = IMX908_VMAX_DEFAULT;

	u32 min_vblank = IMX908_VMAX_DEFAULT - imx908_active_area.height;
	u32 max_vblank = imx908_calc_max_vblank(imx908_active_area.height);

	imx->ctrls.vblank = v4l2_ctrl_new_std(hdl, &imx908_ctrl_ops,
					      V4L2_CID_VBLANK,
					      min_vblank,
					      max_vblank,
					      1,
					      min_vblank);

	u8 bpp = imx908_bits_per_pixel(IMX908_DEFAULT_MBUS_CODE);
	u16 min_hmax = imx908_calc_min_hmax(imx, imx908_active_area.width, bpp);
	u32 min_hblank = imx908_hmax_to_hblank(min_hmax,
					       imx908_active_area.width);
	u32 max_hblank = imx908_hmax_to_hblank(IMX908_HMAX_MAX,
					       imx908_active_area.width);
	u32 hblank = imx908_hmax_to_hblank(IMX908_HMAX_DEFAULT,
					   imx908_active_area.width);

	/* Default HMAX can be infeasible at low link freqs; clamp into range */
	hblank = clamp_t(u32, hblank, min_hblank, max_hblank);

	/* Keep cached HMAX consistent with the clamped default */
	imx->hmax = imx908_calc_hmax(imx908_active_area.width, hblank);

	imx->ctrls.hblank = v4l2_ctrl_new_std(hdl, &imx908_ctrl_ops,
					      V4L2_CID_HBLANK,
					      min_hblank,
					      max_hblank,
					      IMX908_PIX_PER_CLK,
					      hblank);
	u32 max_exp = IMX908_VMAX_DEFAULT - IMX908_MIN_SHR0;

	imx->ctrls.exposure = v4l2_ctrl_new_std(hdl, &imx908_ctrl_ops,
						V4L2_CID_EXPOSURE,
						IMX908_EXPOSURE_MIN,
						max_exp,
						IMX908_EXPOSURE_STEP,
						max_exp / 2);

	v4l2_ctrl_new_std(hdl, &imx908_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX908_ANA_GAIN_MIN, IMX908_ANA_GAIN_MAX,
			  IMX908_ANA_GAIN_STEP, IMX908_ANA_GAIN_DEFAULT);

	/* Set test pattern. Menu (13 entries: Disabled + 12 patterns) */
	imx->ctrls.test_pattern = v4l2_ctrl_new_std_menu_items(hdl,
							       &imx908_ctrl_ops,
							       V4L2_CID_TEST_PATTERN,
							       ARRAY_SIZE(imx908_tpg_menu) - 1,
							       0,
							       0,
							       imx908_tpg_menu);
	if (imx->ctrls.test_pattern)
		imx->ctrls.test_pattern->flags |= V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;

	v4l2_ctrl_new_std(hdl, &imx908_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(hdl, &imx908_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);

	/* Read rotation and orientation properties from the firmware node */
	ret = v4l2_fwnode_device_parse(imx->dev, &props);
	if (ret)
		goto err_free;

	v4l2_ctrl_new_fwnode_properties(hdl, &imx908_ctrl_ops, &props);
	if (hdl->error) {
		ret = hdl->error;
		goto err_free;
	}

	imx->sd.ctrl_handler = hdl;

	return 0;

err_free:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static int imx908_identify_model(struct imx908 *imx)
{
	u16 chip_id;
	u64 val;
	int ret;

	/*
	 * The TYPE_ID registers are not accessible after power-up while
	 * the device remains in standby. Exit standby and wait for the
	 * required stabilization period before reading the chip ID.
	 */
	ret = cci_write(imx->cci, IMX908_REG_STANDBY, IMX908_STANDBY_CANCEL, NULL);
	if (ret)
		return ret;

	msleep(24); /* Datasheet: >=24 ms stabilization after standby cancel */

	ret = cci_read(imx->cci, IMX908_REG_TYPE_ID, &val, NULL);
	if (ret)
		return ret;

	chip_id = val;
	dev_dbg(imx->dev, "IMX908 chip ID: 0x%04x\n", chip_id);

	if (chip_id != IMX908_CHIP_ID) {
		dev_err(imx->dev, "unexpected chip ID 0x%04x (expected 0x%04x)\n",
			chip_id, IMX908_CHIP_ID);
		return -ENXIO;
	}

	/* Set to standby mode */
	ret = cci_write(imx->cci, IMX908_REG_STANDBY, IMX908_STANDBY_EN, NULL);
	if (ret) {
		dev_err(imx->dev, "failed to enter standby state: %d\n", ret);
		return ret;
	}

	return 0;
}

static int imx908_probe(struct i2c_client *client)
{
	struct imx908 *imx;
	int ret;

	imx = devm_kzalloc(&client->dev, sizeof(*imx), GFP_KERNEL);
	if (!imx)
		return -ENOMEM;
	imx->dev = &client->dev;

	v4l2_i2c_subdev_init(&imx->sd, client, &imx908_subdev_ops);
	imx->sd.internal_ops = &imx908_internal_ops;

	imx->cci = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx->cci))
		return dev_err_probe(&client->dev, PTR_ERR(imx->cci),
				     "CCI regmap init failed\n");

	imx->xclk = devm_clk_get(imx->dev, NULL);
	if (IS_ERR(imx->xclk))
		return dev_err_probe(imx->dev, PTR_ERR(imx->xclk),
				     "failed to get clock\n");

	ret = imx908_get_inck_sel(imx, clk_get_rate(imx->xclk));
	if (ret)
		return ret;

	imx->reset_gpio = devm_gpiod_get_optional(imx->dev, "reset",
						  GPIOD_OUT_HIGH);

	if (IS_ERR(imx->reset_gpio))
		return dev_err_probe(imx->dev, PTR_ERR(imx->reset_gpio),
				     "failed to get reset gpio\n");

	ret = imx908_get_regulators(imx);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to get regulators\n");

	ret = imx908_parse_fwnode(imx);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "device tree parse failed\n");

	ret = imx908_power_on(imx);
	if (ret)
		return dev_err_probe(&client->dev, ret, "power-on failed\n");

	ret = imx908_identify_model(imx);
	if (ret) {
		dev_err(imx->dev, "failed to identify model: %d\n", ret);
		goto err_power_off;
	}

	pm_runtime_set_active(imx->dev);
	pm_runtime_enable(imx->dev);

	ret = imx908_init_controls(imx);
	if (ret)
		goto err_pm_disable;

	imx->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	imx->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx->sd.entity, 1, &imx->pad);
	if (ret)
		goto err_hdl;

	/* Share the ctrl handler lock so s_ctrl can access the locked state */
	imx->sd.state_lock = imx->ctrls.handler.lock;

	ret = v4l2_subdev_init_finalize(&imx->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_autosuspend_delay(imx->dev, 1000);
	pm_runtime_use_autosuspend(imx->dev);
	pm_runtime_mark_last_busy(imx->dev);

	ret = v4l2_async_register_subdev_sensor(&imx->sd);
	if (ret)
		goto err_subdev;

	return 0;

err_subdev:
	v4l2_subdev_cleanup(&imx->sd);
err_entity:
	media_entity_cleanup(&imx->sd.entity);
err_hdl:
	v4l2_ctrl_handler_free(imx->sd.ctrl_handler);
err_pm_disable:
	pm_runtime_disable(imx->dev);
	pm_runtime_set_suspended(imx->dev);
err_power_off:
	imx908_power_off(imx);

	return ret;
}

static void imx908_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx908 *imx = to_imx908(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(imx->dev);
	if (!pm_runtime_status_suspended(imx->dev))
		imx908_power_off(imx);
	pm_runtime_set_suspended(imx->dev);
}

static const struct of_device_id imx908_of_ids[] = {
	{ .compatible = "sony,imx908" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx908_of_ids);

static struct i2c_driver imx908_i2c_driver = {
	.driver = {
		.name           = "imx908",
		.pm             = pm_ptr(&imx908_pm_ops),
		.of_match_table = imx908_of_ids,
	},
	.probe  = imx908_probe,
	.remove = imx908_remove,
};
module_i2c_driver(imx908_i2c_driver);

MODULE_AUTHOR("Lachlan Michael <Lachlan.Michael@sony.com>");
MODULE_DESCRIPTION("Sony IMX908 Sensor Driver");
MODULE_LICENSE("GPL");
