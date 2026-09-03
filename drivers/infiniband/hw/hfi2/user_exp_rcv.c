// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 * Copyright(c) 2015-2018 Intel Corporation.
 */
#include <asm/page.h>
#include <linux/string.h>

#include "mmu_rb.h"
#include "user_exp_rcv.h"
#include "trace.h"

static void unlock_exp_tids(struct hfi2_ctxtdata *uctxt,
			    struct exp_tid_set *set, struct hfi2_filedata *fd);
static void cacheless_tid_rb_remove(struct hfi2_filedata *fdata,
				    struct tid_rb_node *tnode);
static int program_rcvarray(struct hfi2_filedata *fd, struct tid_user_buf *tbuf,
			    struct tid_group *grp, u16 count, u32 *tidlist,
			    unsigned int *tididx, struct hfi2_page_iter *iter,
			    unsigned int *pmapped);
static int unprogram_rcvarray(struct hfi2_filedata *fd, u32 tidinfo);
static void __clear_tid_node(struct hfi2_filedata *fd,
			     struct tid_rb_node *node);
static void clear_tid_node(struct hfi2_filedata *fd, struct tid_rb_node *node);

/*
 * Initialize context and file private data needed for Expected
 * receive caching. This needs to be done after the context has
 * been configured with the eager/expected RcvEntry counts.
 */
int hfi2_user_exp_rcv_init(struct hfi2_filedata *fd,
			   struct hfi2_ctxtdata *uctxt)
{
	int ret = 0;

	fd->entry_to_rb = kcalloc(uctxt->expected_count,
				  sizeof(*fd->entry_to_rb), GFP_KERNEL);
	if (!fd->entry_to_rb)
		return -ENOMEM;

	if (!HFI2_CAP_UGET_MASK(uctxt->flags, TID_UNMAP)) {
		fd->invalid_tid_idx = 0;
		fd->invalid_tids = kcalloc(uctxt->expected_count,
					   sizeof(*fd->invalid_tids),
					   GFP_KERNEL);
		if (!fd->invalid_tids) {
			kfree(fd->entry_to_rb);
			fd->entry_to_rb = NULL;
			return -ENOMEM;
		}
		fd->use_mn = true;
	}

	/*
	 * PSM does not have a good way to separate, count, and
	 * effectively enforce a limit on RcvArray entries used by
	 * subctxts (when context sharing is used) when TID caching
	 * is enabled. To help with that, we calculate a per-process
	 * RcvArray entry share and enforce that.
	 * If TID caching is not in use, PSM deals with usage on its
	 * own. In that case, we allow any subctxt to take all of the
	 * entries.
	 *
	 * Make sure that we set the tid counts only after successful
	 * init.
	 */
	spin_lock(&fd->tid_lock);
	if (uctxt->subctxt_cnt && fd->use_mn) {
		u16 remainder;

		fd->tid_limit = uctxt->expected_count / uctxt->subctxt_cnt;
		remainder = uctxt->expected_count % uctxt->subctxt_cnt;
		if (remainder && fd->subctxt < remainder)
			fd->tid_limit++;
	} else {
		fd->tid_limit = uctxt->expected_count;
	}
	spin_unlock(&fd->tid_lock);

	return ret;
}

void hfi2_user_exp_rcv_free(struct hfi2_filedata *fd)
{
	struct hfi2_ctxtdata *uctxt = fd->uctxt;

	mutex_lock(&uctxt->exp_mutex);
	if (!EXP_TID_SET_EMPTY(uctxt->tid_full_list))
		unlock_exp_tids(uctxt, &uctxt->tid_full_list, fd);
	if (!EXP_TID_SET_EMPTY(uctxt->tid_used_list))
		unlock_exp_tids(uctxt, &uctxt->tid_used_list, fd);
	mutex_unlock(&uctxt->exp_mutex);

	kfree(fd->invalid_tids);
	fd->invalid_tids = NULL;

	kfree(fd->entry_to_rb);
	fd->entry_to_rb = NULL;
}

static struct tid_user_buf_ops *bufops[HFI2_MAX_MEMINFO_ENTRIES];
static struct tid_node_ops *nodeops[HFI2_MAX_MEMINFO_ENTRIES];

/*
 * Register TID memory-pinning implementation for @type memory.
 *
 * @type one of the HFI2_MEMINFO_TYPE* defines found in hfi2_ioctl.h
 * @op Buffer ops to register
 * @nops Node ops to register
 *
 * @return 0 on success, non-zero on error
 */
