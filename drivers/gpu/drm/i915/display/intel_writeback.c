// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/slab.h>
#include <linux/err.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_writeback.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_edid.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_vblank.h>

#include "intel_atomic.h"
#include "intel_connector.h"
#include "intel_crtc.h"
#include "intel_de.h"
#include "intel_display_driver.h"
#include "intel_display_regs.h"
#include "intel_display_types.h"
#include "intel_display_utils.h"
#include "intel_fb_pin.h"
#include "intel_writeback.h"
#include "intel_writeback_helper.h"
#include "intel_writeback_reg.h"

struct intel_writeback_connector {
	struct intel_connector connector;
	struct intel_encoder encoder;
	struct intel_writeback_job *job;
	enum transcoder trans;
	enum pipe pipe;
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

static struct intel_writeback_connector
*conn_to_intel_writeback_connector(struct intel_connector *connector)
{
	return container_of(connector, struct intel_writeback_connector, connector);
}

static struct intel_writeback_connector
*enc_to_intel_writeback_connector(struct intel_encoder *encoder)
{
	return container_of(encoder, struct intel_writeback_connector, encoder);
}

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

static void intel_writeback_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
	kfree(connector);
}

static int intel_writeback_check_format(u32 format)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(writeback_formats); i++) {
		if (writeback_formats[i] == format)
			return 0;
	}

	return -EINVAL;
}

static int intel_writeback_atomic_check(struct drm_connector *connector,
					struct drm_atomic_state *state)
{
	struct drm_connector_state *conn_state =
		drm_atomic_get_new_connector_state(state, connector);
	struct drm_crtc_state *crtc_state;
	struct drm_framebuffer *fb;
	int ret;

	/* We return 0 since this is called while disabling writeback encoder */
	if (!conn_state->crtc)
		return 0;

	/* We do not allow a blank commit when using writeback connector */
	if (!conn_state->writeback_job)
		return -EINVAL;

	fb = conn_state->writeback_job->fb;
	if (!fb)
		return -EINVAL;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (fb->width != crtc_state->mode.hdisplay ||
	    fb->height != crtc_state->mode.vdisplay)
		return -EINVAL;

	ret = intel_writeback_check_format(fb->format->format);
	if (ret) {
		drm_dbg_kms(connector->dev,
			    "Unsupported drm format sent in writeback job\n");
		return ret;
	}

	return 0;
}

static int
get_color_mode_bpp(struct intel_display *display, u32 color_format)
{
	int bpp = 0;

	switch (color_format) {
	case DRM_FORMAT_XYUV8888:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_XVYU2101010:
		bpp = 4;
		break;
	default:
		drm_err(display->drm, "Unsupported format for writeback\n");
		break;
	}

	return bpp;
}

static void intel_writeback_capture(struct intel_atomic_state *state,
				    struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);
	struct intel_writeback_connector *wb_conn =
		conn_to_intel_writeback_connector(connector);
	struct drm_connector_state *conn_state =
		drm_atomic_get_new_connector_state(&state->base, &connector->base);
	struct intel_crtc *crtc = intel_crtc_for_pipe(display, wb_conn->pipe);
	struct intel_crtc_state *crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	struct drm_writeback_job *wb_job = conn_state->writeback_job;
	struct intel_writeback_job *job = conn_state->writeback_job->priv;
	enum transcoder trans = wb_conn->trans;
	u32 val = 0;
	int bpp;

	bpp = get_color_mode_bpp(display, wb_job->fb->format->format);
	val = DIV_ROUND_UP((adjusted_mode->hdisplay * bpp), 64);
	intel_de_write(display, WD_STRIDE(trans), WD_STRIDE_VAL(val));

	val = intel_get_ggtt_addr(job->vma);
	intel_de_write(display, WD_SURF(trans), val);

	val = 0;
	val |= START_TRIGGER_FRAME | WD_FRAME_NUMBER(wb_conn->frame_num);
	intel_de_rmw(display, WD_TRANS_FUNC_CTL(trans),
		     START_TRIGGER_FRAME | WD_FRAME_NUMBER_MASK,
		     val);

	if (intel_de_wait_for_set_ms(display, WD_FRAME_STATUS(trans),
				     WD_FRAME_COMPLETE, 50)) {
		drm_dbg_kms(display->drm,
			    "Frame was not captured after triggering a capture\n");
		intel_de_rmw(display, WD_TRANS_FUNC_CTL(trans),
			     STOP_TRIGGER_FRAME,
			     STOP_TRIGGER_FRAME);
	} else {
		drm_writeback_signal_completion(&connector->base, 0);
		intel_de_write(display, WD_FRAME_STATUS(trans), WD_FRAME_COMPLETE);
		wb_conn->frame_num++;
		if (wb_conn->frame_num > 7)
			wb_conn->frame_num = 1;
		wb_conn->job = NULL;
	}
}

