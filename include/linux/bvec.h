/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bvec iterator
 *
 * Copyright (C) 2001 Ming Lei <ming.lei@canonical.com>
 */
#ifndef __LINUX_BVEC_H
#define __LINUX_BVEC_H

#include <linux/highmem.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/types.h>
#include <asm/io.h>

struct page;

/**
 * struct bio_vec - a contiguous range of physical memory addresses
 * @bv_page:   First page associated with the address range.
 * @bv_len:    Number of bytes in the address range.
 * @bv_offset: Start of the address range relative to the start of @bv_page.
 *
 * The following holds for a bvec if n * PAGE_SIZE < bv_offset + bv_len:
 *
 *   nth_page(@bv_page, n) == @bv_page + n
 *
 * This holds because page_is_mergeable() checks the above property.
 */
struct bio_vec {
	struct page	*bv_page;
	unsigned int	bv_len;
	unsigned int	bv_offset;
	dma_addr_t	bv_dma_address;
#ifdef CONFIG_NEED_SG_DMA_LENGTH
	unsigned int	bv_dma_length;
#endif
#ifdef CONFIG_NEED_SG_DMA_FLAGS
	unsigned int	bv_dma_flags;
#endif
};

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

/**
 * bv_phys - return physical address of a bio_vec
 * @bv:		bio_vec
 */
static inline dma_addr_t bv_phys(struct bio_vec *bv)
{
	return page_to_phys(bv->bv_page) + bv->bv_offset;
}

/**
 * bv_virt - return virtual address of a bio_vec
 * @bv:		bio_vec
 */
static inline void *bv_virt(struct bio_vec *bv)
{
	return page_address(bv->bv_page) + bv->bv_offset;
}

struct bvec_iter {
	sector_t		bi_sector;	/* device address in 512 byte
						   sectors */
	unsigned int		bi_size;	/* residual I/O count */

	unsigned int		bi_idx;		/* current index into bvl_vec */

	unsigned int            bi_bvec_done;	/* number of bytes completed in
						   current bvec */
} __packed;

struct bvec_iter_all {
	struct bio_vec	bv;
	int		idx;
	unsigned	done;
};

/*
 * various member access, note that bio_data should of course not be used
 * on highmem page vectors
 */
#define __bvec_iter_bvec(bvec, iter)	(&(bvec)[(iter).bi_idx])

/* multi-page (mp_bvec) helpers */
#define mp_bvec_iter_page(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_page)

#define mp_bvec_iter_len(bvec, iter)				\
	min((iter).bi_size,					\
	    __bvec_iter_bvec((bvec), (iter))->bv_len - (iter).bi_bvec_done)

#define mp_bvec_iter_offset(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

#define mp_bvec_iter_page_idx(bvec, iter)			\
	(mp_bvec_iter_offset((bvec), (iter)) / PAGE_SIZE)

#define mp_bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= mp_bvec_iter_page((bvec), (iter)),	\
	.bv_len		= mp_bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= mp_bvec_iter_offset((bvec), (iter)),	\
})

/* For building single-page bvec in flight */
 #define bvec_iter_offset(bvec, iter)				\
	(mp_bvec_iter_offset((bvec), (iter)) % PAGE_SIZE)

#define bvec_iter_len(bvec, iter)				\
	min_t(unsigned, mp_bvec_iter_len((bvec), (iter)),		\
	      PAGE_SIZE - bvec_iter_offset((bvec), (iter)))

#define bvec_iter_page(bvec, iter)				\
	(mp_bvec_iter_page((bvec), (iter)) +			\
	 mp_bvec_iter_page_idx((bvec), (iter)))

#define bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})

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

/*
 * These macros should be used after a dma_map_bvecs call has been done
 * to get bus addresses of each of the bio_vec array entries and their
 * lengths. You should work only with the number of bio_vec array entries
 * dma_map_bvecs returns, or alternatively stop on the first bv_dma_len(bv)
 * which is 0.
 */
