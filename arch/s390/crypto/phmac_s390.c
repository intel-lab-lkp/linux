// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright IBM Corp. 2024
 *
 * s390 specific HMAC support for protected keys.
 */

#define KMSG_COMPONENT	"phmac_s390"
#define pr_fmt(fmt)	KMSG_COMPONENT ": " fmt

#include <asm/cpacf.h>
#include <asm/pkey.h>
#include <crypto/cryptd.h>
#include <crypto/internal/hash.h>
#include <crypto/sha2.h>
#include <linux/cpufeature.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/spinlock.h>

/*
 * KMAC param block layout for sha2 function codes:
 * The layout of the param block for the KMAC instruction depends on the
 * blocksize of the used hashing sha2-algorithm function codes. The param block
 * contains the hash chaining value (cv), the input message bit-length (imbl)
 * and the hmac-secret (key). To prevent code duplication, the sizes of all
 * these are calculated based on the blocksize.
 *
 * param-block:
 * +-------+
 * | cv    |
 * +-------+
 * | imbl  |
 * +-------+
 * | key   |
 * +-------+
 *
 * sizes:
 * part | sh2-alg | calculation | size | type
 * -----+---------+-------------+------+--------
 * cv   | 224/256 | blocksize/2 |   32 |  u64[8]
 *      | 384/512 |             |   64 | u128[8]
 * imbl | 224/256 | blocksize/8 |    8 |     u64
 *      | 384/512 |             |   16 |    u128
 * key  | 224/256 | blocksize   |   96 |  u8[96]
 *      | 384/512 |             |  160 | u8[160]
 */

#define MAX_DIGEST_SIZE		SHA512_DIGEST_SIZE
#define MAX_IMBL_SIZE		sizeof(u128)
#define MAX_BLOCK_SIZE		SHA512_BLOCK_SIZE

#define SHA2_CV_SIZE(bs)	((bs) >> 1)
#define SHA2_IMBL_SIZE(bs)	((bs) >> 3)

#define SHA2_IMBL_OFFSET(bs)	(SHA2_CV_SIZE(bs))
#define SHA2_KEY_OFFSET(bs)	(SHA2_CV_SIZE(bs) + SHA2_IMBL_SIZE(bs))

#define PHMAC_SHA256_KEY_SIZE	(SHA256_BLOCK_SIZE + 32)
#define PHMAC_SHA512_KEY_SIZE	(SHA512_BLOCK_SIZE + 32)
#define PHMAC_MAX_KEY_SIZE	PHMAC_SHA512_KEY_SIZE

struct phmac_protkey {
	u32 type;
	u32 len;
	u8 protkey[PHMAC_MAX_KEY_SIZE];
};

struct s390_phmac_ctx {
	u8 *key;
	unsigned int keylen;

	struct phmac_protkey pk;
	/* spinlock to atomic update pk */
	spinlock_t pk_lock;
};

union s390_kmac_gr0 {
	unsigned long reg;
	struct {
		unsigned long		: 48;
		unsigned long ikp	:  1;
		unsigned long iimp	:  1;
		unsigned long ccup	:  1;
		unsigned long		:  6;
		unsigned long fc	:  7;
	};
};

struct s390_kmac_sha2_ctx {
	u8 param[MAX_DIGEST_SIZE + MAX_IMBL_SIZE + PHMAC_MAX_KEY_SIZE];
	union s390_kmac_gr0 gr0;
	u8 buf[MAX_BLOCK_SIZE];
	unsigned int buflen;
};

/*
 * kmac_sha2_set_imbl - sets the input message bit-length based on the blocksize
 */
static inline void kmac_sha2_set_imbl(u8 *param, unsigned int buflen,
				      unsigned int blocksize)
{
	u8 *imbl = param + SHA2_IMBL_OFFSET(blocksize);

	switch (blocksize) {
	case SHA256_BLOCK_SIZE:
		*(u64 *)imbl = (u64)buflen * BITS_PER_BYTE;
		break;
	case SHA512_BLOCK_SIZE:
		*(u128 *)imbl = (u128)buflen * BITS_PER_BYTE;
		break;
	default:
		break;
	}
}

