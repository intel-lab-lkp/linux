// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the tlob RV monitor.
 *
 * tlob_task_api:          start/stop lifecycle, error paths, violations.
 * tlob_sched_integration: per-state accounting across real context switches.
 * tlob_uprobe_format:     uprobe binding format; add/remove acceptance and rejection.
 * tlob_trace_output:      trace event format for event_tlob, error_env_tlob.
 * tlob_violation_react:   error count per budget expiry; per-state breakdown.
 *
 * tlob_add_uprobe() duplicate-(binary, offset_start) constraint is not covered
 * here: kern_path() requires a real filesystem; see selftests instead.
 */
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/sched/task.h>

#include "tlob.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

/*
 * Kthread cleanup guard: registers a kunit action that stops a kthread on
 * test exit, even when a KUNIT_ASSERT fires before normal teardown code runs.
 *
 * Caller must call get_task_struct() before registering the guard.
 * Set guard->task = NULL before normal-path teardown to prevent double-stop.
 * Pass the completion to unblock on early exit, or NULL if not needed.
 */
struct tlob_kthread_guard {
	struct task_struct	*task;
	struct completion	*unblock;
};

static void kthread_guard_fn(void *arg)
{
	struct tlob_kthread_guard *g = arg;

	if (!g->task)
		return;
	if (g->unblock)
		complete(g->unblock);
	kthread_stop(g->task);
	put_task_struct(g->task);
}

static struct tlob_kthread_guard *
tlob_guard_kthread(struct kunit *test, struct task_struct *task,
		   struct completion *unblock)
{
	struct tlob_kthread_guard *g;

	g = kunit_kzalloc(test, sizeof(*g), GFP_KERNEL);
	if (!g)
		return NULL;
	g->task = task;
	g->unblock = unblock;
	if (kunit_add_action_or_reset(test, kthread_guard_fn, g))
		return NULL;
	return g;
}

/* Suite 1: task API - lifecycle, error paths, violations. */

/* Basic start/stop cycle */
static void tlob_start_stop_ok(struct kunit *test)
{
	int ret;

	ret = tlob_start_task(current, 10000000ULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), 0);
	KUNIT_EXPECT_EQ(test, tlob_num_monitored_read(), 0);
}

/* Double start must return -EALREADY; double stop must return -ESRCH. */
static void tlob_double_start(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, tlob_start_task(current, 10000000ULL), 0);
	KUNIT_EXPECT_EQ(test, tlob_start_task(current, 10000000ULL), -EALREADY);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), 0);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -ESRCH);
	KUNIT_EXPECT_EQ(test, tlob_num_monitored_read(), 0);
}

/* Stop without start must return -ESRCH. */
static void tlob_stop_without_start(struct kunit *test)
{
	tlob_stop_task(current);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -ESRCH);
	KUNIT_EXPECT_EQ(test, tlob_num_monitored_read(), 0);
}

/* threshold_us == 0 is invalid and must return -ERANGE. */
static void tlob_zero_threshold(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tlob_start_task(current, 0), -ERANGE);
}

/* 1 ns budget: timer almost certainly fires before tlob_stop_task(). */
static void tlob_immediate_deadline(struct kunit *test)
{
	int ret = tlob_start_task(current, 1);

	KUNIT_ASSERT_EQ(test, ret, 0);
	udelay(100);
	/* timer fired -> -EOVERFLOW; if we won the race, 0 is also valid */
	ret = tlob_stop_task(current);
	KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, tlob_num_monitored_read(), 0);
}

/*
 * kthreads provide distinct task_structs; fill to TLOB_MAX_MONITORED,
 * then verify -ENOSPC.
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

	KUNIT_ASSERT_EQ(test, tlob_num_monitored_read(), 0);

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

		ret = tlob_start_task(threads[i], 10000000ULL);
		if (ret != 0) {
			KUNIT_FAIL(test, "tlob_start_task failed at i=%d: %d",
				   i, ret);
			put_task_struct(threads[i]);
			complete(&ctxs[i].start);
			threads[i] = NULL;
			goto cleanup;
		}
	}

	ret = tlob_start_task(current, 10000000ULL);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);

cleanup:
	/* cancel monitoring and unblock first, then wait for full exit */
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
 * Holder kthread holds a mutex for 80 ms; arm a 10 ms budget, burn ~1 ms
 * on-CPU, then block on the mutex; timer fires while sleeping -> -EOVERFLOW.
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

