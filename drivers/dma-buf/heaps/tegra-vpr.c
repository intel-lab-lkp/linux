// SPDX-License-Identifier: GPL-2.0
/*
 * DMA-BUF restricted heap exporter for NVIDIA Video-Protection-Region (VPR)
 *
 * Copyright (C) 2024-2026 NVIDIA Corporation
 */

#define pr_fmt(fmt) "tegra-vpr: " fmt

#include <linux/arm-smccc.h>
#include <linux/cma.h>
#include <linux/debugfs.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/find.h>
#include <linux/of_reserved_mem.h>

#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <linux/freezer.h>

#define CREATE_TRACE_POINTS
#include <trace/events/tegra_vpr.h>

#define TEGRA_VPR_MAX_CHUNKS 64

struct tegra_vpr;

struct tegra_vpr_device {
	struct list_head node;
	struct device *dev;
};

struct tegra_vpr_chunk {
	phys_addr_t start;
	phys_addr_t limit;
	size_t size;

	struct tegra_vpr *vpr;
	struct cma *cma;
	bool active;

	struct page *start_page;
	unsigned int offset;
	unsigned long virt;
	pgoff_t num_pages;

	unsigned int num_buffers;
};

struct tegra_vpr {
	struct device_node *dev_node;
	unsigned long align;
	phys_addr_t base;
	phys_addr_t size;
	bool use_freezer;
	bool resizable;

	struct list_head buffers;
	struct page *start_page;
	unsigned long *bitmap;
	pgoff_t num_pages;

	/* resizable VPR */
	DECLARE_BITMAP(active, TEGRA_VPR_MAX_CHUNKS);
	struct tegra_vpr_chunk *chunks;
	unsigned int num_chunks;

	unsigned int first;
	unsigned int last;

	struct list_head devices;
	struct mutex lock;
};

struct tegra_vpr_buffer {
	struct list_head attachments;
	struct tegra_vpr *vpr;
	struct list_head list;
	struct mutex lock;

	struct page *start_page;
	struct page **pages;
	pgoff_t num_pages;
	phys_addr_t start;
	phys_addr_t limit;
	size_t size;
	int pageno;
	int order;

	DECLARE_BITMAP(chunks, TEGRA_VPR_MAX_CHUNKS);
};

struct tegra_vpr_attachment {
	struct device *dev;
	struct sg_table sgt;
	struct list_head list;
};

#define ARM_SMCCC_TE_FUNC_PROGRAM_VPR 0x3

#define ARM_SMCCC_VENDOR_SIP_TE_PROGRAM_VPR_FUNC_ID		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,			\
			   ARM_SMCCC_SMC_32,			\
			   ARM_SMCCC_OWNER_SIP,			\
			   ARM_SMCCC_TE_FUNC_PROGRAM_VPR)

static int tegra_vpr_set(phys_addr_t base, phys_addr_t size)
{
	struct arm_smccc_res res;

	arm_smccc_smc(ARM_SMCCC_VENDOR_SIP_TE_PROGRAM_VPR_FUNC_ID, base, size,
		      0, 0, 0, 0, 0, &res);

	return res.a0;
}

static int tegra_vpr_get_extents(struct tegra_vpr *vpr, phys_addr_t *base,
				 phys_addr_t *size)
{
	phys_addr_t start = ~0, limit = 0;
	unsigned int i;

	for (i = 0; i < vpr->num_chunks; i++) {
		struct tegra_vpr_chunk *chunk = &vpr->chunks[i];

		if (chunk->active) {
			if (chunk->start < start)
				start = chunk->start;

			if (chunk->limit > limit)
				limit = chunk->limit;
		}
	}

	if (limit > start) {
		*size = limit - start;
		*base = start;
	} else {
		*base = *size = 0;
	}

	return 0;
}

static int tegra_vpr_resize(struct tegra_vpr *vpr)
{
	struct tegra_vpr_device *node;
	phys_addr_t base, size;
	int err, status = 0;

	err = tegra_vpr_get_extents(vpr, &base, &size);
	if (err < 0) {
		pr_err("%s(): failed to get VPR extents: %d\n", __func__, err);
		return err;
	}

	if (vpr->use_freezer) {
		err = freeze_processes();
		if (err < 0) {
			pr_err("%s(): failed to freeze processes: %d\n",
			       __func__, err);
			return err;
		}
	}

	list_for_each_entry(node, &vpr->devices, node) {
		err = pm_generic_freeze(node->dev);
		if (err < 0) {
			pr_err("failed to runtime suspend %s: %d\n",
			       dev_name(node->dev), err);
			goto thaw;
		}
	}

	trace_tegra_vpr_set(base, size);

	err = tegra_vpr_set(base, size);
	if (err < 0) {
		pr_err("failed to secure VPR: %d\n", err);
		status = err;
	}

thaw:
	list_for_each_entry_continue_reverse(node, &vpr->devices, node) {
		err = pm_generic_thaw(node->dev);
		if (err < 0) {
			pr_err("failed to runtime resume %s\n",
			       dev_name(node->dev));
			continue;
		}
	}

	if (vpr->use_freezer)
		thaw_processes();

	return status;
}

