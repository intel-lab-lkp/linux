/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#ifndef __LSDC_OUTPUT_H__
#define __LSDC_OUTPUT_H__

#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>

struct lsdc_desc;

struct lsdc_output_desc {
	u32 pipe;
	const char type[32];
};

struct lsdc_output {
	struct device *dev;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct lsdc_output_desc *descp;
};

int ls7a1000_output_init(struct drm_device *ddev,
			 struct lsdc_display_pipe *dispipe,
			 struct i2c_adapter *ddc,
			 unsigned int index);

int ls7a2000_output_init(struct drm_device *ldev,
			 struct lsdc_display_pipe *dispipe,
			 struct i2c_adapter *ddc,
			 unsigned int index);

int lsdc_output_preinit(struct device *parent,
			const struct lsdc_desc *descp,
			unsigned int index,
			struct platform_device **ppdev);

#endif
