// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/array_size.h>
#include <linux/gfp.h>
#include <linux/limits.h>
#include <linux/moduleparam.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/string.h>

#include <asm/stackdepot.h>

static int expected_trie_pool_limit = -1;
module_param_named(trie_pool_limit, expected_trie_pool_limit, int, 0);
MODULE_PARM_DESC(trie_pool_limit, "Expected stackdepot hash/trie pool split");

#ifdef CONFIG_ARM64
#include <asm/sections.h>

static inline unsigned long stackdepot_arm64_frame(long offset)
{
	return (unsigned long)((long)_text + offset);
}
#endif

static void stackdepot_trie_max_path_roundtrip(struct kunit *test)
{
	union handle_parts parts;
	unsigned long *entries;
	unsigned long *fetched;
	depot_stack_handle_t handle;
	size_t size = CONFIG_STACKDEPOT_MAX_FRAMES * sizeof(*entries);
	u32 pool_index_plus_1;
	unsigned int i;

	if (expected_trie_pool_limit < 0)
		kunit_skip(test, "trie pool limit was not provided");
	KUNIT_ASSERT_EQ(test, stack_depot_init(), 0);
	entries = kunit_kcalloc(test, CONFIG_STACKDEPOT_MAX_FRAMES,
				sizeof(*entries), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, entries);
	fetched = kunit_kcalloc(test, CONFIG_STACKDEPOT_MAX_FRAMES,
				sizeof(*fetched), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fetched);
	for (i = 0; i < CONFIG_STACKDEPOT_MAX_FRAMES; i++) {
#ifdef CONFIG_ARM64
		entries[i] = i & 1 ? 0x1000UL + i * 0x1000UL :
			stackdepot_arm64_frame(i * 4);
#elif defined(CONFIG_X86_64) && !defined(CONFIG_UML)
		entries[i] = i & 1 ? 0xffff888000000000UL + i * 0x1000UL :
			0xffffffff10000000UL + i * 0x10UL;
#else
		entries[i] = 0x1000UL + i * 0x1000UL;
#endif
	}

	handle = stack_depot_save(entries, CONFIG_STACKDEPOT_MAX_FRAMES,
				  GFP_KERNEL);
	KUNIT_ASSERT_NE(test, handle, (depot_stack_handle_t)0);
	parts.handle = handle;
	pool_index_plus_1 = parts.pool_index_plus_1;
	KUNIT_EXPECT_GT(test, pool_index_plus_1, (u32)expected_trie_pool_limit);
	KUNIT_EXPECT_EQ(test,
			stack_depot_fetch_into(handle, fetched,
					       CONFIG_STACKDEPOT_MAX_FRAMES),
			(unsigned int)CONFIG_STACKDEPOT_MAX_FRAMES);
	KUNIT_EXPECT_MEMEQ(test, fetched, entries, size);
	KUNIT_EXPECT_EQ(test,
			stack_depot_save(entries, CONFIG_STACKDEPOT_MAX_FRAMES,
					 GFP_KERNEL),
			handle);
}