static int tegra_vpr_protect_pages(pte_t *ptep, unsigned long addr,
				   void *unused)
{
	pte_t pte = __ptep_get(ptep);

	pte = clear_pte_bit(pte, __pgprot(PROT_NORMAL));
	pte = set_pte_bit(pte, __pgprot(PROT_DEVICE_nGnRnE));

	__set_pte(ptep, pte);

	return 0;
}

static int tegra_vpr_unprotect_pages(pte_t *ptep, unsigned long addr,
				     void *unused)
{
	pte_t pte = __ptep_get(ptep);

	pte = clear_pte_bit(pte, __pgprot(PROT_DEVICE_nGnRnE));
	pte = set_pte_bit(pte, __pgprot(PROT_NORMAL));

	__set_pte(ptep, pte);

	return 0;
}

static int __init tegra_vpr_chunk_init(struct tegra_vpr *vpr,
				       struct tegra_vpr_chunk *chunk,
				       phys_addr_t start, size_t size,
				       unsigned int order, const char *name)
{
	chunk->start = start;
	chunk->limit = start + size;
	chunk->size = size;
	chunk->vpr = vpr;

	chunk->cma = cma_create(start, size, order, name);
	if (IS_ERR(chunk->cma)) {
		pr_err("cma_create() failed: %ld\n", PTR_ERR(chunk->cma));
		return PTR_ERR(chunk->cma);
	}

	chunk->offset = (start - vpr->base) >> PAGE_SHIFT;
	chunk->num_pages = size >> PAGE_SHIFT;
	chunk->num_buffers = 0;

	/* CMA area is not reserved yet */
	chunk->start_page = NULL;
	chunk->virt = 0;

	return 0;
}

static void tegra_vpr_chunk_free(struct tegra_vpr_chunk *chunk)
{
	cma_free(chunk->cma);
}

static inline bool tegra_vpr_chunk_is_last(const struct tegra_vpr_chunk *chunk)
{
	phys_addr_t limit = chunk->vpr->base + chunk->vpr->size;

	return chunk->limit == limit;
}

static inline bool tegra_vpr_chunk_is_leaf(const struct tegra_vpr_chunk *chunk)
{
	const struct tegra_vpr_chunk *next = chunk + 1;

	if (tegra_vpr_chunk_is_last(chunk))
		return true;

	return !next->active;
}

static int tegra_vpr_chunk_activate(struct tegra_vpr_chunk *chunk)
{
	unsigned long align = get_order(chunk->vpr->align);
	int err;

	trace_tegra_vpr_chunk_activate(chunk->start, chunk->limit);

	chunk->start_page = cma_alloc(chunk->cma, chunk->num_pages, align,
				      false);
	if (!chunk->start_page) {
		err = -ENOMEM;
		goto free;
	}

	chunk->virt = (unsigned long)page_to_virt(chunk->start_page);

	apply_to_existing_page_range(&init_mm, chunk->virt, chunk->size,
				     tegra_vpr_protect_pages, NULL);
	flush_tlb_kernel_range(chunk->virt, chunk->virt + chunk->size);

	chunk->active = true;

	return 0;

free:
	cma_release(chunk->cma, chunk->start_page, chunk->num_pages);
	chunk->start_page = NULL;
	chunk->virt = 0;
	return err;
}

static int tegra_vpr_chunk_deactivate(struct tegra_vpr_chunk *chunk)
{
	if (!chunk->active)
		return 0;

	/* do not deactivate if there are buffers left in this chunk */
	if (WARN_ON(chunk->num_buffers > 0))
		return -EBUSY;

	trace_tegra_vpr_chunk_deactivate(chunk->start, chunk->limit);

	chunk->active = false;

	apply_to_existing_page_range(&init_mm, chunk->virt, chunk->size,
				     tegra_vpr_unprotect_pages, NULL);
	flush_tlb_kernel_range(chunk->virt, chunk->virt + chunk->size);

	cma_release(chunk->cma, chunk->start_page, chunk->num_pages);
	chunk->start_page = NULL;
	chunk->virt = 0;

	return 0;
}

static bool tegra_vpr_chunk_overlaps(struct tegra_vpr_chunk *chunk,
				     unsigned int start, unsigned int limit)
{
	unsigned int first = chunk->offset;
	unsigned int last = chunk->offset + chunk->num_pages - 1;

	if (last < start || first >= limit)
		return false;

	return true;
}

