// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 * Copyright(c) 2015-2018 Intel Corporation.
 */
#include "user_exp_rcv.h"

/*
 * Host memory TID implementation.
 */
struct system_tid_user_buf {
	struct tid_user_buf common;
	struct mmu_interval_notifier notifier;
	/*
	 * cover_mutex serializes mmu_interval_read_retry() and
	 * mmu_interval_set_seq() on notifier
	 */
	struct mutex cover_mutex;
	unsigned int npages;
	struct page **pages;
	long mmu_seq;
};

struct system_tid_node {
	struct tid_rb_node common;
	struct mmu_interval_notifier notifier;
	struct page *pages[];
};

static bool sys_tid_invalidate(struct mmu_interval_notifier *mni,
			       const struct mmu_notifier_range *range,
			       unsigned long cur_seq);

static bool sys_cover_invalidate(struct mmu_interval_notifier *mni,
				 const struct mmu_notifier_range *range,
				 unsigned long cur_seq);
static void sys_unnotify(struct tid_user_buf *tbuf);

/*
 * Still takes a tid_user_buf, not system_tid_user_buf since
 * this may be called through interface in addition to internally.
 */
static void sys_user_buf_free(struct tid_user_buf *tbuf);

static const struct mmu_interval_notifier_ops tid_mn_ops = {
	.invalidate = sys_tid_invalidate,
};

static const struct mmu_interval_notifier_ops tid_cover_ops = {
	.invalidate = sys_cover_invalidate,
};

static inline int num_user_pages(unsigned long addr,
				 unsigned long len)
{
	const unsigned long spage = addr & PAGE_MASK;
	const unsigned long epage = (addr + len - 1) & PAGE_MASK;

	return 1 + ((epage - spage) >> PAGE_SHIFT);
}

static inline struct mm_struct *mm_from_tid_node(struct tid_rb_node *node)
{
	struct system_tid_node *snode =
		container_of(node, struct system_tid_node, common);

	return snode->notifier.mm;
}

static bool sys_tid_invalidate(struct mmu_interval_notifier *mni,
			       const struct mmu_notifier_range *range,
			       unsigned long cur_seq)
{
	struct system_tid_node *node =
		container_of(mni, struct system_tid_node, notifier);

	if (node->common.freed)
		return true;

	/* take action only if unmapping */
	if (range->event != MMU_NOTIFY_UNMAP)
		return true;

	hfi2_user_exp_rcv_invalidate(&node->common);

	return true;
}

static int sys_node_register_notify(struct tid_rb_node *node)
{
	struct system_tid_node *snode =
		container_of(node, struct system_tid_node, common);
	const u32 length = node->npages * (1 << node->page_shift);

	return mmu_interval_notifier_insert(&snode->notifier, current->mm,
					    node->vaddr, length,
					    &tid_mn_ops);
}

static void sys_node_unregister_notify(struct tid_rb_node *node)
{
	struct system_tid_node *snode =
		container_of(node, struct system_tid_node, common);

	if (snode->common.use_mn)
		mmu_interval_notifier_remove(&snode->notifier);
}

static void sys_node_dma_unmap(struct tid_rb_node *node)
{
	struct hfi2_devdata *dd = node->fdata->uctxt->dd;

	dma_unmap_single(&dd->pcidev->dev, node->dma_addr, node->npages * PAGE_SIZE,
			 DMA_FROM_DEVICE);
}

/*
 * Release pinned receive buffer pages.
 */
static void sys_node_unpin_pages(struct hfi2_filedata *fd,
				 struct tid_rb_node *node)
{
	struct system_tid_node *snode =
		container_of(node, struct system_tid_node, common);
	struct page **pages;
	struct hfi2_devdata *dd = fd->uctxt->dd;
	struct mm_struct *mm;

