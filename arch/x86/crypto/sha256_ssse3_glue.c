/*
 * Cryptographic API.
 *
 * Glue code for the SHA256 Secure Hash Algorithm assembler implementations
 * using SSSE3, AVX, AVX2, and SHA-NI instructions.
 *
 * This file is based on sha256_generic.c
 *
 * Copyright (C) 2013 Intel Corporation.
 *
 * Author:
 *     Tim Chen <tim.c.chen@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <crypto/internal/hash.h>
#include <crypto/internal/simd.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <crypto/sha2.h>
#include <crypto/sha256_base.h>
#include <linux/string.h>
#include <asm/cpu_device_id.h>
#include <asm/simd.h>

struct sha256_x8_mbctx {
	u32 state[8][8];
	const u8 *input[8];
};

struct sha256_reqctx {
	struct sha256_state state;
	struct crypto_hash_walk walk;
	const u8 *input;
	unsigned int total;
	unsigned int next;
};

asmlinkage void sha256_transform_ssse3(struct sha256_state *state,
				       const u8 *data, int blocks);
asmlinkage void sha256_transform_rorx(struct sha256_state *state,
				      const u8 *data, int blocks);
asmlinkage void sha256_x8_avx2(struct sha256_x8_mbctx *mbctx, int blocks);

static const struct x86_cpu_id module_cpu_ids[] = {
#ifdef CONFIG_AS_SHA256_NI
	X86_MATCH_FEATURE(X86_FEATURE_SHA_NI, NULL),
#endif
	X86_MATCH_FEATURE(X86_FEATURE_AVX2, NULL),
	X86_MATCH_FEATURE(X86_FEATURE_AVX, NULL),
	X86_MATCH_FEATURE(X86_FEATURE_SSSE3, NULL),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, module_cpu_ids);

static int sha256_import(struct ahash_request *req, const void *in)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);

	memcpy(&rctx->state, in, sizeof(rctx->state));
	return 0;
}

static int sha256_export(struct ahash_request *req, void *out)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);

	memcpy(out, &rctx->state, sizeof(rctx->state));
	return 0;
}

static int sha256_ahash_init(struct ahash_request *req)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct ahash_request *r2;

	sha256_init(&rctx->state);

	if (!ahash_request_chained(req))
		return 0;

	req->base.err = 0;
	list_for_each_entry(r2, &req->base.list, base.list) {
		r2->base.err = 0;
		rctx = ahash_request_ctx(r2);
		sha256_init(&rctx->state);
	}

	return 0;
}

static int sha224_ahash_init(struct ahash_request *req)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct ahash_request *r2;

	sha224_init(&rctx->state);

	if (!ahash_request_chained(req))
		return 0;

	req->base.err = 0;
	list_for_each_entry(r2, &req->base.list, base.list) {
		rctx = ahash_request_ctx(r2);
		sha224_init(&rctx->state);
	}

	return 0;
}

static void __sha256_update(struct sha256_state *sctx, const u8 *data,
			   unsigned int len, sha256_block_fn *sha256_xform)
{
	if (!crypto_simd_usable() ||
	    (sctx->count % SHA256_BLOCK_SIZE) + len < SHA256_BLOCK_SIZE) {
		sha256_update(sctx, data, len);
		return;
	}

	/*
	 * Make sure struct sha256_state begins directly with the SHA256
	 * 256-bit internal state, as this is what the asm functions expect.
	 */
	BUILD_BUG_ON(offsetof(struct sha256_state, state) != 0);

	kernel_fpu_begin();
	lib_sha256_base_do_update(sctx, data, len, sha256_xform);
	kernel_fpu_end();
}

static int _sha256_update(struct shash_desc *desc, const u8 *data,
			  unsigned int len, sha256_block_fn *sha256_xform)
{
	__sha256_update(shash_desc_ctx(desc), data, len, sha256_xform);
	return 0;
}

static int sha256_ahash_update(struct ahash_request *req,
			       sha256_block_fn *sha256_xform)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct crypto_hash_walk *walk = &rctx->walk;
	struct sha256_state *state = &rctx->state;
	int nbytes;

	/*
	 * Make sure struct sha256_state begins directly with the SHA256
	 * 256-bit internal state, as this is what the asm functions expect.
	 */
	BUILD_BUG_ON(offsetof(struct sha256_state, state) != 0);

	for (nbytes = crypto_hash_walk_first(req, walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(walk, 0))
		__sha256_update(state, walk->data, nbytes, sha256_transform_rorx);

	return nbytes;
}

