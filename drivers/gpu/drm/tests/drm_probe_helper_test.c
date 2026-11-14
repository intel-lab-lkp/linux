// SPDX-License-Identifier: GPL-2.0
/*
 * Kunit test for drm_probe_helper functions
 */

#include <linux/completion.h>
#include <linux/kthread.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_mode.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_probe_helper.h>

#include <kunit/test.h>

struct drm_probe_helper_contender;

struct drm_probe_helper_test_priv {
	struct drm_device *drm;
	struct device *dev;
	struct drm_connector connector;

	unsigned int force_count;
	unsigned int force_ctx_count;
	int force_ctx_ret;
	bool force_ctx_locked;
	enum drm_connector_status force_ctx_status;
	struct drm_probe_helper_contender *contender;
};

static const struct drm_connector_helper_funcs drm_probe_helper_connector_helper_funcs = {
};

static const struct drm_connector_funcs drm_probe_helper_connector_funcs = {
	.atomic_create_state	= drm_atomic_helper_connector_create_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
};

static int drm_probe_helper_test_init(struct kunit *test)
{
	struct drm_probe_helper_test_priv *priv;
	struct drm_connector *connector;
	int ret;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	test->priv = priv;

	priv->dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->dev);

	priv->drm = __drm_kunit_helper_alloc_drm_device(test, priv->dev,
							sizeof(*priv->drm), 0,
							DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->drm);

	connector = &priv->connector;
	ret = drmm_connector_init(priv->drm, connector,
				  &drm_probe_helper_connector_funcs,
				  DRM_MODE_CONNECTOR_Unknown,
				  NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_connector_helper_add(connector, &drm_probe_helper_connector_helper_funcs);

	return 0;
}

typedef struct drm_display_mode *(*expected_mode_func_t)(struct drm_device *);

struct drm_connector_helper_tv_get_modes_test {
	const char *name;
	unsigned int supported_tv_modes;
	enum drm_connector_tv_mode default_mode;
	bool cmdline;
	enum drm_connector_tv_mode cmdline_mode;
	expected_mode_func_t *expected_modes;
	unsigned int num_expected_modes;
};

#define _TV_MODE_TEST(_name, _supported, _default, _cmdline, _cmdline_mode, ...)		\
	{											\
		.name = _name,									\
		.supported_tv_modes = _supported,						\
		.default_mode = _default,							\
		.cmdline = _cmdline,								\
		.cmdline_mode = _cmdline_mode,							\
		.expected_modes = (expected_mode_func_t[]) { __VA_ARGS__ },			\
		.num_expected_modes = sizeof((expected_mode_func_t[]) { __VA_ARGS__ }) /	\
				      (sizeof(expected_mode_func_t)),				\
	}

#define TV_MODE_TEST(_name, _supported, _default, ...)			\
	_TV_MODE_TEST(_name, _supported, _default, false, 0, __VA_ARGS__)

#define TV_MODE_TEST_CMDLINE(_name, _supported, _default, _cmdline, ...) \
	_TV_MODE_TEST(_name, _supported, _default, true, _cmdline, __VA_ARGS__)

