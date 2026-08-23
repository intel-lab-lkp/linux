// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for user namespace map insertion and sorting.
 */

#include <kunit/test.h>
#include <linux/user_namespace.h>

static void test_user_ns_map_insert_base(struct kunit *test)
{
	struct uid_gid_map map;
	struct uid_gid_extent extent;
	int i, ret;

	memset(&map, 0, sizeof(map));

	/* Insert up to UID_GID_MAP_MAX_BASE_EXTENTS (5) elements */
	for (i = 0; i < UID_GID_MAP_MAX_BASE_EXTENTS; i++) {
		extent.first = i * 10;
		extent.lower_first = i * 100;
		extent.count = 5;

		ret = insert_extent(&map, &extent);
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, map.nr_extents, i + 1);
		KUNIT_EXPECT_EQ(test, map.extent[i].first, i * 10);
		KUNIT_EXPECT_EQ(test, map.extent[i].lower_first, i * 100);
		KUNIT_EXPECT_EQ(test, map.extent[i].count, 5);
	}
}

static void test_user_ns_map_insert_extended(struct kunit *test)
{
	struct uid_gid_map map;
	struct uid_gid_extent extent;
	int i, ret;

	memset(&map, 0, sizeof(map));

	/* Insert more than UID_GID_MAP_MAX_BASE_EXTENTS (e.g., 10) elements */
	for (i = 0; i < 10; i++) {
		extent.first = i * 10;
		extent.lower_first = i * 100;
		extent.count = 5;

		ret = insert_extent(&map, &extent);
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, map.nr_extents, i + 1);

		if (i < UID_GID_MAP_MAX_BASE_EXTENTS) {
			KUNIT_EXPECT_EQ(test, map.extent[i].first, i * 10);
		} else {
			KUNIT_EXPECT_NOT_ERR_OR_NULL(test, map.forward);
			KUNIT_EXPECT_EQ(test, map.forward[i].first, i * 10);
			KUNIT_EXPECT_EQ(test, map.forward[i].lower_first, i * 100);
			KUNIT_EXPECT_EQ(test, map.forward[i].count, 5);
		}
	}

	/* Now sort the map to set up reverse mapping */
	ret = sort_idmaps(&map);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, map.reverse);

	/* Verify sorting is correct */
	for (i = 0; i < map.nr_extents; i++) {
		KUNIT_EXPECT_EQ(test, map.forward[i].count, 5);
		KUNIT_EXPECT_EQ(test, map.reverse[i].count, 5);
	}

	/* Clean up allocations to avoid leaks */
	kfree(map.forward);
	kfree(map.reverse);
}

static struct kunit_case user_ns_map_test_cases[] = {
	KUNIT_CASE(test_user_ns_map_insert_base),
	KUNIT_CASE(test_user_ns_map_insert_extended),
	{}
};

static struct kunit_suite user_ns_map_test_suite = {
	.name = "user_ns_map",
	.test_cases = user_ns_map_test_cases,
};

kunit_test_suite(user_ns_map_test_suite);
