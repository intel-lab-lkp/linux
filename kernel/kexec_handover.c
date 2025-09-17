// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_handover.c - kexec handover metadata processing
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Google LLC, Changyuan Lyu <changyuanl@google.com>
 */

#define pr_fmt(fmt) "KHO: " fmt

#include <linux/cma.h>
#include <linux/count_zeros.h>
#include <linux/debugfs.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/notifier.h>
#include <linux/page-isolation.h>

#include <asm/early_ioremap.h>

/*
 * KHO is tightly coupled with mm init and needs access to some of mm
 * internal APIs.
 */
#include "../mm/internal.h"
#include "kexec_internal.h"

#define KHO_FDT_COMPATIBLE "kho-v1"
#define PROP_PRESERVED_ORDER_TABLE "preserved-order-table"
#define PROP_SUB_FDT "fdt"

static bool kho_enable __ro_after_init;

bool kho_is_enabled(void)
{
	return kho_enable;
}
EXPORT_SYMBOL_GPL(kho_is_enabled);

static int __init kho_parse_enable(char *p)
{
	return kstrtobool(p, &kho_enable);
}
early_param("kho", kho_parse_enable);

/*
 * KHO page tables provide a page-table-like data structure for tracking
 * preserved memory pages. It is a hierarchical structure that starts with a
 * `struct kho_order_table`. Each entry in this table points to the root of a
 * `struct kho_page_table` tree, which tracks the preserved memory pages for a
 * specific page order.
 *
 * Each entry in a `struct kho_page_table` points to the next level page table,
 * until level 2, which points to a `struct kho_bitmap_table`. The lowest level
 * (level 1) is a bitmap table where each bit represents a preserved page.
 *
 * The table hierarchy is shown as below.
 *
 * kho_order_table
 * +-------------------------------+--------------------+
 * | 0 order| 1 order| 2 order ... | HUGETLB_PAGE_ORDER |
 * ++------------------------------+--------------------+
 *  |
 *  |
 *  v
 * ++------+
 * |  Lv6  | kho_page_table
 * ++------+
 *  |
 *  |
 *  |   +-------+
 *  +-> |  Lv5  | kho_page_table
 *      ++------+
 *       |
 *       |
 *       |   +-------+
 *       +-> |  Lv4  | kho_page_table
 *           ++------+
 *            |
 *            |
 *            |   +-------+
 *            +-> |  Lv3  | kho_page_table
 *                ++------+
 *                 |
 *                 |
 *                 |  +-------+
 *                 +> |  Lv2  | kho_page_table
 *                    ++------+
 *                     |
 *                     |
 *                     |   +-------+
 *                     +-> |  Lv1  | kho_bitmap_table
 *                         +-------+
 *
 * The depth of the KHO page tables depends on the system's page size and the
 * page order. Both larger page sizes and higher page orders result in
 * shallower KHO page tables. For example, on a system with a 4KB native
 * page size, 0-order tables have a depth of 6 levels.
 *
 * The following diagram illustrates how a physical address is split into
 * indices for the different KHO page table levels and the final bitmap.
 *
 *      63      62:54    53:45    44:36    35:27        26:0
 * +--------+--------+--------+--------+--------+-----------------+
 * |  Lv 6  |  Lv 5  |  Lv 4  |  Lv 3  |  Lv 2  |  Lv 1 (bitmap)  |
 * +--------+--------+--------+--------+--------+-----------------+
 *
 * For higher order pages, the bit fields for each level shift to the left by
 * the page order.
 *
 * Each KHO page table and bitmap table is PAGE_SIZE in size. For 0-order
 * pages, the bitmap table contains (PAGE_SIZE * 8) bits, covering a
 * (PAGE_SIZE * 8 * PAGE_SIZE) memory range. For example, on a system with a
 * 4KB native page size, the bitmap table contains 32768 bits and covers a
 * 128MB memory range.
 *
 * Each KHO page table contains (PAGE_SIZE / 8) entries, where each entry is a
 * descriptor (a physical address) pointing to the next level table.
 * For example, with a 4KB page size, each page table holds 512 entries.
 * The level 2 KHO page table is an exception, where each entry points to a
 * KHO bitmap table instead.
 *
 * An entry of a KHO page table of a 4KB page system is shown as below as an
 * example.
 *
 *         63:12                       11:0
 * +------------------------------+--------------+
 * | descriptor to next table     |    zeros     |
 * +------------------------------+--------------+
 */

#define BITMAP_TABLE_SHIFT(_order) (PAGE_SHIFT + PAGE_SHIFT + 3 + (_order))
#define BITMAP_TABLE_MASK(_order) ((1ULL << BITMAP_TABLE_SHIFT(_order)) - 1)
#define PRESERVED_PAGE_OFFSET_SHIFT(_order) (PAGE_SHIFT + (_order))
#define PAGE_TABLE_SHIFT_PER_LEVEL (ilog2(PAGE_SIZE / sizeof(unsigned long)))
#define PAGE_TABLE_LEVEL_MASK ((1ULL << PAGE_TABLE_SHIFT_PER_LEVEL) - 1)
#define PTR_PER_LEVEL (PAGE_SIZE / sizeof(unsigned long))

