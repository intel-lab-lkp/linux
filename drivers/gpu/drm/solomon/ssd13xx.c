// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for Solomon SSD13xx OLED displays
 *
 * Copyright 2022 Red Hat Inc.
 * Author: Javier Martinez Canillas <javierm@redhat.com>
 *
 * Based on drivers/video/fbdev/ssd1307fb.c
 * Copyright 2012 Free Electrons
 */

#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_rect.h>
#include <drm/drm_probe_helper.h>

#include "ssd13xx.h"

#define DRIVER_NAME	"ssd13xx"
#define DRIVER_DESC	"DRM driver for Solomon SSD13xx OLED displays"
#define DRIVER_DATE	"20220131"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

#define SSD130X_PAGE_HEIGHT 8

/* ssd13xx commands */
#define SSD13XX_CONTRAST			0x81
#define SSD13XX_SET_SEG_REMAP			0xa0
#define SSD13XX_SET_MULTIPLEX_RATIO		0xa8
#define SSD13XX_DISPLAY_OFF			0xae
#define SSD13XX_DISPLAY_ON			0xaf

#define SSD13XX_SET_SEG_REMAP_MASK		GENMASK(0, 0)
#define SSD13XX_SET_SEG_REMAP_SET(val)		FIELD_PREP(SSD13XX_SET_SEG_REMAP_MASK, (val))

/* ssd130x commands */
#define SSD130X_PAGE_COL_START_LOW		0x00
#define SSD130X_PAGE_COL_START_HIGH		0x10
#define SSD130X_SET_ADDRESS_MODE		0x20
#define SSD130X_SET_COL_RANGE			0x21
#define SSD130X_SET_PAGE_RANGE			0x22
#define SSD130X_SET_LOOKUP_TABLE		0x91
#define SSD130X_CHARGE_PUMP			0x8d
#define SSD130X_START_PAGE_ADDRESS		0xb0
#define SSD130X_SET_COM_SCAN_DIR		0xc0
#define SSD130X_SET_DISPLAY_OFFSET		0xd3
#define SSD130X_SET_CLOCK_FREQ			0xd5
#define SSD130X_SET_AREA_COLOR_MODE		0xd8
#define SSD130X_SET_PRECHARGE_PERIOD		0xd9
#define SSD130X_SET_COM_PINS_CONFIG		0xda
#define SSD130X_SET_VCOMH			0xdb

/* ssd130x commands accessors */
#define SSD130X_PAGE_COL_START_MASK		GENMASK(3, 0)
#define SSD130X_PAGE_COL_START_HIGH_SET(val)	FIELD_PREP(SSD130X_PAGE_COL_START_MASK, (val) >> 4)
#define SSD130X_PAGE_COL_START_LOW_SET(val)	FIELD_PREP(SSD130X_PAGE_COL_START_MASK, (val))
#define SSD130X_START_PAGE_ADDRESS_MASK		GENMASK(2, 0)
#define SSD130X_START_PAGE_ADDRESS_SET(val)	FIELD_PREP(SSD130X_START_PAGE_ADDRESS_MASK, (val))
#define SSD130X_SET_COM_SCAN_DIR_MASK		GENMASK(3, 3)
#define SSD130X_SET_COM_SCAN_DIR_SET(val)	FIELD_PREP(SSD130X_SET_COM_SCAN_DIR_MASK, (val))
#define SSD130X_SET_CLOCK_DIV_MASK		GENMASK(3, 0)
#define SSD130X_SET_CLOCK_DIV_SET(val)		FIELD_PREP(SSD130X_SET_CLOCK_DIV_MASK, (val))
#define SSD130X_SET_CLOCK_FREQ_MASK		GENMASK(7, 4)
#define SSD130X_SET_CLOCK_FREQ_SET(val)		FIELD_PREP(SSD130X_SET_CLOCK_FREQ_MASK, (val))
#define SSD130X_SET_PRECHARGE_PERIOD1_MASK	GENMASK(3, 0)
#define SSD130X_SET_PRECHARGE_PERIOD1_SET(val)	FIELD_PREP(SSD130X_SET_PRECHARGE_PERIOD1_MASK, (val))
#define SSD130X_SET_PRECHARGE_PERIOD2_MASK	GENMASK(7, 4)
#define SSD130X_SET_PRECHARGE_PERIOD2_SET(val)	FIELD_PREP(SSD130X_SET_PRECHARGE_PERIOD2_MASK, (val))
#define SSD130X_SET_COM_PINS_CONFIG1_MASK	GENMASK(4, 4)
#define SSD130X_SET_COM_PINS_CONFIG1_SET(val)	FIELD_PREP(SSD130X_SET_COM_PINS_CONFIG1_MASK, (val))
#define SSD130X_SET_COM_PINS_CONFIG2_MASK	GENMASK(5, 5)
#define SSD130X_SET_COM_PINS_CONFIG2_SET(val)	FIELD_PREP(SSD130X_SET_COM_PINS_CONFIG2_MASK, (val))

#define SSD130X_SET_ADDRESS_MODE_HORIZONTAL	0x00
#define SSD130X_SET_ADDRESS_MODE_VERTICAL	0x01
#define SSD130X_SET_ADDRESS_MODE_PAGE		0x02

#define SSD130X_SET_AREA_COLOR_MODE_ENABLE	0x1e
#define SSD130X_SET_AREA_COLOR_MODE_LOW_POWER	0x05

#define MAX_CONTRAST 255

