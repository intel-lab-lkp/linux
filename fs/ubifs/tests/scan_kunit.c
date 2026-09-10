// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for fs/ubifs/scan.c
 *
 * The tests build UBIFS nodes in memory and feed them to the scanner; no UBI
 * device is involved. They all pass quiet = 1 unless the logging behaviour is
 * the thing under test, so that ubifs_dump_node() cannot flood the kernel log.
 */

#include <kunit/test.h>

#include "kunit_helpers.h"

/*
 * LEB number and offset used by the scanner tests; both satisfy the
 * ubifs_assert() checks at the beginning of ubifs_check_node().
 */
#define TEST_LNUM	0
#define TEST_OFFS	0

/* Data nodes are the simplest variable length nodes. */
#define TEST_DATA_SZ	UBIFS_DATA_NODE_SZ

/*
 * Allocate a buffer of exactly @len bytes and fill it with a valid node of
 * type @type. The buffer is never smaller than the length recorded in the
 * common header, as ubifs_check_node() reads the CRC over @ch->len bytes.
 */
static void *make_node(struct kunit *test, u8 type, int len)
{
	return ubifs_test_make_node(test, len, type, len);
}

/*
 * Check that a node with a corrupted payload is reported as corrupt by the
 * scanner and rejected by ubifs_check_node() with the given error code.
 */
static void expect_corrupt(struct kunit *test, void *node, int len, int err)
{
	struct ubifs_info *c = ubifs_test_info(test);

	KUNIT_ASSERT_NOT_NULL(test, c);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, len, TEST_LNUM,
					       TEST_OFFS, 1, 1), err);
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node, len, TEST_LNUM,
						TEST_OFFS, 1),
			SCANNED_A_CORRUPT_NODE);
}

/* ------------------------------------------------------------------------- */
/* ubifs_scan_a_node(): the node path                                         */
/* ------------------------------------------------------------------------- */

static void scan_a_node_valid_data_node_returns_a_node(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1), 0);
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node, TEST_DATA_SZ,
						TEST_LNUM, TEST_OFFS, 1),
			SCANNED_A_NODE);
}

static void scan_a_node_crc_covers_byte_8(struct kunit *test)
{
	void *node;

	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	/* The first byte covered by the CRC is the first sqnum byte. */
	((u8 *)node)[8] ^= 0xff;
	expect_corrupt(test, node, TEST_DATA_SZ, -EUCLEAN);
}

static void scan_a_node_crc_covers_last_byte(struct kunit *test)
{
	void *node;

	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	((u8 *)node)[TEST_DATA_SZ - 1] ^= 0xff;
	expect_corrupt(test, node, TEST_DATA_SZ, -EUCLEAN);
}

static void scan_a_node_crc_field_not_covered_by_crc(struct kunit *test)
{
	struct ubifs_ch *ch;
	void *node;

	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	/* magic and crc themselves are outside the CRC coverage. */
	ch = node;
	ch->crc = cpu_to_le32(le32_to_cpu(ch->crc) ^ 1);
	expect_corrupt(test, node, TEST_DATA_SZ, -EUCLEAN);
}

static void scan_a_node_crc_covers_whole_payload(struct kunit *test)
{
	void *node;

	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	((u8 *)node)[UBIFS_CH_SZ] ^= 0xff;
	expect_corrupt(test, node, TEST_DATA_SZ, -EUCLEAN);
}

static void scan_a_node_data_len_below_min_returns_corrupt(struct kunit *test)
{
	void *node;

	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ - 1);
	KUNIT_ASSERT_NOT_NULL(test, node);

	expect_corrupt(test, node, TEST_DATA_SZ - 1, -EINVAL);
}

static void scan_a_node_data_len_at_min_returns_a_node(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, UBIFS_DATA_NODE_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node, UBIFS_DATA_NODE_SZ,
						TEST_LNUM, TEST_OFFS, 1),
			SCANNED_A_NODE);
}

static void scan_a_node_data_len_at_max_returns_a_node(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, UBIFS_MAX_DATA_NODE_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node,
						UBIFS_MAX_DATA_NODE_SZ,
						TEST_LNUM, TEST_OFFS, 1),
			SCANNED_A_NODE);
}

