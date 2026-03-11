// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <drm/drm_suballoc.h>

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/sizes.h>

#define DRM_SUBALLOC_TEST_ITERATIONS        128
#define DRM_SUBALLOC_TEST_MANAGER_ALIGN     SZ_16
#define DRM_SUBALLOC_TEST_MANAGER_SIZE      SZ_16K
#define DRM_SUBALLOC_TEST_MAX_ALLOCS        16
#define DRM_SUBALLOC_TEST_MAX_ALLOC_SIZE    SZ_512

static bool fence_disable = true;
module_param_named(fence_disable, fence_disable, bool, 0644);
MODULE_PARM_DESC(fence_disable, "Disable suballoc fence tracking in test");

static void drm_test_suballoc_alloc_insert(struct kunit *test)
{
	struct drm_suballoc_manager manager;
	struct drm_suballoc *sa_arr[DRM_SUBALLOC_TEST_MAX_ALLOCS], *sa;
	struct drm_suballoc
		*sa_alloc_arr[DRM_SUBALLOC_TEST_ITERATIONS / DRM_SUBALLOC_TEST_MAX_ALLOCS] = {0};
	int i, size, sa_index, sa_alloc_arr_index;

	drm_suballoc_manager_init(&manager,
				  DRM_SUBALLOC_TEST_MANAGER_SIZE,
				  DRM_SUBALLOC_TEST_MANAGER_ALIGN);
	drm_suballoc_manager_fence_disable(&manager, fence_disable);

	kunit_info(test, "Starting suballoc test with %d iterations with fence %s\n",
		   DRM_SUBALLOC_TEST_ITERATIONS, fence_disable ? "disabled" : "enabled");

	for (i = 0, sa_index = 0, sa_alloc_arr_index = 0;
	     i < DRM_SUBALLOC_TEST_ITERATIONS; i++) {
		size = get_random_u32_below(DRM_SUBALLOC_TEST_MAX_ALLOC_SIZE) + 1;
		size = ALIGN(size, 16);

		sa = drm_suballoc_new(&manager, size, GFP_KERNEL, true, 0);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sa);
		KUNIT_ASSERT_EQ(test, drm_suballoc_size(sa), size);
		sa_arr[sa_index++] = sa;

		if (sa_index == DRM_SUBALLOC_TEST_MAX_ALLOCS) {
			for (int free_iter = 0; free_iter < DRM_SUBALLOC_TEST_MAX_ALLOCS - 1;
			     free_iter++) {
				drm_suballoc_free(sa_arr[free_iter], NULL);
			}
			sa_alloc_arr[sa_alloc_arr_index++] =
				sa_arr[DRM_SUBALLOC_TEST_MAX_ALLOCS - 1];
			sa_index = 0;
		}
	}

	for (i = 0; i < sa_alloc_arr_index; i++)
		drm_suballoc_free(sa_alloc_arr[i], NULL);

	drm_suballoc_manager_fini(&manager);
}

static struct kunit_case drm_suballoc_tests[] = {
	KUNIT_CASE(drm_test_suballoc_alloc_insert),
	{}
};

static struct kunit_suite drm_suballoc_test = {
	.name = "drm_suballoc",
	.test_cases = drm_suballoc_tests,
};

kunit_test_suite(drm_suballoc_test);

MODULE_DESCRIPTION("KUnit DRM suballoc test suite");
MODULE_LICENSE("GPL");
