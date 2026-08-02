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

/*
 * rv_unregister_reactor() on a reactor that was never registered would
 * list_del() an uninitialized list_head, so track registration and only
 * unregister in the teardown if it is still on the list.
 */
static bool test_reactor_registered;

static int unregister_test_reactor(void)
{
	int ret = 0;

	if (test_reactor_registered) {
		ret = rv_unregister_reactor(&test_reactor);
		test_reactor_registered = false;
	}

	return ret;
}

/*
 * The teardown action guarantees the reactor is unregistered even if a
 * test fails mid-way, so a leftover entry cannot corrupt later tests.
 */
static void reactor_teardown(void *arg)
{
	unregister_test_reactor();
}

static void register_test_reactor(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, rv_register_reactor(&test_reactor), 0);
	test_reactor_registered = true;
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, reactor_teardown, NULL), 0);
}

static void test_register_unregister(struct kunit *test)
{
	register_test_reactor(test);

	KUNIT_EXPECT_EQ(test, unregister_test_reactor(), 0);
}

static void test_double_register(struct kunit *test)
{
	register_test_reactor(test);

	KUNIT_EXPECT_EQ(test, rv_register_reactor(&test_reactor), -EINVAL);

	KUNIT_EXPECT_EQ(test, unregister_test_reactor(), 0);
}

static void test_name_too_long(struct kunit *test)
{
	/* Name length of MAX_RV_REACTOR_NAME_SIZE (32) must be rejected. */
	static struct rv_reactor long_reactor = {
		.name = "kunit_reactor_name_too_long_xxx_",
	};

	KUNIT_ASSERT_EQ(test, (int)strlen(long_reactor.name),
			MAX_RV_REACTOR_NAME_SIZE);
	KUNIT_EXPECT_EQ(test, rv_register_reactor(&long_reactor), -EINVAL);
}

static struct kunit_case rv_reactor_registration_cases[] = {
	KUNIT_CASE(test_register_unregister),
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

	/* rv_react() must silently return when monitor->react is NULL. */
	rv_react(&monitor, "no callback");
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
