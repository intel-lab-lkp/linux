// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/array_size.h>
#include <linux/gfp.h>
#include <linux/stackdepot.h>
#include <linux/string.h>

static void stackdepot_countable_public(struct kunit *test)
{
	unsigned long plain_entries[] = {
		0x141000UL,
		0x142000UL,
		0x143000UL,
	};
	unsigned long get_entries[] = {
		0x151000UL,
		0x152000UL,
		0x153000UL,
	};
	unsigned long fetched[ARRAY_SIZE(plain_entries)] = {};
	depot_flags_t countable = STACK_DEPOT_FLAG_CAN_ALLOC |
				  STACK_DEPOT_FLAG_COUNTABLE;
	struct stack_record *record;
	depot_stack_handle_t count_handle;
	depot_stack_handle_t plain_handle;
	depot_stack_handle_t get_handle;
	unsigned int get_nr = ARRAY_SIZE(get_entries);
	unsigned int plain_nr = ARRAY_SIZE(plain_entries);
	unsigned int nr_entries;

	KUNIT_ASSERT_EQ(test, stack_depot_init(), 0);

	plain_handle = stack_depot_save(plain_entries, plain_nr, GFP_KERNEL);
	KUNIT_ASSERT_NE(test, plain_handle, (depot_stack_handle_t)0);
	count_handle = stack_depot_save_flags(plain_entries, plain_nr, GFP_KERNEL,
					      countable);
	KUNIT_ASSERT_NE(test, count_handle, (depot_stack_handle_t)0);
	record = __stack_depot_get_stack_record(count_handle);
	KUNIT_ASSERT_NOT_NULL(test, record);
	KUNIT_EXPECT_EQ(test, record->size, (u16)plain_nr);
	KUNIT_EXPECT_MEMEQ(test, record->entries, plain_entries,
			   sizeof(plain_entries));
	nr_entries = stack_depot_fetch_into(count_handle, fetched,
					    ARRAY_SIZE(fetched));
	KUNIT_EXPECT_EQ(test, nr_entries, plain_nr);
	KUNIT_EXPECT_MEMEQ(test, fetched, plain_entries, sizeof(plain_entries));

	get_handle = stack_depot_save_flags(get_entries, get_nr, GFP_KERNEL,
					    STACK_DEPOT_FLAG_CAN_ALLOC |
					    STACK_DEPOT_FLAG_GET);
	KUNIT_ASSERT_NE(test, get_handle, (depot_stack_handle_t)0);
	count_handle = stack_depot_save_flags(get_entries, get_nr, GFP_KERNEL,
					      countable);
	KUNIT_ASSERT_NE(test, count_handle, (depot_stack_handle_t)0);
	record = __stack_depot_get_stack_record(count_handle);
	KUNIT_ASSERT_NOT_NULL(test, record);
	KUNIT_EXPECT_MEMEQ(test, record->entries, get_entries, sizeof(get_entries));

	stack_depot_put(get_handle);
}

static void stackdepot_fetch_into_roundtrip(struct kunit *test)
{
	unsigned long entries[] = {
		0x101000UL,
		0x102000UL,
		0x103000UL,
	};
	unsigned long exact[ARRAY_SIZE(entries)] = {};
	unsigned long fetched[ARRAY_SIZE(entries) + 1] = {
		[ARRAY_SIZE(entries)] = 0xa5a5a5a5UL,
	};
	unsigned long expected_tail = fetched[ARRAY_SIZE(entries)];
	depot_stack_handle_t handle;
	unsigned int nr_entries;

	KUNIT_ASSERT_EQ(test, stack_depot_init(), 0);

	handle = stack_depot_save(entries, ARRAY_SIZE(entries), GFP_KERNEL);
	KUNIT_ASSERT_NE(test, handle, (depot_stack_handle_t)0);

	nr_entries = stack_depot_fetch_into(handle, exact, ARRAY_SIZE(exact));
	KUNIT_EXPECT_EQ(test, nr_entries, (unsigned int)ARRAY_SIZE(entries));
	KUNIT_EXPECT_MEMEQ(test, exact, entries, sizeof(entries));

	nr_entries = stack_depot_fetch_into(handle, fetched, ARRAY_SIZE(fetched));
	KUNIT_EXPECT_EQ(test, nr_entries, (unsigned int)ARRAY_SIZE(entries));
	KUNIT_EXPECT_MEMEQ(test, fetched, entries, sizeof(entries));
	KUNIT_EXPECT_EQ(test, fetched[ARRAY_SIZE(entries)], expected_tail);
}

static void stackdepot_fetch_into_rejects_missing_or_short_stack(struct kunit *test)
{
	unsigned long entries[] = {
		0x111000UL,
		0x112000UL,
		0x113000UL,
	};
	unsigned long fetched[ARRAY_SIZE(entries)] = {
		0xa1a1a1a1UL,
		0xb2b2b2b2UL,
		0xc3c3c3c3UL,
	};
	unsigned long expected[ARRAY_SIZE(fetched)];
	depot_stack_handle_t handle;
	unsigned int nr_entries;

	KUNIT_ASSERT_EQ(test, stack_depot_init(), 0);

	handle = stack_depot_save(entries, ARRAY_SIZE(entries), GFP_KERNEL);
	KUNIT_ASSERT_NE(test, handle, (depot_stack_handle_t)0);
	memcpy(expected, fetched, sizeof(expected));

	nr_entries = stack_depot_fetch_into(0, fetched, ARRAY_SIZE(fetched));
	KUNIT_EXPECT_EQ(test, nr_entries, 0U);
	KUNIT_EXPECT_MEMEQ(test, fetched, expected, sizeof(expected));

	nr_entries = stack_depot_fetch_into(0, NULL, 0);
	KUNIT_EXPECT_EQ(test, nr_entries, 0U);

	nr_entries = stack_depot_fetch_into(handle, fetched,
					    ARRAY_SIZE(fetched) - 1);
	KUNIT_EXPECT_EQ(test, nr_entries, 0U);
	KUNIT_EXPECT_MEMEQ(test, fetched, expected, sizeof(expected));
}

static struct kunit_case stackdepot_test_cases[] = {
	KUNIT_CASE(stackdepot_countable_public),
	KUNIT_CASE(stackdepot_fetch_into_roundtrip),
	KUNIT_CASE(stackdepot_fetch_into_rejects_missing_or_short_stack),
	{}
};

static struct kunit_suite stackdepot_test_suite = {
	.name = "stackdepot",
	.test_cases = stackdepot_test_cases,
};

kunit_test_suite(stackdepot_test_suite);

MODULE_DESCRIPTION("KUnit tests for stack depot");
MODULE_AUTHOR("Caleb Kan <ckan@cloudflare.com>");
MODULE_LICENSE("GPL");
