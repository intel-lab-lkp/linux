// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

/*
 * Refer to Documentation/block/inline-encryption.rst for detailed explanation.
 */

#define pr_fmt(fmt) "blk-crypto-fallback: " fmt

#include <crypto/skcipher.h>
#include <linux/blk-crypto.h>
#include <linux/blk-crypto-profile.h>
#include <linux/blkdev.h>
#include <linux/crypto.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/scatterlist.h>

#include "blk-cgroup.h"
#include "blk-crypto-internal.h"

static unsigned int num_prealloc_bounce_pg = BIO_MAX_VECS;
module_param(num_prealloc_bounce_pg, uint, 0);
MODULE_PARM_DESC(num_prealloc_bounce_pg,
		 "Number of preallocated bounce pages for the blk-crypto crypto API fallback");

static unsigned int blk_crypto_num_keyslots = 100;
module_param_named(num_keyslots, blk_crypto_num_keyslots, uint, 0);
MODULE_PARM_DESC(num_keyslots,
		 "Number of keyslots for the blk-crypto crypto API fallback");

static unsigned int num_prealloc_fallback_crypt_ctxs = 128;
module_param(num_prealloc_fallback_crypt_ctxs, uint, 0);
MODULE_PARM_DESC(num_prealloc_crypt_fallback_ctxs,
		 "Number of preallocated bio fallback crypto contexts for blk-crypto to use during crypto API fallback");

struct bio_fallback_crypt_ctx {
	struct bio_crypt_ctx crypt_ctx;
	/*
	 * Copy of the bvec_iter when this bio was submitted.
	 * We only want to en/decrypt the part of the bio as described by the
	 * bvec_iter upon submission because bio might be split before being
	 * resubmitted
	 */
	struct bvec_iter crypt_iter;
	union {
		struct {
			struct work_struct work;
			struct bio *bio;
		};
		struct {
			void *bi_private_orig;
			bio_end_io_t *bi_end_io_orig;
		};
	};
};

static struct kmem_cache *bio_fallback_crypt_ctx_cache;
static mempool_t *bio_fallback_crypt_ctx_pool;

/*
 * Allocating a crypto tfm during I/O can deadlock, so we have to preallocate
 * all of a mode's tfms when that mode starts being used. Since each mode may
 * need all the keyslots at some point, each mode needs its own tfm for each
 * keyslot; thus, a keyslot may contain tfms for multiple modes.  However, to
 * match the behavior of real inline encryption hardware (which only supports a
 * single encryption context per keyslot), we only allow one tfm per keyslot to
 * be used at a time - the rest of the unused tfms have their keys cleared.
 */
static DEFINE_MUTEX(tfms_init_lock);
static bool tfms_inited[BLK_ENCRYPTION_MODE_MAX];

static struct blk_crypto_fallback_keyslot {
	enum blk_crypto_mode_num crypto_mode;
	struct crypto_sync_skcipher *tfms[BLK_ENCRYPTION_MODE_MAX];
} *blk_crypto_keyslots;

static struct blk_crypto_profile *blk_crypto_fallback_profile;
static struct workqueue_struct *blk_crypto_wq;
static mempool_t *blk_crypto_bounce_page_pool;
static struct kmem_cache *blk_crypto_du_ctx_cache;
static mempool_t *blk_crypto_du_ctx_pool;
static struct bio_set enc_bio_set;

/*
 * The maximum number of pages a data unit may span: the largest power of
 * two that fits in one encrypted bio.  An encrypted bio holds at most
 * BIO_MAX_VECS bio_vecs, and a data unit can need one more bio_vec than
 * the pages it spans when a bio_vec boundary splits it.
 */
#define BLK_CRYPTO_DU_MAX_PAGES		BIT(ilog2(BIO_MAX_VECS - 1))

/*
 * Data unit sizes that blk-crypto-fallback can en/decrypt: at least a
 * sector, and small enough that the bio_vecs of a data unit fit in one
 * encrypted bio.
 */
#define BLK_CRYPTO_FALLBACK_DU_MASK \
	GENMASK(ilog2(BLK_CRYPTO_DU_MAX_PAGES * PAGE_SIZE), SECTOR_SHIFT)

/*
 * Number of scratch buffers to keep around: mempool_alloc() only uses them
 * when the page allocator fails, so this is the number of bios that can
 * still make progress when memory is short.
 */
#define BLK_CRYPTO_DU_CTX_POOL_SIZE	8

/*
 * Size of one blk_crypto_du_ctx_pool allocation: the bio_vecs of the largest
 * data unit, and a scatterlist for each side of the crypto request.
 */
#define BLK_CRYPTO_DU_CTX_SIZE \
	((BLK_CRYPTO_DU_MAX_PAGES + 1) * \
	(sizeof(struct bio_vec) + 2 * sizeof(struct scatterlist)))

