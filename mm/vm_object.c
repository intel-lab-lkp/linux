// SPDX-License-Identifier: GPL-2.0
/*
 * arch/alpha/boot/bootp.c
 *
 * Copyright (C) 1997 Jay Estabrook
 *
 * This file is used for creating a bootp file for the Linux/AXP kernel
 *
 * based significantly on the arch/alpha/boot/main.c of Linus Torvalds
 */
#include <linux/mm.h>
#include <linux/gmem.h>

/*
 * Sine VM_OBJECT maintains the logical page table under each VMA, and each VMA
 * points to a VM_OBJECT. Ultimately VM_OBJECTs must be maintained as long as VMA
 * gets changed: merge, split, adjust
 */
static struct kmem_cache *vm_object_cachep;
static struct kmem_cache *gm_mapping_cachep;

static inline void release_gm_mapping(struct gm_mapping *mapping)
{
	kmem_cache_free(gm_mapping_cachep, mapping);
}

static inline struct gm_mapping *lookup_gm_mapping(struct vm_object *obj,
						   unsigned long pindex)
{
	return xa_load(obj->logical_page_table, pindex);
}

int __init vm_object_init(void)
{
	vm_object_cachep = KMEM_CACHE(vm_object, 0);
	if (!vm_object_cachep)
		goto out;

	gm_mapping_cachep = KMEM_CACHE(gm_mapping, 0);
	if (!gm_mapping_cachep)
		goto free_vm_object;

	return 0;
free_vm_object:
	kmem_cache_destroy(vm_object_cachep);
out:
	return -ENOMEM;
}

/*
 * Create a VM_OBJECT and attach it to a mm_struct
 * This should be called when a task_struct is created.
 */
struct vm_object *vm_object_create(struct mm_struct *mm)
{
	struct vm_object *obj = kmem_cache_alloc(vm_object_cachep, GFP_KERNEL);

	if (!obj)
		return NULL;

	spin_lock_init(&obj->lock);

	/*
	 * The logical page table maps va >> PAGE_SHIFT
	 * to pointers of struct gm_mapping.
	 */
	obj->logical_page_table = kmalloc(sizeof(struct xarray), GFP_KERNEL);
	if (!obj->logical_page_table) {
		kmem_cache_free(vm_object_cachep, obj);
		return NULL;
	}

	xa_init(obj->logical_page_table);
	atomic_set(&obj->nr_pages, 0);

	return obj;
}

/* This should be called when a mm no longer refers to a VM_OBJECT */
void vm_object_drop_locked(struct mm_struct *mm)
{
	struct vm_object *obj = mm->vm_obj;

	if (!obj)
		return;

	/*
	 * We must enter this with VMA write-locked, which is unfortunately a
	 * giant lock.
	 */
	mmap_assert_write_locked(mm);
	mm->vm_obj = NULL;

	xa_destroy(obj->logical_page_table);
	kfree(obj->logical_page_table);
	kmem_cache_free(vm_object_cachep, obj);
}

/*
 * Given a VA, the page_index is computed by
 * page_index = address >> PAGE_SHIFT
 */
struct gm_mapping *vm_object_lookup(struct vm_object *obj, unsigned long va)
{
	return lookup_gm_mapping(obj, va >> PAGE_SHIFT);
}
EXPORT_SYMBOL_GPL(vm_object_lookup);

void vm_object_mapping_create(struct vm_object *obj, unsigned long start)
{

	unsigned long index = start >> PAGE_SHIFT;
	struct gm_mapping *gm_mapping;

	if (!obj)
		return;

	gm_mapping = alloc_gm_mapping();
	if (!gm_mapping)
		return;

	__xa_store(obj->logical_page_table, index, gm_mapping, GFP_KERNEL);
}

/* gm_mapping will not be release dynamically */
struct gm_mapping *alloc_gm_mapping(void)
{
	struct gm_mapping *gm_mapping = kmem_cache_zalloc(gm_mapping_cachep, GFP_KERNEL);

	if (!gm_mapping)
		return NULL;

	gm_mapping_flags_set(gm_mapping, GM_PAGE_NOMAP);
	mutex_init(&gm_mapping->lock);

	return gm_mapping;
}