static void stackdepot_save_flags_public(struct kunit *test)
{
	unsigned long entries[] = { 0x501000UL, 0x502000UL, 0x503000UL };
	unsigned long get_entries[] = { 0x601000UL, 0x602000UL };
	unsigned long missing_entries[] = { 0x701000UL, 0x702000UL };
	unsigned long fetched[ARRAY_SIZE(entries)] = {};
	depot_stack_handle_t noalloc_handle;
	depot_stack_handle_t overlong_handle;
	depot_stack_handle_t plain_handle;
	depot_stack_handle_t get_handle;
	depot_stack_handle_t again;
	depot_stack_handle_t extra;
	gfp_t no_spin = GFP_NOWAIT & ~__GFP_RECLAIM;
	unsigned long *overlong_fetched;
	unsigned long *overlong_entries;
	unsigned int overlong_nr = CONFIG_STACKDEPOT_MAX_FRAMES + 1;
	unsigned int nr_entries;
	size_t overlong_size;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, stack_depot_init(), 0);
	overlong_entries = kunit_kcalloc(test, overlong_nr,
					 sizeof(*overlong_entries), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, overlong_entries);
	overlong_fetched = kunit_kcalloc(test, CONFIG_STACKDEPOT_MAX_FRAMES,
					 sizeof(*overlong_fetched), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, overlong_fetched);
	for (i = 0; i < overlong_nr; i++)
		overlong_entries[i] = 0x800000UL + i * 0x1000UL;

	plain_handle = stack_depot_save(entries, ARRAY_SIZE(entries), GFP_KERNEL);
	KUNIT_ASSERT_NE(test, plain_handle, (depot_stack_handle_t)0);
	again = stack_depot_save(entries, ARRAY_SIZE(entries), GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, again, plain_handle);

	nr_entries = stack_depot_fetch_into(plain_handle, fetched,
					    ARRAY_SIZE(fetched));
	KUNIT_EXPECT_EQ(test, nr_entries, (unsigned int)ARRAY_SIZE(entries));
	KUNIT_EXPECT_MEMEQ(test, fetched, entries, sizeof(entries));

	noalloc_handle = stack_depot_save_flags(entries, ARRAY_SIZE(entries), no_spin, 0);
	KUNIT_EXPECT_EQ(test, noalloc_handle, plain_handle);
	if (expected_trie_pool_limit >= 0) {
		noalloc_handle =
			stack_depot_save_flags(missing_entries,
					       ARRAY_SIZE(missing_entries),
					       no_spin, 0);
		KUNIT_EXPECT_EQ(test, noalloc_handle, (depot_stack_handle_t)0);
	}

	get_handle = stack_depot_save_flags(get_entries, ARRAY_SIZE(get_entries),
					    GFP_KERNEL,
					    STACK_DEPOT_FLAG_CAN_ALLOC |
					    STACK_DEPOT_FLAG_GET);
	KUNIT_ASSERT_NE(test, get_handle, (depot_stack_handle_t)0);
	stack_depot_put(get_handle);

	overlong_handle = stack_depot_save(overlong_entries, overlong_nr,
					   GFP_KERNEL);
	KUNIT_ASSERT_NE(test, overlong_handle, (depot_stack_handle_t)0);
	nr_entries = stack_depot_fetch_into(overlong_handle, overlong_fetched,
					    CONFIG_STACKDEPOT_MAX_FRAMES);
	KUNIT_EXPECT_EQ(test, nr_entries, (unsigned int)CONFIG_STACKDEPOT_MAX_FRAMES);
	overlong_size = CONFIG_STACKDEPOT_MAX_FRAMES * sizeof(*overlong_entries);
	KUNIT_EXPECT_MEMEQ(test, overlong_fetched, overlong_entries, overlong_size);

	extra = stack_depot_set_extra_bits(plain_handle, 7);
	KUNIT_ASSERT_NE(test, extra, (depot_stack_handle_t)0);
	KUNIT_EXPECT_EQ(test, stack_depot_get_extra_bits(extra), 7U);
	memset(fetched, 0, sizeof(fetched));
	nr_entries = stack_depot_fetch_into(extra, fetched, ARRAY_SIZE(fetched));
	KUNIT_EXPECT_EQ(test, nr_entries, (unsigned int)ARRAY_SIZE(entries));
	KUNIT_EXPECT_MEMEQ(test, fetched, entries, sizeof(entries));
}

static void stackdepot_snprint_public(struct kunit *test)
{
	unsigned long entries[] = { 0x1000UL, 0x2000UL, 0x3000UL };
	char expected[256];
	char actual[256];
	depot_stack_handle_t handle;
	unsigned int expected_len;
	int actual_len;

	KUNIT_ASSERT_EQ(test, stack_depot_init(), 0);
	handle = stack_depot_save(entries, ARRAY_SIZE(entries), GFP_KERNEL);
	KUNIT_ASSERT_NE(test, handle, (depot_stack_handle_t)0);

	expected_len = stack_trace_snprint(expected, sizeof(expected), entries,
					   ARRAY_SIZE(entries), 2);
	actual_len = stack_depot_snprint(handle, actual, sizeof(actual), 2);
	KUNIT_EXPECT_EQ(test, actual_len, (int)expected_len);
	KUNIT_EXPECT_STREQ(test, actual, expected);
}

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

