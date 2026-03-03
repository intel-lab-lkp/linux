// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF heap for coherent reserved-memory regions
 *
 * Copyright (C) 2026 Red Hat, Inc.
 * Author: Albert Esteve <aesteve@redhat.com>
 *
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/iosys-map.h>
#include <linux/of_reserved_mem.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

struct coherent_heap {
	struct dma_heap *heap;
	struct reserved_mem *rmem;
	char *name;
};

struct coherent_heap_buffer {
	struct coherent_heap *heap;
	struct list_head attachments;
	struct mutex lock;
	unsigned long len;
	dma_addr_t dma_addr;
	void *alloc_vaddr;
	struct page **pages;
	pgoff_t pagecount;
	int vmap_cnt;
	void *vaddr;
};

struct dma_heap_attachment {
	struct device *dev;
	struct sg_table table;
	struct list_head list;
	bool mapped;
};

static int coherent_heap_attach(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)
{
	struct coherent_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	int ret;

	a = kzalloc_obj(*a);
	if (!a)
		return -ENOMEM;

	ret = sg_alloc_table_from_pages(&a->table, buffer->pages,
					buffer->pagecount, 0,
					buffer->pagecount << PAGE_SHIFT,
					GFP_KERNEL);
	if (ret) {
		kfree(a);
		return ret;
	}

	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->list);
	a->mapped = false;

	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void coherent_heap_detach(struct dma_buf *dmabuf,
				 struct dma_buf_attachment *attachment)
{
	struct coherent_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(&a->table);
	kfree(a);
}

static struct sg_table *coherent_heap_map_dma_buf(struct dma_buf_attachment *attachment,
						  enum dma_data_direction direction)
{
	struct dma_heap_attachment *a = attachment->priv;
	struct sg_table *table = &a->table;
	int ret;

	ret = dma_map_sgtable(attachment->dev, table, direction, 0);
	if (ret)
		return ERR_PTR(-ENOMEM);
	a->mapped = true;

	return table;
}

static void coherent_heap_unmap_dma_buf(struct dma_buf_attachment *attachment,
					struct sg_table *table,
					enum dma_data_direction direction)
{
	struct dma_heap_attachment *a = attachment->priv;

	a->mapped = false;
	dma_unmap_sgtable(attachment->dev, table, direction, 0);
}

static int coherent_heap_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
						  enum dma_data_direction direction)
{
	struct coherent_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt)
		invalidate_kernel_vmap_range(buffer->vaddr, buffer->len);

	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->mapped)
			continue;
		dma_sync_sgtable_for_cpu(a->dev, &a->table, direction);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int coherent_heap_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
						enum dma_data_direction direction)
{
	struct coherent_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt)
		flush_kernel_vmap_range(buffer->vaddr, buffer->len);

	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->mapped)
			continue;
		dma_sync_sgtable_for_device(a->dev, &a->table, direction);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int coherent_heap_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct coherent_heap_buffer *buffer = dmabuf->priv;
	struct coherent_heap *coh_heap = buffer->heap;
	struct device *heap_dev = dma_heap_get_dev(coh_heap->heap);

	return dma_mmap_coherent(heap_dev, vma, buffer->alloc_vaddr,
				 buffer->dma_addr, buffer->len);
}

static void *coherent_heap_do_vmap(struct coherent_heap_buffer *buffer)
{
	void *vaddr;

	vaddr = vmap(buffer->pages, buffer->pagecount, VM_MAP, PAGE_KERNEL);
	if (!vaddr)
		return ERR_PTR(-ENOMEM);

	return vaddr;
}

static int coherent_heap_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct coherent_heap_buffer *buffer = dmabuf->priv;
	void *vaddr;
	int ret = 0;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt) {
		buffer->vmap_cnt++;
		iosys_map_set_vaddr(map, buffer->vaddr);
		goto out;
	}

	vaddr = coherent_heap_do_vmap(buffer);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto out;
	}

	buffer->vaddr = vaddr;
	buffer->vmap_cnt++;
	iosys_map_set_vaddr(map, buffer->vaddr);
out:
	mutex_unlock(&buffer->lock);

	return ret;
}

static void coherent_heap_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct coherent_heap_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	if (!--buffer->vmap_cnt) {
		vunmap(buffer->vaddr);
		buffer->vaddr = NULL;
	}
	mutex_unlock(&buffer->lock);
	iosys_map_clear(map);
}

static void coherent_heap_dma_buf_release(struct dma_buf *dmabuf)
{
	struct coherent_heap_buffer *buffer = dmabuf->priv;
	struct coherent_heap *coh_heap = buffer->heap;
	struct device *heap_dev = dma_heap_get_dev(coh_heap->heap);

	if (buffer->vmap_cnt > 0) {
		WARN(1, "%s: buffer still mapped in the kernel\n", __func__);
		vunmap(buffer->vaddr);
		buffer->vaddr = NULL;
		buffer->vmap_cnt = 0;
	}

	if (buffer->alloc_vaddr)
		dma_free_coherent(heap_dev, buffer->len, buffer->alloc_vaddr,
				  buffer->dma_addr);
	kfree(buffer->pages);
	kfree(buffer);
}