const struct ssd13xx_deviceinfo ssd13xx_variants[] = {
	[SH1106_ID] = {
		.default_vcomh = 0x40,
		.default_dclk_div = 1,
		.default_dclk_frq = 5,
		.default_width = 132,
		.default_height = 64,
		.page_mode_only = 1,
		.family_id = SSD130X_FAMILY,
	},
	[SSD1305_ID] = {
		.default_vcomh = 0x34,
		.default_dclk_div = 1,
		.default_dclk_frq = 7,
		.default_width = 132,
		.default_height = 64,
		.family_id = SSD130X_FAMILY,
	},
	[SSD1306_ID] = {
		.default_vcomh = 0x20,
		.default_dclk_div = 1,
		.default_dclk_frq = 8,
		.need_chargepump = 1,
		.default_width = 128,
		.default_height = 64,
		.family_id = SSD130X_FAMILY,
	},
	[SSD1307_ID] = {
		.default_vcomh = 0x20,
		.default_dclk_div = 2,
		.default_dclk_frq = 12,
		.need_pwm = 1,
		.default_width = 128,
		.default_height = 39,
		.family_id = SSD130X_FAMILY,
	},
	[SSD1309_ID] = {
		.default_vcomh = 0x34,
		.default_dclk_div = 1,
		.default_dclk_frq = 10,
		.default_width = 128,
		.default_height = 64,
		.family_id = SSD130X_FAMILY,
	}
};
EXPORT_SYMBOL_NS_GPL(ssd13xx_variants, DRM_SSD13XX);

struct ssd13xx_crtc_state {
	struct drm_crtc_state base;
	/* Buffer to store pixels in HW format and written to the panel */
	u8 *data_array;
};

struct ssd13xx_plane_state {
	struct drm_shadow_plane_state base;
	/* Intermediate buffer to convert pixels from XRGB8888 to HW format */
	u8 *buffer;
};

static inline struct ssd13xx_crtc_state *to_ssd13xx_crtc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct ssd13xx_crtc_state, base);
}

static inline struct ssd13xx_plane_state *to_ssd13xx_plane_state(struct drm_plane_state *state)
{
	return container_of(state, struct ssd13xx_plane_state, base.base);
}

static inline struct ssd13xx_device *drm_to_ssd13xx(struct drm_device *drm)
{
	return container_of(drm, struct ssd13xx_device, drm);
}

/*
 * Helper to write data (SSD13XX_DATA) to the device.
 */
static int ssd13xx_write_data(struct ssd13xx_device *ssd13xx, u8 *values, int count)
{
	return regmap_bulk_write(ssd13xx->regmap, SSD13XX_DATA, values, count);
}

/*
 * Helper to write command (SSD13XX_COMMAND). The fist variadic argument
 * is the command to write and the following are the command options.
 *
 * Note that the ssd13xx protocol requires each command and option to be
 * written as a SSD13XX_COMMAND device register value. That is why a call
 * to regmap_write(..., SSD13XX_COMMAND, ...) is done for each argument.
 */
static int ssd13xx_write_cmd(struct ssd13xx_device *ssd13xx, int count,
			     /* u8 cmd, u8 option, ... */...)
{
	va_list ap;
	u8 value;
	int ret;

	va_start(ap, count);

	do {
		value = va_arg(ap, int);
		ret = regmap_write(ssd13xx->regmap, SSD13XX_COMMAND, value);
		if (ret)
			goto out_end;
	} while (--count);

out_end:
	va_end(ap);

	return ret;
}

/* Set address range for horizontal/vertical addressing modes */
static int ssd13xx_set_col_range(struct ssd13xx_device *ssd13xx,
				 u8 col_start, u8 cols)
{
	u8 col_end = col_start + cols - 1;
	int ret;

	if (col_start == ssd13xx->col_start && col_end == ssd13xx->col_end)
		return 0;

	ret = ssd13xx_write_cmd(ssd13xx, 3, SSD130X_SET_COL_RANGE, col_start, col_end);
	if (ret < 0)
		return ret;

	ssd13xx->col_start = col_start;
	ssd13xx->col_end = col_end;
	return 0;
}

static int ssd13xx_set_page_range(struct ssd13xx_device *ssd13xx,
				  u8 page_start, u8 pages)
{
	u8 page_end = page_start + pages - 1;
	int ret;

	if (page_start == ssd13xx->page_start && page_end == ssd13xx->page_end)
		return 0;

	ret = ssd13xx_write_cmd(ssd13xx, 3, SSD130X_SET_PAGE_RANGE, page_start, page_end);
	if (ret < 0)
		return ret;

	ssd13xx->page_start = page_start;
	ssd13xx->page_end = page_end;
	return 0;
}

/* Set page and column start address for page addressing mode */
static int ssd13xx_set_page_pos(struct ssd13xx_device *ssd13xx,
				u8 page_start, u8 col_start)
{
	int ret;
	u32 page, col_low, col_high;

	page = SSD130X_START_PAGE_ADDRESS |
	       SSD130X_START_PAGE_ADDRESS_SET(page_start);
	col_low = SSD130X_PAGE_COL_START_LOW |
		  SSD130X_PAGE_COL_START_LOW_SET(col_start);
	col_high = SSD130X_PAGE_COL_START_HIGH |
		   SSD130X_PAGE_COL_START_HIGH_SET(col_start);
	ret = ssd13xx_write_cmd(ssd13xx, 3, page, col_low, col_high);
	if (ret < 0)
		return ret;

	return 0;
}

static int ssd13xx_pwm_enable(struct ssd13xx_device *ssd13xx)
{
	struct device *dev = ssd13xx->dev;
	struct pwm_state pwmstate;

	ssd13xx->pwm = pwm_get(dev, NULL);
	if (IS_ERR(ssd13xx->pwm)) {
		dev_err(dev, "Could not get PWM from firmware description!\n");
		return PTR_ERR(ssd13xx->pwm);
	}

	pwm_init_state(ssd13xx->pwm, &pwmstate);
	pwm_set_relative_duty_cycle(&pwmstate, 50, 100);
	pwm_apply_state(ssd13xx->pwm, &pwmstate);

	/* Enable the PWM */
	pwm_enable(ssd13xx->pwm);

	dev_dbg(dev, "Using PWM %s with a %lluns period.\n",
		ssd13xx->pwm->label, pwm_get_period(ssd13xx->pwm));

	return 0;
}