static void _sha256_finup(struct sha256_state *state, const u8 *data,
			  unsigned int len, u8 *out, unsigned int ds,
			  sha256_block_fn *sha256_xform)
{
	if (!crypto_simd_usable()) {
		sha256_update(state, data, len);
		if (ds == SHA224_DIGEST_SIZE)
			sha224_final(state, out);
		else
			sha256_final(state, out);
		return;
	}

	kernel_fpu_begin();
	if (len)
		lib_sha256_base_do_update(state, data, len, sha256_xform);
	lib_sha256_base_do_finalize(state, sha256_xform);
	kernel_fpu_end();

	lib_sha256_base_finish(state, out, ds);
}

static int sha256_ahash_finup(struct ahash_request *req,
			      sha256_block_fn *sha256_xform)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct crypto_hash_walk *walk = &rctx->walk;
	struct sha256_state *state = &rctx->state;
	unsigned int ds;
	int nbytes;

	ds = crypto_ahash_digestsize(crypto_ahash_reqtfm(req));
	if (!req->nbytes) {
		_sha256_finup(state, NULL, 0, req->result,
			      ds, sha256_transform_rorx);
		return 0;
	}

	for (nbytes = crypto_hash_walk_first(req, walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(walk, 0)) {
		if (crypto_hash_walk_last(walk)) {
			_sha256_finup(state, walk->data, nbytes, req->result,
				      ds, sha256_transform_rorx);
			continue;
		}

		__sha256_update(state, walk->data, nbytes, sha256_transform_rorx);
	}

	return nbytes;
}

static int sha256_finup(struct shash_desc *desc, const u8 *data,
	      unsigned int len, u8 *out, sha256_block_fn *sha256_xform)
{
	unsigned int ds = crypto_shash_digestsize(desc->tfm);

	_sha256_finup(shash_desc_ctx(desc), data, len, out, ds, sha256_xform);
	return 0;
}

static int sha256_ssse3_update(struct shash_desc *desc, const u8 *data,
			 unsigned int len)
{
	return _sha256_update(desc, data, len, sha256_transform_ssse3);
}

static int sha256_ssse3_finup(struct shash_desc *desc, const u8 *data,
	      unsigned int len, u8 *out)
{
	return sha256_finup(desc, data, len, out, sha256_transform_ssse3);
}

/* Add padding and return the message digest. */
static int sha256_ssse3_final(struct shash_desc *desc, u8 *out)
{
	return sha256_ssse3_finup(desc, NULL, 0, out);
}

static int sha256_ssse3_digest(struct shash_desc *desc, const u8 *data,
	      unsigned int len, u8 *out)
{
	return sha256_base_init(desc) ?:
	       sha256_ssse3_finup(desc, data, len, out);
}

static struct shash_alg sha256_ssse3_algs[] = { {
	.digestsize	=	SHA256_DIGEST_SIZE,
	.init		=	sha256_base_init,
	.update		=	sha256_ssse3_update,
	.final		=	sha256_ssse3_final,
	.finup		=	sha256_ssse3_finup,
	.digest		=	sha256_ssse3_digest,
	.descsize	=	sizeof(struct sha256_state),
	.base		=	{
		.cra_name	=	"sha256",
		.cra_driver_name =	"sha256-ssse3",
		.cra_priority	=	150,
		.cra_blocksize	=	SHA256_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
	}
}, {
	.digestsize	=	SHA224_DIGEST_SIZE,
	.init		=	sha224_base_init,
	.update		=	sha256_ssse3_update,
	.final		=	sha256_ssse3_final,
	.finup		=	sha256_ssse3_finup,
	.descsize	=	sizeof(struct sha256_state),
	.base		=	{
		.cra_name	=	"sha224",
		.cra_driver_name =	"sha224-ssse3",
		.cra_priority	=	150,
		.cra_blocksize	=	SHA224_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
	}
} };

static int register_sha256_ssse3(void)
{
	if (boot_cpu_has(X86_FEATURE_SSSE3))
		return crypto_register_shashes(sha256_ssse3_algs,
				ARRAY_SIZE(sha256_ssse3_algs));
	return 0;
}

static void unregister_sha256_ssse3(void)
{
	if (boot_cpu_has(X86_FEATURE_SSSE3))
		crypto_unregister_shashes(sha256_ssse3_algs,
				ARRAY_SIZE(sha256_ssse3_algs));
}