static void scan_a_node_data_len_above_max_returns_corrupt(struct kunit *test)
{
	void *node;

	node = make_node(test, UBIFS_DATA_NODE, UBIFS_MAX_DATA_NODE_SZ + 1);
	KUNIT_ASSERT_NOT_NULL(test, node);

	expect_corrupt(test, node, UBIFS_MAX_DATA_NODE_SZ + 1, -EINVAL);
}

static void scan_a_node_crossing_leb_end_returns_corrupt(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	const int offs = UBIFS_TEST_LEB_SIZE - 40;
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	/* offs + node_len == leb_size + 8, which is past the LEB end. */
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, offs, 1, 1),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node, TEST_DATA_SZ,
						TEST_LNUM, offs, 1),
			SCANNED_A_CORRUPT_NODE);
}

static void scan_a_node_fixed_len_below_size_returns_corrupt(struct kunit *test)
{
	const int len = UBIFS_PAD_NODE_SZ - 1;
	void *node;

	node = make_node(test, UBIFS_PAD_NODE, len);
	KUNIT_ASSERT_NOT_NULL(test, node);

	expect_corrupt(test, node, len, -EINVAL);
}

static void scan_a_node_fixed_len_above_size_returns_corrupt(struct kunit *test)
{
	const int len = UBIFS_PAD_NODE_SZ + 1;
	void *node;

	node = make_node(test, UBIFS_PAD_NODE, len);
	KUNIT_ASSERT_NOT_NULL(test, node);

	expect_corrupt(test, node, len, -EINVAL);
}

static void scan_a_node_bad_type_returns_corrupt(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_stats_info *stats;
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	stats = kunit_kzalloc(test, sizeof(*stats), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stats);
	c->stats = stats;

	node = make_node(test, UBIFS_NODE_TYPES_CNT, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, stats->node_errors, 1);

	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node, TEST_DATA_SZ,
						TEST_LNUM, TEST_OFFS, 1),
			SCANNED_A_CORRUPT_NODE);
	KUNIT_EXPECT_EQ(test, stats->node_errors, 2);
}

static void scan_a_node_valid_trun_node_returns_a_node(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_TRUN_NODE, UBIFS_TRUN_NODE_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	/* Any valid node which is not a padding node is SCANNED_A_NODE. */
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node, UBIFS_TRUN_NODE_SZ,
						TEST_LNUM, TEST_OFFS, 1),
			SCANNED_A_NODE);
}

/* ------------------------------------------------------------------------- */
/* ubifs_scan_a_node(): padding and empty space                               */
/* ------------------------------------------------------------------------- */

static void expect_padding(struct kunit *test, int count, int len,
			   int expected)
{
	struct ubifs_info *c = ubifs_test_info(test);
	u8 *buf;

	KUNIT_ASSERT_NOT_NULL(test, c);
	buf = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	memset(buf, UBIFS_PADDING_BYTE, count);
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, buf, len, TEST_LNUM,
						TEST_OFFS, 1),
			expected);
}

static void scan_a_node_padding_8_returns_8(struct kunit *test)
{
	expect_padding(test, 8, 64, 8);
}

static void scan_a_node_padding_16_returns_16(struct kunit *test)
{
	expect_padding(test, 16, 64, 16);
}

static void scan_a_node_padding_24_returns_24(struct kunit *test)
{
	expect_padding(test, 24, 64, 24);
}

static void scan_a_node_padding_short_count_returns_garbage(struct kunit *test)
{
	int i;

	for (i = 1; i <= 7; i++)
		expect_padding(test, i, 64, SCANNED_GARBAGE);
}

static void scan_a_node_padding_28_or_more_returns_garbage(struct kunit *test)
{
	expect_padding(test, 28, 64, SCANNED_GARBAGE);
	expect_padding(test, 32, 64, SCANNED_GARBAGE);
	expect_padding(test, 64, 64, SCANNED_GARBAGE);
}

static void scan_a_node_padding_limited_by_len(struct kunit *test)
{
	expect_padding(test, 32, 8, 8);
	expect_padding(test, 32, 16, 16);
	expect_padding(test, 32, 24, 24);
	expect_padding(test, 32, 28, SCANNED_GARBAGE);
	expect_padding(test, 32, 7, SCANNED_GARBAGE);
	expect_padding(test, 32, 4, SCANNED_GARBAGE);
}

