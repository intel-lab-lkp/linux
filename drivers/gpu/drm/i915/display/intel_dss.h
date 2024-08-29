/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_DSS_H__
#define __INTEL_DSS_H__

#include "linux/types.h"

struct intel_crtc_state;
struct intel_display;
struct intel_encoder;

u8 intel_dss_mso_pipe_mask(struct intel_display *display);
void intel_dss_mso_get_config(struct intel_encoder *encoder,
			      struct intel_crtc_state *pipe_config);
void intel_dss_mso_configure(const struct intel_crtc_state *crtc_state);
void intel_dss_dsi_dual_link_mode_configure(struct intel_encoder *encoder,
					    const struct intel_crtc_state *pipe_config,
					    u8 dual_link, u8 pixel_overlap);
void intel_dss_reset(const struct intel_crtc_state *old_crtc_state);
void intel_dss_enable_uncompressed_joiner(const struct intel_crtc_state *crtc_state);
void intel_dss_enable_compressed_joiner(const struct intel_crtc_state *crtc_state,
					int vdsc_instances_per_pipe);
void intel_dss_dsc_get_config(struct intel_crtc_state *crtc_state);

#endif /* __INTEL_DSS_H__ */

