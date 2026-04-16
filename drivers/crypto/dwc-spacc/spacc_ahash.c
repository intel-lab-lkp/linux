// SPDX-License-Identifier: GPL-2.0

#include <linux/dmapool.h>
#include <crypto/sha1.h>
#include <crypto/sm4.h>
#include <crypto/sha2.h>
#include <crypto/sha3.h>
#include <crypto/md5.h>
#include <crypto/aes.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <crypto/scatterwalk.h>
#include <crypto/internal/hash.h>
#include <crypto/engine.h>
#include <linux/delay.h>
#include <linux/atomic.h>

#include "spacc_device.h"
#include "spacc_core.h"

#define PPP_BUF_SIZE 128

static struct mode_tab possible_hashes[] = {
	{ .keylen[0] = 16, MODE_TAB_HASH("cmac(aes)", MAC_CMAC, 16,  16) },
	{ .keylen[0] = 48 | MODE_TAB_HASH_XCBC, MODE_TAB_HASH("xcbc(aes)",
	MAC_XCBC, 16,  16) },

	{ MODE_TAB_HASH("cmac(sm4)",	MAC_SM4_CMAC, 16, 16) },
	{ .keylen[0] = 32 | MODE_TAB_HASH_XCBC, MODE_TAB_HASH("xcbc(sm4)",
	MAC_SM4_XCBC, 16, 16) },

	{ MODE_TAB_HASH("hmac(md5)",	HMAC_MD5, MD5_DIGEST_SIZE,
	MD5_HMAC_BLOCK_SIZE) },
	 { MODE_TAB_HASH("md5",		HASH_MD5, MD5_DIGEST_SIZE,
	MD5_HMAC_BLOCK_SIZE) },

	{ MODE_TAB_HASH("hmac(sha1)",	HMAC_SHA1, SHA1_DIGEST_SIZE,
	SHA1_BLOCK_SIZE) },
	{ MODE_TAB_HASH("sha1",		HASH_SHA1, SHA1_DIGEST_SIZE,
	SHA1_BLOCK_SIZE) },

	{ MODE_TAB_HASH("sha224",	HASH_SHA224, SHA224_DIGEST_SIZE,
	SHA224_BLOCK_SIZE) },
	{ MODE_TAB_HASH("sha256",	HASH_SHA256, SHA256_DIGEST_SIZE,
	SHA256_BLOCK_SIZE) },
	{ MODE_TAB_HASH("sha384",	HASH_SHA384, SHA384_DIGEST_SIZE,
	SHA384_BLOCK_SIZE) },
	{ MODE_TAB_HASH("sha512",	HASH_SHA512, SHA512_DIGEST_SIZE,
	SHA512_BLOCK_SIZE) },

	{ MODE_TAB_HASH("hmac(sha512)",	HMAC_SHA512, SHA512_DIGEST_SIZE,
	SHA512_BLOCK_SIZE) },
	{ MODE_TAB_HASH("hmac(sha224)",	HMAC_SHA224, SHA224_DIGEST_SIZE,
	SHA224_BLOCK_SIZE) },
	{ MODE_TAB_HASH("hmac(sha256)",	HMAC_SHA256, SHA256_DIGEST_SIZE,
	SHA256_BLOCK_SIZE) },
	{ MODE_TAB_HASH("hmac(sha384)",	HMAC_SHA384, SHA384_DIGEST_SIZE,
	SHA384_BLOCK_SIZE) },

	{ MODE_TAB_HASH("sha3-224", HASH_SHA3_224, SHA3_224_DIGEST_SIZE,
	SHA3_224_BLOCK_SIZE) },
	{ MODE_TAB_HASH("sha3-256", HASH_SHA3_256, SHA3_256_DIGEST_SIZE,
	SHA3_256_BLOCK_SIZE) },
	{ MODE_TAB_HASH("sha3-384", HASH_SHA3_384, SHA3_384_DIGEST_SIZE,
	SHA3_384_BLOCK_SIZE) },
	{ MODE_TAB_HASH("sha3-512", HASH_SHA3_512, SHA3_512_DIGEST_SIZE,
	SHA3_512_BLOCK_SIZE) },

