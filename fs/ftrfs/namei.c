// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — Filename / directory entry operations
 * Author: Aurélien DESBRIERES <aurelien@hackers.camp>
 *
 * Implements: create, mkdir, unlink, rmdir, link, rename
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/time.h>
#include "ftrfs.h"

/* ------------------------------------------------------------------ */
/* Helper: write a raw ftrfs_inode to disk                             */
/* ------------------------------------------------------------------ */

static int ftrfs_write_inode_raw(struct inode *inode)
{
	struct super_block      *sb  = inode->i_sb;
	struct ftrfs_sb_info    *sbi = FTRFS_SB(sb);
	struct ftrfs_inode_info *fi  = FTRFS_I(inode);
	struct ftrfs_inode      *raw;
	struct buffer_head      *bh;
	unsigned long            inodes_per_block;
	unsigned long            block, offset;

	inodes_per_block = FTRFS_BLOCK_SIZE / sizeof(struct ftrfs_inode);
	block  = le64_to_cpu(sbi->s_ftrfs_sb->s_inode_table_blk)
		 + (inode->i_ino - 1) / inodes_per_block;
	offset = (inode->i_ino - 1) % inodes_per_block;

	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;

	raw = (struct ftrfs_inode *)bh->b_data + offset;

	raw->i_mode   = cpu_to_le16(inode->i_mode);
	raw->i_uid    = cpu_to_le32(i_uid_read(inode));
	raw->i_gid    = cpu_to_le32(i_gid_read(inode));
	raw->i_nlink  = cpu_to_le16(inode->i_nlink);
	raw->i_size   = cpu_to_le64(inode->i_size);
	raw->i_atime  = cpu_to_le64(inode_get_atime_sec(inode) * NSEC_PER_SEC
				     + inode_get_atime_nsec(inode));
	raw->i_mtime  = cpu_to_le64(inode_get_mtime_sec(inode) * NSEC_PER_SEC
				     + inode_get_mtime_nsec(inode));
	raw->i_ctime  = cpu_to_le64(inode_get_ctime_sec(inode) * NSEC_PER_SEC
				     + inode_get_ctime_nsec(inode));
	raw->i_flags  = cpu_to_le32(fi->i_flags);

	memcpy(raw->i_direct, fi->i_direct, sizeof(fi->i_direct));
	raw->i_indirect  = fi->i_indirect;
	raw->i_dindirect = fi->i_dindirect;
	raw->i_tindirect = fi->i_tindirect;

	raw->i_crc32 = ftrfs_crc32(raw,
				    offsetof(struct ftrfs_inode, i_crc32));

	mark_buffer_dirty(bh);
	brelse(bh);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: add a directory entry to a directory inode                  */
/* ------------------------------------------------------------------ */

static int ftrfs_add_dirent(struct inode *dir, const struct qstr *name,
			    u64 ino, unsigned int file_type)
{
	struct super_block      *sb = dir->i_sb;
	struct ftrfs_inode_info *fi = FTRFS_I(dir);
	struct ftrfs_dir_entry  *de;
	struct buffer_head      *bh;
	unsigned int             offset;
	u64                      block_no;
	int                      i;

	/* Look for space in existing direct blocks */
	for (i = 0; i < FTRFS_DIRECT_BLOCKS; i++) {
		block_no = le64_to_cpu(fi->i_direct[i]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			return -EIO;

		offset = 0;
		while (offset + sizeof(*de) <= FTRFS_BLOCK_SIZE) {
			de = (struct ftrfs_dir_entry *)(bh->b_data + offset);

			/* Free slot: ino == 0 */
			if (!de->d_ino) {
				de->d_ino       = cpu_to_le64(ino);
				de->d_name_len  = name->len;
				de->d_file_type = file_type;
				de->d_rec_len   = cpu_to_le16(
					sizeof(struct ftrfs_dir_entry));
				memcpy(de->d_name, name->name, name->len);
				de->d_name[name->len] = '\0';
				mark_buffer_dirty(bh);
				brelse(bh);
				inode_set_mtime_to_ts(dir,
					current_time(dir));
				mark_inode_dirty(dir);
				return 0;
			}
			offset += le16_to_cpu(de->d_rec_len);
			if (!de->d_rec_len)
				break;
		}
		brelse(bh);
	}

	/* Need a new block */
	if (i >= FTRFS_DIRECT_BLOCKS)
		return -ENOSPC;

	block_no = ftrfs_alloc_block(sb);
	if (!block_no)
		return -ENOSPC;

	bh = sb_bread(sb, block_no);
	if (!bh) {
		ftrfs_free_block(sb, block_no);
		return -EIO;
	}

	memset(bh->b_data, 0, FTRFS_BLOCK_SIZE);

	de = (struct ftrfs_dir_entry *)bh->b_data;
	de->d_ino       = cpu_to_le64(ino);
	de->d_name_len  = name->len;
	de->d_file_type = file_type;
	de->d_rec_len   = cpu_to_le16(sizeof(struct ftrfs_dir_entry));
	memcpy(de->d_name, name->name, name->len);
	de->d_name[name->len] = '\0';

	mark_buffer_dirty(bh);
	brelse(bh);

	fi->i_direct[i] = cpu_to_le64(block_no);
	dir->i_size += FTRFS_BLOCK_SIZE;
	inode_set_mtime_to_ts(dir, current_time(dir));
	mark_inode_dirty(dir);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: remove a directory entry from a directory                   */
/* ------------------------------------------------------------------ */

static int ftrfs_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct super_block      *sb = dir->i_sb;
	struct ftrfs_inode_info *fi = FTRFS_I(dir);
	struct ftrfs_dir_entry  *de;
	struct buffer_head      *bh;
	unsigned int             offset;
	u64                      block_no;
	int                      i;

	for (i = 0; i < FTRFS_DIRECT_BLOCKS; i++) {
		block_no = le64_to_cpu(fi->i_direct[i]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			return -EIO;

		offset = 0;
		while (offset + sizeof(*de) <= FTRFS_BLOCK_SIZE) {
			de = (struct ftrfs_dir_entry *)(bh->b_data + offset);

			if (de->d_ino &&
			    de->d_name_len == name->len &&
			    !memcmp(de->d_name, name->name, name->len)) {
				/* Zero out the entry (mark as free) */
				memset(de, 0, sizeof(*de));
				mark_buffer_dirty(bh);
				brelse(bh);
				inode_set_mtime_to_ts(dir,
					current_time(dir));
				mark_inode_dirty(dir);
				return 0;
			}

			if (!de->d_rec_len)
				break;
			offset += le16_to_cpu(de->d_rec_len);
		}
		brelse(bh);
	}

	return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Helper: allocate and initialize a new VFS inode                     */
/* ------------------------------------------------------------------ */

struct inode *ftrfs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block   *sb = dir->i_sb;
	struct inode         *inode;
	struct ftrfs_inode_info *fi;
	u64                   ino;

	ino = ftrfs_alloc_inode_num(sb);
	if (!ino)
		return ERR_PTR(-ENOSPC);

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_ino    = ino;
	inode->i_size   = 0;
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_to_ts(inode, current_time(inode));

	fi = FTRFS_I(inode);
	memset(fi->i_direct, 0, sizeof(fi->i_direct));
	fi->i_indirect  = 0;
	fi->i_dindirect = 0;
	fi->i_flags     = 0;

	if (S_ISDIR(mode)) {
		inode->i_op  = &ftrfs_dir_inode_operations;
		inode->i_fop = &ftrfs_dir_operations;
		set_nlink(inode, 2);
	} else {
		inode->i_op  = &ftrfs_file_inode_operations;
		inode->i_fop = &ftrfs_file_operations;
		inode->i_mapping->a_ops = &ftrfs_aops;
		set_nlink(inode, 1);
	}

	if (insert_inode_locked(inode) < 0) {
		make_bad_inode(inode);
		iput(inode);
		return ERR_PTR(-EIO);
	}
	mark_inode_dirty(inode);
	return inode;
}

/* ------------------------------------------------------------------ */
/* create — create a regular file                                       */
/* ------------------------------------------------------------------ */

static int ftrfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;
	int           ret;

	inode = ftrfs_new_inode(dir, mode | S_IFREG);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ret = ftrfs_write_inode_raw(inode);
	if (ret)
		goto out_iput;

	ret = ftrfs_add_dirent(dir, &dentry->d_name, inode->i_ino, 1 /* DT_REG */);
	if (ret)
		goto out_iput;

	ret = ftrfs_write_inode_raw(dir);
	if (ret)
		goto out_iput;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return 0;

out_iput:
	unlock_new_inode(inode);
	iput(inode);
	return ret;
}

/* ------------------------------------------------------------------ */
/* mkdir — create a directory                                          */
/* ------------------------------------------------------------------ */

static struct dentry *ftrfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int           ret;

	inode_inc_link_count(dir);

	inode = ftrfs_new_inode(dir, mode | S_IFDIR);
	if (IS_ERR(inode)) {
		inode_dec_link_count(dir);
		return ERR_CAST(inode);
	}

	/* Add . and .. entries */
	ret = ftrfs_add_dirent(inode, &(struct qstr)QSTR_INIT(".", 1),
			       inode->i_ino, 4 /* DT_DIR */);
	if (ret)
		goto out_fail;

	ret = ftrfs_add_dirent(inode, &(struct qstr)QSTR_INIT("..", 2),
			       dir->i_ino, 4 /* DT_DIR */);
	if (ret)
		goto out_fail;

	ret = ftrfs_write_inode_raw(inode);
	if (ret)
		goto out_fail;

	ret = ftrfs_add_dirent(dir, &dentry->d_name, inode->i_ino,
			       4 /* DT_DIR */);
	if (ret)
		goto out_fail;

	ret = ftrfs_write_inode_raw(dir);
	if (ret)
		goto out_fail;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return NULL;

out_fail:
	unlock_new_inode(inode);
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	iput(inode);
	inode_dec_link_count(dir);
	return ERR_PTR(ret);
}

/* ------------------------------------------------------------------ */
/* unlink — remove a file                                              */
/* ------------------------------------------------------------------ */

static int ftrfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int           ret;

	ret = ftrfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		return ret;

	inode_set_ctime_to_ts(inode, current_time(inode));
	inode_dec_link_count(inode);
	ftrfs_write_inode_raw(dir);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rmdir — remove an empty directory                                   */
/* ------------------------------------------------------------------ */

static int ftrfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int           ret;

	if (inode->i_nlink > 2)
		return -ENOTEMPTY;

	ret = ftrfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		return ret;

	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	inode_dec_link_count(dir);
	ftrfs_write_inode_raw(dir);
	return 0;
}

