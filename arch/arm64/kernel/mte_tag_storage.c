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
#include <linux/page-flags.h>
#include <linux/page_owner.h>
#include <linux/range.h>
#include <linux/sched/mm.h>
#include <linux/string.h>
#include <linux/vm_event_item.h>
#include <linux/xarray.h>

#include <asm/cacheflush.h>
#include <asm/mte_tag_storage.h>

__ro_after_init DEFINE_STATIC_KEY_FALSE(tag_storage_enabled_key);

struct tag_region {
	struct range mem_range;	/* Memory associated with the tag storage, in PFNs. */
	struct range tag_range;	/* Tag storage memory, in PFNs. */
	u32 block_size;		/* Tag block size, in pages. */
};

#define MAX_TAG_REGIONS	32

static struct tag_region tag_regions[MAX_TAG_REGIONS];
static int num_tag_regions;

/*
 * A note on locking. Reserving tag storage takes the tag_blocks_lock mutex,
 * because alloc_contig_range() might sleep.
 *
 * Freeing tag storage takes the xa_lock spinlock with interrupts disabled
 * because pages can be freed from non-preemptible contexts, including from an
 * interrupt handler.
 *
 * Because tag storage can be freed from interrupt contexts, the xarray is
 * defined with the XA_FLAGS_LOCK_IRQ flag to disable interrupts when calling
 * xa_store(). This is done to prevent a deadlock with free_tag_storage() being
 * called from an interrupt raised before xa_store() releases the xa_lock.
 *
 * All of the above means that reserve_tag_storage() cannot run concurrently
 * with itself (no concurrent insertions), but it can run at the same time as
 * free_tag_storage(). The first thing that reserve_tag_storage() does after
 * taking the mutex is increase the refcount on all present tag storage blocks
 * with the xa_lock held, to serialize against freeing the blocks. This is an
 * optimization to avoid taking and releasing the xa_lock after each iteration
 * if the refcount operation was moved inside the loop, where it would have had
 * to be executed for each block.
 */
static DEFINE_XARRAY_FLAGS(tag_blocks_reserved, XA_FLAGS_LOCK_IRQ);
static DEFINE_MUTEX(tag_blocks_lock);

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

/* alloc_contig_range() requires all pages to be in the same zone. */
static int __init mte_tag_storage_check_zone(void)
{
	struct range *tag_range;
	struct zone *zone;
	unsigned long pfn;
	u32 block_size;
	int i, j;

	for (i = 0; i < num_tag_regions; i++) {
		block_size = tag_regions[i].block_size;
		if (block_size == 1)
			continue;

		tag_range = &tag_regions[i].tag_range;
		for (pfn = tag_range->start; pfn <= tag_range->end; pfn += block_size) {
			zone = page_zone(pfn_to_page(pfn));
			for (j = 1; j < block_size; j++) {
				if (page_zone(pfn_to_page(pfn + j)) != zone) {
					pr_err("Tag storage block pages in different zones");
					return -EINVAL;
				}
			}
		}
	}

	 return 0;
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

	/*
	 * The kernel allocates memory in non-preemptible contexts, which makes
	 * migration impossible when reserving the associated tag storage.
	 *
	 * The check is safe to make because KASAN HW tags are enabled before
	 * the rest of the init functions are called, in smp_prepare_boot_cpu().
	 */
	if (kasan_hw_tags_enabled()) {
		pr_info("KASAN HW tags incompatible with MTE tag storage management");
		ret = 0;
		goto out_disabled;
	}

	ret = mte_tag_storage_check_zone();
	if (ret)
		goto out_disabled;

	for (i = 0; i < num_tag_regions; i++) {
		tag_range = &tag_regions[i].tag_range;
		for (pfn = tag_range->start; pfn <= tag_range->end; pfn += pageblock_nr_pages)
			init_cma_reserved_pageblock(pfn_to_page(pfn));
		totalcma_pages += range_len(tag_range);
	}

	reserve_tag_storage(ZERO_PAGE(0), 0, GFP_HIGHUSER_MOVABLE);

	return 0;

out_disabled:
	pr_info("MTE tag storage region management disabled");
	return ret;
}
arch_initcall(mte_tag_storage_activate_regions);