#define bv_dma_address(bv)	((bv)->bv_dma_address)

#ifdef CONFIG_NEED_SG_DMA_LENGTH
#define bv_dma_len(bv)		((bv)->bv_dma_length)
#else
#define bv_dma_len(bv)		((bv)->bv_len)
#endif

/*
 * On 64-bit architectures there is a 4-byte padding in struct scatterlist
 * (assuming also CONFIG_NEED_SG_DMA_LENGTH is set). Use this padding for DMA
 * flags bits to indicate when a specific dma address is a bus address or the
 * buffer may have been bounced via SWIOTLB.
 */
#ifdef CONFIG_NEED_SG_DMA_FLAGS

#define BV_DMA_BUS_ADDRESS	BIT(0)
#define BV_DMA_SWIOTLB		BIT(1)

/**
 * bv_dma_is_bus_address - Return whether a given segment was marked
 *			   as a bus address
 * @bv:		 bio_vec array entry
 *
 * Description:
 *   Returns true if bv_dma_mark_bus_address() has been called on
 *   this bio_vec.
 **/
static inline bool bv_dma_is_bus_address(struct bio_vec *bv)
{
	return bv->bv_dma_flags & BV_DMA_BUS_ADDRESS;
}

/**
 * bv_dma_mark_bus_address - Mark the bio_vec entry as a bus address
 * @bv:		 bio_vec array entry
 *
 * Description:
 *   Marks the passed-in bv entry to indicate that the dma_address is
 *   a bus address and doesn't need to be unmapped. This should only be
 *   used by dma_map_bvecs() implementations to mark bus addresses
 *   so they can be properly cleaned up in dma_unmap_bvecs().
 **/
static inline void bv_dma_mark_bus_address(struct bio_vec *bv)
{
	bv->bv_dma_flags |= BV_DMA_BUS_ADDRESS;
}

/**
 * bv_unmark_bus_address - Unmark the bio_vec entry as a bus address
 * @bv:		 bio_vec array entry
 *
 * Description:
 *   Clears the bus address mark.
 **/
static inline void bv_dma_unmark_bus_address(struct bio_vec *bv)
{
	bv->bv_dma_flags &= ~BV_DMA_BUS_ADDRESS;
}

/**
 * bv_dma_is_swiotlb - Return whether the bio_vec was marked for SWIOTLB
 *		       bouncing
 * @bv:		bio_vec array entry
 *
 * Description:
 *   Returns true if the bio_vec was marked for SWIOTLB bouncing. Not all
 *   elements may have been bounced, so the caller would have to check
 *   individual BV entries with is_swiotlb_buffer().
 */
static inline bool bv_dma_is_swiotlb(struct bio_vec *bv)
{
	return bv->bv_dma_flags & BV_DMA_SWIOTLB;
}

/**
 * bv_dma_mark_swiotlb - Mark the bio_vec for SWIOTLB bouncing
 * @bv:		bio_vec array entry
 *
 * Description:
 *   Marks a a bio_vec for SWIOTLB bounce. Not all bio_vec entries may
 *   be bounced.
 */
static inline void bv_dma_mark_swiotlb(struct bio_vec *bv)
{
	bv->bv_dma_flags |= BV_DMA_SWIOTLB;
}

#else

static inline bool bv_dma_is_bus_address(struct bio_vec *bv)
{
	return false;
}
static inline void bv_dma_mark_bus_address(struct bio_vec *bv)
{
}
static inline void bv_dma_unmark_bus_address(struct bio_vec *bv)
{
}
static inline bool bv_dma_is_swiotlb(struct bio_vec *bv)
{
	return false;
}
static inline void bv_dma_mark_swiotlb(struct bio_vec *bv)
{
}

#endif	/* CONFIG_NEED_SG_DMA_FLAGS */

#endif /* __LINUX_BVEC_H */
