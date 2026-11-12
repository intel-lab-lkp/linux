// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Crypto API support for AEGIS-128X4, using the library
 *
 * Copyright 2026 Antonio Quartulli <antonio@mandelbit.com>
 */

#include <crypto/aegis128x4.h>
#include <crypto/internal/aead.h>
#include <crypto/scatterwalk.h>
#include <linux/module.h>

static_assert(__alignof__(struct aegis128x4_key) <= CRYPTO_MINALIGN);

static int crypto_aegis128x4_setkey(struct crypto_aead *tfm, const u8 *in_key,
				    unsigned int key_len)
{
	struct aegis128x4_key *key = crypto_aead_ctx(tfm);

	return aegis128x4_preparekey(key, in_key, key_len,
				     crypto_aead_authsize(tfm));
}

static int crypto_aegis128x4_setauthsize(struct crypto_aead *tfm,
					 unsigned int authsize)
{
	struct aegis128x4_key *key = crypto_aead_ctx(tfm);

	if (authsize != 16 && authsize != 32)
		return -EINVAL;
	/* Synchronize the tag length to the struct aegis128x4_key. */
	key->authtag_len = authsize;
	return 0;
}

static void crypto_aegis128x4_process_ad(struct aegis128x4_ctx *ctx,
					 struct scatterlist *src,
					 unsigned int assoclen)
{
	struct scatter_walk walk;

	scatterwalk_start(&walk, src);
	while (assoclen) {
		unsigned int n = scatterwalk_next(&walk, assoclen);

		aegis128x4_auth_update(ctx, walk.addr, n);
		scatterwalk_done_src(&walk, n);
		assoclen -= n;
	}
}

/*
 * En/decrypt @len bytes from @src to @dst, starting @pos bytes into both.  The
 * incremental functions accept any length, so each piece is passed directly.
 * The scatterlists must describe either entirely different memory or entirely
 * the same memory.
 */
static void crypto_aegis128x4_crypt(struct aegis128x4_ctx *ctx,
				    struct scatterlist *dst,
				    struct scatterlist *src, unsigned int pos,
				    unsigned int len, bool enc)
{
	struct scatter_walk dst_walk, src_walk;

	if (!len)
		return;

	scatterwalk_start_at_pos(&dst_walk, dst, pos);
	scatterwalk_start_at_pos(&src_walk, src, pos);
	do {
		unsigned int n = min(scatterwalk_clamp(&dst_walk, len),
				     scatterwalk_clamp(&src_walk, len));
		const u8 *src_virt;
		u8 *dst_virt;

		scatterwalk_map(&dst_walk);
		dst_virt = dst_walk.addr;
		/* map a highmem page only once when in-place */
		if (IS_ENABLED(CONFIG_HIGHMEM) &&
		    offset_in_page(src_walk.offset) ==
			    offset_in_page(dst_walk.offset) &&
		    sg_page(src_walk.sg) + src_walk.offset / PAGE_SIZE ==
			    sg_page(dst_walk.sg) + dst_walk.offset / PAGE_SIZE) {
			src_virt = dst_virt;
		} else {
			scatterwalk_map(&src_walk);
			src_virt = src_walk.addr;
		}
		if (enc)
			aegis128x4_encrypt_update(ctx, dst_virt, src_virt, n);
		else
			aegis128x4_decrypt_update(ctx, dst_virt, src_virt, n);
		if (src_virt != dst_virt)
			scatterwalk_unmap(&src_walk);
		scatterwalk_advance(&src_walk, n);
		scatterwalk_done_dst(&dst_walk, n);
		len -= n;
	} while (len);
}

static int crypto_aegis128x4_encrypt(struct aead_request *req)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	const struct aegis128x4_key *key = crypto_aead_ctx(tfm);
	u8 authtag[AEGIS128X4_MAX_AUTHTAG_SIZE];
	struct aegis128x4_ctx ctx;

	aegis128x4_init(&ctx, req->iv, key);
	crypto_aegis128x4_process_ad(&ctx, req->src, req->assoclen);
	crypto_aegis128x4_crypt(&ctx, req->dst, req->src, req->assoclen,
				req->cryptlen, true);
	aegis128x4_encrypt_final(&ctx, authtag);
	memcpy_to_sglist(req->dst, req->assoclen + req->cryptlen, authtag,
			 key->authtag_len);
	memzero_explicit(authtag, sizeof(authtag));
	return 0;
}

static int crypto_aegis128x4_decrypt(struct aead_request *req)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	const struct aegis128x4_key *key = crypto_aead_ctx(tfm);
	u8 authtag[AEGIS128X4_MAX_AUTHTAG_SIZE];
	struct aegis128x4_ctx ctx;
	unsigned int data_len;
	int err;

	/* crypto_aead_decrypt() already checked cryptlen >= authtag_len. */
	data_len = req->cryptlen - key->authtag_len;

	aegis128x4_init(&ctx, req->iv, key);
	crypto_aegis128x4_process_ad(&ctx, req->src, req->assoclen);
	crypto_aegis128x4_crypt(&ctx, req->dst, req->src, req->assoclen,
				data_len, false);
	memcpy_from_sglist(authtag, req->src, req->assoclen + data_len,
			   key->authtag_len);
	err = aegis128x4_decrypt_final(&ctx, authtag);
	memzero_explicit(authtag, sizeof(authtag));
	return err;
}

static struct aead_alg crypto_aegis128x4_alg = {
	.base.cra_name = "aegis128x4",
	.base.cra_driver_name = "aegis128x4-lib",
	.base.cra_priority = 300,
	.base.cra_blocksize = 1,
	.base.cra_ctxsize = sizeof(struct aegis128x4_key),
	.base.cra_module = THIS_MODULE,
	.setkey = crypto_aegis128x4_setkey,
	.setauthsize = crypto_aegis128x4_setauthsize,
	.encrypt = crypto_aegis128x4_encrypt,
	.decrypt = crypto_aegis128x4_decrypt,
	.ivsize = AEGIS128X4_NONCE_SIZE,
	.maxauthsize = AEGIS128X4_MAX_AUTHTAG_SIZE,
	.chunksize = AEGIS128X4_BLOCK_SIZE,
};

static int __init crypto_aegis128x4_mod_init(void)
{
	return crypto_register_aead(&crypto_aegis128x4_alg);
}
module_init(crypto_aegis128x4_mod_init);

static void __exit crypto_aegis128x4_mod_exit(void)
{
	crypto_unregister_aead(&crypto_aegis128x4_alg);
}
module_exit(crypto_aegis128x4_mod_exit);

MODULE_DESCRIPTION("AEGIS-128X4 AEAD algorithm");
MODULE_LICENSE("GPL");
MODULE_ALIAS_CRYPTO("aegis128x4");
MODULE_ALIAS_CRYPTO("aegis128x4-lib");
