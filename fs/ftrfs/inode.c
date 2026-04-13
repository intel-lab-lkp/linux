// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — Inode operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/time.h>
#include "ftrfs.h"

/*
 * ftrfs_iget — read inode from disk into VFS
 * @sb:  superblock
 * @ino: inode number (1-based)
 *
 * Inode table starts at s_inode_table_blk.
 * Each block holds FTRFS_BLOCK_SIZE / sizeof(ftrfs_inode) inodes.
 */
struct inode *ftrfs_iget(struct super_block *sb, unsigned long ino)
{
	struct ftrfs_sb_info    *sbi = FTRFS_SB(sb);
	struct ftrfs_inode_info *fi;
	struct ftrfs_inode      *raw;
	struct buffer_head      *bh;
	struct inode            *inode;
	unsigned long            inodes_per_block;
	unsigned long            block, offset;
	__u32                    crc;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	/* Already in cache */
	if (!ftrfs_inode_is_new(inode))
		return inode;

	inodes_per_block = FTRFS_BLOCK_SIZE / sizeof(struct ftrfs_inode);
	block  = le64_to_cpu(sbi->s_ftrfs_sb->s_inode_table_blk)
		 + (ino - 1) / inodes_per_block;
	offset = (ino - 1) % inodes_per_block;

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("ftrfs: unable to read inode block %lu\n", block);
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}

	raw = (struct ftrfs_inode *)bh->b_data + offset;
	/* Verify inode CRC32 */
	crc = ftrfs_crc32(raw, offsetof(struct ftrfs_inode, i_crc32));
	if (crc != le32_to_cpu(raw->i_crc32)) {
		pr_err("ftrfs: inode %lu CRC32 mismatch\n", ino);
		brelse(bh);
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}

	fi = FTRFS_I(inode);

	/* Populate VFS inode */
	inode->i_mode  = le16_to_cpu(raw->i_mode);
	inode->i_uid   = make_kuid(sb->s_user_ns, le32_to_cpu(raw->i_uid));
	inode->i_gid   = make_kgid(sb->s_user_ns, le32_to_cpu(raw->i_gid));
	set_nlink(inode, le16_to_cpu(raw->i_nlink));
	inode->i_size  = le64_to_cpu(raw->i_size);

	inode_set_atime(inode,
		le64_to_cpu(raw->i_atime) / NSEC_PER_SEC,
		le64_to_cpu(raw->i_atime) % NSEC_PER_SEC);
	inode_set_mtime(inode,
		le64_to_cpu(raw->i_mtime) / NSEC_PER_SEC,
		le64_to_cpu(raw->i_mtime) % NSEC_PER_SEC);
	inode_set_ctime(inode,
		le64_to_cpu(raw->i_ctime) / NSEC_PER_SEC,
		le64_to_cpu(raw->i_ctime) % NSEC_PER_SEC);

	/* Copy block pointers to in-memory inode */
	memcpy(fi->i_direct, raw->i_direct, sizeof(fi->i_direct));
	fi->i_indirect  = raw->i_indirect;
	fi->i_dindirect = raw->i_dindirect;
	fi->i_tindirect = raw->i_tindirect;
	fi->i_flags     = le32_to_cpu(raw->i_flags);

	/* Set ops based on file type */
	if (S_ISDIR(inode->i_mode)) {
		inode->i_op  = &ftrfs_dir_inode_operations;
		inode->i_fop = &ftrfs_dir_operations;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op  = &ftrfs_file_inode_operations;
		inode->i_fop = &ftrfs_file_operations;
		inode->i_mapping->a_ops = &ftrfs_aops;
	} else {
		/* Special files: use generic */
		init_special_inode(inode, inode->i_mode, 0);
	}

	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}
