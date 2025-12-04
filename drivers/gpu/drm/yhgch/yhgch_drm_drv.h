/* SPDX-License-Identifier: GPL-2.0 */

#ifndef YHGCH_DRM_DRV_H
#define YHGCH_DRM_DRV_H

#include <drm/drm_framebuffer.h>
#include <drm/drm_encoder.h>
#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c-algo-bit.h>
#include <linux/i2c.h>
#include <linux/version.h>

struct yhgch_ddc {
	struct yhgch_drm_private *priv;
	struct i2c_adapter adapter;
	struct i2c_algo_bit_data bit_data;
};

struct yhgch_drm_private {
	/* hw */
	void __iomem *mmio;
	void __iomem *vram_base;

	/* drm */
	struct drm_device dev;
	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static inline struct yhgch_drm_private *to_yhgch_drm_private(struct drm_device *dev)
{
	return container_of(dev, struct yhgch_drm_private, dev);
}

void yhgch_set_power_mode(struct yhgch_drm_private *priv,
			  u32 power_mode);
void yhgch_set_current_gate(struct yhgch_drm_private *priv,
			    u32 gate);

int yhgch_de_init(struct yhgch_drm_private *priv);
int yhgch_vdac_init(struct yhgch_drm_private *priv);
int yhgch_mm_init(struct yhgch_drm_private *yhgch);
struct i2c_adapter *yhgch_ddc_create(struct yhgch_drm_private *priv);

#endif