int hfi2_register_tid_ops(u16 type, struct tid_user_buf_ops *op,
			  struct tid_node_ops *nops)
{
	if (type >= HFI2_MAX_MEMINFO_ENTRIES)
		return -EINVAL;
	bufops[type] = op;
	nodeops[type] = nops;
	return 0;
}

void hfi2_deregister_tid_ops(u16 type)
{
	if (type >= HFI2_MAX_MEMINFO_ENTRIES)
		return;
	bufops[type] = NULL;
	nodeops[type] = NULL;
}

static struct tid_user_buf_ops *get_bufops(u16 type)
{
	if (type >= HFI2_MAX_MEMINFO_ENTRIES)
		return NULL;
	return bufops[type];
}

static struct tid_node_ops *get_nodeops(u16 type)
{
	if (type >= HFI2_MAX_MEMINFO_ENTRIES)
		return NULL;
	return nodeops[type];
}

int hfi2_tid_user_buf_init(u16 pset_size, unsigned long vaddr,
			   unsigned long length, bool notify,
			   struct tid_user_buf_ops *ops, u16 type,
			   struct tid_user_buf *tbuf)
{
	tbuf->vaddr = vaddr;
	tbuf->length = length;
	tbuf->use_mn = notify;
	tbuf->psets = kcalloc(pset_size, sizeof(*tbuf->psets), GFP_KERNEL);
	if (!tbuf->psets)
		return -ENOMEM;
	tbuf->ops = ops;
	tbuf->type = type;
	return 0;
}

void hfi2_tid_user_buf_free(struct tid_user_buf *tbuf)
{
	kfree(tbuf->psets);
	tbuf->psets = NULL;
}

/**
 * create_user_buf - create user buf for @memtype, store at @*ubuf
 * @fd: filedata pointer
 * @memtype: memory type (one of HFI2_MEMINFO_TYPE* defines)
 * @tinfo: TID info from user
 * @allow_unaligned: whether unaligned buffers are permitted
 * @ubuf: output pointer for the created user buffer
 *
 * Return: 0 on success, non-zero on failure.
 */
static int create_user_buf(struct hfi2_filedata *fd, u16 memtype,
			   struct hfi2_tid_info *tinfo, bool allow_unaligned,
			   struct tid_user_buf **ubuf)
{
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	struct tid_user_buf_ops *ops;
	int ret;

	ops = get_bufops(memtype);
	if (!ops)
		return -EINVAL;

	if (tinfo->length == 0)
		return -EINVAL;

	ret = ops->init(uctxt->expected_count, fd->use_mn, tinfo->vaddr,
			tinfo->length, allow_unaligned, ubuf);
	if (ret)
		return ret;

	return 0;
}

static int page_array_iter_next(struct hfi2_page_iter *piter)
{
	struct page_array_iter *iter =
		container_of(piter, struct page_array_iter, common);

	if (!iter->tbuf->psets || !iter->tbuf->n_psets)
		return -EINVAL;

	iter->setidx++;

	return (iter->setidx < iter->tbuf->n_psets);
}

static void page_array_iter_free(struct hfi2_page_iter *piter)
{
	struct page_array_iter *iter =
		container_of(piter, struct page_array_iter, common);

	kfree(iter);
}

static struct hfi2_page_iter_ops page_array_iter_ops = {
	.next = page_array_iter_next,
	.free = page_array_iter_free
};

static struct hfi2_page_iter *tid_user_buf_iter_begin(struct tid_user_buf *tbuf)
{
	struct page_array_iter *iter;

	if (!tbuf->psets || !tbuf->n_psets)
		return ERR_PTR(-EINVAL);

	iter = kzalloc_obj(iter, GFP_KERNEL);
	if (!iter)
		return ERR_PTR(-ENOMEM);

	iter->common.ops = &page_array_iter_ops;
	iter->tbuf = tbuf;

	return &iter->common;
}

static struct hfi2_page_iter *create_dma_iter(struct tid_user_buf *tbuf)
{
	if (tbuf->ops->iter_begin)
		return tbuf->ops->iter_begin(tbuf);

	return tid_user_buf_iter_begin(tbuf);
}

/*
 * Get number of TID-ready pinned-pagesets for @tbuf using hfi2_page_iter.
 *
 * @return >= 0 for number of pagesets, < 0 on error.
 */
