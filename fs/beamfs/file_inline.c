// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - File operations for BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE
 *
 * Per-block Reed-Solomon FEC on user data: each 4096-byte disk block
 * carries 16 shortened RS(255,239) codewords, in one of two layouts.
 *
 * With BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE (the capsule), 3808 data
 * bytes, an 8-byte checksum and an 8-byte self-identity make 3824 coded
 * bytes, interleaved across the 16 codewords. 256 parity bytes follow,
 * 16 per codeword, then 16 bytes outside the codewords: an 8-byte
 * generation and 8 reserved bytes.
 *
 * Without it (alternating), each codeword is 239 data bytes followed by
 * its 16 parity bytes, 16 x 255 = 4080 bytes, and the last 16 bytes of
 * the block, outside every codeword, hold the checksum and the
 * self-identity.
 *
 * This file implements the data path for scheme=2 (UNIVERSAL_INLINE).
 * The legacy iomap-based path in file.c is preserved for scheme=5
 * (INODE_UNIVERSAL); the dispatch happens in inode.c / namei.c when
 * setting i_fop and a_ops based on sbi->s_scheme.
 *
 * Each codeword corrects up to 8 corrupted bytes, so a block survives up
 * to 128 when they fall 8 to a codeword. Interleaving puts consecutive
 * coded bytes in different codewords, so a burst within the coded bytes
 * is shared among the 16 codewords instead of landing in one.
 *
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/writeback.h>
#include <linux/crc32.h>
#include <linux/unaligned.h>
#include "beamfs.h"
#include "beamfs_trace.h"
#include <linux/iomap.h>
#include <linux/refcount.h>

/* ------------------------------------------------------------------------- */
/* Forward declarations                                                      */
/* ------------------------------------------------------------------------- */

static int     beamfs_inline_read_folio(struct file *file,
					struct folio *folio);
static int     beamfs_inline_writepages(struct address_space *mapping,
					struct writeback_control *wbc);
static ssize_t beamfs_inline_file_write_iter(struct kiocb *iocb,
					     struct iov_iter *from);
static int     beamfs_inline_setattr(const struct mnt_idmap *idmap,
				     struct dentry *dentry,
				    struct iattr *attr);
static void    beamfs_inline_free_blocks_from(struct inode *inode,
					      u64 b_first_freed);
static void    beamfs_inline_stamp_tail_pad(struct beamfs_sb_info *sbi,
					    u8 *block, const u8 *payload,
					    u64 ino, u64 iblock);
static int     beamfs_inline_zero_tail_block(struct inode *inode,
					     u64 b, u32 zero_offset);
static int     beamfs_check_intermediate_block(struct super_block *sb,
					       u64 block, ino_t ino,
					       u64 iblock_logical,
					       const char *label);

/* ------------------------------------------------------------------------- */
/* Block-mapping helpers                                                     */
/*                                                                           */
/* These mirror the layout used by beamfs_iomap_begin() in file.c (legacy    */
/* iomap path, scheme=5 INODE_UNIVERSAL): direct blocks 0..11 in             */
/* fi->i_direct[], single indirect via fi->i_indirect (512 entries) for      */
/* iblocks 12..523. Beyond those, fi->i_dindirect and fi->i_tindirect map    */
/* double and triple indirect trees, up to BEAMFS_MAX_IBLOCK_TINDIRECT.      */
/*                                                                           */
/* Pure lookup, no allocation. Used by the read paths, by iomap_begin for    */
/* anything but a write, by the scrubber, and by writeback and               */
/* zero_tail_block to log a pointer the allocating variant changed. The      */
/* allocating variant lives below.                                           */
/*                                                                           */
/* Returns:                                                                  */
/*   0  + *phys_out = block number  (mapped)                                 */
/*   0  + *phys_out = 0             (HOLE: not allocated yet)                */
/*   <0 on error                    (-EIO indirect read fail,                */
/*                                   -EUCLEAN corrupted or unallocated       */
/*                                    pointer, or indirect parity failure,   */
/*                                   -EOPNOTSUPP beyond triple indirect,     */
/*                                   -EINVAL on null phys_out)               */
/* ------------------------------------------------------------------------- */
/*
 * beamfs_check_intermediate_block -- bounds and allocation check for a
 * pointer-to-pointers block (indirect, dindirect, tindirect and their L1/L2
 * levels) before it is read as 512 raw __le64 entries.
 *
 * beamfs_inline_lookup_phys checks every terminal pointer -- the one that
 * addresses a leaf data block -- against both the
 * [s_data_start, s_data_start + s_nblocks) range and the allocation bitmap.
 * The blocks that hold indirection pointers need the same two checks: a
 * bit flip on fi->i_indirect (or i_dindirect, i_tindirect, or an L1/L2
 * entry) reaching a value inside the valid range but pointing at an
 * unrelated or unallocated block would otherwise be read via sb_bread and
 * its raw bytes reinterpreted as 512 pointers, with no signal. Fail closed
 * before the read rather than trust whatever the corrupted pointer happens
 * to reach, as DATA_SELFID and beamfs_block_is_allocated do for file data
 * and terminal pointers.
 */
static int beamfs_check_intermediate_block(struct super_block *sb,
					   u64 block, ino_t ino,
					   u64 iblock_logical,
					   const char *label)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (block < sbi->s_data_start ||
	    block >= sbi->s_data_start + sbi->s_nblocks) {
		pr_err_ratelimited("beamfs/inline: corrupted %s block ino=%llu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
				   label, (unsigned long long)ino,
				   (unsigned long long)iblock_logical,
				   (unsigned long long)block,
				   sbi->s_data_start,
				   sbi->s_data_start + sbi->s_nblocks);
		beamfs_log_rs_event_flagged(sb, block, NULL, 0,
					    BEAMFS_SUBBLOCK_DATA, 0);
		return -EUCLEAN;
	}
	if (!beamfs_block_is_allocated(sb, block)) {
		/*
		 * Journal it as uncorrectable, same as the DATA_CSUM and
		 * DATA_SELFID fail-closed paths. An indirection pointer
		 * corrupted beyond the RS correction radius is exactly what
		 * the forensic journal exists to record; without an entry
		 * the run reads as an unexplained failure, since dmesg
		 * carries the pr_err but DMESG_UNCORRECTABLE stays 0. That
		 * is how the 2026-08-27 multifs run scored RS_FAILED rather
		 * than RS_FAIL_CLOSED. positions == NULL with
		 * n_positions == 0 is the uncorrectable call shape; the flag
		 * is reserved and set by beamfs_log_rs_event_flagged itself;
		 * passing it in extra_flags would trip the reserved-mask
		 * guard.
		 */
		beamfs_log_rs_event_flagged(sb, block, NULL, 0,
					    BEAMFS_SUBBLOCK_DATA, 0);
		pr_err_ratelimited("beamfs/inline: unallocated %s block ino=%llu iblock=%llu phys=%llu\n",
				   label, (unsigned long long)ino,
				   (unsigned long long)iblock_logical,
				   (unsigned long long)block);
		return -EUCLEAN;
	}
	return 0;
}

/*
 * Count a block of pointers and what it points to, @depth levels down:
 * 1 for a block of pointers to data, 2 and 3 for the double and triple
 * trees. The pointers are copied out under the buffer lock, as the
 * allocator installs them under it, and the children walked after.
 */
static int beamfs_count_tree(struct inode *inode, u64 blk,
			     unsigned int depth, u64 *count)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	__le64 *ptrs;
	u64 *kids = NULL;
	unsigned int i, n = 0;
	int ret;

	ret = beamfs_check_intermediate_block(sb, blk, inode->i_ino, 0,
					      "indirect");
	if (ret)
		return ret;
	bh = sb_bread(sb, blk);
	if (!bh)
		return -EIO;
	if (beamfs_ind_parity_verify(sb, bh)) {
		brelse(bh);
		return -EUCLEAN;
	}
	if (depth > 1) {
		kids = kmalloc_array(BEAMFS_INDIRECT_PTRS, sizeof(*kids),
				     GFP_NOFS);
		if (!kids) {
			brelse(bh);
			return -ENOMEM;
		}
	}

	(*count)++;
	lock_buffer(bh);
	ptrs = (__le64 *)bh->b_data;
	for (i = 0; i < BEAMFS_INDIRECT_PTRS; i++) {
		u64 p = le64_to_cpu(ptrs[i]);

		if (!p)
			continue;
		if (kids)
			kids[n++] = p;
		else
			(*count)++;
	}
	unlock_buffer(bh);
	brelse(bh);

	for (i = 0; i < n && !ret; i++)
		ret = beamfs_count_tree(inode, kids[i], depth - 1, count);
	kfree(kids);
	return ret;
}

