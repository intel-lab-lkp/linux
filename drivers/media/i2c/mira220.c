// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for ams MIRA220 cameras.
 *
 * Mira220 comes in monochrome, RGB and IR-RGB variants. The driver supports
 * the mono and RGB variants.
 *
 * Copyright (C) 2026, ams-OSRAM
 *
 * Based on Sony IMX219 camera driver
 * Copyright (C) 2019, Raspberry Pi (Trading) Ltd
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

/*
 * Active pixel array is 1600 (H) x 1400 (V) pixels.
 * Physical resolution including buffer pixels: 1642 (H) x 1464 (V) pixels.
 */
#define MIRA220_NATIVE_WIDTH 1642U
#define MIRA220_NATIVE_HEIGHT 1464U
#define MIRA220_PIXEL_ARRAY_LEFT 21U
#define MIRA220_PIXEL_ARRAY_TOP 32U
#define MIRA220_PIXEL_ARRAY_WIDTH 1600U
#define MIRA220_PIXEL_ARRAY_HEIGHT 1400U

/* Mira220 does not support analog gain. */
#define MIRA220_ANALOG_GAIN_MIN 1
#define MIRA220_ANALOG_GAIN_MAX 1
#define MIRA220_ANALOG_GAIN_STEP 1
#define MIRA220_ANALOG_GAIN_DEFAULT MIRA220_ANALOG_GAIN_MIN

/* Bit order */
#define MIRA220_BIT_ORDER_REG CCI_REG8(0x2063)
#define MIRA220_BIT_ORDER_NORMAL 0x00
#define MIRA220_BIT_ORDER_REVERSED 0x01

/* Bit depth */
#define MIRA220_BIT_DEPTH_REG CCI_REG8(0x209E)
#define MIRA220_BIT_DEPTH_12_BIT 0x02
#define MIRA220_BIT_DEPTH_10_BIT 0x04
#define MIRA220_BIT_DEPTH_8_BIT 0x06
#define MIRA220_CSI_DATA_TYPE_REG CCI_REG8(0x208D)
#define MIRA220_CSI_DATA_TYPE_12_BIT 0x04
#define MIRA220_CSI_DATA_TYPE_10_BIT 0x02
#define MIRA220_CSI_DATA_TYPE_8_BIT 0x01

/* Imager state master/slave registers */
#define MIRA220_IMAGER_STATE_REG CCI_REG8(0x1003)
#define MIRA220_IMAGER_STATE_STOP_AT_ROW 0x02
#define MIRA220_IMAGER_STATE_STOP_AT_FRAME 0x04
#define MIRA220_IMAGER_STATE_MASTER_CONTROL 0x10
#define MIRA220_IMAGER_STATE_SLAVE_CONTROL 0x08

/* Start image acquisition */
#define MIRA220_IMAGER_RUN_REG CCI_REG8(0x10F0)
#define MIRA220_IMAGER_RUN_START 0x01
#define MIRA220_IMAGER_RUN_STOP 0x00

/* Continuous running, not limited to nr of frames. */
#define MIRA220_IMAGER_RUN_CONT_REG CCI_REG8(0x1002)
#define MIRA220_IMAGER_RUN_CONT_ENABLE 0x04
#define MIRA220_IMAGER_RUN_CONT_DISABLE 0x00

/* Exposure time is indicated in number of rows */
#define MIRA220_EXP_TIME_REG CCI_REG16_LE(0x100C)

/* Vertical Blank */
#define MIRA220_VBLANK_REG CCI_REG16_LE(0x1012)
#define MIRA220_MIN_VBLANK_MARGIN 11
#define MIRA220_MAX_VBLANK 0xffff

/* Horizontal flip */
#define MIRA220_HFLIP_REG CCI_REG8(0x209C)
#define MIRA220_HFLIP_ENABLE_MIRROR 1
#define MIRA220_HFLIP_DISABLE_MIRROR 0

/* Vertical flip */
#define MIRA220_VFLIP_REG CCI_REG8(0x1095)
#define MIRA220_VFLIP_ENABLE_FLIP 1
#define MIRA220_VFLIP_DISABLE_FLIP 0

#define MIRA220_VSIZE_REG CCI_REG16_LE(0x1087)
#define MIRA220_VSTART_REG CCI_REG16_LE(0x107d)
#define MIRA220_HSIZE_REG CCI_REG16_LE(0x2008)
#define MIRA220_HSIZE_MASK GENMASK(9, 0)
#define MIRA220_HSTART_REG CCI_REG16_LE(0x200a)
#define MIRA220_HSTART_MASK GENMASK(9, 0)
#define MIRA220_MIPI_HSIZE_REG CCI_REG16_LE(0x207d)

/* MIPI bus control */
#define MIRA220_MIPI_LANES_REG CCI_REG8(0x6012)

#define MIRA220_MIPI_CLK_MODE_REG CCI_REG8(0x6013)
#define MIRA220_MIPI_CLK_CONTINUOUS 0x00
#define MIRA220_MIPI_CLK_NON_CONTINUOUS 0x01

/* OTP control */
#define MIRA220_OTP_CMD_REG CCI_REG8(0x0080)
#define MIRA220_OTP_CMD_UP 0x4
#define MIRA220_OTP_CMD_DOWN 0x8

/* Global sampling time */
#define MIRA220_GLOB_NUM_CLK_CYCLES 1928

/* External clock frequency is 38.4 M */
#define MIRA220_SUPPORTED_XCLK_FREQ 38400000

/* Default exposure is adjusted to mode with smallest height */
#define MIRA220_DEFAULT_EXPOSURE 1000
#define MIRA220_MAX_EXPOSURE 0xffff
#define MIRA220_EXPOSURE_MIN 1

/*
 * Pixel rate is an artificial value. This value is used for timing calculations
 * in combination with vblank/hblank.
 */
#define MIRA220_PIXEL_RATE 384000000

/* Total line length for 2 lanes calculated for ROW_LENGTH = 304. */
#define MIRA220_LLP_1600x1400_304 3040

/*
 * ROW_LENGTH: duration of row length in clk_in cycles; set to minimum supported
 * value for higher frame rate.
 */
#define MIRA220_REG_ROW_LENGTH		CCI_REG16_LE(0x102b)
#define MIRA220_ROW_LENGTH_MIN		304

/* Test Pattern */
#define MIRA220_REG_TEST_PATTERN CCI_REG8(0x2091)
#define MIRA220_TEST_PATTERN_DISABLE 0x00
#define MIRA220_TEST_PATTERN_VERTICAL_GRADIENT 0x01

/* Module variant identifiers */
#define MIRA220_MONO_PLAIN_PF		0x1e62a032
#define MIRA220_MONO_PLAIN_NO_PF	0x1e62a045
#define MIRA220_MONO_AR_PF		0x1e62a036
#define MIRA220_RGB_PLAIN_PF		0x1e62a03b
#define MIRA220_RGB_PLAIN_NO_PF		0x1e62a046
#define MIRA220_RGB_AR_PF		0x1e62a03d

enum mira220_variants {
	MIRA220_VARIANT_MONO,
	MIRA220_VARIANT_RGB,
};