static void
drm_test_connector_helper_tv_get_modes_check(struct kunit *test)
{
	const struct drm_connector_helper_tv_get_modes_test *params = test->param_value;
	struct drm_probe_helper_test_priv *priv = test->priv;
	struct drm_connector *connector = &priv->connector;
	struct drm_cmdline_mode *cmdline = &connector->cmdline_mode;
	struct drm_display_mode *mode;
	struct drm_display_mode *expected;
	size_t len;
	int ret;

	if (params->cmdline) {
		cmdline->tv_mode_specified = true;
		cmdline->tv_mode = params->cmdline_mode;
	}

	ret = drm_mode_create_tv_properties(priv->drm, params->supported_tv_modes);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_object_attach_property(&connector->base,
				   priv->drm->mode_config.tv_mode_property,
				   params->default_mode);

	mutex_lock(&priv->drm->mode_config.mutex);

	ret = drm_connector_helper_tv_get_modes(connector);
	KUNIT_EXPECT_EQ(test, ret, params->num_expected_modes);

	len = 0;
	list_for_each_entry(mode, &connector->probed_modes, head)
		len++;
	KUNIT_EXPECT_EQ(test, len, params->num_expected_modes);

	if (params->num_expected_modes >= 1) {
		mode = list_first_entry_or_null(&connector->probed_modes,
						struct drm_display_mode, head);
		KUNIT_ASSERT_NOT_NULL(test, mode);

		expected = params->expected_modes[0](priv->drm);
		KUNIT_ASSERT_NOT_NULL(test, expected);

		KUNIT_EXPECT_TRUE(test, drm_mode_equal(mode, expected));
		KUNIT_EXPECT_TRUE(test, mode->type & DRM_MODE_TYPE_PREFERRED);

		ret = drm_kunit_add_mode_destroy_action(test, expected);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	if (params->num_expected_modes >= 2) {
		mode = list_next_entry(mode, head);
		KUNIT_ASSERT_NOT_NULL(test, mode);

		expected = params->expected_modes[1](priv->drm);
		KUNIT_ASSERT_NOT_NULL(test, expected);

		KUNIT_EXPECT_TRUE(test, drm_mode_equal(mode, expected));
		KUNIT_EXPECT_FALSE(test, mode->type & DRM_MODE_TYPE_PREFERRED);

		ret = drm_kunit_add_mode_destroy_action(test, expected);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	mutex_unlock(&priv->drm->mode_config.mutex);
}

static const
struct drm_connector_helper_tv_get_modes_test drm_connector_helper_tv_get_modes_tests[] = {
	{ .name = "None" },
	TV_MODE_TEST("PAL",
		     BIT(DRM_MODE_TV_MODE_PAL),
		     DRM_MODE_TV_MODE_PAL,
		     drm_mode_analog_pal_576i),
	TV_MODE_TEST("NTSC",
		     BIT(DRM_MODE_TV_MODE_NTSC),
		     DRM_MODE_TV_MODE_NTSC,
		     drm_mode_analog_ntsc_480i),
	TV_MODE_TEST("Both, NTSC Default",
		     BIT(DRM_MODE_TV_MODE_NTSC) | BIT(DRM_MODE_TV_MODE_PAL),
		     DRM_MODE_TV_MODE_NTSC,
		     drm_mode_analog_ntsc_480i, drm_mode_analog_pal_576i),
	TV_MODE_TEST("Both, PAL Default",
		     BIT(DRM_MODE_TV_MODE_NTSC) | BIT(DRM_MODE_TV_MODE_PAL),
		     DRM_MODE_TV_MODE_PAL,
		     drm_mode_analog_pal_576i, drm_mode_analog_ntsc_480i),
	TV_MODE_TEST_CMDLINE("Both, NTSC Default, with PAL on command-line",
			     BIT(DRM_MODE_TV_MODE_NTSC) | BIT(DRM_MODE_TV_MODE_PAL),
			     DRM_MODE_TV_MODE_NTSC,
			     DRM_MODE_TV_MODE_PAL,
			     drm_mode_analog_pal_576i, drm_mode_analog_ntsc_480i),
	TV_MODE_TEST_CMDLINE("Both, PAL Default, with NTSC on command-line",
			     BIT(DRM_MODE_TV_MODE_NTSC) | BIT(DRM_MODE_TV_MODE_PAL),
			     DRM_MODE_TV_MODE_PAL,
			     DRM_MODE_TV_MODE_NTSC,
			     drm_mode_analog_ntsc_480i, drm_mode_analog_pal_576i),
};

static void
drm_connector_helper_tv_get_modes_desc(const struct drm_connector_helper_tv_get_modes_test *t,
				       char *desc)
{
	sprintf(desc, "%s", t->name);
}

KUNIT_ARRAY_PARAM(drm_connector_helper_tv_get_modes,
		  drm_connector_helper_tv_get_modes_tests,
		  drm_connector_helper_tv_get_modes_desc);

static struct kunit_case drm_test_connector_helper_tv_get_modes_tests[] = {
	KUNIT_CASE_PARAM(drm_test_connector_helper_tv_get_modes_check,
			 drm_connector_helper_tv_get_modes_gen_params),
	{ }
};

static struct kunit_suite drm_test_connector_helper_tv_get_modes_suite = {
	.name = "drm_connector_helper_tv_get_modes",
	.init = drm_probe_helper_test_init,
	.test_cases = drm_test_connector_helper_tv_get_modes_tests,
};

kunit_test_suite(drm_test_connector_helper_tv_get_modes_suite);

/*
 * Runs an acquire context older than the probe one, which first takes @lock,
 * then waits to be told to also request the connection_mutex held by the
 * probe context.  Being older, it wounds the probe context, which will get
 * -EDEADLK as soon as it tries to take @lock.
 */
struct drm_probe_helper_contender {
	struct drm_device *drm;
	struct drm_modeset_lock lock;
	struct completion locked;
	struct completion go;
	struct completion done;
	int ret;
};

static int drm_probe_helper_contender_fn(void *data)
{
	struct drm_probe_helper_contender *contender = data;
	struct drm_modeset_acquire_ctx ctx;

	drm_modeset_acquire_init(&ctx, 0);

	contender->ret = drm_modeset_lock(&contender->lock, &ctx);
	complete(&contender->locked);

	if (!contender->ret) {
		if (wait_for_completion_timeout(&contender->go, 5 * HZ))
			contender->ret =
				drm_modeset_lock(&contender->drm->mode_config.connection_mutex,
						 &ctx);
		else
			contender->ret = -ETIMEDOUT;
	}

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	complete(&contender->done);

	return 0;
}

static struct drm_probe_helper_test_priv *
connector_to_probe_priv(struct drm_connector *connector)
{
	return container_of(connector, struct drm_probe_helper_test_priv, connector);
}

static void drm_probe_helper_test_force(struct drm_connector *connector)
{
	connector_to_probe_priv(connector)->force_count++;
}

static int drm_probe_helper_test_force_ctx(struct drm_connector *connector,
					   struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_probe_helper_test_priv *priv = connector_to_probe_priv(connector);
	struct drm_probe_helper_contender *contender = priv->contender;
	int ret;

	priv->force_ctx_count++;
	priv->force_ctx_status = connector->status;
	priv->force_ctx_locked =
		drm_modeset_is_locked(&connector->dev->mode_config.connection_mutex);

	if (contender && priv->force_ctx_count == 1) {
		complete(&contender->go);

		ret = drm_modeset_lock(&contender->lock, ctx);
		if (ret)
			return ret;
	}

	return priv->force_ctx_ret;
}

static int drm_probe_helper_test_get_modes(struct drm_connector *connector)
{
	return 0;
}

static const struct drm_connector_helper_funcs drm_probe_helper_force_ctx_helper_funcs = {
	.get_modes	= drm_probe_helper_test_get_modes,
	.force_ctx	= drm_probe_helper_test_force_ctx,
};

static const struct drm_connector_helper_funcs drm_probe_helper_force_helper_funcs = {
	.get_modes	= drm_probe_helper_test_get_modes,
};

static const struct drm_connector_funcs drm_probe_helper_force_connector_funcs = {
	.force			= drm_probe_helper_test_force,
	.atomic_create_state	= drm_atomic_helper_connector_create_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
};

static int drm_probe_helper_force_test_init(struct kunit *test)
{
	struct drm_probe_helper_test_priv *priv;
	int ret;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	test->priv = priv;

	priv->dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->dev);

	priv->drm = __drm_kunit_helper_alloc_drm_device(test, priv->dev,
							sizeof(*priv->drm), 0,
							DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->drm);

	ret = drmm_connector_init(priv->drm, &priv->connector,
				  &drm_probe_helper_force_connector_funcs,
				  DRM_MODE_CONNECTOR_HDMIA,
				  NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_connector_helper_add(&priv->connector,
				 &drm_probe_helper_force_ctx_helper_funcs);

	priv->connector.force = DRM_FORCE_ON;

	return 0;
}

static int drm_probe_helper_test_probe(struct drm_probe_helper_test_priv *priv)
{
	int count;

	mutex_lock(&priv->drm->mode_config.mutex);

	count = drm_helper_probe_single_connector_modes(&priv->connector, 1920, 1080);

	mutex_unlock(&priv->drm->mode_config.mutex);

	return count;
}

/*
 * Test that probing a forced connector calls .force_ctx(), in favour of
 * .force(), with the forced status already set and the connection_mutex held
 * through the acquire context.
 */
static void drm_test_probe_helper_force_ctx(struct kunit *test)
{
	struct drm_probe_helper_test_priv *priv = test->priv;
	int count;

	count = drm_probe_helper_test_probe(priv);
	KUNIT_EXPECT_GT(test, count, 0);

	KUNIT_EXPECT_EQ(test, priv->force_ctx_count, 1);
	KUNIT_EXPECT_EQ(test, priv->force_count, 0);
	KUNIT_EXPECT_EQ(test, priv->force_ctx_status, connector_status_connected);
	KUNIT_EXPECT_TRUE(test, priv->force_ctx_locked);
	KUNIT_EXPECT_EQ(test, priv->connector.status, connector_status_connected);
}

/*
 * Test that probing a connector forced off calls .force_ctx() with the
 * disconnected status set.
 */
static void drm_test_probe_helper_force_ctx_off(struct kunit *test)
{
	struct drm_probe_helper_test_priv *priv = test->priv;
	int count;

	priv->connector.force = DRM_FORCE_OFF;

	count = drm_probe_helper_test_probe(priv);
	KUNIT_EXPECT_EQ(test, count, 0);

	KUNIT_EXPECT_EQ(test, priv->force_ctx_count, 1);
	KUNIT_EXPECT_EQ(test, priv->force_ctx_status, connector_status_disconnected);
	KUNIT_EXPECT_EQ(test, priv->connector.status, connector_status_disconnected);
}

/*
 * Test that a .force_ctx() error other than -EDEADLK doesn't prevent the
 * forced connector from being probed.
 */
static void drm_test_probe_helper_force_ctx_error(struct kunit *test)
{
	struct drm_probe_helper_test_priv *priv = test->priv;
	int count;

	priv->force_ctx_ret = -EIO;

	count = drm_probe_helper_test_probe(priv);
	KUNIT_EXPECT_GT(test, count, 0);

	KUNIT_EXPECT_EQ(test, priv->force_ctx_count, 1);
	KUNIT_EXPECT_EQ(test, priv->connector.status, connector_status_connected);
}

/*
 * Test that a .force_ctx() returning -EDEADLK makes the probe back off and
 * retry, while restoring the connector status beforehand, so that the status
 * change still gets detected.
 */
static void drm_test_probe_helper_force_ctx_deadlock(struct kunit *test)
{
	struct drm_probe_helper_test_priv *priv = test->priv;
	struct drm_probe_helper_contender *contender;
	struct task_struct *task;
	int count = 0;
	bool done;

	KUNIT_ASSERT_EQ(test, priv->connector.status, connector_status_unknown);
	KUNIT_ASSERT_FALSE(test, priv->drm->mode_config.delayed_event);

	contender = kunit_kzalloc(test, sizeof(*contender), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, contender);

	contender->drm = priv->drm;
	drm_modeset_lock_init(&contender->lock);
	init_completion(&contender->locked);
	init_completion(&contender->go);
	init_completion(&contender->done);

	task = kthread_run(drm_probe_helper_contender_fn, contender,
			   "drm-probe-contender");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, task);

	/* Make sure the contender context is the older one */
	wait_for_completion(&contender->locked);
	if (!contender->ret) {
		priv->contender = contender;
		count = drm_probe_helper_test_probe(priv);
	}

	/* The contender must be gone before releasing its resources */
	done = wait_for_completion_timeout(&contender->done, 10 * HZ);
	KUNIT_ASSERT_TRUE(test, done);
	KUNIT_ASSERT_EQ(test, contender->ret, 0);

	KUNIT_EXPECT_GT(test, count, 0);
	KUNIT_EXPECT_EQ(test, priv->force_ctx_count, 2);
	KUNIT_EXPECT_EQ(test, priv->force_ctx_status, connector_status_connected);
	KUNIT_EXPECT_EQ(test, priv->connector.status, connector_status_connected);
	KUNIT_EXPECT_TRUE(test, priv->drm->mode_config.delayed_event);
}

/*
 * Test that probing a forced connector still calls .force() when no
 * .force_ctx() is provided.
 */
static void drm_test_probe_helper_force(struct kunit *test)
{
	struct drm_probe_helper_test_priv *priv = test->priv;
	int count;

	drm_connector_helper_add(&priv->connector,
				 &drm_probe_helper_force_helper_funcs);

	count = drm_probe_helper_test_probe(priv);
	KUNIT_EXPECT_GT(test, count, 0);

	KUNIT_EXPECT_EQ(test, priv->force_count, 1);
	KUNIT_EXPECT_EQ(test, priv->force_ctx_count, 0);
	KUNIT_EXPECT_EQ(test, priv->connector.status, connector_status_connected);
}

static struct kunit_case drm_test_probe_helper_force_tests[] = {
	KUNIT_CASE(drm_test_probe_helper_force_ctx),
	KUNIT_CASE(drm_test_probe_helper_force_ctx_off),
	KUNIT_CASE(drm_test_probe_helper_force_ctx_error),
	KUNIT_CASE(drm_test_probe_helper_force_ctx_deadlock),
	KUNIT_CASE(drm_test_probe_helper_force),
	{ }
};

static struct kunit_suite drm_test_probe_helper_force_suite = {
	.name = "drm_probe_helper_force",
	.init = drm_probe_helper_force_test_init,
	.test_cases = drm_test_probe_helper_force_tests,
};

kunit_test_suite(drm_test_probe_helper_force_suite);

MODULE_AUTHOR("Maxime Ripard <maxime@cerno.tech>");
MODULE_DESCRIPTION("Kunit test for drm_probe_helper functions");
MODULE_LICENSE("GPL");
