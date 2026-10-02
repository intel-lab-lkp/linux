// SPDX-License-Identifier: MIT
/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 */

#define pr_fmt(fmt) "drm_exec: " fmt

#include <kunit/test.h>

#include <linux/module.h>
#include <linux/prime_numbers.h>

#include <drm/drm_exec.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_kunit_helpers.h>

struct drm_exec_priv {
	struct device *dev;
	struct drm_device *drm;
};

static int drm_exec_test_init(struct kunit *test)
{
	struct drm_exec_priv *priv;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	test->priv = priv;

	priv->dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->dev);

	priv->drm = __drm_kunit_helper_alloc_drm_device(test, priv->dev, sizeof(*priv->drm), 0,
							DRIVER_MODESET);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->drm);

	return 0;
}

static void sanitycheck(struct kunit *test)
{
	struct drm_exec exec;

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	drm_exec_fini(&exec);
	KUNIT_SUCCEED(test);
}

static void test_lock(struct kunit *test)
{
	struct drm_exec_priv *priv = test->priv;
	struct drm_gem_object gobj = { };
	struct drm_exec exec;
	int ret;

	drm_gem_private_object_init(priv->drm, &gobj, PAGE_SIZE);

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	drm_exec_until_all_locked(&exec) {
		ret = drm_exec_lock_obj(&exec, &gobj);
		drm_exec_retry_on_contention(&exec);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			break;
	}
	drm_exec_fini(&exec);
}

static void test_lock_unlock(struct kunit *test)
{
	struct drm_exec_priv *priv = test->priv;
	struct drm_gem_object gobj = { };
	struct drm_exec exec;
	int ret;

	drm_gem_private_object_init(priv->drm, &gobj, PAGE_SIZE);

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	drm_exec_until_all_locked(&exec) {
		ret = drm_exec_lock_obj(&exec, &gobj);
		drm_exec_retry_on_contention(&exec);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			break;

		drm_exec_unlock_obj(&exec, &gobj);
		ret = drm_exec_lock_obj(&exec, &gobj);
		drm_exec_retry_on_contention(&exec);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			break;
	}
	drm_exec_fini(&exec);
}

static void test_duplicates(struct kunit *test)
{
	struct drm_exec_priv *priv = test->priv;
	struct drm_gem_object gobj = { };
	struct drm_exec exec;
	int ret;

	drm_gem_private_object_init(priv->drm, &gobj, PAGE_SIZE);

	drm_exec_init(&exec, DRM_EXEC_IGNORE_DUPLICATES, 0);
	drm_exec_until_all_locked(&exec) {
		ret = drm_exec_lock_obj(&exec, &gobj);
		drm_exec_retry_on_contention(&exec);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			break;

		ret = drm_exec_lock_obj(&exec, &gobj);
		drm_exec_retry_on_contention(&exec);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			break;
	}
	drm_exec_unlock_obj(&exec, &gobj);
	drm_exec_fini(&exec);
}

static void test_prepare(struct kunit *test)
{
	struct drm_exec_priv *priv = test->priv;
	struct drm_gem_object gobj = { };
	struct drm_exec exec;
	int ret;

	drm_gem_private_object_init(priv->drm, &gobj, PAGE_SIZE);

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	drm_exec_until_all_locked(&exec) {
		ret = drm_exec_prepare_obj(&exec, &gobj, 1);
		drm_exec_retry_on_contention(&exec);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			break;
	}
	drm_exec_fini(&exec);

	drm_gem_private_object_fini(&gobj);
}

