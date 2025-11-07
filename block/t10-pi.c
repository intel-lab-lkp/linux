// SPDX-License-Identifier: GPL-2.0
/*
 * t10_pi.c - Functions for generating and verifying T10 Protection
 *	      Information.
 */

#include <linux/t10-pi.h>
#include <linux/blk-integrity.h>
#include <linux/crc-t10dif.h>
#include <linux/crc64.h>
#include <net/checksum.h>
#include <linux/unaligned.h>
#include "blk.h"

#define APP_TAG_ESCAPE 0xffff
#define REF_TAG_ESCAPTE 0xffffffff

/**
 * This union is used for onstack allocations when the pi field is split across
 * segments. blk_validate_integrity_limits() guarantees pi_tuple_size matches
 * the sizeof one of these two types.
 */
union pi_tuple {
	struct crc64_pi_tuple	crc64_pi;
	struct t10_pi_tuple	t10_pi;
};

struct blk_integrity_iter {
	struct bio			*bio;
	struct bio_integrity_payload	*bip;
	struct blk_integrity		*bi;
	struct bvec_iter		data_iter;
	struct bvec_iter		prot_iter;
	unsigned int			interval_remaining;
	u64				seed;
	u64				crc;
};

static void blk_crc(struct blk_integrity_iter *iter, void *data,
		    unsigned int len)
{
	switch (iter->bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		iter->crc = crc64_nvme(iter->crc, data, len);
		break;
	case BLK_INTEGRITY_CSUM_CRC:
		iter->crc = crc_t10dif_update(iter->crc, data, len);
		break;
	case BLK_INTEGRITY_CSUM_IP:
		iter->crc = (__force u32)csum_partial(data, len,
						(__force __wsum)iter->crc);
		break;
	default:
		WARN_ON_ONCE(1);
		iter->crc = U64_MAX;
		break;
	}
}

/**
 * blk_integrity_crc_offset - update the crc for formats that have metadata
 * 			      padding in front of the data integrity field
 */
static void blk_integrity_crc_offset(struct blk_integrity_iter *iter)
{
	unsigned int offset = iter->bi->pi_offset;
	struct bio_vec *bvec = iter->bip->bip_vec;

	while (offset > 0) {
		struct bio_vec pbv = mp_bvec_iter_bvec(bvec, iter->prot_iter);
		unsigned int len = min(pbv.bv_len, offset);
		void *prot_buf = bvec_kmap_local(&pbv);

		blk_crc(iter, prot_buf, len);
		kunmap_local(prot_buf);
		offset -= len;
		bvec_iter_advance_single(bvec, &iter->prot_iter, len);
	}
}

/**
 * __blk_integrity_copy_from_tuple() - copy from @tuple to @iter
 */
static void __blk_integrity_copy_from_tuple(struct bio_integrity_payload *bip,
					    struct bvec_iter *iter, void *tuple,
					    unsigned int tuple_size)
{
	void *prot_buf;

	while (tuple_size) {
		struct bio_vec pbv = mp_bvec_iter_bvec(bip->bip_vec, *iter);
		unsigned int len = min(tuple_size, pbv.bv_len);

		prot_buf = bvec_kmap_local(&pbv);
		memcpy(prot_buf, tuple, len);
		kunmap_local(prot_buf);

		bvec_iter_advance_single(bip->bip_vec, iter, len);
		tuple_size -= len;
		tuple += len;
	}
}

static void blk_integrity_copy_from_tuple(struct blk_integrity_iter *iter,
					  void *tuple)
{
	__blk_integrity_copy_from_tuple(iter->bip, &iter->prot_iter,
					tuple, iter->bi->pi_tuple_size);
}

/**
 * __blk_integrity_copy_to_tuple() - copy to &tuple from @iter
 */
static void __blk_integrity_copy_to_tuple(struct bio_integrity_payload *bip,
					  struct bvec_iter *iter, void *tuple,
					  unsigned int tuple_size)
{
	void *prot_buf;

	while (tuple_size) {
		struct bio_vec pbv = mp_bvec_iter_bvec(bip->bip_vec, *iter);
		unsigned int len = min(tuple_size, pbv.bv_len);

		prot_buf = bvec_kmap_local(&pbv);
		memcpy(tuple, prot_buf, len);
		kunmap_local(prot_buf);

		bvec_iter_advance_single(bip->bip_vec, iter, len);
		tuple_size -= len;
		tuple += len;
	}
}