typedef int (*kho_walk_callback_t)(phys_addr_t pa, int order);

struct kho_bitmap_table {
	unsigned long bitmaps[PAGE_SIZE / sizeof(unsigned long)];
};

struct kho_page_table {
	unsigned long tables[PTR_PER_LEVEL];
};

struct kho_order_table {
	unsigned long orders[HUGETLB_PAGE_ORDER + 1];
};

/*
 * `kho_order_table` points to a page that serves as the root of the KHO page
 * table hierarchy. This page is allocated during KHO module initialization.
 * Its physical address is written to the FDT and passed to the next kernel
 * during kexec.
 */
static struct kho_order_table *kho_order_table;

static unsigned long kho_page_table_level_shift(int level, int order)
{
	/*
	 * Calculate the cumulative bit shift required to extract the page table
	 * index for a given physical address at a specific `level` and `order`.
	 *
	 * - Level 1 is the bitmap table, which has its own indexing logic, so
	 *   the shift is 0.
	 * - Level 2 and above: The base shift is `BITMAP_TABLE_SHIFT(order)`,
	 *   which corresponds to the entire address space covered by a single
	 *   level 1 bitmap table.
	 * - Each subsequent level adds `PAGE_TABLE_SHIFT_PER_LEVEL` to the
	 *   total shift amount.
	 */
	return level <= 1 ? 0 :
		BITMAP_TABLE_SHIFT(order) + PAGE_TABLE_SHIFT_PER_LEVEL * (level - 2);
}

static int kho_get_bitmap_table_index(unsigned long pa, int order)
{
	/* 4KB (12bits of addr) + 8B per entries (6bits of addr) + order bits */
	unsigned long idx = pa >> (PAGE_SHIFT + 6 + order);

	return idx;
}

static int kho_get_page_table_index(unsigned long pa, int order, int level)
{
	unsigned long high_addr;
	unsigned long page_table_offset;
	unsigned long shift;

	if (level == 1)
		return kho_get_bitmap_table_index(pa, order);

	shift = kho_page_table_level_shift(level, order);
	high_addr = pa >> shift;

	page_table_offset = high_addr & PAGE_TABLE_LEVEL_MASK;
	return page_table_offset;
}

static int kho_table_level(int order)
{
	unsigned long bits_to_resolve;
	int page_table_num;

	/* We just need 1 bitmap table to cover all addresses */
	if (BITMAP_TABLE_SHIFT(order) >= 64)
		return 1;

	bits_to_resolve = 64 - BITMAP_TABLE_SHIFT(order);

	/*
	 * The level we need is the bits to resolve over the bits a page tabel
	 * can resolve. Get the ceiling as ceil(a/b) = (a + b - 1) / b.
	 * Total level is the all table levels plus the buttom
	 * bitmap level.
	 */
	page_table_num = (bits_to_resolve + PAGE_TABLE_SHIFT_PER_LEVEL - 1)
		/ PAGE_TABLE_SHIFT_PER_LEVEL;
	return page_table_num + 1;
}

static struct kho_page_table *kho_alloc_page_table(void)
{
	return (struct kho_page_table *)get_zeroed_page(GFP_KERNEL);
}

static void kho_set_preserved_page_bit(struct kho_bitmap_table *bitmap_table,
				       unsigned long pa, int order)
{
	int bitmap_table_index = kho_get_bitmap_table_index(pa, order);
	int offset;

	/* Get the bit offset in a 64bits bitmap entry */
	offset = (pa >> PRESERVED_PAGE_OFFSET_SHIFT(order)) & 0x3f;

	set_bit(offset,
		(unsigned long *)&bitmap_table->bitmaps[bitmap_table_index]);
}

static unsigned long kho_pgt_desc(struct kho_page_table *va)
{
	return (unsigned long)virt_to_phys(va);
}

static struct kho_page_table *kho_page_table(unsigned long desc)
{
	return (struct kho_page_table *)phys_to_virt(desc);
}

