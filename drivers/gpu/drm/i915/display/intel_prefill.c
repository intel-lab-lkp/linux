// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <linux/debugfs.h>

#include <drm/drm_print.h>

#include "intel_cdclk.h"
#include "intel_display_core.h"
#include "intel_display_types.h"
#include "intel_prefill.h"
#include "intel_vdsc.h"
#include "skl_scaler.h"
#include "skl_watermark.h"

#define FP_FMT "%u.%06u"
#define FP_ARG(val) (val) >> 16, (((val) & 0xffff) * 15625) >> 10

static void intel_prefill_dump(struct intel_prefill_ctx *ctx,
			       const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);

	drm_dbg_kms(display->drm, "prefill      prefill.fixed: " FP_FMT "\n", FP_ARG(ctx->prefill.fixed));
	drm_dbg_kms(display->drm, "prefill        prefill.wm0: " FP_FMT "\n", FP_ARG(ctx->prefill.wm0));
	drm_dbg_kms(display->drm, "prefill prefill.scaler_1st: " FP_FMT "\n", FP_ARG(ctx->prefill.scaler_1st));
	drm_dbg_kms(display->drm, "prefill prefill.scaler_2nd: " FP_FMT "\n", FP_ARG(ctx->prefill.scaler_2nd));
	drm_dbg_kms(display->drm, "prefill        prefill.dsc: " FP_FMT "\n", FP_ARG(ctx->prefill.dsc));
	drm_dbg_kms(display->drm, "prefill       prefill.full: " FP_FMT "\n", FP_ARG(ctx->prefill.full));

	drm_dbg_kms(display->drm, "prefill          adj.cdclk: " FP_FMT "\n", FP_ARG(ctx->adj.cdclk));
	drm_dbg_kms(display->drm, "prefill     adj.scaler_1st: " FP_FMT "\n", FP_ARG(ctx->adj.scaler_1st));
	drm_dbg_kms(display->drm, "prefill     adj.scaler_2nd: " FP_FMT "\n", FP_ARG(ctx->adj.scaler_2nd));
}

static unsigned int prefill_usecs_to_lines(const struct intel_crtc_state *crtc_state, unsigned int usecs)
{
	const struct drm_display_mode *pipe_mode = &crtc_state->hw.pipe_mode;

	return DIV_ROUND_UP_ULL(mul_u32_u32(pipe_mode->crtc_clock, usecs << 16),
				pipe_mode->crtc_htotal * 1000);
}

static void _intel_prefill_init(struct intel_prefill_ctx *ctx,
				const struct intel_crtc_state *crtc_state)
{
	ctx->prefill.fixed = crtc_state->framestart_delay;

	/* 20 usec for translation walks/etc. */
	ctx->prefill.fixed += prefill_usecs_to_lines(crtc_state, 20);

	ctx->prefill.dsc = intel_vdsc_prefill_lines(crtc_state);

	ctx->prefill.full = 0;
}

static void intel_prefill_init_nocdclk_worst(struct intel_prefill_ctx *ctx,
					     const struct intel_crtc_state *crtc_state)
{
	_intel_prefill_init(ctx, crtc_state);

	ctx->prefill.wm0 = skl_wm0_prefill_lines_worst(crtc_state);
	ctx->prefill.scaler_1st = skl_scaler_1st_prefill_lines_worst(crtc_state);
	ctx->prefill.scaler_2nd = skl_scaler_2nd_prefill_lines_worst(crtc_state);

	ctx->adj.scaler_1st = skl_scaler_1st_prefill_adjustment_worst(crtc_state);
	ctx->adj.scaler_2nd = skl_scaler_2nd_prefill_adjustment_worst(crtc_state);
}

static void intel_prefill_init_nocdclk(struct intel_prefill_ctx *ctx,
				       const struct intel_crtc_state *crtc_state)
{
	_intel_prefill_init(ctx, crtc_state);

	ctx->prefill.wm0 = skl_wm0_prefill_lines(crtc_state);
	ctx->prefill.scaler_1st = skl_scaler_1st_prefill_lines(crtc_state);
	ctx->prefill.scaler_2nd = skl_scaler_2nd_prefill_lines(crtc_state);

	ctx->adj.scaler_1st = skl_scaler_1st_prefill_adjustment(crtc_state);
	ctx->adj.scaler_2nd = skl_scaler_2nd_prefill_adjustment(crtc_state);
}

static unsigned int prefill_adjust(unsigned int value, unsigned int factor)
{
	return DIV_ROUND_UP_ULL(mul_u32_u32(value, factor), 0x10000);
}