static int pagesets_iter(struct tid_user_buf *tbuf, int cap)
{
	struct hfi2_page_iter *iter;
	int p = 0;
	int ret;

	iter = create_dma_iter(tbuf);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	while (true) {
		if (p >= cap)
			break;
		p++;
		ret = iter->ops->next(iter);
		if (ret < 0)
			goto bail;
		else if (!ret)
			break;
	}
	ret = p;
bail:
	iter->ops->free(iter);
	return ret;
}

/*
 * Get number of TID-ready pinned-pagesets for @tbuf.
 *
 * Each pageset is a physically contiguous range of pages and:
 * - Starts on a 4KiB-aligned address.
 * - Length is power-of-two in range [4KiB,2MiB].
 *
 * @cap hint on how many pagesets can be returned.
 *
 * @return >= 0 number of pagesets, < 0 on error.
 */
static int pagesets(struct tid_user_buf *tbuf, int cap)
{
	int ret;

	if (tbuf->ops->find_phys_blocks) {
		ret = tbuf->ops->find_phys_blocks(tbuf, cap);
		if (ret)
			return (ret < 0 ? ret : -EFAULT);

		return tbuf->n_psets;
	}

	/* No find_phys_blocks(); count using iterator */
	return pagesets_iter(tbuf, cap);
}

/*
 * RcvArray entry allocation for Expected Receives is done by the
 * following algorithm:
 *
 * The context keeps 3 lists of groups of RcvArray entries:
 *   1. List of empty groups - tid_group_list
 *      This list is created during user context creation and
 *      contains elements which describe sets (of 8) of empty
 *      RcvArray entries.
 *   2. List of partially used groups - tid_used_list
 *      This list contains sets of RcvArray entries which are
 *      not completely used up. Another mapping request could
 *      use some of all of the remaining entries.
 *   3. List of full groups - tid_full_list
 *      This is the list where sets that are completely used
 *      up go.
 *
 * An attempt to optimize the usage of RcvArray entries is
 * made by finding all sets of physically contiguous pages in a
 * user's buffer.
 * These physically contiguous sets are further split into
 * sizes supported by the receive engine of the HFI. The
 * resulting sets of pages are stored in struct tid_pageset,
 * which describes the sets as:
 *    * .count - number of pages in this set
 *    * .idx - starting index into struct page ** array
 *                    of this set
 *
 * From this point on, the algorithm deals with the page sets
 * described above. The number of pagesets is divided by the
 * RcvArray group size to produce the number of full groups
 * needed.
 *
 * Groups from the 3 lists are manipulated using the following
 * rules:
 *   1. For each set of 8 pagesets, a complete group from
 *      tid_group_list is taken, programmed, and moved to
 *      the tid_full_list list.
 *   2. For all remaining pagesets:
 *      2.1 If the tid_used_list is empty and the tid_group_list
 *          is empty, stop processing pageset and return only
 *          what has been programmed up to this point.
 *      2.2 If the tid_used_list is empty and the tid_group_list
 *          is not empty, move a group from tid_group_list to
 *          tid_used_list.
 *      2.3 For each group is tid_used_group, program as much as
 *          can fit into the group. If the group becomes fully
 *          used, move it to tid_full_list.
 */
