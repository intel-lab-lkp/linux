// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024, Qualcomm Innovation Center, Inc. All rights reserved
 *
 * Based on work by Israel Rukshin file: dm-crypt.c
 *
 */

#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/crypto.h>
#include <linux/blk-crypto.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "inline-crypt"

struct inlinecrypt_config {
	struct dm_dev *dev;
	sector_t start;
	u64 iv_offset;
	unsigned int iv_size;
	unsigned short sector_size;
	unsigned char sector_shift;
	unsigned int key_size;
	enum blk_crypto_mode_num crypto_mode;
	struct blk_crypto_key *blk_key;
	u8 key[] __counted_by(key_size);
};

#define DM_CRYPT_DEFAULT_MAX_READ_SIZE		131072
#define DM_CRYPT_DEFAULT_MAX_WRITE_SIZE		131072

static unsigned int get_max_request_size(struct inlinecrypt_config *cc, bool wrt)
{
	unsigned int val, sector_align;

	val = !wrt ? DM_CRYPT_DEFAULT_MAX_READ_SIZE : DM_CRYPT_DEFAULT_MAX_WRITE_SIZE;
	if (wrt) {
		if (unlikely(val > BIO_MAX_VECS << PAGE_SHIFT))
			val = BIO_MAX_VECS << PAGE_SHIFT;
	}
	sector_align = max(bdev_logical_block_size(cc->dev->bdev), (unsigned int)cc->sector_size);
	val = round_down(val, sector_align);
	if (unlikely(!val))
		val = sector_align;
	return val >> SECTOR_SHIFT;
}

static int crypt_select_inline_crypt_mode(struct dm_target *ti, char *cipher,
					  char *ivmode)
{
	struct inlinecrypt_config *cc = ti->private;

	if (strcmp(cipher, "xts(aes128)") == 0) {
		cc->crypto_mode = BLK_ENCRYPTION_MODE_AES_128_XTS;
	} else if (strcmp(cipher, "xts(aes256)") == 0) {
		cc->crypto_mode = BLK_ENCRYPTION_MODE_AES_256_XTS;
	} else if (strcmp(cipher, "cbc(aes128)") == 0) {
		cc->crypto_mode = BLK_ENCRYPTION_MODE_AES_128_CBC;
	} else if (strcmp(cipher, "cbc(aes256)") == 0) {
		cc->crypto_mode = BLK_ENCRYPTION_MODE_AES_256_CBC;
	} else {
		ti->error = "Invalid cipher for inline_crypt";
		return -EINVAL;
	}

	cc->iv_size = 4;

	return 0;
}

static int crypt_prepare_inline_crypt_key(struct inlinecrypt_config *cc)
{
	int ret;

	cc->blk_key = kzalloc(sizeof(*cc->blk_key), GFP_KERNEL);
	if (!cc->blk_key)
		return -ENOMEM;

	ret = blk_crypto_init_key(cc->blk_key, cc->key, cc->crypto_mode,
				  cc->iv_size, cc->sector_size);
	if (ret) {
		DMERR("Failed to init inline encryption key");
		goto bad_key;
	}

	ret = blk_crypto_start_using_key(cc->dev->bdev, cc->blk_key);
	if (ret) {
		DMERR("Failed to use inline encryption key");
		goto bad_key;
	}

	return 0;
bad_key:
	kfree_sensitive(cc->blk_key);
	cc->blk_key = NULL;
	return ret;
}

static void crypt_destroy_inline_crypt_key(struct inlinecrypt_config *cc)
{
	if (cc->blk_key) {
		blk_crypto_evict_key(cc->dev->bdev, cc->blk_key);
		kfree_sensitive(cc->blk_key);
		cc->blk_key = NULL;
	}
}

static void crypt_inline_encrypt_submit(struct dm_target *ti, struct bio *bio)
{
	struct inlinecrypt_config *cc = ti->private;
	u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE];

	bio_set_dev(bio, cc->dev->bdev);
	if (bio_sectors(bio)) {
		memset(dun, 0, BLK_CRYPTO_MAX_IV_SIZE);
		bio->bi_iter.bi_sector = cc->start +
			dm_target_offset(ti, bio->bi_iter.bi_sector);
		dun[0] = le64_to_cpu(bio->bi_iter.bi_sector + cc->iv_offset);
		bio_crypt_set_ctx(bio, cc->blk_key, dun, GFP_KERNEL);
	}

	submit_bio_noacct(bio);
}

static int inlinecrypt_setkey(struct inlinecrypt_config *cc)
{
	crypt_destroy_inline_crypt_key(cc);

	return crypt_prepare_inline_crypt_key(cc);

	return 0;
}

static int inlinecrypt_set_key(struct inlinecrypt_config *cc, char *key)
{
	int r = -EINVAL;
	int key_string_len = strlen(key);

	/* Decode key from its hex representation. */
	if (cc->key_size && hex2bin(cc->key, key, cc->key_size) < 0)
		goto out;

	r = inlinecrypt_setkey(cc);
out:
	memset(key, '0', key_string_len);

	return r;
}

