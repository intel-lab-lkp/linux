// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the tlob RV monitor.
 *
 * tlob_automaton:         DA transition table coverage.
 * tlob_task_api:          tlob_start_task()/tlob_stop_task() lifecycle and errors.
 * tlob_sched_integration: on/off-CPU accounting across real context switches.
 * tlob_trace_output:      tlob_budget_exceeded tracepoint field verification.
 * tlob_event_buf:         ring buffer push, overflow, and wakeup.
 * tlob_parse_uprobe:      uprobe format string parser acceptance and rejection.
 *
 * The duplicate-(binary, offset_start) constraint enforced by tlob_add_uprobe()
 * is not covered here: that function calls kern_path() and requires a real
 * filesystem, which is outside the scope of unit tests. It is covered by the
 * uprobe_duplicate_offset case in tools/testing/selftests/rv/test_tlob.sh.
 */
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/tracepoint.h>

/*
 * Pull in the rv tracepoint declarations so that
 * register_trace_tlob_budget_exceeded() is available.
 * No CREATE_TRACE_POINTS here  --  the tracepoint implementation lives in rv.c.
 */
#include <rv_trace.h>

#include "tlob.h"

/*
 * da_handle_event_tlob - apply one automaton transition on @da_mon.
 *
 * This helper is used only by the KUnit automaton suite. It applies the
 * tlob transition table directly on a supplied da_monitor without touching
 * per-task slots, tracepoints, or timers.
 */
static void da_handle_event_tlob(struct da_monitor *da_mon,
				 enum events_tlob event)
{
	enum states_tlob curr_state = (enum states_tlob)da_mon->curr_state;
	enum states_tlob next_state =
		(enum states_tlob)automaton_tlob.function[curr_state][event];

	if (next_state != INVALID_STATE)
		da_mon->curr_state = next_state;
}

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

/*
 * Suite 1: automaton state-machine transitions
 */

/* unmonitored -> trace_start -> on_cpu */
static void tlob_unmonitored_to_on_cpu(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = unmonitored_tlob };

	da_handle_event_tlob(&mon, trace_start_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)on_cpu_tlob);
}

/* on_cpu -> switch_out -> off_cpu */
static void tlob_on_cpu_switch_out(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = on_cpu_tlob };

	da_handle_event_tlob(&mon, switch_out_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)off_cpu_tlob);
}

/* off_cpu -> switch_in -> on_cpu */
static void tlob_off_cpu_switch_in(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = off_cpu_tlob };

	da_handle_event_tlob(&mon, switch_in_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)on_cpu_tlob);
}

/* on_cpu -> budget_expired -> unmonitored */
static void tlob_on_cpu_budget_expired(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = on_cpu_tlob };

	da_handle_event_tlob(&mon, budget_expired_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)unmonitored_tlob);
}

/* off_cpu -> budget_expired -> unmonitored */
static void tlob_off_cpu_budget_expired(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = off_cpu_tlob };

	da_handle_event_tlob(&mon, budget_expired_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)unmonitored_tlob);
}

/* on_cpu -> trace_stop -> unmonitored */
static void tlob_on_cpu_trace_stop(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = on_cpu_tlob };

	da_handle_event_tlob(&mon, trace_stop_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)unmonitored_tlob);
}

/* off_cpu -> trace_stop -> unmonitored */
static void tlob_off_cpu_trace_stop(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = off_cpu_tlob };

	da_handle_event_tlob(&mon, trace_stop_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)unmonitored_tlob);
}

/* budget_expired -> unmonitored; a single trace_start re-enters on_cpu. */
static void tlob_violation_then_restart(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = unmonitored_tlob };

	da_handle_event_tlob(&mon, trace_start_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)on_cpu_tlob);

	da_handle_event_tlob(&mon, budget_expired_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)unmonitored_tlob);

	/* Single trace_start is sufficient to re-enter on_cpu */
	da_handle_event_tlob(&mon, trace_start_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)on_cpu_tlob);

	da_handle_event_tlob(&mon, trace_stop_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)unmonitored_tlob);
}

/* off_cpu self-loops on switch_out and sched_wakeup. */
static void tlob_off_cpu_self_loops(struct kunit *test)
{
	static const enum events_tlob events[] = {
		switch_out_tlob, sched_wakeup_tlob,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(events); i++) {
		struct da_monitor mon = { .curr_state = off_cpu_tlob };

		da_handle_event_tlob(&mon, events[i]);
		KUNIT_EXPECT_EQ_MSG(test, (int)mon.curr_state,
				    (int)off_cpu_tlob,
				    "event %u should self-loop in off_cpu",
				    events[i]);
	}
}

/* on_cpu self-loops on sched_wakeup. */
static void tlob_on_cpu_self_loops(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = on_cpu_tlob };

	da_handle_event_tlob(&mon, sched_wakeup_tlob);
	KUNIT_EXPECT_EQ_MSG(test, (int)mon.curr_state, (int)on_cpu_tlob,
			    "sched_wakeup should self-loop in on_cpu");
}

