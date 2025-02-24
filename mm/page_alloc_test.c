// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errname.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/nodemask.h>
#include <linux/percpu.h>
#include <linux/smp.h>

#include <kunit/test.h>

#include "internal.h"

#define EXPECT_PCPLIST_EMPTY(test, zone, cpu, pindex) ({			\
	struct per_cpu_pages *pcp = per_cpu_ptr(zone->per_cpu_pageset, cpu);	\
	struct page *page;							\
										\
	lockdep_assert_held(&pcp->lock);					\
	page = list_first_entry_or_null(					\
		&pcp->lists[pindex], struct page, pcp_list);			\
										\
	if (page) {								\
		KUNIT_FAIL(test, "PCPlist %d on CPU %d wasn't empty", i, cpu);	\
		dump_page(page, "unexpectedly on pcplist");			\
	}									\
})

#define EXPECT_WITHIN_ZONE(test, page, zone) ({					\
	unsigned long pfn = page_to_pfn(page);					\
	unsigned long start_pfn = zone->zone_start_pfn;				\
	unsigned long end_pfn = start_pfn + zone->spanned_pages;		\
										\
	KUNIT_EXPECT_TRUE_MSG(test,						\
		pfn >= start_pfn && pfn < end_pfn,				\
		"Wanted PFN 0x%lx - 0x%lx, got 0x%lx",				\
		start_pfn, end_pfn, pfn);					\
	KUNIT_EXPECT_PTR_EQ_MSG(test, page_zone(page), zone,			\
		"Wanted %px (%s), got %px (%s)",				\
		zone, zone->name, page_zone(page), page_zone(page)->name);	\
})

static void action_nodemask_free(void *ctx)
{
	NODEMASK_FREE(ctx);
}

/*
 * Call __alloc_pages_noprof with a nodemask containing only the nid.
 *
 * Never returns NULL.
 */
static inline struct page *alloc_pages_force_nid(struct kunit *test,
						 gfp_t gfp, int order, int nid)
{
	NODEMASK_ALLOC(nodemask_t, nodemask, GFP_KERNEL);
	struct page *page;

	KUNIT_ASSERT_NOT_NULL(test, nodemask);
	kunit_add_action(test, action_nodemask_free, &nodemask);
	nodes_clear(*nodemask);
	node_set(nid, *nodemask);

	page = __alloc_pages_noprof(GFP_KERNEL, 0, nid, nodemask);
	KUNIT_ASSERT_NOT_NULL(test, page);
	return page;
}

static inline bool page_on_buddy_list(struct page *want_page, struct list_head *head)
{
	struct page *found_page;

	list_for_each_entry(found_page, head, buddy_list) {
		if (found_page == want_page)
			return true;
	}

	return false;
}

/* Test case parameters that are independent of alloc order.  */
static const struct {
	gfp_t gfp_flags;
	enum zone_type want_zone;
} alloc_fresh_gfps[] = {
	/*
	 * The way we currently set up the isolated node, everything ends up in
	 * ZONE_NORMAL.
	 */
	{ .gfp_flags = GFP_KERNEL,	.want_zone = ZONE_NORMAL },
	{ .gfp_flags = GFP_ATOMIC,	.want_zone = ZONE_NORMAL },
	{ .gfp_flags = GFP_USER,	.want_zone = ZONE_NORMAL },
	{ .gfp_flags = GFP_DMA32,	.want_zone = ZONE_NORMAL },
};

struct alloc_fresh_test_case {
	int order;
	int gfp_idx;
};

/* Generate test cases as the cross product of orders and alloc_fresh_gfps.  */
static const void *alloc_fresh_gen_params(const void *prev, char *desc)
{
	/* Buffer to avoid allocations. */
	static struct alloc_fresh_test_case tc;

	if (!prev) {
		/* First call */
		tc.order = 0;
		tc.gfp_idx = 0;
		return &tc;
	}

	tc.gfp_idx++;
	if (tc.gfp_idx >= ARRAY_SIZE(alloc_fresh_gfps)) {
		tc.gfp_idx = 0;
		tc.order++;
	}
	if (tc.order > MAX_PAGE_ORDER)
		/* Finished. */
		return NULL;

	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "order %d %pGg\n",
		tc.order, &alloc_fresh_gfps[tc.gfp_idx].gfp_flags);
	return &tc;
}

/* Smoke test: allocate from a node where everything is in a pristine state. */
static void test_alloc_fresh(struct kunit *test)
{
	const struct alloc_fresh_test_case *tc = test->param_value;
	gfp_t gfp_flags = alloc_fresh_gfps[tc->gfp_idx].gfp_flags;
	enum zone_type want_zone_type = alloc_fresh_gfps[tc->gfp_idx].want_zone;
	struct zone *want_zone = &NODE_DATA(isolated_node)->node_zones[want_zone_type];
	struct list_head *buddy_list;
	struct per_cpu_pages *pcp;
	struct page *page, *merged_page;
	int cpu;

	page = alloc_pages_force_nid(test, gfp_flags, tc->order, isolated_node);

	EXPECT_WITHIN_ZONE(test, page, want_zone);

	cpu = get_cpu();
	__free_pages(page, 0);
	pcp = per_cpu_ptr(want_zone->per_cpu_pageset, cpu);
	put_cpu();

	/*
	 * Should end up back in the free area when drained. Because everything
	 * is free, it should get buddy-merged up to the maximum order.
	 */
	drain_zone_pages(want_zone, pcp);
	KUNIT_EXPECT_TRUE(test, PageBuddy(page));
	KUNIT_EXPECT_EQ(test, buddy_order(page), MAX_PAGE_ORDER);
	KUNIT_EXPECT_TRUE(test, list_empty(&pcp->lists[MIGRATE_UNMOVABLE]));
	merged_page = pfn_to_page(round_down(page_to_pfn(page), 1 << MAX_PAGE_ORDER));
	buddy_list = &want_zone->free_area[MAX_PAGE_ORDER].free_list[MIGRATE_UNMOVABLE];
	KUNIT_EXPECT_TRUE(test, page_on_buddy_list(merged_page, buddy_list));
}

