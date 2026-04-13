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
	struct inode       *inode = file_inode(file);
	struct super_block *sb    = inode->i_sb;
	struct ftrfs_inode_info *fi = FTRFS_I(inode);
	struct buffer_head *bh;
	struct ftrfs_dir_entry *de;
	unsigned long block_idx, block_no;
	unsigned int  offset;

	/* EOF guard */
	if (ctx->pos == INT_MAX)
		return 0;
	/* Emit . and .. (ctx->pos: 0=., 1=.., 2+=real entries) */
	if (ctx->pos < 2) {
		if (!dir_emit_dots(file, ctx))
			return 0;
	}

	/* Iterate over direct blocks only (skeleton: no indirect yet) */
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
				break; /* end of dir block */

			if (de->d_ino && de->d_name_len) {
				if (!dir_emit(ctx,
					      de->d_name,
					      de->d_name_len,
					      le64_to_cpu(de->d_ino),
					      de->d_file_type)) {
					brelse(bh);
					return 0;
				}
				ctx->pos++;
			}

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
	struct super_block      *sb = dir->i_sb;
	struct ftrfs_inode_info *fi = FTRFS_I(dir);
	struct buffer_head      *bh;
	struct ftrfs_dir_entry  *de;
	struct inode            *inode = NULL;
	unsigned long            block_idx, block_no;
	unsigned int             offset;

	if (dentry->d_name.len > FTRFS_MAX_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

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
				break; /* end of dir block */

			if (de->d_ino &&
			    de->d_name_len == dentry->d_name.len &&
			    !memcmp(de->d_name, dentry->d_name.name,
				    de->d_name_len)) {
				unsigned long ino = le64_to_cpu(de->d_ino);

				brelse(bh);
				inode = ftrfs_iget(sb, ino);
				goto found;
			}

			offset += le16_to_cpu(de->d_rec_len);
		}
		brelse(bh);
	}

found:
	return d_splice_alias(inode, dentry);
}

const struct file_operations ftrfs_dir_operations = {
	.llseek  = generic_file_llseek,
	.read    = generic_read_dir,
	.iterate_shared = ftrfs_readdir,
};