static void ssd13xx_reset(struct ssd13xx_device *ssd13xx)
{
	if (!ssd13xx->reset)
		return;

	/* Reset the screen */
	gpiod_set_value_cansleep(ssd13xx->reset, 1);
	udelay(4);
	gpiod_set_value_cansleep(ssd13xx->reset, 0);
	udelay(4);
}

static int ssd13xx_power_on(struct ssd13xx_device *ssd13xx)
{
	struct device *dev = ssd13xx->dev;
	int ret;

	ssd13xx_reset(ssd13xx);

	ret = regulator_enable(ssd13xx->vcc_reg);
	if (ret) {
		dev_err(dev, "Failed to enable VCC: %d\n", ret);
		return ret;
	}

	if (ssd13xx->device_info->need_pwm) {
		ret = ssd13xx_pwm_enable(ssd13xx);
		if (ret) {
			dev_err(dev, "Failed to enable PWM: %d\n", ret);
			regulator_disable(ssd13xx->vcc_reg);
			return ret;
		}
	}

	return 0;
}

static void ssd13xx_power_off(struct ssd13xx_device *ssd13xx)
{
	pwm_disable(ssd13xx->pwm);
	pwm_put(ssd13xx->pwm);

	regulator_disable(ssd13xx->vcc_reg);
}

static int ssd130x_init(struct ssd13xx_device *ssd13xx)
{
	u32 precharge, dclk, com_invdir, compins, chargepump, seg_remap;
	bool scan_mode;
	int ret;

	/* Set initial contrast */
	ret = ssd13xx_write_cmd(ssd13xx, 2, SSD13XX_CONTRAST, ssd13xx->contrast);
	if (ret < 0)
		return ret;

	/* Set segment re-map */
	seg_remap = (SSD13XX_SET_SEG_REMAP |
		     SSD13XX_SET_SEG_REMAP_SET(ssd13xx->seg_remap));
	ret = ssd13xx_write_cmd(ssd13xx, 1, seg_remap);
	if (ret < 0)
		return ret;

	/* Set COM direction */
	com_invdir = (SSD130X_SET_COM_SCAN_DIR |
		      SSD130X_SET_COM_SCAN_DIR_SET(ssd13xx->com_invdir));
	ret = ssd13xx_write_cmd(ssd13xx,  1, com_invdir);
	if (ret < 0)
		return ret;

	/* Set multiplex ratio value */
	ret = ssd13xx_write_cmd(ssd13xx, 2, SSD13XX_SET_MULTIPLEX_RATIO, ssd13xx->height - 1);
	if (ret < 0)
		return ret;

	/* set display offset value */
	ret = ssd13xx_write_cmd(ssd13xx, 2, SSD130X_SET_DISPLAY_OFFSET, ssd13xx->com_offset);
	if (ret < 0)
		return ret;

	/* Set clock frequency */
	dclk = (SSD130X_SET_CLOCK_DIV_SET(ssd13xx->dclk_div - 1) |
		SSD130X_SET_CLOCK_FREQ_SET(ssd13xx->dclk_frq));
	ret = ssd13xx_write_cmd(ssd13xx, 2, SSD130X_SET_CLOCK_FREQ, dclk);
	if (ret < 0)
		return ret;

	/* Set Area Color Mode ON/OFF & Low Power Display Mode */
	if (ssd13xx->area_color_enable || ssd13xx->low_power) {
		u32 mode = 0;

		if (ssd13xx->area_color_enable)
			mode |= SSD130X_SET_AREA_COLOR_MODE_ENABLE;

		if (ssd13xx->low_power)
			mode |= SSD130X_SET_AREA_COLOR_MODE_LOW_POWER;

		ret = ssd13xx_write_cmd(ssd13xx, 2, SSD130X_SET_AREA_COLOR_MODE, mode);
		if (ret < 0)
			return ret;
	}

	/* Set precharge period in number of ticks from the internal clock */
	precharge = (SSD130X_SET_PRECHARGE_PERIOD1_SET(ssd13xx->prechargep1) |
		     SSD130X_SET_PRECHARGE_PERIOD2_SET(ssd13xx->prechargep2));
	ret = ssd13xx_write_cmd(ssd13xx, 2, SSD130X_SET_PRECHARGE_PERIOD, precharge);
	if (ret < 0)
		return ret;

	/* Set COM pins configuration */
	compins = BIT(1);
	/*
	 * The COM scan mode field values are the inverse of the boolean DT
	 * property "solomon,com-seq". The value 0b means scan from COM0 to
	 * COM[N - 1] while 1b means scan from COM[N - 1] to COM0.
	 */
	scan_mode = !ssd13xx->com_seq;
	compins |= (SSD130X_SET_COM_PINS_CONFIG1_SET(scan_mode) |
		    SSD130X_SET_COM_PINS_CONFIG2_SET(ssd13xx->com_lrremap));
	ret = ssd13xx_write_cmd(ssd13xx, 2, SSD130X_SET_COM_PINS_CONFIG, compins);
	if (ret < 0)
		return ret;

	/* Set VCOMH */
	ret = ssd13xx_write_cmd(ssd13xx, 2, SSD130X_SET_VCOMH, ssd13xx->vcomh);
	if (ret < 0)
		return ret;

	/* Turn on the DC-DC Charge Pump */
	chargepump = BIT(4);

	if (ssd13xx->device_info->need_chargepump)
		chargepump |= BIT(2);

	ret = ssd13xx_write_cmd(ssd13xx, 2, SSD130X_CHARGE_PUMP, chargepump);
	if (ret < 0)
		return ret;

	/* Set lookup table */
	if (ssd13xx->lookup_table_set) {
		int i;

		ret = ssd13xx_write_cmd(ssd13xx, 1, SSD130X_SET_LOOKUP_TABLE);
		if (ret < 0)
			return ret;

		for (i = 0; i < ARRAY_SIZE(ssd13xx->lookup_table); i++) {
			u8 val = ssd13xx->lookup_table[i];

			if (val < 31 || val > 63)
				dev_warn(ssd13xx->dev,
					 "lookup table index %d value out of range 31 <= %d <= 63\n",
					 i, val);
			ret = ssd13xx_write_cmd(ssd13xx, 1, val);
			if (ret < 0)
				return ret;
		}
	}

	/* Switch to page addressing mode */
	if (ssd13xx->page_address_mode)
		return ssd13xx_write_cmd(ssd13xx, 2, SSD130X_SET_ADDRESS_MODE,
					 SSD130X_SET_ADDRESS_MODE_PAGE);

	/* Switch to horizontal addressing mode */
	return ssd13xx_write_cmd(ssd13xx, 2, SSD130X_SET_ADDRESS_MODE,
				 SSD130X_SET_ADDRESS_MODE_HORIZONTAL);
}