static int __kho_preserve_page_table(unsigned long pa, int order)
{
	int num_table_level = kho_table_level(order);
	struct kho_page_table *cur;
	struct kho_page_table *next;
	struct kho_bitmap_table *bitmap_table;
	int i, page_table_index;
	unsigned long page_table_desc;

	if (!kho_order_table->orders[order]) {
		cur = kho_alloc_page_table();
		if (!cur)
			return -ENOMEM;
		page_table_desc = kho_pgt_desc(cur);
		kho_order_table->orders[order] = page_table_desc;
	}

	cur = kho_page_table(kho_order_table->orders[order]);

	/* Go from high level tables to low level tables */
	for (i = num_table_level; i > 1; i--) {
		page_table_index = kho_get_page_table_index(pa, order, i);

		if (!cur->tables[page_table_index]) {
			next = kho_alloc_page_table();
			if (!next)
				return -ENOMEM;
			cur->tables[page_table_index] = kho_pgt_desc(next);
		} else {
			next = kho_page_table(cur->tables[page_table_index]);
		}

		cur = next;
	}

	/* Cur is now pointing to the level 1 bitmap table */
	bitmap_table = (struct kho_bitmap_table *)cur;
	kho_set_preserved_page_bit(bitmap_table,
				   pa & BITMAP_TABLE_MASK(order),
				   order);

	return 0;
}

static int kho_preserve_page_table(unsigned long pfn, int order)
{
	unsigned long pa = PFN_PHYS(pfn);

	might_sleep();

	return __kho_preserve_page_table(pa, order);
}

static int __kho_walk_bitmap_table(int order,
				   struct kho_bitmap_table *bitmap_table,
				   unsigned long pa,
				   kho_walk_callback_t cb)
{
	int i;
	unsigned long offset;
	int ret = 0;
	int order_factor = 1 << order;
	unsigned long *bitmap = (unsigned long *)bitmap_table;

	for_each_set_bit(i, bitmap, PAGE_SIZE * BITS_PER_BYTE) {
		offset = (unsigned long)PAGE_SIZE * order_factor * i;
		ret = cb(offset + pa, order);
		if (ret)
			return ret;
	}

	return 0;
}