static const struct cci_reg_sequence mira220_init_reg_list[] = {
	/* Base configuration*/
	{ CCI_REG8(0x6006), 0x0 },
	{ CCI_REG8(0x6012), 0x1 },
	{ CCI_REG8(0x6013), 0x0 },
	{ CCI_REG8(0x6006), 0x1 },
	{ CCI_REG8(0x205D), 0x0 },
	{ CCI_REG8(0x2063), 0x0 },
	{ CCI_REG8(0x24DC), 0x13 },
	{ CCI_REG8(0x24DD), 0x3 },
	{ CCI_REG8(0x24DE), 0x3 },
	{ CCI_REG8(0x24DF), 0x0 },
	{ CCI_REG8(0x4006), 0x8 },
	{ CCI_REG8(0x401C), 0x6F },
	{ CCI_REG8(0x204B), 0x3 },
	{ CCI_REG8(0x205B), 0x64 },
	{ CCI_REG8(0x205C), 0x0 },
	{ CCI_REG8(0x4018), 0x3F },
	{ CCI_REG8(0x403B), 0xB },
	{ CCI_REG8(0x403E), 0xE },
	{ CCI_REG8(0x402B), 0x6 },
	{ CCI_REG8(0x401E), 0x2 },
	{ CCI_REG8(0x4038), 0x3B },
	{ CCI_REG8(0x1077), 0x0 },
	{ CCI_REG8(0x1078), 0x0 },
	{ CCI_REG8(0x1009), 0x8 },
	{ CCI_REG8(0x100A), 0x0 },
	{ CCI_REG8(0x110F), 0x8 },
	{ CCI_REG8(0x1110), 0x0 },
	{ CCI_REG8(0x1006), 0x2 },
	{ CCI_REG8(0x402C), 0x64 },
	{ CCI_REG8(0x3064), 0x0 },
	{ CCI_REG8(0x3065), 0xF0 },
	{ CCI_REG8(0x4013), 0x13 },
	{ CCI_REG8(0x401F), 0x9 },
	{ CCI_REG8(0x4020), 0x13 },
	{ CCI_REG8(0x4044), 0x75 },
	{ CCI_REG8(0x4027), 0x0 },
	{ CCI_REG8(0x3215), 0x69 },
	{ CCI_REG8(0x3216), 0xF },
	{ CCI_REG8(0x322B), 0x69 },
	{ CCI_REG8(0x322C), 0xF },
	{ CCI_REG8(0x4051), 0x80 },
	{ CCI_REG8(0x4052), 0x10 },
	{ CCI_REG8(0x4057), 0x80 },
	{ CCI_REG8(0x4058), 0x10 },
	{ CCI_REG8(0x3212), 0x59 },
	{ CCI_REG8(0x4047), 0x8F },
	{ CCI_REG8(0x4026), 0x10 },
	{ CCI_REG8(0x4032), 0x53 },
	{ CCI_REG8(0x4036), 0x17 },
	{ CCI_REG8(0x50B8), 0xF4 },
	{ CCI_REG8(0x3016), 0x0 },
	{ CCI_REG8(0x3017), 0x2C },
	{ CCI_REG8(0x3018), 0x8C },
	{ CCI_REG8(0x3019), 0x45 },
	{ CCI_REG8(0x301A), 0x5 },
	{ CCI_REG8(0x3013), 0xA },
	{ CCI_REG8(0x301B), 0x0 },
	{ CCI_REG8(0x301C), 0x4 },
	{ CCI_REG8(0x301D), 0x88 },
	{ CCI_REG8(0x301E), 0x45 },
	{ CCI_REG8(0x301F), 0x5 },
	{ CCI_REG8(0x3020), 0x0 },
	{ CCI_REG8(0x3021), 0x4 },
	{ CCI_REG8(0x3022), 0x88 },
	{ CCI_REG8(0x3023), 0x45 },
	{ CCI_REG8(0x3024), 0x5 },
	{ CCI_REG8(0x3025), 0x0 },
	{ CCI_REG8(0x3026), 0x4 },
	{ CCI_REG8(0x3027), 0x88 },
	{ CCI_REG8(0x3028), 0x45 },
	{ CCI_REG8(0x3029), 0x5 },
	{ CCI_REG8(0x302F), 0x0 },
	{ CCI_REG8(0x3056), 0x0 },
	{ CCI_REG8(0x3057), 0x0 },
	{ CCI_REG8(0x3300), 0x1 },
	{ CCI_REG8(0x3301), 0x0 },
	{ CCI_REG8(0x3302), 0xB0 },
	{ CCI_REG8(0x3303), 0xB0 },
	{ CCI_REG8(0x3304), 0x16 },
	{ CCI_REG8(0x3305), 0x15 },
	{ CCI_REG8(0x3306), 0x1 },
	{ CCI_REG8(0x3307), 0x0 },
	{ CCI_REG8(0x3308), 0x30 },
	{ CCI_REG8(0x3309), 0xA0 },
	{ CCI_REG8(0x330A), 0x16 },
	{ CCI_REG8(0x330B), 0x15 },
	{ CCI_REG8(0x330C), 0x1 },
	{ CCI_REG8(0x330D), 0x0 },
	{ CCI_REG8(0x330E), 0x30 },
	{ CCI_REG8(0x330F), 0xA0 },
	{ CCI_REG8(0x3310), 0x16 },
	{ CCI_REG8(0x3311), 0x15 },
	{ CCI_REG8(0x3312), 0x1 },
	{ CCI_REG8(0x3313), 0x0 },
	{ CCI_REG8(0x3314), 0x30 },
	{ CCI_REG8(0x3315), 0xA0 },
	{ CCI_REG8(0x3316), 0x16 },
	{ CCI_REG8(0x3317), 0x15 },
	{ CCI_REG8(0x3318), 0x1 },
	{ CCI_REG8(0x3319), 0x0 },
	{ CCI_REG8(0x331A), 0x30 },
	{ CCI_REG8(0x331B), 0xA0 },
	{ CCI_REG8(0x331C), 0x16 },
	{ CCI_REG8(0x331D), 0x15 },
	{ CCI_REG8(0x331E), 0x1 },
	{ CCI_REG8(0x331F), 0x0 },
	{ CCI_REG8(0x3320), 0x30 },
	{ CCI_REG8(0x3321), 0xA0 },
	{ CCI_REG8(0x3322), 0x16 },
	{ CCI_REG8(0x3323), 0x15 },
	{ CCI_REG8(0x3324), 0x1 },
	{ CCI_REG8(0x3325), 0x0 },
	{ CCI_REG8(0x3326), 0x30 },
	{ CCI_REG8(0x3327), 0xA0 },
	{ CCI_REG8(0x3328), 0x16 },
	{ CCI_REG8(0x3329), 0x15 },
	{ CCI_REG8(0x332A), 0x2B },
	{ CCI_REG8(0x332B), 0x0 },
	{ CCI_REG8(0x332C), 0x30 },
	{ CCI_REG8(0x332D), 0xA0 },
	{ CCI_REG8(0x332E), 0x16 },
	{ CCI_REG8(0x332F), 0x15 },
	{ CCI_REG8(0x3330), 0x1 },
	{ CCI_REG8(0x3331), 0x0 },
	{ CCI_REG8(0x3332), 0x10 },
	{ CCI_REG8(0x3333), 0xA0 },
	{ CCI_REG8(0x3334), 0x16 },
	{ CCI_REG8(0x3335), 0x15 },
	{ CCI_REG8(0x3058), 0x8 },
	{ CCI_REG8(0x3059), 0x0 },
	{ CCI_REG8(0x305A), 0x9 },
	{ CCI_REG8(0x305B), 0x0 },
	{ CCI_REG8(0x3336), 0x1 },
	{ CCI_REG8(0x3337), 0x0 },
	{ CCI_REG8(0x3338), 0x90 },
	{ CCI_REG8(0x3339), 0xB0 },
	{ CCI_REG8(0x333A), 0x16 },
	{ CCI_REG8(0x333B), 0x15 },
	{ CCI_REG8(0x333C), 0x1F },
	{ CCI_REG8(0x333D), 0x0 },
	{ CCI_REG8(0x333E), 0x10 },
	{ CCI_REG8(0x333F), 0xA0 },
	{ CCI_REG8(0x3340), 0x16 },
	{ CCI_REG8(0x3341), 0x15 },
	{ CCI_REG8(0x3342), 0x52 },
	{ CCI_REG8(0x3343), 0x0 },
	{ CCI_REG8(0x3344), 0x10 },
	{ CCI_REG8(0x3345), 0x80 },
	{ CCI_REG8(0x3346), 0x16 },
	{ CCI_REG8(0x3347), 0x15 },
	{ CCI_REG8(0x3348), 0x1 },
	{ CCI_REG8(0x3349), 0x0 },
	{ CCI_REG8(0x334A), 0x10 },
	{ CCI_REG8(0x334B), 0x80 },
	{ CCI_REG8(0x334C), 0x16 },
	{ CCI_REG8(0x334D), 0x1D },
	{ CCI_REG8(0x334E), 0x1 },
	{ CCI_REG8(0x334F), 0x0 },
	{ CCI_REG8(0x3350), 0x50 },
	{ CCI_REG8(0x3351), 0x84 },
	{ CCI_REG8(0x3352), 0x16 },
	{ CCI_REG8(0x3353), 0x1D },
	{ CCI_REG8(0x3354), 0x18 },
	{ CCI_REG8(0x3355), 0x0 },
	{ CCI_REG8(0x3356), 0x10 },
	{ CCI_REG8(0x3357), 0x84 },
	{ CCI_REG8(0x3358), 0x16 },
	{ CCI_REG8(0x3359), 0x1D },
	{ CCI_REG8(0x335A), 0x80 },
	{ CCI_REG8(0x335B), 0x2 },
	{ CCI_REG8(0x335C), 0x10 },
	{ CCI_REG8(0x335D), 0xC4 },
	{ CCI_REG8(0x335E), 0x14 },
	{ CCI_REG8(0x335F), 0x1D },
	{ CCI_REG8(0x3360), 0xA5 },
	{ CCI_REG8(0x3361), 0x0 },
	{ CCI_REG8(0x3362), 0x10 },
	{ CCI_REG8(0x3363), 0x84 },
	{ CCI_REG8(0x3364), 0x16 },
	{ CCI_REG8(0x3365), 0x1D },
	{ CCI_REG8(0x3366), 0x1 },
	{ CCI_REG8(0x3367), 0x0 },
	{ CCI_REG8(0x3368), 0x90 },
	{ CCI_REG8(0x3369), 0x84 },
	{ CCI_REG8(0x336A), 0x16 },
	{ CCI_REG8(0x336B), 0x1D },
	{ CCI_REG8(0x336C), 0x12 },
	{ CCI_REG8(0x336D), 0x0 },
	{ CCI_REG8(0x336E), 0x10 },
	{ CCI_REG8(0x336F), 0x84 },
	{ CCI_REG8(0x3370), 0x16 },
	{ CCI_REG8(0x3371), 0x15 },
	{ CCI_REG8(0x3372), 0x32 },
	{ CCI_REG8(0x3373), 0x0 },
	{ CCI_REG8(0x3374), 0x30 },
	{ CCI_REG8(0x3375), 0x84 },
	{ CCI_REG8(0x3376), 0x16 },
	{ CCI_REG8(0x3377), 0x15 },
	{ CCI_REG8(0x3378), 0x26 },
	{ CCI_REG8(0x3379), 0x0 },
	{ CCI_REG8(0x337A), 0x10 },
	{ CCI_REG8(0x337B), 0x84 },
	{ CCI_REG8(0x337C), 0x16 },
	{ CCI_REG8(0x337D), 0x15 },
	{ CCI_REG8(0x337E), 0x80 },
	{ CCI_REG8(0x337F), 0x2 },
	{ CCI_REG8(0x3380), 0x10 },
	{ CCI_REG8(0x3381), 0xC4 },
	{ CCI_REG8(0x3382), 0x14 },
	{ CCI_REG8(0x3383), 0x15 },
	{ CCI_REG8(0x3384), 0xA9 },
	{ CCI_REG8(0x3385), 0x0 },
	{ CCI_REG8(0x3386), 0x10 },
	{ CCI_REG8(0x3387), 0x84 },
	{ CCI_REG8(0x3388), 0x16 },
	{ CCI_REG8(0x3389), 0x15 },
	{ CCI_REG8(0x338A), 0x41 },
	{ CCI_REG8(0x338B), 0x0 },
	{ CCI_REG8(0x338C), 0x10 },
	{ CCI_REG8(0x338D), 0x80 },
	{ CCI_REG8(0x338E), 0x16 },
	{ CCI_REG8(0x338F), 0x15 },
	{ CCI_REG8(0x3390), 0x2 },
	{ CCI_REG8(0x3391), 0x0 },
	{ CCI_REG8(0x3392), 0x10 },
	{ CCI_REG8(0x3393), 0xA0 },
	{ CCI_REG8(0x3394), 0x16 },
	{ CCI_REG8(0x3395), 0x15 },
	{ CCI_REG8(0x305C), 0x18 },
	{ CCI_REG8(0x305D), 0x0 },
	{ CCI_REG8(0x305E), 0x19 },
	{ CCI_REG8(0x305F), 0x0 },
	{ CCI_REG8(0x3396), 0x1 },
	{ CCI_REG8(0x3397), 0x0 },
	{ CCI_REG8(0x3398), 0x90 },
	{ CCI_REG8(0x3399), 0x30 },
	{ CCI_REG8(0x339A), 0x56 },
	{ CCI_REG8(0x339B), 0x57 },
	{ CCI_REG8(0x339C), 0x1 },
	{ CCI_REG8(0x339D), 0x0 },
	{ CCI_REG8(0x339E), 0x10 },
	{ CCI_REG8(0x339F), 0x20 },
	{ CCI_REG8(0x33A0), 0xD6 },
	{ CCI_REG8(0x33A1), 0x17 },
	{ CCI_REG8(0x33A2), 0x1 },
	{ CCI_REG8(0x33A3), 0x0 },
	{ CCI_REG8(0x33A4), 0x10 },
	{ CCI_REG8(0x33A5), 0x28 },
	{ CCI_REG8(0x33A6), 0xD6 },
	{ CCI_REG8(0x33A7), 0x17 },
	{ CCI_REG8(0x33A8), 0x3 },
	{ CCI_REG8(0x33A9), 0x0 },
	{ CCI_REG8(0x33AA), 0x10 },
	{ CCI_REG8(0x33AB), 0x20 },
	{ CCI_REG8(0x33AC), 0xD6 },
	{ CCI_REG8(0x33AD), 0x17 },
	{ CCI_REG8(0x33AE), 0x61 },
	{ CCI_REG8(0x33AF), 0x0 },
	{ CCI_REG8(0x33B0), 0x10 },
	{ CCI_REG8(0x33B1), 0x20 },
	{ CCI_REG8(0x33B2), 0xD6 },
	{ CCI_REG8(0x33B3), 0x15 },
	{ CCI_REG8(0x33B4), 0x1 },
	{ CCI_REG8(0x33B5), 0x0 },
	{ CCI_REG8(0x33B6), 0x10 },
	{ CCI_REG8(0x33B7), 0x20 },
	{ CCI_REG8(0x33B8), 0xD6 },
	{ CCI_REG8(0x33B9), 0x1D },
	{ CCI_REG8(0x33BA), 0x1 },
	{ CCI_REG8(0x33BB), 0x0 },
	{ CCI_REG8(0x33BC), 0x50 },
	{ CCI_REG8(0x33BD), 0x20 },
	{ CCI_REG8(0x33BE), 0xD6 },
	{ CCI_REG8(0x33BF), 0x1D },
	{ CCI_REG8(0x33C0), 0x2C },
	{ CCI_REG8(0x33C1), 0x0 },
	{ CCI_REG8(0x33C2), 0x10 },
	{ CCI_REG8(0x33C3), 0x20 },
	{ CCI_REG8(0x33C4), 0xD6 },
	{ CCI_REG8(0x33C5), 0x1D },
	{ CCI_REG8(0x33C6), 0x1 },
	{ CCI_REG8(0x33C7), 0x0 },
	{ CCI_REG8(0x33C8), 0x90 },
	{ CCI_REG8(0x33C9), 0x20 },
	{ CCI_REG8(0x33CA), 0xD6 },
	{ CCI_REG8(0x33CB), 0x1D },
	{ CCI_REG8(0x33CC), 0x83 },
	{ CCI_REG8(0x33CD), 0x0 },
	{ CCI_REG8(0x33CE), 0x10 },
	{ CCI_REG8(0x33CF), 0x20 },
	{ CCI_REG8(0x33D0), 0xD6 },
	{ CCI_REG8(0x33D1), 0x15 },
	{ CCI_REG8(0x33D2), 0x1 },
	{ CCI_REG8(0x33D3), 0x0 },
	{ CCI_REG8(0x33D4), 0x10 },
	{ CCI_REG8(0x33D5), 0x30 },
	{ CCI_REG8(0x33D6), 0xD6 },
	{ CCI_REG8(0x33D7), 0x15 },
	{ CCI_REG8(0x33D8), 0x1 },
	{ CCI_REG8(0x33D9), 0x0 },
	{ CCI_REG8(0x33DA), 0x10 },
	{ CCI_REG8(0x33DB), 0x20 },
	{ CCI_REG8(0x33DC), 0xD6 },
	{ CCI_REG8(0x33DD), 0x15 },
	{ CCI_REG8(0x33DE), 0x1 },
	{ CCI_REG8(0x33DF), 0x0 },
	{ CCI_REG8(0x33E0), 0x10 },
	{ CCI_REG8(0x33E1), 0x20 },
	{ CCI_REG8(0x33E2), 0x56 },
	{ CCI_REG8(0x33E3), 0x15 },
	{ CCI_REG8(0x33E4), 0x7 },
	{ CCI_REG8(0x33E5), 0x0 },
	{ CCI_REG8(0x33E6), 0x10 },
	{ CCI_REG8(0x33E7), 0x20 },
	{ CCI_REG8(0x33E8), 0x16 },
	{ CCI_REG8(0x33E9), 0x15 },
	{ CCI_REG8(0x3060), 0x26 },
	{ CCI_REG8(0x3061), 0x0 },
	{ CCI_REG8(0x302A), 0xFF },
	{ CCI_REG8(0x302B), 0xFF },
	{ CCI_REG8(0x302C), 0xFF },
	{ CCI_REG8(0x302D), 0xFF },
	{ CCI_REG8(0x302E), 0x3F },
	{ CCI_REG8(0x3013), 0xB },
	{ CCI_REG8(0x102B), 0x2C },
	{ CCI_REG8(0x102C), 0x1 },
	{ CCI_REG8(0x1035), 0x54 },
	{ CCI_REG8(0x1036), 0x0 },
	{ CCI_REG8(0x3090), 0x2A },
	{ CCI_REG8(0x3091), 0x1 },
	{ CCI_REG8(0x30C6), 0x5 },
	{ CCI_REG8(0x30C7), 0x0 },
	{ CCI_REG8(0x30C8), 0x0 },
	{ CCI_REG8(0x30C9), 0x0 },
	{ CCI_REG8(0x30CA), 0x0 },
	{ CCI_REG8(0x30CB), 0x0 },
	{ CCI_REG8(0x30CC), 0x0 },
	{ CCI_REG8(0x30CD), 0x0 },
	{ CCI_REG8(0x30CE), 0x0 },
	{ CCI_REG8(0x30CF), 0x5 },
	{ CCI_REG8(0x30D0), 0x0 },
	{ CCI_REG8(0x30D1), 0x0 },
	{ CCI_REG8(0x30D2), 0x0 },
	{ CCI_REG8(0x30D3), 0x0 },
	{ CCI_REG8(0x30D4), 0x0 },
	{ CCI_REG8(0x30D5), 0x0 },
	{ CCI_REG8(0x30D6), 0x0 },
	{ CCI_REG8(0x30D7), 0x0 },
	{ CCI_REG8(0x30F3), 0x5 },
	{ CCI_REG8(0x30F4), 0x0 },
	{ CCI_REG8(0x30F5), 0x0 },
	{ CCI_REG8(0x30F6), 0x0 },
	{ CCI_REG8(0x30F7), 0x0 },
	{ CCI_REG8(0x30F8), 0x0 },
	{ CCI_REG8(0x30F9), 0x0 },
	{ CCI_REG8(0x30FA), 0x0 },
	{ CCI_REG8(0x30FB), 0x0 },
	{ CCI_REG8(0x30D8), 0x5 },
	{ CCI_REG8(0x30D9), 0x0 },
	{ CCI_REG8(0x30DA), 0x0 },
	{ CCI_REG8(0x30DB), 0x0 },
	{ CCI_REG8(0x30DC), 0x0 },
	{ CCI_REG8(0x30DD), 0x0 },
	{ CCI_REG8(0x30DE), 0x0 },
	{ CCI_REG8(0x30DF), 0x0 },
	{ CCI_REG8(0x30E0), 0x0 },
	{ CCI_REG8(0x30E1), 0x5 },
	{ CCI_REG8(0x30E2), 0x0 },
	{ CCI_REG8(0x30E3), 0x0 },
	{ CCI_REG8(0x30E4), 0x0 },
	{ CCI_REG8(0x30E5), 0x0 },
	{ CCI_REG8(0x30E6), 0x0 },
	{ CCI_REG8(0x30E7), 0x0 },
	{ CCI_REG8(0x30E8), 0x0 },
	{ CCI_REG8(0x30E9), 0x0 },
	{ CCI_REG8(0x30F3), 0x5 },
	{ CCI_REG8(0x30F4), 0x2 },
	{ CCI_REG8(0x30F5), 0x0 },
	{ CCI_REG8(0x30F6), 0x17 },
	{ CCI_REG8(0x30F7), 0x1 },
	{ CCI_REG8(0x30F8), 0x0 },
	{ CCI_REG8(0x30F9), 0x0 },
	{ CCI_REG8(0x30FA), 0x0 },
	{ CCI_REG8(0x30FB), 0x0 },
	{ CCI_REG8(0x30D8), 0x3 },
	{ CCI_REG8(0x30D9), 0x1 },
	{ CCI_REG8(0x30DA), 0x0 },
	{ CCI_REG8(0x30DB), 0x19 },
	{ CCI_REG8(0x30DC), 0x1 },
	{ CCI_REG8(0x30DD), 0x0 },
	{ CCI_REG8(0x30DE), 0x0 },
	{ CCI_REG8(0x30DF), 0x0 },
	{ CCI_REG8(0x30E0), 0x0 },
	{ CCI_REG8(0x30A2), 0x5 },
	{ CCI_REG8(0x30A3), 0x2 },
	{ CCI_REG8(0x30A4), 0x0 },
	{ CCI_REG8(0x30A5), 0x22 },
	{ CCI_REG8(0x30A6), 0x0 },
	{ CCI_REG8(0x30A7), 0x0 },
	{ CCI_REG8(0x30A8), 0x0 },
	{ CCI_REG8(0x30A9), 0x0 },
	{ CCI_REG8(0x30AA), 0x0 },
	{ CCI_REG8(0x30AB), 0x5 },
	{ CCI_REG8(0x30AC), 0x2 },
	{ CCI_REG8(0x30AD), 0x0 },
	{ CCI_REG8(0x30AE), 0x22 },
	{ CCI_REG8(0x30AF), 0x0 },
	{ CCI_REG8(0x30B0), 0x0 },
	{ CCI_REG8(0x30B1), 0x0 },
	{ CCI_REG8(0x30B2), 0x0 },
	{ CCI_REG8(0x30B3), 0x0 },
	{ CCI_REG8(0x30BD), 0x5 },
	{ CCI_REG8(0x30BE), 0x9F },
	{ CCI_REG8(0x30BF), 0x0 },
	{ CCI_REG8(0x30C0), 0x7D },
	{ CCI_REG8(0x30C1), 0x0 },
	{ CCI_REG8(0x30C2), 0x0 },
	{ CCI_REG8(0x30C3), 0x0 },
	{ CCI_REG8(0x30C4), 0x0 },
	{ CCI_REG8(0x30C5), 0x0 },
	{ CCI_REG8(0x30B4), 0x4 },
	{ CCI_REG8(0x30B5), 0x9C },
	{ CCI_REG8(0x30B6), 0x0 },
	{ CCI_REG8(0x30B7), 0x7D },
	{ CCI_REG8(0x30B8), 0x0 },
	{ CCI_REG8(0x30B9), 0x0 },
	{ CCI_REG8(0x30BA), 0x0 },
	{ CCI_REG8(0x30BB), 0x0 },
	{ CCI_REG8(0x30BC), 0x0 },
	{ CCI_REG8(0x30FC), 0x5 },
	{ CCI_REG8(0x30FD), 0x0 },
	{ CCI_REG8(0x30FE), 0x0 },
	{ CCI_REG8(0x30FF), 0x0 },
	{ CCI_REG8(0x3100), 0x0 },
	{ CCI_REG8(0x3101), 0x0 },
	{ CCI_REG8(0x3102), 0x0 },
	{ CCI_REG8(0x3103), 0x0 },
	{ CCI_REG8(0x3104), 0x0 },
	{ CCI_REG8(0x3105), 0x5 },
	{ CCI_REG8(0x3106), 0x0 },
	{ CCI_REG8(0x3107), 0x0 },
	{ CCI_REG8(0x3108), 0x0 },
	{ CCI_REG8(0x3109), 0x0 },
	{ CCI_REG8(0x310A), 0x0 },
	{ CCI_REG8(0x310B), 0x0 },
	{ CCI_REG8(0x310C), 0x0 },
	{ CCI_REG8(0x310D), 0x0 },
	{ CCI_REG8(0x3099), 0x5 },
	{ CCI_REG8(0x309A), 0x96 },
	{ CCI_REG8(0x309B), 0x0 },
	{ CCI_REG8(0x309C), 0x6 },
	{ CCI_REG8(0x309D), 0x0 },
	{ CCI_REG8(0x309E), 0x0 },
	{ CCI_REG8(0x309F), 0x0 },
	{ CCI_REG8(0x30A0), 0x0 },
	{ CCI_REG8(0x30A1), 0x0 },
	{ CCI_REG8(0x310E), 0x5 },
	{ CCI_REG8(0x310F), 0x2 },
	{ CCI_REG8(0x3110), 0x0 },
	{ CCI_REG8(0x3111), 0x2B },
	{ CCI_REG8(0x3112), 0x0 },
	{ CCI_REG8(0x3113), 0x0 },
	{ CCI_REG8(0x3114), 0x0 },
	{ CCI_REG8(0x3115), 0x0 },
	{ CCI_REG8(0x3116), 0x0 },
	{ CCI_REG8(0x3117), 0x5 },
	{ CCI_REG8(0x3118), 0x2 },
	{ CCI_REG8(0x3119), 0x0 },
	{ CCI_REG8(0x311A), 0x2C },
	{ CCI_REG8(0x311B), 0x0 },
	{ CCI_REG8(0x311C), 0x0 },
	{ CCI_REG8(0x311D), 0x0 },
	{ CCI_REG8(0x311E), 0x0 },
	{ CCI_REG8(0x311F), 0x0 },
	{ CCI_REG8(0x30EA), 0x0 },
	{ CCI_REG8(0x30EB), 0x0 },
	{ CCI_REG8(0x30EC), 0x0 },
	{ CCI_REG8(0x30ED), 0x0 },
	{ CCI_REG8(0x30EE), 0x0 },
	{ CCI_REG8(0x30EF), 0x0 },
	{ CCI_REG8(0x30F0), 0x0 },
	{ CCI_REG8(0x30F1), 0x0 },
	{ CCI_REG8(0x30F2), 0x0 },
	{ CCI_REG8(0x313B), 0x3 },
	{ CCI_REG8(0x313C), 0x31 },
	{ CCI_REG8(0x313D), 0x0 },
	{ CCI_REG8(0x313E), 0x7 },
	{ CCI_REG8(0x313F), 0x0 },
	{ CCI_REG8(0x3140), 0x68 },
	{ CCI_REG8(0x3141), 0x0 },
	{ CCI_REG8(0x3142), 0x34 },
	{ CCI_REG8(0x3143), 0x0 },
	{ CCI_REG8(0x31A0), 0x3 },
	{ CCI_REG8(0x31A1), 0x16 },
	{ CCI_REG8(0x31A2), 0x0 },
	{ CCI_REG8(0x31A3), 0x8 },
	{ CCI_REG8(0x31A4), 0x0 },
	{ CCI_REG8(0x31A5), 0x7E },
	{ CCI_REG8(0x31A6), 0x0 },
	{ CCI_REG8(0x31A7), 0x8 },
	{ CCI_REG8(0x31A8), 0x0 },
	{ CCI_REG8(0x31A9), 0x3 },
	{ CCI_REG8(0x31AA), 0x16 },
	{ CCI_REG8(0x31AB), 0x0 },
	{ CCI_REG8(0x31AC), 0x8 },
	{ CCI_REG8(0x31AD), 0x0 },
	{ CCI_REG8(0x31AE), 0x7E },
	{ CCI_REG8(0x31AF), 0x0 },
	{ CCI_REG8(0x31B0), 0x8 },
	{ CCI_REG8(0x31B1), 0x0 },
	{ CCI_REG8(0x31B2), 0x3 },
	{ CCI_REG8(0x31B3), 0x16 },
	{ CCI_REG8(0x31B4), 0x0 },
	{ CCI_REG8(0x31B5), 0x8 },
	{ CCI_REG8(0x31B6), 0x0 },
	{ CCI_REG8(0x31B7), 0x7E },
	{ CCI_REG8(0x31B8), 0x0 },
	{ CCI_REG8(0x31B9), 0x8 },
	{ CCI_REG8(0x31BA), 0x0 },
	{ CCI_REG8(0x3120), 0x5 },
	{ CCI_REG8(0x3121), 0x45 },
	{ CCI_REG8(0x3122), 0x0 },
	{ CCI_REG8(0x3123), 0x1D },
	{ CCI_REG8(0x3124), 0x0 },
	{ CCI_REG8(0x3125), 0xA9 },
	{ CCI_REG8(0x3126), 0x0 },
	{ CCI_REG8(0x3127), 0x6D },
	{ CCI_REG8(0x3128), 0x0 },
	{ CCI_REG8(0x3129), 0x5 },
	{ CCI_REG8(0x312A), 0x15 },
	{ CCI_REG8(0x312B), 0x0 },
	{ CCI_REG8(0x312C), 0xA },
	{ CCI_REG8(0x312D), 0x0 },
	{ CCI_REG8(0x312E), 0x45 },
	{ CCI_REG8(0x312F), 0x0 },
	{ CCI_REG8(0x3130), 0x1D },
	{ CCI_REG8(0x3131), 0x0 },
	{ CCI_REG8(0x3132), 0x5 },
	{ CCI_REG8(0x3133), 0x7D },
	{ CCI_REG8(0x3134), 0x0 },
	{ CCI_REG8(0x3135), 0xA },
	{ CCI_REG8(0x3136), 0x0 },
	{ CCI_REG8(0x3137), 0xA9 },
	{ CCI_REG8(0x3138), 0x0 },
	{ CCI_REG8(0x3139), 0x6D },
	{ CCI_REG8(0x313A), 0x0 },
	{ CCI_REG8(0x3144), 0x5 },
	{ CCI_REG8(0x3145), 0x0 },
	{ CCI_REG8(0x3146), 0x0 },
	{ CCI_REG8(0x3147), 0x30 },
	{ CCI_REG8(0x3148), 0x0 },
	{ CCI_REG8(0x3149), 0x0 },
	{ CCI_REG8(0x314A), 0x0 },
	{ CCI_REG8(0x314B), 0x0 },
	{ CCI_REG8(0x314C), 0x0 },
	{ CCI_REG8(0x314D), 0x3 },
	{ CCI_REG8(0x314E), 0x0 },
	{ CCI_REG8(0x314F), 0x0 },
	{ CCI_REG8(0x3150), 0x31 },
	{ CCI_REG8(0x3151), 0x0 },
	{ CCI_REG8(0x3152), 0x0 },
	{ CCI_REG8(0x3153), 0x0 },
	{ CCI_REG8(0x3154), 0x0 },
	{ CCI_REG8(0x3155), 0x0 },
	{ CCI_REG8(0x31D8), 0x5 },
	{ CCI_REG8(0x31D9), 0x3A },
	{ CCI_REG8(0x31DA), 0x0 },
	{ CCI_REG8(0x31DB), 0x2E },
	{ CCI_REG8(0x31DC), 0x0 },
	{ CCI_REG8(0x31DD), 0x9E },
	{ CCI_REG8(0x31DE), 0x0 },
	{ CCI_REG8(0x31DF), 0x7E },
	{ CCI_REG8(0x31E0), 0x0 },
	{ CCI_REG8(0x31E1), 0x5 },
	{ CCI_REG8(0x31E2), 0x4 },
	{ CCI_REG8(0x31E3), 0x0 },
	{ CCI_REG8(0x31E4), 0x4 },
	{ CCI_REG8(0x31E5), 0x0 },
	{ CCI_REG8(0x31E6), 0x73 },
	{ CCI_REG8(0x31E7), 0x0 },
	{ CCI_REG8(0x31E8), 0x4 },
	{ CCI_REG8(0x31E9), 0x0 },
	{ CCI_REG8(0x31EA), 0x5 },
	{ CCI_REG8(0x31EB), 0x0 },
	{ CCI_REG8(0x31EC), 0x0 },
	{ CCI_REG8(0x31ED), 0x0 },
	{ CCI_REG8(0x31EE), 0x0 },
	{ CCI_REG8(0x31EF), 0x0 },
	{ CCI_REG8(0x31F0), 0x0 },
	{ CCI_REG8(0x31F1), 0x0 },
	{ CCI_REG8(0x31F2), 0x0 },
	{ CCI_REG8(0x31F3), 0x0 },
	{ CCI_REG8(0x31F4), 0x0 },
	{ CCI_REG8(0x31F5), 0x0 },
	{ CCI_REG8(0x31F6), 0x0 },
	{ CCI_REG8(0x31F7), 0x0 },
	{ CCI_REG8(0x31F8), 0x0 },
	{ CCI_REG8(0x31F9), 0x0 },
	{ CCI_REG8(0x31FA), 0x0 },
	{ CCI_REG8(0x31FB), 0x5 },
	{ CCI_REG8(0x31FC), 0x0 },
	{ CCI_REG8(0x31FD), 0x0 },
	{ CCI_REG8(0x31FE), 0x0 },
	{ CCI_REG8(0x31FF), 0x0 },
	{ CCI_REG8(0x3200), 0x0 },
	{ CCI_REG8(0x3201), 0x0 },
	{ CCI_REG8(0x3202), 0x0 },
	{ CCI_REG8(0x3203), 0x0 },
	{ CCI_REG8(0x3204), 0x0 },
	{ CCI_REG8(0x3205), 0x0 },
	{ CCI_REG8(0x3206), 0x0 },
	{ CCI_REG8(0x3207), 0x0 },
	{ CCI_REG8(0x3208), 0x0 },
	{ CCI_REG8(0x3209), 0x0 },
	{ CCI_REG8(0x320A), 0x0 },
	{ CCI_REG8(0x320B), 0x0 },
	{ CCI_REG8(0x3164), 0x5 },
	{ CCI_REG8(0x3165), 0x14 },
	{ CCI_REG8(0x3166), 0x0 },
	{ CCI_REG8(0x3167), 0xC },
	{ CCI_REG8(0x3168), 0x0 },
	{ CCI_REG8(0x3169), 0x44 },
	{ CCI_REG8(0x316A), 0x0 },
	{ CCI_REG8(0x316B), 0x1F },
	{ CCI_REG8(0x316C), 0x0 },
	{ CCI_REG8(0x316D), 0x5 },
	{ CCI_REG8(0x316E), 0x7C },
	{ CCI_REG8(0x316F), 0x0 },
	{ CCI_REG8(0x3170), 0xC },
	{ CCI_REG8(0x3171), 0x0 },
	{ CCI_REG8(0x3172), 0xA8 },
	{ CCI_REG8(0x3173), 0x0 },
	{ CCI_REG8(0x3174), 0x6F },
	{ CCI_REG8(0x3175), 0x0 },
	{ CCI_REG8(0x31C4), 0x5 },
	{ CCI_REG8(0x31C5), 0x24 },
	{ CCI_REG8(0x31C6), 0x1 },
	{ CCI_REG8(0x31C7), 0x4 },
	{ CCI_REG8(0x31C8), 0x0 },
	{ CCI_REG8(0x31C9), 0x5 },
	{ CCI_REG8(0x31CA), 0x24 },
	{ CCI_REG8(0x31CB), 0x1 },
	{ CCI_REG8(0x31CC), 0x4 },
	{ CCI_REG8(0x31CD), 0x0 },
	{ CCI_REG8(0x31CE), 0x5 },
	{ CCI_REG8(0x31CF), 0x24 },
	{ CCI_REG8(0x31D0), 0x1 },
	{ CCI_REG8(0x31D1), 0x4 },
	{ CCI_REG8(0x31D2), 0x0 },
	{ CCI_REG8(0x31D3), 0x5 },
	{ CCI_REG8(0x31D4), 0x73 },
	{ CCI_REG8(0x31D5), 0x0 },
	{ CCI_REG8(0x31D6), 0xB1 },
	{ CCI_REG8(0x31D7), 0x0 },
	{ CCI_REG8(0x3176), 0x5 },
	{ CCI_REG8(0x3177), 0x10 },
	{ CCI_REG8(0x3178), 0x0 },
	{ CCI_REG8(0x3179), 0x56 },
	{ CCI_REG8(0x317A), 0x0 },
	{ CCI_REG8(0x317B), 0x0 },
	{ CCI_REG8(0x317C), 0x0 },
	{ CCI_REG8(0x317D), 0x0 },
	{ CCI_REG8(0x317E), 0x0 },
	{ CCI_REG8(0x317F), 0x5 },
	{ CCI_REG8(0x3180), 0x6A },
	{ CCI_REG8(0x3181), 0x0 },
	{ CCI_REG8(0x3182), 0xAD },
	{ CCI_REG8(0x3183), 0x0 },
	{ CCI_REG8(0x3184), 0x0 },
	{ CCI_REG8(0x3185), 0x0 },
	{ CCI_REG8(0x3186), 0x0 },
	{ CCI_REG8(0x3187), 0x0 },
	{ CCI_REG8(0x100D), 0x0 },
	{ CCI_REG8(0x1013), 0x2B },
	/* Sensor control mode */
	{ CCI_REG8(0x0043), 0x0 }, //  Sensor Control Mode.SLEEP_POWER_MODE(0)
	{ CCI_REG8(0x0043), 0x0 }, //  Sensor Control Mode.IDLE_POWER_MODE(0)
	{ CCI_REG8(0x0043), 0x4 }, //  Sensor Control Mode.SYSTEM_CLOCK_ENABLE(0)
	{ CCI_REG8(0x0043), 0xC }, //  Sensor Control Mode.SRAM_CLOCK_ENABLE(0)
	{ CCI_REG8(0x1001), 0x41 }, //  Sensor Control Mode.EXT_EVENT_SEL(0)
	{ CCI_REG8(0x10F2), 0x1 }, //  Sensor Control Mode.NB_OF_FRAMES_A(0)
	{ CCI_REG8(0x10F3), 0x0 }, //  Sensor Control Mode.NB_OF_FRAMES_A(1)
	{ CCI_REG8(0x0012), 0x0 }, //  IO Drive Strength.DIG_DRIVE_STRENGTH(0)
	{ CCI_REG8(0x0012), 0x0 }, //  IO Drive Strength.CCI_DRIVE_STRENGTH(0)
	{ CCI_REG8(0x1001), 0x41 }, //  Readout && Exposure.EXT_EXP_PW_SEL(0)
	{ CCI_REG8(0x10D0), 0x0 }, //  Readout && Exposure.EXT_EXP_PW_DELAY(0)
	{ CCI_REG8(0x10D1), 0x0 }, //  Readout && Exposure.EXT_EXP_PW_DELAY(1)
	/* MIPI */
	{ CCI_REG8(0x6006), 0x0 }, //  MIPI.TX_CTRL_EN(0)
	{ CCI_REG8(0x5004), 0x1 }, //  MIPI.datarate
	{ CCI_REG8(0x5086), 0x2 }, //  MIPI.datarate
	{ CCI_REG8(0x5087), 0x4E }, //  MIPI.datarate
	{ CCI_REG8(0x5088), 0x0 }, //  MIPI.datarate
	{ CCI_REG8(0x5090), 0x0 }, //  MIPI.datarate
	{ CCI_REG8(0x5091), 0x8 }, //  MIPI.datarate
	{ CCI_REG8(0x5092), 0x14 }, //  MIPI.datarate
	{ CCI_REG8(0x5093), 0xF }, //  MIPI.datarate
	{ CCI_REG8(0x5094), 0x6 }, //  MIPI.datarate
	{ CCI_REG8(0x5095), 0x32 }, //  MIPI.datarate
	{ CCI_REG8(0x5096), 0xE }, //  MIPI.datarate
	{ CCI_REG8(0x5097), 0x0 }, //  MIPI.datarate
	{ CCI_REG8(0x5098), 0x11 }, //  MIPI.datarate
	{ CCI_REG8(0x5004), 0x0 }, //  MIPI.datarate
	{ CCI_REG8(0x2066), 0x6C }, //  MIPI.datarate
	{ CCI_REG8(0x2067), 0x7 }, //  MIPI.datarate
	{ CCI_REG8(0x206E), 0x7E }, //  MIPI.datarate
	{ CCI_REG8(0x206F), 0x6 }, //  MIPI.datarate
	{ CCI_REG8(0x20AC), 0x7E }, //  MIPI.datarate
	{ CCI_REG8(0x20AD), 0x6 }, //  MIPI.datarate
	{ CCI_REG8(0x2076), 0xC8 }, //  MIPI.datarate
	{ CCI_REG8(0x2077), 0x0 }, //  MIPI.datarate
	{ CCI_REG8(0x20B4), 0xC8 }, //  MIPI.datarate
	{ CCI_REG8(0x20B5), 0x0 }, //  MIPI.datarate
	{ CCI_REG8(0x2078), 0x1E }, //  MIPI.datarate
	{ CCI_REG8(0x2079), 0x4 }, //  MIPI.datarate
	{ CCI_REG8(0x20B6), 0x1E }, //  MIPI.datarate
	{ CCI_REG8(0x20B7), 0x4 }, //  MIPI.datarate
	{ CCI_REG8(0x207A), 0xD4 }, //  MIPI.datarate
	{ CCI_REG8(0x207B), 0x4 }, //  MIPI.datarate
	{ CCI_REG8(0x20B8), 0xD4 }, //  MIPI.datarate
	{ CCI_REG8(0x20B9), 0x4 }, //  MIPI.datarate
	{ CCI_REG8(0x207C), 0x0 }, //  MIPI.VC_ID(0)
	{ CCI_REG8(0x6001), 0x7 }, //  MIPI.TINIT(0)
	{ CCI_REG8(0x6002), 0xD8 }, //  MIPI.TINIT(1)
	{ CCI_REG8(0x6010), 0x0 }, //  MIPI.FRAME_MODE(0)
	{ CCI_REG8(0x6010), 0x0 }, //  MIPI.EMBEDDED_FRAME_MODE(0)
	{ CCI_REG8(0x6011), 0x0 }, //  MIPI.DATA_ENABLE_POLARITY(0)
	{ CCI_REG8(0x6011), 0x0 }, //  MIPI.HSYNC_POLARITY(0)
	{ CCI_REG8(0x6011), 0x0 }, //  MIPI.VSYNC_POLARITY(0)
	{ CCI_REG8(0x6016), 0x0 }, //  MIPI.FRAME_COUNTER(0)
	{ CCI_REG8(0x6017), 0x0 }, //  MIPI.FRAME_COUNTER(1)
	{ CCI_REG8(0x6037), 0x1 }, //  MIPI.LINE_COUNT_RAW8(0)
	{ CCI_REG8(0x6037), 0x3 }, //  MIPI.LINE_COUNT_RAW10(0)
	{ CCI_REG8(0x6037), 0x7 }, //  MIPI.LINE_COUNT_RAW12(0)
	{ CCI_REG8(0x6039), 0x1 }, //  MIPI.LINE_COUNT_EMB(0)
	{ CCI_REG8(0x6018), 0x0 }, //  MIPI.CCI_READ_INTERRUPT_EN(0)
	{ CCI_REG8(0x6018), 0x0 }, //  MIPI.CCI_WRITE_INTERRUPT_EN(0)
	{ CCI_REG8(0x6065), 0x0 }, //  MIPI.TWAKE_TIMER(0)
	{ CCI_REG8(0x6066), 0x0 }, //  MIPI.TWAKE_TIMER(1)
	{ CCI_REG8(0x601C), 0x0 }, //  MIPI.SKEW_CAL_EN(0)
	{ CCI_REG8(0x601D), 0x0 }, //  MIPI.SKEW_COUNT(0)
	{ CCI_REG8(0x601E), 0x22 }, //  MIPI.SKEW_COUNT(1)
	{ CCI_REG8(0x601F), 0x0 }, //  MIPI.SCRAMBLING_EN(0)
	{ CCI_REG8(0x6003), 0x1 }, //  MIPI.INIT_SKEW_EN(0)
	{ CCI_REG8(0x6004), 0x7A }, //  MIPI.INIT_SKEW(0)
	{ CCI_REG8(0x6005), 0x12 }, //  MIPI.INIT_SKEW(1)
	{ CCI_REG8(0x6006), 0x1 }, //  MIPI.TX_CTRL_EN(0)
	/* Processing */
	{ CCI_REG8(0x4006), 0x8 }, //  Processing.BSP(0)
	{ CCI_REG8(0x2045), 0x1 }, //  Processing.CDS_RNC(0)
	{ CCI_REG8(0x2048), 0x1 }, //  Processing.CDS_IMG(0)
	{ CCI_REG8(0x204B), 0x3 }, //  Processing.RNC_EN(0)
	{ CCI_REG8(0x205B), 0x64 }, //  Processing.RNC_DARK_TARGET(0)
	{ CCI_REG8(0x205C), 0x0 }, //  Processing.RNC_DARK_TARGET(1)
	{ CCI_REG8(0x24DC), 0x12 }, //  Defect Pixel Correction.DC_ENABLE(0)
	{ CCI_REG8(0x24DC), 0x10 }, //  Defect Pixel Correction.DC_MODE(0)
	{ CCI_REG8(0x24DC),	0x0 }, //  Defect Pixel Correction.DC_REPLACEMENT_VALUE(0)
	{ CCI_REG8(0x24DD), 0x0 }, //  Defect Pixel Correction.DC_LIMIT_LOW(0)
	{ CCI_REG8(0x24DE), 0x0 }, //  Defect Pixel Correction.DC_LIMIT_HIGH(0)
	{ CCI_REG8(0x24DF),	0x0 }, //  Defect Pixel Correction.DC_LIMIT_HIGH_MODE(0)
	/* Illumination */
	{ CCI_REG8(0x10D7), 0x1 }, //  Illumination Trigger.ILLUM_EN(0)
	{ CCI_REG8(0x10D8), 0x2 }, //  Illumination Trigger.ILLUM_POL(0)
	/* Histogram */
	{ CCI_REG8(0x205D), 0x0 }, //  Histogram.HIST_EN(0)
	{ CCI_REG8(0x205E), 0x0 }, //  Histogram.HIST_USAGE_RATIO(0)
	{ CCI_REG8(0x2063), 0x0 }, //  Histogram.PIXEL_DATA_SUPP(0)
	{ CCI_REG8(0x2063), 0x0 }, //  Histogram.PIXEL_TRANSMISSION(0)
	/* TP */
	{ CCI_REG8(0x2091), 0x0 }, //  Test Pattern Generator.TPG_EN(0)
	{ CCI_REG8(0x2091), 0x0 }, //  Test Pattern Generator.TPG_CONFIG(0)
	/* Reduce Slew Rate - fix for defect line */
	{ CCI_REG8(0x402D), 0x7B },

};