static void tlob_deadline_fires_sleeping(struct kunit *test)
{
	struct tlob_holder_ctx *ctx;
	struct tlob_kthread_guard *guard;
	struct task_struct *holder;
	ktime_t t0;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->hold_ms = 80;
	mutex_init(&ctx->lock);
	init_completion(&ctx->ready);

	holder = kthread_run(tlob_holder_fn, ctx, "tlob_holder_kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, holder);
	get_task_struct(holder);

	guard = tlob_guard_kthread(test, holder, NULL);
	KUNIT_ASSERT_NOT_NULL(test, guard);

	wait_for_completion(&ctx->ready);

	ret = tlob_start_task(current, 10000);
	KUNIT_ASSERT_EQ(test, ret, 0);

	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 1000)
		cpu_relax();

	/* block on mutex: running->sleeping; timer fires while sleeping */
	mutex_lock(&ctx->lock);
	mutex_unlock(&ctx->lock);

	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -EOVERFLOW);

	guard->task = NULL;
	kthread_stop(holder);
	put_task_struct(holder);
}

/*
 * yield() triggers a preempt sched_switch (prev_state==0): running->waiting.
 * Busy-spin 50 ms so the 2 ms budget fires regardless of scheduler timing.
 */
static void tlob_deadline_fires_waiting(struct kunit *test)
{
	ktime_t t0;
	int ret;

	ret = tlob_start_task(current, 2000);
	KUNIT_ASSERT_EQ(test, ret, 0);

	yield();

	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 50000)
		cpu_relax();

	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -EOVERFLOW);
}

/* Arm a 1 ms budget and busy-spin for 50 ms; timer fires in running state. */
static void tlob_deadline_fires_running(struct kunit *test)
{
	ktime_t t0;
	int ret;

	ret = tlob_start_task(current, 1000);
	KUNIT_ASSERT_EQ(test, ret, 0);

	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 50000)
		cpu_relax();

	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -EOVERFLOW);
}

/* Start three tasks, reinit monitor, verify all entries are gone. */
static int tlob_dummy_fn(void *arg)
{
	wait_for_completion((struct completion *)arg);
	return 0;
}

static void tlob_reinit_clears_all(struct kunit *test)
{
	struct completion *done1, *done2;
	struct tlob_kthread_guard *guard1, *guard2;
	struct task_struct *t1, *t2;
	int ret;

	done1 = kunit_kzalloc(test, sizeof(*done1), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, done1);
	done2 = kunit_kzalloc(test, sizeof(*done2), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, done2);

	init_completion(done1);
	init_completion(done2);

	t1 = kthread_run(tlob_dummy_fn, done1, "tlob_dummy1");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t1);
	get_task_struct(t1);
	guard1 = tlob_guard_kthread(test, t1, done1);
	KUNIT_ASSERT_NOT_NULL(test, guard1);

	t2 = kthread_run(tlob_dummy_fn, done2, "tlob_dummy2");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t2);
	get_task_struct(t2);
	guard2 = tlob_guard_kthread(test, t2, done2);
	KUNIT_ASSERT_NOT_NULL(test, guard2);

	KUNIT_ASSERT_EQ(test, tlob_start_task(current, 10000000ULL), 0);
	KUNIT_ASSERT_EQ(test, tlob_start_task(t1, 10000000ULL), 0);
	KUNIT_ASSERT_EQ(test, tlob_start_task(t2, 10000000ULL), 0);

	tlob_destroy_monitor();
	ret = tlob_init_monitor();
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), -ESRCH);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(t1), -ESRCH);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(t2), -ESRCH);

	/* null guards before teardown to prevent double-stop */
	guard1->task = NULL;
	guard2->task = NULL;
	complete(done1);
	complete(done2);
	kthread_stop(t1);
	kthread_stop(t2);
	put_task_struct(t1);
	put_task_struct(t2);
}