int hfi2_user_exp_rcv_setup(struct hfi2_filedata *fd,
			    struct hfi2_tid_info *tinfo, bool allow_unaligned,
			    bool do_tidcnt_check)
{
	int ret = 0, need_group = 0, pinned;
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	struct hfi2_devdata *dd = uctxt->dd;
	/*
	 * mapped and mapped_pages are implementation-sized pages, not
	 * EXP_TID_ADDR_SIZE-sized.
	 */
	unsigned int ngroups, pageset_count, tididx = 0, mapped,
					     mapped_pages = 0;

	u16 memtype = (tinfo->flags & HFI2_TID_UPDATE_FLAGS_MEMINFO_MASK);
	struct tid_user_buf *tidbuf;
	struct hfi2_page_iter *iter;
	u32 *tidlist = NULL;
	unsigned int psets;

	trace_hfi2_exp_tid_update(uctxt->ctxt, fd->subctxt, tinfo);

	ret = create_user_buf(fd, memtype, tinfo, allow_unaligned, &tidbuf);
	if (ret)
		return ret;

	pinned = tidbuf->ops->pin_pages(fd, tidbuf);
	if (pinned <= 0) {
		dd_dev_warn_ratelimited(dd,
					"%s: Failed to pin %lu bytes (%d)\n",
					__func__, tidbuf->length, pinned);
		ret = (pinned < 0) ? pinned : -ENOSPC;
		goto fail_free;
	}

	/* Cannot program TIDs for < EXP_TID_ADDR_SIZE pages */
	if (tidbuf->ops->page_size(tidbuf) < EXP_TID_ADDR_SIZE) {
		ret = -EOPNOTSUPP;
		goto fail_unpin;
	}

	/* Find sets of physically contiguous pages */
	ret = pagesets(tidbuf, pinned);
	if (ret < 0)
		goto fail_unpin;
	psets = (unsigned int)ret;

	/* Reserve the number of expected tids to be used. */
	spin_lock(&fd->tid_lock);
	if (fd->tid_used + psets > fd->tid_limit)
		pageset_count = fd->tid_limit - fd->tid_used;
	else
		pageset_count = psets;
	fd->tid_used += pageset_count;
	spin_unlock(&fd->tid_lock);

	if (!pageset_count) {
		ret = -ENOSPC;
		goto fail_unreserve;
	}

	ngroups = pageset_count / dd->rcv_entries.group_size;
	tidlist = kcalloc(pageset_count, sizeof(*tidlist), GFP_KERNEL);
	if (!tidlist) {
		ret = -ENOMEM;
		goto fail_unreserve;
	}

	tididx = 0;
	iter = create_dma_iter(tidbuf);
	if (IS_ERR(iter)) {
		ret = PTR_ERR(iter);
		goto fail_unreserve;
	} else if (!iter) {
		ret = -EFAULT;
		goto fail_unreserve;
	}

	/*
	 * From this point on, we are going to be using shared (between master
	 * and subcontexts) context resources. We need to take the lock.
	 */
	mutex_lock(&uctxt->exp_mutex);
	/*
	 * The first step is to program the RcvArray entries which are complete
	 * groups.
	 */
	while (ngroups && uctxt->tid_group_list.count) {
		struct tid_group *grp = tid_group_pop(&uctxt->tid_group_list);

		ret = program_rcvarray(fd, tidbuf, grp,
				       dd->rcv_entries.group_size, tidlist,
				       &tididx, iter, &mapped);
		/*
		 * If there was a failure to program the RcvArray
		 * entries for the entire group, reset the grp fields
		 * and add the grp back to the free group list.
		 */
		if (ret <= 0) {
			tid_group_add_tail(grp, &uctxt->tid_group_list);
			hfi2_cdbg(TID, "Failed to program RcvArray group %d",
				  ret);
			goto unlock;
		}

		tid_group_add_tail(grp, &uctxt->tid_full_list);
		ngroups--;
		mapped_pages += mapped;
	}

	while (tididx < pageset_count) {
		struct tid_group *grp, *ptr;
		/*
		 * If we don't have any partially used tid groups, check
		 * if we have empty groups. If so, take one from there and
		 * put in the partially used list.
		 */
		if (!uctxt->tid_used_list.count || need_group) {
			if (!uctxt->tid_group_list.count)
				goto unlock;

			grp = tid_group_pop(&uctxt->tid_group_list);
			tid_group_add_tail(grp, &uctxt->tid_used_list);
			need_group = 0;
		}
		/*
		 * There is an optimization opportunity here - instead of
		 * fitting as many page sets as we can, check for a group
		 * later on in the list that could fit all of them.
		 */
		list_for_each_entry_safe(grp, ptr, &uctxt->tid_used_list.list,
					 list) {
			unsigned int use = min_t(unsigned int,
						 pageset_count - tididx,
						 grp->size - grp->used);

			ret = program_rcvarray(fd, tidbuf, grp, use, tidlist,
					       &tididx, iter, &mapped);
			if (ret < 0) {
				hfi2_cdbg(
					TID,
					"Failed to program RcvArray entries %d",
					ret);
				goto unlock;
			} else if (ret > 0) {
				if (grp->used == grp->size)
					tid_group_move(grp,
						       &uctxt->tid_used_list,
						       &uctxt->tid_full_list);
				mapped_pages += mapped;
				need_group = 0;
				/* Check if we are done so we break out early */
				if (tididx >= pageset_count)
					break;
			} else if (WARN_ON(ret == 0)) {
				/*
				 * If ret is 0, we did not program any entries
				 * into this group, which can only happen if
				 * we've screwed up the accounting somewhere.
				 * Warn and try to continue.
				 */
				need_group = 1;
			}
		}
	}
unlock:
	mutex_unlock(&uctxt->exp_mutex);

	iter->ops->free(iter);

	/*
	 * mapped_pages is based on implementation page size, not expected
	 * receive addressing.
	 *
	 * E.g. if implementation uses 64KiB pages and expected receive
	 * addressing is based on 4KiB, for 128KiB of mapped memory,
	 * mapped_pages=2 not mapped_pages=32.
	 */
	hfi2_cdbg(TID, "total mapped: tidpairs:%u pages:%u (%d)", tididx,
		  mapped_pages, ret);

	/* fail if nothing was programmed, set error if none provided */
	if (tididx == 0) {
		if (ret >= 0)
			ret = -ENOSPC;
		goto fail_unreserve;
	}

	/* adjust reserved tid_used to actual count */
	spin_lock(&fd->tid_lock);
	fd->tid_used -= pageset_count - tididx;
	spin_unlock(&fd->tid_lock);

	/* unpin all pages not covered by a TID */
	tidbuf->ops->unpin_pages(fd, tidbuf, mapped_pages,
				 pinned - mapped_pages);

	/* check for an invalidate during setup */
	if (tidbuf->ops->invalidated(tidbuf)) {
		ret = -EBUSY;
		goto fail_unprogram;
	}

	/* verify claimed incoming TID buffer has enough entries for result */
	if (do_tidcnt_check && tinfo->tidcnt < tididx) {
		ret = -ENOSPC;
		goto fail_unprogram;
	}

	tinfo->tidcnt = tididx;
	/* Should never happen but detect if somehow implementation pinned too many pages */
	if (check_mul_overflow(mapped_pages, tidbuf->ops->page_size(tidbuf),
			       &tinfo->length)) {
		ret = -EFAULT;
		goto fail_unprogram;
	}

	if (copy_to_user(u64_to_user_ptr(tinfo->tidlist), tidlist,
			 sizeof(tidlist[0]) * tididx)) {
		ret = -EFAULT;
		goto fail_unprogram;
	}

	tidbuf->ops->unnotify(tidbuf);
	tidbuf->ops->free(tidbuf);
	kfree(tidlist);
	return 0;

fail_unprogram:
	/* unprogram, unmap, and unpin all allocated TIDs directly
	 * using the kernel tidlist pointer, bypassing the user-copy
	 * path in hfi2_user_exp_rcv_clear which would fail SMAP.
	 */
	mutex_lock(&uctxt->exp_mutex);
	for (unsigned int i = 0; i < tididx; i++)
		unprogram_rcvarray(fd, tidlist[i]);
	spin_lock(&fd->tid_lock);
	fd->tid_used -= tididx;
	spin_unlock(&fd->tid_lock);
	mutex_unlock(&uctxt->exp_mutex);
	pinned = 0; /* nothing left to unpin */
	pageset_count = 0; /* nothing left reserved */
fail_unreserve:
	spin_lock(&fd->tid_lock);
	fd->tid_used -= pageset_count;
	spin_unlock(&fd->tid_lock);
fail_unpin:
	tidbuf->ops->unnotify(tidbuf);
	if (pinned > 0)
		tidbuf->ops->unpin_pages(fd, tidbuf, 0, pinned);
fail_free:
	tidbuf->ops->free(tidbuf);
	kfree(tidlist);
	return ret;
}