static const char *const mira220_test_pattern_menu[] = {
	"Disabled",
	"Vertical Gradient",
};

static const int mira220_test_pattern_val[] = {
	MIRA220_TEST_PATTERN_DISABLE,
	MIRA220_TEST_PATTERN_VERTICAL_GRADIENT,
};

/* regulator supplies */
static const char *const mira220_supply_name[] = {
	/* Supplies can be enabled in any order */
	"vana", /* Analog (2.8V) supply */
	"vdig", /* Digital Core (1.8V) supply */
	"vddl", /* IF (1.2V) supply */
};

#define MIRA220_NUM_SUPPLIES ARRAY_SIZE(mira220_supply_name)

/*
 * The supported formats.
 * This table MUST contain 4 entries per format, to cover the various flip
 * combinations in the order
 * - no flip
 * - h flip
 * - v flip
 * - h&v flips
 */
static const u32 mira220_mbus_color_formats[] = {
	MEDIA_BUS_FMT_SRGGB12_1X12,
	MEDIA_BUS_FMT_SGRBG12_1X12,
	MEDIA_BUS_FMT_SGBRG12_1X12,
	MEDIA_BUS_FMT_SBGGR12_1X12,

	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,

	MEDIA_BUS_FMT_SRGGB8_1X8,
	MEDIA_BUS_FMT_SGRBG8_1X8,
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SBGGR8_1X8,
};

