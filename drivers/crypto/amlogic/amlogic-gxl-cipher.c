// SPDX-License-Identifier: GPL-2.0
/*
 * amlogic-cipher.c - hardware cryptographic offloader for Amlogic GXL SoC
 *
 * Copyright (C) 2018-2019 Corentin LABBE <clabbe@baylibre.com>
 *
 * This file add support for AES cipher with 128,192,256 bits keysize in
 * CBC and ECB mode.
 */

#include <linux/crypto.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <crypto/scatterwalk.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <crypto/internal/skcipher.h>
#include "amlogic-gxl.h"

static bool meson_cipher_need_fallback_sg(struct skcipher_request *areq,
					  struct scatterlist *sg)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	unsigned int blocksize = crypto_skcipher_blocksize(tfm);
	unsigned int cryptlen = areq->cryptlen;

	while (cryptlen) {
		unsigned int len = min(cryptlen, sg->length);

		if (!IS_ALIGNED(sg->offset, sizeof(u32)))
			return true;
		if (len % blocksize != 0)
			return true;

		cryptlen -= len;
		sg = sg_next(sg);
	}

	return false;
}

static bool meson_cipher_need_fallback(struct skcipher_request *areq)
{
	if (areq->cryptlen == 0)
		return true;

	if (meson_cipher_need_fallback_sg(areq, areq->src))
		return true;

	if (areq->dst == areq->src)
		return false;

	if (meson_cipher_need_fallback_sg(areq, areq->dst))
		return true;

	return false;
}

static int meson_cipher_do_fallback(struct skcipher_request *areq)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(areq);
	int err;
#ifdef CONFIG_CRYPTO_DEV_AMLOGIC_GXL_DEBUG
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct meson_alg_template *algt;

	algt = container_of(alg, struct meson_alg_template, alg.skcipher.base);
	algt->stat_fb++;
#endif
	skcipher_request_set_tfm(&rctx->fallback_req, op->fallback_tfm);
	skcipher_request_set_callback(&rctx->fallback_req, areq->base.flags,
				      areq->base.complete, areq->base.data);
	skcipher_request_set_crypt(&rctx->fallback_req, areq->src, areq->dst,
				   areq->cryptlen, areq->iv);

	if (rctx->op_dir == MESON_DECRYPT)
		err = crypto_skcipher_decrypt(&rctx->fallback_req);
	else
		err = crypto_skcipher_encrypt(&rctx->fallback_req);
	return err;
}

struct cipher_ctx {
	struct {
		dma_addr_t addr;
		unsigned int len;
	} keyiv;

	struct skcipher_request *areq;
	struct scatterlist *src_sg;
	struct scatterlist *dst_sg;
	void *bkeyiv;

	unsigned int src_offset;
	unsigned int dst_offset;
	unsigned int cryptlen;
	unsigned int tloffset;
};

static int meson_map_scatterlist(struct skcipher_request *areq, struct meson_dev *mc)
{
	int nr_sgs, nr_sgd;

	if (areq->src == areq->dst) {
		nr_sgs = dma_map_sg(mc->dev, areq->src, sg_nents(areq->src),
				    DMA_BIDIRECTIONAL);
		if (!nr_sgs) {
			dev_err(mc->dev, "Invalid SG count %d\n", nr_sgs);
			return -EINVAL;
		}
	} else {
		nr_sgs = dma_map_sg(mc->dev, areq->src, sg_nents(areq->src),
				    DMA_TO_DEVICE);
		if (!nr_sgs) {
			dev_err(mc->dev, "Invalid SG count %d\n", nr_sgs);
			return -EINVAL;
		}

		nr_sgd = dma_map_sg(mc->dev, areq->dst, sg_nents(areq->dst),
				    DMA_FROM_DEVICE);
		if (!nr_sgd) {
			dma_unmap_sg(mc->dev, areq->src, sg_nents(areq->src), DMA_TO_DEVICE);
			dev_err(mc->dev, "Invalid SG count %d\n", nr_sgd);
			return -EINVAL;
		}
	}

	return 0;
}