/* This should be called when a PEER_SHAERD vma is freed */
void free_gm_mappings(struct vm_area_struct *vma)
{
	struct gm_mapping *gm_mapping;
	struct vm_object *obj;

	if (vma_is_peer_shared(vma))
		return;

	obj = vma->vm_mm->vm_obj;
	if (!obj)
		return;

	XA_STATE(xas, obj->logical_page_table, vma->vm_start >> PAGE_SHIFT);

	xa_lock(obj->logical_page_table);
		xas_for_each(&xas, gm_mapping, vma->vm_end >> PAGE_SHIFT) {
		release_gm_mapping(gm_mapping);
		xas_store(&xas, NULL);
	}
	xa_unlock(obj->logical_page_table);
}

void unmap_gm_mappings_range(struct vm_area_struct *vma, unsigned long start,
			    unsigned long end)
{
	struct xarray *logical_page_table;
	struct gm_mapping *gm_mapping;
	struct page *page = NULL;

	if (!vma_is_peer_shared(vma))
		return;

	if (!vma->vm_mm->vm_obj)
		return;

	logical_page_table = vma->vm_mm->vm_obj->logical_page_table;
	if (!logical_page_table)
		return;

	XA_STATE(xas, logical_page_table, start >> PAGE_SHIFT);

	xa_lock(logical_page_table);
	xas_for_each(&xas, gm_mapping, end >> PAGE_SHIFT) {
		page = gm_mapping->page;
		if (page && (page_ref_count(page) != 0)) {
			put_page(page);
			gm_mapping->page = NULL;
		}
	}
	xa_unlock(logical_page_table);
}

struct gm_vma_list {
	struct vm_area_struct *vma;
	struct list_head list;
};

void gm_reserve_vma(struct vm_area_struct *value, struct list_head *head)
{
	struct gm_vma_list *node;

	if (!gmem_is_enabled())
		return;

	node = kmalloc(sizeof(struct gm_vma_list), GFP_KERNEL);
	if (!node)
		return;

	node->vma = value;
	list_add_tail(&node->list, head);
}

void gm_release_vma(struct mm_struct *mm, struct list_head *head)
{
	struct gm_vma_list *node, *next;

	if (!gmem_is_enabled())
		return;

	list_for_each_entry_safe(node, next, head, list) {
		struct vm_area_struct *vma = node->vma;

		if (vma != NULL)
			vm_area_free(vma);

		list_del(&node->list);
		kfree(node);
	}
}

static int munmap_in_peer_devices_inner(struct mm_struct *mm,
					struct vm_area_struct *vma,
					unsigned long start, unsigned long end,
					int page_size)
{
	struct vm_object *obj = mm->vm_obj;
	struct gm_mapping *gm_mapping;
	struct gm_fault_t gmf = {
		.mm = mm,
		.copy = false,
	};
	int ret;

	start = start > vma->vm_start ? start : vma->vm_start;
	end = end < vma->vm_end ? end : vma->vm_end;

	for (; start < end; start += page_size) {
		xa_lock(obj->logical_page_table);
		gm_mapping = vm_object_lookup(obj, start);
		if (!gm_mapping) {
			xa_unlock(obj->logical_page_table);
			continue;
		}
		xa_unlock(obj->logical_page_table);

		mutex_lock(&gm_mapping->lock);
		if (!gm_mapping_device(gm_mapping)) {
			mutex_unlock(&gm_mapping->lock);
			continue;
		}

		gmf.va = start;
		gmf.size = page_size;
		gmf.dev = gm_mapping->dev;
		ret = gm_mapping->dev->mmu->peer_unmap(&gmf);
		if (ret != GM_RET_SUCCESS) {
			pr_err("%s: call dev peer_unmap error %d\n", __func__,
			       ret);
			mutex_unlock(&gm_mapping->lock);
			continue;
		}
		mutex_unlock(&gm_mapping->lock);
	}

	return 0;
}

void munmap_in_peer_devices(struct mm_struct *mm, unsigned long start,
			    unsigned long end)
{
	struct vm_object *obj = mm->vm_obj;
	struct vm_area_struct *vma;

	if (!gmem_is_enabled())
		return;

	if (!obj)
		return;

	if (!mm->gm_as)
		return;

	mmap_read_lock(mm);
	do {
		vma = find_vma_intersection(mm, start, end);
		if (!vma) {
			pr_debug("gmem: there is no valid vma\n");
			break;
		}

		if (!vma_is_peer_shared(vma)) {
			pr_debug("gmem: not peer-shared vma, skip dontneed\n");
			start = vma->vm_end;
			continue;
		}

		munmap_in_peer_devices_inner(mm, vma, start, end, HPAGE_SIZE);
	} while (start < end);
	mmap_read_unlock(mm);
}
