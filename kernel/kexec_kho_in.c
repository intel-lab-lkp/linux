// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_kho_in.c - kexec handover code to ingest metadata.
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kexec.h>
#include <linux/device.h>
#include <linux/compiler.h>
#include <linux/io.h>
#include <linux/kmsg_dump.h>
#include <linux/memblock.h>

/* The kho dt during runtime */
static void *fdt;

/* Globals to hand over phys/len from early to runtime */
static phys_addr_t handover_phys __initdata;
static u32 handover_len __initdata;

static phys_addr_t mem_phys __initdata;
static u32 mem_len __initdata;

phys_addr_t kho_scratch_phys;
phys_addr_t kho_scratch_len;

void *kho_get_fdt(void)
{
	return fdt;
}
EXPORT_SYMBOL_GPL(kho_get_fdt);

/**
 * kho_populate_refcount - Scan the DT for any memory ranges. Increase the
 * affected pages' refcount by 1 for each.
 */
__init void kho_populate_refcount(void)
{
	void *fdt = kho_get_fdt();
	void *mem_virt = __va(mem_phys);
	int offset = 0, depth = 0, initial_depth = 0, len;

	if (!fdt)
		return;

	/* Go through the mem list and add 1 for each reference */
	for (offset = 0;
	     offset >= 0 && depth >= initial_depth;
	     offset = fdt_next_node(fdt, offset, &depth)) {
		const struct kho_mem *mems;
		u32 i;

		mems = fdt_getprop(fdt, offset, "mem", &len);
		if (!mems || len & (sizeof(*mems) - 1))
			continue;

		for (i = 0; i < len; i += sizeof(*mems)) {
			const struct kho_mem *mem = ((void *)mems) + i;
			u64 start_pfn = PFN_DOWN(mem->addr);
			u64 end_pfn = PFN_UP(mem->addr + mem->len);
			u64 pfn;

			for (pfn = start_pfn; pfn < end_pfn; pfn++)
				get_page(pfn_to_page(pfn));
		}
	}

	/*
	 * Then reduce the reference count by 1 to offset the initial ref count
	 * of 1. In addition, unreserve the page. That way, we can free_page()
	 * it for every consumer and automatically free it to the global memory
	 * pool when everyone is done.
	 */
	for (offset = 0; offset < mem_len; offset += sizeof(struct kho_mem)) {
		struct kho_mem *mem = mem_virt + offset;
		u64 start_pfn = PFN_DOWN(mem->addr);
		u64 end_pfn = PFN_UP(mem->addr + mem->len);
		u64 pfn;

		for (pfn = start_pfn; pfn < end_pfn; pfn++) {
			struct page *page = pfn_to_page(pfn);

			/*
			 * This is similar to free_reserved_page(), but
			 * preserves the reference count
			 */
			ClearPageReserved(page);
			__free_page(page);
			adjust_managed_page_count(page, 1);
		}
	}
}

static void kho_return_pfn(ulong pfn)
{
	struct page *page = pfn_to_page(pfn);

	if (WARN_ON(!page))
		return;
	__free_page(page);
}

/**
 * kho_return_mem - Notify the kernel that initially reserved memory is no
 * longer needed. When the last consumer of a page returns their mem, kho
 * returns the page to the buddy allocator as free page.
 */
void kho_return_mem(const struct kho_mem *mem)
{
	uint64_t start_pfn, end_pfn, pfn;

	start_pfn = PFN_DOWN(mem->addr);
	end_pfn = PFN_UP(mem->addr + mem->len);

	for (pfn = start_pfn; pfn < end_pfn; pfn++)
		kho_return_pfn(pfn);
}
EXPORT_SYMBOL_GPL(kho_return_mem);

static void kho_claim_pfn(ulong pfn)
{
	struct page *page = pfn_to_page(pfn);

	WARN_ON(!page);
	if (WARN_ON(page_count(page) != 1))
		pr_err("Claimed non kho pfn %lx", pfn);
}

/**
 * kho_claim_mem - Notify the kernel that a handed over memory range is now in
 * use by a kernel subsystem and considered an allocated page. This function
 * removes the reserved state for all pages that the mem spans.
 */
void *kho_claim_mem(const struct kho_mem *mem)
{
	u64 start_pfn, end_pfn, pfn;
	void *va = __va(mem->addr);

	start_pfn = PFN_DOWN(mem->addr);
	end_pfn = PFN_UP(mem->addr + mem->len);

	for (pfn = start_pfn; pfn < end_pfn; pfn++)
		kho_claim_pfn(pfn);

	return va;
}
EXPORT_SYMBOL_GPL(kho_claim_mem);

