// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/kthread.h>

#include "../thermal_core.h"

struct aggregation_source {
	int temperature;
	int error;
	unsigned int reads;
};

struct aggregation_fixture {
	struct thermal_zone_device *sources[3];
	struct aggregation_source data[3];
	struct thermal_zone_device *destination;
	struct thermal_zone_aggregate_entry entries[3];
	struct thermal_zone_aggregate aggregate;
	struct thermal_zone_params params;
};

static int aggregation_source_temp(struct thermal_zone_device *tz, int *temp)
{
	struct aggregation_source *source = thermal_zone_device_priv(tz);

	lockdep_assert_held(&tz->lock);
	source->reads++;
	if (source->error)
		return source->error;
	*temp = source->temperature;
	return 0;
}

static const struct thermal_zone_device_ops aggregation_source_ops = {
	.get_temp = aggregation_source_temp,
};

static const struct thermal_zone_device_ops aggregation_destination_ops;

static void aggregation_cleanup(void *data)
{
	struct aggregation_fixture *fixture = data;
	unsigned int i;

	if (fixture->destination)
		thermal_zone_device_unregister(fixture->destination);
	for (i = 0; i < ARRAY_SIZE(fixture->sources); i++)
		if (fixture->sources[i])
			thermal_zone_device_unregister(fixture->sources[i]);
}

static int aggregation_init(struct kunit *test)
{
	const struct thermal_zone_params params = { .no_hwmon = true };
	struct aggregation_fixture *fixture;
	unsigned int i;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return -ENOMEM;
	test->priv = fixture;
	if (kunit_add_action_or_reset(test, aggregation_cleanup, fixture))
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(fixture->sources); i++) {
		struct thermal_zone_device *tz;

		fixture->data[i].temperature = 20000 + i * 10000;
		tz = thermal_zone_device_register_with_trips("kunit-source", NULL, 0,
							     &fixture->data[i],
							     &aggregation_source_ops,
							     &params, 0, 0);
		if (IS_ERR(tz))
			return PTR_ERR(tz);
		fixture->sources[i] = tz;
		fixture->entries[i].tz = tz;
	}
	fixture->aggregate.entries = fixture->entries;
	fixture->aggregate.num_entries = 3;
	fixture->params.no_hwmon = true;
	fixture->params.aggregate = &fixture->aggregate;
	return 0;
}

static void aggregation_defaults_test(struct kunit *test)
{
	static const unsigned int counts[] = { 1, 2, 3, 4, 5, 6, 1000 };
	static const s32 six[] = { 167, 167, 167, 167, 166, 166 };
	struct thermal_zone_aggregate aggregate = {};
	unsigned int c, i;

	aggregate.entries = kunit_kcalloc(test, 1001, sizeof(*aggregate.entries), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, aggregate.entries);
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_init_coefficients(NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_init_coefficients(&aggregate), -EINVAL);
	for (c = 0; c < ARRAY_SIZE(counts); c++) {
		s32 sum = 0;

		memset(aggregate.entries, 0, 1001 * sizeof(*aggregate.entries));
		aggregate.num_entries = counts[c];
		KUNIT_ASSERT_EQ(test, thermal_zone_aggregate_init_coefficients(&aggregate), 0);
		for (i = 0; i < aggregate.num_entries; i++) {
			s32 value = aggregate.entries[i].coefficient;

			sum += value;
			KUNIT_EXPECT_GT(test, value, 0);
			if (i)
				KUNIT_EXPECT_LE(test,
						aggregate.entries[i - 1].coefficient - value, 1);
			if (counts[c] == 3)
				KUNIT_EXPECT_EQ(test, value, i ? 333 : 334);
			if (counts[c] == 6)
				KUNIT_EXPECT_EQ(test, value, six[i]);
		}
		KUNIT_EXPECT_EQ(test, sum, 1000);
	}
	memset(aggregate.entries, 0, 1001 * sizeof(*aggregate.entries));
	aggregate.num_entries = 1001;
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_init_coefficients(&aggregate), -EINVAL);
	KUNIT_EXPECT_EQ(test, aggregate.entries[0].coefficient, 0);
}

static int aggregation_set_trips(struct thermal_zone_device *tz, int low, int high)
{
	return 0;
}

