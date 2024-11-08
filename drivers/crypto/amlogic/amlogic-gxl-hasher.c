// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware asynchronous hasher for Amlogic SoC's.
 *
 * Copyright (c) 2023, SaluteDevices. All Rights Reserved.
 *
 * Author: Alexey Romanov <avromanov@salutedevices.com>
 */

#include <linux/crypto.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <crypto/internal/hash.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>

#include "amlogic-gxl.h"

static int meson_sha_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);

	memset(rctx, 0, sizeof(struct meson_hasher_req_ctx));

	rctx->flow = meson_get_engine_number(tctx->mc);
	rctx->begin_req = true;

	return 0;
}

static int meson_sha_update(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);
	struct crypto_engine *engine = tctx->mc->chanlist[rctx->flow].engine;

	return crypto_transfer_hash_request_to_engine(engine, req);
}

static int meson_sha_final(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);
	struct crypto_engine *engine = tctx->mc->chanlist[rctx->flow].engine;

	rctx->final_req = true;

	return crypto_transfer_hash_request_to_engine(engine, req);
}

static int meson_sha_digest(struct ahash_request *req)
{
	struct crypto_wait wait;
	int ret;

	crypto_init_wait(&wait);
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP |
					CRYPTO_TFM_REQ_MAY_BACKLOG,
					crypto_req_done, &wait);

	meson_sha_init(req);

	ret = crypto_wait_req(meson_sha_update(req), &wait);
	if (ret)
		return ret;

	return crypto_wait_req(meson_sha_final(req), &wait);
}

static int meson_hasher_req_map(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_dev *mc = tctx->mc;
	int ret;

	if (!req->nbytes)
		return 0;

	ret = dma_map_sg(mc->dev, req->src, sg_nents(req->src), DMA_TO_DEVICE);
	if (!ret) {
		dev_err(mc->dev, "Cannot DMA MAP request data\n");
		return -ENOMEM;
	}

	return 0;
}

static void meson_hasher_req_unmap(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_dev *mc = tctx->mc;

	if (!req->nbytes)
		return;

	dma_unmap_sg(mc->dev, req->src, sg_nents(req->src), DMA_TO_DEVICE);
}

struct hasher_ctx {
	struct crypto_async_request *areq;

	unsigned int tloffset;
	unsigned int nbytes;
	unsigned int todo;

	dma_addr_t state_addr;
	dma_addr_t src_addr;
	unsigned int src_offset;
	struct scatterlist *src_sg;
};

static bool meson_final(struct hasher_ctx *ctx)
{
	struct ahash_request *req = ahash_request_cast(ctx->areq);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);

	return !ctx->nbytes && rctx->final_req;
}

static int meson_fill_partial_buffer(struct hasher_ctx *ctx, unsigned int len)
{
	struct ahash_request *req = ahash_request_cast(ctx->areq);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);
	struct meson_dev *mc = tctx->mc;
	unsigned int blocksize = crypto_ahash_blocksize(tfm);
	unsigned int copy;

	if (len) {
		copy = min(blocksize - rctx->partial_size, len);
		memcpy(rctx->partial + rctx->partial_size,
		       sg_virt(ctx->src_sg) + ctx->src_offset, copy);

		rctx->partial_size += copy;
		ctx->nbytes -= copy;
		ctx->src_offset += copy;
	}

	if (rctx->partial_size == blocksize || meson_final(ctx)) {
		rctx->partial_addr = dma_map_single(mc->dev,
						    rctx->partial,
						    rctx->partial_size,
						    DMA_TO_DEVICE);
		if (dma_mapping_error(mc->dev, rctx->partial_addr)) {
			dev_err(mc->dev, "Cannot DMA MAP SHA partial buffer\n");
			return -ENOMEM;
		}

		rctx->partial_mapped = true;
		ctx->todo = rctx->partial_size;
		ctx->src_addr = rctx->partial_addr;
	}

	return 0;
}

static unsigned int meson_setup_data_descs(struct hasher_ctx *ctx)
{
	struct ahash_request *req = ahash_request_cast(ctx->areq);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_dev *mc = tctx->mc;
	struct meson_flow *flow = &mc->chanlist[rctx->flow];
	struct hash_alg_common *alg = crypto_hash_alg_common(tfm);
	struct meson_alg_template *algt = container_of(alg,
		struct meson_alg_template, alg.ahash.base.halg);
	struct meson_desc *desc = &flow->tl[ctx->tloffset];
	u32 v;

	ctx->tloffset++;

	v = DESC_OWN | DESC_ENCRYPTION | DESC_OPMODE_SHA |
	    ctx->todo | algt->blockmode;
	if (rctx->begin_req) {
		rctx->begin_req = false;
		v |= DESC_BEGIN;
	}

	if (!ctx->nbytes && rctx->final_req) {
		rctx->final_req = false;
		v |= DESC_END;
	}

	if (!ctx->nbytes || ctx->tloffset == MAXDESC || rctx->partial_mapped)
		v |= DESC_LAST;

	desc->t_src = cpu_to_le32(ctx->src_addr);
	desc->t_dst = cpu_to_le32(ctx->state_addr);
	desc->t_status = cpu_to_le32(v);

	return v & DESC_LAST;
}