static int ssd130x_update_rect(struct ssd13xx_device *ssd13xx,
			       struct drm_rect *rect, u8 *buf,
			       u8 *data_array)
{
	unsigned int x = rect->x1;
	unsigned int y = rect->y1;
	unsigned int width = drm_rect_width(rect);
	unsigned int height = drm_rect_height(rect);
	unsigned int line_length = DIV_ROUND_UP(width, 8);
	unsigned int page_height = SSD130X_PAGE_HEIGHT;
	unsigned int pages = DIV_ROUND_UP(height, page_height);
	struct drm_device *drm = &ssd13xx->drm;
	u32 array_idx = 0;
	int ret, i, j, k;

	drm_WARN_ONCE(drm, y % page_height != 0, "y must be aligned to screen page\n");

	/*
	 * The screen is divided in pages, each having a height of 8
	 * pixels, and the width of the screen. When sending a byte of
	 * data to the controller, it gives the 8 bits for the current
	 * column. I.e, the first byte are the 8 bits of the first
	 * column, then the 8 bits for the second column, etc.
	 *
	 *
	 * Representation of the screen, assuming it is 5 bits
	 * wide. Each letter-number combination is a bit that controls
	 * one pixel.
	 *
	 * A0 A1 A2 A3 A4
	 * B0 B1 B2 B3 B4
	 * C0 C1 C2 C3 C4
	 * D0 D1 D2 D3 D4
	 * E0 E1 E2 E3 E4
	 * F0 F1 F2 F3 F4
	 * G0 G1 G2 G3 G4
	 * H0 H1 H2 H3 H4
	 *
	 * If you want to update this screen, you need to send 5 bytes:
	 *  (1) A0 B0 C0 D0 E0 F0 G0 H0
	 *  (2) A1 B1 C1 D1 E1 F1 G1 H1
	 *  (3) A2 B2 C2 D2 E2 F2 G2 H2
	 *  (4) A3 B3 C3 D3 E3 F3 G3 H3
	 *  (5) A4 B4 C4 D4 E4 F4 G4 H4
	 */

	if (!ssd13xx->page_address_mode) {
		u8 page_start;

		/* Set address range for horizontal addressing mode */
		ret = ssd13xx_set_col_range(ssd13xx, ssd13xx->col_offset + x, width);
		if (ret < 0)
			return ret;

		page_start = ssd13xx->page_offset + y / page_height;
		ret = ssd13xx_set_page_range(ssd13xx, page_start, pages);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < pages; i++) {
		int m = page_height;

		/* Last page may be partial */
		if (page_height * (y / page_height + i + 1) > ssd13xx->height)
			m = ssd13xx->height % page_height;

		for (j = 0; j < width; j++) {
			u8 data = 0;

			for (k = 0; k < m; k++) {
				u32 idx = (page_height * i + k) * line_length + j / 8;
				u8 byte = buf[idx];
				u8 bit = (byte >> (j % 8)) & 1;

				data |= bit << k;
			}
			data_array[array_idx++] = data;
		}

		/*
		 * In page addressing mode, the start address needs to be reset,
		 * and each page then needs to be written out separately.
		 */
		if (ssd13xx->page_address_mode) {
			ret = ssd13xx_set_page_pos(ssd13xx,
						   ssd13xx->page_offset + i,
						   ssd13xx->col_offset + x);
			if (ret < 0)
				return ret;

			ret = ssd13xx_write_data(ssd13xx, data_array, width);
			if (ret < 0)
				return ret;

			array_idx = 0;
		}
	}

	/* Write out update in one go if we aren't using page addressing mode */
	if (!ssd13xx->page_address_mode)
		ret = ssd13xx_write_data(ssd13xx, data_array, width * pages);

	return ret;
}

static void ssd130x_clear_screen(struct ssd13xx_device *ssd13xx, u8 *data_array)
{
	unsigned int pages = DIV_ROUND_UP(ssd13xx->height, SSD130X_PAGE_HEIGHT);
	unsigned int width = ssd13xx->width;
	int ret, i;

	if (!ssd13xx->page_address_mode) {
		memset(data_array, 0, width * pages);

		/* Set address range for horizontal addressing mode */
		ret = ssd13xx_set_col_range(ssd13xx, ssd13xx->col_offset, width);
		if (ret < 0)
			return;

		ret = ssd13xx_set_page_range(ssd13xx, ssd13xx->page_offset, pages);
		if (ret < 0)
			return;

		/* Write out update in one go if we aren't using page addressing mode */
		ssd13xx_write_data(ssd13xx, data_array, width * pages);
	} else {
		/*
		 * In page addressing mode, the start address needs to be reset,
		 * and each page then needs to be written out separately.
		 */
		memset(data_array, 0, width);

		for (i = 0; i < pages; i++) {
			ret = ssd13xx_set_page_pos(ssd13xx,
						   ssd13xx->page_offset + i,
						   ssd13xx->col_offset);
			if (ret < 0)
				return;

			ret = ssd13xx_write_data(ssd13xx, data_array, width);
			if (ret < 0)
				return;
		}
	}
}