int hfi2_user_exp_rcv_clear(struct hfi2_filedata *fd,
			    struct hfi2_tid_info *tinfo)
{
	int ret = 0;
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	u32 *tidinfo;
	unsigned int tididx;

	if (unlikely(tinfo->tidcnt > fd->tid_used))
		return -EINVAL;

	tidinfo = memdup_array_user(u64_to_user_ptr(tinfo->tidlist),
				    tinfo->tidcnt, sizeof(tidinfo[0]));
	if (IS_ERR(tidinfo))
		return PTR_ERR(tidinfo);

	mutex_lock(&uctxt->exp_mutex);
	for (tididx = 0; tididx < tinfo->tidcnt; tididx++) {
		ret = unprogram_rcvarray(fd, tidinfo[tididx]);
		if (ret) {
			hfi2_cdbg(TID, "Failed to unprogram rcv array %d", ret);
			break;
		}
	}
	spin_lock(&fd->tid_lock);
	fd->tid_used -= tididx;
	spin_unlock(&fd->tid_lock);
	tinfo->tidcnt = tididx;
	mutex_unlock(&uctxt->exp_mutex);

	kfree(tidinfo);
	return ret;
}

int hfi2_user_exp_rcv_invalid(struct hfi2_filedata *fd,
			      struct hfi2_tid_info *tinfo, bool do_tidcnt_check)
{
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	unsigned long *ev =
		uctxt->dd->events + (uctxt_offset(uctxt) + fd->subctxt);
	u32 *array;
	int ret = 0;

