// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for fs/ubifs/key.h
 *
 * The tested helpers are self-contained, so they only need a zeroed fake
 * struct ubifs_info; the ones which call c->key_hash use the test hash
 * function installed by ubifs_test_info().
 */

#include <kunit/test.h>

#include "kunit_helpers.h"

/*
 * Check that @buf holds the little-endian on-flash form of the key word pair
 * (@v0, @v1).
 */
static void expect_le_key(struct kunit *test, const void *buf, u32 v0, u32 v1)
{
	__le32 expected[2];

	expected[0] = cpu_to_le32(v0);
	expected[1] = cpu_to_le32(v1);
	KUNIT_EXPECT_MEMEQ(test, buf, expected, sizeof(expected));
}

/* ------------------------------------------------------------------------- */
/* key_write() / key_read() / key_write_idx()                                 */
/* ------------------------------------------------------------------------- */

static void key_write_encodes_key_and_zeroes_tail(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key key = { .u32 = { 0x12345678, 0x89ABCDEF } };
	const u8 zero[UBIFS_MAX_KEY_LEN - 8] = { 0 };
	u8 buf[UBIFS_MAX_KEY_LEN];

	KUNIT_ASSERT_NOT_NULL(test, c);

	memset(buf, 0xaa, sizeof(buf));
	key_write(c, &key, buf);

	expect_le_key(test, buf, key.u32[0], key.u32[1]);
	KUNIT_EXPECT_MEMEQ(test, buf + 8, zero, sizeof(zero));
}

static void key_write_stays_within_sixteen_bytes(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key key = { .u32 = { 0xDEADBEEF, 0xCAFEBABE } };
	const u8 zero[8] = { 0 };
	u8 guard[8];
	u8 buf[32];

	KUNIT_ASSERT_NOT_NULL(test, c);

	memset(guard, 0xaa, sizeof(guard));
	memset(buf, 0xaa, sizeof(buf));
	key_write(c, &key, buf + 8);

	/* Nothing before the 16-byte key and nothing after it is written. */
	KUNIT_EXPECT_MEMEQ(test, buf, guard, sizeof(guard));
	KUNIT_EXPECT_MEMEQ(test, buf + 24, guard, sizeof(guard));
	expect_le_key(test, buf + 8, key.u32[0], key.u32[1]);
	KUNIT_EXPECT_MEMEQ(test, buf + 16, zero, sizeof(zero));
}

static void key_read_reads_only_first_eight_bytes(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	const u8 src[UBIFS_MAX_KEY_LEN] = {
		0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89,
		0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
	};
	union ubifs_key key;

	KUNIT_ASSERT_NOT_NULL(test, c);

	key_read(c, src, &key);
	KUNIT_EXPECT_EQ(test, key.u32[0], 0x12345678);
	KUNIT_EXPECT_EQ(test, key.u32[1], 0x89ABCDEF);
}

static void key_write_read_roundtrip(struct kunit *test)
{
	static const u32 keys[][2] = {
		{ 0x00000000, 0x00000000 },
		{ 0x00000001, 0x00000000 },
		{ 0xffffffff, 0xffffffff },
		{ 0x12345678, 0x9abcdef0 },
		{ 0xdeadbeaf, UBIFS_INVALID_KEY },
	};
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key in, out;
	u8 buf[UBIFS_MAX_KEY_LEN];
	int i;

	KUNIT_ASSERT_NOT_NULL(test, c);

	for (i = 0; i < ARRAY_SIZE(keys); i++) {
		in.u32[0] = keys[i][0];
		in.u32[1] = keys[i][1];
		key_write(c, &in, buf);
		key_read(c, buf, &out);
		KUNIT_EXPECT_EQ(test, out.u32[0], in.u32[0]);
		KUNIT_EXPECT_EQ(test, out.u32[1], in.u32[1]);
	}
}

