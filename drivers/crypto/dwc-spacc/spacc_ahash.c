// SPDX-License-Identifier: GPL-2.0

#include <crypto/internal/hash.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <crypto/sm3.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/sha3.h>
#include <crypto/md5.h>
#include <crypto/aes.h>
#include "spacc_device.h"

static LIST_HEAD(spacc_hash_alg_list);
static DEFINE_MUTEX(spacc_hash_alg_mutex);

static struct dma_pool *spacc_hash_pool;
struct sdesc {
	struct shash_desc shash;
	char ctx[];
};

/*Linked List Node*/
struct my_list {
	struct list_head list;     //linux kernel list implementation
	char *buffer;
};

/*Declare and init the head node of the linked list*/
LIST_HEAD(head_sglbuf);

struct scatterlist *ppp_sgl;

u8 cmac_aes_zero_message_hash[16];

const u8 cmac_aes_zero_message_hash_key16[16] = {
	0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
	0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46,
};

const u8 cmac_aes_zero_message_hash_key32[16] = {
	0x02, 0x89, 0x62, 0xf6, 0x1b, 0x7b, 0xf8, 0x9e,
	0xfc, 0x6b, 0x55, 0x1f, 0x46, 0x67, 0xd9, 0x83,
};

const u8 xcbc_aes_zero_message_hash[16] = {
	0x75, 0xf0, 0x25, 0x1d, 0x52, 0x8a, 0xc0, 0x1c,
	0x45, 0x73, 0xdf, 0xd5, 0x84, 0xd7, 0x9f, 0x29
};

const u8 sha3_224_zero_message_hash[SHA3_224_DIGEST_SIZE] = {
	0x6B, 0x4E, 0x03, 0x42, 0x36, 0x67, 0xDB, 0xB7, 0x3B, 0x6E, 0x15, 0x45,
	0x4F, 0x0E, 0xB1, 0xAB, 0xD4, 0x59, 0x7F, 0x9A, 0x1B, 0x07, 0x8E, 0x3F,
	0x5B, 0x5A, 0x6B, 0xC7
};

const u8 sha3_256_zero_message_hash[SHA3_256_DIGEST_SIZE] = {
	0xA7, 0xFF, 0xC6, 0xF8, 0xBF, 0x1E, 0xD7, 0x66,
	0x51, 0xC1, 0x47, 0x56, 0xA0, 0x61, 0xD6, 0x62,
	0xF5, 0x80, 0xFF, 0x4D, 0xE4, 0x3B, 0x49, 0xFA,
	0x82, 0xD8, 0x0A, 0x4B, 0x80, 0xF8, 0x43, 0x4A
};

const u8 sha3_384_zero_message_hash[SHA3_384_DIGEST_SIZE] = {
		0x0c, 0x63, 0xa7, 0x5b, 0x84, 0x5e, 0x4f, 0x7d,
		0x01, 0x10, 0x7d, 0x85, 0x2e, 0x4c, 0x24, 0x85,
		0xc5, 0x1a, 0x50, 0xaa, 0xaa, 0x94,	0xfc, 0x61,
		0x99, 0x5e, 0x71, 0xbb, 0xee, 0x98, 0x3a, 0x2a,
		0xc3, 0x71, 0x38, 0x31, 0x26, 0x4a, 0xdb, 0x47,
		0xfb, 0x6b, 0xd1, 0xe0,	0x58, 0xd5, 0xf0, 0x04,
};

const u8 sha3_512_zero_message_hash[SHA3_512_DIGEST_SIZE] = {
		0xa6, 0x9f, 0x73, 0xcc, 0xa2, 0x3a, 0x9a, 0xc5,
		0xc8, 0xb5, 0x67, 0xdc, 0x18, 0x5a, 0x75, 0x6e,
		0x97, 0xc9, 0x82, 0x16, 0x4f, 0xe2, 0x58, 0x59,
		0xe0, 0xd1, 0xdc, 0xc1, 0x47, 0x5c, 0x80, 0xa6,
		0x15, 0xb2, 0x12, 0x3a, 0xf1, 0xf5, 0xf9, 0x4c,
		0x11, 0xe3, 0xe9, 0x40, 0x2c, 0x3a, 0xc5, 0x58,
		0xf5, 0x00, 0x19, 0x9d, 0x95, 0xb6, 0xd3, 0xe3,
		0x01, 0x75, 0x85, 0x86, 0x28, 0x1d, 0xcd, 0x26,
};

const u8 michael_mic_zero_message_hash[] = {
		0x82, 0x92, 0x5c, 0x1c, 0xa1, 0xd1, 0x30, 0xb8
};

const u8 sm4_xcbc128_zero_message_hash[] = {
	0xa9, 0x9a, 0x5c, 0x44, 0xe2, 0x34,	0xee, 0x2c,
	0x9b, 0xe4, 0x9d, 0xca, 0x64, 0xb0, 0xa5, 0xc4
};

static struct mode_tab possible_hashes[] = {
	{ .keylen[0] = 16, MODE_TAB_HASH("cmac(aes)", MAC_CMAC, 16,  16),
	.sw_fb = true },
	{ .keylen[0] = 48 | MODE_TAB_HASH_XCBC, MODE_TAB_HASH("xcbc(aes)",
	MAC_XCBC, 16,  16), .sw_fb = true },

	{ MODE_TAB_HASH("cmac(sm4)", MAC_SM4_CMAC, 16, 16), .sw_fb = true },
	{ .keylen[0] = 32 | MODE_TAB_HASH_XCBC, MODE_TAB_HASH("xcbc(sm4)",
	MAC_SM4_XCBC, 16, 16), .sw_fb = true },