static void test_prepare_array(struct kunit *test)
{
	struct drm_exec_priv *priv = test->priv;
	struct drm_gem_object *gobj1;
	struct drm_gem_object *gobj2;
	struct drm_gem_object *array[] = {
		(gobj1 = kunit_kzalloc(test, sizeof(*gobj1), GFP_KERNEL)),
		(gobj2 = kunit_kzalloc(test, sizeof(*gobj2), GFP_KERNEL)),
	};
	struct drm_exec exec;
	int ret;

	if (!gobj1 || !gobj2) {
		KUNIT_FAIL(test, "Failed to allocate GEM objects.\n");
		return;
	}

	drm_gem_private_object_init(priv->drm, gobj1, PAGE_SIZE);
	drm_gem_private_object_init(priv->drm, gobj2, PAGE_SIZE);

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	drm_exec_until_all_locked(&exec)
		ret = drm_exec_prepare_array(&exec, array, ARRAY_SIZE(array),
					     1);
	KUNIT_EXPECT_EQ(test, ret, 0);
	drm_exec_fini(&exec);

	drm_gem_private_object_fini(gobj1);
	drm_gem_private_object_fini(gobj2);
}

static void test_multiple_loops(struct kunit *test)
{
	struct drm_exec exec;

	{
		__label__ drm_exec_retry;

		drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
		drm_exec_until_all_locked(&exec)
		{
			break;
		}
		drm_exec_fini(&exec);
	}

	{
		__label__ drm_exec_retry;

		drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
		drm_exec_until_all_locked(&exec)
		{
			break;
		}
		drm_exec_fini(&exec);
	}
	KUNIT_SUCCEED(test);
}

static void test_inline_objects(struct kunit *test)
{
	struct drm_exec_priv *priv = test->priv;
	const unsigned int count = DRM_EXEC_INLINE_OBJECTS + 1;
	DECLARE_BITMAP(seen, DRM_EXEC_INLINE_OBJECTS + 1) = {};
	struct drm_gem_object *gobj, *obj;
	struct drm_exec exec;
	unsigned int i;
	int ret = 0;

	gobj = kunit_kcalloc(test, count, sizeof(*gobj), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, gobj);

	for (i = 0; i < count; i++)
		drm_gem_private_object_init(priv->drm, &gobj[i], PAGE_SIZE);

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	KUNIT_EXPECT_PTR_EQ(test, exec.objects, &exec.inline_objects[0]);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < count; i++) {
			ret = drm_exec_lock_obj(&exec, &gobj[i]);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				break;

			/* Only spills out of the struct past the inline ones */
			if (exec.num_objects <= DRM_EXEC_INLINE_OBJECTS)
				KUNIT_EXPECT_PTR_EQ(test, exec.objects,
						    &exec.inline_objects[0]);
			else
				KUNIT_EXPECT_PTR_NE(test, exec.objects,
						    &exec.inline_objects[0]);
		}
	}
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, exec.num_objects, count);
	KUNIT_EXPECT_EQ(test, exec.max_objects, PAGE_SIZE / sizeof(void *));
	/* Contention may reorder them, but each must be tracked once */
	drm_exec_for_each_locked_object(&exec, obj) {
		KUNIT_ASSERT_TRUE(test, obj >= gobj && obj < gobj + count);
		KUNIT_EXPECT_FALSE(test, test_and_set_bit(obj - gobj, seen));
	}
	KUNIT_EXPECT_TRUE(test, bitmap_full(seen, count));
	drm_exec_fini(&exec);

	/* A larger initial size skips the inline objects altogether */
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, count);
	KUNIT_EXPECT_PTR_NE(test, exec.objects, &exec.inline_objects[0]);
	KUNIT_EXPECT_EQ(test, exec.max_objects, count);
	drm_exec_fini(&exec);

	for (i = 0; i < count; i++)
		drm_gem_private_object_fini(&gobj[i]);
}

static struct kunit_case drm_exec_tests[] = {
	KUNIT_CASE(sanitycheck),
	KUNIT_CASE(test_lock),
	KUNIT_CASE(test_lock_unlock),
	KUNIT_CASE(test_duplicates),
	KUNIT_CASE(test_prepare),
	KUNIT_CASE(test_prepare_array),
	KUNIT_CASE(test_multiple_loops),
	KUNIT_CASE(test_inline_objects),
	{}
};

static struct kunit_suite drm_exec_test_suite = {
	.name = "drm_exec",
	.init = drm_exec_test_init,
	.test_cases = drm_exec_tests,
};

kunit_test_suite(drm_exec_test_suite);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("Kunit test for drm_exec functions");
MODULE_LICENSE("GPL and additional rights");