static void key_write_idx_does_not_zero_tail(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key key = { .u32 = { 0x01020304, 0x05060708 } };
	const u8 zero[8] = { 0 };
	const u8 guard[8] = { 0xaa, 0xaa, 0xaa, 0xaa,
			      0xaa, 0xaa, 0xaa, 0xaa };
	u8 idx[UBIFS_MAX_KEY_LEN];
	u8 full[UBIFS_MAX_KEY_LEN];

	KUNIT_ASSERT_NOT_NULL(test, c);

	memset(idx, 0xaa, sizeof(idx));
	memset(full, 0xaa, sizeof(full));
	key_write_idx(c, &key, idx);
	key_write(c, &key, full);

	expect_le_key(test, idx, key.u32[0], key.u32[1]);
	KUNIT_EXPECT_MEMEQ(test, idx, full, 8);
	KUNIT_EXPECT_MEMEQ(test, idx + 8, guard, sizeof(guard));
	KUNIT_EXPECT_MEMEQ(test, full + 8, zero, sizeof(zero));
}

static void key_copy_copies_whole_key(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key from = { .u32 = { 0x11223344, 0x55667788 } };
	union ubifs_key to;

	KUNIT_ASSERT_NOT_NULL(test, c);

	to.u64[0] = 0;
	key_copy(c, &from, &to);
	KUNIT_EXPECT_EQ(test, to.u32[0], from.u32[0]);
	KUNIT_EXPECT_EQ(test, to.u32[1], from.u32[1]);
	KUNIT_EXPECT_EQ(test, keys_eq(c, &from, &to), 1);
}

/* ------------------------------------------------------------------------- */
/* keys_cmp() / keys_eq()                                                     */
/* ------------------------------------------------------------------------- */

static void keys_cmp_orders_by_inum_first(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key k1 = { .u32 = { 1, 0xffffffff } };
	union ubifs_key k2 = { .u32 = { 2, 0x00000000 } };

	KUNIT_ASSERT_NOT_NULL(test, c);

	KUNIT_EXPECT_EQ(test, keys_cmp(c, &k1, &k2), -1);
	KUNIT_EXPECT_EQ(test, keys_cmp(c, &k2, &k1), 1);
}

static void keys_cmp_orders_by_second_word(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key k1 = { .u32 = { 7, 0x00000000 } };
	union ubifs_key k2 = { .u32 = { 7, 0x00000001 } };

	KUNIT_ASSERT_NOT_NULL(test, c);

	KUNIT_EXPECT_EQ(test, keys_cmp(c, &k1, &k2), -1);
	KUNIT_EXPECT_EQ(test, keys_cmp(c, &k2, &k1), 1);
	KUNIT_EXPECT_EQ(test, keys_cmp(c, &k1, &k1), 0);
}

