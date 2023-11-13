// SPDX-License-Identifier: GPL-2.0
/*
 * Test cases for MTE tags compression algorithm.
 */

#include <linux/bits.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <kunit/test.h>

#include <asm/mtecomp.h>

#include "mtecomp.h"

/* Per-test storage allocated in mtecomp_test_init(). */
struct test_data {
	u8 *tags, *dtags;
	unsigned short *r_sizes;
	size_t r_len;
	u8 *r_tags;
};

/*
 * Split td->tags to ranges stored in td->r_tags, td->r_sizes, td->r_len,
 * then convert those ranges back to tags stored in td->dtags.
 */
static void tags_to_ranges_to_tags_helper(struct kunit *test)
{
	struct test_data *td = test->priv;

	mte_tags_to_ranges(td->tags, td->r_tags, td->r_sizes, &td->r_len);
	mte_ranges_to_tags(td->r_tags, td->r_sizes, td->r_len, td->dtags);
	KUNIT_EXPECT_EQ(test, memcmp(td->tags, td->dtags, MTE_PAGE_TAG_STORAGE),
			0);
}

/*
 * Test that mte_tags_to_ranges() produces a single range for a zero-filled tag
 * buffer.
 */
static void test_tags_to_ranges_zero(struct kunit *test)
{
	struct test_data *td = test->priv;

	memset(td->tags, 0, MTE_PAGE_TAG_STORAGE);
	tags_to_ranges_to_tags_helper(test);

	KUNIT_EXPECT_EQ(test, td->r_len, 1);
	KUNIT_EXPECT_EQ(test, td->r_tags[0], 0);
	KUNIT_EXPECT_EQ(test, td->r_sizes[0], MTE_GRANULES_PER_PAGE);
}

/*
 * Test that a small number of different tags is correctly transformed into
 * ranges.
 */
static void test_tags_to_ranges_simple(struct kunit *test)
{
	struct test_data *td = test->priv;
	const u8 ex_tags[] = { 0xa, 0x0, 0xa, 0xb, 0x0 };
	const unsigned short ex_sizes[] = { 1, 2, 2, 1,
					    MTE_GRANULES_PER_PAGE - 6 };

	memset(td->tags, 0, MTE_PAGE_TAG_STORAGE);
	td->tags[0] = 0xa0;
	td->tags[1] = 0x0a;
	td->tags[2] = 0xab;
	tags_to_ranges_to_tags_helper(test);

	KUNIT_EXPECT_EQ(test, td->r_len, 5);
	KUNIT_EXPECT_EQ(test, memcmp(td->r_tags, ex_tags, sizeof(ex_tags)), 0);
	KUNIT_EXPECT_EQ(test, memcmp(td->r_sizes, ex_sizes, sizeof(ex_sizes)),
			0);
}

/* Test that repeated 0xa0 byte produces MTE_GRANULES_PER_PAGE ranges of length 1. */
static void test_tags_to_ranges_repeated(struct kunit *test)
{
	struct test_data *td = test->priv;

	memset(td->tags, 0xa0, MTE_PAGE_TAG_STORAGE);
	tags_to_ranges_to_tags_helper(test);

	KUNIT_EXPECT_EQ(test, td->r_len, MTE_GRANULES_PER_PAGE);
}

/* Generate a buffer that will contain @nranges of tag ranges. */
static void gen_tag_range_helper(u8 *tags, int nranges)
{
	unsigned int i;

	memset(tags, 0, MTE_PAGE_TAG_STORAGE);
	if (nranges > 1) {
		nranges--;
		for (i = 0; i < nranges / 2; i++)
			tags[i] = 0xab;
		if (nranges % 2)
			tags[nranges / 2] = 0xa0;
	}
}

/*
 * Test that mte_tags_to_ranges()/mte_ranges_to_tags() work for various
 * r_len values.
 */
static void test_tag_to_ranges_n(struct kunit *test)
{
	struct test_data *td = test->priv;
	unsigned int i, j, sum;

	for (i = 1; i <= MTE_GRANULES_PER_PAGE; i++) {
		gen_tag_range_helper(td->tags, i);
		tags_to_ranges_to_tags_helper(test);
		sum = 0;
		for (j = 0; j < td->r_len; j++)
			sum += td->r_sizes[j];
		KUNIT_EXPECT_EQ(test, sum, MTE_GRANULES_PER_PAGE);
	}
}

