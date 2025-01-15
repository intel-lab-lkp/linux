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
#include <linux/workqueue.h>

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

#define PK_STATE_NO_KEY		     0
#define PK_STATE_NEEDS_CONVERT	     1
#define PK_STATE_CONVERT_IN_PROGRESS 2
#define PK_STATE_VALID		     3

struct s390_phmac_ctx {
	u8 *key;
	unsigned int keylen;

	/* the work struct for asynch key convert */
	struct delayed_work work;

	/* spinlock to atomic read/update the following fields */
	spinlock_t pk_lock;
	/* see PK_STATE* defines above, < 0 holds convert failure rc  */
	int pk_state;
	/* if state is valid, pk holds the protected key */
	struct phmac_protkey pk;
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

struct s390_phmac_req_ctx {
	struct delayed_work work;
	struct ahash_request *req;
	struct s390_kmac_sha2_ctx sha2_ctx;
};

/*
 * Pkey 'token' struct used to derive a protected key value from a clear key.
 */
struct hmac_clrkey_token {
	u8  type;
	u8  res0[3];
	u8  version;
	u8  res1[3];
	u32 keytype;
	u32 len;
	u8 key[];
} __packed;

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

static int hash_key(const u8 *in, unsigned int inlen,
		    u8 *digest, unsigned int digestsize)
{
	unsigned long func;
	union {
		struct sha256_paramblock {
			u32 h[8];
			u64 mbl;
		} sha256;
		struct sha512_paramblock {
			u64 h[8];
			u128 mbl;
		} sha512;
	} __packed param;

#define PARAM_INIT(x, y, z)		   \
	param.sha##x.h[0] = SHA##y ## _H0; \
	param.sha##x.h[1] = SHA##y ## _H1; \
	param.sha##x.h[2] = SHA##y ## _H2; \
	param.sha##x.h[3] = SHA##y ## _H3; \
	param.sha##x.h[4] = SHA##y ## _H4; \
	param.sha##x.h[5] = SHA##y ## _H5; \
	param.sha##x.h[6] = SHA##y ## _H6; \
	param.sha##x.h[7] = SHA##y ## _H7; \
	param.sha##x.mbl = (z)

	switch (digestsize) {
	case SHA224_DIGEST_SIZE:
		func = CPACF_KLMD_SHA_256;
		PARAM_INIT(256, 224, inlen * 8);
		break;
	case SHA256_DIGEST_SIZE:
		func = CPACF_KLMD_SHA_256;
		PARAM_INIT(256, 256, inlen * 8);
		break;
	case SHA384_DIGEST_SIZE:
		func = CPACF_KLMD_SHA_512;
		PARAM_INIT(512, 384, inlen * 8);
		break;
	case SHA512_DIGEST_SIZE:
		func = CPACF_KLMD_SHA_512;
		PARAM_INIT(512, 512, inlen * 8);
		break;
	default:
		return -EINVAL;
	}

#undef PARAM_INIT

	cpacf_klmd(func, &param, in, inlen);

	memcpy(digest, &param, digestsize);

	return 0;
}

/*
 * make_clrkey_token() - wrap the clear key into a pkey clearkey token.
 */
static inline int make_clrkey_token(const u8 *clrkey, size_t clrkeylen,
				    unsigned int digestsize, u8 *dest)
{
	struct hmac_clrkey_token *token = (struct hmac_clrkey_token *)dest;
	unsigned int blocksize;
	int rc;

	token->type = 0x00;
	token->version = 0x02;
	switch (digestsize) {
	case SHA224_DIGEST_SIZE:
	case SHA256_DIGEST_SIZE:
		token->keytype = PKEY_KEYTYPE_HMAC_512;
		blocksize = 64;
		break;
	case SHA384_DIGEST_SIZE:
	case SHA512_DIGEST_SIZE:
		token->keytype = PKEY_KEYTYPE_HMAC_1024;
		blocksize = 128;
		break;
	default:
		return -EINVAL;
	}
	token->len = blocksize;

	if (clrkeylen > blocksize) {
		rc = hash_key(clrkey, clrkeylen, token->key, digestsize);
		if (rc)
			return rc;
	} else {
		memcpy(token->key, clrkey, clrkeylen);
	}

	return 0;
}

/*
 * Convert the raw key material into a protected key via PKEY api.
 * This function may sleep - don't call in non-sleeping context.
 */
static int phmac_convert_key(struct s390_phmac_ctx *tfm_ctx)
{
	struct phmac_protkey pk;
	int i, rc;

	pk.len = sizeof(pk.protkey);

	/* try three times in case of busy card */
	for (rc = -EIO, i = 0; rc && i < 3; i++) {
		if (rc == -EBUSY && msleep_interruptible((1 << i) * 100)) {
			rc = -EINTR;
			goto out;
		}
		rc = pkey_key2protkey(tfm_ctx->key, tfm_ctx->keylen,
				      pk.protkey, &pk.len, &pk.type);
	}
	if (rc)
		goto out;

	spin_lock_bh(&tfm_ctx->pk_lock);
	tfm_ctx->pk = pk;
	tfm_ctx->pk_state = PK_STATE_VALID;
	spin_unlock_bh(&tfm_ctx->pk_lock);

	memzero_explicit(&pk, sizeof(pk));

out:
	pr_debug("rc=%d\n", rc);
	return rc;
}

static void phmac_wq_convert_key_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct s390_phmac_ctx *tfm_ctx =
		container_of(dwork, struct s390_phmac_ctx, work);
	int rc;

