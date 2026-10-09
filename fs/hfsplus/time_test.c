// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for HFS+ date/time conversion
 *
 * Copyright (C) 2026 Viacheslav Dubeyko <slava@dubeyko.com>
 */

#include <kunit/test.h>
#include "hfsplus_fs.h"
#include "xattr.h"

/* January 1, 1970, 00:00:00 UTC */
#define TEST_UNIX_EPOCH_SECS		0LL
/* December 31, 1969, 23:59:59 UTC */
#define TEST_BEFORE_UNIX_EPOCH_SECS	-1LL
/* January 1, 2000, 00:00:00 UTC */
#define TEST_Y2000_SECS			946684800LL
/* January 19, 2038, 03:14:08 UTC (signed 32-bit overflow) */
#define TEST_Y2038_SECS			2147483648LL
/* January 1, 2026, 00:00:00 UTC */
#define TEST_Y2026_SECS			1767225600LL
/* January 1, 2100, 00:00:00 UTC */
#define TEST_Y2100_SECS			4102444800LL
/* January 1, 1900, 00:00:00 UTC */
#define TEST_Y1900_SECS			-2208988800LL

static u32 mt_raw(__be32 mt)
{
	return be32_to_cpu(mt);
}

/* Test the time range constants */
static void hfsplus_time_constants_test(struct kunit *test)
{
	/* 32-bit unsigned on-disk field covers exactly U32_MAX seconds */
	KUNIT_EXPECT_EQ(test, (s64)U32_MAX,
			HFS_MAX_TIMESTAMP_SECS - HFS_MIN_TIMESTAMP_SECS);
	KUNIT_EXPECT_EQ(test, -(s64)HFS_UTC_OFFSET, HFS_MIN_TIMESTAMP_SECS);

	/* xattrs extend the supported range beyond February 2040 */
	KUNIT_EXPECT_GT(test, (s64)HFSPLUS_MAX_TIMESTAMP_SECS,
			(s64)HFS_MAX_TIMESTAMP_SECS);

	/* the extended timestamp mark is the maximal on-disk value */
	KUNIT_EXPECT_EQ(test, U32_MAX, mt_raw(HFSPLUS_EXT_TIMESTAMP_MARK));
}

/* Test conversion of on-disk (1904-based) timestamp into 1970-based one */
static void hfsplus_mt2ut_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, HFS_MIN_TIMESTAMP_SECS,
			__hfsp_mt2ut(cpu_to_be32(0)));
	KUNIT_EXPECT_EQ(test, TEST_UNIX_EPOCH_SECS,
			__hfsp_mt2ut(cpu_to_be32(HFS_UTC_OFFSET)));
	KUNIT_EXPECT_EQ(test, TEST_BEFORE_UNIX_EPOCH_SECS,
			__hfsp_mt2ut(cpu_to_be32(HFS_UTC_OFFSET - 1)));
	KUNIT_EXPECT_EQ(test, TEST_Y2000_SECS,
			__hfsp_mt2ut(cpu_to_be32(3029529600U)));
	KUNIT_EXPECT_EQ(test, TEST_Y2026_SECS,
			__hfsp_mt2ut(cpu_to_be32(3850070400U)));
	KUNIT_EXPECT_EQ(test, TEST_Y2038_SECS,
			__hfsp_mt2ut(cpu_to_be32(4230328448U)));
	KUNIT_EXPECT_EQ(test, HFS_MAX_TIMESTAMP_SECS,
			__hfsp_mt2ut(cpu_to_be32(U32_MAX)));
	KUNIT_EXPECT_EQ(test, HFS_MAX_TIMESTAMP_SECS,
			__hfsp_mt2ut(HFSPLUS_EXT_TIMESTAMP_MARK));
}