static const u32 mira220_mbus_mono_formats[] = {
	MEDIA_BUS_FMT_Y12_1X12,
	MEDIA_BUS_FMT_Y10_1X10,
	MEDIA_BUS_FMT_Y8_1X8,
};

struct mira220 {
	struct v4l2_subdev sd;
	struct media_pad pad;

	enum mira220_variants variant;

	struct v4l2_mbus_framefmt fmt;

	struct clk *xclk;
	u32 xclk_freq;

	struct regulator_bulk_data supplies[MIRA220_NUM_SUPPLIES];

	struct gpio_desc *reset_gpio;

	unsigned int lanes;
	unsigned int row_length;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;

	const struct mira220_mode *mode;

	struct regmap *regmap;
};

static inline struct mira220 *to_mira220(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mira220, sd);
}

static bool mira220_is_mono(struct mira220 *mira220)
{
	return mira220->variant == MIRA220_VARIANT_MONO;
}

/* Power/clock management functions */
static int mira220_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mira220 *mira220 = to_mira220(sd);
	int ret;

	ret = regulator_bulk_enable(MIRA220_NUM_SUPPLIES, mira220->supplies);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	ret = clk_prepare_enable(mira220->xclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n", __func__);
		goto reg_off;
	}

	gpiod_set_value_cansleep(mira220->reset_gpio, 0);

	return 0;

