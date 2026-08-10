// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/fdtable.h>
#include <linux/file.h>

static void fdtable_test_alloc(struct kunit *test)
{
	struct fdtable *fdt;
	unsigned int slots = 64;

	fdt = alloc_fdtable(slots);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fdt);

	/* Check that max_fds is set correctly and is >= slots */
	KUNIT_EXPECT_GE(test, fdt->max_fds, slots);

	/* Check that fd is allocated */
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, fdt->fd);

	/* Check dynamic object size of fdt->fd if compiler supports __counted_by_ptr */
#ifdef CONFIG_CC_HAS_COUNTED_BY_PTR
	KUNIT_EXPECT_EQ(test, __builtin_dynamic_object_size(fdt->fd, 0),
			fdt->max_fds * sizeof(struct file *));
#endif

	/* Free the fdtable */
	__free_fdtable(fdt);
}

static struct kunit_case fdtable_test_cases[] = {
	KUNIT_CASE(fdtable_test_alloc),
	{}
};

static struct kunit_suite fdtable_test_suite = {
	.name = "fdtable",
	.test_cases = fdtable_test_cases,
};

kunit_test_suite(fdtable_test_suite);
