#include <linux/security.h>
#include <kunit/test.h>

int lock_kernel_down(const char *where, enum lockdown_reason level);

static void lockdown_test_invalid_level(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, -EINVAL, lock_kernel_down("TEST", LOCKDOWN_CONFIDENTIALITY_MAX+1));
}

static void lockdown_test_depth_locking(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 0, lock_kernel_down("TEST", LOCKDOWN_INTEGRITY_MAX));
	for (int i = 1; i < LOCKDOWN_INTEGRITY_MAX; i++)
		KUNIT_EXPECT_EQ_MSG(test, -EPERM, security_locked_down(i), "at i=%d", i);

	KUNIT_EXPECT_EQ(test, -EPERM, security_locked_down(LOCKDOWN_INTEGRITY_MAX));
}

static void lockdown_test_individual_level(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 0, lock_kernel_down("TEST", LOCKDOWN_PERF));
	KUNIT_EXPECT_EQ(test, -EPERM, security_locked_down(LOCKDOWN_PERF));
	/* Ensure adjacent levels are untouched */
	KUNIT_EXPECT_EQ(test, 0, security_locked_down(LOCKDOWN_TRACEFS));
	KUNIT_EXPECT_EQ(test, 0, security_locked_down(LOCKDOWN_DBG_READ_KERNEL));
}

static void lockdown_test_no_downgrade(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 0, lock_kernel_down("TEST", LOCKDOWN_CONFIDENTIALITY_MAX));
	KUNIT_EXPECT_EQ(test, 0, lock_kernel_down("TEST", LOCKDOWN_INTEGRITY_MAX));
	/*
	 * Ensure having locked down to a lower leve after a higher level
	 * lockdown nothing is lost
	 */
	KUNIT_EXPECT_EQ(test, -EPERM, security_locked_down(LOCKDOWN_TRACEFS));
}

static struct kunit_case lockdown_tests[] = {
	KUNIT_CASE(lockdown_test_invalid_level),
	KUNIT_CASE(lockdown_test_depth_locking),
	KUNIT_CASE(lockdown_test_individual_level),
	KUNIT_CASE(lockdown_test_no_downgrade),
	{}
};

static struct kunit_suite lockdown_test_suite = {
	.name = "lockdown test",
	.test_cases = lockdown_tests,
};
kunit_test_suite(lockdown_test_suite);

MODULE_LICENSE("GPL");
