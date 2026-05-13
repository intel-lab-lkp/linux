// SPDX-License-Identifier: GPL-2.0
/*
 * V4L2 driver for Sony IMX678
 *
 * Diagonal 8.86 mm (Type 1/1.8) CMOS image sensor with 8.40 M effective pixels.
 *
 * Copyright (C) 2026 Ideas On Board Oy.
 *
 * Based on Sony IMX678 driver prepared by Will Whang & Soho Enterprise Ltd.
 *
 * Based on Sony imx477 camera driver
 * Copyright (C) 2019-2020 Raspberry Pi (Trading) Ltd
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-rect.h>
#include <media/v4l2-subdev.h>

/* Standby or streaming mode */
#define IMX678_REG_MODE_SELECT          CCI_REG8(0x3000)
#define IMX678_MODE_STANDBY             0x01
#define IMX678_MODE_STREAMING           0x00
#define IMX678_STREAM_DELAY_US          25000
#define IMX678_STREAM_DELAY_RANGE_US    1000

/* XVS/XHS sync control */
#define IMX678_REG_XMSTA                CCI_REG8(0x3002)
#define IMX678_REG_XXS_DRV              CCI_REG8(0x30A6)
#define IMX678_REG_XXS_OUTSEL           CCI_REG8(0x30A4)

/* Clk selection */
#define IMX678_REG_INCK_SEL             CCI_REG8(0x3014)

/* Link Speed */
#define IMX678_REG_DATARATE_SEL         CCI_REG8(0x3015)

/* Lane Count */
#define IMX678_REG_LANEMODE             CCI_REG8(0x3040)

/* VMAX internal VBLANK*/
#define IMX678_REG_VMAX                 CCI_REG24_LE(0x3028)
#define IMX678_VMAX_MAX                 0xfffff
#define IMX678_VMAX_DEFAULT             2250

/* HMAX internal HBLANK*/
#define IMX678_REG_HMAX                 CCI_REG16_LE(0x302C)
#define IMX678_HMAX_MAX                 0xffff

/* SHR internal */
#define IMX678_REG_SHR                  CCI_REG24_LE(0x3050)
#define IMX678_SHR_MIN                  8
#define IMX678_SHR_MIN_CLEARHDR         10
#define IMX678_SHR_MAX                  0xfffff

/* Exposure control */
#define IMX678_EXPOSURE_MIN             2
#define IMX678_EXPOSURE_STEP            1
#define IMX678_EXPOSURE_DEFAULT         1000
#define IMX678_EXPOSURE_MAX             49865

/* Black level control */
#define IMX678_REG_BLKLEVEL             CCI_REG16_LE(0x30DC)
#define IMX678_BLKLEVEL_DEFAULT         0x4032

/* Digital Clamp */
#define IMX678_REG_DIGITAL_CLAMP        CCI_REG8(0x3458)

/* Analog gain control */
#define IMX678_REG_ANALOG_GAIN          CCI_REG16_LE(0x3070)
#define IMX678_ANA_GAIN_MIN_NORMAL      0
#define IMX678_ANA_GAIN_MAX_NORMAL      240
#define IMX678_ANA_GAIN_STEP            1
#define IMX678_ANA_GAIN_DEFAULT         0

/* Crop */
#define IMX678_REG_WINMODE		CCI_REG8(0x3018)
#define IMX678_REG_PIX_HST		CCI_REG16_LE(0x303c)
#define IMX678_REG_PIX_HWIDTH		CCI_REG16_LE(0x303e)
#define IMX678_REG_PIX_VST		CCI_REG16_LE(0x3044)
#define IMX678_REG_PIX_VWIDTH		CCI_REG16_LE(0x3046)

/* Flip */
#define IMX678_REG_WINMODEH             CCI_REG8(0x3020)
#define IMX678_REG_WINMODEV             CCI_REG8(0x3021)

/* Sensor Identification */
#define IMX678_REG_MONOCHROME		CCI_REG8(0x4D18)
#define IMX678_TYPE			BIT(0)
#define IMX678_REG_MODULE_ID		CCI_REG16_LE(0x4D1C)
#define IMX678_ID			0x02a6
#define IMX678_MODULE_ID_DELAY		80000
#define IMX678_MODULE_ID_DELAY_RANGE	1000

/* Common configuration registers */
#define IMX678_REG_WDMODE               CCI_REG8(0x301A)
#define IMX678_REG_ADDMODE              CCI_REG8(0x301B)
#define IMX678_REG_THIN_V_EN            CCI_REG8(0x301C)
#define IMX678_REG_VCMODE               CCI_REG8(0x301E)
#define IMX678_REG_ADBIT                CCI_REG8(0x3022)
#define IMX678_REG_MDBIT                CCI_REG8(0x3023)
#define IMX678_REG_GAIN_PGC_FIDMD       CCI_REG8(0x3400)

/* Test pattern generator */
#define IMX678_REG_TPG_EN_DUOUT		CCI_REG8(0x30E0)
#define IMX678_REG_TPG_PATSEL_DUOUT	CCI_REG8(0x30E2)
#define IMX678_TPG_ALL_000		0
#define IMX678_TPG_ALL_FFF		1
#define IMX678_TPG_ALL_555		2
#define IMX678_TPG_ALL_AAA		3
#define IMX678_TPG_TOG_555_AAA		4
#define IMX678_TPG_TOG_AAA_555		5
#define IMX678_TPG_TOG_000_555		6
#define IMX678_TPG_TOG_555_000		7
#define IMX678_TPG_TOG_000_FFF		8
#define IMX678_TPG_TOG_FFF_000		9
#define IMX678_TPG_H_COLOR_BARS		10
#define IMX678_TPG_V_COLOR_BARS		11
#define IMX678_REG_TPG_COLORWIDTH	CCI_REG8(0x30E4)
#define IMX678_TPG_COLORWIDTH_80PIX	0
#define IMX678_TPG_COLORWIDTH_160PIX	1
#define IMX678_TPG_COLORWIDTH_320PIX	2
#define IMX678_TPG_COLORWIDTH_640PIX	3

#define IMX678_REG_INTERFACE_SEL	CCI_REG8(0x4E3C)
#define IMX678_INTERFACE_2L_4L		0x07
#define IMX678_INTERFACE_8L_2x4L	0x7f

#define IMX678_PIXEL_RATE               74250000

/* Minimum output resolution */
#define IMX678_PIXEL_ARRAY_MIN_WIDTH	128
#define IMX678_PIXEL_ARRAY_MIN_HEIGHT	96

/* Sensor windowing register alignment */
#define IMX678_CROP_HWIDTH_ALIGN	16
#define IMX678_CROP_VWIDTH_ALIGN	4
#define IMX678_CROP_HST_ALIGN		4
#define IMX678_CROP_VST_ALIGN		4

/* IMX678 native and active pixel array size. */
static const struct v4l2_rect imx678_native_area = {
	.top = 0,
	.left = 0,
	.width = 3857,
	.height = 2201,
};

static const struct v4l2_rect imx678_active_area = {
	.top = 20,
	.left = 0,
	.width = 3856,
	.height = 2180,
};

enum imx678_type {
	IMX678_COLOR = 0,
	IMX678_MONOCHROME = 1,
};

/* Link frequency setup (DDR: lane rate = 2 x link freq) */
enum {
	IMX678_LINK_FREQ_297MHZ,
	IMX678_LINK_FREQ_360MHZ,
	IMX678_LINK_FREQ_445MHZ,
	IMX678_LINK_FREQ_594MHZ,
	IMX678_LINK_FREQ_720MHZ,
	IMX678_LINK_FREQ_891MHZ,
	IMX678_LINK_FREQ_1039MHZ,
	IMX678_LINK_FREQ_1188MHZ,
};

