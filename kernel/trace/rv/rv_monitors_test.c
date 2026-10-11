// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026-2029 Red Hat, Inc. Gabriele Monaco <gmonaco@redhat.com>
 *
 * RV monitor kunit tests:
 *   Tests the RV monitors by triggering fake events to verify monitor
 *   behavior and reactions. Tests start from the first defined event and
 *   trigger events in order to verify error detection.
 */
#include <rv/kunit.h>
#include <kunit/test-bug.h>
#include <linux/kernel.h>
#include <linux/rv.h>
#include "rv.h"

#include <rv_trace.h>

/* rvtest: a minimal per-object HA monitor driven directly by the tests. */
#define MONITOR_NAME rvtest
#define RV_MON_TYPE RV_MON_PER_OBJ
#define HA_TIMER_TYPE HA_TIMER_HRTIMER
typedef void *monitor_target;

/* The framework calls these for every monitor; this one reports nothing. */
#define trace_event_rvtest(...)		do { } while (0)
#define trace_error_rvtest(...)		do { } while (0)
#define trace_error_env_rvtest(...)	do { } while (0)

enum states_rvtest {
	idle_rvtest,
	armed_rvtest,
	state_max_rvtest,
};

#define INVALID_STATE state_max_rvtest

enum events_rvtest {
	start_rvtest,
	event_max_rvtest,
};

enum envs_rvtest {
	clk_rvtest,
	env_max_rvtest,
	env_max_stored_rvtest = env_max_rvtest,
};

#define HA_CLK_NS

struct automaton_rvtest {
	char *state_names[state_max_rvtest];
	char *event_names[event_max_rvtest];
	char *env_names[env_max_rvtest];
	unsigned char function[state_max_rvtest][event_max_rvtest];
	unsigned char initial_state;
	bool final_states[state_max_rvtest];
};

static const struct automaton_rvtest automaton_rvtest = {
	.state_names = { "idle", "armed" },
	.event_names = { "start" },
	.env_names = { "clk" },
	.function = {
		{ armed_rvtest },
		{ INVALID_STATE },
	},
	.initial_state = idle_rvtest,
	.final_states = { 1, 0 },
};

#include <rv/ha_monitor.h>

#define RVTEST_BUDGET_NS (300ULL * NSEC_PER_MSEC)

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_rvtest env,
		      u64 time_ns)
{
	return ha_get_clk_ns(ha_mon, env, time_ns);
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_rvtest env,
			 u64 time_ns)
{
	ha_reset_clk_ns(ha_mon, env, time_ns);
}

/* The tests arm the timer themselves. */
static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	return true;
}

static const struct rv_kunit_mon rvtest_mon = RV_MON_OPS_INIT();

/*
 * An easy way to pass the context is to use kunit_get_current_test()->priv,
 * but this doesn't always work (e.g. a reactor running from another context
 * like softirq). Store the current value here whenever a test is running.
 */
static struct rv_kunit_ctx *active_ctx;

__printf(1, 0)
static void rv_kunit_mock_react(const char *msg, va_list args)
{
	if (active_ctx)
		++active_ctx->reactions;
}

/*
 * teardown_test - Disable the monitor for a kunit test
 *
 * Since per-task monitors are special, make sure we reset all the ones we
 * started manually here, if required.
 */
void teardown_test(void *arg)
{
	const struct rv_kunit_mon *mon = arg;
	struct kunit *test = kunit_get_current_test();

	if (test) {
		struct rv_kunit_ctx *ctx = test->priv;

		RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

		if (mon->is_per_task && mon->task_reset) {
			for (int i = 0; i < ctx->mock_task_count; i++)
				mon->task_reset(ctx->mock_tasks[i]);
			synchronize_rcu();
		}
	}

	mon->rv_this->enabled = 0;

	if (mon->rv_this->reactor)
		mon->rv_this->react = mon->rv_this->reactor->react;
	else
		mon->rv_this->react = NULL;
	active_ctx = NULL;
	rv_mock_current(NULL);

	if (mon->is_per_task)
		*mon->task_slot = RV_PER_TASK_MONITOR_INIT;
	else
		mon->monitor_destroy();
}

/*
 * prepare_test - Enable the monitor for a kunit test
 *
 * Do the bare minimum to set up the monitor, per-task monitors are special as
 * "real" initialisation/destruction iterates over real tasks, and may register
 * handlers. All we need is to select the right slot in the task_struct.
 */
void prepare_test(struct kunit *test, const struct rv_kunit_mon *mon)
{
	KUNIT_ASSERT_FALSE(test, mon->rv_this->enabled);

	active_ctx = test->priv;
	mon->rv_this->react = rv_kunit_mock_react;

	if (mon->is_per_task)
		*mon->task_slot = 0;
	else
		KUNIT_ASSERT_EQ(test, mon->monitor_init(), 0);

	mon->rv_this->enabled = 1;

	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, teardown_test, (void *)mon));
}

struct task_struct *rv_kunit_alloc_mock_task(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	struct task_struct *tsk;

	KUNIT_ASSERT_LT(test, ctx->mock_task_count, RV_KUNIT_MAX_MOCK_TASKS);

	tsk = kunit_kzalloc(test, sizeof(struct task_struct), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tsk);

	if (!IS_ENABLED(CONFIG_THREAD_INFO_IN_TASK)) {
		tsk->stack = kunit_kzalloc(test, sizeof(struct thread_info), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, tsk->stack);
	}

	ctx->mock_tasks[ctx->mock_task_count++] = tsk;
	return tsk;
}