static void
scan_a_node_bad_magic_without_padding_returns_garbage(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	u8 *buf;

	KUNIT_ASSERT_NOT_NULL(test, c);
	buf = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	/* A zeroed buffer has magic 0 and starts with a non-padding byte. */
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, buf, 64, TEST_LNUM,
						TEST_OFFS, 1),
			SCANNED_GARBAGE);

	/*
	 * The padding count starts at byte 0, so a bogus magic followed by
	 * padding bytes is still reported as garbage.
	 */
	*(__le32 *)buf = cpu_to_le32(0xDEADBEEF);
	memset(buf + 4, UBIFS_PADDING_BYTE, 8);
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, buf, 64, TEST_LNUM,
						TEST_OFFS, 1),
			SCANNED_GARBAGE);
}

static void scan_a_node_empty_space_returns_empty(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	u8 *buf;

	KUNIT_ASSERT_NOT_NULL(test, c);
	buf = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, 0xff, 64);

	/* Empty space is detected before the header length is checked. */
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, buf, 64, TEST_LNUM,
						TEST_OFFS, 1),
			SCANNED_EMPTY_SPACE);
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, buf, 4, TEST_LNUM,
						TEST_OFFS, 1),
			SCANNED_EMPTY_SPACE);
}

static void scan_a_node_short_header_returns_garbage(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	/* A valid magic but fewer than UBIFS_CH_SZ readable bytes. */
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node, UBIFS_CH_SZ - 1,
						TEST_LNUM, TEST_OFFS, 1),
			SCANNED_GARBAGE);
}

/* ------------------------------------------------------------------------- */
/* ubifs_scan_a_node(): padding nodes                                         */
/* ------------------------------------------------------------------------- */

static void expect_pad(struct kunit *test, int pad_len, int offs, int expected)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_pad_node *pad;

	KUNIT_ASSERT_NOT_NULL(test, c);
	pad = ubifs_test_make_pad_node(test, pad_len);
	KUNIT_ASSERT_NOT_NULL(test, pad);

	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, pad, UBIFS_PAD_NODE_SZ,
						TEST_LNUM, offs, 1),
			expected);
}

static void
scan_a_node_pad_len_beyond_leb_returns_bad_pad(struct kunit *test)
{
	expect_pad(test, UBIFS_TEST_LEB_SIZE, TEST_OFFS,
		   SCANNED_A_BAD_PAD_NODE);
}

static void
scan_a_node_pad_len_crossing_leb_end_returns_bad_pad(struct kunit *test)
{
	expect_pad(test, UBIFS_TEST_LEB_SIZE - UBIFS_PAD_NODE_SZ, 8,
		   SCANNED_A_BAD_PAD_NODE);
}

static void scan_a_node_pad_len_negative_returns_bad_pad(struct kunit *test)
{
	expect_pad(test, -1, TEST_OFFS, SCANNED_A_BAD_PAD_NODE);
}

static void
scan_a_node_pad_len_not_aligned_returns_bad_pad(struct kunit *test)
{
	static const int pad_lens[] = { 0, 1, 5, 7 };
	int i;

	for (i = 0; i < ARRAY_SIZE(pad_lens); i++)
		expect_pad(test, pad_lens[i], TEST_OFFS,
			   SCANNED_A_BAD_PAD_NODE);
}

static void scan_a_node_valid_pad_returns_skip_len(struct kunit *test)
{
	static const int pad_lens[] = { 4, 12, 20 };
	int i;

	for (i = 0; i < ARRAY_SIZE(pad_lens); i++)
		expect_pad(test, pad_lens[i], TEST_OFFS,
			   UBIFS_PAD_NODE_SZ + pad_lens[i]);
}

static void scan_a_node_pad_len_reaching_leb_end_is_ok(struct kunit *test)
{
	/* offs + node_len + pad_len == leb_size is still a valid pad node. */
	expect_pad(test, UBIFS_TEST_LEB_SIZE - UBIFS_PAD_NODE_SZ, TEST_OFFS,
		   UBIFS_TEST_LEB_SIZE);
}

static void
scan_a_node_corrupt_node_quiet_off_returns_corrupt(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_ch *ch;
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	ch = node;
	ch->crc = cpu_to_le32(le32_to_cpu(ch->crc) ^ 1);

	/* quiet = 0 only enables the error messages and ubifs_dump_node(). */
	KUNIT_EXPECT_EQ(test, ubifs_scan_a_node(c, node, TEST_DATA_SZ,
						TEST_LNUM, TEST_OFFS, 0),
			SCANNED_A_CORRUPT_NODE);
}