static int __kho_walk_page_tables(int order, int level,
				  struct kho_page_table *cur, unsigned long pa,
				  kho_walk_callback_t cb)
{
	struct kho_page_table *next;
	struct kho_bitmap_table *bitmap_table;
	int i;
	unsigned long offset;
	int ret = 0;

	if (level == 1) {
		bitmap_table = (struct kho_bitmap_table *)cur;
		return __kho_walk_bitmap_table(order, bitmap_table, pa, cb);
	}

	for (i = 0; i < PTR_PER_LEVEL; i++) {
		if (cur->tables[i]) {
			next = kho_page_table(cur->tables[i]);
			offset = i;
			offset <<= kho_page_table_level_shift(level, order);
			ret = __kho_walk_page_tables(order, level - 1,
						     next, offset + pa, cb);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

static int kho_walk_page_tables(struct kho_page_table *top, int order,
				kho_walk_callback_t cb)
{
	int num_table_level;

	if (top) {
		num_table_level = kho_table_level(order);
		return __kho_walk_page_tables(order, num_table_level, top, 0, cb);
	}

	return 0;
}

static int kho_memblock_reserve(phys_addr_t pa, int order)
{
	int sz = 1 << (order + PAGE_SHIFT);
	struct page *page = phys_to_page(pa);

	memblock_reserve(pa, sz);
	memblock_reserved_mark_noinit(pa, sz);
	page->private = order;

	return 0;
}

struct kho_serialization {
	struct page *fdt;
	struct list_head fdt_list;
	struct dentry *sub_fdt_dir;
};

/* almost as free_reserved_page(), just don't free the page */
static void kho_restore_page(struct page *page, unsigned int order)
{
	unsigned int nr_pages = (1 << order);

	/* Head page gets refcount of 1. */
	set_page_count(page, 1);

	/* For higher order folios, tail pages get a page count of zero. */
	for (unsigned int i = 1; i < nr_pages; i++)
		set_page_count(page + i, 0);

	if (order > 0)
		prep_compound_page(page, order);

	adjust_managed_page_count(page, nr_pages);
}

/**
 * kho_restore_folio - recreates the folio from the preserved memory.
 * @phys: physical address of the folio.
 *
 * Return: pointer to the struct folio on success, NULL on failure.
 */
struct folio *kho_restore_folio(phys_addr_t phys)
{
	struct page *page = pfn_to_online_page(PHYS_PFN(phys));
	unsigned long order;

	if (!page)
		return NULL;

	order = page->private;
	if (order > MAX_PAGE_ORDER)
		return NULL;

	kho_restore_page(page, order);
	return page_folio(page);
}
EXPORT_SYMBOL_GPL(kho_restore_folio);

static void __init kho_mem_deserialize(const void *fdt)
{
	const phys_addr_t *mem;
	int len, i;
	struct kho_order_table *order_table;

	/* Retrieve the KHO order table from passed-in FDT. */
	mem = fdt_getprop(fdt, 0, PROP_PRESERVED_ORDER_TABLE, &len);
	if (!mem || len != sizeof(*mem)) {
		pr_err("failed to get preserved order table\n");
		return;
	}

	order_table = *mem ?
		(struct kho_order_table *)phys_to_virt(*mem) :
		NULL;

	if (!order_table)
		return;

	for (i = 0; i < HUGETLB_PAGE_ORDER + 1; i++) {
		kho_walk_page_tables(kho_page_table(order_table->orders[i]),
				     i, kho_memblock_reserve);
	}
}

/*
 * With KHO enabled, memory can become fragmented because KHO regions may
 * be anywhere in physical address space. The scratch regions give us a
 * safe zones that we will never see KHO allocations from. This is where we
 * can later safely load our new kexec images into and then use the scratch
 * area for early allocations that happen before page allocator is
 * initialized.
 */
static struct kho_scratch *kho_scratch;
static unsigned int kho_scratch_cnt;

/*
 * The scratch areas are scaled by default as percent of memory allocated from
 * memblock. A user can override the scale with command line parameter:
 *
 * kho_scratch=N%
 *
 * It is also possible to explicitly define size for a lowmem, a global and
 * per-node scratch areas:
 *
 * kho_scratch=l[KMG],n[KMG],m[KMG]
 *
 * The explicit size definition takes precedence over scale definition.
 */
static unsigned int scratch_scale __initdata = 200;
static phys_addr_t scratch_size_global __initdata;
static phys_addr_t scratch_size_pernode __initdata;
static phys_addr_t scratch_size_lowmem __initdata;

static int __init kho_parse_scratch_size(char *p)
{
	size_t len;
	unsigned long sizes[3];
	int i;

	if (!p)
		return -EINVAL;

	len = strlen(p);
	if (!len)
		return -EINVAL;

	/* parse nn% */
	if (p[len - 1] == '%') {
		/* unsigned int max is 4,294,967,295, 10 chars */
		char s_scale[11] = {};
		int ret = 0;

		if (len > ARRAY_SIZE(s_scale))
			return -EINVAL;

		memcpy(s_scale, p, len - 1);
		ret = kstrtouint(s_scale, 10, &scratch_scale);
		if (!ret)
			pr_notice("scratch scale is %d%%\n", scratch_scale);
		return ret;
	}

	/* parse ll[KMG],mm[KMG],nn[KMG] */
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		char *endp = p;

		if (i > 0) {
			if (*p != ',')
				return -EINVAL;
			p += 1;
		}

		sizes[i] = memparse(p, &endp);
		if (!sizes[i] || endp == p)
			return -EINVAL;
		p = endp;
	}

	scratch_size_lowmem = sizes[0];
	scratch_size_global = sizes[1];
	scratch_size_pernode = sizes[2];
	scratch_scale = 0;

	pr_notice("scratch areas: lowmem: %lluMiB global: %lluMiB pernode: %lldMiB\n",
		  (u64)(scratch_size_lowmem >> 20),
		  (u64)(scratch_size_global >> 20),
		  (u64)(scratch_size_pernode >> 20));

	return 0;
}
early_param("kho_scratch", kho_parse_scratch_size);

static void __init scratch_size_update(void)
{
	phys_addr_t size;

	if (!scratch_scale)
		return;

	size = memblock_reserved_kern_size(ARCH_LOW_ADDRESS_LIMIT,
					   NUMA_NO_NODE);
	size = size * scratch_scale / 100;
	scratch_size_lowmem = round_up(size, CMA_MIN_ALIGNMENT_BYTES);

	size = memblock_reserved_kern_size(MEMBLOCK_ALLOC_ANYWHERE,
					   NUMA_NO_NODE);
	size = size * scratch_scale / 100 - scratch_size_lowmem;
	scratch_size_global = round_up(size, CMA_MIN_ALIGNMENT_BYTES);
}

static phys_addr_t __init scratch_size_node(int nid)
{
	phys_addr_t size;

	if (scratch_scale) {
		size = memblock_reserved_kern_size(MEMBLOCK_ALLOC_ANYWHERE,
						   nid);
		size = size * scratch_scale / 100;
	} else {
		size = scratch_size_pernode;
	}

	return round_up(size, CMA_MIN_ALIGNMENT_BYTES);
}

/**
 * kho_reserve_scratch - Reserve a contiguous chunk of memory for kexec
 *
 * With KHO we can preserve arbitrary pages in the system. To ensure we still
 * have a large contiguous region of memory when we search the physical address
 * space for target memory, let's make sure we always have a large CMA region
 * active. This CMA region will only be used for movable pages which are not a
 * problem for us during KHO because we can just move them somewhere else.
 */
static void __init kho_reserve_scratch(void)
{
	phys_addr_t addr, size;
	int nid, i = 0;

	if (!kho_enable)
		return;

	scratch_size_update();

	/* FIXME: deal with node hot-plug/remove */
	kho_scratch_cnt = num_online_nodes() + 2;
	size = kho_scratch_cnt * sizeof(*kho_scratch);
	kho_scratch = memblock_alloc(size, PAGE_SIZE);
	if (!kho_scratch)
		goto err_disable_kho;

	/*
	 * reserve scratch area in low memory for lowmem allocations in the
	 * next kernel
	 */
	size = scratch_size_lowmem;
	addr = memblock_phys_alloc_range(size, CMA_MIN_ALIGNMENT_BYTES, 0,
					 ARCH_LOW_ADDRESS_LIMIT);
	if (!addr)
		goto err_free_scratch_desc;

	kho_scratch[i].addr = addr;
	kho_scratch[i].size = size;
	i++;

	/* reserve large contiguous area for allocations without nid */
	size = scratch_size_global;
	addr = memblock_phys_alloc(size, CMA_MIN_ALIGNMENT_BYTES);
	if (!addr)
		goto err_free_scratch_areas;

	kho_scratch[i].addr = addr;
	kho_scratch[i].size = size;
	i++;

	for_each_online_node(nid) {
		size = scratch_size_node(nid);
		addr = memblock_alloc_range_nid(size, CMA_MIN_ALIGNMENT_BYTES,
						0, MEMBLOCK_ALLOC_ACCESSIBLE,
						nid, true);
		if (!addr)
			goto err_free_scratch_areas;

		kho_scratch[i].addr = addr;
		kho_scratch[i].size = size;
		i++;
	}

	return;

err_free_scratch_areas:
	for (i--; i >= 0; i--)
		memblock_phys_free(kho_scratch[i].addr, kho_scratch[i].size);
err_free_scratch_desc:
	memblock_free(kho_scratch, kho_scratch_cnt * sizeof(*kho_scratch));
err_disable_kho:
	pr_warn("Failed to reserve scratch area, disabling kexec handover\n");
	kho_enable = false;
}

struct fdt_debugfs {
	struct list_head list;
	struct debugfs_blob_wrapper wrapper;
	struct dentry *file;
};

static int kho_debugfs_fdt_add(struct list_head *list, struct dentry *dir,
			       const char *name, const void *fdt)
{
	struct fdt_debugfs *f;
	struct dentry *file;

	f = kmalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;

	f->wrapper.data = (void *)fdt;
	f->wrapper.size = fdt_totalsize(fdt);

	file = debugfs_create_blob(name, 0400, dir, &f->wrapper);
	if (IS_ERR(file)) {
		kfree(f);
		return PTR_ERR(file);
	}

	f->file = file;
	list_add(&f->list, list);

	return 0;
}

/**
 * kho_add_subtree - record the physical address of a sub FDT in KHO root tree.
 * @ser: serialization control object passed by KHO notifiers.
 * @name: name of the sub tree.
 * @fdt: the sub tree blob.
 *
 * Creates a new child node named @name in KHO root FDT and records
 * the physical address of @fdt. The pages of @fdt must also be preserved
 * by KHO for the new kernel to retrieve it after kexec.
 *
 * A debugfs blob entry is also created at
 * ``/sys/kernel/debug/kho/out/sub_fdts/@name``.
 *
 * Return: 0 on success, error code on failure
 */
int kho_add_subtree(struct kho_serialization *ser, const char *name, void *fdt)
{
	int err = 0;
	u64 phys = (u64)virt_to_phys(fdt);
	void *root = page_to_virt(ser->fdt);

	err |= fdt_begin_node(root, name);
	err |= fdt_property(root, PROP_SUB_FDT, &phys, sizeof(phys));
	err |= fdt_end_node(root);

	if (err)
		return err;

	return kho_debugfs_fdt_add(&ser->fdt_list, ser->sub_fdt_dir, name, fdt);
}
EXPORT_SYMBOL_GPL(kho_add_subtree);

struct kho_out {
	struct blocking_notifier_head chain_head;
	struct dentry *dir;
	struct kho_serialization ser;
};

static struct kho_out kho_out = {
	.chain_head = BLOCKING_NOTIFIER_INIT(kho_out.chain_head),
	.ser = {
		.fdt_list = LIST_HEAD_INIT(kho_out.ser.fdt_list),
	},
};

int register_kho_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&kho_out.chain_head, nb);
}
EXPORT_SYMBOL_GPL(register_kho_notifier);

