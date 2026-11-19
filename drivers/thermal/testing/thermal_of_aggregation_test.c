// SPDX-License-Identifier: GPL-2.0
/* Included by thermal_of.c to exercise private OF lifecycle operations. */

#include <kunit/of.h>
#include <kunit/test.h>
#include <linux/kthread.h>

struct thermal_of_test_reading {
	int temperature;
	struct completion *entered;
	struct completion *release;
};

struct thermal_of_test_context {
	struct device_node *provider;
	struct device_node *root;
	struct device_node *provider_zero;
	struct thermal_zone_device *sources[2];
	struct thermal_of_test_reading readings[2];
	struct thermal_of_aggregate aggregate;
	bool listed;
	bool was_ready;
};

static int thermal_of_test_temp(struct thermal_zone_device *tz, int *temp)
{
	struct thermal_of_test_reading *reading = thermal_zone_device_priv(tz);

	lockdep_assert_held(&tz->lock);
	if (reading->entered) {
		complete(reading->entered);
		wait_for_completion(reading->release);
	}
	*temp = reading->temperature;
	return 0;
}

static const struct thermal_zone_device_ops thermal_of_test_ops = {
	.get_temp = thermal_of_test_temp,
};

static void thermal_of_test_cleanup(void *data)
{
	struct thermal_of_test_context *ctx = data;
	unsigned int i;

	{
		guard(mutex)(&thermal_of_lock);
		if (ctx->listed)
			list_del(&ctx->aggregate.node);
		thermal_of_aggregate_release(&ctx->aggregate);
		thermal_of_aggregates_ready = ctx->was_ready;
	}
	for (i = 0; i < ARRAY_SIZE(ctx->sources); i++)
		if (ctx->sources[i])
			thermal_of_zone_unregister(ctx->sources[i]);
	of_node_put(ctx->aggregate.np);
	of_node_put(ctx->provider);
	of_node_put(ctx->provider_zero);
	of_node_put(ctx->root);
}

static int thermal_of_test_init(struct kunit *test)
{
	const struct thermal_zone_params params = { .no_hwmon = true };
	struct thermal_of_test_context *ctx;
	unsigned int i;
	int ret;

	ret = of_overlay_apply_kunit(test, thermal_aggregation);
	if (ret)
		return ret;
	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	{
		guard(mutex)(&thermal_of_lock);
		ctx->was_ready = thermal_of_aggregates_ready;
		/* The empty UML tree has no /thermal-zones for the late init scan. */
		thermal_of_aggregates_ready = true;
	}
	ret = kunit_add_action_or_reset(test, thermal_of_test_cleanup, ctx);
	if (ret)
		return ret;
	ctx->root = of_find_node_by_path("/thermal-kunit");
	if (!ctx->root)
		return -ENODEV;
	ctx->provider = of_get_child_by_name(ctx->root, "sensor");
	if (!ctx->provider)
		return -ENODEV;
	ctx->provider_zero = of_get_child_by_name(ctx->root, "sensor-zero");
	if (!ctx->provider_zero)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(ctx->sources); i++) {
		struct thermal_zone_device *tz;

		ctx->readings[i].temperature = 20000 + i * 10000;
		tz = thermal_zone_device_register_with_trips("of-kunit-source", NULL, 0,
							     &ctx->readings[i],
							     &thermal_of_test_ops, &params, 0, 0);
		if (IS_ERR(tz))
			return PTR_ERR(tz);
		ctx->sources[i] = tz;
	}
	return 0;
}