static void keys_cmp_and_keys_eq_agree(struct kunit *test)
{
	static const u32 pairs[][4] = {
		{ 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
		{ 0x00000001, 0x00000000, 0x00000001, 0x00000001 },
		{ 0x00000001, 0x00000001, 0x00000001, 0x00000000 },
		{ 0xffffffff, 0x00000000, 0x00000000, 0xffffffff },
		{ 0x00000005, 0x40000000, 0x00000005, 0x40000000 },
	};
	struct ubifs_info *c = ubifs_test_info(test);
	int i;

	KUNIT_ASSERT_NOT_NULL(test, c);

	for (i = 0; i < ARRAY_SIZE(pairs); i++) {
		union ubifs_key a = { .u32 = { pairs[i][0], pairs[i][1] } };
		union ubifs_key b = { .u32 = { pairs[i][2], pairs[i][3] } };
		int cmp = keys_cmp(c, &a, &b);

		KUNIT_EXPECT_EQ(test, cmp, -keys_cmp(c, &b, &a));
		KUNIT_EXPECT_EQ(test, keys_eq(c, &a, &b), cmp == 0);
	}
}

/* ------------------------------------------------------------------------- */
/* Key constructors                                                           */
/* ------------------------------------------------------------------------- */

static void ino_key_init_sets_type_and_inum(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	const ino_t inum = 0x12345678;
	union ubifs_key key;

	KUNIT_ASSERT_NOT_NULL(test, c);

	ino_key_init(c, &key, inum);
	KUNIT_EXPECT_EQ(test, key.u32[0], inum);
	KUNIT_EXPECT_EQ(test, key.u32[1], UBIFS_INO_KEY <<
			UBIFS_S_KEY_BLOCK_BITS);
	KUNIT_EXPECT_EQ(test, key_type(c, &key), UBIFS_INO_KEY);
	KUNIT_EXPECT_EQ(test, key_inum(c, &key), inum);
	KUNIT_EXPECT_EQ(test, key_block(c, &key), 0);
}

static void ino_key_init_flash_zeroes_tail_and_roundtrips(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	const u8 zero[UBIFS_MAX_KEY_LEN - 8] = { 0 };
	const ino_t inum = 0x12345678;
	union ubifs_key key, out;
	u8 flash[UBIFS_MAX_KEY_LEN];

	KUNIT_ASSERT_NOT_NULL(test, c);

	memset(flash, 0xaa, sizeof(flash));
	ino_key_init_flash(c, flash, inum);
	KUNIT_EXPECT_MEMEQ(test, flash + 8, zero, sizeof(zero));

	key_read(c, flash, &out);
	ino_key_init(c, &key, inum);
	KUNIT_EXPECT_EQ(test, keys_eq(c, &key, &out), 1);
}

static void dent_key_init_sets_type_and_hash(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct fscrypt_name nm = { .disk_name = FSTR_INIT("abc", 3) };
	const u32 hash = 0x00000005;
	union ubifs_key key;
	u32 expected;

	KUNIT_ASSERT_NOT_NULL(test, c);

	expected = c->key_hash(fname_name(&nm), fname_len(&nm));
	KUNIT_EXPECT_LE(test, expected, UBIFS_S_KEY_HASH_MASK);

	dent_key_init(c, &key, 0x1234, &nm);
	KUNIT_EXPECT_EQ(test, key.u32[0], 0x1234);
	KUNIT_EXPECT_EQ(test, key.u32[1], expected |
			(UBIFS_DENT_KEY << UBIFS_S_KEY_HASH_BITS));
	KUNIT_EXPECT_EQ(test, key_type(c, &key), UBIFS_DENT_KEY);
	KUNIT_EXPECT_EQ(test, key_hash(c, &key), expected);

	dent_key_init_hash(c, &key, 0x1234, hash);
	KUNIT_EXPECT_EQ(test, key.u32[1], 0x40000005);
	KUNIT_EXPECT_EQ(test, key_hash(c, &key), hash);
}

static void dent_key_init_flash_zeroes_tail_and_roundtrips(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct fscrypt_name nm = { .disk_name = FSTR_INIT("abc", 3) };
	const u8 zero[UBIFS_MAX_KEY_LEN - 8] = { 0 };
	union ubifs_key key, out;
	u8 flash[UBIFS_MAX_KEY_LEN];

	KUNIT_ASSERT_NOT_NULL(test, c);

	memset(flash, 0xaa, sizeof(flash));
	dent_key_init_flash(c, flash, 0x20, &nm);
	KUNIT_EXPECT_MEMEQ(test, flash + 8, zero, sizeof(zero));

	key_read(c, flash, &out);
	dent_key_init(c, &key, 0x20, &nm);
	KUNIT_EXPECT_EQ(test, keys_eq(c, &key, &out), 1);
	KUNIT_EXPECT_EQ(test, key_type(c, &out), UBIFS_DENT_KEY);
}

static void xent_key_init_sets_type_and_hash(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct fscrypt_name nm = { .disk_name = FSTR_INIT("abc", 3) };
	union ubifs_key key;
	u32 expected;

	KUNIT_ASSERT_NOT_NULL(test, c);

	expected = c->key_hash(fname_name(&nm), fname_len(&nm));
	xent_key_init(c, &key, 0x1234, &nm);
	KUNIT_EXPECT_EQ(test, key.u32[0], 0x1234);
	KUNIT_EXPECT_EQ(test, key.u32[1], expected |
			(UBIFS_XENT_KEY << UBIFS_S_KEY_HASH_BITS));
	KUNIT_EXPECT_EQ(test, key_type(c, &key), UBIFS_XENT_KEY);
	KUNIT_EXPECT_EQ(test, key_hash(c, &key), expected);
}

static void xent_key_init_flash_zeroes_tail_and_roundtrips(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct fscrypt_name nm = { .disk_name = FSTR_INIT("abc", 3) };
	const u8 zero[UBIFS_MAX_KEY_LEN - 8] = { 0 };
	union ubifs_key key, out;
	u8 flash[UBIFS_MAX_KEY_LEN];

	KUNIT_ASSERT_NOT_NULL(test, c);

	memset(flash, 0xaa, sizeof(flash));
	xent_key_init_flash(c, flash, 0x20, &nm);
	KUNIT_EXPECT_MEMEQ(test, flash + 8, zero, sizeof(zero));

	key_read(c, flash, &out);
	xent_key_init(c, &key, 0x20, &nm);
	KUNIT_EXPECT_EQ(test, keys_eq(c, &key, &out), 1);
	KUNIT_EXPECT_EQ(test, key_type(c, &out), UBIFS_XENT_KEY);
}

static void data_key_init_sets_type_and_block(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key key;

	KUNIT_ASSERT_NOT_NULL(test, c);

	data_key_init(c, &key, 0x77, 0x12345);
	KUNIT_EXPECT_EQ(test, key.u32[0], 0x77);
	KUNIT_EXPECT_EQ(test, key.u32[1], 0x12345 |
			(UBIFS_DATA_KEY << UBIFS_S_KEY_BLOCK_BITS));
	KUNIT_EXPECT_EQ(test, key_type(c, &key), UBIFS_DATA_KEY);
	KUNIT_EXPECT_EQ(test, key_block(c, &key), 0x12345);
}

static void trun_key_init_sets_truncation_type(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key key;

	KUNIT_ASSERT_NOT_NULL(test, c);

	trun_key_init(c, &key, 0x77);
	KUNIT_EXPECT_EQ(test, key.u32[0], 0x77);
	KUNIT_EXPECT_EQ(test, key.u32[1], UBIFS_TRUN_KEY <<
			UBIFS_S_KEY_BLOCK_BITS);
	KUNIT_EXPECT_EQ(test, key.u32[1], 0x80000000);
	KUNIT_EXPECT_EQ(test, key_type(c, &key), UBIFS_TRUN_KEY);
}

static void invalid_key_init_sets_invalid_marker(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key key;

	KUNIT_ASSERT_NOT_NULL(test, c);

	invalid_key_init(c, &key);
	KUNIT_EXPECT_EQ(test, key.u32[0], 0xDEADBEAF);
	KUNIT_EXPECT_EQ(test, key.u32[1], UBIFS_INVALID_KEY);
	KUNIT_EXPECT_EQ(test, key.u32[1], 0x00000004);
	/* UBIFS_INVALID_KEY is not shifted, unlike UBIFS_TRUN_KEY. */
	KUNIT_EXPECT_EQ(test, key_type(c, &key), UBIFS_INO_KEY);
}

static void lowest_and_highest_keys_are_bounds(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key lino, hino, ldent, lxent, hdata, dzero;

	KUNIT_ASSERT_NOT_NULL(test, c);

	lowest_ino_key(c, &lino, 1);
	highest_ino_key(c, &hino, 1);
	lowest_dent_key(c, &ldent, 1);
	lowest_xent_key(c, &lxent, 1);
	highest_data_key(c, &hdata, 1);
	data_key_init(c, &dzero, 1, 0);

	KUNIT_EXPECT_EQ(test, lino.u32[1], 0x00000000);
	KUNIT_EXPECT_EQ(test, hino.u32[1], 0xffffffff);
	KUNIT_EXPECT_EQ(test, ldent.u32[1], 0x40000000);
	KUNIT_EXPECT_EQ(test, lxent.u32[1], 0x60000000);
	KUNIT_EXPECT_EQ(test, hdata.u32[1], 0x3fffffff);

	KUNIT_EXPECT_EQ(test, keys_cmp(c, &lino, &dzero), -1);
	KUNIT_EXPECT_EQ(test, keys_cmp(c, &dzero, &hdata), -1);
	KUNIT_EXPECT_EQ(test, keys_cmp(c, &hdata, &ldent), -1);
	KUNIT_EXPECT_EQ(test, keys_cmp(c, &ldent, &lxent), -1);
}

/* ------------------------------------------------------------------------- */
/* Key accessors                                                              */
/* ------------------------------------------------------------------------- */

static void key_accessors_roundtrip(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key key;
	u8 flash[UBIFS_MAX_KEY_LEN];

	KUNIT_ASSERT_NOT_NULL(test, c);

	data_key_init(c, &key, 0x77, 0x12345);
	KUNIT_EXPECT_EQ(test, key_type(c, &key), UBIFS_DATA_KEY);
	KUNIT_EXPECT_EQ(test, key_inum(c, &key), 0x77);
	KUNIT_EXPECT_EQ(test, key_hash(c, &key), 0x12345);
	KUNIT_EXPECT_EQ(test, key_block(c, &key), 0x12345);

	key_write(c, &key, flash);
	KUNIT_EXPECT_EQ(test, key_type_flash(c, flash), UBIFS_DATA_KEY);
	KUNIT_EXPECT_EQ(test, key_inum_flash(c, flash), 0x77);
	KUNIT_EXPECT_EQ(test, key_hash_flash(c, flash), 0x12345);
	KUNIT_EXPECT_EQ(test, key_block_flash(c, flash), 0x12345);
}

static void highest_ino_key_type_is_out_of_range(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	union ubifs_key key;

	KUNIT_ASSERT_NOT_NULL(test, c);

	highest_ino_key(c, &key, 1);
	/* 7 is the sorting upper bound, not a valid key type. */
	KUNIT_EXPECT_EQ(test, key_type(c, &key), 7);
	KUNIT_EXPECT_GT(test, key_type(c, &key), UBIFS_KEY_TYPES_CNT - 1);
}

static void is_hash_key_identifies_hashed_keys(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct fscrypt_name nm = { .disk_name = FSTR_INIT("abc", 3) };
	union ubifs_key key;

	KUNIT_ASSERT_NOT_NULL(test, c);

	ino_key_init(c, &key, 1);
	KUNIT_EXPECT_EQ(test, is_hash_key(c, &key), 0);
	data_key_init(c, &key, 1, 0);
	KUNIT_EXPECT_EQ(test, is_hash_key(c, &key), 0);
	dent_key_init_hash(c, &key, 1, 3);
	KUNIT_EXPECT_EQ(test, is_hash_key(c, &key), 1);
	xent_key_init(c, &key, 1, &nm);
	KUNIT_EXPECT_EQ(test, is_hash_key(c, &key), 1);
	trun_key_init(c, &key, 1);
	KUNIT_EXPECT_EQ(test, is_hash_key(c, &key), 0);
}

/* ------------------------------------------------------------------------- */
/* Hash helpers                                                               */
/* ------------------------------------------------------------------------- */

static void key_mask_hash_maps_reserved_values(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, key_mask_hash(0), 3);
	KUNIT_EXPECT_EQ(test, key_mask_hash(1), 4);
	KUNIT_EXPECT_EQ(test, key_mask_hash(2), 5);
	KUNIT_EXPECT_EQ(test, key_mask_hash(3), 3);
	KUNIT_EXPECT_EQ(test, key_mask_hash(0x1fffffff), 0x1fffffff);
	KUNIT_EXPECT_EQ(test, key_mask_hash(0xffffffff), 0x1fffffff);
	KUNIT_EXPECT_EQ(test, key_mask_hash(0x20000002), 5);
}

