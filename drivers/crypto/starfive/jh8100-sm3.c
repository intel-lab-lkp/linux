// SPDX-License-Identifier: GPL-2.0
/*
 * SM3 Hash function and HMAC support for StarFive driver
 *
 * Copyright (c) 2022 - 2023 StarFive Technology
 *
 */

#include <crypto/engine.h>
#include <crypto/hash.h>
#include <crypto/scatterwalk.h>
#include <crypto/internal/hash.h>
#include "jh7110-cryp.h"
#include <linux/crypto.h>
#include <linux/dma/dw_axi.h>
#include <linux/io.h>
#include <linux/iopoll.h>

#define STARFIVE_SM3_REGS_OFFSET	0x4200
#define STARFIVE_SM3_CSR		(STARFIVE_SM3_REGS_OFFSET + 0x0)
#define STARFIVE_SM3_WDR		(STARFIVE_SM3_REGS_OFFSET + 0x4)
#define STARFIVE_SM3_RDR		(STARFIVE_SM3_REGS_OFFSET + 0x8)
#define STARFIVE_SM3_WSR		(STARFIVE_SM3_REGS_OFFSET + 0xC)
#define STARFIVE_SM3_WLEN3		(STARFIVE_SM3_REGS_OFFSET + 0x10)
#define STARFIVE_SM3_WLEN2		(STARFIVE_SM3_REGS_OFFSET + 0x14)
#define STARFIVE_SM3_WLEN1		(STARFIVE_SM3_REGS_OFFSET + 0x18)
#define STARFIVE_SM3_WLEN0		(STARFIVE_SM3_REGS_OFFSET + 0x1C)
#define STARFIVE_SM3_WKR		(STARFIVE_SM3_REGS_OFFSET + 0x20)
#define STARFIVE_SM3_WKLEN		(STARFIVE_SM3_REGS_OFFSET + 0x24)

#define STARFIVE_SM3_BUFLEN		SHA512_BLOCK_SIZE
#define STARFIVE_SM3_RESET		0x2

static inline int starfive_sm3_wait_busy(struct starfive_cryp_dev *cryp)
{
	u32 status;

	return readl_relaxed_poll_timeout(cryp->base + STARFIVE_SM3_CSR, status,
					  !(status & STARFIVE_SM3_BUSY), 10, 100000);
}

static inline int starfive_sm3_wait_hmac_done(struct starfive_cryp_dev *cryp)
{
	u32 status;

	return readl_relaxed_poll_timeout(cryp->base + STARFIVE_SM3_CSR, status,
					  (status & STARFIVE_SM3_HMAC_DONE), 10, 100000);
}

static inline int starfive_sm3_wait_key_done(struct starfive_cryp_dev *cryp)
{
	u32 status;

	return readl_relaxed_poll_timeout(cryp->base + STARFIVE_SM3_CSR, status,
					  (status & STARFIVE_SM3_KEY_DONE), 10, 100000);
}

static int starfive_sm3_hmac_key(struct starfive_cryp_ctx *ctx)
{
	struct starfive_cryp_request_ctx *rctx = ctx->rctx;
	struct starfive_cryp_dev *cryp = ctx->cryp;
	int klen = ctx->keylen, loop;
	unsigned int *key = (unsigned int *)ctx->key;
	unsigned char *cl;

	writel(ctx->keylen, cryp->base + STARFIVE_SM3_WKLEN);

	rctx->csr.sm3.hmac = 1;
	rctx->csr.sm3.key_flag = 1;

	writel(rctx->csr.sm3.v, cryp->base + STARFIVE_SM3_CSR);

	for (loop = 0; loop < klen / sizeof(unsigned int); loop++, key++)
		writel(*key, cryp->base + STARFIVE_SM3_WKR);

	if (klen & 0x3) {
		cl = (unsigned char *)key;
		for (loop = 0; loop < (klen & 0x3); loop++, cl++)
			writeb(*cl, cryp->base + STARFIVE_SM3_WKR);
	}

	if (starfive_sm3_wait_key_done(cryp))
		return dev_err_probe(cryp->dev, -ETIMEDOUT,
				     "starfive_sm3_wait_key_done error\n");

	return 0;
}