/*
 * This is the key we set when evicting a keyslot. This *should* be the all 0's
 * key, but AES-XTS rejects that key, so we use some random bytes instead.
 */
static u8 blank_key[BLK_CRYPTO_MAX_RAW_KEY_SIZE];

static void blk_crypto_fallback_evict_keyslot(unsigned int slot)
{
	struct blk_crypto_fallback_keyslot *slotp = &blk_crypto_keyslots[slot];
	enum blk_crypto_mode_num crypto_mode = slotp->crypto_mode;
	int err;

	WARN_ON(slotp->crypto_mode == BLK_ENCRYPTION_MODE_INVALID);

	/* Clear the key in the skcipher */
	err = crypto_sync_skcipher_setkey(slotp->tfms[crypto_mode], blank_key,
				     blk_crypto_modes[crypto_mode].keysize);
	WARN_ON(err);
	slotp->crypto_mode = BLK_ENCRYPTION_MODE_INVALID;
}

static int
blk_crypto_fallback_keyslot_program(struct blk_crypto_profile *profile,
				    const struct blk_crypto_key *key,
				    unsigned int slot)
{
	struct blk_crypto_fallback_keyslot *slotp = &blk_crypto_keyslots[slot];
	const enum blk_crypto_mode_num crypto_mode =
						key->crypto_cfg.crypto_mode;
	int err;

	if (crypto_mode != slotp->crypto_mode &&
	    slotp->crypto_mode != BLK_ENCRYPTION_MODE_INVALID)
		blk_crypto_fallback_evict_keyslot(slot);

	slotp->crypto_mode = crypto_mode;
	err = crypto_sync_skcipher_setkey(slotp->tfms[crypto_mode], key->bytes,
				     key->size);
	if (err) {
		blk_crypto_fallback_evict_keyslot(slot);
		return err;
	}
	return 0;
}

static int blk_crypto_fallback_keyslot_evict(struct blk_crypto_profile *profile,
					     const struct blk_crypto_key *key,
					     unsigned int slot)
{
	blk_crypto_fallback_evict_keyslot(slot);
	return 0;
}

static const struct blk_crypto_ll_ops blk_crypto_fallback_ll_ops = {
	.keyslot_program        = blk_crypto_fallback_keyslot_program,
	.keyslot_evict          = blk_crypto_fallback_keyslot_evict,
};

static void blk_crypto_free_enc_pages(struct page **pages, unsigned int nr)
{
	unsigned int i;

	if (!nr)
		return;
	i = mempool_free_bulk(blk_crypto_bounce_page_pool, (void **)pages, nr);
	if (i < nr)
		release_pages(pages + i, nr - i);
}

static void blk_crypto_fallback_encrypt_endio(struct bio *enc_bio)
{
	struct bio *src_bio = enc_bio->bi_private;
	struct page **pages = (struct page **)enc_bio->bi_io_vec;
	struct bio_vec *bv;
	unsigned int i;

	/*
	 * Use the same trick as the alloc side to avoid the need for an extra
	 * pages array.
	 */
	bio_for_each_bvec_all(bv, enc_bio, i)
		pages[i] = bv->bv_page;

	blk_crypto_free_enc_pages(pages, enc_bio->bi_vcnt);

	if (enc_bio->bi_status)
		cmpxchg(&src_bio->bi_status, 0, enc_bio->bi_status);

	bio_put(enc_bio);
	bio_endio(src_bio);
}

#define PAGE_PTRS_PER_BVEC     (sizeof(struct bio_vec) / sizeof(struct page *))

static struct bio *blk_crypto_alloc_enc_bio(struct bio *bio_src,
		unsigned int nr_segs, struct page ***pages_ret)
{
	unsigned int memflags = memalloc_noio_save();
	unsigned int nr_allocated;
	struct page **pages;
	struct bio *bio;

	bio = bio_alloc_bioset(bio_src->bi_bdev, nr_segs, bio_src->bi_opf,
			GFP_NOIO, &enc_bio_set);
	if (bio_flagged(bio_src, BIO_REMAPPED))
		bio_set_flag(bio, BIO_REMAPPED);
	bio->bi_private		= bio_src;
	bio->bi_end_io		= blk_crypto_fallback_encrypt_endio;
	bio->bi_ioprio		= bio_src->bi_ioprio;
	bio->bi_write_hint	= bio_src->bi_write_hint;
	bio->bi_write_stream	= bio_src->bi_write_stream;
	bio->bi_iter.bi_sector	= bio_src->bi_iter.bi_sector;
	bio_clone_blkg_association(bio, bio_src);

	/*
	 * Move page array up in the allocated memory for the bio vecs as far as
	 * possible so that we can start filling biovecs from the beginning
	 * without overwriting the temporary page array.
	 */
	static_assert(PAGE_PTRS_PER_BVEC > 1);
	pages = (struct page **)bio->bi_io_vec;
	pages += nr_segs * (PAGE_PTRS_PER_BVEC - 1);

	/*
	 * Try a bulk allocation first.  This might not fill all allocated
	 * pages, but we'll fix that up later in mempool_alloc_bulk.
	 *
	 * Note: alloc_pages_bulk needs the array to be zeroed, as it assumes
	 * any non-zero slot already contains a valid allocation.
	 */
	memset(pages, 0, sizeof(struct page *) * nr_segs);
	nr_allocated = alloc_pages_bulk(GFP_KERNEL, nr_segs, pages);
	if (nr_allocated < nr_segs)
		mempool_alloc_bulk(blk_crypto_bounce_page_pool,
				(void **)pages + nr_allocated,
				nr_segs - nr_allocated);
	memalloc_noio_restore(memflags);
	*pages_ret = pages;
	return bio;
}