/* Test conversion of 1970-based timestamp into on-disk (1904-based) one */
static void hfsplus_ut2mt_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 0U,
			mt_raw(__hfsp_ut2mt(HFS_MIN_TIMESTAMP_SECS)));
	KUNIT_EXPECT_EQ(test, HFS_UTC_OFFSET,
			mt_raw(__hfsp_ut2mt(TEST_UNIX_EPOCH_SECS)));
	KUNIT_EXPECT_EQ(test, 3029529600U,
			mt_raw(__hfsp_ut2mt(TEST_Y2000_SECS)));
	KUNIT_EXPECT_EQ(test, 3850070400U,
			mt_raw(__hfsp_ut2mt(TEST_Y2026_SECS)));
	KUNIT_EXPECT_EQ(test, 4230328448U,
			mt_raw(__hfsp_ut2mt(TEST_Y2038_SECS)));
	KUNIT_EXPECT_EQ(test, U32_MAX - 1,
			mt_raw(__hfsp_ut2mt(HFS_MAX_TIMESTAMP_SECS - 1)));
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfsp_ut2mt(HFS_MAX_TIMESTAMP_SECS)));
}

/*
 * Test timestamps before January 1, 1970 (xfstests generic/258).
 * The old logic added HFS_UTC_OFFSET to lower 32 bits of negative
 * value and produced completely wrong result.
 */
static void hfsplus_ut2mt_before_1970_test(struct kunit *test)
{
	time64_t ut;

	KUNIT_EXPECT_EQ(test, HFS_UTC_OFFSET - 1,
			mt_raw(__hfsp_ut2mt(TEST_BEFORE_UNIX_EPOCH_SECS)));

	/* January 1, 1960, 00:00:00 UTC */
	ut = -315619200LL;
	KUNIT_EXPECT_EQ(test, ut, __hfsp_mt2ut(__hfsp_ut2mt(ut)));

	/* January 1, 1904, 00:00:01 UTC */
	ut = HFS_MIN_TIMESTAMP_SECS + 1;
	KUNIT_EXPECT_EQ(test, 1U, mt_raw(__hfsp_ut2mt(ut)));
	KUNIT_EXPECT_EQ(test, ut, __hfsp_mt2ut(__hfsp_ut2mt(ut)));
}

/* Test that out-of-range timestamps are clamped instead of wrapped */
static void hfsplus_ut2mt_clamp_test(struct kunit *test)
{
	/* below January 1, 1904 */
	KUNIT_EXPECT_EQ(test, 0U,
			mt_raw(__hfsp_ut2mt(HFS_MIN_TIMESTAMP_SECS - 1)));
	KUNIT_EXPECT_EQ(test, 0U,
			mt_raw(__hfsp_ut2mt(TEST_Y1900_SECS)));
	KUNIT_EXPECT_EQ(test, 0U,
			mt_raw(__hfsp_ut2mt(TIME64_MIN)));

	/* after February 6, 2040 */
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfsp_ut2mt(HFS_MAX_TIMESTAMP_SECS + 1)));
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfsp_ut2mt(TEST_Y2100_SECS)));
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfsp_ut2mt(HFSPLUS_MAX_TIMESTAMP_SECS)));

	/* lower 32 bits of these values would wrap into small dates */
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfsp_ut2mt(HFS_MAX_TIMESTAMP_SECS +
					    (s64)U32_MAX + 1)));
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfsp_ut2mt(1LL << 32)));
}

/* Test that representable timestamps survive the round trip */
static void hfsplus_time_roundtrip_test(struct kunit *test)
{
	static const time64_t values[] = {
		HFS_MIN_TIMESTAMP_SECS,
		HFS_MIN_TIMESTAMP_SECS + 1,
		TEST_BEFORE_UNIX_EPOCH_SECS,
		TEST_UNIX_EPOCH_SECS,
		1,
		TEST_Y2000_SECS,
		TEST_Y2026_SECS,
		(s64)S32_MAX,
		TEST_Y2038_SECS,
		HFS_MAX_TIMESTAMP_SECS - 1,
		HFS_MAX_TIMESTAMP_SECS,
	};
	time64_t ut;
	int i;

	for (i = 0; i < ARRAY_SIZE(values); i++) {
		KUNIT_EXPECT_EQ_MSG(test, values[i],
				    __hfsp_mt2ut(__hfsp_ut2mt(values[i])),
				    "round trip failed: index %d", i);
	}

	/* sweep the whole range with a large prime step */
	for (ut = HFS_MIN_TIMESTAMP_SECS; ut <= HFS_MAX_TIMESTAMP_SECS;
	     ut += 999983) {
		if (__hfsp_mt2ut(__hfsp_ut2mt(ut)) != ut) {
			KUNIT_FAIL(test, "round trip failed: ut %lld", ut);
			break;
		}
	}

	/* every on-disk value maps back to itself */
	for (i = 0; i < 32; i++) {
		u32 raw = (u32)BIT_ULL(i);

		KUNIT_EXPECT_EQ(test, raw,
				mt_raw(__hfsp_ut2mt(__hfsp_mt2ut(cpu_to_be32(raw)))));
	}
}

