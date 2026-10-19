// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Inode operations
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include "beamfs.h"

/*
 * Split a nanosecond count into the seconds and nanoseconds a timespec64
 * wants, with the nanosecond part never negative.
 */
static void beamfs_split_ns(s64 ns, s64 *sec, long *nsec)
{
	s32 rem;

	*sec = div_s64_rem(ns, NSEC_PER_SEC, &rem);
	if (rem < 0) {
		*sec -= 1;
		rem += NSEC_PER_SEC;
	}
	*nsec = rem;
}

#define beamfs_set_time_from_ns(inode, setter, ns)			\
	do {								\
		s64  _sec;						\
		long _nsec;						\
									\
		beamfs_split_ns((ns), &_sec, &_nsec);			\
		setter((inode), _sec, _nsec);				\
	} while (0)

/*
 * beamfs_iget - read inode from disk into VFS
 * @sb:  superblock
 * @ino: inode number (1-based)
 *
 * Inode table starts at s_inode_table_blk.
 * Each block holds BEAMFS_BLOCK_SIZE / sizeof(beamfs_inode) inodes.
 */
struct inode *beamfs_iget(struct super_block *sb, unsigned long ino)
{
	struct beamfs_sb_info    *sbi = BEAMFS_SB(sb);
	struct beamfs_inode_info *fi;
	struct beamfs_inode      *raw;
	struct buffer_head      *bh;
	struct inode            *inode;
	unsigned long            inodes_per_block;
	unsigned long            block, offset;
	u64                      ninodes;
	__u32                    crc;

	/*
	 * An inode number is on-disk data, so it is attacker- and
	 * flip-controlled. beamfs_lookup passes de->d_ino straight here
	 * and beamfs_dirent_valid checks d_rec_len and d_name_len only,
	 * never d_ino. Without this bound the block computed just below,
	 * s_inode_table_blk + (ino - 1) / inodes_per_block, walks out of
	 * the inode table and an arbitrary block is read as inodes; any
	 * write to the inode that comes back then allocates blocks under
	 * a number no volume of this size can hold. Observed as ino=27192
	 * on a 16384-inode volume, 241 blocks.
	 *
	 * The bound comes from the superblock, not from sbi->s_ninodes:
	 * beamfs_setup_bitmap runs after the root iget in fill_super, so
	 * s_ninodes is still zero on that first call and every mount
	 * would be refused here.
	 */
	ninodes = le64_to_cpu(sbi->s_beamfs_sb->s_inode_count);
	if (ino < BEAMFS_RESERVED_INO_ROOT || (u64)ino > ninodes) {
		pr_err_ratelimited("beamfs: inode number %lu out of range (1..%llu)\n",
				   ino, ninodes);
		return ERR_PTR(-EUCLEAN);
	}

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	/* Already in cache */
	if (!beamfs_inode_is_new(inode))
		return inode;

	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	block  = le64_to_cpu(sbi->s_beamfs_sb->s_inode_table_blk)
		 + (ino - 1) / inodes_per_block;
	offset = (ino - 1) % inodes_per_block;

	bh = beamfs_bread(sb, block, "inode table");
	if (!bh) {
		pr_err("beamfs: unable to read inode block %lu\n", block);
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}

	raw = (struct beamfs_inode *)bh->b_data + offset;

	/*
	 * Integrity check, two stages.
	 *
	 * Stage A: CRC32. The nominal path. >99% of inode reads are
	 * expected to match here at low cost; CRC32 is hardware-accelerated
	 * by crc32_le on architectures that support it.
	 *
	 * Stage B: RS FEC, only invoked if CRC32 fails AND the format
	 * declares RS protection on inodes. Two activation paths:
	 *   - s_data_protection_scheme == INODE_UNIVERSAL (legacy v4)
	 *   - s_feat_incompat & BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS (v5)
	 * Both paths share the same on-disk parity layout in i_reserved
	 * [0..15] (16 bytes covering BEAMFS_INODE_RS_DATA = 172 data
	 * bytes). The kernel write path always emits this parity
	 * (namei.c::beamfs_write_inode_raw) and mkfs.beamfs always seeds
	 * it at format time (rs_encode_inode), so the decoder is safe to
	 * invoke whenever either path is active.
	 *
	 * After a successful RS correction we re-verify CRC32 on the
	 * corrected buffer before accepting the inode. This guards
	 * against the rare case where an SEU on the parity field itself
	 * makes decode_rs8 return success while the data has actually
	 * been altered toward a wrong codeword.
	 */
	/*
	 * A free inode is not a corrupt one.
	 *
	 * Free slots are zeroed, i_crc32 included, and beamfs_inode_crc of
	 * a zeroed inode is 0x0ee93c94 rather than zero -- so every read of
	 * a free inode fails the check below by construction. Dozens of them
	 * appeared in twenty milliseconds during one test run, reported as
	 * "CRC32 mismatch", which is both the wrong error and the wrong
	 * diagnosis: nothing was corrupt, something had followed a stale
	 * reference.
	 *
	 * ESTALE is what the VFS expects here, and it is what ext2 returns
	 * for an inode with no links.
	 */
	if (le16_to_cpu(raw->i_mode) == 0) {
		brelse(bh);
		iget_failed(inode);
		return ERR_PTR(-ESTALE);
	}