static struct crypto_sync_skcipher *
blk_crypto_fallback_tfm(struct blk_crypto_keyslot *slot)
{
	const struct blk_crypto_fallback_keyslot *slotp =
		&blk_crypto_keyslots[blk_crypto_keyslot_index(slot)];

	return slotp->tfms[slotp->crypto_mode];
}

union blk_crypto_iv {
	__le64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE];
	u8 bytes[BLK_CRYPTO_MAX_IV_SIZE];
};

static void blk_crypto_dun_to_iv(const u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
				 union blk_crypto_iv *iv)
{
	int i;

	for (i = 0; i < BLK_CRYPTO_DUN_ARRAY_SIZE; i++)
		iv->dun[i] = cpu_to_le64(dun[i]);
}

/*
 * Describes one data unit: the bio_vecs that cover it and the
 * scatterlists to en/decrypt it with.
 */
struct blk_crypto_du_ctx {
	/* Backing store for bvecs, for the one-bio_vec case. */
	struct bio_vec		stack_bvec;
	/* Backing store for src_sg and dst_sg, one entry each. */
	struct scatterlist	stack_sgs[2];
	/* Backing store for bvecs and the scatterlists, if not on the stack. */
	void			*pool_buf;

	/* The data unit size in bytes. */
	unsigned int		du_size;
	/* Number of entries available in bvecs. */
	unsigned int		max_bvecs;

	/* The bio_vecs that cover the data unit. */
	struct bio_vec		*bvecs;
	/* The scatterlist for each side of the crypto request. */
	struct scatterlist	*src_sg;
	struct scatterlist	*dst_sg;

	/* The crypto request to en/decrypt the data unit with. */
	struct skcipher_request	*ciph_req;
	/* The data unit's DUN, advanced on each successful crypt. */
	u64			curr_dun[BLK_CRYPTO_DUN_ARRAY_SIZE];
};

/*
 * Prepare @du_ctx for the data unit of @bc's key.  A data unit that needs
 * more than one bio_vec gets its arrays from blk_crypto_du_ctx_pool instead
 * of the ones in @du_ctx; that never fails, as the pool is a mempool.
 */
static void blk_crypto_du_ctx_init(struct blk_crypto_du_ctx *du_ctx,
				   struct skcipher_request *ciph_req,
				   const struct bio_crypt_ctx *bc)
{
	du_ctx->ciph_req = ciph_req;
	du_ctx->du_size = bc->bc_key->crypto_cfg.data_unit_size;
	memcpy(du_ctx->curr_dun, bc->bc_dun, sizeof(bc->bc_dun));

	if (du_ctx->du_size <= PAGE_SIZE) {
		du_ctx->max_bvecs = 1;
		du_ctx->bvecs = &du_ctx->stack_bvec;
		du_ctx->src_sg = &du_ctx->stack_sgs[0];
		du_ctx->dst_sg = &du_ctx->stack_sgs[1];
		du_ctx->pool_buf = NULL;
	} else {
		/* blk_crypto_fallback_bio_prep() checks the pool is set up. */
		du_ctx->max_bvecs = PFN_UP(du_ctx->du_size) + 1;
		du_ctx->pool_buf = mempool_alloc(blk_crypto_du_ctx_pool,
						 GFP_NOIO);
		du_ctx->bvecs = du_ctx->pool_buf;
		du_ctx->src_sg = du_ctx->pool_buf +
				 du_ctx->max_bvecs * sizeof(*du_ctx->bvecs);
		du_ctx->dst_sg = du_ctx->src_sg + du_ctx->max_bvecs;
	}
}

static void blk_crypto_du_ctx_exit(struct blk_crypto_du_ctx *du_ctx)
{
	mempool_free(du_ctx->pool_buf, blk_crypto_du_ctx_pool);
}