static void action_drain_pages_all(void *unused)
{
	int cpu;

	for_each_online_cpu(cpu)
		drain_pages(cpu);
}

/* Runs before each test. */
static int test_init(struct kunit *test)
{
	struct zone *zone_normal;
	int cpu;

	if (isolated_node == NUMA_NO_NODE)
		kunit_skip(test, "No fake NUMA node ID allocated");

	zone_normal = &NODE_DATA(isolated_node)->node_zones[ZONE_NORMAL];

	/*
	 * Nothing except these tests should be allocating from the fake node so
	 * the pcplists should be empty. Obviously this is racy but at least it
	 * can probabilistically detect issues that would otherwise make for
	 * really confusing test results.
	 */
	for_each_possible_cpu(cpu) {
		struct per_cpu_pages *pcp = per_cpu_ptr(zone_normal->per_cpu_pageset, cpu);
		unsigned long flags;
		int i;

		spin_lock_irqsave(&pcp->lock, flags);
		for (i = 0; i < ARRAY_SIZE(pcp->lists); i++)
			EXPECT_PCPLIST_EMPTY(test, zone_normal, cpu, i);
		spin_unlock_irqrestore(&pcp->lock, flags);
	}

	/* Also ensure we don't leave a mess for the next test. */
	kunit_add_action(test, action_drain_pages_all, NULL);

	return 0;
}

static int memory_block_online_cb(struct memory_block *mem, void *unused)
{
	return memory_block_online(mem);
}

struct region {
	int node;
	unsigned long start;
	unsigned long size;
};

/*
 * Unplug some memory from a "real" node and plug it into the isolated node, for
 * use during the tests.
 */
static int populate_isolated_node(struct kunit_suite *suite)
{
	struct zone *zone_movable = &NODE_DATA(0)->node_zones[ZONE_MOVABLE];
	phys_addr_t zone_start = zone_movable->zone_start_pfn << PAGE_SHIFT;
	phys_addr_t zone_size = zone_movable->spanned_pages << PAGE_SHIFT;
	unsigned long bs = memory_block_size_bytes();
	u64 start = round_up(zone_start, bs);
	/* Plug a memory block if we can find it. */
	unsigned long size = round_down(min(zone_size, bs), bs);
	int err;

	if (!size) {
		pr_err("Couldn't find ZONE_MOVABLE block to offline\n");
		pr_err("Try setting/expanding movablecore=\n");
		return -1;
	}

	err = offline_and_remove_memory(start, size);
	if (err) {
		pr_notice("Couldn't offline PFNs 0x%llx - 0x%llx\n",
			start >> PAGE_SHIFT, (start + size) >> PAGE_SHIFT);
		return err;
	}
	err = add_memory(isolated_node, start, size, MMOP_ONLINE);
	if (err) {
		pr_notice("Couldn't add PFNs 0x%llx - 0x%llx\n",
			start >> PAGE_SHIFT, (start + size) >> PAGE_SHIFT);
		goto add_and_online_memory;
	}
	err = walk_memory_blocks(start, size, NULL, memory_block_online_cb);
	if (err) {
		pr_notice("Couldn't online PFNs 0x%llx - 0x%llx\n",
			start >> PAGE_SHIFT, (start + size) >> PAGE_SHIFT);
		goto remove_memory;
	}

	return 0;

remove_memory:
	if (WARN_ON(remove_memory(start, size)))
		return err;
add_and_online_memory:
	if (WARN_ON(add_memory(0, start, size, MMOP_ONLINE)))
		return err;
	WARN_ON(walk_memory_blocks(start, size, NULL, memory_block_online_cb));
	return err;
}

static void depopulate_isolated_node(struct kunit_suite *suite)
{
	unsigned long start, size = memory_block_size_bytes();

	if (suite->suite_init_err)
		return;

	start = NODE_DATA(isolated_node)->node_start_pfn << PAGE_SHIFT;

	WARN_ON(remove_memory(start, size));
	WARN_ON(add_memory(0, start, size, MMOP_ONLINE));
	WARN_ON(walk_memory_blocks(start, size, NULL, memory_block_online_cb));
}

static struct kunit_case test_cases[] = {
	KUNIT_CASE_PARAM(test_alloc_fresh, alloc_fresh_gen_params),
	{}
};

struct kunit_suite page_alloc_test_suite = {
	.name = "page_alloc",
	.test_cases = test_cases,
	.suite_init = populate_isolated_node,
	.suite_exit = depopulate_isolated_node,
	.init = test_init,
};
kunit_test_suite(page_alloc_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