static void blk_integrity_copy_to_tuple(struct blk_integrity_iter *iter,
					void *tuple)
{
	__blk_integrity_copy_to_tuple(iter->bip, &iter->prot_iter,
					tuple, iter->bi->pi_tuple_size);
}

static void blk_set_ext_pi(void *prot_buf, struct blk_integrity_iter *iter)
{
	struct crc64_pi_tuple *pi = prot_buf;

	if (unlikely((unsigned long)prot_buf & (sizeof(*pi) - 1))) {
		put_unaligned_be64(iter->crc, &pi->guard_tag);
		put_unaligned_be16(0, &pi->app_tag);
		put_unaligned_be48(iter->seed, &pi->ref_tag);
	} else {
		pi->guard_tag = cpu_to_be64(iter->crc);
		pi->app_tag = 0;
		put_unaligned_be48(iter->seed, &pi->ref_tag);
	}
}

static void blk_set_t10_pi(void *prot_buf, struct blk_integrity_iter *iter)
{
	struct t10_pi_tuple *pi = prot_buf;

	if (unlikely((unsigned long)prot_buf & (sizeof(*pi) - 1))) {
		put_unaligned_be16(iter->crc, &pi->guard_tag);
		put_unaligned_be16(0, &pi->app_tag);
		put_unaligned_be32(iter->seed, &pi->ref_tag);
	} else {
		pi->guard_tag = cpu_to_be16(iter->crc);
		pi->app_tag = 0;
		pi->ref_tag = cpu_to_be32(iter->seed);
	}
}

static void blk_set_ip_pi(void *prot_buf, struct blk_integrity_iter *iter)
{
	__be16 csum = (__force __be16)~(lower_16_bits(iter->crc));
	struct t10_pi_tuple *pi = prot_buf;

	if (unlikely((unsigned long)prot_buf & (sizeof(*pi) - 1))) {
		__put_unaligned_t(__be16, csum, &pi->guard_tag);
		put_unaligned_be16(0, &pi->app_tag);
		put_unaligned_be32(iter->seed, &pi->ref_tag);
	} else {
		pi->guard_tag = csum;
		pi->app_tag = 0;
		pi->ref_tag = cpu_to_be32(iter->seed);
	}
}