/*
 * En/decrypt one data unit: run the request over the data unit's
 * scatterlists with its DUN as the IV, and advance the DUN on success.
 */
static int blk_crypto_crypt_du(struct blk_crypto_du_ctx *du_ctx, bool encrypt)
{
	union blk_crypto_iv iv;
	int err;

	blk_crypto_dun_to_iv(du_ctx->curr_dun, &iv);
	skcipher_request_set_crypt(du_ctx->ciph_req,
				   du_ctx->src_sg, du_ctx->dst_sg,
				   du_ctx->du_size, iv.bytes);

	if (encrypt)
		err = crypto_skcipher_encrypt(du_ctx->ciph_req);
	else
		err = crypto_skcipher_decrypt(du_ctx->ciph_req);
	if (err)
		return -EIO;

	bio_crypt_dun_increment(du_ctx->curr_dun, 1);
	return 0;
}

/**
 * blk_crypto_du_bvecs() - collect the bio_vecs that make up one data unit
 * @bio: the bio to collect from
 * @iter: the position in @bio to collect at.  Not modified.
 * @du_ctx: the context to collect the data unit into.  Uses
 *	    @du_ctx->du_size and @du_ctx->max_bvecs, and stores the
 *	    bio_vecs in @du_ctx->bvecs
 *
 * Collect the bio_vecs that cover one data unit starting at @iter.  Each
 * bio_vec is clamped to its page, so a data unit spans several bio_vecs if it
 * covers several pages or if a bio_vec boundary splits it.
 *
 * Return: the number of bio_vecs collected, or 0 if @bio doesn't have a whole
 *	   data unit at @iter, the data unit doesn't start at a
 *	   min(@du_ctx->du_size, PAGE_SIZE) boundary, or the data unit
 *	   needs more than @du_ctx->max_bvecs bio_vecs.
 */
static unsigned int blk_crypto_du_bvecs(struct bio *bio,
					const struct bvec_iter *iter,
					struct blk_crypto_du_ctx *du_ctx)
{
	const unsigned int align = min(du_ctx->du_size, PAGE_SIZE);
	struct bvec_iter du_iter = *iter;
	struct bio_vec bv;
	unsigned int n = 0;

	if (du_iter.bi_size < du_ctx->du_size)
		return 0;

	/* Walk a copy of the iterator that ends after one data unit. */
	du_iter.bi_size = du_ctx->du_size;

	__bio_for_each_segment(bv, bio, du_iter, du_iter) {
		if (n == du_ctx->max_bvecs)
			return 0;
		if (!n && !IS_ALIGNED(bv.bv_offset, align))
			return 0;

		du_ctx->bvecs[n++] = bv;
	}

	return n;
}

static void __blk_crypto_fallback_encrypt_bio(struct bio *src_bio,
		struct crypto_sync_skcipher *tfm)
{
	struct bio_crypt_ctx *bc = src_bio->bi_crypt_context;
	SYNC_SKCIPHER_REQUEST_ON_STACK(ciph_req, tfm);
	struct blk_crypto_du_ctx du_ctx;
	unsigned int nr_enc_pages, enc_idx, nr_new_pages;
	struct page **enc_pages;
	struct page *cur_enc_page = NULL;
	struct bio *enc_bio;
	bool reuse_enc_page;
	unsigned int i, n;

	skcipher_request_set_callback(ciph_req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			NULL, NULL);
	blk_crypto_du_ctx_init(&du_ctx, ciph_req, bc);