asmlinkage void sha256_transform_avx(struct sha256_state *state,
				     const u8 *data, int blocks);

static int sha256_avx_update(struct shash_desc *desc, const u8 *data,
			 unsigned int len)
{
	return _sha256_update(desc, data, len, sha256_transform_avx);
}

static int sha256_avx_finup(struct shash_desc *desc, const u8 *data,
		      unsigned int len, u8 *out)
{
	return sha256_finup(desc, data, len, out, sha256_transform_avx);
}

static int sha256_avx_final(struct shash_desc *desc, u8 *out)
{
	return sha256_avx_finup(desc, NULL, 0, out);
}

static int sha256_avx_digest(struct shash_desc *desc, const u8 *data,
		      unsigned int len, u8 *out)
{
	return sha256_base_init(desc) ?:
	       sha256_avx_finup(desc, data, len, out);
}

static struct shash_alg sha256_avx_algs[] = { {
	.digestsize	=	SHA256_DIGEST_SIZE,
	.init		=	sha256_base_init,
	.update		=	sha256_avx_update,
	.final		=	sha256_avx_final,
	.finup		=	sha256_avx_finup,
	.digest		=	sha256_avx_digest,
	.descsize	=	sizeof(struct sha256_state),
	.base		=	{
		.cra_name	=	"sha256",
		.cra_driver_name =	"sha256-avx",
		.cra_priority	=	160,
		.cra_blocksize	=	SHA256_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
	}
}, {
	.digestsize	=	SHA224_DIGEST_SIZE,
	.init		=	sha224_base_init,
	.update		=	sha256_avx_update,
	.final		=	sha256_avx_final,
	.finup		=	sha256_avx_finup,
	.descsize	=	sizeof(struct sha256_state),
	.base		=	{
		.cra_name	=	"sha224",
		.cra_driver_name =	"sha224-avx",
		.cra_priority	=	160,
		.cra_blocksize	=	SHA224_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
	}
} };

static bool avx_usable(void)
{
	if (!cpu_has_xfeatures(XFEATURE_MASK_SSE | XFEATURE_MASK_YMM, NULL)) {
		if (boot_cpu_has(X86_FEATURE_AVX))
			pr_info("AVX detected but unusable.\n");
		return false;
	}

	return true;
}

static int register_sha256_avx(void)
{
	if (avx_usable())
		return crypto_register_shashes(sha256_avx_algs,
				ARRAY_SIZE(sha256_avx_algs));
	return 0;
}

static void unregister_sha256_avx(void)
{
	if (avx_usable())
		crypto_unregister_shashes(sha256_avx_algs,
				ARRAY_SIZE(sha256_avx_algs));
}

static int sha256_pad2(struct ahash_request *req)
{
	const int bit_offset = SHA256_BLOCK_SIZE - sizeof(__be64);
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct sha256_state *state = &rctx->state;
	unsigned int partial = state->count;
	__be64 *bits;

	if (rctx->total)
		return 0;

	rctx->total = 1;

	partial %= SHA256_BLOCK_SIZE;
	memset(state->buf + partial, 0, bit_offset - partial);
	bits = (__be64 *)(state->buf + bit_offset);
	*bits = cpu_to_be64(state->count << 3);

	return SHA256_BLOCK_SIZE;
}

static int sha256_pad1(struct ahash_request *req, bool final)
{
	const int bit_offset = SHA256_BLOCK_SIZE - sizeof(__be64);
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct sha256_state *state = &rctx->state;
	unsigned int partial = state->count;

	if (!final)
		return 0;

	rctx->total = 0;
	rctx->input = state->buf;

	partial %= SHA256_BLOCK_SIZE;
	state->buf[partial++] = 0x80;

	if (partial > bit_offset) {
		memset(state->buf + partial, 0, SHA256_BLOCK_SIZE - partial);
		return SHA256_BLOCK_SIZE;
	}

	return sha256_pad2(req);
}

