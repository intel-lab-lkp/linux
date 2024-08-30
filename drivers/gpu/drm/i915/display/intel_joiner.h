/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_JOINER_H__
#define __INTEL_JOINER_H__

#include "linux/types.h"

enum pipe;
struct drm_display_mode;
struct drm_i915_private;
struct intel_atomic_state;
struct intel_crtc_state;

enum pipe intel_joiner_get_primary_pipe(const struct intel_crtc_state *crtc_state);
int intel_joiner_get_num_pipes(const struct intel_crtc_state *crtc_state);
void intel_joiner_adjust_timings(const struct intel_crtc_state *crtc_state,
				 struct drm_display_mode *mode);
void intel_joiner_compute_pipe_src(struct intel_crtc_state *crtc_state);
void intel_joiner_adjust_pipe_src(struct intel_crtc_state *crtc_state);
u8 intel_joiner_supported_pipes(struct drm_i915_private *i915);
void intel_joiner_enabled_pipes(struct drm_i915_private *dev_priv,
				u8 *primary_pipes, u8 *secondary_pipes);
enum pipe intel_joiner_find_primary_pipe(enum pipe pipe, u8 primary_pipes, u8 secondary_pipes);
u8 intel_joiner_find_secondary_pipes(enum pipe pipe, u8 primary_pipes, u8 secondary_pipes);
void intel_joiner_get_config(struct intel_crtc_state *crtc_state);
int intel_joiner_add_affected_planes(struct intel_atomic_state *state);
int intel_joiner_add_affected_crtcs(struct intel_atomic_state *state);
u8 intel_joiner_crtc_joined_pipe_mask(const struct intel_crtc_state *crtc_state);

#endif/* __INTEL_JOINER_H__ */
