/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) StarFive Technology Co., Ltd.
 */

#ifndef __INNO_COMMON_HDMI__
#define __INNO_COMMON_HDMI__

#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_bridge.h>
#include <linux/i2c.h>

struct inno_hdmi;

struct inno_hdmi_i2c {
	u8 ddc_addr;
	u8 segment_addr;

	struct i2c_adapter adap;
	struct mutex lock; /* For i2c operation. */
	struct completion cmp;
};

struct inno_hdmi_plat_data {
	const char *vendor;
	const char *product;

	/* Platform-specific mode validation*/
	enum drm_mode_status (*mode_valid)(struct inno_hdmi *hdmi,
					   const struct drm_display_mode *mode);
	int (*hdmi_clear_infoframe)(struct inno_hdmi *hdmi, enum hdmi_infoframe_type type);
	int (*hdmi_write_infoframe)(struct inno_hdmi *hdmi, enum hdmi_infoframe_type type,
				    const u8 *buffer, size_t len);
};

struct inno_hdmi {
	struct device *dev;
	void __iomem *regs;

	struct i2c_adapter *ddc;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	struct inno_hdmi_i2c *i2c;

	const struct inno_hdmi_plat_data *plat_data;
};

u8 hdmi_readb(struct inno_hdmi *hdmi, u16 offset);
void hdmi_writeb(struct inno_hdmi *hdmi, u16 offset, u32 val);
void hdmi_modb(struct inno_hdmi *hdmi, u16 offset, u32 msk, u32 val);

void inno_hdmi_i2c_init(struct inno_hdmi *hdmi, unsigned long long rate);
int inno_hdmi_bind(struct drm_device *drm, struct inno_hdmi *hdmi, struct drm_encoder *encoder);

#endif /* __INNO_COMMON_HDMI__ */
