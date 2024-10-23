/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_VDSC_H__
#define __INTEL_VDSC_H__

#include <linux/types.h>

struct drm_printer;

enum intel_output_format;
enum transcoder;
struct intel_crtc;
struct intel_crtc_state;
struct intel_display;
struct intel_encoder;

bool intel_dsc_source_support(const struct intel_crtc_state *crtc_state);
void intel_uncompressed_joiner_enable(const struct intel_crtc_state *crtc_state);
void intel_dsc_enable(const struct intel_crtc_state *crtc_state);
void intel_dsc_disable(const struct intel_crtc_state *crtc_state);
int intel_dsc_compute_params(struct intel_crtc_state *pipe_config);
void intel_dsc_get_config(struct intel_crtc_state *crtc_state);
enum intel_display_power_domain
intel_dsc_power_domain(struct intel_crtc *crtc, enum transcoder cpu_transcoder);
struct intel_crtc *intel_dsc_get_bigjoiner_secondary(const struct intel_crtc *primary_crtc);
int intel_dsc_get_num_vdsc_instances(const struct intel_crtc_state *crtc_state);
void intel_dsc_dsi_pps_write(struct intel_encoder *encoder,
			     const struct intel_crtc_state *crtc_state);
void intel_dsc_dp_pps_write(struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state);
void intel_vdsc_state_dump(struct drm_printer *p, int indent,
			   const struct intel_crtc_state *crtc_state);
bool intel_dsc_can_use_pixel_replication(struct intel_display *display,
					 int mode_hdisplay, u8 slice_count,
					 enum intel_output_format output_format);
int intel_dsc_get_replicated_pixels(struct intel_display *display,
				    int mode_hdisplay,
				    int slice_count,
				    enum intel_output_format output_format);

#endif /* __INTEL_VDSC_H__ */