static void starfive_sm3_start(struct starfive_cryp_dev *cryp)
{
	union starfive_sm3_csr csr;

	csr.v = readl(cryp->base + STARFIVE_SM3_CSR);
	csr.firstb = 0;
	csr.final = 1;
	writel(csr.v, cryp->base + STARFIVE_SM3_CSR);
}

static void starfive_sm3_dma_callback(void *param)
{
	struct starfive_cryp_dev *cryp = param;

	complete(&cryp->dma_done);
}

static void starfive_sm3_dma_init(struct starfive_cryp_dev *cryp)
{
	struct dw_axi_peripheral_config periph_conf = {};

	memset(&cryp->cfg_in, 0, sizeof(struct dma_slave_config));
	periph_conf.quirks = DWAXIDMAC_STARFIVE_SM_ALGO;

	cryp->cfg_in.direction = DMA_MEM_TO_DEV;
	cryp->cfg_in.src_addr_width = DMA_SLAVE_BUSWIDTH_8_BYTES;
	cryp->cfg_in.dst_addr_width = DMA_SLAVE_BUSWIDTH_8_BYTES;
	cryp->cfg_in.src_maxburst = cryp->dma_maxburst;
	cryp->cfg_in.dst_maxburst = cryp->dma_maxburst;
	cryp->cfg_in.dst_addr = cryp->phys_base + STARFIVE_SM_ALG_FIFO_IN_OFFSET;
	cryp->cfg_in.peripheral_config = &periph_conf;
	cryp->cfg_in.peripheral_size = sizeof(struct dw_axi_peripheral_config);

	dmaengine_slave_config(cryp->tx, &cryp->cfg_in);

	init_completion(&cryp->dma_done);
}

static int starfive_sm3_dma_xfer(struct starfive_cryp_dev *cryp,
				 struct scatterlist *sg)
{
	struct dma_async_tx_descriptor *in_desc;
	union  starfive_sm_alg_cr alg_cr;
	int ret = 0;

	alg_cr.v = 0;
	alg_cr.start = 1;
	alg_cr.sm3_dma_en = 1;
	writel(alg_cr.v, cryp->base + STARFIVE_SM_ALG_CR_OFFSET);

	writel(sg_dma_len(sg), cryp->base + STARFIVE_SM_DMA_IN_LEN_OFFSET);
	sg_dma_len(sg) = ALIGN(sg_dma_len(sg), sizeof(u32));

	in_desc = dmaengine_prep_slave_sg(cryp->tx, sg, 1, DMA_MEM_TO_DEV,
					  DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!in_desc) {
		ret = -EINVAL;
		goto end;
	}

	reinit_completion(&cryp->dma_done);
	in_desc->callback = starfive_sm3_dma_callback;
	in_desc->callback_param = cryp;

	dmaengine_submit(in_desc);
	dma_async_issue_pending(cryp->tx);

	if (!wait_for_completion_timeout(&cryp->dma_done,
					 msecs_to_jiffies(1000)))
		ret = -ETIMEDOUT;

end:
	alg_cr.v = 0;
	alg_cr.clear = 1;
	writel(alg_cr.v, cryp->base + STARFIVE_SM_ALG_CR_OFFSET);

	return ret;
}

static int starfive_sm3_copy_hash(struct ahash_request *req)
{
	struct starfive_cryp_request_ctx *rctx = ahash_request_ctx(req);
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(crypto_ahash_reqtfm(req));
	int count, *data;
	int mlen;

	if (!req->result)
		return 0;

	mlen = rctx->digsize / sizeof(u32);
	data = (u32 *)req->result;

	for (count = 0; count < mlen; count++)
		data[count] = readl(ctx->cryp->base + STARFIVE_SM3_RDR);

	return 0;
}

static void starfive_sm3_done_task(struct starfive_cryp_dev *cryp)
{
	int err = cryp->err;

	if (!err)
		err = starfive_sm3_copy_hash(cryp->req.hreq);

	crypto_finalize_hash_request(cryp->engine, cryp->req.hreq, err);
}

