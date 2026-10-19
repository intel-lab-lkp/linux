// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Directory operations
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 */
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/fs_dirent.h>
#include "beamfs.h"

/*
 * beamfs_readdir - iterate directory entries
 *
 * ctx->pos encoding:
 *   0, 1       : '.' and '..' (emitted by dir_emit_dots)
 *   INT_MAX    : EOF
 *   other      : 2 + block_idx * BEAMFS_DATA_INLINE_BYTES + offset
 *
 * block_idx is the 0-based logical block, direct then single indirect;
 * offset is a byte offset into that block's 3824-byte payload. The
 * cookie names where reading resumes, and any value is accepted: one
 * that falls inside a record resumes at the next record boundary.
 *
 * At most BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS = 524 blocks, so
 * pos is at most 2 + 524 * 3824, well within the 32-bit pos space.
 */
static int beamfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode            *inode = file_inode(file);
	struct super_block      *sb    = inode->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de;
	u8            *payload;
	unsigned long  block_no;
	u32            offset;
	int            block_idx;
	int            start_block;
	u32            start_off;
	int            ret = 0;

	/*
	 * ctx->pos is a byte offset into the directory, and any value is
	 * legal.
	 *
	 * It used to be (block + 1) << 16 | offset, which cannot express
	 * a position below 65536 as anything but block -1: seekdir to a
	 * cookie this filesystem never issued -- which is exactly what
	 * t_readdir_3 does -- started the walk outside every block and
	 * returned nothing. "Unexpected EOF while reading dir",
	 * reproducible at thirty files and above.
	 *
	 * ext2 treats pos as the offset it is: block = pos / blocksize,
	 * offset = pos % blocksize, and a position past the end simply
	 * finds no entries. Same here, with 3824-byte blocks. Positions
	 * 0 and 1 stay reserved for dot and dotdot, so the first real
	 * entry sits at 2 and the arithmetic starts one step in.
	 */
	if (ctx->pos == INT_MAX)
		return 0;

	if (ctx->pos < 2) {
		if (!dir_emit_dots(file, ctx))
			return 0;
	}

	if (ctx->pos <= 2) {
		start_block = 0;
		start_off   = 0;
	} else {
		u64 p = (u64)ctx->pos - 2;

		/*
		 * A directory block is not a capsule.
		 *
		 * dirent.c encodes and decodes one in the alternating
		 * layout, so its payload is 3824 whatever the volume's
		 * data blocks do. Following beamfs_block_payload here
		 * would have the reader seek by 3808 into blocks laid
		 * out in 3824 -- every entry past the first block off by
		 * sixteen bytes.
		 */
		start_block = (int)div_u64_rem(p, BEAMFS_DATA_INLINE_BYTES, &start_off);
	}

	payload = beamfs_scratch_get(sb);
	if (!payload)
		return -ENOMEM;

	for (block_idx = start_block;
	     block_idx < (int)(BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS);
	     block_idx++) {
		u64 _bno;
		int _ret = beamfs_dir_get_block(inode, block_idx, false, &_bno);

		if (_ret) {
			ret = _ret;
			goto out;
		}
		block_no = (unsigned long)_bno;
		if (!block_no)
			break;

		bh = beamfs_bread(sb, block_no, "directory");
		if (!bh)
			continue;

		_ret = beamfs_dirent_decode(sb, bh, payload);
		brelse(bh);
		if (_ret < 0) {
			pr_err_ratelimited("beamfs: directory block %lu uncorrectable\n",
					   block_no);
			ret = _ret;
			goto out;
		}

		offset = (block_idx == start_block) ? start_off : 0;

		/*
		 * Slide to a record boundary.
		 *
		 * seekdir can land mid-record, and reading a header from
		 * the middle of a name yields nonsense. Walk from the top
		 * of the block to the first record at or after the
		 * requested offset -- ext2_validate_entry does the same,
		 * for the same reason.
		 */
		if (offset != 0 && offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 scan = 0;

			while (scan < offset) {
				u32 nxt;

				de = (struct beamfs_dir_entry *)(payload + scan);
				if (!beamfs_dirent_valid(de, scan))
					break;
				nxt = beamfs_dirent_next(de, scan);
				if (nxt == BEAMFS_DIRENT_NOSPACE || nxt <= scan)
					break;
				scan = nxt;
			}
			offset = scan;
		}

		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			if (de->d_ino &&
			    !(de->d_name_len == 1 && de->d_name[0] == '.') &&
			    !(de->d_name_len == 2 && de->d_name[0] == '.' &&
			      de->d_name[1] == '.')) {
				u8 dt;

				switch (de->d_file_type) {
				case 1:
					dt = DT_REG;
					break;
				case 2:
					dt = DT_DIR;
					break;
				default:
					dt = de->d_file_type;
					break;
				}

				if (!dir_emit(ctx, de->d_name, de->d_name_len,
					      le64_to_cpu(de->d_ino), dt))
					goto out;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			offset = next;

			/*
			 * Advance after emitting, and for every record, so
			 * the cookie names where reading resumes rather
			 * than the entry just returned.
			 */
			ctx->pos = 2 + (loff_t)block_idx *
					BEAMFS_DATA_INLINE_BYTES + offset;
		}
	}

	ctx->pos = INT_MAX;
out:
	beamfs_scratch_put(sb, payload);
	return ret;
}

/*
 * beamfs_lookup - find dentry in directory
 */
struct dentry *beamfs_lookup(struct inode *dir,
			     struct dentry *dentry,
			     unsigned int flags)
{
	struct super_block      *sb = dir->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de;
	struct inode            *inode = NULL;
	u8            *payload;
	unsigned long  block_no;
	u32            offset;
	unsigned int   i;
	int            ret = 0;

	if (dentry->d_name.len > BEAMFS_MAX_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

	payload = beamfs_scratch_get(sb);
	if (!payload)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS; i++) {
		u64 _bno;
		int _ret = beamfs_dir_get_block(dir, i, false, &_bno);

		if (_ret) {
			ret = _ret;
			goto out;
		}
		block_no = (unsigned long)_bno;
		if (!block_no)
			break;

		bh = beamfs_bread(sb, block_no, "directory");
		if (!bh)
			continue;

		_ret = beamfs_dirent_decode(sb, bh, payload);
		brelse(bh);
		if (_ret < 0) {
			ret = _ret;
			goto out;
		}

		offset = 0;
		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			if (de->d_ino &&
			    de->d_name_len == dentry->d_name.len &&
			    !memcmp(de->d_name, dentry->d_name.name,
				    de->d_name_len)) {
				inode = beamfs_iget(sb,
						    (unsigned long)le64_to_cpu(de->d_ino));
				goto out;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			offset = next;
		}
	}

out:
	beamfs_scratch_put(sb, payload);
	if (ret)
		return ERR_PTR(ret);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	return d_splice_alias(inode, dentry);
}

const struct file_operations beamfs_dir_operations = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = beamfs_readdir,
};