int unregister_kho_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&kho_out.chain_head, nb);
}
EXPORT_SYMBOL_GPL(unregister_kho_notifier);

/**
 * kho_preserve_folio - preserve a folio across kexec.
 * @folio: folio to preserve.
 *
 * Instructs KHO to preserve the whole folio across kexec. The order
 * will be preserved as well.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_folio(struct folio *folio)
{
	const unsigned long pfn = folio_pfn(folio);
	const unsigned int order = folio_order(folio);

	return kho_preserve_page_table(pfn, order);
}
EXPORT_SYMBOL_GPL(kho_preserve_folio);

/**
 * kho_preserve_phys - preserve a physically contiguous range across kexec.
 * @phys: physical address of the range.
 * @size: size of the range.
 *
 * Instructs KHO to preserve the memory range from @phys to @phys + @size
 * across kexec.
 *
 * Return: 0 on success, error code on failure
 */
int kho_preserve_phys(phys_addr_t phys, size_t size)
{
	unsigned long pfn = PHYS_PFN(phys);
	const unsigned long end_pfn = PHYS_PFN(phys + size);
	int err = 0;

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size))
		return -EINVAL;

	while (pfn < end_pfn) {
		const unsigned int order =
			min(count_trailing_zeros(pfn), ilog2(end_pfn - pfn));

		err = kho_preserve_page_table(pfn, order);
		if (err)
			return err;

		pfn += 1 << order;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(kho_preserve_phys);

/* Handling for debug/kho/out */

static struct dentry *debugfs_root;

static int scratch_phys_show(struct seq_file *m, void *v)
{
	for (int i = 0; i < kho_scratch_cnt; i++)
		seq_printf(m, "0x%llx\n", kho_scratch[i].addr);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scratch_phys);

static int scratch_len_show(struct seq_file *m, void *v)
{
	for (int i = 0; i < kho_scratch_cnt; i++)
		seq_printf(m, "0x%llx\n", kho_scratch[i].size);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(scratch_len);

static __init int kho_out_debugfs_init(void)
{
	struct dentry *dir, *f, *sub_fdt_dir;

	dir = debugfs_create_dir("out", debugfs_root);
	if (IS_ERR(dir))
		return -ENOMEM;

	sub_fdt_dir = debugfs_create_dir("sub_fdts", dir);
	if (IS_ERR(sub_fdt_dir))
		goto err_rmdir;

	f = debugfs_create_file("scratch_phys", 0400, dir, NULL,
				&scratch_phys_fops);
	if (IS_ERR(f))
		goto err_rmdir;

	f = debugfs_create_file("scratch_len", 0400, dir, NULL,
				&scratch_len_fops);
	if (IS_ERR(f))
		goto err_rmdir;

	kho_out.dir = dir;
	kho_out.ser.sub_fdt_dir = sub_fdt_dir;
	return 0;

err_rmdir:
	debugfs_remove_recursive(dir);
	return -ENOENT;
}

struct kho_in {
	struct dentry *dir;
	phys_addr_t fdt_phys;
	phys_addr_t scratch_phys;
	struct list_head fdt_list;
};

static struct kho_in kho_in = {
	.fdt_list = LIST_HEAD_INIT(kho_in.fdt_list),
};

static const void *kho_get_fdt(void)
{
	return kho_in.fdt_phys ? phys_to_virt(kho_in.fdt_phys) : NULL;
}

/**
 * kho_retrieve_subtree - retrieve a preserved sub FDT by its name.
 * @name: the name of the sub FDT passed to kho_add_subtree().
 * @phys: if found, the physical address of the sub FDT is stored in @phys.
 *
 * Retrieve a preserved sub FDT named @name and store its physical
 * address in @phys.
 *
 * Return: 0 on success, error code on failure
 */
int kho_retrieve_subtree(const char *name, phys_addr_t *phys)
{
	const void *fdt = kho_get_fdt();
	const u64 *val;
	int offset, len;

	if (!fdt)
		return -ENOENT;

	if (!phys)
		return -EINVAL;

	offset = fdt_subnode_offset(fdt, 0, name);
	if (offset < 0)
		return -ENOENT;

	val = fdt_getprop(fdt, offset, PROP_SUB_FDT, &len);
	if (!val || len != sizeof(*val))
		return -EINVAL;

	*phys = (phys_addr_t)*val;

	return 0;
}
EXPORT_SYMBOL_GPL(kho_retrieve_subtree);

/* Handling for debugfs/kho/in */

static __init int kho_in_debugfs_init(const void *fdt)
{
	struct dentry *sub_fdt_dir;
	int err, child;

	kho_in.dir = debugfs_create_dir("in", debugfs_root);
	if (IS_ERR(kho_in.dir))
		return PTR_ERR(kho_in.dir);

	sub_fdt_dir = debugfs_create_dir("sub_fdts", kho_in.dir);
	if (IS_ERR(sub_fdt_dir)) {
		err = PTR_ERR(sub_fdt_dir);
		goto err_rmdir;
	}

	err = kho_debugfs_fdt_add(&kho_in.fdt_list, kho_in.dir, "fdt", fdt);
	if (err)
		goto err_rmdir;

	fdt_for_each_subnode(child, fdt, 0) {
		int len = 0;
		const char *name = fdt_get_name(fdt, child, NULL);
		const u64 *fdt_phys;

		fdt_phys = fdt_getprop(fdt, child, "fdt", &len);
		if (!fdt_phys)
			continue;
		if (len != sizeof(*fdt_phys)) {
			pr_warn("node `%s`'s prop `fdt` has invalid length: %d\n",
				name, len);
			continue;
		}
		err = kho_debugfs_fdt_add(&kho_in.fdt_list, sub_fdt_dir, name,
					  phys_to_virt(*fdt_phys));
		if (err) {
			pr_warn("failed to add fdt `%s` to debugfs: %d\n", name,
				err);
			continue;
		}
	}

	return 0;

err_rmdir:
	debugfs_remove_recursive(kho_in.dir);
	return err;
}

static int kho_out_fdt_init(void)
{
	int err = 0;
	void *fdt = page_to_virt(kho_out.ser.fdt);
	u64 *preserved_order_table;

	err |= fdt_create(fdt, PAGE_SIZE);
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");
	err |= fdt_property_string(fdt, "compatible", KHO_FDT_COMPATIBLE);

	err |= fdt_property_placeholder(fdt, PROP_PRESERVED_ORDER_TABLE,
					sizeof(*preserved_order_table),
					(void **)&preserved_order_table);
	if (err)
		goto abort;

	*preserved_order_table = (u64)virt_to_phys(kho_order_table);

	err |= fdt_end_node(fdt);
	err |= fdt_finish(fdt);

abort:
	if (err)
		pr_err("Failed to convert KHO state tree: %d\n", err);

	return err;
}

static __init int kho_init(void)
{
	int err = 0;
	const void *fdt = kho_get_fdt();

	if (!kho_enable)
		return 0;

	kho_out.ser.fdt = alloc_page(GFP_KERNEL);
	if (!kho_out.ser.fdt) {
		err = -ENOMEM;
		goto err_free_scratch;
	}

	kho_order_table = (struct kho_order_table *)
		kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!kho_order_table) {
		err = -ENOMEM;
		goto err_free_fdt;
	}

	err = kho_out_fdt_init();
	if (err)
		goto err_free_kho_order_table;

	debugfs_root = debugfs_create_dir("kho", NULL);
	if (IS_ERR(debugfs_root)) {
		err = -ENOENT;
		goto err_free_kho_order_table;
	}

	err = kho_out_debugfs_init();
	if (err)
		goto err_free_kho_order_table;

	if (fdt) {
		err = kho_in_debugfs_init(fdt);
		/*
		 * Failure to create /sys/kernel/debug/kho/in does not prevent
		 * reviving state from KHO and setting up KHO for the next
		 * kexec.
		 */
		if (err)
			pr_err("failed exposing handover FDT in debugfs: %d\n",
			       err);

		return 0;
	}

	for (int i = 0; i < kho_scratch_cnt; i++) {
		unsigned long base_pfn = PHYS_PFN(kho_scratch[i].addr);
		unsigned long count = kho_scratch[i].size >> PAGE_SHIFT;
		unsigned long pfn;

		for (pfn = base_pfn; pfn < base_pfn + count;
		     pfn += pageblock_nr_pages)
			init_cma_reserved_pageblock(pfn_to_page(pfn));
	}

	return 0;

err_free_kho_order_table:
	kfree(kho_order_table);
	kho_order_table = NULL;
err_free_fdt:
	put_page(kho_out.ser.fdt);
	kho_out.ser.fdt = NULL;
err_free_scratch:
	for (int i = 0; i < kho_scratch_cnt; i++) {
		void *start = __va(kho_scratch[i].addr);
		void *end = start + kho_scratch[i].size;

		free_reserved_area(start, end, -1, "");
	}
	kho_enable = false;
	return err;
}
late_initcall(kho_init);

static void __init kho_release_scratch(void)
{
	phys_addr_t start, end;
	u64 i;

	memmap_init_kho_scratch_pages();

	/*
	 * Mark scratch mem as CMA before we return it. That way we
	 * ensure that no kernel allocations happen on it. That means
	 * we can reuse it as scratch memory again later.
	 */
	__for_each_mem_range(i, &memblock.memory, NULL, NUMA_NO_NODE,
			     MEMBLOCK_KHO_SCRATCH, &start, &end, NULL) {
		ulong start_pfn = pageblock_start_pfn(PFN_DOWN(start));
		ulong end_pfn = pageblock_align(PFN_UP(end));
		ulong pfn;

		for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages)
			init_pageblock_migratetype(pfn_to_page(pfn),
						   MIGRATE_CMA, false);
	}
}