	/*
	 * Encrypt each data unit in the source bio.  A data unit is described by
	 * one bio_vec per page, and may span more than a single page.  Because
	 * the encrypted bios are limited to a single page per bio_vec, this can
	 * generate more than a single encrypted bio per source bio.
	 */
new_bio:
	nr_enc_pages = min(bio_segments(src_bio), BIO_MAX_VECS);
	enc_bio = blk_crypto_alloc_enc_bio(src_bio, nr_enc_pages, &enc_pages);
	enc_idx = 0;
	reuse_enc_page = false;
	for (;;) {
		struct bio_vec src_bv =
			bio_iter_iovec(src_bio, src_bio->bi_iter);

		n = blk_crypto_du_bvecs(src_bio, &src_bio->bi_iter, &du_ctx);
		if (!n) {
			enc_bio->bi_status = BLK_STS_INVAL;
			goto out_free_enc_bio;
		}

		/*
		 * The first bio_vec of the data unit reuses the bounce page
		 * that the previous data unit added for the bio_vec it ended
		 * in, if any; every other bio_vec needs a bounce page of its
		 * own.
		 *
		 * reuse_enc_page implies nr_new_pages == 0: a data unit that
		 * reuses a bounce page fits in a single bio_vec, as max_bvecs
		 * is 1 for data units up to a page (see
		 * blk_crypto_du_ctx_init()).  A submit therefore only happens
		 * at a segment boundary and the encrypted bio never extends
		 * past the encrypted data.
		 */
		nr_new_pages = n - reuse_enc_page;
		WARN_ON_ONCE(reuse_enc_page && nr_new_pages);

		/*
		 * Is there room for this data unit in the current encrypted bio?
		 * (There always is in a newly allocated one.)
		 */
		if (enc_idx + nr_new_pages > nr_enc_pages) {
			if (WARN_ON_ONCE(!enc_idx)) {
				enc_bio->bi_status = BLK_STS_INVAL;
				goto out_free_enc_bio;
			}
			blk_crypto_free_enc_pages(enc_pages + enc_idx,
						  nr_enc_pages - enc_idx);
			/*
			 * For each additional encrypted bio submitted,
			 * increment the source bio's remaining count.  Each
			 * encrypted bio's completion handler calls bio_endio on
			 * the source bio, so this keeps the source bio from
			 * completing until the last encrypted bio does.
			 */
			bio_inc_remaining(src_bio);
			submit_bio(enc_bio);
			goto new_bio;
		}

		/*
		 * Encrypt the data unit, scattering the ciphertext into the
		 * bounce pages.  The bounce pages mirror the data layout of the
		 * source bio, so that the encrypted bio can be submitted for the
		 * same disk location as the source bio.
		 */
		sg_init_table(du_ctx.src_sg, n);
		sg_init_table(du_ctx.dst_sg, n);
		for (i = 0; i < n; i++) {
			struct page *enc_page;

			if (!i && reuse_enc_page) {
				/* Covered by the previous bounce page. */
				enc_page = cur_enc_page;
			} else {
				/*
				 * The whole bio_vec is added for the first
				 * bio_vec of a data unit; the data units that
				 * follow it fill in the same bounce page
				 * before the encrypted bio is submitted.
				 */
				unsigned int len = i ? du_ctx.bvecs[i].bv_len :
						      src_bv.bv_len;
				unsigned int off = i ? du_ctx.bvecs[i].bv_offset :
						      src_bv.bv_offset;

				enc_page = enc_pages[enc_idx++];
				__bio_add_page(enc_bio, enc_page, len, off);
				cur_enc_page = enc_page;
			}

			sg_set_page(&du_ctx.src_sg[i], du_ctx.bvecs[i].bv_page,
				    du_ctx.bvecs[i].bv_len,
				    du_ctx.bvecs[i].bv_offset);
			sg_set_page(&du_ctx.dst_sg[i], enc_page,
				    du_ctx.bvecs[i].bv_len,
				    du_ctx.bvecs[i].bv_offset);
		}

		/*
		 * The next data unit starts in the same bio_vec, and thus
		 * shares the bounce page added above, unless this data unit
		 * reaches the end of the bio_vec it is in.  A page can be
		 * covered by several bio_vecs, so the sharing is per bio_vec,
		 * not per page.
		 */
		reuse_enc_page = n == 1 && src_bv.bv_len > du_ctx.du_size;

		if (blk_crypto_crypt_du(&du_ctx, true)) {
			enc_bio->bi_status = BLK_STS_IOERR;
			goto out_free_enc_bio;
		}

		/* A data unit can span several bio_vecs, e.g. for direct I/O. */
		bio_advance_iter(src_bio, &src_bio->bi_iter, du_ctx.du_size);
		if (!src_bio->bi_iter.bi_size)
			break;
	}

	blk_crypto_du_ctx_exit(&du_ctx);
	blk_crypto_free_enc_pages(enc_pages + enc_idx, nr_enc_pages - enc_idx);
	submit_bio(enc_bio);
	return;

out_free_enc_bio:
	blk_crypto_du_ctx_exit(&du_ctx);
	blk_crypto_free_enc_pages(enc_pages + enc_idx, nr_enc_pages - enc_idx);
	bio_endio(enc_bio);
}

/*
 * The crypto API fallback's encryption routine.
 *
 * Allocate one or more bios for encryption, encrypt the input bio using the
 * crypto API, and submit the encrypted bios.  Sets bio->bi_status and
 * completes the source bio on error
 */
static void blk_crypto_fallback_encrypt_bio(struct bio *src_bio)
{
	struct bio_crypt_ctx *bc = src_bio->bi_crypt_context;
	struct blk_crypto_keyslot *slot;
	blk_status_t status;

	status = blk_crypto_get_keyslot(blk_crypto_fallback_profile,
					bc->bc_key, &slot);
	if (status != BLK_STS_OK) {
		bio_endio_status(src_bio, status);
		return;
	}
	__blk_crypto_fallback_encrypt_bio(src_bio,
			blk_crypto_fallback_tfm(slot));
	blk_crypto_put_keyslot(slot);
}