/**
 * kho_reserve_mem - Adds all memory reservations into memblocks
 * and moves us out of the scratch only phase. Must be called after page tables
 * are initialized and memblock_allow_resize().
 */
void __init kho_reserve_mem(void)
{
	void *mem_virt = __va(mem_phys);
	int off, err;

	if (!handover_phys || !mem_phys)
		return;

	/*
	 * We reached here because we are running inside a working linear map
	 * that allows us to resize memblocks dynamically. Use the chance and
	 * populate the global fdt pointer
	 */
	fdt = __va(handover_phys);

	off = fdt_path_offset(fdt, "/");
	if (off < 0) {
		fdt = NULL;
		return;
	}

	err = fdt_node_check_compatible(fdt, off, "kho-v1");
	if (err) {
		pr_warn("KHO has invalid compatible, disabling.");
		return;
	}

	/* Then populate all preserved memory areas as reserved */
	for (off = 0; off < mem_len; off += sizeof(struct kho_mem)) {
		struct kho_mem *mem = mem_virt + off;

		memblock_reserve(mem->addr, mem->len);
	}

	/* Unreserve the mem cache - we don't need it from here on */
	memblock_phys_free(mem_phys, mem_len);

	/*
	 * Now we know about all memory reservations, release the scratch only
	 * constraint and allow normal allocations from the scratch region.
	 */
	memblock_clear_scratch_only();
}

/* Handling for /sys/firmware/kho */
static struct kobject *kho_kobj;

static ssize_t raw_read(struct file *file, struct kobject *kobj,
			struct bin_attribute *attr, char *buf,
			loff_t pos, size_t count)
{
	memcpy(buf, attr->private + pos, count);
	return count;
}

static BIN_ATTR(dt, 0400, raw_read, NULL, 0);

static __init int kho_in_init(void)
{
	int ret = 0;

	if (!fdt)
		return 0;

	kho_kobj = kobject_create_and_add("kho", firmware_kobj);
	if (!kho_kobj) {
		ret = -ENOMEM;
		goto err;
	}

	bin_attr_dt.size = fdt_totalsize(fdt);
	bin_attr_dt.private = fdt;
	ret = sysfs_create_bin_file(kho_kobj, &bin_attr_dt);
	if (ret)
		goto err;

err:
	return ret;
}
subsys_initcall(kho_in_init);

void __init kho_populate(phys_addr_t handover_dt_phys, phys_addr_t scratch_phys,
			 u64 scratch_len, phys_addr_t mem_cache_phys,
			 u64 mem_cache_len)
{
	void *handover_dt;

	/* Determine the real size of the DT */
	handover_dt = early_memremap(handover_dt_phys, sizeof(struct fdt_header));
	if (!handover_dt) {
		pr_warn("setup: failed to memremap kexec FDT (0x%llx)\n", handover_dt_phys);
		return;
	}

	if (fdt_check_header(handover_dt)) {
		pr_warn("setup: kexec handover FDT is invalid (0x%llx)\n", handover_dt_phys);
		early_memunmap(handover_dt, PAGE_SIZE);
		return;
	}

	handover_len = fdt_totalsize(handover_dt);
	handover_phys = handover_dt_phys;

	/* Reserve the DT so we can still access it in late boot */
	memblock_reserve(handover_phys, handover_len);

	/* Reserve the mem cache so we can still access it later */
	memblock_reserve(mem_cache_phys, mem_cache_len);

	/*
	 * We pass a safe contiguous block of memory to use for early boot purporses from
	 * the previous kernel so that we can resize the memblock array as needed.
	 */
	memblock_add(scratch_phys, scratch_len);

	if (WARN_ON(memblock_mark_scratch(scratch_phys, scratch_len))) {
		pr_err("Kexec failed to mark the scratch region. Disabling KHO.");
		handover_len = 0;
		handover_phys = 0;
		return;
	}
	pr_debug("Marked 0x%lx+0x%lx as scratch", (long)scratch_phys, (long)scratch_len);

	/*
	 * Now that we have a viable region of scratch memory, let's tell the memblocks
	 * allocator to only use that for any allocations. That way we ensure that nothing
	 * scribbles over in use data while we initialize the page tables which we will need
	 * to ingest all memory reservations from the previous kernel.
	 */
	memblock_set_scratch_only();

	early_memunmap(handover_dt, sizeof(struct fdt_header));

	/* Remember the mem cache location for kho_reserve_mem() */
	mem_len = mem_cache_len;
	mem_phys = mem_cache_phys;

	/* Remember the scratch block - we will reuse it again for the next kexec */
	kho_scratch_phys = scratch_phys;
	kho_scratch_len = scratch_len;

	pr_info("setup: Found kexec handover data. Will skip init for some devices\n");
}