void __init kho_memory_init(void)
{
	struct folio *folio;

	if (kho_in.scratch_phys) {
		kho_scratch = phys_to_virt(kho_in.scratch_phys);
		kho_release_scratch();

		kho_mem_deserialize(kho_get_fdt());
		folio = kho_restore_folio(kho_in.fdt_phys);
		if (!folio)
			pr_warn("failed to restore folio for KHO fdt\n");
	} else {
		kho_reserve_scratch();
	}
}

void __init kho_populate(phys_addr_t fdt_phys, u64 fdt_len,
			 phys_addr_t scratch_phys, u64 scratch_len)
{
	void *fdt = NULL;
	struct kho_scratch *scratch = NULL;
	int err = 0;
	unsigned int scratch_cnt = scratch_len / sizeof(*kho_scratch);

	/* Validate the input FDT */
	fdt = early_memremap(fdt_phys, fdt_len);
	if (!fdt) {
		pr_warn("setup: failed to memremap FDT (0x%llx)\n", fdt_phys);
		err = -EFAULT;
		goto out;
	}
	err = fdt_check_header(fdt);
	if (err) {
		pr_warn("setup: handover FDT (0x%llx) is invalid: %d\n",
			fdt_phys, err);
		err = -EINVAL;
		goto out;
	}
	err = fdt_node_check_compatible(fdt, 0, KHO_FDT_COMPATIBLE);
	if (err) {
		pr_warn("setup: handover FDT (0x%llx) is incompatible with '%s': %d\n",
			fdt_phys, KHO_FDT_COMPATIBLE, err);
		err = -EINVAL;
		goto out;
	}

	scratch = early_memremap(scratch_phys, scratch_len);
	if (!scratch) {
		pr_warn("setup: failed to memremap scratch (phys=0x%llx, len=%lld)\n",
			scratch_phys, scratch_len);
		err = -EFAULT;
		goto out;
	}

	/*
	 * We pass a safe contiguous blocks of memory to use for early boot
	 * purporses from the previous kernel so that we can resize the
	 * memblock array as needed.
	 */
	for (int i = 0; i < scratch_cnt; i++) {
		struct kho_scratch *area = &scratch[i];
		u64 size = area->size;

		memblock_add(area->addr, size);
		err = memblock_mark_kho_scratch(area->addr, size);
		if (WARN_ON(err)) {
			pr_warn("failed to mark the scratch region 0x%pa+0x%pa: %d",
				&area->addr, &size, err);
			goto out;
		}
		pr_debug("Marked 0x%pa+0x%pa as scratch", &area->addr, &size);
	}

	memblock_reserve(scratch_phys, scratch_len);

	/*
	 * Now that we have a viable region of scratch memory, let's tell
	 * the memblocks allocator to only use that for any allocations.
	 * That way we ensure that nothing scribbles over in use data while
	 * we initialize the page tables which we will need to ingest all
	 * memory reservations from the previous kernel.
	 */
	memblock_set_kho_scratch_only();

	kho_in.fdt_phys = fdt_phys;
	kho_in.scratch_phys = scratch_phys;
	kho_scratch_cnt = scratch_cnt;
	pr_info("found kexec handover data. Will skip init for some devices\n");

out:
	if (fdt)
		early_memunmap(fdt, fdt_len);
	if (scratch)
		early_memunmap(scratch, scratch_len);
	if (err)
		pr_warn("disabling KHO revival: %d\n", err);
}