	rc = phmac_convert_key(tfm_ctx);
	pr_debug("asynch convert done, rc=%d\n", rc);
}

static int phmac_init(struct crypto_ahash *tfm,
		      struct s390_kmac_sha2_ctx *ctx,
		      bool maysleep)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);
	unsigned int ds = crypto_ahash_digestsize(tfm);
	unsigned int bs = crypto_ahash_blocksize(tfm);
	int i, rc, pk_state;

	spin_lock_bh(&tfm_ctx->pk_lock);
	pk_state = tfm_ctx->pk_state;
	spin_unlock_bh(&tfm_ctx->pk_lock);

	switch (pk_state) {
	case PK_STATE_NO_KEY:
		return -ENOKEY;
	case PK_STATE_NEEDS_CONVERT:
		if (!maysleep)
			return -EKEYEXPIRED;
		rc = phmac_convert_key(tfm_ctx);
		if (rc)
			return rc;
		break;
	case PK_STATE_CONVERT_IN_PROGRESS:
		if (!maysleep)
			return -EKEYEXPIRED;
		for (i = 0; pk_state != PK_STATE_VALID && i < 3; i++) {
			if (msleep_interruptible((1 << i) * 100))
				return -EINTR;
			spin_lock_bh(&tfm_ctx->pk_lock);
			pk_state = tfm_ctx->pk_state;
			spin_unlock_bh(&tfm_ctx->pk_lock);
		}
		if (pk_state != PK_STATE_VALID)
			return -EKEYEXPIRED;
		break;
	case PK_STATE_VALID:
		break;
	default:
		return pk_state < 0 ? pk_state : -EIO;
	}

	/* pk is valid, prepare the sha2 context */

	spin_lock_bh(&tfm_ctx->pk_lock);
	memcpy(ctx->param + SHA2_KEY_OFFSET(bs),
	       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
	spin_unlock_bh(&tfm_ctx->pk_lock);

	ctx->buflen = 0;
	ctx->gr0.reg = 0;

	/* set function code, check for valid protected key type */
	rc = 0;
	switch (ds) {
	case SHA224_DIGEST_SIZE:
		ctx->gr0.fc = CPACF_KMAC_PHMAC_SHA_224;
		if (tfm_ctx->pk.type != PKEY_KEYTYPE_HMAC_512)
			rc = -EINVAL;
		break;
	case SHA256_DIGEST_SIZE:
		ctx->gr0.fc = CPACF_KMAC_PHMAC_SHA_256;
		if (tfm_ctx->pk.type != PKEY_KEYTYPE_HMAC_512)
			rc = -EINVAL;
		break;
	case SHA384_DIGEST_SIZE:
		ctx->gr0.fc = CPACF_KMAC_PHMAC_SHA_384;
		if (tfm_ctx->pk.type != PKEY_KEYTYPE_HMAC_1024)
			rc = -EINVAL;
		break;
	case SHA512_DIGEST_SIZE:
		ctx->gr0.fc = CPACF_KMAC_PHMAC_SHA_512;
		if (tfm_ctx->pk.type != PKEY_KEYTYPE_HMAC_1024)
			rc = -EINVAL;
		break;
	default:
		rc = -EINVAL;
	}

	return rc;
}

static void phmac_wq_init_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct s390_phmac_req_ctx *req_ctx =
		container_of(dwork, struct s390_phmac_req_ctx, work);
	struct ahash_request *req = req_ctx->req;
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	int rc;

	rc = phmac_init(tfm, ctx, true);

	pr_debug("req complete with rc=%d\n", rc);
	local_bh_disable();
	ahash_request_complete(req, rc);
	local_bh_enable();
}

