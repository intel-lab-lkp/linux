// SPDX-License-Identifier: GPL-2.0

#include <crypto/aes.h>
#include <crypto/akcipher.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>

/*
 * AES-128 single-block ECB encryption: out = AES(key, in).
 *
 * A helper because aes_encrypt() takes a transparent union (aes_encrypt_arg)
 * that bindgen cannot express. SHA-256 and HMAC-SHA256 are plain extern
 * functions and are bound directly.
 */
__rust_helper int
rust_helper_aes128_encrypt_block(const u8 *key, const u8 *in, u8 *out)
{
	struct aes_enckey enckey;
	int ret;

	ret = aes_prepareenckey(&enckey, key, AES_KEYSIZE_128);
	if (ret)
		return ret;
	aes_encrypt(&enckey, out, in);
	memzero_explicit(&enckey, sizeof(enckey));
	return 0;
}

/*
 * crypto_free_akcipher() and crypto_akcipher_set_pub_key() are static inlines,
 * so they are exposed to Rust through these 1:1 shims (the same convention as
 * the other rust_helper_ wrappers).
 */
__rust_helper void
rust_helper_crypto_free_akcipher(struct crypto_akcipher *tfm)
{
	crypto_free_akcipher(tfm);
}

__rust_helper int
rust_helper_crypto_akcipher_set_pub_key(struct crypto_akcipher *tfm,
					const void *key, unsigned int keylen)
{
	return crypto_akcipher_set_pub_key(tfm, key, keylen);
}

/*
 * One-shot synchronous public-key encrypt over the akcipher API: encrypts
 * @src_len bytes at @src into @dst (capacity @dst_len) and returns the
 * ciphertext length or -errno.
 *
 * The request object, the scatterlists and the completion wait are all
 * static-inline or on-stack state that cannot be expressed from Rust, so the
 * canonical synchronous akcipher sequence lives here. The akcipher data path
 * also needs DMA-capable (kmalloc, not vmap-stack) buffers, so @src/@dst are
 * bounced through kmalloc'd copies; @dst is overwritten only on success and
 * the bounce buffers are wiped on free.
 */
__rust_helper int
rust_helper_akcipher_encrypt_oneshot(struct crypto_akcipher *tfm,
				     const u8 *src, unsigned int src_len,
				     u8 *dst, unsigned int dst_len)
{
	struct akcipher_request *req;
	struct scatterlist src_sg, dst_sg;
	DECLARE_CRYPTO_WAIT(wait);
	u8 *in, *out;
	int ret = -ENOMEM;

	req = akcipher_request_alloc(tfm, GFP_KERNEL);
	in = kmemdup(src, src_len, GFP_KERNEL);
	out = kzalloc(dst_len, GFP_KERNEL);
	if (!req || !in || !out)
		goto out;

	sg_init_one(&src_sg, in, src_len);
	sg_init_one(&dst_sg, out, dst_len);
	akcipher_request_set_crypt(req, &src_sg, &dst_sg, src_len, dst_len);
	akcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      crypto_req_done, &wait);

	ret = crypto_wait_req(crypto_akcipher_encrypt(req), &wait);
	if (ret == 0) {
		memcpy(dst, out, dst_len);
		ret = req->dst_len;
	}
out:
	kfree_sensitive(in);
	kfree_sensitive(out);
	if (req)
		akcipher_request_free(req);
	return ret;
}
