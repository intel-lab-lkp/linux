/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON kunit tests for the unified paddr/vaddr report drain path.
 *
 * Included at the bottom of core.c (after kdamond_check_reported_accesses
 * is defined) so the static function is visible.
 */

#ifdef CONFIG_DAMON_KUNIT_TEST

#ifndef _DAMON_DRAIN_KUNIT_H
#define _DAMON_DRAIN_KUNIT_H

#include <kunit/test.h>
#include <linux/damon.h>

/*
 * Reports are dispatched by probe_idx: probe_idx == DAMON_PROBE_IDX_NONE (0)
 * has no ring to feed and is dropped by damon_report_access(); probe_idx >= 1
 * lands in the owning context's per-context perf ring (ctx->perf_rings).  The
 * drain dispatcher kdamond_check_reported_accesses() drains the perf ring only
 * for a ctx that has event-driven probes.
 *
 * Attach a dummy event-driven probe AND allocate the ctx's per-ctx perf ring
 * so the ctx both drains the perf ring and has ring storage for injected
 * probe_idx>=1 reports.  In a live run damon_perf_probe_setup() allocates the
 * ring; kunit has no real perf event, so it allocates directly.  Returns
 * 0/-ENOMEM.
 */
static int damon_test_attach_perf_probe(struct damon_ctx *ctx)
{
	struct damon_probe *p = damon_new_probe();
	int err;

	if (!p)
		return -ENOMEM;
	p->event_driven = true;
	damon_add_probe(ctx, p);

	err = damon_ctx_alloc_perf_ring(ctx);
	if (err)
		return err;
	return 0;
}

/*
 * Mark @ctx as monitoring the physical address space.
 *
 * The drain matches a report against the address space of the context, which
 * damon_target_has_pid() derives from ctx->ops.id, so a context whose targets
 * carry no pid needs the paddr id for its reports to be matched by paddr.
 * Only the id is set: the drain reads no other operations field, and these
 * tests call it directly rather than through a kdamond.
 */
static void damon_test_set_paddr_ctx(struct damon_ctx *ctx)
{
	ctx->ops.id = DAMON_OPS_PADDR;
}

/*
 * Test A: vaddr entry with a matching thread group id drains correctly.
 *
 * Create a vaddr ctx with target pid=current, region [0x1000, 0x2000).
 * Inject entry: paddr=0, vaddr=0x1500, tgid=current tgid, probe_idx=1, ctx=ctx.
 * After drain: probe_hits[0]==1 (probe_idx 1 stored 0-based), samples_drained
 * increments.
 */
static void damon_test_unified_vaddr_match(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0,
		.vaddr     = 0x1500,
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = get_pid(task_tgid(current));
	if (!t->pid) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "pid alloc failed");
	}
	rep.tgid = task_tgid_nr(current);
	rep.ctx = ctx;	/* route to this ctx's per-ctx perf ring */

	/*
	 * Region must fully contain the report [vaddr, vaddr + size): a report
	 * straddling the region end is rejected by the drain.  With
	 * vaddr=0x1500 and size=PAGE_SIZE the region must reach >= 0x2500.
	 */
	r = damon_new_region(0x1000, 0x3000);
	if (!r) {
		put_pid(t->pid);
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_drained();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_drained();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 1);
	KUNIT_EXPECT_GT(test, after, before);

	put_pid(t->pid);
	damon_destroy_ctx(ctx);
}

/*
 * Test B: a vaddr entry whose thread group id matches no target is dropped.
 *
 * Same setup but inject with a thread group id no target carries.
 * probe_hits[0]==0 (probe_idx 1 stored 0-based), samples_no_region increments.
 */
static void damon_test_unified_vaddr_tgid_mismatch(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0,
		.vaddr     = 0x1500,
		.tgid      = 9999,	/* matches no target */
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = get_pid(task_tgid(current));
	if (!t->pid) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "pid alloc failed");
	}
	rep.ctx = ctx;

	/* wide enough for the report: the id mismatch is the sole reject */
	r = damon_new_region(0x1000, 0x3000);
	if (!r) {
		put_pid(t->pid);
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_no_region();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_no_region();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 0);
	KUNIT_EXPECT_GT(test, after, before);

	put_pid(t->pid);
	damon_destroy_ctx(ctx);
}

