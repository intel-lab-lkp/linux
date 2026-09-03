/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 * Copyright(c) 2015 - 2017 Intel Corporation.
 */

#ifndef _HFI2_USER_EXP_RCV_H
#define _HFI2_USER_EXP_RCV_H

#include "hfi2.h"
#include "exp_rcv.h"

struct hfi2_page_iter_ops;

/**
 * Base type for iterating over sets of pinned-page ranges (pagesets).
 *
 * Depending on implementation, pages may also already be DMA-mapped.
 */
struct hfi2_page_iter {
	struct hfi2_page_iter_ops *ops;
};

struct hfi2_page_iter_ops {
	/**
	 * Advance iterator to next pageset.
	 *
	 * Implementation must construct TID-compatible pagesets. TID-compatible means:
	 * - starting address is 4KiB-aligned.
	 * - size in range of [4KiB,2MiB].
	 * - size is a power-of-two.
	 *
	 * @return > 0 on successful advancement, 0 when iterator is at last
	 * element, < 0 on error. It is an error to call .next() after it
	 * returns 0.
	 */
	int (*next)(struct hfi2_page_iter *iter);

	void (*free)(struct hfi2_page_iter *iter);
};

struct tid_user_buf;

/*
 * hfi2_page_iter implementation for tid_system.
 *
 * Built around struct tid_user_buf->{psets,n_psets}.
 */
struct page_array_iter {
	struct hfi2_page_iter common;
	struct tid_user_buf *tbuf;
	unsigned int setidx;
};

/*
 * structs tid_pageset, tid_user_buf, tid_rb_node, tid_user_buf_ops,
 * tid_node_ops - Generic memory-pinning and DMA-mapping datastructures and
 * interfaces for user expected receive.
 *
 * Here's a high-level flow of how generic device user expected receive works:
 * 1. Available memory implementations are registered at driver load.
 * 2. When TID_UPDATE with memory type information is received,
 *    struct tid_user_buf_ops* for memory type is looked up.
 * 3. struct tid_user_buf 'tbuf' is created using tid_user_buf_ops.init(),
 *    passing in the virtual address range from userspace.
 * 4. Pages are pinned using tid_user_buf_ops.pin_pages(tbuf).
 * 5. tid_user_buf_ops.find_phys_blocks() produces an array of sets
 *    (struct tid_pageset[]) of physically contiguous pages.
 * 6. struct tid_node_ops* for memory type is looked up.
 * 7. For each set of physically contiguous pinned pages:
 *    7.1 A struct tid_rb_node is created using tid_node_ops.init(). It stores
 *        its memory type for later lookup.
 *    7.2 tid_node_ops.init() DMA-maps the physically-contiguous page range.
 *    7.3 An expected RcvArray entry (TID) is programmed with the DMA address.
 *    7.4 The tid_rb_node* is stored in struct hfi2_filedata.entry_to_rb[].
 * 8. The tid_user_buf is no longer needed and is destroyed.
 * 9. When a TID is to be unmapped or the receive memory the TID is programmed
 *    for is invalidated, the struct tid_rb_node* for the TID or receive memory
 *    is found, the struct tid_node_ops looked up, and
 *    tid_node_ops.dma_unmap(node) and tid_node_ops.free(node) called.
 *
 * This is an overview. See struct tid_user_buf_ops and struct tid_node_ops for
 * the methods and their semantics that an implementation must provide.
 */

struct tid_pageset {
	u16 idx;
	u16 count;
};

struct tid_node_ops;
struct tid_user_buf_ops;

struct tid_user_buf {
	unsigned long vaddr;
	unsigned long length;
	struct tid_pageset *psets;
	struct tid_user_buf_ops *ops;
	unsigned int n_psets;
	bool use_mn;
	u16 type; /* Implementation must set to HFI2_MEMINFO_TYPE* in tid_user_buf_ops.init() */
};

int hfi2_tid_user_buf_init(u16 pset_size, unsigned long vaddr, unsigned long length, bool notify,
		      struct tid_user_buf_ops *ops, u16 type, struct tid_user_buf *tbuf);
void hfi2_tid_user_buf_free(struct tid_user_buf *tbuf);

struct tid_rb_node {
	struct hfi2_filedata *fdata;
	struct mutex invalidate_mutex; /* covers hw removal */
	/* Only used for debug and tracing */
	unsigned long phys;
	struct tid_group *grp;
	struct tid_node_ops *ops;
	dma_addr_t dma_addr;
	/* Starting virtual address for this node's page range */
	unsigned long vaddr;
	/* Number of pages, implementation-sized */
	unsigned int npages;
	/* Implementation page-size shift */
	unsigned int page_shift;
	u32 rcventry;
	bool use_mn;
	bool freed;
	/* Implementation must set to HFI2_MEMINFO_TYPE* in tid_node_ops.init() */
	u16 type;
};