static const struct ssd13xx_funcs ssd13xx_family_funcs[] = {
	[SSD130X_FAMILY] = {
		.init = ssd130x_init,
		.update_rect = ssd130x_update_rect,
		.clear_screen = ssd130x_clear_screen,
		.fmt_convert = drm_fb_xrgb8888_to_mono,
	},
};

static int ssd13xx_fb_blit_rect(struct drm_framebuffer *fb,
				const struct iosys_map *vmap,
				struct drm_rect *rect, u8 *buf,
				const struct drm_format_info *fi,
				u8 *data_array)
{
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(fb->dev);
	const struct ssd13xx_funcs *ssd13xx_funcs = ssd13xx->funcs;
	struct iosys_map dst;
	unsigned int dst_pitch;
	int ret = 0;

	/* Align y to display page boundaries */
	rect->y1 = round_down(rect->y1, SSD130X_PAGE_HEIGHT);
	rect->y2 = min_t(unsigned int, round_up(rect->y2, SSD130X_PAGE_HEIGHT), ssd13xx->height);

	dst_pitch = drm_format_info_min_pitch(fi, 0, drm_rect_width(rect));

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		return ret;

	iosys_map_set_vaddr(&dst, buf);
	ssd13xx_funcs->fmt_convert(&dst, &dst_pitch, vmap, fb, rect);

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

	return ssd13xx_funcs->update_rect(ssd13xx, rect, buf, data_array);
}

static int ssd13xx_primary_plane_atomic_check(struct drm_plane *plane,
					      struct drm_atomic_state *state)
{
	struct drm_device *drm = plane->dev;
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(drm);
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct ssd13xx_plane_state *ssd13xx_state = to_ssd13xx_plane_state(plane_state);
	struct drm_crtc *crtc = plane_state->crtc;
	struct drm_crtc_state *crtc_state;
	int ret;

	if (!crtc)
		return -EINVAL;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_plane_helper_atomic_check(plane, state);
	if (ret)
		return ret;

	ssd13xx_state->buffer = kzalloc(ssd13xx->buffer_size, GFP_KERNEL);
	if (!ssd13xx_state->buffer)
		return -ENOMEM;

	return 0;
}

static void ssd13xx_primary_plane_atomic_update(struct drm_plane *plane,
						struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	struct ssd13xx_crtc_state *ssd13xx_crtc_state =  to_ssd13xx_crtc_state(crtc_state);
	struct ssd13xx_plane_state *ssd13xx_plane_state = to_ssd13xx_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_device *drm = plane->dev;
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(drm);
	struct drm_rect dst_clip;
	struct drm_rect damage;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;

	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		dst_clip = plane_state->dst;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;

		ssd13xx_fb_blit_rect(fb, &shadow_plane_state->data[0], &dst_clip,
				     ssd13xx_plane_state->buffer,
				     ssd13xx->buffer_fi,
				     ssd13xx_crtc_state->data_array);
	}

	drm_dev_exit(idx);
}

static void ssd13xx_primary_plane_atomic_disable(struct drm_plane *plane,
						 struct drm_atomic_state *state)
{
	struct drm_device *drm = plane->dev;
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(drm);
	const struct ssd13xx_funcs *ssd13xx_funcs = ssd13xx->funcs;
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;
	struct ssd13xx_crtc_state *ssd13xx_crtc_state;
	int idx;

	if (!plane_state->crtc)
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ssd13xx_crtc_state = to_ssd13xx_crtc_state(crtc_state);

	if (!drm_dev_enter(drm, &idx))
		return;

	ssd13xx_funcs->clear_screen(ssd13xx, ssd13xx_crtc_state->data_array);

	drm_dev_exit(idx);
}

/* Called during init to allocate the plane's atomic state. */
static void ssd13xx_primary_plane_reset(struct drm_plane *plane)
{
	struct ssd13xx_plane_state *ssd13xx_state;

	WARN_ON(plane->state);

	ssd13xx_state = kzalloc(sizeof(*ssd13xx_state), GFP_KERNEL);
	if (!ssd13xx_state)
		return;

	__drm_gem_reset_shadow_plane(plane, &ssd13xx_state->base);
}

static struct drm_plane_state *ssd13xx_primary_plane_duplicate_state(struct drm_plane *plane)
{
	struct drm_shadow_plane_state *new_shadow_plane_state;
	struct ssd13xx_plane_state *old_ssd13xx_state;
	struct ssd13xx_plane_state *ssd13xx_state;

	if (WARN_ON(!plane->state))
		return NULL;

	old_ssd13xx_state = to_ssd13xx_plane_state(plane->state);
	ssd13xx_state = kmemdup(old_ssd13xx_state, sizeof(*ssd13xx_state), GFP_KERNEL);
	if (!ssd13xx_state)
		return NULL;

	/* The buffer is not duplicated and is allocated in .atomic_check */
	ssd13xx_state->buffer = NULL;

	new_shadow_plane_state = &ssd13xx_state->base;

	__drm_gem_duplicate_shadow_plane_state(plane, new_shadow_plane_state);

	return &new_shadow_plane_state->base;
}

static void ssd13xx_primary_plane_destroy_state(struct drm_plane *plane,
						struct drm_plane_state *state)
{
	struct ssd13xx_plane_state *ssd13xx_state = to_ssd13xx_plane_state(state);

	kfree(ssd13xx_state->buffer);

	__drm_gem_destroy_shadow_plane_state(&ssd13xx_state->base);