static void meson_unmap_scatterlist(struct skcipher_request *areq, struct meson_dev *mc)
{
	if (areq->src == areq->dst) {
		dma_unmap_sg(mc->dev, areq->src, sg_nents(areq->src), DMA_BIDIRECTIONAL);
	} else {
		dma_unmap_sg(mc->dev, areq->src, sg_nents(areq->src), DMA_TO_DEVICE);
		dma_unmap_sg(mc->dev, areq->dst, sg_nents(areq->dst), DMA_FROM_DEVICE);
	}
}

static void meson_setup_keyiv_descs(struct cipher_ctx *ctx)
{
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(ctx->areq);
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(ctx->areq);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct meson_alg_template *algt = container_of(alg,
		struct meson_alg_template, alg.skcipher.base);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_dev *mc = op->mc;
	unsigned int ivsize = crypto_skcipher_ivsize(tfm);
	unsigned int blockmode = algt->blockmode;
	int i;

	if (ctx->tloffset)
		return;

	if (blockmode == DESC_OPMODE_CBC) {
		memcpy(ctx->bkeyiv + AES_MAX_KEY_SIZE, ctx->areq->iv, ivsize);
		dma_sync_single_for_device(mc->dev, ctx->keyiv.addr,
					   ctx->keyiv.len, DMA_TO_DEVICE);
	}

	for (i = 0; i < mc->pdata->setup_desc_cnt; i++) {
		struct meson_desc *desc =
			&mc->chanlist[rctx->flow].tl[ctx->tloffset];
		int offset = i * 16;

		desc->t_src = cpu_to_le32(ctx->keyiv.addr + offset);
		desc->t_dst = cpu_to_le32(offset);
		desc->t_status = cpu_to_le32(DESC_OWN | DESC_MODE_KEY | ctx->keyiv.len);

		ctx->tloffset++;
	}
}

