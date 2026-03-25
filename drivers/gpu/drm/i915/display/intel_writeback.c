// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <linux/slab.h>
#include <linux/err.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_writeback.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_edid.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "intel_atomic.h"
#include "intel_connector.h"
#include "intel_de.h"
#include "intel_display_driver.h"
#include "intel_display_types.h"
#include "intel_display_utils.h"
#include "intel_fb_pin.h"
#include "intel_writeback.h"
#include "intel_writeback_reg.h"

struct intel_writeback_connector {
	struct intel_connector connector;
	struct intel_encoder encoder;
	struct intel_writeback_job *job;
	enum transcoder trans;
	int frame_num;
};

struct intel_writeback_job {
	struct drm_framebuffer *fb;
	struct drm_writeback_connector *wb_connector;
	struct i915_vma *vma;
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

	conn_state = kzalloc(sizeof(*conn_state), GFP_KERNEL);
	if (!conn_state)
		return -ENOMEM;

	__drm_atomic_helper_connector_reset(&connector->base,
					    &conn_state->base);
	return 0;
}

static int
intel_writeback_connector_alloc(struct intel_connector *connector)
{
	if (intel_writeback_connector_init(connector) < 0) {
		kfree(connector);
		return -ENOMEM;
	}

	return 0;
}

static enum drm_mode_status
intel_writeback_mode_valid(struct drm_connector *_connector,
			   const struct drm_display_mode *mode)
{
	int refresh_rate;

	if (mode->hdisplay > 3840)
		return MODE_H_ILLEGAL;

	if (mode->vdisplay > 2160)
		return MODE_V_ILLEGAL;

	refresh_rate = drm_mode_vrefresh(mode);
	if (refresh_rate > 60)
		return MODE_BAD;

	return MODE_OK;
}

static int intel_writeback_get_modes(struct drm_connector *connector)
{
	return drm_add_modes_noedid(connector, 3840, 2160);
}

static int intel_writeback_prepare_job(struct drm_connector *connector,
				       struct drm_writeback_job *job)
{
	struct i915_vma *vma;
	struct intel_writeback_job *wb_job;
	unsigned long out_flags = 0;
	const struct i915_gtt_view view = {
		.type = I915_GTT_VIEW_NORMAL,
	};
	int ret;

	if (!job->fb)
		return 0;

	if (job->fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;

	wb_job = kzalloc(sizeof(*wb_job), GFP_KERNEL);
	if (!wb_job)
		return -ENOMEM;

	vma = intel_fb_pin_to_ggtt(job->fb, &view, 4 * 1024, 0, 0, true, &out_flags);
	if (IS_ERR(vma)) {
		drm_err(job->fb->dev, "Failed to map framebuffer: %d\n", ret);
		ret = PTR_ERR(vma);
		goto err;
	}

	wb_job->fb = job->fb;
	wb_job->vma = vma;
	drm_framebuffer_get(wb_job->fb);
	job->priv = wb_job;

	return 0;

err:
	kfree(wb_job);
	return ret;
}

static void intel_writeback_cleanup_job(struct drm_connector *connector,
					struct drm_writeback_job *job)
{
	struct intel_writeback_job *wb_job = job->priv;
	struct i915_vma *vma;
	unsigned long out_flags = 0;

	if (!job->fb)
		return;