	kfree(ssd13xx_state);
}

static const struct drm_plane_helper_funcs ssd13xx_primary_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = ssd13xx_primary_plane_atomic_check,
	.atomic_update = ssd13xx_primary_plane_atomic_update,
	.atomic_disable = ssd13xx_primary_plane_atomic_disable,
};

static const struct drm_plane_funcs ssd13xx_primary_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = ssd13xx_primary_plane_reset,
	.atomic_duplicate_state = ssd13xx_primary_plane_duplicate_state,
	.atomic_destroy_state = ssd13xx_primary_plane_destroy_state,
	.destroy = drm_plane_cleanup,
};

static enum drm_mode_status ssd13xx_crtc_mode_valid(struct drm_crtc *crtc,
						    const struct drm_display_mode *mode)
{
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(crtc->dev);

	if (mode->hdisplay != ssd13xx->mode.hdisplay &&
	    mode->vdisplay != ssd13xx->mode.vdisplay)
		return MODE_ONE_SIZE;
	else if (mode->hdisplay != ssd13xx->mode.hdisplay)
		return MODE_ONE_WIDTH;
	else if (mode->vdisplay != ssd13xx->mode.vdisplay)
		return MODE_ONE_HEIGHT;

	return MODE_OK;
}

static int ssd13xx_crtc_atomic_check(struct drm_crtc *crtc,
				     struct drm_atomic_state *state)
{
	struct drm_device *drm = crtc->dev;
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(drm);
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct ssd13xx_crtc_state *ssd13xx_state = to_ssd13xx_crtc_state(crtc_state);
	int ret;

	ret = drm_crtc_helper_atomic_check(crtc, state);
	if (ret)
		return ret;

	ssd13xx_state->data_array = kmalloc(ssd13xx->data_array_size, GFP_KERNEL);
	if (!ssd13xx_state->data_array)
		return -ENOMEM;

	return 0;
}

/* Called during init to allocate the CRTC's atomic state. */
static void ssd13xx_crtc_reset(struct drm_crtc *crtc)
{
	struct ssd13xx_crtc_state *ssd13xx_state;

	WARN_ON(crtc->state);

	ssd13xx_state = kzalloc(sizeof(*ssd13xx_state), GFP_KERNEL);
	if (!ssd13xx_state)
		return;

	__drm_atomic_helper_crtc_reset(crtc, &ssd13xx_state->base);
}

static struct drm_crtc_state *ssd13xx_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct ssd13xx_crtc_state *old_ssd13xx_state;
	struct ssd13xx_crtc_state *ssd13xx_state;

	if (WARN_ON(!crtc->state))
		return NULL;

	old_ssd13xx_state = to_ssd13xx_crtc_state(crtc->state);
	ssd13xx_state = kmemdup(old_ssd13xx_state, sizeof(*ssd13xx_state), GFP_KERNEL);
	if (!ssd13xx_state)
		return NULL;

	/* The buffer is not duplicated and is allocated in .atomic_check */
	ssd13xx_state->data_array = NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &ssd13xx_state->base);

	return &ssd13xx_state->base;
}

static void ssd13xx_crtc_destroy_state(struct drm_crtc *crtc,
				       struct drm_crtc_state *state)
{
	struct ssd13xx_crtc_state *ssd13xx_state = to_ssd13xx_crtc_state(state);

	kfree(ssd13xx_state->data_array);

	__drm_atomic_helper_crtc_destroy_state(state);

	kfree(ssd13xx_state);
}

/*
 * The CRTC is always enabled. Screen updates are performed by
 * the primary plane's atomic_update function. Disabling clears
 * the screen in the primary plane's atomic_disable function.
 */
static const struct drm_crtc_helper_funcs ssd13xx_crtc_helper_funcs = {
	.mode_valid = ssd13xx_crtc_mode_valid,
	.atomic_check = ssd13xx_crtc_atomic_check,
};

static const struct drm_crtc_funcs ssd13xx_crtc_funcs = {
	.reset = ssd13xx_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = ssd13xx_crtc_duplicate_state,
	.atomic_destroy_state = ssd13xx_crtc_destroy_state,
};

static void ssd13xx_encoder_atomic_enable(struct drm_encoder *encoder,
					  struct drm_atomic_state *state)
{
	struct drm_device *drm = encoder->dev;
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(drm);
	const struct ssd13xx_funcs *ssd13xx_funcs = ssd13xx->funcs;
	int ret;

	ret = ssd13xx_power_on(ssd13xx);
	if (ret)
		return;

	ret = ssd13xx_funcs->init(ssd13xx);
	if (ret)
		goto power_off;

	ssd13xx_write_cmd(ssd13xx, 1, SSD13XX_DISPLAY_ON);

	backlight_enable(ssd13xx->bl_dev);

	return;

power_off:
	ssd13xx_power_off(ssd13xx);
	return;
}

static void ssd13xx_encoder_atomic_disable(struct drm_encoder *encoder,
					   struct drm_atomic_state *state)
{
	struct drm_device *drm = encoder->dev;
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(drm);

	backlight_disable(ssd13xx->bl_dev);

	ssd13xx_write_cmd(ssd13xx, 1, SSD13XX_DISPLAY_OFF);

	ssd13xx_power_off(ssd13xx);
}

static const struct drm_encoder_helper_funcs ssd13xx_encoder_helper_funcs = {
	.atomic_enable = ssd13xx_encoder_atomic_enable,
	.atomic_disable = ssd13xx_encoder_atomic_disable,
};