static int sha256_mb_start(struct ahash_request *req, bool final)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct sha256_state *state = &rctx->state;
	unsigned int partial;
	int nbytes;

	nbytes = crypto_hash_walk_first(req, &rctx->walk);
	if (!nbytes)
		return sha256_pad1(req, final);

	rctx->input = rctx->walk.data;

	partial = state->count % SHA256_BLOCK_SIZE;
	while (partial + nbytes < SHA256_BLOCK_SIZE) {
		memcpy(state->buf + partial, rctx->input, nbytes);
		state->count += nbytes;
		partial += nbytes;

		nbytes = crypto_hash_walk_done(&rctx->walk, 0);
		if (!nbytes)
			return sha256_pad1(req, final);

		rctx->input = rctx->walk.data;
	}

	rctx->total = nbytes;
	if (nbytes == 1) {
		rctx->total = 0;
		state->count++;
	}

	if (partial) {
		unsigned int offset = SHA256_BLOCK_SIZE - partial;

		memcpy(state->buf + partial, rctx->input, offset);
		rctx->input = state->buf;

		return SHA256_BLOCK_SIZE;
	}

	return nbytes;
}

static int sha256_mb_next(struct ahash_request *req, unsigned int len,
			  bool final)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct sha256_state *state = &rctx->state;
	unsigned int partial;

	if (rctx->input != state->buf) {
		rctx->input += len;
		rctx->total -= len;
		state->count += len;
	} else if (rctx->total > 1) {
		unsigned int offset;

		offset = SHA256_BLOCK_SIZE - state->count % SHA256_BLOCK_SIZE;
		rctx->input = rctx->walk.data + offset;
		rctx->total -= offset;
		state->count += offset;
	} else
		return sha256_pad2(req);

	partial = 0;
	while (partial + rctx->total < SHA256_BLOCK_SIZE) {
		memcpy(state->buf + partial, rctx->input, rctx->total);
		state->count += rctx->total;
		partial += rctx->total;

		rctx->total = crypto_hash_walk_done(&rctx->walk, 0);
		if (!rctx->total)
			return sha256_pad1(req, final);

		rctx->input = rctx->walk.data;
	}

	return rctx->total;
}

static void sha256_update_x8x1(struct list_head *list,
			       struct ahash_request *reqs[8], bool final)
{
	struct sha256_x8_mbctx mbctx;
	unsigned int len = 0;
	u32 *states[8];
	int i = 0;

	do {
		struct sha256_reqctx *rctx = ahash_request_ctx(reqs[i]);
		unsigned int nbytes;

		nbytes = rctx->next;
		if (!i || nbytes < len)
			len = nbytes;

		states[i] = rctx->state.state;
		mbctx.input[i] = rctx->input;
	} while (++i < 8 && reqs[i]);

	for (; i < 8; i++) {
		mbctx.input[i] = mbctx.input[0];
	}

	for (i = 0; i < 8; i++) {
		int j;

		for (j = 0; j < 8; j++)
			mbctx.state[i][j] = states[j][i];
	}

	sha256_x8_avx2(&mbctx, len / SHA256_BLOCK_SIZE);

	for (i = 0; i < 8; i++) {
		int j;

		for (j = 0; j < 8; j++)
			states[i][j] = mbctx.state[j][i];
	}

	i = 0;
	do {
		struct sha256_reqctx *rctx = ahash_request_ctx(reqs[i]);

		rctx->next = sha256_mb_next(reqs[i], len, final);

		if (rctx->next) {
			if (++i >= 8)
				break;
			continue;
		}

		if (i == 7 || !reqs[i + 1]) {
			struct ahash_request *r2 = reqs[i];

			reqs[i] = NULL;
			do {
				while (!list_is_last(&r2->base.list, list)) {
					r2 = list_next_entry(r2, base.list);
					r2->base.err = 0;

					rctx = ahash_request_ctx(r2);
					rctx->next = sha256_mb_start(r2, final);
					if (rctx->next) {
						reqs[i] = r2;
						break;
					}
				}
			} while (reqs[i] && ++i < 8);

			break;
		}

		memmove(reqs + i, reqs + i + 1, sizeof(reqs[0]) * (7 - i));
		reqs[7] = NULL;
	} while (reqs[i]);
}

static void sha256_update_x8(struct list_head *list,
			     struct ahash_request *reqs[8],
			     bool final)
{
	do {
		sha256_update_x8x1(list, reqs, final);
	} while (reqs[0]);
}

