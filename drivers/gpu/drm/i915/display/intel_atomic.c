/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * DOC: atomic modeset support
 *
 * The functions here implement the state management and hardware programming
 * dispatch required by the atomic modeset infrastructure.
 * See intel_atomic_plane.c for the plane-specific atomic functionality.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>

#include "i915_drv.h"
#include "i915_reg.h"
#include "intel_atomic.h"
#include "intel_cdclk.h"
#include "intel_display_types.h"
#include "intel_fdi.h"
#include "intel_global_state.h"
#include "intel_hdcp.h"
#include "intel_psr.h"
#include "intel_fb.h"
#include "skl_universal_plane.h"

/**
 * intel_digital_connector_atomic_get_property - hook for connector->atomic_get_property.
 * @connector: Connector to get the property for.
 * @state: Connector state to retrieve the property from.
 * @property: Property to retrieve.
 * @val: Return value for the property.
 *
 * Returns the atomic property value for a digital connector.
 */
int intel_digital_connector_atomic_get_property(struct drm_connector *connector,
						const struct drm_connector_state *state,
						struct drm_property *property,
						u64 *val)
{
	struct drm_device *dev = connector->dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(state);

	if (property == dev_priv->display.properties.force_audio)
		*val = intel_conn_state->force_audio;
	else if (property == dev_priv->display.properties.broadcast_rgb)
		*val = intel_conn_state->broadcast_rgb;
	else {
		drm_dbg_atomic(&dev_priv->drm,
			       "Unknown property [PROP:%d:%s]\n",
			       property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}

/**
 * intel_digital_connector_atomic_set_property - hook for connector->atomic_set_property.
 * @connector: Connector to set the property for.
 * @state: Connector state to set the property on.
 * @property: Property to set.
 * @val: New value for the property.
 *
 * Sets the atomic property value for a digital connector.
 */
int intel_digital_connector_atomic_set_property(struct drm_connector *connector,
						struct drm_connector_state *state,
						struct drm_property *property,
						u64 val)
{
	struct drm_device *dev = connector->dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_digital_connector_state *intel_conn_state =
		to_intel_digital_connector_state(state);

	if (property == dev_priv->display.properties.force_audio) {
		intel_conn_state->force_audio = val;
		return 0;
	}

	if (property == dev_priv->display.properties.broadcast_rgb) {
		intel_conn_state->broadcast_rgb = val;
		return 0;
	}

	drm_dbg_atomic(&dev_priv->drm, "Unknown property [PROP:%d:%s]\n",
		       property->base.id, property->name);
	return -EINVAL;
}

int intel_digital_connector_atomic_check(struct drm_connector *conn,
					 struct drm_atomic_state *state)
{
	struct drm_connector_state *new_state =
		drm_atomic_get_new_connector_state(state, conn);
	struct intel_digital_connector_state *new_conn_state =
		to_intel_digital_connector_state(new_state);
	struct drm_connector_state *old_state =
		drm_atomic_get_old_connector_state(state, conn);
	struct intel_digital_connector_state *old_conn_state =
		to_intel_digital_connector_state(old_state);
	struct drm_crtc_state *crtc_state;

	intel_hdcp_atomic_check(conn, old_state, new_state);

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);

	/*
	 * These properties are handled by fastset, and might not end
	 * up in a modeset.
	 */
	if (new_conn_state->force_audio != old_conn_state->force_audio ||
	    new_conn_state->broadcast_rgb != old_conn_state->broadcast_rgb ||
	    new_conn_state->base.colorspace != old_conn_state->base.colorspace ||
	    new_conn_state->base.picture_aspect_ratio != old_conn_state->base.picture_aspect_ratio ||
	    new_conn_state->base.content_type != old_conn_state->base.content_type ||
	    new_conn_state->base.scaling_mode != old_conn_state->base.scaling_mode ||
	    new_conn_state->base.privacy_screen_sw_state != old_conn_state->base.privacy_screen_sw_state ||
	    !drm_connector_atomic_hdr_metadata_equal(old_state, new_state))
		crtc_state->mode_changed = true;

	return 0;
}

/**
 * intel_digital_connector_duplicate_state - duplicate connector state
 * @connector: digital connector
 *
 * Allocates and returns a copy of the connector state (both common and
 * digital connector specific) for the specified connector.
 *
 * Returns: The newly allocated connector state, or NULL on failure.
 */
struct drm_connector_state *
intel_digital_connector_duplicate_state(struct drm_connector *connector)
{
	struct intel_digital_connector_state *state;

	state = kmemdup(connector->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_connector_duplicate_state(connector, &state->base);
	return &state->base;
}

/**
 * intel_connector_needs_modeset - check if connector needs a modeset
 * @state: the atomic state corresponding to this modeset
 * @connector: the connector
 */
bool
intel_connector_needs_modeset(struct intel_atomic_state *state,
			      struct drm_connector *connector)
{
	const struct drm_connector_state *old_conn_state, *new_conn_state;

	old_conn_state = drm_atomic_get_old_connector_state(&state->base, connector);
	new_conn_state = drm_atomic_get_new_connector_state(&state->base, connector);

	return old_conn_state->crtc != new_conn_state->crtc ||
	       (new_conn_state->crtc &&
		drm_atomic_crtc_needs_modeset(drm_atomic_get_new_crtc_state(&state->base,
									    new_conn_state->crtc)));
}

/**
 * intel_any_crtc_needs_modeset - check if any CRTC needs a modeset
 * @state: the atomic state corresponding to this modeset
 *
 * Returns true if any CRTC in @state needs a modeset.
 */
bool intel_any_crtc_needs_modeset(struct intel_atomic_state *state)
{
	struct intel_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	int i;

	for_each_new_intel_crtc_in_state(state, crtc, crtc_state, i) {
		if (intel_crtc_needs_modeset(crtc_state))
			return true;
	}

	return false;
}

struct intel_digital_connector_state *
intel_atomic_get_digital_connector_state(struct intel_atomic_state *state,
					 struct intel_connector *connector)
{
	struct drm_connector_state *conn_state;

	conn_state = drm_atomic_get_connector_state(&state->base,
						    &connector->base);
	if (IS_ERR(conn_state))
		return ERR_CAST(conn_state);

	return to_intel_digital_connector_state(conn_state);
}

/**
 * intel_crtc_duplicate_state - duplicate crtc state
 * @crtc: drm crtc
 *
 * Allocates and returns a copy of the crtc state (both common and
 * Intel-specific) for the specified crtc.
 *
 * Returns: The newly allocated crtc state, or NULL on failure.
 */
struct drm_crtc_state *
intel_crtc_duplicate_state(struct drm_crtc *crtc)
{
	const struct intel_crtc_state *old_crtc_state = to_intel_crtc_state(crtc->state);
	struct intel_crtc_state *crtc_state;

	crtc_state = kmemdup(old_crtc_state, sizeof(*crtc_state), GFP_KERNEL);
	if (!crtc_state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &crtc_state->uapi);

	/* copy color blobs */
	if (crtc_state->hw.degamma_lut)
		drm_property_blob_get(crtc_state->hw.degamma_lut);
	if (crtc_state->hw.ctm)
		drm_property_blob_get(crtc_state->hw.ctm);
	if (crtc_state->hw.gamma_lut)
		drm_property_blob_get(crtc_state->hw.gamma_lut);

	if (crtc_state->pre_csc_lut)
		drm_property_blob_get(crtc_state->pre_csc_lut);
	if (crtc_state->post_csc_lut)
		drm_property_blob_get(crtc_state->post_csc_lut);

	crtc_state->update_pipe = false;
	crtc_state->disable_lp_wm = false;
	crtc_state->disable_cxsr = false;
	crtc_state->update_wm_pre = false;
	crtc_state->update_wm_post = false;
	crtc_state->fifo_changed = false;
	crtc_state->preload_luts = false;
	crtc_state->wm.need_postvbl_update = false;
	crtc_state->do_async_flip = false;
	crtc_state->fb_bits = 0;
	crtc_state->update_planes = 0;
	crtc_state->dsb = NULL;

	return &crtc_state->uapi;
}

static void intel_crtc_put_color_blobs(struct intel_crtc_state *crtc_state)
{
	drm_property_blob_put(crtc_state->hw.degamma_lut);
	drm_property_blob_put(crtc_state->hw.gamma_lut);
	drm_property_blob_put(crtc_state->hw.ctm);

	drm_property_blob_put(crtc_state->pre_csc_lut);
	drm_property_blob_put(crtc_state->post_csc_lut);
}

void intel_crtc_free_hw_state(struct intel_crtc_state *crtc_state)
{
	intel_crtc_put_color_blobs(crtc_state);
}

/**
 * intel_crtc_destroy_state - destroy crtc state
 * @crtc: drm crtc
 * @state: the state to destroy
 *
 * Destroys the crtc state (both common and Intel-specific) for the
 * specified crtc.
 */
void
intel_crtc_destroy_state(struct drm_crtc *crtc,
			 struct drm_crtc_state *state)
{
	struct intel_crtc_state *crtc_state = to_intel_crtc_state(state);

	drm_WARN_ON(crtc->dev, crtc_state->dsb);

	__drm_atomic_helper_crtc_destroy_state(&crtc_state->uapi);
	intel_crtc_free_hw_state(crtc_state);
	kfree(crtc_state);
}

struct drm_atomic_state *
intel_atomic_state_alloc(struct drm_device *dev)
{
	struct intel_atomic_state *state = kzalloc(sizeof(*state), GFP_KERNEL);

	if (!state || drm_atomic_state_init(dev, &state->base) < 0) {
		kfree(state);
		return NULL;
	}

	return &state->base;
}

void intel_atomic_state_free(struct drm_atomic_state *_state)
{
	struct intel_atomic_state *state = to_intel_atomic_state(_state);

	drm_atomic_state_default_release(&state->base);
	kfree(state->global_objs);

	i915_sw_fence_fini(&state->commit_ready);

	kfree(state);
}

void intel_atomic_state_clear(struct drm_atomic_state *s)
{
	struct intel_atomic_state *state = to_intel_atomic_state(s);

	drm_atomic_state_default_clear(&state->base);
	intel_atomic_clear_global_state(state);

	/* state->internal not reset on purpose */

	state->dpll_set = state->modeset = false;
}

struct intel_crtc_state *
intel_atomic_get_crtc_state(struct drm_atomic_state *state,
			    struct intel_crtc *crtc)
{
	struct drm_crtc_state *crtc_state;
	crtc_state = drm_atomic_get_crtc_state(state, &crtc->base);
	if (IS_ERR(crtc_state))
		return ERR_CAST(crtc_state);

	return to_intel_crtc_state(crtc_state);
}

/**
 * intel_atomic_compute_pipe_bpp - compute pipe bpp limited by max link bpp
 * @crtc_state: the crtc state
 *
 * Compute the pipe bpp limited by the CRTC's maximum link bpp. Encoders can
 * call this function during state computation in the simple case where the
 * link bpp will always match the pipe bpp. This is the case for all non-DP
 * encoders, while DP encoders will use a link bpp lower than pipe bpp in case
 * of DSC compression.
 *
 * Returns %true in case of success, %false if pipe bpp would need to be
 * reduced below its valid range.
 */
bool intel_atomic_compute_pipe_bpp(struct intel_crtc_state *crtc_state)
{
	int pipe_bpp = min(crtc_state->pipe_bpp,
			   to_bpp_int(crtc_state->max_link_bpp_x16));

	pipe_bpp = rounddown(pipe_bpp, 2 * 3);

	if (pipe_bpp < 6 * 3)
		return false;

	crtc_state->pipe_bpp = pipe_bpp;

	return true;
}

/**
 * intel_atomic_reduce_link_bpp - reduce maximum link bpp for a selected pipe
 * @state: atomic state
 * @limits: link BW limits
 * @pipe_mask: mask of pipes to select from
 * @reason: explanation of why bpp reduction is needed
 *
 * Select the pipe from @pipe_mask with the biggest link bpp value and set the
 * maximum of link bpp in @limits below this value. Modeset the selected pipe,
 * so that its state will get recomputed.
 *
 * This function can be called to resolve a link's BW overallocation by reducing
 * the link bpp of one pipe on the link and hence reducing the total link BW.
 *
 * Returns
 *   - 0 in case of success
 *   - %-EINVAL if no pipe can further reduce its link bpp
 *   - Other negative error, if modesetting the selected pipe failed
 */
int intel_atomic_reduce_link_bpp(struct intel_atomic_state *state,
				 struct intel_link_bw_limits *limits,
				 u8 pipe_mask,
				 const char *reason)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	enum pipe max_bpp_pipe = INVALID_PIPE;
	struct intel_crtc *crtc;
	int max_bpp = 0;

	for_each_intel_crtc_in_pipe_mask(&i915->drm, crtc, pipe_mask) {
		struct intel_crtc_state *crtc_state;
		int pipe_bpp;

		if (limits->min_bpp_pipes & BIT(crtc->pipe))
			continue;

		crtc_state = intel_atomic_get_crtc_state(&state->base,
							 crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		if (crtc_state->dsc.compression_enable)
			pipe_bpp = crtc_state->dsc.compressed_bpp;
		else
			pipe_bpp = crtc_state->pipe_bpp;

		if (pipe_bpp > max_bpp) {
			max_bpp = pipe_bpp;
			max_bpp_pipe = crtc->pipe;
		}
	}

	if (max_bpp_pipe == INVALID_PIPE)
		return -EINVAL;

	limits->max_bpp_x16[max_bpp_pipe] = to_bpp_x16(max_bpp) - 1;

	return intel_modeset_pipes_in_mask(state, reason,
					   BIT(max_bpp_pipe), false);
}

static int intel_atomic_check_link(struct intel_atomic_state *state,
				   struct intel_link_bw_limits *limits)
{
	int ret;

	ret = intel_fdi_atomic_check_link(state, limits);
	if (ret)
		return ret;

	return 0;
}

static bool
assert_link_limit_change_valid(struct drm_i915_private *i915,
			       const struct intel_link_bw_limits *old_limits,
			       const struct intel_link_bw_limits *new_limits)
{
	bool bpps_changed = false;
	enum pipe pipe;

	for_each_pipe(i915, pipe) {
		/* The bpp limit can only decrease. */
		if (drm_WARN_ON(&i915->drm,
				new_limits->max_bpp_x16[pipe] >
				old_limits->max_bpp_x16[pipe]))
			return false;

		if (new_limits->max_bpp_x16[pipe] <
		    old_limits->max_bpp_x16[pipe])
			bpps_changed = true;
	}

	if (drm_WARN_ON(&i915->drm,
			!bpps_changed))
		return false;

	return true;
}

static bool
reset_link_bpp_limit_to_min(struct intel_atomic_state *state,
			    const struct intel_link_bw_limits *old_limits,
			    struct intel_link_bw_limits *new_limits,
			    enum pipe failed_pipe)
{
	if (failed_pipe == INVALID_PIPE)
		return false;

	if (new_limits->min_bpp_pipes & BIT(failed_pipe))
		return false;

	if (new_limits->max_bpp_x16[failed_pipe] ==
	    old_limits->max_bpp_x16[failed_pipe])
		return false;

	new_limits->max_bpp_x16[failed_pipe] =
		old_limits->max_bpp_x16[failed_pipe];
	new_limits->min_bpp_pipes |= BIT(failed_pipe);

	return true;
}

/**
 * intel_atomic_check_config_and_link - compute CRTC configs, resolving any BW limits
 * @state: atomic state
 *
 * Compute the configuration of all CRTCs in @state and resolve any BW
 * limitations on links shared by these CRTCs.
 *
 * Return 0 in case of success, or a negative error code otherwise.
 */
int intel_atomic_check_config_and_link(struct intel_atomic_state *state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_link_bw_limits new_limits = {};
	struct intel_link_bw_limits old_limits;
	enum pipe pipe;
	int ret;

	for_each_pipe(i915, pipe)
		new_limits.max_bpp_x16[pipe] = INT_MAX;

	old_limits = new_limits;

	while (true) {
		enum pipe failed_pipe;

		ret = intel_atomic_check_config(state, &new_limits,
						&failed_pipe);
		if (ret) {
			if (ret == -EINVAL &&
			    reset_link_bpp_limit_to_min(state,
							&old_limits,
							&new_limits,
							failed_pipe))
				continue;

			break;
		}

		old_limits = new_limits;

		ret = intel_atomic_check_link(state, &new_limits);
		if (ret != -EAGAIN)
			break;

		if (!assert_link_limit_change_valid(i915,
						    &old_limits,
						    &new_limits)) {
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}