static const struct drm_encoder_funcs ssd13xx_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int ssd13xx_connector_get_modes(struct drm_connector *connector)
{
	struct ssd13xx_device *ssd13xx = drm_to_ssd13xx(connector->dev);
	struct drm_display_mode *mode;
	struct device *dev = ssd13xx->dev;

	mode = drm_mode_duplicate(connector->dev, &ssd13xx->mode);
	if (!mode) {
		dev_err(dev, "Failed to duplicated mode\n");
		return 0;
	}

	drm_mode_probed_add(connector, mode);
	drm_set_preferred_mode(connector, mode->hdisplay, mode->vdisplay);

	/* There is only a single mode */
	return 1;
}

static const struct drm_connector_helper_funcs ssd13xx_connector_helper_funcs = {
	.get_modes = ssd13xx_connector_get_modes,
};

static const struct drm_connector_funcs ssd13xx_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs ssd13xx_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const uint32_t ssd13xx_formats[] = {
	DRM_FORMAT_XRGB8888,
};

DEFINE_DRM_GEM_FOPS(ssd13xx_fops);

static const struct drm_driver ssd13xx_drm_driver = {
	DRM_GEM_SHMEM_DRIVER_OPS,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.date			= DRIVER_DATE,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops			= &ssd13xx_fops,
};

static int ssd13xx_update_bl(struct backlight_device *bdev)
{
	struct ssd13xx_device *ssd13xx = bl_get_data(bdev);
	int brightness = backlight_get_brightness(bdev);
	int ret;

	ssd13xx->contrast = brightness;

	ret = ssd13xx_write_cmd(ssd13xx, 1, SSD13XX_CONTRAST);
	if (ret < 0)
		return ret;

	ret = ssd13xx_write_cmd(ssd13xx, 1, ssd13xx->contrast);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct backlight_ops ssd13xxfb_bl_ops = {
	.update_status	= ssd13xx_update_bl,
};

static void ssd13xx_parse_properties(struct ssd13xx_device *ssd13xx)
{
	struct device *dev = ssd13xx->dev;

	if (device_property_read_u32(dev, "solomon,width", &ssd13xx->width))
		ssd13xx->width = ssd13xx->device_info->default_width;

	if (device_property_read_u32(dev, "solomon,height", &ssd13xx->height))
		ssd13xx->height = ssd13xx->device_info->default_height;

	if (device_property_read_u32(dev, "solomon,page-offset", &ssd13xx->page_offset))
		ssd13xx->page_offset = 1;

	if (device_property_read_u32(dev, "solomon,col-offset", &ssd13xx->col_offset))
		ssd13xx->col_offset = 0;

	if (device_property_read_u32(dev, "solomon,com-offset", &ssd13xx->com_offset))
		ssd13xx->com_offset = 0;

	if (device_property_read_u32(dev, "solomon,prechargep1", &ssd13xx->prechargep1))
		ssd13xx->prechargep1 = 2;

	if (device_property_read_u32(dev, "solomon,prechargep2", &ssd13xx->prechargep2))
		ssd13xx->prechargep2 = 2;

	if (!device_property_read_u8_array(dev, "solomon,lookup-table",
					   ssd13xx->lookup_table,
					   ARRAY_SIZE(ssd13xx->lookup_table)))
		ssd13xx->lookup_table_set = 1;

	ssd13xx->seg_remap = !device_property_read_bool(dev, "solomon,segment-no-remap");
	ssd13xx->com_seq = device_property_read_bool(dev, "solomon,com-seq");
	ssd13xx->com_lrremap = device_property_read_bool(dev, "solomon,com-lrremap");
	ssd13xx->com_invdir = device_property_read_bool(dev, "solomon,com-invdir");
	ssd13xx->area_color_enable =
		device_property_read_bool(dev, "solomon,area-color-enable");
	ssd13xx->low_power = device_property_read_bool(dev, "solomon,low-power");

	ssd13xx->contrast = 127;
	ssd13xx->vcomh = ssd13xx->device_info->default_vcomh;

	/* Setup display timing */
	if (device_property_read_u32(dev, "solomon,dclk-div", &ssd13xx->dclk_div))
		ssd13xx->dclk_div = ssd13xx->device_info->default_dclk_div;
	if (device_property_read_u32(dev, "solomon,dclk-frq", &ssd13xx->dclk_frq))
		ssd13xx->dclk_frq = ssd13xx->device_info->default_dclk_frq;
}

static int ssd13xx_init_modeset(struct ssd13xx_device *ssd13xx)
{
	struct drm_display_mode *mode = &ssd13xx->mode;
	struct device *dev = ssd13xx->dev;
	struct drm_device *drm = &ssd13xx->drm;
	unsigned long max_width, max_height;
	struct drm_plane *primary_plane;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	int ret;

	/*
	 * Modesetting
	 */

	ret = drmm_mode_config_init(drm);
	if (ret) {
		dev_err(dev, "DRM mode config init failed: %d\n", ret);
		return ret;
	}

	mode->type = DRM_MODE_TYPE_DRIVER;
	mode->clock = 1;
	mode->hdisplay = mode->htotal = ssd13xx->width;
	mode->hsync_start = mode->hsync_end = ssd13xx->width;
	mode->vdisplay = mode->vtotal = ssd13xx->height;
	mode->vsync_start = mode->vsync_end = ssd13xx->height;
	mode->width_mm = 27;
	mode->height_mm = 27;

	max_width = max_t(unsigned long, mode->hdisplay, DRM_SHADOW_PLANE_MAX_WIDTH);
	max_height = max_t(unsigned long, mode->vdisplay, DRM_SHADOW_PLANE_MAX_HEIGHT);

	drm->mode_config.min_width = mode->hdisplay;
	drm->mode_config.max_width = max_width;
	drm->mode_config.min_height = mode->vdisplay;
	drm->mode_config.max_height = max_height;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &ssd13xx_mode_config_funcs;

	/* Primary plane */

	primary_plane = &ssd13xx->primary_plane;
	ret = drm_universal_plane_init(drm, primary_plane, 0, &ssd13xx_primary_plane_funcs,
				       ssd13xx_formats, ARRAY_SIZE(ssd13xx_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		dev_err(dev, "DRM primary plane init failed: %d\n", ret);
		return ret;
	}

	drm_plane_helper_add(primary_plane, &ssd13xx_primary_plane_helper_funcs);

	drm_plane_enable_fb_damage_clips(primary_plane);

	/* CRTC */

	crtc = &ssd13xx->crtc;
	ret = drm_crtc_init_with_planes(drm, crtc, primary_plane, NULL,
					&ssd13xx_crtc_funcs, NULL);
	if (ret) {
		dev_err(dev, "DRM crtc init failed: %d\n", ret);
		return ret;
	}

	drm_crtc_helper_add(crtc, &ssd13xx_crtc_helper_funcs);

	/* Encoder */

	encoder = &ssd13xx->encoder;
	ret = drm_encoder_init(drm, encoder, &ssd13xx_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret) {
		dev_err(dev, "DRM encoder init failed: %d\n", ret);
		return ret;
	}

	drm_encoder_helper_add(encoder, &ssd13xx_encoder_helper_funcs);

	encoder->possible_crtcs = drm_crtc_mask(crtc);

	/* Connector */

	connector = &ssd13xx->connector;
	ret = drm_connector_init(drm, connector, &ssd13xx_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret) {
		dev_err(dev, "DRM connector init failed: %d\n", ret);
		return ret;
	}

	drm_connector_helper_add(connector, &ssd13xx_connector_helper_funcs);

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret) {
		dev_err(dev, "DRM attach connector to encoder failed: %d\n", ret);
		return ret;
	}

	drm_mode_config_reset(drm);

	return 0;
}

static int ssd13xx_get_resources(struct ssd13xx_device *ssd13xx)
{
	struct device *dev = ssd13xx->dev;

	ssd13xx->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ssd13xx->reset))
		return dev_err_probe(dev, PTR_ERR(ssd13xx->reset),
				     "Failed to get reset gpio\n");

	ssd13xx->vcc_reg = devm_regulator_get(dev, "vcc");
	if (IS_ERR(ssd13xx->vcc_reg))
		return dev_err_probe(dev, PTR_ERR(ssd13xx->vcc_reg),
				     "Failed to get VCC regulator\n");

	return 0;
}

