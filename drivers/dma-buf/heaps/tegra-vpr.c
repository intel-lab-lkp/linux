// SPDX-License-Identifier: GPL-2.0
/*
 * DMA-BUF restricted heap exporter for NVIDIA Video-Protection-Region (VPR)
 *
 * Copyright (C) 2024-2025 NVIDIA Corporation
 */

#define pr_fmt(fmt) "tegra-vpr: " fmt

#include <linux/arm-smccc.h>
#include <linux/cma.h>
#include <linux/debugfs.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/of_reserved_mem.h>

#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <linux/freezer.h>

#define CREATE_TRACE_POINTS
#include <trace/events/tegra_vpr.h>

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
	unsigned long *bitmap;
	unsigned long virt;
	pgoff_t num_pages;

	struct list_head buffers;
	struct mutex lock;
};

struct tegra_vpr {
	struct device_node *dev_node;
	unsigned long align;
	phys_addr_t base;
	phys_addr_t size;
	bool use_freezer;

	struct tegra_vpr_chunk *chunks;
	unsigned int num_chunks;

	struct list_head devices;
	struct mutex lock;
};

struct tegra_vpr_buffer {
	struct tegra_vpr_chunk *chunk;
	struct list_head attachments;
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

	unsigned long virt;
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

		if (!chunk->active)
			break;

		if (chunk->start < start)
			start = chunk->start;

		if (chunk->limit > limit)
			limit = chunk->limit;
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
	int err;

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
			pr_err("failed to runtime suspend %s\n",
			       dev_name(node->dev));
			continue;
		}
	}

	trace_tegra_vpr_set(base, size);

	err = tegra_vpr_set(base, size);
	if (err < 0) {
		pr_err("failed to secure VPR: %d\n", err);
		return err;
	}

	list_for_each_entry(node, &vpr->devices, node) {
		err = pm_generic_thaw(node->dev);
		if (err < 0) {
			pr_err("failed to runtime resume %s\n",
			       dev_name(node->dev));
			continue;
		}
	}

	if (vpr->use_freezer)
		thaw_processes();

	return 0;
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

static int tegra_vpr_chunk_init(struct tegra_vpr *vpr,
				struct tegra_vpr_chunk *chunk,
				phys_addr_t start, size_t size,
				unsigned int order, const char *name)
{
	INIT_LIST_HEAD(&chunk->buffers);
	chunk->start = start;
	chunk->limit = start + size;
	chunk->size = size;
	chunk->vpr = vpr;

	chunk->cma = cma_create(start, size, order, name);
	if (IS_ERR(chunk->cma))
		return PTR_ERR(chunk->cma);

	chunk->num_pages = size >> PAGE_SHIFT;

	chunk->bitmap = bitmap_zalloc(chunk->num_pages, GFP_KERNEL);
	if (!chunk->bitmap) {
		cma_free(chunk->cma);
		return -ENOMEM;
	}

	/* CMA area is not reserved yet */
	chunk->start_page = NULL;
	chunk->virt = 0;

	return 0;
}

static void tegra_vpr_chunk_free(struct tegra_vpr_chunk *chunk)
{
	kfree(chunk->bitmap);
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

	if (chunk->active)
		return 0;

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

	err = tegra_vpr_resize(chunk->vpr);
	if (err < 0)
		goto unprotect;

	bitmap_zero(chunk->bitmap, chunk->num_pages);

	return 0;

unprotect:
	chunk->active = false;
	apply_to_existing_page_range(&init_mm, chunk->virt, chunk->size,
				     tegra_vpr_unprotect_pages, NULL);
	flush_tlb_kernel_range(chunk->virt, chunk->virt + chunk->size);
free:
	cma_release(chunk->cma, chunk->start_page, chunk->num_pages);
	chunk->start_page = NULL;
	chunk->virt = 0;
	return err;
}

static int tegra_vpr_chunk_deactivate(struct tegra_vpr_chunk *chunk)
{
	int err;

	if (!chunk->active || !tegra_vpr_chunk_is_leaf(chunk))
		return 0;

	/* do not deactivate if there are buffers left in this chunk */
	if (WARN_ON(!list_empty(&chunk->buffers)))
		return 0;

	trace_tegra_vpr_chunk_deactivate(chunk->start, chunk->limit);

	chunk->active = false;

	err = tegra_vpr_resize(chunk->vpr);
	if (err < 0) {
		chunk->active = true;
		return err;
	}

	apply_to_existing_page_range(&init_mm, chunk->virt, chunk->size,
				     tegra_vpr_unprotect_pages, NULL);
	flush_tlb_kernel_range(chunk->virt, chunk->virt + chunk->size);

	cma_release(chunk->cma, chunk->start_page, chunk->num_pages);
	chunk->start_page = NULL;
	chunk->virt = 0;

	return 0;
}