/* ------------------------------------------------------------------------- */
/* ubifs_check_node()                                                         */
/* ------------------------------------------------------------------------- */

static void check_node_valid_data_node_returns_zero(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1), 0);
}

static void check_node_does_not_validate_len_argument(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	/* The CRC is read according to ch->len, len is only used for dumps. */
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, UBIFS_CH_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1), 0);
}

static void check_node_bad_magic_returns_euclean(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_ch *ch;
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	ch = node;
	ch->magic = cpu_to_le32(0xDEADBEEF);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EUCLEAN);
}

static void check_node_bad_type_returns_einval(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_NODE_TYPES_CNT, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EINVAL);
}

static void check_node_data_len_below_min_returns_einval(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ - 1);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ - 1,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EINVAL);
}

static void check_node_data_len_at_min_returns_zero(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, UBIFS_DATA_NODE_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, UBIFS_DATA_NODE_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1), 0);
}

static void check_node_data_len_at_max_returns_zero(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, UBIFS_MAX_DATA_NODE_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, UBIFS_MAX_DATA_NODE_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1), 0);
}

static void check_node_data_len_above_max_returns_einval(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, UBIFS_MAX_DATA_NODE_SZ + 1);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node,
					       UBIFS_MAX_DATA_NODE_SZ + 1,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EINVAL);
}

static void
check_node_data_crc_skipped_with_no_chk_data_crc(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);
	((u8 *)node)[TEST_DATA_SZ - 1] ^= 0xff;

	c->no_chk_data_crc = 1;
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 0), 0);
}

static void check_node_data_crc_checked_while_mounting(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);
	((u8 *)node)[TEST_DATA_SZ - 1] ^= 0xff;

	c->no_chk_data_crc = 1;
	c->mounting = 1;
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 0),
			-EUCLEAN);
}

static void
check_node_data_crc_checked_while_remounting_rw(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);
	((u8 *)node)[TEST_DATA_SZ - 1] ^= 0xff;

	c->no_chk_data_crc = 1;
	c->remounting_rw = 1;
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 0),
			-EUCLEAN);
}

static void
check_node_data_crc_checked_with_must_chk_crc(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);
	((u8 *)node)[TEST_DATA_SZ - 1] ^= 0xff;

	c->no_chk_data_crc = 1;
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EUCLEAN);
}

static void check_node_non_data_crc_is_never_skipped(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_pad_node *pad;

	KUNIT_ASSERT_NOT_NULL(test, c);
	pad = ubifs_test_make_pad_node(test, 4);
	KUNIT_ASSERT_NOT_NULL(test, pad);
	pad->ch.crc = cpu_to_le32(le32_to_cpu(pad->ch.crc) ^ 1);

	c->no_chk_data_crc = 1;
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, pad, UBIFS_PAD_NODE_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 0),
			-EUCLEAN);
}

static void check_node_updates_stats_counters(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_stats_info *stats;
	struct ubifs_ch *ch;
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	stats = kunit_kzalloc(test, sizeof(*stats), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stats);
	c->stats = stats;

	/* A wrong magic number. */
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);
	ch = node;
	ch->magic = 0;
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EUCLEAN);
	KUNIT_EXPECT_EQ(test, stats->magic_errors, 1);

	/* An unknown node type. */
	node = make_node(test, UBIFS_NODE_TYPES_CNT, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, stats->node_errors, 1);

	/* A wrong CRC. */
	node = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);
	((u8 *)node)[TEST_DATA_SZ - 1] ^= 0xff;
	KUNIT_EXPECT_EQ(test, ubifs_check_node(c, node, TEST_DATA_SZ,
					       TEST_LNUM, TEST_OFFS, 1, 1),
			-EUCLEAN);
	KUNIT_EXPECT_EQ(test, stats->crc_errors, 1);
}

/* ------------------------------------------------------------------------- */
/* ubifs_add_snod()                                                           */
/* ------------------------------------------------------------------------- */

static void free_scanned_nodes(struct ubifs_scan_leb *sleb)
{
	struct ubifs_scan_node *node, *tmp;

	list_for_each_entry_safe(node, tmp, &sleb->nodes, list) {
		list_del(&node->list);
		kfree(node);
	}
}

