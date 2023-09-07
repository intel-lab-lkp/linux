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

#include "../lib/drm_random.h"

struct drm_exec_priv {
	struct device *dev;
	struct drm_device *drm;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct drm_exec *exec;
#endif
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

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
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

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
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

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
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

	drm_exec_init(&exec, DRM_EXEC_IGNORE_DUPLICATES);
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

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
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
	struct drm_gem_object gobj1 = { };
	struct drm_gem_object gobj2 = { };
	struct drm_gem_object *array[] = { &gobj1, &gobj2 };
	struct drm_exec exec;
	int ret;

	drm_gem_private_object_init(priv->drm, &gobj1, PAGE_SIZE);
	drm_gem_private_object_init(priv->drm, &gobj2, PAGE_SIZE);

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
	drm_exec_until_all_locked(&exec)
		ret = drm_exec_prepare_array(&exec, array, ARRAY_SIZE(array),
					     1);
	KUNIT_EXPECT_EQ(test, ret, 0);
	drm_exec_fini(&exec);

	drm_gem_private_object_fini(&gobj1);
	drm_gem_private_object_fini(&gobj2);
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
static void drm_exec_test_obj_free(struct drm_gem_object *gem)
{
	struct kunit *test = current->kunit_test;
	struct drm_exec_priv *priv = test->priv;
	bool resv_class_held;
	bool first_object_locked;

	/*
	 * The lock alloc tracking code may warn if the dma_resv lock
	 * class is still held, and we're freeing the first object we
	 * locked.
	 */
	resv_class_held = (lockdep_is_held(&gem->resv->lock.base) ==
			   LOCK_STATE_HELD);
	first_object_locked = (gem == priv->exec->objects[0]);
	KUNIT_EXPECT_FALSE(current->kunit_test,
			   resv_class_held && first_object_locked);

	dma_resv_fini(gem->resv);
	kfree(gem);
}

static const struct drm_gem_object_funcs put_funcs = {
	.free = drm_exec_test_obj_free,
};

/*
 * Check that freeing objects from within drm_exec_fini()
 * doesn't trigger a false lock alloc warning due to
 * the dma_resv lock *class* still being held and we're
 * freeing the first object locked, which *might* be
 * registered as the address of the held lock of that
 * lock class.
 */
static void test_early_put(struct kunit *test)
{
	struct drm_exec_priv *priv = test->priv;
	struct drm_gem_object *gobj1;
	struct drm_gem_object *gobj2;
	struct drm_gem_object *array[2];
	struct drm_exec exec;
	int ret;

	priv->exec = &exec;

	gobj1 = kzalloc(sizeof(*gobj1), GFP_KERNEL);
	KUNIT_EXPECT_NOT_NULL(test, gobj1);
	if (!gobj1)
		return;

	gobj2 = kzalloc(sizeof(*gobj2), GFP_KERNEL);
	KUNIT_EXPECT_NOT_NULL(test, gobj2);
	if (!gobj2) {
		kfree(gobj1);
		return;
	}

	gobj1->funcs = &put_funcs;
	gobj2->funcs = &put_funcs;
	drm_gem_private_object_init(priv->drm, gobj1, PAGE_SIZE);
	drm_gem_private_object_init(priv->drm, gobj2, PAGE_SIZE);
	array[0] = gobj1;
	array[1] = gobj2;

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
	drm_exec_until_all_locked(&exec)
		ret = drm_exec_prepare_array(&exec, array, ARRAY_SIZE(array),
					     1);
	KUNIT_EXPECT_EQ(test, ret, 0);
	drm_gem_object_put(gobj1);
	drm_gem_object_put(gobj2);
	drm_exec_fini(&exec);
}
#endif

static void test_multiple_loops(struct kunit *test)
{
	struct drm_exec exec;

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
	drm_exec_until_all_locked(&exec)
	{
		break;
	}
	drm_exec_fini(&exec);

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
	drm_exec_until_all_locked(&exec)
	{
		break;
	}
	drm_exec_fini(&exec);
	KUNIT_SUCCEED(test);
}

static struct kunit_case drm_exec_tests[] = {
	KUNIT_CASE(sanitycheck),
	KUNIT_CASE(test_lock),
	KUNIT_CASE(test_lock_unlock),
	KUNIT_CASE(test_duplicates),
	KUNIT_CASE(test_prepare),
	KUNIT_CASE(test_prepare_array),
	KUNIT_CASE(test_multiple_loops),
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	KUNIT_CASE(test_early_put),
#endif
	{}
};

static struct kunit_suite drm_exec_test_suite = {
	.name = "drm_exec",
	.init = drm_exec_test_init,
	.test_cases = drm_exec_tests,
};

kunit_test_suite(drm_exec_test_suite);

MODULE_AUTHOR("AMD");
MODULE_LICENSE("GPL and additional rights");