/* Scheduling events in unmonitored self-loop (no state change). */
static void tlob_unmonitored_ignores_sched(struct kunit *test)
{
	static const enum events_tlob events[] = {
		switch_in_tlob, switch_out_tlob, sched_wakeup_tlob,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(events); i++) {
		struct da_monitor mon = { .curr_state = unmonitored_tlob };

		da_handle_event_tlob(&mon, events[i]);
		KUNIT_EXPECT_EQ_MSG(test, (int)mon.curr_state,
				    (int)unmonitored_tlob,
				    "event %u should self-loop in unmonitored",
				    events[i]);
	}
}

static void tlob_full_happy_path(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = unmonitored_tlob };

	da_handle_event_tlob(&mon, trace_start_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)on_cpu_tlob);

	da_handle_event_tlob(&mon, switch_out_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)off_cpu_tlob);

	da_handle_event_tlob(&mon, switch_in_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)on_cpu_tlob);

	da_handle_event_tlob(&mon, trace_stop_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)unmonitored_tlob);
}

static void tlob_multiple_switches(struct kunit *test)
{
	struct da_monitor mon = { .curr_state = unmonitored_tlob };
	int i;

	da_handle_event_tlob(&mon, trace_start_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)on_cpu_tlob);

	for (i = 0; i < 3; i++) {
		da_handle_event_tlob(&mon, switch_out_tlob);
		KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)off_cpu_tlob);
		da_handle_event_tlob(&mon, switch_in_tlob);
		KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)on_cpu_tlob);
	}

	da_handle_event_tlob(&mon, trace_stop_tlob);
	KUNIT_EXPECT_EQ(test, (int)mon.curr_state, (int)unmonitored_tlob);
}

static struct kunit_case tlob_automaton_cases[] = {
	KUNIT_CASE(tlob_unmonitored_to_on_cpu),
	KUNIT_CASE(tlob_on_cpu_switch_out),
	KUNIT_CASE(tlob_off_cpu_switch_in),
	KUNIT_CASE(tlob_on_cpu_budget_expired),
	KUNIT_CASE(tlob_off_cpu_budget_expired),
	KUNIT_CASE(tlob_on_cpu_trace_stop),
	KUNIT_CASE(tlob_off_cpu_trace_stop),
	KUNIT_CASE(tlob_off_cpu_self_loops),
	KUNIT_CASE(tlob_on_cpu_self_loops),
	KUNIT_CASE(tlob_unmonitored_ignores_sched),
	KUNIT_CASE(tlob_full_happy_path),
	KUNIT_CASE(tlob_violation_then_restart),
	KUNIT_CASE(tlob_multiple_switches),
	{}
};

static struct kunit_suite tlob_automaton_suite = {
	.name       = "tlob_automaton",
	.test_cases = tlob_automaton_cases,
};

/*
 * Suite 2: task registration API
 */

/* Basic start/stop cycle */
static void tlob_start_stop_ok(struct kunit *test)
{
	int ret;

	ret = tlob_start_task(current, 10000000 /* 10 s, won't fire */, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), 0);
}

/* Double start must return -EEXIST. */
static void tlob_double_start(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, tlob_start_task(current, 10000000, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, tlob_start_task(current, 10000000, NULL, 0), -EEXIST);
	tlob_stop_task(current);
}

/* Stop without start must return -ESRCH. */
static void tlob_stop_without_start(struct kunit *test)
{
	tlob_stop_task(current);  /* clear any stale entry first */
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -ESRCH);
}

/*
 * A 1 us budget fires before tlob_stop_task() is called. Either the
 * timer wins (-ESRCH) or we are very fast (0); both are valid.
 */
static void tlob_immediate_deadline(struct kunit *test)
{
	int ret = tlob_start_task(current, 1 /* 1 us - fires almost immediately */, NULL, 0);

	KUNIT_ASSERT_EQ(test, ret, 0);
	/* Let the 1 us timer fire */
	udelay(100);
	/*
	 * By now the hrtimer has almost certainly fired. Either it has
	 * (returns -ESRCH) or we were very fast (returns 0). Both are
	 * acceptable; just ensure no crash and the table is clean after.
	 */
	ret = tlob_stop_task(current);
	KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ESRCH);
}

/*
 * Fill the table to TLOB_MAX_MONITORED using kthreads (each needs a
 * distinct task_struct), then verify the next start returns -ENOSPC.
 */
struct tlob_waiter_ctx {
	struct completion start;
	struct completion done;
};

static int tlob_waiter_fn(void *arg)
{
	struct tlob_waiter_ctx *ctx = arg;

	wait_for_completion(&ctx->start);
	complete(&ctx->done);
	return 0;
}