	{ MODE_TAB_HASH("michael_mic", MAC_MICHAEL, 8, 8) },

};

static void spacc_hash_cleanup_dma_dst(struct spacc_crypto_ctx *tctx,
				       struct ahash_request *req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	pdu_ddt_free(&ctx->dst);
}

static void spacc_hash_cleanup_dma_src(struct spacc_crypto_ctx *tctx,
				       struct ahash_request *req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	if (tctx->tmp_sgl && tctx->tmp_sgl[0].length != 0) {
		dma_unmap_sg(tctx->dev, tctx->tmp_sgl, ctx->src_nents,
			     DMA_TO_DEVICE);
		kfree(tctx->tmp_sgl_buff);
		tctx->tmp_sgl_buff = NULL;
		tctx->tmp_sgl[0].length = 0;
	} else
		dma_unmap_sg(tctx->dev, req->src, ctx->src_nents,
			     DMA_TO_DEVICE);

	pdu_ddt_free(&ctx->src);
}

static void spacc_hash_cleanup_dma(struct device *dev,
				   struct ahash_request *req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	struct spacc_priv *priv = dev_get_drvdata(dev);

	dma_unmap_sg(dev, req->src, ctx->src_nents, DMA_TO_DEVICE);
	pdu_ddt_free(&ctx->src);

	dma_pool_free(priv->hash_pool, ctx->digest_buf, ctx->digest_dma);
	pdu_ddt_free(&ctx->dst);
}

static void spacc_init_calg(struct crypto_alg *calg,
			    const struct mode_tab *mode)
{
	strscpy(calg->cra_name, mode->name);
	calg->cra_name[sizeof(mode->name) - 1] = '\0';

	strscpy(calg->cra_driver_name, "spacc-");
	strcat(calg->cra_driver_name, mode->name);
	calg->cra_driver_name[sizeof(calg->cra_driver_name) - 1] = '\0';

	calg->cra_blocksize = mode->blocklen;
}

static int spacc_ctx_clone_handle(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(tfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);

	if (tctx->handle < 0)
		return -EINVAL;

	ctx->acb.new_handle = spacc_clone_handle(&priv->spacc, tctx->handle,
						 &ctx->acb);

	if (ctx->acb.new_handle < 0) {
		spacc_hash_cleanup_dma(tctx->dev, req);
		return -ENOMEM;
	}

	ctx->acb.tctx  = tctx;
	ctx->acb.ctx   = ctx;
	ctx->acb.req   = req;
	ctx->acb.spacc = &priv->spacc;

	return 0;
}