static const struct dma_buf_ops coherent_heap_buf_ops = {
	.attach = coherent_heap_attach,
	.detach = coherent_heap_detach,
	.map_dma_buf = coherent_heap_map_dma_buf,
	.unmap_dma_buf = coherent_heap_unmap_dma_buf,
	.begin_cpu_access = coherent_heap_dma_buf_begin_cpu_access,
	.end_cpu_access = coherent_heap_dma_buf_end_cpu_access,
	.mmap = coherent_heap_mmap,
	.vmap = coherent_heap_vmap,
	.vunmap = coherent_heap_vunmap,
	.release = coherent_heap_dma_buf_release,
};

static struct dma_buf *coherent_heap_allocate(struct dma_heap *heap,
					      unsigned long len,
					      u32 fd_flags,
					      u64 heap_flags)
{
	struct coherent_heap *coh_heap;
	struct coherent_heap_buffer *buffer;
	struct device *heap_dev;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	size_t size = PAGE_ALIGN(len);
	pgoff_t pagecount = size >> PAGE_SHIFT;
	struct dma_buf *dmabuf;
	int ret = -ENOMEM;
	pgoff_t pg;

	coh_heap = dma_heap_get_drvdata(heap);
	if (!coh_heap)
		return ERR_PTR(-EINVAL);

	heap_dev = dma_heap_get_dev(coh_heap->heap);
	if (!heap_dev)
		return ERR_PTR(-ENODEV);

	buffer = kzalloc_obj(*buffer);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->lock);
	buffer->len = size;
	buffer->heap = coh_heap;
	buffer->pagecount = pagecount;

	buffer->alloc_vaddr = dma_alloc_coherent(heap_dev, buffer->len,
						 &buffer->dma_addr, GFP_KERNEL);
	if (!buffer->alloc_vaddr) {
		ret = -ENOMEM;
		goto free_buffer;
	}

	buffer->pages = kmalloc_array(pagecount, sizeof(*buffer->pages),
				      GFP_KERNEL);
	if (!buffer->pages) {
		ret = -ENOMEM;
		goto free_dma;
	}

	for (pg = 0; pg < pagecount; pg++)
		buffer->pages[pg] = virt_to_page((char *)buffer->alloc_vaddr +
						 (pg * PAGE_SIZE));

	/* create the dmabuf */
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.ops = &coherent_heap_buf_ops;
	exp_info.size = buffer->len;
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto free_pages;
	}
	return dmabuf;

free_pages:
	kfree(buffer->pages);
free_dma:
	dma_free_coherent(heap_dev, buffer->len, buffer->alloc_vaddr,
			  buffer->dma_addr);
free_buffer:
	kfree(buffer);
	return ERR_PTR(ret);
}

static const struct dma_heap_ops coherent_heap_ops = {
	.allocate = coherent_heap_allocate,
};

static int coherent_heap_init_dma_mask(struct device *dev)
{
	int ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (!ret)
		return 0;

	/* Fallback to 32-bit DMA mask */
	return dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
}

static int __coherent_heap_register(struct reserved_mem *rmem)
{
	struct dma_heap_export_info exp_info;
	struct coherent_heap *coh_heap;
	struct device *heap_dev;
	int ret;

	if (!rmem || !rmem->name)
		return -EINVAL;

	coh_heap = kzalloc_obj(*coh_heap);
	if (!coh_heap)
		return -ENOMEM;

	coh_heap->rmem = rmem;
	coh_heap->name = kstrdup(rmem->name, GFP_KERNEL);
	if (!coh_heap->name) {
		ret = -ENOMEM;
		goto free_coherent_heap;
	}

	exp_info.name = coh_heap->name;
	exp_info.ops = &coherent_heap_ops;
	exp_info.priv = coh_heap;

	coh_heap->heap = dma_heap_create(&exp_info);
	if (IS_ERR(coh_heap->heap)) {
		ret = PTR_ERR(coh_heap->heap);
		goto free_name;
	}

	heap_dev = dma_heap_get_dev(coh_heap->heap);
	ret = coherent_heap_init_dma_mask(heap_dev);
	if (ret) {
		pr_err("coherent_heap: failed to set DMA mask (%d)\n", ret);
		goto destroy_heap;
	}

	ret = of_reserved_mem_device_init_with_mem(heap_dev, rmem);
	if (ret) {
		pr_err("coherent_heap: failed to initialize memory (%d)\n", ret);
		goto destroy_heap;
	}

	ret = dma_heap_register(coh_heap->heap);
	if (ret) {
		pr_err("coherent_heap: failed to register heap (%d)\n", ret);
		goto destroy_heap;
	}

	return 0;

destroy_heap:
	dma_heap_destroy(coh_heap->heap);
	coh_heap->heap = NULL;
free_name:
	kfree(coh_heap->name);
free_coherent_heap:
	kfree(coh_heap);

	return ret;
}

static int __init coherent_heap_register(void)
{
	struct reserved_mem *rmem;
	unsigned int i;
	int ret;

	for (i = 0; (rmem = dma_coherent_get_reserved_region(i)) != NULL; i++) {
		ret = __coherent_heap_register(rmem);
		if (ret) {
			pr_warn("Failed to add coherent heap %s",
				rmem->name ? rmem->name : "unknown");
			continue;
		}
	}

	return 0;
}
module_init(coherent_heap_register);
MODULE_DESCRIPTION("DMA-BUF heap for coherent reserved-memory regions");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_IMPORT_NS("DMA_BUF_HEAP");