static const u8 link_freqs_reg_value[] = {
	[IMX678_LINK_FREQ_297MHZ]  = 0x07,
	[IMX678_LINK_FREQ_360MHZ]  = 0x06,
	[IMX678_LINK_FREQ_445MHZ]  = 0x05,
	[IMX678_LINK_FREQ_594MHZ]  = 0x04,
	[IMX678_LINK_FREQ_720MHZ]  = 0x03,
	[IMX678_LINK_FREQ_891MHZ]  = 0x02,
	[IMX678_LINK_FREQ_1039MHZ] = 0x01,
	[IMX678_LINK_FREQ_1188MHZ] = 0x00,
};

static const u64 link_freqs[] = {
	[IMX678_LINK_FREQ_297MHZ]  = 297000000,
	[IMX678_LINK_FREQ_360MHZ]  = 360000000,
	[IMX678_LINK_FREQ_445MHZ]  = 445500000,
	[IMX678_LINK_FREQ_594MHZ]  = 594000000,
	[IMX678_LINK_FREQ_720MHZ]  = 720000000,
	[IMX678_LINK_FREQ_891MHZ]  = 891000000,
	[IMX678_LINK_FREQ_1039MHZ] = 1039500000,
	[IMX678_LINK_FREQ_1188MHZ] = 1188000000,
};

static const u16 min_hmax_4lane[] = {
	[IMX678_LINK_FREQ_297MHZ] = 1584,
	[IMX678_LINK_FREQ_360MHZ] = 1320,
	[IMX678_LINK_FREQ_445MHZ] = 1100,
	[IMX678_LINK_FREQ_594MHZ] =  792,
	[IMX678_LINK_FREQ_720MHZ] =  660,
	[IMX678_LINK_FREQ_891MHZ] =  550,
	[IMX678_LINK_FREQ_1039MHZ] = 550,
	[IMX678_LINK_FREQ_1188MHZ] = 550,
};

struct imx678_inck_cfg {
	u32 xclk_hz;   /* platform clock rate  */
	u8  inck_sel;  /* value for reg        */
};

static const struct imx678_inck_cfg imx678_inck_table[] = {
	{ 74250000, 0x00 },
	{ 37125000, 0x01 },
	{ 72000000, 0x02 },
	{ 27000000, 0x03 },
	{ 24000000, 0x04 },
	{ 36000000, 0x05 },
	{ 18000000, 0x06 },
	{ 13500000, 0x07 },
};

static const char * const imx678_tpg_menu[] = {
	"Disabled",
	"All 000h",
	"All FFFh",
	"All 555h",
	"All AAAh",
	"Toggle 555/AAAh",
	"Toggle AAA/555h",
	"Toggle 000/555h",
	"Toggle 555/000h",
	"Toggle 000/FFFh",
	"Toggle FFF/000h",
	"Horizontal color bars",
	"Vertical color bars",
};

static const int imx678_tpg_val[] = {
	IMX678_TPG_ALL_000,
	IMX678_TPG_ALL_000,
	IMX678_TPG_ALL_FFF,
	IMX678_TPG_ALL_555,
	IMX678_TPG_ALL_AAA,
	IMX678_TPG_TOG_555_AAA,
	IMX678_TPG_TOG_AAA_555,
	IMX678_TPG_TOG_000_555,
	IMX678_TPG_TOG_555_000,
	IMX678_TPG_TOG_000_FFF,
	IMX678_TPG_TOG_FFF_000,
	IMX678_TPG_H_COLOR_BARS,
	IMX678_TPG_V_COLOR_BARS,
};