/*
 * Test C: paddr entry drains correctly (no id filter for paddr ops).
 *
 * Create a paddr ctx (no pid), region [0x10000, 0x20000).
 * Inject: paddr=0x15000, vaddr=0, probe_idx=1, ctx=ctx.
 * After drain: probe_hits[0]==1 (probe_idx 1 stored 0-based).
 */
static void damon_test_unified_paddr_no_regression(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0x15000,
		.vaddr     = 0,
		.tid       = 0,
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;	/* paddr target: no pid */
	damon_test_set_paddr_ctx(ctx);
	rep.ctx = ctx;

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_drained();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_drained();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 1);
	KUNIT_EXPECT_GT(test, after, before);

	damon_destroy_ctx(ctx);
}

/* Reports add one probe hit per interval and never change nr_accesses. */
static void damon_test_report_hits_only(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0x15000,
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;
	damon_test_set_paddr_ctx(ctx);
	rep.ctx = ctx;

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	preempt_disable();
	damon_report_access(&rep);
	damon_report_access(&rep);
	preempt_enable();
	kdamond_check_reported_accesses(ctx);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, 0u);
	KUNIT_EXPECT_EQ(test, r->probe_hits[0], 1);

	/* a report drained in the next interval adds another hit */
	ctx->passed_sample_intervals++;
	rep.report_jiffies = jiffies;
	preempt_disable();
	damon_report_access(&rep);
	preempt_enable();
	kdamond_check_reported_accesses(ctx);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, 0u);
	KUNIT_EXPECT_EQ(test, r->probe_hits[0], 2);

	damon_destroy_ctx(ctx);
}

/* The same holds with a probe weight set (data attributes-only mode). */
static void damon_test_report_hits_only_weighted(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_probe *p;
	struct damon_access_report rep = {
		.paddr     = 0x15000,
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}
	damon_for_each_probe(p, ctx)
		p->weight = 1;

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;
	damon_test_set_paddr_ctx(ctx);
	rep.ctx = ctx;

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	preempt_disable();
	damon_report_access(&rep);
	damon_report_access(&rep);
	preempt_enable();
	kdamond_check_reported_accesses(ctx);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, 0u);
	KUNIT_EXPECT_EQ(test, r->probe_hits[0], 1);

	ctx->passed_sample_intervals++;
	preempt_disable();
	damon_report_access(&rep);
	preempt_enable();
	kdamond_check_reported_accesses(ctx);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, 0u);
	KUNIT_EXPECT_EQ(test, r->probe_hits[0], 2);

	damon_destroy_ctx(ctx);
}

static void damon_test_ring1_perf_credit(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0x15000,
		.vaddr     = 0,
		.tid       = 0,
		.probe_idx = 1,	/* -> per-ctx perf ring */
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;
	int hits;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;
	damon_test_set_paddr_ctx(ctx);
	rep.ctx = ctx;

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_drained();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_drained();

	hits = 0;
	damon_for_each_region(r, t)
		hits += r->probe_hits[0];

	KUNIT_EXPECT_EQ(test, hits, 1);
	KUNIT_EXPECT_GT(test, after, before);

	damon_destroy_ctx(ctx);
}

/*
 * Test per-context perf ring isolation.
 *
 * Two independent perf ctxs (each with its own event-driven probe and its own
 * per-ctx perf ring) each receive one probe_idx=1 report tagged with their
 * respective ctx.  Each ctx must credit exactly its own report and see nothing
 * from the other: perf reports route to the owning ctx's ring, and two
 * perf-driven ctxs coexist without sharing a ring.
 */
