// SPDX-License-Identifier: GPL-2.0
/*
 * Crypto offloader support for Rockchip RK3568/RK3588
 *
 * Copyright (c) 2022-2023 Corentin Labbe <clabbe@baylibre.com>
 */
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/unaligned.h>
#include <crypto/aes.h>
#include <crypto/md5.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/sm3.h>
#include "rk2_crypto.h"

static bool rk2_ahash_need_fallback(struct ahash_request *areq)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(areq);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk2_crypto_template *algt =
	    container_of(alg, struct rk2_crypto_template, alg.hash.base);
	struct scatterlist *sg;
	int nents = sg_nents_for_len(areq->src, areq->nbytes);

	/*
	 * The hardware's Merkle-Damgard padding engine (HW_PAD) requires the
	 * total message length to be known upfront via CH0_PC_LEN_0. When a
	 * request spans multiple LLI descriptors, the hardware either treats
	 * each descriptor as an independent padded message, or loses running
	 * hash state at descriptor boundaries. Either way the result is a
	 * wrong digest. This behaviour is not documented in the RK3588 TRM,
	 * which advertises LLI chaining but does not specify whether hash
	 * operations may span multiple linked descriptors.
	 * Work around this by falling back to software for any multi-SG
	 * request. Single-SG requests with HW_PAD work correctly.
	 */

	if (nents < 0) {
		dev_err(algt->dev->dev, "Invalid SG list: length mismatch\n");
		return true;	/* force fallback safely */
	}
	if (nents > 1) {
		algt->stat_fb_sgdiff++;
		return true;
	}

	sg = areq->src;
	while (sg) {
		if (!IS_ALIGNED(sg->offset, sizeof(u32))) {
			algt->stat_fb_align++;
			return true;
		}
		if (sg->length % 4) {
			algt->stat_fb_sglen++;
			return true;
		}
		sg = sg_next(sg);
	}
	return false;
}

static int rk2_ahash_digest_fb(struct ahash_request *areq)
{
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(areq);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(areq);
	struct rk2_ahash_ctx *tfmctx = crypto_ahash_ctx(tfm);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk2_crypto_template *algt =
	    container_of(alg, struct rk2_crypto_template, alg.hash.base);

	algt->stat_fb++;

	ahash_request_set_tfm(&rctx->fallback_req, tfmctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req, areq->base.flags,
				   areq->base.complete, areq->base.data);

	rctx->fallback_req.nbytes = areq->nbytes;
	rctx->fallback_req.src = areq->src;
	rctx->fallback_req.result = areq->result;

	return crypto_ahash_digest(&rctx->fallback_req);
}