/* IMX678 Register List */
/* Common Modes */
static const struct cci_reg_sequence common_regs[] = {
	{IMX678_REG_THIN_V_EN, 0x00}, {IMX678_REG_VCMODE, 0x01},
	{CCI_REG8(0x306B), 0x00}, {IMX678_REG_GAIN_PGC_FIDMD, 0x01},
	{CCI_REG8(0x3460), 0x22}, {CCI_REG8(0x355A), 0x64}, {CCI_REG8(0x3A02), 0x7A},
	{CCI_REG8(0x3A10), 0xEC}, {CCI_REG8(0x3A12), 0x71}, {CCI_REG8(0x3A14), 0xDE},
	{CCI_REG8(0x3A20), 0x2B}, {CCI_REG8(0x3A24), 0x22}, {CCI_REG8(0x3A25), 0x25},
	{CCI_REG8(0x3A26), 0x2A}, {CCI_REG8(0x3A27), 0x2C}, {CCI_REG8(0x3A28), 0x39},
	{CCI_REG8(0x3A29), 0x38}, {CCI_REG8(0x3A30), 0x04}, {CCI_REG8(0x3A31), 0x04},
	{CCI_REG8(0x3A32), 0x03}, {CCI_REG8(0x3A33), 0x03}, {CCI_REG8(0x3A34), 0x09},
	{CCI_REG8(0x3A35), 0x06}, {CCI_REG8(0x3A38), 0xCD}, {CCI_REG8(0x3A3A), 0x4C},
	{CCI_REG8(0x3A3C), 0xB9}, {CCI_REG8(0x3A3E), 0x30}, {CCI_REG8(0x3A40), 0x2C},
	{CCI_REG8(0x3A42), 0x39}, {CCI_REG8(0x3A4E), 0x00}, {CCI_REG8(0x3A52), 0x00},
	{CCI_REG8(0x3A56), 0x00}, {CCI_REG8(0x3A5A), 0x00}, {CCI_REG8(0x3A5E), 0x00},
	{CCI_REG8(0x3A62), 0x00}, {CCI_REG8(0x3A64), 0x00}, {CCI_REG8(0x3A6E), 0xA0},
	{CCI_REG8(0x3A70), 0x50}, {CCI_REG8(0x3A8C), 0x04}, {CCI_REG8(0x3A8D), 0x03},
	{CCI_REG8(0x3A8E), 0x09}, {CCI_REG8(0x3A90), 0x38}, {CCI_REG8(0x3A91), 0x42},
	{CCI_REG8(0x3A92), 0x3C}, {CCI_REG8(0x3B0E), 0xF3}, {CCI_REG8(0x3B12), 0xE5},
	{CCI_REG8(0x3B27), 0xC0}, {CCI_REG8(0x3B2E), 0xEF}, {CCI_REG8(0x3B30), 0x6A},
	{CCI_REG8(0x3B32), 0xF6}, {CCI_REG8(0x3B36), 0xE1}, {CCI_REG8(0x3B3A), 0xE8},
	{CCI_REG8(0x3B5A), 0x17}, {CCI_REG8(0x3B5E), 0xEF}, {CCI_REG8(0x3B60), 0x6A},
	{CCI_REG8(0x3B62), 0xF6}, {CCI_REG8(0x3B66), 0xE1}, {CCI_REG8(0x3B6A), 0xE8},
	{CCI_REG8(0x3B88), 0xEC}, {CCI_REG8(0x3B8A), 0xED}, {CCI_REG8(0x3B94), 0x71},
	{CCI_REG8(0x3B96), 0x72}, {CCI_REG8(0x3B98), 0xDE}, {CCI_REG8(0x3B9A), 0xDF},
	{CCI_REG8(0x3C0F), 0x06}, {CCI_REG8(0x3C10), 0x06}, {CCI_REG8(0x3C11), 0x06},
	{CCI_REG8(0x3C12), 0x06}, {CCI_REG8(0x3C13), 0x06}, {CCI_REG8(0x3C18), 0x20},
	{CCI_REG8(0x3C37), 0x10}, {CCI_REG8(0x3C3A), 0x7A}, {CCI_REG8(0x3C40), 0xF4},
	{CCI_REG8(0x3C48), 0xE6}, {CCI_REG8(0x3C54), 0xCE}, {CCI_REG8(0x3C56), 0xD0},
	{CCI_REG8(0x3C6C), 0x53}, {CCI_REG8(0x3C6E), 0x55}, {CCI_REG8(0x3C70), 0xC0},
	{CCI_REG8(0x3C72), 0xC2}, {CCI_REG8(0x3C7E), 0xCE}, {CCI_REG8(0x3C8C), 0xCF},
	{CCI_REG8(0x3C8E), 0xEB}, {CCI_REG8(0x3C98), 0x54}, {CCI_REG8(0x3C9A), 0x70},
	{CCI_REG8(0x3C9C), 0xC1}, {CCI_REG8(0x3C9E), 0xDD}, {CCI_REG8(0x3CB0), 0x7A},
	{CCI_REG8(0x3CB2), 0xBA}, {CCI_REG8(0x3CC8), 0xBC}, {CCI_REG8(0x3CCA), 0x7C},
	{CCI_REG8(0x3CD4), 0xEA}, {CCI_REG8(0x3CD5), 0x01}, {CCI_REG8(0x3CD6), 0x4A},
	{CCI_REG8(0x3CD8), 0x00}, {CCI_REG8(0x3CD9), 0x00}, {CCI_REG8(0x3CDA), 0xFF},
	{CCI_REG8(0x3CDB), 0x03}, {CCI_REG8(0x3CDC), 0x00}, {CCI_REG8(0x3CDD), 0x00},
	{CCI_REG8(0x3CDE), 0xFF}, {CCI_REG8(0x3CDF), 0x03}, {CCI_REG8(0x3CE4), 0x4C},
	{CCI_REG8(0x3CE6), 0xEC}, {CCI_REG8(0x3CE7), 0x01}, {CCI_REG8(0x3CE8), 0xFF},
	{CCI_REG8(0x3CE9), 0x03}, {CCI_REG8(0x3CEA), 0x00}, {CCI_REG8(0x3CEB), 0x00},
	{CCI_REG8(0x3CEC), 0xFF}, {CCI_REG8(0x3CED), 0x03}, {CCI_REG8(0x3CEE), 0x00},
	{CCI_REG8(0x3CEF), 0x00}, {CCI_REG8(0x3CF2), 0xFF}, {CCI_REG8(0x3CF3), 0x03},
	{CCI_REG8(0x3CF4), 0x00}, {CCI_REG8(0x3E28), 0x82}, {CCI_REG8(0x3E2A), 0x80},
	{CCI_REG8(0x3E30), 0x85}, {CCI_REG8(0x3E32), 0x7D}, {CCI_REG8(0x3E5C), 0xCE},
	{CCI_REG8(0x3E5E), 0xD3}, {CCI_REG8(0x3E70), 0x53}, {CCI_REG8(0x3E72), 0x58},
	{CCI_REG8(0x3E74), 0xC0}, {CCI_REG8(0x3E76), 0xC5}, {CCI_REG8(0x3E78), 0xC0},
	{CCI_REG8(0x3E79), 0x01}, {CCI_REG8(0x3E7A), 0xD4}, {CCI_REG8(0x3E7B), 0x01},
	{CCI_REG8(0x3EB4), 0x0B}, {CCI_REG8(0x3EB5), 0x02}, {CCI_REG8(0x3EB6), 0x4D},
	{CCI_REG8(0x3EB7), 0x42}, {CCI_REG8(0x3EEC), 0xF3}, {CCI_REG8(0x3EEE), 0xE7},
	{CCI_REG8(0x3F01), 0x01}, {CCI_REG8(0x3F24), 0x10}, {CCI_REG8(0x3F28), 0x2D},
	{CCI_REG8(0x3F2A), 0x2D}, {CCI_REG8(0x3F2C), 0x2D}, {CCI_REG8(0x3F2E), 0x2D},
	{CCI_REG8(0x3F30), 0x23}, {CCI_REG8(0x3F38), 0x2D}, {CCI_REG8(0x3F3A), 0x2D},
	{CCI_REG8(0x3F3C), 0x2D}, {CCI_REG8(0x3F3E), 0x28}, {CCI_REG8(0x3F40), 0x1E},
	{CCI_REG8(0x3F48), 0x2D}, {CCI_REG8(0x3F4A), 0x2D}, {CCI_REG8(0x3F4C), 0x00},
	{CCI_REG8(0x4004), 0xE4}, {CCI_REG8(0x4006), 0xFF}, {CCI_REG8(0x4018), 0x69},
	{CCI_REG8(0x401A), 0x84}, {CCI_REG8(0x401C), 0xD6}, {CCI_REG8(0x401E), 0xF1},
	{CCI_REG8(0x4038), 0xDE}, {CCI_REG8(0x403A), 0x00}, {CCI_REG8(0x403B), 0x01},
	{CCI_REG8(0x404C), 0x63}, {CCI_REG8(0x404E), 0x85}, {CCI_REG8(0x4050), 0xD0},
	{CCI_REG8(0x4052), 0xF2}, {CCI_REG8(0x4108), 0xDD}, {CCI_REG8(0x410A), 0xF7},
	{CCI_REG8(0x411C), 0x62}, {CCI_REG8(0x411E), 0x7C}, {CCI_REG8(0x4120), 0xCF},
	{CCI_REG8(0x4122), 0xE9}, {CCI_REG8(0x4138), 0xE6}, {CCI_REG8(0x413A), 0xF1},
	{CCI_REG8(0x414C), 0x6B}, {CCI_REG8(0x414E), 0x76}, {CCI_REG8(0x4150), 0xD8},
	{CCI_REG8(0x4152), 0xE3}, {CCI_REG8(0x417E), 0x03}, {CCI_REG8(0x417F), 0x01},
	{CCI_REG8(0x4186), 0xE0}, {CCI_REG8(0x4190), 0xF3}, {CCI_REG8(0x4192), 0xF7},
	{CCI_REG8(0x419C), 0x78}, {CCI_REG8(0x419E), 0x7C}, {CCI_REG8(0x41A0), 0xE5},
	{CCI_REG8(0x41A2), 0xE9}, {CCI_REG8(0x41C8), 0xE2}, {CCI_REG8(0x41CA), 0xFD},
	{CCI_REG8(0x41DC), 0x67}, {CCI_REG8(0x41DE), 0x82}, {CCI_REG8(0x41E0), 0xD4},
	{CCI_REG8(0x41E2), 0xEF}, {CCI_REG8(0x4200), 0xDE}, {CCI_REG8(0x4202), 0xDA},
	{CCI_REG8(0x4218), 0x63}, {CCI_REG8(0x421A), 0x5F}, {CCI_REG8(0x421C), 0xD0},
	{CCI_REG8(0x421E), 0xCC}, {CCI_REG8(0x425A), 0x82}, {CCI_REG8(0x425C), 0xEF},
	{CCI_REG8(0x4348), 0xFE}, {CCI_REG8(0x4349), 0x06}, {CCI_REG8(0x4352), 0xCE},
	{CCI_REG8(0x4420), 0x0B}, {CCI_REG8(0x4421), 0x02}, {CCI_REG8(0x4422), 0x4D},
	{CCI_REG8(0x4423), 0x0A}, {CCI_REG8(0x4426), 0xF5}, {CCI_REG8(0x442A), 0xE7},
	{CCI_REG8(0x4432), 0xF5}, {CCI_REG8(0x4436), 0xE7}, {CCI_REG8(0x4466), 0xB4},
	{CCI_REG8(0x446E), 0x32}, {CCI_REG8(0x449F), 0x1C}, {CCI_REG8(0x44A4), 0x2C},
	{CCI_REG8(0x44A6), 0x2C}, {CCI_REG8(0x44A8), 0x2C}, {CCI_REG8(0x44AA), 0x2C},
	{CCI_REG8(0x44B4), 0x2C}, {CCI_REG8(0x44B6), 0x2C}, {CCI_REG8(0x44B8), 0x2C},
	{CCI_REG8(0x44BA), 0x2C}, {CCI_REG8(0x44C4), 0x2C}, {CCI_REG8(0x44C6), 0x2C},
	{CCI_REG8(0x44C8), 0x2C}, {CCI_REG8(0x4506), 0xF3}, {CCI_REG8(0x450E), 0xE5},
	{CCI_REG8(0x4516), 0xF3}, {CCI_REG8(0x4522), 0xE5}, {CCI_REG8(0x4524), 0xF3},
	{CCI_REG8(0x452C), 0xE5}, {CCI_REG8(0x453C), 0x22}, {CCI_REG8(0x453D), 0x1B},
	{CCI_REG8(0x453E), 0x1B}, {CCI_REG8(0x453F), 0x15}, {CCI_REG8(0x4540), 0x15},
	{CCI_REG8(0x4541), 0x15}, {CCI_REG8(0x4542), 0x15}, {CCI_REG8(0x4543), 0x15},
	{CCI_REG8(0x4544), 0x15}, {CCI_REG8(0x4548), 0x00}, {CCI_REG8(0x4549), 0x01},
	{CCI_REG8(0x454A), 0x01}, {CCI_REG8(0x454B), 0x06}, {CCI_REG8(0x454C), 0x06},
	{CCI_REG8(0x454D), 0x06}, {CCI_REG8(0x454E), 0x06}, {CCI_REG8(0x454F), 0x06},
	{CCI_REG8(0x4550), 0x06}, {CCI_REG8(0x4554), 0x55}, {CCI_REG8(0x4555), 0x02},
	{CCI_REG8(0x4556), 0x42}, {CCI_REG8(0x4557), 0x05}, {CCI_REG8(0x4558), 0xFD},
	{CCI_REG8(0x4559), 0x05}, {CCI_REG8(0x455A), 0x94}, {CCI_REG8(0x455B), 0x06},
	{CCI_REG8(0x455D), 0x06}, {CCI_REG8(0x455E), 0x49}, {CCI_REG8(0x455F), 0x07},
	{CCI_REG8(0x4560), 0x7F}, {CCI_REG8(0x4561), 0x07}, {CCI_REG8(0x4562), 0xA5},
	{CCI_REG8(0x4564), 0x55}, {CCI_REG8(0x4565), 0x02}, {CCI_REG8(0x4566), 0x42},
	{CCI_REG8(0x4567), 0x05}, {CCI_REG8(0x4568), 0xFD}, {CCI_REG8(0x4569), 0x05},
	{CCI_REG8(0x456A), 0x94}, {CCI_REG8(0x456B), 0x06}, {CCI_REG8(0x456D), 0x06},
	{CCI_REG8(0x456E), 0x49}, {CCI_REG8(0x456F), 0x07}, {CCI_REG8(0x4572), 0xA5},
	{CCI_REG8(0x460C), 0x7D}, {CCI_REG8(0x460E), 0xB1}, {CCI_REG8(0x4614), 0xA8},
	{CCI_REG8(0x4616), 0xB2}, {CCI_REG8(0x461C), 0x7E}, {CCI_REG8(0x461E), 0xA7},
	{CCI_REG8(0x4624), 0xA8}, {CCI_REG8(0x4626), 0xB2}, {CCI_REG8(0x462C), 0x7E},
	{CCI_REG8(0x462E), 0x8A}, {CCI_REG8(0x4630), 0x94}, {CCI_REG8(0x4632), 0xA7},
	{CCI_REG8(0x4634), 0xFB}, {CCI_REG8(0x4636), 0x2F}, {CCI_REG8(0x4638), 0x81},
	{CCI_REG8(0x4639), 0x01}, {CCI_REG8(0x463A), 0xB5}, {CCI_REG8(0x463B), 0x01},
	{CCI_REG8(0x463C), 0x26}, {CCI_REG8(0x463E), 0x30}, {CCI_REG8(0x4640), 0xAC},
	{CCI_REG8(0x4641), 0x01}, {CCI_REG8(0x4642), 0xB6}, {CCI_REG8(0x4643), 0x01},
	{CCI_REG8(0x4644), 0xFC}, {CCI_REG8(0x4646), 0x25}, {CCI_REG8(0x4648), 0x82},
	{CCI_REG8(0x4649), 0x01}, {CCI_REG8(0x464A), 0xAB}, {CCI_REG8(0x464B), 0x01},
	{CCI_REG8(0x464C), 0x26}, {CCI_REG8(0x464E), 0x30}, {CCI_REG8(0x4654), 0xFC},
	{CCI_REG8(0x4656), 0x08}, {CCI_REG8(0x4658), 0x12}, {CCI_REG8(0x465A), 0x25},
	{CCI_REG8(0x4662), 0xFC}, {CCI_REG8(0x46A2), 0xFB}, {CCI_REG8(0x46D6), 0xF3},
	{CCI_REG8(0x46E6), 0x00}, {CCI_REG8(0x46E8), 0xFF}, {CCI_REG8(0x46E9), 0x03},
	{CCI_REG8(0x46EC), 0x7A}, {CCI_REG8(0x46EE), 0xE5}, {CCI_REG8(0x46F4), 0xEE},
	{CCI_REG8(0x46F6), 0xF2}, {CCI_REG8(0x470C), 0xFF}, {CCI_REG8(0x470D), 0x03},
	{CCI_REG8(0x470E), 0x00}, {CCI_REG8(0x4714), 0xE0}, {CCI_REG8(0x4716), 0xE4},
	{CCI_REG8(0x471E), 0xED}, {CCI_REG8(0x472E), 0x00}, {CCI_REG8(0x4730), 0xFF},
	{CCI_REG8(0x4731), 0x03}, {CCI_REG8(0x4734), 0x7B}, {CCI_REG8(0x4736), 0xDF},
	{CCI_REG8(0x4754), 0x7D}, {CCI_REG8(0x4756), 0x8B}, {CCI_REG8(0x4758), 0x93},
	{CCI_REG8(0x475A), 0xB1}, {CCI_REG8(0x475C), 0xFB}, {CCI_REG8(0x475E), 0x09},
	{CCI_REG8(0x4760), 0x11}, {CCI_REG8(0x4762), 0x2F}, {CCI_REG8(0x4766), 0xCC},
	{CCI_REG8(0x4776), 0xCB}, {CCI_REG8(0x477E), 0x4A}, {CCI_REG8(0x478E), 0x49},
	{CCI_REG8(0x4794), 0x7C}, {CCI_REG8(0x4796), 0x8F}, {CCI_REG8(0x4798), 0xB3},
	{CCI_REG8(0x4799), 0x00}, {CCI_REG8(0x479A), 0xCC}, {CCI_REG8(0x479C), 0xC1},
	{CCI_REG8(0x479E), 0xCB}, {CCI_REG8(0x47A4), 0x7D}, {CCI_REG8(0x47A6), 0x8E},
	{CCI_REG8(0x47A8), 0xB4}, {CCI_REG8(0x47A9), 0x00}, {CCI_REG8(0x47AA), 0xC0},
	{CCI_REG8(0x47AC), 0xFA}, {CCI_REG8(0x47AE), 0x0D}, {CCI_REG8(0x47B0), 0x31},
	{CCI_REG8(0x47B1), 0x01}, {CCI_REG8(0x47B2), 0x4A}, {CCI_REG8(0x47B3), 0x01},
	{CCI_REG8(0x47B4), 0x3F}, {CCI_REG8(0x47B6), 0x49}, {CCI_REG8(0x47BC), 0xFB},
	{CCI_REG8(0x47BE), 0x0C}, {CCI_REG8(0x47C0), 0x32}, {CCI_REG8(0x47C1), 0x01},
	{CCI_REG8(0x47C2), 0x3E}, {CCI_REG8(0x47C3), 0x01}, {IMX678_REG_WDMODE, 0x00},
	{IMX678_REG_MDBIT, 0x01}, {IMX678_REG_XXS_DRV, 0x00},
};

