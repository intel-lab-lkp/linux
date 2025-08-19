// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU-agnostic ARM page table allocator.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */
#define pr_fmt(fmt)	"arm-lpae io-pgtable: " fmt

#include <linux/device/faux.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "io-pgtable-arm.h"
#include "iommu-pages.h"

static dma_addr_t __arm_lpae_dma_addr(void *pages)
{
	return (dma_addr_t)virt_to_phys(pages);
}

void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
			     struct io_pgtable_cfg *cfg,
			     void *cookie)
{
	struct device *dev = cfg->iommu_dev;
	size_t alloc_size;
	dma_addr_t dma;
	void *pages;

	/*
	 * For very small starting-level translation tables the HW requires a
	 * minimum alignment of at least 64 to cover all cases.
	 */
	alloc_size = max(size, 64);
	if (cfg->alloc)
		pages = cfg->alloc(cookie, alloc_size, gfp);
	else
		pages = iommu_alloc_pages_node_sz(dev_to_node(dev), gfp,
						  alloc_size);

	if (!pages)
		return NULL;

	if (!cfg->coherent_walk) {
		dma = dma_map_single(dev, pages, size, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, dma))
			goto out_free;
		/*
		 * We depend on the IOMMU being able to work with any physical
		 * address directly, so if the DMA layer suggests otherwise by
		 * translating or truncating them, that bodes very badly...
		 */
		if (dma != virt_to_phys(pages))
			goto out_unmap;
	}

	return pages;

out_unmap:
	dev_err(dev, "Cannot accommodate DMA translation for IOMMU page tables\n");
	dma_unmap_single(dev, dma, size, DMA_TO_DEVICE);

out_free:
	if (cfg->free)
		cfg->free(cookie, pages, size);
	else
		iommu_free_pages(pages);

	return NULL;
}

void __arm_lpae_free_pages(void *pages, size_t size,
			   struct io_pgtable_cfg *cfg,
			   void *cookie)
{
	if (!cfg->coherent_walk)
		dma_unmap_single(cfg->iommu_dev, __arm_lpae_dma_addr(pages),
				 size, DMA_TO_DEVICE);

	if (cfg->free)
		cfg->free(cookie, pages, size);
	else
		iommu_free_pages(pages);
}

void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
			 struct io_pgtable_cfg *cfg)
{
	dma_sync_single_for_device(cfg->iommu_dev, __arm_lpae_dma_addr(ptep),
				   sizeof(*ptep) * num_entries, DMA_TO_DEVICE);
}

void *__arm_lpae_alloc_data(size_t size, gfp_t gfp)
{
	return kmalloc(size, gfp);
}

void __arm_lpae_free_data(void *p)
{
	return kfree(p);
}

#ifdef CONFIG_IOMMU_IO_PGTABLE_LPAE_SELFTEST

static struct io_pgtable_cfg *cfg_cookie __initdata;

static void __init dummy_tlb_flush_all(void *cookie)
{
	WARN_ON(cookie != cfg_cookie);
}

static void __init dummy_tlb_flush(unsigned long iova, size_t size,
				   size_t granule, void *cookie)
{
	WARN_ON(cookie != cfg_cookie);
	WARN_ON(!(size & cfg_cookie->pgsize_bitmap));
}

static void __init dummy_tlb_add_page(struct iommu_iotlb_gather *gather,
				      unsigned long iova, size_t granule,
				      void *cookie)
{
	dummy_tlb_flush(iova, granule, granule, cookie);
}

static const struct iommu_flush_ops dummy_tlb_ops __initconst = {
	.tlb_flush_all	= dummy_tlb_flush_all,
	.tlb_flush_walk	= dummy_tlb_flush,
	.tlb_add_page	= dummy_tlb_add_page,
};

static void __init arm_lpae_dump_ops(struct io_pgtable_ops *ops)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;

	pr_err("cfg: pgsize_bitmap 0x%lx, ias %u-bit\n",
		cfg->pgsize_bitmap, cfg->ias);
	pr_err("data: %d levels, 0x%zx pgd_size, %u pg_shift, %u bits_per_level, pgd @ %p\n",
		ARM_LPAE_MAX_LEVELS - data->start_level, ARM_LPAE_PGD_SIZE(data),
		ilog2(ARM_LPAE_GRANULE(data)), data->bits_per_level, data->pgd);
}

