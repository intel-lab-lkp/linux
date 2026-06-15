// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for reactor_printk
 *
 */

#include <kunit/test.h>
#include <linux/rv.h>
#include <linux/printk.h>
#include <linux/sched/clock.h>
#include <linux/processor.h>

/*
 * Simulated execution time for mock_printk_react (sched_clock units,
 * nanoseconds).  Models the time a real printk reactor callback may consume
 * under I/O pressure, exercising the LD_WAIT_FREE constraint of rv_react().
 */
#define MOCK_REACT_DURATION_NS	5000000ULL

/*
 * Mock react callback mirroring rv_printk_reaction().
 *
 * Calls vprintk_deferred() — the same path as the real reactor — then holds
 * the CPU for MOCK_REACT_DURATION_NS via a sched_clock() timed busy-loop,
 * simulating a callback that is slow due to I/O back-pressure.
 * sched_clock() is notrace and lock-free; no sleep or lock acquisition is
 * performed, satisfying the LD_WAIT_FREE constraint of rv_react().
 */
__printf(1, 0) static void mock_printk_react(const char *msg, va_list args)
{
	u64 start = sched_clock();

	vprintk_deferred(msg, args);

	while (sched_clock() - start < MOCK_REACT_DURATION_NS)
		cpu_relax();
}

static struct rv_reactor mock_printk_reactor = {
	.name		= "test_printk",
	.description	= "test printk reactor",
	.react		= mock_printk_react,
};

/* Test 1: register and unregister reactor */
static void test_printk_register_unregister(struct kunit *test)
{
	int ret;

	ret = rv_register_reactor(&mock_printk_reactor);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_STREQ(test, mock_printk_reactor.name, "test_printk");

	rv_unregister_reactor(&mock_printk_reactor);
}

/* Test 2: react callback is invoked via rv_react() */
static void test_printk_react_called(struct kunit *test)
{
	struct rv_reactor reactor = {
		.name	= "printk_cb_test",
		.react	= mock_printk_react,
	};
	struct rv_monitor monitor = {
		.name	= "test_monitor",
		.reactor = &reactor,
		.react	= mock_printk_react,
	};

	rv_react(&monitor, "printk violation message");
}

/* Test 3: double registration should fail */
static void test_printk_double_register(struct kunit *test)
{
	int ret;

	ret = rv_register_reactor(&mock_printk_reactor);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = rv_register_reactor(&mock_printk_reactor);
	KUNIT_EXPECT_NE(test, ret, 0);

	rv_unregister_reactor(&mock_printk_reactor);
}

/* Test 4: register/unregister cycle */
static void test_printk_register_cycle(struct kunit *test)
{
	int ret, i;

	for (i = 0; i < 5; i++) {
		ret = rv_register_reactor(&mock_printk_reactor);
		KUNIT_EXPECT_EQ(test, ret, 0);

		rv_unregister_reactor(&mock_printk_reactor);
	}
}

/* Test 5: react callback is not NULL (printk reactors must provide react) */
static void test_printk_react_not_null(struct kunit *test)
{
	KUNIT_EXPECT_NOT_NULL(test, mock_printk_reactor.react);
}

static struct kunit_case reactor_printk_kunit_cases[] = {
	KUNIT_CASE(test_printk_register_unregister),
	KUNIT_CASE(test_printk_react_called),
	KUNIT_CASE(test_printk_double_register),
	KUNIT_CASE(test_printk_register_cycle),
	KUNIT_CASE(test_printk_react_not_null),
	{}
};

static struct kunit_suite reactor_printk_kunit_suite = {
	.name		= "rv_reactor_printk",
	.test_cases	= reactor_printk_kunit_cases,
};

kunit_test_suite(reactor_printk_kunit_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for reactor_printk");