	dma_unmap_single(&dd->pcidev->dev, node->dma_addr,
			 node->npages * PAGE_SIZE, DMA_FROM_DEVICE);
	pages = &snode->pages[0];
	mm = mm_from_tid_node(node);
	hfi2_release_user_pages(mm, pages, node->npages, true);
	fd->tid_n_pinned -= node->npages;
}

static struct tid_node_ops sys_nodeops;

static struct tid_rb_node *sys_node_init(struct hfi2_filedata *fd,
					 struct tid_user_buf *tbuf,
					 u32 rcventry,
					 struct tid_group *grp,
					 struct hfi2_page_iter *piter)
{
	struct system_tid_user_buf *sbuf =
		container_of(tbuf, struct system_tid_user_buf, common);
	struct page_array_iter *iter =
		container_of(piter, struct page_array_iter, common);
	struct hfi2_devdata *dd = fd->uctxt->dd;
	struct system_tid_node *snode;
	unsigned int npages;
	struct page **pages;
	dma_addr_t phys;
	u16 pageidx;

	if (iter->setidx >= tbuf->n_psets)
		return ERR_PTR(-EINVAL);

	npages = tbuf->psets[iter->setidx].count;
	pageidx = tbuf->psets[iter->setidx].idx;
	pages = sbuf->pages + pageidx;

	if (!npages)
		return ERR_PTR(-EINVAL);

	/*
	 * Allocate snode first so we can handle a potential failure before
	 * we've programmed anything.
	 */
	snode = kzalloc(struct_size(snode, pages, npages), GFP_KERNEL);
	if (!snode)
		return ERR_PTR(-ENOMEM);

	phys = dma_map_single(&dd->pcidev->dev, __va(page_to_phys(pages[0])),
			      npages * PAGE_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(&dd->pcidev->dev, phys)) {
		dd_dev_err(dd, "Failed to DMA map Exp Rcv pages 0x%llx\n",
			   phys);
		kfree(snode);
		return ERR_PTR(-EFAULT);
	}

	snode->common.fdata = fd;
	mutex_init(&snode->common.invalidate_mutex);
	snode->common.phys = page_to_phys(pages[0]);
	snode->common.npages = npages;
	snode->common.page_shift = PAGE_SHIFT;
	snode->common.rcventry = rcventry;
	snode->common.dma_addr = phys;
	snode->common.vaddr = tbuf->vaddr + (pageidx * PAGE_SIZE);
	snode->common.grp = grp;
	snode->common.freed = false;
	snode->common.ops = &sys_nodeops;
	snode->common.use_mn = fd->use_mn;
	snode->common.type = HFI2_MEMINFO_TYPE_SYSTEM;
	memcpy(snode->pages, pages, flex_array_size(snode, pages, npages));

	return &snode->common;
}

static void sys_node_free(struct tid_rb_node *node)
{
	struct system_tid_node *snode =
		container_of(node, struct system_tid_node, common);

	kfree(snode);
}

static struct tid_node_ops sys_nodeops = {
	.init = sys_node_init,
	.free = sys_node_free,
	.register_notify = sys_node_register_notify,
	.unregister_notify = sys_node_unregister_notify,
	.dma_unmap = sys_node_dma_unmap,
	.unpin_pages = sys_node_unpin_pages,
};

static unsigned int sys_page_size(struct tid_user_buf *tbuf)
{
	return PAGE_SIZE;
}

/*
 * Invalidation during insertion callback.
 */
static bool sys_cover_invalidate(struct mmu_interval_notifier *mni,
				 const struct mmu_notifier_range *range,
				 unsigned long cur_seq)
{
	struct system_tid_user_buf *sbuf =
		container_of(mni, struct system_tid_user_buf, notifier);

	/* take action only if unmapping */
	if (range->event == MMU_NOTIFY_UNMAP) {
		mutex_lock(&sbuf->cover_mutex);
		mmu_interval_set_seq(mni, cur_seq);
		mutex_unlock(&sbuf->cover_mutex);
	}

	return true;
}

static struct tid_user_buf_ops sys_bufops;

/*
 * System memory never honors @allow_unaligned.
 */