static blk_status_t __blk_crypto_fallback_decrypt_bio(struct bio *bio,
		struct bio_crypt_ctx *bc, struct bvec_iter iter,
		struct crypto_sync_skcipher *tfm)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(ciph_req, tfm);
	struct blk_crypto_du_ctx du_ctx;
	blk_status_t status = BLK_STS_OK;
	unsigned int i, n;

	skcipher_request_set_callback(ciph_req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			NULL, NULL);
	blk_crypto_du_ctx_init(&du_ctx, ciph_req, bc);
	/* Decrypt in place, so both sides use the same scatterlist. */
	du_ctx.dst_sg = du_ctx.src_sg;

	/* Decrypt each data unit in the bio, in place */
	while (iter.bi_size) {
		n = blk_crypto_du_bvecs(bio, &iter, &du_ctx);
		if (!n) {
			status = BLK_STS_INVAL;
			break;
		}

		sg_init_table(du_ctx.src_sg, n);
		for (i = 0; i < n; i++)
			sg_set_page(&du_ctx.src_sg[i], du_ctx.bvecs[i].bv_page,
				    du_ctx.bvecs[i].bv_len,
				    du_ctx.bvecs[i].bv_offset);

		if (blk_crypto_crypt_du(&du_ctx, false)) {
			status = BLK_STS_IOERR;
			break;
		}

		/* A data unit can span several bio_vecs, e.g. for direct I/O. */
		bio_advance_iter(bio, &iter, du_ctx.du_size);
	}

	blk_crypto_du_ctx_exit(&du_ctx);
	return status;
}

/*
 * The crypto API fallback's main decryption routine.
 *
 * Decrypts input bio in place, and calls bio_endio on the bio.
 */
static void blk_crypto_fallback_decrypt_bio(struct work_struct *work)
{
	struct bio_fallback_crypt_ctx *f_ctx =
		container_of(work, struct bio_fallback_crypt_ctx, work);
	struct bio *bio = f_ctx->bio;
	struct bio_crypt_ctx *bc = &f_ctx->crypt_ctx;
	struct blk_crypto_keyslot *slot;
	blk_status_t status;

	status = blk_crypto_get_keyslot(blk_crypto_fallback_profile,
					bc->bc_key, &slot);
	if (status == BLK_STS_OK) {
		status = __blk_crypto_fallback_decrypt_bio(bio, bc,
				f_ctx->crypt_iter,
				blk_crypto_fallback_tfm(slot));
		blk_crypto_put_keyslot(slot);
	}
	mempool_free(f_ctx, bio_fallback_crypt_ctx_pool);

	bio_endio_status(bio, status);
}

/**
 * blk_crypto_fallback_decrypt_endio - queue bio for fallback decryption
 *
 * @bio: the bio to queue
 *
 * Restore bi_private and bi_end_io, and queue the bio for decryption into a
 * workqueue, since this function will be called from an atomic context.
 */
static void blk_crypto_fallback_decrypt_endio(struct bio *bio)
{
	struct bio_fallback_crypt_ctx *f_ctx = bio->bi_private;

	bio->bi_private = f_ctx->bi_private_orig;
	bio->bi_end_io = f_ctx->bi_end_io_orig;

	/* If there was an IO error, don't queue for decrypt. */
	if (bio->bi_status) {
		mempool_free(f_ctx, bio_fallback_crypt_ctx_pool);
		bio_endio(bio);
		return;
	}

	INIT_WORK(&f_ctx->work, blk_crypto_fallback_decrypt_bio);
	f_ctx->bio = bio;
	queue_work(blk_crypto_wq, &f_ctx->work);
}

/**
 * blk_crypto_fallback_bio_prep - Prepare a bio to use fallback en/decryption
 * @bio: bio to prepare
 *
 * If bio is doing a WRITE operation, allocate one or more bios to contain the
 * encrypted payload and submit them.
 *
 * For a READ operation, mark the bio for decryption by using bi_private and
 * bi_end_io.
 *
 * In either case, this function will make the submitted bio(s) look like
 * regular bios (i.e. as if no encryption context was ever specified) for the
 * purposes of the rest of the stack except for blk-integrity (blk-integrity and
 * blk-crypto are not currently supported together).
 *
 * Return: true if @bio should be submitted to the driver by the caller, else
 * false.  Sets bio->bi_status, calls bio_endio and returns false on error.
 */