	if (!fd->invalid_tids)
		return -EINVAL;

	/*
	 * copy_to_user() can sleep, which will leave the invalid_lock
	 * locked and cause the MMU notifier to be blocked on the lock
	 * for a long time.
	 * Copy the data to a local buffer so we can release the lock.
	 */
	array = kcalloc(uctxt->expected_count, sizeof(*array), GFP_KERNEL);
	if (!array)
		return -EFAULT;

	spin_lock(&fd->invalid_lock);
	if (do_tidcnt_check && tinfo->tidcnt < fd->invalid_tid_idx) {
		ret = -ENOSPC;
	} else if (fd->invalid_tid_idx) {
		memcpy(array, fd->invalid_tids,
		       sizeof(*array) * fd->invalid_tid_idx);
		memset(fd->invalid_tids, 0,
		       sizeof(*fd->invalid_tids) * fd->invalid_tid_idx);
		tinfo->tidcnt = fd->invalid_tid_idx;
		fd->invalid_tid_idx = 0;
		/*
		 * Reset the user flag while still holding the lock.
		 * Otherwise, PSM can miss events.
		 */
		clear_bit(_HFI2_EVENT_TID_MMU_NOTIFY_BIT, ev);
	} else {
		tinfo->tidcnt = 0;
	}
	spin_unlock(&fd->invalid_lock);

	if (ret == 0 && tinfo->tidcnt) {
		if (copy_to_user((void __user *)tinfo->tidlist, array,
				 sizeof(*array) * tinfo->tidcnt))
			ret = -EFAULT;
	}
	kfree(array);

	return ret;
}

/*
 * Convert @node's implementation-defined npages to number of
 * EXP_TID_ADDR_SIZE pages.
 *
 * @return number of EXP_TID_ADDR_SIZE pages
 */
static unsigned int node_npages(const struct tid_rb_node *node)
{
	/* Underflow/overflow protection here depends on other places enforcing that:
	 *   page_shift >= EXP_TID_ADDR_SHIFT
	 *   node->npages * (1 << page_shift) <= MAX_EXPECTED_BUFFER
	 */
	return (node->npages << node->page_shift) >> EXP_TID_ADDR_SHIFT;
}

/*
 * DMA-map and program single TID entry for physically contiguous pinned page
 * range.
 *
 * @fd
 * @tbuf
 * @rcventry
 * @grp
 * @iter
 * @onode out node. Undefined on error.
 *
 * @return 0 on success, non-zero on error.
 */
static int set_rcvarray_entry(struct hfi2_filedata *fd,
			      struct tid_user_buf *tbuf, u32 rcventry,
			      struct tid_group *grp,
			      struct hfi2_page_iter *iter,
			      struct tid_rb_node **onode)
{
	struct tid_node_ops *nodeops = get_nodeops(tbuf->type);
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	struct hfi2_devdata *dd = uctxt->dd;
	struct tid_rb_node *node;
	int ret;

	if (WARN_ON(!nodeops))
		return -EINVAL;

	node = nodeops->init(fd, tbuf, rcventry, grp, iter);
	if (IS_ERR(node))
		return PTR_ERR(node);

	if (node->use_mn) {
		ret = node->ops->register_notify(node);
		if (ret)
			goto out_unmap;
	}
	*onode = node;
	fd->entry_to_rb[node->rcventry] = node;

	/* RcvArray entry requires EXP_TID_ADDR_SIZE page-size npages */
	dd->params->put_tid(uctxt, rcventry, PT_EXPECTED, node->dma_addr,
			    ilog2(node_npages(node)) + 1, false);

	trace_hfi2_exp_tid_reg(uctxt->ctxt, fd->subctxt, rcventry,
			       node_npages(node), node->vaddr, node->phys,
			       node->dma_addr, node->type);
	return 0;
out_unmap:
	hfi2_cdbg(TID, "Failed to insert RB node %u 0x%lx, 0x%lx %d",
		  node->rcventry, node->vaddr, node->phys, ret);

	node->ops->dma_unmap(node);
	node->ops->free(node);

	return -EFAULT;
}