static int zero_message_process(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk2_crypto_template *algt =
	    container_of(alg, struct rk2_crypto_template, alg.hash.base);
	int digestsize = crypto_ahash_digestsize(tfm);

	switch (algt->rk2_mode) {
	case RK2_CRYPTO_SHA1:
		memcpy(req->result, sha1_zero_message_hash, digestsize);
		break;
	case RK2_CRYPTO_SHA256:
		memcpy(req->result, sha256_zero_message_hash, digestsize);
		break;
	case RK2_CRYPTO_SHA384:
		memcpy(req->result, sha384_zero_message_hash, digestsize);
		break;
	case RK2_CRYPTO_SHA512:
		memcpy(req->result, sha512_zero_message_hash, digestsize);
		break;
	case RK2_CRYPTO_MD5:
		memcpy(req->result, md5_zero_message_hash, digestsize);
		break;
	case RK2_CRYPTO_SM3:
		memcpy(req->result, sm3_zero_message_hash, digestsize);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * rk2_ahash_init() - Initialize context for a hash request
 * @req: The asynchronous hash request structure.
 *
 * Initializes the software fallback context. The physical hardware engine
 * is only utilized during atomic digest operations.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int rk2_ahash_init(struct ahash_request *req)
{
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk2_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req, req->base.flags,
				   req->base.complete, req->base.data);

	return crypto_ahash_init(&rctx->fallback_req);
}

/**
 * rk2_ahash_update() - Feed a message block into the hash stream
 * @req: The asynchronous hash request structure.
 *
 * Passes the message block to the software fallback. The hardware engine
 * does not support fragmented streaming updates.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int rk2_ahash_update(struct ahash_request *req)
{
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk2_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req, req->base.flags,
				   req->base.complete, req->base.data);
	rctx->fallback_req.nbytes = req->nbytes;
	rctx->fallback_req.src = req->src;

	return crypto_ahash_update(&rctx->fallback_req);
}

/**
 * rk2_ahash_final() - Finalize the hashing operation
 * @req: The asynchronous hash request structure.
 *
 * Finalizes the hash and extracts the digest via the software fallback.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int rk2_ahash_final(struct ahash_request *req)
{
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk2_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req, req->base.flags,
				   req->base.complete, req->base.data);
	rctx->fallback_req.result = req->result;

	return crypto_ahash_final(&rctx->fallback_req);
}

/**
 * rk2_ahash_finup() - Perform update and final hash operations sequentially
 * @req: The asynchronous hash request structure.
 *
 * Convenience wrapper that performs update and final operations
 * via the software fallback.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int rk2_ahash_finup(struct ahash_request *req)
{
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk2_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req, req->base.flags,
				   req->base.complete, req->base.data);

	rctx->fallback_req.nbytes = req->nbytes;
	rctx->fallback_req.src = req->src;
	rctx->fallback_req.result = req->result;

	return crypto_ahash_finup(&rctx->fallback_req);
}

/**
 * rk2_ahash_import() - Restore a saved hash context
 * @req: The target asynchronous hash request structure.
 * @in: Buffer containing the previously exported state.
 *
 * Restores the software fallback state from an export block.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int rk2_ahash_import(struct ahash_request *req, const void *in)
{
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk2_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req, req->base.flags,
				   req->base.complete, req->base.data);

	return crypto_ahash_import(&rctx->fallback_req, in);
}

/**
 * rk2_ahash_export() - Serialize an active hash context
 * @req: The source asynchronous hash request structure.
 * @out: Destination buffer where the state will be written.
 *
 * Freezes the progression of the software fallback stream into a byte array.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int rk2_ahash_export(struct ahash_request *req, void *out)
{
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rk2_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->fallback_req, ctx->fallback_tfm);
	ahash_request_set_callback(&rctx->fallback_req, req->base.flags,
				   req->base.complete, req->base.data);

	return crypto_ahash_export(&rctx->fallback_req, out);
}

/**
 * rk2_ahash_digest() - Compute a complete message digest in a single transaction
 * @req: The asynchronous hash request structure.
 *
 * Evaluates hardware constraints (e.g., scatterlist alignment) and either
 * routes the atomic request to the hardware engine or diverts to the fallback.
 *
 * Return: 0 on synchronous completion, -EINPROGRESS if submitted to the
 *         hardware engine, or a negative error code on failure.
 */
int rk2_ahash_digest(struct ahash_request *req)
{
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk2_crypto_template *algt;
	struct rk2_crypto_dev *rkc;
	struct crypto_engine *engine;

	if (!req->nbytes)
		return zero_message_process(req);

	if (rk2_ahash_need_fallback(req))
		return rk2_ahash_digest_fb(req);

	/* Extract the device pointer from the algorithm template! */
	algt = container_of(alg, struct rk2_crypto_template, alg.hash.base);
	rkc = algt->dev;
	if (!rkc)
		return -ENODEV;

	rctx->dev = rkc;
	engine = rkc->engine;

	return crypto_transfer_hash_request_to_engine(engine, req);
}

static int rk2_hash_prepare(struct crypto_engine *engine, void *breq)
{
	struct ahash_request *areq =
	    container_of(breq, struct ahash_request, base);
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(areq);
	struct rk2_crypto_dev *rkc = rctx->dev;
	int n = sg_nents_for_len(areq->src, areq->nbytes);
	int ret;

	if (n < 0) {
		dev_err(rkc->dev, "SG list too short for %u bytes\n", areq->nbytes);
		return -EINVAL;
	}
	rctx->nrsgs = n;
	ret = dma_map_sg(rkc->dev, areq->src, rctx->nrsgs, DMA_TO_DEVICE);
	if (ret <= 0) {
		/*
		 * clear nrsgs on map failure to prevent spurious unmap in unprepare
		 */
		rctx->nrsgs = 0;
		return -EINVAL;
	}
	return 0;
}

static void rk2_hash_unprepare(struct crypto_engine *engine, void *breq)
{
	struct ahash_request *areq =
	    container_of(breq, struct ahash_request, base);
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(areq);
	struct rk2_crypto_dev *rkc = rctx->dev;

	if (rctx->nrsgs)
		dma_unmap_sg(rkc->dev, areq->src, rctx->nrsgs, DMA_TO_DEVICE);
}

/**
 * rk2_hash_run() - Execute an asynchronous hash request via the hardware
 * @engine: The crypto engine queue managing this request.
 * @breq: The asynchronous hash request to process.
 *
 * Configures the hardware hash engine, programs DMA block descriptors,
 * and copies the final digest back from the hardware registers.
 *
 * Return: Always 0. Errors are reported through the crypto engine
 *         finalization callback.
 */
int rk2_hash_run(struct crypto_engine *engine, void *breq)
{
	struct ahash_request *areq =
	    container_of(breq, struct ahash_request, base);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(areq);
	struct rk2_ahash_rctx *rctx = ahash_request_ctx(areq);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk2_crypto_template *algt =
	    container_of(alg, struct rk2_crypto_template, alg.hash.base);
	struct scatterlist *sgs = areq->src;
	struct rk2_crypto_dev *rkc = rctx->dev;
	struct rk2_crypto_lli *dd = &rkc->tl[0];
	int ddi = 0;
	int err = 0;
	unsigned int len = areq->nbytes;
	unsigned int todo;
	unsigned long timeout;
	u32 v;
	int i;

	err = rk2_hash_prepare(engine, breq);
	if (err)
		goto exit_unmap;

	err = pm_runtime_resume_and_get(rkc->dev);
	if (err)
		goto exit_unmap;

	dev_dbg(rkc->dev, "%s %s len=%u\n", __func__,
		crypto_tfm_alg_name(areq->base.tfm), areq->nbytes);

	algt->stat_req++;
	rkc->nreq++;

	/* the upper bits are a write enable mask, so we need to write 1 to all
	 * upper 16 bits to allow write to the 16 lower bits
	 */
	rctx->mode = algt->rk2_mode;
	rctx->mode |= 0xffff0000;
	rctx->mode |= RK2_CRYPTO_ENABLE | RK2_CRYPTO_HW_PAD;
	writel(rctx->mode, rkc->reg + RK2_CRYPTO_HASH_CTL);

	while (sgs && len > 0) {
		if (ddi >= MAX_LLI) {
			dev_err(rkc->dev,
				"Too many SG entries (current: %d, max: %d)\n",
				ddi, MAX_LLI);
			err = -EINVAL;
			goto exit;
		}
		dd = &rkc->tl[ddi];

		todo = min(sg_dma_len(sgs), len);
		dd->src_addr = sg_dma_address(sgs);
		dd->src_len = todo;
		dd->dst_addr = 0;
		dd->dst_len = 0;
		dd->dma_ctrl = ddi << 24;
		dd->iv = 0;
		dd->next =
		    rkc->t_phy + sizeof(struct rk2_crypto_lli) * (ddi + 1);

		if (ddi == 0)
			dd->user = RK2_LLI_CIPHER_START | RK2_LLI_STRING_FIRST;
		else
			dd->user = 0;

		len -= todo;
		if (len == 0) {
			dd->user |= RK2_LLI_STRING_LAST;
			dd->dma_ctrl |= RK2_LLI_DMA_CTRL_LAST |
			    RK2_LLI_DMA_CTRL_SRC_INT |
			    RK2_LLI_DMA_CTRL_LIST_INT;
		}
		dev_dbg(rkc->dev,
			"HASH SG %d sglen=%u user=%x dma=%x mode=%x len=%u todo=%u phy=%pad\n",
			ddi, sgs->length, dd->user, dd->dma_ctrl, rctx->mode,
			len, todo, &rkc->t_phy);

		sgs = sg_next(sgs);
		ddi++;
	}

	/*
	 * next is ignored by hardware when RK2_LLI_DMA_CTRL_LAST is set in
	 * dma_ctrl. Set it to an obviously-invalid-but-non-zero sentinel so
	 * it stands out if ever read in a debug dump.
	 */
	dd->next = 1;

	/* Program total payload length for hardware padding */
	writel(areq->nbytes, rkc->reg + RK2_CRYPTO_CH0_PC_LEN_0);

	/* Clear stale interrupts, then enable with proper write-mask */
	writel(RK2_CRYPTO_DMA_INT_ALL_MASK, rkc->reg + RK2_CRYPTO_DMA_INT_ST);
	writel(RK2_CRYPTO_DMA_INT_ENABLE_ALL, rkc->reg + RK2_CRYPTO_DMA_INT_EN);

	writel(rkc->t_phy, rkc->reg + RK2_CRYPTO_DMA_LLI_ADDR);

	reinit_completion(&rkc->complete);
	rkc->status = 0;

	writel(RK2_CRYPTO_DMA_CTL_START | (RK2_CRYPTO_DMA_CTL_START << 16),
	       rkc->reg + RK2_CRYPTO_DMA_CTL);

	timeout = wait_for_completion_timeout(&rkc->complete,
					      msecs_to_jiffies(2000));
	if (!timeout) {
		dev_err(rkc->dev, "DMA timeout\n");
		err = -ETIMEDOUT;
		reset_control_assert(rkc->rst);
		udelay(10);
		reset_control_deassert(rkc->rst);
		goto exit;
	}
	if (!rkc->status) {
		dev_err(rkc->dev, "DMA error\n");
		err = -EIO;
		reset_control_assert(rkc->rst);
		udelay(10);
		reset_control_deassert(rkc->rst);
		goto exit;
	}

	err =
	    readl_poll_timeout_atomic(rkc->reg + RK2_CRYPTO_HASH_VALID, v,
				      v == 1, 10, 1000);
	if (err) {
		dev_err(rkc->dev, "Hash result not valid\n");
		goto exit;
	}

	/*
	 * Hardware outputs digest words in big-endian format.
	 * Because readl() performs a native little-endian read,
	 * put_unaligned_be32() is used to store the result correctly
	 * into the byte array.
	 */
	for (i = 0; i < crypto_ahash_digestsize(tfm) / 4; i++) {
		v = readl(rkc->reg + RK2_CRYPTO_HASH_DOUT_0 + i * 4);
		put_unaligned_be32(v, areq->result + i * 4);
	}
 exit:
	writel(0xffff0000, rkc->reg + RK2_CRYPTO_HASH_CTL);
	pm_runtime_mark_last_busy(rkc->dev);
	pm_runtime_put_autosuspend(rkc->dev);

 exit_unmap:
	rk2_hash_unprepare(engine, breq);
	local_bh_disable();
	crypto_finalize_hash_request(engine, breq, err);
	local_bh_enable();

	return 0;
}

/**
 * rk2_hash_init_tfm() - Initialize the transformation context
 * @tfm: The crypto ahash handle.
 *
 * Allocates software fallback transformations required to guarantee
 * processing integrity when hardware constraints are violated.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int rk2_hash_init_tfm(struct crypto_ahash *tfm)
{
	struct rk2_ahash_ctx *tctx = crypto_ahash_ctx(tfm);
	const char *alg_name = crypto_ahash_alg_name(tfm);
	struct ahash_alg *alg = crypto_ahash_alg(tfm);
	struct rk2_crypto_template *algt =
	    container_of(alg, struct rk2_crypto_template, alg.hash.base);
	unsigned int fallback_statesize;

	tctx->fallback_tfm = crypto_alloc_ahash(alg_name, 0,
						CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(tctx->fallback_tfm)) {
		dev_err(algt->dev->dev, "Could not load fallback driver.\n");
		return PTR_ERR(tctx->fallback_tfm);
	}

	/* Promote statesize if fallback needs more space for export/import */
	fallback_statesize = crypto_ahash_statesize(tctx->fallback_tfm);
	if (fallback_statesize > crypto_ahash_statesize(tfm))
		crypto_ahash_set_statesize(tfm, fallback_statesize);

	crypto_ahash_set_reqsize(tfm,
				 sizeof(struct rk2_ahash_rctx) +
				 crypto_ahash_reqsize(tctx->fallback_tfm));
	return 0;
}

/**
 * rk2_hash_exit_tfm() - Clean up an ahash transformation context
 * @tfm: The crypto ahash handle.
 *
 * Safely frees internal software fallback transformations.
 */
void rk2_hash_exit_tfm(struct crypto_ahash *tfm)
{
	struct rk2_ahash_ctx *tctx = crypto_ahash_ctx(tfm);

	crypto_free_ahash(tctx->fallback_tfm);
}