static int sys_user_buf_init(u16 expected_count, bool notify,
			     unsigned long vaddr, unsigned long length,
			     bool allow_unaligned,
			     struct tid_user_buf **tbuf)
{
	struct system_tid_user_buf *sbuf;
	int ret;

	if (!IS_ALIGNED(vaddr, max(EXP_TID_ADDR_SIZE, PAGE_SIZE)))
		return -EINVAL;

	sbuf = kzalloc_obj(sbuf, GFP_KERNEL);
	if (!sbuf)
		return -ENOMEM;
	*tbuf = &sbuf->common;
	mutex_init(&sbuf->cover_mutex);

	ret = hfi2_tid_user_buf_init(expected_count, vaddr, length, notify, &sys_bufops,
				HFI2_MEMINFO_TYPE_SYSTEM, *tbuf);
	if (ret)
		goto fail_release_mem;

	sbuf->npages = num_user_pages(vaddr, length);

	return 0;
fail_release_mem:
	sys_user_buf_free(&sbuf->common);
	return ret;
}

static void sys_user_buf_free(struct tid_user_buf *tbuf)
{
	struct system_tid_user_buf *sbuf =
		container_of(tbuf, struct system_tid_user_buf, common);

	kfree(sbuf->pages);
	hfi2_tid_user_buf_free(tbuf);
	kfree(sbuf);
}

/*
 * Pin receive buffer pages.
 */