static void aggregation_validation_test(struct kunit *test)
{
	struct aggregation_fixture *fixture = test->priv;
	struct thermal_zone_device_ops ops = {};
	struct thermal_zone_params nested = { .aggregate = &fixture->aggregate };
	struct thermal_zone_device *source = fixture->sources[0];
	struct thermal_zone_params *saved_params = source->tzp;

	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(NULL, &ops), -EINVAL);
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(&fixture->aggregate, NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(&fixture->aggregate, &ops), 0);
	fixture->entries[0].coefficient = 500;
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(&fixture->aggregate, &ops), -EINVAL);
	fixture->entries[1].coefficient = -500;
	fixture->entries[2].coefficient = 2000;
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(&fixture->aggregate, &ops), 0);
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_init_coefficients(&fixture->aggregate), 0);
	KUNIT_EXPECT_EQ(test, fixture->entries[0].coefficient, 500);
	KUNIT_EXPECT_EQ(test, fixture->entries[1].coefficient, -500);
	ops.set_trips = aggregation_set_trips;
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(&fixture->aggregate, &ops), -EINVAL);
	ops.set_trips = NULL;
	fixture->entries[0].tz = NULL;
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(&fixture->aggregate, &ops), -ENODEV);
	fixture->entries[0].tz = source;
	source->tzp = &nested;
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(&fixture->aggregate, &ops), -EINVAL);
	source->tzp = saved_params;
	source->state |= TZ_STATE_FLAG_EXIT;
	KUNIT_EXPECT_EQ(test, thermal_zone_aggregate_validate(&fixture->aggregate, &ops), -ENODEV);
	source->state &= ~TZ_STATE_FLAG_EXIT;
}

struct aggregation_case {
	const char *name;
	unsigned int count;
	s32 coefficients[3];
	int temperatures[3];
	int offset;
	int error;
	int expected;
	int result;
};

static const struct aggregation_case aggregation_cases[] = {
	{ "single", 1, { 1000 }, { 42000 }, 0, 0, 42000, 0 },
	{ "affine", 2, { 700, 500 }, { 20000, 30000 }, -2000, 0, 27000, 0 },
	{ "fractional", 2, { 250, 750 }, { 10000, 20000 }, 0, 0, 17500, 0 },
	{ "divide once", 2, { 500, 500 }, { 1, 1 }, 0, 0, 1, 0 },
	{ "negative rounding", 1, { -500 }, { 3 }, -2, 0, -3, 0 },
	{ "negative source", 1, { 1500 }, { -10000 }, 0, 0, -15000, 0 },
	{ "zero sum", 2, { 500, -500 }, { 20000, 10000 }, 0, 0, 5000, 0 },
	{ "zero multiplier at read time", 2, { 0, 1000 }, { 20000, 30000 }, 0, 0, 30000, 0 },
	{ "read error", 2, { 1000, 1000 }, { 20000, 30000 }, 0, -EIO, 0, -EIO },
	{ "invalid reading", 1, { 1000 }, { THERMAL_TEMP_INVALID }, 0, 0, 0, -ENODATA },
	{ "positive overflow", 3, { S32_MAX, S32_MAX, S32_MAX },
	  { INT_MAX, INT_MAX, INT_MAX }, 0, 0, 0, -ERANGE },
	{ "negative overflow", 3, { S32_MIN, S32_MIN, S32_MIN },
	  { INT_MAX, INT_MAX, INT_MAX }, 0, 0, 0, -ERANGE },
	{ "upper range", 1, { 1000 }, { INT_MAX }, 1, 0, 0, -ERANGE },
	{ "lower range", 1, { -2000 }, { INT_MAX }, 0, 0, 0, -ERANGE },
};