static void stackdepot_trie_topology_roundtrip(struct kunit *test)
{
	union handle_parts parts;
	unsigned long stacks[][3] = {
		{ 0x201000UL, 0x202000UL },
		{ 0x201000UL, 0x203000UL },
		{ 0x201000UL },
		{ 0x201000UL, 0x203000UL, 0x204000UL },
		{ 0x201000UL, 0x205000UL },
		{ 0x201000UL, 0x204000UL },
		{ 0x201000UL, 0x206000UL },
		{ 0x201000UL, 0x207000UL },
		{ 0x301000UL, 0x302000UL },
		{ 0x301000UL, 0x302000UL, 0x303000UL },
		{ 0x301000UL, 0x304000UL },
		{ 0x401000UL, 0x402000UL, 0x403000UL },
		{ 0x401000UL, 0x402000UL },
	};
	unsigned int nr_entries[] = { 2, 2, 1, 3, 2, 2, 2, 2, 2, 3, 2, 3, 2 };
	depot_stack_handle_t handles[ARRAY_SIZE(stacks)];
	unsigned long fetched[ARRAY_SIZE(stacks[0])];
	u32 pool_index_plus_1;
	unsigned int i;

	if (expected_trie_pool_limit < 0)
		kunit_skip(test, "trie pool limit was not provided");
	KUNIT_ASSERT_EQ(test, stack_depot_init(), 0);

	for (i = 0; i < ARRAY_SIZE(stacks); i++) {
		handles[i] = stack_depot_save(stacks[i], nr_entries[i], GFP_KERNEL);
		KUNIT_ASSERT_NE(test, handles[i], (depot_stack_handle_t)0);
	}
	parts.handle = handles[0];
	pool_index_plus_1 = parts.pool_index_plus_1;
	KUNIT_ASSERT_GT(test, pool_index_plus_1,
			(u32)expected_trie_pool_limit);

	for (i = 0; i < ARRAY_SIZE(stacks); i++) {
		memset(fetched, 0, sizeof(fetched));
		KUNIT_EXPECT_EQ(test,
				stack_depot_fetch_into(handles[i], fetched,
						       ARRAY_SIZE(fetched)),
				nr_entries[i]);
		KUNIT_EXPECT_MEMEQ(test, fetched, stacks[i],
				   nr_entries[i] * sizeof(fetched[0]));
		KUNIT_EXPECT_EQ(test,
				stack_depot_save(stacks[i], nr_entries[i], GFP_KERNEL),
				handles[i]);
	}
}

static void stackdepot_frame_storage_roundtrip(struct kunit *test)
{
	union handle_parts parts;
	unsigned long fetched[3] = {};
	depot_stack_handle_t handle;
	u32 pool_index_plus_1;
	unsigned int nr_entries;
#if defined(CONFIG_ARM64)
	unsigned long entries[] = {
		stackdepot_arm64_frame(S32_MIN),
		0x1000UL,
		stackdepot_arm64_frame(S32_MAX),
	};
#elif defined(CONFIG_X86_64)
	unsigned long entries[] = {
		0xffffffff10001000UL,
		0xffff888000001000UL,
		0xffffffff20002000UL,
	};
#else
	unsigned long entries[] = { 0x301000UL, 0x302000UL, 0x303000UL };
#endif

	if (expected_trie_pool_limit < 0)
		kunit_skip(test, "trie pool limit was not provided");
	KUNIT_ASSERT_EQ(test, stack_depot_init(), 0);
	handle = stack_depot_save(entries, ARRAY_SIZE(entries), GFP_KERNEL);
	KUNIT_ASSERT_NE(test, handle, (depot_stack_handle_t)0);
	parts.handle = handle;
	pool_index_plus_1 = parts.pool_index_plus_1;
	KUNIT_ASSERT_GT(test, pool_index_plus_1,
			(u32)expected_trie_pool_limit);

	nr_entries = stack_depot_fetch_into(handle, fetched, ARRAY_SIZE(fetched));
	KUNIT_EXPECT_EQ(test, nr_entries, (unsigned int)ARRAY_SIZE(entries));
	KUNIT_EXPECT_MEMEQ(test, fetched, entries, sizeof(entries));
}

