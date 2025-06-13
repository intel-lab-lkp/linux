/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#include <linux/slab.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_writeback.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_encoder.h>

#include "i915_drv.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_display_driver.h"
#include "intel_connector.h"
#include "intel_writeback.h"

struct intel_writeback_connector {
	struct drm_writeback_connector base;
	struct intel_encoder encoder;
	struct intel_connector connector;
};

static const u32 writeback_formats[] = {
	DRM_FORMAT_XYUV8888,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XVYU2101010,
	DRM_FORMAT_VYUY,
	DRM_FORMAT_XBGR2101010,
};

static int intel_writeback_connector_init(struct intel_connector *connector)
{
	struct intel_digital_connector_state *conn_state;

	conn_state = kzalloc(sizeof(conn_state), GFP_KERNEL);
	if (!conn_state)
		return -ENOMEM;

	__drm_atomic_helper_connector_reset(&connector->base,
					    &conn_state->base);

	return 0;
}

static struct
intel_connector *intel_writeback_connector_alloc(struct intel_connector *connector)
{
	connector = kzalloc(sizeof(connector), GFP_KERNEL);
	if (!connector)
		return NULL;

	if (intel_writeback_connector_init(connector) < 0) {
		kfree(connector);
		return NULL;
	}

	return connector;
}

static const struct drm_encoder_helper_funcs enc_helper_funcs = {
};

static const struct drm_encoder_funcs drm_writeback_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

const struct drm_connector_funcs conn_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs intel_writeback_conn_helper_funcs = {
};

int intel_writeback_init(struct intel_display *display)
{
	struct intel_encoder *encoder;
	struct intel_writeback_connector *writeback_conn;
	struct intel_connector *connector;
	int ret;

	writeback_conn = kzalloc(sizeof(*writeback_conn), GFP_KERNEL);
	if (!writeback_conn)
		return -ENOSPC;

	connector = &writeback_conn->connector;
	intel_writeback_connector_alloc(connector);

	encoder = &writeback_conn->encoder;
	drm_encoder_helper_add(&encoder->base, &enc_helper_funcs);
	ret = drm_encoder_init(display->drm, &encoder->base,
			       &drm_writeback_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret) {
		intel_connector_free(connector);
		kfree(writeback_conn);
		return ret;
	}

	encoder->base.possible_crtcs = 0xf;
	encoder->type = INTEL_OUTPUT_WRITEBACK;
	encoder->pipe_mask = ~0;

	connector->base.interlace_allowed = 0;
	drm_connector_helper_add(&connector->base, &intel_writeback_conn_helper_funcs);
	ret = drm_writeback_connector_init_with_conn(display->drm, &connector->base,
						     &writeback_conn->base,
						     &encoder->base, &conn_funcs,
						     writeback_formats,
						     ARRAY_SIZE(writeback_formats));
	if (ret) {
		intel_connector_free(connector);
		drm_encoder_cleanup(&encoder->base);
		kfree(&writeback_conn->encoder);
		kfree(writeback_conn);
		return ret;
	}

	intel_connector_attach_encoder(connector, encoder);

	return 0;
}


