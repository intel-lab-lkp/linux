// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support for dynamic tag storage.
 *
 * Copyright (C) 2023 ARM Ltd.
 */

#include <linux/cma.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/of_device.h>
#include <linux/of_fdt.h>
#include <linux/pageblock-flags.h>
#include <linux/range.h>
#include <linux/string.h>
#include <linux/xarray.h>

#include <asm/mte_tag_storage.h>

struct tag_region {
	struct range mem_range;	/* Memory associated with the tag storage, in PFNs. */
	struct range tag_range;	/* Tag storage memory, in PFNs. */
	u32 block_size;		/* Tag block size, in pages. */
};

#define MAX_TAG_REGIONS	32

static struct tag_region tag_regions[MAX_TAG_REGIONS];
static int num_tag_regions;

static int __init tag_storage_of_flat_get_range(unsigned long node, const __be32 *reg,
						int reg_len, struct range *range)
{
	int addr_cells = dt_root_addr_cells;
	int size_cells = dt_root_size_cells;
	u64 size;

	if (reg_len / 4 > addr_cells + size_cells)
		return -EINVAL;

	range->start = PHYS_PFN(of_read_number(reg, addr_cells));
	size = PHYS_PFN(of_read_number(reg + addr_cells, size_cells));
	if (size == 0) {
		pr_err("Invalid node");
		return -EINVAL;
	}
	range->end = range->start + size - 1;

	return 0;
}

static int __init tag_storage_of_flat_get_tag_range(unsigned long node,
						    struct range *tag_range)
{
	const __be32 *reg;
	int reg_len;

	reg = of_get_flat_dt_prop(node, "reg", &reg_len);
	if (reg == NULL) {
		pr_err("Invalid metadata node");
		return -EINVAL;
	}

	return tag_storage_of_flat_get_range(node, reg, reg_len, tag_range);
}

static int __init tag_storage_of_flat_get_memory_range(unsigned long node, struct range *mem)
{
	const __be32 *reg;
	int reg_len;

	reg = of_get_flat_dt_prop(node, "linux,usable-memory", &reg_len);
	if (reg == NULL)
		reg = of_get_flat_dt_prop(node, "reg", &reg_len);

	if (reg == NULL) {
		pr_err("Invalid memory node");
		return -EINVAL;
	}

	return tag_storage_of_flat_get_range(node, reg, reg_len, mem);
}

struct find_memory_node_arg {
	unsigned long node;
	u32 phandle;
};

static int __init fdt_find_memory_node(unsigned long node, const char *uname,
				       int depth, void *data)
{
	const char *type = of_get_flat_dt_prop(node, "device_type", NULL);
	struct find_memory_node_arg *arg = data;

	if (depth != 1 || !type || strcmp(type, "memory") != 0)
		return 0;

	if (of_get_flat_dt_phandle(node) == arg->phandle) {
		arg->node = node;
		return 1;
	}

	return 0;
}

static int __init tag_storage_get_memory_node(unsigned long tag_node, unsigned long *mem_node)
{
	struct find_memory_node_arg arg = { 0 };
	const __be32 *memory_prop;
	u32 mem_phandle;
	int ret, reg_len;

	memory_prop = of_get_flat_dt_prop(tag_node, "memory", &reg_len);
	if (!memory_prop) {
		pr_err("Missing 'memory' property in the tag storage node");
		return -EINVAL;
	}

	mem_phandle = be32_to_cpup(memory_prop);
	arg.phandle = mem_phandle;

	ret = of_scan_flat_dt(fdt_find_memory_node, &arg);
	if (ret != 1) {
		pr_err("Associated memory node not found");
		return -EINVAL;
	}

	*mem_node = arg.node;

	return 0;
}

static int __init tag_storage_of_flat_read_u32(unsigned long node, const char *propname,
					       u32 *retval)
{
	const __be32 *reg;

	reg = of_get_flat_dt_prop(node, propname, NULL);
	if (!reg)
		return -EINVAL;

	*retval = be32_to_cpup(reg);
	return 0;
}

static u32 __init get_block_size_pages(u32 block_size_bytes)
{
	u32 a = PAGE_SIZE;
	u32 b = block_size_bytes;
	u32 r;

	/* Find greatest common divisor using the Euclidian algorithm. */
	do {
		r = a % b;
		a = b;
		b = r;
	} while (b != 0);

	return PHYS_PFN(PAGE_SIZE * block_size_bytes / a);
}