reg_off:
	return regulator_bulk_disable(MIRA220_NUM_SUPPLIES, mira220->supplies);
}

static int mira220_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mira220 *mira220 = to_mira220(sd);

	gpiod_set_value_cansleep(mira220->reset_gpio, 1);
	clk_disable_unprepare(mira220->xclk);
	regulator_bulk_disable(MIRA220_NUM_SUPPLIES, mira220->supplies);

	return 0;
}

static int mira220_write_start_streaming_regs(struct mira220 *mira220)
{
	int ret;

	/* Setting master control */
	ret = cci_write(mira220->regmap, MIRA220_IMAGER_STATE_REG,
			MIRA220_IMAGER_STATE_MASTER_CONTROL, NULL);
	if (ret)
		return ret;

	/* Enable continuous streaming */
	ret = cci_write(mira220->regmap, MIRA220_IMAGER_RUN_CONT_REG,
			MIRA220_IMAGER_RUN_CONT_ENABLE, NULL);
	if (ret)
		return ret;

	ret = cci_write(mira220->regmap, MIRA220_IMAGER_RUN_REG,
			MIRA220_IMAGER_RUN_START, NULL);
	if (ret)
		return ret;

	return ret;
}

static int mira220_write_stop_streaming_regs(struct mira220 *mira220)
{
	int ret;

	ret = cci_write(mira220->regmap, MIRA220_IMAGER_STATE_REG,
			MIRA220_IMAGER_STATE_STOP_AT_ROW, NULL);
	if (ret)
		return ret;

	ret = cci_write(mira220->regmap, MIRA220_IMAGER_RUN_REG,
			MIRA220_IMAGER_RUN_STOP, NULL);
	if (ret)
		return ret;

	return ret;
}

