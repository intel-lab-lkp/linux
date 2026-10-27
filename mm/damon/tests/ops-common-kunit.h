/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CONFIG_DAMON_KUNIT_TEST

#include <kunit/test.h>

static void damon_test_hot_score_large_weight(struct kunit *test)
{
	struct damon_ctx ctx = {
		.attrs = {
			.sample_interval = 1000,
			.aggr_interval = 100000,
		},
	};
	struct damon_region region = {
		.nr_accesses = 100,
	};
	struct damos scheme = {
		.quota = {
			.weight_nr_accesses = 50000000,
		},
	};

	KUNIT_EXPECT_EQ(test, damon_hot_score(&ctx, &region, &scheme), 99);

	scheme.quota.weight_nr_accesses = UINT_MAX;
	scheme.quota.weight_age = UINT_MAX;
	KUNIT_EXPECT_EQ(test, damon_hot_score(&ctx, &region, &scheme), 74);

	scheme.quota.weight_nr_accesses = 0;
	scheme.quota.weight_age = 0;
	KUNIT_EXPECT_EQ(test, damon_hot_score(&ctx, &region, &scheme), 0);
}

static struct kunit_case damon_ops_common_test_cases[] = {
	KUNIT_CASE(damon_test_hot_score_large_weight),
	{}
};

static struct kunit_suite damon_ops_common_test_suite = {
	.name = "damon-ops-common",
	.test_cases = damon_ops_common_test_cases,
};

kunit_test_suite(damon_ops_common_test_suite);

#endif /* CONFIG_DAMON_KUNIT_TEST */