static int spacc_hash_init_dma(struct device *dev, struct ahash_request *req)
{
	int rc = -1;
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(tfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	struct spacc_priv *priv = dev_get_drvdata(dev);

	gfp_t mflags = GFP_KERNEL;

	ctx->digest_buf = dma_pool_alloc(priv->hash_pool, mflags,
					 &ctx->digest_dma);

	if (!ctx->digest_buf)
		return -ENOMEM;

	rc = pdu_ddt_init(dev, &ctx->dst, 1 | 0x80000000);
	if (rc < 0) {
		dev_err(dev, "ERR: PDU DDT init error\n");
		rc = -EIO;
		goto err_free_digest;
	}

	pdu_ddt_add(dev, &ctx->dst, ctx->digest_dma, SPACC_MAX_DIGEST_SIZE);

	if (ctx->total_nents > 0 && ctx->single_shot) {
		/* single shot */
		rc = spacc_ctx_clone_handle(req);
		if (rc < 0)
			goto err_free_dst;

		if (req->nbytes) {
			rc = spacc_sg_to_ddt(dev, req->src, req->nbytes,
					     &ctx->src, DMA_TO_DEVICE);
		} else {
			memset(tctx->tmp_buffer, '\0', PPP_BUF_SIZE);
			sg_set_buf(&tctx->tmp_sgl[0], tctx->tmp_buffer,
				   PPP_BUF_SIZE);
			rc = spacc_sg_to_ddt(dev, &tctx->tmp_sgl[0],
					     tctx->tmp_sgl[0].length,
					     &ctx->src, DMA_TO_DEVICE);
		}
	} else if (ctx->total_nents == 0 && req->nbytes == 0) {
		rc = spacc_ctx_clone_handle(req);
		if (rc < 0)
			goto err_free_dst;

		/* zero length case */
		memset(tctx->tmp_buffer, '\0', PPP_BUF_SIZE);
		sg_set_buf(&tctx->tmp_sgl[0], tctx->tmp_buffer, PPP_BUF_SIZE);
		rc = spacc_sg_to_ddt(dev, &tctx->tmp_sgl[0],
				     tctx->tmp_sgl[0].length,
				     &ctx->src, DMA_TO_DEVICE);
	}

	if (rc < 0)
		goto err_free_dst;

	ctx->src_nents = rc;

	return rc;

err_free_dst:
	pdu_ddt_free(&ctx->dst);
err_free_digest:
	dma_pool_free(priv->hash_pool, ctx->digest_buf, ctx->digest_dma);

	return rc;
}

static void spacc_free_mems(struct spacc_crypto_reqctx *ctx,
			    struct spacc_crypto_ctx *tctx,
			    struct ahash_request *req)
{
	spacc_hash_cleanup_dma_dst(tctx, req);
	spacc_hash_cleanup_dma_src(tctx, req);

	if (ctx->single_shot) {
		kfree(tctx->tmp_sgl);
		tctx->tmp_sgl = NULL;

		ctx->single_shot = 0;
		if (ctx->total_nents)
			ctx->total_nents = 0;
	}
}

static void spacc_digest_cb(void *spacc, void *tfm)
{
	struct ahash_cb_data *cb = tfm;
	struct spacc_device *device = spacc;
	struct spacc_priv *priv = container_of(device, struct spacc_priv,
						spacc);
	int dig_sz;
	int err;

	dig_sz = crypto_ahash_digestsize(crypto_ahash_reqtfm(cb->req));

	if (cb->ctx->single_shot)
		memcpy(cb->req->result, cb->ctx->digest_buf, dig_sz);
	else
		memcpy(cb->tctx->digest_ctx_buf, cb->ctx->digest_buf, dig_sz);

	err = cb->spacc->job[cb->new_handle].job_err;

	dma_pool_free(priv->hash_pool, cb->ctx->digest_buf,
			cb->ctx->digest_dma);

	spacc_free_mems(cb->ctx, cb->tctx, cb->req);
	spacc_close(cb->spacc, cb->new_handle);

	local_bh_disable();
	crypto_finalize_hash_request(priv->engine, cb->req, err);
	local_bh_enable();

}

static int spacc_hash_setkey(struct crypto_ahash *tfm, const u8 *key,
		unsigned int keylen)
{
	int rc = 0;
	int ret = 0;
	unsigned int block_size;
	unsigned int digest_size;
	const struct spacc_alg *salg = spacc_tfm_ahash(&tfm->base);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(tfm);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);

	block_size = crypto_tfm_alg_blocksize(&tfm->base);
	digest_size = crypto_ahash_digestsize(tfm);

	/*
	 * We are using hardware for HMAC operations. The software fallback is
	 * only for key pre-processing in case of HMACs.
	 * This was meant for hashes but it also works for cmac/xcbc since we
	 * only intend to support 128-bit keys...
	 */
	if (keylen > block_size && salg->mode->id != CRYPTO_MODE_MAC_CMAC) {
		dev_dbg(salg->dev, "Exceeds keylen: %u\n", keylen);
		dev_dbg(salg->dev, "Req. keylen hashing %s\n",
				salg->calg->cra_name);

		switch (salg->mode->id)	{
		case CRYPTO_MODE_HMAC_SHA224:
			sha224(key, keylen, tctx->ipad);
			break;

		case CRYPTO_MODE_HMAC_SHA256:
			sha256(key, keylen, tctx->ipad);
			break;

		case CRYPTO_MODE_HMAC_SHA384:
			sha384(key, keylen, tctx->ipad);
			break;

		case CRYPTO_MODE_HMAC_SHA512:
			sha512(key, keylen, tctx->ipad);
			break;

		case CRYPTO_MODE_HMAC_MD5:
			md5(key, keylen, tctx->ipad);
			break;

		case CRYPTO_MODE_HMAC_SHA1:
			sha1(key, keylen, tctx->ipad);
			break;
		default:
			return -EINVAL;
		}

		keylen = digest_size;
		dev_dbg(salg->dev, "updated keylen: %u\n", keylen);

		tctx->ctx_valid = false;
	} else {
		memcpy(tctx->ipad, key, keylen);
		tctx->ctx_valid = false;
	}

	/*
	 * Save the processed key length so that do_one_request can
	 * write the key context to HW later if setkey couldn't
	 * acquire a HW context now (e.g., all contexts busy).
	 */
	tctx->keylen = keylen;

	/* close handle since key size may have changed */
	if (tctx->handle >= 0) {
		spacc_close(&priv->spacc, tctx->handle);
		put_device(tctx->dev);
		tctx->handle = -1;
		tctx->dev = NULL;
	}

	/* reset priv */
	priv = NULL;
	priv = dev_get_drvdata(salg->dev);
	tctx->dev = get_device(salg->dev);
	ret = spacc_is_mode_keysize_supported(&priv->spacc, salg->mode->id,
					      keylen, 1);
	if (!ret) {
		dev_err(salg->dev, "Keylen: %d not enabled for algo: %d\n",
			keylen, salg->mode->id);
		put_device(tctx->dev);
		return -EINVAL;
	}

	/* Try to grab a HW context; if busy, defer to do_one_request */
	tctx->handle = spacc_open(&priv->spacc,
				  CRYPTO_MODE_NULL,
				  salg->mode->id, -1,
				  0, spacc_digest_cb, tfm);
	if (tctx->handle < 0) {
		dev_dbg(salg->dev, "Could not open SPAcc context now\n");
		tctx->handle = -1;
		/*
		 * Don't fail setkey — the fallback has the key and
		 * do_one_request will retry opening the HW context
		 * and writing the key when a request actually comes in.
		 */
		return 0;
	}

	rc = spacc_set_operation(&priv->spacc, tctx->handle, OP_ENCRYPT,
				 ICV_HASH, IP_ICV_OFFSET, 0, 0, 0);
	if (rc < 0) {
		spacc_close(&priv->spacc, tctx->handle);
		tctx->handle = -1;
		/* Not fatal — fallback will handle it */
		return 0;
	}

	if (salg->mode->id == CRYPTO_MODE_MAC_XCBC ||
	    salg->mode->id == CRYPTO_MODE_MAC_SM4_XCBC) {
		rc = spacc_compute_xcbc_key(&priv->spacc, salg->mode->id,
					    tctx->handle, tctx->ipad,
					    keylen, tctx->ipad);
		if (rc < 0) {
			dev_err(tctx->dev,
				"Failed to compute XCBC key: %d\n", rc);
			spacc_close(&priv->spacc, tctx->handle);
			tctx->handle = -1;
			/* Not fatal — fallback will handle it */
			return 0;
		}
		rc = spacc_write_context(&priv->spacc, tctx->handle,
					 SPACC_HASH_OPERATION, tctx->ipad,
					 32 + keylen, NULL, 0);
	} else {
		rc = spacc_write_context(&priv->spacc, tctx->handle,
					 SPACC_HASH_OPERATION, tctx->ipad,
					 keylen, NULL, 0);
	}

	memzero_explicit(tctx->ipad, sizeof(tctx->ipad));
	if (rc < 0) {
		dev_err(tctx->dev, "ERR: Failed to write SPAcc context\n");
		/* Non-fatal, we continue with the software fallback */
		return 0;
	}

	tctx->ctx_valid = true;

	return 0;
}

