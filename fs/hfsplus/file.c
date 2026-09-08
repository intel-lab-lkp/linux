// SPDX-License-Identifier: GPL-2.0
/*
 * File operations: open/release/fsync and iomap-based read/write/seek
 */

#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/mount.h>

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"

static int hfsplus_file_open(struct inode *inode, struct file *file)
{
	if (HFSPLUS_IS_RSRC(inode))
		inode = HFSPLUS_I(inode)->rsrc_inode;
	if (!(file->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;
	atomic_inc(&HFSPLUS_I(inode)->opencnt);
	return 0;
}

static int hfsplus_file_release(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;

	if (HFSPLUS_IS_RSRC(inode))
		inode = HFSPLUS_I(inode)->rsrc_inode;
	if (atomic_dec_and_test(&HFSPLUS_I(inode)->opencnt)) {
		inode_lock(inode);
		hfsplus_file_truncate(inode);
		if (inode->i_flags & S_DEAD) {
			hfsplus_delete_cat(inode->i_ino,
					   HFSPLUS_SB(sb)->hidden_dir, NULL);
			hfsplus_delete_inode(inode);
		}
		inode_unlock(inode);
	}
	return 0;
}

int hfsplus_file_fsync(struct file *file, loff_t start, loff_t end,
		       int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct hfsplus_inode_info *hip = HFSPLUS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(inode->i_sb);
	struct hfsplus_vh *vhdr = sbi->s_vhdr;
	int error = 0, error2;

	hfs_dbg("inode->i_ino %llu, start %llu, end %llu\n",
		inode->i_ino, start, end);

	error = file_write_and_wait_range(file, start, end);
	if (error)
		return error;
	inode_lock(inode);

	/*
	 * Sync inode metadata into the catalog and extent trees.
	 */
	sync_inode_metadata(inode, 1);

	/*
	 * And explicitly write out the btrees.
	 */
	if (test_and_clear_bit(HFSPLUS_I_CAT_DIRTY,
				&HFSPLUS_I(HFSPLUS_CAT_TREE_I(sb))->flags)) {
		clear_bit(HFSPLUS_I_CAT_DIRTY, &hip->flags);
		error = filemap_write_and_wait(sbi->cat_tree->inode->i_mapping);
	}

	if (test_and_clear_bit(HFSPLUS_I_EXT_DIRTY,
				&HFSPLUS_I(HFSPLUS_EXT_TREE_I(sb))->flags)) {
		clear_bit(HFSPLUS_I_EXT_DIRTY, &hip->flags);
		error2 =
			filemap_write_and_wait(sbi->ext_tree->inode->i_mapping);
		if (!error)
			error = error2;
	}

	if (sbi->attr_tree) {
		if (test_and_clear_bit(HFSPLUS_I_ATTR_DIRTY,
				&HFSPLUS_I(HFSPLUS_ATTR_TREE_I(sb))->flags)) {
			clear_bit(HFSPLUS_I_ATTR_DIRTY, &hip->flags);
			error2 =
				filemap_write_and_wait(
					    sbi->attr_tree->inode->i_mapping);
			if (!error)
				error = error2;
		}
	} else {
		if (test_and_clear_bit(HFSPLUS_I_ATTR_DIRTY, &hip->flags))
			pr_err("sync non-existent attributes tree\n");
	}

	if (test_and_clear_bit(HFSPLUS_I_ALLOC_DIRTY,
				&HFSPLUS_I(sbi->alloc_file)->flags)) {
		clear_bit(HFSPLUS_I_ALLOC_DIRTY, &hip->flags);
		error2 = filemap_write_and_wait(sbi->alloc_file->i_mapping);
		if (!error)
			error = error2;
	}

	mutex_lock(&sbi->vh_mutex);
	hfsplus_prepare_volume_header_for_commit(vhdr);
	mutex_unlock(&sbi->vh_mutex);

	error2 = hfsplus_commit_superblock(inode->i_sb);
	if (!error)
		error = error2;

	if (!test_bit(HFSPLUS_SB_NOBARRIER, &sbi->flags))
		blkdev_issue_flush(inode->i_sb->s_bdev);

	inode_unlock(inode);

	return error;
}

const struct file_operations hfsplus_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.fsync		= hfsplus_file_fsync,
	.open		= hfsplus_file_open,
	.release	= hfsplus_file_release,
	.unlocked_ioctl = hfsplus_ioctl,
};
