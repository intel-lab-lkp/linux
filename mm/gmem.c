/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generalized Memory Management.
 *
 * Copyright (C) 2023- Huawei, Inc.
 * Author: Weixi Zhu
 *
 */
#include <linux/mm.h>
#include <linux/gmem.h>
#include <linux/dma-mapping.h>
#include <linux/syscalls.h>
#include <linux/mman.h>

DEFINE_STATIC_KEY_FALSE(gmem_status);
EXPORT_SYMBOL_GPL(gmem_status);

static struct kmem_cache *gm_as_cache;
static struct kmem_cache *gm_dev_cache;
static struct kmem_cache *gm_ctx_cache;
static DEFINE_XARRAY_ALLOC(gm_dev_id_pool);

static bool enable_gmem;

DEFINE_SPINLOCK(hnode_lock);

struct hnode {
	unsigned int id;
	struct gm_dev *dev;
	struct xarray pages;
};

struct hnode *hnodes[MAX_NUMNODES];

static bool is_hnode(int node)
{
	return !node_isset(node, node_possible_map) &&
	       node_isset(node, hnode_map);
}

static bool is_hnode_allowed(int node)
{
	return is_hnode(node) && node_isset(node, current->mems_allowed);
}

static struct hnode *get_hnode(unsigned int hnid)
{
	return hnodes[hnid];
}

void __init hnuma_init(void)
{
	unsigned int node;

	for_each_node(node)
		node_set(node, hnode_map);
}

static unsigned int alloc_hnode_id(void)
{
	unsigned int node;

	spin_lock(&hnode_lock);
	node = first_unset_node(hnode_map);
	node_set(node, hnode_map);
	spin_unlock(&hnode_lock);

	return node;
}

static void free_hnode_id(unsigned int nid)
{
	node_clear(nid, hnode_map);
}

static void hnode_init(struct hnode *hnode, unsigned int hnid,
		       struct gm_dev *dev)
{
	hnodes[hnid] = hnode;
	hnodes[hnid]->id = hnid;
	hnodes[hnid]->dev = dev;
	node_set(hnid, dev->registered_hnodes);
	xa_init(&hnodes[hnid]->pages);
}

static void hnode_deinit(unsigned int hnid, struct gm_dev *dev)
{
	hnodes[hnid]->id = 0;
	hnodes[hnid]->dev = NULL;
	node_clear(hnid, dev->registered_hnodes);
	xa_destroy(&hnodes[hnid]->pages);
	hnodes[hnid] = NULL;
}

static struct workqueue_struct *prefetch_wq;

#define GM_WORK_CONCURRENCY 4

static int __init gmem_init(void)
{
	int err = -ENOMEM;

	if (!enable_gmem)
		return 0;

	gm_as_cache = KMEM_CACHE(gm_as, 0);
	if (!gm_as_cache)
		goto out;

	gm_dev_cache = KMEM_CACHE(gm_dev, 0);
	if (!gm_dev_cache)
		goto free_as;

	gm_ctx_cache = KMEM_CACHE(gm_context, 0);
	if (!gm_ctx_cache)
		goto free_dev;

	err = vm_object_init();
	if (err)
		goto free_ctx;

	prefetch_wq = alloc_workqueue("prefetch",
				      __WQ_LEGACY | WQ_UNBOUND | WQ_HIGHPRI |
					      WQ_CPU_INTENSIVE,
				      GM_WORK_CONCURRENCY);
	if (!prefetch_wq) {
		pr_info("fail to alloc workqueue prefetch_wq\n");
		err = -EFAULT;
		goto free_ctx;
	}

	static_branch_enable(&gmem_status);

	return 0;

free_ctx:
	kmem_cache_destroy(gm_ctx_cache);
free_dev:
	kmem_cache_destroy(gm_dev_cache);
free_as:
	kmem_cache_destroy(gm_as_cache);
out:
	return -ENOMEM;
}
subsys_initcall(gmem_init);

static int __init setup_gmem(char *str)
{
	(void)kstrtobool(str, &enable_gmem);

	return 1;
}
__setup("gmem=", setup_gmem);

/*
 * Create a GMEM device, register its MMU function and the page table.
 * The returned device pointer will be passed by new_dev.
 * A unique id will be assigned to the GMEM device, using Linux's xarray.
 */
int gm_dev_create(struct gm_mmu *mmu, void *dev_data, unsigned long cap,
		  struct gm_dev **new_dev)
{
	struct gm_dev *dev;

	if (!gmem_is_enabled())
		return GM_RET_FAILURE_UNKNOWN;