static int pin_rcv_pages(struct hfi2_filedata *fd, struct system_tid_user_buf *sbuf)
{
	int pinned;
	unsigned int npages = sbuf->npages;
	unsigned long vaddr = sbuf->common.vaddr;
	struct page **pages = NULL;
	struct hfi2_devdata *dd = fd->uctxt->dd;

	if (npages > fd->uctxt->expected_count) {
		dd_dev_err(dd, "Expected buffer too big\n");
		return -EINVAL;
	}

	/* Allocate the array of struct page pointers needed for pinning */
	pages = kcalloc(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/*
	 * Pin all the pages of the user buffer. If we can't pin all the
	 * pages, accept the amount pinned so far and program only that.
	 * User space knows how to deal with partially programmed buffers.
	 */
	if (!hfi2_can_pin_pages(dd, current->mm, fd->tid_n_pinned, npages)) {
		kfree(pages);
		return -ENOMEM;
	}

	pinned = hfi2_acquire_user_pages(current->mm, vaddr, npages, true, pages);
	if (pinned <= 0) {
		kfree(pages);
		return pinned;
	}
	sbuf->pages = pages;
	fd->tid_n_pinned += pinned;
	return pinned;
}

static int sys_pin_pages(struct hfi2_filedata *fd, struct tid_user_buf *tbuf)
{
	struct system_tid_user_buf *sbuf =
		container_of(tbuf, struct system_tid_user_buf, common);
	int ret;

	if (WARN_ON(fd->use_mn != tbuf->use_mn))
		return -EINVAL;

	if (tbuf->use_mn) {

		ret = mmu_interval_notifier_insert(&sbuf->notifier, current->mm, tbuf->vaddr,
						   sbuf->npages * PAGE_SIZE, &tid_cover_ops);
		if (ret)
			return ret;
		sbuf->mmu_seq = mmu_interval_read_begin(&sbuf->notifier);
	}

	ret = pin_rcv_pages(fd, sbuf);
	if (ret <= 0 && tbuf->use_mn)
		sys_unnotify(tbuf);

	return ret;
}

static void sys_unpin_pages(struct hfi2_filedata *fd,
			    struct tid_user_buf *tbuf,
			    unsigned int idx,
			    unsigned int npages)
{
	struct system_tid_user_buf *sbuf =
		container_of(tbuf, struct system_tid_user_buf, common);
	struct page **pages;
	struct mm_struct *mm;

	pages = &sbuf->pages[idx];
	mm = current->mm;
	hfi2_release_user_pages(mm, pages, npages, false);
	fd->tid_n_pinned -= npages;
}

static int sys_find_phys_blocks(struct tid_user_buf *tidbuf, unsigned int npages)
{
	struct system_tid_user_buf *sbuf =
		container_of(tidbuf, struct system_tid_user_buf, common);
	unsigned int pagecount, pageidx, setcount = 0, i;
	unsigned long pfn, this_pfn;
	struct page **pages = sbuf->pages;
	struct tid_pageset *list = tidbuf->psets;

	if (!npages)
		return -EINVAL;

	/*
	 * Look for sets of physically contiguous pages in the user buffer.
	 * This will allow us to optimize Expected RcvArray entry usage by
	 * using the bigger supported sizes.
	 */
	pfn = page_to_pfn(pages[0]);
	for (pageidx = 0, pagecount = 1, i = 1; i <= npages; i++) {
		this_pfn = i < npages ? page_to_pfn(pages[i]) : 0;

		/*
		 * If the pfn's are not sequential, pages are not physically
		 * contiguous.
		 */
		if (this_pfn != ++pfn) {
			/*
			 * At this point we have to loop over the set of
			 * physically contiguous pages and break them down it
			 * sizes supported by the HW.
			 * There are two main constraints:
			 *     1. The max buffer size is MAX_EXPECTED_BUFFER.
			 *        If the total set size is bigger than that
			 *        program only a MAX_EXPECTED_BUFFER chunk.
			 *     2. The buffer size has to be a power of two. If
			 *        it is not, round down to the closes power of
			 *        2 and program that size.
			 */
			while (pagecount) {
				int maxpages = pagecount;
				u32 bufsize = pagecount * PAGE_SIZE;

				if (bufsize > MAX_EXPECTED_BUFFER)
					maxpages =
						MAX_EXPECTED_BUFFER >>
						PAGE_SHIFT;
				else if (!is_power_of_2(bufsize))
					maxpages =
						rounddown_pow_of_two(bufsize) >>
						PAGE_SHIFT;

				list[setcount].idx = pageidx;
				list[setcount].count = maxpages;
				pagecount -= maxpages;
				pageidx += maxpages;
				setcount++;
			}
			pageidx = i;
			pagecount = 1;
			pfn = this_pfn;
		} else {
			pagecount++;
		}
	}
	tidbuf->n_psets = setcount;
	return 0;
}

static bool sys_invalidated(struct tid_user_buf *tbuf)
{
	struct system_tid_user_buf *sbuf =
		container_of(tbuf, struct system_tid_user_buf, common);
	bool ret = false;

	if (!tbuf->use_mn)
		return false;

	mutex_lock(&sbuf->cover_mutex);
	ret = mmu_interval_read_retry(&sbuf->notifier, sbuf->mmu_seq);
	mutex_unlock(&sbuf->cover_mutex);
	return ret;
}

static void sys_unnotify(struct tid_user_buf *tbuf)
{
	struct system_tid_user_buf *sbuf =
		container_of(tbuf, struct system_tid_user_buf, common);

	if (tbuf->use_mn)
		mmu_interval_notifier_remove(&sbuf->notifier);
}

static struct tid_user_buf_ops sys_bufops = {
	.init = sys_user_buf_init,
	.free = sys_user_buf_free,
	.pin_pages = sys_pin_pages,
	.page_size = sys_page_size,
	.unpin_pages = sys_unpin_pages,
	.find_phys_blocks = sys_find_phys_blocks,
	.invalidated = sys_invalidated,
	.unnotify = sys_unnotify,
};

int hfi2_register_system_tid_ops(void)
{
	return hfi2_register_tid_ops(HFI2_MEMINFO_TYPE_SYSTEM, &sys_bufops, &sys_nodeops);
}

void hfi2_deregister_system_tid_ops(void)
{
	hfi2_deregister_tid_ops(HFI2_MEMINFO_TYPE_SYSTEM);
}