static unsigned int prefill_lines_nocdclk(const struct intel_prefill_ctx *ctx)
{
	unsigned int prefill = 0;

	prefill += ctx->prefill.dsc;
	prefill = prefill_adjust(prefill, ctx->adj.scaler_2nd);

	prefill += ctx->prefill.scaler_2nd;
	prefill = prefill_adjust(prefill, ctx->adj.scaler_1st);

	prefill += ctx->prefill.scaler_1st;
	prefill += ctx->prefill.wm0;

	return prefill;
}

static unsigned int prefill_lines_cdclk(const struct intel_prefill_ctx *ctx)
{
	return prefill_adjust(prefill_lines_nocdclk(ctx), ctx->adj.cdclk);
}

static unsigned int prefill_lines_full(const struct intel_prefill_ctx *ctx)
{
	return ctx->prefill.fixed + prefill_lines_cdclk(ctx);
}

void intel_prefill_init_worst(struct intel_prefill_ctx *ctx,
			      const struct intel_crtc_state *crtc_state)
{
	intel_prefill_init_nocdclk_worst(ctx, crtc_state);

	ctx->adj.cdclk = intel_cdclk_prefill_adjustment_worst(crtc_state);

	ctx->prefill.full = prefill_lines_full(ctx);

	intel_prefill_dump(ctx, crtc_state);
}

void intel_prefill_init(struct intel_prefill_ctx *ctx,
			const struct intel_crtc_state *crtc_state,
			const struct intel_cdclk_state *cdclk_state)
{
	intel_prefill_init_nocdclk(ctx, crtc_state);

	ctx->adj.cdclk = intel_cdclk_prefill_adjustment(crtc_state, cdclk_state);

	ctx->prefill.full = prefill_lines_full(ctx);

	intel_prefill_dump(ctx, crtc_state);
}

static unsigned int prefill_lines_with_latency(const struct intel_prefill_ctx *ctx,
					       const struct intel_crtc_state *crtc_state,
					       unsigned int latency_us)
{
	return ctx->prefill.full + prefill_usecs_to_lines(crtc_state, latency_us);
}

int intel_prefill_min_guardband(const struct intel_prefill_ctx *ctx,
				const struct intel_crtc_state *crtc_state,
				unsigned int latency_us)
{
	unsigned int prefill = prefill_lines_with_latency(ctx, crtc_state, latency_us);

	return DIV_ROUND_UP(prefill, 0x10000);
}

static int intel_guardband(const struct intel_crtc_state *crtc_state)
{
	const struct drm_display_mode *pipe_mode = &crtc_state->hw.pipe_mode;

	if (crtc_state->vrr.enable)
		return crtc_state->vrr.guardband;
	else
		return pipe_mode->crtc_vblank_end - pipe_mode->crtc_vblank_start;
}

static int intel_prefill_guardband(const struct intel_crtc_state *crtc_state)
{
	return intel_guardband(crtc_state) << 16;
}

bool intel_prefill_vblank_too_short(const struct intel_prefill_ctx *ctx,
				    const struct intel_crtc_state *crtc_state,
				    unsigned int latency_us)
{
	struct intel_display *display = to_intel_display(crtc_state);
	unsigned int guardband = intel_prefill_guardband(crtc_state);
	unsigned int prefill = prefill_lines_with_latency(ctx, crtc_state, latency_us);

	drm_dbg_kms(display->drm, "  prefill (%d): " FP_FMT "\n", latency_us, FP_ARG(prefill));
	drm_dbg_kms(display->drm, "guardband (%d): " FP_FMT "\n", latency_us, FP_ARG(guardband));

	drm_dbg_kms(display->drm, "min guardband (%d): %d lines\n", latency_us,
		    intel_prefill_min_guardband(ctx, crtc_state, latency_us));
	drm_dbg_kms(display->drm, "min cdclk     (%d): %d khz\n", latency_us,
		    intel_prefill_min_cdclk(ctx, crtc_state));

	return guardband < prefill;
}

int intel_prefill_min_cdclk(const struct intel_prefill_ctx *ctx,
			    const struct intel_crtc_state *crtc_state)
{
	unsigned int prefill_unadjusted = prefill_lines_nocdclk(ctx);
	unsigned int prefill_available = intel_prefill_guardband(crtc_state) -
		ctx->prefill.fixed;

	return intel_cdclk_min_cdclk_for_prefill(crtc_state, prefill_unadjusted,
						 prefill_available);
}
