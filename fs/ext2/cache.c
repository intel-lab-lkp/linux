// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024 Oracle. All rights reserved.
 */

#include "ext2.h"
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/rhashtable.h>
#include <linux/mm.h>
#include <linux/types.h>

static const struct rhashtable_params buffer_cache_params = {
	.key_len     = sizeof(sector_t),
	.key_offset  = offsetof(struct ext2_buffer, b_block),
	.head_offset = offsetof(struct ext2_buffer, b_rhash),
	.automatic_shrinking = true,
};

static struct ext2_buffer *insert_buffer_cache(struct super_block *sb, struct ext2_buffer *new_buf)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct rhashtable *buffer_cache = &sbi->buffer_cache;
	struct ext2_buffer *old_buf;

	spin_lock(&sbi->buffer_cache_lock);
	old_buf = rhashtable_lookup_get_insert_fast(buffer_cache,
				&new_buf->b_rhash, buffer_cache_params);
	spin_unlock(&sbi->buffer_cache_lock);

	if (old_buf)
		return old_buf;

	return new_buf;
}

static void buf_write_end_io(struct bio *bio)
{
	struct ext2_buffer *buf = bio->bi_private;

	clear_bit(EXT2_BUF_DIRTY_BIT, &buf->b_flags);
	bio_put(bio);
}

static int submit_buffer_read(struct super_block *sb, struct ext2_buffer *buf)
{
	struct bio_vec bio_vec;
	struct bio bio;
	sector_t sector = buf->b_block * (sb->s_blocksize >> 9);

	bio_init(&bio, sb->s_bdev, &bio_vec, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = sector;

	__bio_add_page(&bio, buf->b_page, buf->b_size, 0);

	return submit_bio_wait(&bio);
}

static void submit_buffer_write(struct super_block *sb, struct ext2_buffer *buf)
{
	struct bio *bio;
	sector_t sector = buf->b_block * (sb->s_blocksize >> 9);

	bio = bio_alloc(sb->s_bdev, 1, REQ_OP_WRITE, GFP_KERNEL);

	bio->bi_iter.bi_sector = sector;
	bio->bi_end_io = buf_write_end_io;
	bio->bi_private = buf;

	__bio_add_page(bio, buf->b_page, buf->b_size, 0);

	submit_bio(bio);
}

int sync_buffers(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct rhashtable *buffer_cache = &sbi->buffer_cache;
	struct rhashtable_iter iter;
	struct ext2_buffer *buf;
	struct blk_plug plug;

	blk_start_plug(&plug);
	rhashtable_walk_enter(buffer_cache, &iter);
	rhashtable_walk_start(&iter);
	while ((buf = rhashtable_walk_next(&iter)) != NULL) {
		if (IS_ERR(buf))
			continue;
		if (test_bit(EXT2_BUF_DIRTY_BIT, &buf->b_flags)) {
			submit_buffer_write(sb, buf);
		}
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	blk_finish_plug(&plug);

	return 0;
}

static struct ext2_buffer *lookup_buffer_cache(struct super_block *sb, sector_t block)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct rhashtable *buffer_cache = &sbi->buffer_cache;
	struct ext2_buffer *found = NULL;

	found = rhashtable_lookup_fast(buffer_cache, &block, buffer_cache_params);

	return found;
}

static int init_buffer(struct super_block *sb, sector_t block, struct ext2_buffer **buf_ptr)
{
	struct ext2_buffer *buf;

	buf = kmalloc(sizeof(struct ext2_buffer), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->b_block = block;
	buf->b_size = sb->s_blocksize;
	buf->b_flags = 0;

	mutex_init(&buf->b_lock);
	refcount_set(&buf->b_refcount, 1);

	buf->b_page = alloc_page(GFP_KERNEL);
	if (!buf->b_page)
		return -ENOMEM;

	buf->b_data = page_address(buf->b_page);

	*buf_ptr = buf;

	return 0;
}

void put_buffer(struct ext2_buffer *buf)
{
	refcount_dec(&buf->b_refcount);
	mutex_unlock(&buf->b_lock);
}

struct ext2_buffer *get_buffer(struct super_block *sb, sector_t block, bool need_uptodate)
{
	int err;
	struct ext2_buffer *buf;
	struct ext2_buffer *new_buf;

	buf = lookup_buffer_cache(sb, block);

	if (!buf) {
		err = init_buffer(sb, block, &new_buf);
		if (err)
			return ERR_PTR(err);

		if (need_uptodate) {
			err = submit_buffer_read(sb, new_buf);
			if (err)
				return ERR_PTR(err);
		}

		buf = insert_buffer_cache(sb, new_buf);
		if (IS_ERR(buf))
			kfree(new_buf);
	}

	mutex_lock(&buf->b_lock);
	refcount_inc(&buf->b_refcount);

	return buf;
}

void buffer_set_dirty(struct ext2_buffer *buf)
{
    set_bit(EXT2_BUF_DIRTY_BIT, &buf->b_flags);
}

int init_buffer_cache(struct rhashtable *buffer_cache)
{
	return rhashtable_init(buffer_cache, &buffer_cache_params);
}

static void destroy_buffer(void *ptr, void *arg)
{
	struct ext2_buffer *buf = ptr;

	WARN_ON(test_bit(EXT2_BUF_DIRTY_BIT, &buf->b_flags));
	__free_page(buf->b_page);
	kfree(buf);
}

void destroy_buffer_cache(struct rhashtable *buffer_cache)
{
	rhashtable_free_and_destroy(buffer_cache, destroy_buffer, NULL);
}
