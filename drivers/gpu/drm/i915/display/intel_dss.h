/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_DSS_H__
#define __INTEL_DSS_H__

#include "linux/types.h"

enum pipe;

struct intel_crtc_state;
struct intel_display;
struct intel_encoder;
struct intel_crtc;

u8 intel_dss_splitter_pipe_mask(struct intel_display *display);
void intel_dss_get_mso_config(struct intel_encoder *encoder,
			      struct intel_crtc_state *pipe_config);
void intel_dss_configure_mso(const struct intel_crtc_state *crtc_state);
void intel_dss_configure_dsi_dual_link_mode(struct intel_encoder *encoder,
					    const struct intel_crtc_state *pipe_config,
					    u8 dual_link, u8 pixel_overlap);
void intel_dss_reset(const struct intel_crtc_state *old_crtc_state);
void intel_dss_enable_uncompressed_joiner(const struct intel_crtc_state *crtc_state);
void intel_dss_enable_compressed_joiner(const struct intel_crtc_state *crtc_state,
					int vdsc_instances_per_pipe);
void intel_dss_get_dsc_config(struct intel_crtc_state *crtc_state);
void intel_dss_get_compressed_joiner_pipes(struct intel_display *display,
					   struct intel_crtc *crtc,
					   u8 *primary_pipes,
					   u8 *secondary_pipes);
void intel_dss_get_uncompressed_joiner_pipes(struct intel_display *display,
					     struct intel_crtc *crtc,
					     u8 *primary_pipes,
					     u8 *secondary_pipes);
u8 intel_dss_get_joined_pipe_mask(const struct intel_crtc_state *crtc_state);
enum pipe intel_dss_get_primary_joiner_pipe(const struct intel_crtc_state *crtc_state);
bool intel_dss_is_primary_joiner_pipe(const struct intel_crtc_state *crtc_state);
bool intel_dss_is_secondary_joiner_pipe(const struct intel_crtc_state *crtc_state);
u8 intel_dss_get_secondary_joiner_pipes(const struct intel_crtc_state *crtc_state);

#endif /* __INTEL_DSS_H__ */

