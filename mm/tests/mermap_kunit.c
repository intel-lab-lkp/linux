// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cacheflush.h>
#include <linux/kthread.h>
#include <linux/mermap.h>
#include <linux/pgtable.h>

#include <kunit/test.h>

#define MERMAP_NR_ALLOCS ARRAY_SIZE(((struct mm_struct *)NULL)->mermap.cpu->allocs)

KUNIT_DEFINE_ACTION_WRAPPER(__free_page_wrapper, __free_page, struct page *);

static inline struct page *alloc_page_wrapper(struct kunit *test, gfp_t gfp)
{
	struct page *page = alloc_page(gfp);

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, __free_page_wrapper, page), 0);
	return page;
}

KUNIT_DEFINE_ACTION_WRAPPER(mmput_wrapper, mmput, struct mm_struct *);

static inline struct mm_struct *mm_alloc_wrapper(struct kunit *test)
{
	struct mm_struct *mm = mm_alloc();

	KUNIT_ASSERT_NOT_NULL(test, mm);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, mmput_wrapper, mm), 0);
	return mm;
}

static inline struct mm_struct *get_mm(struct kunit *test)
{
	struct mm_struct *mm = mm_alloc_wrapper(test);

	KUNIT_ASSERT_EQ(test, mermap_mm_prepare(mm), 0);
	return mm;
}

struct __mermap_put_args {
	struct mm_struct *mm;
	struct mermap_alloc *alloc;
	unsigned long size;
};

static inline void __mermap_put_wrapper(void *ctx)
{
	struct __mermap_put_args *args = (struct __mermap_put_args *)ctx;

	__mermap_put(args->mm, args->alloc);
}

/* Call __mermap_get() with use_reserve=false, deal with cleanup. */
static inline struct __mermap_put_args *
__mermap_get_wrapper(struct kunit *test, struct mm_struct *mm,
		     struct page *page, unsigned long size, pgprot_t prot)
{
	struct __mermap_put_args *args =
		kunit_kmalloc(test, sizeof(struct __mermap_put_args), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, args);
	args->mm = mm;
	args->alloc = __mermap_get(mm, page, size, prot, false);
	args->size = size;

	if (args->alloc) {
		int err = kunit_add_action_or_reset(test, __mermap_put_wrapper, args);

		KUNIT_ASSERT_EQ(test, err, 0);
	}

	return args;
}

/* Do the cleanup from __mermap_get_wrapper, now. */
static inline void __mermap_put_early(struct kunit *test, struct __mermap_put_args *args)
{
	kunit_release_action(test, __mermap_put_wrapper, args);
}

static void test_basic_alloc(struct kunit *test)
{
	struct page *page = alloc_page_wrapper(test, GFP_KERNEL);
	struct mm_struct *mm = get_mm(test);
	struct __mermap_put_args *args;

	args = __mermap_get_wrapper(test, mm, page, PAGE_SIZE, PAGE_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, args->alloc);
}

/* Dumb check for off-by-ones. */
static void test_size(struct kunit *test)
{
	struct page *page = alloc_page_wrapper(test, GFP_KERNEL);
	struct __mermap_put_args *full, *large, *small, *fail;
	struct mm_struct *mm = get_mm(test);
	unsigned long region_size, large_size;
	struct mermap_alloc *alloc;
	int cpu;

	migrate_disable();
	cpu = raw_smp_processor_id();
	region_size = mermap_cpu_end(cpu) - mermap_cpu_base(cpu) - PAGE_SIZE;
	large_size = region_size - PAGE_SIZE;

	/* Allocate whole region at once. */
	full = __mermap_get_wrapper(test, mm, page, region_size, PAGE_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, full->alloc);
	__mermap_put_early(test, full);

	/* Allocate larger than region size. */
	fail = __mermap_get_wrapper(test, mm, page, region_size + PAGE_SIZE, PAGE_KERNEL);
	KUNIT_ASSERT_NULL(test, fail->alloc);

	/* Tiptoe up to the edge then past it. */
	large = __mermap_get_wrapper(test, mm, page, large_size, PAGE_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, large->alloc);
	small = __mermap_get_wrapper(test, mm, page, PAGE_SIZE, PAGE_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, small->alloc);
	fail = __mermap_get_wrapper(test, mm, page, PAGE_SIZE, PAGE_KERNEL);
	KUNIT_ASSERT_NULL(test, fail->alloc);

	/* Can still allocate the reserved page. */
	local_irq_disable();
	alloc = __mermap_get(mm, page, PAGE_SIZE, PAGE_KERNEL, true);
	local_irq_enable();
	KUNIT_ASSERT_NOT_NULL(test, alloc);
	__mermap_put(mm, alloc);
}