/*
 * beamfs_count_blocks -- the blocks @inode holds on the medium, data and
 * indirect alike, read from its tree. The inode does not store the
 * count; beamfs_getattr takes it from here once. A short symlink target
 * lives in i_direct[] as bytes, not pointers, and a long one is a block.
 *
 * Returns 0, or the error of a block of pointers that failed its check;
 * *count then holds what was counted before it.
 */
int beamfs_count_blocks(struct inode *inode, u64 *count)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	u64 roots[3];
	unsigned int i;
	int ret = 0;

	*count = 0;
	if (S_ISLNK(inode->i_mode)) {
		if (i_size_read(inode) >= (loff_t)sizeof(fi->i_direct) &&
		    fi->i_direct[0])
			*count = 1;
		return 0;
	}
	if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode))
		return 0;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
		if (fi->i_direct[i])
			(*count)++;
	}
	roots[0] = le64_to_cpu(fi->i_indirect);
	roots[1] = le64_to_cpu(fi->i_dindirect);
	roots[2] = le64_to_cpu(fi->i_tindirect);
	for (i = 0; i < ARRAY_SIZE(roots) && !ret; i++) {
		if (roots[i])
			ret = beamfs_count_tree(inode, roots[i], i + 1, count);
	}
	return ret;
}