/* Helper functions for kexec_file_load */

int kho_fill_kimage(struct kimage *image)
{
	ssize_t scratch_size;
	int err = 0;
	struct kexec_buf scratch;

	if (!kho_enable)
		return 0;

	image->kho.fdt = page_to_phys(kho_out.ser.fdt);
	/* Preserve the memory page of FDT for the next kernel */
	kho_preserve_phys(image->kho.fdt, PAGE_SIZE);

	scratch_size = sizeof(*kho_scratch) * kho_scratch_cnt;
	scratch = (struct kexec_buf){
		.image = image,
		.buffer = kho_scratch,
		.bufsz = scratch_size,
		.mem = KEXEC_BUF_MEM_UNKNOWN,
		.memsz = scratch_size,
		.buf_align = SZ_64K, /* Makes it easier to map */
		.buf_max = ULONG_MAX,
		.top_down = true,
	};
	err = kexec_add_buffer(&scratch);
	if (err)
		return err;
	image->kho.scratch = &image->segment[image->nr_segments - 1];

	return 0;
}

static int kho_walk_scratch(struct kexec_buf *kbuf,
			    int (*func)(struct resource *, void *))
{
	int ret = 0;
	int i;

	for (i = 0; i < kho_scratch_cnt; i++) {
		struct resource res = {
			.start = kho_scratch[i].addr,
			.end = kho_scratch[i].addr + kho_scratch[i].size - 1,
		};

		/* Try to fit the kimage into our KHO scratch region */
		ret = func(&res, kbuf);
		if (ret)
			break;
	}

	return ret;
}

int kho_locate_mem_hole(struct kexec_buf *kbuf,
			int (*func)(struct resource *, void *))
{
	int ret;

	if (!kho_enable || kbuf->image->type == KEXEC_TYPE_CRASH)
		return 1;

	ret = kho_walk_scratch(kbuf, func);

	return ret == 1 ? 0 : -EADDRNOTAVAIL;
}