static int __init fdt_init_tag_storage(unsigned long node, const char *uname,
				       int depth, void *data)
{
	struct tag_region *region;
	unsigned long mem_node;
	struct range *mem_range;
	struct range *tag_range;
	u32 block_size_bytes;
	u32 nid = 0;
	int ret;

	if (depth != 1 || !strstr(uname, "tag-storage"))
		return 0;

	if (!of_flat_dt_is_compatible(node, "arm,mte-tag-storage"))
		return 0;

	if (num_tag_regions == MAX_TAG_REGIONS) {
		pr_err("Maximum number of tag storage regions exceeded");
		return -EINVAL;
	}

	region = &tag_regions[num_tag_regions];
	mem_range = &region->mem_range;
	tag_range = &region->tag_range;

	ret = tag_storage_of_flat_get_tag_range(node, tag_range);
	if (ret) {
		pr_err("Invalid tag storage node");
		return ret;
	}

	/* Pages are managed in pageblock_nr_pages chunks */
	if (!IS_ALIGNED(tag_range->start | range_len(tag_range), pageblock_nr_pages)) {
		pr_err("Tag storage region 0x%llx-0x%llx not aligned to pageblock size 0x%llx",
		       PFN_PHYS(tag_range->start), PFN_PHYS(tag_range->end),
		       PFN_PHYS(pageblock_nr_pages));
		return -EINVAL;
	}

	ret = tag_storage_get_memory_node(node, &mem_node);
	if (ret)
		return ret;

	ret = tag_storage_of_flat_get_memory_range(mem_node, mem_range);
	if (ret) {
		pr_err("Invalid address for associated data memory node");
		return ret;
	}

	/* The tag region must exactly match the corresponding memory. */
	if (range_len(tag_range) * 32 != range_len(mem_range)) {
		pr_err("Tag storage region 0x%llx-0x%llx does not cover the memory region 0x%llx-0x%llx",
		       PFN_PHYS(tag_range->start), PFN_PHYS(tag_range->end),
		       PFN_PHYS(mem_range->start), PFN_PHYS(mem_range->end));
		return -EINVAL;
	}

	ret = tag_storage_of_flat_read_u32(node, "block-size", &block_size_bytes);
	if (ret || block_size_bytes == 0) {
		pr_err("Invalid or missing 'block-size' property");
		return -EINVAL;
	}
	region->block_size = get_block_size_pages(block_size_bytes);
	if (range_len(tag_range) % region->block_size != 0) {
		pr_err("Tag storage region size 0x%llx is not a multiple of block size %u",
		       PFN_PHYS(range_len(tag_range)), region->block_size);
		return -EINVAL;
	}

	ret = tag_storage_of_flat_read_u32(mem_node, "numa-node-id", &nid);
	if (ret)
		nid = numa_node_id();

	ret = memblock_add_node(PFN_PHYS(tag_range->start), PFN_PHYS(range_len(tag_range)),
				nid, MEMBLOCK_NONE);
	if (ret) {
		pr_err("Error adding tag memblock (%d)", ret);
		return ret;
	}
	memblock_reserve(PFN_PHYS(tag_range->start), PFN_PHYS(range_len(tag_range)));

	pr_info("Found tag storage region 0x%llx-0x%llx, block size %u pages",
		PFN_PHYS(tag_range->start), PFN_PHYS(tag_range->end), region->block_size);

	num_tag_regions++;

	return 0;
}

void __init mte_tag_storage_init(void)
{
	struct range *tag_range;
	int i, ret;

	ret = of_scan_flat_dt(fdt_init_tag_storage, NULL);
	if (ret) {
		for (i = 0; i < num_tag_regions; i++) {
			tag_range = &tag_regions[i].tag_range;
			memblock_remove(PFN_PHYS(tag_range->start), PFN_PHYS(range_len(tag_range)));
		}
		num_tag_regions = 0;
		pr_info("MTE tag storage region management disabled");
	}
}

static int __init mte_tag_storage_activate_regions(void)
{
	phys_addr_t dram_start, dram_end;
	struct range *tag_range;
	unsigned long pfn;
	int i, ret;

	if (num_tag_regions == 0)
		return 0;

	dram_start = memblock_start_of_DRAM();
	dram_end = memblock_end_of_DRAM();

	for (i = 0; i < num_tag_regions; i++) {
		tag_range = &tag_regions[i].tag_range;
		/*
		 * Tag storage region was clipped by arm64_bootmem_init()
		 * enforcing addressing limits.
		 */
		if (PFN_PHYS(tag_range->start) < dram_start ||
				PFN_PHYS(tag_range->end) >= dram_end) {
			pr_err("Tag storage region 0x%llx-0x%llx outside addressable memory",
			       PFN_PHYS(tag_range->start), PFN_PHYS(tag_range->end));
			ret = -EINVAL;
			goto out_disabled;
		}
	}

	/*
	 * MTE disabled, tag storage pages can be used like any other pages. The
	 * only restriction is that the pages cannot be used by kexec because
	 * the memory remains marked as reserved in the memblock allocator.
	 */
	if (!system_supports_mte()) {
		for (i = 0; i< num_tag_regions; i++) {
			tag_range = &tag_regions[i].tag_range;
			for (pfn = tag_range->start; pfn <= tag_range->end; pfn++)
				free_reserved_page(pfn_to_page(pfn));
		}
		ret = 0;
		goto out_disabled;
	}

	for (i = 0; i < num_tag_regions; i++) {
		tag_range = &tag_regions[i].tag_range;
		for (pfn = tag_range->start; pfn <= tag_range->end; pfn += pageblock_nr_pages)
			init_cma_reserved_pageblock(pfn_to_page(pfn));
		totalcma_pages += range_len(tag_range);
	}

	return 0;

out_disabled:
	pr_info("MTE tag storage region management disabled");
	return ret;
}
arch_initcall(mte_tag_storage_activate_regions);