static void inlinecrypt_dtr(struct dm_target *ti)
{
	struct inlinecrypt_config *cc = ti->private;

	ti->private = NULL;

	if (!cc)
		return;

	crypt_destroy_inline_crypt_key(cc);

	if (cc->dev)
		dm_put_device(ti, cc->dev);

	kfree_sensitive(cc);
}

static int inlinecrypt_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct inlinecrypt_config *cc;
	char *cipher_api = NULL;
	char *cipher, *chainmode;
	unsigned long long tmpll;
	char *ivmode;
	int key_size;
	char dummy;
	int ret;

	if (argc < 5) {
		ti->error = "Not enough arguments";
		return -EINVAL;
	}

	key_size = strlen(argv[1]) >> 1;

	cc = kzalloc(struct_size(cc, key, key_size), GFP_KERNEL);
	if (!cc) {
		ti->error = "Cannot allocate encryption context";
		return -ENOMEM;
	}
	cc->key_size = key_size;
	cc->sector_size = (1 << SECTOR_SHIFT);
	cc->sector_shift = 0;

	ti->private = cc;

	if ((sscanf(argv[2], "%llu%c", &tmpll, &dummy) != 1) ||
	    (tmpll & ((cc->sector_size >> SECTOR_SHIFT) - 1))) {
		ti->error = "Invalid iv_offset sector";
		goto bad;
	}
	cc->iv_offset = tmpll;

	ret = dm_get_device(ti, argv[3], dm_table_get_mode(ti->table),
			    &cc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	ret = -EINVAL;
	if (sscanf(argv[4], "%llu%c", &tmpll, &dummy) != 1 ||
	    tmpll != (sector_t)tmpll) {
		ti->error = "Invalid device sector";
		goto bad;
	}

	cc->start = tmpll;

	cipher = strsep(&argv[0], "-");
	chainmode = strsep(&argv[0], "-");
	ivmode = strsep(&argv[0], "-");

	cipher_api = kmalloc(CRYPTO_MAX_ALG_NAME, GFP_KERNEL);
	if (!cipher_api)
		goto bad;

	ret = snprintf(cipher_api, CRYPTO_MAX_ALG_NAME,
		       "%s(%s)", chainmode, cipher);
	if (ret < 0 || ret >= CRYPTO_MAX_ALG_NAME) {
		kfree(cipher_api);
		ret = -ENOMEM;
		goto bad;
	}

	ret = crypt_select_inline_crypt_mode(ti, cipher_api, ivmode);

	/* Initialize and set key */
	ret = inlinecrypt_set_key(cc, argv[1]);
	if (ret < 0) {
		ti->error = "Error decoding and setting key";
		return ret;
	}

	return 0;
bad:
	ti->error = "Error in inlinecrypt mapping";
	inlinecrypt_dtr(ti);
	return ret;
}

static int inlinecrypt_map(struct dm_target *ti, struct bio *bio)
{
	struct inlinecrypt_config *cc = ti->private;
	unsigned int max_sectors;

	/*
	 * If bio is REQ_PREFLUSH or REQ_OP_DISCARD, just bypass crypt queues.
	 * - for REQ_PREFLUSH device-mapper core ensures that no IO is in-flight
	 * - for REQ_OP_DISCARD caller must use flush if IO ordering matters
	 */
	if (unlikely(bio->bi_opf & REQ_PREFLUSH ||
		     bio_op(bio) == REQ_OP_DISCARD)) {
		bio_set_dev(bio, cc->dev->bdev);
		if (bio_sectors(bio))
			bio->bi_iter.bi_sector = cc->start +
				dm_target_offset(ti, bio->bi_iter.bi_sector);
		return DM_MAPIO_REMAPPED;
	}

	/*
	 * Check if bio is too large, split as needed.
	 */
	max_sectors = get_max_request_size(cc, bio_data_dir(bio) == WRITE);
	if (unlikely(bio_sectors(bio) > max_sectors))
		dm_accept_partial_bio(bio, max_sectors);

	/*
	 * Ensure that bio is a multiple of internal sector eninlinecryption size
	 * and is aligned to this size as defined in IO hints.
	 */
	if (unlikely((bio->bi_iter.bi_sector & ((cc->sector_size >> SECTOR_SHIFT) - 1)) != 0))
		return DM_MAPIO_KILL;

	if (unlikely(bio->bi_iter.bi_size & (cc->sector_size - 1)))
		return DM_MAPIO_KILL;

	crypt_inline_encrypt_submit(ti, bio);
		return DM_MAPIO_SUBMITTED;

	return 0;
}

static int inlinecrypt_iterate_devices(struct dm_target *ti,
				       iterate_devices_callout_fn fn, void *data)
{
	struct inlinecrypt_config *cc = ti->private;

	return fn(ti, cc->dev, cc->start, ti->len, data);
}

static struct target_type inlinecrypt_target = {
	.name   = "inline-crypt",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr    = inlinecrypt_ctr,
	.dtr    = inlinecrypt_dtr,
	.map    = inlinecrypt_map,
	.iterate_devices = inlinecrypt_iterate_devices,
};
module_dm(inlinecrypt);

MODULE_AUTHOR("Md Sadre Alam <quic_mdalam@quicinc.com>");
MODULE_DESCRIPTION(DM_NAME " target for inline encryption / decryption");
MODULE_LICENSE("GPL");