static bool ext_pi_ref_escape(const u8 ref_tag[6])
{
	static const u8 ref_escape[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	return memcmp(ref_tag, ref_escape, sizeof(ref_escape)) == 0;
}

static blk_status_t ext_pi_crc64_verify(struct blk_integrity_iter *iter,
					struct crc64_pi_tuple *pi)
{
	u64 guard, ref, seed = lower_48_bits(iter->seed);
	u16 app;

	if (unlikely((unsigned long)pi & (sizeof(*pi) - 1))) {
		guard = get_unaligned_be64(&pi->guard_tag);
		app = get_unaligned_be16(&pi->app_tag);
		ref = get_unaligned_be48(pi->ref_tag);
	} else {
		guard = be64_to_cpu(pi->guard_tag);
		app = be16_to_cpu(pi->app_tag);
		ref = get_unaligned_be48(pi->ref_tag);
	}

	if (iter->bi->flags & BLK_INTEGRITY_REF_TAG) {
		if (app == APP_TAG_ESCAPE)
			return BLK_STS_OK;
		if (ref != seed) {
			pr_err("%s: ref tag error at location %llu (rcvd %llu)\n",
				iter->bio->bi_bdev->bd_disk->disk_name, seed,
				ref);
			return BLK_STS_PROTECTION;
		}
	} else if (app == APP_TAG_ESCAPE && ext_pi_ref_escape(pi->ref_tag)) {
		return BLK_STS_OK;
	}

	if (guard != iter->crc) {
		pr_err("%s: guard tag error at sector %llu (rcvd %016llx, want %016llx)\n",
			iter->bio->bi_bdev->bd_disk->disk_name, iter->seed,
			guard, iter->crc);
		return BLK_STS_PROTECTION;
	}

	return BLK_STS_OK;
}

static blk_status_t t10_pi_verify(struct blk_integrity_iter *iter,
				  struct t10_pi_tuple *pi)
{
	u32 ref, seed = lower_32_bits(iter->seed);
	u16 guard;
	u16 app;

	if (unlikely((unsigned long)pi & (sizeof(*pi) - 1))) {
		guard = get_unaligned_be16(&pi->guard_tag);
		app = get_unaligned_be16(&pi->app_tag);
		ref = get_unaligned_be32(&pi->ref_tag);
	} else {
		guard = be16_to_cpu(pi->guard_tag);
		app = be16_to_cpu(pi->app_tag);
		ref = be32_to_cpu(pi->ref_tag);
	}

	if (iter->bi->flags & BLK_INTEGRITY_REF_TAG) {
		if (app == APP_TAG_ESCAPE)
			return BLK_STS_OK;
		if (ref != seed) {
			pr_err("%s: ref tag error at location %u (rcvd %u)\n",
				iter->bio->bi_bdev->bd_disk->disk_name, seed,
				ref);
			return BLK_STS_PROTECTION;
		}
	} else if (app == APP_TAG_ESCAPE && ref == REF_TAG_ESCAPTE) {
		return BLK_STS_OK;
	}

	if (guard != (u16)iter->crc) {
		pr_err("%s: guard tag error at sector %llu (rcvd %04x, want %04x)\n",
			iter->bio->bi_bdev->bd_disk->disk_name, iter->seed,
			guard, (u16)iter->crc);
		return BLK_STS_PROTECTION;
	}

	return BLK_STS_OK;
}

static blk_status_t blk_integrity_verify(struct blk_integrity_iter *iter,
					 void *tuple)
{
	switch (iter->bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		return ext_pi_crc64_verify(iter, tuple);
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		return t10_pi_verify(iter, tuple);
	default:
		return BLK_STS_OK;
	}
}

static void blk_integrity_set(struct blk_integrity_iter *iter,
			      void *tuple)
{
	switch (iter->bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		return blk_set_ext_pi(tuple, iter);
	case BLK_INTEGRITY_CSUM_CRC:
		return blk_set_t10_pi(tuple, iter);
	case BLK_INTEGRITY_CSUM_IP:
		return blk_set_ip_pi(tuple, iter);
	default:
		WARN_ON_ONCE(1);
		return;
	}
}

static blk_status_t blk_integrity_interval(struct blk_integrity_iter *iter,
					   bool verify)
{
	blk_status_t ret = BLK_STS_OK;
	union pi_tuple tuple;
	void *ptuple = &tuple;
	struct bio_vec pbv;

	blk_integrity_crc_offset(iter);
	pbv = mp_bvec_iter_bvec(iter->bip->bip_vec, iter->prot_iter);
	if (pbv.bv_len >= iter->bi->pi_tuple_size) {
		ptuple = bvec_kmap_local(&pbv);
		bvec_iter_advance_single(iter->bip->bip_vec, &iter->prot_iter,
				iter->bi->metadata_size - iter->bi->pi_offset);
	} else if (verify) {
		blk_integrity_copy_to_tuple(iter, ptuple);
	}

	if (verify)
		ret = blk_integrity_verify(iter, ptuple);
	else
		blk_integrity_set(iter, ptuple);

	if (likely(ptuple != &tuple))
		kunmap_local(ptuple);
	else if (!verify)
		blk_integrity_copy_from_tuple(iter, ptuple);

	iter->interval_remaining = 1 << iter->bi->interval_exp;
	iter->crc = 0;
	iter->seed++;

	return ret;
}

static void blk_integrity_iterate(struct bio *bio, struct bvec_iter *data_iter,
				  bool verify)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct blk_integrity_iter iter = {
		.bio = bio,
		.bip = bip,
		.bi = bi,
		.data_iter = *data_iter,
		.prot_iter = bip->bip_iter,
		.interval_remaining = 1 << bi->interval_exp,
		.seed = data_iter->bi_sector,
		.crc = 0,
	};
	blk_status_t ret = BLK_STS_OK;

	while (iter.data_iter.bi_size && ret == BLK_STS_OK) {
		struct bio_vec bv = mp_bvec_iter_bvec(iter.bio->bi_io_vec,
						      iter.data_iter);
		void *kaddr = bvec_kmap_local(&bv);
		void *data = kaddr;
		unsigned int len;

		bvec_iter_advance_single(iter.bio->bi_io_vec, &iter.data_iter,
					 bv.bv_len);
		while (bv.bv_len && ret == BLK_STS_OK) {
			len = min(iter.interval_remaining, bv.bv_len);
			blk_crc(&iter, data, len);
			bv.bv_len -= len;
			data += len;
			iter.interval_remaining -= len;
			if (!iter.interval_remaining)
				ret = blk_integrity_interval(&iter, verify);
		}
		kunmap_local(kaddr);
	}

	if (ret)
		bio->bi_status = ret;
}

