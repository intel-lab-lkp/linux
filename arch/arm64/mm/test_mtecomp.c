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
static unsigned long compress_decompress_helper(struct kunit *test)
{
	struct test_data *td = test->priv;
	unsigned long handle;

	handle = mte_compress(td->tags);
	KUNIT_EXPECT_EQ(test, handle & BIT_ULL(63), 0);
	KUNIT_EXPECT_TRUE(test, mte_decompress(handle, td->dtags));
	KUNIT_EXPECT_EQ(test, memcmp(td->tags, td->dtags, MTE_PAGE_TAG_STORAGE),
			0);
	return handle;
}

/* Test that a zero-filled array is compressed into inline storage. */
static void test_compress_zero(struct kunit *test)
{
	struct test_data *td = test->priv;
	unsigned long handle;

	memset(td->tags, 0, MTE_PAGE_TAG_STORAGE);
	handle = compress_decompress_helper(test);
	/* Tags are stored inline. */
	KUNIT_EXPECT_EQ(test, mte_storage_size(handle), 8);
	mte_release_handle(handle);
}

/*
 * Test that a very small number of tag ranges ends up compressed into 8 bytes.
 */
static void test_compress_simple(struct kunit *test)
{
	struct test_data *td = test->priv;
	unsigned long handle;

	memset(td->tags, 0, MTE_PAGE_TAG_STORAGE);
	td->tags[0] = 0xa0;
	td->tags[1] = 0x0a;

	handle = compress_decompress_helper(test);
	/* Tags are stored inline. */
	KUNIT_EXPECT_EQ(test, mte_storage_size(handle), 8);
	mte_release_handle(handle);
}

/*
 * Test that a buffer containing @nranges ranges compresses into @exp_size
 * bytes and decompresses into the original tag sequence.
 */
static void compress_range_helper(struct kunit *test, int nranges,
				  size_t exp_size)
{
	struct test_data *td = test->priv;
	unsigned long handle;

	gen_tag_range_helper(td->tags, nranges);
	handle = compress_decompress_helper(test);
	KUNIT_EXPECT_EQ(test, mte_storage_size(handle), exp_size);
	mte_release_handle(handle);
}

static size_t expected_size_from_ranges(unsigned int ranges)
{
#if defined CONFIG_ARM64_4K_PAGES
	unsigned int range_exp[4] = { 6, 11, 23, 46 };
#elif defined(CONFIG_ARM64_16K_PAGES)
	unsigned int range_exp[6] = { 5, 9, 19, 39, 78, 157 };
#elif defined(CONFIG_ARM64_64K_PAGES)
	unsigned int range_exp[8] = { 4, 8, 17, 34, 68, 136, 273, 546 };
#endif
	unsigned int i;
	size_t size = 8;

	for (i = 0; i < ARRAY_SIZE(range_exp); i++) {
		if (ranges <= range_exp[i])
			return size;
		size <<= 1;
	}
	return size;
}

/*
 * Test that every number of tag ranges is correctly compressed and
 * decompressed.
 */
static void test_compress_ranges(struct kunit *test)
{
	size_t exp_size;
	unsigned int i;

	for (i = 1; i <= MTE_GRANULES_PER_PAGE; i++) {
		exp_size = expected_size_from_ranges(i);
		compress_range_helper(test, i, exp_size);
	}
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
	KUNIT_CASE(test_compress_simple),
	KUNIT_CASE(test_compress_ranges),
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
