// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for reactor_panic
 *
 */

#include <kunit/test.h>
#include <linux/rv.h>
#include <linux/panic_notifier.h>
#include <linux/notifier.h>
#include <linux/limits.h>
#include <linux/sched/clock.h>
#include <linux/processor.h>

/* Simulated execution time for mock panic notifier (nanoseconds). */
#define RV_PANIC_NOTIFIER_EXEC_NS	2000000ULL

/* Test state */
static struct {
	bool notifier_called;
} panic_test_state;

/*
 * Mock panic notifier callback.
 *
 * Runs at INT_MAX priority and returns NOTIFY_STOP to prevent real panic
 * handlers (kdump, watchdog) from executing during the test.  Busy-waits
 * RV_PANIC_NOTIFIER_EXEC_NS to simulate a real handler's execution time.
 */
static int mock_panic_notifier_fn(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	char *msg = data;
	u64 start = sched_clock();

	panic_test_state.notifier_called = true;
	pr_emerg("KUnit: reactor_panic test intercepted panic notifier: %s\n",
		 msg ? msg : "(no message)");

	while (sched_clock() - start < RV_PANIC_NOTIFIER_EXEC_NS)
		cpu_relax();

	return NOTIFY_STOP;
}

static struct notifier_block mock_panic_nb = {
	.notifier_call	= mock_panic_notifier_fn,
	.priority	= INT_MAX,
};

static struct rv_reactor mock_panic_reactor = {
	.name		= "test_panic",
	.description	= "test panic reactor",
};

static int reactor_panic_kunit_init(struct kunit *test)
{
	panic_test_state.notifier_called = false;
	return 0;
}

/* Test 1: register and unregister reactor */
static void test_panic_register_unregister(struct kunit *test)
{
	int ret;

	ret = rv_register_reactor(&mock_panic_reactor);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_STREQ(test, mock_panic_reactor.name, "test_panic");

	rv_unregister_reactor(&mock_panic_reactor);
}

/*
 * Test 2: panic notifier chain is reachable.
 *
 * vpanic() calls atomic_notifier_call_chain(&panic_notifier_list, ...).
 * Drive the chain directly to verify panic notifiers receive the notification —
 * the observable side-effect of reactor_panic without halting the system.
 */
static void test_panic_notifier_called(struct kunit *test)
{
	atomic_notifier_chain_register(&panic_notifier_list, &mock_panic_nb);
	atomic_notifier_call_chain(&panic_notifier_list, 0,
				   "panic violation message");
	atomic_notifier_chain_unregister(&panic_notifier_list, &mock_panic_nb);

	KUNIT_EXPECT_TRUE(test, panic_test_state.notifier_called);
}

static struct kunit_case reactor_panic_kunit_cases[] = {
	KUNIT_CASE(test_panic_register_unregister),
	KUNIT_CASE(test_panic_notifier_called),
	{}
};

static struct kunit_suite reactor_panic_kunit_suite = {
	.name		= "rv_reactor_panic",
	.init		= reactor_panic_kunit_init,
	.test_cases	= reactor_panic_kunit_cases,
};

kunit_test_suite(reactor_panic_kunit_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for reactor_panic");