/*
 * Check that the tag buffer in test->priv can be compressed and decompressed
 * without changes.
 */
static void *compress_decompress_helper(struct kunit *test)
{
	struct test_data *td = test->priv;
	void *handle;

	handle = mte_compress(td->tags);
	KUNIT_EXPECT_EQ(test, (unsigned long)handle & BIT_ULL(63), 0);
	if (handle) {
		KUNIT_EXPECT_TRUE(test, mte_decompress(handle, td->dtags));
		KUNIT_EXPECT_EQ(test, memcmp(td->tags, td->dtags, MTE_PAGE_TAG_STORAGE),
				0);
	}
	return handle;
}

/* Test that a zero-filled array is compressed into inline storage. */
static void test_compress_zero(struct kunit *test)
{
	struct test_data *td = test->priv;
	void *handle;

	memset(td->tags, 0, MTE_PAGE_TAG_STORAGE);
	handle = compress_decompress_helper(test);
	/* Tags are stored inline. */
	KUNIT_EXPECT_TRUE(test, mte_is_compressed(handle));
}

/* Test that a 0xaa-filled array is compressed into inline storage. */
static void test_compress_nonzero(struct kunit *test)
{
	struct test_data *td = test->priv;
	void *handle;

	memset(td->tags, 0xaa, MTE_PAGE_TAG_STORAGE);
	handle = compress_decompress_helper(test);
	/* Tags are stored inline. */
	KUNIT_EXPECT_TRUE(test, mte_is_compressed(handle));
}

/*
 * Test that two tag ranges are compressed into inline storage.
 *
 * This also covers a special case where both ranges contain
 * `MTE_GRANULES_PER_PAGE / 2` tags and overflow the designated range size.
 */
static void test_two_ranges(struct kunit *test)
{
	struct test_data *td = test->priv;
	void *handle;
	unsigned int i;
	size_t r_len = 2;
	unsigned char r_tags[2] = { 0xe, 0x0 };
	unsigned short r_sizes[2];

	for (i = 1; i < MTE_GRANULES_PER_PAGE; i++) {
		r_sizes[0] = i;
		r_sizes[1] = MTE_GRANULES_PER_PAGE - i;
		mte_ranges_to_tags(r_tags, r_sizes, r_len, td->tags);
		handle = compress_decompress_helper(test);
		KUNIT_EXPECT_TRUE(test, mte_is_compressed(handle));
	}
}

/*
 * Test that a very small number of tag ranges ends up compressed into 8 bytes.
 */
static void test_compress_simple(struct kunit *test)
{
	struct test_data *td = test->priv;
	void *handle;

	memset(td->tags, 0, MTE_PAGE_TAG_STORAGE);
	td->tags[0] = 0xa0;
	td->tags[1] = 0x0a;

	handle = compress_decompress_helper(test);
	/* Tags are stored inline. */
	KUNIT_EXPECT_TRUE(test, mte_is_compressed(handle));
}

/*
 * Test that a buffer containing @nranges ranges compresses into @exp_size
 * bytes and decompresses into the original tag sequence.
 */
static void compress_range_helper(struct kunit *test, int nranges,
				  bool exp_inl)
{
	struct test_data *td = test->priv;
	void *handle;

	gen_tag_range_helper(td->tags, nranges);
	handle = compress_decompress_helper(test);
	KUNIT_EXPECT_EQ(test, mte_is_compressed(handle), exp_inl);
}

static inline size_t max_inline_ranges(void)
{
#if defined CONFIG_ARM64_4K_PAGES
	return 6;
#elif defined(CONFIG_ARM64_16K_PAGES)
	return 5;
#else
	return 4;
#endif
}

/*
 * Test that every number of tag ranges is correctly compressed and
 * decompressed.
 */
static void test_compress_ranges(struct kunit *test)
{
	unsigned int i;
	bool exp_inl;

	for (i = 1; i <= MTE_GRANULES_PER_PAGE; i++) {
		exp_inl = i <= max_inline_ranges();
		compress_range_helper(test, i, exp_inl);
	}
}

/*
 * Test that invalid handles are ignored by mte_decompress().
 */