static int tegra_vpr_activate_chunks(struct tegra_vpr *vpr,
				     struct tegra_vpr_buffer *buffer)
{
	DECLARE_BITMAP(dirty, vpr->num_chunks);
	unsigned int i, bottom, top;
	int err = 0, ret;

	bitmap_zero(dirty, vpr->num_chunks);

	/* activate any inactive chunks that overlap this buffer */
	for_each_set_bit(i, buffer->chunks, vpr->num_chunks) {
		struct tegra_vpr_chunk *chunk = &vpr->chunks[i];

		if (chunk->active)
			continue;

		err = tegra_vpr_chunk_activate(chunk);
		if (err < 0)
			goto deactivate;

		set_bit(i, vpr->active);
		set_bit(i, dirty);
	}

	/*
	 * Activating chunks above may have created holes, but since the VPR
	 * can only ever be a single contiguous region, make sure to activate
	 * any missing chunks.
	 */
	for_each_clear_bitrange(bottom, top, vpr->active, vpr->num_chunks) {
		/* inactive chunks at the bottom or the top are harmless */
		if (bottom == 0 || top == vpr->num_chunks)
			continue;

		for (i = bottom; i < top; i++) {
			struct tegra_vpr_chunk *chunk = &vpr->chunks[i];

			err = tegra_vpr_chunk_activate(chunk);
			if (err < 0)
				goto deactivate;

			set_bit(i, vpr->active);
			set_bit(i, dirty);
		}
	}

	/* if any chunks have been activated, VPR needs to be resized */
	if (!bitmap_empty(dirty, vpr->num_chunks)) {
		err = tegra_vpr_resize(vpr);
		if (err < 0) {
			pr_err("failed to grow VPR: %d\n", err);
			goto deactivate;
		}
	}

	/* increment buffer count for each chunk */
	for_each_set_bit(i, buffer->chunks, vpr->num_chunks)
		vpr->chunks[i].num_buffers++;

	return 0;

deactivate:
	/* deactivate any of the previously inactive chunks on failure */
	for_each_set_bit(i, dirty, vpr->num_chunks) {
		ret = tegra_vpr_chunk_deactivate(&vpr->chunks[i]);
		if (ret < 0)
			WARN(1, "failed to deactivate chunk #%u: %d\n", i, ret);

		clear_bit(i, vpr->active);
	}

	return err;
}

/*
 * Retrieve the range of pages within the activate region of the VPR.
 */
static bool tegra_vpr_get_active_range(struct tegra_vpr *vpr,
				       unsigned int *first,
				       unsigned int *last)
{
	unsigned long i, j;

	i = find_first_bit(vpr->active, vpr->num_chunks);
	if (i >= vpr->num_chunks)
		return false;

	j = find_last_bit(vpr->active, vpr->num_chunks);
	if (j >= vpr->num_chunks)
		return false;

	*first = vpr->chunks[i].offset;
	*last = vpr->chunks[j].offset + vpr->chunks[j].num_pages;

	return true;
}

/*
 * Try to find and allocate a free region within a specific page range.
 * Returns the page number if successful, -ENOSPC otherwise.
 *
 * This function mimics bitmap_find_free_region() but restricts the search
 * to a specific range to enable allocation within individual chunks.
 */
static int tegra_vpr_find_free_region_in_range(struct tegra_vpr *vpr,
					       unsigned int start_page,
					       unsigned int end_page,
					       unsigned int num_pages,
					       unsigned int align)
{
	unsigned int pos, next = ALIGN(start_page, align);

	/* Scan through aligned positions, trying to allocate at each one */
	for (pos = next; pos + num_pages <= end_page; pos = next) {
		next = find_next_bit(vpr->bitmap, pos + num_pages, pos);

		if (next >= pos + num_pages) {
			bitmap_set(vpr->bitmap, pos, num_pages);
			return pos;
		}

		next = find_next_zero_bit(vpr->bitmap, vpr->num_pages, next);
		next = ALIGN(next, align);
	}

	return -ENOSPC;
}

static int tegra_vpr_find_free_region(struct tegra_vpr *vpr,
				      unsigned int num_pages,
				      unsigned long align)
{
	return tegra_vpr_find_free_region_in_range(vpr, 0, vpr->num_pages - 1,
						   num_pages, align);
}