int beamfs_inline_lookup_phys(struct inode *inode, u64 iblock_logical,
			      u64 *phys_out)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	struct buffer_head *ibh = NULL;
	__le64                   *ptrs;
	u64                       indirect_blk;
	u64                       indirect_slot;
	u64                       phys;
	int                       ret;

	if (!phys_out)
		return -EINVAL;

	*phys_out = 0;

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS) {
		u64 dphys = le64_to_cpu(fi->i_direct[iblock_logical]);

		if (dphys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (dphys < sbi->s_data_start ||
			    dphys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted direct pointer ino=%llu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)dphys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
			/*
			 * A direct pointer into a freed block fails closed
			 * here, as the same case on an indirect pointer does
			 * in beamfs_check_intermediate_block.
			 */
			if (!beamfs_block_is_allocated(sb, dphys)) {
				pr_err_ratelimited("beamfs/inline: unallocated direct pointer ino=%llu iblock=%llu phys=%llu\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)dphys);
				return -EUCLEAN;
			}
		}
		*phys_out = dphys;
		return 0;
	}

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS) {
		indirect_slot = iblock_logical - BEAMFS_DIRECT_BLOCKS;
		indirect_blk  = le64_to_cpu(fi->i_indirect);

		if (!indirect_blk)
			return 0; /* HOLE: indirect block not yet allocated */

		ret = beamfs_check_intermediate_block(sb, indirect_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "indirect");
		if (ret)
			return ret;

		{
			int fresh = 0;

			if (!ibh) {
				/*
				 * find_get_block, not sb_getblk: the
				 * question is whether the cache already
				 * holds this block, and sb_getblk would
				 * answer it by creating one. A probe that
				 * changes what it measures is worse than
				 * no probe -- generic/464 passed with one
				 * and the result could not be read.
				 */
				struct buffer_head *probe =
					sb_find_get_block(sb, indirect_blk);

				fresh = !probe;
				if (probe)
					brelse(probe);
				ibh = sb_bread(sb, indirect_blk);
			}
			if (!ibh) {
				pr_err_ratelimited("beamfs/inline: failed to read indirect block %llu\n",
						   (unsigned long long)indirect_blk);
				return -EIO;
			}
			trace_beamfs_ind_read(inode->i_sb->s_dev, inode->i_ino, indirect_blk,
					      (unsigned int)indirect_slot,
				le64_to_cpu(((__le64 *)ibh->b_data)[indirect_slot]),
				buffer_uptodate(ibh), fresh);
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		/*
		 * Read under the buffer lock, as the writer installs
		 * under it.
		 *
		 * lookup_phys took no lock at all while
		 * lookup_or_alloc_phys installs a pointer holding
		 * both i_alloc_mutex and lock_buffer, so a reader
		 * could see a slot mid-install: treecheck reported
		 * one that held 18806 and read as 0, in the same
		 * function for the same inode, with the write order
		 * forced -- so the block was on the medium and the
		 * read was simply early.
		 */
		lock_buffer(ibh);
		phys = le64_to_cpu(ptrs[indirect_slot]);
		unlock_buffer(ibh);
		brelse(ibh);

		if (phys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (phys < sbi->s_data_start ||
			    phys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted indirect pointer ino=%llu iblock=%llu slot=%llu phys=%llu (out of [%lu, %lu))\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)indirect_slot,
						   (unsigned long long)phys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
			if (!beamfs_block_is_allocated(sb, phys)) {
				pr_err_ratelimited("beamfs/inline: unallocated indirect pointer ino=%llu iblock=%llu phys=%llu\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys);
				return -EUCLEAN;
			}
		}
		*phys_out = phys;
		return 0;
	}

	if (iblock_logical < BEAMFS_MAX_IBLOCK_DINDIRECT) {
		/*
		 * Double-indirect lookup. Map iblock to (level-1 slot,
		 * level-2 slot) where level-1 is an indirect block of
		 * pointers to indirect blocks, and level-2 is the leaf
		 * indirect block whose entries point to user data blocks.
		 * Coverage: 12 + 512 + 512*512 = 262668 iblocks ~= 1 GiB.
		 */
		u64 didx, l1_slot, l2_slot, dindirect_blk, l1_blk;
		struct buffer_head *l1bh = NULL;

		didx = iblock_logical - BEAMFS_MAX_IBLOCK_INDIRECT;
		l1_slot = didx / BEAMFS_INDIRECT_PTRS;
		l2_slot = didx % BEAMFS_INDIRECT_PTRS;

		dindirect_blk = le64_to_cpu(fi->i_dindirect);
		if (!dindirect_blk)
			return 0; /* HOLE: dindirect block not yet allocated */

		ret = beamfs_check_intermediate_block(sb, dindirect_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "dindirect");
		if (ret)
			return ret;

		ibh = sb_bread(sb, dindirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read dindirect block %llu\n",
					   (unsigned long long)dindirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		/*
		 * Read under the buffer lock, as the writer installs
		 * under it. See the single-indirect case above.
		 */
		lock_buffer(ibh);
		l1_blk = le64_to_cpu(ptrs[l1_slot]);
		unlock_buffer(ibh);
		brelse(ibh);

		if (!l1_blk)
			return 0; /* HOLE: level-1 indirect not allocated */

		ret = beamfs_check_intermediate_block(sb, l1_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "dindirect L1");
		if (ret)
			return ret;

		if (!l1bh)
			l1bh = sb_bread(sb, l1_blk);
		if (!l1bh) {
			pr_err_ratelimited("beamfs/inline: failed to read dindirect L1 block %llu\n",
					   (unsigned long long)l1_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, l1bh)) {
			brelse(l1bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l1bh->b_data;
		/*
		 * Read under the buffer lock, as the writer installs
		 * under it.
		 *
		 * lookup_phys took no lock at all while
		 * lookup_or_alloc_phys installs a pointer holding
		 * both i_alloc_mutex and lock_buffer, so a reader
		 * could see a slot mid-install: treecheck reported
		 * one that held 18806 and read as 0, in the same
		 * function for the same inode, with the write order
		 * forced -- so the block was on the medium and the
		 * read was simply early.
		 */
		lock_buffer(l1bh);
		phys = le64_to_cpu(ptrs[l2_slot]);
		unlock_buffer(l1bh);
		brelse(l1bh);

		if (phys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (phys < sbi->s_data_start ||
			    phys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted dindirect pointer ino=%llu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
			if (!beamfs_block_is_allocated(sb, phys)) {
				pr_err_ratelimited("beamfs/inline: unallocated dindirect pointer ino=%llu iblock=%llu phys=%llu\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys);
				return -EUCLEAN;
			}
		}
		*phys_out = phys;
		return 0;
	}

	if (iblock_logical < BEAMFS_MAX_IBLOCK_TINDIRECT) {
		/*
		 * Triple-indirect lookup. Map iblock to (level-1, level-2,
		 * level-3) where level-1 is an indirect block of pointers to
		 * dindirect blocks, level-2 is an indirect block of pointers
		 * to indirect blocks, and level-3 is the leaf indirect block
		 * whose entries point to user data blocks.
		 * Coverage: 12 + 512 + 512^2 + 512^3 = 134480396 iblocks
		 * ~= 478 GiB at 3824 user bytes/block.
		 */
		u64 tidx, l1_slot, l2_slot, l3_slot;
		u64 tindirect_blk, l1_blk, l2_blk;
		struct buffer_head *l1bh = NULL, *l2bh = NULL;

		tidx = iblock_logical - BEAMFS_MAX_IBLOCK_DINDIRECT;
		l1_slot = tidx / (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS);
		l2_slot = (tidx / BEAMFS_INDIRECT_PTRS) % BEAMFS_INDIRECT_PTRS;
		l3_slot = tidx % BEAMFS_INDIRECT_PTRS;

		tindirect_blk = le64_to_cpu(fi->i_tindirect);
		if (!tindirect_blk)
			return 0; /* HOLE: tindirect block not yet allocated */

		ret = beamfs_check_intermediate_block(sb, tindirect_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "tindirect");
		if (ret)
			return ret;

		ibh = sb_bread(sb, tindirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect block %llu\n",
					   (unsigned long long)tindirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		/*
		 * Read under the buffer lock, as the writer installs
		 * under it. See the single-indirect case above.
		 */
		lock_buffer(ibh);
		l1_blk = le64_to_cpu(ptrs[l1_slot]);
		unlock_buffer(ibh);
		brelse(ibh);

		if (!l1_blk)
			return 0; /* HOLE: level-1 not allocated */

		ret = beamfs_check_intermediate_block(sb, l1_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "tindirect L1");
		if (ret)
			return ret;

		if (!l1bh)
			l1bh = sb_bread(sb, l1_blk);
		if (!l1bh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect L1 block %llu\n",
					   (unsigned long long)l1_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, l1bh)) {
			brelse(l1bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l1bh->b_data;
		/*
		 * Read under the buffer lock, as the writer installs
		 * under it. See the single-indirect case above.
		 */
		lock_buffer(l1bh);
		l2_blk = le64_to_cpu(ptrs[l2_slot]);
		unlock_buffer(l1bh);
		brelse(l1bh);

		if (!l2_blk)
			return 0; /* HOLE: level-2 not allocated */

		ret = beamfs_check_intermediate_block(sb, l2_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "tindirect L2");
		if (ret)
			return ret;

		if (!l2bh)
			l2bh = sb_bread(sb, l2_blk);
		if (!l2bh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect L2 block %llu\n",
					   (unsigned long long)l2_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, l2bh)) {
			brelse(l2bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l2bh->b_data;
		/*
		 * Read under the buffer lock, as the writer installs
		 * under it. See the single-indirect case above.
		 */
		lock_buffer(l2bh);
		phys = le64_to_cpu(ptrs[l3_slot]);
		unlock_buffer(l2bh);
		brelse(l2bh);

		if (phys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (phys < sbi->s_data_start ||
			    phys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted tindirect pointer ino=%llu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
			if (!beamfs_block_is_allocated(sb, phys)) {
				pr_err_ratelimited("beamfs/inline: unallocated tindirect pointer ino=%llu iblock=%llu phys=%llu\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys);
				return -EUCLEAN;
			}
		}
		*phys_out = phys;
		return 0;
	}

	pr_err_ratelimited("beamfs/inline: iblock %llu beyond tindirect capacity\n",
			   (unsigned long long)iblock_logical);
	return -EOPNOTSUPP;
}

/* ------------------------------------------------------------------------- */
/* Allocating block-mapping (write path)                                     */
/*                                                                           */
/* Variant of beamfs_inline_lookup_phys() that allocates on demand. Used by  */
/* iomap_begin for a write, by writeback_range and by zero_tail_block to     */
/* map a logical iblock to a physical block, allocating direct, indirect,    */
/* double and triple indirect and data blocks as needed. A freshly           */
/* allocated data block is zeroed, stamped and RS-encoded at once, so a      */
/* subsequent read finds a valid block of zeros rather than whatever the     */
/* medium held.                                                              */
/*                                                                           */
/* Allocates:                                                                */
/*   - Direct: writes phys into fi->i_direct[iblock] + mark_inode_dirty.     */
/*   - Indirect levels: allocates each missing level first (zero-init),      */
/*     then the data block, and writes phys into the leaf ptrs[slot].        */
/*                                                                           */
/* The freshly allocated data block is built via sb_getblk + memset, NOT     */
/* via sb_bread, because there is no on-disk content to read (the block      */
/* was unallocated). The buffer is marked uptodate + dirty, so a partial     */
/* write at writeback finds it in the buffer cache instead of reading the    */
/* device.                                                                   */
/*                                                                           */
/* Returns:                                                                  */
/*   0  + *phys_out = block number (mapped or freshly allocated)             */
/*   <0 on error (-ENOSPC if alloc fails, -EIO on indirect read fail, on     */
/*                a failed volume or an inode being evicted, -EUCLEAN on     */
/*                indirect parity failure, -EOPNOTSUPP beyond triple         */
/*                indirect, -EINVAL on null phys_out)                        */
/* ------------------------------------------------------------------------- */
/*
 * Is this a block a file may point at?
 *
 * Everything below s_data_start is metadata: the superblock, the inode
 * table, the bitmap, the indirect parity region. A file's data never
 * lives there, and a pointer naming it is wrong whatever produced it.
 *
 * generic/075 found two blocks of the inode table holding fsx's fill
 * bytes, seven inodes in them beyond correction and 40437 pointers
 * naming blocks past the end of the device -- and the first thing to
 * notice was fsck, three minutes and one unmount later, by which time
 * the path that did it was gone.
 *
 * Refused here instead, with the block and the inode named, while the
 * stack that produced it is still on the stack.
 */
static bool beamfs_phys_is_sane(struct super_block *sb, struct inode *inode,
				u64 phys, const char *where)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u64 last = (u64)sbi->s_data_start + sbi->s_nblocks;

	if (phys == 0)
		return true;			/* a hole, not an address */

	if (phys < sbi->s_data_start || phys >= last) {
		/*
		 * The inode number in its own variable: a ternary between
		 * i_ino and a literal is promoted, and %lu given the
		 * result compiles clean where the promotion happens to
		 * land on unsigned long and breaks where it does not.
		 */
		unsigned long ino = inode ? inode->i_ino : 0;

		pr_err_ratelimited("beamfs: %s: inode %lu names block %llu, outside %llu..%llu\n",
				   where, ino,
				   (unsigned long long)phys,
				   (unsigned long long)sbi->s_data_start,
				   (unsigned long long)last - 1);
		beamfs_fail(sb, where, -EUCLEAN);
		return false;
	}
	return true;
}

#ifdef CONFIG_BEAMFS_ORDERED_META
/*
 * Put a metadata block on the medium before anything names it.
 *
 * The safe order for a tree is pointee before pointer, always, and
 * nothing in beamfs establishes it: mark_buffer_dirty hands the buffer
 * to writeback, which sorts by age and by position.
 *
 * This is the brute form -- one synchronous write per level, up to
 * four for a block reached through triple indirection -- and it is
 * meant to answer one question: is the missing order the reason nine
 * xfstests fail? A journal is how a filesystem gets the same property
 * without paying this.
 */
static void beamfs_order_before_pointer(struct buffer_head *bh)
{
	if (!bh)
		return;
	/*
	 * write_dirty_buffer submits and returns; the wait is what makes
	 * this an ordering and not a hint.
	 */
	write_dirty_buffer(bh, REQ_SYNC);
	wait_on_buffer(bh);
}
#else
static inline void beamfs_order_before_pointer(struct buffer_head *bh)
{
	(void)bh;
}
#endif

/*
 * Did this call allocate, or did it find what was already there?
 *
 * iomap needs the answer. A write shorter than the mapping it was
 * given leaves the rest of that mapping allocated, and only the blocks
 * this call created may be given back -- a block that was already
 * there belongs to the file whether or not this write reached it.
 *
 * generic/013 leaves 28 consecutive blocks allocated and never
 * written, the length of one write, and nothing can tell them from
 * blocks the file already owned.
 *
 * @allocated may be NULL for callers that do not care.
 */
static int beamfs_inline_lookup_or_alloc_phys_new(struct inode *inode,
						  u64 iblock_logical,
						  u64 *phys_out,
						  bool *allocated);

static int beamfs_inline_lookup_or_alloc_phys(struct inode *inode,
					      u64 iblock_logical,
					      u64 *phys_out)
{
	return beamfs_inline_lookup_or_alloc_phys_new(inode, iblock_logical,
						      phys_out, NULL);
}

/*
 * Lay a block's payload down in whichever layout the volume uses.
 *
 * Alternating, the payload is sixteen runs of 239 bytes 255 apart, and
 * the copy has to break it up. A capsule holds it contiguously, and
 * breaking it up writes every run 16 bytes further along than it
 * belongs -- measured as the last 222 bytes of every block coming back
 * wrong, which is where the fifteenth run ends up.
 *
 * Paired with beamfs_seal_data_block, which encodes what this writes.
 */
void beamfs_lay_data_payload(struct super_block *sb, u8 *block,
			     const u8 *payload)
{
	unsigned int i;

	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE) {
		memcpy(block + BEAMFS_CAPSULE_DATA_OFF, payload,
		       BEAMFS_CAPSULE_DATA_BYTES);
		return;
	}

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		memcpy(block + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       payload + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
}

/*
 * Seal a data block: compute its RS parity in whichever layout the
 * volume uses. Every site that encodes a data block goes through here,
 * so a layout change touches one place.
 */
int beamfs_seal_data_block(struct super_block *sb, u8 *block)
{
	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE)
		return beamfs_rs_encode_woven(block + BEAMFS_CAPSULE_DATA_OFF,
					      block + BEAMFS_CAPSULE_PARITY_OFF,
					      BEAMFS_RS_PARITY,
					      BEAMFS_SUBBLOCK_DATA,
					      BEAMFS_DATA_INLINE_SUBBLOCKS);

	return beamfs_rs_encode_region(block, BEAMFS_SUBBLOCK_TOTAL,
				       block + BEAMFS_SUBBLOCK_DATA,
				       BEAMFS_SUBBLOCK_TOTAL,
				       BEAMFS_SUBBLOCK_DATA,
				       BEAMFS_DATA_INLINE_SUBBLOCKS);
}

/*
 * Dirty an indirect block and attach it to @inode, unless its last
 * write failed.
 *
 * An install reads the parent block, allocates the child -- which can
 * sleep -- and only then stores the pointer and dirties the parent. A
 * write of the parent that fails in between leaves it not uptodate
 * (end_buffer_write_sync clears the bit on an error): the medium holds
 * an older block than memory does, and dirtying it anyway is what
 * mark_buffer_dirty warns about. The volume is failed instead, as
 * beamfs_write_bitmap_block does for a bitmap block.
 *
 * Tested under the buffer lock: a write in flight holds it until its
 * completion has set or cleared uptodate.
 */
static void beamfs_attach_dirty(struct inode *inode, struct buffer_head *bh)
{
	lock_buffer(bh);
	if (!buffer_uptodate(bh)) {
		unlock_buffer(bh);
		pr_err_ratelimited("beamfs: block %llu: its last write failed, not dirtied again\n",
				   (unsigned long long)bh->b_blocknr);
		beamfs_fail(inode->i_sb, "indirect block write", -EIO);
		return;
	}
	mmb_mark_buffer_dirty(bh, &BEAMFS_I(inode)->i_metadata_bhs);
	unlock_buffer(bh);
}

static int beamfs_inline_lookup_or_alloc_phys_new(struct inode *inode,
						  u64 iblock_logical,
						  u64 *phys_out,
						  bool *allocated)
{
	/*
	 * False until proven otherwise: a caller reading this after an
	 * error path must not find a stale true.
	 */
	if (allocated)
		*allocated = false;

	/*
	 * Nothing to allocate for an inode being destroyed.
	 *
	 * __mark_inode_dirty returns without queueing when I_FREEING is
	 * set -- fs-writeback.c, "if (inode_state_read(inode) &
	 * I_FREEING) goto out_unlock". A block allocated here has its
	 * pointer installed into a tree whose inode will never be
	 * written: the bitmap says used, nothing references it, and fsck
	 * calls it used-but-unreferenced.
	 *
	 * The check belongs here rather than in the callers, because all
	 * three of them -- iomap_begin, writeback_range, zero_tail_block
	 * -- can run against an evicting inode, and guarding one of the
	 * three leaves the other two leaking.
	 *
	 * ext2 cannot reach this: ext2_get_block runs with create=0 on
	 * the writeback path, and an evicting inode has been through
	 * truncate_inode_pages_final. beamfs allocates at writeback and
	 * has to check for itself.
	 */
	if (inode_state_read_once(inode) &
	    (I_FREEING | I_WILL_FREE | I_CLEAR))
		return -EIO;

	/*
	 * Nothing free: say so once and return, rather than walking the
	 * whole indirection tree to discover it at every level.
	 *
	 * Without this a full volume produced one failed allocation per
	 * block per level -- 19531 suppressed log callbacks in three
	 * seconds under generic/224 -- and the writeback path spent its
	 * time in the allocator and the ring buffer instead of returning
	 * ENOSPC. The test then exceeded its timeout for want of an error
	 * the filesystem already knew.
	 *
	 * s_free_blocks is read without the lock. It is a hint here: a
	 * concurrent free racing this check costs one retry, and the
	 * allocation below is still the thing that decides.
	 */
	if (BEAMFS_SB(inode->i_sb)->s_free_blocks == 0)
		return -ENOSPC;

	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	struct buffer_head *ibh = NULL;
	struct buffer_head       *dbh;
	__le64                   *ptrs;
	u64                       indirect_blk;
	u64                       indirect_slot;
	u64                       phys;
	u64                       new_block;

	if (!phys_out)
		return -EINVAL;

	*phys_out = 0;

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS) {
		/* --- Direct block --- */
		phys = le64_to_cpu(fi->i_direct[iblock_logical]);
		if (phys) {
			*phys_out = phys;
			return 0;
		}
		new_block = beamfs_alloc_block(sb, inode);
		if (!new_block) {
			if (beamfs_failed(sb))
				return -EIO;
			return -ENOSPC;
		}
		/* Zero-init the freshly allocated data block on disk. */
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block, inode);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		/*
		 * Stamp the DATA_CSUM descriptor on the freshly allocated data
		 * block. An all-zero block is a valid RS codeword by linearity,
		 * so decode accepts it, but the descriptor would stay 0x00 and
		 * the read path would have nothing to verify against. Before
		 * the type byte was made fail-closed that meant silent
		 * acceptance; afterwards it means a legitimate newly allocated
		 * block is rejected. Observed 2026-08-18: iblocks 0..5 of a
		 * test file logged "bad descriptor type=0x00" on five runs out
		 * of six, because allocation writes the block here while the
		 * only other stamp sites are writeback and
		 * zero_tail_block, neither of which runs for a block that is
		 * allocated but not yet written through the folio path. The
		 * payload is the zeroed block itself. Indirect blocks carry no
		 * DATA_CSUM descriptor and are deliberately left untouched.
		 */
		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)dbh->b_data,
					     (const u8 *)dbh->b_data,
					     (unsigned long long)inode->i_ino, iblock_logical);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		/*
		 * A data block carries sixteen RS codewords, not indirect
		 * parity.
		 *
		 * ind_parity_update files a CRC for this block number in
		 * the separate parity region that covers indirect blocks.
		 * A newly allocated data block was being registered there,
		 * and its own sixteen codewords were left as the zeros
		 * memset put down -- correct for a zero block, wrong the
		 * moment anything is written into it and the parity is not
		 * recomputed.
		 *
		 * The sweep found exactly that: subblocks holding data,
		 * uncorrectable; subblocks still zero, fine. Encoding here
		 * makes a fresh block valid on its own terms from the
		 * start.
		 */
		{
			int _e = beamfs_seal_data_block(sb, (u8 *)dbh->b_data);

			if (_e < 0)
				pr_err_ratelimited("beamfs/inline: encode of new data block %llu failed: %d\n",
						   (unsigned long long)new_block,
						   _e);
		}
		mark_buffer_dirty(dbh);

		/*
		 * The direct array is a slot store like any other.
		 *
		 * The three indirect levels emit this and the direct array
		 * did not, so a probe pairing allocations against stores
		 * counted every direct block as an allocation that went
		 * nowhere: 176180 of them in one generic/269, against 327
		 * blocks fsck actually finds leaked.
		 *
		 * parent 0 and level 0: there is no block above a direct
		 * pointer, and nothing else uses those values.
		 */
		trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, 0, iblock_logical,
					le64_to_cpu(fi->i_direct[iblock_logical]),
					new_block, 0);

		/* The block, before the pointer that names it. */
		beamfs_order_before_pointer(dbh);
		brelse(dbh);
		fi->i_direct[iblock_logical] = cpu_to_le64(new_block);
		mark_inode_dirty(inode);

		if (allocated)
			*allocated = true;
		*phys_out = new_block;
		return 0;
	}

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS) {
		/* --- Single indirect block --- */
		indirect_slot = iblock_logical - BEAMFS_DIRECT_BLOCKS;
		indirect_blk  = le64_to_cpu(fi->i_indirect);

		if (!indirect_blk) {
			/* Allocate indirect block first, zero-init. */
			indirect_blk = beamfs_alloc_block(sb, inode);
			if (!indirect_blk) {
				if (beamfs_failed(sb))
					return -EIO;
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, indirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, indirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			beamfs_tc_zeroed(sb, inode->i_ino, ibh->b_blocknr, __func__);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			/*
			 * Parity taken under the lock, like the install
			 * sites do.
			 *
			 * The block is allocated and visible before its
			 * pointer is installed a few lines down, so
			 * another writer can reach it. Computing parity
			 * after unlock_buffer describes whatever the block
			 * held at that instant, which need not be what
			 * reaches the medium: generic/464 writes and
			 * fsyncs the same files from several tasks and
			 * produced 194 indirect blocks whose parity was
			 * written and did not match, seven orphaned blocks
			 * under each.
			 */
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(ibh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */

			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(ibh);
			/*
			 * The inode's own pointer is a slot store too.
			 *
			 * parent 0 and level 0, as for the direct array:
			 * there is no block above an inode's pointer. Without
			 * it a probe following a block from allocation to its
			 * tree loses every root indirect block.
			 */
			trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, 0, 0, 0,
						indirect_blk, 0);
			fi->i_indirect = cpu_to_le64(indirect_blk);
			mark_inode_dirty(inode);
		}

		/* Read indirect to look up / install the slot. */
		/* Already held when this call created it. */
		{
			int cached = 1;

			if (!ibh) {
				/*
				 * Same probe as the read path: was this
				 * block already in the cache, or does it
				 * come off the medium?
				 *
				 * The allocation path had no tracepoint at
				 * all, so a slot installed here and read
				 * back as zero 150 ms later could not be
				 * told apart from a slot never written.
				 * generic/083 lost 47 blocks that way and
				 * the trace could not say which.
				 */
				struct buffer_head *probe =
					sb_find_get_block(sb, indirect_blk);

				cached = !!probe;
				if (probe)
					brelse(probe);
				ibh = sb_bread(sb, indirect_blk);
			}
			if (ibh)
				trace_beamfs_ind_read(inode->i_sb->s_dev,
						      inode->i_ino, indirect_blk,
					(unsigned int)indirect_slot,
					le64_to_cpu(((__le64 *)ibh->b_data)[indirect_slot]),
					buffer_uptodate(ibh), !cached);
		}
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read indirect block %llu\n",
					   (unsigned long long)indirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		phys = le64_to_cpu(ptrs[indirect_slot]);

		if (phys) {
			brelse(ibh);
			*phys_out = phys;
			return 0;
		}

		/* Allocate data block and zero-init. */
		new_block = beamfs_alloc_block(sb, inode);
		if (!new_block) {
			brelse(ibh);
			if (beamfs_failed(sb))
				return -EIO;
			return -ENOSPC;
		}
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block, inode);
			brelse(ibh);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		/*
		 * Stamp the DATA_CSUM descriptor on the freshly allocated data
		 * block. An all-zero block is a valid RS codeword by linearity,
		 * so decode accepts it, but the descriptor would stay 0x00 and
		 * the read path would have nothing to verify against. Before
		 * the type byte was made fail-closed that meant silent
		 * acceptance; afterwards it means a legitimate newly allocated
		 * block is rejected. Observed 2026-08-18: iblocks 0..5 of a
		 * test file logged "bad descriptor type=0x00" on five runs out
		 * of six, because allocation writes the block here while the
		 * only other stamp sites are writeback and
		 * zero_tail_block, neither of which runs for a block that is
		 * allocated but not yet written through the folio path. The
		 * payload is the zeroed block itself. Indirect blocks carry no
		 * DATA_CSUM descriptor and are deliberately left untouched.
		 */
		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)dbh->b_data,
					     (const u8 *)dbh->b_data,
					     (unsigned long long)inode->i_ino, iblock_logical);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		/*
		 * A data block carries sixteen RS codewords, not indirect
		 * parity.
		 *
		 * ind_parity_update files a CRC for this block number in
		 * the separate parity region that covers indirect blocks.
		 * A newly allocated data block was being registered there,
		 * and its own sixteen codewords were left as the zeros
		 * memset put down -- correct for a zero block, wrong the
		 * moment anything is written into it and the parity is not
		 * recomputed.
		 *
		 * The sweep found exactly that: subblocks holding data,
		 * uncorrectable; subblocks still zero, fine. Encoding here
		 * makes a fresh block valid on its own terms from the
		 * start.
		 */
		{
			int _e = beamfs_seal_data_block(sb, (u8 *)dbh->b_data);

			if (_e < 0)
				pr_err_ratelimited("beamfs/inline: encode of new data block %llu failed: %d\n",
						   (unsigned long long)new_block,
						   _e);
		}
		mark_buffer_dirty(dbh);

		/*
		 * Under the buffer lock, like ext2 splicing a branch.
		 *
		 * This stores a pointer into a shared indirect block. Without
		 * the lock the store races the flusher submitting that same
		 * buffer: the pointer reaches disk half-written, or a
		 * concurrent installer's store is lost. generic/464 lost
		 * blocks whose final event was exactly this store into an L1
		 * shared across inodes, with no free and no truncate after.
		 * ind_parity_update reads b_data, so it belongs inside the
		 * lock too.
		 */
		lock_buffer(ibh);
		trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, ibh->b_blocknr,
					indirect_slot, le64_to_cpu(ptrs[indirect_slot]),
					new_block, 1);
		beamfs_tc_store(sb, inode->i_ino, ibh->b_blocknr,
				(u32)indirect_slot, le64_to_cpu(ptrs[indirect_slot]), new_block,
			__func__);
		/* The block, before the pointer that names it. */
		beamfs_order_before_pointer(dbh);
		brelse(dbh);
		ptrs[indirect_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, ibh, inode);
		unlock_buffer(ibh);
		/*
		 * Attach the block to the inode and dirty the inode, the
		 * way ext2_splice_branch ends:
		 *
		 *   mmb_mark_buffer_dirty(where->bh,
		 *                         &EXT2_I(inode)->i_metadata_bhs);
		 *   inode_set_ctime_current(inode);
		 *   mark_inode_dirty(inode);
		 *
		 * A plain mark_buffer_dirty leaves the block on no inode's
		 * metadata list, so __writeback_single_inode never flushes
		 * it: the pointer is in memory, the buffer is dirty, and
		 * nothing carries it to the disk. The three sites that
		 * create a fresh indirect block already dirty the inode --
		 * they change i_indirect -- and these three, installing
		 * into a block that already exists, did not.
		 *
		 * That is the generic/464 leak: 28 blocks allocated by one
		 * writer into one inode from this site, no free after, and
		 * the flusher reallocating for the same inode seconds later
		 * because the tree it read had none of them.
		 */
		beamfs_attach_dirty(inode, ibh);
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
		brelse(ibh);

		if (allocated)
			*allocated = true;
		*phys_out = new_block;
		return 0;
	}

	if (iblock_logical < BEAMFS_MAX_IBLOCK_DINDIRECT) {
		/*
		 * Double-indirect write path: allocate dindirect block,
		 * level-1 indirect block, and data block in a cascade,
		 * each zero-initialized. The allocation order minimizes
		 * orphaned blocks on crash: data block last, so a crash
		 * between dindirect alloc and data alloc leaves a zero
		 * level-1 pointer (treated as HOLE on read).
		 */
		u64 didx, l1_slot, l2_slot;
		u64 dindirect_blk, l1_blk;
		struct buffer_head *l1bh = NULL;

		didx = iblock_logical - BEAMFS_MAX_IBLOCK_INDIRECT;
		l1_slot = didx / BEAMFS_INDIRECT_PTRS;
		l2_slot = didx % BEAMFS_INDIRECT_PTRS;

		/* --- Stage 1: dindirect block --- */
		dindirect_blk = le64_to_cpu(fi->i_dindirect);
		if (!dindirect_blk) {
			dindirect_blk = beamfs_alloc_block(sb, inode);
			if (!dindirect_blk) {
				if (beamfs_failed(sb))
					return -EIO;
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, dindirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, dindirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			beamfs_tc_zeroed(sb, inode->i_ino, ibh->b_blocknr, __func__);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			/*
			 * Parity taken under the lock, like the install
			 * sites do.
			 *
			 * The block is allocated and visible before its
			 * pointer is installed a few lines down, so
			 * another writer can reach it. Computing parity
			 * after unlock_buffer describes whatever the block
			 * held at that instant, which need not be what
			 * reaches the medium: generic/464 writes and
			 * fsyncs the same files from several tasks and
			 * produced 194 indirect blocks whose parity was
			 * written and did not match, seven orphaned blocks
			 * under each.
			 */
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(ibh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(ibh);
			/*
			 * The inode's own pointer is a slot store too.
			 *
			 * parent 0 and level 0, as for the direct array:
			 * there is no block above an inode's pointer. Without
			 * it a probe following a block from allocation to its
			 * tree loses every root indirect block.
			 */
			trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, 0, 0, 0,
						dindirect_blk, 0);
			fi->i_dindirect = cpu_to_le64(dindirect_blk);
			mark_inode_dirty(inode);
		}

		/* --- Stage 2: level-1 indirect block --- */
		{
			/*
			 * The last unlit sb_bread of the allocation path.
			 *
			 * generic/083 on 2026-09-18, inode 39: block 1067
			 * became its i_dindirect at 479.487 and slots 0
			 * to 4 were filled; from 483.756 slots 0, 1, 2 and
			 * 3 were each written again, every one of them
			 * reading zero, and treecheck reported three lost
			 * pointers. Not one of those stores had an
			 * ind_read beside it, because the read that
			 * decides them happens here and here alone.
			 *
			 * 1067 had been a DATA block of inode 7 until
			 * 479.485 -- 1.7 ms earlier. A data block recycled
			 * into an indirect one passes through no path that
			 * detaches its buffer from the old owner.
			 */
			struct buffer_head *probe =
				sb_find_get_block(sb, dindirect_blk);
			int cached = !!probe;

			if (probe)
				brelse(probe);
			ibh = sb_bread(sb, dindirect_blk);
			if (ibh)
				trace_beamfs_ind_read(inode->i_sb->s_dev,
						      inode->i_ino, dindirect_blk,
					(unsigned int)l1_slot,
					le64_to_cpu(((__le64 *)ibh->b_data)[l1_slot]),
					buffer_uptodate(ibh), !cached);
		}
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read dindirect block %llu\n",
					   (unsigned long long)dindirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		l1_blk = le64_to_cpu(ptrs[l1_slot]);
		if (!l1_blk) {
			l1_blk = beamfs_alloc_block(sb, inode);
			if (!l1_blk) {
				brelse(ibh);
				if (beamfs_failed(sb))
					return -EIO;
				return -ENOSPC;
			}
			l1bh = sb_getblk(sb, l1_blk);
			if (!l1bh) {
				beamfs_free_block(sb, l1_blk, inode);
				brelse(ibh);
				return -EIO;
			}
			lock_buffer(l1bh);
			beamfs_tc_zeroed(sb, inode->i_ino, l1bh->b_blocknr, __func__);
			memset(l1bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l1bh);
			unlock_buffer(l1bh);
			beamfs_ind_parity_update(sb, l1bh, inode);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(l1bh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * And the inode, so the list gets flushed.
			 *
			 * mmb_mark_buffer_dirty puts the buffer on the inode's
			 * metadata list, and mmb_sync empties that list -- from
			 * write_inode, which the VFS calls only for an inode it
			 * believes is dirty. Installing a pointer into a parent block
			 * changes no field of the inode, so nothing marked it, and
			 * both buffers stayed in memory with the pointer in them.
			 *
			 * generic/013 loses fourteen blocks that way: allocated,
			 * placed in a slot, and the block holding the slot never
			 * written -- "indirect block N has no parity written yet".
			 */
			mark_inode_dirty(inode);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
			/*
			 * Traced like every other slot install.
			 *
			 * Three of the ten allocation sites called tc_store
			 * and not this, so a probe pairing allocations with
			 * stores counted every L1 and L2 indirect block as an
			 * allocation that went nowhere: 4126 of them in one
			 * generic/476, against the one block fsck found
			 * leaked.
			 */
			/*
			 * Under the buffer lock, like the data installs above.
			 *
			 * This stores a pointer into a shared indirect block. Without
			 * the lock the store races the flusher submitting that same
			 * buffer: the pointer reaches disk half-written, or a
			 * concurrent installer's store is lost. treecheck caught it
			 * from wb_workfn -- LOST POINTER, held N, read as 0 -- and the
			 * child block then has no parity written for it and its whole
			 * subtree is unreachable. The three data installs took this
			 * lock; the three indirect installs did not.
			 * ind_parity_update reads b_data, so it belongs inside too.
			 */
			lock_buffer(ibh);
			trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, ibh->b_blocknr,
						l1_slot, le64_to_cpu(ptrs[l1_slot]),
						l1_blk, 2);
			beamfs_tc_store(sb, inode->i_ino, ibh->b_blocknr,
					(u32)l1_slot, le64_to_cpu(ptrs[l1_slot]), l1_blk,
				__func__);
			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(l1bh);
			ptrs[l1_slot] = cpu_to_le64(l1_blk);
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			/* Splicing a child into its parent is an install like
			 * any other: the parent goes on the inode's list too,
			 * or it is dirty on nobody's.
			 */
			beamfs_attach_dirty(inode, ibh);
			/*
			 * And the inode, so the list gets flushed.
			 *
			 * mmb_mark_buffer_dirty puts the buffer on the inode's
			 * metadata list, and mmb_sync empties that list -- from
			 * write_inode, which the VFS calls only for an inode it
			 * believes is dirty. Installing a pointer into a parent block
			 * changes no field of the inode, so nothing marked it, and
			 * both buffers stayed in memory with the pointer in them.
			 *
			 * generic/013 loses fourteen blocks that way: allocated,
			 * placed in a slot, and the block holding the slot never
			 * written -- "indirect block N has no parity written yet".
			 */
			mark_inode_dirty(inode);
		}
		brelse(ibh);

		/* --- Stage 3: data block --- */
		/* Already held when this call created it. */
		{
			int cached = 1;

			if (!l1bh) {
				/*
				 * The level-1 path had this probe and the
				 * double indirect had none, so every lost
				 * pointer below a dindirect was invisible.
				 *
				 * generic/083 on 2026-09-18: inode 112 put
				 * 2127 into slot 0 of block 1248, then put
				 * 11106 into that same slot 4.3 seconds
				 * later, both stores reading zero. Far too
				 * far apart for a race, and the trace had
				 * nothing at all between them because the
				 * read that returned zero was here.
				 */
				struct buffer_head *probe =
					sb_find_get_block(sb, l1_blk);

				cached = !!probe;
				if (probe)
					brelse(probe);
				l1bh = sb_bread(sb, l1_blk);
			}
			if (l1bh)
				trace_beamfs_ind_read(inode->i_sb->s_dev,
						      inode->i_ino, l1_blk,
					(unsigned int)l2_slot,
					le64_to_cpu(((__le64 *)l1bh->b_data)[l2_slot]),
					buffer_uptodate(l1bh), !cached);
		}
		if (!l1bh) {
			pr_err_ratelimited("beamfs/inline: failed to read L1 indirect block %llu\n",
					   (unsigned long long)l1_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, l1bh)) {
			brelse(l1bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l1bh->b_data;
		phys = le64_to_cpu(ptrs[l2_slot]);
		if (phys) {
			brelse(l1bh);
			*phys_out = phys;
			return 0;
		}

		new_block = beamfs_alloc_block(sb, inode);
		if (!new_block) {
			brelse(l1bh);
			if (beamfs_failed(sb))
				return -EIO;
			return -ENOSPC;
		}
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block, inode);
			brelse(l1bh);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		/*
		 * Stamp the DATA_CSUM descriptor on the freshly allocated data
		 * block. An all-zero block is a valid RS codeword by linearity,
		 * so decode accepts it, but the descriptor would stay 0x00 and
		 * the read path would have nothing to verify against. Before
		 * the type byte was made fail-closed that meant silent
		 * acceptance; afterwards it means a legitimate newly allocated
		 * block is rejected. Observed 2026-08-18: iblocks 0..5 of a
		 * test file logged "bad descriptor type=0x00" on five runs out
		 * of six, because allocation writes the block here while the
		 * only other stamp sites are writeback and
		 * zero_tail_block, neither of which runs for a block that is
		 * allocated but not yet written through the folio path. The
		 * payload is the zeroed block itself. Indirect blocks carry no
		 * DATA_CSUM descriptor and are deliberately left untouched.
		 */
		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)dbh->b_data,
					     (const u8 *)dbh->b_data,
					     (unsigned long long)inode->i_ino, iblock_logical);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		/*
		 * A data block carries sixteen RS codewords, not indirect
		 * parity.
		 *
		 * ind_parity_update files a CRC for this block number in
		 * the separate parity region that covers indirect blocks.
		 * A newly allocated data block was being registered there,
		 * and its own sixteen codewords were left as the zeros
		 * memset put down -- correct for a zero block, wrong the
		 * moment anything is written into it and the parity is not
		 * recomputed.
		 *
		 * The sweep found exactly that: subblocks holding data,
		 * uncorrectable; subblocks still zero, fine. Encoding here
		 * makes a fresh block valid on its own terms from the
		 * start.
		 */
		{
			int _e = beamfs_seal_data_block(sb, (u8 *)dbh->b_data);

			if (_e < 0)
				pr_err_ratelimited("beamfs/inline: encode of new data block %llu failed: %d\n",
						   (unsigned long long)new_block,
						   _e);
		}
		mark_buffer_dirty(dbh);

		/*
		 * Under the buffer lock, like ext2 splicing a branch.
		 *
		 * This stores a pointer into a shared indirect block. Without
		 * the lock the store races the flusher submitting that same
		 * buffer: the pointer reaches disk half-written, or a
		 * concurrent installer's store is lost. generic/464 lost
		 * blocks whose final event was exactly this store into an L1
		 * shared across inodes, with no free and no truncate after.
		 * ind_parity_update reads b_data, so it belongs inside the
		 * lock too.
		 */
		lock_buffer(l1bh);
		trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, l1bh->b_blocknr,
					l2_slot, le64_to_cpu(ptrs[l2_slot]),
					new_block, 2);
		beamfs_tc_store(sb, inode->i_ino, l1bh->b_blocknr,
				(u32)l2_slot, le64_to_cpu(ptrs[l2_slot]), new_block,
			__func__);
		/* The block, before the pointer that names it. */
		beamfs_order_before_pointer(dbh);
		brelse(dbh);
		ptrs[l2_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, l1bh, inode);
		unlock_buffer(l1bh);
		/*
		 * Attach the block to the inode and dirty the inode, the
		 * way ext2_splice_branch ends:
		 *
		 *   mmb_mark_buffer_dirty(where->bh,
		 *                         &EXT2_I(inode)->i_metadata_bhs);
		 *   inode_set_ctime_current(inode);
		 *   mark_inode_dirty(inode);
		 *
		 * A plain mark_buffer_dirty leaves the block on no inode's
		 * metadata list, so __writeback_single_inode never flushes
		 * it: the pointer is in memory, the buffer is dirty, and
		 * nothing carries it to the disk. The three sites that
		 * create a fresh indirect block already dirty the inode --
		 * they change i_indirect -- and these three, installing
		 * into a block that already exists, did not.
		 *
		 * That is the generic/464 leak: 28 blocks allocated by one
		 * writer into one inode from this site, no free after, and
		 * the flusher reallocating for the same inode seconds later
		 * because the tree it read had none of them.
		 */
		beamfs_attach_dirty(inode, l1bh);
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
		brelse(l1bh);

		if (allocated)
			*allocated = true;
		*phys_out = new_block;
		return 0;
	}

	if (iblock_logical < BEAMFS_MAX_IBLOCK_TINDIRECT) {
		/*
		 * Triple-indirect write path: allocate tindirect block,
		 * level-1 indirect block, level-2 indirect block, and data
		 * block in a cascade, each zero-initialized. Allocation order
		 * minimizes orphaned blocks on crash: data block last.
		 */
		u64 tidx, l1_slot, l2_slot, l3_slot;
		u64 tindirect_blk, l1_blk, l2_blk;
		struct buffer_head *l1bh = NULL, *l2bh = NULL;

		tidx = iblock_logical - BEAMFS_MAX_IBLOCK_DINDIRECT;
		l1_slot = tidx / (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS);
		l2_slot = (tidx / BEAMFS_INDIRECT_PTRS) % BEAMFS_INDIRECT_PTRS;
		l3_slot = tidx % BEAMFS_INDIRECT_PTRS;

		/* --- Stage 1: tindirect block --- */
		tindirect_blk = le64_to_cpu(fi->i_tindirect);
		if (!tindirect_blk) {
			tindirect_blk = beamfs_alloc_block(sb, inode);
			if (!tindirect_blk) {
				if (beamfs_failed(sb))
					return -EIO;
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, tindirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, tindirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			beamfs_tc_zeroed(sb, inode->i_ino, ibh->b_blocknr, __func__);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			/*
			 * Parity taken under the lock, like the install
			 * sites do.
			 *
			 * The block is allocated and visible before its
			 * pointer is installed a few lines down, so
			 * another writer can reach it. Computing parity
			 * after unlock_buffer describes whatever the block
			 * held at that instant, which need not be what
			 * reaches the medium: generic/464 writes and
			 * fsyncs the same files from several tasks and
			 * produced 194 indirect blocks whose parity was
			 * written and did not match, seven orphaned blocks
			 * under each.
			 */
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(ibh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(ibh);
			/*
			 * The inode's own pointer is a slot store too.
			 *
			 * parent 0 and level 0, as for the direct array:
			 * there is no block above an inode's pointer. Without
			 * it a probe following a block from allocation to its
			 * tree loses every root indirect block.
			 */
			trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, 0, 0, 0,
						tindirect_blk, 0);
			fi->i_tindirect = cpu_to_le64(tindirect_blk);
			mark_inode_dirty(inode);
		}

		/* --- Stage 2: level-1 indirect block --- */
		ibh = sb_bread(sb, tindirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect block %llu\n",
					   (unsigned long long)tindirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		l1_blk = le64_to_cpu(ptrs[l1_slot]);
		if (!l1_blk) {
			l1_blk = beamfs_alloc_block(sb, inode);
			if (!l1_blk) {
				brelse(ibh);
				if (beamfs_failed(sb))
					return -EIO;
				return -ENOSPC;
			}
			l1bh = sb_getblk(sb, l1_blk);
			if (!l1bh) {
				beamfs_free_block(sb, l1_blk, inode);
				brelse(ibh);
				return -EIO;
			}
			lock_buffer(l1bh);
			beamfs_tc_zeroed(sb, inode->i_ino, l1bh->b_blocknr, __func__);
			memset(l1bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l1bh);
			unlock_buffer(l1bh);
			beamfs_ind_parity_update(sb, l1bh, inode);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(l1bh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * And the inode, so the list gets flushed.
			 *
			 * mmb_mark_buffer_dirty puts the buffer on the inode's
			 * metadata list, and mmb_sync empties that list -- from
			 * write_inode, which the VFS calls only for an inode it
			 * believes is dirty. Installing a pointer into a parent block
			 * changes no field of the inode, so nothing marked it, and
			 * both buffers stayed in memory with the pointer in them.
			 *
			 * generic/013 loses fourteen blocks that way: allocated,
			 * placed in a slot, and the block holding the slot never
			 * written -- "indirect block N has no parity written yet".
			 */
			mark_inode_dirty(inode);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
			/*
			 * Traced like every other slot install.
			 *
			 * Three of the ten allocation sites called tc_store
			 * and not this, so a probe pairing allocations with
			 * stores counted every L1 and L2 indirect block as an
			 * allocation that went nowhere: 4126 of them in one
			 * generic/476, against the one block fsck found
			 * leaked.
			 */
			/*
			 * Under the buffer lock, like the data installs above.
			 *
			 * This stores a pointer into a shared indirect block. Without
			 * the lock the store races the flusher submitting that same
			 * buffer: the pointer reaches disk half-written, or a
			 * concurrent installer's store is lost. treecheck caught it
			 * from wb_workfn -- LOST POINTER, held N, read as 0 -- and the
			 * child block then has no parity written for it and its whole
			 * subtree is unreachable. The three data installs took this
			 * lock; the three indirect installs did not.
			 * ind_parity_update reads b_data, so it belongs inside too.
			 */
			lock_buffer(ibh);
			trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, ibh->b_blocknr,
						l1_slot, le64_to_cpu(ptrs[l1_slot]),
						l1_blk, 2);
			beamfs_tc_store(sb, inode->i_ino, ibh->b_blocknr,
					(u32)l1_slot, le64_to_cpu(ptrs[l1_slot]), l1_blk,
				__func__);
			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(l1bh);
			ptrs[l1_slot] = cpu_to_le64(l1_blk);
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			/* Splicing a child into its parent is an install like
			 * any other: the parent goes on the inode's list too,
			 * or it is dirty on nobody's.
			 */
			beamfs_attach_dirty(inode, ibh);
			/*
			 * And the inode, so the list gets flushed.
			 *
			 * mmb_mark_buffer_dirty puts the buffer on the inode's
			 * metadata list, and mmb_sync empties that list -- from
			 * write_inode, which the VFS calls only for an inode it
			 * believes is dirty. Installing a pointer into a parent block
			 * changes no field of the inode, so nothing marked it, and
			 * both buffers stayed in memory with the pointer in them.
			 *
			 * generic/013 loses fourteen blocks that way: allocated,
			 * placed in a slot, and the block holding the slot never
			 * written -- "indirect block N has no parity written yet".
			 */
			mark_inode_dirty(inode);
		}
		brelse(ibh);

		/* --- Stage 3: level-2 indirect block --- */
		/* Already held when this call created it. */
		if (!l1bh)
			l1bh = sb_bread(sb, l1_blk);
		if (!l1bh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect L1 block %llu\n",
					   (unsigned long long)l1_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, l1bh)) {
			brelse(l1bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l1bh->b_data;
		l2_blk = le64_to_cpu(ptrs[l2_slot]);
		if (!l2_blk) {
			l2_blk = beamfs_alloc_block(sb, inode);
			if (!l2_blk) {
				brelse(l1bh);
				if (beamfs_failed(sb))
					return -EIO;
				return -ENOSPC;
			}
			l2bh = sb_getblk(sb, l2_blk);
			if (!l2bh) {
				beamfs_free_block(sb, l2_blk, inode);
				brelse(l1bh);
				return -EIO;
			}
			lock_buffer(l2bh);
			beamfs_tc_zeroed(sb, inode->i_ino, l2bh->b_blocknr, __func__);
			memset(l2bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l2bh);
			unlock_buffer(l2bh);
			beamfs_ind_parity_update(sb, l2bh, inode);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(l2bh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * And the inode, so the list gets flushed.
			 *
			 * mmb_mark_buffer_dirty puts the buffer on the inode's
			 * metadata list, and mmb_sync empties that list -- from
			 * write_inode, which the VFS calls only for an inode it
			 * believes is dirty. Installing a pointer into a parent block
			 * changes no field of the inode, so nothing marked it, and
			 * both buffers stayed in memory with the pointer in them.
			 *
			 * generic/013 loses fourteen blocks that way: allocated,
			 * placed in a slot, and the block holding the slot never
			 * written -- "indirect block N has no parity written yet".
			 */
			mark_inode_dirty(inode);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
			/*
			 * Traced like every other slot install.
			 *
			 * Three of the ten allocation sites called tc_store
			 * and not this, so a probe pairing allocations with
			 * stores counted every L1 and L2 indirect block as an
			 * allocation that went nowhere: 4126 of them in one
			 * generic/476, against the one block fsck found
			 * leaked.
			 */
			/*
			 * Under the buffer lock, like the data installs above.
			 *
			 * This stores a pointer into a shared indirect block. Without
			 * the lock the store races the flusher submitting that same
			 * buffer: the pointer reaches disk half-written, or a
			 * concurrent installer's store is lost. treecheck caught it
			 * from wb_workfn -- LOST POINTER, held N, read as 0 -- and the
			 * child block then has no parity written for it and its whole
			 * subtree is unreachable. The three data installs took this
			 * lock; the three indirect installs did not.
			 * ind_parity_update reads b_data, so it belongs inside too.
			 */
			lock_buffer(l1bh);
			trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, l1bh->b_blocknr,
						l2_slot, le64_to_cpu(ptrs[l2_slot]),
						l2_blk, 3);
			beamfs_tc_store(sb, inode->i_ino, l1bh->b_blocknr,
					(u32)l2_slot, le64_to_cpu(ptrs[l2_slot]), l2_blk,
				__func__);
			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(l2bh);
			ptrs[l2_slot] = cpu_to_le64(l2_blk);
			beamfs_ind_parity_update(sb, l1bh, inode);
			unlock_buffer(l1bh);
			/* Splicing a child into its parent is an install like
			 * any other: the parent goes on the inode's list too,
			 * or it is dirty on nobody's.
			 */
			beamfs_attach_dirty(inode, l1bh);
			/*
			 * And the inode, so the list gets flushed.
			 *
			 * mmb_mark_buffer_dirty puts the buffer on the inode's
			 * metadata list, and mmb_sync empties that list -- from
			 * write_inode, which the VFS calls only for an inode it
			 * believes is dirty. Installing a pointer into a parent block
			 * changes no field of the inode, so nothing marked it, and
			 * both buffers stayed in memory with the pointer in them.
			 *
			 * generic/013 loses fourteen blocks that way: allocated,
			 * placed in a slot, and the block holding the slot never
			 * written -- "indirect block N has no parity written yet".
			 */
			mark_inode_dirty(inode);
		}
		brelse(l1bh);

		/* --- Stage 4: data block --- */
		/* Already held when this call created it. */
		if (!l2bh)
			l2bh = sb_bread(sb, l2_blk);
		if (!l2bh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect L2 block %llu\n",
					   (unsigned long long)l2_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS a copy
		 * is decoded and what it corrects is journalled, but the
		 * pointers are read as the medium holds them: a correctable
		 * flip is reported, not repaired.
		 */
		if (beamfs_ind_parity_verify(sb, l2bh)) {
			brelse(l2bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l2bh->b_data;
		phys = le64_to_cpu(ptrs[l3_slot]);
		if (phys) {
			brelse(l2bh);
			*phys_out = phys;
			return 0;
		}

		new_block = beamfs_alloc_block(sb, inode);
		if (!new_block) {
			brelse(l2bh);
			if (beamfs_failed(sb))
				return -EIO;
			return -ENOSPC;
		}
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block, inode);
			brelse(l2bh);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		/*
		 * Stamp the DATA_CSUM descriptor on the freshly allocated data
		 * block. An all-zero block is a valid RS codeword by linearity,
		 * so decode accepts it, but the descriptor would stay 0x00 and
		 * the read path would have nothing to verify against. Before
		 * the type byte was made fail-closed that meant silent
		 * acceptance; afterwards it means a legitimate newly allocated
		 * block is rejected. Observed 2026-08-18: iblocks 0..5 of a
		 * test file logged "bad descriptor type=0x00" on five runs out
		 * of six, because allocation writes the block here while the
		 * only other stamp sites are writeback and
		 * zero_tail_block, neither of which runs for a block that is
		 * allocated but not yet written through the folio path. The
		 * payload is the zeroed block itself. Indirect blocks carry no
		 * DATA_CSUM descriptor and are deliberately left untouched.
		 */
		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)dbh->b_data,
					     (const u8 *)dbh->b_data,
					     (unsigned long long)inode->i_ino, iblock_logical);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		/*
		 * A data block carries sixteen RS codewords, not indirect
		 * parity.
		 *
		 * ind_parity_update files a CRC for this block number in
		 * the separate parity region that covers indirect blocks.
		 * A newly allocated data block was being registered there,
		 * and its own sixteen codewords were left as the zeros
		 * memset put down -- correct for a zero block, wrong the
		 * moment anything is written into it and the parity is not
		 * recomputed.
		 *
		 * The sweep found exactly that: subblocks holding data,
		 * uncorrectable; subblocks still zero, fine. Encoding here
		 * makes a fresh block valid on its own terms from the
		 * start.
		 */
		{
			int _e = beamfs_seal_data_block(sb, (u8 *)dbh->b_data);

			if (_e < 0)
				pr_err_ratelimited("beamfs/inline: encode of new data block %llu failed: %d\n",
						   (unsigned long long)new_block,
						   _e);
		}
		mark_buffer_dirty(dbh);

		/*
		 * Under the buffer lock, like ext2 splicing a branch.
		 *
		 * This stores a pointer into a shared indirect block. Without
		 * the lock the store races the flusher submitting that same
		 * buffer: the pointer reaches disk half-written, or a
		 * concurrent installer's store is lost. generic/464 lost
		 * blocks whose final event was exactly this store into an L1
		 * shared across inodes, with no free and no truncate after.
		 * ind_parity_update reads b_data, so it belongs inside the
		 * lock too.
		 */
		lock_buffer(l2bh);
		trace_beamfs_slot_store(inode->i_sb->s_dev, inode->i_ino, l2bh->b_blocknr,
					l3_slot, le64_to_cpu(ptrs[l3_slot]),
					new_block, 3);
		beamfs_tc_store(sb, inode->i_ino, l2bh->b_blocknr,
				(u32)l3_slot, le64_to_cpu(ptrs[l3_slot]), new_block,
			__func__);
		/* The block, before the pointer that names it. */
		beamfs_order_before_pointer(dbh);
		brelse(dbh);
		ptrs[l3_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, l2bh, inode);
		unlock_buffer(l2bh);
		/*
		 * Attach the block to the inode and dirty the inode, the
		 * way ext2_splice_branch ends:
		 *
		 *   mmb_mark_buffer_dirty(where->bh,
		 *                         &EXT2_I(inode)->i_metadata_bhs);
		 *   inode_set_ctime_current(inode);
		 *   mark_inode_dirty(inode);
		 *
		 * A plain mark_buffer_dirty leaves the block on no inode's
		 * metadata list, so __writeback_single_inode never flushes
		 * it: the pointer is in memory, the buffer is dirty, and
		 * nothing carries it to the disk. The three sites that
		 * create a fresh indirect block already dirty the inode --
		 * they change i_indirect -- and these three, installing
		 * into a block that already exists, did not.
		 *
		 * That is the generic/464 leak: 28 blocks allocated by one
		 * writer into one inode from this site, no free after, and
		 * the flusher reallocating for the same inode seconds later
		 * because the tree it read had none of them.
		 */
		beamfs_attach_dirty(inode, l2bh);
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
		brelse(l2bh);

		if (allocated)
			*allocated = true;
		*phys_out = new_block;
		return 0;
	}

	pr_err_ratelimited("beamfs/inline: iblock %llu beyond tindirect capacity (write)\n",
			   (unsigned long long)iblock_logical);
	return -EOPNOTSUPP;
}