static struct tegra_vpr_buffer *
tegra_vpr_chunk_allocate(struct tegra_vpr_chunk *chunk, size_t size)
{
	unsigned int order = get_order(size);
	struct tegra_vpr_buffer *buffer;
	int pageno, err;
	pgoff_t i;

	err = tegra_vpr_chunk_activate(chunk);
	if (err < 0)
		return ERR_PTR(err);

	/*
	 * "order" defines the alignment and size, so this may result in
	 * fragmented memory depending on the allocation patterns. However,
	 * since this is used primarily for video frames, it is expected that
	 * a number of buffers of the same size will be allocated, so
	 * fragmentation should be negligible.
	 */
	pageno = bitmap_find_free_region(chunk->bitmap, chunk->num_pages,
					 order);
	if (pageno < 0)
		return ERR_PTR(-ENOSPC);

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		err = -ENOMEM;
		goto release;
	}

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->lock);
	buffer->chunk = chunk;
	buffer->start = chunk->start + (pageno << PAGE_SHIFT);
	buffer->limit = buffer->start + size;
	buffer->size = size;
	buffer->num_pages = buffer->size >> PAGE_SHIFT;
	buffer->pageno = pageno;
	buffer->order = order;

	buffer->virt = (unsigned long)page_to_virt(chunk->start_page + pageno);

	buffer->pages = kmalloc_array(buffer->num_pages,
				      sizeof(*buffer->pages),
				      GFP_KERNEL);
	if (!buffer->pages) {
		err = -ENOMEM;
		goto free;
	}

	for (i = 0; i < buffer->num_pages; i++)
		buffer->pages[i] = &chunk->start_page[pageno + i];

	list_add_tail(&buffer->list, &chunk->buffers);

	return buffer;

free:
	kfree(buffer);
release:
	bitmap_release_region(chunk->bitmap, pageno, order);
	return ERR_PTR(err);
}

static void tegra_vpr_chunk_release(struct tegra_vpr_chunk *chunk,
				    struct tegra_vpr_buffer *buffer)
{
	list_del(&buffer->list);
	kfree(buffer->pages);
	kfree(buffer);

	bitmap_release_region(chunk->bitmap, buffer->pageno, buffer->order);
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
	unsigned int i;
	int err;

	/*
	 * Walk the list of chunks in reverse order and check if they can be
	 * deactivated.
	 */
	for (i = 0; i < vpr->num_chunks; i++) {
		unsigned int index = vpr->num_chunks - i - 1;
		struct tegra_vpr_chunk *chunk = &vpr->chunks[index];

		/*
		 * Stop at any chunk that has remaining buffers. We cannot
		 * deactivate any chunks at lower addresses because the
		 * protected region needs to remain contiguous. Technically we
		 * could shrink from top and bottom, but for the sake of
		 * simplicity we'll only shrink from the top for now.
		 */
		if (!list_empty(&chunk->buffers))
			break;

		err = tegra_vpr_chunk_deactivate(chunk);
		if (err < 0)
			pr_err("failed to deactivate chunk\n");
	}
}

static void tegra_vpr_release(struct dma_buf *buf)
{
	struct tegra_vpr_buffer *buffer = buf->priv;
	struct tegra_vpr_chunk *chunk = buffer->chunk;
	struct tegra_vpr *vpr = chunk->vpr;

	mutex_lock(&vpr->lock);

	tegra_vpr_chunk_release(chunk, buffer);
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
	DEFINE_DMA_BUF_EXPORT_INFO(export);
	struct tegra_vpr_buffer *buffer;
	struct dma_buf *buf;
	unsigned int i;

	mutex_lock(&vpr->lock);

	for (i = 0; i < vpr->num_chunks; i++) {
		struct tegra_vpr_chunk *chunk = &vpr->chunks[i];
		size_t size = ALIGN(len, vpr->align);

		buffer = tegra_vpr_chunk_allocate(chunk, size);
		if (IS_ERR(buffer)) {
			/* try the next chunk if the current one is exhausted */
			if (PTR_ERR(buffer) == -ENOSPC)
				continue;

			mutex_unlock(&vpr->lock);
			return ERR_CAST(buffer);
		}

		/*
		 * If a valid buffer was allocated, wrap it in a dma_buf and
		 * return it.
		 */
		if (buffer) {
			export.exp_name = dma_heap_get_name(heap);
			export.ops = &tegra_vpr_buf_ops;
			export.size = buffer->size;
			export.flags = fd_flags;
			export.priv = buffer;

			buf = dma_buf_export(&export);
			if (IS_ERR(buf)) {
				tegra_vpr_chunk_release(chunk, buffer);
				return ERR_CAST(buf);
			}

			mutex_unlock(&vpr->lock);
			return buf;
		}
	}

	mutex_unlock(&vpr->lock);

	/*
	 * If we get here, none of the chunks could allocate a buffer, so
	 * there's nothing else we can do.
	 */
	return ERR_PTR(-ENOMEM);
}

