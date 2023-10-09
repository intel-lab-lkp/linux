/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Header file for:
 * DRM driver for Solomon SSD13xx OLED displays
 *
 * Copyright 2022 Red Hat Inc.
 * Author: Javier Martinez Canillas <javierm@redhat.com>
 *
 * Based on drivers/video/fbdev/ssd1307fb.c
 * Copyright 2012 Free Electrons
 */

#ifndef __SSD13XX_H__
#define __SSD13XX_H__

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane_helper.h>

#include <linux/regmap.h>
#include <linux/iosys-map.h>

#define SSD13XX_DATA				0x40
#define SSD13XX_COMMAND				0x80

enum ssd13xx_family_ids {
	SSD130X_FAMILY,
};

enum ssd13xx_variants {
	/* ssd130x family */
	SH1106_ID,
	SSD1305_ID,
	SSD1306_ID,
	SSD1307_ID,
	SSD1309_ID,
	NR_SSD13XX_VARIANTS
};

struct ssd13xx_deviceinfo {
	u32 default_vcomh;
	u32 default_dclk_div;
	u32 default_dclk_frq;
	u32 default_width;
	u32 default_height;
	enum ssd13xx_family_ids family_id;
	bool need_pwm;
	bool need_chargepump;
	bool page_mode_only;
};

struct ssd13xx_device {
	struct drm_device drm;
	struct device *dev;
	struct drm_display_mode mode;
	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct i2c_client *client;

	struct regmap *regmap;

	const struct ssd13xx_deviceinfo *device_info;

	unsigned page_address_mode : 1;
	unsigned area_color_enable : 1;
	unsigned com_invdir : 1;
	unsigned com_lrremap : 1;
	unsigned com_seq : 1;
	unsigned lookup_table_set : 1;
	unsigned low_power : 1;
	unsigned seg_remap : 1;
	u32 com_offset;
	u32 contrast;
	u32 dclk_div;
	u32 dclk_frq;
	u32 height;
	u8 lookup_table[4];
	u32 page_offset;
	u32 col_offset;
	u32 prechargep1;
	u32 prechargep2;
	/* HW format buffer size */
	u32 data_array_size;
	/* Intermediate buffer size */
	u32 buffer_size;
	/* Pixel format info for the intermediate buffer */
	const struct drm_format_info *buffer_fi;

	struct backlight_device *bl_dev;
	struct pwm_device *pwm;
	struct gpio_desc *reset;
	struct regulator *vcc_reg;
	u32 vcomh;
	u32 width;
	/* Cached address ranges */
	u8 col_start;
	u8 col_end;
	u8 page_start;
	u8 page_end;

	/* Chip family specific operations */
	const struct ssd13xx_funcs *funcs;
};

struct ssd13xx_funcs {
	int (*init)(struct ssd13xx_device *ssd130x);
	int (*update_rect)(struct ssd13xx_device *ssd13xx, struct drm_rect *rect,
			   u8 *buf, u8 *data_array);
	void (*clear_screen)(struct ssd13xx_device *ssd13xx,
			     u8 *data_array);
	void (*fmt_convert)(struct iosys_map *dst, const unsigned int *dst_pitch,
			    const struct iosys_map *src, const struct drm_framebuffer *fb,
			    const struct drm_rect *clip);
};

extern const struct ssd13xx_deviceinfo ssd13xx_variants[];

struct ssd13xx_device *ssd13xx_probe(struct device *dev, struct regmap *regmap);
void ssd13xx_remove(struct ssd13xx_device *ssd13xx);
void ssd13xx_shutdown(struct ssd13xx_device *ssd13xx);

#endif /* __SSD13XX_H__ */
