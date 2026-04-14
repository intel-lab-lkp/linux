// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — File operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include "ftrfs.h"

const struct file_operations ftrfs_file_operations = {
	.llseek         = generic_file_llseek,
	.read_iter      = generic_file_read_iter,
	.write_iter     = generic_file_write_iter,
	.mmap           = generic_file_mmap,
	.fsync          = generic_file_fsync,
	.splice_read    = filemap_splice_read,
};

const struct inode_operations ftrfs_file_inode_operations = {
	.getattr        = simple_getattr,
};

/*
 * ftrfs_get_block — map logical block to physical block
 * Handles allocation when create=1, returns -EIO for holes on read.
 */
static int ftrfs_get_block(struct inode *inode, sector_t iblock,
			   struct buffer_head *bh_result, int create)
{
	struct ftrfs_inode_info *fi = FTRFS_I(inode);
	u64 new_block;
	__le64 phys;

	if (iblock >= FTRFS_DIRECT_BLOCKS) {
		pr_err("ftrfs: indirect block not yet supported\n");
		return -EOPNOTSUPP;
	}

	phys = fi->i_direct[iblock];
	if (phys) {
		map_bh(bh_result, inode->i_sb, le64_to_cpu(phys));
		bh_result->b_size = 1 << inode->i_blkbits;
		return 0;
	}

	if (!create)
		return -EIO;

	new_block = ftrfs_alloc_block(inode->i_sb);
	if (!new_block) {
		pr_err("ftrfs: no free blocks\n");
		return -ENOSPC;
	}

	fi->i_direct[iblock] = cpu_to_le64(new_block);
	map_bh(bh_result, inode->i_sb, new_block);
	bh_result->b_size = 1 << inode->i_blkbits;
	set_buffer_new(bh_result);
	return 0;
}

static int ftrfs_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, ftrfs_get_block);
}

static int ftrfs_writepages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, ftrfs_get_block);
}

static void ftrfs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ftrfs_get_block);
}

static int ftrfs_write_begin(const struct kiocb *iocb,
			     struct address_space *mapping,
			     loff_t pos, unsigned int len,
			     struct folio **foliop, void **fsdata)
{
	return block_write_begin(mapping, pos, len, foliop, ftrfs_get_block);
}

static int ftrfs_write_end(const struct kiocb *iocb,
			   struct address_space *mapping,
			   loff_t pos, unsigned int len, unsigned int copied,
			   struct folio *folio, void *fsdata)
{
	return generic_write_end(iocb, mapping, pos, len, copied, folio, fsdata);
}

static sector_t ftrfs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, ftrfs_get_block);
}

const struct address_space_operations ftrfs_aops = {
	.read_folio       = ftrfs_read_folio,
	.readahead        = ftrfs_readahead,
	.write_begin      = ftrfs_write_begin,
	.write_end        = ftrfs_write_end,
	.writepages       = ftrfs_writepages,
	.bmap             = ftrfs_bmap,
	.dirty_folio      = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
};