static inline int phmac_keyblob2pkey(const u8 *key, unsigned int keylen,
				     struct phmac_protkey *pk)
{
	int i, rc = -EIO;

	/* try three times in case of busy card */
	for (i = 0; rc && i < 3; i++) {
		if (rc == -EBUSY && msleep_interruptible(1000))
			return -EINTR;
		rc = pkey_key2protkey(key, keylen,
				      pk->protkey, &pk->len, &pk->type);
	}

	return rc;
}

static inline int phmac_convert_key(struct s390_phmac_ctx *tfm_ctx)
{
	struct phmac_protkey pk;
	int rc;

	pk.len = sizeof(pk.protkey);
	rc = phmac_keyblob2pkey(tfm_ctx->key, tfm_ctx->keylen, &pk);
	if (rc)
		return rc;

	spin_lock_bh(&tfm_ctx->pk_lock);
	tfm_ctx->pk = pk;
	spin_unlock_bh(&tfm_ctx->pk_lock);

	return 0;
}

static int s390_phmac_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);
	struct s390_kmac_sha2_ctx *ctx = ahash_request_ctx(req);
	unsigned int bs = crypto_ahash_blocksize(tfm);

	spin_lock_bh(&tfm_ctx->pk_lock);
	memcpy(ctx->param + SHA2_KEY_OFFSET(bs),
	       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
	spin_unlock_bh(&tfm_ctx->pk_lock);

	ctx->buflen = 0;
	ctx->gr0.reg = 0;

	switch (crypto_ahash_digestsize(tfm)) {
	case SHA224_DIGEST_SIZE:
		ctx->gr0.fc = CPACF_KMAC_PHMAC_SHA_224;
		break;
	case SHA256_DIGEST_SIZE:
		ctx->gr0.fc = CPACF_KMAC_PHMAC_SHA_256;
		break;
	case SHA384_DIGEST_SIZE:
		ctx->gr0.fc = CPACF_KMAC_PHMAC_SHA_384;
		break;
	case SHA512_DIGEST_SIZE:
		ctx->gr0.fc = CPACF_KMAC_PHMAC_SHA_512;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static inline int s390_phmac_walk(struct s390_phmac_ctx *tfm_ctx,
				  struct s390_kmac_sha2_ctx *ctx,
				  unsigned int blocksize,
				  const u8 *data, unsigned int len)
{
	unsigned int offset, n, k;

	/* check current buffer */
	offset = ctx->buflen % blocksize;
	ctx->buflen += len;
	if (offset + len < blocksize)
		goto store;

	/* process one stored block */
	if (offset) {
		n = blocksize - offset;
		memcpy(ctx->buf + offset, data, n);
		ctx->gr0.iimp = 1;
		for (k = blocksize;;) {
			k -= _cpacf_kmac(&ctx->gr0.reg, ctx->param,
					 ctx->buf + blocksize - k, k);
			if (!k)
				break;
			if (phmac_convert_key(tfm_ctx))
				return -EIO;
			spin_lock_bh(&tfm_ctx->pk_lock);
			memcpy(ctx->param + SHA2_KEY_OFFSET(blocksize),
			       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
			spin_unlock_bh(&tfm_ctx->pk_lock);
		}
		data += n;
		len -= n;
		offset = 0;
	}

	/* process as many blocks as possible */
	if (len >= blocksize) {
		n = (len / blocksize) * blocksize;
		ctx->gr0.iimp = 1;
		for (k = n;;) {
			k -= _cpacf_kmac(&ctx->gr0.reg, ctx->param,
					 data + n - k, k);
			if (!k)
				break;
			if (phmac_convert_key(tfm_ctx))
				return -EIO;
			spin_lock_bh(&tfm_ctx->pk_lock);
			memcpy(ctx->param + SHA2_KEY_OFFSET(blocksize),
			       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
			spin_unlock_bh(&tfm_ctx->pk_lock);
		}
		data += n;
		len -= n;
	}

store:
	/* store incomplete block in buffer */
	if (len)
		memcpy(ctx->buf + offset, data, len);

	return 0;
}

static int s390_phmac_update(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);
	struct s390_kmac_sha2_ctx *ctx = ahash_request_ctx(req);
	unsigned int bs = crypto_ahash_blocksize(tfm);
	struct crypto_hash_walk walk;
	int nbytes, rc;

	for (nbytes = crypto_hash_walk_first(req, &walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(&walk, 0)) {
		rc = s390_phmac_walk(tfm_ctx, ctx, bs, walk.data, nbytes);
		if (rc)
			return crypto_hash_walk_done(&walk, rc);
	}

	return 0;
}

static int s390_phmac_final(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);
	struct s390_kmac_sha2_ctx *ctx = ahash_request_ctx(req);
	unsigned int bs = crypto_ahash_blocksize(tfm);
	unsigned int ds = crypto_ahash_digestsize(tfm);
	struct crypto_hash_walk walk;
	unsigned int n, k;
	int nbytes, rc;

	for (nbytes = crypto_hash_walk_first(req, &walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(&walk, 0)) {
		rc = s390_phmac_walk(tfm_ctx, ctx, bs, walk.data, nbytes);
		if (rc)
			return crypto_hash_walk_done(&walk, rc);
	}

	n = ctx->buflen % bs;
	ctx->gr0.iimp = 0;
	kmac_sha2_set_imbl(ctx->param, ctx->buflen, bs);
	for (k = n;;) {
		k -= _cpacf_kmac(&ctx->gr0.reg, ctx->param,
				 ctx->buf + n - k, k);
		if (!k)
			break;
		if (phmac_convert_key(tfm_ctx))
			return -EIO;
		spin_lock_bh(&tfm_ctx->pk_lock);
		memcpy(ctx->param + SHA2_KEY_OFFSET(bs),
		       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
		spin_unlock_bh(&tfm_ctx->pk_lock);
	}
	memcpy(req->result, ctx->param, ds);

	return 0;
}

static int s390_phmac_setkey(struct crypto_ahash *tfm,
			     const u8 *key, unsigned int keylen)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);
	int rc = -ENOMEM;

	if (tfm_ctx->keylen) {
		kfree_sensitive(tfm_ctx->key);
		tfm_ctx->key = NULL;
		tfm_ctx->keylen = 0;
	}

	tfm_ctx->key = kmemdup(key, keylen, GFP_KERNEL);
	if (!tfm_ctx->key)
		goto out;
	tfm_ctx->keylen = keylen;

	rc = phmac_convert_key(tfm_ctx);
	if (rc)
		goto out;

	rc = -EINVAL;
	switch (crypto_ahash_digestsize(tfm)) {
	case SHA224_DIGEST_SIZE:
	case SHA256_DIGEST_SIZE:
		if (tfm_ctx->pk.type != PKEY_KEYTYPE_HMAC_512)
			goto out;
		break;
	case SHA384_DIGEST_SIZE:
	case SHA512_DIGEST_SIZE:
		if (tfm_ctx->pk.type != PKEY_KEYTYPE_HMAC_1024)
			goto out;
		break;
	default:
		goto out;
	}
	rc = 0;

out:
	pr_debug("rc=%d\n", rc);
	return rc;
}

static int s390_phmac_import(struct ahash_request *req, const void *in)
{
	return -EOPNOTSUPP;
}

static int s390_phmac_export(struct ahash_request *req, void *out)
{
	return -EOPNOTSUPP;
}

static int s390_phmac_init_tfm(struct crypto_ahash *tfm)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);

	tfm_ctx->key = NULL;
	tfm_ctx->keylen = 0;
	spin_lock_init(&tfm_ctx->pk_lock);

	crypto_ahash_set_reqsize(tfm, sizeof(struct s390_kmac_sha2_ctx));

	return 0;
}

static void s390_phmac_exit_tfm(struct crypto_ahash *tfm)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);

	memzero_explicit(&tfm_ctx->pk, sizeof(tfm_ctx->pk));
	kfree_sensitive(tfm_ctx->key);
}