/**
 * User expected receive requires @vaddr from userspace be aligned on a page
 * boundary.
 *
 * User expected receive requires this because it cannot communicate the offset
 * between @vaddr and the page start.
 *
 * TID memory implementation must check that @vaddr is aligned on a page
 * boundary but may delay this check until as late as .pin_pages().
 *
 * This allows for implementations where the page size is not known until the
 * pages are pinned.
 */
struct tid_user_buf_ops {
	/**
	 * Allocate and initialize @*tbuf.
	 *
	 * Implementation must initialize:
	 *   - vaddr
	 *   - length
	 *   - psets
	 *   - ops
	 *   - type
	 *
	 * @expected_count
	 * @notify when false, implementation may use invalidation callback
	 *   underneath.
	 *
	 *   When true, implementation must use invalidation callback
	 *   underneath.
	 *
	 *   If true and implementation does not have an invalidation callback,
	 *   must return an error.
	 * @vaddr
	 * @length
	 * @allow_unaligned when true, implementation may, but is not required
	 *   to, handle unaligned @vaddr. When false, implementation must return
	 *   -EINVAL for unaligned @vaddr.
	 * @tbuf [out] allocated tid_user_buf
	 *
	 * @return 0 on success, non-zero on error.
	 *
	 * Errors including but not limited to:
	 * - Number of pages based on @length too long for @expected_count
	 * - @vaddr is not suitably aligned
	 * - @length invalid for implementation
	 * - @notify is true but implementation does not support
	 *   memory-invalidation notification
	 */
	int (*init)(u16 expected_count,
		    bool notify,
		    unsigned long vaddr,
		    unsigned long length,
		    bool allow_unaligned,
		    struct tid_user_buf **tbuf);

	/**
	 * Free @tbuf.
	 */
	void (*free)(struct tid_user_buf *tbuf);

	/**
	 * Pin pages for @tbuf based on (@vaddr,@length) passed into
	 * @tid_user_buf_ops.init().
	 *
	 * Implementation may also DMA-map pages at this time.
	 *
	 * Implementation may store @fd at this time.
	 *
	 * @fd
	 * @tbuf
	 *
	 * @return > 0 number of pages pinned on success, < 0 error value on
	 *   failure. 0 is treated as a failure but not an error value.
	 */
	int (*pin_pages)(struct hfi2_filedata *fd, struct tid_user_buf *tbuf);

	/**
	 * Get page size of @tbuf's pinned pages.
	 *
	 * Caller should not assume that page size is known until after
	 * pin_pages(fd, @tbuf).
	 *
	 * @tbuf
	 *
	 * @return page size. No way to tell if error.
	 */
	unsigned int (*page_size)(struct tid_user_buf *tbuf);

	/**
	 * Unpin only @npages starting at @idx.
	 *
	 * Implementation may implement partial unpinning.
	 *
	 * If not, implementation must ensure that pages are unmapped and
	 * unpinned when last reference to them is released.
	 *
	 * @fd Same fd as given in tid_user_buf_ops.pin_pages() call
	 * @tbuf
	 * @idx
	 * @npages
	 */
	void (*unpin_pages)(struct hfi2_filedata *fd,
			    struct tid_user_buf *tbuf,
			    unsigned int idx,
			    unsigned int npages);

	/**
	 * Optional; implementation must either implement find_phys_blocks() or
	 * ensure that iter_begin() will return a valid iterator after
	 * .pin_pages().
	 *
	 * Implementation must program @tbuf->psets elements such that:
	 * 1. All pages in a pageset are physically contiguous
	 * 2. All pages in all pagesets have the same page size
	 * 3. The total size of a pageset is a power-of-two in the range
	 *    [4KiB, 2MiB]
	 *
	 * Implementation must set @tbuf->n_psets to number of page sets
	 * programmed.
	 *
	 * @tbuf TID user buf to set (@tbuf->psets,@tbuf->n_psets) on.
	 * @npages limit on number of pages to process into page sets before
	 *         stopping.
	 *
	 * @return 0 on success, non-zero on error
	 */
	int (*find_phys_blocks)(struct tid_user_buf *tbuf,
				unsigned int npages);

	/**
	 * @return true when:
	 * - @tbuf's virtual->physical mapping has been invalidated
	 * - @tbuf's physical pages have been released
	 */
	bool (*invalidated)(struct tid_user_buf *tbuf);

	/**
	 * Unregister memory-invalidation callback registered in struct
	 * tid_user_buf_ops.init() implementation.
	 */
	void (*unnotify)(struct tid_user_buf *tbuf);