static bool meson_setup_data_descs(struct cipher_ctx *ctx)
{
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(ctx->areq);
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(ctx->areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct meson_alg_template *algt = container_of(alg,
						       struct meson_alg_template,
						       alg.skcipher.base);
	struct meson_dev *mc = op->mc;
	struct meson_desc *desc = &mc->chanlist[rctx->flow].tl[ctx->tloffset];
	unsigned int blocksize = crypto_skcipher_blocksize(tfm);
	unsigned int blockmode = algt->blockmode;
	unsigned int maxlen = rounddown(DESC_MAXLEN, blocksize);
	unsigned int todo;
	u32 v;

	ctx->tloffset++;

	todo = min(ctx->cryptlen, maxlen);
	todo = min(todo, ctx->cryptlen);
	todo = min(todo, sg_dma_len(ctx->src_sg) - ctx->src_offset);
	todo = min(todo, sg_dma_len(ctx->dst_sg) - ctx->dst_offset);

	desc->t_src = cpu_to_le32(sg_dma_address(ctx->src_sg) + ctx->src_offset);
	desc->t_dst = cpu_to_le32(sg_dma_address(ctx->dst_sg) + ctx->dst_offset);

	ctx->cryptlen -= todo;
	ctx->src_offset += todo;
	ctx->dst_offset += todo;

	v = DESC_OWN | blockmode | op->keymode | todo;
	if (rctx->op_dir == MESON_ENCRYPT)
		v |= DESC_ENCRYPTION;

	if (!ctx->cryptlen || ctx->tloffset == MAXDESC)
		v |= DESC_LAST;

	desc->t_status = cpu_to_le32(v);

	return v & DESC_LAST;
}

static int meson_kick_hardware(struct cipher_ctx *ctx)
{
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(ctx->areq);
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(ctx->areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct meson_alg_template *algt = container_of(alg,
						       struct meson_alg_template,
						       alg.skcipher.base);
	struct meson_dev *mc = op->mc;
	unsigned int ivsize = crypto_skcipher_ivsize(tfm);
	unsigned int blockmode = algt->blockmode;
	enum dma_data_direction new_iv_dir;
	struct scatterlist *sg_head;
	dma_addr_t new_iv_phys;
	void *new_iv;
	int err;

	if (blockmode == DESC_OPMODE_CBC) {
		struct scatterlist *sg_current;
		unsigned int offset;

		if (rctx->op_dir == MESON_ENCRYPT) {
			sg_current = ctx->dst_sg;
			sg_head = ctx->areq->dst;
			offset = ctx->dst_offset;
			new_iv_dir = DMA_FROM_DEVICE;
		} else {
			sg_current = ctx->src_sg;
			sg_head = ctx->areq->src;
			offset = ctx->src_offset;
			new_iv_dir = DMA_TO_DEVICE;
		}

		if (ctx->areq->src == ctx->areq->dst)
			new_iv_dir = DMA_BIDIRECTIONAL;

		offset -= ivsize;
		new_iv = sg_virt(sg_current) + offset;
		new_iv_phys = sg_dma_address(sg_current) + offset;
	}

	if (blockmode == DESC_OPMODE_CBC &&
	    rctx->op_dir == MESON_DECRYPT) {
		dma_sync_sg_for_cpu(mc->dev, sg_head,
				    sg_nents(sg_head), new_iv_dir);
		memcpy(ctx->areq->iv, new_iv, ivsize);
	}

	reinit_completion(&mc->chanlist[rctx->flow].complete);
	meson_dma_start(mc, rctx->flow);
	err = wait_for_completion_interruptible_timeout(&mc->chanlist[rctx->flow].complete,
							msecs_to_jiffies(500));
	if (err == 0) {
		dev_err(mc->dev, "DMA timeout for flow %d\n", rctx->flow);
		return -EINVAL;
	} else if (err < 0) {
		dev_err(mc->dev, "Waiting for DMA completion is failed (%d)\n", err);
		return err;
	}

	if (blockmode == DESC_OPMODE_CBC &&
	    rctx->op_dir == MESON_ENCRYPT) {
		dma_sync_sg_for_cpu(mc->dev, sg_head,
				    sg_nents(sg_head), new_iv_dir);
		memcpy(ctx->areq->iv, new_iv, ivsize);
	}

	ctx->tloffset = 0;

	return 0;
}

static int meson_cipher(struct skcipher_request *areq)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(areq);
	struct meson_dev *mc = op->mc;
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct meson_alg_template *algt;
	struct cipher_ctx ctx = {
		.areq = areq,
		.src_offset = 0,
		.dst_offset = 0,
		.src_sg = areq->src,
		.dst_sg = areq->dst,
		.cryptlen = areq->cryptlen,
	};
	int err;

	dev_dbg(mc->dev, "%s %s %u %x IV(%u) key=%u ctx.flow=%d\n", __func__,
		crypto_tfm_alg_name(areq->base.tfm),
		areq->cryptlen,
		rctx->op_dir, crypto_skcipher_ivsize(tfm),
		op->keylen, rctx->flow);

	algt = container_of(alg, struct meson_alg_template, alg.skcipher.base);

#ifdef CONFIG_CRYPTO_DEV_AMLOGIC_GXL_DEBUG
	algt->stat_req++;
	mc->chanlist[rctx->flow].stat_req++;
#endif

	ctx.bkeyiv = kzalloc(48, GFP_KERNEL | GFP_DMA);
	if (!ctx.bkeyiv)
		return -ENOMEM;

	memcpy(ctx.bkeyiv, op->key, op->keylen);
	ctx.keyiv.len = op->keylen;
	if (ctx.keyiv.len == AES_KEYSIZE_192)
		ctx.keyiv.len = AES_MAX_KEY_SIZE;

	ctx.keyiv.addr = dma_map_single(mc->dev, ctx.bkeyiv, ctx.keyiv.len,
					DMA_TO_DEVICE);
	err = dma_mapping_error(mc->dev, ctx.keyiv.addr);
	if (err) {
		dev_err(mc->dev, "Cannot DMA MAP KEY IV\n");
		goto free_keyiv;
	}

	err = meson_map_scatterlist(areq, mc);
	if (err)
		goto unmap_keyiv;

	ctx.tloffset = 0;

	while (ctx.cryptlen) {
		meson_setup_keyiv_descs(&ctx);

		if (meson_setup_data_descs(&ctx)) {
			err = meson_kick_hardware(&ctx);
			if (err)
				break;
		}

		if (ctx.src_offset == sg_dma_len(ctx.src_sg)) {
			ctx.src_offset = 0;
			ctx.src_sg = sg_next(ctx.src_sg);
		}

		if (ctx.dst_offset == sg_dma_len(ctx.dst_sg)) {
			ctx.dst_offset = 0;
			ctx.dst_sg = sg_next(ctx.dst_sg);
		}
	}

	meson_unmap_scatterlist(areq, mc);

unmap_keyiv:
	dma_unmap_single(mc->dev, ctx.keyiv.addr, ctx.keyiv.len, DMA_TO_DEVICE);

free_keyiv:
	kfree_sensitive(ctx.bkeyiv);

	return err;
}

int meson_handle_cipher_request(struct crypto_engine *engine, void *areq)
{
	int err;
	struct skcipher_request *breq = container_of(areq, struct skcipher_request, base);

	err = meson_cipher(breq);
	local_bh_disable();
	crypto_finalize_skcipher_request(engine, breq, err);
	local_bh_enable();

	return 0;
}

static int meson_skdecrypt(struct skcipher_request *areq)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(areq);
	struct crypto_engine *engine;
	int e;

	rctx->op_dir = MESON_DECRYPT;
	if (meson_cipher_need_fallback(areq))
		return meson_cipher_do_fallback(areq);
	e = meson_get_engine_number(op->mc);
	engine = op->mc->chanlist[e].engine;
	rctx->flow = e;

	return crypto_transfer_skcipher_request_to_engine(engine, areq);
}