static void thermal_of_deferred_lifecycle_test(struct kunit *test)
{
	struct thermal_of_test_context *ctx = test->priv;
	struct thermal_zone_device *old_source;
	const struct thermal_zone_params params = { .no_hwmon = true };
	int temp;

	ctx->aggregate.np = of_get_child_by_name(ctx->root, "average");
	KUNIT_ASSERT_NOT_NULL(test, ctx->aggregate.np);
	{
		guard(mutex)(&thermal_of_lock);
		list_add(&ctx->aggregate.node, &thermal_of_aggregates);
		ctx->listed = true;
		KUNIT_EXPECT_EQ(test, thermal_of_aggregate_create(&ctx->aggregate), -EPROBE_DEFER);
		KUNIT_EXPECT_PTR_EQ(test, ctx->aggregate.aggregate.entries, NULL);
	}
	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider, 0, ctx->sources[0]), 0);
	KUNIT_EXPECT_PTR_EQ(test, ctx->aggregate.tz, NULL);
	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider, 1, ctx->sources[1]), 0);
	KUNIT_ASSERT_NOT_NULL(test, ctx->aggregate.tz);
	KUNIT_ASSERT_EQ(test, thermal_zone_get_temp(ctx->aggregate.tz, &temp), 0);
	KUNIT_EXPECT_EQ(test, temp, 25000);
	KUNIT_EXPECT_EQ(test, ctx->aggregate.aggregate.entries[0].coefficient, 500);
	KUNIT_EXPECT_EQ(test, thermal_of_source_add(ctx->provider, 0, ctx->sources[0]), -EEXIST);
	{
		struct thermal_zone_device *tz = ctx->aggregate.tz;

		guard(mutex)(&thermal_of_lock);
		thermal_of_aggregate_retry();
		KUNIT_EXPECT_PTR_EQ(test, ctx->aggregate.tz, tz);
	}

	/* Removal must destroy the consumer and release references before returning. */
	old_source = ctx->sources[0];
	ctx->sources[0] = NULL;
	thermal_of_zone_unregister(old_source);
	KUNIT_EXPECT_PTR_EQ(test, ctx->aggregate.tz, NULL);
	KUNIT_EXPECT_PTR_EQ(test, ctx->aggregate.aggregate.entries, NULL);
	KUNIT_EXPECT_FALSE(test, ctx->aggregate.failed);
	old_source = thermal_zone_device_register_with_trips("of-kunit-reloaded", NULL, 0,
							     &ctx->readings[0],
							     &thermal_of_test_ops,
							     &params, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_source);
	ctx->sources[0] = old_source;
	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider, 0, ctx->sources[0]), 0);
	KUNIT_ASSERT_NOT_NULL(test, ctx->aggregate.tz);
	KUNIT_EXPECT_EQ(test, thermal_zone_get_temp(ctx->aggregate.tz, &temp), 0);
	KUNIT_EXPECT_EQ(test, temp, 25000);
}

static void thermal_of_coefficients_test(struct kunit *test)
{
	static const struct {
		const char *name;
		int result;
	} cases[] = {
		{ "explicit", 0 },
		{ "partial", -EINVAL },
		{ "zero", -EINVAL },
		{ "overflow", -ERANGE },
		{ "bad-delay", -EINVAL },
	};
	struct thermal_of_test_context *ctx = test->priv;
	unsigned int i;
	int temp;

	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider, 0, ctx->sources[0]), 0);
	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider, 1, ctx->sources[1]), 0);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct thermal_of_aggregate aggregate = {};
		int ret;

		aggregate.np = of_get_child_by_name(ctx->root, cases[i].name);
		KUNIT_ASSERT_NOT_NULL(test, aggregate.np);
		{
			guard(mutex)(&thermal_of_lock);
			ret = thermal_of_aggregate_create(&aggregate);
		}
		KUNIT_EXPECT_EQ_MSG(test, ret, cases[i].result, "%s", cases[i].name);
		if (!ret) {
			KUNIT_EXPECT_EQ(test, aggregate.aggregate.entries[0].coefficient, 2000);
			KUNIT_EXPECT_EQ(test, aggregate.aggregate.entries[1].coefficient, -1000);
			KUNIT_EXPECT_EQ(test, thermal_zone_get_temp(aggregate.tz, &temp), 0);
			KUNIT_EXPECT_EQ(test, temp, 9000);
		} else {
			KUNIT_EXPECT_PTR_EQ(test, aggregate.aggregate.entries, NULL);
			KUNIT_EXPECT_EQ(test, aggregate.aggregate.num_entries, 0);
		}
		{
			guard(mutex)(&thermal_of_lock);
			thermal_of_aggregate_release(&aggregate);
		}
		of_node_put(aggregate.np);
	}
}

static void thermal_of_zero_cells_test(struct kunit *test)
{
	struct thermal_of_test_context *ctx = test->priv;
	int temp;

	ctx->aggregate.np = of_get_child_by_name(ctx->root, "zero-cells");
	KUNIT_ASSERT_NOT_NULL(test, ctx->aggregate.np);
	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider_zero, 0, ctx->sources[0]), 0);
	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider, 1, ctx->sources[1]), 0);
	{
		guard(mutex)(&thermal_of_lock);
		KUNIT_EXPECT_EQ(test, thermal_of_aggregate_create(&ctx->aggregate), 0);
	}
	KUNIT_ASSERT_NOT_NULL(test, ctx->aggregate.tz);
	KUNIT_EXPECT_EQ(test, thermal_zone_get_temp(ctx->aggregate.tz, &temp), 0);
	KUNIT_EXPECT_EQ(test, temp, 25000);
}

