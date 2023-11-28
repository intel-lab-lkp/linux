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

#define GMEM_MMAP_RETRY_TIMES 10 /* gmem retry times before OOM */

DECLARE_STATIC_KEY_FALSE(gmem_status);

static inline bool gmem_is_enabled(void)
{
	return static_branch_likely(&gmem_status);
}

struct gm_dev {
	int id;

	/*
	 * TODO: define more device capabilities and consider different device
	 * base page sizes
	 */
	unsigned long capability;
	struct gm_mmu *mmu;
	void *dev_data;
	/* A device may support time-sliced context switch. */
	struct gm_context *current_ctx;

	struct list_head gm_ctx_list;

	/* Add tracking of registered device local physical memory. */
	nodemask_t registered_hnodes;
	struct device *dma_dev;
};

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

/* core gmem */
enum gm_ret {
	GM_RET_SUCCESS = 0,
	GM_RET_NOMEM,
	GM_RET_PAGE_EXIST,
	GM_RET_MIGRATING,
	GM_RET_FAILURE_UNKNOWN,
};

/**
 * enum gm_mmu_mode - defines the method to share a physical page table.
 *
 * @GM_MMU_MODE_SHARE: Share a physical page table with another attached
 * device's MMU, requiring one of the attached MMUs to be compatible. For
 * example, the IOMMU is compatible with the CPU MMU on most modern machines.
 * This mode requires the device physical memory to be cache-coherent.
 * TODO: add MMU cookie to detect compatible MMUs.
 *
 * @GM_MMU_MODE_COHERENT_EXCLUSIVE: Maintain a coherent page table that holds
 * exclusive mapping entries, so that device memory accesses can trigger
 * fault-driven migration for automatic data locality optimizations.
 * This mode does not require a cache-coherent link between the CPU and device.
 *
 * @GM_MMU_MODE_REPLICATE: Maintain a coherent page table that replicates
 * physical mapping entries whenever a physical mapping is installed inside the
 * address space, so that it may minimize the page faults to be triggered by
 * this device.
 * This mode requires the device physical memory to be cache-coherent.
 */
enum gm_mmu_mode {
	GM_MMU_MODE_SHARE,
	GM_MMU_MODE_COHERENT_EXCLUSIVE,
	GM_MMU_MODE_REPLICATE,
};

enum gm_fault_hint {
	GM_FAULT_HINT_MARK_HOT,
	/*
	 * TODO: introduce other fault hints, e.g. read-only duplication, map
	 * remotely instead of migrating.
	 */
};

/* Parameter list for peer_map/peer_unmap mmu functions. */
struct gm_fault_t {
	struct mm_struct *mm;
	struct gm_dev *dev;
	unsigned long va;
	unsigned long size;
	unsigned long prot;
	bool copy;	/* Set dma_addr with a valid address if true */
	dma_addr_t dma_addr;
	enum gm_fault_hint hint;
};

/**
 * This struct defines a series of MMU functions registered by a peripheral
 * device that is to be invoked by GMEM.
 *
 * pmap is an opaque pointer that identifies a physical page table of a device.
 * A physical page table holds the physical mappings that can be interpreted by
 * the hardware MMU.
 */
struct gm_mmu {
	/*
	 * TODO: currently the device is assumed to support the same base page
	 * size and huge page size as the host, which is not necessarily the
	 * fact. Consider customized page sizes and add MMU cookie to identify
	 * compatible MMUs which can share page tables.
	 */

	/* Synchronize VMA in a peer OS to interact with the host OS */
	int (*peer_va_alloc_fixed)(struct mm_struct *mm, unsigned long va,
				   unsigned long size, unsigned long prot);
	int (*peer_va_free)(struct mm_struct *mm, unsigned long va,
			    unsigned long size);

	/* Create physical mappings on peer host.
	 * If copy is set, copy data [dma_addr, dma_addr + size] to peer host
	 */
	int (*peer_map)(struct gm_fault_t *gmf);
	/*
	 * Destroy physical mappings on peer host.
	 * If copy is set, copy data back to [dma_addr, dma_addr + size]
	 */
	int (*peer_unmap)(struct gm_fault_t *gmf);

	/* Create or destroy a device's physical page table. */
	int (*pmap_create)(struct gm_dev *dev, void **pmap);
	int (*pmap_destroy)(void *pmap);

	/* Create or destroy a physical mapping of a created physical page table */
	int (*pmap_enter)(void *pmap, unsigned long va, unsigned long size,
			  unsigned long pa, unsigned long prot);
	int (*pmap_release)(void *pmap, unsigned long va, unsigned long size);

	/* Change the protection of a virtual page */
	int (*pmap_protect)(void *pmap, unsigned long va, unsigned long size,
			    unsigned long new_prot);

	/* Invalidation functions of the MMU TLB */
	int (*tlb_invl)(void *pmap, unsigned long va, unsigned long size);
	int (*tlb_invl_coalesced)(void *pmap, struct list_head *mappings);
};

/**
 * gm dev cap defines a composable flag to describe the capabilities of a device.
 *
 * @GM_DEV_CAP_REPLAYABLE: Memory accesses can be replayed to recover page faults.
 * @GM_DEV_CAP_PEER: The device has its own VMA/PA management, controlled by another peer OS
 */
#define GM_DEV_CAP_REPLAYABLE	0x00000001
#define GM_DEV_CAP_PEER		0x00000010

#define gm_dev_is_peer(dev) (((dev)->capability & GM_DEV_CAP_PEER) != 0)

struct gm_context {
	struct gm_as *as;
	struct gm_dev *dev;
	void *pmap;
	/* List of device contexts with the same struct gm_dev */
	struct list_head gm_dev_link;

	/* List of device contexts within the same address space */
	struct list_head gm_as_link;
};

vm_fault_t gm_host_fault_locked(struct vm_fault *vmf, unsigned int order);

/* GMEM Device KPI */
int gm_dev_create(struct gm_mmu *mmu, void *dev_data, unsigned long cap,
		  struct gm_dev **new_dev);
int gm_dev_destroy(struct gm_dev *dev);
int gm_dev_register_physmem(struct gm_dev *dev, unsigned long begin,
			    unsigned long end);
int gm_dev_fault(struct mm_struct *mm, unsigned long addr, struct gm_dev *dev,
		 enum gm_fault_hint hint);

/* Defines an address space. */
struct gm_as {
	spinlock_t lock; /* spinlock of struct gm_as */
	unsigned long start_va;
	unsigned long end_va;

	struct list_head gm_ctx_list; /* tracks device contexts attached to this va space, using gm_as_link */
};

/* GMEM address space KPI */
int gm_as_create(unsigned long begin, unsigned long end, struct gm_as **new_as);
int gm_as_destroy(struct gm_as *as);
int gm_as_attach(struct gm_as *as, struct gm_dev *dev, enum gm_mmu_mode mode,
		 bool activate, struct gm_context **out_ctx);
#else
static inline bool gmem_is_enabled(void) { return false; }
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
int gm_as_create(unsigned long begin, unsigned long end, struct gm_as **new_as)
{
	return 0;
}
int gm_as_destroy(struct gm_as *as)
{
	return 0;
}
int gm_as_attach(struct gm_as *as, struct gm_dev *dev, enum gm_mmu_mode mode,
		 bool activate, struct gm_context **out_ctx)
{
	return 0;
}
#endif

#endif /* _GMEM_H */