	dev = kmem_cache_alloc(gm_dev_cache, GFP_KERNEL);
	if (!dev)
		return GM_RET_NOMEM;

	if (xa_alloc(&gm_dev_id_pool, &dev->id, dev, xa_limit_32b, GFP_KERNEL)) {
		kmem_cache_free(gm_dev_cache, dev);
		return GM_RET_NOMEM;
	}

	dev->capability = cap;
	dev->mmu = mmu;
	dev->dev_data = dev_data;
	dev->current_ctx = NULL;
	INIT_LIST_HEAD(&dev->gm_ctx_list);
	*new_dev = dev;
	nodes_clear(dev->registered_hnodes);
	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_dev_create);

int gm_dev_destroy(struct gm_dev *dev)
{
	/* TODO: implement it */
	xa_erase(&gm_dev_id_pool, dev->id);
	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_dev_destroy);

int gm_dev_fault(struct mm_struct *mm, unsigned long addr, struct gm_dev *dev,
		 enum gm_fault_hint hint)
{
	int ret = GM_RET_SUCCESS;
	struct gm_mmu *mmu = dev->mmu;
	struct device *dma_dev = dev->dma_dev;
	struct vm_area_struct *vma;
	struct vm_object *obj;
	struct gm_mapping *gm_mapping;
	unsigned long size = HPAGE_SIZE;
	struct gm_fault_t gmf = {
		.mm = mm,
		.va = addr,
		.dev = dev,
		.size = size,
		.copy = false,
		.hint = hint
	};
	struct page *page = NULL;

	mmap_read_lock(mm);
	obj = mm->vm_obj;
	if (!obj) {
		pr_info("gmem: %s no vm_obj\n", __func__);
		ret = GM_RET_FAILURE_UNKNOWN;
		goto mmap_unlock;
	}

	vma = find_vma(mm, addr);
	if (!vma) {
		pr_info("gmem: %s no vma\n", __func__);
		ret = GM_RET_FAILURE_UNKNOWN;
		goto mmap_unlock;
	}
	obj = mm->vm_obj;
	if (!obj) {
		pr_info("gmem: %s no vm_obj\n", __func__);
		ret = GM_RET_FAILURE_UNKNOWN;
		goto mmap_unlock;
	}

	xa_lock(obj->logical_page_table);
	gm_mapping = vm_object_lookup(obj, addr);
	if (!gm_mapping) {
		vm_object_mapping_create(obj, addr);
		gm_mapping = vm_object_lookup(obj, addr);
	}
	xa_unlock(obj->logical_page_table);

	mutex_lock(&gm_mapping->lock);
	if (gm_mapping_nomap(gm_mapping)) {
		goto peer_map;
	} else if (gm_mapping_device(gm_mapping)) {
		if (hint == GM_FAULT_HINT_MARK_HOT) {
			goto peer_map;
		} else {
			ret = 0;
			goto unlock;
		}
	} else if (gm_mapping_cpu(gm_mapping)) {
		page = gm_mapping->page;
		if (!page) {
			pr_err("gmem: host gm_mapping page is NULL. Set nomap\n");
			gm_mapping_flags_set(gm_mapping, GM_PAGE_NOMAP);
			goto unlock;
		}
		get_page(page);
		zap_page_range_single(vma, addr, size, NULL);
		gmf.dma_addr = dma_map_page(dma_dev, page, 0, size, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dma_dev, gmf.dma_addr))
			pr_info("gmem: dma map failed\n");

		gmf.copy = true;
	}

peer_map:
	ret = mmu->peer_map(&gmf);
	if (ret != GM_RET_SUCCESS) {
		if (ret == GM_RET_MIGRATING) {
			/*
			 * gmem page is migrating due to overcommit.
			 * update page to willneed and this will stop page evicting
			 */
			gm_mapping_flags_set(gm_mapping, GM_PAGE_WILLNEED);
			ret = GM_RET_SUCCESS;
		} else {
			pr_err("gmem: peer map failed\n");
			if (page) {
				gm_mapping_flags_set(gm_mapping, GM_PAGE_NOMAP);
				put_page(page);
			}
		}
		goto unlock;
	}

	if (page) {
		dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
		put_page(page);
	}

	gm_mapping_flags_set(gm_mapping, GM_PAGE_DEVICE);
	gm_mapping->dev = dev;
unlock:
	mutex_unlock(&gm_mapping->lock);
mmap_unlock:
	mmap_read_unlock(mm);
	return ret;
}
EXPORT_SYMBOL_GPL(gm_dev_fault);

