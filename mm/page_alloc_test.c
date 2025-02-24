// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errname.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/memory.h>
#include <linux/nodemask.h>
#include <linux/percpu.h>
#include <linux/smp.h>

#include <kunit/test.h>

static struct kunit_case test_cases[] = { {} };

static struct kunit_suite test_suite = {
	.name = "page_alloc",
	.test_cases = test_cases,
};
kunit_test_suite(test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