static int s390_phmac_init(struct ahash_request *req)
{
	struct s390_phmac_req_ctx *req_ctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	int rc;

	/*
	 * First try synchronous. If this fails for any reason
	 * schedule this request asynchronous via workqueue.
	 */

	rc = phmac_init(tfm, ctx, false);
	if (!rc)
		goto out;

	req_ctx->req = req;
	INIT_DELAYED_WORK(&req_ctx->work, phmac_wq_init_fn);
	schedule_delayed_work(&req_ctx->work, 0);
	rc = -EINPROGRESS;

out:
	pr_debug("rc=%d\n", rc);
	return rc;
}

static int phmac_update(struct crypto_ahash *tfm,
			struct s390_kmac_sha2_ctx *ctx,
			const u8 *data, unsigned int len,
			bool maysleep)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);
	unsigned int bs = crypto_ahash_blocksize(tfm);
	unsigned int offset, n, k;
	int rc;

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
			if (likely(!k))
				break;
			if (!maysleep)
				return -EKEYEXPIRED;
			rc = phmac_convert_key(tfm_ctx);
			if (rc)
				return rc;
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
			if (likely(!k))
				break;
			if (!maysleep)
				return -EKEYEXPIRED;
			rc = phmac_convert_key(tfm_ctx);
			if (rc)
				return rc;
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

static void phmac_wq_update_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct s390_phmac_req_ctx *req_ctx =
		container_of(dwork, struct s390_phmac_req_ctx, work);
	struct ahash_request *req = req_ctx->req;
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	struct crypto_hash_walk walk;
	int nbytes, rc = 0;

	for (nbytes = crypto_hash_walk_first(req, &walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(&walk, 0)) {
		rc = phmac_update(tfm, ctx, walk.data, nbytes, true);
		if (rc) {
			crypto_hash_walk_done(&walk, rc);
			break;
		}
	}

	pr_debug("req complete with rc=%d\n", rc);
	local_bh_disable();
	ahash_request_complete(req, rc);
	local_bh_enable();
}

static int s390_phmac_update(struct ahash_request *req)
{
	struct s390_phmac_req_ctx *req_ctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	struct s390_kmac_sha2_ctx ctx_backup;
	struct crypto_hash_walk walk;
	int nbytes, rc = 0;

	/*
	 * First try synchronous. If this fails for any reason
	 * schedule this request asynchronous via workqueue.
	 */

	memcpy(&ctx_backup, ctx, sizeof(*ctx));

	for (nbytes = crypto_hash_walk_first(req, &walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(&walk, 0)) {
		rc = phmac_update(tfm, ctx, walk.data, nbytes, false);
		if (rc) {
			crypto_hash_walk_done(&walk, rc);
			break;
		}
	}
	if (!rc)
		goto out;

	memcpy(ctx, &ctx_backup, sizeof(*ctx));

	req_ctx->req = req;
	INIT_DELAYED_WORK(&req_ctx->work, phmac_wq_update_fn);
	schedule_delayed_work(&req_ctx->work, 0);
	rc = -EINPROGRESS;

out:
	pr_debug("rc=%d\n", rc);
	return rc;
}

static int phmac_final(struct crypto_ahash *tfm,
		       struct s390_kmac_sha2_ctx *ctx,
		       unsigned char *result,
		       bool maysleep)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);
	unsigned int ds = crypto_ahash_digestsize(tfm);
	unsigned int bs = crypto_ahash_blocksize(tfm);
	unsigned int n, k;
	int rc;

	n = ctx->buflen % bs;
	ctx->gr0.iimp = 0;
	kmac_sha2_set_imbl(ctx->param, ctx->buflen, bs);
	for (k = n;;) {
		k -= _cpacf_kmac(&ctx->gr0.reg, ctx->param,
				 ctx->buf + n - k, k);
		if (likely(!k))
			break;
		if (!maysleep)
			return -EKEYEXPIRED;
		rc = phmac_convert_key(tfm_ctx);
		if (rc)
			return rc;
		spin_lock_bh(&tfm_ctx->pk_lock);
		memcpy(ctx->param + SHA2_KEY_OFFSET(bs),
		       tfm_ctx->pk.protkey, tfm_ctx->pk.len);
		spin_unlock_bh(&tfm_ctx->pk_lock);
	}

	memcpy(result, ctx->param, ds);

	return 0;
}

static void phmac_wq_final_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct s390_phmac_req_ctx *req_ctx =
		container_of(dwork, struct s390_phmac_req_ctx, work);
	struct ahash_request *req = req_ctx->req;
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	int rc;

	rc = phmac_final(tfm, ctx, req->result, true);

	pr_debug("req complete with rc=%d\n", rc);
	local_bh_disable();
	ahash_request_complete(req, rc);
	local_bh_enable();
}