static const u32 codes_normal[] = {
	MEDIA_BUS_FMT_SRGGB12_1X12,
};

static const u32 mono_codes[] = {
	MEDIA_BUS_FMT_Y12_1X12,   /* 12-bit mono */
};

static const char * const imx678_supply_name[] = {
	"avdd",  /* Analog (3.3V) supply */
	"dvdd",  /* Digital Core (1.1V) supply */
	"ovdd",  /* IF (1.8V) supply */
};

struct imx678 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *cci;

	enum imx678_type type;

	struct clk *xclk;
	u32 xclk_freq;

	/* chosen INCK_SEL register value */
	u8  inck_sel_val;

	/* Link configurations */
	unsigned int lane_count;
	unsigned int link_freq_idx;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx678_supply_name)];

	struct v4l2_ctrl_handler ctrl_handler;

	/* V4L2 Controls */
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;

	/* Tracking sensor VMAX/HMAX value */
	u16 HMAX;
	u32 VMAX;

	/* Rewrite common registers on stream on? */
	bool common_regs_written;
};

static inline struct imx678 *to_imx678(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx678, sd);
}

static inline struct v4l2_mbus_framefmt *
imx678_state_format(struct v4l2_subdev_state *state)
{
	return v4l2_subdev_state_get_format(state, 0);
}