static void tlob_enospc(struct kunit *test)
{
	struct tlob_waiter_ctx *ctxs;
	struct task_struct **threads;
	int i, ret;

	ctxs = kunit_kcalloc(test, TLOB_MAX_MONITORED,
			     sizeof(*ctxs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctxs);

	threads = kunit_kcalloc(test, TLOB_MAX_MONITORED,
				sizeof(*threads), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, threads);

	/* Start TLOB_MAX_MONITORED kthreads and monitor each */
	for (i = 0; i < TLOB_MAX_MONITORED; i++) {
		init_completion(&ctxs[i].start);
		init_completion(&ctxs[i].done);

		threads[i] = kthread_run(tlob_waiter_fn, &ctxs[i],
					 "tlob_waiter_%d", i);
		if (IS_ERR(threads[i])) {
			KUNIT_FAIL(test, "kthread_run failed at i=%d", i);
			threads[i] = NULL;
			goto cleanup;
		}
		get_task_struct(threads[i]);

		ret = tlob_start_task(threads[i], 10000000, NULL, 0);
		if (ret != 0) {
			KUNIT_FAIL(test, "tlob_start_task failed at i=%d: %d",
				   i, ret);
			put_task_struct(threads[i]);
			complete(&ctxs[i].start);
			goto cleanup;
		}
	}

	/* The table is now full: one more must fail with -ENOSPC */
	ret = tlob_start_task(current, 10000000, NULL, 0);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);

cleanup:
	/*
	 * Two-pass cleanup: cancel tlob monitoring and unblock kthreads first,
	 * then kthread_stop() to wait for full exit before releasing refs.
	 */
	for (i = 0; i < TLOB_MAX_MONITORED; i++) {
		if (!threads[i])
			break;
		tlob_stop_task(threads[i]);
		complete(&ctxs[i].start);
	}
	for (i = 0; i < TLOB_MAX_MONITORED; i++) {
		if (!threads[i])
			break;
		kthread_stop(threads[i]);
		put_task_struct(threads[i]);
	}
}

/*
 * A kthread holds a mutex for 80 ms; arm a 10 ms budget, burn ~1 ms
 * on-CPU, then block on the mutex. The timer fires off-CPU; stop
 * must return -ESRCH.
 */
struct tlob_holder_ctx {
	struct mutex		lock;
	struct completion	ready;
	unsigned int		hold_ms;
};

static int tlob_holder_fn(void *arg)
{
	struct tlob_holder_ctx *ctx = arg;

	mutex_lock(&ctx->lock);
	complete(&ctx->ready);
	msleep(ctx->hold_ms);
	mutex_unlock(&ctx->lock);
	return 0;
}

static void tlob_deadline_fires_off_cpu(struct kunit *test)
{
	struct tlob_holder_ctx ctx = { .hold_ms = 80 };
	struct task_struct *holder;
	ktime_t t0;
	int ret;

	mutex_init(&ctx.lock);
	init_completion(&ctx.ready);

	holder = kthread_run(tlob_holder_fn, &ctx, "tlob_holder_kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, holder);
	wait_for_completion(&ctx.ready);

	/* Arm 10 ms budget while kthread holds the mutex. */
	ret = tlob_start_task(current, 10000, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Phase 1: burn ~1 ms on-CPU to exercise on_cpu accounting. */
	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 1000)
		cpu_relax();

	/*
	 * Phase 2: block on the mutex -> on_cpu->off_cpu transition.
	 * The 10 ms budget fires while we are off-CPU.
	 */
	mutex_lock(&ctx.lock);
	mutex_unlock(&ctx.lock);

	/* Timer already fired and removed the entry -> -ESRCH */
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -ESRCH);
}

/* Arm a 1 ms budget and busy-spin for 50 ms; timer fires on-CPU. */
static void tlob_deadline_fires_on_cpu(struct kunit *test)
{
	ktime_t t0;
	int ret;

	ret = tlob_start_task(current, 1000 /* 1 ms */, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Busy-spin 50 ms - 50x the budget */
	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 50000)
		cpu_relax();

	/* Timer fired during the spin; entry is gone */
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -ESRCH);
}

/*
 * Start three tasks, call tlob_destroy_monitor() + tlob_init_monitor(),
 * and verify the table is empty afterwards.
 */
static int tlob_dummy_fn(void *arg)
{
	wait_for_completion((struct completion *)arg);
	return 0;
}

