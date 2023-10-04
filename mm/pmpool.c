// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "pmpool: " fmt

#include <linux/bitmap.h>
#include <linux/cma.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kexec.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pmpool.h>

#include "cma.h"

struct pmpool {
	struct resource resource;
	struct cma *cma;
};

static struct pmpool *default_pmpool;

bool pmpool_release(struct page *pages, unsigned long count)
{
	if (!default_pmpool)
		return false;

	return cma_release(default_pmpool->cma, pages, count);
}

struct page *pmpool_alloc(unsigned long count)
{
	if (!default_pmpool)
		return NULL;

	return cma_alloc(default_pmpool->cma, count, 0, true);
}

static void pmpool_cma_accomodate_bitmap(struct cma *cma)
{
	unsigned long bitmap_size;

	bitmap_free(cma->bitmap);
	cma->bitmap = phys_to_virt(PFN_PHYS(cma->base_pfn));

	bitmap_size = BITS_TO_LONGS(cma_bitmap_maxno(cma));
	memset(cma->bitmap, 0, bitmap_size);
	bitmap_set(cma->bitmap, 0, PAGE_ALIGN(bitmap_size) >> PAGE_SHIFT);

	pr_info("CMA bitmap moved to %#llx\n", virt_to_phys(cma->bitmap));
}

static int __init default_pmpool_fixup(void)
{
	if (!default_pmpool)
		return 0;

	if (insert_resource(&iomem_resource, &default_pmpool->resource))
		pr_err("failed to insert resource\n");

	pmpool_cma_accomodate_bitmap(default_pmpool->cma);
	return 0;
}
postcore_initcall(default_pmpool_fixup);

static int __init parse_pmpool_opt(char *str)
{
	static struct pmpool pmpool = {
		.resource = {
			.name  = "Persistent Memory Pool",
			.flags = IORESOURCE_BUSY | IORESOURCE_SYSTEM_RAM,
			.desc  = IORES_DESC_CXL
		}
	};
	phys_addr_t base, size, end;
	int err;

	/* Format is pmpool=<base>,<size> */
	base = memparse(str, &str);
	size = memparse(str + 1, NULL);
	end = base + size - 1;

	err = memblock_is_region_reserved(base, size);
	if (err) {
		pr_err("memory block overlaps with another one: %d\n", err);
		return 0;
	}

	err = memblock_reserve(base, size);
	if (err) {
		pr_err("failed to reserve memory block: %d\n", err);
		return 0;
	}

	err = cma_init_reserved_mem(base, size, 0, "pmpool", &pmpool.cma);
	if (err) {
		pr_err("failed to initialize CMA: %d\n", err);
		goto free_memblock;
	}

	pmpool.resource.start = base;
	pmpool.resource.end = end;

	pr_info("default memory pool is created: %#llx-%#llx\n",
		base, end);

	default_pmpool = &pmpool;

	return 0;

free_memblock:
	memblock_phys_free(base, size);
	return 0;
}
early_param("pmpool", parse_pmpool_opt);
