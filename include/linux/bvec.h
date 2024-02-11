/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bvec iterator
 *
 * Copyright (C) 2001 Ming Lei <ming.lei@canonical.com>
 */
#ifndef __LINUX_BVEC_H
#define __LINUX_BVEC_H

#include <linux/bvec_types.h>
#include <linux/highmem.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/types.h>

/**
 * bvec_set_page - initialize a bvec based off a struct page
 * @bv:		bvec to initialize
 * @page:	page the bvec should point to
 * @len:	length of the bvec
 * @offset:	offset into the page
 */
static inline void bvec_set_page(struct bio_vec *bv, struct page *page,
		unsigned int len, unsigned int offset)
{
	bv->bv_page = page;
	bv->bv_len = len;
	bv->bv_offset = offset;
}

/**
 * bvec_set_folio - initialize a bvec based off a struct folio
 * @bv:		bvec to initialize
 * @folio:	folio the bvec should point to
 * @len:	length of the bvec
 * @offset:	offset into the folio
 */
static inline void bvec_set_folio(struct bio_vec *bv, struct folio *folio,
		unsigned int len, unsigned int offset)
{
	bvec_set_page(bv, &folio->page, len, offset);
}

/**
 * bvec_set_virt - initialize a bvec based on a virtual address
 * @bv:		bvec to initialize
 * @vaddr:	virtual address to set the bvec to
 * @len:	length of the bvec
 */
static inline void bvec_set_virt(struct bio_vec *bv, void *vaddr,
		unsigned int len)
{
	bvec_set_page(bv, virt_to_page(vaddr), len, offset_in_page(vaddr));
}

static inline bool bvec_iter_advance(const struct bio_vec *bv,
		struct bvec_iter *iter, unsigned bytes)
{
	unsigned int idx = iter->bi_idx;

	if (WARN_ONCE(bytes > iter->bi_size,
		     "Attempted to advance past end of bvec iter\n")) {
		iter->bi_size = 0;
		return false;
	}

	iter->bi_size -= bytes;
	bytes += iter->bi_bvec_done;

	while (bytes && bytes >= bv[idx].bv_len) {
		bytes -= bv[idx].bv_len;
		idx++;
	}

	iter->bi_idx = idx;
	iter->bi_bvec_done = bytes;
	return true;
}

/*
 * A simpler version of bvec_iter_advance(), @bytes should not span
 * across multiple bvec entries, i.e. bytes <= bv[i->bi_idx].bv_len
 */
static inline void bvec_iter_advance_single(const struct bio_vec *bv,
				struct bvec_iter *iter, unsigned int bytes)
{
	unsigned int done = iter->bi_bvec_done + bytes;

	if (done == bv[iter->bi_idx].bv_len) {
		done = 0;
		iter->bi_idx++;
	}
	iter->bi_bvec_done = done;
	iter->bi_size -= bytes;
}

#define for_each_bvec(bvl, bio_vec, iter, start)			\
	for (iter = (start);						\
	     (iter).bi_size &&						\
		((bvl = bvec_iter_bvec((bio_vec), (iter))), 1);	\
	     bvec_iter_advance_single((bio_vec), &(iter), (bvl).bv_len))

/* for iterating one bio from start to end */
#define BVEC_ITER_ALL_INIT (struct bvec_iter)				\
{									\
	.bi_sector	= 0,						\
	.bi_size	= UINT_MAX,					\
	.bi_idx		= 0,						\
	.bi_bvec_done	= 0,						\
}

static inline struct bio_vec *bvec_init_iter_all(struct bvec_iter_all *iter_all)
{
	iter_all->done = 0;
	iter_all->idx = 0;

	return &iter_all->bv;
}

static inline void bvec_advance(const struct bio_vec *bvec,
				struct bvec_iter_all *iter_all)
{
	struct bio_vec *bv = &iter_all->bv;

	if (iter_all->done) {
		bv->bv_page++;
		bv->bv_offset = 0;
	} else {
		bv->bv_page = bvec->bv_page + (bvec->bv_offset >> PAGE_SHIFT);
		bv->bv_offset = bvec->bv_offset & ~PAGE_MASK;
	}
	bv->bv_len = min_t(unsigned int, PAGE_SIZE - bv->bv_offset,
			   bvec->bv_len - iter_all->done);
	iter_all->done += bv->bv_len;

	if (iter_all->done == bvec->bv_len) {
		iter_all->idx++;
		iter_all->done = 0;
	}
}

/**
 * bvec_kmap_local - map a bvec into the kernel virtual address space
 * @bvec: bvec to map
 *
 * Must be called on single-page bvecs only.  Call kunmap_local on the returned
 * address to unmap.
 */
static inline void *bvec_kmap_local(struct bio_vec *bvec)
{
	return kmap_local_page(bvec->bv_page) + bvec->bv_offset;
}

/**
 * memcpy_from_bvec - copy data from a bvec
 * @bvec: bvec to copy from
 *
 * Must be called on single-page bvecs only.
 */
static inline void memcpy_from_bvec(char *to, struct bio_vec *bvec)
{
	memcpy_from_page(to, bvec->bv_page, bvec->bv_offset, bvec->bv_len);
}

/**
 * memcpy_to_bvec - copy data to a bvec
 * @bvec: bvec to copy to
 *
 * Must be called on single-page bvecs only.
 */
static inline void memcpy_to_bvec(struct bio_vec *bvec, const char *from)
{
	memcpy_to_page(bvec->bv_page, bvec->bv_offset, from, bvec->bv_len);
}

/**
 * memzero_bvec - zero all data in a bvec
 * @bvec: bvec to zero
 *
 * Must be called on single-page bvecs only.
 */
static inline void memzero_bvec(struct bio_vec *bvec)
{
	memzero_page(bvec->bv_page, bvec->bv_offset, bvec->bv_len);
}

/**
 * bvec_virt - return the virtual address for a bvec
 * @bvec: bvec to return the virtual address for
 *
 * Note: the caller must ensure that @bvec->bv_page is not a highmem page.
 */
static inline void *bvec_virt(struct bio_vec *bvec)
{
	WARN_ON_ONCE(PageHighMem(bvec->bv_page));
	return page_address(bvec->bv_page) + bvec->bv_offset;
}

#endif /* __LINUX_BVEC_H */