vm_fault_t gm_host_fault_locked(struct vm_fault *vmf, unsigned int order)
{
	vm_fault_t ret = 0;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address & ((1 << order) - 1);
	struct vm_object *obj = vma->vm_mm->vm_obj;
	struct gm_mapping *gm_mapping;
	unsigned long size = HPAGE_SIZE;
	struct gm_dev *dev;
	struct device *dma_dev;
	struct gm_fault_t gmf = {
		.mm = vma->vm_mm,
		.va = addr,
		.size = size,
		.copy = true,
	};

	gm_mapping = vm_object_lookup(obj, addr);
	if (!gm_mapping) {
		pr_err("gmem: host fault gm_mapping should not be NULL\n");
		return VM_FAULT_SIGBUS;
	}

	dev = gm_mapping->dev;
	gmf.dev = dev;
	dma_dev = dev->dma_dev;
	gmf.dma_addr = dma_map_page(dma_dev, vmf->page, 0, size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dma_dev, gmf.dma_addr)) {
		pr_err("gmem: host fault dma mapping error\n");
		return VM_FAULT_SIGBUS;
	}
	if (dev->mmu->peer_unmap(&gmf) != GM_RET_SUCCESS) {
		pr_err("gmem: peer unmap failed\n");
		dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
		return VM_FAULT_SIGBUS;
	}

	dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
	return ret;
}

