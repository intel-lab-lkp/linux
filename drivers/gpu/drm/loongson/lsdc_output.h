/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#ifndef __LSDC_OUTPUT_H__
#define __LSDC_OUTPUT_H__

#include <linux/component.h>

#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>

struct lsdc_desc;

struct lsdc_output {
	struct device *dev;
	struct drm_encoder encoder;
	struct drm_connector connector;
	unsigned int pipe;
};

static inline struct lsdc_output *
connector_to_lsdc_output(struct drm_connector *connector)
{
	return container_of(connector, struct lsdc_output, connector);
}

static inline struct lsdc_output *
encoder_to_lsdc_output(struct drm_encoder *encoder)
{
	return container_of(encoder, struct lsdc_output, encoder);
}

int ls7a1000_output_init(struct drm_device *ddev,
			 struct lsdc_output *output,
			 struct i2c_adapter *ddc,
			 unsigned int index);

int ls7a2000_output_init(struct drm_device *ldev,
			 struct lsdc_output *output,
			 struct i2c_adapter *ddc,
			 unsigned int index);

int lsdc_output_preinit(struct device *parent,
			const struct lsdc_desc *descp);

void lsdc_output_match_add(struct device *parent,
			   struct component_match **matchptr);

#endif