static int starfive_sm3_one_request(struct crypto_engine *engine, void *areq)
{
	struct ahash_request *req =
		container_of(areq, struct ahash_request, base);
	struct starfive_cryp_ctx *ctx =
		crypto_ahash_ctx(crypto_ahash_reqtfm(req));
	struct starfive_cryp_dev *cryp = ctx->cryp;
	struct starfive_cryp_request_ctx *rctx = ctx->rctx;
	struct scatterlist *tsg;
	int ret, src_nents, i;

	rctx->csr.sm3.v = 0;
	rctx->csr.sm3.reset = 1;
	writel(rctx->csr.sm3.v, cryp->base + STARFIVE_SM3_CSR);

	if (starfive_sm3_wait_busy(cryp))
		return dev_err_probe(cryp->dev, -ETIMEDOUT, "Error resetting hardware.\n");

	cryp->err = 0;
	rctx->csr.sm3.v = 0;
	rctx->csr.sm3.mode = ctx->hash_mode;

	if (ctx->is_hmac) {
		ret = starfive_sm3_hmac_key(ctx);
		if (ret)
			return ret;
	} else {
		rctx->csr.sm3.start = 1;
		rctx->csr.sm3.firstb = 1;
		writel(rctx->csr.sm3.v, cryp->base + STARFIVE_SM3_CSR);
	}

	/* No input message, get digest and end. */
	if (!rctx->total)
		goto hash_start;

	starfive_sm3_dma_init(cryp);

	for_each_sg(rctx->in_sg, tsg, rctx->in_sg_len, i) {
		src_nents = dma_map_sg(cryp->dev, tsg, 1, DMA_TO_DEVICE);
		if (src_nents == 0)
			return dev_err_probe(cryp->dev, -ENOMEM,
					     "dma_map_sg error\n");

		ret = starfive_sm3_dma_xfer(cryp, tsg);
		dma_unmap_sg(cryp->dev, tsg, 1, DMA_TO_DEVICE);
		if (ret)
			return ret;
	}

hash_start:
	starfive_sm3_start(cryp);

	if (starfive_sm3_wait_busy(cryp))
		return dev_err_probe(cryp->dev, -ETIMEDOUT, "Error generating digest.\n");

	if (ctx->is_hmac)
		cryp->err = starfive_sm3_wait_hmac_done(cryp);

	starfive_sm3_done_task(cryp);

	return 0;
}

static void starfive_sm3_set_ahash(struct ahash_request *req,
				   struct starfive_cryp_ctx *ctx,
				   struct starfive_cryp_request_ctx *rctx)
{
	ahash_request_set_tfm(&rctx->ahash_fbk_req, ctx->ahash_fbk);
	ahash_request_set_callback(&rctx->ahash_fbk_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);
	ahash_request_set_crypt(&rctx->ahash_fbk_req, req->src,
				req->result, req->nbytes);
}

static int starfive_sm3_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct starfive_cryp_request_ctx *rctx = ahash_request_ctx(req);
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(tfm);

	starfive_sm3_set_ahash(req, ctx, rctx);

	return crypto_ahash_init(&rctx->ahash_fbk_req);
}

static int starfive_sm3_update(struct ahash_request *req)
{
	struct starfive_cryp_request_ctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(tfm);

	starfive_sm3_set_ahash(req, ctx, rctx);

	return crypto_ahash_update(&rctx->ahash_fbk_req);
}

static int starfive_sm3_final(struct ahash_request *req)
{
	struct starfive_cryp_request_ctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(tfm);

	starfive_sm3_set_ahash(req, ctx, rctx);

	return crypto_ahash_final(&rctx->ahash_fbk_req);
}

static int starfive_sm3_finup(struct ahash_request *req)
{
	struct starfive_cryp_request_ctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(tfm);

	starfive_sm3_set_ahash(req, ctx, rctx);

	return crypto_ahash_finup(&rctx->ahash_fbk_req);
}

