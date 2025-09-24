// SPDX-License-Identifier: GPL-2.0-only
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/set_memory.h>
#include <linux/sched/mm.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include <kunit/resource.h>
#include <kunit/test.h>

#include <asm/asi.h>

struct free_pages_ctx {
	unsigned int order;
	struct list_head pages;
};

static void action_many__free_pages(void *context)
{
	struct free_pages_ctx *ctx = context;
	struct page *page, *tmp;

	list_for_each_entry_safe(page, tmp, &ctx->pages, lru)
		__free_pages(page, ctx->order);
}

/*
 * Allocate a bunch of pages with the same order and GFP flags, transparently
 * take care of error handling and cleanup. Does this all via a single KUnit
 * resource, i.e. has a fixed memory overhead.
 */
static struct free_pages_ctx *do_many_alloc_pages(struct kunit *test, gfp_t gfp,
						unsigned int order, unsigned int count)
{
	struct free_pages_ctx *ctx = kunit_kzalloc(
		test, sizeof(struct free_pages_ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	INIT_LIST_HEAD(&ctx->pages);
	ctx->order = order;

	for (int i = 0; i < count; i++) {
		struct page *page = alloc_pages(gfp, order);

		if (!page) {
			struct page *page, *tmp;

			list_for_each_entry_safe(page, tmp, &ctx->pages, lru)
				__free_pages(page, order);

			KUNIT_FAIL_AND_ABORT(test,
				"Failed to alloc order %d page (GFP *%pG) iter %d",
				order, &gfp, i);
		}
		list_add(&page->lru, &ctx->pages);
	}

	KUNIT_ASSERT_EQ(test,
		kunit_add_action_or_reset(test, action_many__free_pages, ctx), 0);
	return ctx;
}

/*
 * Do some allocations that force the allocator to change the sensitivity of
 * some blocks.
 */
static void test_alloc_sensitive_nonsensitive(struct kunit *test)
{
	unsigned long page_majority;
	struct free_pages_ctx *ctx;
	gfp_t gfp = GFP_KERNEL | __GFP_THISNODE;
	struct page *page;

	if (!cpu_feature_enabled(X86_FEATURE_ASI))
		kunit_skip(test, "ASI off. Set asi=on in kernel cmdline\n");

	/* No cleanup here - assuming kthread "belongs" to this test. */
	set_cpus_allowed_ptr(current, cpumask_of_node(numa_node_id()));

	/*
	 * First allocate more than half of the memory in the node as
	 * nonsensitive. Assuming the memory starts out unmapped, this should
	 * exercise the sensitive->nonsensitive flip already.
	 */
	page_majority = (node_present_pages(numa_node_id()) / 2) + 1;
	ctx = do_many_alloc_pages(test, gfp, 0, page_majority);

	/* Check pages are mapped */
	list_for_each_entry(page, &ctx->pages, lru) {
		/*
		 * Logically it should be an EXPECT, but that would cause heavy
		 * log spam on failure so use ASSERT for concision.
		 */
		KUNIT_ASSERT_FALSE(test, direct_map_sensitive(page));
	}

	/*
	 * Now free them again and allocate the same amount as sensitive.
	 * This will exercise the nonsensitive->sensitive flip.
	 */
	kunit_release_action(test, action_many__free_pages, ctx);
	gfp |= __GFP_SENSITIVE;
	ctx = do_many_alloc_pages(test, gfp, 0, page_majority);

	/* Check pages are unmapped */
	list_for_each_entry(page, &ctx->pages, lru)
		KUNIT_ASSERT_TRUE(test, direct_map_sensitive(page));
}

static struct kunit_case asi_test_cases[] = {
	KUNIT_CASE(test_alloc_sensitive_nonsensitive),
	{}
};

static unsigned long taint_pre;

static int store_taint_pre(struct kunit *test)
{
	taint_pre = get_taint();
	return 0;
}

static void check_taint_post(struct kunit *test)
{
	unsigned long new_taint = get_taint() & ~taint_pre;

	KUNIT_EXPECT_EQ_MSG(test, new_taint, 0,
		"Kernel newly tainted after test. Maybe a WARN?");
}

static struct kunit_suite asi_test_suite = {
	.name = "asi",
	.init = store_taint_pre,
	.exit = check_taint_post,
	.test_cases = asi_test_cases,
};

kunit_test_suite(asi_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