static inline struct v4l2_rect *imx678_state_crop(struct v4l2_subdev_state *state)
{
	return v4l2_subdev_state_get_crop(state, 0);
}

static bool imx678_state_binning(struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format = imx678_state_format(state);
	const struct v4l2_rect *crop = imx678_state_crop(state);

	return crop->width == 2 * format->width &&
	       crop->height == 2 * format->height;
}

static const u32 *imx678_mbus_codes(struct imx678 *imx678,
				    unsigned int *num_codes)
{
	if (imx678->type == IMX678_MONOCHROME) {
		*num_codes = ARRAY_SIZE(mono_codes);
		return mono_codes;
	}

	*num_codes = ARRAY_SIZE(codes_normal);
	return codes_normal;
}

static u32 imx678_default_mbus_code(struct imx678 *imx678)
{
	unsigned int num_codes;
	const u32 *codes = imx678_mbus_codes(imx678, &num_codes);

	return codes[0];
}

static bool imx678_mbus_code_supported(struct imx678 *imx678, u32 code)
{
	unsigned int i, num_codes;
	const u32 *codes = imx678_mbus_codes(imx678, &num_codes);

	for (i = 0; i < num_codes; i++) {
		if (codes[i] == code)
			return true;
	}

	return false;
}

static u32 imx678_get_format_code(struct imx678 *imx678, u32 code)
{
	if (imx678_mbus_code_supported(imx678, code))
		return code;

	return imx678_default_mbus_code(imx678);
}

/*
 * Preserve FoV, default to 2x binning when feasible
 */
static bool imx678_pick_binning(u32 width, u32 height)
{
	return 2 * width <= imx678_active_area.width &&
	       2 * height <= imx678_active_area.height;
}

/*
 * Align width to 16 and height to 4 as the sensor crop requires it
 */
static void imx678_snap_format(u32 *width, u32 *height, bool binning)
{
	const u32 scale = binning ? 2 : 1;
	const u32 max_w = imx678_active_area.width / scale;
	const u32 max_h = imx678_active_area.height / scale;
	const u32 w_align = IMX678_CROP_HWIDTH_ALIGN / scale;
	const u32 h_align = IMX678_CROP_VWIDTH_ALIGN / scale;

	*width = clamp_t(u32, ALIGN(*width, w_align),
			 IMX678_PIXEL_ARRAY_MIN_WIDTH, max_w);
	*height = clamp_t(u32, ALIGN(*height, h_align),
			  IMX678_PIXEL_ARRAY_MIN_HEIGHT, max_h);
}

/*
 * Compute a centered analog crop rectangle of size (width, height) * scale,
 * with origin aligned to the windowing register granularity.
 */
static void imx678_default_crop_for_format(u32 width, u32 height, bool binning,
					   struct v4l2_rect *crop)
{
	const u32 scale = binning ? 2 : 1;

	crop->width = width * scale;
	crop->height = height * scale;
	crop->left = imx678_active_area.left +
		     round_down((imx678_active_area.width - crop->width) / 2,
				IMX678_CROP_HST_ALIGN);
	crop->top = imx678_active_area.top +
		    round_down((imx678_active_area.height - crop->height) / 2,
			       IMX678_CROP_VST_ALIGN);
}

static u64 imx678_output_pixel_rate(struct imx678 *imx678)
{
	const u32 lane_count = imx678->lane_count;
	const u64 link_freq = link_freqs[imx678->link_freq_idx];
	const u8 bpp = 12;
	u64 numerator = link_freq * 2 * lane_count;

	do_div(numerator, bpp);

	return numerator;
}

/*
 * Sensor HMAX is in internal clock cycles @ 74.25 Mhz
 * Convert to the output pixel rate
 */
static u64 imx678_iclk_to_pix(u32 pixel_rate, u32 cycles)
{
	const u32 iclk = 74250;
	const u32 pixclk = pixel_rate / HZ_PER_KHZ;
	u64 numerator = cycles * pixclk;

	return DIV_ROUND_CLOSEST_ULL(numerator, iclk);
}

/*
 * HBLANK control is in units of pixels
 * Convert to HMAX register units (@ internal 74.25 Mhz)
 */
static u64 imx678_pix_to_iclk(u32 pixel_rate, u32 pixels)
{
	const u32 iclk = 74250;
	const u32 pixclk = pixel_rate / HZ_PER_KHZ;
	u64 numerator = pixels * iclk;

	return DIV_ROUND_CLOSEST_ULL(numerator, pixclk);
}

