// SPDX-License-Identifier: GPL-2.0+
/*
 * User (guest) data access routines
 * Implementation of PRP iterator in user memory
 * Copyright (c) 2019 - Maxim Levitsky
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mdev.h>
#include <linux/nvme.h>
#include "priv.h"

#define MAX_PRP ((PAGE_SIZE / sizeof(__le64)) - 1)

/* Setup up a new PRP iterator */
void nvmet_mdev_udata_iter_setup(struct nvmet_mdev_viommu *viommu,
				 struct nvmet_ext_data_iter *iter,
				 struct list_head *mem_map_list)
{
	memset(iter, 0, sizeof(*iter));
	iter->viommu = viommu;
	iter->mem_map_list = mem_map_list;
}

/* Load a new prp list into the iterator. Internal */
static int nvmet_mdev_udata_iter_load_prplist(struct nvmet_ext_data_iter *iter,
					      dma_addr_t iova)
{
	dma_addr_t  data_iova;
	int ret;
	__le64 *map;

	ret = nvmet_mdev_viommu_add(iter->viommu,
				    VFIO_DMA_MAP_FLAG_READ |
				    VFIO_DMA_MAP_FLAG_WRITE, iova, PAGE_SIZE,
				    iter->mem_map_list);
	if (ret)
		return ret;

	/* map the prp list */
	ret = nvmet_mdev_viommu_create_kmap(iter->viommu, PAGE_ADDRESS(iova),
					    &iter->uprp.page);
	if (ret)
		return ret;

	iter->uprp.index = offset_in_page(iova) / (sizeof(__le64));

	/* read its first entry and check its alignment */
	map = iter->uprp.page.kmap;
	data_iova = le64_to_cpu(map[iter->uprp.index]);

	if (offset_in_page(data_iova) != 0) {
		nvmet_mdev_viommu_free_kmap(iter->viommu, &iter->uprp.page);
		return -EINVAL;
	}

	ret = nvmet_mdev_viommu_add(iter->viommu,
				    VFIO_DMA_MAP_FLAG_READ |
				    VFIO_DMA_MAP_FLAG_WRITE, data_iova,
				    PAGE_SIZE, iter->mem_map_list);
	if (ret)
		return ret;

	/* translate the entry to complete the setup */
	ret =  nvmet_mdev_viommu_translate(iter->viommu, data_iova,
					   &iter->physical);
	if (ret)
		nvmet_mdev_viommu_free_kmap(iter->viommu, &iter->uprp.page);

	return ret;
}

/* ->next function when iterator points to prp list */
static int nvmet_mdev_udata_iter_next_prplist(struct nvmet_ext_data_iter *iter)
{
	dma_addr_t iova;
	int ret;
	__le64 *map = iter->uprp.page.kmap;

	if (WARN_ON(iter->count <= 0))
		return 0;

	if (--iter->count == 0) {
		nvmet_mdev_viommu_free_kmap(iter->viommu, &iter->uprp.page);
		return 0;
	}

	iter->uprp.index++;

	if (iter->uprp.index < MAX_PRP || iter->count == 1) {
		/*
		 * advance over next pointer in current prp list these
		 * pointers must be page aligned
		 */
		iova = le64_to_cpu(map[iter->uprp.index]);
		if (offset_in_page(iova) != 0)
			return -EINVAL;

		ret = nvmet_mdev_viommu_add(iter->viommu,
					    VFIO_DMA_MAP_FLAG_READ |
					    VFIO_DMA_MAP_FLAG_WRITE, iova,
					    PAGE_SIZE, iter->mem_map_list);
		if (ret)
			return ret;

		ret  = nvmet_mdev_viommu_translate(iter->viommu, iova,
						   &iter->physical);
		if (ret)
			nvmet_mdev_viommu_free_kmap(iter->viommu,
						    &iter->uprp.page);
		return ret;
	}

	/* switch to next prp list. it must be page aligned as well */
	iova = le64_to_cpu(map[MAX_PRP]);

	if (offset_in_page(iova) != 0)
		return -EINVAL;

	nvmet_mdev_viommu_free_kmap(iter->viommu, &iter->uprp.page);
	return nvmet_mdev_udata_iter_load_prplist(iter, iova);
}

/* ->next function when iterator points to user data pointer */
static int nvmet_mdev_udata_iter_next_dptr(struct nvmet_ext_data_iter *iter)
{
	dma_addr_t  iova;
	int ret;

	if (WARN_ON(iter->count <= 0))
		return 0;

	if (--iter->count == 0)
		return 0;

	/*
	 * we will be called only once to deal with the second
	 * pointer in the data pointer
	 */
	iova = le64_to_cpu(iter->dptr->prp2);

	if (iter->count == 1) {
		/*
		 * only need to read one more entry, meaning the 2nd entry of
		 * the dptr. It must be page aligned
		 */
		if (offset_in_page(iova) != 0)
			return -EINVAL;

		/*
		 * Size may be less than a page but it doesn't matter to
		 * the viommu as we have to get the entire page either way.
		 */
		ret = nvmet_mdev_viommu_add(iter->viommu,
					   VFIO_DMA_MAP_FLAG_READ |
					   VFIO_DMA_MAP_FLAG_WRITE, iova,
					   PAGE_SIZE, iter->mem_map_list);
		if (ret)
			return ret;

		return nvmet_mdev_viommu_translate(iter->viommu, iova,
						   &iter->physical);
	} else {
		/*
		 * Second dptr entry is prp pointer, and it might not
		 * be page aligned (but QWORD aligned at least)
		 */
		if (iova & 0x7ULL)
			return -EINVAL;
		iter->next = nvmet_mdev_udata_iter_next_prplist;
		return nvmet_mdev_udata_iter_load_prplist(iter, iova);
	}
}