void blk_integrity_generate(struct bio *bio)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		blk_integrity_iterate(bio, &bio->bi_iter, false);
		break;
	default:
		break;
	}
}

void blk_integrity_verify_iter(struct bio *bio, struct bvec_iter *saved_iter)
{
	struct blk_integrity *bi = blk_get_integrity(bio->bi_bdev->bd_disk);

	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		blk_integrity_iterate(bio, saved_iter, true);
		break;
	default:
		break;
	}
}

/**
 * blk_pi_advance_offset - advance @iter past the protection offset
 *
 * For protection formats that contain front padding on the metadata region.
 */
static void blk_pi_advance_offset(struct blk_integrity *bi,
				  struct bio_integrity_payload *bip,
				  struct bvec_iter *iter)
{
	unsigned int offset = bi->pi_offset;

	while (offset > 0) {
		struct bio_vec bv = mp_bvec_iter_bvec(bip->bip_vec, *iter);
		unsigned int len = min(bv.bv_len, offset);

		bvec_iter_advance_single(bip->bip_vec, iter, len);
		offset -= len;
	}
}

static void *blk_tuple_remap_begin(union pi_tuple *tuple,
				   struct blk_integrity *bi,
				   struct bio_integrity_payload *bip,
				   struct bvec_iter *iter)
{
	struct bvec_iter titer;
	struct bio_vec pbv;

	blk_pi_advance_offset(bi, bip, iter);
	pbv = mp_bvec_iter_bvec(bip->bip_vec, *iter);
	if (likely(pbv.bv_len >= bi->pi_tuple_size))
		return bvec_kmap_local(&pbv);

	/*
	 * We need to preserve the state of the original iter for the
	 * copy_from_tuple at the end, so make a temp iter for here.
	 */
	titer = *iter;
	__blk_integrity_copy_to_tuple(bip, &titer, tuple, bi->pi_tuple_size);
	return tuple;
}

static void blk_tuple_remap_end(union pi_tuple *tuple, void *ptuple,
				struct blk_integrity *bi,
				struct bio_integrity_payload *bip,
				struct bvec_iter *iter)
{
	unsigned int len = bi->metadata_size - bi->pi_offset;

	if (likely(ptuple != tuple)) {
		kunmap_local(ptuple);
	} else {
		__blk_integrity_copy_from_tuple(bip, iter, ptuple,
						bi->pi_tuple_size);
		len -= bi->pi_tuple_size;
	}

	bvec_iter_advance(bip->bip_vec, iter, len);
}

static void blk_set_ext_unmap_ref(void *prot_buf, u64 virt, u64 ref_tag)
{
	struct crc64_pi_tuple *pi = prot_buf;
	u64 ref = get_unaligned_be48(&pi->ref_tag);

	if (ref == lower_48_bits(virt) && ref != virt)
		put_unaligned_be48(virt, pi->ref_tag);
}