static void imx678_set_framing_limits(struct imx678 *imx678,
				      struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format = imx678_state_format(state);
	u64 min_hblank, default_hblank, max_hblank, vblank, pixel_rate;
	const u32 hmax_4lane = min_hmax_4lane[imx678->link_freq_idx];
	const u32 lane_scale = imx678->lane_count == 2 ? 2 : 1;
	const bool binning = imx678_state_binning(state);
	u32 bpp, min_hmax;

	imx678->VMAX = IMX678_VMAX_DEFAULT;
	imx678->HMAX = hmax_4lane * lane_scale;

	pixel_rate = imx678_output_pixel_rate(imx678);
	__v4l2_ctrl_modify_range(imx678->pixel_rate, pixel_rate, pixel_rate, 1,
				 pixel_rate);

	/* HMAX can go lower when using 10bit AD for binning */
	bpp = binning ? 10 : 12;
	min_hmax = (imx678->HMAX * bpp) / 12;
	min_hblank = imx678_iclk_to_pix(pixel_rate, min_hmax) - format->width;
	default_hblank = imx678_iclk_to_pix(pixel_rate, imx678->HMAX) - format->width;
	max_hblank = imx678_iclk_to_pix(pixel_rate, IMX678_HMAX_MAX) - format->width;

	__v4l2_ctrl_modify_range(imx678->hblank, min_hblank, max_hblank, 1,
				 default_hblank);
	__v4l2_ctrl_s_ctrl(imx678->hblank, default_hblank);

	vblank = imx678->VMAX - format->height;
	__v4l2_ctrl_modify_range(imx678->vblank, vblank,
				 IMX678_VMAX_MAX - format->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(imx678->vblank, IMX678_VMAX_DEFAULT - format->height);

	__v4l2_ctrl_modify_range(imx678->exposure, IMX678_EXPOSURE_MIN,
				 imx678->VMAX - IMX678_SHR_MIN, 1,
				 IMX678_EXPOSURE_DEFAULT);
}

static int imx678_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx678 *imx678 = container_of(ctrl->handler, struct imx678, ctrl_handler);
	struct v4l2_subdev_state *state;
	struct i2c_client *client = v4l2_get_subdevdata(&imx678->sd);
	const struct v4l2_mbus_framefmt *format;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&imx678->sd);
	format = imx678_state_format(state);

	if (ctrl->id == V4L2_CID_VBLANK) {
		u32 current_exposure = imx678->exposure->cur.val;

		imx678->VMAX = (format->height + ctrl->val) & ~1u;

		current_exposure = clamp_t(u32, current_exposure, IMX678_EXPOSURE_MIN,
					   imx678->VMAX - IMX678_SHR_MIN);
		__v4l2_ctrl_modify_range(imx678->exposure, IMX678_EXPOSURE_MIN,
					 imx678->VMAX - IMX678_SHR_MIN, 1,
					 current_exposure);
	}

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE: {
		u32 shr = (imx678->VMAX - ctrl->val) & ~1u;

		cci_write(imx678->cci, IMX678_REG_SHR, shr, &ret);
		break;
	}
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(imx678->cci, IMX678_REG_ANALOG_GAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK: {
		cci_write(imx678->cci, IMX678_REG_VMAX, imx678->VMAX, &ret);
		break;
	}
	case V4L2_CID_HBLANK: {
		u64 pixel_rate = imx678_output_pixel_rate(imx678);
		u32 hmax = format->width + ctrl->val;

		hmax = imx678_pix_to_iclk(pixel_rate, hmax);

		imx678->HMAX = hmax;

		cci_write(imx678->cci, IMX678_REG_HMAX, hmax, &ret);
		break;
	}
	case V4L2_CID_TEST_PATTERN: {
		cci_write(imx678->cci, IMX678_REG_TPG_COLORWIDTH,
			  IMX678_TPG_COLORWIDTH_160PIX, &ret);
		cci_write(imx678->cci, IMX678_REG_TPG_PATSEL_DUOUT,
			  imx678_tpg_val[ctrl->val], &ret);
		cci_write(imx678->cci, IMX678_REG_TPG_EN_DUOUT, (ctrl->val) ? 1 : 0,
			  &ret);
		break;
	}
	case V4L2_CID_HFLIP:
		cci_write(imx678->cci, IMX678_REG_WINMODEH, ctrl->val, &ret);
		break;
	case V4L2_CID_VFLIP:
		cci_write(imx678->cci, IMX678_REG_WINMODEV, ctrl->val, &ret);
		break;
	default:
		dev_warn(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx678_ctrl_ops = {
	.s_ctrl = imx678_set_ctrl,
};

static int imx678_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx678 *imx678 = to_imx678(sd);
	unsigned int num_codes;
	const u32 *codes = imx678_mbus_codes(imx678, &num_codes);

	if (code->index >= num_codes)
		return -EINVAL;

	code->code = codes[code->index];
	return 0;
}

static int imx678_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx678 *imx678 = to_imx678(sd);

	if (fse->index > 0)
		return -EINVAL;

	if (!imx678_mbus_code_supported(imx678, fse->code))
		return -EINVAL;

	fse->min_width = IMX678_PIXEL_ARRAY_MIN_WIDTH;
	fse->max_width = imx678_active_area.width;
	fse->min_height = IMX678_PIXEL_ARRAY_MIN_HEIGHT;
	fse->max_height = imx678_active_area.height;

	return 0;
}

static void imx678_reset_colorspace(struct v4l2_mbus_framefmt *fmt)
{
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
							  fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
}

static int imx678_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx678 *imx678 = to_imx678(sd);
	struct v4l2_mbus_framefmt *framefmt;
	struct v4l2_rect *crop;
	u32 width = fmt->format.width;
	u32 height = fmt->format.height;
	bool binning;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	fmt->format.code = imx678_get_format_code(imx678, fmt->format.code);
	binning = imx678_pick_binning(width, height);
	imx678_snap_format(&width, &height, binning);

	fmt->format.width = width;
	fmt->format.height = height;
	fmt->format.field = V4L2_FIELD_NONE;
	imx678_reset_colorspace(&fmt->format);

	framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
	*framefmt = fmt->format;

	crop = imx678_state_crop(sd_state);
	imx678_default_crop_for_format(width, height, binning, crop);

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		imx678_set_framing_limits(imx678, sd_state);

	return 0;
}

static int imx678_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx678 *imx678 = to_imx678(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = imx678_default_mbus_code(imx678),
			.width = imx678_active_area.width,
			.height = imx678_active_area.height,
		},
	};

	imx678_set_pad_format(sd, state, &fmt);

	return 0;
}

static int imx678_write_common(struct imx678 *imx678)
{
	int ret = 0;

	if (imx678->common_regs_written)
		return 0;

	cci_multi_reg_write(imx678->cci, common_regs, ARRAY_SIZE(common_regs), &ret);

	cci_write(imx678->cci, IMX678_REG_INCK_SEL, imx678->inck_sel_val, &ret);
	cci_write(imx678->cci, IMX678_REG_DATARATE_SEL,
		  link_freqs_reg_value[imx678->link_freq_idx], &ret);

	if (imx678->lane_count == 2)
		cci_write(imx678->cci, IMX678_REG_LANEMODE, 0x01, &ret);
	else
		cci_write(imx678->cci, IMX678_REG_LANEMODE, 0x03, &ret);

	cci_write(imx678->cci, IMX678_REG_INTERFACE_SEL, IMX678_INTERFACE_2L_4L,
		  &ret);

	if (!ret)
		imx678->common_regs_written = true;

	return ret;
}

static int imx678_program_window(struct imx678 *imx678,
				 const struct v4l2_rect *crop, bool binning)
{
	int ret = 0;

	cci_write(imx678->cci, IMX678_REG_ADDMODE, binning ? 0x01 : 0x00, &ret);
	cci_write(imx678->cci, IMX678_REG_WINMODE,
		  v4l2_rect_equal(crop, &imx678_active_area) ? 0x00 : 0x04,
		  &ret);
	cci_write(imx678->cci, IMX678_REG_PIX_HST,
		  crop->left - imx678_active_area.left, &ret);
	cci_write(imx678->cci, IMX678_REG_PIX_HWIDTH, crop->width, &ret);
	cci_write(imx678->cci, IMX678_REG_PIX_VST,
		  crop->top - imx678_active_area.top, &ret);
	cci_write(imx678->cci, IMX678_REG_PIX_VWIDTH, crop->height, &ret);
	cci_write(imx678->cci, IMX678_REG_ADBIT, binning ? 0x00 : 0x01, &ret);

	return ret;
}