static int tlob_task_api_suite_init(struct kunit_suite *suite)
{
	rv_kunit_monitoring_on();
	return tlob_init_monitor();
}

static void tlob_task_api_suite_exit(struct kunit_suite *suite)
{
	tlob_destroy_monitor();
	rv_kunit_monitoring_off();
}

static void tlob_task_api_exit(struct kunit *test)
{
	/*
	 * tlob_stop_task() returns pool slots via call_rcu (da_pool_return_cb).
	 * Wait for all pending callbacks so each test starts with a full pool.
	 */
	rcu_barrier();
}

static struct kunit_case tlob_task_api_cases[] = {
	KUNIT_CASE(tlob_start_stop_ok),
	KUNIT_CASE(tlob_double_start),
	KUNIT_CASE(tlob_stop_without_start),
	KUNIT_CASE(tlob_zero_threshold),
	KUNIT_CASE(tlob_immediate_deadline),
	KUNIT_CASE(tlob_enospc),
	KUNIT_CASE(tlob_deadline_fires_sleeping),
	KUNIT_CASE(tlob_deadline_fires_waiting),
	KUNIT_CASE(tlob_deadline_fires_running),
	KUNIT_CASE(tlob_reinit_clears_all),
	{}
};

static struct kunit_suite tlob_task_api_suite = {
	.name       = "tlob_task_api",
	.suite_init = tlob_task_api_suite_init,
	.suite_exit = tlob_task_api_suite_exit,
	.exit       = tlob_task_api_exit,
	.test_cases = tlob_task_api_cases,
};

/* Suite 2: sched integration - per-state ns accounting. */

struct tlob_ping_ctx {
	struct completion ping;
	struct completion pong;
};

static int tlob_ping_fn(void *arg)
{
	struct tlob_ping_ctx *ctx = arg;

	wait_for_completion(&ctx->ping);
	complete(&ctx->pong);
	return 0;
}

