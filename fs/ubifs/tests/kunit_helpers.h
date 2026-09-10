/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared helpers for the UBIFS KUnit tests.
 *
 * The tests exercise the UBIFS scanning and key helpers against a minimal
 * fake &struct ubifs_info: no UBI device is involved and none of the tested
 * functions dereference any other field. The fake object is allocated with
 * kunit_kzalloc() so that KUnit frees it automatically after the test, and it
 * is fully zeroed because some of the fields read by the tested code (for
 * example assert_action) are bit-fields which must not contain garbage.
 */

#ifndef __UBIFS_TESTS_KUNIT_HELPERS_H__
#define __UBIFS_TESTS_KUNIT_HELPERS_H__

#include <kunit/test.h>
#include <linux/crc32.h>
#include <linux/fscrypt.h>

#include "../ubifs.h"

/* Typical NAND LEB size; large enough for any node. */
#define UBIFS_TEST_LEB_SIZE	131072
#define UBIFS_TEST_LEB_CNT	8
#define UBIFS_TEST_MIN_IO_SIZE	8

/* Arbitrary sequence number, distinct from the all-ones empty space. */
#define UBIFS_TEST_SQNUM	0x0123456789ABCDEFULL

/**
 * ubifs_test_crc() - compute the checksum covered by a UBIFS node CRC.
 * @buf: node buffer
 * @len: node length as stored in the common header
 *
 * This is exactly the calculation ubifs_check_node() performs: the checksum
 * covers everything but the magic and the CRC field itself. Note that the
 * kernel CRC is a plain crc32_le() without the final inversion, so zlib's
 * crc32() must never be used to compute expected values.
 *
 * Return: the checksum over bytes @buf[8 .. len - 1].
 */
static inline u32 ubifs_test_crc(const void *buf, int len)
{
	return crc32(UBIFS_CRC32_INIT, buf + 8, len - 8);
}

/**
 * ubifs_test_info() - allocate a fake UBIFS file-system object.
 * @test: KUnit test context
 *
 * The object is zeroed and only the fields read on the tested code paths are
 * set: the LEB geometry, the node length ranges exactly as
 * init_constants_early() in super.c sets them, and the key helpers used by
 * struct fscrypt_name based key constructors.
 *
 * Return: the fake object, or %NULL on allocation failure.
 */
static inline struct ubifs_info *ubifs_test_info(struct kunit *test)
{
	struct ubifs_info *c;

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	if (!c)
		return NULL;

	c->leb_size = UBIFS_TEST_LEB_SIZE;
	c->leb_cnt = UBIFS_TEST_LEB_CNT;
	c->min_io_size = UBIFS_TEST_MIN_IO_SIZE;
	c->key_fmt = UBIFS_SIMPLE_KEY_FMT;
	c->key_hash = key_test_hash;

	/*
	 * Mirror init_constants_early(). The zeroed object is *not* a safe
	 * default: max_len == 0 means "fixed length", so every type must be
	 * given an explicit range before ubifs_check_node() is called.
	 */
	c->ranges[UBIFS_PAD_NODE].len = UBIFS_PAD_NODE_SZ;
	c->ranges[UBIFS_SB_NODE].len = UBIFS_SB_NODE_SZ;
	c->ranges[UBIFS_MST_NODE].len = UBIFS_MST_NODE_SZ;
	c->ranges[UBIFS_REF_NODE].len = UBIFS_REF_NODE_SZ;
	c->ranges[UBIFS_TRUN_NODE].len = UBIFS_TRUN_NODE_SZ;
	c->ranges[UBIFS_CS_NODE].len = UBIFS_CS_NODE_SZ;
	c->ranges[UBIFS_AUTH_NODE].min_len = UBIFS_AUTH_NODE_SZ;
	c->ranges[UBIFS_AUTH_NODE].max_len = UBIFS_AUTH_NODE_SZ +
					     UBIFS_MAX_HMAC_LEN;
	c->ranges[UBIFS_SIG_NODE].min_len = UBIFS_SIG_NODE_SZ;
	c->ranges[UBIFS_SIG_NODE].max_len = c->leb_size - UBIFS_SB_NODE_SZ;
	c->ranges[UBIFS_INO_NODE].min_len = UBIFS_INO_NODE_SZ;
	c->ranges[UBIFS_INO_NODE].max_len = UBIFS_MAX_INO_NODE_SZ;
	c->ranges[UBIFS_ORPH_NODE].min_len = UBIFS_ORPH_NODE_SZ +
					     sizeof(__le64);
	c->ranges[UBIFS_ORPH_NODE].max_len = c->leb_size;
	c->ranges[UBIFS_DENT_NODE].min_len = UBIFS_DENT_NODE_SZ;
	c->ranges[UBIFS_DENT_NODE].max_len = UBIFS_MAX_DENT_NODE_SZ;
	c->ranges[UBIFS_XENT_NODE].min_len = UBIFS_XENT_NODE_SZ;
	c->ranges[UBIFS_XENT_NODE].max_len = UBIFS_MAX_XENT_NODE_SZ;
	c->ranges[UBIFS_DATA_NODE].min_len = UBIFS_DATA_NODE_SZ;
	c->ranges[UBIFS_DATA_NODE].max_len = UBIFS_MAX_DATA_NODE_SZ;
	c->ranges[UBIFS_IDX_NODE].min_len = UBIFS_IDX_NODE_SZ + UBIFS_BRANCH_SZ;
	c->ranges[UBIFS_IDX_NODE].max_len = INT_MAX;

	return c;
}

