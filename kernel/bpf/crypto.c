// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2023 Meta, Inc */
#include <linux/bpf.h>
#include <linux/bpf_mem_alloc.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/filter.h>
#include <linux/scatterlist.h>
#include <linux/skbuff.h>
#include <crypto/skcipher.h>

/**
 * struct bpf_crypto_skcipher_ctx - refcounted BPF sync skcipher context structure
 * @tfm:	The pointer to crypto_sync_skcipher struct.
 * @rcu:	The RCU head used to free the crypto context with RCU safety.
 * @usage:	Object reference counter. When the refcount goes to 0, the
 *		memory is released back to the BPF allocator, which provides
 *		RCU safety.
 */
struct bpf_crypto_skcipher_ctx {
	struct crypto_sync_skcipher *tfm;
	struct rcu_head rcu;
	refcount_t usage;
};

__bpf_kfunc_start_defs();

/**
 * bpf_crypto_skcipher_ctx_create() - Create a mutable BPF crypto context.
 *
 * Allocates a crypto context that can be used, acquired, and released by
 * a BPF program. The crypto context returned by this function must either
 * be embedded in a map as a kptr, or freed with bpf_crypto_skcipher_ctx_release().
 *
 * bpf_crypto_skcipher_ctx_create() allocates memory using the BPF memory
 * allocator, and will not block. It may return NULL if no memory is available.
 * @algo__str: pointer to string representation of algorithm.
 * @pkey:      bpf_dynptr which holds cipher key to do crypto.
 * @err:       integer to store error code when NULL is returned
 */
__bpf_kfunc struct bpf_crypto_skcipher_ctx *
bpf_crypto_skcipher_ctx_create(const char *algo__str, const struct bpf_dynptr_kern *pkey,
			       int *err)
{
	struct bpf_crypto_skcipher_ctx *ctx;
	const u8 *key;
	u32 key_len;

	if (!crypto_has_skcipher(algo__str, CRYPTO_ALG_TYPE_SKCIPHER, CRYPTO_ALG_TYPE_MASK)) {
		*err = -EOPNOTSUPP;
		return NULL;
	}

	key_len = __bpf_dynptr_size(pkey);
	if (!key_len) {
		*err = -EINVAL;
		return NULL;
	}
	key = __bpf_dynptr_data(pkey, key_len);
	if (!key) {
		*err = -EINVAL;
		return NULL;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		*err = -ENOMEM;
		return NULL;
	}

	ctx->tfm = crypto_alloc_sync_skcipher(algo__str, 0, 0);
	if (IS_ERR(ctx->tfm)) {
		*err = PTR_ERR(ctx->tfm);
		ctx->tfm = NULL;
		goto err;
	}

	*err = crypto_sync_skcipher_setkey(ctx->tfm, key, key_len);
	if (*err)
		goto err;

	refcount_set(&ctx->usage, 1);

	return ctx;
err:
	if (ctx->tfm)
		crypto_free_sync_skcipher(ctx->tfm);
	kfree(ctx);

	return NULL;
}

static void crypto_free_sync_skcipher_cb(struct rcu_head *head)
{
	struct bpf_crypto_skcipher_ctx *ctx;

	ctx = container_of(head, struct bpf_crypto_skcipher_ctx, rcu);
	crypto_free_sync_skcipher(ctx->tfm);
	kfree(ctx);
}

/**
 * bpf_crypto_skcipher_ctx_acquire() - Acquire a reference to a BPF crypto context.
 * @ctx: The BPF crypto context being acquired. The ctx must be a trusted
 *	     pointer.
 *
 * Acquires a reference to a BPF crypto context. The context returned by this function
 * must either be embedded in a map as a kptr, or freed with
 * bpf_crypto_skcipher_ctx_release().
 */
__bpf_kfunc struct bpf_crypto_skcipher_ctx *
bpf_crypto_skcipher_ctx_acquire(struct bpf_crypto_skcipher_ctx *ctx)
{
	refcount_inc(&ctx->usage);
	return ctx;
}

/**
 * bpf_crypto_skcipher_ctx_release() - Release a previously acquired BPF crypto context.
 * @ctx: The crypto context being released.
 *
 * Releases a previously acquired reference to a BPF cpumask. When the final
 * reference of the BPF cpumask has been released, it is subsequently freed in
 * an RCU callback in the BPF memory allocator.
 */
__bpf_kfunc void bpf_crypto_skcipher_ctx_release(struct bpf_crypto_skcipher_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->usage))
		call_rcu(&ctx->rcu, crypto_free_sync_skcipher_cb);
}