static void expect_snod_keyed(struct kunit *test, u8 type, int len)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_scan_leb *sleb;
	struct ubifs_scan_node *snod;
	const u32 inum = 0x11223344;
	const u32 val = 0x55667788;
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	sleb = ubifs_test_scan_leb(test);
	KUNIT_ASSERT_NOT_NULL(test, sleb);
	node = ubifs_test_make_node(test, len, type, len);
	KUNIT_ASSERT_NOT_NULL(test, node);
	ubifs_test_node_set_key(node, inum, val);

	/*
	 * The node has to be linked before the list is dereferenced below;
	 * on -ENOMEM ubifs_add_snod() leaves sleb->nodes empty.
	 */
	KUNIT_ASSERT_EQ(test, ubifs_add_snod(c, sleb, node, 0x40), 0);
	KUNIT_EXPECT_EQ(test, sleb->nodes_cnt, 1);
	KUNIT_ASSERT_FALSE(test, list_empty(&sleb->nodes));

	snod = list_first_entry(&sleb->nodes, struct ubifs_scan_node, list);
	KUNIT_EXPECT_EQ(test, snod->type, type);
	KUNIT_EXPECT_EQ(test, snod->offs, 0x40);
	KUNIT_EXPECT_EQ(test, snod->len, len);
	KUNIT_EXPECT_EQ(test, snod->sqnum, UBIFS_TEST_SQNUM);
	KUNIT_EXPECT_EQ(test, snod->key.u32[0], inum);
	KUNIT_EXPECT_EQ(test, snod->key.u32[1], val);

	free_scanned_nodes(sleb);
}

static void add_snod_extracts_key_from_ino_node(struct kunit *test)
{
	expect_snod_keyed(test, UBIFS_INO_NODE, UBIFS_INO_NODE_SZ);
}

static void add_snod_extracts_key_from_dent_node(struct kunit *test)
{
	expect_snod_keyed(test, UBIFS_DENT_NODE, UBIFS_DENT_NODE_SZ);
}

static void add_snod_extracts_key_from_xent_node(struct kunit *test)
{
	expect_snod_keyed(test, UBIFS_XENT_NODE, UBIFS_XENT_NODE_SZ);
}

static void add_snod_extracts_key_from_data_node(struct kunit *test)
{
	expect_snod_keyed(test, UBIFS_DATA_NODE, UBIFS_DATA_NODE_SZ);
}

static void add_snod_pad_node_gets_invalid_key(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_scan_leb *sleb;
	struct ubifs_scan_node *snod;
	struct ubifs_pad_node *pad;

	KUNIT_ASSERT_NOT_NULL(test, c);
	sleb = ubifs_test_scan_leb(test);
	KUNIT_ASSERT_NOT_NULL(test, sleb);
	pad = ubifs_test_make_pad_node(test, 4);
	KUNIT_ASSERT_NOT_NULL(test, pad);

	KUNIT_ASSERT_EQ(test, ubifs_add_snod(c, sleb, pad, 0x40), 0);
	KUNIT_EXPECT_EQ(test, sleb->nodes_cnt, 1);

	snod = list_first_entry(&sleb->nodes, struct ubifs_scan_node, list);
	KUNIT_EXPECT_EQ(test, snod->type, UBIFS_PAD_NODE);
	KUNIT_EXPECT_EQ(test, snod->key.u32[0], 0xDEADBEAF);
	KUNIT_EXPECT_EQ(test, snod->key.u32[1], UBIFS_INVALID_KEY);

	free_scanned_nodes(sleb);
}

static void add_snod_trun_node_gets_invalid_key(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_scan_leb *sleb;
	struct ubifs_scan_node *snod;
	void *node;

	KUNIT_ASSERT_NOT_NULL(test, c);
	sleb = ubifs_test_scan_leb(test);
	KUNIT_ASSERT_NOT_NULL(test, sleb);
	node = make_node(test, UBIFS_TRUN_NODE, UBIFS_TRUN_NODE_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node);

	KUNIT_ASSERT_EQ(test, ubifs_add_snod(c, sleb, node, 0), 0);

	snod = list_first_entry(&sleb->nodes, struct ubifs_scan_node, list);
	KUNIT_EXPECT_EQ(test, snod->type, UBIFS_TRUN_NODE);
	KUNIT_EXPECT_EQ(test, snod->key.u32[0], 0xDEADBEAF);
	KUNIT_EXPECT_EQ(test, snod->key.u32[1], UBIFS_INVALID_KEY);

	free_scanned_nodes(sleb);
}