static int imx678_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct imx678 *imx678 = to_imx678(sd);
	const struct v4l2_rect *crop = imx678_state_crop(state);
	const bool binning = imx678_state_binning(state);
	int ret = 0;

	ret = pm_runtime_get_sync(&client->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(&client->dev);
		goto err_rpm_put;
	}

	ret = imx678_write_common(imx678);
	if (ret) {
		dev_err(&client->dev, "%s failed to write registers\n", __func__);
		goto err_rpm_put;
	}

	ret = imx678_program_window(imx678, crop, binning);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		goto err_rpm_put;
	}

	ret = __v4l2_ctrl_handler_setup(imx678->sd.ctrl_handler);
	if (ret) {
		dev_err(&client->dev, "%s failed to apply user values\n", __func__);
		goto err_rpm_put;
	}

	cci_write(imx678->cci, IMX678_REG_MODE_SELECT, IMX678_MODE_STREAMING, &ret);
	usleep_range(IMX678_STREAM_DELAY_US, IMX678_STREAM_DELAY_US +
		     IMX678_STREAM_DELAY_RANGE_US);
	cci_write(imx678->cci, IMX678_REG_XMSTA, 0x00, &ret);

	if (ret) {
		dev_err(&client->dev, "%s failed to start streaming\n", __func__);
		goto err_rpm_put;
	}

	return 0;

err_rpm_put:
	pm_runtime_put(&client->dev);

	return ret;
}

static int imx678_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 mask)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct imx678 *imx678 = to_imx678(sd);
	int ret = 0;

	/* Master mode disable */
	cci_write(imx678->cci, IMX678_REG_XMSTA, 0x01, &ret);
	/* Standby */
	cci_write(imx678->cci, IMX678_REG_MODE_SELECT, IMX678_MODE_STANDBY, &ret);
	if (ret)
		dev_err(&client->dev, "%s failed to stop stream\n", __func__);

	pm_runtime_put(&client->dev);

	return ret;
}

static int imx678_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx678 *imx678 = to_imx678(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx678_supply_name), imx678->supplies);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	usleep_range(500, 550); /* Tlow */

	gpiod_set_value_cansleep(imx678->reset_gpio, 1);

	ret = clk_prepare_enable(imx678->xclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n",
			__func__);
		goto reg_off;
	}

	usleep_range(20, 22); /* T4 */

	return 0;

reg_off:
	regulator_bulk_disable(ARRAY_SIZE(imx678_supply_name), imx678->supplies);
	return ret;
}

static int imx678_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx678 *imx678 = to_imx678(sd);

	gpiod_set_value_cansleep(imx678->reset_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(imx678_supply_name), imx678->supplies);
	clk_disable_unprepare(imx678->xclk);

	/* Force reprogramming of the common registers when powered up again. */
	imx678->common_regs_written = false;

	return 0;
}

static int imx678_get_regulators(struct imx678 *imx678)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx678->sd);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(imx678_supply_name); i++)
		imx678->supplies[i].supply = imx678_supply_name[i];

	return devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(imx678_supply_name),
				       imx678->supplies);
}

static int imx678_detect(struct imx678 *imx678)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx678->sd);
	int ret = 0;
	u64 val = 0;

	/*
	 * This sensor's ID registers become accessible 80ms after coming out
	 * of STANDBY mode.
	 */
	cci_write(imx678->cci, IMX678_REG_MODE_SELECT, 0, &ret);
	usleep_range(IMX678_MODULE_ID_DELAY, IMX678_MODULE_ID_DELAY +
		     IMX678_MODULE_ID_DELAY_RANGE);

	cci_read(imx678->cci, IMX678_REG_MODULE_ID, &val, &ret);

	if (val != IMX678_ID) {
		dev_err(&client->dev,
			"Chip ID mismatch: %x!=%llx\n", IMX678_ID, val);
		return -ENXIO;
	}

	cci_read(imx678->cci, IMX678_REG_MONOCHROME, &val, &ret);

	imx678->type = val & IMX678_TYPE;

	if (ret) {
		dev_err(&client->dev,
			"I2C transaction failed ret = %d\n", ret);
		return ret;
	}

	return 0;
}

static int imx678_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *imx678_state_crop(sd_state);
		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = imx678_native_area;
		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = imx678_active_area;
		return 0;
	}

	return -EINVAL;
}

/*
 * Only two analog crop sizes are valid for a given output format:
 *   - crop = format         (no binning)
 *   - crop = 2 x format     (2x2 binning, when it fits)
 *
 * Snap (and not reject) the request: pick the nearer of the two sizes (biased
 * by V4L2_SEL_FLAG_{LE,GE}), align the position to PIX_HST/VST granularity, and
 * clamp into the active area.
 */
static int imx678_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct imx678 *imx678 = to_imx678(sd);
	const struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;
	bool prefer_2x = false;

	if (sel->target != V4L2_SEL_TGT_CROP || sel->pad != 0)
		return -EINVAL;

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	format = v4l2_subdev_state_get_format(sd_state, sel->pad);
	crop = v4l2_subdev_state_get_crop(sd_state, sel->pad);

	/* Current format can support 2x2 binning */
	if (imx678_pick_binning(format->width, format->height)) {
		if (sel->flags & V4L2_SEL_FLAG_LE)
			/* Prefer lower rectangle */
			prefer_2x = sel->r.width  >= 2 * format->width &&
				    sel->r.height >= 2 * format->height;
		else if (sel->flags & V4L2_SEL_FLAG_GE)
			/* Prefer bigger rectangle */
			prefer_2x = (sel->r.width  > format->width ||
				     sel->r.height > format->height);
		else
			/* Snap to closest rectangle */
			prefer_2x = 2 * sel->r.width  >= 3 * format->width &&
				    2 * sel->r.height >= 3 * format->height;
	}

	crop->width  = format->width  * (prefer_2x ? 2 : 1);
	crop->height = format->height * (prefer_2x ? 2 : 1);
	crop->left = imx678_active_area.left +
		     ALIGN(sel->r.left - imx678_active_area.left,
			   IMX678_CROP_HST_ALIGN);
	crop->top  = imx678_active_area.top  +
		     ALIGN(sel->r.top  - imx678_active_area.top,
			   IMX678_CROP_VST_ALIGN);

	/* This is safe to do because width/height are also 4-aligned */
	v4l2_rect_map_inside(crop, &imx678_active_area);

	sel->r = *crop;

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		imx678_set_framing_limits(imx678, sd_state);

	return 0;
}

static const struct v4l2_subdev_core_ops imx678_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops imx678_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx678_pad_ops = {
	.enum_mbus_code = imx678_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx678_set_pad_format,
	.get_selection = imx678_get_selection,
	.set_selection = imx678_set_selection,
	.enum_frame_size = imx678_enum_frame_size,
	.enable_streams = imx678_enable_streams,
	.disable_streams = imx678_disable_streams,
};