static int starfive_sm3_digest(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(tfm);
	struct starfive_cryp_request_ctx *rctx = ahash_request_ctx(req);
	struct starfive_cryp_dev *cryp = ctx->cryp;

	memset(rctx, 0, sizeof(struct starfive_cryp_request_ctx));

	cryp->req.hreq = req;
	rctx->total = req->nbytes;
	rctx->in_sg = req->src;
	rctx->blksize = crypto_tfm_alg_blocksize(crypto_ahash_tfm(tfm));
	rctx->digsize = crypto_ahash_digestsize(tfm);
	rctx->in_sg_len = sg_nents_for_len(rctx->in_sg, rctx->total);
	ctx->rctx = rctx;

	return crypto_transfer_hash_request_to_engine(cryp->engine, req);
}

static int starfive_sm3_export(struct ahash_request *req, void *out)
{
	struct starfive_cryp_request_ctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->ahash_fbk_req, ctx->ahash_fbk);
	ahash_request_set_callback(&rctx->ahash_fbk_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);

	return crypto_ahash_export(&rctx->ahash_fbk_req, out);
}

static int starfive_sm3_import(struct ahash_request *req, const void *in)
{
	struct starfive_cryp_request_ctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(tfm);

	ahash_request_set_tfm(&rctx->ahash_fbk_req, ctx->ahash_fbk);
	ahash_request_set_callback(&rctx->ahash_fbk_req,
				   req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP,
				   req->base.complete, req->base.data);

	return crypto_ahash_import(&rctx->ahash_fbk_req, in);
}

static int starfive_sm3_init_algo(struct crypto_ahash *hash,
				  const char *alg_name,
				  bool is_hmac)
{
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(hash);

	ctx->cryp = starfive_cryp_find_dev(ctx);
	if (!ctx->cryp)
		return -ENODEV;

	ctx->ahash_fbk = crypto_alloc_ahash(alg_name, 0,
					    CRYPTO_ALG_NEED_FALLBACK);

	if (IS_ERR(ctx->ahash_fbk))
		return dev_err_probe(ctx->cryp->dev, PTR_ERR(ctx->ahash_fbk),
				     "starfive-sm3: Could not load fallback driver.\n");

	crypto_ahash_set_statesize(hash, crypto_ahash_statesize(ctx->ahash_fbk));
	crypto_ahash_set_reqsize(hash, sizeof(struct starfive_cryp_request_ctx) +
				 crypto_ahash_reqsize(ctx->ahash_fbk));

	ctx->keylen = 0;
	ctx->hash_mode = STARFIVE_SM3_MODE;
	ctx->is_hmac = is_hmac;

	return 0;
}

static void starfive_sm3_exit_tfm(struct crypto_ahash *hash)
{
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(hash);

	crypto_free_ahash(ctx->ahash_fbk);
}

static int starfive_sm3_long_setkey(struct starfive_cryp_ctx *ctx,
				    const u8 *key, unsigned int keylen)
{
	struct crypto_wait wait;
	struct ahash_request *req;
	struct scatterlist sg;
	struct crypto_ahash *ahash_tfm;
	struct starfive_cryp_dev *cryp = ctx->cryp;
	u8 *buf;
	int ret;

	ahash_tfm = crypto_alloc_ahash("sm3-starfive", 0, 0);
	if (IS_ERR(ahash_tfm))
		return PTR_ERR(ahash_tfm);

	req = ahash_request_alloc(ahash_tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto err_free_ahash;
	}

	crypto_init_wait(&wait);
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &wait);
	crypto_ahash_clear_flags(ahash_tfm, ~0);

	buf = devm_kzalloc(cryp->dev, keylen + STARFIVE_SM3_BUFLEN, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_free_req;
	}

	memcpy(buf, key, keylen);
	sg_init_one(&sg, buf, keylen);
	ahash_request_set_crypt(req, &sg, ctx->key, keylen);

	ret = crypto_wait_req(crypto_ahash_digest(req), &wait);

err_free_req:
	ahash_request_free(req);
