// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtual IOMMU - mapping user memory to the real device
 * Copyright (c) 2019 - Maxim Levitsky
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/mdev.h>
#include <linux/vmalloc.h>
#include <linux/nvme.h>
#include <linux/iommu.h>
#include <linux/interval_tree_generic.h>
#include "priv.h"

struct mem_mapping {
	struct rb_node rb;
	struct list_head link;

	dma_addr_t __subtree_last;
	dma_addr_t iova_start; /* first iova in this mapping */
	dma_addr_t iova_last;  /* last iova in this mapping */

	unsigned long pfn;  /* physical address of this mapping */
	int refcount;
};

#define map_len(m) (((m)->iova_last - (m)->iova_start) + 1ULL)
#define map_pages(m) (map_len(m) >> PAGE_SHIFT)
#define START(node) ((node)->iova_start)
#define LAST(node) ((node)->iova_last)

INTERVAL_TREE_DEFINE(struct mem_mapping, rb, dma_addr_t, __subtree_last,
		     START, LAST, static inline, viommu_int_tree);

static void nvmet_mdev_viommu_dbg_dma_range(struct nvmet_mdev_viommu *viommu,
					    struct mem_mapping *map,
					    const char *action)
{
	dma_addr_t iova_start  = map->iova_start;
	dma_addr_t iova_end    = map->iova_start + map_len(map) - 1;

	_DBG(viommu->vctrl, "vIOMMU: %s RW IOVA %pad-%pad",
	     action, &iova_start, &iova_end);
}

/* unpin N pages starting at given IOVA */
static void nvmet_mdev_viommu_unpin_pages(struct nvmet_mdev_viommu *viommu,
					  dma_addr_t iova, int n)
{
	int i, npages;

	for (i = 0; i < n; i += npages) {
		npages = min_t(int, VFIO_PIN_PAGES_MAX_ENTRIES, n);

		vfio_unpin_pages(viommu->vfio_dev, iova + (i * PAGE_SIZE),
				 npages);
	}
}

/* User memory init code */
void nvmet_mdev_viommu_init(struct nvmet_mdev_viommu *viommu,
			    struct vfio_device *vfio_dev)
{
	viommu->vfio_dev = vfio_dev;
	viommu->maps_tree = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&viommu->mem_map_list);
}

int nvmet_mdev_viommu_remove_list(struct nvmet_mdev_viommu *viommu,
				  struct list_head *remove_list)
{
	struct mem_mapping *map, *tmp;
	int count = 0;

	list_for_each_entry_safe(map, tmp, remove_list, link) {
		list_del(&map->link);

		nvmet_mdev_viommu_unpin_pages(viommu, map->iova_start,
					      map_pages(map));
		viommu_int_tree_remove(map, &viommu->maps_tree);
		kfree(map);
		count++;
	}
	return count;
}

/* User memory end code */
void nvmet_mdev_viommu_reset(struct nvmet_mdev_viommu *viommu)
{
	nvmet_mdev_viommu_remove_list(viommu, &viommu->mem_map_list);
	WARN_ON(!list_empty(&viommu->mem_map_list));
}

/* Adds a new range of user memory */
int nvmet_mdev_viommu_add(struct nvmet_mdev_viommu *viommu, u32 flags,
			  dma_addr_t iova, u64 size,
			  struct list_head *mem_map_list)
{
	struct vfio_device *vfio_dev = viommu->vfio_dev;
	struct nvmet_mdev_vctrl *vctrl = vfio_dev_to_nvmet_mdev_vctrl(vfio_dev);
	dma_addr_t iova_curr, iova_end, iova_start;
	struct mem_mapping *map = NULL, *tmp;
	LIST_HEAD(new_mappings_list);
	int ret, count = 0;

	/*
	 * iova/size may not be page aligned if this is for IO. We align
	 * them here because the viommu requires it.
	 */
	iova_start = iova;
	if (!PAGE_ALIGNED(iova_start)) {
		iova_start = PAGE_ALIGN_DOWN(iova_start);
		_DBG(vctrl, "vIOMMU: realign iova %pad -> %pad\n",
		     &iova, &iova_start);
	}

	iova_end = iova + size;
	if (!PAGE_ALIGNED(iova_end)) {
		iova_end = PAGE_ALIGN(iova_end);
		_DBG(vctrl,
		     "vIOMMU: realign size %llu -> %llu\n",
		     size, PAGE_ALIGN(size));
	}

	if (!(flags & VFIO_DMA_MAP_FLAG_READ) ||
	    !(flags & VFIO_DMA_MAP_FLAG_WRITE)) {
		const char *type = "none";

		if (flags & VFIO_DMA_MAP_FLAG_READ)
			type = "RO";
		else if (flags & VFIO_DMA_MAP_FLAG_WRITE)
			type = "WO";

		_DBG(viommu->vctrl, "vIOMMU: IGN %s IOVA %pad-%pad\n",
		     type, &iova_start, &iova_end);
		return 0;
	}