/* Crypto engine hash operation */

static int spacc_hash_do_one_request(struct crypto_engine *engine, void *areq)
{
	struct ahash_request *req = ahash_request_cast(areq);
	struct crypto_ahash *reqtfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(reqtfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);
	const struct spacc_alg *salg = spacc_tfm_ahash(&reqtfm->base);
	int rc = 0;

	ctx->single_shot = 1;
	ctx->total_nents = sg_nents(req->src);

	tctx->tmp_sgl = kmalloc_array(2, sizeof(*tctx->tmp_sgl), GFP_KERNEL);

	if (!tctx->tmp_sgl)
		goto fallback;

	sg_init_table(tctx->tmp_sgl, 2);
	tctx->tmp_sgl[0].length = 0;

	if (tctx->handle < 0 || !tctx->ctx_valid) {
		priv = dev_get_drvdata(salg->dev);
		tctx->dev = get_device(salg->dev);

		rc = spacc_is_mode_keysize_supported(&priv->spacc,
				salg->mode->id, 0, 1);
		if (!rc) {
			dev_dbg(salg->dev,
				"Mode %d not supported, falling back\n",
				salg->mode->id);
			goto fallback;
		}

		tctx->handle = spacc_open(&priv->spacc, CRYPTO_MODE_NULL,
				salg->mode->id, -1, 0,
				spacc_digest_cb, reqtfm);
		if (tctx->handle < 0) {
			dev_dbg(salg->dev,
				"Failed to open context, falling back\n");
			goto fallback;
		}

		rc = spacc_set_operation(&priv->spacc, tctx->handle,
				OP_ENCRYPT, ICV_HASH, IP_ICV_OFFSET,
				0, 0, 0);

		if (rc < 0) {
			spacc_close(&priv->spacc, tctx->handle);
			tctx->handle = -1;
			goto fallback;
		}

		if (tctx->keylen > 0) {
			if (salg->mode->id == CRYPTO_MODE_MAC_XCBC ||
				salg->mode->id == CRYPTO_MODE_MAC_SM4_XCBC) {
				rc = spacc_compute_xcbc_key(&priv->spacc,
						salg->mode->id,
						tctx->handle,
						tctx->ipad,
						tctx->keylen,
						tctx->ipad);
				if (rc < 0) {
					spacc_close(&priv->spacc, tctx->handle);
					tctx->handle = -1;
					goto fallback;
				}

				rc = spacc_write_context(&priv->spacc,
						tctx->handle,
						SPACC_HASH_OPERATION,
						tctx->ipad,
						32 + tctx->keylen,
						NULL, 0);
			} else {
				rc = spacc_write_context(&priv->spacc,
						tctx->handle,
						SPACC_HASH_OPERATION,
						tctx->ipad,
						tctx->keylen,
						NULL, 0);
			}

			if (rc < 0) {
				spacc_close(&priv->spacc, tctx->handle);
				tctx->handle = -1;
				goto fallback;
			}
		}

		tctx->ctx_valid = true;
	}

	rc = spacc_hash_init_dma(tctx->dev, req);
	if (rc < 0) {
		dev_dbg(salg->dev, "DMA init failed (%d), falling back\n", rc);
		goto fallback;
	}

	rc = spacc_packet_enqueue_ddt(&priv->spacc, ctx->acb.new_handle,
			&ctx->src, &ctx->dst, req->nbytes,
			0, req->nbytes, 0, 0, 0);
	if (rc < 0) {
		spacc_hash_cleanup_dma(tctx->dev, req);

		if (ctx->acb.new_handle >= 0) {
			spacc_close(&priv->spacc, ctx->acb.new_handle);
			ctx->acb.new_handle = -1;
		}

		if (rc == -EBUSY) {
			dev_dbg(salg->dev, "HW full, engine retry\n");
			return -ENOSPC;
		}

		return rc;

	}

	return 0;

fallback:
	kfree(tctx->tmp_sgl);
	tctx->tmp_sgl = NULL;

	HASH_FBREQ_ON_STACK(fbreq, req);

	rc = crypto_ahash_digest(fbreq);

	HASH_REQUEST_ZERO(fbreq);
	local_bh_disable();
	crypto_finalize_hash_request(engine, req, rc);
	local_bh_enable();

	return 0;
}