	/**
	 * Optional. Get pageset iterator. Pages in pageset must be pinned and
	 * physically-contiguous. Whether pages are also DMA-mapped at the time
	 * this method is called is implementation-dependent.
	 *
	 * Implementation must return iterator only if there is at least one
	 * pageset to iterate over. I.e. it is an error if implementation
	 * cannot return an iterator over at least one pageset.
	 *
	 * Returned iterator must be freed with @hfi2_page_iter->ops->free().
	 *
	 * @return pointer on success, ERR_PTR() on error.
	 */
	struct hfi2_page_iter *(*iter_begin)(struct tid_user_buf *tbuf);
};

struct tid_node_ops {
	/**
	 * Create tid_rb_node for pageset given by @iter.
	 *
	 * .init() implementation must initialize the following
	 *   - fdata
	 *   - invalidate_mutex
	 *   - phys
	 *   - grp
	 *   - ops
	 *   - dma_addr
	 *   - vaddr
	 *   - npages
	 *   - page_shift: may not be less than EXP_TID_ADDR_SHIFT
	 *   - rcventry
	 *   - use_mn
	 *   - freed
	 *   - type: one of the HFI2_MEMINFO_TYPE* defines
	 *
	 * Pages in pageset given by @iter must be TID-ready: pinned, physically contiguous,
	 * 4KiB <= size <= 2MiB, starting address is power-of-two.
	 *
	 * Implementation must DMA-map the pages given by @iter if they are not DMA-mapped
	 * already.
	 *
	 * If @fd->use_mn is true and memory implementation does not support
	 * invalidation callbacks, .init() must return an error.
	 *
	 * @fd
	 * @tbuf Contains larger memory pinning to create TID entry from
	 * @rcventry
	 * @grp
	 * @iter
	 *
	 * @return allocated node on success, ERR_PTR() on error.
	 */
	struct tid_rb_node *(*init)(struct hfi2_filedata *fd,
				    struct tid_user_buf *tbuf,
				    u32 rcventry,
				    struct tid_group *grp,
				    struct hfi2_page_iter *iter);

	/**
	 * Free @node.
	 */
	void (*free)(struct tid_rb_node *node);

	/**
	 * Register for memory invalidation callback. Implementation should
	 * only register for notification on @node's page-range.
	 *
	 * When @node->fdata->use_mn is true, invalidation callback must call
	 * hfi2_user_exp_rcv_invalidate(@node).
	 *
	 * When @node->fdata->use_mn is false, invalidation callback may call
	 * hfi2_user_exp_rcv_invalidate(@node).
	 *
	 * Should be no-op when implementation does not support partial
	 * unpinning.
	 *
	 * @return 0 on success, non-zero on failure.
	 */
	int (*register_notify)(struct tid_rb_node *node);

	/**
	 * Unregister from memory-invalidation callback in anticipation of
	 * unprogramming TID.
	 *
	 * Should be no-op when implementation does not support partial
	 * unpinning.
	 *
	 * Not safe to call more than once per register_notify() call.
	 */
	void (*unregister_notify)(struct tid_rb_node *node);

	/**
	 * DMA-unmap mapped memory for @node.
	 *
	 * Should be no-op when implementation does not support partial
	 * unmapping.
	 */
	void (*dma_unmap)(struct tid_rb_node *node);

	/**
	 * Unpin pages covered by @node.
	 *
	 * user_exp_rcv will call this function under node->invalidate_mutex.
	 * user_exp_rcv will only call this function once per node.
	 *
	 * Should be no-op when implementation does not support partial
	 * unpinning.
	 */
	void (*unpin_pages)(struct hfi2_filedata *fd, struct tid_rb_node *node);
};

int hfi2_register_tid_ops(u16 type, struct tid_user_buf_ops *op, struct tid_node_ops *nops);
void hfi2_deregister_tid_ops(u16 type);

int hfi2_register_system_tid_ops(void);
void hfi2_deregister_system_tid_ops(void);

void hfi2_user_exp_rcv_invalidate(struct tid_rb_node *node);

int hfi2_user_exp_rcv_init(struct hfi2_filedata *fd,
			   struct hfi2_ctxtdata *uctxt);
void hfi2_user_exp_rcv_free(struct hfi2_filedata *fd);
int hfi2_user_exp_rcv_setup(struct hfi2_filedata *fd,
			    struct hfi2_tid_info *tinfo,
			    bool allow_unaligned,
			    bool do_tidcnt_check);
int hfi2_user_exp_rcv_clear(struct hfi2_filedata *fd,
			    struct hfi2_tid_info *tinfo);
int hfi2_user_exp_rcv_invalid(struct hfi2_filedata *fd,
			      struct hfi2_tid_info *tinfo,
			      bool do_tidcnt_check);

#endif /* _HFI2_USER_EXP_RCV_H */