bool blk_crypto_fallback_bio_prep(struct bio *bio)
{
	struct bio_crypt_ctx *bc = bio->bi_crypt_context;
	struct bio_fallback_crypt_ctx *f_ctx;

	if (WARN_ON_ONCE(!tfms_inited[bc->bc_key->crypto_cfg.crypto_mode])) {
		/* User didn't call blk_crypto_start_using_key() first */
		bio_io_error(bio);
		return false;
	}

	if (bc->bc_key->crypto_cfg.key_type != BLK_CRYPTO_KEY_TYPE_RAW) {
		bio_endio_status(bio, BLK_STS_NOTSUPP);
		return false;
	}
	if (WARN_ON_ONCE(!(BLK_CRYPTO_FALLBACK_DU_MASK &
			   bc->bc_key->crypto_cfg.data_unit_size))) {
		/* The key was set up for a data unit the fallback can't handle. */
		bio_endio_status(bio, BLK_STS_NOTSUPP);
		return false;
	}
	if (WARN_ON_ONCE(bc->bc_key->crypto_cfg.data_unit_size > PAGE_SIZE &&
			 !blk_crypto_du_ctx_pool)) {
		/* The key was set up without the scratch space it needs. */
		bio_io_error(bio);
		return false;
	}

	if (bio_data_dir(bio) == WRITE) {
		blk_crypto_fallback_encrypt_bio(bio);
		return false;
	}

	/*
	 * bio READ case: Set up a f_ctx in the bio's bi_private and set the
	 * bi_end_io appropriately to trigger decryption when the bio is ended.
	 */
	f_ctx = mempool_alloc(bio_fallback_crypt_ctx_pool, GFP_NOIO);
	f_ctx->crypt_ctx = *bc;
	f_ctx->crypt_iter = bio->bi_iter;
	f_ctx->bi_private_orig = bio->bi_private;
	f_ctx->bi_end_io_orig = bio->bi_end_io;
	bio->bi_private = (void *)f_ctx;
	bio->bi_end_io = blk_crypto_fallback_decrypt_endio;
	bio_crypt_free_ctx(bio);

	return true;
}

int blk_crypto_fallback_evict_key(const struct blk_crypto_key *key)
{
	return __blk_crypto_evict_key(blk_crypto_fallback_profile, key);
}

static bool blk_crypto_fallback_inited;
static int blk_crypto_fallback_init(void)
{
	int i;
	int err;

	if (blk_crypto_fallback_inited)
		return 0;

	get_random_bytes(blank_key, sizeof(blank_key));

	err = bioset_init(&enc_bio_set, 64, 0, BIOSET_NEED_BVECS);
	if (err)
		goto out;

	/* Dynamic allocation is needed because of lockdep_register_key(). */
	blk_crypto_fallback_profile = kzalloc_obj(*blk_crypto_fallback_profile);
	if (!blk_crypto_fallback_profile) {
		err = -ENOMEM;
		goto fail_free_bioset;
	}

	err = blk_crypto_profile_init(blk_crypto_fallback_profile,
				      blk_crypto_num_keyslots);
	if (err)
		goto fail_free_profile;
	err = -ENOMEM;

	blk_crypto_fallback_profile->ll_ops = blk_crypto_fallback_ll_ops;
	blk_crypto_fallback_profile->max_dun_bytes_supported = BLK_CRYPTO_MAX_IV_SIZE;
	blk_crypto_fallback_profile->key_types_supported = BLK_CRYPTO_KEY_TYPE_RAW;

	/* All blk-crypto modes have a crypto API fallback. */
	for (i = 0; i < BLK_ENCRYPTION_MODE_MAX; i++)
		blk_crypto_fallback_profile->modes_supported[i] =
			BLK_CRYPTO_FALLBACK_DU_MASK;
	blk_crypto_fallback_profile->modes_supported[BLK_ENCRYPTION_MODE_INVALID] = 0;

	blk_crypto_wq = alloc_workqueue("blk_crypto_wq",
					WQ_UNBOUND | WQ_HIGHPRI |
					WQ_MEM_RECLAIM, num_online_cpus());
	if (!blk_crypto_wq)
		goto fail_destroy_profile;

	blk_crypto_keyslots = kzalloc_objs(blk_crypto_keyslots[0],
					   blk_crypto_num_keyslots);
	if (!blk_crypto_keyslots)
		goto fail_free_wq;

	blk_crypto_bounce_page_pool =
		mempool_create_page_pool(num_prealloc_bounce_pg, 0);
	if (!blk_crypto_bounce_page_pool)
		goto fail_free_keyslots;

	bio_fallback_crypt_ctx_cache = KMEM_CACHE(bio_fallback_crypt_ctx, 0);
	if (!bio_fallback_crypt_ctx_cache)
		goto fail_free_bounce_page_pool;

	bio_fallback_crypt_ctx_pool =
		mempool_create_slab_pool(num_prealloc_fallback_crypt_ctxs,
					 bio_fallback_crypt_ctx_cache);
	if (!bio_fallback_crypt_ctx_pool)
		goto fail_free_crypt_ctx_cache;

	blk_crypto_fallback_inited = true;

	return 0;
fail_free_crypt_ctx_cache:
	kmem_cache_destroy(bio_fallback_crypt_ctx_cache);
fail_free_bounce_page_pool:
	mempool_destroy(blk_crypto_bounce_page_pool);
fail_free_keyslots:
	kfree(blk_crypto_keyslots);
fail_free_wq:
	destroy_workqueue(blk_crypto_wq);
fail_destroy_profile:
	blk_crypto_profile_destroy(blk_crypto_fallback_profile);
fail_free_profile:
	kfree(blk_crypto_fallback_profile);
fail_free_bioset:
	bioset_exit(&enc_bio_set);
out:
	return err;
}