static void damon_test_perf_per_ctx_isolation(struct kunit *test)
{
	struct damon_ctx *ctx_a, *ctx_b;
	struct damon_target *ta, *tb;
	struct damon_region *ra, *rb;
	struct damon_access_report rep_a = {
		.paddr = 0x15000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	struct damon_access_report rep_b = {
		.paddr = 0x35000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	int hits_a, hits_b;

	ctx_a = damon_new_ctx();
	ctx_b = damon_new_ctx();
	if (!ctx_a || !ctx_b) {
		if (ctx_a)
			damon_destroy_ctx(ctx_a);
		if (ctx_b)
			damon_destroy_ctx(ctx_b);
		kunit_skip(test, "ctx alloc failed");
	}
	if (damon_test_attach_perf_probe(ctx_a) ||
			damon_test_attach_perf_probe(ctx_b)) {
		damon_destroy_ctx(ctx_a);
		damon_destroy_ctx(ctx_b);
		kunit_skip(test, "perf probe alloc failed");
	}

	ta = damon_new_target();
	tb = damon_new_target();
	if (!ta || !tb) {
		if (ta)
			damon_free_target(ta);
		if (tb)
			damon_free_target(tb);
		damon_destroy_ctx(ctx_a);
		damon_destroy_ctx(ctx_b);
		kunit_skip(test, "target alloc failed");
	}
	ta->pid = NULL;
	tb->pid = NULL;
	damon_test_set_paddr_ctx(ctx_a);
	damon_test_set_paddr_ctx(ctx_b);

	ra = damon_new_region(0x10000, 0x20000);	/* holds rep_a paddr */
	rb = damon_new_region(0x30000, 0x40000);	/* holds rep_b paddr */
	if (!ra || !rb) {
		if (ra)
			damon_free_region(ra);
		if (rb)
			damon_free_region(rb);
		damon_free_target(ta);
		damon_free_target(tb);
		damon_destroy_ctx(ctx_a);
		damon_destroy_ctx(ctx_b);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(ra, ta);
	damon_add_target(ctx_a, ta);
	damon_add_region(rb, tb);
	damon_add_target(ctx_b, tb);

	/* Each report is tagged with its owning ctx. */
	rep_a.ctx = ctx_a;
	rep_b.ctx = ctx_b;
	rep_a.report_jiffies = jiffies;
	rep_b.report_jiffies = jiffies;

	/*
	 * Report into both ctx rings, then drain each ctx.  ctx_a must credit
	 * only rep_a; ctx_b must credit only rep_b -- no cross-talk.
	 */
	damon_report_access(&rep_a);
	damon_report_access(&rep_b);
	kdamond_check_reported_accesses(ctx_a);
	kdamond_check_reported_accesses(ctx_b);

	hits_a = 0;
	damon_for_each_region(ra, ta)
		hits_a += ra->probe_hits[0];
	hits_b = 0;
	damon_for_each_region(rb, tb)
		hits_b += rb->probe_hits[0];

	/* each ctx credited its own report only */
	KUNIT_EXPECT_EQ(test, hits_a, 1);
	KUNIT_EXPECT_EQ(test, hits_b, 1);

	damon_destroy_ctx(ctx_a);
	damon_destroy_ctx(ctx_b);
}

/*
 * Test the queued/dropped return value, and that a ring-full drop is counted
 * as ring-full rather than busy-guard.
 *
 * A per-context perf ring is private to its ctx, so a freshly created ctx
 * starts with an empty ring nobody else writes to and the counts are exact.
 * Preemption is held across the loop so every report targets the same CPU's
 * ring, per the SPSC invariant damon_report_access() documents.
 *
 * A ring holds DAMON_REPORT_RING_SIZE - 1 entries (one slot is kept empty to
 * distinguish full from empty), so exactly that many reports are queued and
 * every one after that is dropped.
 */
static void damon_test_report_return_value(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_access_report rep = {
		.paddr = 0x15000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	unsigned long full_before, busy_before;
	unsigned int queued = 0, dropped = 0;
	int i;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}
	rep.ctx = ctx;

	preempt_disable();
	full_before = damon_get_report_ring_full();
	busy_before = damon_get_report_busy_drop();

	/* One past capacity, so the last iteration must be a drop. */
	for (i = 0; i < DAMON_REPORT_RING_SIZE; i++) {
		if (damon_report_access(&rep))
			queued++;
		else
			dropped++;
	}
	preempt_enable();

	KUNIT_EXPECT_EQ(test, queued, (unsigned int)DAMON_REPORT_RING_SIZE - 1);
	KUNIT_EXPECT_EQ(test, dropped, 1u);
	/* No NMI nests here, so the drop must be the full ring. */
	KUNIT_EXPECT_GT(test, damon_get_report_ring_full(), full_before);
	KUNIT_EXPECT_EQ(test, damon_get_report_busy_drop(), busy_before);

	damon_destroy_ctx(ctx);
}

/*
 * Test that draining restores capacity: fill the ring, drain it via the
 * dispatcher, then report again and expect the report to be queued.
 */
static void damon_test_report_drain_restores_capacity(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr = 0x15000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	bool queued;
	int i;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;
	damon_test_set_paddr_ctx(ctx);
	rep.ctx = ctx;

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	/* Fill the ring: the last report is dropped. */
	preempt_disable();
	for (i = 0; i < DAMON_REPORT_RING_SIZE; i++)
		damon_report_access(&rep);
	queued = damon_report_access(&rep);
	preempt_enable();
	KUNIT_EXPECT_FALSE(test, queued);

	kdamond_check_reported_accesses(ctx);

	/* Capacity is back. */
	KUNIT_EXPECT_TRUE(test, damon_report_access(&rep));

	damon_destroy_ctx(ctx);
}

/*
 * Test that a report is matched by the address space of the target rather than
 * by which address it carries.
 *
 * Create a paddr ctx with an event-driven probe, region [0x10000, 0x20000).
 * Inject a report whose vaddr falls inside that region and whose paddr falls
 * outside it.  A paddr target matches the paddr, so the report finds no
 * region and is counted as such.
 */
static void damon_test_report_addr_space_keyed(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	struct damon_access_report rep = {
		.paddr     = 0x95000,	/* outside the region */
		.vaddr     = 0x15000,	/* inside the region */
		.tid       = 0,
		.probe_idx = 1,
		.size      = PAGE_SIZE,
	};
	unsigned long before, after;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}
	rep.ctx = ctx;	/* route to this ctx's per-ctx perf ring */

	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc failed");
	}
	t->pid = NULL;	/* paddr target: no pid */
	damon_test_set_paddr_ctx(ctx);

	r = damon_new_region(0x10000, 0x20000);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc failed");
	}
	damon_add_region(r, t);
	damon_add_target(ctx, t);

	rep.report_jiffies = jiffies;
	before = damon_get_samples_no_region();
	damon_report_access(&rep);
	kdamond_check_reported_accesses(ctx);
	after = damon_get_samples_no_region();

	/* The vaddr was not used to match a paddr target. */
	KUNIT_EXPECT_GT(test, after, before);
	damon_for_each_region(r, t)
		KUNIT_EXPECT_EQ(test, r->probe_hits[0], 0);

	damon_destroy_ctx(ctx);
}