static void add_snod_appends_nodes_in_order(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_scan_leb *sleb;
	struct ubifs_scan_node *first, *second;
	void *node1, *node2;

	KUNIT_ASSERT_NOT_NULL(test, c);
	sleb = ubifs_test_scan_leb(test);
	KUNIT_ASSERT_NOT_NULL(test, sleb);
	node1 = make_node(test, UBIFS_DATA_NODE, TEST_DATA_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node1);
	node2 = make_node(test, UBIFS_INO_NODE, UBIFS_INO_NODE_SZ);
	KUNIT_ASSERT_NOT_NULL(test, node2);

	KUNIT_ASSERT_EQ(test, ubifs_add_snod(c, sleb, node1, 0), 0);
	KUNIT_ASSERT_EQ(test, ubifs_add_snod(c, sleb, node2, 64), 0);
	/*
	 * list_next_entry() below walks past the first entry, so both nodes
	 * must have been linked before the list is dereferenced.
	 */
	KUNIT_ASSERT_EQ(test, sleb->nodes_cnt, 2);

	first = list_first_entry(&sleb->nodes, struct ubifs_scan_node, list);
	second = list_next_entry(first, list);
	KUNIT_EXPECT_PTR_EQ(test, first->node, node1);
	KUNIT_EXPECT_PTR_EQ(test, second->node, node2);
	KUNIT_EXPECT_EQ(test, first->offs, 0);
	KUNIT_EXPECT_EQ(test, second->offs, 64);

	free_scanned_nodes(sleb);
}

/* ------------------------------------------------------------------------- */
/* ubifs_end_scan()                                                           */
/* ------------------------------------------------------------------------- */

static void end_scan_aligned_offset_sets_endpt(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_scan_leb *sleb;

	KUNIT_ASSERT_NOT_NULL(test, c);
	sleb = ubifs_test_scan_leb(test);
	KUNIT_ASSERT_NOT_NULL(test, sleb);

	ubifs_end_scan(c, sleb, TEST_LNUM, 16);
	KUNIT_EXPECT_EQ(test, sleb->endpt, 16);
}

static void end_scan_zero_offset_sets_endpt(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_scan_leb *sleb;

	KUNIT_ASSERT_NOT_NULL(test, c);
	sleb = ubifs_test_scan_leb(test);
	KUNIT_ASSERT_NOT_NULL(test, sleb);

	ubifs_end_scan(c, sleb, TEST_LNUM, 0);
	KUNIT_EXPECT_EQ(test, sleb->endpt, 0);
}

static void
end_scan_unaligned_offset_is_reported_but_aligns(struct kunit *test)
{
	struct ubifs_info *c = ubifs_test_info(test);
	struct ubifs_scan_leb *sleb;

	KUNIT_ASSERT_NOT_NULL(test, c);
	sleb = ubifs_test_scan_leb(test);
	KUNIT_ASSERT_NOT_NULL(test, sleb);

	/*
	 * This trips ubifs_assert(); with the zeroed assert_action the failure
	 * is only reported (and a stack trace dumped).
	 */
	ubifs_end_scan(c, sleb, TEST_LNUM, 12);
	KUNIT_EXPECT_EQ(test, sleb->endpt, 16);
}

/* ------------------------------------------------------------------------- */
/* Test suite registration                                                    */
/* ------------------------------------------------------------------------- */