/* Test the decision to keep timestamp in xattr */
static void hfsplus_ut_needs_xattr_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, hfsp_ut_needs_xattr(HFS_MIN_TIMESTAMP_SECS));
	KUNIT_EXPECT_FALSE(test, hfsp_ut_needs_xattr(TEST_BEFORE_UNIX_EPOCH_SECS));
	KUNIT_EXPECT_FALSE(test, hfsp_ut_needs_xattr(TEST_UNIX_EPOCH_SECS));
	KUNIT_EXPECT_FALSE(test, hfsp_ut_needs_xattr(TEST_Y2026_SECS));
	KUNIT_EXPECT_FALSE(test, hfsp_ut_needs_xattr(TEST_Y2038_SECS));
	KUNIT_EXPECT_FALSE(test,
			   hfsp_ut_needs_xattr(HFS_MAX_TIMESTAMP_SECS - 1));

	KUNIT_EXPECT_TRUE(test, hfsp_ut_needs_xattr(HFS_MAX_TIMESTAMP_SECS));
	KUNIT_EXPECT_TRUE(test,
			  hfsp_ut_needs_xattr(HFS_MAX_TIMESTAMP_SECS + 1));
	KUNIT_EXPECT_TRUE(test, hfsp_ut_needs_xattr(TEST_Y2100_SECS));
	KUNIT_EXPECT_TRUE(test,
			  hfsp_ut_needs_xattr(HFSPLUS_MAX_TIMESTAMP_SECS));
}

/* Test detection of the extended timestamp mark */
static void hfsplus_mt_is_ext_timestamp_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		hfsp_mt_is_ext_timestamp(HFSPLUS_EXT_TIMESTAMP_MARK));
	KUNIT_EXPECT_TRUE(test,
		hfsp_mt_is_ext_timestamp(cpu_to_be32(U32_MAX)));

	KUNIT_EXPECT_FALSE(test, hfsp_mt_is_ext_timestamp(cpu_to_be32(0)));
	KUNIT_EXPECT_FALSE(test,
		hfsp_mt_is_ext_timestamp(cpu_to_be32(U32_MAX - 1)));
	KUNIT_EXPECT_FALSE(test,
		hfsp_mt_is_ext_timestamp(cpu_to_be32(HFS_UTC_OFFSET)));

	/* every timestamp that needs xattr is stored as the mark */
	KUNIT_EXPECT_TRUE(test,
		hfsp_mt_is_ext_timestamp(__hfsp_ut2mt(HFS_MAX_TIMESTAMP_SECS)));
	KUNIT_EXPECT_TRUE(test,
		hfsp_mt_is_ext_timestamp(__hfsp_ut2mt(TEST_Y2100_SECS)));
	KUNIT_EXPECT_FALSE(test,
		hfsp_mt_is_ext_timestamp(__hfsp_ut2mt(HFS_MAX_TIMESTAMP_SECS - 1)));
}

static struct super_block *alloc_mock_sb(struct kunit *test)
{
	struct super_block *sb;
	struct hfsplus_sb_info *sbi;

	sb = kunit_kzalloc(test, sizeof(*sb), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sb);

	sbi = kunit_kzalloc(test, sizeof(*sbi), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sbi);

	/* no attributes tree: xattr lookup returns -EOPNOTSUPP */
	sbi->attr_tree = NULL;
	sb->s_fs_info = sbi;

	return sb;
}

/*
 * Test reading of the on-disk timestamp that doesn't need xattr.
 * The superblock must not be touched in such case.
 */