static int tegra_vpr_find_free_region_clustered(struct tegra_vpr *vpr,
						unsigned int num_pages,
						unsigned int align)
{
	unsigned int target, first, last;
	int pageno;

	/*
	 * If there are no allocations, abort the clustered allocation scheme
	 * and use the generic allocation scheme instead.
	 */
	if (vpr->first > vpr->last)
		return -ENOSPC;

	/*
	 * First, try to allocate within the currently allocated region. This
	 * keeps allocations tightly packed and minimizes the VPR size needed.
	 */
	pageno = tegra_vpr_find_free_region_in_range(vpr, vpr->first,
						     vpr->last + 1, num_pages,
						     align);
	if (pageno >= 0)
		return pageno;

	/*
	 * If not enough free space exists within the currently allocated
	 * region, check to see if the allocation fits anywhere within the
	 * active region, avoiding the need to resize the VPR.
	 */
	if (tegra_vpr_get_active_range(vpr, &first, &last)) {
		pageno = tegra_vpr_find_free_region_in_range(vpr, first, last,
							     num_pages, align);
		if (pageno >= 0)
			return pageno;
	}

	/*
	 * If not enough free space exists within the currently active region,
	 * try to allocate adjacent to it to grow it contiguously and ensure
	 * optimal packing.
	 */

	/*
	 * Calculate where the allocation should start to end right at the
	 * first allocated page, with proper alignment.
	 */
	if (vpr->first >= num_pages) {
		target = ALIGN_DOWN(vpr->first - num_pages, align);

		if (!bitmap_allocate(vpr->bitmap, target, num_pages))
			return target;
	}

	/* Try after the last allocation */
	target = ALIGN(vpr->last + 1, align);

	if (target + num_pages <= vpr->num_pages &&
	    !bitmap_allocate(vpr->bitmap, target, num_pages))
		return target;

	/*
	 * Couldn't allocate at the ideal adjacent position, search for any
	 * available space before the first allocated page.
	 */
	pageno = tegra_vpr_find_free_region_in_range(vpr, 0, vpr->first,
						     num_pages, align);
	if (pageno >= 0)
		return pageno;

	/*
	 * Couldn't allocate at the ideal adjacent position, search
	 * for any available space after the last allocated page.
	 */
	pageno = tegra_vpr_find_free_region_in_range(vpr, vpr->last + 1,
						     vpr->num_pages, num_pages,
						     align);
	if (pageno >= 0)
		return pageno;

	return -ENOSPC;
}

/*
 * Find a free region, preferring locations near existing allocations to
 * minimize VPR fragmentation. The allocation strategy is to first allocate
 * within or adjacent to the existing region to keep allocations clustered.
 * Otherwise fall back to a generic allocation using the first available
 * space.
 *
 * This approach focuses on page-level allocation first, then the chunk
 * system determines which chunks need to be activated based on where the
 * pages ended up.
 */
static int tegra_vpr_allocate_region(struct tegra_vpr *vpr,
				     unsigned int num_pages,
				     unsigned int align)
{
	int pageno;

	/*
	 * For non-resizable VPR (no chunks), use simple first-fit allocation.
	 * Clustering optimization is only beneficial for resizable VPR where
	 * keeping allocations together minimizes the active VPR size.
	 */
	if (vpr->num_chunks == 0)
		return tegra_vpr_find_free_region(vpr, num_pages, align);

	/*
	 * Check if there are any existing allocations in the bitmap. If so,
	 * try to allocate near them to minimize fragmentation.
	 */
	pageno = tegra_vpr_find_free_region_clustered(vpr, num_pages, align);
	if (pageno >= 0)
		return pageno;

	/*
	 * If there are no existing allocations, or no space adjacent to them,
	 * fall back to the first available space anywhere in the VPR.
	 */
	pageno = tegra_vpr_find_free_region(vpr, num_pages, align);
	if (pageno >= 0)
		return pageno;

	return -ENOSPC;
}

static struct tegra_vpr_buffer *
tegra_vpr_buffer_allocate(struct tegra_vpr *vpr, size_t size)
{
	unsigned int num_pages = size >> PAGE_SHIFT;
	unsigned int order = get_order(size);
	struct tegra_vpr_buffer *buffer;
	unsigned long first, last;
	int pageno, err;
	pgoff_t i;

	/*
	 * "order" defines the alignment and size, so this may result in
	 * fragmented memory depending on the allocation patterns. However,
	 * since this is used primarily for video frames, it is expected that
	 * a number of buffers of the same size will be allocated, so
	 * fragmentation should be negligible.
	 */
	pageno = tegra_vpr_allocate_region(vpr, num_pages, 1);
	if (pageno < 0)
		return ERR_PTR(pageno);

	first = find_first_bit(vpr->bitmap, vpr->num_pages);
	last = find_last_bit(vpr->bitmap, vpr->num_pages);

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		err = -ENOMEM;
		goto release;
	}

	INIT_LIST_HEAD(&buffer->attachments);
	INIT_LIST_HEAD(&buffer->list);
	mutex_init(&buffer->lock);
	buffer->start = vpr->base + (pageno << PAGE_SHIFT);
	buffer->limit = buffer->start + size;
	buffer->size = size;
	buffer->num_pages = num_pages;
	buffer->pageno = pageno;
	buffer->order = order;

	buffer->pages = kmalloc_array(buffer->num_pages,
				      sizeof(*buffer->pages),
				      GFP_KERNEL);
	if (!buffer->pages) {
		err = -ENOMEM;
		goto free;
	}

	/* track which chunks this buffer overlaps */
	if (vpr->num_chunks > 0) {
		unsigned int limit = buffer->pageno + buffer->num_pages, i;

		for (i = 0; i < vpr->num_chunks; i++) {
			struct tegra_vpr_chunk *chunk = &vpr->chunks[i];

			if (tegra_vpr_chunk_overlaps(chunk, pageno, limit))
				set_bit(i, buffer->chunks);
		}

		/* activate chunks if necessary */
		err = tegra_vpr_activate_chunks(vpr, buffer);
		if (err < 0)
			goto free;

		/* track first and last allocated pages */
		if (buffer->pageno < vpr->first)
			vpr->first = buffer->pageno;

		if (limit - 1 > vpr->last)
			vpr->last = limit - 1;
	}

	for (i = 0; i < buffer->num_pages; i++)
		buffer->pages[i] = &vpr->start_page[pageno + i];

	return buffer;