	{ MODE_TAB_HASH("hmac(md5)", HMAC_MD5, MD5_DIGEST_SIZE,
	MD5_HMAC_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("md5", HASH_MD5, MD5_DIGEST_SIZE,
	MD5_HMAC_BLOCK_SIZE), .sw_fb = true },

	{ MODE_TAB_HASH("hmac(sha1)", HMAC_SHA1, SHA1_DIGEST_SIZE,
	SHA1_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("sha1", HASH_SHA1, SHA1_DIGEST_SIZE,
	SHA1_BLOCK_SIZE), .sw_fb = true },

	{ MODE_TAB_HASH("sha224",	HASH_SHA224, SHA224_DIGEST_SIZE,
	SHA224_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("sha256",	HASH_SHA256, SHA256_DIGEST_SIZE,
	SHA256_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("sha384",	HASH_SHA384, SHA384_DIGEST_SIZE,
	SHA384_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("sha512",	HASH_SHA512, SHA512_DIGEST_SIZE,
	SHA512_BLOCK_SIZE), .sw_fb = true },

	{ MODE_TAB_HASH("hmac(sha224)",	HMAC_SHA224, SHA224_DIGEST_SIZE,
	SHA224_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("hmac(sha256)",	HMAC_SHA256, SHA256_DIGEST_SIZE,
	SHA256_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("hmac(sha384)",	HMAC_SHA384, SHA384_DIGEST_SIZE,
	SHA384_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("hmac(sha512)",	HMAC_SHA512, SHA512_DIGEST_SIZE,
	SHA512_BLOCK_SIZE), .sw_fb = true },

	{ MODE_TAB_HASH("sha3-224", HASH_SHA3_224, SHA3_224_DIGEST_SIZE,
	SHA3_224_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("sha3-256", HASH_SHA3_256, SHA3_256_DIGEST_SIZE,
	SHA3_256_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("sha3-384", HASH_SHA3_384, SHA3_384_DIGEST_SIZE,
	SHA3_384_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("sha3-512", HASH_SHA3_512, SHA3_512_DIGEST_SIZE,
	SHA3_512_BLOCK_SIZE), .sw_fb = true },

	{ MODE_TAB_HASH("hmac(sm3)", HMAC_SM3, SM3_DIGEST_SIZE,
	SM3_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("sm3", HASH_SM3, SM3_DIGEST_SIZE,
	SM3_BLOCK_SIZE), .sw_fb = true },
	{ MODE_TAB_HASH("michael_mic", MAC_MICHAEL, 8, 8), .sw_fb = true },

	/* Below algorithms are not supported in crypto test manager */
	{ MODE_TAB_HASH("mac(kasumi)", MAC_KASUMI_F9, 4, 64), .sw_fb = false },
	{ MODE_TAB_HASH("mac(snow3g)", MAC_SNOW3G_UIA2, 4, 64),
	.sw_fb = false },
	{ MODE_TAB_HASH("mac(zuc)", MAC_ZUC_UIA3, 4, 64), .sw_fb = false },
	{ MODE_TAB_HASH("sslmac(md5)", SSLMAC_MD5, 32, 64), .sw_fb = false },
	{ MODE_TAB_HASH("sslmac(sha1)", SSLMAC_SHA1, 32, 64), .sw_fb = false },
	{ MODE_TAB_HASH("sha512-224", HASH_SHA512_224, 28, 128),
	.sw_fb = false },
	{ MODE_TAB_HASH("sha512-256", HASH_SHA512_256, 32, 128),
	.sw_fb = false },
	{ MODE_TAB_HASH("hmac(sha512-224)", HMAC_SHA512_224, 28, 128),
	.sw_fb = false },
	{ MODE_TAB_HASH("hmac(sha512-256)", HMAC_SHA512_256, 32, 128),
	.sw_fb = false },
	{ MODE_TAB_HASH("shake128", HASH_SHAKE128, 16, 64), .sw_fb = false },
	{ MODE_TAB_HASH("shake256", HASH_SHAKE256, 32, 64), .sw_fb = false },
	{ MODE_TAB_HASH("cshake128", HASH_CSHAKE128, 32, 64), .sw_fb = false },
	{ MODE_TAB_HASH("cshake256", HASH_CSHAKE256, 32, 64), .sw_fb = false },
	{ MODE_TAB_HASH("kmac128", MAC_KMAC128, 32, 64), .sw_fb = false },
	{ MODE_TAB_HASH("kmac256", MAC_KMAC256, 32, 64), .sw_fb = false },
	{ MODE_TAB_HASH("kmacxof128", MAC_KMACXOF128, 32, 64),
	.sw_fb = false },
	{ MODE_TAB_HASH("kmacxof256", MAC_KMACXOF256, 32, 64), .sw_fb = false },

};

static void spacc_init_calg(struct crypto_alg *calg,
			    const struct mode_tab *mode)
{
	snprintf(calg->cra_name, sizeof(calg->cra_name), "%s", mode->name);
	snprintf(calg->cra_driver_name, sizeof(calg->cra_driver_name),
		 "spacc-%s", mode->name);
	calg->cra_blocksize = mode->blocklen;
}

static void sgl_node_delete(void)
{
	/* Go through the list and free the memory. */
	struct my_list *cursor, *temp;

	list_for_each_entry_safe(cursor, temp, &head_sglbuf, list) {
		kfree(cursor->buffer);
		list_del(&cursor->list);
		kfree(cursor);
	}
}

static void sg_node_create_add(char *sg_buf)
{
	struct my_list *temp_node = NULL;

	/*Creating Node*/
	temp_node = kmalloc(sizeof(struct my_list), GFP_KERNEL);
	/*Assgin the data that is received*/
	temp_node->buffer = sg_buf;
	/*Init the list within the struct*/
	INIT_LIST_HEAD(&temp_node->list);
	/*Add Node to Linked List*/
	list_add_tail(&temp_node->list, &head_sglbuf);
}

static int spacc_hash_init_sg_list(struct device *dev,
				   struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(tfm);
	const struct spacc_alg *salg = spacc_tfm_ahash(&tfm->base);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	int blk_sz;
	int prev_rem_len = ctx->rem_len;
	char *sgl_buffer;

	if (ctx->total_nents && !ctx->single_shot) {
		switch (salg->mode->id) {
		case CRYPTO_MODE_HASH_SHA384:
		case CRYPTO_MODE_HMAC_SHA384:
		case CRYPTO_MODE_HASH_SHA512:
		case CRYPTO_MODE_HMAC_SHA512:
			blk_sz = 128;
			break;
		default:
			blk_sz = 64;
		}

		ctx->rem_len = modify_scatterlist(req->src,
						  &ppp_sgl[ctx->head_sg],
						  tctx->ppp_buffer,
						  prev_rem_len, blk_sz,
						  sgl_buffer,
						  req->nbytes);

		sg_node_create_add(sgl_buffer);

		ctx->head_sg++;
		if (ctx->head_sg > 1)
			ctx->head_sg = 0;
	}

	return ctx->rem_len;
}

static int spacc_hash_init_dma(struct device *dev, struct ahash_request *req,
			       int final)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(tfm);
	const struct spacc_alg *salg = spacc_tfm_ahash(&tfm->base);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	gfp_t mflags = GFP_ATOMIC;
	int rc = -1, blk_sz = 64;
	char *sgl_buffer;

	int prev_rem_len = ctx->rem_len;
	int nbytes = req->nbytes;

	if (req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP)
		mflags = GFP_KERNEL;

	ctx->digest_buf = dma_pool_alloc(spacc_hash_pool, mflags,
					 &ctx->digest_dma);
	if (!ctx->digest_buf)
		return -ENOMEM;

	rc = pdu_ddt_init(&ctx->dst, 1 | 0x80000000);
	if (rc < 0) {
		pr_err("ERR: PDU DDT init error\n");
		rc = -EIO;
		goto err_free_digest;
	}
	pdu_ddt_add(&ctx->dst, ctx->digest_dma, SPACC_MAX_DIGEST_SIZE);

	if (ctx->total_nents && !ctx->single_shot && !final) {
		switch (salg->mode->id) {
		case CRYPTO_MODE_HASH_SHA384:
		case CRYPTO_MODE_HMAC_SHA384:
		case CRYPTO_MODE_HASH_SHA512:
		case CRYPTO_MODE_HMAC_SHA512:
			blk_sz = 128;
			break;
		default:
			blk_sz = 64;
		}

		ctx->rem_len = modify_scatterlist(req->src,
						  &ppp_sgl[ctx->head_sg],
						  tctx->ppp_buffer,
						  prev_rem_len, blk_sz,
						  sgl_buffer,
						  nbytes);

		sg_node_create_add(sgl_buffer);

		ctx->head_sg++;
		if (ctx->head_sg > 1)
			ctx->head_sg = 0;
	}

	if (ctx->total_nents && !ctx->single_shot) {
		if (ppp_sgl[ctx->tail_sg].length == 0) {
			ctx->tail_sg++;
			if (ctx->tail_sg > 1)
				ctx->tail_sg = 0;

			return 0;
		}

		req->nbytes = ppp_sgl[ctx->tail_sg].length;
		rc = spacc_sg_to_ddt(dev, &ppp_sgl[ctx->tail_sg],
				     ppp_sgl[ctx->tail_sg].length,
				&ctx->src, DMA_TO_DEVICE);

		ctx->tail_sg++;
		if (ctx->tail_sg > 1)
			ctx->tail_sg = 0;

	} else {
		rc = spacc_sg_to_ddt(dev, req->src, req->nbytes, &ctx->src,
				     DMA_TO_DEVICE);
	}

	if (rc < 0)
		goto err_free_dst;

	ctx->src_nents = rc;

	return rc;
err_free_dst:
	pdu_ddt_free(&ctx->dst);
err_free_digest:
	dma_pool_free(spacc_hash_pool, ctx->digest_buf, ctx->digest_dma);
	return rc;
}

static void spacc_hash_cleanup_dma_dst(struct device *dev, struct ahash_request
		*req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	dma_pool_free(spacc_hash_pool, ctx->digest_buf, ctx->digest_dma);
	pdu_ddt_free(&ctx->dst);
}

static void spacc_hash_cleanup_dma_src(struct device *dev, struct ahash_request
		*req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	dma_unmap_sg(dev, req->src, ctx->src_nents, DMA_TO_DEVICE);
	pdu_ddt_free(&ctx->src);
}

static void spacc_hash_cleanup_dma(struct device *dev, struct ahash_request
		*req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	dma_unmap_sg(dev, req->src, ctx->src_nents, DMA_TO_DEVICE);
	pdu_ddt_free(&ctx->src);

	dma_pool_free(spacc_hash_pool, ctx->digest_buf, ctx->digest_dma);
	pdu_ddt_free(&ctx->dst);
}

static void spacc_hash_cleanup_ppp(struct ahash_request *req)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	if (ctx->total_nents) {
		ctx->total_nents = 0;
		ctx->cur_part_pck = 0;
	}
}

static void spacc_digest_cb(void *spacc, void *tfm)
{
	struct ahash_cb_data *cb = tfm;
	int err = -1;

	spacc_hash_cleanup_dma_src(cb->tctx->dev, cb->req);
	if (cb->ctx->single_shot || cb->ctx->final_part_pck) {
		if (cb->ctx->single_shot)
			cb->ctx->single_shot = 0;

		memcpy(cb->req->result,
		       cb->ctx->digest_buf,
				crypto_ahash_digestsize
				(crypto_ahash_reqtfm(cb->req)));
		err = cb->spacc->job[cb->new_handle].job_err;
		spacc_hash_cleanup_dma_dst(cb->tctx->dev, cb->req);

		if (cb->ctx->final_part_pck) {
			cb->ctx->final_part_pck = 0;
			spacc_hash_cleanup_ppp(cb->req);
		}
		spacc_close(cb->spacc, cb->new_handle);
	} else {
		memcpy(cb->tctx->digest_ctx_buf, cb->ctx->digest_buf,
		       crypto_ahash_digestsize
		       (crypto_ahash_reqtfm(cb->req)));
		err = cb->spacc->job[cb->new_handle].job_err;
		spacc_hash_cleanup_dma_dst(cb->tctx->dev, cb->req);
		spacc_handle_release(cb->spacc, cb->new_handle);
	}

	/* call complete */
	ahash_request_complete(cb->req, err);
}

static int zero_message_process(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	int digest_sz = crypto_ahash_digestsize(tfm);
	const struct spacc_alg *salg = spacc_tfm_ahash(&tfm->base);

	switch (salg->mode->id) {
	case CRYPTO_MODE_HASH_SM3:
	case CRYPTO_MODE_HMAC_SM3:
		memcpy(req->result, sm3_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HMAC_SHA224:
	case CRYPTO_MODE_HASH_SHA224:
		memcpy(req->result, sha224_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HMAC_SHA256:
	case CRYPTO_MODE_HASH_SHA256:
		memcpy(req->result, sha256_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HMAC_SHA384:
	case CRYPTO_MODE_HASH_SHA384:
		memcpy(req->result, sha384_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HMAC_SHA512:
	case CRYPTO_MODE_HASH_SHA512:
		memcpy(req->result, sha512_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HMAC_MD5:
	case CRYPTO_MODE_HASH_MD5:
		memcpy(req->result, md5_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HMAC_SHA1:
	case CRYPTO_MODE_HASH_SHA1:
		memcpy(req->result, sha1_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_MAC_XCBC:
		memcpy(req->result, xcbc_aes_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_MAC_CMAC:
		memcpy(req->result, cmac_aes_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HASH_SHA3_224:
		memcpy(req->result, sha3_224_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HASH_SHA3_256:
		memcpy(req->result, sha3_256_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HASH_SHA3_384:
		memcpy(req->result, sha3_384_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_HASH_SHA3_512:
		memcpy(req->result, sha3_512_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_MAC_MICHAEL:
		memcpy(req->result, michael_mic_zero_message_hash, digest_sz);
		break;

	case CRYPTO_MODE_MAC_SM4_XCBC:
		memcpy(req->result, sm4_xcbc128_zero_message_hash, digest_sz);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

int do_shash(unsigned char *name, unsigned char *result,
	     const u8 *data1, unsigned int data1_len,
	     const u8 *data2, unsigned int data2_len,
	     const u8 *key, unsigned int key_len)
{
	int rc;
	unsigned int size;
	struct crypto_shash *hash;
	struct sdesc *sdesc;

	hash = crypto_alloc_shash(name, 0, 0);
	if (IS_ERR(hash)) {
		rc = PTR_ERR(hash);
		pr_err("ERR: Crypto %s allocation error %d\n", name, rc);
		return rc;
	}

	size = sizeof(struct shash_desc) + crypto_shash_descsize(hash);
	sdesc = kmalloc(size, GFP_KERNEL);
	if (!sdesc) {
		rc = -ENOMEM;
		goto do_shash_err;
	}
	sdesc->shash.tfm = hash;

	if (key_len > 0) {
		rc = crypto_shash_setkey(hash, key, key_len);
		if (rc) {
			pr_err("ERR: Could not setkey %s shash\n", name);
			goto do_shash_err;
		}
	}

	rc = crypto_shash_init(&sdesc->shash);
	if (rc) {
		pr_err("ERR: Could not init %s shash\n", name);
		goto do_shash_err;
	}
	rc = crypto_shash_update(&sdesc->shash, data1, data1_len);
	if (rc) {
		pr_err("ERR: Could not update1\n");
		goto do_shash_err;
	}
	if (data2 && data2_len) {
		rc = crypto_shash_update(&sdesc->shash, data2, data2_len);
		if (rc) {
			pr_err("ERR: Could not update2\n");
			goto do_shash_err;
		}
	}
	rc = crypto_shash_final(&sdesc->shash, result);
	if (rc)
		pr_err("ERR: Could not generate %s hash\n", name);

do_shash_err:
	crypto_free_shash(hash);
	kfree(sdesc);

	return rc;
}

static int spacc_hash_setkey(struct crypto_ahash *tfm, const u8 *key, unsigned
		int keylen)
{
	const struct spacc_alg *salg = spacc_tfm_ahash(&tfm->base);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(tfm);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);

	unsigned int digest_size, block_size;
	int x, rc;

	char hash_alg[CRYPTO_MAX_ALG_NAME];

	dev_dbg(salg->dev[0], "keylen: %u\n", keylen);

	block_size = crypto_tfm_alg_blocksize(&tfm->base);
	digest_size = crypto_ahash_digestsize(tfm);

	/*
	 * If keylen > hash block len, the key is supposed to be hashed so that
	 * it  is less than the block length.  This is kind of a useless
	 * property of  HMAC as you can just use that hash as the key directly.
	 * We will just  not use the hardware in this case to avoid the issue.
	 * This test was meant for hashes but it works for cmac/xcbc since we
	 * only intend to support 128-bit keys...
	 */

	if (keylen > block_size && salg->mode->id != CRYPTO_MODE_MAC_CMAC) {
		dev_dbg(salg->dev[0], "Exceeds keylen: %u\n", keylen);
		dev_dbg(salg->dev[0], "Req. keylen hashing %s\n",
			salg->calg->cra_name);

		memset(hash_alg, 0x00, CRYPTO_MAX_ALG_NAME);
		switch (salg->mode->id)	{
		case CRYPTO_MODE_HMAC_SHA224:
			rc = do_shash("sha224", tctx->ipad, key, keylen,
				      NULL, 0, NULL, 0);
			break;

		case CRYPTO_MODE_HMAC_SHA256:
			rc = do_shash("sha256", tctx->ipad, key, keylen,
				      NULL, 0, NULL, 0);
			break;

		case CRYPTO_MODE_HMAC_SHA384:
			rc = do_shash("sha384", tctx->ipad, key, keylen,
				      NULL, 0, NULL, 0);
			break;

		case CRYPTO_MODE_HMAC_SHA512:
			rc = do_shash("sha512", tctx->ipad, key, keylen,
				      NULL, 0, NULL, 0);
			break;

		case CRYPTO_MODE_HMAC_MD5:
			rc = do_shash("md5", tctx->ipad, key, keylen,
				      NULL, 0, NULL, 0);
			break;

		case CRYPTO_MODE_HMAC_SHA1:
			rc = do_shash("sha1", tctx->ipad, key, keylen,
				      NULL, 0, NULL, 0);
			break;

		default:
			return -EINVAL;
		}

		if (rc < 0) {
			pr_err("ERR: %d computing shash for %s\n",
								rc, hash_alg);
			return -EIO;
		}

		keylen = digest_size;
		dev_dbg(salg->dev[0], "updated keylen: %u\n", keylen);
	} else {
		memcpy(tctx->ipad, key, keylen);
	}

	tctx->ctx_valid = false;

	if (salg->mode->id == CRYPTO_MODE_MAC_CMAC) {
		switch (keylen) {
		case AES_KEYSIZE_128:
			memcpy(cmac_aes_zero_message_hash,
			       cmac_aes_zero_message_hash_key16,
			       AES_BLOCK_SIZE);
			break;

		case AES_KEYSIZE_256:
			memcpy(cmac_aes_zero_message_hash,
			       cmac_aes_zero_message_hash_key32,
			       AES_BLOCK_SIZE);
			break;
		}
	}

	if (salg->mode->sw_fb) {
		rc = crypto_ahash_setkey(tctx->fb.hash, key, keylen);
		if (rc < 0)
			return rc;
	}

	/* close handle since key size may have changed */
	if (tctx->handle >= 0) {
		spacc_close(&priv->spacc, tctx->handle);
		put_device(tctx->dev);
		tctx->handle = -1;
		tctx->dev = NULL;
	}

	priv = NULL;
	for (x = 0; x < ELP_CAPI_MAX_DEV && salg->dev[x]; x++) {
		priv = dev_get_drvdata(salg->dev[x]);
		tctx->dev = get_device(salg->dev[x]);
		if (spacc_isenabled(&priv->spacc, salg->mode->id, keylen))
			tctx->handle = spacc_open(&priv->spacc,
						  CRYPTO_MODE_NULL,
						  salg->mode->id, -1,
						  0, spacc_digest_cb, tfm);

		if (tctx->handle >= 0)
			break;

		put_device(salg->dev[x]);
	}

	if (tctx->handle < 0) {
		pr_err("ERR: Failed to open SPAcc context\n");
		dev_dbg(salg->dev[0], "Failed to open SPAcc context\n");
		return -EIO;
	}

	rc = spacc_set_operation(&priv->spacc, tctx->handle, OP_ENCRYPT,
				 ICV_HASH, IP_ICV_OFFSET, 0, 0, 0);
	if (rc < 0) {
		spacc_close(&priv->spacc, tctx->handle);
		tctx->handle = -1;
		put_device(tctx->dev);
		return -EIO;
	}

	if (salg->mode->id == CRYPTO_MODE_MAC_XCBC ||
	    salg->mode->id == CRYPTO_MODE_MAC_SM4_XCBC) {
		rc = spacc_compute_xcbc_key(&priv->spacc, salg->mode->id,
					    tctx->handle, tctx->ipad,
					    keylen, tctx->ipad);
		if (rc < 0) {
			dev_warn(tctx->dev, "Failed to compute XCBC key: %d\n",
				 rc);
			return -EIO;
		}
		rc = spacc_write_context(&priv->spacc, tctx->handle,
					 SPACC_HASH_OPERATION, tctx->ipad, 32 +
					 keylen, NULL, 0);
	} else {
		rc = spacc_write_context(&priv->spacc, tctx->handle,
					 SPACC_HASH_OPERATION, tctx->ipad,
					 keylen, NULL, 0);
	}
	memset(tctx->ipad, 0, sizeof(tctx->ipad));
	if (rc < 0) {
		pr_err("ERR: Failed to write SPAcc context\n");
		dev_warn(tctx->dev, "Failed to write SPAcc context %d: %d\n",
			 tctx->handle, rc);

		/* Non-fatal; we continue with the software fallback. */
		return 0;
	}

	tctx->ctx_valid = true;

	return 0;
}

static int spacc_hash_cra_init(struct crypto_tfm *tfm)
{
	const struct spacc_alg *salg = spacc_tfm_ahash(tfm);
	struct spacc_crypto_ctx *tctx = crypto_tfm_ctx(tfm);
	struct spacc_priv *priv;

	dev_dbg(salg->dev[0], "%s: %s\n", __func__, salg->calg->cra_name);

	tctx->handle = -1;
	tctx->ctx_valid = false;
	tctx->dev = get_device(salg->dev[0]);

	if (salg->mode->sw_fb) {
		tctx->fb.hash = crypto_alloc_ahash(salg->calg->cra_name, 0,
						   CRYPTO_ALG_NEED_FALLBACK);
		if (IS_ERR(tctx->fb.hash)) {
			if (tctx->handle >= 0)
				spacc_close(&priv->spacc, tctx->handle);
			put_device(tctx->dev);
			return PTR_ERR(tctx->fb.hash);
		}

		crypto_ahash_set_reqsize(__crypto_ahash_cast(tfm),
					 sizeof(struct spacc_crypto_reqctx)
					 + crypto_ahash_reqsize(tctx->fb.hash));

	} else {
		crypto_ahash_set_reqsize(__crypto_ahash_cast(tfm),
					 sizeof(struct spacc_crypto_reqctx));
	}

	return 0;
}

static void spacc_hash_cra_exit(struct crypto_tfm *tfm)
{
	struct spacc_crypto_ctx *tctx = crypto_tfm_ctx(tfm);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);

	sgl_node_delete();
	crypto_free_ahash(tctx->fb.hash);

	if (tctx->handle >= 0)
		spacc_close(&priv->spacc, tctx->handle);

	put_device(tctx->dev);
}

static int spacc_hash_init(struct ahash_request *req)
{
	struct crypto_ahash *reqtfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(reqtfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	const struct spacc_alg *salg = spacc_tfm_ahash(&reqtfm->base);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);
	int x = 0, rc = 0;

	ctx->digest_buf = NULL;
	ctx->single_shot = 0;
	ctx->total_nents = 0;
	ctx->cur_part_pck = 0;
	ctx->small_pck = 0;
	ctx->final_part_pck = 0;
	ctx->head_sg = 0;
	ctx->tail_sg = 0;
	ctx->rem_len = 0;

	if (tctx->handle < 0 || !tctx->ctx_valid) {
		priv = NULL;
		dev_dbg(tctx->dev, "%s: open SPAcc context\n", __func__);
		for (x = 0; x < ELP_CAPI_MAX_DEV && salg->dev[x]; x++) {
			priv = dev_get_drvdata(salg->dev[x]);
			tctx->dev = get_device(salg->dev[x]);
			if (spacc_isenabled(&priv->spacc, salg->mode->id, 0))
				tctx->handle = spacc_open(&priv->spacc,
							  CRYPTO_MODE_NULL,
						salg->mode->id, -1, 0,
						spacc_digest_cb, reqtfm);

			if (tctx->handle >= 0)
				break;

			put_device(salg->dev[x]);
		}

		if (tctx->handle < 0) {
			dev_dbg(salg->dev[0], "fail to open SPAcc context\n");
			goto fallback;
		}

		rc = spacc_set_operation(&priv->spacc, tctx->handle,
					 OP_ENCRYPT, ICV_HASH, IP_ICV_OFFSET,
					 0, 0, 0);
		if (rc < 0) {
			spacc_close(&priv->spacc, tctx->handle);
			dev_dbg(salg->dev[0], "fail to open SPAcc context\n");
			tctx->handle = -1;
			put_device(tctx->dev);
			goto fallback;
		}
		tctx->ctx_valid = true;
	}

	return 0;
fallback:

	ctx->fb.hash_req.base = req->base;
	ahash_request_set_tfm(&ctx->fb.hash_req, tctx->fb.hash);

	return crypto_ahash_init(&ctx->fb.hash_req);
}

static int spacc_hash_final_part_pck(struct ahash_request *req)
{
	struct crypto_ahash *reqtfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(reqtfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);

	int rc;

	ctx->final_part_pck = 1;
	rc = spacc_hash_init_dma(tctx->dev, req, 1);

	if (rc < 0)
		return -ENOMEM;

	if (rc == 0) {
		ctx->small_pck = 1;
		return 0;
	}

	ctx->acb.new_handle = spacc_clone_handle(&priv->spacc, tctx->handle,
						 &ctx->acb);

	if (ctx->acb.new_handle < 0) {
		spacc_hash_cleanup_dma(tctx->dev, req);
		return -ENOMEM;
	}

	if (!ctx->small_pck) {
		spacc_partial_packet(&priv->spacc, ctx->acb.new_handle,
				     LAST_PARTIAL_PCK);
	}

	ctx->acb.tctx = tctx;
	ctx->acb.ctx = ctx;
	ctx->acb.req = req;
	ctx->acb.spacc = &priv->spacc;

	rc = spacc_packet_enqueue_ddt(&priv->spacc, ctx->acb.new_handle,
				      &ctx->src, &ctx->dst, req->nbytes,
			0, req->nbytes, 0, 0, 0);

	if (rc < 0) {
		spacc_hash_cleanup_dma(tctx->dev, req);
		spacc_close(&priv->spacc, ctx->acb.new_handle);

		if (rc != -EBUSY)
			dev_err(tctx->dev, "ERR: Failed to enqueue job: %d\n",
				rc);
		else if (!(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG))
			return -EBUSY;
	}

	return -EINPROGRESS;
}

static int spacc_hash_update(struct ahash_request *req)
{
	struct crypto_ahash *reqtfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(reqtfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);

	int nents = sg_nents(req->src);
	int nbytes = req->nbytes;
	int rc;

	if (req->src) {
		ctx->cur_part_pck =  ctx->cur_part_pck + 1;

		if (ctx->total_nents == 0 && nents > 1) {
			ppp_sgl = kmalloc(sizeof(*ppp_sgl) * 2,
					  GFP_KERNEL);
			if (!ppp_sgl)
				return -ENOMEM;

			sg_init_table(ppp_sgl, 2);
			ctx->total_nents = nents;
		}
	}

	if (!nbytes)
		return 0;

	if (tctx->handle < 0 || !tctx->ctx_valid || nbytes >
			priv->max_msg_len)
		goto fallback;

	if (ctx->total_nents && ctx->cur_part_pck == 1) {
		spacc_hash_init_sg_list(tctx->dev, req);
		return 0;
	}

	rc = spacc_hash_init_dma(tctx->dev, req, 0);

	if (rc < 0)
		goto fallback;

	if (rc == 0) {
		ctx->small_pck = 1;
		return 0;
	}

	ctx->acb.new_handle = spacc_clone_handle(&priv->spacc, tctx->handle,
						 &ctx->acb);
	if (ctx->acb.new_handle < 0) {
		spacc_hash_cleanup_dma(tctx->dev, req);
		goto fallback;
	}

	if (ctx->total_nents && !ctx->small_pck) {
		if ((ctx->cur_part_pck - 1) == 1) {
			spacc_partial_packet(&priv->spacc, ctx->acb.new_handle,
					     FIRST_PARTIAL_PCK);
		} else {
			spacc_partial_packet(&priv->spacc, ctx->acb.new_handle,
					     MIDDLE_PARTIAL_PCK);
		}
	}

	ctx->acb.tctx = tctx;
	ctx->acb.ctx = ctx;
	ctx->acb.req = req;
	ctx->acb.spacc = &priv->spacc;

	rc = spacc_packet_enqueue_ddt(&priv->spacc, ctx->acb.new_handle,
				      &ctx->src, &ctx->dst, req->nbytes,
			0, req->nbytes, 0, 0, 0);

	if (rc < 0) {
		spacc_hash_cleanup_dma(tctx->dev, req);
		spacc_close(&priv->spacc, ctx->acb.new_handle);

		if (rc != -EBUSY)
			dev_err(tctx->dev, "ERR: Failed to enqueue job: %d\n",
				rc);
		else if (!(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG))
			return -EBUSY;

		goto fallback;
	}

	return -EINPROGRESS;

fallback:
	dev_dbg(tctx->dev, "%s Using SW fallback\n", __func__);

	ctx->fb.hash_req.base.flags = req->base.flags;
	ctx->fb.hash_req.nbytes = req->nbytes;
	ctx->fb.hash_req.src = req->src;

	return crypto_ahash_update(&ctx->fb.hash_req);
}

static int spacc_hash_final(struct ahash_request *req)
{
	struct crypto_ahash *reqtfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(reqtfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	struct ahash_cb_data *acb = &ctx->acb;
	int err;

	if (tctx->handle < 0 || !tctx->ctx_valid)
		goto fallback;

	if (!ctx->digest_buf)
		return zero_message_process(req);

	if (ctx->total_nents || ctx->small_pck)
		return spacc_hash_final_part_pck(req);

	memcpy(req->result, acb->tctx->digest_ctx_buf,
	       crypto_ahash_digestsize(crypto_ahash_reqtfm(acb->req)));

	err = acb->spacc->job[acb->new_handle].job_err;
	spacc_close(acb->spacc, acb->new_handle);

	return 0;
fallback:
	ctx->fb.hash_req.base.flags = req->base.flags;
	ctx->fb.hash_req.result = req->result;

	return crypto_ahash_final(&ctx->fb.hash_req);
}

static int spacc_hash_digest(struct ahash_request *req)
{
	struct crypto_ahash *reqtfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(reqtfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);
	struct spacc_priv *priv = dev_get_drvdata(tctx->dev);
	int rc;

	if (!req->nbytes)
		return zero_message_process(req);

	if (tctx->handle < 0 || !tctx->ctx_valid || req->nbytes >
			priv->max_msg_len)
		goto fallback;

	ctx->single_shot = 1;
	rc = spacc_hash_init_dma(tctx->dev, req, 0);
	if (rc < 0)
		goto fallback;

	if (rc == 0) {
		ctx->small_pck = 1;
		return 0;
	}

	ctx->acb.new_handle = spacc_clone_handle(&priv->spacc, tctx->handle,
						 &ctx->acb);
	if (ctx->acb.new_handle < 0) {
		spacc_hash_cleanup_dma(tctx->dev, req);
		goto fallback;
	}

	ctx->acb.tctx = tctx;
	ctx->acb.ctx = ctx;
	ctx->acb.req = req;
	ctx->acb.spacc = &priv->spacc;

	rc = spacc_packet_enqueue_ddt(&priv->spacc, ctx->acb.new_handle,
				      &ctx->src, &ctx->dst, req->nbytes,
			0, req->nbytes, 0, 0, 0);
	if (rc < 0) {
		spacc_hash_cleanup_dma(tctx->dev, req);
		spacc_close(&priv->spacc, ctx->acb.new_handle);

		if (rc != -EBUSY)
			pr_debug("Failed to enqueue job, ERR: %d\n", rc);
		else if (!(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG))
			return -EBUSY;

		goto fallback;
	}

	return -EINPROGRESS;
fallback:
	/* Start from scratch as init is not called before digest. */
	ctx->fb.hash_req.base = req->base;
	ahash_request_set_tfm(&ctx->fb.hash_req, tctx->fb.hash);

	ctx->fb.hash_req.nbytes = req->nbytes;
	ctx->fb.hash_req.src = req->src;
	ctx->fb.hash_req.result = req->result;

	return crypto_ahash_digest(&ctx->fb.hash_req);
}

static int spacc_hash_finup(struct ahash_request *req)
{
	struct crypto_ahash *reqtfm = crypto_ahash_reqtfm(req);
	struct spacc_crypto_ctx *tctx = crypto_ahash_ctx(reqtfm);
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	if (tctx->handle < 0 || !tctx->ctx_valid)
		goto fallback;

	return spacc_hash_digest(req);
fallback:
	ctx->fb.hash_req.base.flags = req->base.flags;
	ctx->fb.hash_req.nbytes     = req->nbytes;
	ctx->fb.hash_req.src        = req->src;
	ctx->fb.hash_req.result     = req->result;

	return crypto_ahash_finup(&ctx->fb.hash_req);
}

static int spacc_hash_import(struct ahash_request *req, const void *in)
{
	struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	memcpy(ctx, in, sizeof(*ctx));

	return 0;
}

static int spacc_hash_export(struct ahash_request *req, void *out)
{
	const struct spacc_crypto_reqctx *ctx = ahash_request_ctx(req);

	memcpy(out, ctx, sizeof(*ctx));

	return 0;
}

const struct ahash_alg spacc_hash_template = {
	.init   = spacc_hash_init,
	.update = spacc_hash_update,
	.final  = spacc_hash_final,
	.finup  = spacc_hash_finup,
	.digest = spacc_hash_digest,
	.setkey = spacc_hash_setkey,
	.export = spacc_hash_export,
	.import = spacc_hash_import,

	.halg.base = {
		.cra_priority = 300,
		.cra_module = THIS_MODULE,
		.cra_init = spacc_hash_cra_init,
		.cra_exit = spacc_hash_cra_exit,
		.cra_ctxsize = sizeof(struct spacc_crypto_ctx),
		.cra_flags = CRYPTO_ALG_TYPE_AHASH
			| CRYPTO_ALG_ASYNC
			| CRYPTO_ALG_NEED_FALLBACK
			| CRYPTO_ALG_OPTIONAL_KEY
	},
};

static int spacc_register_hash(struct spacc_alg *salg)
{
	int rc;

	salg->calg = &salg->alg.hash.halg.base;
	salg->alg.hash = spacc_hash_template;

	spacc_init_calg(salg->calg, salg->mode);
	salg->alg.hash.halg.digestsize = salg->mode->hashlen;
	salg->alg.hash.halg.statesize = sizeof(struct spacc_crypto_reqctx);

	rc = crypto_register_ahash(&salg->alg.hash);
	if (rc < 0)
		return rc;

	mutex_lock(&spacc_hash_alg_mutex);
	list_add(&salg->list, &spacc_hash_alg_list);
	mutex_unlock(&spacc_hash_alg_mutex);

	return 0;
}

int probe_hashes(struct platform_device *spacc_pdev)
{
	struct spacc_alg *salg;
	int rc;
	int registered = 0;
	unsigned int i;
	struct spacc_priv *priv = dev_get_drvdata(&spacc_pdev->dev);

	spacc_hash_pool = dma_pool_create("spacc-digest", &spacc_pdev->dev,
					  SPACC_MAX_DIGEST_SIZE,
					  SPACC_DMA_ALIGN, SPACC_DMA_BOUNDARY);

	if (!spacc_hash_pool)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(possible_hashes); i++)
		possible_hashes[i].valid = 0;

	for (i = 0; i < ARRAY_SIZE(possible_hashes); i++) {
		if (possible_hashes[i].valid == 0 &&
		    spacc_isenabled(&priv->spacc, possible_hashes[i].id
				    & 0xFF, possible_hashes[i].hashlen)) {
			salg = kmalloc(sizeof(*salg), GFP_KERNEL);
			if (!salg)
				return -ENOMEM;

			salg->mode = &possible_hashes[i];

			/* Copy all dev's over to the salg */
			salg->dev[0] = &spacc_pdev->dev;
			salg->dev[1] = NULL;

			rc = spacc_register_hash(salg);
			if (rc < 0) {
				kfree(salg);
				continue;
			}
			dev_dbg(&spacc_pdev->dev, "registered %s\n",
				 possible_hashes[i].name);
			registered++;
			possible_hashes[i].valid = 1;
		}
	}

	return registered;
}

int spacc_unregister_hash_algs(void)
{
	struct spacc_alg *salg, *tmp;

	mutex_lock(&spacc_hash_alg_mutex);
	list_for_each_entry_safe(salg, tmp, &spacc_hash_alg_list, list) {
		crypto_unregister_alg(salg->calg);
		list_del(&salg->list);
		kfree(salg);
	}
	mutex_unlock(&spacc_hash_alg_mutex);

	dma_pool_destroy(spacc_hash_pool);

	return 0;
}
