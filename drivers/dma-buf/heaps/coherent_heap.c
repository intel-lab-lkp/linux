// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF heap for coherent reserved-memory regions
 *
 * Copyright (C) 2026 Red Hat, Inc.
 * Author: Albert Esteve <aesteve@redhat.com>
 *
 */

#include <linux/cgroup_dmem.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/iosys-map.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define DEFERRED_AREAS_MAX CONFIG_COHERENT_AREAS_DEFERRED

/*
 * Early init can't use normal memory management yet (memblock is used
 * instead), so keep a small deferred list and retry at late_initcall.
 */
static struct reserved_mem *rmem_areas_deferred[DEFERRED_AREAS_MAX];
static unsigned int rmem_areas_deferred_num;

static int coherent_heap_add_deferred(struct reserved_mem *rmem)
{
	if (rmem_areas_deferred_num >= DEFERRED_AREAS_MAX) {
		pr_warn("Deferred heap areas list full, dropping %s\n",
			rmem->name ? rmem->name : "unknown");
		return -EINVAL;
	}
	rmem_areas_deferred[rmem_areas_deferred_num++] = rmem;
	return 0;
}

struct coherent_heap {
	struct dma_heap *heap;
	struct reserved_mem *rmem;
	char *name;
	struct device *dev;
	struct platform_device *pdev;
#if IS_ENABLED(CONFIG_CGROUP_DMEM)
	struct dmem_cgroup_region *cg;
#endif
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
#if IS_ENABLED(CONFIG_CGROUP_DMEM)
	struct dmem_cgroup_pool_state *pool;
#endif
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

	return dma_mmap_coherent(coh_heap->dev, vma, buffer->alloc_vaddr,
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

	if (buffer->vmap_cnt > 0) {
		WARN(1, "%s: buffer still mapped in the kernel\n", __func__);
		vunmap(buffer->vaddr);
		buffer->vaddr = NULL;
		buffer->vmap_cnt = 0;
	}

	if (buffer->alloc_vaddr)
		dma_free_coherent(coh_heap->dev, buffer->len, buffer->alloc_vaddr,
			       buffer->dma_addr);
	kfree(buffer->pages);
#if IS_ENABLED(CONFIG_CGROUP_DMEM)
	dmem_cgroup_uncharge(buffer->pool, buffer->len);
#endif
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
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	size_t size = PAGE_ALIGN(len);
	pgoff_t pagecount = size >> PAGE_SHIFT;
	struct dma_buf *dmabuf;
	int ret = -ENOMEM;
	pgoff_t pg;

	coh_heap = dma_heap_get_drvdata(heap);
	if (!coh_heap)
		return ERR_PTR(-EINVAL);
	if (!coh_heap->dev)
		return ERR_PTR(-ENODEV);

	buffer = kzalloc_obj(*buffer);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->lock);
	buffer->len = size;
	buffer->heap = coh_heap;
	buffer->pagecount = pagecount;

#if IS_ENABLED(CONFIG_CGROUP_DMEM)
	if (mem_accounting) {
		ret = dmem_cgroup_try_charge(coh_heap->cg, size,
					     &buffer->pool, NULL);
		if (ret)
			goto free_buffer;
	}
#endif

	buffer->alloc_vaddr = dma_alloc_coherent(coh_heap->dev, buffer->len,
						 &buffer->dma_addr, GFP_KERNEL);
	if (!buffer->alloc_vaddr) {
		ret = -ENOMEM;
#if IS_ENABLED(CONFIG_CGROUP_DMEM)
		goto uncharge_cgroup;
#else
		goto free_buffer;
#endif
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
	dma_free_coherent(coh_heap->dev, buffer->len, buffer->alloc_vaddr,
			  buffer->dma_addr);
#if IS_ENABLED(CONFIG_CGROUP_DMEM)
uncharge_cgroup:
	dmem_cgroup_uncharge(buffer->pool, size);
#endif
free_buffer:
	kfree(buffer);
	return ERR_PTR(ret);
}

static const struct dma_heap_ops coherent_heap_ops = {
	.allocate = coherent_heap_allocate,
};

static int __coherent_heap_register(struct reserved_mem *rmem)
{
	struct dma_heap_export_info exp_info;
	struct coherent_heap *coh_heap;
#if IS_ENABLED(CONFIG_CGROUP_DMEM)
	struct dmem_cgroup_region *region;
#endif
	const char *rmem_name;
	int ret;

	if (!rmem)
		return -EINVAL;

	rmem_name = rmem->name ? rmem->name : "unknown";

	coh_heap = kzalloc_obj(*coh_heap);
	if (!coh_heap)
		return -ENOMEM;

	coh_heap->name = kasprintf(GFP_KERNEL, "coherent_%s", rmem_name);
	if (!coh_heap->name) {
		ret = -ENOMEM;
		goto free_coherent_heap;
	}

	coh_heap->rmem = rmem;

	/* create a platform device per rmem and bind it */
	coh_heap->pdev = platform_device_register_simple("coherent-heap",
							 PLATFORM_DEVID_AUTO,
							 NULL, 0);
	if (IS_ERR(coh_heap->pdev)) {
		ret = PTR_ERR(coh_heap->pdev);
		goto free_name;
	}

	if (rmem->ops && rmem->ops->device_init) {
		ret = rmem->ops->device_init(rmem, &coh_heap->pdev->dev);
		if (ret)
			goto pdev_unregister;
	}

	coh_heap->dev = &coh_heap->pdev->dev;
#if IS_ENABLED(CONFIG_CGROUP_DMEM)
	region = dmem_cgroup_register_region(rmem->size, "coh/%s", rmem_name);
	if (IS_ERR(region)) {
		ret = PTR_ERR(region);
		goto pdev_unregister;
	}
	coh_heap->cg = region;
#endif

	exp_info.name = coh_heap->name;
	exp_info.ops = &coherent_heap_ops;
	exp_info.priv = coh_heap;

	coh_heap->heap = dma_heap_add(&exp_info);
	if (IS_ERR(coh_heap->heap)) {
		ret = PTR_ERR(coh_heap->heap);
		goto cg_unregister;
	}

	return 0;

cg_unregister:
#if IS_ENABLED(CONFIG_CGROUP_DMEM)
	dmem_cgroup_unregister_region(coh_heap->cg);
#endif
pdev_unregister:
	platform_device_unregister(coh_heap->pdev);
	coh_heap->pdev = NULL;
free_name:
	kfree(coh_heap->name);
free_coherent_heap:
	kfree(coh_heap);

	return ret;
}

int dma_heap_coherent_register(struct reserved_mem *rmem)
{
	int ret;

	ret = __coherent_heap_register(rmem);
	if (ret == -ENOMEM)
		return coherent_heap_add_deferred(rmem);
	return ret;
}

static int __init coherent_heap_register_deferred(void)
{
	unsigned int i;
	int ret;

	for (i = 0; i < rmem_areas_deferred_num; i++) {
		struct reserved_mem *rmem = rmem_areas_deferred[i];

		ret = __coherent_heap_register(rmem);
		if (ret) {
			pr_warn("Failed to add coherent heap %s",
				rmem->name ? rmem->name : "unknown");
			continue;
		}
	}

	return 0;
}
late_initcall(coherent_heap_register_deferred);
MODULE_DESCRIPTION("DMA-BUF heap for coherent reserved-memory regions");