static int bpf_crypto_skcipher_crypt(struct crypto_sync_skcipher *tfm,
				     const struct bpf_dynptr_kern *src,
				     struct bpf_dynptr_kern *dst,
				     const struct bpf_dynptr_kern *iv,
				     bool decrypt)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(req, tfm);
	struct scatterlist sgin, sgout;
	u32 src_len, dst_len, iv_len;
	const u8 *psrc;
	u8 *pdst, *piv;
	int err;

	if (crypto_sync_skcipher_get_flags(tfm) & CRYPTO_TFM_NEED_KEY)
		return -EINVAL;

	if (__bpf_dynptr_is_rdonly(dst))
		return -EINVAL;

	iv_len = __bpf_dynptr_size(iv);
	src_len = __bpf_dynptr_size(src);
	dst_len = __bpf_dynptr_size(dst);
	if (!src_len || !dst_len)
		return -EINVAL;

	if (iv_len != crypto_sync_skcipher_ivsize(tfm))
		return -EINVAL;

	psrc = __bpf_dynptr_data(src, src_len);
	if (!psrc)
		return -EINVAL;
	pdst = __bpf_dynptr_data_rw(dst, dst_len);
	if (!pdst)
		return -EINVAL;

	piv = iv_len ? __bpf_dynptr_data_rw(iv, iv_len) : NULL;
	if (iv_len && !piv)
		return -EINVAL;

	sg_init_one(&sgin, psrc, src_len);
	sg_init_one(&sgout, pdst, dst_len);

	skcipher_request_set_sync_tfm(req, tfm);
	skcipher_request_set_callback(req, 0, NULL, NULL);
	skcipher_request_set_crypt(req, &sgin, &sgout, src_len, piv);

	err = decrypt ? crypto_skcipher_decrypt(req) : crypto_skcipher_encrypt(req);
	skcipher_request_zero(req);

	return err;
}

/**
 * bpf_crypto_skcipher_decrypt() - Decrypt buffer using configured context and IV provided.
 * @ctx:	The crypto context being used. The ctx must be a trusted pointer.
 * @src:	bpf_dynptr to the encrypted data. Must be a trusted pointer.
 * @dst:	bpf_dynptr to the buffer where to store the result. Must be a trusted pointer.
 * @iv:		bpf_dynptr to IV data to be used by decryptor.
 *
 * Decrypts provided buffer using IV data and the crypto context. Crypto context must be configured.
 */
__bpf_kfunc int bpf_crypto_skcipher_decrypt(struct bpf_crypto_skcipher_ctx *ctx,
					    const struct bpf_dynptr_kern *src,
					    struct bpf_dynptr_kern *dst,
					    struct bpf_dynptr_kern *iv)
{
	return bpf_crypto_skcipher_crypt(ctx->tfm, src, dst, iv, true);
}

/**
 * bpf_crypto_skcipher_encrypt() - Encrypt buffer using configured context and IV provided.
 * @ctx:	The crypto context being used. The ctx must be a trusted pointer.
 * @src:	bpf_dynptr to the plain data. Must be a trusted pointer.
 * @dst:	bpf_dynptr to buffer where to store the result. Must be a trusted pointer.
 * @iv:		bpf_dynptr to IV data to be used by decryptor.
 *
 * Encrypts provided buffer using IV data and the crypto context. Crypto context must be configured.
 */
__bpf_kfunc int bpf_crypto_skcipher_encrypt(struct bpf_crypto_skcipher_ctx *ctx,
					    const struct bpf_dynptr_kern *src,
					    struct bpf_dynptr_kern *dst,
					    struct bpf_dynptr_kern *iv)
{
	return bpf_crypto_skcipher_crypt(ctx->tfm, src, dst, iv, false);
}

__bpf_kfunc_end_defs();

BTF_SET8_START(crypt_skcipher_init_kfunc_btf_ids)
BTF_ID_FLAGS(func, bpf_crypto_skcipher_ctx_create, KF_ACQUIRE | KF_RET_NULL | KF_SLEEPABLE)
BTF_ID_FLAGS(func, bpf_crypto_skcipher_ctx_release, KF_RELEASE)
BTF_ID_FLAGS(func, bpf_crypto_skcipher_ctx_acquire, KF_ACQUIRE | KF_TRUSTED_ARGS)
BTF_SET8_END(crypt_skcipher_init_kfunc_btf_ids)

static const struct btf_kfunc_id_set crypt_skcipher_init_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &crypt_skcipher_init_kfunc_btf_ids,
};

BTF_SET8_START(crypt_skcipher_kfunc_btf_ids)
BTF_ID_FLAGS(func, bpf_crypto_skcipher_decrypt, KF_RCU)
BTF_ID_FLAGS(func, bpf_crypto_skcipher_encrypt, KF_RCU)
BTF_SET8_END(crypt_skcipher_kfunc_btf_ids)

static const struct btf_kfunc_id_set crypt_skcipher_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &crypt_skcipher_kfunc_btf_ids,
};

BTF_ID_LIST(crypto_skcipher_dtor_ids)
BTF_ID(struct, bpf_crypto_skcipher_ctx)
BTF_ID(func, bpf_crypto_skcipher_ctx_release)

static int __init crypto_skcipher_kfunc_init(void)
{
	int ret;
	const struct btf_id_dtor_kfunc crypto_skcipher_dtors[] = {
		{
			.btf_id	      = crypto_skcipher_dtor_ids[0],
			.kfunc_btf_id = crypto_skcipher_dtor_ids[1]
		},
	};

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_SCHED_CLS, &crypt_skcipher_kfunc_set);
	ret = ret ?: register_btf_kfunc_id_set(BPF_PROG_TYPE_SCHED_ACT, &crypt_skcipher_kfunc_set);
	ret = ret ?: register_btf_kfunc_id_set(BPF_PROG_TYPE_XDP, &crypt_skcipher_kfunc_set);
	ret = ret ?: register_btf_kfunc_id_set(BPF_PROG_TYPE_UNSPEC,
					       &crypt_skcipher_init_kfunc_set);
	return  ret ?: register_btf_id_dtor_kfuncs(crypto_skcipher_dtors,
						   ARRAY_SIZE(crypto_skcipher_dtors),
						   THIS_MODULE);
}

late_initcall(crypto_skcipher_kfunc_init);