free:
	kfree(buffer->pages);
	kfree(buffer);
release:
	bitmap_release_region(vpr->bitmap, pageno, order);
	return ERR_PTR(err);
}

static void tegra_vpr_buffer_release(struct tegra_vpr_buffer *buffer)
{
	struct tegra_vpr *vpr = buffer->vpr;
	struct tegra_vpr_buffer *entry;
	unsigned long first, last;
	unsigned int i;

	/*
	 * Decrement buffer count for each overlapping chunk. Note that chunks
	 * are not deactivated here yet, that's done in tegra_vpr_recycle()
	 * instead.
	 */
	for_each_set_bit(i, buffer->chunks, vpr->num_chunks) {
		if (!WARN_ON(vpr->chunks[i].num_buffers == 0))
			vpr->chunks[i].num_buffers--;
	}

	/* track first and last allocated pages */
	if (list_is_first(&buffer->list, &vpr->buffers) &&
	    list_is_last(&buffer->list, &vpr->buffers)) {
		/* if there are no remaining buffers after this, reset */
		vpr->first = ~0U;
		vpr->last = 0U;
	} else if (list_is_first(&buffer->list, &vpr->buffers)) {
		entry = list_next_entry(buffer, list);
		vpr->first = entry->pageno;
	} else if (list_is_last(&buffer->list, &vpr->buffers)) {
		entry = list_prev_entry(buffer, list);
		vpr->last = entry->pageno + entry->num_pages - 1;
	}

	bitmap_release_region(vpr->bitmap, buffer->pageno, buffer->order);
	list_del(&buffer->list);
	kfree(buffer->pages);
	kfree(buffer);

	first = find_first_bit(vpr->bitmap, vpr->num_pages);
	last = find_last_bit(vpr->bitmap, vpr->num_pages);
}

static int tegra_vpr_attach(struct dma_buf *buf,
			    struct dma_buf_attachment *attachment)
{
	struct tegra_vpr_buffer *buffer = buf->priv;
	struct tegra_vpr_attachment *attach;
	int err;

	attach = kzalloc(sizeof(*attach), GFP_KERNEL);
	if (!attach)
		return -ENOMEM;

	err = sg_alloc_table_from_pages(&attach->sgt, buffer->pages,
					buffer->num_pages, 0, buffer->size,
					GFP_KERNEL);
	if (err < 0)
		goto free;

	attach->dev = attach->dev;
	INIT_LIST_HEAD(&attach->list);
	attachment->priv = attach;

	mutex_lock(&buffer->lock);
	list_add(&attach->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;

free:
	kfree(attach);
	return err;
}

static void tegra_vpr_detach(struct dma_buf *buf,
			     struct dma_buf_attachment *attachment)
{
	struct tegra_vpr_buffer *buffer = buf->priv;
	struct tegra_vpr_attachment *attach = attachment->priv;

	mutex_lock(&buffer->lock);
	list_del(&attach->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(&attach->sgt);
	kfree(attach);
}

static struct sg_table *
tegra_vpr_map_dma_buf(struct dma_buf_attachment *attachment,
		      enum dma_data_direction direction)
{
	struct tegra_vpr_attachment *attach = attachment->priv;
	struct sg_table *sgt = &attach->sgt;
	int err;

	err = dma_map_sgtable(attachment->dev, sgt, direction,
			      DMA_ATTR_SKIP_CPU_SYNC);
	if (err < 0)
		return ERR_PTR(err);