static void blk_set_t10_unmap_ref(void *prot_buf, u32 virt, u32 ref_tag)
{
	struct t10_pi_tuple *pi = prot_buf;
	u32 ref;

	if (unlikely((unsigned long)pi & (sizeof(*pi) - 1)))
		ref = get_unaligned_be32(&pi->ref_tag);
	else
		ref = be32_to_cpu(pi->ref_tag);

	if (ref != ref_tag || ref == virt)
		return;

	if (unlikely((unsigned long)pi & (sizeof(*pi) - 1)))
		put_unaligned_be32(virt, &pi->ref_tag);
	else
		pi->ref_tag = cpu_to_be32(virt);
}

static void blk_reftag_remap_complete(struct blk_integrity *bi, void *tuple,
				      u64 virt, u64 ref)
{
	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		blk_set_ext_unmap_ref(tuple, virt, ref);
		break;
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		blk_set_t10_unmap_ref(tuple, virt, ref);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

static void blk_set_ext_map_ref(void *prot_buf, u64 virt, u64 ref_tag)
{
	struct crc64_pi_tuple *pi = prot_buf;
	u64 ref = get_unaligned_be48(&pi->ref_tag);

	if (ref == lower_48_bits(virt) && ref != ref_tag)
		put_unaligned_be48(ref_tag, pi->ref_tag);
}

static void blk_set_t10_map_ref(void *prot_buf, u32 virt, u32 ref_tag)
{
	struct t10_pi_tuple *pi = prot_buf;
	u32 ref;

	if (unlikely((unsigned long)pi & (sizeof(*pi) - 1)))
		ref = get_unaligned_be32(&pi->ref_tag);
	else
		ref = be32_to_cpu(pi->ref_tag);

	if (ref != virt || ref == ref_tag)
		return;

	if (unlikely((unsigned long)pi & (sizeof(*pi) - 1)))
		put_unaligned_be32(ref_tag, &pi->ref_tag);
	else
		pi->ref_tag = cpu_to_be32(ref_tag);
}

static void blk_reftag_remap_prepare(struct blk_integrity *bi, void *tuple,
				     u64 virt, u64 ref)
{
	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_CRC64:
		blk_set_ext_map_ref(tuple, virt, ref);
		break;
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		blk_set_t10_map_ref(tuple, virt, ref);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

static void __blk_reftag_remap(struct bio *bio, struct blk_integrity *bi,
			       unsigned *intervals, u64 *ref, bool prep)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct bvec_iter iter = bip->bip_iter;
	u64 virt = bip_get_seed(bip);
	union pi_tuple tuple;
	void *ptuple;

	if (prep && bip->bip_flags & BIP_MAPPED_INTEGRITY) {
		*ref += bio->bi_iter.bi_size >> bi->interval_exp;
		return;
	}

	while (iter.bi_size && *intervals) {
		ptuple = blk_tuple_remap_begin(&tuple, bi, bip, &iter);

		if (prep)
			blk_reftag_remap_prepare(bi, ptuple, virt, *ref);
		else
			blk_reftag_remap_complete(bi, ptuple, virt, *ref);

		blk_tuple_remap_end(&tuple, ptuple, bi, bip, &iter);
		(*intervals)--;
		(*ref)++;
		virt++;
	}

	if (prep)
		bip->bip_flags |= BIP_MAPPED_INTEGRITY;
}

static void blk_integrity_remap(struct request *rq, unsigned int nr_bytes,
				bool prep)
{
	struct blk_integrity *bi = &rq->q->limits.integrity;
	u64 ref = blk_rq_pos(rq) >> (bi->interval_exp - SECTOR_SHIFT);
	unsigned intervals = nr_bytes >> bi->interval_exp;
	struct bio *bio;

	if (!(bi->flags & BLK_INTEGRITY_REF_TAG))
		return;

	__rq_for_each_bio(bio, rq) {
		__blk_reftag_remap(bio, bi, &intervals, &ref, prep);
		if (!intervals)
			break;
	}
}

void blk_integrity_prepare(struct request *rq)
{
	blk_integrity_remap(rq, blk_rq_bytes(rq), true);
}

void blk_integrity_complete(struct request *rq, unsigned int nr_bytes)
{
	blk_integrity_remap(rq, nr_bytes, false);
}