void intel_writeback_atomic_commit(struct intel_atomic_state *state)
{
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	int i;

	for_each_new_connector_in_state(&state->base, connector, conn_state, i) {
		struct intel_connector *intel_connector = to_intel_connector(connector);

		if (!conn_state)
			return;

		if (conn_state->writeback_job && conn_state->writeback_job->fb) {
			WARN_ON(connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK);

			drm_writeback_queue_job(connector, conn_state);
			intel_writeback_capture(state, intel_connector);
		}
	}
}

static void
intel_writeback_enable_interrupts(struct intel_display *display,
				  enum transcoder trans)
{
	u32 tmp;

	tmp = intel_de_read(display, WD_IIR(trans));
	intel_de_write_fw(display, WD_IIR(trans), tmp);

	tmp = ~(WD_GTT_FAULT_INT | WD_WRITE_COMPLETE_INT |
		WD_VBLANK_INT | WD_CAPTURING_INT);
	intel_de_write(display, WD_IMR(trans), tmp);
}

static void intel_writeback_enable_encoder(struct intel_atomic_state *state,
					   struct intel_encoder *encoder,
					   const struct intel_crtc_state *crtc_state,
					   const struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct intel_writeback_connector *wb_conn =
		enc_to_intel_writeback_connector(encoder);
	struct intel_writeback_job *job = wb_conn->job;
	const struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;
	enum transcoder trans = crtc_state->cpu_transcoder;
	struct intel_crtc *pipe_crtc;
	struct drm_framebuffer *fb;
	u32 val = 0, hactive, vactive;
	int i = 0;

	if (!conn_state->writeback_job)
		return;

	wb_conn->trans = trans;
	wb_conn->pipe = crtc->pipe;
	fb = job->fb;
	hactive = adjusted_mode->hdisplay;
	vactive = adjusted_mode->vdisplay;
	intel_writeback_enable_interrupts(display, trans);

	/* Configure WD_STRIDE, WD_SURF and WD_TAIL_CFG */
	/* Enable Planes, Pipes and Transcoder */
	/* TRANSCODER TIMINGS and other transcoder setting*/
	/* minimum hactive as per bspec: 64 pixels */
	if (hactive < 64)
		drm_err(display->drm, "hactive is less then 64 pixels\n");

	intel_de_write(display, TRANS_HTOTAL(display, trans), HACTIVE(hactive - 1));
	intel_de_write(display, TRANS_VTOTAL(display, trans), VACTIVE(vactive - 1));

	val = 0;
	/* 2f) Configure and enable TRANS_WD_FUNC_CTL */
	switch (crtc->pipe) {
	default:
		fallthrough;
	case PIPE_A:
		val |= WD_INPUT_PIPE_A;
		break;
	case PIPE_B:
		val |= WD_INPUT_PIPE_B;
		break;
	case PIPE_C:
		val |= WD_INPUT_PIPE_C;
		break;
	case PIPE_D:
		val |= WD_INPUT_PIPE_D;
		break;
	}

	switch (fb->format->format) {
	default:
		fallthrough;
	case DRM_FORMAT_YUYV:
		val |= WD_PIX_FMT_YUYV;
		break;
	case DRM_FORMAT_XYUV8888:
		val |= WD_PIX_FMT_XYUV8888;
		break;
	case DRM_FORMAT_XBGR8888:
		val |= WD_PIX_FMT_XBGR8888;
		break;
	case DRM_FORMAT_XBGR2101010:
		val |= WD_PIX_FMT_XBGR2101010;
		break;
	}

	val |= TRANS_WD_FUNC_ENABLE | WD_TRIGGERED_CAP_MODE_ENABLE |
		WD_DISABLE_POINTERS;
	intel_de_write(display, WD_TRANS_FUNC_CTL(trans), val);

	if (DISPLAY_VER(display) >= 13)
		intel_de_rmw(display, PIPE_CHICKEN(crtc->pipe),
			     UNDERRUN_RECOVERY_DISABLE_ADLP,
			     UNDERRUN_RECOVERY_DISABLE_ADLP);

	/*  Configure and enable TRANS_CONF */
	intel_de_write(display, TRANSCONF_WD(trans), WD_TRANS_ENABLE);
	intel_de_posting_read(display, TRANSCONF_WD(trans));

	for_each_pipe_crtc_modeset_enable(display, pipe_crtc, crtc_state, i) {
		const struct intel_crtc_state *pipe_crtc_state =
			intel_atomic_get_new_crtc_state(state, pipe_crtc);

		intel_crtc_vblank_on(pipe_crtc_state);
	}
}