struct thermal_of_test_thread {
	struct thermal_zone_device *tz;
	struct completion started;
	struct completion done;
	bool remove;
	int result;
	int temperature;
};

static int thermal_of_test_thread_fn(void *data)
{
	struct thermal_of_test_thread *thread = data;

	complete(&thread->started);
	if (thread->remove)
		thermal_of_zone_unregister(thread->tz);
	else
		thread->result = thermal_zone_get_temp(thread->tz, &thread->temperature);
	complete(&thread->done);
	/* Keep task storage valid until the test joins with kthread_stop(). */
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void thermal_of_concurrent_removal_test(struct kunit *test)
{
	struct thermal_of_test_context *ctx = test->priv;
	struct thermal_of_test_thread reader = {}, remover = { .remove = true };
	struct task_struct *read_task, *remove_task;
	struct completion entered, release;
	unsigned long done;

	ctx->aggregate.np = of_get_child_by_name(ctx->root, "average");
	KUNIT_ASSERT_NOT_NULL(test, ctx->aggregate.np);
	{
		guard(mutex)(&thermal_of_lock);
		list_add(&ctx->aggregate.node, &thermal_of_aggregates);
		ctx->listed = true;
	}
	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider, 0, ctx->sources[0]), 0);
	KUNIT_ASSERT_EQ(test, thermal_of_source_add(ctx->provider, 1, ctx->sources[1]), 0);
	KUNIT_ASSERT_NOT_NULL(test, ctx->aggregate.tz);
	init_completion(&entered);
	init_completion(&release);
	init_completion(&reader.started);
	init_completion(&reader.done);
	init_completion(&remover.started);
	init_completion(&remover.done);
	ctx->readings[0].entered = &entered;
	ctx->readings[0].release = &release;
	reader.tz = ctx->aggregate.tz;
	remover.tz = ctx->sources[0];
	read_task = kthread_run(thermal_of_test_thread_fn, &reader, "thermal-of-read");
	KUNIT_ASSERT_FALSE(test, IS_ERR(read_task));
	if (!wait_for_completion_timeout(&entered, HZ)) {
		complete(&release);
		kthread_stop(read_task);
		ctx->readings[0].entered = NULL;
		KUNIT_FAIL(test, "read did not reach source callback");
		return;
	}
	remove_task = kthread_run(thermal_of_test_thread_fn, &remover, "thermal-of-remove");
	if (IS_ERR(remove_task)) {
		complete(&release);
		kthread_stop(read_task);
		ctx->readings[0].entered = NULL;
		KUNIT_FAIL(test, "cannot start removal thread");
		return;
	}
	wait_for_completion_timeout(&remover.started, HZ);
	done = wait_for_completion_timeout(&remover.done, msecs_to_jiffies(20));
	KUNIT_EXPECT_EQ(test, done, 0);
	complete(&release);
	done = wait_for_completion_timeout(&reader.done, HZ);
	KUNIT_EXPECT_NE(test, done, 0);
	done = wait_for_completion_timeout(&remover.done, HZ);
	KUNIT_EXPECT_NE(test, done, 0);
	kthread_stop(read_task);
	kthread_stop(remove_task);
	ctx->sources[0] = NULL;
	ctx->readings[0].entered = NULL;
	KUNIT_EXPECT_EQ(test, reader.result, 0);
	KUNIT_EXPECT_EQ(test, reader.temperature, 25000);
	KUNIT_EXPECT_PTR_EQ(test, ctx->aggregate.tz, NULL);
	KUNIT_EXPECT_PTR_EQ(test, ctx->aggregate.aggregate.entries, NULL);
}

static struct kunit_case thermal_of_aggregation_cases[] = {
	KUNIT_CASE(thermal_of_deferred_lifecycle_test),
	KUNIT_CASE(thermal_of_coefficients_test),
	KUNIT_CASE(thermal_of_zero_cells_test),
	KUNIT_CASE(thermal_of_concurrent_removal_test),
	{}
};

static struct kunit_suite thermal_of_aggregation_suite = {
	.name = "thermal-of-aggregation",
	.init = thermal_of_test_init,
	.test_cases = thermal_of_aggregation_cases,
};

kunit_test_suite(thermal_of_aggregation_suite);