	return sgt;
}

static void tegra_vpr_unmap_dma_buf(struct dma_buf_attachment *attachment,
				    struct sg_table *sgt,
				    enum dma_data_direction direction)
{
	dma_unmap_sgtable(attachment->dev, sgt, direction,
			  DMA_ATTR_SKIP_CPU_SYNC);
}

static void tegra_vpr_recycle(struct tegra_vpr *vpr)
{
	DECLARE_BITMAP(dirty, vpr->num_chunks);
	unsigned int i;
	int err;

	bitmap_zero(dirty, vpr->num_chunks);

	/*
	 * Deactivate any unused chunks from the bottom...
	 */
	for (i = 0; i < vpr->num_chunks; i++) {
		struct tegra_vpr_chunk *chunk = &vpr->chunks[i];

		if (!chunk->active)
			continue;

		if (chunk->num_buffers > 0)
			break;

		err = tegra_vpr_chunk_deactivate(chunk);
		if (err < 0)
			pr_err("failed to deactivate chunk #%u\n", i);
		else {
			clear_bit(i, vpr->active);
			set_bit(i, dirty);
		}
	}

	/*
	 * ... and the top.
	 */
	for (i = 0; i < vpr->num_chunks; i++) {
		unsigned int index = vpr->num_chunks - i - 1;
		struct tegra_vpr_chunk *chunk = &vpr->chunks[index];

		if (!chunk->active)
			continue;

		if (chunk->num_buffers > 0)
			break;

		err = tegra_vpr_chunk_deactivate(chunk);
		if (err < 0)
			pr_err("failed to deactivate chunk #%u\n", index);
		else {
			clear_bit(i, vpr->active);
			set_bit(i, dirty);
		}
	}

	if (!bitmap_empty(dirty, vpr->num_chunks)) {
		err = tegra_vpr_resize(vpr);
		if (err < 0) {
			pr_err("failed to shrink VPR: %d\n", err);
			goto activate;
		}
	}

	return;

activate:
	for_each_set_bit(i, dirty, vpr->num_chunks) {
		err = tegra_vpr_chunk_activate(&vpr->chunks[i]);
		if (WARN_ON(err < 0))
			pr_err("failed to activate chunk #%u: %d\n", i, err);
	}
}

static void tegra_vpr_release(struct dma_buf *buf)
{
	struct tegra_vpr_buffer *buffer = buf->priv;
	struct tegra_vpr *vpr = buffer->vpr;

	mutex_lock(&vpr->lock);

	tegra_vpr_buffer_release(buffer);

	if (vpr->num_chunks > 0)
		tegra_vpr_recycle(vpr);

	mutex_unlock(&vpr->lock);
}

/*
 * Prohibit userspace mapping because the CPU cannot access this memory
 * anyway.
 */
static int tegra_vpr_begin_cpu_access(struct dma_buf *buf,
				      enum dma_data_direction direction)
{
	return -EPERM;
}

static int tegra_vpr_end_cpu_access(struct dma_buf *buf,
				    enum dma_data_direction direction)
{
	return -EPERM;
}

static int tegra_vpr_mmap(struct dma_buf *buf, struct vm_area_struct *vma)
{
	return -EPERM;
}

static const struct dma_buf_ops tegra_vpr_buf_ops = {
	.attach = tegra_vpr_attach,
	.detach = tegra_vpr_detach,
	.map_dma_buf = tegra_vpr_map_dma_buf,
	.unmap_dma_buf = tegra_vpr_unmap_dma_buf,
	.release = tegra_vpr_release,
	.begin_cpu_access = tegra_vpr_begin_cpu_access,
	.end_cpu_access = tegra_vpr_end_cpu_access,
	.mmap = tegra_vpr_mmap,
};

static struct dma_buf *tegra_vpr_allocate(struct dma_heap *heap,
					  unsigned long len, u32 fd_flags,
					  u64 heap_flags)
{
	struct tegra_vpr *vpr = dma_heap_get_drvdata(heap);
	struct tegra_vpr_buffer *buffer, *entry;
	size_t size = ALIGN(len, vpr->align);
	DEFINE_DMA_BUF_EXPORT_INFO(export);
	struct dma_buf *buf;

	mutex_lock(&vpr->lock);

	buffer = tegra_vpr_buffer_allocate(vpr, size);
	if (IS_ERR(buffer)) {
		mutex_unlock(&vpr->lock);
		return ERR_CAST(buffer);
	}

	/* insert in the correct order */
	if (!list_empty(&vpr->buffers)) {
		list_for_each_entry(entry, &vpr->buffers, list) {
			if (buffer->pageno < entry->pageno) {
				list_add_tail(&buffer->list, &entry->list);
				break;
			}
		}
	}

	if (list_empty(&buffer->list))
		list_add_tail(&buffer->list, &vpr->buffers);

	buffer->vpr = vpr;

	/*
	 * If a valid buffer was allocated, wrap it in a dma_buf
	 * and return it.
	 */
	export.exp_name = dma_heap_get_name(heap);
	export.ops = &tegra_vpr_buf_ops;
	export.size = buffer->size;
	export.flags = fd_flags;
	export.priv = buffer;

	buf = dma_buf_export(&export);
	if (IS_ERR(buf))
		tegra_vpr_buffer_release(buffer);

	mutex_unlock(&vpr->lock);
	return buf;
}

static void tegra_vpr_debugfs_show_buffers(struct tegra_vpr *vpr,
					   struct seq_file *s)
{
	struct tegra_vpr_buffer *buffer;
	char buf[16];