static void tlob_stop_all_cleanup(struct kunit *test)
{
	struct completion done1, done2;
	struct task_struct *t1, *t2;
	int ret;

	init_completion(&done1);
	init_completion(&done2);

	t1 = kthread_run(tlob_dummy_fn, &done1, "tlob_dummy1");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t1);
	get_task_struct(t1);

	t2 = kthread_run(tlob_dummy_fn, &done2, "tlob_dummy2");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t2);
	get_task_struct(t2);

	KUNIT_ASSERT_EQ(test, tlob_start_task(current, 10000000, NULL, 0), 0);
	KUNIT_ASSERT_EQ(test, tlob_start_task(t1, 10000000, NULL, 0), 0);
	KUNIT_ASSERT_EQ(test, tlob_start_task(t2, 10000000, NULL, 0), 0);

	/* Destroy clears all entries via tlob_stop_all() */
	tlob_destroy_monitor();
	ret = tlob_init_monitor();
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Table must be empty now */
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -ESRCH);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(t1), -ESRCH);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(t2), -ESRCH);

	complete(&done1);
	complete(&done2);
	/*
	 * completions live on stack; wait for kthreads to exit before return.
	 */
	kthread_stop(t1);
	kthread_stop(t2);
	put_task_struct(t1);
	put_task_struct(t2);
}

/* A threshold that overflows ktime_t must be rejected with -ERANGE. */
static void tlob_overflow_threshold(struct kunit *test)
{
	/* KTIME_MAX / NSEC_PER_USEC + 1 overflows ktime_t */
	u64 too_large = (u64)(KTIME_MAX / NSEC_PER_USEC) + 1;

	KUNIT_EXPECT_EQ(test,
		tlob_start_task(current, too_large, NULL, 0),
		-ERANGE);
}

static int tlob_task_api_suite_init(struct kunit_suite *suite)
{
	return tlob_init_monitor();
}

static void tlob_task_api_suite_exit(struct kunit_suite *suite)
{
	tlob_destroy_monitor();
}

static struct kunit_case tlob_task_api_cases[] = {
	KUNIT_CASE(tlob_start_stop_ok),
	KUNIT_CASE(tlob_double_start),
	KUNIT_CASE(tlob_stop_without_start),
	KUNIT_CASE(tlob_immediate_deadline),
	KUNIT_CASE(tlob_enospc),
	KUNIT_CASE(tlob_overflow_threshold),
	KUNIT_CASE(tlob_deadline_fires_off_cpu),
	KUNIT_CASE(tlob_deadline_fires_on_cpu),
	KUNIT_CASE(tlob_stop_all_cleanup),
	{}
};

static struct kunit_suite tlob_task_api_suite = {
	.name       = "tlob_task_api",
	.suite_init = tlob_task_api_suite_init,
	.suite_exit = tlob_task_api_suite_exit,
	.test_cases = tlob_task_api_cases,
};

/*
 * Suite 3: scheduling integration
 */

struct tlob_ping_ctx {
	struct completion ping;
	struct completion pong;
};

static int tlob_ping_fn(void *arg)
{
	struct tlob_ping_ctx *ctx = arg;

	/* Wait for main to give us the CPU back */
	wait_for_completion(&ctx->ping);
	complete(&ctx->pong);
	return 0;
}