/* Force two context switches and verify stop returns 0 (within budget). */
static void tlob_sched_switch_accounting(struct kunit *test)
{
	struct tlob_ping_ctx *ctx;
	struct tlob_kthread_guard *guard;
	struct task_struct *peer;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	init_completion(&ctx->ping);
	init_completion(&ctx->pong);

	peer = kthread_run(tlob_ping_fn, ctx, "tlob_ping_kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, peer);
	get_task_struct(peer);

	guard = tlob_guard_kthread(test, peer, &ctx->ping);
	KUNIT_ASSERT_NOT_NULL(test, guard);

	ret = tlob_start_task(current, 5000000ULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* complete(ping) -> peer runs, forcing a context switch out and back */
	complete(&ctx->ping);
	wait_for_completion(&ctx->pong);

	ret = tlob_stop_task(current);
	KUNIT_EXPECT_EQ(test, ret, 0);

	guard->task = NULL;
	kthread_stop(peer);
	put_task_struct(peer);
}

/* start/stop monitoring a kthread other than current */
static int tlob_block_fn(void *arg)
{
	struct completion *done = arg;

	msleep(20);
	complete(done);
	return 0;
}

static void tlob_monitor_other_task(struct kunit *test)
{
	struct completion *done;
	struct tlob_kthread_guard *guard;
	struct task_struct *target;
	int ret;

	done = kunit_kzalloc(test, sizeof(*done), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, done);
	init_completion(done);

	target = kthread_run(tlob_block_fn, done, "tlob_target_kunit");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, target);
	get_task_struct(target);

	guard = tlob_guard_kthread(test, target, NULL);
	KUNIT_ASSERT_NOT_NULL(test, guard);

	ret = tlob_start_task(target, 5000000ULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	wait_for_completion(done);

	/* 5 s budget won't fire in 20 ms; 0 or -EOVERFLOW are both valid */
	ret = tlob_stop_task(target);
	KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EOVERFLOW);

	guard->task = NULL;
	kthread_stop(target);
	put_task_struct(target);
}

static int tlob_sched_suite_init(struct kunit_suite *suite)
{
	rv_kunit_monitoring_on();
	return tlob_init_monitor();
}

static void tlob_sched_suite_exit(struct kunit_suite *suite)
{
	tlob_destroy_monitor();
	rv_kunit_monitoring_off();
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

/* Suite 3: uprobe binding format - add/remove acceptance and rejection. */

static const char * const tlob_format_valid[] = {
	"p /usr/bin/myapp:4768 4848 threshold=5000",
	"p /usr/bin/myapp:0x12a0 0x12f0 threshold=10000",
	"p /opt/my:app/bin:0x100 0x200 threshold=1000",
};

static const char * const tlob_format_invalid[] = {
	/* add: malformed */
	"p /usr/bin/myapp:0x100 0x200 threshold=0",
	"p :0x100 0x200 threshold=5000",
	"p /usr/bin/myapp:0x100 threshold=5000",
	"p /usr/bin/myapp:-1 0x200 threshold=5000",
	"p /usr/bin/myapp:0x100 0x200",
	"p /usr/bin/myapp:0x100 0x100 threshold=5000",
	/* remove: malformed */
	"-usr/bin/myapp:0x100",
	"-/usr/bin/myapp",
	"-/:0x100",
	"-/usr/bin/myapp:abc",
};

/*
 * Valid add lines return -ENOENT (path does not exist in the test environment)
 * rather than 0; a non-(-EINVAL) return confirms the format was accepted.
 */
static void tlob_format_accepted(struct kunit *test)
{
	char buf[128];
	int i;

	for (i = 0; i < ARRAY_SIZE(tlob_format_valid); i++) {
		strscpy(buf, tlob_format_valid[i], sizeof(buf));
		KUNIT_EXPECT_NE(test, tlob_create_or_delete_uprobe(buf), -EINVAL);
	}
}

static void tlob_format_rejected(struct kunit *test)
{
	char buf[128];
	int i;

	for (i = 0; i < ARRAY_SIZE(tlob_format_invalid); i++) {
		strscpy(buf, tlob_format_invalid[i], sizeof(buf));
		KUNIT_EXPECT_EQ(test, tlob_create_or_delete_uprobe(buf), -EINVAL);
	}
}

static struct kunit_case tlob_uprobe_format_cases[] = {
	KUNIT_CASE(tlob_format_accepted),
	KUNIT_CASE(tlob_format_rejected),
	{}
};

static struct kunit_suite tlob_uprobe_format_suite = {
	.name       = "tlob_uprobe_format",
	.test_cases = tlob_uprobe_format_cases,
};

/* Suite 4: trace output - verify event_tlob and error_env_tlob field values. */

static void tlob_trace_event_format(struct kunit *test)
{
	const struct tlob_captured_event *ev;
	int pid = current->pid;
	int ret;

	tlob_event_count_reset();
	ret = tlob_start_task(current, 5000000ULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* sleep/wakeup/switch_in: running->sleeping->waiting->running */
	msleep(20);

	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), 0);

	KUNIT_EXPECT_GE(test, tlob_event_count_read(), 3);

	ev = tlob_last_event_read();
	KUNIT_EXPECT_EQ(test,    ev->id,          pid);
	KUNIT_EXPECT_STREQ(test, ev->state,       "waiting");
	KUNIT_EXPECT_STREQ(test, ev->event,       "switch_in");
	KUNIT_EXPECT_STREQ(test, ev->next_state,  "running");
	KUNIT_EXPECT_TRUE(test,  ev->final_state);
}

static void tlob_trace_error_env_format(struct kunit *test)
{
	const struct tlob_captured_error_env *err;
	ktime_t t0;
	int pid = current->pid;
	int ret;

	tlob_error_env_count_reset();
	ret = tlob_start_task(current, 1000);
	KUNIT_ASSERT_EQ(test, ret, 0);

	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 50000)
		cpu_relax();

	tlob_stop_task(current);

	KUNIT_ASSERT_GE(test, tlob_error_env_count_read(), 1);

	err = tlob_last_error_env_read();
	KUNIT_EXPECT_EQ(test,    err->id,    pid);
	KUNIT_EXPECT_STREQ(test, err->state, "running");
	KUNIT_EXPECT_STREQ(test, err->event, "budget_exceeded");
	KUNIT_EXPECT_TRUE(test, strncmp(err->env, "clk_elapsed=", 12) == 0);
}