static int ssd13xx_set_buffer_sizes(struct ssd13xx_device *ssd13xx,
				    enum ssd13xx_family_ids family_id)
{
	const struct drm_format_info *fi;
	unsigned int buffer_pitch;

	switch (family_id) {
	case SSD130X_FAMILY:
		unsigned int pages = DIV_ROUND_UP(ssd13xx->height, SSD130X_PAGE_HEIGHT);

		ssd13xx->data_array_size = ssd13xx->width * pages;

		fi = drm_format_info(DRM_FORMAT_R1);
		break;
	}

	if (!fi)
		return -EINVAL;

	buffer_pitch = drm_format_info_min_pitch(fi, 0, ssd13xx->width);
	ssd13xx->buffer_size = buffer_pitch * ssd13xx->height;
	ssd13xx->buffer_fi = fi;

	return 0;
}

struct ssd13xx_device *ssd13xx_probe(struct device *dev, struct regmap *regmap)
{
	struct ssd13xx_device *ssd13xx;
	struct backlight_device *bl;
	struct drm_device *drm;
	enum ssd13xx_family_ids family_id;
	int ret;

	ssd13xx = devm_drm_dev_alloc(dev, &ssd13xx_drm_driver,
				     struct ssd13xx_device, drm);
	if (IS_ERR(ssd13xx))
		return ERR_PTR(dev_err_probe(dev, PTR_ERR(ssd13xx),
					     "Failed to allocate DRM device\n"));

	drm = &ssd13xx->drm;

	ssd13xx->dev = dev;
	ssd13xx->regmap = regmap;
	ssd13xx->device_info = device_get_match_data(dev);

	family_id = ssd13xx->device_info->family_id;

	ssd13xx->funcs = &ssd13xx_family_funcs[family_id];

	if (ssd13xx->device_info->page_mode_only)
		ssd13xx->page_address_mode = 1;

	ssd13xx_parse_properties(ssd13xx);

	ret = ssd13xx_set_buffer_sizes(ssd13xx, family_id);
	if (ret)
		return ERR_PTR(ret);

	ret = ssd13xx_get_resources(ssd13xx);
	if (ret)
		return ERR_PTR(ret);

	bl = devm_backlight_device_register(dev, dev_name(dev), dev, ssd13xx,
					    &ssd13xxfb_bl_ops, NULL);
	if (IS_ERR(bl))
		return ERR_PTR(dev_err_probe(dev, PTR_ERR(bl),
					     "Unable to register backlight device\n"));

	bl->props.brightness = ssd13xx->contrast;
	bl->props.max_brightness = MAX_CONTRAST;
	ssd13xx->bl_dev = bl;

	ret = ssd13xx_init_modeset(ssd13xx);
	if (ret)
		return ERR_PTR(ret);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret, "DRM device register failed\n"));

	drm_fbdev_generic_setup(drm, 32);

	return ssd13xx;
}
EXPORT_SYMBOL_GPL(ssd13xx_probe);

void ssd13xx_remove(struct ssd13xx_device *ssd13xx)
{
	drm_dev_unplug(&ssd13xx->drm);
	drm_atomic_helper_shutdown(&ssd13xx->drm);
}
EXPORT_SYMBOL_GPL(ssd13xx_remove);

void ssd13xx_shutdown(struct ssd13xx_device *ssd13xx)
{
	drm_atomic_helper_shutdown(&ssd13xx->drm);
}
EXPORT_SYMBOL_GPL(ssd13xx_shutdown);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Javier Martinez Canillas <javierm@redhat.com>");
MODULE_LICENSE("GPL v2");