static const struct drm_encoder_funcs drm_writeback_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

const struct drm_connector_funcs conn_funcs = {
	.detect = intel_writeback_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state = intel_digital_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.destroy = intel_writeback_connector_destroy,
};

static const struct drm_connector_helper_funcs conn_helper_funcs = {
	.get_modes = intel_writeback_get_modes,
	.mode_valid = intel_writeback_mode_valid,
	.atomic_check = intel_writeback_atomic_check,
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

void intel_writeback_isr_handler(struct intel_display *display)
{
	struct intel_encoder *encoder;
	struct intel_writeback_connector *wb_conn;
	struct intel_crtc *crtc;
	u32 iir;

	for_each_intel_encoder(display->drm, encoder) {
		if (encoder->type != INTEL_OUTPUT_WRITEBACK)
			continue;

		wb_conn = enc_to_intel_writeback_connector(encoder);
		if (!wb_conn->job) {
			drm_err(display->drm, "No writeback job for the connector\n");
			continue;
		}

		crtc = intel_crtc_for_pipe(display, wb_conn->pipe);
		iir = intel_de_read(display, WD_IIR(wb_conn->trans));
		if (iir & WD_GTT_FAULT_INT)
			drm_err(display->drm, " GTT fault during writeback\n");
		if (iir & WD_WRITE_COMPLETE_INT)
			drm_dbg_kms(display->drm, "Writeback job write completed\n");
		if (iir & WD_VBLANK_INT) {
			drm_crtc_handle_vblank(&crtc->base);
			drm_dbg_kms(display->drm, "Writeback vblank raised\n");
		}
		if (iir & WD_CAPTURING_INT)
			drm_dbg_kms(display->drm, "Writeback job capture has started\n");

		intel_de_write(display, WD_IIR(wb_conn->trans), iir);
	}
}

static void
intel_writeback_disable_encoder(struct intel_atomic_state *state,
				struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state,
				const struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(encoder);
	struct intel_writeback_connector *wb_conn =
		enc_to_intel_writeback_connector(encoder);
	struct intel_crtc *pipe_crtc;
	int i = 0;

	for_each_pipe_crtc_modeset_disable(display, pipe_crtc, crtc_state, i) {
		const struct intel_crtc_state *old_pipe_crtc_state =
			intel_atomic_get_old_crtc_state(state, pipe_crtc);

		intel_crtc_vblank_off(old_pipe_crtc_state);
	}

	intel_de_rmw(display, TRANSCONF_WD(crtc_state->cpu_transcoder), WD_TRANS_ENABLE,
		     REG_FIELD_PREP(WD_TRANS_ENABLE, 0));
	intel_de_rmw(display, WD_TRANS_FUNC_CTL(crtc_state->cpu_transcoder),
		     TRANS_WD_FUNC_ENABLE,
		     REG_FIELD_PREP(TRANS_WD_FUNC_ENABLE, 0));
	wb_conn->frame_num = 1;
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
	encoder->enable = intel_writeback_enable_encoder;
	encoder->disable = intel_writeback_disable_encoder;

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