static void aggregation_case_desc(const struct aggregation_case *param, char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(aggregation, aggregation_cases, aggregation_case_desc);

static void aggregation_calculation_test(struct kunit *test)
{
	const struct aggregation_case *param = test->param_value;
	struct aggregation_fixture *fixture = test->priv;
	struct thermal_zone_device *tz;
	unsigned int i;
	int temp = 12345;
	int ret;

	/* A private destination allows testing post-initialization zero values. */
	tz = kunit_kzalloc(test, sizeof(*tz), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tz);
	mutex_init(&tz->lock);
	tz->tzp = &fixture->params;
	fixture->aggregate.num_entries = param->count;
	fixture->params.offset = param->offset;
	for (i = 0; i < param->count; i++) {
		fixture->entries[i].coefficient = param->coefficients[i];
		fixture->data[i].temperature = param->temperatures[i];
	}
	fixture->data[0].error = param->error;
	mutex_lock(&tz->lock);
	ret = thermal_zone_get_temp_aggregate(tz, &temp);
	mutex_unlock(&tz->lock);
	KUNIT_EXPECT_EQ(test, ret, param->result);
	KUNIT_EXPECT_EQ(test, temp, ret ? 12345 : param->expected);
	if (param->error)
		KUNIT_EXPECT_EQ(test, fixture->data[1].reads, 0);
	mutex_destroy(&tz->lock);
}

static void aggregation_registration_test(struct kunit *test)
{
	struct aggregation_fixture *fixture = test->priv;
	struct thermal_zone_device *tz;
	int temp;

	tz = thermal_zone_device_register_with_trips("kunit-aggregate", NULL, 0, NULL,
						     &aggregation_destination_ops,
						     &fixture->params, 0, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tz);
	fixture->destination = tz;
	KUNIT_EXPECT_EQ(test, fixture->entries[0].coefficient, 334);
	KUNIT_ASSERT_EQ(test, thermal_zone_get_temp(tz, &temp), 0);
	KUNIT_EXPECT_EQ(test, temp, 29990);
	KUNIT_ASSERT_EQ(test, thermal_zone_device_enable(tz), 0);
	KUNIT_EXPECT_EQ(test, tz->temperature, 29990);
	KUNIT_ASSERT_EQ(test, thermal_zone_get_temp(fixture->sources[0], &temp), 0);
	KUNIT_EXPECT_EQ(test, temp, 20000);
	KUNIT_EXPECT_EQ(test, thermal_zone_get_slope(fixture->sources[0]), 0);
	fixture->sources[0]->tzp->slope = 348;
	fixture->sources[0]->tzp->offset = -9301;
	KUNIT_EXPECT_EQ(test, thermal_zone_get_slope(fixture->sources[0]), 348);
	KUNIT_EXPECT_EQ(test, thermal_zone_get_offset(fixture->sources[0]), -9301);
	KUNIT_ASSERT_EQ(test, thermal_zone_get_temp(fixture->sources[0], &temp), 0);
	KUNIT_EXPECT_EQ(test, temp, 20000);

	fixture->sources[0]->state |= TZ_STATE_FLAG_EXIT;
	KUNIT_EXPECT_EQ(test, thermal_zone_get_temp(tz, &temp), -ENODEV);
	fixture->sources[0]->state &= ~TZ_STATE_FLAG_EXIT;
}

static void aggregation_registration_failure_test(struct kunit *test)
{
	struct aggregation_fixture *fixture = test->priv;
	struct thermal_zone_device_ops ops = { .set_trips = aggregation_set_trips };
	struct thermal_zone_device *tz;

	tz = thermal_zone_device_register_with_trips("kunit-rejected", NULL, 0, NULL,
						     &ops, &fixture->params, 0, 0);
	KUNIT_EXPECT_EQ(test, PTR_ERR(tz), -EINVAL);
	fixture->entries[0].coefficient = 1000;
	tz = thermal_zone_device_register_with_trips("kunit-partial", NULL, 0, NULL,
						     &aggregation_destination_ops,
						     &fixture->params, 0, 0);
	KUNIT_EXPECT_EQ(test, PTR_ERR(tz), -EINVAL);
	KUNIT_EXPECT_EQ(test, fixture->entries[1].coefficient, 0);
}

struct aggregation_busy_context {
	struct thermal_zone_device *tz;
	struct completion locked;
	struct completion release;
};

static int aggregation_hold_source(void *data)
{
	struct aggregation_busy_context *ctx = data;

	mutex_lock(&ctx->tz->lock);
	complete(&ctx->locked);
	wait_for_completion(&ctx->release);
	mutex_unlock(&ctx->tz->lock);
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void aggregation_busy_source_test(struct kunit *test)
{
	struct aggregation_fixture *fixture = test->priv;
	struct aggregation_busy_context ctx = { .tz = fixture->sources[0] };
	struct task_struct *task;
	struct thermal_zone_device *tz;
	int temp = 12345;

	tz = thermal_zone_device_register_with_trips("kunit-busy", NULL, 0, NULL,
						     &aggregation_destination_ops,
						     &fixture->params, 0, 0);
	if (IS_ERR(tz)) {
		KUNIT_FAIL(test, "cannot register destination");
		return;
	}
	fixture->destination = tz;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fixture->destination);
	init_completion(&ctx.locked);
	init_completion(&ctx.release);
	task = kthread_run(aggregation_hold_source, &ctx, "thermal-kunit-lock");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	if (!wait_for_completion_timeout(&ctx.locked, HZ)) {
		complete(&ctx.release);
		kthread_stop(task);
		KUNIT_FAIL(test, "source lock was not acquired");
		return;
	}
	KUNIT_EXPECT_EQ(test, thermal_zone_get_temp(fixture->destination, &temp), -EAGAIN);
	KUNIT_EXPECT_EQ(test, temp, 12345);
	complete(&ctx.release);
	kthread_stop(task);
}

static struct kunit_case thermal_aggregation_cases[] = {
	KUNIT_CASE(aggregation_defaults_test),
	KUNIT_CASE(aggregation_validation_test),
	KUNIT_CASE_PARAM(aggregation_calculation_test, aggregation_gen_params),
	KUNIT_CASE(aggregation_registration_test),
	KUNIT_CASE(aggregation_registration_failure_test),
	KUNIT_CASE(aggregation_busy_source_test),
	{}
};

static struct kunit_suite thermal_aggregation_suite = {
	.name = "thermal-aggregation",
	.init = aggregation_init,
	.test_cases = thermal_aggregation_cases,
};

kunit_test_suite(thermal_aggregation_suite);

MODULE_LICENSE("GPL");