int gm_dev_register_physmem(struct gm_dev *dev, unsigned long begin,
			    unsigned long end)
{
	struct gm_mapping *mapping;
	unsigned long addr = PAGE_ALIGN(begin);
	unsigned int nid;
	int i, page_num = (end - addr) >> PAGE_SHIFT;
	struct hnode *hnode = kmalloc(sizeof(struct hnode), GFP_KERNEL);

	if (!hnode)
		goto err;

	nid = alloc_hnode_id();
	if (nid == MAX_NUMNODES)
		goto free_hnode;
	hnode_init(hnode, nid, dev);

	/*
	 * TODO: replace the xarray bookkeeping code with an isolated buddy
	 * allocator here. Implement customized device page struct, which is
	 * trimmed for application-level usage.
	 */
	mapping = kvmalloc(sizeof(struct gm_mapping) * page_num, GFP_KERNEL);
	if (!mapping)
		goto deinit_hnode;

	for (i = 0; i < page_num; i++, addr += PAGE_SIZE) {
		mapping[i].pfn = addr >> PAGE_SHIFT;
		mapping[i].flag = 0;
	}

	xa_lock(&hnode->pages);
	for (i = 0; i < page_num; i++) {
		if (xa_err(__xa_store(&hnode->pages, i, mapping + i, GFP_KERNEL))) {
			kvfree(mapping);
			xa_unlock(&hnode->pages);
			goto deinit_hnode;
		}
		__xa_set_mark(&hnode->pages, i, XA_MARK_0);
	}
	xa_unlock(&hnode->pages);

	return GM_RET_SUCCESS;

deinit_hnode:
	hnode_deinit(nid, dev);
	free_hnode_id(nid);
free_hnode:
	kfree(hnode);
err:
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(gm_dev_register_physmem);

void gm_dev_unregister_physmem(struct gm_dev *dev, unsigned int nid)
{
	struct hnode *hnode = get_hnode(nid);
	struct gm_mapping *mapping = xa_load(&hnode->pages, 0);

	kvfree(mapping);
	hnode_deinit(nid, dev);
	free_hnode_id(nid);
	kfree(hnode);
}
EXPORT_SYMBOL_GPL(gm_dev_unregister_physmem);

/* GMEM Virtual Address Space API */
int gm_as_create(unsigned long begin, unsigned long end, struct gm_as **new_as)
{
	struct gm_as *as;

	if (!new_as)
		return -EINVAL;

	as = kmem_cache_alloc(gm_as_cache, GFP_ATOMIC);
	if (!as)
		return -ENOMEM;

	spin_lock_init(&as->lock);
	as->start_va = begin;
	as->end_va = end;

	INIT_LIST_HEAD(&as->gm_ctx_list);

	*new_as = as;
	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_as_create);

int gm_as_destroy(struct gm_as *as)
{
	struct gm_context *ctx, *tmp_ctx;

	list_for_each_entry_safe(ctx, tmp_ctx, &as->gm_ctx_list, gm_as_link)
		kfree(ctx);

	kmem_cache_free(gm_as_cache, as);

	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_as_destroy);

int gm_as_attach(struct gm_as *as, struct gm_dev *dev, enum gm_mmu_mode mode,
		 bool activate, struct gm_context **out_ctx)
{
	struct gm_context *ctx;
	int nid;
	int ret;

	ctx = kmem_cache_alloc(gm_ctx_cache, GFP_KERNEL);
	if (!ctx)
		return GM_RET_NOMEM;

	ctx->as = as;
	ctx->dev = dev;
	ctx->pmap = NULL;
	ret = dev->mmu->pmap_create(dev, &ctx->pmap);
	if (ret) {
		kmem_cache_free(gm_ctx_cache, ctx);
		return ret;
	}

	INIT_LIST_HEAD(&ctx->gm_dev_link);
	INIT_LIST_HEAD(&ctx->gm_as_link);
	list_add_tail(&dev->gm_ctx_list, &ctx->gm_dev_link);
	list_add_tail(&ctx->gm_as_link, &as->gm_ctx_list);

	if (activate) {
		/*
		 * Here we should really have a callback function to perform the context switch
		 * for the hardware. E.g. in x86 this function is effectively flushing the CR3 value.
		 * Currently we do not care time-sliced context switch, unless someone wants to support it.
		 */
		dev->current_ctx = ctx;
	}
	*out_ctx = ctx;

	for_each_node_mask(nid, dev->registered_hnodes)
		node_set(nid, current->mems_allowed);
	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_as_attach);

struct prefetch_data {
	struct mm_struct *mm;
	struct gm_dev *dev;
	unsigned long addr;
	size_t size;
	struct work_struct work;
	int *res;
};

static void prefetch_work_cb(struct work_struct *work)
{
	struct prefetch_data *d =
		container_of(work, struct prefetch_data, work);
	unsigned long addr = d->addr, end = d->addr + d->size;
	int page_size = HPAGE_SIZE;
	int ret;

	do {
		/*
		 * Pass a hint to tell gm_dev_fault() to invoke peer_map anyways
		 * and implicitly mark the mapped physical page as recently-used.
		 */
		ret = gm_dev_fault(d->mm, addr, d->dev, GM_FAULT_HINT_MARK_HOT);
		if (ret == GM_RET_PAGE_EXIST) {
			pr_info("%s: device has done page fault, ignore prefetch\n", __func__);
		} else if (ret != GM_RET_SUCCESS) {
			*d->res = -EFAULT;
			pr_err("%s: call dev fault error %d\n", __func__, ret);
		}
	} while (addr += page_size, addr != end);

	kfree(d);
}

static int hmadvise_do_prefetch(struct gm_dev *dev, unsigned long addr, size_t size)
{
	unsigned long start, end, per_size;
	int page_size = HPAGE_SIZE;
	struct prefetch_data *data;
	struct vm_area_struct *vma;
	int res = GM_RET_SUCCESS;

	end = round_up(addr + size, page_size);
	start = round_down(addr, page_size);
	size = end - start;

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, start);
	if (!vma || start < vma->vm_start || end > vma->vm_end) {
		mmap_read_unlock(current->mm);
		return GM_RET_FAILURE_UNKNOWN;
	}
	mmap_read_unlock(current->mm);

	per_size = (size / GM_WORK_CONCURRENCY) & ~(page_size - 1);

	while (start < end) {
		data = kzalloc(sizeof(struct prefetch_data), GFP_KERNEL);
		if (!data) {
			flush_workqueue(prefetch_wq);
			return GM_RET_NOMEM;
		}

		INIT_WORK(&data->work, prefetch_work_cb);
		data->mm = current->mm;
		data->dev = dev;
		data->addr = start;
		data->res = &res;
		if (per_size == 0)
			data->size = size;
		else
			data->size = (end - start < 2 * per_size) ? (end - start) : per_size;
		queue_work(prefetch_wq, &data->work);
		start += data->size;
	}

	flush_workqueue(prefetch_wq);
	return res;
}