	crc = beamfs_inode_crc(raw);
	if (crc != le32_to_cpu(raw->i_crc32)) {
		struct beamfs_super_block *bsb = BEAMFS_SB(sb)->s_beamfs_sb;
		u32 scheme = le32_to_cpu(bsb->s_data_protection_scheme);
		int nerr;

		if (scheme != BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL &&
		    !(BEAMFS_SB(sb)->s_feat_incompat &
		      BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS)) {
			pr_err("beamfs: inode %lu CRC32 mismatch (no RS available, scheme=%u feat_incompat=0x%016llx)\n",
			       ino, scheme,
			       (unsigned long long)BEAMFS_SB(sb)->s_feat_incompat);
			brelse(bh);
			iget_failed(inode);
			return ERR_PTR(-EIO);
		}

		{
			u8 positions[BEAMFS_RS_PARITY / 2];
			struct beamfs_inode *fixed;

			/*
			 * Decode a copy, not the shared buffer.
			 *
			 * decode_rs8 corrects in place, and raw points into
			 * the inode table block, which holds several inodes.
			 * Correcting there rewrites a buffer other inodes are
			 * read from, and the mark_buffer_dirty below then
			 * carries the result to disk -- a speculative fix
			 * written before the CRC has confirmed it, on a block
			 * that may simply have been mid-write.
			 *
			 * The same shape as beamfs_ind_parity_verify had, and
			 * as the data read path avoids: "never into
			 * bh->b_data. decode_rs8 mutates both data and parity."
			 *
			 * Decode into a private copy, confirm with the CRC,
			 * and only then write the corrected inode back under
			 * the buffer lock. A correction that does not pass the
			 * CRC touches nothing.
			 */
			/*
			 * The copy lives on the heap: with it on the stack
			 * the frame of beamfs_iget was 560 bytes, past the
			 * 512 submit-checklist allows, and iget_locked has
			 * just allocated the in-core inode anyway.
			 */
			fixed = kmemdup(raw, sizeof(*fixed), GFP_NOFS);
			if (!fixed) {
				brelse(bh);
				iget_failed(inode);
				return ERR_PTR(-ENOMEM);
			}
			nerr = beamfs_rs_decode((u8 *)fixed,
						BEAMFS_INODE_RS_DATA,
					       fixed->i_reserved,
					       positions,
					       BEAMFS_RS_PARITY / 2,
				"inode");
			if (nerr < 0) {
				pr_err("beamfs: inode %lu uncorrectable\n", ino);
				kfree(fixed);
				brelse(bh);
				iget_failed(inode);
				return ERR_PTR(-EIO);
			}

			crc = beamfs_inode_crc(fixed);
			if (crc != le32_to_cpu(fixed->i_crc32)) {
				pr_err("beamfs: inode %lu CRC32 mismatch after RS correction\n",
				       ino);
				kfree(fixed);
				brelse(bh);
				iget_failed(inode);
				return ERR_PTR(-EIO);
			}

			/* Confirmed: publish the correction. */
			lock_buffer(bh);
			memcpy(raw, fixed, sizeof(*fixed));
			unlock_buffer(bh);
			kfree(fixed);

			/* Log the RS event with its positions: nerr is the number
			 * of data bytes corrected, the entries positions[] holds.
			 */
			if (nerr > 0) {
				unsigned int np = (unsigned int)nerr;

				if (np > BEAMFS_RS_PARITY / 2)
					np = BEAMFS_RS_PARITY / 2;
				/*
				 * The table block, not the inode number: a
				 * clustering pass asks where on the medium the
				 * damage landed, and an inode number is not a
				 * position.
				 */
				beamfs_log_rs_event_flagged(sb, bh->b_blocknr,
							    positions, np,
						   BEAMFS_INODE_RS_DATA,
						   BEAMFS_RS_EVENT_FLAG_INODE);
			}
			mark_buffer_dirty(bh);
			pr_warn("beamfs: inode %lu corrected by RS FEC (%d symbols)\n",
				ino, nerr);
		}
	}

	fi = BEAMFS_I(inode);

	/* Populate VFS inode */
	inode->i_mode  = le16_to_cpu(raw->i_mode);
	inode->i_uid   = make_kuid(sb->s_user_ns, le32_to_cpu(raw->i_uid));
	inode->i_gid   = make_kgid(sb->s_user_ns, le32_to_cpu(raw->i_gid));
	set_nlink(inode, le16_to_cpu(raw->i_nlink));
	inode->i_size  = le64_to_cpu(raw->i_size);