static void hfsplus_read_timestamp_regular_test(struct kunit *test)
{
	static const time64_t values[] = {
		HFS_MIN_TIMESTAMP_SECS,
		TEST_BEFORE_UNIX_EPOCH_SECS,
		TEST_UNIX_EPOCH_SECS,
		TEST_Y2026_SECS,
		HFS_MAX_TIMESTAMP_SECS - 1,
	};
	time64_t ut;
	bool has_xattr;
	int err;
	int i;

	for (i = 0; i < ARRAY_SIZE(values); i++) {
		ut = 0;
		has_xattr = true;

		err = hfsplus_read_timestamp(NULL, HFSPLUS_FIRSTUSER_CNID,
					     XATTR_LINUX_ACCESS_DATE_NAME,
					     __hfsp_ut2mt(values[i]),
					     &ut, &has_xattr);
		KUNIT_EXPECT_EQ(test, 0, err);
		KUNIT_EXPECT_EQ(test, values[i], ut);
		KUNIT_EXPECT_FALSE(test, has_xattr);
	}
}

/*
 * Test reading of the extended timestamp mark without xattr
 * (for example, the record has been created by Mac OS X or
 * the volume has no attributes tree). The timestamp must be
 * HFS_MAX_TIMESTAMP_SECS without any error.
 */
static void hfsplus_read_timestamp_mark_no_xattr_test(struct kunit *test)
{
	struct super_block *sb = alloc_mock_sb(test);
	static const char * const names[] = {
		XATTR_LINUX_CREATE_DATE_NAME,
		XATTR_LINUX_CONTENT_MOD_DATE_NAME,
		XATTR_LINUX_ATTRIBUTE_MOD_DATE_NAME,
		XATTR_LINUX_ACCESS_DATE_NAME,
	};
	time64_t ut;
	bool has_xattr;
	int err;
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ut = 0;
		has_xattr = true;

		err = hfsplus_read_timestamp(sb, HFSPLUS_FIRSTUSER_CNID,
					     names[i],
					     HFSPLUS_EXT_TIMESTAMP_MARK,
					     &ut, &has_xattr);
		KUNIT_EXPECT_EQ(test, 0, err);
		KUNIT_EXPECT_EQ(test, HFS_MAX_TIMESTAMP_SECS, ut);
		KUNIT_EXPECT_FALSE(test, has_xattr);
	}
}

/* Test that names of internal xattrs fit the name buffer */
static void hfsplus_timestamp_xattr_names_test(struct kunit *test)
{
	static const char * const names[] = {
		XATTR_LINUX_CREATE_DATE_NAME,
		XATTR_LINUX_CONTENT_MOD_DATE_NAME,
		XATTR_LINUX_ATTRIBUTE_MOD_DATE_NAME,
		XATTR_LINUX_ACCESS_DATE_NAME,
		XATTR_LINUX_BACKUP_DATE_NAME,
	};
	int i;

	KUNIT_EXPECT_EQ(test, strlen(XATTR_LINUX_HFS_PREFIX),
			(size_t)XATTR_LINUX_HFS_PREFIX_LEN);

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		size_t len = XATTR_LINUX_HFS_PREFIX_LEN + strlen(names[i]);

		KUNIT_EXPECT_LT_MSG(test, len, (size_t)XATTR_LINUX_HFS_NAME_MAX,
				    "name %s is too long", names[i]);
		KUNIT_EXPECT_LE(test, len, (size_t)HFSPLUS_ATTR_MAX_STRLEN);
	}
}

static struct kunit_case hfsplus_time_test_cases[] = {
	KUNIT_CASE(hfsplus_time_constants_test),
	KUNIT_CASE(hfsplus_mt2ut_test),
	KUNIT_CASE(hfsplus_ut2mt_test),
	KUNIT_CASE(hfsplus_ut2mt_before_1970_test),
	KUNIT_CASE(hfsplus_ut2mt_clamp_test),
	KUNIT_CASE(hfsplus_time_roundtrip_test),
	KUNIT_CASE(hfsplus_ut_needs_xattr_test),
	KUNIT_CASE(hfsplus_mt_is_ext_timestamp_test),
	KUNIT_CASE(hfsplus_read_timestamp_regular_test),
	KUNIT_CASE(hfsplus_read_timestamp_mark_no_xattr_test),
	KUNIT_CASE(hfsplus_timestamp_xattr_names_test),
	{}
};

static struct kunit_suite hfsplus_time_test_suite = {
	.name = "hfsplus_time",
	.test_cases = hfsplus_time_test_cases,
};

kunit_test_suite(hfsplus_time_test_suite);

MODULE_DESCRIPTION("KUnit tests for HFS+ date/time conversion");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
