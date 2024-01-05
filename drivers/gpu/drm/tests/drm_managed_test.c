// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_drv.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_managed.h>

#include <kunit/resource.h>

#include <linux/device.h>

struct managed_test_priv {
	struct drm_device *drm;
	bool action_done;
};

static void drm_action(struct drm_device *drm, void *ptr)
{
	struct managed_test_priv *priv = ptr;

	priv->action_done = true;
}

/*
 * The test verifies that the release action is called automatically when the
 * device is released.
 */
static void drm_test_managed_run_action(struct kunit *test)
{
	struct managed_test_priv *priv = test->priv;
	int ret;

	ret = drmm_add_action_or_reset(priv->drm, drm_action, priv);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = drm_dev_register(priv->drm, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_dev_unregister(priv->drm);
	drm_kunit_helper_free_device(test, priv->drm->dev);

	KUNIT_EXPECT_TRUE_MSG(test, priv->action_done, "Release action was not called");
}

static int drm_managed_test_init(struct kunit *test)
{
	struct managed_test_priv *priv;
	struct device *dev;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	/*
	 * DRM device can't be embedded in priv, since priv->action_done needs
	 * to remain allocated beyond both parent device and drm_device
	 * lifetime.
	 */
	priv->drm = __drm_kunit_helper_alloc_drm_device(test, dev, sizeof(*priv->drm), 0,
							DRIVER_MODESET);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->drm);

	test->priv = priv;

	return 0;
}

static struct kunit_case drm_managed_tests[] = {
	KUNIT_CASE(drm_test_managed_run_action),
	{}
};

static struct kunit_suite drm_managed_test_suite = {
	.name = "drm_test_managed",
	.init = drm_managed_test_init,
	.test_cases = drm_managed_tests
};

kunit_test_suite(drm_managed_test_suite);

MODULE_AUTHOR("Maxime Ripard <maxime@cerno.tech>");
MODULE_LICENSE("GPL");