/*
 * Test that filling a perf ring beyond capacity increments the ring-full
 * counter.  The existing damon_test_report_drain_restores_capacity verifies
 * capacity returns after a drain; this verifies the counter side of the
 * same overflow.
 */
static void damon_test_ring_full_counter_increments(struct kunit *test)
{
	struct damon_ctx *ctx;
	struct damon_access_report rep = {
		.paddr = 0x15000, .probe_idx = 1, .size = PAGE_SIZE,
	};
	unsigned long full_before;
	int i;

	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc failed");
	if (damon_test_attach_perf_probe(ctx)) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "perf probe alloc failed");
	}
	damon_test_set_paddr_ctx(ctx);
	rep.ctx = ctx;

	full_before = damon_get_report_ring_full();

	/* Fill the ring to capacity, then push one more to trigger overflow. */
	preempt_disable();
	for (i = 0; i < DAMON_REPORT_RING_SIZE; i++)
		damon_report_access(&rep);
	damon_report_access(&rep);   /* this one overflows */
	preempt_enable();

	KUNIT_EXPECT_GT(test, damon_get_report_ring_full(), full_before);

	damon_destroy_ctx(ctx);
}

static struct kunit_case damon_drain_test_cases[] = {
	KUNIT_CASE(damon_test_unified_vaddr_match),
	KUNIT_CASE(damon_test_unified_vaddr_tgid_mismatch),
	KUNIT_CASE(damon_test_unified_paddr_no_regression),
	KUNIT_CASE(damon_test_report_hits_only),
	KUNIT_CASE(damon_test_report_hits_only_weighted),
	KUNIT_CASE(damon_test_ring1_perf_credit),
	KUNIT_CASE(damon_test_perf_per_ctx_isolation),
	KUNIT_CASE(damon_test_report_return_value),
	KUNIT_CASE(damon_test_report_drain_restores_capacity),
	KUNIT_CASE(damon_test_report_addr_space_keyed),
	KUNIT_CASE(damon_test_ring_full_counter_increments),
	{}
};

static struct kunit_suite damon_drain_test_suite = {
	.name = "damon_drain",
	.test_cases = damon_drain_test_cases,
};

kunit_test_suite(damon_drain_test_suite);

#endif /* _DAMON_DRAIN_KUNIT_H */

#endif	/* CONFIG_DAMON_KUNIT_TEST */