static void stackdepot_frame_raw_fallback(struct kunit *test)
{
	unsigned long frame = 0x1000UL;
	bool compressed;
	u32 payload;

#ifdef CONFIG_ARM64
	frame = (unsigned long)_text + (unsigned long)S32_MAX + 1UL;
#endif

	compressed = arch_stack_depot_frame_try_compress(frame, &payload);
	KUNIT_EXPECT_FALSE(test, compressed);
}

#if defined(CONFIG_X86_64) && !defined(CONFIG_UML)
static void stackdepot_frame_x86_64(struct kunit *test)
{
	unsigned long direct_map = 0xffff888000001000UL;
	unsigned long frame = 0xffffffff81234567UL;
	unsigned long out;
	bool compressed;
	u32 low;

	compressed = arch_stack_depot_frame_try_compress(frame, &low);
	KUNIT_EXPECT_TRUE(test, compressed);
	KUNIT_EXPECT_EQ(test, low, (u32)0x81234567);
	arch_stack_depot_frame_decompress(low, &out);
	KUNIT_EXPECT_EQ(test, out, frame);

	compressed = arch_stack_depot_frame_try_compress(direct_map, &low);
	KUNIT_EXPECT_FALSE(test, compressed);
}
#endif /* CONFIG_X86_64 && !CONFIG_UML */

#ifdef CONFIG_ARM64
static void stackdepot_frame_arm64(struct kunit *test)
{
	long negative_offset = S32_MIN;
	long positive_offset = S32_MAX;
	long offset = 0x123456;
	unsigned long frame = stackdepot_arm64_frame(offset);
	unsigned long out;
	bool compressed;
	u32 payload;

	compressed = arch_stack_depot_frame_try_compress(frame, &payload);
	KUNIT_EXPECT_TRUE(test, compressed);
	KUNIT_EXPECT_EQ(test, payload, (u32)(s32)offset);
	arch_stack_depot_frame_decompress(payload, &out);
	KUNIT_EXPECT_EQ(test, out, frame);

	frame = stackdepot_arm64_frame(negative_offset);
	compressed = arch_stack_depot_frame_try_compress(frame, &payload);
	KUNIT_EXPECT_TRUE(test, compressed);
	KUNIT_EXPECT_EQ(test, payload, (u32)(s32)negative_offset);
	arch_stack_depot_frame_decompress(payload, &out);
	KUNIT_EXPECT_EQ(test, out, frame);

	frame = stackdepot_arm64_frame(positive_offset);
	compressed = arch_stack_depot_frame_try_compress(frame, &payload);
	KUNIT_EXPECT_TRUE(test, compressed);
	KUNIT_EXPECT_EQ(test, payload, (u32)(s32)positive_offset);
	arch_stack_depot_frame_decompress(payload, &out);
	KUNIT_EXPECT_EQ(test, out, frame);
}
#endif /* CONFIG_ARM64 */

static struct kunit_case stackdepot_test_cases[] = {
	KUNIT_CASE(stackdepot_trie_max_path_roundtrip),
	KUNIT_CASE(stackdepot_save_flags_public),
	KUNIT_CASE(stackdepot_snprint_public),
	KUNIT_CASE(stackdepot_countable_public),
	KUNIT_CASE(stackdepot_fetch_into_roundtrip),
	KUNIT_CASE(stackdepot_fetch_into_rejects_missing_or_short_stack),
	KUNIT_CASE(stackdepot_trie_topology_roundtrip),
	KUNIT_CASE(stackdepot_frame_storage_roundtrip),
	KUNIT_CASE(stackdepot_frame_raw_fallback),
#if defined(CONFIG_X86_64) && !defined(CONFIG_UML)
	KUNIT_CASE(stackdepot_frame_x86_64),
#endif
#ifdef CONFIG_ARM64
	KUNIT_CASE(stackdepot_frame_arm64),
#endif
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