	vma = wb_job->vma;
	wb_job->vma = NULL;
	intel_fb_unpin_vma(vma, out_flags);
	drm_framebuffer_put(wb_job->fb);
	kfree(wb_job);
	job->priv = NULL;
}

static enum drm_connector_status
intel_writeback_detect(struct drm_connector *connector,
		       bool force)
{
	return connector_status_connected;
}

static const struct drm_encoder_funcs drm_writeback_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

const struct drm_connector_funcs conn_funcs = {
	.detect = intel_writeback_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state = intel_digital_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs conn_helper_funcs = {
	.get_modes = intel_writeback_get_modes,
	.mode_valid = intel_writeback_mode_valid,
	.prepare_writeback_job = intel_writeback_prepare_job,
	.cleanup_writeback_job = intel_writeback_cleanup_job,
};

static int
intel_writeback_compute_config(struct intel_encoder *encoder,
			       struct intel_crtc_state *pipe_config,
			       struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(encoder);

	if (!conn_state->writeback_job)
		return 0;

	if (HAS_TRANSCODER(display, TRANSCODER_WD_0))
		pipe_config->cpu_transcoder = TRANSCODER_WD_0;

	pipe_config->output_types |= BIT(INTEL_OUTPUT_WRITEBACK);
	pipe_config->output_format = INTEL_OUTPUT_FORMAT_RGB;

	return 0;
}

static void
intel_writeback_get_config(struct intel_encoder *encoder,
			   struct intel_crtc_state *crtc_state)
{
	crtc_state->output_types |= BIT(INTEL_OUTPUT_WRITEBACK);
	crtc_state->output_format = INTEL_OUTPUT_FORMAT_RGB;
}

static bool
intel_writeback_get_hw_state(struct intel_encoder *encoder,
			     enum pipe *pipe)
{
	struct intel_display *display = to_intel_display(encoder);
	u8 pipe_mask = 0;
	u32 tmp;

	/* TODO need to be done for both the wd transcoder */
	tmp = intel_de_read(display,
			    TRANSCONF_WD(TRANSCODER_WD_0));
	if (!(tmp & WD_TRANS_ENABLE))
		return false;

	tmp = intel_de_read(display,
			    WD_TRANS_FUNC_CTL(TRANSCODER_WD_0));

	if (!(tmp & TRANS_WD_FUNC_ENABLE))
		return false;

	switch (tmp & WD_INPUT_SELECT_MASK) {
	case WD_INPUT_PIPE_A:
		pipe_mask |= BIT(PIPE_A);
		break;
	case WD_INPUT_PIPE_B:
		pipe_mask |= BIT(PIPE_B);
		break;
	case WD_INPUT_PIPE_C:
		pipe_mask |= BIT(PIPE_C);
		break;
	case WD_INPUT_PIPE_D:
		pipe_mask |= BIT(PIPE_D);
		break;
	default:
		MISSING_CASE(tmp & WD_INPUT_SELECT_MASK);
		fallthrough;
	}

	if (pipe_mask == 0)
		return false;

	*pipe = ffs(pipe_mask) - 1;

	return true;
}

int intel_writeback_init(struct intel_display *display)
{
	struct intel_encoder *encoder;
	struct intel_writeback_connector *writeback_conn;
	struct intel_connector *connector;
	int ret;

	writeback_conn = kzalloc(sizeof(*writeback_conn), GFP_KERNEL);
	if (!writeback_conn)
		return -ENOSPC;

	encoder = &writeback_conn->encoder;
	encoder->base.possible_crtcs = 0xf;
	ret = drm_encoder_init(display->drm, &encoder->base,
			       &drm_writeback_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret) {
		kfree(writeback_conn);
		return ret;
	}

	encoder->type = INTEL_OUTPUT_WRITEBACK;
	encoder->pipe_mask = ~0;
	encoder->cloneable = 0;
	encoder->get_config = intel_writeback_get_config;
	encoder->get_hw_state = intel_writeback_get_hw_state;
	encoder->compute_config = intel_writeback_compute_config;

	connector = &writeback_conn->connector;
	ret = intel_writeback_connector_alloc(connector);
	if (ret) {
		kfree(writeback_conn);
		return ret;
	}

	connector->base.interlace_allowed = 0;
	drm_connector_helper_add(&connector->base, &conn_helper_funcs);
	ret = drm_writeback_connector_init(display->drm, &connector->base,
					   &conn_funcs, &encoder->base,
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
	connector->get_hw_state = intel_connector_get_hw_state;
	connector->base.status = connector_status_disconnected;
	writeback_conn->frame_num = 1;

	return 0;
}