static void page_set_tag_storage_reserved(struct page *page, int order)
{
	int i;

	for (i = 0; i < (1 << order); i++)
		set_bit(PG_tag_storage_reserved, &(page + i)->flags);
}

static void block_ref_add(unsigned long block, struct tag_region *region, int order)
{
	int count;

	count = min(1u << order, 32 * region->block_size);
	page_ref_add(pfn_to_page(block), count);
}

static int block_ref_sub_return(unsigned long block, struct tag_region *region, int order)
{
	int count;

	count = min(1u << order, 32 * region->block_size);
	return page_ref_sub_return(pfn_to_page(block), count);
}

static bool tag_storage_block_is_reserved(unsigned long block)
{
	return xa_load(&tag_blocks_reserved, block) != NULL;
}

static int tag_storage_reserve_block(unsigned long block, struct tag_region *region, int order)
{
	unsigned long block_va;
	int ret;

	block_va = (unsigned long)page_to_virt(pfn_to_page(block));
	/* Avoid writeback of dirty data cache lines corrupting tags. */
	dcache_inval_poc(block_va, block_va + region->block_size * PAGE_SIZE);

	ret = xa_err(xa_store(&tag_blocks_reserved, block, pfn_to_page(block), GFP_KERNEL));
	if (!ret)
		block_ref_add(block, region, order);

	return ret;
}

static int order_to_num_blocks(int order)
{
	return max((1 << order) / 32, 1);
}

static int tag_storage_find_block_in_region(struct page *page, unsigned long *blockp,
					    struct tag_region *region)
{
	struct range *tag_range = &region->tag_range;
	struct range *mem_range = &region->mem_range;
	u64 page_pfn = page_to_pfn(page);
	u64 block, block_offset;

	if (!(mem_range->start <= page_pfn && page_pfn <= mem_range->end))
		return -ERANGE;

	block_offset = (page_pfn - mem_range->start) / 32;
	block = tag_range->start + rounddown(block_offset, region->block_size);

	if (block + region->block_size - 1 > tag_range->end) {
		pr_err("Block 0x%llx-0x%llx is outside tag region 0x%llx-0x%llx\n",
			PFN_PHYS(block), PFN_PHYS(block + region->block_size),
			PFN_PHYS(tag_range->start), PFN_PHYS(tag_range->end));
		return -ERANGE;
	}
	*blockp = block;

	return 0;

}

static int tag_storage_find_block(struct page *page, unsigned long *block,
				  struct tag_region **region)
{
	int i, ret;

	for (i = 0; i < num_tag_regions; i++) {
		ret = tag_storage_find_block_in_region(page, block, &tag_regions[i]);
		if (ret == 0) {
			*region = &tag_regions[i];
			return 0;
		}
	}

	return -EINVAL;
}

bool page_tag_storage_reserved(struct page *page)
{
	return test_bit(PG_tag_storage_reserved, &page->flags);
}

bool page_is_tag_storage(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct range *tag_range;
	int i;

	for (i = 0; i < num_tag_regions; i++) {
		tag_range = &tag_regions[i].tag_range;
		if (tag_range->start <= pfn && pfn <= tag_range->end)
			return true;
	}

	return false;
}

