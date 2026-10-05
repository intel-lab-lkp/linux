/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON kunit tests for the per-context perf report ring.
 *
 * Included at the bottom of core.c, after tests/drain-kunit.h, whose
 * damon_test_attach_perf_probe() helper these tests reuse.
 */

#ifdef CONFIG_DAMON_KUNIT_TEST

#ifndef _DAMON_PERF_KUNIT_H
#define _DAMON_PERF_KUNIT_H

#include <kunit/test.h>
#include <linux/damon.h>

/*
 * A report with probe_idx >= 1 is enqueued into the ring of the context named
 * by report->ctx, so these tests build a context with an allocated perf ring
 * and point the injected reports at it.  A freshly allocated ring is empty,
 * which makes the accepted and rejected counts below exact.
 */

/*
 * Test A: perf ring basic write
 *
 * Inject reports into a context's perf ring via damon_report_access() and
 * verify each one is accepted.
 */
static void damon_test_perf_ring_basic(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_access_report report = {
		.paddr = 0x1000, .size = PAGE_SIZE, .probe_idx = 1,
	};
	int i;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf ring alloc failed");
	}
	report.ctx = ctx;

	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_TRUE(test, damon_report_access(&report));

	damon_destroy_ctx(ctx);
}

/*
 * Test B: ring overflow is reported and does not corrupt head/tail
 *
 * Fill a context's perf ring to capacity and verify that further writes are
 * refused and counted rather than overwriting live entries.  The ring holds
 * DAMON_REPORT_RING_SIZE - 1 entries, one slot being reserved to distinguish
 * full from empty, so the last two of the writes below must be refused.
 */
static void damon_test_perf_ring_overflow_safety(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_access_report report = {
		.paddr = 0x3000, .size = PAGE_SIZE, .probe_idx = 1,
	};
	unsigned long overflow_before, overflow_after;
	int queued = 0, refused = 0;
	int i;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf ring alloc failed");
	}
	report.ctx = ctx;

	/* Pinned so every write lands in the same CPU's ring. */
	preempt_disable();
	overflow_before = damon_get_report_overflow();

	for (i = 0; i < DAMON_REPORT_RING_SIZE + 1; i++) {
		if (damon_report_access(&report))
			queued++;
		else
			refused++;
	}

	overflow_after = damon_get_report_overflow();
	preempt_enable();

	KUNIT_EXPECT_EQ(test, queued, DAMON_REPORT_RING_SIZE - 1);
	KUNIT_EXPECT_EQ(test, refused, 2);
	KUNIT_EXPECT_GT(test, overflow_after, overflow_before);

	damon_destroy_ctx(ctx);
}

/*
 * Test C: a perf report without an owning context is refused
 *
 * A report with probe_idx >= 1 but no ctx cannot be routed to a ring.  Verify
 * it is refused instead of dereferenced.
 */
static void damon_test_perf_report_requires_ctx(struct kunit *test)
{
	struct damon_access_report report = {
		.paddr = 0x5000, .size = PAGE_SIZE, .probe_idx = 1,
		.ctx = NULL,
	};

	KUNIT_EXPECT_FALSE(test, damon_report_access(&report));
}

static struct kunit_case damon_perf_test_cases[] = {
	KUNIT_CASE(damon_test_perf_ring_basic),
	KUNIT_CASE(damon_test_perf_ring_overflow_safety),
	KUNIT_CASE(damon_test_perf_report_requires_ctx),
	{}
};

static struct kunit_suite damon_perf_test_suite = {
	.name = "damon_perf",
	.test_cases = damon_perf_test_cases,
};

kunit_test_suite(damon_perf_test_suite);

#endif /* _DAMON_PERF_KUNIT_H */

#endif	/* CONFIG_DAMON_KUNIT_TEST */
