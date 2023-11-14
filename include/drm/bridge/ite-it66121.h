/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ITE_IT66121_H__
#define __ITE_IT66121_H__

#include <linux/i2c.h>

#include <drm/drm_bridge.h>
#include <drm/drm_device.h>

int it66121_create_bridge(struct i2c_client *client, bool of_support,
			  bool hpd_support, bool audio_support,
			  struct drm_bridge **bridge);

void it66121_destroy_bridge(struct drm_bridge *bridge);

#endif