static int tegra_vpr_debugfs_show(struct seq_file *s, struct dma_heap *heap)
{
	struct tegra_vpr *vpr = dma_heap_get_drvdata(heap);
	phys_addr_t limit = vpr->base + vpr->size;
	unsigned int i;
	char buf[16];

	string_get_size(vpr->size, 1, STRING_UNITS_2, buf, sizeof(buf));
	seq_printf(s, "%pap-%pap (%s)\n", &vpr->base, &limit, buf);

	for (i = 0; i < vpr->num_chunks; i++) {
		const struct tegra_vpr_chunk *chunk = &vpr->chunks[i];
		struct tegra_vpr_buffer *buffer;

		string_get_size(chunk->size, 1, STRING_UNITS_2, buf,
				sizeof(buf));
		seq_printf(s, "  %pap-%pap (%s)\n", &chunk->start,
			   &chunk->limit, buf);

		list_for_each_entry(buffer, &chunk->buffers, list) {
			string_get_size(buffer->size, 1, STRING_UNITS_2, buf,
					sizeof(buf));
			seq_printf(s, "    %pap-%pap (%s)\n", &buffer->start,
				   &buffer->limit, buf);
		}
	}

	return 0;
}

static const struct dma_heap_ops tegra_vpr_heap_ops = {
	.allocate = tegra_vpr_allocate,
	.show = tegra_vpr_debugfs_show,
};

static int __init tegra_vpr_add_heap(struct reserved_mem *rmem,
				     struct device_node *np)
{
	struct dma_heap_export_info info = {};
	phys_addr_t start, limit;
	struct dma_heap *heap;
	struct tegra_vpr *vpr;
	unsigned int order, i;
	size_t max_size;
	int err;

	vpr = kzalloc(sizeof(*vpr), GFP_KERNEL);
	if (!vpr) {
		err = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&vpr->devices);
	vpr->use_freezer = true;
	vpr->dev_node = np;
	vpr->align = SZ_1M;
	vpr->base = rmem->base;
	vpr->size = rmem->size;
	vpr->num_chunks = 4;

	max_size = PAGE_SIZE << (get_order(vpr->size) - ilog2(vpr->num_chunks));
	order = get_order(vpr->align);

	vpr->chunks = kcalloc(vpr->num_chunks, sizeof(*vpr->chunks),
			      GFP_KERNEL);
	if (!vpr) {
		err = -ENOMEM;
		goto free;
	}

	/*
	 * Allocate CMA areas for VPR. All areas will be roughtly the same
	 * size, with the last area taking up the rest.
	 */
	start = vpr->base;
	limit = vpr->base + vpr->size;

	pr_debug("VPR: %pap-%pap (%u chunks, %lu MiB)\n", &start, &limit,
		 vpr->num_chunks, (unsigned long)vpr->size / 1024 / 1024);

	for (i = 0; i < vpr->num_chunks; i++) {
		size_t size = limit - start;
		phys_addr_t end;

		size = min_t(size_t, size, max_size);
		end = start + size - 1;

		err = tegra_vpr_chunk_init(vpr, &vpr->chunks[i], start, size,
					   order, rmem->name);
		if (err < 0) {
			pr_err("failed to create VPR chunk: %d\n", err);
			goto free;
		}

		pr_debug("  %2u: %pap-%pap (%lu MiB)\n", i, &start, &end,
			 size / 1024 / 1024);
		start += size;
	}

	info.name = vpr->dev_node->name;
	info.ops = &tegra_vpr_heap_ops;
	info.priv = vpr;

	heap = dma_heap_add(&info);
	if (IS_ERR(heap)) {
		err = PTR_ERR(heap);
		goto cma_free;
	}

	rmem->priv = heap;

	return 0;

cma_free:
	while (i--)
		tegra_vpr_chunk_free(&vpr->chunks[i]);
free:
	kfree(vpr->chunks);
	kfree(vpr);
out:
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
	int err = 0;

	if (!dev->driver->pm->freeze || !dev->driver->pm->thaw)
		return -EINVAL;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		err = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&node->node);
	node->dev = dev;

	list_add_tail(&node->node, &vpr->devices);

out:
	return err;
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