static int rv_mon_test_init(struct kunit *test)
{
	struct rv_kunit_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	test->priv = ctx;

	return 0;
}

static void __maybe_unused rv_test_stub(struct kunit *test)
{
	kunit_skip(test, "Monitor not enabled\n");
}

/*
 * rv_test_dummy - test reactions work as expected
 */
static void rv_test_dummy(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	static struct rv_monitor dummy_monitor = {
		.name = "dummy",
		.react = rv_kunit_mock_react,
	};

	active_ctx = ctx;

	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_react(&dummy_monitor, "dummy");
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

	active_ctx = NULL;
}

/*
 * rv_test_rvtest_underflow - the HA clock math must not underflow as u64
 *
 * An invariant checked while the uptime is below the budget holds, and a
 * timer armed after the budget elapsed expires now.
 */
static void rv_test_rvtest_underflow(struct kunit *test)
{
	struct ha_monitor *ha_mon;
	struct da_monitor *da_mon;
	const int id = 4242;

	prepare_test(test, &rvtest_mon);
	da_handle_start_run_event(id, NULL, start_rvtest);

	scoped_guard(rcu)
		da_mon = da_get_monitor(id, NULL);
	KUNIT_ASSERT_NOT_NULL(test, da_mon);
	ha_mon = to_ha_monitor(da_mon);

	/* Uptime below the budget: time_ns - expire_ns would underflow. */
	ha_reset_clk_ns(ha_mon, clk_rvtest, 0);
	KUNIT_EXPECT_TRUE(test, ha_check_invariant_ns(ha_mon, clk_rvtest, 0,
						      RVTEST_BUDGET_NS));

	/*
	 * Budget already elapsed: expire - passed would underflow and queue
	 * the timer at KTIME_SEC_MAX.
	 */
	da_monitor_reset(&ha_mon->da_mon);
	ha_reset_clk_ns(ha_mon, clk_rvtest, 0);
	ha_start_timer_ns(ha_mon, clk_rvtest, RVTEST_BUDGET_NS,
			  RVTEST_BUDGET_NS + NSEC_PER_SEC);
	KUNIT_EXPECT_TRUE(test,
			  ktime_before(hrtimer_get_expires(&ha_mon->hrtimer),
				       ktime_get() + NSEC_PER_SEC));

	ha_cancel_timer_sync(ha_mon);
}

/*
 * rv_test_rvtest_arm_after_destroy - a storage outlived by its own timer
 *
 * da_destroy_storage() cancels the timer, then unlinks:
 *
 *	da_monitor_reset_hook()		cancels
 *	hash_del_rcu()
 *	call_rcu()
 *
 * A reader that resolved the storage before the unlink can arm past that
 * cancel.  Handlers resolve and use under one guard(rcu)(), so the two steps
 * are issued separately here.
 */
static void rv_test_rvtest_arm_after_destroy(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	const int id = 4242;
	struct da_monitor *stale;

	prepare_test(test, &rvtest_mon);

	da_handle_start_run_event(id, NULL, start_rvtest);

	scoped_guard(rcu)			/* resolve before the unlink */
		stale = da_get_monitor(id, NULL);
	KUNIT_ASSERT_NOT_NULL(test, stale);

	da_destroy_storage(id);			/* cancel, unlink, call_rcu */
	ha_start_timer_ns(to_ha_monitor(stale), clk_rvtest,
			  RVTEST_BUDGET_NS, ha_get_ns());

	rcu_barrier();

	msleep(600);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
}

#include "monitors/sco/sco_kunit.c"
#include "monitors/sssw/sssw_kunit.c"
#include "monitors/sts/sts_kunit.c"
#include "monitors/opid/opid_kunit.c"
#include "monitors/nomiss/nomiss_kunit.c"
#include "monitors/tlob/tlob_kunit.c"
#include "monitors/pagefault/pagefault_kunit.c"
#include "monitors/sleep/sleep_kunit.c"

static struct kunit_case rv_mon_test_cases[] = {
	KUNIT_CASE(rv_test_dummy),
	KUNIT_CASE(rv_test_sco),
	KUNIT_CASE(rv_test_sssw),
	KUNIT_CASE(rv_test_sts),
	KUNIT_CASE(rv_test_opid),
	KUNIT_CASE(rv_test_nomiss),
	KUNIT_CASE(rv_test_pagefault),
	KUNIT_CASE(rv_test_sleep),
	KUNIT_CASE(rv_test_rvtest_underflow),
	KUNIT_CASE(rv_test_rvtest_arm_after_destroy),
	KUNIT_CASE(rv_test_tlob),
	{}
};

static struct kunit_suite rv_mon_test_suite = {
	.name = "rv_mon",
	.suite_init = rv_set_testing,
	.suite_exit = rv_clear_testing,
	.init = rv_mon_test_init,
	.test_cases = rv_mon_test_cases,
};

kunit_test_suites(&rv_mon_test_suite);

MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("RV monitor kunit tests: test monitors by triggering reactions");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