#define S390_ASYNC_PHMAC_ALG(x)						\
{									\
	.init = s390_phmac_init,					\
	.update = s390_phmac_update,					\
	.final = s390_phmac_final,					\
	.setkey = s390_phmac_setkey,					\
	.import = s390_phmac_import,					\
	.export = s390_phmac_export,					\
	.init_tfm = s390_phmac_init_tfm,				\
	.exit_tfm = s390_phmac_exit_tfm,				\
	.halg = {							\
		.digestsize = SHA##x##_DIGEST_SIZE,			\
		.statesize = sizeof(struct s390_kmac_sha2_ctx),		\
		.base = {						\
			.cra_name = "phmac(sha" #x ")",			\
			.cra_driver_name = "phmac_s390_sha" #x,		\
			.cra_blocksize = SHA##x##_BLOCK_SIZE,		\
			.cra_priority = 400,				\
			.cra_flags = CRYPTO_ALG_ASYNC,			\
			.cra_ctxsize = sizeof(struct s390_phmac_ctx),	\
			.cra_module = THIS_MODULE,			\
		},							\
	},								\
}

static struct s390_hmac_alg {
	unsigned int fc;
	struct ahash_alg alg;
	bool registered;
} s390_hmac_algs[] = {
	{
		.fc = CPACF_KMAC_PHMAC_SHA_224,
		.alg = S390_ASYNC_PHMAC_ALG(224),
	}, {
		.fc = CPACF_KMAC_PHMAC_SHA_256,
		.alg = S390_ASYNC_PHMAC_ALG(256),
	}, {
		.fc = CPACF_KMAC_PHMAC_SHA_384,
		.alg = S390_ASYNC_PHMAC_ALG(384),
	}, {
		.fc = CPACF_KMAC_PHMAC_SHA_512,
		.alg = S390_ASYNC_PHMAC_ALG(512),
	}
};

