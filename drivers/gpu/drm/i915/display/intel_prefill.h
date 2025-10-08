/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_PREFILL_H__
#define __INTEL_PREFILL_H__

#include <linux/types.h>

struct intel_cdclk_state;
struct intel_crtc_state;

struct intel_prefill_ctx {
	/* .16 scanlines */
	struct {
		unsigned int fixed;
		unsigned int wm0;
		unsigned int scaler_1st;
		unsigned int scaler_2nd;
		unsigned int dsc;
		unsigned int full;
	} prefill;

	/* .16 adjustment factors */
	struct {
		unsigned int cdclk;
		unsigned int scaler_1st;
		unsigned int scaler_2nd;
	} adj;
};

void intel_prefill_init_worst(struct intel_prefill_ctx *ctx,
			      const struct intel_crtc_state *crtc_state);
void intel_prefill_init(struct intel_prefill_ctx *ctx,
			const struct intel_crtc_state *crtc_state,
			const struct intel_cdclk_state *cdclk_state);

bool intel_prefill_vblank_too_short(const struct intel_prefill_ctx *ctx,
				    const struct intel_crtc_state *crtc_state,
				    unsigned int latency_us);
int intel_prefill_min_guardband(const struct intel_prefill_ctx *ctx,
				const struct intel_crtc_state *crtc_state,
				unsigned int latency_us);
int intel_prefill_min_cdclk(const struct intel_prefill_ctx *ctx,
			    const struct intel_crtc_state *crtc_state);

#endif /* __INTEL_PREFILL_H__ */