static int spacc_hash_init_tfm(struct crypto_ahash *tfm)
{
	const struct spacc_alg *salg = container_of(crypto_ahash_alg(tfm),
						    struct spacc_alg,
						    alg.hash.base);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(tfm);

	tctx->handle    = -1;
	tctx->ctx_valid = false;
	tctx->dev       = get_device(salg->dev);

	return 0;
}

static void spacc_hash_exit_tfm(struct crypto_ahash *tfm)
{
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(tfm);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);

	if (tctx->handle >= 0)
		spacc_close(&priv->spacc, tctx->handle);

	put_device(tctx->dev);
}

static int spacc_hash_init(struct ahash_request *req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	int rc;

	HASH_FBREQ_ON_STACK(fbreq, req);

	rc = crypto_ahash_init(fbreq);
	if (rc)
		goto out;

	rc = crypto_ahash_export(fbreq, ctx->state_buffer);
	if (rc)
		goto out;

out:
	HASH_REQUEST_ZERO(fbreq);
	return rc;
}

static int spacc_hash_update(struct ahash_request *req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	int rc;

	if (!req->nbytes)
		return 0;

	HASH_FBREQ_ON_STACK(fbreq, req);

	rc = crypto_ahash_import(fbreq, ctx->state_buffer);
	if (rc)
		goto out;

	rc = crypto_ahash_update(fbreq);
	if (rc)
		goto out;

	rc = crypto_ahash_export(fbreq, ctx->state_buffer);

out:
	HASH_REQUEST_ZERO(fbreq);
	return rc;
}