static void sha256_chain(struct ahash_request *req, bool final)
{
	struct sha256_reqctx *rctx = ahash_request_ctx(req);
	struct ahash_request *reqs[8];
	struct ahash_request *r2;
	int i;

	req->base.err = 0;
	reqs[0] = req;
	rctx->next = sha256_mb_start(req, final);
	i = !!rctx->next;
	list_for_each_entry(r2, &req->base.list, base.list) {
		r2->base.err = 0;

		rctx = ahash_request_ctx(r2);
		rctx->next = sha256_mb_start(r2, final);
		if (!rctx->next)
			continue;

		reqs[i++] = r2;
		if (i < 8)
			continue;

		sha256_update_x8(&req->base.list, reqs, final);
		i = 0;
	}

	if (i) {
		memset(reqs + i, 0, sizeof(reqs) * (8 - i));
		sha256_update_x8(&req->base.list, reqs, final);
	}

	return;
}

static int sha256_avx2_update(struct ahash_request *req)
{
	struct ahash_request *r2;
	int err;

	if (ahash_request_chained(req) && crypto_simd_usable()) {
		sha256_chain(req, false);
		return 0;
	}

	err = sha256_ahash_update(req, sha256_transform_rorx);
	if (!ahash_request_chained(req))
		return err;

	req->base.err = err;

	list_for_each_entry(r2, &req->base.list, base.list) {
		err = sha256_ahash_update(r2, sha256_transform_rorx);
		r2->base.err = err;
	}

	return 0;
}

static int sha256_avx2_finup(struct ahash_request *req)
{
	struct ahash_request *r2;
	int err;

	if (ahash_request_chained(req) && crypto_simd_usable()) {
		sha256_chain(req, true);
		return 0;
	}

	err = sha256_ahash_finup(req, sha256_transform_rorx);
	if (!ahash_request_chained(req))
		return err;

	req->base.err = err;

	list_for_each_entry(r2, &req->base.list, base.list) {
		err = sha256_ahash_finup(r2, sha256_transform_rorx);
		r2->base.err = err;
	}

	return 0;
}

static int sha256_avx2_final(struct ahash_request *req)
{
	req->nbytes = 0;
	return sha256_avx2_finup(req);
}

static int sha256_avx2_digest(struct ahash_request *req)
{
	return sha256_ahash_init(req) ?:
	       sha256_avx2_finup(req);
}

static int sha224_avx2_digest(struct ahash_request *req)
{
	return sha224_ahash_init(req) ?:
	       sha256_avx2_finup(req);
}

static struct ahash_alg sha256_avx2_algs[] = { {
	.halg.digestsize =	SHA256_DIGEST_SIZE,
	.halg.statesize	=	sizeof(struct sha256_state),
	.reqsize	=	sizeof(struct sha256_reqctx),
	.init		=	sha256_ahash_init,
	.update		=	sha256_avx2_update,
	.final		=	sha256_avx2_final,
	.finup		=	sha256_avx2_finup,
	.digest		=	sha256_avx2_digest,
	.import		=	sha256_import,
	.export		=	sha256_export,
	.halg.base	=	{
		.cra_name	=	"sha256",
		.cra_driver_name =	"sha256-avx2",
		.cra_priority	=	170,
		.cra_blocksize	=	SHA256_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
		.cra_flags	=	CRYPTO_ALG_REQ_CHAIN,
	}
}, {
	.halg.digestsize =	SHA224_DIGEST_SIZE,
	.halg.statesize	=	sizeof(struct sha256_state),
	.reqsize	=	sizeof(struct sha256_reqctx),
	.init		=	sha224_ahash_init,
	.update		=	sha256_avx2_update,
	.final		=	sha256_avx2_final,
	.finup		=	sha256_avx2_finup,
	.digest		=	sha224_avx2_digest,
	.import		=	sha256_import,
	.export		=	sha256_export,
	.halg.base	=	{
		.cra_name	=	"sha224",
		.cra_driver_name =	"sha224-avx2",
		.cra_priority	=	170,
		.cra_blocksize	=	SHA224_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
		.cra_flags	=	CRYPTO_ALG_REQ_CHAIN,
	}
} };

static bool avx2_usable(void)
{
	if (avx_usable() && boot_cpu_has(X86_FEATURE_AVX2) &&
		    boot_cpu_has(X86_FEATURE_BMI2))
		return true;

	return false;
}

static int register_sha256_avx2(void)
{
	if (avx2_usable())
		return crypto_register_ahashes(sha256_avx2_algs,
				ARRAY_SIZE(sha256_avx2_algs));
	return 0;
}

static void unregister_sha256_avx2(void)
{
	if (avx2_usable())
		crypto_unregister_ahashes(sha256_avx2_algs,
				ARRAY_SIZE(sha256_avx2_algs));
}