/*
 * Prepare blk-crypto-fallback for the specified crypto configuration.
 * Returns -EOPNOTSUPP if the fallback can't handle the data unit size of the
 * configuration, -ENOPKG if the needed crypto API support is missing, or
 * -ENOMEM if the scratch space for a data unit larger than a page can't be
 * allocated.
 */
int blk_crypto_fallback_start_using_key(const struct blk_crypto_config *cfg)
{
	enum blk_crypto_mode_num mode_num = cfg->crypto_mode;
	const char *cipher_str = blk_crypto_modes[mode_num].cipher_str;
	struct blk_crypto_fallback_keyslot *slotp;
	unsigned int i;
	int err = 0;

	if (!(BLK_CRYPTO_FALLBACK_DU_MASK & cfg->data_unit_size)) {
		pr_warn_ratelimited("can't handle data unit size %u\n",
				    cfg->data_unit_size);
		return -EOPNOTSUPP;
	}

	/*
	 * A data unit that needs more than one bio_vec, e.g. a data unit that
	 * is larger than a page, gets its data unit context from this pool.
	 * Set the pool up before the fast path below, which can't create it.
	 */
	if (cfg->data_unit_size > PAGE_SIZE) {
		mutex_lock(&tfms_init_lock);
		if (!blk_crypto_du_ctx_pool) {
			blk_crypto_du_ctx_cache =
				kmem_cache_create("blk_crypto_du_ctx",
						  BLK_CRYPTO_DU_CTX_SIZE,
						  0, 0, NULL);
			if (!blk_crypto_du_ctx_cache) {
				mutex_unlock(&tfms_init_lock);
				return -ENOMEM;
			}
			blk_crypto_du_ctx_pool =
				mempool_create_slab_pool(BLK_CRYPTO_DU_CTX_POOL_SIZE,
							 blk_crypto_du_ctx_cache);
			if (!blk_crypto_du_ctx_pool) {
				kmem_cache_destroy(blk_crypto_du_ctx_cache);
				blk_crypto_du_ctx_cache = NULL;
				mutex_unlock(&tfms_init_lock);
				return -ENOMEM;
			}
		}
		mutex_unlock(&tfms_init_lock);
	}

	/*
	 * Fast path
	 * Ensure that updates to blk_crypto_keyslots[i].tfms[mode_num]
	 * for each i are visible before we try to access them.
	 */
	if (likely(smp_load_acquire(&tfms_inited[mode_num])))
		return 0;

	mutex_lock(&tfms_init_lock);
	if (tfms_inited[mode_num])
		goto out;

	err = blk_crypto_fallback_init();
	if (err)
		goto out;

	for (i = 0; i < blk_crypto_num_keyslots; i++) {
		slotp = &blk_crypto_keyslots[i];
		slotp->tfms[mode_num] = crypto_alloc_sync_skcipher(cipher_str,
				0, 0);
		if (IS_ERR(slotp->tfms[mode_num])) {
			err = PTR_ERR(slotp->tfms[mode_num]);
			if (err == -ENOENT) {
				pr_warn_once("Missing crypto API support for \"%s\"\n",
					     cipher_str);
				err = -ENOPKG;
			}
			slotp->tfms[mode_num] = NULL;
			goto out_free_tfms;
		}

		crypto_sync_skcipher_set_flags(slotp->tfms[mode_num],
					  CRYPTO_TFM_REQ_FORBID_WEAK_KEYS);
	}

	/*
	 * Ensure that updates to blk_crypto_keyslots[i].tfms[mode_num]
	 * for each i are visible before we set tfms_inited[mode_num].
	 */
	smp_store_release(&tfms_inited[mode_num], true);
	goto out;

out_free_tfms:
	for (i = 0; i < blk_crypto_num_keyslots; i++) {
		slotp = &blk_crypto_keyslots[i];
		crypto_free_sync_skcipher(slotp->tfms[mode_num]);
		slotp->tfms[mode_num] = NULL;
	}
out:
	mutex_unlock(&tfms_init_lock);
	return err;
}
