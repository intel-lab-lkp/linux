// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — Directory operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include "ftrfs.h"

/*
 * ftrfs_readdir — iterate directory entries
 */
static int ftrfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct ftrfs_inode_info *fi = FTRFS_I(inode);
	struct buffer_head *bh;
	struct ftrfs_dir_entry *de;
	unsigned long block_idx, block_no;
	unsigned int offset;

	if (ctx->pos == INT_MAX)
		return 0;

	/* Emit . and .. (ctx->pos: 0=., 1=.., 2+=real entries) */
	if (ctx->pos < 2) {
		if (!dir_emit_dots(file, ctx))
			return 0;
	}

	for (block_idx = 0; block_idx < FTRFS_DIRECT_BLOCKS; block_idx++) {
		block_no = le64_to_cpu(fi->i_direct[block_idx]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			continue;

		offset = 0;
		while (offset < FTRFS_BLOCK_SIZE) {
			de = (struct ftrfs_dir_entry *)(bh->b_data + offset);
			if (!de->d_rec_len)
				break;

			/* Skip . and .. — emitted by dir_emit_dots */
			if (!de->d_ino || !de->d_name_len)
				goto next;
			if (de->d_name_len == 1 && de->d_name[0] == '.')
				goto next;
			if (de->d_name_len == 2 && de->d_name[0] == '.'
			    && de->d_name[1] == '.')
				goto next;

			if (!dir_emit(ctx, de->d_name, de->d_name_len,
				      le64_to_cpu(de->d_ino),
				      de->d_file_type)) {
				brelse(bh);
				return 0;
			}
			ctx->pos++;
next:
			offset += le16_to_cpu(de->d_rec_len);
		}
		brelse(bh);
	}

	ctx->pos = INT_MAX;
	return 0;
}

/*
 * ftrfs_lookup — find dentry in directory
 */
struct dentry *ftrfs_lookup(struct inode *dir,
			    struct dentry *dentry,
			    unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct ftrfs_inode_info *fi = FTRFS_I(dir);
	struct ftrfs_dir_entry *de;
	struct buffer_head *bh;
	unsigned int offset;
	unsigned long block_no;
	int i;

	for (i = 0; i < FTRFS_DIRECT_BLOCKS; i++) {
		block_no = le64_to_cpu(fi->i_direct[i]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			continue;

		offset = 0;
		while (offset + sizeof(*de) <= FTRFS_BLOCK_SIZE) {
			de = (struct ftrfs_dir_entry *)(bh->b_data + offset);
			if (!de->d_rec_len)
				break;
			if (de->d_ino &&
			    de->d_name_len == dentry->d_name.len &&
			    !memcmp(de->d_name, dentry->d_name.name,
				    dentry->d_name.len)) {
				u64 ino = le64_to_cpu(de->d_ino);
				struct inode *inode;

				brelse(bh);
				inode = ftrfs_iget(sb, ino);
				return d_splice_alias(inode, dentry);
			}
			offset += le16_to_cpu(de->d_rec_len);
			if (!de->d_rec_len)
				break;
		}
		brelse(bh);
	}
	return d_splice_alias(NULL, dentry);
}

const struct file_operations ftrfs_dir_operations = {
	.llseek  = generic_file_llseek,
	.read    = generic_read_dir,
	.iterate_shared = ftrfs_readdir,
};
