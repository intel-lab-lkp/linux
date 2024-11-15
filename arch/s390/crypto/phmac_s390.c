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
#include <crypto/sha2.h>
#include <crypto/internal/hash.h>
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
 * cv	| 224/256 | blocksize/2 |   32 |  u64[8]
 *	| 384/512 |		|   64 | u128[8]
 * imbl | 224/256 | blocksize/8 |    8 |     u64
 *	| 384/512 |		|   16 |    u128
 * key	| 224/256 | blocksize	|   96 |  u8[96]
 *	| 384/512 |		|  160 | u8[160]
 *
 * The setkey() function and also update(), final() and digest() functions
 * for this protected key hmac implementation are not allowed to be called
 * in non-sleeping context. On attempt the underlying key conversion function
 * will return with -EOPNOTSUPP.
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

	/*
	 * Dependent on the key type, the PKEY module may contact
	 * a crypto card which is an IO operation and thus implies
	 * a non-atomic context. So refuse to contact PKEY at all
	 * if this is really a non-sleeping context.
	 */
	if (!in_task())
		return -EOPNOTSUPP;

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

static inline int s390_phmac_sha2_setkey(struct crypto_shash *tfm,
					 const u8 *key, unsigned int keylen)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_shash_ctx(tfm);
	int rc = -ENOMEM;

	if (tfm_ctx->keylen) {
		kfree_sensitive(tfm_ctx->key);
		tfm_ctx->key = NULL;
		tfm_ctx->keylen = 0;
	}

	tfm_ctx->key = kmemdup(key, keylen, GFP_ATOMIC);
	if (!tfm_ctx->key)
		goto out;
	tfm_ctx->keylen = keylen;

	rc = phmac_convert_key(tfm_ctx);
	if (rc)
		goto out;

	rc = -EINVAL;
	switch (crypto_shash_digestsize(tfm)) {
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

static int s390_phmac_sha2_init(struct shash_desc *desc)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_shash_ctx(desc->tfm);
	struct s390_kmac_sha2_ctx *ctx = shash_desc_ctx(desc);
	unsigned int bs = crypto_shash_blocksize(desc->tfm);

	spin_lock_bh(&tfm_ctx->pk_lock);
	memcpy(ctx->param + SHA2_KEY_OFFSET(bs),
	       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
	spin_unlock_bh(&tfm_ctx->pk_lock);

	ctx->buflen = 0;
	ctx->gr0.reg = 0;

	switch (crypto_shash_digestsize(desc->tfm)) {
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

static int s390_phmac_sha2_update(struct shash_desc *desc,
				  const u8 *data, unsigned int len)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_shash_ctx(desc->tfm);
	struct s390_kmac_sha2_ctx *ctx = shash_desc_ctx(desc);
	unsigned int bs = crypto_shash_blocksize(desc->tfm);
	unsigned int offset, n, k;

	/* check current buffer */
	offset = ctx->buflen % bs;
	ctx->buflen += len;
	if (offset + len < bs)
		goto store;

	/* process one stored block */
	if (offset) {
		n = bs - offset;
		memcpy(ctx->buf + offset, data, n);
		ctx->gr0.iimp = 1;
		for (k = bs;;) {
			k -= _cpacf_kmac(&ctx->gr0.reg, ctx->param,
					 ctx->buf + bs - k, k);
			if (!k)
				break;
			if (phmac_convert_key(tfm_ctx))
				return -EIO;
			spin_lock_bh(&tfm_ctx->pk_lock);
			memcpy(ctx->param + SHA2_KEY_OFFSET(bs),
			       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
			spin_unlock_bh(&tfm_ctx->pk_lock);
		}
		data += n;
		len -= n;
		offset = 0;
	}
	/* process as many blocks as possible */
	if (len >= bs) {
		n = (len / bs) * bs;
		ctx->gr0.iimp = 1;
		for (k = n;;) {
			k -= _cpacf_kmac(&ctx->gr0.reg, ctx->param,
					 data + n - k, k);
			if (!k)
				break;
			if (phmac_convert_key(tfm_ctx))
				return -EIO;
			spin_lock_bh(&tfm_ctx->pk_lock);
			memcpy(ctx->param + SHA2_KEY_OFFSET(bs),
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

static int s390_phmac_sha2_final(struct shash_desc *desc, u8 *out)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_shash_ctx(desc->tfm);
	struct s390_kmac_sha2_ctx *ctx = shash_desc_ctx(desc);
	unsigned int bs = crypto_shash_blocksize(desc->tfm);
	unsigned int n, k;

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
	memcpy(out, ctx->param, crypto_shash_digestsize(desc->tfm));

	return 0;
}

static int s390_phmac_sha2_digest(struct shash_desc *desc,
				  const u8 *data, unsigned int len, u8 *out)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_shash_ctx(desc->tfm);
	struct s390_kmac_sha2_ctx *ctx = shash_desc_ctx(desc);
	unsigned int ds = crypto_shash_digestsize(desc->tfm);
	unsigned int bs = crypto_shash_blocksize(desc->tfm);
	unsigned int k;
	int rc;

	rc = s390_phmac_sha2_init(desc);
	if (rc)
		return rc;

	ctx->gr0.iimp = 0;
	kmac_sha2_set_imbl(ctx->param, len, bs);
	for (k = len;;) {
		k -= _cpacf_kmac(&ctx->gr0.reg, ctx->param,
				 data + len - k, k);
		if (!k)
			break;
		if (phmac_convert_key(tfm_ctx))
			return -EIO;
		spin_lock_bh(&tfm_ctx->pk_lock);
		memcpy(ctx->param + SHA2_KEY_OFFSET(bs),
		       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
		spin_unlock_bh(&tfm_ctx->pk_lock);
	}
	memcpy(out, ctx->param, ds);

	return 0;
}

static int s390_phmac_sha2_init_tfm(struct crypto_shash *tfm)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_shash_ctx(tfm);

	tfm_ctx->key = NULL;
	tfm_ctx->keylen = 0;
	spin_lock_init(&tfm_ctx->pk_lock);

	return 0;
}

static void s390_phmac_sha2_exit_tfm(struct crypto_shash *tfm)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_shash_ctx(tfm);

	memzero_explicit(&tfm_ctx->pk, sizeof(tfm_ctx->pk));
	kfree_sensitive(tfm_ctx->key);
}

static int s390_phmac_sha2_clone_tfm(struct crypto_shash *dst,
				     struct crypto_shash *src)
{
	struct s390_phmac_ctx *src_ctx = crypto_shash_ctx(src);
	int rc;

	rc = s390_phmac_sha2_init_tfm(dst);
	if (rc)
		return rc;

	if (src_ctx->key && src_ctx->keylen)
		rc = s390_phmac_sha2_setkey(dst, src_ctx->key, src_ctx->keylen);

	return rc;
}

#define S390_HMAC_SHA2_ALG(x) {						\
	.fc = CPACF_KMAC_PHMAC_SHA_##x,					\
	.alg = {							\
		.init = s390_phmac_sha2_init,				\
		.update = s390_phmac_sha2_update,			\
		.final = s390_phmac_sha2_final,				\
		.digest = s390_phmac_sha2_digest,			\
		.setkey = s390_phmac_sha2_setkey,			\
		.init_tfm = s390_phmac_sha2_init_tfm,			\
		.exit_tfm = s390_phmac_sha2_exit_tfm,			\
		.clone_tfm = s390_phmac_sha2_clone_tfm,			\
		.descsize = sizeof(struct s390_kmac_sha2_ctx),		\
		.halg = {						\
			.digestsize = SHA##x##_DIGEST_SIZE,		\
			.base = {					\
				.cra_name = "phmac(sha" #x ")",		\
				.cra_driver_name = "phmac_s390_sha" #x,	\
				.cra_blocksize = SHA##x##_BLOCK_SIZE,	\
				.cra_priority = 400,			\
				.cra_ctxsize = sizeof(struct s390_phmac_ctx), \
				.cra_module = THIS_MODULE,		\
			},						\
		},							\
	},								\
}

static struct s390_hmac_alg {
	bool registered;
	unsigned int fc;
	struct shash_alg alg;
} s390_hmac_algs[] = {
	S390_HMAC_SHA2_ALG(224),
	S390_HMAC_SHA2_ALG(256),
	S390_HMAC_SHA2_ALG(384),
	S390_HMAC_SHA2_ALG(512),
};

static __always_inline void _s390_hmac_algs_unregister(void)
{
	struct s390_hmac_alg *hmac;
	int i;

	for (i = ARRAY_SIZE(s390_hmac_algs) - 1; i >= 0; i--) {
		hmac = &s390_hmac_algs[i];
		if (!hmac->registered)
			continue;
		crypto_unregister_shash(&hmac->alg);
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

		rc = crypto_register_shash(&hmac->alg);
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

MODULE_DESCRIPTION("S390 HMAC driver for protected keys");
MODULE_LICENSE("GPL");
