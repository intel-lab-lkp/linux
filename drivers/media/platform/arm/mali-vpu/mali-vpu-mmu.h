/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_MMU_H__
#define __MALI_VPU_MMU_H__

#include <linux/dma-mapping.h>
#include <linux/hashtable.h>
#include <linux/types.h>

/* Page size is 4 KB - pow(2,12) bytes */
#define MALI_VPU_PAGE_SHIFT		12
#define MALI_VPU_PAGE_SIZE		BIT(MALI_VPU_PAGE_SHIFT)
#define MALI_VPU_PAGE_MASK		(MALI_VPU_PAGE_SIZE - 1)

/**
 * typedef mali_vpu_mmu_va - 32 bit virtual address.
 *
 * This is the address the firmware/MVE will use.
 */
typedef u32 mali_vpu_mmu_va;

/**
 * typedef mali_vpu_mmu_pte - Page table entry.
 *
 * A PTE pointer should always point at a Linux kernel virtual address.
 *
 * AT - Attribute.
 * PA - VPU physical address bits [39:12], supplied as a DMA address.
 * AP - Access permission.
 *
 *    30                                                       2   0
 * +---+-------------------------------------------------------+---+
 * | AT|                          PA                           | AP|
 * +---+-------------------------------------------------------+---+
 */
typedef u32 mali_vpu_mmu_pte;

enum mali_vpu_mmu_attr {
	MALI_VPU_ATTR_PRIVATE   = 0,
	MALI_VPU_ATTR_REFFRAME  = 1,
	MALI_VPU_ATTR_SHARED_RO = 2,
	MALI_VPU_ATTR_SHARED_RW = 3
};

enum mali_vpu_mmu_access {
	MALI_VPU_ACCESS_NO         = 0,
	MALI_VPU_ACCESS_READ_ONLY  = 1,
	MALI_VPU_ACCESS_EXECUTABLE = 2,
	MALI_VPU_ACCESS_READ_WRITE = 3
};

/**
 * struct mali_vpu_mmu_pages - Structure used to allocate an array of pages.
 * @dev: Pointer to device.
 * @node: Hash table node. Used to keep track of allocated pages objects.
 * @mmu: Pointer to MMU instance.
 * @va: MVE virtual address. Set to 0 if objects is unmapped.
 * @offset: Offset from mapped VA to where the data begins.
 * @attr: Page table attributes.
 * @access: Page table access.
 * @capacity: Maximum number of MVE pages this object can hold.
 * @count: Current number of allocated pages.
 * @is_external: If the physical pages have been externally allocated.
 * @pages: DMA address and original backing Linux page for each MVE page.
 *         The backing page is NULL for external memory and may be shared by
 *         multiple MVE entries when Linux pages are larger than MVE pages.
 */
struct mali_vpu_mmu_pages {
	struct device *dev;
	struct hlist_node node;
	struct mali_vpu_mmu *mmu;
	mali_vpu_mmu_va va;
	size_t offset;
	enum mali_vpu_mmu_attr attr;
	enum mali_vpu_mmu_access access;
	size_t capacity;
	size_t count;
	bool is_external;
	struct {
		dma_addr_t dma;
		struct page *linux_page;
	} pages[];
};

/**
 * struct mali_vpu_mmu_pt_page - Backing memory for one VPU page table.
 * @linux_page: Original Linux page backing the table.
 * @cpu: Kernel virtual address used to access the table entries.
 * @dma: DMA address used by the VPU page-table walker.
 */
struct mali_vpu_mmu_pt_page {
	struct page *linux_page;
	mali_vpu_mmu_pte *cpu;
	dma_addr_t dma;
};

/**
 * struct mali_vpu_mmu - VPU page tables for one session.
 * @dev: Device used for DMA mapping and synchronization.
 * @root: Root (L1) page table.
 * @l2_tables: Array of L2 table descriptors indexed by the L1 index.
 *             L2 backing pages are allocated on demand.
 */
struct mali_vpu_mmu {
	struct device *dev;
	struct mali_vpu_mmu_pt_page root;
	struct mali_vpu_mmu_pt_page *l2_tables;
};

int mali_vpu_mmu_init(struct mali_vpu_mmu *mmu, struct device *dev);
void mali_vpu_mmu_cleanup(struct mali_vpu_mmu *mmu);

dma_addr_t mali_vpu_mmu_alloc_page(struct device *dev, struct page **out_page,
				   enum dma_data_direction dir);
void mali_vpu_mmu_free_page(struct device *dev, dma_addr_t dma, struct page *page,
			    enum dma_data_direction dir);

struct mali_vpu_mmu_pages *mali_vpu_mmu_alloc_pages(struct device *dev,
						    size_t count,
						    size_t capacity);

struct mali_vpu_mmu_pages *mali_vpu_mmu_alloc_pages_sg(struct device *dev,
						       struct sg_table *sgt,
						       size_t capacity);

size_t mali_vpu_mmu_size_pages(struct mali_vpu_mmu_pages *pages);
int mali_vpu_mmu_resize_pages(struct mali_vpu_mmu_pages *pages, size_t npages);
void mali_vpu_mmu_free_pages(struct mali_vpu_mmu_pages *pages);
int mali_vpu_mmu_sync_pages(struct mali_vpu_mmu_pages *pages, enum dma_data_direction dir);

int mali_vpu_mmu_map_pages(struct mali_vpu_mmu *mmu,
			   mali_vpu_mmu_va va,
			   struct mali_vpu_mmu_pages *pages,
			   enum mali_vpu_mmu_attr attr,
			   enum mali_vpu_mmu_access access);

void mali_vpu_mmu_unmap_pages(struct mali_vpu_mmu_pages *pages);

int mali_vpu_mmu_map_dma(struct mali_vpu_mmu *mmu,
			 mali_vpu_mmu_va va,
			 dma_addr_t dma,
			 size_t size,
			 enum mali_vpu_mmu_attr attr,
			 enum mali_vpu_mmu_access access);

void mali_vpu_mmu_unmap_va(struct mali_vpu_mmu *mmu, mali_vpu_mmu_va va, size_t size);
int mali_vpu_mmu_write(struct mali_vpu_mmu_pages *pages, const void *data, size_t size);

mali_vpu_mmu_pte mali_vpu_mmu_get_root_pte(const struct mali_vpu_mmu *mmu);

#endif /* __MALI_VPU_MMU_H__ */