static int tlob_trace_suite_init(struct kunit_suite *suite)
{
	int ret;

	rv_kunit_monitoring_on();
	ret = tlob_init_monitor();
	if (ret)
		goto err_mon_off;
	ret = tlob_register_kunit_probes();
	if (ret)
		goto err_destroy;
	ret = tlob_enable_hooks();
	if (ret)
		goto err_probes;
	return 0;

err_probes:
	tlob_unregister_kunit_probes();
err_destroy:
	tlob_destroy_monitor();
err_mon_off:
	rv_kunit_monitoring_off();
	return ret;
}

static void tlob_trace_suite_exit(struct kunit_suite *suite)
{
	tlob_disable_hooks();
	tlob_unregister_kunit_probes();
	tlob_destroy_monitor();
	rv_kunit_monitoring_off();
}

static struct kunit_case tlob_trace_output_cases[] = {
	KUNIT_CASE(tlob_trace_event_format),
	KUNIT_CASE(tlob_trace_error_env_format),
	{}
};

static struct kunit_suite tlob_trace_output_suite = {
	.name       = "tlob_trace_output",
	.suite_init = tlob_trace_suite_init,
	.suite_exit = tlob_trace_suite_exit,
	.test_cases = tlob_trace_output_cases,
};

/*
 * Suite 5: violation reaction - complement to Suite 4.
 * Suite 4 checks trace field values; Suite 5 checks semantics:
 * error count per budget expiry and per-state ns breakdown.
 */

/* generous budget; usleep forces state transitions; no error must fire */
static void tlob_no_error_within_budget(struct kunit *test)
{
	tlob_error_env_count_reset();
	tlob_event_count_reset();

	KUNIT_ASSERT_EQ(test, tlob_start_task(current, 10000000ULL), 0);
	usleep_range(5000, 10000);
	KUNIT_EXPECT_EQ(test, tlob_stop_task(current), 0);
	KUNIT_EXPECT_EQ(test, tlob_error_env_count_read(), 0);
	KUNIT_EXPECT_GE(test, tlob_event_count_read(), 2);
}

/* busy-spin 50 ms >> 1 ms budget; running_ns must dominate */
static void tlob_detail_running_dominates(struct kunit *test)
{
	const struct tlob_captured_detail *d;
	u64 total_ns;
	ktime_t t0;
	int ret;

	tlob_error_env_count_reset();

	ret = tlob_start_task(current, 1000);
	KUNIT_ASSERT_EQ(test, ret, 0);

	t0 = ktime_get();
	while (ktime_us_delta(ktime_get(), t0) < 50000)
		cpu_relax();

	tlob_stop_task(current);

	KUNIT_EXPECT_EQ(test, tlob_error_env_count_read(), 1);
	d = tlob_last_detail_read();
	KUNIT_EXPECT_EQ(test, d->pid, current->pid);
	KUNIT_EXPECT_EQ(test, d->threshold_us, 1000ULL);
	total_ns = d->running_ns + d->waiting_ns + d->sleeping_ns;
	KUNIT_EXPECT_GE(test, total_ns, 1000ULL * 1000);
	KUNIT_EXPECT_GT(test, d->running_ns, d->sleeping_ns + d->waiting_ns);
}

struct tlob_hog_ctx {
	int spin_ms;
};

static int tlob_hog_fn(void *arg)
{
	struct tlob_hog_ctx *ctx = arg;
	ktime_t t0 = ktime_get();

	while (!kthread_should_stop() &&
	       ktime_ms_delta(ktime_get(), t0) < ctx->spin_ms)
		cpu_relax();
	return 0;
}

/*
 * SCHED_FIFO kthread bound to the same CPU preempts the monitored task
 * (sched_switch prev_state == 0: running->waiting) and holds the CPU for
 * 80 ms >> 10 ms budget, guaranteeing the timer fires in waiting state.
 */