	list_for_each_entry(buffer, &vpr->buffers, list) {
		string_get_size(buffer->size, 1, STRING_UNITS_2, buf,
				sizeof(buf));
		seq_printf(s, "  %pap-%pap (%s)\n", &buffer->start,
			   &buffer->limit, buf);

	}
}

static void tegra_vpr_debugfs_show_chunks(struct tegra_vpr *vpr,
					  struct seq_file *s)
{
	struct tegra_vpr_buffer *buffer;
	unsigned int i;
	char buf[16];

	for (i = 0; i < vpr->num_chunks; i++) {
		const struct tegra_vpr_chunk *chunk = &vpr->chunks[i];

		string_get_size(chunk->size, 1, STRING_UNITS_2, buf,
				sizeof(buf));
		seq_printf(s, "  %pap-%pap (%s) (%s, %u buffers)\n",
			   &chunk->start, &chunk->limit, buf,
			   chunk->active ? "active" : "inactive",
			   chunk->num_buffers);
	}

	list_for_each_entry(buffer, &vpr->buffers, list) {
		string_get_size(buffer->size, 1, STRING_UNITS_2, buf,
				sizeof(buf));
		seq_printf(s, "%pap-%pap (%s, chunks: %*pbl)\n",
			   &buffer->start, &buffer->limit, buf,
			   vpr->num_chunks, buffer->chunks);
	}
}

static int tegra_vpr_debugfs_show(struct seq_file *s, struct dma_heap *heap)
{
	struct tegra_vpr *vpr = dma_heap_get_drvdata(heap);
	phys_addr_t limit = vpr->base + vpr->size;
	char buf[16];

	string_get_size(vpr->size, 1, STRING_UNITS_2, buf, sizeof(buf));
	seq_printf(s, "%pap-%pap (%s)\n", &vpr->base, &limit, buf);

	if (vpr->num_chunks == 0)
		tegra_vpr_debugfs_show_buffers(vpr, s);
	else
		tegra_vpr_debugfs_show_chunks(vpr, s);

	return 0;
}

static const struct dma_heap_ops tegra_vpr_heap_ops = {
	.allocate = tegra_vpr_allocate,
	.show = tegra_vpr_debugfs_show,
};

static int tegra_vpr_setup_chunks(struct tegra_vpr *vpr, const char *name)
{
	phys_addr_t start, limit;
	unsigned int order, i;
	size_t max_size;
	int err;

	/* This seems a reasonable value, so hard-code this for now. */
	vpr->num_chunks = 4;

	vpr->chunks = kcalloc(vpr->num_chunks, sizeof(*vpr->chunks),
			      GFP_KERNEL);
	if (!vpr->chunks)
		return -ENOMEM;

	max_size = PAGE_SIZE << (get_order(vpr->size) - ilog2(vpr->num_chunks));
	order = get_order(vpr->align);

	/*
	 * Allocate CMA areas for VPR. All areas will be roughtly the same
	 * size, with the last area taking up the rest.
	 */
	start = vpr->base;
	limit = vpr->base + vpr->size;

	pr_debug("VPR: %pap-%pap (%lu pages, %u chunks, %lu MiB)\n", &start,
		 &limit, vpr->num_pages, vpr->num_chunks,
		 (unsigned long)vpr->size / 1024 / 1024);

	for (i = 0; i < vpr->num_chunks; i++) {
		size_t size = limit - start;
		phys_addr_t end;

		size = min_t(size_t, size, max_size);
		end = start + size - 1;

		err = tegra_vpr_chunk_init(vpr, &vpr->chunks[i], start, size,
					   order, name);
		if (err < 0) {
			pr_err("failed to create VPR chunk: %d\n", err);
			goto free;
		}

		pr_debug("  %2u: %pap-%pap (%lu MiB)\n", i, &start, &end,
			 size / 1024 / 1024);
		start += size;
	}

	vpr->first = ~0U;
	vpr->last = 0U;

	return 0;

free:
	while (i--)
		tegra_vpr_chunk_free(&vpr->chunks[i]);

	kfree(vpr->chunks);
	return err;
}

static void tegra_vpr_free_chunks(struct tegra_vpr *vpr)
{
	unsigned int i;

	for (i = 0; i < vpr->num_chunks; i++)
		tegra_vpr_chunk_free(&vpr->chunks[i]);

	kfree(vpr->chunks);
}

static int tegra_vpr_setup_static(struct tegra_vpr *vpr)
{
	phys_addr_t start, limit;

	start = vpr->base;
	limit = vpr->base + vpr->size;

	pr_debug("VPR: %pap-%pap (%lu pages, %lu MiB)\n", &start, &limit,
		 vpr->num_pages, (unsigned long)vpr->size / 1024 / 1024);

	return 0;
}

static int __init tegra_vpr_add_heap(struct reserved_mem *rmem,
				     struct device_node *np)
{
	struct dma_heap_export_info info = {};
	unsigned long first, last;
	struct dma_heap *heap;
	struct tegra_vpr *vpr;
	int err;