static int meson_kick_hardware(struct hasher_ctx *ctx)
{
	struct ahash_request *req = ahash_request_cast(ctx->areq);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_dev *mc = tctx->mc;
	struct meson_flow *flow = &mc->chanlist[rctx->flow];
	int ret;

	reinit_completion(&flow->complete);
	meson_dma_start(mc, rctx->flow);

	ret = wait_for_completion_timeout(&flow->complete,
					  msecs_to_jiffies(500));
	if (ret == 0) {
		dev_err(mc->dev, "DMA timeout for flow %d\n", rctx->flow);
		return -EINVAL;
	} else if (ret < 0) {
		dev_err(mc->dev, "Waiting for DMA completion is failed (%d)\n", ret);
		return ret;
	}

	if (rctx->partial_mapped) {
		dma_unmap_single(mc->dev, rctx->partial_addr,
				 rctx->partial_size,
				 DMA_TO_DEVICE);
		rctx->partial_size = 0;
		rctx->partial_mapped = false;
	}

	ctx->tloffset = 0;

	return 0;
}

static void meson_setup_state_descs(struct hasher_ctx *ctx)
{
	struct ahash_request *req = ahash_request_cast(ctx->areq);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_dev *mc = tctx->mc;
	struct meson_desc *desc;
	int i;

	if (ctx->tloffset || rctx->begin_req)
		return;

	for (i = 0; i < mc->pdata->setup_desc_cnt; i++) {
		int offset = i * 16;

		desc = &mc->chanlist[rctx->flow].tl[ctx->tloffset];
		desc->t_src = cpu_to_le32(ctx->state_addr + offset);
		desc->t_dst = cpu_to_le32(offset);
		desc->t_status = cpu_to_le32(MESON_SHA_BUFFER_SIZE |
					     DESC_MODE_KEY | DESC_OWN);

		ctx->tloffset++;
	}
}

static int meson_hasher_do_one_request(struct crypto_engine *engine, void *areq)
{
	struct ahash_request *req = ahash_request_cast(areq);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct meson_hasher_tfm_ctx *tctx = crypto_ahash_ctx_dma(tfm);
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);
	struct meson_dev *mc = tctx->mc;
	struct hasher_ctx ctx = {
		.tloffset = 0,
		.src_offset = 0,
		.nbytes = rctx->final_req ? 0 : req->nbytes,
		.src_sg = req->src,
		.areq = areq,
	};
	unsigned int blocksize = crypto_ahash_blocksize(tfm);
	unsigned int digest_size = crypto_ahash_digestsize(tfm);
	bool final_req = rctx->final_req;
	int ret;

	ctx.state_addr = dma_map_single(mc->dev, rctx->state,
					sizeof(rctx->state), DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(mc->dev, ctx.state_addr);
	if (ret) {
		dev_err(mc->dev, "Cannot DMA MAP SHA state buffer");
		goto fail_map_single;
	}

	ret = meson_hasher_req_map(req);
	if (ret)
		goto fail_map_req;

	for (;;) {
		unsigned int len = ctx.src_sg ?
			min(sg_dma_len(ctx.src_sg) - ctx.src_offset, ctx.nbytes) : 0;

		ctx.src_addr = 0;
		ctx.todo = 0;

		if (!rctx->final_req && !ctx.nbytes)
			break;

		meson_setup_state_descs(&ctx);

		if (rctx->partial_size && rctx->partial_size < blocksize) {
			ret = meson_fill_partial_buffer(&ctx, len);
			if (ret)
				goto fail;
		} else if (len && len < blocksize) {
			memcpy(rctx->partial, sg_virt(ctx.src_sg) + ctx.src_offset, len);

			rctx->partial_size = len;
			ctx.nbytes -= len;
			ctx.src_offset += len;
		} else if (len) {
			ctx.src_addr = sg_dma_address(ctx.src_sg) + ctx.src_offset;
			ctx.todo = min(rounddown(DESC_MAXLEN, blocksize),
				       rounddown(len, blocksize));
			ctx.nbytes -= ctx.todo;
			ctx.src_offset += ctx.todo;
		}

		if (ctx.src_sg && ctx.src_offset == sg_dma_len(ctx.src_sg)) {
			ctx.src_offset = 0;
			ctx.src_sg = sg_next(ctx.src_sg);
		}

		if (!ctx.todo && ctx.nbytes)
			continue;

		if (!ctx.todo && !rctx->final_req && !ctx.tloffset)
			continue;

		if (meson_setup_data_descs(&ctx)) {
			ret = meson_kick_hardware(&ctx);
			if (ret)
				goto fail;
		}
	}

fail:
	meson_hasher_req_unmap(req);

fail_map_req:
	dma_unmap_single(mc->dev, ctx.state_addr, sizeof(rctx->state),
			 DMA_BIDIRECTIONAL);

fail_map_single:
	if (final_req && ret == 0)
		memcpy(req->result, rctx->state, digest_size);

	local_bh_disable();
	crypto_finalize_hash_request(engine, req, ret);
	local_bh_enable();

	return ret;
}

