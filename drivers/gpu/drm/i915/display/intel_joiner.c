// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <drm/drm_rect.h>

#include "i915_drv.h"
#include "intel_atomic.h"
#include "intel_display.h"
#include "intel_display_device.h"
#include "intel_display_types.h"
#include "intel_dss.h"
#include "intel_joiner.h"

enum pipe intel_joiner_get_primary_pipe(const struct intel_crtc_state *crtc_state)
{
	return ffs(crtc_state->joiner_pipes) - 1;
}

int intel_joiner_get_num_pipes(const struct intel_crtc_state *crtc_state)
{
	return hweight8(crtc_state->joiner_pipes);
}

void intel_joiner_adjust_timings(const struct intel_crtc_state *crtc_state,
				 struct drm_display_mode *mode)
{
	int num_pipes = intel_joiner_get_num_pipes(crtc_state);

	if (num_pipes < 2)
		return;

	mode->crtc_clock /= num_pipes;
	mode->crtc_hdisplay /= num_pipes;
	mode->crtc_hblank_start /= num_pipes;
	mode->crtc_hblank_end /= num_pipes;
	mode->crtc_hsync_start /= num_pipes;
	mode->crtc_hsync_end /= num_pipes;
	mode->crtc_htotal /= num_pipes;
}

void intel_joiner_compute_pipe_src(struct intel_crtc_state *crtc_state)
{
	int num_pipes = intel_joiner_get_num_pipes(crtc_state);
	int width, height;

	if (num_pipes < 2)
		return;

	width = drm_rect_width(&crtc_state->pipe_src);
	height = drm_rect_height(&crtc_state->pipe_src);

	drm_rect_init(&crtc_state->pipe_src, 0, 0,
		      width / num_pipes, height);
}

void intel_joiner_adjust_pipe_src(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	int num_pipes = intel_joiner_get_num_pipes(crtc_state);
	enum pipe primary_pipe, pipe = crtc->pipe;
	int width;

	if (num_pipes < 2)
		return;

	primary_pipe = intel_joiner_get_primary_pipe(crtc_state);
	width = drm_rect_width(&crtc_state->pipe_src);

	drm_rect_translate_to(&crtc_state->pipe_src,
			      (pipe - primary_pipe) * width, 0);
}

u8 intel_joiner_supported_pipes(struct drm_i915_private *i915)
{
	u8 pipes;

	if (DISPLAY_VER(i915) >= 12)
		pipes = BIT(PIPE_A) | BIT(PIPE_B) | BIT(PIPE_C) | BIT(PIPE_D);
	else if (DISPLAY_VER(i915) >= 11)
		pipes = BIT(PIPE_B) | BIT(PIPE_C);
	else
		pipes = 0;

	return pipes & DISPLAY_RUNTIME_INFO(i915)->pipe_mask;
}

void intel_joiner_enabled_pipes(struct drm_i915_private *dev_priv,
				u8 *primary_pipes, u8 *secondary_pipes)
{
	struct intel_crtc *crtc;

	*primary_pipes = 0;
	*secondary_pipes = 0;

	for_each_intel_crtc_in_pipe_mask(&dev_priv->drm, crtc,
					 intel_joiner_supported_pipes(dev_priv)) {
		intel_dss_get_compressed_joiner_pipes(crtc,
						      primary_pipes,
						      secondary_pipes);

		intel_dss_get_uncompressed_joiner_pipes(crtc,
							primary_pipes,
							secondary_pipes);
	}

	/* Joiner pipes should always be consecutive primary and secondary */
	drm_WARN(&dev_priv->drm, *secondary_pipes != *primary_pipes << 1,
		 "Joiner misconfigured (primary pipes 0x%x, secondary pipes 0x%x)\n",
		 *primary_pipes, *secondary_pipes);
}

enum pipe intel_joiner_find_primary_pipe(enum pipe pipe, u8 primary_pipes, u8 secondary_pipes)
{
	if ((secondary_pipes & BIT(pipe)) == 0)
		return pipe;

	/* ignore everything above our pipe */
	primary_pipes &= ~GENMASK(7, pipe);