static void tlob_detail_waiting_dominates(struct kunit *test)
{
	struct tlob_hog_ctx *ctx;
	struct task_struct *hog;
	struct tlob_kthread_guard *guard;
	const struct tlob_captured_detail *d;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	int ret;

	tlob_error_env_count_reset();

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->spin_ms = 80;

	hog = kthread_create(tlob_hog_fn, ctx, "tlob_s5_hog");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hog);
	get_task_struct(hog);

	kthread_bind(hog, smp_processor_id());
	sched_setscheduler_nocheck(hog, SCHED_FIFO, &param);

	guard = tlob_guard_kthread(test, hog, NULL);
	KUNIT_ASSERT_NOT_NULL(test, guard);

	ret = tlob_start_task(current, 10000); /* 10 ms budget */
	KUNIT_ASSERT_EQ(test, ret, 0);

	wake_up_process(hog);
	yield(); /* sched_switch prev_state == 0: running->waiting */

	tlob_stop_task(current);

	KUNIT_EXPECT_EQ(test, tlob_error_env_count_read(), 1);
	d = tlob_last_detail_read();
	KUNIT_EXPECT_EQ(test, d->sleeping_ns, 0ULL);
	KUNIT_EXPECT_GT(test, d->waiting_ns, d->running_ns + d->sleeping_ns);

	guard->task = NULL;
	kthread_stop(hog);
	put_task_struct(hog);
}

/* block on mutex for 80 ms >> 10 ms budget; sleeping_ns must dominate */
static void tlob_detail_sleeping_dominates(struct kunit *test)
{
	struct tlob_holder_ctx *ctx;
	struct tlob_kthread_guard *guard;
	struct task_struct *holder;
	const struct tlob_captured_detail *d;
	int ret;

	tlob_error_env_count_reset();

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->hold_ms = 80;
	mutex_init(&ctx->lock);
	init_completion(&ctx->ready);

	holder = kthread_run(tlob_holder_fn, ctx, "tlob_s5_detail");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, holder);
	get_task_struct(holder);

	guard = tlob_guard_kthread(test, holder, NULL);
	KUNIT_ASSERT_NOT_NULL(test, guard);

	wait_for_completion(&ctx->ready);

	ret = tlob_start_task(current, 10000);
	KUNIT_ASSERT_EQ(test, ret, 0);

	mutex_lock(&ctx->lock);
	mutex_unlock(&ctx->lock);

	tlob_stop_task(current);

	KUNIT_EXPECT_EQ(test, tlob_error_env_count_read(), 1);
	d = tlob_last_detail_read();
	KUNIT_EXPECT_GT(test, d->sleeping_ns, d->running_ns + d->waiting_ns);

	guard->task = NULL;
	kthread_stop(holder);
	put_task_struct(holder);
}

static int tlob_violation_suite_init(struct kunit_suite *suite)
{
	int ret;

	rv_kunit_monitoring_on();
	ret = tlob_init_monitor();
	if (ret)
		goto err_mon_off;
	ret = tlob_register_kunit_probes();
	if (ret)
		goto err_destroy;
	ret = tlob_enable_hooks();
	if (ret)
		goto err_probes;
	return 0;

err_probes:
	tlob_unregister_kunit_probes();
err_destroy:
	tlob_destroy_monitor();
err_mon_off:
	rv_kunit_monitoring_off();
	return ret;
}

static void tlob_violation_suite_exit(struct kunit_suite *suite)
{
	tlob_disable_hooks();
	tlob_unregister_kunit_probes();
	tlob_destroy_monitor();
	rv_kunit_monitoring_off();
}

static struct kunit_case tlob_violation_react_cases[] = {
	KUNIT_CASE(tlob_no_error_within_budget),
	KUNIT_CASE(tlob_detail_running_dominates),
	KUNIT_CASE(tlob_detail_sleeping_dominates),
	KUNIT_CASE(tlob_detail_waiting_dominates),
	{}
};

static struct kunit_suite tlob_violation_react_suite = {
	.name       = "tlob_violation_react",
	.suite_init = tlob_violation_suite_init,
	.suite_exit = tlob_violation_suite_exit,
	.test_cases = tlob_violation_react_cases,
};

kunit_test_suites(&tlob_task_api_suite,
		  &tlob_sched_integration_suite,
		  &tlob_uprobe_format_suite,
		  &tlob_trace_output_suite,
		  &tlob_violation_react_suite);

MODULE_DESCRIPTION("KUnit tests for the tlob RV monitor");
MODULE_LICENSE("GPL");
