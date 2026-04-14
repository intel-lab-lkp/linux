// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — Block and inode allocator
 * Author: Aurélien DESBRIERES <aurelien@hackers.camp>
 *
 * Simple bitmap allocator. The free block bitmap is stored in-memory
 * (loaded at mount time) and persisted to disk on each allocation/free.
 *
 * Layout assumption (from mkfs.ftrfs):
 *   Block 0          : superblock
 *   Block 1..N       : inode table
 *   Block N+1        : root dir data
 *   Block N+2..end   : data blocks
 *
 * The bitmap itself is stored in the first data block after the inode
 * table. Each bit represents one data block (1 = free, 0 = used).
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bitmap.h>
#include <linux/slab.h>
#include "ftrfs.h"

/*
 * ftrfs_setup_bitmap — allocate and initialize the in-memory block bitmap
 * Called from ftrfs_fill_super() after the superblock is read.
 *
 * For the skeleton we use a simple in-memory bitmap initialized from
 * s_free_blocks. A full implementation would read the on-disk bitmap block.
 */
int ftrfs_setup_bitmap(struct super_block *sb)
{
	struct ftrfs_sb_info *sbi = FTRFS_SB(sb);
	unsigned long total_blocks;
	unsigned long data_start;

	total_blocks = le64_to_cpu(sbi->s_ftrfs_sb->s_block_count);
	data_start   = le64_to_cpu(sbi->s_ftrfs_sb->s_data_start_blk);

	if (total_blocks <= data_start) {
		pr_err("ftrfs: invalid block layout (total=%lu data_start=%lu)\n",
		       total_blocks, data_start);
		return -EINVAL;
	}

	sbi->s_nblocks     = total_blocks - data_start;
	sbi->s_data_start  = data_start;

	/* Allocate bitmap: one bit per data block */
	sbi->s_block_bitmap = bitmap_zalloc(sbi->s_nblocks, GFP_KERNEL);
	if (!sbi->s_block_bitmap)
		return -ENOMEM;

	/*
	 * Mark all blocks as free initially.
	 * A full implementation would read the on-disk bitmap here.
	 * For now we derive free blocks from s_free_blocks in the superblock.
	 */
	bitmap_fill(sbi->s_block_bitmap, sbi->s_nblocks);

	/*
	 * Mark blocks already used (total - free) as allocated.
	 * We mark from block 0 of the data area upward.
	 */
	{
		unsigned long used = sbi->s_nblocks - sbi->s_free_blocks;
		unsigned long i;

		for (i = 0; i < used && i < sbi->s_nblocks; i++)
			clear_bit(i, sbi->s_block_bitmap);
	}

	pr_info("ftrfs: bitmap initialized (%lu data blocks, %lu free)\n",
		sbi->s_nblocks, sbi->s_free_blocks);

	return 0;
}

/*
 * ftrfs_destroy_bitmap — free the in-memory bitmap
 * Called from ftrfs_put_super().
 */