static void test_decompress_invalid(struct kunit *test)
{
	void *handle1 = (void *)0xeb0b0b0100804020;
	void *handle2 = (void *)0x6b0b0b010080402f;
	struct test_data *td = test->priv;

	/* handle1 has bit 0 set to 1. */
	KUNIT_EXPECT_FALSE(test, mte_decompress(handle1, td->dtags));
	/*
	 * handle2 is an inline handle, but its largest_idx (bits 1..3)
	 * is out of bounds for the inline storage.
	 */
	KUNIT_EXPECT_FALSE(test, mte_decompress(handle2, td->dtags));
}

/*
 * Test that compressed inline tags cannot be confused with out-of-line
 * pointers.
 *
 * Compressed values are written from bit 0 to bit 63, so the size of the last
 * tag range initially ends up in the upper bits of the inline representation.
 * Make sure mte_compress() rearranges the bits so that the resulting handle does
 * not have 0b0111 as the upper four bits.
 */
static void test_upper_bits(struct kunit *test)
{
	struct test_data *td = test->priv;
	void *handle;
	unsigned char r_tags[6] = { 7, 0, 7, 0, 7, 0 };
	unsigned short r_sizes[6] = { 1, 1, 1, 1, 1, 1 };
	size_t r_len;

	/* Maximum number of ranges that can be encoded inline. */
	r_len = max_inline_ranges();
	/* Maximum range size possible, will be omitted. */
	r_sizes[0] = MTE_GRANULES_PER_PAGE / 2 - 1;
	/* A number close to r_sizes[0] that has most of its bits set. */
	r_sizes[r_len - 1] = MTE_GRANULES_PER_PAGE - r_sizes[0] - r_len + 2;

	mte_ranges_to_tags(r_tags, r_sizes, r_len, td->tags);
	handle = compress_decompress_helper(test);
	KUNIT_EXPECT_TRUE(test, mte_is_compressed(handle));
}

static void mtecomp_dealloc_testdata(struct test_data *td)
{
	kfree(td->tags);
	kfree(td->dtags);
	kfree(td->r_sizes);
	kfree(td->r_tags);
}

static int mtecomp_test_init(struct kunit *test)
{
	struct test_data *td;

	td = kmalloc(sizeof(struct test_data), GFP_KERNEL);
	if (!td)
		return 1;
	td->tags = kmalloc(MTE_PAGE_TAG_STORAGE, GFP_KERNEL);
	if (!td->tags)
		goto error;
	td->dtags = kmalloc(MTE_PAGE_TAG_STORAGE, GFP_KERNEL);
	if (!td->dtags)
		goto error;
	td->r_len = MTE_GRANULES_PER_PAGE;
	td->r_sizes = kmalloc_array(MTE_GRANULES_PER_PAGE,
				    sizeof(unsigned short), GFP_KERNEL);
	if (!td->r_sizes)
		goto error;
	td->r_tags = kmalloc(MTE_GRANULES_PER_PAGE, GFP_KERNEL);
	if (!td->r_tags)
		goto error;
	test->priv = (void *)td;
	return 0;
error:
	mtecomp_dealloc_testdata(td);
	return 1;
}

static void mtecomp_test_exit(struct kunit *test)
{
	struct test_data *td = test->priv;

	mtecomp_dealloc_testdata(td);
}

static struct kunit_case mtecomp_test_cases[] = {
	KUNIT_CASE(test_tags_to_ranges_zero),
	KUNIT_CASE(test_tags_to_ranges_simple),
	KUNIT_CASE(test_tags_to_ranges_repeated),
	KUNIT_CASE(test_tag_to_ranges_n),
	KUNIT_CASE(test_compress_zero),
	KUNIT_CASE(test_compress_nonzero),
	KUNIT_CASE(test_two_ranges),
	KUNIT_CASE(test_compress_simple),
	KUNIT_CASE(test_compress_ranges),
	KUNIT_CASE(test_decompress_invalid),
	KUNIT_CASE(test_upper_bits),
	{}
};

static struct kunit_suite mtecomp_test_suite = {
	.name = "mtecomp",
	.init = mtecomp_test_init,
	.exit = mtecomp_test_exit,
	.test_cases = mtecomp_test_cases,
};
kunit_test_suites(&mtecomp_test_suite);

MODULE_IMPORT_NS(MTECOMP);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexander Potapenko <glider@google.com>");
