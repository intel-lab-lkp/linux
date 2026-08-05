// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the vm.kvmalloc_max_contig_order limit.
 *
 * The observable the tests rely on is is_vmalloc_addr(): a request the limit
 * denied must have been served by vmalloc(), never by kmalloc().  The reverse
 * direction is deliberately not asserted, as kmalloc() is always free to fail
 * and fall back to vmalloc() on its own, which would make such a test depend
 * on how fragmented the machine happens to be.
 */
#include <kunit/test.h>
#include <kunit/visibility.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include "../mm/slab.h"

/* The order used whenever a test needs a limit below MAX_PAGE_ORDER. */
#define TEST_ORDER		PAGE_ALLOC_COSTLY_ORDER
/* The smallest limit the sysctl accepts, i.e. the order of a full kmalloc cache. */
#define TEST_ORDER_MIN		(KMALLOC_SHIFT_HIGH - PAGE_SHIFT)

static unsigned int saved_order;

static unsigned long forced_vmalloc_count(struct kunit *test)
{
	unsigned long *events, count;

	events = kunit_kcalloc(test, NR_VM_EVENT_ITEMS, sizeof(*events),
			       GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, events);

	all_vm_events(events);
	count = events[KVMALLOC_FORCED_VMALLOC];

	kunit_kfree(test, events);
	return count;
}

/*
 * Allocate @size with the limit set to @order and report whether the result
 * came from vmalloc(), along with how much the counter moved.
 */
static bool alloc_at_order(struct kunit *test, size_t size, unsigned int order,
			   unsigned long *counted)
{
	unsigned long before, after;
	bool vmalloced;
	void *p;

	sysctl_kvmalloc_max_contig_order = order;

	before = forced_vmalloc_count(test);
	p = kvmalloc(size, GFP_KERNEL);
	after = forced_vmalloc_count(test);

	sysctl_kvmalloc_max_contig_order = MAX_PAGE_ORDER;

	KUNIT_ASSERT_NOT_NULL(test, p);
	vmalloced = is_vmalloc_addr(p);
	kvfree(p);

	if (counted)
		*counted = after - before;

	return vmalloced;
}

/* A request above the limit must not be served by kmalloc(). */
static void test_over_limit_is_vmalloc(struct kunit *test)
{
	size_t size = (PAGE_SIZE << TEST_ORDER) + 1;
	unsigned long nr;

	KUNIT_EXPECT_TRUE(test, alloc_at_order(test, size, TEST_ORDER, &nr));
	KUNIT_EXPECT_EQ(test, nr, 1UL);
}

/* A request of exactly the limit is still allowed to use kmalloc(). */
static void test_at_limit_is_not_denied(struct kunit *test)
{
	size_t size = PAGE_SIZE << TEST_ORDER;
	unsigned long nr;

	alloc_at_order(test, size, TEST_ORDER, &nr);
	KUNIT_EXPECT_EQ(test, nr, 0UL);
}

/*
 * Requests a kmalloc cache can serve are never denied, even with the limit at
 * the smallest value the sysctl accepts.
 */
static void test_cache_sized_never_denied(struct kunit *test)
{
	size_t size = KMALLOC_MAX_CACHE_SIZE;
	unsigned long nr;

	KUNIT_EXPECT_FALSE(test, alloc_at_order(test, size, TEST_ORDER_MIN, &nr));
	KUNIT_EXPECT_EQ(test, nr, 0UL);

	size = PAGE_SIZE;
	KUNIT_EXPECT_FALSE(test, alloc_at_order(test, size, TEST_ORDER_MIN, &nr));
	KUNIT_EXPECT_EQ(test, nr, 0UL);
}

/* At the default the limit must be inert, whichever way the allocation goes. */
static void test_default_does_not_deny(struct kunit *test)
{
	size_t size = PAGE_SIZE << TEST_ORDER;
	unsigned long nr;

	alloc_at_order(test, size, MAX_PAGE_ORDER, &nr);
	KUNIT_EXPECT_EQ(test, nr, 0UL);

	size = KMALLOC_MAX_SIZE;
	alloc_at_order(test, size, MAX_PAGE_ORDER, &nr);
	KUNIT_EXPECT_EQ(test, nr, 0UL);
}

/*
 * kmalloc() cannot serve anything above KMALLOC_MAX_SIZE, so such a request
 * reaches vmalloc() either way and must not be accounted to the limit.
 */
static void test_over_kmalloc_max_not_counted(struct kunit *test)
{
	size_t size = KMALLOC_MAX_SIZE + PAGE_SIZE;
	unsigned long nr;

	KUNIT_EXPECT_TRUE(test, alloc_at_order(test, size, TEST_ORDER, &nr));
	KUNIT_EXPECT_EQ(test, nr, 0UL);
}

/* Sub-page requests never fall back to vmalloc(), limit or not. */
static void test_sub_page_untouched(struct kunit *test)
{
	unsigned long nr;

	KUNIT_EXPECT_FALSE(test, alloc_at_order(test, 64, TEST_ORDER_MIN, &nr));
	KUNIT_EXPECT_EQ(test, nr, 0UL);
}

static int kvmalloc_limit_init(struct kunit *test)
{
	/*
	 * TEST_ORDER has to be a value the sysctl would accept, otherwise the
	 * tests would be exercising a state userspace cannot reach.
	 */
	if (TEST_ORDER < TEST_ORDER_MIN || TEST_ORDER >= MAX_PAGE_ORDER)
		kunit_skip(test, "TEST_ORDER %d outside the accepted range [%d, %d]",
			   TEST_ORDER, TEST_ORDER_MIN, MAX_PAGE_ORDER);

	saved_order = sysctl_kvmalloc_max_contig_order;
	return 0;
}

static void kvmalloc_limit_exit(struct kunit *test)
{
	sysctl_kvmalloc_max_contig_order = saved_order;
}

static struct kunit_case kvmalloc_limit_cases[] = {
	KUNIT_CASE(test_over_limit_is_vmalloc),
	KUNIT_CASE(test_at_limit_is_not_denied),
	KUNIT_CASE(test_cache_sized_never_denied),
	KUNIT_CASE(test_default_does_not_deny),
	KUNIT_CASE(test_over_kmalloc_max_not_counted),
	KUNIT_CASE(test_sub_page_untouched),
	{}
};

static struct kunit_suite kvmalloc_limit_suite = {
	.name = "kvmalloc_order_limit",
	.init = kvmalloc_limit_init,
	.exit = kvmalloc_limit_exit,
	.test_cases = kvmalloc_limit_cases,
};

kunit_test_suite(kvmalloc_limit_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for the kvmalloc order limit");
MODULE_LICENSE("GPL");