static int s390_phmac_final(struct ahash_request *req)
{
	struct s390_phmac_req_ctx *req_ctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	struct s390_kmac_sha2_ctx ctx_backup;
	int rc;

	/*
	 * First try synchronous. If this fails for any reason
	 * schedule this request asynchronous via workqueue.
	 */

	memcpy(&ctx_backup, ctx, sizeof(*ctx));

	rc = phmac_final(tfm, ctx, req->result, false);
	if (!rc)
		goto out;

	memcpy(ctx, &ctx_backup, sizeof(*ctx));

	req_ctx->req = req;
	INIT_DELAYED_WORK(&req_ctx->work, phmac_wq_final_fn);
	schedule_delayed_work(&req_ctx->work, 0);
	rc = -EINPROGRESS;

out:
	pr_debug("rc=%d\n", rc);
	return rc;
}

static void phmac_wq_finup_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct s390_phmac_req_ctx *req_ctx =
		container_of(dwork, struct s390_phmac_req_ctx, work);
	struct ahash_request *req = req_ctx->req;
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	struct crypto_hash_walk walk;
	int nbytes, rc = 0;

	for (nbytes = crypto_hash_walk_first(req, &walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(&walk, 0)) {
		rc = phmac_update(tfm, ctx, walk.data, nbytes, true);
		if (rc) {
			crypto_hash_walk_done(&walk, rc);
			break;
		}
	}
	if (rc)
		goto out;

	rc = phmac_final(tfm, ctx, req->result, true);

out:
	pr_debug("req complete with rc=%d\n", rc);
	local_bh_disable();
	ahash_request_complete(req, rc);
	local_bh_enable();
}

static int s390_phmac_finup(struct ahash_request *req)
{
	struct s390_phmac_req_ctx *req_ctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	struct s390_kmac_sha2_ctx ctx_backup;
	struct crypto_hash_walk walk;
	int nbytes, rc = 0;

	/*
	 * First try synchronous. If this fails for any reason
	 * schedule this request asynchronous via workqueue.
	 */

	memcpy(&ctx_backup, ctx, sizeof(*ctx));

	for (nbytes = crypto_hash_walk_first(req, &walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(&walk, 0)) {
		rc = phmac_update(tfm, ctx, walk.data, nbytes, false);
		if (rc) {
			crypto_hash_walk_done(&walk, rc);
			break;
		}
	}

	if (!rc)
		rc = phmac_final(tfm, ctx, req->result, false);
	if (!rc)
		goto out;

	memcpy(ctx, &ctx_backup, sizeof(*ctx));

	req_ctx->req = req;
	INIT_DELAYED_WORK(&req_ctx->work, phmac_wq_finup_fn);
	schedule_delayed_work(&req_ctx->work, 0);
	rc = -EINPROGRESS;

out:
	pr_debug("rc=%d\n", rc);
	return rc;
}

static void phmac_wq_digest_fn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct s390_phmac_req_ctx *req_ctx =
		container_of(dwork, struct s390_phmac_req_ctx, work);
	struct ahash_request *req = req_ctx->req;
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	struct crypto_hash_walk walk;
	int nbytes, rc = 0;

	rc = phmac_init(tfm, ctx, true);
	if (rc)
		goto out;

	for (nbytes = crypto_hash_walk_first(req, &walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(&walk, 0)) {
		rc = phmac_update(tfm, ctx, walk.data, nbytes, true);
		if (rc) {
			crypto_hash_walk_done(&walk, rc);
			break;
		}
	}
	if (rc)
		goto out;

	rc = phmac_final(tfm, ctx, req->result, true);

out:
	pr_debug("req complete with rc=%d\n", rc);
	local_bh_disable();
	ahash_request_complete(req, rc);
	local_bh_enable();
}

static int s390_phmac_digest(struct ahash_request *req)
{
	struct s390_phmac_req_ctx *req_ctx = ahash_request_ctx(req);
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;
	struct crypto_hash_walk walk;
	int nbytes, rc;

	/*
	 * First try synchronous. If this fails for any reason
	 * schedule this request asynchronous via workqueue.
	 */

	rc = phmac_init(tfm, ctx, false);
	if (rc)
		goto via_wq;

	for (nbytes = crypto_hash_walk_first(req, &walk); nbytes > 0;
	     nbytes = crypto_hash_walk_done(&walk, 0)) {
		rc = phmac_update(tfm, ctx, walk.data, nbytes, false);
		if (rc) {
			crypto_hash_walk_done(&walk, rc);
			break;
		}
	}
	if (rc)
		goto via_wq;

	rc = phmac_final(tfm, ctx, req->result, false);
	if (!rc)
		goto out;

via_wq:
	req_ctx->req = req;
	INIT_DELAYED_WORK(&req_ctx->work, phmac_wq_digest_fn);
	schedule_delayed_work(&req_ctx->work, 0);
	rc = -EINPROGRESS;

out:
	pr_debug("rc=%d\n", rc);
	return rc;
}

static int s390_phmac_setkey(struct crypto_ahash *tfm,
			     const u8 *key, unsigned int keylen)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);
	struct crypto_tfm *tfm_base = crypto_ahash_tfm(tfm);
	unsigned int ds = crypto_ahash_digestsize(tfm);
	unsigned int bs = crypto_ahash_blocksize(tfm);
	unsigned int tmpkeylen;
	u8 *tmpkey = NULL;
	int rc = 0;

	if (tfm_ctx->keylen) {
		kfree_sensitive(tfm_ctx->key);
		tfm_ctx->key = NULL;
		tfm_ctx->keylen = 0;
	}

	if (!(crypto_tfm_alg_get_flags(tfm_base) & CRYPTO_ALG_TESTED)) {
		/*
		 * selftest running: key is a raw hmac clear key and needs
		 * to get embedded into a 'clear key token' in order to have
		 * it correctly processed by the pkey module.
		 */
		tmpkeylen = sizeof(struct hmac_clrkey_token) + bs;
		tmpkey = kzalloc(tmpkeylen, GFP_ATOMIC);
		if (!tmpkey) {
			rc = -ENOMEM;
			goto out;
		}
		rc = make_clrkey_token(key, keylen, ds, tmpkey);
		if (rc)
			goto out;
		keylen = tmpkeylen;
		key = tmpkey;
	}

	/* key is always a key token digestable by PKEY */
	tfm_ctx->key = kmemdup(key, keylen, GFP_ATOMIC);
	if (!tfm_ctx->key) {
		rc = -ENOMEM;
		goto out;
	}
	tfm_ctx->keylen = keylen;

	/* Always trigger an asynch key convert */
	spin_lock_bh(&tfm_ctx->pk_lock);
	tfm_ctx->pk_state = PK_STATE_CONVERT_IN_PROGRESS;
	spin_unlock_bh(&tfm_ctx->pk_lock);
	schedule_delayed_work(&tfm_ctx->work, 0);