/**
 * program_rcvarray() - program an RcvArray group with receive buffers
 * @fd: filedata pointer
 * @tbuf: pointer to struct tid_user_buf that has the user buffer starting
 *	  virtual address, buffer length, page pointers, pagesets (array of
 *	  struct tid_pageset holding information on physically contiguous
 *	  chunks from the user buffer), and other fields.
 * @grp: RcvArray group
 * @count: number of struct tid_pageset's to program
 * @tidlist: the array of u32 elements when the information about the
 *           programmed RcvArray entries is to be encoded.
 * @tididx: starting offset into tidlist
 * @iter: iterator for the user buffer pagesets
 * @pmapped: (output parameter) number of implementation pages programmed into the RcvArray
 *           entries.
 *
 * This function will program up to 'count' number of RcvArray entries from the
 * group 'grp'. To make best use of write-combining writes, the function will
 * perform writes to the unused RcvArray entries which will be ignored by the
 * HW. Each RcvArray entry will be programmed with a physically contiguous
 * buffer chunk from the user's virtual buffer.
 *
 * Return:
 * -EINVAL if the requested count is larger than the size of the group,
 * -ENOMEM or -EFAULT on error from set_rcvarray_entry(), or
 * number of RcvArray entries programmed.
 */
static int program_rcvarray(struct hfi2_filedata *fd, struct tid_user_buf *tbuf,
			    struct tid_group *grp, u16 count, u32 *tidlist,
			    unsigned int *tididx, struct hfi2_page_iter *iter,
			    unsigned int *pmapped)
{
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	u16 idx;
	u32 tidinfo = 0, rcventry, useidx = 0;
	int mapped = 0;
	int ret;

	/* Count should never be larger than the group size */
	if (count > grp->size)
		return -EINVAL;

	/* Find the first unused entry in the group */
	for (idx = 0; idx < grp->size; idx++) {
		if (!(grp->map & (1 << idx))) {
			useidx = idx;
			break;
		}
		uctxt->dd->params->rcv_array_wc_fill(uctxt, grp->base + idx,
						     PT_EXPECTED);
	}

	idx = 0;
	while (idx < count) {
		struct tid_rb_node *node;

		/*
		 * If this entry in the group is used, move to the next one.
		 * If we go past the end of the group, exit the loop.
		 */
		if (useidx >= grp->size) {
			break;
		} else if (grp->map & (1 << useidx)) {
			uctxt->dd->params->rcv_array_wc_fill(
				uctxt, grp->base + useidx, PT_EXPECTED);
			useidx++;
			continue;
		}

		rcventry = grp->base + useidx;
		ret = set_rcvarray_entry(fd, tbuf, rcventry, grp, iter, &node);
		if (ret)
			return ret;
		mapped += node->npages;

		/* In-memory TIDs requires EXP_TID_ADDR_SIZE page-size npages */
		tidinfo = create_tid(rcventry, node_npages(node));
		tidlist[(*tididx)++] = tidinfo;
		grp->used++;
		grp->map |= 1 << useidx++;
		idx++;
		ret = iter->ops->next(iter);
		if (ret < 0) {
			/* Make sure ret won't be treated as a success value */
			return ret;
		} else if (!ret && idx < count) {
			/* Exhausted all DMA-pagesets but not done programming */
			return -EFAULT;
		}
	}

	/* Fill the rest of the group with "blank" writes */
	for (; useidx < grp->size; useidx++)
		uctxt->dd->params->rcv_array_wc_fill(uctxt, grp->base + useidx,
						     PT_EXPECTED);
	*pmapped = mapped;
	return idx;
}

static int unprogram_rcvarray(struct hfi2_filedata *fd, u32 tidinfo)
{
	struct hfi2_ctxtdata *uctxt = fd->uctxt;
	struct hfi2_devdata *dd = uctxt->dd;
	struct tid_rb_node *node;
	u32 tidctrl = EXP_TID_GET(tidinfo, CTRL);
	u32 tididx = EXP_TID_GET(tidinfo, IDX) << 1, rcventry;

	if (tidctrl == 0x3 || tidctrl == 0x0)
		return -EINVAL;

	rcventry = tididx + (tidctrl - 1);

	if (rcventry >= uctxt->expected_count) {
		dd_dev_err(dd,
			   "Invalid RcvArray entry (%u) index for ctxt %u\n",
			   rcventry, uctxt->ctxt);
		return -EINVAL;
	}

	node = fd->entry_to_rb[rcventry];
	if (!node || node->rcventry != rcventry)
		return -EBADF;

	node->ops->unregister_notify(node);
	cacheless_tid_rb_remove(fd, node);

	return 0;
}