#ifdef CONFIG_AS_SHA256_NI
asmlinkage void sha256_ni_transform(struct sha256_state *digest,
				    const u8 *data, int rounds);

static int sha256_ni_update(struct shash_desc *desc, const u8 *data,
			 unsigned int len)
{
	return _sha256_update(desc, data, len, sha256_ni_transform);
}

static int sha256_ni_finup(struct shash_desc *desc, const u8 *data,
		      unsigned int len, u8 *out)
{
	return sha256_finup(desc, data, len, out, sha256_ni_transform);
}

static int sha256_ni_final(struct shash_desc *desc, u8 *out)
{
	return sha256_ni_finup(desc, NULL, 0, out);
}

static int sha256_ni_digest(struct shash_desc *desc, const u8 *data,
		      unsigned int len, u8 *out)
{
	return sha256_base_init(desc) ?:
	       sha256_ni_finup(desc, data, len, out);
}

static struct shash_alg sha256_ni_algs[] = { {
	.digestsize	=	SHA256_DIGEST_SIZE,
	.init		=	sha256_base_init,
	.update		=	sha256_ni_update,
	.final		=	sha256_ni_final,
	.finup		=	sha256_ni_finup,
	.digest		=	sha256_ni_digest,
	.descsize	=	sizeof(struct sha256_state),
	.base		=	{
		.cra_name	=	"sha256",
		.cra_driver_name =	"sha256-ni",
		.cra_priority	=	250,
		.cra_blocksize	=	SHA256_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
	}
}, {
	.digestsize	=	SHA224_DIGEST_SIZE,
	.init		=	sha224_base_init,
	.update		=	sha256_ni_update,
	.final		=	sha256_ni_final,
	.finup		=	sha256_ni_finup,
	.descsize	=	sizeof(struct sha256_state),
	.base		=	{
		.cra_name	=	"sha224",
		.cra_driver_name =	"sha224-ni",
		.cra_priority	=	250,
		.cra_blocksize	=	SHA224_BLOCK_SIZE,
		.cra_module	=	THIS_MODULE,
	}
} };

static int register_sha256_ni(void)
{
	if (boot_cpu_has(X86_FEATURE_SHA_NI))
		return crypto_register_shashes(sha256_ni_algs,
				ARRAY_SIZE(sha256_ni_algs));
	return 0;
}

static void unregister_sha256_ni(void)
{
	if (boot_cpu_has(X86_FEATURE_SHA_NI))
		crypto_unregister_shashes(sha256_ni_algs,
				ARRAY_SIZE(sha256_ni_algs));
}

#else
static inline int register_sha256_ni(void) { return 0; }
static inline void unregister_sha256_ni(void) { }
#endif

static int __init sha256_ssse3_mod_init(void)
{
	if (!x86_match_cpu(module_cpu_ids))
		return -ENODEV;

	if (register_sha256_ssse3())
		goto fail;

	if (register_sha256_avx()) {
		unregister_sha256_ssse3();
		goto fail;
	}

	if (register_sha256_avx2()) {
		unregister_sha256_avx();
		unregister_sha256_ssse3();
		goto fail;
	}

	if (register_sha256_ni()) {
		unregister_sha256_avx2();
		unregister_sha256_avx();
		unregister_sha256_ssse3();
		goto fail;
	}

	return 0;
fail:
	return -ENODEV;
}

static void __exit sha256_ssse3_mod_fini(void)
{
	unregister_sha256_ni();
	unregister_sha256_avx2();
	unregister_sha256_avx();
	unregister_sha256_ssse3();
}

module_init(sha256_ssse3_mod_init);
module_exit(sha256_ssse3_mod_fini);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SHA256 Secure Hash Algorithm, Supplemental SSE3 accelerated");

MODULE_ALIAS_CRYPTO("sha256");
MODULE_ALIAS_CRYPTO("sha256-ssse3");
MODULE_ALIAS_CRYPTO("sha256-avx");
MODULE_ALIAS_CRYPTO("sha256-avx2");
MODULE_ALIAS_CRYPTO("sha224");
MODULE_ALIAS_CRYPTO("sha224-ssse3");
MODULE_ALIAS_CRYPTO("sha224-avx");
MODULE_ALIAS_CRYPTO("sha224-avx2");
#ifdef CONFIG_AS_SHA256_NI
MODULE_ALIAS_CRYPTO("sha256-ni");
MODULE_ALIAS_CRYPTO("sha224-ni");
#endif