err_free_ahash:
	crypto_free_ahash(ahash_tfm);
	return ret;
}

static int starfive_sm3_setkey(struct crypto_ahash *hash,
			       const u8 *key, unsigned int keylen)
{
	struct starfive_cryp_ctx *ctx = crypto_ahash_ctx(hash);
	unsigned int digestsize = crypto_ahash_digestsize(hash);
	unsigned int blocksize = crypto_ahash_blocksize(hash);

	crypto_ahash_setkey(ctx->ahash_fbk, key, keylen);

	if (keylen <= blocksize) {
		memcpy(ctx->key, key, keylen);
		ctx->keylen = keylen;
		return 0;
	}

	ctx->keylen = digestsize;

	return starfive_sm3_long_setkey(ctx, key, keylen);
}

static int starfive_sm3_init_tfm(struct crypto_ahash *hash)
{
	return starfive_sm3_init_algo(hash, "sm3-generic", 0);
}

static int starfive_hmac_sm3_init_tfm(struct crypto_ahash *hash)
{
	return starfive_sm3_init_algo(hash, "hmac(sm3-generic)", 1);
}

static struct ahash_engine_alg algs_sm3[] = {
{
	.base.init	= starfive_sm3_init,
	.base.update	= starfive_sm3_update,
	.base.final	= starfive_sm3_final,
	.base.finup	= starfive_sm3_finup,
	.base.digest	= starfive_sm3_digest,
	.base.export	= starfive_sm3_export,
	.base.import	= starfive_sm3_import,
	.base.init_tfm	= starfive_sm3_init_tfm,
	.base.exit_tfm	= starfive_sm3_exit_tfm,
	.base.halg = {
		.digestsize	= SM3_DIGEST_SIZE,
		.statesize	= sizeof(struct sm3_state),
		.base = {
			.cra_name		= "sm3",
			.cra_driver_name	= "sm3-starfive",
			.cra_priority		= 200,
			.cra_flags		= CRYPTO_ALG_ASYNC |
						  CRYPTO_ALG_TYPE_AHASH |
						  CRYPTO_ALG_NEED_FALLBACK,
			.cra_blocksize		= SM3_BLOCK_SIZE,
			.cra_ctxsize		= sizeof(struct starfive_cryp_ctx),
			.cra_module		= THIS_MODULE,
		}
	},
	.op = {
		.do_one_request = starfive_sm3_one_request,
	},
}, {
	.base.init	= starfive_sm3_init,
	.base.update	= starfive_sm3_update,
	.base.final	= starfive_sm3_final,
	.base.finup	= starfive_sm3_finup,
	.base.digest	= starfive_sm3_digest,
	.base.export	= starfive_sm3_export,
	.base.import	= starfive_sm3_import,
	.base.init_tfm	= starfive_hmac_sm3_init_tfm,
	.base.exit_tfm	= starfive_sm3_exit_tfm,
	.base.setkey	= starfive_sm3_setkey,
	.base.halg = {
		.digestsize	= SM3_DIGEST_SIZE,
		.statesize	= sizeof(struct sm3_state),
		.base = {
			.cra_name		= "hmac(sm3)",
			.cra_driver_name	= "sm3-hmac-starfive",
			.cra_priority		= 200,
			.cra_flags		= CRYPTO_ALG_ASYNC |
						  CRYPTO_ALG_TYPE_AHASH |
						  CRYPTO_ALG_NEED_FALLBACK,
			.cra_blocksize		= SM3_BLOCK_SIZE,
			.cra_ctxsize		= sizeof(struct starfive_cryp_ctx),
			.cra_module		= THIS_MODULE,
		}
	},
	.op = {
		.do_one_request = starfive_sm3_one_request,
	},
},
};

int starfive_sm3_register_algs(void)
{
	return crypto_engine_register_ahashes(algs_sm3, ARRAY_SIZE(algs_sm3));
}

void starfive_sm3_unregister_algs(void)
{
	crypto_engine_unregister_ahashes(algs_sm3, ARRAY_SIZE(algs_sm3));
}