static void key_r5_hash_invariants(struct kunit *test)
{
	static const char *const names[] = { "", "a", "abc", "file", "name" };
	int i;

	/* An empty name hashes to the first non-reserved value. */
	KUNIT_EXPECT_EQ(test, key_r5_hash("", 0), 3);

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		const char *name = names[i];
		int len = strlen(name);
		u32 h = key_r5_hash(name, len);

		KUNIT_EXPECT_GE(test, h, 3);
		KUNIT_EXPECT_LE(test, h, UBIFS_S_KEY_HASH_MASK);
		KUNIT_EXPECT_EQ(test, h, key_r5_hash(name, len));
	}
}

static void key_test_hash_invariants(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, key_test_hash("", 0), 3);
	KUNIT_EXPECT_GE(test, key_test_hash("abc", 3), 3);
	KUNIT_EXPECT_LE(test, key_test_hash("abc", 3),
			UBIFS_S_KEY_HASH_MASK);

	/* Only the first four bytes of the name are read. */
	KUNIT_EXPECT_EQ(test, key_test_hash("abcd", 100),
			key_test_hash("abcd", 4));
	KUNIT_EXPECT_EQ(test, key_test_hash("abcd", 100),
			key_test_hash("abcd", 5));
}

static void key_max_inode_size_depends_on_key_fmt(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);

	KUNIT_ASSERT_NOT_NULL(test, c);

	KUNIT_EXPECT_EQ(test, key_max_inode_size(c), 2199023255552ULL);
	KUNIT_EXPECT_EQ(test, key_max_inode_size(c),
			(1ULL << UBIFS_S_KEY_BLOCK_BITS) * UBIFS_BLOCK_SIZE);

	/* An unknown key format has no size limit to report. */
	c->key_fmt = 1;
	KUNIT_EXPECT_EQ(test, key_max_inode_size(c), 0);
}

