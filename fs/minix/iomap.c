// SPDX-License-Identifier: GPL-2.0-only
/*
 * iomap functions for minix.
 */

static inline void minix_chain_cleanup(Indirect *chain, Indirect *partial)
{
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
}

static inline void minix_iomap_set_mapped(struct iomap *iomap, sector_t phys,
		unsigned int blkbits, sector_t iblock)
{
	iomap->type = IOMAP_MAPPED;
	iomap->addr = (u64)phys << blkbits;
	iomap->length = 1 << blkbits;
	iomap->offset = (u64)iblock << blkbits;
}

static inline void minix_iomap_set_hole(struct iomap *iomap,
		unsigned int blkbits, sector_t iblock)
{
	iomap->type = IOMAP_HOLE;
	iomap->addr = IOMAP_NULL_ADDR;
	iomap->length = 1 << blkbits;
	iomap->offset = (u64)iblock << blkbits;
}

/*
 * minix_iomap_begin - map a file range to disk blocks. It acts as a replacement
 * for get_block in itree_common.c, at least in the important ways, and is
 * adapted from it, but it uses iomap instead of buffer_head.
 */
static int minix_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
	unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct super_block *sb = inode->i_sb;
	unsigned int blkbits = sb->s_blocksize_bits;
	sector_t iblock = offset >> blkbits;
	int create = flags & IOMAP_WRITE;

	int offsets[DEPTH];
	Indirect chain[DEPTH];
	Indirect *partial;
	int depth = block_to_path(inode, iblock, offsets);
	int left;
	int err = -EIO;

	sector_t phys;

	/* block is beyond max file size */
	if (depth == 0)
		return -EINVAL;

	iomap->bdev = inode->i_sb->s_bdev;

reread:
	partial = get_branch(inode, depth, offsets, chain, &err);

	/* Simplest case - block found, no allocation needed */
	if (!partial) {
		iomap->flags = 0;
		phys = block_to_cpu(chain[depth - 1].key);
		partial = chain+depth-1;
		minix_iomap_set_mapped(iomap, phys, blkbits, iblock);
		minix_chain_cleanup(chain, partial);
		return err;
	}

	/* Next simple case - plain lookup or failed read of indirect block */
	if (!create || err == -EIO) {
		minix_iomap_set_hole(iomap, blkbits, iblock);
		minix_chain_cleanup(chain, partial);
		return err;
	}

	/*
	 * This is held over from the original get_block logic, where it
	 * acted as a guard in case truncate() deleted blocks from under that
	 * function. There should not be a race with iomap operations, but
	 * we're retaining the defensive coding here to be extra safe just in
	 * case.
	 */
	if (err == -EAGAIN) {
		minix_chain_cleanup(chain, partial);
		goto reread;
	}

	left = (chain + depth) - partial;
	err = alloc_branch(inode, left, offsets + (partial - chain), partial);
	if (err) {
		minix_chain_cleanup(chain, partial);
		return err;
	}

	if (splice_branch(inode, chain, partial, left) < 0) {
		minix_chain_cleanup(chain, partial);
		goto reread;
	}

	/* Successful allocation, mapping it. */
	iomap->flags = IOMAP_F_NEW;
	phys = block_to_cpu(chain[depth - 1].key);
	minix_iomap_set_mapped(iomap, phys, blkbits, iblock);
	minix_chain_cleanup(chain, partial);

	return err;
}

/*
 * minix_iomap_end ends up being a nop; since minix doesn't have any extents or
 * transactions to worry about, there isn't anything to update here. The on-disk
 * indirect blocks get dirtied in minix_iomap_begin.
 */
static int minix_iomap_end(struct inode *inode, loff_t offset, loff_t length,
	ssize_t written, unsigned int flags, struct iomap *iomap)
{
	return 0;
}
