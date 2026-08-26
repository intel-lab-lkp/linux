// SPDX-License-Identifier: GPL-2.0
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "minix.h"

enum {DEPTH = 3, DIRECT = 7};	/* Only double indirect */

typedef u16 block_t;	/* 16 bit, host order */

static inline unsigned long block_to_cpu(block_t n)
{
	return n;
}

static inline block_t cpu_to_block(unsigned long n)
{
	return n;
}

static inline block_t *i_data(struct inode *inode)
{
	return (block_t *)minix_i(inode)->u.i1_data;
}

static int block_to_path(struct inode * inode, long block, int offsets[DEPTH])
{
	int n = 0;

	if (block < 0) {
		printk("MINIX-fs: block_to_path: block %ld < 0 on dev %pg\n",
			block, inode->i_sb->s_bdev);
		return 0;
	}
	if ((u64)block * BLOCK_SIZE >= inode->i_sb->s_maxbytes)
		return 0;

	if (block < 7) {
		offsets[n++] = block;
	} else if ((block -= 7) < 512) {
		offsets[n++] = 7;
		offsets[n++] = block;
	} else {
		block -= 512;
		offsets[n++] = 8;
		offsets[n++] = block>>9;
		offsets[n++] = block & 511;
	}
	return n;
}

#include "itree_common.c"
/* NOTA BENE:
 *
 * This is icky to me, but at the same time having it be a standalone C file
 * that's compiled to object form and linked separately like it is in xiafs is
 * much nastier in minix because of the different versions of the minix fs that
 * have some very, very different aspects, like the size of block_t. I don't
 * like it, but since minix already has this pattern where a common itree file
 * is included in the itree_v1 and itree_v2(and v3) files, I'm including iomap.c
 * in these files as well. It does at least avoid exporting some currently
 * static functions that aren't needed anywhere but itree_common.c and iomap.c.
 */
#include "iomap.c"

int V1_minix_get_block(struct inode * inode, long block,
			struct buffer_head *bh_result, int create)
{
	return get_block(inode, block, bh_result, create);
}

void V1_minix_truncate(struct inode * inode)
{
	truncate(inode);
}

unsigned int V1_minix_blocks(loff_t size, struct super_block *sb)
{
	return nblocks(size, sb);
}

int V1_minix_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
	unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	return minix_iomap_begin(inode, offset, length, flags, iomap, srcmap);
}

const struct iomap_ops V1_minix_iomap_ops = {
	.iomap_begin = V1_minix_iomap_begin,
	.iomap_end   = minix_iomap_end,
};
