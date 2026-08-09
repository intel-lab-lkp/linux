// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for RV reactor registration and dispatch.
 */

#include <kunit/test.h>
#include <linux/rv.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include "rv.h"

static struct rv_reactor test_reactor = {
	.name		= "kunit_test_reactor",
	.description	= "KUnit test reactor",
};

static void reactor_teardown(void *arg)
{
	rv_unregister_reactor(&test_reactor);
}

static void register_test_reactor(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, rv_register_reactor(&test_reactor), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, reactor_teardown, NULL), 0);
}

static void test_double_register(struct kunit *test)
{
	register_test_reactor(test);
	KUNIT_EXPECT_EQ(test, rv_register_reactor(&test_reactor), -EINVAL);
}

/*
 * Use a fixed-size array so sizeof() gives the exact byte count at
 * compile time.
 */
static const char long_reactor_name[] = "kunit_reactor_name_too_long_xxx_";
_Static_assert(sizeof(long_reactor_name) - 1 >= MAX_RV_REACTOR_NAME_SIZE,
	       "long_reactor_name must be at least MAX_RV_REACTOR_NAME_SIZE chars");

static void test_name_too_long(struct kunit *test)
{
	static struct rv_reactor long_reactor = {
		.name = long_reactor_name,
	};

	KUNIT_EXPECT_EQ(test, rv_register_reactor(&long_reactor), -EINVAL);
}

static struct kunit_case rv_reactor_registration_cases[] = {
	KUNIT_CASE(test_double_register),
	KUNIT_CASE(test_name_too_long),
	{}
};

static struct kunit_suite rv_reactor_registration_suite = {
	.name		= "rv_reactor_registration",
	.test_cases	= rv_reactor_registration_cases,
};

static atomic_t react_call_count;

__printf(1, 0) static void mock_react(const char *msg, va_list args)
{
	atomic_inc(&react_call_count);
	/*
	 * Hold the CPU for 5 ms so a timer interrupt is likely to fire
	 * inside rv_react()'s lockdep context, exercising the LD_WAIT_SPIN
	 * constraint.  mdelay() is a calibrated busy-wait with no scheduler
	 * interaction.
	 */
	mdelay(5);
}

static void test_react_no_callback(struct kunit *test)
{
	struct rv_monitor monitor = {
		.name = "kunit_null_react",
	};

	atomic_set(&react_call_count, 0);
	rv_react(&monitor, "no callback");

	/*
	 * The only possible failure in this test case is a kernel panic.
	 * NULL react guard: callback must NOT have been invoked
	 */
	KUNIT_EXPECT_EQ(test, atomic_read(&react_call_count), 0);
}

static void test_react_callback_invoked(struct kunit *test)
{
	struct rv_monitor monitor = {
		.name	= "kunit_dispatch_monitor",
		.react	= mock_react,
	};

	atomic_set(&react_call_count, 0);
	rv_react(&monitor, "callback invocation test");
	KUNIT_EXPECT_EQ(test, atomic_read(&react_call_count), 1);
}

static struct kunit_case rv_react_dispatch_cases[] = {
	KUNIT_CASE(test_react_no_callback),
	KUNIT_CASE(test_react_callback_invoked),
	{}
};

static struct kunit_suite rv_react_dispatch_suite = {
	.name		= "rv_react_dispatch",
	.test_cases	= rv_react_dispatch_cases,
};

kunit_test_suites(&rv_reactor_registration_suite, &rv_react_dispatch_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for RV reactor registration and dispatch");