static int meson_hasher_export(struct ahash_request *req, void *out)
{
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);

	memcpy(out, rctx, sizeof(*rctx));
	return 0;
}

static int meson_hasher_import(struct ahash_request *req, const void *in)
{
	struct meson_hasher_req_ctx *rctx = ahash_request_ctx(req);

	memcpy(rctx, in, sizeof(*rctx));
	return 0;
}

static int meson_hasher_init(struct crypto_tfm *tfm)
{
	struct meson_hasher_tfm_ctx *tctx = crypto_tfm_ctx_dma(tfm);
	struct crypto_ahash *atfm = __crypto_ahash_cast(tfm);
	struct hash_alg_common *alg = crypto_hash_alg_common(atfm);
	struct meson_alg_template *algt = container_of(alg,
		struct meson_alg_template, alg.ahash.base.halg);

	crypto_ahash_set_reqsize(atfm, crypto_ahash_statesize(atfm));

	memset(tctx, 0, sizeof(struct meson_hasher_tfm_ctx));

	tctx->mc = algt->mc;

	return 0;
}

static struct meson_alg_template mc_algs[] = {
{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.blockmode = DESC_MODE_SHA1,
	.alg.ahash.base = {
		.halg = {
			.base = {
				.cra_name = "sha1",
				.cra_driver_name = "sha1-gxl",
				.cra_priority = 400,
				.cra_blocksize = SHA1_BLOCK_SIZE,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_ctxsize = sizeof(struct meson_hasher_tfm_ctx) +
					       CRYPTO_DMA_PADDING,
				.cra_module = THIS_MODULE,
				.cra_alignmask = 0,
				.cra_init = meson_hasher_init,
			},
			.digestsize = SHA1_DIGEST_SIZE,
			.statesize = sizeof(struct meson_hasher_req_ctx),
		},
		.init = meson_sha_init,
		.update = meson_sha_update,
		.final = meson_sha_final,
		.digest = meson_sha_digest,
		.export = meson_hasher_export,
		.import = meson_hasher_import,
	},
	.alg.ahash.op = {
		.do_one_request = meson_hasher_do_one_request,
	},
},
{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.blockmode = DESC_MODE_SHA224,
	.alg.ahash.base = {
		.halg = {
			.base = {
				.cra_name = "sha224",
				.cra_driver_name = "sha224-gxl",
				.cra_priority = 400,
				.cra_blocksize = SHA224_BLOCK_SIZE,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_ctxsize = sizeof(struct meson_hasher_tfm_ctx) +
					       CRYPTO_DMA_PADDING,
				.cra_module = THIS_MODULE,
				.cra_alignmask = 0,
				.cra_init = meson_hasher_init,
			},
			.digestsize = SHA224_DIGEST_SIZE,
			.statesize = sizeof(struct meson_hasher_req_ctx),
		},
		.init = meson_sha_init,
		.update = meson_sha_update,
		.final = meson_sha_final,
		.digest = meson_sha_digest,
		.export = meson_hasher_export,
		.import = meson_hasher_import,
	},
	.alg.ahash.op = {
		.do_one_request = meson_hasher_do_one_request,
	},
},
{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.blockmode = DESC_MODE_SHA256,
	.alg.ahash.base = {
		.halg = {
			.base = {
				.cra_name = "sha256",
				.cra_driver_name = "sha256-gxl",
				.cra_priority = 400,
				.cra_blocksize = SHA256_BLOCK_SIZE,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_ctxsize = sizeof(struct meson_hasher_tfm_ctx) +
					       CRYPTO_DMA_PADDING,
				.cra_module = THIS_MODULE,
				.cra_alignmask = 0,
				.cra_init = meson_hasher_init,
			},
			.digestsize = SHA256_DIGEST_SIZE,
			.statesize = sizeof(struct meson_hasher_req_ctx),
		},
		.init = meson_sha_init,
		.update = meson_sha_update,
		.final = meson_sha_final,
		.digest = meson_sha_digest,
		.export = meson_hasher_export,
		.import = meson_hasher_import,
	},
	.alg.ahash.op = {
		.do_one_request = meson_hasher_do_one_request,
	},
},
};

int meson_hasher_register(struct meson_dev *mc)
{
	if (!mc->pdata->hasher_supported) {
		pr_info("amlogic-gxl-hasher: hasher not supported at current platform");
		return 0;
	}

	return meson_register_algs(mc, mc_algs, ARRAY_SIZE(mc_algs));
}

void meson_hasher_unregister(struct meson_dev *mc)
{
	if (!mc->pdata->hasher_supported)
		return;

	meson_unregister_algs(mc, mc_algs, ARRAY_SIZE(mc_algs));
}
