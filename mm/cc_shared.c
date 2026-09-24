// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 ARM Ltd.
 */
#include <linux/align.h>
#include <linux/cc_platform.h>
#include <linux/cc_shared.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/mem_encrypt.h>
#include <linux/numa.h>
#include <linux/overflow.h>
#include <linux/set_memory.h>

size_t __weak arch_cc_shared_granule_size(void)
{
	return PAGE_SIZE;
}

size_t cc_shared_granule_size(void)
{
	size_t granule = arch_cc_shared_granule_size();

	if (WARN_ON_ONCE(granule < PAGE_SIZE || !is_power_of_2(granule)))
		return PAGE_SIZE;

	return granule;
}
EXPORT_SYMBOL_GPL(cc_shared_granule_size);

int cc_shared_calc_layout(size_t requested, struct cc_shared_layout *layout)
{
	size_t granule, rounded;

	if (!requested || !layout)
		return -EINVAL;

	granule = cc_shared_granule_size();
	if (check_add_overflow(requested, granule - 1, &rounded))
		return -EOVERFLOW;

	rounded = ALIGN_DOWN(rounded, granule);
	layout->requested_size = requested;
	layout->shared_size = rounded;
	layout->alignment = granule;

	return 0;
}
EXPORT_SYMBOL_GPL(cc_shared_calc_layout);

bool cc_shared_range_valid(phys_addr_t base, size_t size)
{
	size_t granule = cc_shared_granule_size();

	if (!size)
		return false;

	return IS_ALIGNED(base, granule) && IS_ALIGNED(size, granule);
}
EXPORT_SYMBOL_GPL(cc_shared_range_valid);

static int cc_validate_transition(void *addr, size_t size)
{
	phys_addr_t phys;

	if (!addr || !size || !PAGE_ALIGNED(addr) ||
	    !virt_addr_valid(addr))
		return -EINVAL;

	phys = page_to_phys(virt_to_page(addr));
	if (!cc_shared_range_valid(phys, size))
		return -EINVAL;

	return 0;
}

int cc_make_shared(void *addr, size_t size)
{
	int ret = cc_validate_transition(addr, size);

	if (ret)
		return ret;

	return set_memory_decrypted((unsigned long)addr, size >> PAGE_SHIFT);
}

int cc_make_private(void *addr, size_t size)
{
	int ret = cc_validate_transition(addr, size);

	if (ret)
		return ret;

	return set_memory_encrypted((unsigned long)addr, size >> PAGE_SHIFT);
}

static int __alloc_cc_shared_pages_node(int nid, gfp_t gfp,
					size_t requested,
					struct cc_shared_pages *mem)
{
	struct cc_shared_layout layout;
	struct page *page;
	unsigned int order;
	int ret;

	ret = cc_shared_calc_layout(requested, &layout);
	if (ret)
		return ret;

	order = get_order(layout.shared_size);
	if (order > MAX_PAGE_ORDER)
		return -EINVAL;

	/*
	 * State transitions require a linear-map address and may modify memory.
	 * Allocate from low memory and let the architecture place zeroing at the
	 * appropriate point in the transition.
	 */
	gfp &= ~(__GFP_HIGHMEM | __GFP_ZERO);
	if (nid == NUMA_NO_NODE)
		page = alloc_pages(gfp, order);
	else
		page = alloc_pages_node(nid, gfp, order);
	if (!page)
		return -ENOMEM;

	ret = cc_make_shared(page_address(page), layout.shared_size);
	if (ret) {
		if (!cc_make_private(page_address(page), layout.shared_size))
			__free_pages(page, order);
		else
			pr_warn_ratelimited("leaking %zu bytes with uncertain shared state\n",
					    layout.shared_size);
		return ret;
	}

	mem->page = page;
	mem->shared_size = layout.shared_size;
	return 0;
}

/**
 * alloc_cc_shared_pages_node - allocate memory that can be shared
 * @nid: NUMA node from which to allocate, or %NUMA_NO_NODE
 * @gfp: allocation flags
 * @requested: number of bytes requested; must be nonzero
 * @mem: storage for the allocated page and the size of the shared range
 *
 * Allocate at least @requested bytes and make the allocation shared when
 * memory encryption is active.  A memory-state transition requires a valid
 * linear-map address, so such allocations never come from high memory
 *
 * The shared range may be rounded up to the architecture's transition
 * granule.  On success, @mem->shared_size records the actual size that was
 * made shared and must be retained unchanged for free_cc_shared_pages().
 * @mem is not modified on failure.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int alloc_cc_shared_pages_node(int nid, gfp_t gfp,
			       size_t requested,
			       struct cc_shared_pages *mem)
{
	struct page *page;
	unsigned int order;

	if (!mem || !requested)
		return -EINVAL;

	if (cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return __alloc_cc_shared_pages_node(nid, gfp, requested, mem);

	order = get_order(requested);
	if (order > MAX_PAGE_ORDER)
		return -EINVAL;

	if (nid == NUMA_NO_NODE)
		page = alloc_pages(gfp, order);
	else
		page = alloc_pages_node(nid, gfp, order);
	if (!page)
		return -ENOMEM;

	mem->page = page;
	mem->shared_size = requested;
	return 0;
}
EXPORT_SYMBOL_GPL(alloc_cc_shared_pages_node);

/**
 * alloc_cc_shared_pages - allocate memory that can be shared
 * @gfp: allocation flags
 * @requested: number of bytes requested; must be nonzero
 * @mem: storage for the allocated page and the size of the shared range
 *
 * Equivalent to alloc_cc_shared_pages_node() with %NUMA_NO_NODE.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int alloc_cc_shared_pages(gfp_t gfp,
			  size_t requested, struct cc_shared_pages *mem)
{
	return alloc_cc_shared_pages_node(NUMA_NO_NODE, gfp, requested, mem);
}
EXPORT_SYMBOL_GPL(alloc_cc_shared_pages);

void free_cc_shared_pages(struct cc_shared_pages *mem)
{
	if (!mem || !mem->page)
		return;

	if (!cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		goto free_pages;

	if (cc_make_private(page_address(mem->page), mem->shared_size)) {
		pr_warn_ratelimited("leaking %zu bytes that cannot be made private\n",
				    mem->shared_size);
		return;
	}

free_pages:
	__free_pages(mem->page, get_order(mem->shared_size));
	mem->page = NULL;
	mem->shared_size = 0;
}
EXPORT_SYMBOL_GPL(free_cc_shared_pages);