static void nvmet_mdev_udata_iter_release(struct nvmet_ext_data_iter *iter)
{
	nvmet_mdev_viommu_free_kmap(iter->viommu, &iter->uprp.page);
	nvmet_mdev_viommu_remove_list(iter->viommu, iter->mem_map_list);
}

/* Set prp list iterator to point to data pointer found in NVME command */
int nvmet_mdev_udata_iter_set_dptr(struct nvmet_ext_data_iter *iter,
				   const union nvme_data_ptr *dptr, u64 size)
{
	int ret;
	u64 prp1 = le64_to_cpu(dptr->prp1);
	dma_addr_t iova = PAGE_ADDRESS(prp1);
	unsigned int page_offset = offset_in_page(prp1);

	/* first dptr pointer must be at least DWORD aligned */
	if (page_offset & 0x3)
		return -EINVAL;

	ret = nvmet_mdev_viommu_add(iter->viommu,
				   VFIO_DMA_MAP_FLAG_READ |
				   VFIO_DMA_MAP_FLAG_WRITE, iova, size,
				   iter->mem_map_list);
	if (ret)
		return ret;

	iter->dptr = dptr;
	iter->next = nvmet_mdev_udata_iter_next_dptr;
	iter->release = nvmet_mdev_udata_iter_release;
	iter->count = DIV_ROUND_UP_ULL(size + page_offset, PAGE_SIZE);

	ret = nvmet_mdev_viommu_translate(iter->viommu, iova, &iter->physical);
	if (ret)
		goto release;

	iter->physical += page_offset;
	return 0;

release:
	nvmet_mdev_udata_iter_release(iter);
	return ret;
}

/* Map an SQ/CQ queue (contiguous in guest physical memory) */
static int
nvmet_mdev_queue_getpages_contiguous(struct nvmet_mdev_viommu *viommu,
				     dma_addr_t iova, struct page **pages,
				     unsigned int npages)
{
	dma_addr_t curr_iova = iova;
	phys_addr_t physical;
	unsigned int i;
	int ret;

	for (i = 0 ; i < npages; i++) {
		ret = nvmet_mdev_viommu_add(viommu,
					    VFIO_DMA_MAP_FLAG_READ |
					    VFIO_DMA_MAP_FLAG_WRITE,
					    curr_iova, PAGE_SIZE,
					    &viommu->mem_map_list);
		if (ret)
			goto remove;

		ret = nvmet_mdev_viommu_translate(viommu, curr_iova, &physical);
		if (ret)
			goto remove;

		pages[i] = pfn_to_page(PHYS_PFN(physical));
		curr_iova += PAGE_SIZE;
	}
	return 0;

remove:
	nvmet_mdev_viommu_remove(viommu, iova, npages * PAGE_SIZE);
	return ret;
}

/* map a SQ/CQ queue to host physical memory */
static void *nvmet_mdev_udata_queue_vmap(struct nvmet_mdev_viommu *viommu,
					 dma_addr_t iova, unsigned int size)
{
	unsigned int npages;
	struct page **pages;
	void *map = NULL;

	/* queue must be page aligned */
	if (offset_in_page(iova) != 0)
		return NULL;

	npages = DIV_ROUND_UP(size, PAGE_SIZE);
	pages = kcalloc(npages, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return NULL;

	if (nvmet_mdev_queue_getpages_contiguous(viommu, iova, pages, npages))
		goto out;

	map = vmap(pages, npages, VM_MAP, PAGE_KERNEL);
out:
	kfree(pages);
	return map;
}

void nvmet_mdev_udata_queue_vunmap(struct nvmet_mdev_viommu *viommu,
				   dma_addr_t iova, void *data,
				   unsigned int data_size)
{
	if (!data)
		return;

	vunmap(data);
	nvmet_mdev_viommu_remove(viommu, iova,
				 DIV_ROUND_UP(data_size, PAGE_SIZE));
}

void *nvmet_mdev_udata_update_queue_vmap(struct nvmet_mdev_viommu *viommu,
					 dma_addr_t iova, void *data,
					 unsigned int data_size)
{
	if (!iova)
		return NULL;

	if (data)
		vunmap(data);

	return nvmet_mdev_udata_queue_vmap(viommu, iova, data_size);
}
