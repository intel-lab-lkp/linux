/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generalized Memory Management.
 *
 * Copyright (C) 2023- Huawei, Inc.
 * Author: Weixi Zhu
 *
 */
#ifndef _GMEM_H
#define _GMEM_H

#include <linux/mm_types.h>

#ifdef CONFIG_GMEM

#define GM_PAGE_CPU	0x10 /* Determines whether page is a pointer or a pfn number. */
#define GM_PAGE_DEVICE	0x20
#define GM_PAGE_NOMAP	0x40
#define GM_PAGE_WILLNEED	0x80

#define GM_PAGE_TYPE_MASK	(GM_PAGE_CPU | GM_PAGE_DEVICE | GM_PAGE_NOMAP)

struct gm_mapping {
	unsigned int flag;

	union {
		struct page *page;	/* CPU node */
		struct gm_dev *dev;	/* hetero-node. TODO: support multiple devices */
		unsigned long pfn;
	};

	struct mutex lock;
};

static inline void gm_mapping_flags_set(struct gm_mapping *gm_mapping, int flags)
{
	if (flags & GM_PAGE_TYPE_MASK)
		gm_mapping->flag &= ~GM_PAGE_TYPE_MASK;

	gm_mapping->flag |= flags;
}

static inline void gm_mapping_flags_clear(struct gm_mapping *gm_mapping, int flags)
{
	gm_mapping->flag &= ~flags;
}

static inline bool gm_mapping_cpu(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_PAGE_CPU);
}

static inline bool gm_mapping_device(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_PAGE_DEVICE);
}

static inline bool gm_mapping_nomap(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_PAGE_NOMAP);
}

static inline bool gm_mapping_willneed(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_PAGE_WILLNEED);
}

/* h-NUMA topology */
void __init hnuma_init(void);

/* vm object */
/*
 * Each per-process vm_object tracks the mapping status of virtual pages from
 * all VMAs mmap()-ed with MAP_PRIVATE | MAP_PEER_SHARED.
 */
struct vm_object {
	spinlock_t lock;

	/*
	 * The logical_page_table is a container that holds the mapping
	 * information between a VA and a struct page.
	 */
	struct xarray *logical_page_table;
	atomic_t nr_pages;
};

int __init vm_object_init(void);
struct vm_object *vm_object_create(struct mm_struct *mm);
void vm_object_drop_locked(struct mm_struct *mm);

struct gm_mapping *alloc_gm_mapping(void);
void free_gm_mappings(struct vm_area_struct *vma);
struct gm_mapping *vm_object_lookup(struct vm_object *obj, unsigned long va);
void vm_object_mapping_create(struct vm_object *obj, unsigned long start);
void unmap_gm_mappings_range(struct vm_area_struct *vma, unsigned long start,
			     unsigned long end);
void munmap_in_peer_devices(struct mm_struct *mm, unsigned long start,
			    unsigned long end);
#else
static inline void hnuma_init(void) {}
static inline void __init vm_object_init(void)
{
}
static inline struct vm_object *vm_object_create(struct vm_area_struct *vma)
{
	return NULL;
}
static inline void vm_object_drop_locked(struct vm_area_struct *vma)
{
}
static inline struct gm_mapping *alloc_gm_mapping(void)
{
	return NULL;
}
static inline void free_gm_mappings(struct vm_area_struct *vma)
{
}
static inline struct gm_mapping *vm_object_lookup(struct vm_object *obj,
						  unsigned long va)
{
	return NULL;
}
static inline void vm_object_mapping_create(struct vm_object *obj,
					    unsigned long start)
{
}
static inline void unmap_gm_mappings_range(struct vm_area_struct *vma,
					   unsigned long start,
					   unsigned long end)
{
}
static inline void munmap_in_peer_devices(struct mm_struct *mm,
					  unsigned long start,
					  unsigned long end)
{
}
#endif

#endif /* _GMEM_H */