int reserve_tag_storage(struct page *page, int order, gfp_t gfp)
{
	unsigned long start_block, end_block;
	struct tag_region *region;
	unsigned long block;
	unsigned long flags;
	unsigned int tries;
	int ret = 0;

	VM_WARN_ON_ONCE(!preemptible());

	if (page_tag_storage_reserved(page))
		return 0;

	/*
	 * __alloc_contig_migrate_range() ignores gfp when allocating the
	 * destination page for migration. Regardless, massage gfp flags and
	 * remove __GFP_TAGGED to avoid recursion in case gfp stops being
	 * ignored.
	 */
	gfp &= ~__GFP_TAGGED;
	if (!(gfp & __GFP_NORETRY))
		gfp |= __GFP_RETRY_MAYFAIL;

	ret = tag_storage_find_block(page, &start_block, &region);
	if (WARN_ONCE(ret, "Missing tag storage block for pfn 0x%lx", page_to_pfn(page)))
		return 0;
	end_block = start_block + order_to_num_blocks(order) * region->block_size;

	mutex_lock(&tag_blocks_lock);

	/* Check again, this time with the lock held. */
	if (page_tag_storage_reserved(page)) {
		mutex_unlock(&tag_blocks_lock);
		return 0;
	}

	/* Make sure existing entries are not freed from out under out feet. */
	xa_lock_irqsave(&tag_blocks_reserved, flags);
	for (block = start_block; block < end_block; block += region->block_size) {
		if (tag_storage_block_is_reserved(block))
			block_ref_add(block, region, order);
	}
	xa_unlock_irqrestore(&tag_blocks_reserved, flags);

	for (block = start_block; block < end_block; block += region->block_size) {
		/* Refcount incremented above. */
		if (tag_storage_block_is_reserved(block))
			continue;

		tries = 3;
		while (tries--) {
			ret = alloc_contig_range(block, block + region->block_size, MIGRATE_CMA, gfp);
			if (ret == 0 || ret != -EBUSY)
				break;
		}

		/*
		 * alloc_contig_range() returns -EINTR from
		 * __alloc_contig_migrate_range() if a fatal signal is pending.
		 * As long as the signal hasn't been handled, it is impossible
		 * to reserve tag storage for any page. Stop trying to reserve
		 * tag storage, but return 0 so the page allocator can make
		 * forward progress, instead of printing an OOM splat.
		 *
		 * The tagged page with missing tag storage will be mapped with
		 * PAGE_FAULT_ON_ACCESS in set_pte_at(), which means accesses
		 * until the signal is delivered will cause a fault.
		 */
		if (ret == -EINTR) {
			ret = 0;
			goto out_error;
		}

		if (ret)
			goto out_error;

		ret = tag_storage_reserve_block(block, region, order);
		if (ret) {
			free_contig_range(block, region->block_size);
			goto out_error;
		}

		count_vm_events(CMA_ALLOC_SUCCESS, region->block_size);
	}

	page_set_tag_storage_reserved(page, order);
	mutex_unlock(&tag_blocks_lock);

	mte_restore_tags_for_pfn(page_to_pfn(page), order);

	return 0;

out_error:
	xa_lock_irqsave(&tag_blocks_reserved, flags);
	for (block = start_block; block < end_block; block += region->block_size) {
		if (tag_storage_block_is_reserved(block) &&
		    block_ref_sub_return(block, region, order) == 1) {
			__xa_erase(&tag_blocks_reserved, block);
			free_contig_range(block, region->block_size);
		}
	}
	xa_unlock_irqrestore(&tag_blocks_reserved, flags);

	mutex_unlock(&tag_blocks_lock);

	count_vm_events(CMA_ALLOC_FAIL, region->block_size);

	return ret;
}

void free_tag_storage(struct page *page, int order)
{
	unsigned long block, start_block, end_block;
	struct tag_region *region;
	unsigned long page_va;
	unsigned long flags;
	void *tags;
	int i, ret;

	ret = tag_storage_find_block(page, &start_block, &region);
	if (WARN_ONCE(ret, "Missing tag storage block for pfn 0x%lx", page_to_pfn(page)))
		return;

	page_va = (unsigned long)page_to_virt(page);
	/* Avoid writeback of dirty tag cache lines corrupting data. */
	dcache_inval_tags_poc(page_va, page_va + (PAGE_SIZE << order));

	tags_by_pfn_lock();
	for (i = 0; i < (1 << order); i++) {
		tags = mte_erase_tags_for_pfn(page_to_pfn(page + i));
		if (unlikely(tags))
			mte_free_tag_buf(tags);
	}
	tags_by_pfn_unlock();

	end_block = start_block + order_to_num_blocks(order) * region->block_size;

	xa_lock_irqsave(&tag_blocks_reserved, flags);
	for (block = start_block; block < end_block; block += region->block_size) {
		if (WARN_ONCE(!tag_storage_block_is_reserved(block),
		    "Block 0x%lx is not reserved for pfn 0x%lx", block, page_to_pfn(page)))
			continue;

		if (block_ref_sub_return(block, region, order) == 1) {
			__xa_erase(&tag_blocks_reserved, block);
			free_contig_range(block, region->block_size);
		}
	}
	xa_unlock_irqrestore(&tag_blocks_reserved, flags);
}