static const struct v4l2_subdev_ops imx678_subdev_ops = {
	.core = &imx678_core_ops,
	.video = &imx678_video_ops,
	.pad = &imx678_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx678_internal_ops = {
	.init_state = imx678_init_state,
};

static int imx678_init_controls(struct imx678 *imx678)
{
	struct v4l2_ctrl_handler *ctrl_hdlr;
	const u32 hmax_4lane = min_hmax_4lane[imx678->link_freq_idx];
	const u32 lane_scale = imx678->lane_count == 2 ? 2 : 1;
	struct i2c_client *client = v4l2_get_subdevdata(&imx678->sd);
	struct v4l2_fwnode_device_properties props;
	u32 pixel_rate, hblank, max_hblank;
	int ret;

	ctrl_hdlr = &imx678->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 32);
	if (ret)
		return ret;

	imx678->VMAX = IMX678_VMAX_DEFAULT;
	imx678->HMAX = hmax_4lane * lane_scale;

	pixel_rate = imx678_output_pixel_rate(imx678);

	/* By default, PIXEL_RATE is read only */
	imx678->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx678_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       pixel_rate,
					       pixel_rate, 1,
					       pixel_rate);

	/* LINK_FREQ is also read only */
	imx678->link_freq =
		v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx678_ctrl_ops,
				       V4L2_CID_LINK_FREQ, 0, 0,
				       &link_freqs[imx678->link_freq_idx]);
	if (imx678->link_freq)
		imx678->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx678->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx678_ctrl_ops, V4L2_CID_VBLANK,
					   imx678->VMAX - imx678_active_area.height,
					   IMX678_VMAX_MAX - imx678_active_area.height, 1,
					   imx678->VMAX - imx678_active_area.height);

	hblank = imx678_iclk_to_pix(pixel_rate, imx678->HMAX) - imx678_active_area.width;
	max_hblank = imx678_iclk_to_pix(pixel_rate, IMX678_HMAX_MAX) - imx678_active_area.width;
	imx678->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx678_ctrl_ops, V4L2_CID_HBLANK,
					   hblank, max_hblank, 1, hblank);

	imx678->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx678_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX678_EXPOSURE_MIN,
					     IMX678_VMAX_DEFAULT -
					     IMX678_SHR_MIN,
					     IMX678_EXPOSURE_STEP,
					     IMX678_EXPOSURE_DEFAULT);

	imx678->gain = v4l2_ctrl_new_std(ctrl_hdlr, &imx678_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
					 IMX678_ANA_GAIN_MIN_NORMAL, IMX678_ANA_GAIN_MAX_NORMAL,
					 IMX678_ANA_GAIN_STEP, IMX678_ANA_GAIN_DEFAULT);

	imx678->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx678_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	imx678->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx678_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx678_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx678_tpg_menu) - 1, 0, 0,
				     imx678_tpg_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx678_ctrl_ops, &props);
	if (ret)
		goto error;

	imx678->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static void imx678_free_controls(struct imx678 *imx678)
{
	v4l2_ctrl_handler_free(imx678->sd.ctrl_handler);
}

static const struct of_device_id imx678_dt_ids[] = {
	{ .compatible = "sony,imx678"},
	{ /* sentinel */ }
};

static int imx678_check_hwcfg(struct device *dev, struct imx678 *imx678)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = -EINVAL;
	int i;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err(dev, "could not parse endpoint\n");
		goto error_out;
	}

	/* Check the number of MIPI CSI2 data lanes */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 2 && ep_cfg.bus.mipi_csi2.num_data_lanes != 4) {
		dev_err(dev, "only 2 or 4 data lanes are currently supported\n");
		goto error_out;
	}
	imx678->lane_count = ep_cfg.bus.mipi_csi2.num_data_lanes;

	/* Check the link frequency set in device tree */
	if (!ep_cfg.nr_of_link_frequencies) {
		dev_err(dev, "link-frequency property not found in DT\n");
		goto error_out;
	}

	for (i = 0; i < ARRAY_SIZE(link_freqs); i++) {
		if (link_freqs[i] == ep_cfg.link_frequencies[0]) {
			imx678->link_freq_idx = i;
			break;
		}
	}

	if (i == ARRAY_SIZE(link_freqs)) {
		dev_err(dev, "Link frequency not supported: %lld\n",
			ep_cfg.link_frequencies[0]);
			ret = -EINVAL;
			goto error_out;
	}

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int imx678_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx678 *imx678;
	const struct of_device_id *match;
	int ret, i;

	imx678 = devm_kzalloc(&client->dev, sizeof(*imx678), GFP_KERNEL);
	if (!imx678)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&imx678->sd, client, &imx678_subdev_ops);

	imx678->cci = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx678->cci))
		return dev_err_probe(dev, PTR_ERR(imx678->cci),
				     "failed to init CCI\n");

	match = of_match_device(imx678_dt_ids, dev);
	if (!match)
		return -ENODEV;

	if (imx678_check_hwcfg(dev, imx678))
		return -EINVAL;

	imx678->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(imx678->xclk))
		return dev_err_probe(dev, PTR_ERR(imx678->xclk),
				     "failed to get xclk\n");

	imx678->xclk_freq = clk_get_rate(imx678->xclk);

	for (i = 0; i < ARRAY_SIZE(imx678_inck_table); ++i) {
		if (imx678_inck_table[i].xclk_hz == imx678->xclk_freq) {
			imx678->inck_sel_val = imx678_inck_table[i].inck_sel;
			break;
		}
	}

	if (i == ARRAY_SIZE(imx678_inck_table))
		return dev_err_probe(dev, -EINVAL,
				     "unsupported XCLK rate %u Hz\n",
				     imx678->xclk_freq);

	ret = imx678_get_regulators(imx678);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	imx678->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx678->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(imx678->reset_gpio),
				     "failed to get reset GPIO\n");

	ret = imx678_power_on(dev);
	if (ret)
		return ret;

	ret = imx678_detect(imx678);
	if (ret)
		goto error_power_off;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	ret = imx678_init_controls(imx678);
	if (ret)
		goto error_pm_runtime;

	imx678->sd.internal_ops = &imx678_internal_ops;
	imx678->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
				V4L2_SUBDEV_FL_HAS_EVENTS;
	imx678->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	imx678->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx678->sd.entity, 1, &imx678->pad);
	if (ret) {
		dev_err(dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	imx678->sd.state_lock = imx678->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&imx678->sd);
	if (ret < 0) {
		dev_err(dev, "subdev init error\n");
		goto error_media_entity;
	}

	ret = v4l2_async_register_subdev_sensor(&imx678->sd);
	if (ret < 0) {
		dev_err(dev, "failed to register sensor sub-device: %d\n", ret);
		goto error_subdev_cleanup;
	}

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&imx678->sd);

error_media_entity:
	media_entity_cleanup(&imx678->sd.entity);

error_handler_free:
	imx678_free_controls(imx678);

error_pm_runtime:
	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);

error_power_off:
	imx678_power_off(&client->dev);

	return ret;
}

static void imx678_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx678 *imx678 = to_imx678(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	imx678_free_controls(imx678);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx678_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

MODULE_DEVICE_TABLE(of, imx678_dt_ids);

static const struct dev_pm_ops imx678_pm_ops = {
	SET_RUNTIME_PM_OPS(imx678_power_off, imx678_power_on, NULL)
};

static struct i2c_driver imx678_i2c_driver = {
	.driver = {
		.name = "imx678",
		.of_match_table = imx678_dt_ids,
		.pm = &imx678_pm_ops,
	},
	.probe = imx678_probe,
	.remove = imx678_remove,
};

module_i2c_driver(imx678_i2c_driver);

MODULE_AUTHOR("Will Whang <will@willwhang.com>");
MODULE_AUTHOR("Tetsuya NOMURA <tetsuya.nomura@soho-enterprise.com>");
MODULE_AUTHOR("Jai Luthra <jai.luthra@ideasonboard.com>");
MODULE_DESCRIPTION("Sony imx678 sensor driver");
MODULE_LICENSE("GPL");