/* ------------------------------------------------------------------------- */
/* Test suite registration                                                    */
/* ------------------------------------------------------------------------- */

static struct kunit_case ubifs_key_test_cases[] = {
	KUNIT_CASE(key_write_encodes_key_and_zeroes_tail),
	KUNIT_CASE(key_write_stays_within_sixteen_bytes),
	KUNIT_CASE(key_read_reads_only_first_eight_bytes),
	KUNIT_CASE(key_write_read_roundtrip),
	KUNIT_CASE(key_write_idx_does_not_zero_tail),
	KUNIT_CASE(key_copy_copies_whole_key),
	KUNIT_CASE(keys_cmp_orders_by_inum_first),
	KUNIT_CASE(keys_cmp_orders_by_second_word),
	KUNIT_CASE(keys_cmp_and_keys_eq_agree),
	KUNIT_CASE(ino_key_init_sets_type_and_inum),
	KUNIT_CASE(ino_key_init_flash_zeroes_tail_and_roundtrips),
	KUNIT_CASE(dent_key_init_sets_type_and_hash),
	KUNIT_CASE(dent_key_init_flash_zeroes_tail_and_roundtrips),
	KUNIT_CASE(xent_key_init_sets_type_and_hash),
	KUNIT_CASE(xent_key_init_flash_zeroes_tail_and_roundtrips),
	KUNIT_CASE(data_key_init_sets_type_and_block),
	KUNIT_CASE(trun_key_init_sets_truncation_type),
	KUNIT_CASE(invalid_key_init_sets_invalid_marker),
	KUNIT_CASE(lowest_and_highest_keys_are_bounds),
	KUNIT_CASE(key_accessors_roundtrip),
	KUNIT_CASE(highest_ino_key_type_is_out_of_range),
	KUNIT_CASE(is_hash_key_identifies_hashed_keys),
	KUNIT_CASE(key_mask_hash_maps_reserved_values),
	KUNIT_CASE(key_r5_hash_invariants),
	KUNIT_CASE(key_test_hash_invariants),
	KUNIT_CASE(key_max_inode_size_depends_on_key_fmt),
	{}
};

static struct kunit_suite ubifs_key_test_suite = {
	.name = "ubifs-key",
	.test_cases = ubifs_key_test_cases,
};

kunit_test_suite(ubifs_key_test_suite);
