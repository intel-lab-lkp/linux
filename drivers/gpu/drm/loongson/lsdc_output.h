/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#ifndef __LSDC_OUTPUT_H__
#define __LSDC_OUTPUT_H__

#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>

struct lsdc_output_desc {
	u32 pipe;
	u32 encoder_type;
	u32 connector_type;
	const struct drm_encoder_funcs *encoder_funcs;
	const struct drm_encoder_helper_funcs *encoder_helper_funcs;
	const struct drm_connector_funcs *connector_funcs;
	const struct drm_connector_helper_funcs *connector_helper_funcs;
	const char name[32];
};

struct lsdc_output {
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_bridge *bridge;
	const struct lsdc_output_desc *descp;
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

extern const struct drm_connector_funcs lsdc_connector_funcs;
extern const struct drm_connector_funcs lsdc_pipe0_hdmi_connector_funcs;
extern const struct drm_connector_funcs lsdc_pipe1_hdmi_connector_funcs;
extern const struct drm_connector_helper_funcs lsdc_connector_helper_funcs;

extern const struct drm_encoder_funcs lsdc_pipe0_hdmi_encoder_funcs;
extern const struct drm_encoder_funcs lsdc_pipe1_hdmi_encoder_funcs;
extern const struct drm_encoder_helper_funcs lsdc_encoder_helper_funcs;
extern const struct drm_encoder_helper_funcs lsdc_pipe0_hdmi_encoder_helper_funcs;
extern const struct drm_encoder_helper_funcs lsdc_pipe1_hdmi_encoder_helper_funcs;

int ls7a1000_output_init(struct drm_device *ddev,
			 struct lsdc_display_pipe *dispipe,
			 struct i2c_adapter *ddc,
			 unsigned int index);

int ls7a2000_output_init(struct drm_device *ddev,
			 struct lsdc_display_pipe *dispipe,
			 struct i2c_adapter *ddc,
			 unsigned int index);

int ls2k2000_output_init(struct drm_device *ddev,
			 struct lsdc_display_pipe *dispipe,
			 struct i2c_adapter *ddc,
			 unsigned int pipe);

int lsdc_output_init(struct drm_device *ddev,
		     struct lsdc_display_pipe *dispipe,
		     struct i2c_adapter *ddc,
		     unsigned int pipe);

#endif