	/* highest remaining bit should be our primary pipe */
	return fls(primary_pipes) - 1;
}

u8 intel_joiner_find_secondary_pipes(enum pipe pipe, u8 primary_pipes, u8 secondary_pipes)
{
	enum pipe primary_pipe, next_primary_pipe;

	primary_pipe = intel_joiner_find_primary_pipe(pipe, primary_pipes, secondary_pipes);

	if ((primary_pipes & BIT(primary_pipe)) == 0)
		return 0;

	/* ignore our primary pipe and everything below it */
	primary_pipes &= ~GENMASK(primary_pipe, 0);
	/* make sure a high bit is set for the ffs() */
	primary_pipes |= BIT(7);
	/* lowest remaining bit should be the next primary pipe */
	next_primary_pipe = ffs(primary_pipes) - 1;

	return secondary_pipes & GENMASK(next_primary_pipe - 1, primary_pipe);
}

void intel_joiner_get_config(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	u8 primary_pipes, secondary_pipes;
	enum pipe pipe = crtc->pipe;

	intel_joiner_enabled_pipes(i915, &primary_pipes, &secondary_pipes);

	if (((primary_pipes | secondary_pipes) & BIT(pipe)) == 0)
		return;

	crtc_state->joiner_pipes =
		BIT(intel_joiner_find_primary_pipe(pipe, primary_pipes, secondary_pipes)) |
		intel_joiner_find_secondary_pipes(pipe, primary_pipes, secondary_pipes);
}

int intel_joiner_add_affected_planes(struct intel_atomic_state *state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	const struct intel_crtc_state *crtc_state;
	struct intel_crtc *crtc;
	int i;

	for_each_new_intel_crtc_in_state(state, crtc, crtc_state, i) {
		struct intel_crtc *other;

		for_each_intel_crtc_in_pipe_mask(&i915->drm, other,
						 crtc_state->joiner_pipes) {
			int ret;

			if (crtc == other)
				continue;

			ret = intel_crtc_add_joiner_planes(state, crtc, other);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int intel_joiner_add_affected_crtcs(struct intel_atomic_state *state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_crtc_state *crtc_state;
	struct intel_crtc *crtc;
	u8 affected_pipes = 0;
	u8 modeset_pipes = 0;
	int i;

	for_each_new_intel_crtc_in_state(state, crtc, crtc_state, i) {
		affected_pipes |= crtc_state->joiner_pipes;
		if (intel_crtc_needs_modeset(crtc_state))
			modeset_pipes |= crtc_state->joiner_pipes;
	}

	for_each_intel_crtc_in_pipe_mask(&i915->drm, crtc, affected_pipes) {
		crtc_state = intel_atomic_get_crtc_state(&state->base, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);
	}

	for_each_intel_crtc_in_pipe_mask(&i915->drm, crtc, modeset_pipes) {
		int ret;

		crtc_state = intel_atomic_get_new_crtc_state(state, crtc);

		crtc_state->uapi.mode_changed = true;

		ret = drm_atomic_add_affected_connectors(&state->base, &crtc->base);
		if (ret)
			return ret;

		ret = intel_atomic_add_affected_planes(state, crtc);
		if (ret)
			return ret;
	}

	for_each_new_intel_crtc_in_state(state, crtc, crtc_state, i) {
		/* Kill old joiner link, we may re-establish afterwards */
		if (intel_crtc_needs_modeset(crtc_state) &&
		    intel_joiner_crtc_is_joiner_primary(crtc_state))
			intel_crtc_kill_joiner_secondaries(state, crtc);
	}

	return 0;
}

u8 intel_joiner_crtc_joined_pipe_mask(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	return BIT(crtc->pipe) | crtc_state->joiner_pipes;
}

bool intel_joiner_crtc_is_joiner_primary(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	return crtc_state->joiner_pipes &&
		crtc->pipe == intel_joiner_get_primary_pipe(crtc_state);
}

bool intel_joiner_crtc_is_joiner_secondary(const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	return crtc_state->joiner_pipes &&
		crtc->pipe != intel_joiner_get_primary_pipe(crtc_state);
}