static int spacc_hash_final(struct ahash_request *req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	int rc;

	HASH_FBREQ_ON_STACK(fbreq, req);

	rc = crypto_ahash_import(fbreq, ctx->state_buffer);
	if (rc)
		goto out;

	rc = crypto_ahash_final(fbreq);
	if (rc)
		goto out;


out:
	HASH_REQUEST_ZERO(fbreq);
	return rc;
}

static int spacc_hash_digest(struct ahash_request *req)
{
	struct crypto_ahash *reqtfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(reqtfm);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);

	return crypto_transfer_hash_request_to_engine(priv->engine, req);
}

static int spacc_hash_finup(struct ahash_request *req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	int rc;

	HASH_FBREQ_ON_STACK(fbreq, req);

	rc = crypto_ahash_import(fbreq, ctx->state_buffer);
	if (rc)
		goto out;

	rc = crypto_ahash_finup(fbreq);
	if (rc)
		goto out;

out:
	HASH_REQUEST_ZERO(fbreq);
	return rc;
}

static int spacc_hash_export(struct ahash_request *req, void *out)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	memcpy(out, ctx->state_buffer, sizeof(ctx->state_buffer));
	return 0;
}

static int spacc_hash_import(struct ahash_request *req, const void *in)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	memcpy(ctx->state_buffer, in, sizeof(ctx->state_buffer));
	return 0;
}