out:
	kfree(tmpkey);
	pr_debug("rc=%d\n", rc);
	return rc;
}

static int s390_phmac_import(struct ahash_request *req, const void *in)
{
	struct s390_phmac_req_ctx *req_ctx = ahash_request_ctx(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;

	memcpy(ctx, in, sizeof(*ctx));

	return 0;
}

static int s390_phmac_export(struct ahash_request *req, void *out)
{
	struct s390_phmac_req_ctx *req_ctx = ahash_request_ctx(req);
	struct s390_kmac_sha2_ctx *ctx = &req_ctx->sha2_ctx;

	memcpy(out, ctx, sizeof(*ctx));

	return 0;
}

static int s390_phmac_init_tfm(struct crypto_ahash *tfm)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);

	tfm_ctx->key = NULL;
	tfm_ctx->keylen = 0;

	INIT_DELAYED_WORK(&tfm_ctx->work, phmac_wq_convert_key_fn);

	tfm_ctx->pk_state = PK_STATE_NO_KEY;
	spin_lock_init(&tfm_ctx->pk_lock);

	crypto_ahash_set_reqsize(tfm, sizeof(struct s390_phmac_req_ctx));

	pr_debug("rc=0\n");
	return 0;
}

static void s390_phmac_exit_tfm(struct crypto_ahash *tfm)
{
	struct s390_phmac_ctx *tfm_ctx = crypto_ahash_ctx(tfm);

	flush_delayed_work(&tfm_ctx->work);

	memzero_explicit(&tfm_ctx->pk, sizeof(tfm_ctx->pk));
	kfree_sensitive(tfm_ctx->key);

	pr_debug("\n");
}

#define S390_ASYNC_PHMAC_ALG(x)						\
{									\
	.init = s390_phmac_init,					\
	.update = s390_phmac_update,					\
	.final = s390_phmac_final,					\
	.finup = s390_phmac_finup,					\
	.digest = s390_phmac_digest,					\
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

	if (!cpacf_query_func(CPACF_KLMD, CPACF_KLMD_SHA_256))
		return -ENODEV;
	if (!cpacf_query_func(CPACF_KLMD, CPACF_KLMD_SHA_512))
		return -ENODEV;

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