/* ------------------------------------------------------------------ */
/* link — create a hard link                                           */
/* ------------------------------------------------------------------ */

static int ftrfs_link(struct dentry *old_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int           ret;

	inode_set_ctime_to_ts(inode, current_time(inode));
	inode_inc_link_count(inode);

	ret = ftrfs_add_dirent(dir, &dentry->d_name, inode->i_ino, 1);
	if (ret) {
		inode_dec_link_count(inode);
		return ret;
	}

	ftrfs_write_inode_raw(inode);
	ftrfs_write_inode_raw(dir);
	d_instantiate(dentry, inode);
	ihold(inode);
	return 0;
}

/* ------------------------------------------------------------------ */
/* write_inode — VFS super_op: persist inode to disk                  */
/* ------------------------------------------------------------------ */

int ftrfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return ftrfs_write_inode_raw(inode);
}

/* ------------------------------------------------------------------ */
/* dir inode_operations — exported                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* rename — move/rename a directory entry                              */
/* ------------------------------------------------------------------ */

static int ftrfs_rename(struct mnt_idmap *idmap,
			struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	int	      is_dir    = S_ISDIR(old_inode->i_mode);
	int	      ret;

	/* FTRFS v1: no RENAME_EXCHANGE or RENAME_WHITEOUT */
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	if (flags & RENAME_NOREPLACE && new_inode)
		return -EEXIST;

	/*
	 * If destination exists, unlink it first.
	 * For directories: target must be empty (nlink == 2: . and ..)
	 */
	if (new_inode) {
		if (is_dir) {
			if (new_inode->i_nlink > 2)
				return -ENOTEMPTY;
		}

		ret = ftrfs_del_dirent(new_dir, &new_dentry->d_name);
		if (ret)
			return ret;

		if (is_dir) {
			inode_dec_link_count(new_inode);
			inode_dec_link_count(new_inode);
			inode_dec_link_count(new_dir);
		} else {
			inode_dec_link_count(new_inode);
		}

		inode_set_ctime_to_ts(new_inode, current_time(new_inode));
	}

	/* Add entry in new_dir */
	ret = ftrfs_add_dirent(new_dir, &new_dentry->d_name,
			       old_inode->i_ino,
			       is_dir ? 4 /* DT_DIR */ : 1 /* DT_REG */);
	if (ret)
		return ret;

	/* Remove entry from old_dir */
	ret = ftrfs_del_dirent(old_dir, &old_dentry->d_name);
	if (ret) {
		pr_err("ftrfs: rename: del_dirent failed after add, fs may be inconsistent\n");
		return ret;
	}

	/*
	 * Update ".." in the moved directory to point to new_dir.
	 * Also fix nlink on old_dir and new_dir.
	 */
	if (is_dir && old_dir != new_dir) {
		struct qstr dotdot = QSTR_INIT("..", 2);

		ret = ftrfs_del_dirent(old_inode, &dotdot);
		if (ret)
			return ret;

		ret = ftrfs_add_dirent(old_inode, &dotdot,
				       new_dir->i_ino, 4 /* DT_DIR */);
		if (ret)
			return ret;

		inode_dec_link_count(old_dir);
		inode_inc_link_count(new_dir);

		ret = ftrfs_write_inode_raw(old_inode);
		if (ret)
			return ret;
	}

	/* Update timestamps */
	inode_set_ctime_to_ts(old_inode, current_time(old_inode));
	inode_set_mtime_to_ts(old_dir,   current_time(old_dir));
	inode_set_ctime_to_ts(old_dir,   current_time(old_dir));
	inode_set_mtime_to_ts(new_dir,   current_time(new_dir));
	inode_set_ctime_to_ts(new_dir,   current_time(new_dir));

	/* Persist all touched inodes */
	ret = ftrfs_write_inode_raw(old_inode);
	if (ret)
		return ret;

	ret = ftrfs_write_inode_raw(old_dir);
	if (ret)
		return ret;

	if (old_dir != new_dir) {
		ret = ftrfs_write_inode_raw(new_dir);
		if (ret)
			return ret;
	}

	if (new_inode)
		ftrfs_write_inode_raw(new_inode);

	return 0;
}

const struct inode_operations ftrfs_dir_inode_operations = {
	.lookup  = ftrfs_lookup,
	.create  = ftrfs_create,
	.mkdir   = ftrfs_mkdir,
	.unlink  = ftrfs_unlink,
	.rmdir   = ftrfs_rmdir,
	.link    = ftrfs_link,
	.rename  = ftrfs_rename,
};