	/*
	 * Times before 1970 are negative, and there is nothing exotic
	 * about them: tar preserves them, build systems set them, and a
	 * file dated 1960 is a file. le64_to_cpu returns unsigned, so
	 * dividing it turned -315M seconds into 1.8e19 -- generic/258
	 * reports it as "Timestamp wrapped".
	 *
	 * div_s64_rem rather than / and %, because C truncates toward
	 * zero: -1.5e9 ns would give -1 second and -5e8 nanoseconds, and
	 * a negative nanosecond field is not a time the VFS can use. The
	 * floor form gives -2 seconds and +5e8 nanoseconds, which is the
	 * same instant expressed the way timespec64 requires.
	 */
	beamfs_set_time_from_ns(inode, inode_set_atime,
				(s64)le64_to_cpu(raw->i_atime));
	beamfs_set_time_from_ns(inode, inode_set_mtime,
				(s64)le64_to_cpu(raw->i_mtime));
	beamfs_set_time_from_ns(inode, inode_set_ctime,
				(s64)le64_to_cpu(raw->i_ctime));

	/* Copy block pointers to in-memory inode */
	memcpy(fi->i_direct, raw->i_direct, sizeof(fi->i_direct));
	fi->i_indirect  = raw->i_indirect;
	fi->i_dindirect = raw->i_dindirect;
	fi->i_tindirect = raw->i_tindirect;
	fi->i_flags     = le32_to_cpu(raw->i_flags);

	/* Layer 1 defense: mark reserved inodes (canary) as immutable.
	 * VFS rejects open(O_TRUNC), setattr(ATTR_SIZE), unlink, chmod
	 * with EPERM before reaching beamfs hooks. Layer 2 in alloc.c
	 * provides the safety net if VFS check is bypassed (corruption).
	 */
	if (beamfs_ino_is_reserved((u64)ino) && ino != BEAMFS_RESERVED_INO_ROOT)
		inode->i_flags |= S_IMMUTABLE;

	/* Set ops based on file type */
	if (S_ISDIR(inode->i_mode)) {
		inode->i_op  = &beamfs_dir_inode_operations;
		inode->i_fop = &beamfs_dir_operations;
	} else if (S_ISREG(inode->i_mode)) {
		if (sbi->s_scheme == BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE) {
			/*
			 * v2 INLINE: per-block RS FEC requires gather/scatter
			 * across 16 subblocks per disk block. Single-page
			 * folios only; large folios deferred to v2.1.
			 * Dedicated i_op carries setattr/truncate (sub-step 6).
			 */
			inode->i_op = &beamfs_inline_inode_operations;
			inode->i_fop = &beamfs_inline_file_operations;
			inode->i_mapping->a_ops = &beamfs_inline_aops;
			mapping_set_folio_order_range(inode->i_mapping, 0, 0);
		} else {
			/* legacy iomap path (scheme=5 INODE_UNIVERSAL) */
			inode->i_op = &beamfs_file_inode_operations;
			inode->i_fop = &beamfs_file_operations;
			inode->i_mapping->a_ops = &beamfs_aops;
		}
	} else if (S_ISLNK(inode->i_mode)) {
		/*
		 * Symlink: a target under 96 bytes is inline in i_direct[],
		 * a longer one in the data block i_direct[0] names.
		 */
		inode->i_op = &beamfs_symlink_inode_operations;
	} else {
		/* Special files: use generic */
		init_special_inode(inode, inode->i_mode, 0);
	}

	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}

/*
 * beamfs_getattr -- stat for every beamfs inode.
 *
 * st_blocks is the count of blocks the inode holds on the medium, data
 * and indirect alike. The inode does not store it, so it is taken from
 * the tree the first time it is asked for, and beamfs_alloc_block and
 * beamfs_free_block keep it from there. Regular files and symlinks used
 * simple_getattr, which counts the pages in the page cache: 0 for any
 * file after a mount.
 *
 * The count is taken under the allocation mutex alone, which every path
 * that installs or frees a block of the inode holds, a directory's
 * included. Not the inode lock: ->getattr is called without it, and a
 * caller of vfs_getattr that holds it would wait on itself here.
 *
 * A count stopped by a block of pointers that failed its check is not
 * kept, and is taken again on the next call; the block was reported
 * where it failed. Until a count is kept, the mask statx returns leaves
 * STATX_BLOCKS out rather than vouch for the number.
 */
int beamfs_getattr(const struct mnt_idmap *idmap, const struct path *path,
		   struct kstat *stat, u32 request_mask,
		   unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	bool counted;

	/* Pairs with the release below: the count is in before the flag. */
	counted = smp_load_acquire(&fi->i_blocks_counted);
	if ((request_mask & STATX_BLOCKS) && !counted) {
		mutex_lock(&fi->i_alloc_mutex);
		counted = fi->i_blocks_counted;
		if (!counted) {
			u64 n = 0;
			int err = beamfs_count_blocks(inode, &n);

			inode_set_bytes(inode, (loff_t)n * BEAMFS_BLOCK_SIZE);
			if (!err) {
				/* Pairs with the acquire above. */
				smp_store_release(&fi->i_blocks_counted, true);
				counted = true;
			}
		}
		mutex_unlock(&fi->i_alloc_mutex);
	}
	generic_fillattr(idmap, request_mask, inode, stat);
	if (!counted)
		stat->result_mask &= ~STATX_BLOCKS;
	return 0;
}