/* Force two context switches and verify stop returns 0 (within budget). */
static void tlob_sched_switch_accounting(struct kunit *test)
{
	struct tlob_ping_ctx ctx;
	struct task_struct *peer;
	int ret;

	init_completion(&ctx.ping);
	init_completion(&ctx.pong);

	peer = kthread_run(tlob_ping_fn, &ctx, "tlob_ping_kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, peer);

	/* Arm a generous 5 s budget so the timer never fires */
	ret = tlob_start_task(current, 5000000, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * complete(ping) -> peer runs, forcing a context switch out and back.
	 */
	complete(&ctx.ping);
	wait_for_completion(&ctx.pong);

	/*
	 * Back on CPU after one off-CPU interval; stop must return 0.
	 */
	ret = tlob_stop_task(current);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/*
 * Verify that monitoring a kthread (not current) works: start on behalf
 * of a kthread, let it block, then stop it.
 */
static int tlob_block_fn(void *arg)
{
	struct completion *done = arg;

	/* Block briefly, exercising off_cpu accounting for this task */
	msleep(20);
	complete(done);
	return 0;
}

static void tlob_monitor_other_task(struct kunit *test)
{
	struct completion done;
	struct task_struct *target;
	int ret;

	init_completion(&done);

	target = kthread_run(tlob_block_fn, &done, "tlob_target_kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, target);
	get_task_struct(target);

	/* Arm a 5 s budget for the target task */
	ret = tlob_start_task(target, 5000000, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	wait_for_completion(&done);

	/*
	 * Target has finished; stop_task may return 0 (still in htable)
	 * or -ESRCH (kthread exited and timer fired / entry cleaned up).
	 */
	ret = tlob_stop_task(target);
	KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ESRCH);
	put_task_struct(target);
}

static int tlob_sched_suite_init(struct kunit_suite *suite)
{
	return tlob_init_monitor();
}

static void tlob_sched_suite_exit(struct kunit_suite *suite)
{
	tlob_destroy_monitor();
}

static struct kunit_case tlob_sched_integration_cases[] = {
	KUNIT_CASE(tlob_sched_switch_accounting),
	KUNIT_CASE(tlob_monitor_other_task),
	{}
};

static struct kunit_suite tlob_sched_integration_suite = {
	.name       = "tlob_sched_integration",
	.suite_init = tlob_sched_suite_init,
	.suite_exit = tlob_sched_suite_exit,
	.test_cases = tlob_sched_integration_cases,
};

/*
 * Suite 4: ftrace tracepoint field verification
 */

/* Capture fields from trace_tlob_budget_exceeded for inspection. */
struct tlob_exceeded_capture {
	atomic_t	fired;		/* 1 after first call */
	pid_t		pid;
	u64		threshold_us;
	u64		on_cpu_us;
	u64		off_cpu_us;
	u32		switches;
	bool		state_is_on_cpu;
	u64		tag;
};

static void
probe_tlob_budget_exceeded(void *data,
			   struct task_struct *task, u64 threshold_us,
			   u64 on_cpu_us, u64 off_cpu_us,
			   u32 switches, bool state_is_on_cpu, u64 tag)
{
	struct tlob_exceeded_capture *cap = data;

	/* Only capture the first event to avoid races. */
	if (atomic_cmpxchg(&cap->fired, 0, 1) != 0)
		return;

	cap->pid		= task->pid;
	cap->threshold_us	= threshold_us;
	cap->on_cpu_us		= on_cpu_us;
	cap->off_cpu_us		= off_cpu_us;
	cap->switches		= switches;
	cap->state_is_on_cpu	= state_is_on_cpu;
	cap->tag		= tag;
}

/*
 * Arm a 2 ms budget and busy-spin for 60 ms. Verify the tracepoint fires
 * once with matching threshold, correct pid, and total time >= budget.
 *
 * state_is_on_cpu is not asserted: preemption during the spin makes it
 * non-deterministic.
 */
static void tlob_trace_budget_exceeded_on_cpu(struct kunit *test)
{
	struct tlob_exceeded_capture cap = {};
	const u64 threshold_us = 2000; /* 2 ms */
	ktime_t t0;
	int ret;

	atomic_set(&cap.fired, 0);

	ret = register_trace_tlob_budget_exceeded(probe_tlob_budget_exceeded,
						  &cap);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = tlob_start_task(current, threshold_us, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Busy-spin 60 ms  --  30x the budget */
	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 60000)
		cpu_relax();

	/* Entry removed by timer; stop returns -ESRCH */
	tlob_stop_task(current);

	/*
	 * Synchronise: ensure the probe callback has completed before we
	 * read the captured fields.
	 */
	tracepoint_synchronize_unregister();
	unregister_trace_tlob_budget_exceeded(probe_tlob_budget_exceeded, &cap);

	KUNIT_EXPECT_EQ(test, atomic_read(&cap.fired), 1);
	KUNIT_EXPECT_EQ(test, (int)cap.pid, (int)current->pid);
	KUNIT_EXPECT_EQ(test, cap.threshold_us, threshold_us);
	/* Total elapsed must cover at least the budget */
	KUNIT_EXPECT_GE(test, cap.on_cpu_us + cap.off_cpu_us, threshold_us);
}

/*
 * Holder kthread grabs a mutex for 80 ms; arm 10 ms budget, burn ~1 ms
 * on-CPU, then block on the mutex. Timer fires off-CPU. Verify:
 * state_is_on_cpu == false, switches >= 1, off_cpu_us > 0.
 */
static void tlob_trace_budget_exceeded_off_cpu(struct kunit *test)
{
	struct tlob_exceeded_capture cap = {};
	struct tlob_holder_ctx ctx = { .hold_ms = 80 };
	struct task_struct *holder;
	const u64 threshold_us = 10000; /* 10 ms */
	ktime_t t0;
	int ret;

	atomic_set(&cap.fired, 0);

	mutex_init(&ctx.lock);
	init_completion(&ctx.ready);

	holder = kthread_run(tlob_holder_fn, &ctx, "tlob_holder2_kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, holder);
	wait_for_completion(&ctx.ready);

	ret = register_trace_tlob_budget_exceeded(probe_tlob_budget_exceeded,
						  &cap);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = tlob_start_task(current, threshold_us, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Phase 1: ~1 ms on-CPU */
	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 1000)
		cpu_relax();

	/* Phase 2: block -> off-CPU; timer fires here */
	mutex_lock(&ctx.lock);
	mutex_unlock(&ctx.lock);

	tlob_stop_task(current);

	tracepoint_synchronize_unregister();
	unregister_trace_tlob_budget_exceeded(probe_tlob_budget_exceeded, &cap);

	KUNIT_EXPECT_EQ(test, atomic_read(&cap.fired), 1);
	KUNIT_EXPECT_EQ(test, cap.threshold_us, threshold_us);
	/* Violation happened off-CPU */
	KUNIT_EXPECT_FALSE(test, cap.state_is_on_cpu);
	/* At least the switch_out event was counted */
	KUNIT_EXPECT_GE(test, (u64)cap.switches, (u64)1);
	/* Off-CPU time must be non-zero */
	KUNIT_EXPECT_GT(test, cap.off_cpu_us, (u64)0);
}

/* threshold_us in the tracepoint must exactly match the start argument. */
static void tlob_trace_threshold_field_accuracy(struct kunit *test)
{
	static const u64 thresholds[] = { 500, 1000, 3000 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(thresholds); i++) {
		struct tlob_exceeded_capture cap = {};
		ktime_t t0;
		int ret;

		atomic_set(&cap.fired, 0);

		ret = register_trace_tlob_budget_exceeded(
			probe_tlob_budget_exceeded, &cap);
		KUNIT_ASSERT_EQ(test, ret, 0);

		ret = tlob_start_task(current, thresholds[i], NULL, 0);
		KUNIT_ASSERT_EQ(test, ret, 0);

		/* Spin for 20x the threshold to ensure timer fires */
		t0 = ktime_get();
		while (ktime_us_delta(ktime_get(), t0) <
		       (s64)(thresholds[i] * 20))
			cpu_relax();

		tlob_stop_task(current);

		tracepoint_synchronize_unregister();
		unregister_trace_tlob_budget_exceeded(
			probe_tlob_budget_exceeded, &cap);

		KUNIT_EXPECT_EQ_MSG(test, cap.threshold_us, thresholds[i],
				    "threshold mismatch for entry %u", i);
	}
}

static int tlob_trace_suite_init(struct kunit_suite *suite)
{
	int ret;

	ret = tlob_init_monitor();
	if (ret)
		return ret;
	return tlob_enable_hooks();
}

static void tlob_trace_suite_exit(struct kunit_suite *suite)
{
	tlob_disable_hooks();
	tlob_destroy_monitor();
}

static struct kunit_case tlob_trace_output_cases[] = {
	KUNIT_CASE(tlob_trace_budget_exceeded_on_cpu),
	KUNIT_CASE(tlob_trace_budget_exceeded_off_cpu),
	KUNIT_CASE(tlob_trace_threshold_field_accuracy),
	{}
};

static struct kunit_suite tlob_trace_output_suite = {
	.name       = "tlob_trace_output",
	.suite_init = tlob_trace_suite_init,
	.suite_exit = tlob_trace_suite_exit,
	.test_cases = tlob_trace_output_cases,
};

/* Suite 5: ring buffer */

/*
 * Allocate a synthetic rv_file_priv for ring buffer tests. Uses
 * kunit_kzalloc() instead of __get_free_pages() since the ring is never
 * mmap'd here.
 */
static struct rv_file_priv *alloc_priv_kunit(struct kunit *test, u32 cap)
{
	struct rv_file_priv *priv;
	struct tlob_ring *ring;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;

	ring = &priv->ring;

	ring->page = kunit_kzalloc(test, sizeof(struct tlob_mmap_page),
				   GFP_KERNEL);
	if (!ring->page)
		return NULL;

	ring->data = kunit_kzalloc(test, cap * sizeof(struct tlob_event),
				   GFP_KERNEL);
	if (!ring->data)
		return NULL;

	ring->mask            = cap - 1;
	ring->page->capacity  = cap;
	ring->page->version   = 1;
	ring->page->data_offset = PAGE_SIZE; /* nominal; not used in tests */
	ring->page->record_size = sizeof(struct tlob_event);
	spin_lock_init(&ring->lock);
	init_waitqueue_head(&priv->waitq);
	return priv;
}

/* Push one record and verify all fields survive the round-trip. */
static void tlob_event_push_one(struct kunit *test)
{
	struct rv_file_priv *priv;
	struct tlob_ring *ring;
	struct tlob_event in = {
		.tid		= 1234,
		.threshold_us	= 5000,
		.on_cpu_us	= 3000,
		.off_cpu_us	= 2000,
		.switches	= 3,
		.state		= 1,
	};
	struct tlob_event out = {};
	u32 tail;

	priv = alloc_priv_kunit(test, TLOB_RING_DEFAULT_CAP);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	ring = &priv->ring;

	tlob_event_push_kunit(priv, &in);

	/* One record written, none dropped */
	KUNIT_EXPECT_EQ(test, ring->page->data_head, 1u);
	KUNIT_EXPECT_EQ(test, ring->page->data_tail, 0u);
	KUNIT_EXPECT_EQ(test, ring->page->dropped,   0ull);

	/* Dequeue manually */
	tail = ring->page->data_tail;
	out  = ring->data[tail & ring->mask];
	ring->page->data_tail = tail + 1;

	KUNIT_EXPECT_EQ(test, out.tid,          in.tid);
	KUNIT_EXPECT_EQ(test, out.threshold_us, in.threshold_us);
	KUNIT_EXPECT_EQ(test, out.on_cpu_us,    in.on_cpu_us);
	KUNIT_EXPECT_EQ(test, out.off_cpu_us,   in.off_cpu_us);
	KUNIT_EXPECT_EQ(test, out.switches,     in.switches);
	KUNIT_EXPECT_EQ(test, out.state,        in.state);

	/* Ring is now empty */
	KUNIT_EXPECT_EQ(test, ring->page->data_head, ring->page->data_tail);
}

/*
 * Fill to capacity, push one more. Drop-new policy: head stays at cap,
 * dropped == 1, oldest record is preserved.
 */
static void tlob_event_push_overflow(struct kunit *test)
{
	struct rv_file_priv *priv;
	struct tlob_ring *ring;
	struct tlob_event ntf = {};
	struct tlob_event out = {};
	const u32 cap = TLOB_RING_MIN_CAP;
	u32 i;

	priv = alloc_priv_kunit(test, cap);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	ring = &priv->ring;

	/* Push cap + 1 records; tid encodes the sequence */
	for (i = 0; i <= cap; i++) {
		ntf.tid          = i;
		ntf.threshold_us = (u64)i * 1000;
		tlob_event_push_kunit(priv, &ntf);
	}

	/* Drop-new: head stopped at cap; one record was silently discarded */
	KUNIT_EXPECT_EQ(test, ring->page->data_head, cap);
	KUNIT_EXPECT_EQ(test, ring->page->data_tail, 0u);
	KUNIT_EXPECT_EQ(test, ring->page->dropped,   1ull);

	/* Oldest surviving record must be the first one pushed (tid == 0) */
	out = ring->data[ring->page->data_tail & ring->mask];
	KUNIT_EXPECT_EQ(test, out.tid, 0u);

	/* Drain the ring; the last record must have tid == cap - 1 */
	for (i = 0; i < cap; i++) {
		u32 tail = ring->page->data_tail;

		out = ring->data[tail & ring->mask];
		ring->page->data_tail = tail + 1;
	}
	KUNIT_EXPECT_EQ(test, out.tid, cap - 1);
	KUNIT_EXPECT_EQ(test, ring->page->data_head, ring->page->data_tail);
}

/* A freshly initialised ring is empty. */
static void tlob_event_empty(struct kunit *test)
{
	struct rv_file_priv *priv;
	struct tlob_ring *ring;

	priv = alloc_priv_kunit(test, TLOB_RING_DEFAULT_CAP);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	ring = &priv->ring;

	KUNIT_EXPECT_EQ(test, ring->page->data_head, 0u);
	KUNIT_EXPECT_EQ(test, ring->page->data_tail, 0u);
	KUNIT_EXPECT_EQ(test, ring->page->dropped,   0ull);
}

/* A kthread blocks on wait_event_interruptible(); pushing one record must
 * wake it within 1 s.
 */

struct tlob_wakeup_ctx {
	struct rv_file_priv	*priv;
	struct completion	 ready;
	struct completion	 done;
	int			 woke;
};

static int tlob_wakeup_thread(void *arg)
{
	struct tlob_wakeup_ctx *ctx = arg;
	struct tlob_ring *ring = &ctx->priv->ring;

	complete(&ctx->ready);

	wait_event_interruptible(ctx->priv->waitq,
		smp_load_acquire(&ring->page->data_head) !=
		READ_ONCE(ring->page->data_tail) ||
		kthread_should_stop());

	if (smp_load_acquire(&ring->page->data_head) !=
	    READ_ONCE(ring->page->data_tail))
		ctx->woke = 1;

	complete(&ctx->done);
	return 0;
}

static void tlob_ring_wakeup(struct kunit *test)
{
	struct rv_file_priv *priv;
	struct tlob_wakeup_ctx ctx;
	struct task_struct *t;
	struct tlob_event ev = { .tid = 99 };
	long timeout;

	priv = alloc_priv_kunit(test, TLOB_RING_DEFAULT_CAP);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	init_completion(&ctx.ready);
	init_completion(&ctx.done);
	ctx.priv = priv;
	ctx.woke = 0;

	t = kthread_run(tlob_wakeup_thread, &ctx, "tlob_wakeup_kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	get_task_struct(t);

	/* Let the kthread reach wait_event_interruptible */
	wait_for_completion(&ctx.ready);
	usleep_range(10000, 20000);

	/* Push one record  --  must wake the waiter */
	tlob_event_push_kunit(priv, &ev);

	timeout = wait_for_completion_timeout(&ctx.done, msecs_to_jiffies(1000));
	kthread_stop(t);
	put_task_struct(t);

	KUNIT_EXPECT_GT(test, timeout, 0L);
	KUNIT_EXPECT_EQ(test, ctx.woke, 1);
	KUNIT_EXPECT_EQ(test, priv->ring.page->data_head, 1u);
}

static struct kunit_case tlob_event_buf_cases[] = {
	KUNIT_CASE(tlob_event_push_one),
	KUNIT_CASE(tlob_event_push_overflow),
	KUNIT_CASE(tlob_event_empty),
	KUNIT_CASE(tlob_ring_wakeup),
	{}
};

static struct kunit_suite tlob_event_buf_suite = {
	.name       = "tlob_event_buf",
	.test_cases = tlob_event_buf_cases,
};

/* Suite 6: uprobe format string parser */

/* Happy path: decimal offsets, plain path. */
static void tlob_parse_decimal_offsets(struct kunit *test)
{
	char buf[] = "5000:4768:4848:/usr/bin/myapp";
	u64 thr; loff_t start, stop; char *path;

	KUNIT_EXPECT_EQ(test,
		tlob_parse_uprobe_line(buf, &thr, &path, &start, &stop),
		0);
	KUNIT_EXPECT_EQ(test, thr,      (u64)5000);
	KUNIT_EXPECT_EQ(test, start,    (loff_t)4768);
	KUNIT_EXPECT_EQ(test, stop,     (loff_t)4848);
	KUNIT_EXPECT_STREQ(test, path,  "/usr/bin/myapp");
}

/* Happy path: 0x-prefixed hex offsets. */
static void tlob_parse_hex_offsets(struct kunit *test)
{
	char buf[] = "10000:0x12a0:0x12f0:/usr/bin/myapp";
	u64 thr; loff_t start, stop; char *path;

	KUNIT_EXPECT_EQ(test,
		tlob_parse_uprobe_line(buf, &thr, &path, &start, &stop),
		0);
	KUNIT_EXPECT_EQ(test, start,   (loff_t)0x12a0);
	KUNIT_EXPECT_EQ(test, stop,    (loff_t)0x12f0);
	KUNIT_EXPECT_STREQ(test, path, "/usr/bin/myapp");
}

/* Path containing ':' must not be truncated. */
static void tlob_parse_path_with_colon(struct kunit *test)
{
	char buf[] = "1000:0x100:0x200:/opt/my:app/bin";
	u64 thr; loff_t start, stop; char *path;

	KUNIT_EXPECT_EQ(test,
		tlob_parse_uprobe_line(buf, &thr, &path, &start, &stop),
		0);
	KUNIT_EXPECT_STREQ(test, path, "/opt/my:app/bin");
}

/* Zero threshold must be rejected. */
static void tlob_parse_zero_threshold(struct kunit *test)
{
	char buf[] = "0:0x100:0x200:/usr/bin/myapp";
	u64 thr; loff_t start, stop; char *path;

	KUNIT_EXPECT_EQ(test,
		tlob_parse_uprobe_line(buf, &thr, &path, &start, &stop),
		-EINVAL);
}

/* Empty path (trailing ':' with nothing after) must be rejected. */
static void tlob_parse_empty_path(struct kunit *test)
{
	char buf[] = "5000:0x100:0x200:";
	u64 thr; loff_t start, stop; char *path;

	KUNIT_EXPECT_EQ(test,
		tlob_parse_uprobe_line(buf, &thr, &path, &start, &stop),
		-EINVAL);
}

/* Missing field (3 tokens instead of 4) must be rejected. */
static void tlob_parse_too_few_fields(struct kunit *test)
{
	char buf[] = "5000:0x100:/usr/bin/myapp";
	u64 thr; loff_t start, stop; char *path;

	KUNIT_EXPECT_EQ(test,
		tlob_parse_uprobe_line(buf, &thr, &path, &start, &stop),
		-EINVAL);
}

/* Negative offset must be rejected. */
static void tlob_parse_negative_offset(struct kunit *test)
{
	char buf[] = "5000:-1:0x200:/usr/bin/myapp";
	u64 thr; loff_t start, stop; char *path;

	KUNIT_EXPECT_EQ(test,
		tlob_parse_uprobe_line(buf, &thr, &path, &start, &stop),
		-EINVAL);
}

static struct kunit_case tlob_parse_uprobe_cases[] = {
	KUNIT_CASE(tlob_parse_decimal_offsets),
	KUNIT_CASE(tlob_parse_hex_offsets),
	KUNIT_CASE(tlob_parse_path_with_colon),
	KUNIT_CASE(tlob_parse_zero_threshold),
	KUNIT_CASE(tlob_parse_empty_path),
	KUNIT_CASE(tlob_parse_too_few_fields),
	KUNIT_CASE(tlob_parse_negative_offset),
	{}
};

static struct kunit_suite tlob_parse_uprobe_suite = {
	.name       = "tlob_parse_uprobe",
	.test_cases = tlob_parse_uprobe_cases,
};

kunit_test_suites(&tlob_automaton_suite,
		  &tlob_task_api_suite,
		  &tlob_sched_integration_suite,
		  &tlob_trace_output_suite,
		  &tlob_event_buf_suite,
		  &tlob_parse_uprobe_suite);

MODULE_DESCRIPTION("KUnit tests for the tlob RV monitor");
MODULE_LICENSE("GPL");