	/* VFIO pinning all the pages */
	for (iova_curr = iova_start; iova_curr < iova_end;
	     iova_curr += PAGE_SIZE) {
		struct page *page;

		ret = vfio_pin_pages(viommu->vfio_dev, iova_curr, 1,
				     VFIO_DMA_MAP_FLAG_READ |
				     VFIO_DMA_MAP_FLAG_WRITE,
				     &page);
		if (ret != 1) {
			_DBG(viommu->vctrl,
			     "vIOMMU: ADD RW IOVA %pad len: %llu - pin failed %d\n",
			     &iova, size, ret);
			goto unwind;
		}

		/* new mapping needed */
		if (!map || map->pfn + map_pages(map) != page_to_pfn(page)) {
			map = kzalloc(sizeof(*map), GFP_KERNEL);
			if (!map) {
				vfio_unpin_pages(viommu->vfio_dev, iova_curr,
						 1);
				ret = -ENOMEM;
				goto unwind;
			}
			map->iova_start = iova_curr;
			map->iova_last = iova_curr + PAGE_SIZE - 1ULL;
			map->pfn = page_to_pfn(page);
			map->refcount = 1;
			INIT_LIST_HEAD(&map->link);
			list_add_tail(&map->link, &new_mappings_list);
		} else {
			/* current map can be extended */
			map->iova_last += PAGE_SIZE;
		}
	}

	/* DMA mapping the pages */
	list_for_each_entry_safe(map, tmp, &new_mappings_list, link) {
		nvmet_mdev_viommu_dbg_dma_range(viommu, map, "ADD");
		list_move_tail(&map->link, mem_map_list);
		viommu_int_tree_insert(map, &viommu->maps_tree);
		count++;
	}

	_DBG(viommu->vctrl, "vIOMMU: ADD RW IOVA %pad-%pad len %llu count %d\n",
	     &iova_start, &iova_end, size, count);

	return 0;
unwind:
	list_for_each_entry_safe(map, tmp, &new_mappings_list, link) {
		nvmet_mdev_viommu_unpin_pages(viommu, map->iova_start,
					      map_pages(map));

		list_del(&map->link);
		kfree(map);
	}
	return ret;
}

/* Removes a range of user memory */
int nvmet_mdev_viommu_remove(struct nvmet_mdev_viommu *viommu, dma_addr_t iova,
			     u64 size)
{
	dma_addr_t last_iova = iova + (size) - 1ULL;
	struct mem_mapping *map;
	LIST_HEAD(remove_list);
	int count = 0;

	/* find out all the relevant ranges */
	map = viommu_int_tree_iter_first(&viommu->maps_tree, iova, last_iova);
	while (map) {
		list_move_tail(&map->link, &remove_list);
		map = viommu_int_tree_iter_next(map, iova, last_iova);
	}

	/* remove them */
	count = nvmet_mdev_viommu_remove_list(viommu, &remove_list);
	return count;
}

/* Translate an IOVA to a physical address and read device bus address */
int nvmet_mdev_viommu_translate(struct nvmet_mdev_viommu *viommu,
				dma_addr_t iova, dma_addr_t *physical)
{
	struct mem_mapping *mapping;
	u64 offset;

	if (WARN_ON_ONCE(offset_in_page(iova) != 0))
		return -EINVAL;

	mapping = viommu_int_tree_iter_first(&viommu->maps_tree,
					     iova, iova + PAGE_SIZE - 1);
	if (!mapping) {
		_DBG(viommu->vctrl,
		     "vIOMMU: translation of IOVA %pad failed\n", &iova);
		return -EFAULT;
	}

	WARN_ON(iova > mapping->iova_last);
	WARN_ON(offset_in_page(mapping->iova_start) != 0);

	offset = iova - mapping->iova_start;
	*physical = PFN_PHYS(mapping->pfn) + offset;
	return 0;
}

/* map an IOVA to kernel address space  */
int nvmet_mdev_viommu_create_kmap(struct nvmet_mdev_viommu *viommu,
				  dma_addr_t iova, struct page_map *page)
{
	phys_addr_t physical;
	struct page *new_page;
	int ret;

	page->iova = iova;

	ret = nvmet_mdev_viommu_translate(viommu, iova, &physical);
	if (ret)
		return ret;

	new_page = pfn_to_page(PHYS_PFN(physical));

	page->kmap = kmap_local_page(new_page);
	if (!page->kmap)
		return -ENOMEM;

	page->page = new_page;
	return 0;
}

/* update IOVA <-> kernel mapping. If fails, removes the previous mapping */
void nvmet_mdev_viommu_update_kmap(struct nvmet_mdev_viommu *viommu,
				   struct page_map *page)
{
	phys_addr_t physical;
	struct page *new_page;
	int ret;

	ret = nvmet_mdev_viommu_translate(viommu, page->iova, &physical);
	if (ret) {
		nvmet_mdev_viommu_free_kmap(viommu, page);
		return;
	}

	new_page = pfn_to_page(PHYS_PFN(physical));
	if (new_page == page->page)
		return;

	nvmet_mdev_viommu_free_kmap(viommu, page);

	page->kmap = kmap_local_page(new_page);
	if (!page->kmap)
		return;
	page->page = new_page;
}

/* unmap an IOVA to kernel address space  */
void nvmet_mdev_viommu_free_kmap(struct nvmet_mdev_viommu *viommu,
				 struct page_map *page)
{
	if (page->page) {
		kunmap_local(page->kmap);
		page->page = NULL;
		page->kmap = NULL;
	}
}