	vpr = kzalloc(sizeof(*vpr), GFP_KERNEL);
	if (!vpr)
		return -ENOMEM;

	INIT_LIST_HEAD(&vpr->buffers);
	INIT_LIST_HEAD(&vpr->devices);
	vpr->resizable = !of_property_read_bool(np, "no-map");
	vpr->use_freezer = true;
	vpr->dev_node = np;
	vpr->align = PAGE_SIZE;
	vpr->base = rmem->base;
	vpr->size = rmem->size;

	/* common setup */
	vpr->start_page = phys_to_page(vpr->base);
	vpr->num_pages = vpr->size >> PAGE_SHIFT;

	vpr->bitmap = bitmap_zalloc(vpr->num_pages, GFP_KERNEL);
	if (!vpr->bitmap) {
		err = -ENOMEM;
		goto free;
	}

	first = find_first_bit(vpr->bitmap, vpr->num_pages);
	last = find_last_bit(vpr->bitmap, vpr->num_pages);

	if (vpr->resizable)
		err = tegra_vpr_setup_chunks(vpr, rmem->name);
	else
		err = tegra_vpr_setup_static(vpr);

	if (err < 0)
		goto free;

	info.name = vpr->dev_node->name;
	info.ops = &tegra_vpr_heap_ops;
	info.priv = vpr;

	heap = dma_heap_add(&info);
	if (IS_ERR(heap)) {
		err = PTR_ERR(heap);
		goto cleanup;
	}

	rmem->priv = heap;

	return 0;

cleanup:
	if (vpr->resizable)
		tegra_vpr_free_chunks(vpr);
free:
	bitmap_free(vpr->bitmap);
	kfree(vpr);
	return err;
}

static int __init tegra_vpr_init(void)
{
	const char *compatible = "nvidia,tegra-video-protection-region";
	struct device_node *parent;
	struct reserved_mem *rmem;
	int err;

	parent = of_find_node_by_path("/reserved-memory");
	if (!parent)
		return 0;

	for_each_child_of_node_scoped(parent, child) {
		if (!of_device_is_compatible(child, compatible))
			continue;

		rmem = of_reserved_mem_lookup(child);
		if (!rmem)
			continue;

		err = tegra_vpr_add_heap(rmem, child);
		if (err < 0)
			pr_err("failed to add VPR heap for %pOF: %d\n", child,
			       err);

		/* only a single VPR heap is supported */
		break;
	}

	return 0;
}
module_init(tegra_vpr_init);

static int tegra_vpr_device_init(struct reserved_mem *rmem, struct device *dev)
{
	struct dma_heap *heap = rmem->priv;
	struct tegra_vpr *vpr = dma_heap_get_drvdata(heap);
	struct tegra_vpr_device *node;

	if (!dev->driver->pm->freeze || !dev->driver->pm->thaw)
		return -EINVAL;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	INIT_LIST_HEAD(&node->node);
	node->dev = dev;

	list_add_tail(&node->node, &vpr->devices);

	return 0;
}

static void tegra_vpr_device_release(struct reserved_mem *rmem,
				     struct device *dev)
{
	struct dma_heap *heap = rmem->priv;
	struct tegra_vpr *vpr = dma_heap_get_drvdata(heap);
	struct tegra_vpr_device *node, *tmp;

	list_for_each_entry_safe(node, tmp, &vpr->devices, node) {
		if (node->dev == dev) {
			list_del(&node->node);
			kfree(node);
		}
	}
}

static const struct reserved_mem_ops tegra_vpr_ops = {
	.device_init = tegra_vpr_device_init,
	.device_release = tegra_vpr_device_release,
};

static int tegra_vpr_rmem_init(struct reserved_mem *rmem)
{
	rmem->ops = &tegra_vpr_ops;

	return 0;
}
RESERVEDMEM_OF_DECLARE(tegra_vpr, "nvidia,tegra-video-protection-region",
		       tegra_vpr_rmem_init);

MODULE_DESCRIPTION("NVIDIA Tegra Video-Protection-Region DMA-BUF heap driver");
MODULE_LICENSE("GPL");
