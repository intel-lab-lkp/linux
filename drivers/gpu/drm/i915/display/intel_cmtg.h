/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Intel Corporation
 */

#ifndef __INTEL_CMTG_H__
#define __INTEL_CMTG_H__

struct intel_display;
struct intel_crtc_state;

void intel_cmtg_set_clk_select(const struct intel_crtc_state *crtc_state);
void intel_cmtg_sanitize(struct intel_display *display);
void intel_cmtg_enable(const struct intel_crtc_state *crtc_state);

#endif /* __INTEL_CMTG_H__ */
