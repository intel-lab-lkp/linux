// SPDX-License-Identifier: GPL-2.0-only
/*
 * Low-delay IDR + P structure of the RKVENC v1 encoder: one short-term
 * reference, no B frames. A failed frame never commits, so rollback is
 * free.
 */

#include "rkvenc.h"

void rkvenc_gop_init(struct rkvenc_ctx *ctx)
{
	ctx->gop.frames_since_idr = 0;
	ctx->gop.idr_pic_id = 0;
	ctx->cur = 0;
	ctx->last = 0;
	ctx->last_good_valid = false;
	ctx->consecutive_errors = 0;
}

/* The hardware codes P frames of these sizes wrongly */
static bool rkvenc_gop_intra_only(struct rkvenc_ctx *ctx)
{
	u32 w16 = ALIGN(ctx->width, 16) / 16;
	u32 h16 = ALIGN(ctx->height, 16) / 16;

	if (ctx->codec == RKVENC_CODEC_HEVC)
		return ctx->height <= 64;

	return !((w16 >= 3 && h16 >= 2) || (w16 == 1 && h16 == 1));
}

bool rkvenc_gop_is_idr(struct rkvenc_ctx *ctx)
{
	struct rkvenc_gop *g = &ctx->gop;

	if (g->frames_since_idr == 0 || rkvenc_gop_intra_only(ctx))
		return true;
	if (ctx->c.gop_size && g->frames_since_idr >= ctx->c.gop_size)
		return true;
	if (ctx->c.force_keyframe)
		return true;
	if (!ctx->last_good_valid)
		return true;
	return false;
}

void rkvenc_gop_commit(struct rkvenc_ctx *ctx)
{
	struct rkvenc_gop *g = &ctx->gop;

	if (ctx->cur_intra) {
		g->idr_pic_id++;
		ctx->last_good = ctx->cur;
		ctx->last_good_valid = true;
		g->frames_since_idr = 1;
	} else {
		g->frames_since_idr++;
	}
	ctx->last = ctx->cur;
	ctx->cur ^= 1;
	ctx->c.force_keyframe = false;
}

void rkvenc_gop_rollback(struct rkvenc_ctx *ctx)
{
	ctx->c.force_keyframe = false;
}

void rkvenc_gop_force_keyframe(struct rkvenc_ctx *ctx)
{
	ctx->c.force_keyframe = true;
}