static struct kunit_case ubifs_scan_test_cases[] = {
	KUNIT_CASE(scan_a_node_valid_data_node_returns_a_node),
	KUNIT_CASE(scan_a_node_crc_covers_byte_8),
	KUNIT_CASE(scan_a_node_crc_covers_last_byte),
	KUNIT_CASE(scan_a_node_crc_field_not_covered_by_crc),
	KUNIT_CASE(scan_a_node_crc_covers_whole_payload),
	KUNIT_CASE(scan_a_node_data_len_below_min_returns_corrupt),
	KUNIT_CASE(scan_a_node_data_len_at_min_returns_a_node),
	KUNIT_CASE(scan_a_node_data_len_at_max_returns_a_node),
	KUNIT_CASE(scan_a_node_data_len_above_max_returns_corrupt),
	KUNIT_CASE(scan_a_node_crossing_leb_end_returns_corrupt),
	KUNIT_CASE(scan_a_node_fixed_len_below_size_returns_corrupt),
	KUNIT_CASE(scan_a_node_fixed_len_above_size_returns_corrupt),
	KUNIT_CASE(scan_a_node_bad_type_returns_corrupt),
	KUNIT_CASE(scan_a_node_valid_trun_node_returns_a_node),
	KUNIT_CASE(scan_a_node_padding_8_returns_8),
	KUNIT_CASE(scan_a_node_padding_16_returns_16),
	KUNIT_CASE(scan_a_node_padding_24_returns_24),
	KUNIT_CASE(scan_a_node_padding_short_count_returns_garbage),
	KUNIT_CASE(scan_a_node_padding_28_or_more_returns_garbage),
	KUNIT_CASE(scan_a_node_padding_limited_by_len),
	KUNIT_CASE(scan_a_node_bad_magic_without_padding_returns_garbage),
	KUNIT_CASE(scan_a_node_empty_space_returns_empty),
	KUNIT_CASE(scan_a_node_short_header_returns_garbage),
	KUNIT_CASE(scan_a_node_pad_len_beyond_leb_returns_bad_pad),
	KUNIT_CASE(scan_a_node_pad_len_crossing_leb_end_returns_bad_pad),
	KUNIT_CASE(scan_a_node_pad_len_negative_returns_bad_pad),
	KUNIT_CASE(scan_a_node_pad_len_not_aligned_returns_bad_pad),
	KUNIT_CASE(scan_a_node_valid_pad_returns_skip_len),
	KUNIT_CASE(scan_a_node_pad_len_reaching_leb_end_is_ok),
	KUNIT_CASE(scan_a_node_corrupt_node_quiet_off_returns_corrupt),
	{}
};

static struct kunit_case ubifs_check_node_test_cases[] = {
	KUNIT_CASE(check_node_valid_data_node_returns_zero),
	KUNIT_CASE(check_node_does_not_validate_len_argument),
	KUNIT_CASE(check_node_bad_magic_returns_euclean),
	KUNIT_CASE(check_node_bad_type_returns_einval),
	KUNIT_CASE(check_node_data_len_below_min_returns_einval),
	KUNIT_CASE(check_node_data_len_at_min_returns_zero),
	KUNIT_CASE(check_node_data_len_at_max_returns_zero),
	KUNIT_CASE(check_node_data_len_above_max_returns_einval),
	KUNIT_CASE(check_node_data_crc_skipped_with_no_chk_data_crc),
	KUNIT_CASE(check_node_data_crc_checked_while_mounting),
	KUNIT_CASE(check_node_data_crc_checked_while_remounting_rw),
	KUNIT_CASE(check_node_data_crc_checked_with_must_chk_crc),
	KUNIT_CASE(check_node_non_data_crc_is_never_skipped),
	KUNIT_CASE(check_node_updates_stats_counters),
	{}
};

static struct kunit_case ubifs_add_snod_test_cases[] = {
	KUNIT_CASE(add_snod_extracts_key_from_ino_node),
	KUNIT_CASE(add_snod_extracts_key_from_dent_node),
	KUNIT_CASE(add_snod_extracts_key_from_xent_node),
	KUNIT_CASE(add_snod_extracts_key_from_data_node),
	KUNIT_CASE(add_snod_pad_node_gets_invalid_key),
	KUNIT_CASE(add_snod_trun_node_gets_invalid_key),
	KUNIT_CASE(add_snod_appends_nodes_in_order),
	{}
};

static struct kunit_case ubifs_end_scan_test_cases[] = {
	KUNIT_CASE(end_scan_aligned_offset_sets_endpt),
	KUNIT_CASE(end_scan_zero_offset_sets_endpt),
	KUNIT_CASE(end_scan_unaligned_offset_is_reported_but_aligns),
	{}
};

static struct kunit_suite ubifs_scan_test_suite = {
	.name = "ubifs-scan",
	.test_cases = ubifs_scan_test_cases,
};

static struct kunit_suite ubifs_check_node_test_suite = {
	.name = "ubifs-check-node",
	.test_cases = ubifs_check_node_test_cases,
};

static struct kunit_suite ubifs_add_snod_test_suite = {
	.name = "ubifs-add-snod",
	.test_cases = ubifs_add_snod_test_cases,
};

static struct kunit_suite ubifs_end_scan_test_suite = {
	.name = "ubifs-end-scan",
	.test_cases = ubifs_end_scan_test_cases,
};

kunit_test_suites(&ubifs_scan_test_suite, &ubifs_check_node_test_suite,
		  &ubifs_add_snod_test_suite, &ubifs_end_scan_test_suite);