static void test_multiple_allocs(struct kunit *test)
{
	struct mm_struct *mm = get_mm(test);
	struct __mermap_put_args *argss[MERMAP_NR_ALLOCS] = { };
	struct page *pages[MERMAP_NR_ALLOCS];
	int magic = 0xE4A4;

	for (int i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page_wrapper(test, GFP_KERNEL);
		WRITE_ONCE(*(int *)page_to_virt(pages[i]), magic + i);
	}

	for (int i = 0; i < ARRAY_SIZE(argss); i++) {
		unsigned long base = mermap_cpu_base(raw_smp_processor_id());
		unsigned long end = mermap_cpu_end(raw_smp_processor_id());
		unsigned long addr;

		argss[i] = __mermap_get_wrapper(test, mm, pages[i], PAGE_SIZE, PAGE_KERNEL);
		KUNIT_ASSERT_NOT_NULL_MSG(test, argss[i], "alloc %d failed", i);

		addr = (unsigned long) mermap_addr(argss[i]->alloc);
		KUNIT_EXPECT_GE_MSG(test, addr, base, "alloc %d out of range", i);
		KUNIT_EXPECT_LT_MSG(test, addr, end, "alloc %d out of range", i);
	};

	/*
	 * Read through the mappings to try and detect if they point to the
	 * pages we wrote earlier.
	 */
	kthread_use_mm(mm);
	for (int i = 0; i < ARRAY_SIZE(pages); i++) {
		int *ptr  = (int *)mermap_addr(argss[i]->alloc);

		KUNIT_EXPECT_EQ(test, *ptr, magic + i);
	}
	kthread_unuse_mm(mm);
}

static void test_tlb_flushed(struct kunit *test)
{
	struct page *page = alloc_page_wrapper(test, GFP_KERNEL);
	struct mm_struct *mm = get_mm(test);
	unsigned long addr, prev_addr = 0;
	/* Avoid running for ever in failure case. */
	unsigned long max_iters = 1000000;
	struct mermap_cpu *mc;

	migrate_disable();
	mc = this_cpu_ptr(mm->mermap.cpu);

	/*
	 * Allocate until we see an address less than what we had before - assume
	 * that means a reuse.
	 */
	for (int i = 0; i < max_iters; i++) {
		struct mermap_alloc *alloc;

		/*
		 * Obviously flushing the TLB already is not wrong per se, but
		 * it's unexpected and probably means there's some bug.
		 * Use ASSERT to avoid spamming the log in the failure case.
		 */
		KUNIT_ASSERT_EQ_MSG(test, mc->tlb_flushes, 0,
				    "unexpected flush before alloc %d", i);

		alloc = __mermap_get(mm, page, PAGE_SIZE, PAGE_KERNEL, false);
		KUNIT_ASSERT_NOT_NULL_MSG(test, alloc, "alloc %d failed", i);

		addr = (unsigned long)mermap_addr(alloc);
		__mermap_put(mm, alloc);
		if (addr < prev_addr)
			break;

		prev_addr = addr;
		cond_resched();
	}
	KUNIT_ASSERT_TRUE_MSG(test, addr < prev_addr, "no address reuse");
	/* Again, more than one flush isn't wrong per se, but probably a bug. */
	KUNIT_ASSERT_EQ(test, mc->tlb_flushes, 1);

	migrate_enable();
}

static struct kunit_case mermap_test_cases[] = {
	KUNIT_CASE(test_basic_alloc),
	KUNIT_CASE(test_size),
	KUNIT_CASE(test_multiple_allocs),
	KUNIT_CASE(test_tlb_flushed),
	{}
};

static struct kunit_suite mermap_test_suite = {
	.name = "mermap",
	.test_cases = mermap_test_cases,
};
kunit_test_suite(mermap_test_suite);

MODULE_DESCRIPTION("Mermap unit tests");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