static int gm_unmap_page_range(struct vm_area_struct *vma, unsigned long start,
			       unsigned long end, int page_size)
{
	struct gm_fault_t gmf = {
		.mm = current->mm,
		.size = page_size,
		.copy = false,
	};
	struct gm_mapping *gm_mapping;
	struct vm_object *obj;
	int ret;

	obj = current->mm->vm_obj;
	if (!obj) {
		pr_err("gmem: peer-shared vma should have vm_object\n");
		return -EINVAL;
	}

	for (; start < end; start += page_size) {
		xa_lock(obj->logical_page_table);
		gm_mapping = vm_object_lookup(obj, start);
		if (!gm_mapping) {
			xa_unlock(obj->logical_page_table);
			continue;
		}
		xa_unlock(obj->logical_page_table);
		mutex_lock(&gm_mapping->lock);
		if (gm_mapping_nomap(gm_mapping)) {
			mutex_unlock(&gm_mapping->lock);
			continue;
		} else if (gm_mapping_cpu(gm_mapping)) {
			zap_page_range_single(vma, start, page_size, NULL);
		} else {
			gmf.va = start;
			gmf.dev = gm_mapping->dev;
			ret = gm_mapping->dev->mmu->peer_unmap(&gmf);
			if (ret) {
				pr_err("gmem: peer_unmap failed. ret %d\n",
				       ret);
				mutex_unlock(&gm_mapping->lock);
				continue;
			}
		}
		gm_mapping_flags_set(gm_mapping, GM_PAGE_NOMAP);
		mutex_unlock(&gm_mapping->lock);
	}

	return 0;
}

int gm_alloc_va_in_peer_devices(struct mm_struct *mm,
				struct vm_area_struct *vma, unsigned long addr,
				unsigned long len, vm_flags_t vm_flags)
{
	struct gm_context *ctx, *tmp;
	int ret;

	pr_debug("gmem: start mmap, as %p\n", mm->gm_as);
	if (!mm->gm_as)
		return -ENODEV;

	if (!mm->vm_obj)
		mm->vm_obj = vm_object_create(mm);
	if (!mm->vm_obj)
		return -ENOMEM;
	/*
	 * TODO: solve the race condition if a device is concurrently attached
	 * to mm->gm_as.
	 */
	list_for_each_entry_safe(ctx, tmp, &mm->gm_as->gm_ctx_list, gm_as_link) {
		if (!gm_dev_is_peer(ctx->dev))
			continue;

		if (!ctx->dev->mmu->peer_va_alloc_fixed) {
			pr_debug("gmem: mmu ops has no alloc_vma\n");
			continue;
		}

		ret = ctx->dev->mmu->peer_va_alloc_fixed(mm, addr, len, vm_flags);
		if (ret != GM_RET_SUCCESS) {
			pr_debug("gmem: alloc_vma ret %d\n", ret);
			return ret;
		}
	}

	return GM_RET_SUCCESS;
}

static int hmadvise_do_eagerfree(unsigned long addr, size_t size)
{
	unsigned long start, end, i_start, i_end;
	int page_size = HPAGE_SIZE;
	struct vm_area_struct *vma;
	int ret = GM_RET_SUCCESS;
	unsigned long old_start;

	if (check_add_overflow(addr, size, &end))
		return -EINVAL;

	old_start = addr;

	end = round_down(addr + size, page_size);
	start = round_up(addr, page_size);
	if (start >= end)
		return ret;

	mmap_read_lock(current->mm);
	do {
		vma = find_vma_intersection(current->mm, start, end);
		if (!vma) {
			pr_info("gmem: there is no valid vma\n");
			break;
		}

		if (!vma_is_peer_shared(vma)) {
			pr_debug("gmem: not peer-shared vma, skip dontneed\n");
			start = vma->vm_end;
			continue;
		}

		i_start = start > vma->vm_start ? start : vma->vm_start;
		i_end = end < vma->vm_end ? end : vma->vm_end;
		ret = gm_unmap_page_range(vma, i_start, i_end, page_size);
		if (ret)
			break;

		start = vma->vm_end;
	} while (start < end);

	mmap_read_unlock(current->mm);
	return ret;
}

static bool check_hmadvise_behavior(int behavior)
{
	return behavior == MADV_DONTNEED;
}

SYSCALL_DEFINE4(hmadvise, int, hnid, unsigned long, start, size_t, len_in, int, behavior)
{
	int error = -EINVAL;
	struct hnode *node;

	if (hnid == -1) {
		if (check_hmadvise_behavior(behavior)) {
			goto no_hnid;
		} else {
			pr_err("hmadvise: behavior %d need hnid or is invalid\n",
				behavior);
			return error;
		}
	}

	if (hnid < 0)
		return error;

	if (!is_hnode(hnid) || !is_hnode_allowed(hnid))
		return error;

	node = get_hnode(hnid);
	if (!node) {
		pr_err("hmadvise: hnode id %d is invalid\n", hnid);
		return error;
	}

no_hnid:
	switch (behavior) {
	case MADV_PREFETCH:
		return hmadvise_do_prefetch(node->dev, start, len_in);
	case MADV_DONTNEED:
		return hmadvise_do_eagerfree(start, len_in);
	default:
		pr_err("hmadvise: unsupported behavior %d\n", behavior);
	}

	return error;
}