/**
 * ubifs_test_scan_leb() - allocate an initialized scanning object.
 * @test: KUnit test context
 *
 * Return: an empty &struct ubifs_scan_leb, or %NULL on allocation failure.
 */
static inline struct ubifs_scan_leb *ubifs_test_scan_leb(struct kunit *test)
{
	struct ubifs_scan_leb *sleb;

	sleb = kunit_kzalloc(test, sizeof(*sleb), GFP_KERNEL);
	if (!sleb)
		return NULL;

	INIT_LIST_HEAD(&sleb->nodes);
	return sleb;
}

/**
 * ubifs_test_node_init() - fill in the common header of a node.
 * @buf: node buffer
 * @type: node type
 * @len: full node length stored in the header
 *
 * The payload (including any key or pad_len field) has to be filled in by
 * the caller, and ubifs_test_node_sign() has to be called last.
 */
static inline void ubifs_test_node_init(void *buf, u8 type, int len)
{
	struct ubifs_ch *ch = buf;

	ch->magic = cpu_to_le32(UBIFS_NODE_MAGIC);
	ch->sqnum = cpu_to_le64(UBIFS_TEST_SQNUM);
	ch->len = cpu_to_le32(len);
	ch->node_type = type;
	ch->group_type = UBIFS_NO_NODE_GROUP;
	ch->padding[0] = 0;
	ch->padding[1] = 0;
}

/**
 * ubifs_test_node_sign() - compute and store the CRC of a node.
 * @buf: node buffer
 * @len: full node length stored in the header
 */
static inline void ubifs_test_node_sign(void *buf, int len)
{
	struct ubifs_ch *ch = buf;

	ch->crc = cpu_to_le32(ubifs_test_crc(buf, len));
}

/**
 * ubifs_test_make_node() - allocate a valid node with a zeroed payload.
 * @test: KUnit test context
 * @buf_size: size of the buffer to allocate, must be >= @len
 * @type: node type
 * @len: full node length stored in the header
 *
 * Return: the node, or %NULL if @buf_size < @len or on allocation failure.
 */
static inline void *ubifs_test_make_node(struct kunit *test, size_t buf_size,
					 u8 type, int len)
{
	void *buf;

	if (buf_size < (size_t)len)
		return NULL;

	buf = kunit_kzalloc(test, buf_size, GFP_KERNEL);
	if (!buf)
		return NULL;

	ubifs_test_node_init(buf, type, len);
	ubifs_test_node_sign(buf, len);

	return buf;
}

/**
 * ubifs_test_make_pad_node() - allocate a valid padding node.
 * @test: KUnit test context
 * @pad_len: value of the pad_len field (may be negative)
 *
 * Return: the padding node, or %NULL on allocation failure.
 */
static inline struct ubifs_pad_node *
ubifs_test_make_pad_node(struct kunit *test, int pad_len)
{
	struct ubifs_pad_node *pad;

	pad = kunit_kzalloc(test, sizeof(*pad), GFP_KERNEL);
	if (!pad)
		return NULL;

	ubifs_test_node_init(pad, UBIFS_PAD_NODE, UBIFS_PAD_NODE_SZ);
	pad->pad_len = cpu_to_le32((u32)pad_len);
	ubifs_test_node_sign(pad, UBIFS_PAD_NODE_SZ);

	return pad;
}

/**
 * ubifs_test_node_set_key() - store an on-flash key in a keyed node.
 * @buf: node buffer
 * @inum: value of the first key word (inode number)
 * @val: value of the second key word (key type and block/hash)
 */
static inline void ubifs_test_node_set_key(void *buf, u32 inum, u32 val)
{
	union ubifs_key *key = (union ubifs_key *)
			       ((char *)buf + UBIFS_KEY_OFFSET);

	key->j32[0] = cpu_to_le32(inum);
	key->j32[1] = cpu_to_le32(val);
}

#endif /* __UBIFS_TESTS_KUNIT_HELPERS_H__ */