/*
 * Returns the exposure in lines. Calculation is baded on Mira220 datasheet
 * Section 9.2.
 */
static u32 mira220_calc_exposure(struct mira220 *mira220, u32 height, u32 vblank)
{
	u32 max_exposure = (height + vblank) -
			   (MIRA220_GLOB_NUM_CLK_CYCLES / mira220->row_length);

	return min(max_exposure, MIRA220_MAX_EXPOSURE);
}

static u32 mira220_calc_min_vblank(struct mira220 *mira220)
{
	return (MIRA220_GLOB_NUM_CLK_CYCLES / mira220->row_length)
	       + MIRA220_MIN_VBLANK_MARGIN;
}

static int mira220_write_exposure_reg(struct mira220 *mira220, u32 exposure)
{
	struct i2c_client *const client = v4l2_get_subdevdata(&mira220->sd);
	int ret;

	ret = cci_write(mira220->regmap, MIRA220_EXP_TIME_REG, exposure, NULL);
	if (ret) {
		dev_err_ratelimited(&client->dev,
				    "Error setting exposure time to %d",
				    exposure);
		return ret;
	}

	return 0;
}

/* Get bayer order based on flip setting. */
static u32 mira220_get_format_code(struct mira220 *mira220, u32 code)
{
	unsigned int i;

	if (mira220_is_mono(mira220)) {
		/* for now only support Y10. */
		for (i = 0; i < ARRAY_SIZE(mira220_mbus_mono_formats); i++) {
			if (mira220_mbus_mono_formats[i] == code)
				return mira220_mbus_mono_formats[i];
		}

		return mira220_mbus_mono_formats[0];
	}

	for (i = 0; i < ARRAY_SIZE(mira220_mbus_color_formats); i++) {
		if (mira220_mbus_color_formats[i] == code)
			break;
	}
	if (i >= ARRAY_SIZE(mira220_mbus_color_formats))
		i = 0;

	i = (i & ~3) | (mira220->vflip->val ? 2 : 0) | (mira220->hflip->val ? 0 : 1);

	return mira220_mbus_color_formats[i];
}

static int mira220_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mira220 *mira220 =
		container_of(ctrl->handler, struct mira220, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&mira220->sd);
	struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&mira220->sd);
	format = v4l2_subdev_state_get_format(state, 0);

	if (ctrl->id == V4L2_CID_VBLANK) {
		int exposure_max, exposure_def;

		/* Update max exposure while meeting expected vblanking */
		exposure_max = mira220_calc_exposure(mira220, format->height,
						     ctrl->val);
		exposure_def = min(exposure_max, MIRA220_DEFAULT_EXPOSURE);
		ret = __v4l2_ctrl_modify_range(mira220->exposure,
					       mira220->exposure->minimum,
					       exposure_max,
					       mira220->exposure->step,
					       exposure_def);
		if (ret)
			return ret;
	}

	/* Update the format code to adjust it to the new flip state. */
	if (ctrl->id == V4L2_CID_VFLIP || ctrl->id == V4L2_CID_HFLIP)
		format->code = mira220_get_format_code(mira220, format->code);

	if (pm_runtime_get_if_in_use(&client->dev) <= 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = mira220_write_exposure_reg(mira220, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(mira220->regmap, MIRA220_REG_TEST_PATTERN,
				mira220_test_pattern_val[ctrl->val], NULL);
		break;
	case V4L2_CID_HFLIP:
		ret = cci_write(mira220->regmap, MIRA220_HFLIP_REG,
				mira220->hflip->val, NULL);
		break;
	case V4L2_CID_VFLIP:
		ret = cci_write(mira220->regmap, MIRA220_VFLIP_REG,
				mira220->vflip->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(mira220->regmap, MIRA220_VBLANK_REG, ctrl->val,
				NULL);
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n", ctrl->id,
			 ctrl->val);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops mira220_ctrl_ops = {
	.s_ctrl = mira220_set_ctrl,
};

static void mira220_update_pad_format(struct mira220 *mira220,
				      struct v4l2_mbus_framefmt *fmt, u32 code)
{
	/* Bayer order varies with flips */
	fmt->code = mira220_get_format_code(mira220, code);
	/*
	 * The mira220 sensor doesn't support binning/skipping but only
	 * cropping to reduce the frame size so the correct API to configure
	 * windowing is the .set_selection one, while format is fixed to
	 * the full resolution 1600x1400 one.
	 */
	fmt->width = MIRA220_PIXEL_ARRAY_WIDTH;
	fmt->height = MIRA220_PIXEL_ARRAY_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int mira220_set_pad_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_format *fmt)
{
	struct mira220 *mira220 = to_mira220(sd);
	struct v4l2_rect *crop;
	u32 min_vblank;
	int ret;

	mira220_update_pad_format(mira220, &fmt->format, fmt->format.code);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	crop = v4l2_subdev_state_get_crop(state, 0);
	crop->width = fmt->format.width * 1;
	crop->height = fmt->format.height * 1;
	crop->left = MIRA220_PIXEL_ARRAY_LEFT;
	crop->top = MIRA220_PIXEL_ARRAY_TOP;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	/* Update vblank based on new mode. */
	min_vblank = mira220_calc_min_vblank(mira220);
	ret = __v4l2_ctrl_modify_range(mira220->vblank, min_vblank,
				       MIRA220_MAX_VBLANK, 1, min_vblank);
	if (ret)
		return ret;

	return __v4l2_ctrl_s_ctrl(mira220->vblank, min_vblank);
}

static int mira220_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct mira220 *mira220 = to_mira220(sd);
	bool mono = mira220_is_mono(mira220);
	unsigned int num_formats;
	unsigned int index;
	const u32 *codes;

	num_formats = mono ? ARRAY_SIZE(mira220_mbus_mono_formats) :
			     (ARRAY_SIZE(mira220_mbus_color_formats) / 4);

	if (code->index >= num_formats)
		return -EINVAL;

	index = mono ? code->index : (code->index * 4);
	codes = mono ? mira220_mbus_mono_formats : mira220_mbus_color_formats;
	code->code = mira220_get_format_code(mira220, codes[index]);

	return 0;
}

static int mira220_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct mira220 *mira220 = to_mira220(sd);
	u32 code;

	if (fse->index)
		return -EINVAL;

	code = mira220_get_format_code(mira220, fse->code);
	if (fse->code != code)
		return -EINVAL;

	fse->min_width = MIRA220_PIXEL_ARRAY_WIDTH;
	fse->max_width = fse->min_width;
	fse->min_height = MIRA220_PIXEL_ARRAY_HEIGHT;
	fse->max_height = fse->min_height;

	return 0;
}