static __always_inline void _s390_hmac_algs_unregister(void)
{
	struct s390_hmac_alg *hmac;
	int i;

	for (i = ARRAY_SIZE(s390_hmac_algs) - 1; i >= 0; i--) {
		hmac = &s390_hmac_algs[i];
		if (hmac->registered)
			crypto_unregister_ahash(&hmac->alg);
	}
}

static int __init phmac_s390_init(void)
{
	struct s390_hmac_alg *hmac;
	int i, rc = -ENODEV;

	for (i = 0; i < ARRAY_SIZE(s390_hmac_algs); i++) {
		hmac = &s390_hmac_algs[i];
		if (!cpacf_query_func(CPACF_KMAC, hmac->fc))
			continue;
		rc = crypto_register_ahash(&hmac->alg);
		if (rc) {
			pr_err("unable to register %s\n",
			       hmac->alg.halg.base.cra_name);
			goto out;
		}
		hmac->registered = true;
		pr_debug("registered %s\n", hmac->alg.halg.base.cra_name);
	}
	return rc;
out:
	_s390_hmac_algs_unregister();
	return rc;
}

static void __exit phmac_s390_exit(void)
{
	_s390_hmac_algs_unregister();
}

module_init(phmac_s390_init);
module_exit(phmac_s390_exit);

MODULE_ALIAS_CRYPTO("phmac(sha224)");
MODULE_ALIAS_CRYPTO("phmac(sha256)");
MODULE_ALIAS_CRYPTO("phmac(sha384)");
MODULE_ALIAS_CRYPTO("phmac(sha512)");

MODULE_DESCRIPTION("S390 HMAC driver for protected keys");
MODULE_LICENSE("GPL");