void ftrfs_destroy_bitmap(struct super_block *sb)
{
	struct ftrfs_sb_info *sbi = FTRFS_SB(sb);

	if (sbi->s_block_bitmap) {
		bitmap_free(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
	}
}

/*
 * ftrfs_alloc_block — allocate a free data block
 * @sb:  superblock
 *
 * Returns the absolute block number (>= s_data_start) on success,
 * or 0 on failure (0 is the superblock, never a valid data block).
 *
 * Caller must hold sbi->s_lock.
 */
u64 ftrfs_alloc_block(struct super_block *sb)
{
	struct ftrfs_sb_info *sbi = FTRFS_SB(sb);
	unsigned long bit;

	if (!sbi->s_block_bitmap) {
		pr_err("ftrfs: bitmap not initialized\n");
		return 0;
	}

	spin_lock(&sbi->s_lock);

	if (sbi->s_free_blocks == 0) {
		spin_unlock(&sbi->s_lock);
		pr_warn("ftrfs: no free blocks\n");
		return 0;
	}

	/* Find first free bit (set = free in our convention) */
	bit = find_first_bit(sbi->s_block_bitmap, sbi->s_nblocks);
	if (bit >= sbi->s_nblocks) {
		spin_unlock(&sbi->s_lock);
		pr_err("ftrfs: bitmap inconsistency (free_blocks=%lu but no free bit)\n",
		       sbi->s_free_blocks);
		return 0;
	}

	/* Mark as used */
	clear_bit(bit, sbi->s_block_bitmap);
	sbi->s_free_blocks--;

	/* Update on-disk superblock counter */
	sbi->s_ftrfs_sb->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	mark_buffer_dirty(sbi->s_sbh);

	spin_unlock(&sbi->s_lock);

	/* Return absolute block number */
	return (u64)(sbi->s_data_start + bit);
}

/*
 * ftrfs_free_block — release a data block back to the free pool
 * @sb:    superblock
 * @block: absolute block number to free
 */
void ftrfs_free_block(struct super_block *sb, u64 block)
{
	struct ftrfs_sb_info *sbi = FTRFS_SB(sb);
	unsigned long bit;

	if (block < sbi->s_data_start) {
		pr_err("ftrfs: attempt to free non-data block %llu\n", block);
		return;
	}

	bit = (unsigned long)(block - sbi->s_data_start);

	if (bit >= sbi->s_nblocks) {
		pr_err("ftrfs: block %llu out of range\n", block);
		return;
	}

	spin_lock(&sbi->s_lock);

	if (test_bit(bit, sbi->s_block_bitmap)) {
		pr_warn("ftrfs: double free of block %llu\n", block);
		spin_unlock(&sbi->s_lock);
		return;
	}

	set_bit(bit, sbi->s_block_bitmap);
	sbi->s_free_blocks++;

	/* Update on-disk superblock counter */
	sbi->s_ftrfs_sb->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	mark_buffer_dirty(sbi->s_sbh);

	spin_unlock(&sbi->s_lock);
}

/*
 * ftrfs_alloc_inode_num — allocate a free inode number
 * @sb: superblock
 *
 * Returns inode number >= 2 on success (1 = root, reserved),
 * or 0 on failure.
 *
 * Simple linear scan of the inode table for a free slot.
 * A full implementation uses an inode bitmap block.
 */
u64 ftrfs_alloc_inode_num(struct super_block *sb)
{
	struct ftrfs_sb_info    *sbi = FTRFS_SB(sb);
	struct ftrfs_inode      *raw;
	struct buffer_head      *bh;
	unsigned long            inodes_per_block;
	unsigned long            inode_table_blk;
	unsigned long            total_inodes;
	unsigned long            block, i;
	u64                      ino = 0;

	inodes_per_block = FTRFS_BLOCK_SIZE / sizeof(struct ftrfs_inode);
	inode_table_blk  = le64_to_cpu(sbi->s_ftrfs_sb->s_inode_table_blk);
	total_inodes     = le64_to_cpu(sbi->s_ftrfs_sb->s_inode_count);

	spin_lock(&sbi->s_lock);

	if (sbi->s_free_inodes == 0) {
		spin_unlock(&sbi->s_lock);
		return 0;
	}

	/* Scan inode table blocks looking for a free inode (i_mode == 0) */
	for (block = 0; block * inodes_per_block < total_inodes; block++) {
		bh = sb_bread(sb, inode_table_blk + block);
		if (!bh)
			continue;

		raw = (struct ftrfs_inode *)bh->b_data;

		for (i = 0; i < inodes_per_block; i++) {
			unsigned long ino_num = block * inodes_per_block + i + 1;

			if (ino_num > total_inodes)
				break;

			/* inode 1 = root, always reserved */
			if (ino_num == 1)
				continue;

			if (le16_to_cpu(raw[i].i_mode) == 0) {
				/* Found a free inode slot */
				ino = (u64)ino_num;
				sbi->s_free_inodes--;
				sbi->s_ftrfs_sb->s_free_inodes =
					cpu_to_le64(sbi->s_free_inodes);
				mark_buffer_dirty(sbi->s_sbh);
				brelse(bh);
				goto found;
			}
		}
		brelse(bh);
	}

found:
	spin_unlock(&sbi->s_lock);
	return ino;
}