static int meson_skencrypt(struct skcipher_request *areq)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(areq);
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_cipher_req_ctx *rctx = skcipher_request_ctx(areq);
	struct crypto_engine *engine;
	int e;

	rctx->op_dir = MESON_ENCRYPT;
	if (meson_cipher_need_fallback(areq))
		return meson_cipher_do_fallback(areq);
	e = meson_get_engine_number(op->mc);
	engine = op->mc->chanlist[e].engine;
	rctx->flow = e;

	return crypto_transfer_skcipher_request_to_engine(engine, areq);
}

static int meson_cipher_init(struct crypto_tfm *tfm)
{
	struct meson_cipher_tfm_ctx *op = crypto_tfm_ctx(tfm);
	struct meson_alg_template *algt;
	const char *name = crypto_tfm_alg_name(tfm);
	struct crypto_skcipher *sktfm = __crypto_skcipher_cast(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(sktfm);

	memset(op, 0, sizeof(struct meson_cipher_tfm_ctx));

	algt = container_of(alg, struct meson_alg_template, alg.skcipher.base);
	op->mc = algt->mc;

	op->fallback_tfm = crypto_alloc_skcipher(name, 0, CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(op->fallback_tfm)) {
		dev_err(op->mc->dev, "ERROR: Cannot allocate fallback for %s %ld\n",
			name, PTR_ERR(op->fallback_tfm));
		return PTR_ERR(op->fallback_tfm);
	}

	crypto_skcipher_set_reqsize(sktfm, sizeof(struct meson_cipher_req_ctx) +
				    crypto_skcipher_reqsize(op->fallback_tfm));

	return 0;
}

static void meson_cipher_exit(struct crypto_tfm *tfm)
{
	struct meson_cipher_tfm_ctx *op = crypto_tfm_ctx(tfm);

	kfree_sensitive(op->key);
	crypto_free_skcipher(op->fallback_tfm);
}

static int meson_aes_setkey(struct crypto_skcipher *tfm, const u8 *key,
			    unsigned int keylen)
{
	struct meson_cipher_tfm_ctx *op = crypto_skcipher_ctx(tfm);
	struct meson_dev *mc = op->mc;

	switch (keylen) {
	case AES_KEYSIZE_128:
		op->keymode = DESC_MODE_AES_128;
		break;
	case AES_KEYSIZE_192:
		op->keymode = DESC_MODE_AES_192;
		break;
	case AES_KEYSIZE_256:
		op->keymode = DESC_MODE_AES_256;
		break;
	default:
		dev_dbg(mc->dev, "ERROR: Invalid keylen %u\n", keylen);
		return -EINVAL;
	}
	kfree_sensitive(op->key);
	op->keylen = keylen;
	op->key = kmemdup(key, keylen, GFP_KERNEL | GFP_DMA);
	if (!op->key)
		return -ENOMEM;

	return crypto_skcipher_setkey(op->fallback_tfm, key, keylen);
}

static struct meson_alg_template algs[] = {
{
	.type = CRYPTO_ALG_TYPE_SKCIPHER,
	.blockmode = DESC_OPMODE_CBC,
	.alg.skcipher.base = {
		.base = {
			.cra_name = "cbc(aes)",
			.cra_driver_name = "cbc-aes-gxl",
			.cra_priority = 400,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_flags = CRYPTO_ALG_TYPE_SKCIPHER |
				CRYPTO_ALG_ASYNC | CRYPTO_ALG_ALLOCATES_MEMORY |
				CRYPTO_ALG_NEED_FALLBACK,
			.cra_ctxsize = sizeof(struct meson_cipher_tfm_ctx),
			.cra_module = THIS_MODULE,
			.cra_alignmask = 0xf,
			.cra_init = meson_cipher_init,
			.cra_exit = meson_cipher_exit,
		},
		.min_keysize	= AES_MIN_KEY_SIZE,
		.max_keysize	= AES_MAX_KEY_SIZE,
		.ivsize		= AES_BLOCK_SIZE,
		.setkey		= meson_aes_setkey,
		.encrypt	= meson_skencrypt,
		.decrypt	= meson_skdecrypt,
	},
	.alg.skcipher.op = {
		.do_one_request = meson_handle_cipher_request,
	},
},
{
	.type = CRYPTO_ALG_TYPE_SKCIPHER,
	.blockmode = DESC_OPMODE_ECB,
	.alg.skcipher.base = {
		.base = {
			.cra_name = "ecb(aes)",
			.cra_driver_name = "ecb-aes-gxl",
			.cra_priority = 400,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_flags = CRYPTO_ALG_TYPE_SKCIPHER |
				CRYPTO_ALG_ASYNC | CRYPTO_ALG_ALLOCATES_MEMORY |
				CRYPTO_ALG_NEED_FALLBACK,
			.cra_ctxsize = sizeof(struct meson_cipher_tfm_ctx),
			.cra_module = THIS_MODULE,
			.cra_alignmask = 0xf,
			.cra_init = meson_cipher_init,
			.cra_exit = meson_cipher_exit,
		},
		.min_keysize	= AES_MIN_KEY_SIZE,
		.max_keysize	= AES_MAX_KEY_SIZE,
		.setkey		= meson_aes_setkey,
		.encrypt	= meson_skencrypt,
		.decrypt	= meson_skdecrypt,
	},
	.alg.skcipher.op = {
		.do_one_request = meson_handle_cipher_request,
	},
},
};

int meson_cipher_register(struct meson_dev *mc)
{
	return meson_register_algs(mc, algs, ARRAY_SIZE(algs));
}

void meson_cipher_unregister(struct meson_dev *mc)
{
	meson_unregister_algs(mc, algs, ARRAY_SIZE(algs));
}

void meson_cipher_debugfs_show(struct seq_file *seq, void *v)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(algs); i++) {
		seq_printf(seq, "%s %s %lu %lu\n",
			   algs[i].alg.skcipher.base.base.cra_driver_name,
			   algs[i].alg.skcipher.base.base.cra_name,
#ifdef CONFIG_CRYPTO_DEV_AMLOGIC_GXL_DEBUG
			   algs[i].stat_req, algs[i].stat_fb);
#else
			   0ul, 0ul);
#endif
	}
}