static const struct ahash_engine_alg spacc_hash_template = {
	.base = {
		.init   = spacc_hash_init,
		.update = spacc_hash_update,
		.final  = spacc_hash_final,
		.finup  = spacc_hash_finup,
		.digest = spacc_hash_digest,
		.setkey = spacc_hash_setkey,
		.export = spacc_hash_export,
		.import = spacc_hash_import,
		.init_tfm = spacc_hash_init_tfm,
		.exit_tfm = spacc_hash_exit_tfm,

		.halg.base = {
			.cra_priority	= 300,
			.cra_module	= THIS_MODULE,
			.cra_ctxsize	= sizeof(struct spacc_crypto_ctx),
			.cra_reqsize	= sizeof(struct spacc_crypto_reqctx),
			.cra_flags	= CRYPTO_ALG_TYPE_AHASH    |
					  CRYPTO_ALG_ASYNC	   |
					  CRYPTO_ALG_OPTIONAL_KEY
		},
	},
	.op = {
		.do_one_request = spacc_hash_do_one_request,
	},
};

static int spacc_register_hash(struct spacc_alg *salg)
{
	int rc = 0;
	struct spacc_priv *priv = dev_get_drvdata(salg->dev);

	salg->calg = &salg->alg.hash.base.halg.base;
	salg->alg.hash = spacc_hash_template;

	spacc_init_calg(salg->calg, salg->mode);
	salg->alg.hash.base.halg.digestsize = salg->mode->hashlen;
	salg->alg.hash.base.halg.statesize = HASH_MAX_STATESIZE;

	rc = crypto_engine_register_ahash(&salg->alg.hash);
	if (rc < 0)
		return rc;

	guard(mutex)(&priv->hash_alg_mutex);
	list_add(&salg->list, &priv->hash_alg_list);

	return 0;

}

int spacc_probe_hashes(struct platform_device *spacc_pdev)
{
	int rc = 0;
	unsigned int index;
	int registered = 0;
	struct spacc_alg *salg;
	struct spacc_priv *priv = dev_get_drvdata(&spacc_pdev->dev);
	const char *name = NULL;

	/* Create per-device DMA pool */
	priv->hash_pool = dma_pool_create("spacc-digest", &spacc_pdev->dev,
					  SPACC_MAX_DIGEST_SIZE,
					  SPACC_DMA_ALIGN, SPACC_DMA_BOUNDARY);

	if (!priv->hash_pool)
		return -ENOMEM;

	INIT_LIST_HEAD(&priv->hash_alg_list);
	mutex_init(&priv->hash_alg_mutex);

	for (index = 0; index < ARRAY_SIZE(possible_hashes); index++) {
		name = possible_hashes[index].name;

		if (!crypto_has_ahash(name, 0, 0))
			continue;

		/* Just check hardware support - no .valid flag needed */
		if (spacc_is_mode_keysize_supported(&priv->spacc,
				    possible_hashes[index].id & 0xFF,
				    possible_hashes[index].hashlen, 1)) {
			salg = kmalloc_obj(*salg, GFP_KERNEL);
			if (!salg) {
				rc = -ENOMEM;
				goto err_destroy_pool;
			}

			salg->mode = &possible_hashes[index];
			salg->dev = &spacc_pdev->dev;

			rc = spacc_register_hash(salg);
			if (rc < 0) {
				kfree(salg);
				continue;
			}

			registered++;
		}
	}

	return registered;

err_destroy_pool:
	dma_pool_destroy(priv->hash_pool);
	priv->hash_pool = NULL;
	return rc;
}

int spacc_unregister_hash_algs(struct spacc_priv *priv)
{
	struct spacc_alg *salg, *tmp;

	if (!priv)
		return 0;

	guard(mutex)(&priv->hash_alg_mutex);

	list_for_each_entry_safe(salg, tmp, &priv->hash_alg_list, list) {
		crypto_engine_unregister_ahash(&salg->alg.hash);
		list_del(&salg->list);
		kfree(salg);
	}


	dma_pool_destroy(priv->hash_pool);
	priv->hash_pool = NULL;

	return 0;
}