static int mira220_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = MEDIA_BUS_FMT_SGRBG12_1X12,
			.width = MIRA220_PIXEL_ARRAY_WIDTH,
			.height = MIRA220_PIXEL_ARRAY_HEIGHT,
		},
	};

	mira220_set_pad_format(sd, state, &fmt);

	return 0;
}

static int mira220_set_bus_config(struct mira220 *mira220)
{
	int ret = 0;

	cci_write(mira220->regmap, MIRA220_MIPI_LANES_REG,
		  mira220->lanes - 1, &ret);

	cci_write(mira220->regmap, MIRA220_MIPI_CLK_MODE_REG,
		  MIRA220_MIPI_CLK_CONTINUOUS, &ret);

	return ret;
}

static int mira220_set_framefmt(struct mira220 *mira220,
				struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format;
	u32 top, left;
	int ret = 0;

	format = v4l2_subdev_state_get_format(state, 0);

	/* Program the image format bit order, bit depth and CSI-2 data type */

	ret = cci_write(mira220->regmap, MIRA220_BIT_ORDER_REG,
			MIRA220_BIT_ORDER_NORMAL, NULL);
	if (ret)
		return ret;

	switch (format->code) {
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		cci_write(mira220->regmap, MIRA220_BIT_DEPTH_REG,
			  MIRA220_BIT_DEPTH_8_BIT, &ret);
		cci_write(mira220->regmap, MIRA220_CSI_DATA_TYPE_REG,
			  MIRA220_CSI_DATA_TYPE_8_BIT, &ret);
		break;
	case MEDIA_BUS_FMT_Y10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		cci_write(mira220->regmap, MIRA220_BIT_DEPTH_REG,
			  MIRA220_BIT_DEPTH_10_BIT, &ret);
		cci_write(mira220->regmap, MIRA220_CSI_DATA_TYPE_REG,
			  MIRA220_CSI_DATA_TYPE_10_BIT, &ret);

		break;
	case MEDIA_BUS_FMT_Y12_1X12:
	case MEDIA_BUS_FMT_SGRBG12_1X12:
	case MEDIA_BUS_FMT_SGBRG12_1X12:
	case MEDIA_BUS_FMT_SBGGR12_1X12:
	case MEDIA_BUS_FMT_SRGGB12_1X12:
		cci_write(mira220->regmap, MIRA220_BIT_DEPTH_REG,
			  MIRA220_BIT_DEPTH_12_BIT, &ret);
		cci_write(mira220->regmap, MIRA220_CSI_DATA_TYPE_REG,
			  MIRA220_CSI_DATA_TYPE_12_BIT, &ret);

		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret)
		return ret;

	/* Program the sensor's row length. */
	ret = cci_write(mira220->regmap, MIRA220_REG_ROW_LENGTH,
			mira220->row_length, NULL);
	if (ret)
		return ret;

	/*
	 * Program the ROI area, centered in the sensor's pixel array.
	 *
	 * TODO: Use the crop rectangle sizes once the driver is ported to the
	 * new RAW camera sensor model.
	 */
	top = (MIRA220_PIXEL_ARRAY_HEIGHT - format->height) / 2;
	left = (MIRA220_PIXEL_ARRAY_WIDTH - format->width) / 2;

	cci_write(mira220->regmap, MIRA220_VSTART_REG, top, &ret);
	cci_write(mira220->regmap, MIRA220_HSTART_REG, left, &ret);
	cci_write(mira220->regmap, MIRA220_VSIZE_REG, format->height, &ret);
	cci_write(mira220->regmap, MIRA220_HSIZE_REG,
		  (format->width / 2) & MIRA220_HSIZE_MASK, &ret);
	cci_write(mira220->regmap, MIRA220_MIPI_HSIZE_REG, format->width, &ret);

	return ret;
}

static int mira220_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = MIRA220_NATIVE_WIDTH;
		sel->r.height = MIRA220_NATIVE_HEIGHT;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = MIRA220_PIXEL_ARRAY_TOP;
		sel->r.left = MIRA220_PIXEL_ARRAY_LEFT;
		sel->r.width = MIRA220_PIXEL_ARRAY_WIDTH;
		sel->r.height = MIRA220_PIXEL_ARRAY_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

/* OTP power on */
static int mira220_otp_power_on(struct mira220 *mira220)
{
	int ret = cci_write(mira220->regmap, MIRA220_OTP_CMD_REG,
			    MIRA220_OTP_CMD_UP, NULL);
	fsleep(100);

	return ret;
}

/* OTP power off */
static int mira220_otp_power_off(struct mira220 *mira220)
{
	return cci_write(mira220->regmap, MIRA220_OTP_CMD_REG,
			 MIRA220_OTP_CMD_DOWN, NULL);
}

/* OTP power on */
static int mira220_otp_read(struct mira220 *mira220, u8 addr, u8 offset,
			    u8 *val, int *err)
{
	int ret;
	u64 readback;

	if (err && *err)
		return *err;

	ret = cci_write(mira220->regmap, CCI_REG8(0x0086), addr, NULL);
	if (ret)
		goto out;

	ret = cci_write(mira220->regmap, CCI_REG8(0x0080), 0x02, NULL);
	if (ret)
		goto out;

	ret = cci_read(mira220->regmap, CCI_REG8(0x0082 + offset), &readback,
		       NULL);
	if (ret)
		goto out;

	*val = readback & 0xFF;

	return 0;

out:
	if (err)
		*err = ret;
	return ret;
}

/* Verify chip ID, module version, unique ID and variant */
static int mira220_identify_module(struct mira220 *mira220)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mira220->sd);
	u8 b0, b1, b2, b3, b4, b5, b6, b7;
	u32 variant;
	int ret;
	u8 val;
	u8 id;

	ret = mira220_otp_power_on(mira220);
	if (ret)
		return ret;

	/* Log module version checks */
	mira220_otp_read(mira220, 0x0d, 0, &val, &ret);
	mira220_otp_read(mira220, 0x1e, 0, &val, &ret);
	if (ret)
		return ret;

	/* Check unique module hardware ID version */
	ret = mira220_otp_read(mira220, 0x3a, 0, &id, NULL);
	if (ret)
		return ret;

	if (id < 1 || id > 2) {
		dev_err(&client->dev,
			"Read OTP 0x3a, id must be 1 or 2, but got: 0x%02x\n", id);
		ret = -EINVAL;
		goto err_power_off;
	}

	/* Read Unique ID bytes sequentially using the correct 4-argument signature */
	mira220_otp_read(mira220, 0x25, 0, &b0, &ret);
	mira220_otp_read(mira220, 0x1e, 0, &b1, &ret);
	mira220_otp_read(mira220, 0x1e, 1, &b2, &ret);
	mira220_otp_read(mira220, 0x1e, 2, &b3, &ret);
	mira220_otp_read(mira220, 0x1d, 0, &b4, &ret);
	mira220_otp_read(mira220, 0x1d, 1, &b5, &ret);
	mira220_otp_read(mira220, 0x1d, 2, &b6, &ret);
	mira220_otp_read(mira220, 0x1d, 3, &b7, &ret);
	if (ret)
		goto err_power_off;

	dev_dbg(&client->dev, "Unique ID: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
		b7, b6, b5, b4, b3, b2, b1, b0);

	mira220_otp_read(mira220, 0x3e, 0, &b0, &ret);
	mira220_otp_read(mira220, 0x3e, 1, &b1, &ret);
	mira220_otp_read(mira220, 0x3e, 2, &b2, &ret);
	mira220_otp_read(mira220, 0x3e, 3, &b3, &ret);
	if (ret)
		goto err_power_off;

	variant = b0 | b1 << 8 | b2 << 16 | b3 << 24;

	switch (variant) {
	case MIRA220_MONO_PLAIN_PF:
	case MIRA220_MONO_PLAIN_NO_PF:
	case MIRA220_MONO_AR_PF:
		dev_dbg(&client->dev, "Mira220 MONO variant: %x\n", variant);
		mira220->variant = MIRA220_VARIANT_MONO;
		break;
	case MIRA220_RGB_PLAIN_PF:
	case MIRA220_RGB_PLAIN_NO_PF:
	case MIRA220_RGB_AR_PF:
		dev_dbg(&client->dev, "Mira220 RGB variant: %x\n", variant);
		mira220->variant = MIRA220_VARIANT_RGB;
		break;
	default:
		dev_info(&client->dev,
			 "Unsupported mira220 variant %x; default to RGB\n",
			 variant);
		mira220->variant = MIRA220_VARIANT_RGB;
		break;
	}

err_power_off:
	mira220_otp_power_off(mira220);
	return ret;
}