#define __FAIL(ops, i)	({						\
		WARN(1, "selftest: test failed for fmt idx %d\n", (i));	\
		arm_lpae_dump_ops(ops);					\
		-EFAULT;						\
})

static int __init arm_lpae_run_tests(struct io_pgtable_cfg *cfg)
{
	static const enum io_pgtable_fmt fmts[] __initconst = {
		ARM_64_LPAE_S1,
		ARM_64_LPAE_S2,
	};

	int i, j;
	unsigned long iova;
	size_t size, mapped;
	struct io_pgtable_ops *ops;

	for (i = 0; i < ARRAY_SIZE(fmts); ++i) {
		cfg_cookie = cfg;
		ops = alloc_io_pgtable_ops(fmts[i], cfg, cfg);
		if (!ops) {
			pr_err("selftest: failed to allocate io pgtable ops\n");
			return -ENOMEM;
		}

		/*
		 * Initial sanity checks.
		 * Empty page tables shouldn't provide any translations.
		 */
		if (ops->iova_to_phys(ops, 42))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, SZ_1G + 42))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, SZ_2G + 42))
			return __FAIL(ops, i);

		/*
		 * Distinct mappings of different granule sizes.
		 */
		iova = 0;
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {
			size = 1UL << j;

			if (ops->map_pages(ops, iova, iova, size, 1,
					   IOMMU_READ | IOMMU_WRITE |
					   IOMMU_NOEXEC | IOMMU_CACHE,
					   GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			/* Overlapping mappings */
			if (!ops->map_pages(ops, iova, iova + size, size, 1,
					    IOMMU_READ | IOMMU_NOEXEC,
					    GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
				return __FAIL(ops, i);

			iova += SZ_1G;
		}

		/* Full unmap */
		iova = 0;
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {
			size = 1UL << j;

			if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42))
				return __FAIL(ops, i);

			/* Remap full block */
			if (ops->map_pages(ops, iova, iova, size, 1,
					   IOMMU_WRITE, GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
				return __FAIL(ops, i);

			iova += SZ_1G;
		}

		/*
		 * Map/unmap the last largest supported page of the IAS, this can
		 * trigger corner cases in the concatednated page tables.
		 */
		mapped = 0;
		size = 1UL << __fls(cfg->pgsize_bitmap);
		iova = (1UL << cfg->ias) - size;
		if (ops->map_pages(ops, iova, iova, size, 1,
				   IOMMU_READ | IOMMU_WRITE |
				   IOMMU_NOEXEC | IOMMU_CACHE,
				   GFP_KERNEL, &mapped))
			return __FAIL(ops, i);
		if (mapped != size)
			return __FAIL(ops, i);
		if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
			return __FAIL(ops, i);

		free_io_pgtable_ops(ops);
	}

	return 0;
}

static int __init arm_lpae_do_selftests(void)
{
	static const unsigned long pgsize[] __initconst = {
		SZ_4K | SZ_2M | SZ_1G,
		SZ_16K | SZ_32M,
		SZ_64K | SZ_512M,
	};

	static const unsigned int address_size[] __initconst = {
		32, 36, 40, 42, 44, 48,
	};

	int i, j, k, pass = 0, fail = 0;
	struct faux_device *dev;
	struct io_pgtable_cfg cfg = {
		.tlb = &dummy_tlb_ops,
		.coherent_walk = true,
		.quirks = IO_PGTABLE_QUIRK_NO_WARN,
	};

	dev = faux_device_create("io-pgtable-test", NULL, 0);
	if (!dev)
		return -ENOMEM;

	cfg.iommu_dev = &dev->dev;

	for (i = 0; i < ARRAY_SIZE(pgsize); ++i) {
		for (j = 0; j < ARRAY_SIZE(address_size); ++j) {
			/* Don't use ias > oas as it is not valid for stage-2. */
			for (k = 0; k <= j; ++k) {
				cfg.pgsize_bitmap = pgsize[i];
				cfg.ias = address_size[k];
				cfg.oas = address_size[j];
				pr_info("selftest: pgsize_bitmap 0x%08lx, IAS %u OAS %u\n",
					pgsize[i], cfg.ias, cfg.oas);
				if (arm_lpae_run_tests(&cfg))
					fail++;
				else
					pass++;
			}
		}
	}

	pr_info("selftest: completed with %d PASS %d FAIL\n", pass, fail);
	faux_device_destroy(dev);

	return fail ? -EFAULT : 0;
}
subsys_initcall(arm_lpae_do_selftests);
#endif