static void __clear_tid_node(struct hfi2_filedata *fd, struct tid_rb_node *node)
{
	struct hfi2_ctxtdata *uctxt = fd->uctxt;

	mutex_lock(&node->invalidate_mutex);
	if (node->freed)
		goto done;
	node->freed = true;

	trace_hfi2_exp_tid_unreg(uctxt->ctxt, fd->subctxt, node->rcventry,
				 node_npages(node), node->vaddr, node->phys,
				 node->dma_addr, node->type);

	/* Make sure device has seen the write before pages are unpinned */
	uctxt->dd->params->put_tid(uctxt, node->rcventry, PT_EXPECTED, 0, 0,
				   true);

	node->ops->unpin_pages(fd, node);
done:
	mutex_unlock(&node->invalidate_mutex);
}

static void clear_tid_node(struct hfi2_filedata *fd, struct tid_rb_node *node)
{
	struct hfi2_ctxtdata *uctxt = fd->uctxt;

	__clear_tid_node(fd, node);

	node->grp->used--;
	node->grp->map &= ~(1 << (node->rcventry - node->grp->base));

	if (node->grp->used == node->grp->size - 1)
		tid_group_move(node->grp, &uctxt->tid_full_list,
			       &uctxt->tid_used_list);
	else if (!node->grp->used)
		tid_group_move(node->grp, &uctxt->tid_used_list,
			       &uctxt->tid_group_list);
	node->ops->free(node);
}

/*
 * As a simple helper for hfi2_user_exp_rcv_free, this function deals with
 * clearing nodes in the non-cached case.
 */
static void unlock_exp_tids(struct hfi2_ctxtdata *uctxt,
			    struct exp_tid_set *set, struct hfi2_filedata *fd)
{
	struct tid_group *grp, *ptr;
	int i;

	list_for_each_entry_safe(grp, ptr, &set->list, list) {
		list_del_init(&grp->list);

		for (i = 0; i < grp->size; i++) {
			if (grp->map & (1 << i)) {
				u16 rcventry = grp->base + i;
				struct tid_rb_node *node;

				node = fd->entry_to_rb[rcventry];
				if (!node || node->rcventry != rcventry)
					continue;

				node->ops->unregister_notify(node);
				cacheless_tid_rb_remove(fd, node);
			}
		}
	}
}

/**
 * hfi2_user_exp_rcv_invalidate - Unprogram TID for @node, updating user TID
 * invalidation events when @node->fdata->use_mn is true.
 * @node: TID rb node to unprogram
 */
void hfi2_user_exp_rcv_invalidate(struct tid_rb_node *node)
{
	struct hfi2_filedata *fdata = node->fdata;
	struct hfi2_ctxtdata *uctxt = fdata->uctxt;

	trace_hfi2_exp_tid_inval(uctxt->ctxt, fdata->subctxt, node->vaddr,
				 node->rcventry, node_npages(node),
				 node->dma_addr, node->type);

	/* clear the hardware rcvarray entry */
	__clear_tid_node(fdata, node);

	/* User TID invalidation events not in use, nothing else to do */
	if (!node->use_mn)
		return;

	spin_lock(&fdata->invalid_lock);
	if (fdata->invalid_tid_idx < uctxt->expected_count) {
		/* In-memory TIDs requires EXP_TID_ADDR_SIZE page-size npages */
		fdata->invalid_tids[fdata->invalid_tid_idx] =
			create_tid(node->rcventry, node_npages(node));
		if (!fdata->invalid_tid_idx) {
			unsigned long *ev;

			/*
			 * hfi2_set_uevent_bits() sets a user event flag
			 * for all processes. Because calling into the
			 * driver to process TID cache invalidations is
			 * expensive and TID cache invalidations are
			 * handled on a per-process basis, we can
			 * optimize this to set the flag only for the
			 * process in question.
			 */
			ev = uctxt->dd->events +
			     (uctxt_offset(uctxt) + fdata->subctxt);
			set_bit(_HFI2_EVENT_TID_MMU_NOTIFY_BIT, ev);
		}
		fdata->invalid_tid_idx++;
	}
	spin_unlock(&fdata->invalid_lock);
}

static void cacheless_tid_rb_remove(struct hfi2_filedata *fdata,
				    struct tid_rb_node *tnode)
{
	fdata->entry_to_rb[tnode->rcventry] = NULL;
	clear_tid_node(fdata, tnode);
}