static int mira220_otp_restore(struct mira220 *mira220)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mira220->sd);
	static const u16 reg_list[] = {
		0x4015, 0x4016, 0x4017, 0x4018, 0x403B, 0x4040, 0x4041, 0x4042,
		0x402a, 0x4029, 0x4009
	};
	u8 val;
	int ret;

	ret = mira220_otp_power_on(mira220);
	if (ret)
		return ret;

	/* Write OTP to image sensor when programmed */
	ret = mira220_otp_read(mira220, 0x1d, 0, &val, NULL);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to read OTP programming status\n");
		goto out;
	}

	if (val == 0xff) {
		dev_info(&client->dev, "OTP not programmed, skipping calibration\n");
		return mira220_otp_power_off(mira220);
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(reg_list); i++) {
		ret = mira220_otp_read(mira220, i, 0, &val, NULL);
		if (ret < 0) {
			dev_err(&client->dev, "Failed to read OTP address %d\n", i);
			goto out;
		}
		ret = cci_write(mira220->regmap, CCI_REG8(reg_list[i]), val, NULL);
		if (ret < 0) {
			dev_err(&client->dev,
				"Failed to write register 0x%04x\n", reg_list[i]);
			goto out;
		}
		dev_dbg(&client->dev, "OTP CALIBRATION: 0x%04x, val=0x%02x\n",
			reg_list[i], val);
	}

	ret = mira220_otp_read(mira220, 0x0d, 0, &val, NULL);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to read OTP address 0x0d\n");
		goto out;
	}

	ret = cci_write(mira220->regmap, CCI_REG8(0x403e), val, NULL);
	if (ret < 0)
		dev_err(&client->dev, "Failed to write register 0x403e\n");

out:
	mira220_otp_power_off(mira220);
	return ret;
}

static int mira220_enable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct mira220 *mira220 = to_mira220(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&mira220->sd);
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret < 0)
		return ret;

	/*
	 * Apply default values of current mode. Stop streaming before
	 * uploading register sequence.
	 */
	ret = mira220_write_stop_streaming_regs(mira220);
	if (ret)
		goto err_rpm_put;

	ret = cci_multi_reg_write(mira220->regmap, mira220_init_reg_list,
				  ARRAY_SIZE(mira220_init_reg_list), NULL);
	if (ret)
		goto err_rpm_put;

	ret = mira220_otp_restore(mira220);
	if (ret)
		goto err_rpm_put;

	ret = mira220_set_bus_config(mira220);
	if (ret)
		goto err_rpm_put;

	ret = mira220_set_framefmt(mira220, state);
	if (ret)
		goto err_rpm_put;

	/* Apply customized values from user */
	ret = __v4l2_ctrl_handler_setup(mira220->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = mira220_write_start_streaming_regs(mira220);
	if (ret)
		goto err_rpm_put;

	/* vflip and hflip cannot change during streaming */
	__v4l2_ctrl_grab(mira220->hflip, true);
	__v4l2_ctrl_grab(mira220->vflip, true);

	return 0;

err_rpm_put:
	pm_runtime_put_autosuspend(&client->dev);
	return ret;
}

static int mira220_disable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state, u32 pad,
				   u64 streams_mask)
{
	struct mira220 *mira220 = to_mira220(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&mira220->sd);

	mira220_write_stop_streaming_regs(mira220);

	__v4l2_ctrl_grab(mira220->hflip, false);
	__v4l2_ctrl_grab(mira220->vflip, false);

	pm_runtime_put_autosuspend(&client->dev);

	return 0;
}

static int mira220_get_regulators(struct mira220 *mira220)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mira220->sd);

	for (unsigned int i = 0; i < MIRA220_NUM_SUPPLIES; i++)
		mira220->supplies[i].supply = mira220_supply_name[i];

	return devm_regulator_bulk_get(&client->dev, MIRA220_NUM_SUPPLIES,
				       mira220->supplies);
}

static const struct v4l2_subdev_core_ops mira220_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops mira220_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops mira220_pad_ops = {
	.enum_mbus_code = mira220_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mira220_set_pad_format,
	.get_selection = mira220_get_selection,
	.enum_frame_size = mira220_enum_frame_size,
	.enable_streams = mira220_enable_streams,
	.disable_streams = mira220_disable_streams,
};

static const struct v4l2_subdev_ops mira220_subdev_ops = {
	.core = &mira220_core_ops,
	.video = &mira220_video_ops,
	.pad = &mira220_pad_ops,
};

static const struct v4l2_subdev_internal_ops mira220_internal_ops = {
	.init_state = mira220_init_state,
};

/* Initialize control handlers */
static int mira220_init_controls(struct mira220 *mira220)
{
	struct i2c_client *client = v4l2_get_subdevdata(&mira220->sd);
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *hblank;
	u32 max_exposure = 0;
	u32 min_vblank;
	u32 hblank_val;
	int ret;

	ctrl_hdlr = &mira220->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 9);
	if (ret)
		return ret;

	/* By default, PIXEL_RATE is read only */
	v4l2_ctrl_new_std(ctrl_hdlr, &mira220_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  MIRA220_PIXEL_RATE, MIRA220_PIXEL_RATE, 1,
			  MIRA220_PIXEL_RATE);

	min_vblank = mira220_calc_min_vblank(mira220);
	mira220->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &mira220_ctrl_ops,
					    V4L2_CID_VBLANK,
					    min_vblank, MIRA220_MAX_VBLANK, 1,
					    min_vblank);

	/*
	 * Scale hblank according to the number of enabled data lanes to match
	 * row_length.
	 */
	hblank_val = MIRA220_LLP_1600x1400_304 * (2 / mira220->lanes)
		   - MIRA220_PIXEL_ARRAY_WIDTH;
	hblank = v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_HBLANK, hblank_val,
				   hblank_val, 1, hblank_val);
	if (hblank)
		hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Max exposure is determined by vblank + vsize and Tglob. */
	max_exposure = mira220_calc_exposure(mira220,
					     MIRA220_PIXEL_ARRAY_HEIGHT,
					     min_vblank);

	mira220->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &mira220_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      MIRA220_EXPOSURE_MIN,
					      max_exposure, 1,
					      MIRA220_DEFAULT_EXPOSURE);

	v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_ANALOGUE_GAIN,
			  MIRA220_ANALOG_GAIN_MIN, MIRA220_ANALOG_GAIN_MAX,
			  MIRA220_ANALOG_GAIN_STEP,
			  MIRA220_ANALOG_GAIN_DEFAULT);

	mira220->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &mira220_ctrl_ops,
					   V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (mira220->hflip)
		mira220->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	mira220->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &mira220_ctrl_ops,
					   V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (mira220->vflip)
		mira220->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &mira220_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(mira220_test_pattern_menu) - 1,
				     0, 0, mira220_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &mira220_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	mira220->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int mira220_parse_endpoint(struct device *dev, struct mira220 *mira220)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = 0;

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
	if (!endpoint) {
		dev_err(dev, "Endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		ret = -EINVAL;
		dev_err(dev, "Failed to parse endpoint\n");
		goto error_out;
	}

	/* Check the number of MIPI CSI2 data lanes */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 1 &&
	    ep_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		ret = -EINVAL;
		dev_err(dev, "%u data lanes are not supported\n",
			ep_cfg.bus.mipi_csi2.num_data_lanes);
		goto error_out;
	}

	mira220->lanes = ep_cfg.bus.mipi_csi2.num_data_lanes;
	mira220->row_length = MIRA220_ROW_LENGTH_MIN * (2 / mira220->lanes);

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int mira220_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mira220 *mira220;
	int ret;

	mira220 = devm_kzalloc(&client->dev, sizeof(*mira220), GFP_KERNEL);
	if (!mira220)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&mira220->sd, client, &mira220_subdev_ops);
	mira220->sd.internal_ops = &mira220_internal_ops;

	ret = mira220_parse_endpoint(dev, mira220);
	if (ret)
		return ret;

	mira220->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(mira220->regmap))
		return dev_err_probe(dev, PTR_ERR(mira220->regmap),
				     "failed to initialize CCI\n");

	/* Get system clock (xclk) */
	mira220->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(mira220->xclk))
		return dev_err_probe(dev, PTR_ERR(mira220->xclk),
				     "failed to get xclk\n");

	mira220->xclk_freq = clk_get_rate(mira220->xclk);
	if (mira220->xclk_freq != MIRA220_SUPPORTED_XCLK_FREQ) {
		dev_err(dev, "xclk frequency not supported: %d Hz\n",
			mira220->xclk_freq);
		return -EINVAL;
	}

	ret = mira220_get_regulators(mira220);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	mira220->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(mira220->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(mira220->reset_gpio),
				     "failed to get reset gpio\n");

	ret = mira220_power_on(dev);
	if (ret)
		return ret;

	/* Enable runtime PM and power on the device */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = mira220_identify_module(mira220);
	if (ret)
		goto error_power_off;

	ret = mira220_init_controls(mira220);
	if (ret)
		goto error_power_off;

	/* Initialize subdev */
	mira220->sd.internal_ops = &mira220_internal_ops;
	mira220->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			     V4L2_SUBDEV_FL_HAS_EVENTS;
	mira220->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pads */
	mira220->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&mira220->sd.entity, 1, &mira220->pad);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init entity pads\n");
		goto error_handler_free;
	}

	mira220->sd.state_lock = mira220->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&mira220->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "subdev init error\n");
		goto error_media_entity;
	}

	ret = v4l2_async_register_subdev_sensor(&mira220->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret,
			      "failed to register sensor sub-device\n");
		goto error_subdev_cleanup;
	}

	pm_runtime_idle(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&mira220->sd);
error_media_entity:
	media_entity_cleanup(&mira220->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(mira220->sd.ctrl_handler);
error_power_off:
	pm_runtime_disable(dev);
	mira220_power_off(dev);
	pm_runtime_set_suspended(dev);
	return ret;
}

static void mira220_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct mira220 *mira220 = to_mira220(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(&mira220->sd);
	media_entity_cleanup(&sd->entity);

	v4l2_ctrl_handler_free(mira220->sd.ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		mira220_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct dev_pm_ops mira220_pm_ops = {
	SET_RUNTIME_PM_OPS(mira220_power_off, mira220_power_on, NULL)
};

static const struct of_device_id mira220_dt_ids[] = {
	{ .compatible = "ams,mira220" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mira220_dt_ids);

static struct i2c_driver mira220_i2c_driver = {
	.driver = {
		.name = "mira220",
		.of_match_table	= mira220_dt_ids,
		.pm = &mira220_pm_ops,
	},
	.probe = mira220_probe,
	.remove = mira220_remove,
};

module_i2c_driver(mira220_i2c_driver);

MODULE_AUTHOR("Philippe Baetens <philippe.baetens@ams-osram.com>");
MODULE_DESCRIPTION("ams MIRA220 sensor driver");
MODULE_LICENSE("GPL");
