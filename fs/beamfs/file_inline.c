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

/* ------------------------------------------------------------------------- */
/*
 * beamfs_inline_stamp_tail_pad -- write the DATA_CSUM descriptor and the
 * DATA_SELFID digest into a data block. The 16-byte tail at
 * BEAMFS_DATA_INLINE_TOTAL is always zeroed first; in the alternating
 * layout that is where both go, and the zeroing covers the reserved
 * bytes and the csum-disabled (NONE) case. When the volume has DATA_CSUM
 * active, stamp csum_type = CRC32 and the beamfs_crc32 (crc32_le) of the
 * payload, beamfs_block_payload() bytes of it. Where the descriptor and
 * the digest live in each layout is below.
 */
/*
 * Where the descriptor lives, and how much it covers.
 *
 * Alternating: at 4080, outside every codeword, over the 3824 bytes of
 * payload. A flip there fails a read closed rather than accepting
 * wrong data.
 *
 * Capsule: at 3808, inside the coded area, over the 3808 bytes of
 * payload. The argument for staying outside was that a descriptor
 * cannot be repaired -- interleaved it can, along with everything
 * else, and nine bad bytes there stop condemning a block whose data is
 * intact. The sixteen bytes it costs are already spent: the payload is
 * 3808 and the encoder covers 3824, so 3808..3823 were protected and
 * empty.
 */
static u32 beamfs_desc_off(struct super_block *sb)
{
	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE)
		return BEAMFS_CAPSULE_CSUM_OFF;

	return BEAMFS_DATA_CSUM_TYPE_OFF;
}

static u32 beamfs_selfid_off(struct super_block *sb)
{
	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE)
		return BEAMFS_CAPSULE_SELFID_OFF;

	return BEAMFS_DATA_SELFID_OFF;
}

static void beamfs_inline_stamp_tail_pad(struct beamfs_sb_info *sbi,
					 u8 *block, const u8 *payload,
					 u64 ino, u64 iblock)
{
	struct super_block *sb = sbi->s_sb;
	u32 doff = beamfs_desc_off(sb);
	u32 soff = beamfs_selfid_off(sb);
	u32 covered = beamfs_block_payload(sb);

	memset(block + BEAMFS_DATA_INLINE_TOTAL, 0, BEAMFS_DATA_INLINE_PAD);
	/*
	 * The descriptor whole, whatever is stamped in it below: in a
	 * capsule it is coded data, and left as the page held it, it
	 * carried sixteen bytes of kernel memory to the medium.
	 */
	memset(block + doff, 0,
	       BEAMFS_DATA_CSUM_DESC_BYTES + BEAMFS_DATA_SELFID_BYTES);
	if (sbi->s_data_csum) {
		u32 crc = beamfs_crc32(payload, covered);

		block[doff] = BEAMFS_CSUM_CRC32;
		put_unaligned_le32(crc, block + doff + 4);
	}
	/*
	 * DATA_SELFID: bind the block to the (inode, logical index) pair it
	 * was written for, so a read reaching it through a corrupted pointer
	 * fails closed instead of returning another file's intact data.
	 */
	if (sbi->s_data_selfid) {
		u64 id = beamfs_data_selfid(ino, iblock);

		put_unaligned_le64(id, block + soff);
	}
}

/*
 * Stamp a data block's descriptor, before its seal. Exposed for
 * namei.c: the target of a long symlink is a data block written there.
 */
void beamfs_stamp_data_block(struct super_block *sb, u8 *block,
			     const u8 *payload, u64 ino, u64 iblock)
{
	beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), block, payload, ino, iblock);
}

/*
 * beamfs_inline_payload_crc -- recompute the DATA_CSUM over the decoded
 * payload held in @codeword, in whichever layout it is written.
 *
 * A capsule holds the payload contiguously: one crc32_le over its 3808
 * bytes. The alternating layout holds it in sixteen runs of 239 bytes,
 * 255 apart: crc32_le is chained over the sixteen runs, which equals
 * beamfs_crc32() over the contiguous 3824-byte payload, the value the
 * write path stored. Walking one layout as the other hashes the parity
 * along with the data and disagrees with what was stamped. No alloc.
 */
static u32 beamfs_inline_payload_crc(struct super_block *sb,
				     const u8 *codeword)
{
	u32 c = 0xFFFFFFFF;
	unsigned int i;

	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE)
		return crc32_le(c, codeword + BEAMFS_CAPSULE_DATA_OFF,
				BEAMFS_CAPSULE_DATA_BYTES) ^ 0xFFFFFFFF;

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		c = crc32_le(c,
			     codeword + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
			     BEAMFS_SUBBLOCK_DATA);
	return c ^ 0xFFFFFFFF;
}

/* beamfs_inline_decode_block_into_buf -- RS-decode a raw block image and    */
/*                                        copy a user-byte slice into the    */
/*                                        supplied buffer.                   */
/*                                                                           */
/* Called from read_folio_range, the partial-write fill                      */
/* (write_read_folio_range), writeback's read of a partial block             */
/* (beamfs_wb_read_old), zero_tail_block and the symlink reader.             */
/*                                                                           */
/* Inputs:                                                                   */
/*   sb                       superblock (layout, journal)                   */
/*   raw                      the block as read, BEAMFS_BLOCK_SIZE bytes;    */
/*                            copied, never modified                         */
/*   phys                     non-zero physical disk block number to decode  */
/*   inode                    inode (its ino for DATA_SELFID and log lines)  */
/*   iblock_logical_for_log   logical iblock, checked against DATA_SELFID    */
/*                            and named in log lines                         */
/*   dst_buf                  destination buffer; caller-allocated, must     */
/*                            have at least slice_length bytes available     */
/*   slice_offset             byte offset within the disk block user-area    */
/*                            (0..BEAMFS_DATA_INLINE_BYTES-1)                */
/*   slice_length             number of user bytes to copy into dst_buf      */
/*                            (1..BEAMFS_DATA_INLINE_BYTES)                  */
/*   rmw_path                 caller context flag: true when invoked from a  */
/*                            read-modify-write transit (beamfs_wb_read_old  */
/*                            or zero_tail_block), false otherwise.          */
/*                            Propagated into the journal entry as           */
/*                            BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED so that   */
/*                            silent neutralisation events become            */
/*                            empirically distinguishable from user-visible  */
/*                            corrections in post-run forensic analysis.     */
/*                            Also discriminates the pr_warn / pr_err log    */
/*                            line wording between "neutralised" and         */
/*                            "corrected" / "uncorrectable".                 */
/*                                                                           */
/* Slice contract: slice_offset + slice_length <= BEAMFS_DATA_INLINE_BYTES   */
/* (3824). HOLE (phys == 0) is NOT handled here -- caller must check.        */
/*                                                                           */
/* On any subblock corrected: the correction is journalled and the           */
/* corrected bytes are copied out; the block on the medium is not            */
/* rewritten here (see the end of the function).                             */
/* On any subblock uncorrectable, or a DATA_CSUM or DATA_SELFID mismatch:    */
/* journal entry with UNCORRECTABLE flag, then -EIO; no slice copy is        */
/* performed.                                                                */
/*                                                                           */
/* This isolates the RS decode from folio lifecycle management. The folio    */
/* lock and the end of the read are the caller's responsibility.             */
/* ------------------------------------------------------------------------- */
static int beamfs_inline_decode_block_into_buf(struct super_block *sb,
					       const u8 *raw,
					       u64 phys,
					       struct inode *inode,
					       u64 iblock_logical_for_log,
					       u8 *dst_buf,
					       u32 slice_offset,
					       u32 slice_length,
					       bool rmw_path)
{
	u8                 *tmp = NULL;
	int                 rs_results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	u8                 rs_positions[BEAMFS_DATA_INLINE_SUBBLOCKS *
				      (BEAMFS_RS_PARITY / 2)];
	bool                corrected = false;
	bool                uncorrectable = false;
	unsigned int        i;
	int                 ret = 0;
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	/* Defensive contract checks (cheap; helpful in audit and fuzzing). */
	if (WARN_ON_ONCE(phys == 0))
		return -EINVAL;
	if (WARN_ON_ONCE(!dst_buf))
		return -EINVAL;
	if (WARN_ON_ONCE(slice_length == 0))
		return -EINVAL;
	if (WARN_ON_ONCE((u64)slice_offset + slice_length >
			 BEAMFS_DATA_INLINE_BYTES))
		return -EINVAL;

	/*
	 * @raw is only read: it is copied into a private scratch buffer
	 * (tmp) and decoded there, never in place. decode_rs8 mutates both
	 * data and parity bytes in its input, and @raw may be the b_data of
	 * a buffer that other paths read. The decode_rs8 upstream race on
	 * rs_control->buffers[] is resolved by the per-CPU rs_control
	 * allocation in edac.c.
	 */
	tmp = beamfs_scratch_get(sb);
	if (!tmp)
		return -ENOMEM;
	/*
	 * No lock_buffer here, and the reason is in the callers.
	 *
	 * Those that pass a buffer's b_data -- write_read_folio_range,
	 * zero_tail_block and the symlink reader -- already hold its lock,
	 * so taking it again deadlocks on the first decode, which on this
	 * filesystem means the rootfs mount. That was tried and the node
	 * never reached a login prompt.
	 *
	 * The others -- read_folio_range and beamfs_wb_read_old -- pass a
	 * copy taken under the buffer lock, or a block read straight from
	 * the device into a scratch page.
	 */
	memcpy(tmp, raw, BEAMFS_BLOCK_SIZE);

	/*
	 * A capsule is gathered, not walked.
	 *
	 * Interleaved, symbol i of codeword j is at byte i*16 + j rather
	 * than j*255 + i, and the header sits inside the coded area. The
	 * two layouts share no arithmetic: reading one with the other's
	 * gathers the wrong symbols and decodes to noise with every
	 * check passing. The feature bit is what keeps them apart.
	 */
	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE) {
		beamfs_rs_decode_woven(tmp + BEAMFS_CAPSULE_DATA_OFF,
				       tmp + BEAMFS_CAPSULE_PARITY_OFF,
				       BEAMFS_RS_PARITY,
				       BEAMFS_SUBBLOCK_DATA,
				       BEAMFS_DATA_INLINE_SUBBLOCKS,
				       rs_results, "file data");
	} else {
		beamfs_rs_decode_region(tmp, BEAMFS_SUBBLOCK_TOTAL,
					tmp + BEAMFS_SUBBLOCK_DATA,
					BEAMFS_SUBBLOCK_TOTAL,
					BEAMFS_SUBBLOCK_DATA,
					BEAMFS_DATA_INLINE_SUBBLOCKS,
					rs_results, rs_positions,
					BEAMFS_RS_PARITY / 2, "file data");
	}

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		int rc = rs_results[i];

		if (rc < 0) {
			u32 xflags = rmw_path ?
				BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED : 0;

			/*
			 * Journal the uncorrectable event before raising the
			 * error: forensic record takes priority over the alert.
			 *
			 * rmw_path=true here means the uncorrectable was
			 * detected during a write transit. The caller fails the
			 * read-modify-write on the -EIO below, so the damaged
			 * block is not re-encoded over; the journal entry
			 * preserves the forensic record of the event.
			 */
			beamfs_log_rs_event_flagged(sb, (u64)phys,
						    NULL, 0,
				BEAMFS_SUBBLOCK_DATA,
				xflags | beamfs_rs_event_subblock_bits(i));
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu subblock=%u uncorrectable%s\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   i,
					   rmw_path ? " (rmw)" : "");
			beamfs_alert_uncorrectable(sb, phys);
			uncorrectable = true;
		} else if (rc > 0) {
			unsigned int np = (unsigned int)rc;
			u8 *pos = rs_positions +
				   (size_t)i * (BEAMFS_RS_PARITY / 2);
			u32 xflags = rmw_path ?
				BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED : 0;

			if (np > BEAMFS_RS_PARITY / 2)
				np = BEAMFS_RS_PARITY / 2;
			pr_warn_ratelimited("beamfs/inline: ino=%llu iblock=%llu subblock=%u: %d symbol(s) %s\n",
					    (unsigned long long)inode->i_ino,
					    (unsigned long long)iblock_logical_for_log,
					    i, rc,
					    rmw_path ? "neutralised" : "corrected");
			beamfs_log_rs_event_flagged(sb, (u64)phys,
						    pos, np,
				BEAMFS_SUBBLOCK_DATA,
				xflags | beamfs_rs_event_subblock_bits(i));
			/*
			 * The journal says this happened; the budget says
			 * how close the block now is to having nothing left.
			 * A high-water mark, so the byte reports the worst
			 * any subblock of this block has been rather than
			 * the most recent, since the worst is what will
			 * saturate first.
			 */
			beamfs_budget_record(sb, phys, np);
			corrected = true;
		}
	}

	if (uncorrectable) {
		ret = -EIO;
		goto out_brelse;
	}

	/*
	 * DATA_CSUM verification. decode_rs8 can return success while having
	 * converged to a wrong valid codeword (silent miscorrection).
	 * Recompute the payload checksum and reject on mismatch. The
	 * descriptor is read from tmp: in a capsule it sits inside the coded
	 * area and has just been decoded with the rest; in the alternating
	 * layout it sits in the tail pad, which decode does not touch. Any
	 * type other than CRC32 fails closed (see below). On mismatch,
	 * journal an UNCORRECTABLE event and fail closed before any slice
	 * copy, as the per-subblock uncorrectable path does.
	 */
	if (sbi->s_data_csum) {
		u32 doff  = beamfs_desc_off(sb);
		u8  ctype = tmp[doff];
		u32 want  = get_unaligned_le32(tmp + doff + 4);
		u32 got   = beamfs_inline_payload_crc(sb, tmp);

		/*
		 * csum_type gates the whole check. In the alternating layout
		 * the descriptor is outside every RS codeword, so it is
		 * neither corrected nor detected by the code. Accepting any
		 * type other than CRC32 as "no checksum present" therefore let
		 * a single flip on offset 4080 disable verification for that
		 * block, silently. Measured 2026-08-17 at 128 flips on a
		 * 64-block file: 20 RS symbols corrected, no uncorrectable, no
		 * csum mismatch logged, and 15296 wrong bits returned to
		 * userspace across two blocks -- the silent accept a descriptor
		 * outside the codewords was meant to rule out. That holds for
		 * the csum value (a corrupted CRC can only over-reject) but not
		 * for the type byte.
		 *
		 * On a DATA_CSUM volume every data block is stamped at write
		 * time, so any other type is a corrupted descriptor, not an
		 * unstamped block: fail closed instead of waving it through.
		 */
		if (ctype != BEAMFS_CSUM_CRC32) {
			/*
			 * UNCORRECTABLE is reserved:
			 * beamfs_log_rs_event_flagged sets it itself for
			 * the positions == NULL, n_positions == 0 call shape
			 * used here. Passing it in extra_flags trips the
			 * WARN_ON_ONCE reserved-mask guard there; that
			 * mistake is what made the master node report
			 * RS_FAILED on 2026-08-24.
			 */
			u32 xflags = 0;

			if (rmw_path)
				xflags |= BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED;

			beamfs_log_rs_event_flagged(sb,
						    (u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS,
						    NULL, 0,
						    BEAMFS_SUBBLOCK_DATA,
						    xflags);
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu data_csum bad descriptor type=0x%02x (expected 0x%02x)%s\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   ctype, BEAMFS_CSUM_CRC32,
					   rmw_path ? " (rmw)" : "");
			ret = -EIO;
			goto out_brelse;
		}

		if (want != got) {
			beamfs_log_rs_event_flagged(sb,
						    (u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS,
				NULL, 0,
				BEAMFS_SUBBLOCK_DATA,
				rmw_path ? BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED : 0);
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu data_csum mismatch want=0x%08x got=0x%08x%s\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   want, got,
					   rmw_path ? " (rmw)" : "");
			ret = -EIO;
			goto out_brelse;
		}
	}

	/*
	 * DATA_SELFID verification. DATA_CSUM proves the block is intact; it
	 * cannot prove it is the block that was asked for. A corrupted
	 * pointer landing inside the data area reaches a different but valid
	 * block whose own descriptor verifies, so every check above passes on
	 * the wrong data. Measured 2026-08-19: a direct pointer moved from
	 * physical 471 to 503 and the read returned that block's contents,
	 * 15245 wrong bits, with no kernel signal. Comparing the stored
	 * identity digest against the (inode, iblock) actually being read
	 * closes that path.
	 */
	if (sbi->s_data_selfid) {
		u64 want_id = beamfs_data_selfid(inode->i_ino,
						 iblock_logical_for_log);
		u64 got_id  = get_unaligned_le64(tmp +
						 beamfs_selfid_off(sb));

		if (want_id != got_id) {
			/*
			 * UNCORRECTABLE is reserved:
			 * beamfs_log_rs_event_flagged sets it itself for
			 * the positions == NULL, n_positions == 0 call shape
			 * used here. Passing it in extra_flags trips the
			 * WARN_ON_ONCE reserved-mask guard there; that
			 * mistake is what made the master node report
			 * RS_FAILED on 2026-08-24.
			 */
			u32 xflags = 0;

			if (rmw_path)
				xflags |= BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED;

			beamfs_log_rs_event_flagged(sb,
						    (u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS,
						    NULL, 0,
						    BEAMFS_SUBBLOCK_DATA,
						    xflags);
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu data_selfid mismatch want=0x%016llx got=0x%016llx%s\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   want_id, got_id,
					   rmw_path ? " (rmw)" : "");
			ret = -EIO;
			goto out_brelse;
		}
	}

	/*
	 * Slice gather: copy [slice_offset, slice_offset + slice_length)
	 * of the payload into dst_buf. The alternating layout holds it as
	 * 16 segments of 239 bytes with the parity between them: walk the
	 * relevant subblocks and copy the intersecting portion of each.
	 */
	{
		u32 slice_end = slice_offset + slice_length;
		u32 sb_first  = slice_offset / BEAMFS_SUBBLOCK_DATA;
		u32 sb_last   = (slice_end - 1) / BEAMFS_SUBBLOCK_DATA;
		u32 dst_off   = 0;
		u32 sb_idx;

		if (BEAMFS_SB(sb)->s_feat_incompat &
		    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE) {
			/*
			 * A capsule holds its payload contiguously, so
			 * the slice is one copy. Walking it in sixteen
			 * runs 255 apart reads each one 16 bytes further
			 * along than it lies.
			 */
			memcpy(dst_buf + dst_off,
			       tmp + BEAMFS_CAPSULE_DATA_OFF + slice_offset,
			       slice_length);
			dst_off += slice_length;
		} else {
			for (sb_idx = sb_first; sb_idx <= sb_last; sb_idx++) {
				u32 sb_user_start = sb_idx * BEAMFS_SUBBLOCK_DATA;
				u32 sb_user_end   = sb_user_start +
						    BEAMFS_SUBBLOCK_DATA;
				u32 from_in_sb    = (slice_offset > sb_user_start)
					? (slice_offset - sb_user_start) : 0;
				u32 to_in_sb      = (slice_end < sb_user_end)
					? (slice_end - sb_user_start)
					: BEAMFS_SUBBLOCK_DATA;
				u32 copy_len      = to_in_sb - from_in_sb;

				memcpy(dst_buf + dst_off,
				       tmp
					+ (size_t)sb_idx * BEAMFS_SUBBLOCK_TOTAL
					+ from_in_sb,
				       copy_len);
				dst_off += copy_len;
			}
		}
	}

	/*
	 * No write-back of the correction from here.
	 *
	 * The corrected bytes are in tmp; @raw, and the medium, still hold
	 * what was read. Writing the block back from this path would need
	 * either:
	 *  (a) an exclusive lock on phys around the read-modify-write
	 *      window, OR
	 *  (b) a CoW write path that allocates a fresh phys and
	 *      updates the indirect pointer.
	 * The correction is journalled above. The background scrubber
	 * writes corrected blocks back (beamfs_scrub_check_block), and a
	 * later write to the block re-encodes it whole.
	 */
	(void)corrected;

	beamfs_scratch_put(sb, tmp);
	return 0;

out_brelse:
	beamfs_scratch_put(sb, tmp);
	return ret;
}

/*
 * beamfs_inline_decode_symlink -- RS-decode a block holding a link target.
 *
 * The same decode as file data, exposed for namei.c because a long
 * symlink is a data block in every respect but the path that reaches it.
 * Separate entry point rather than a wider signature on the file path:
 * this one takes no folio, no iomap iterator and no RMW flag, and
 * threading three unused arguments through the reader to avoid twelve
 * lines here would be the worse trade.
 */
int beamfs_inline_decode_symlink(struct super_block *sb,
				 struct buffer_head *bh, u64 phys,
				 struct inode *inode, u8 *dst, u32 len)
{
	if (len > beamfs_block_payload(sb))
		return -EUCLEAN;

	return beamfs_inline_decode_block_into_buf(sb, bh->b_data, phys, inode, 0,
						   dst, 0, len, false);
}

/* ------------------------------------------------------------------------- */
/* iomap read path                                                           */
/*                                                                           */
/* Every 4096-byte block carries its RS parity alongside its payload: 3808   */
/* bytes of file data in a capsule, 3824 in the alternating layout. File     */
/* offset is therefore not disk offset plus a constant -- which is what      */
/* iomap_sector() assumes.                                                   */
/*                                                                           */
/* Two consequences shape this code. iomap_begin bounds a write or fiemap    */
/* mapping by the block that backs pos, and iomap calls back for the rest;   */
/* a read mapping keeps the full requested length (see iomap_begin). And     */
/* the read is done by beamfs_inline_read_folio_range() rather than by       */
/* iomap_bio_read_ops, because the bytes that reach the page cache are not   */
/* the bytes on disk: they are what RS decoding produces from them.          */
/* ------------------------------------------------------------------------- */

/*
 * A mapping iomap_begin allocated, for iomap_end to give back if the
 * write stops short of it.
 *
 * Not IOMAP_F_NEW. To iomap that flag means the folio range is new:
 * __iomap_write_begin zeroes it outside the bytes written instead of
 * reading it (iomap_block_needs_zeroing). A folio here holds the end
 * of one block and the start of the next, so a write starting a fresh
 * block zeroed the data of the block before it in the same folio.
 * Without the flag iomap reads the folio through
 * beamfs_inline_write_read_folio_range, which finds the fresh block
 * zeroed in the buffer cache.
 */
#define BEAMFS_IOMAP_F_FRESH	IOMAP_F_PRIVATE

static int beamfs_inline_iomap_begin(struct inode *inode, loff_t pos,
				     loff_t length, unsigned int flags,
				     struct iomap *iomap,
				     struct iomap *srcmap)
{
	loff_t i_size = i_size_read(inode);
	u32 payload = beamfs_block_payload(inode->i_sb);
	u64 b = div_u64((u64)pos, payload);
	u64 phys = 0;
	int ret;

	iomap->flags = 0;
	iomap->bdev = inode->i_sb->s_bdev;
	iomap->addr = IOMAP_NULL_ADDR;

	/*
	 * Start the mapping at the position the caller asked for, not at
	 * the start of the INLINE block that backs it. iomap tracks its
	 * position within the folio against the mapping it was given, and
	 * readahead ends a folio when offset_in_folio(folio, pos) reaches
	 * zero; handing back a range that starts before the requested
	 * position leaves that comparison never matching, and the iterator
	 * walks off a folio it has already released.
	 */
	iomap->offset = pos;

	/*
	 * Past EOF the whole remaining range is a hole, in one mapping:
	 * iomap zero-fills it without calling back. Splitting it into
	 * INLINE-sized pieces would leave iomap iterating after
	 * iomap_read_folio_iter() has released the folio, which
	 * dereferences NULL on the next round.
	 */
	/*
	 * Past EOF is a hole to a reader and nothing of the sort to a
	 * writer.
	 *
	 * A write at pos == i_size is the ordinary case of extending a
	 * file, and the block holding that position usually exists
	 * already: at 3808 or 3824 usable bytes per block, appending to a
	 * 32-byte file writes into block 0, which has data in it.
	 *
	 * Reporting IOMAP_HOLE there made iomap_block_needs_zeroing true
	 * on the first term, so __iomap_write_begin took the zeroing
	 * branch and called folio_zero_segments over the whole folio
	 * outside the written range -- erasing bytes 0..31, which were on
	 * disk and below i_size. generic/639 writes 32 bytes, cycles the
	 * mount, writes 32 more, and reads back thirty-two zeros followed
	 * by the second write. Silent loss of data the caller never
	 * touched.
	 *
	 * iomap works in i_blocksize units, 4096, while a block carries
	 * 3808 or 3824 bytes of payload, so its block boundaries are not ours
	 * and a range it considers past EOF can hold live bytes. Answering
	 * the question it actually asked -- what is mapped here -- rather
	 * than a shortcut about EOF keeps the two views consistent.
	 */
	if (!(flags & IOMAP_WRITE) && pos >= i_size) {
		iomap->length = length;
		iomap->type = IOMAP_HOLE;
		return 0;
	}

	/*
	 * One mapping covers the whole requested range, even where that
	 * spans two or three INLINE blocks.
	 *
	 * iomap_read_folio_iter() releases the folio as soon as a call
	 * returns with no iomap_folio_state attached -- which is always
	 * the case here, folio size and block size both being 4096, so
	 * ifs_alloc() has nothing to track. A mapping shorter than the
	 * folio therefore leaves iomap iterating over a folio it has
	 * already given up, and the next round dereferences NULL at
	 * folio_size(). Verified by disassembly: iomap_read_folio_iter+0x7c
	 * is mov (%r12),%rax with r12 = ctx->cur_folio.
	 *
	 * Spanning blocks is fine because read_folio_range does its own
	 * reading: it walks the blocks the range covers and decodes each.
	 * iomap->addr is reported to fiemap and tracing but nothing
	 * derives an I/O sector from it.
	 */
	/*
	 * Report asks about one block; read asks about the whole range.
	 *
	 * IOMAP_REPORT is fiemap, and it wants the truth: this mapping
	 * describes the block pos falls in and nothing beyond it.
	 * Claiming the requested length told fiemap that the answer for
	 * the first block held for the entire file, so a leading hole
	 * was reported as spanning 1 MiB, iomap stepped past EOF, and a
	 * file with 512 KiB of data came back with no extents at all.
	 *
	 * Everything else keeps the full length, and must. A mapping
	 * shorter than the folio makes iomap_read_folio_iter release the
	 * folio -- nothing attached an iomap_folio_state, folio and
	 * block size both being 4096 -- and then dereference it on the
	 * next round at folio_size(). That is the NULL documented above,
	 * confirmed by disassembly, and a 3824-byte mapping is always
	 * shorter than a 4096-byte folio.
	 */
	/*
	 * A write is bounded by the block it allocated; a read is not.
	 *
	 * The write leg allocates exactly one block -- b = pos / payload
	 * -- and then declared a mapping covering the whole requested
	 * length. iomap believes it: iomap_write_iter copies the entire
	 * range into the page cache without asking again, so a 1 MiB
	 * write allocated one block of 3808 bytes and accepted 275 more
	 * on the strength of it.
	 *
	 * Measured on 2026-09-19: 1.25 GiB written in fourteen seconds
	 * onto a 256 MiB volume, df moving by 11 MiB, dd returning
	 * success throughout. The file reached 1.41 GiB before the dirty
	 * page limit stopped it, and the allocation it needs happens at
	 * writeback, where ENOSPC has nobody to report to. That is the
	 * EFBIG generic/015 ends on, and the silent acceptance the whole
	 * filesystem exists to prevent.
	 *
	 * iomap handles a short mapping by calling back for the rest;
	 * every filesystem in the tree relies on that. The read leg is
	 * the one that cannot take it -- iomap_read_folio_iter releases
	 * a folio no iomap_folio_state is attached to and dereferences
	 * it on the next round, which is what the comment above records
	 * and disassembly confirmed -- so it keeps the full length.
	 */
	if (flags & (IOMAP_REPORT | IOMAP_WRITE)) {
		u64 in_block = (u64)pos - b * payload;

		/*
		 * payload, not BEAMFS_DATA_INLINE_BYTES.
		 *
		 * The remainder above is taken modulo payload and the
		 * bound was taken on the constant. On a capsule volume
		 * those are 3808 and 3824, so every extent fiemap
		 * reported ran up to sixteen bytes past the block that
		 * backs it, and the next one started inside it.
		 *
		 * beamfs_block_payload says what this costs in as many
		 * words: writing the constant instead "reads a file back
		 * shifted by sixteen bytes on a capsule volume,
		 * silently". generic/473 is where it shows: the test
		 * expects [128..255] and gets [128..254].
		 */
		iomap->length = min_t(u64, (u64)length,
				      (u64)payload - in_block);
	} else {
		iomap->length = length;
	}

	/*
	 * A write allocates here, not at writeback.
	 *
	 * Only lookup_phys was called, for every caller, so a write to an
	 * unallocated block was told IOMAP_HOLE and accepted into the page
	 * cache; the allocation happened later in the flusher, where an
	 * ENOSPC has nobody left to report to. A volume at 100% took a
	 * 10 MiB write at 5.5 GB/s -- nothing reached the disk, and the
	 * application was told it had.
	 *
	 * That is silent data loss, on a filesystem whose whole purpose is
	 * that data survives.
	 *
	 * It also explains the EFBIG that ended generic/015 and
	 * generic/269: dd kept writing past the end of a full 1 GiB volume
	 * until the file hit the 134480396-block ceiling of the
	 * indirection tree -- 479 GiB of file on a gigabyte of disk --
	 * because no write ever failed.
	 *
	 * ext2 draws the same line: create = flags & IOMAP_WRITE.
	 */
	if (flags & IOMAP_WRITE) {
		struct beamfs_inode_info *fi = BEAMFS_I(inode);

		/*
		 * Allocation is serialised by i_alloc_mutex, and this is
		 * an allocation site.
		 *
		 * The two other callers take it; this one did not, so two
		 * threads writing the same logical block each allocated a
		 * physical one and the second overwrote the first's
		 * pointer. The overwritten block stayed marked used with
		 * nothing referencing it -- 36 leaked out of 317559
		 * allocations under generic/344, which runs holetest with
		 * 256 threads on one sparse file. A narrow window, hit
		 * often enough by 256 threads.
		 */
		bool fresh = false;

		/*
		 * Not on a failed volume.
		 *
		 * beamfs_fail says "refusing further writes" and nothing in
		 * this file asked: a write into a block the file already had
		 * was accepted, and one that needed a block was told ENOSPC by
		 * the allocator, which answers 0 for a failed volume as for a
		 * full one. generic/361 on aarch64, 2026-10-01: "pwrite: No
		 * space left on device" on a 1 GiB volume holding 520 MiB.
		 */
		if (beamfs_failed(inode->i_sb))
			return -EIO;

		mutex_lock(&fi->i_alloc_mutex);
		ret = beamfs_inline_lookup_or_alloc_phys_new(inode, b, &phys,
							     &fresh);
		mutex_unlock(&fi->i_alloc_mutex);

		/*
		 * BEAMFS_IOMAP_F_FRESH: this mapping did not exist before the call.
		 *
		 * iomap_end may only give back what this call created. A
		 * block the file already owned belongs to it whether or
		 * not the write that asked for the mapping reached it, and
		 * freeing that would lose data nobody asked to lose.
		 */
		if (ret == 0 && fresh)
			iomap->flags |= BEAMFS_IOMAP_F_FRESH;
	} else {
		ret = beamfs_inline_lookup_phys(inode, b, &phys);
	}
	/*
	 * The last place a wrong address can still be stopped.
	 *
	 * What goes into the iomap below becomes a bio, and a bio does
	 * not ask where it is going: an address under s_data_start lands
	 * on the inode table. generic/075 put fsx's fill bytes in blocks
	 * 7 and 246 that way and left no trace in any buffer head,
	 * because iomap does not use them.
	 *
	 * Checked here rather than at each place the two lookups return a
	 * block: one gate the address must pass, and one place to read when
	 * it does not.
	 */
	if (ret == 0 &&
	    !beamfs_phys_is_sane(inode->i_sb, inode, phys, "iomap_begin"))
		ret = -EUCLEAN;

	if (ret < 0)
		return ret;

	if (phys == 0) {
		iomap->type = IOMAP_HOLE;
		return 0;
	}

	iomap->type = IOMAP_MAPPED;
	/*
	 * Physical block start. Nothing derives a sector from it for I/O --
	 * read_folio_range reads the block itself -- but fiemap and the
	 * tracepoints report it, so it has to be the real address.
	 */
	iomap->addr = (u64)phys * BEAMFS_BLOCK_SIZE;

	(void)srcmap;
	return 0;
}

/*
 * Give back what a short write allocated and did not use.
 *
 * iomap_begin allocates a block per call as it walks the mapping, and
 * a write that stops short -- ENOSPC, a signal, an error partway --
 * leaves the rest of them allocated and named by nothing. generic/013
 * ends with ten such blocks in two runs, the longer of nine: one
 * write's worth.
 *
 * Only what this mapping created, which BEAMFS_IOMAP_F_FRESH says. A
 * block the file already owned belongs to it whether or not this write
 * reached it, and freeing that loses data nobody asked to lose.
 *
 * Rounded up to the next block: a partially written block is kept
 * whole, because the bytes before the stopping point are the user's.
 */
static int beamfs_inline_iomap_end(struct inode *inode, loff_t pos,
				   loff_t length, ssize_t written,
				   unsigned int flags, struct iomap *iomap)
{
	u32 payload = beamfs_block_payload(inode->i_sb);
	u64 first_unused;
	loff_t reached;

	if (!(flags & IOMAP_WRITE) || !(iomap->flags & BEAMFS_IOMAP_F_FRESH))
		return 0;
	if (written < 0 || written >= length)
		return 0;

	/*
	 * The first block entirely past what the write reached. Anything
	 * from here to the end of the mapping was allocated by it and
	 * never filled.
	 */
	reached = pos + written;
	first_unused = DIV_ROUND_UP_ULL((u64)reached, payload);

	if ((loff_t)(first_unused * payload) >= pos + length)
		return 0;

	/*
	 * Only past the end of the file.
	 *
	 * free_blocks_from frees everything from a block onwards, which
	 * is what a truncate wants and not what this does. A write that
	 * fails at offset zero on a file that already holds data would
	 * take all of it: the mapping is new, the blocks past it are
	 * not.
	 *
	 * i_size is what iomap has already set from what was written, so
	 * anything beyond it is out of the file by definition.
	 */
	{
		loff_t size = i_size_read(inode);
		u64 first_past_eof = DIV_ROUND_UP_ULL((u64)size, payload);

		if (first_unused < first_past_eof)
			return 0;
	}

	mutex_lock(&BEAMFS_I(inode)->i_alloc_mutex);
	beamfs_inline_free_blocks_from(inode, first_unused);
	mutex_unlock(&BEAMFS_I(inode)->i_alloc_mutex);

	return 0;
}

static const struct iomap_ops beamfs_inline_iomap_ops = {
	.iomap_begin = beamfs_inline_iomap_begin,
	.iomap_end   = beamfs_inline_iomap_end,
};

/*
 * Read one mapped range into the folio, decoding it on the way.
 *
 * iomap has already resolved which INLINE block backs this file range;
 * what is left is to fetch the physical block, run the 16 RS codewords
 * through the decoder, and copy out the slice the folio asked for. The
 * read is synchronous, as iomap_bio_read_folio_range_sync() shows is
 * expected, so iomap_finish_folio_read() is called before returning.
 */
static int beamfs_inline_read_folio_range(const struct iomap_iter *iter,
					  struct iomap_read_folio_ctx *ctx,
					  size_t len)
{
	struct inode       *inode = iter->inode;
	struct super_block *sb    = inode->i_sb;
	struct folio       *folio = ctx->cur_folio;
	loff_t              i_size = i_size_read(inode);
	u64                 pos   = (u64)iter->pos;
	u64                 end   = pos + len;
	size_t              done  = 0;
	int                 ret   = 0;

	/*
	 * The range can straddle INLINE blocks, so walk them: for each,
	 * read the physical block, run its 16 RS codewords through the
	 * decoder, and copy out the slice this folio wants.
	 */
	/*
	 * Past EOF within this range: zero it here rather than returning a
	 * second HOLE mapping. iomap releases the folio as soon as a call
	 * returns without an iomap_folio_state attached -- always the case
	 * at 4096-byte folios and blocks -- so a folio must be covered by
	 * exactly one mapping. Traced on 2026-08-29: a 1000-byte file
	 * produced MAPPED [0, 0x3e8) then HOLE [0x3e8, 0x1000), and the
	 * second iteration dereferenced the released folio.
	 */
	if (end > (u64)i_size) {
		u64 eof = max_t(u64, pos, (u64)i_size);

		folio_zero_range(folio, offset_in_folio(folio, eof),
				 (size_t)(end - eof));
		end = eof;
	}

	while (pos < end) {
		u32    payload      = beamfs_block_payload(inode->i_sb);
		u64    b            = div_u64(pos, payload);
		u32    slice_offset = (u32)(pos - b * payload);
		u32    slice_length = (u32)min_t(u64, end - pos,
						 payload - slice_offset);
		size_t folio_off    = offset_in_folio(folio, pos);
		u64    phys = 0;
		u8    *dst;

		/*
		 * The tree read under the allocator's mutex.
		 *
		 * lookup_phys walks i_direct and the indirect blocks while
		 * writeback, truncate and eviction are free to rewrite them.
		 * Reading a pointer mid-update returns a block number that
		 * belongs to no one, or to another file once it has been
		 * reallocated. ext2 holds truncate_mutex across the same walk.
		 *
		 * Held for the lookup only, released before the read that
		 * follows: the block I/O may sleep and has no business
		 * holding up the allocator.
		 */
		mutex_lock(&BEAMFS_I(inode)->i_alloc_mutex);
		ret = beamfs_inline_lookup_phys(inode, b, &phys);
		mutex_unlock(&BEAMFS_I(inode)->i_alloc_mutex);
		if (ret < 0)
			break;

		if (phys == 0) {
			folio_zero_range(folio, folio_off, slice_length);
			pos  += slice_length;
			done += slice_length;
			continue;
		}

		/*
		 * Not sb_bread, and that is the whole point.
		 *
		 * __bread_gfp adds __GFP_NOFAIL whatever mask it is given,
		 * so this read could not fail -- it looped in the
		 * allocator instead. On a machine whose page cache is full
		 * of this filesystem's own blocks, that is a machine that
		 * stops: captured 2026-09-18 with the node at 270% CPU,
		 * kswapd running, eight tasks on this exact line and
		 * nothing advancing, with generic/083 under -I crc.
		 *
		 * The cache is consulted first and never created. A block
		 * with a dirty buffer still holds the newest bytes -- the
		 * medium has the previous ones until writeback lands -- so
		 * reading past it would hand back stale data. When there
		 * is no buffer, the block is read straight into a scratch
		 * page: one bounded allocation that can fail cleanly,
		 * instead of an unbounded one that cannot fail at all.
		 *
		 * A capsule is exactly BEAMFS_BLOCK_SIZE and the scratch
		 * pool hands out exactly that, so the block lands in one
		 * piece with no bounce.
		 */
		{
			struct buffer_head *cached = sb_find_get_block(sb, phys);
			u8 *raw;

			if (cached) {
				raw = beamfs_scratch_get(sb);
				if (!raw) {
					brelse(cached);
					ret = -ENOMEM;
					break;
				}
				lock_buffer(cached);
				memcpy(raw, cached->b_data, BEAMFS_BLOCK_SIZE);
				unlock_buffer(cached);
				brelse(cached);
			} else {
				raw = beamfs_scratch_get(sb);
				if (!raw) {
					ret = -ENOMEM;
					break;
				}
				ret = bdev_rw_virt(sb->s_bdev,
						   (sector_t)phys *
						   (BEAMFS_BLOCK_SIZE >> SECTOR_SHIFT),
						   raw, BEAMFS_BLOCK_SIZE,
						   REQ_OP_READ);
				if (ret) {
					pr_err_ratelimited("beamfs/inline: read_folio_range: read of phys=%llu failed: %d\n",
							   (unsigned long long)phys,
							   ret);
					beamfs_scratch_put(sb, raw);
					ret = -EIO;
					break;
				}
			}

			dst = kmap_local_folio(folio, folio_off);
			ret = beamfs_inline_decode_block_into_buf(sb, raw, phys,
								  inode, b, dst,
								  slice_offset,
								  slice_length,
								  false);
			kunmap_local(dst);
			beamfs_scratch_put(sb, raw);
			if (ret < 0)
				break;
		}

		pos  += slice_length;
		done += slice_length;
	}

	/*
	 * On failure, return without finishing the folio.
	 * iomap_read_folio_iter() propagates a non-zero return before
	 * counting the range as submitted, and iomap_read_folio() then
	 * ends the folio itself through iomap_read_end(). Calling
	 * iomap_finish_folio_read() here as well runs folio_end_read()
	 * on a folio iomap still owns -- with no iomap_folio_state
	 * attached, which is always the case at 4096-byte folios and
	 * blocks, that call is unconditional -- and the second end
	 * leaves any waiter parked in folio_wait_bit_common() forever.
	 *
	 * Observed on 2026-08-30 by the saturation protocol: a block with
	 * nine symbol errors in one subblock was correctly detected and
	 * journalled, but the read never returned EIO -- cat sat in
	 * uninterruptible sleep instead. The fail-closed half of the
	 * saturation contract was the casualty, not the detection half.
	 *
	 * iomap_bio_read_folio_range_sync() has the same shape: it
	 * returns the error and finishes nothing.
	 */
	if (ret < 0)
		return ret;

	iomap_finish_folio_read(folio, offset_in_folio(folio, iter->pos),
				len, 0);
	(void)done;
	return 0;
}

static const struct iomap_read_ops beamfs_inline_read_ops = {
	.read_folio_range = beamfs_inline_read_folio_range,
};

/*
 * No .readahead. iomap_readahead_iter() moves to the next folio only when
 * offset_in_folio(cur_folio, iter->pos) reaches zero, so it assumes
 * mappings line up with folio boundaries. INLINE mappings end on block
 * payload boundaries, 3808 or 3824 bytes apart, and folios are 4096
 * bytes, so that comparison never matches at the right moment:
 * iomap_read_folio_iter() clears cur_folio once the folio is full, the
 * transition to the next folio does not fire, and the following
 * iteration dereferences NULL. Confirmed twice on kernel 7.1.3.
 *
 * readahead is optional in address_space_operations. Aligning the format
 * to folios would mean moving parity out of the data block, which is what
 * lets a single read correct a flip without depending on a second block --
 * the property the filesystem exists for. Sequential reads simply go one
 * folio at a time through read_folio.
 *
 * read_folio: iomap resolves which INLINE block backs each
 * file range, then calls beamfs_inline_read_folio_range() to produce the
 * bytes. The folio locking and uptodate accounting are iomap's; what
 * stays here is the RS decode.
 */
static int beamfs_inline_read_folio(struct file *file, struct folio *folio)
{
	struct iomap_read_folio_ctx ctx = {
		.ops       = &beamfs_inline_read_ops,
		.cur_folio = folio,
	};

	iomap_read_folio(&beamfs_inline_iomap_ops, &ctx, NULL);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* iomap write path                                                          */
/*                                                                           */
/* Same shape as the read side. iomap owns the folio lifecycle -- locking,   */
/* dirty accounting, writeback tagging -- and beamfs owns what the bytes     */
/* look like on disk: the read-modify-write cycle that decodes a block,      */
/* splices the folio's contribution in, re-encodes all 16 RS codewords and   */
/* stamps the DATA_CSUM descriptor.                                          */
/* ------------------------------------------------------------------------- */

/*
 * Fill a folio range for a partial write. iomap calls this when a write
 * covers only part of a folio that is not uptodate, so the rest has to
 * come off disk first. The contract requires a synchronous read, which
 * is what the decode path does anyway.
 */
static int beamfs_inline_write_read_folio_range(const struct iomap_iter *iter,
						struct folio *folio,
						loff_t pos, size_t len)
{
	struct inode       *inode = iter->inode;
	struct super_block *sb    = inode->i_sb;
	loff_t              i_size = i_size_read(inode);
	u64                 p     = (u64)pos;
	u64                 end   = p + len;
	int                 ret   = 0;

	if (end > (u64)i_size) {
		u64 eof = max_t(u64, p, (u64)i_size);

		folio_zero_range(folio, offset_in_folio(folio, eof),
				 (size_t)(end - eof));
		end = eof;
	}

	while (p < end) {
		u32    payload      = beamfs_block_payload(inode->i_sb);
		u64    b            = div_u64(p, payload);
		u32    slice_offset = (u32)(p - b * payload);
		u32    slice_length = (u32)min_t(u64, end - p,
						 payload - slice_offset);
		size_t folio_off    = offset_in_folio(folio, p);
		struct buffer_head *bh;
		u64    phys = 0;
		u8    *dst;

		/*
		 * The tree read under the allocator's mutex.
		 *
		 * lookup_phys walks i_direct and the indirect blocks while
		 * writeback, truncate and eviction are free to rewrite them.
		 * Reading a pointer mid-update returns a block number that
		 * belongs to no one, or to another file once it has been
		 * reallocated. ext2 holds truncate_mutex across the same walk.
		 *
		 * Held for the lookup only, released before the read that
		 * follows: the block I/O may sleep and has no business
		 * holding up the allocator.
		 */
		mutex_lock(&BEAMFS_I(inode)->i_alloc_mutex);
		ret = beamfs_inline_lookup_phys(inode, b, &phys);
		mutex_unlock(&BEAMFS_I(inode)->i_alloc_mutex);
		if (ret < 0)
			break;

		if (phys == 0) {
			folio_zero_range(folio, folio_off, slice_length);
			p += slice_length;
			continue;
		}

		bh = sb_bread(sb, phys);
		if (!bh) {
			ret = -EIO;
			break;
		}

		dst = kmap_local_folio(folio, folio_off);
		lock_buffer(bh);
		ret = beamfs_inline_decode_block_into_buf(sb, bh->b_data, phys, inode,
							  b, dst, slice_offset,
							  slice_length, false);
		unlock_buffer(bh);
		kunmap_local(dst);
		brelse(bh);
		if (ret < 0)
			break;

		p += slice_length;
	}

	return ret;
}

static const struct iomap_write_ops beamfs_inline_write_ops = {
	.read_folio_range = beamfs_inline_write_read_folio_range,
};

/*
 * Writeback: encode into bounce pages, gather the pages into bios.
 *
 * How ext2 and ext4 write a file back, functionally: the flusher hands
 * the filesystem one folio after another; the filesystem resolves
 * where each block goes, appends the folio's pages to the bio it is
 * building while the blocks stay contiguous, and submits that bio
 * when they stop being contiguous, when it is full, or at the end of
 * the pass. Nothing waits per block, dozens of writes are in flight at
 * once, the device sees requests of 128 KiB rather than 4, and a
 * folio's writeback ends from the completion of the bio that carried
 * it. A block is a page, so no block is ever half a folio's and half
 * another's, and nothing is read back to be rewritten.
 *
 * beamfs cannot hand the folio's own pages to the device: a block
 * carries 3808 or 3824 bytes of payload alongside 256 bytes of
 * Reed-Solomon parity and its checksum and identity fields, so every
 * block has to be encoded into a bounce page first. And because the
 * payload is not 4096, a folio ends inside a block that the next folio
 * continues.
 *
 * What was here before did the encode through the block device's
 * buffer cache, one buffer_head per block, one bio per buffer_head,
 * and took the buffer lock for each: the block a folio ended in was
 * read back, decoded, merged, re-encoded and written, and then the
 * next folio did the same to the same block -- waiting first on the
 * lock, which the write in flight still held. Measured on 2026-09-22
 * under generic/074: 2 176 467 writes of one block, 1.12 ms each, one
 * in flight at a time, 4.8 MB/s, every boundary block written twice
 * and decoded once; ext2 on the same device ran the same test in 17
 * seconds against a budget beamfs could not fit in.
 *
 * Now the boundary block is not written until the folio that
 * continues it has arrived: its payload waits in wb->pend, the next
 * folio fills the rest, and the block is encoded once and queued
 * once. Encoded pages come from a pool, not the buffer cache, so no
 * buffer lock is held across anything that can sleep -- the rule whose
 * breach generic/076 turned into a four-party deadlock. Blocks
 * contiguous on the medium go into one bio of up to
 * 32 pages, submitted when the run breaks, when it is full, or from
 * writeback_submit at the end of the pass. Each folio carries a
 * counter of the blocks that hold its bytes; the bio completion
 * decrements it and finishes the folio at zero, so a folio whose two
 * blocks travel in two bios is finished exactly once.
 *
 * The buffer cache still holds aliases of data blocks: the zero image
 * a fresh allocation writes through sb_getblk, the copy a read-modify-
 * write took through sb_bread, the copy read_folio_range consults
 * first. Before an encoded page is queued, any alias is brought to the
 * same bytes and made clean, under its lock, so a reader that finds it
 * reads what the device will hold, and the block device's own flusher
 * does not later write the stale zeros over the data.
 */

/* 32 blocks of 4 KiB: one bio of 128 KiB, the size ext4 builds. */
#define BEAMFS_WB_MAX_BLOCKS 32

/*
 * One folio in flight. pending counts the blocks that carry bytes of
 * this folio and have not completed; the folio is finished at zero.
 * abandoned is set when writeback_range failed for this folio after
 * queueing some of its blocks: iomap ends such a folio itself, and a
 * second finish from here would end a writeback that is not ours.
 */
struct beamfs_wb_folio {
	struct inode *inode;
	struct folio *folio;
	size_t        len;
	refcount_t    pending;
	bool          abandoned;
};

/* One encoded block in a bio, and the folio or folios it came from. */
struct beamfs_wb_block {
	struct page            *page;
	struct beamfs_wb_folio *owner;
	struct beamfs_wb_folio *owner2;
};

/* A bio being built: contiguous encoded blocks, submitted together. */
struct beamfs_wb_ioend {
	struct bio            *bio;
	struct super_block    *sb;
	unsigned int           n;
	sector_t               next;
	struct beamfs_wb_block blk[BEAMFS_WB_MAX_BLOCKS];
};

/*
 * The state of one writeback pass, kept in wpc->wb_ctx from the first
 * folio to writeback_submit.
 */
struct beamfs_wb {
	struct super_block     *sb;
	struct beamfs_wb_ioend *ioend;
	/* The boundary block waiting for the folio that continues it. */
	bool                    have_pend;
	u64                     pend_phys;
	u64                     pend_b;
	u32                     pend_filled;
	loff_t                  pend_next_pos;
	struct inode           *pend_inode;
	struct beamfs_wb_folio *pend_owner;
	u8                     *pend_payload;
	/* The first error a completion or a flush saw. */
	int                     error;
};

static void beamfs_wb_folio_done(struct beamfs_wb_folio *o)
{
	if (!o)
		return;
	if (refcount_dec_and_test(&o->pending)) {
		if (!o->abandoned)
			iomap_finish_folio_write(o->inode, o->folio, o->len);
		kfree(o);
	}
}

static void beamfs_wb_end_bio(struct bio *bio)
{
	struct beamfs_wb_ioend *io = bio->bi_private;
	struct beamfs_sb_info *sbi = BEAMFS_SB(io->sb);
	bool failed = bio->bi_status != BLK_STS_OK;
	unsigned int i;

	for (i = 0; i < io->n; i++) {
		struct beamfs_wb_block *blk = &io->blk[i];

		if (failed) {
			if (blk->owner)
				mapping_set_error(blk->owner->inode->i_mapping, -EIO);
			if (blk->owner2)
				mapping_set_error(blk->owner2->inode->i_mapping, -EIO);
		}
		mempool_free(blk->page, sbi->s_wb_pages);
		beamfs_wb_folio_done(blk->owner);
		beamfs_wb_folio_done(blk->owner2);
	}
	bio_put(bio);
	kfree(io);
}

static void beamfs_wb_submit_ioend(struct beamfs_wb *wb)
{
	struct beamfs_wb_ioend *io = wb->ioend;

	if (!io)
		return;
	wb->ioend = NULL;
	submit_bio(io->bio);
}

/*
 * A page for one encoded block.
 *
 * Without waiting first: the pool is shared by every flusher thread,
 * and a thread that waits on it while holding an unsubmitted bio full
 * of pool pages is waiting for pages that only its own submission can
 * return. So: try without waiting, then submit what is held, then
 * wait -- what is held is on its way back by then.
 */
static struct page *beamfs_wb_page(struct beamfs_wb *wb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(wb->sb);
	struct page *page;

	page = mempool_alloc(sbi->s_wb_pages, GFP_NOWAIT | __GFP_NOWARN);
	if (page)
		return page;
	beamfs_wb_submit_ioend(wb);
	return mempool_alloc(sbi->s_wb_pages, GFP_NOFS);
}

/*
 * Bring the buffer cache's alias of @phys, if there is one, to the
 * bytes the device is about to receive, and make it clean.
 *
 * The lock waits for a write of the alias already in flight -- the
 * zero image of a fresh block, most often -- so that write lands
 * before ours, whatever the device does with two requests for the
 * same sectors. Nothing else is held here.
 */
static void beamfs_wb_sync_alias(struct super_block *sb, u64 phys,
				 const void *block)
{
	struct buffer_head *bh = sb_find_get_block(sb, phys);

	if (!bh)
		return;
	lock_buffer(bh);
	memcpy(bh->b_data, block, BEAMFS_BLOCK_SIZE);
	set_buffer_uptodate(bh);
	clear_buffer_dirty(bh);
	unlock_buffer(bh);
	brelse(bh);
}

/*
 * Encode @payload into a fresh page and queue the page for @phys.
 *
 * Contiguous with the bio being built: appended. Otherwise, or when
 * the bio is full, that bio goes and a new one starts here.
 */
static int beamfs_wb_queue(struct beamfs_wb *wb, struct inode *inode,
			   u64 b, u64 phys, const u8 *payload,
			   struct beamfs_wb_folio *owner,
			   struct beamfs_wb_folio *owner2,
			   struct writeback_control *wbc)
{
	struct super_block *sb = wb->sb;
	sector_t sector = (sector_t)phys << (BEAMFS_BLOCK_SHIFT - SECTOR_SHIFT);
	struct beamfs_wb_ioend *io;
	struct page *page;
	u8 *block;
	int ret;

	page = beamfs_wb_page(wb);
	block = page_address(page);

	beamfs_lay_data_payload(sb, block, payload);
	/*
	 * The descriptor before the parity. In a capsule it is coded
	 * data: stamped after the seal, the next read corrected it back
	 * to what the page held when the parity was computed.
	 */
	beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), block, payload,
				     inode->i_ino, b);
	ret = beamfs_seal_data_block(sb, block);
	if (ret < 0) {
		pr_err_ratelimited("beamfs/inline: writeback: rs_encode_region failed for block %llu: %d\n",
				   (unsigned long long)phys, ret);
		mempool_free(page, BEAMFS_SB(sb)->s_wb_pages);
		return ret;
	}

	beamfs_wb_sync_alias(sb, phys, block);

	if (wb->ioend &&
	    (wb->ioend->n == BEAMFS_WB_MAX_BLOCKS || wb->ioend->next != sector))
		beamfs_wb_submit_ioend(wb);

	if (!wb->ioend) {
		io = kzalloc_obj(*io, GFP_NOFS);
		if (!io) {
			mempool_free(page, BEAMFS_SB(sb)->s_wb_pages);
			return -ENOMEM;
		}
		io->sb = sb;
		io->bio = bio_alloc(sb->s_bdev, BEAMFS_WB_MAX_BLOCKS,
				    REQ_OP_WRITE |
				    ((wbc && wbc->sync_mode == WB_SYNC_ALL) ?
				     REQ_SYNC : 0),
				    GFP_NOFS);
		io->bio->bi_iter.bi_sector = sector;
		io->bio->bi_private = io;
		io->bio->bi_end_io = beamfs_wb_end_bio;
		io->next = sector;
		wb->ioend = io;
	}
	io = wb->ioend;
	__bio_add_page(io->bio, page, BEAMFS_BLOCK_SIZE, 0);
	io->blk[io->n].page   = page;
	io->blk[io->n].owner  = owner;
	io->blk[io->n].owner2 = owner2;
	io->n++;
	io->next += BEAMFS_BLOCK_SIZE >> SECTOR_SHIFT;
	return 0;
}

/*
 * The payload a block holds now, for the bytes a partial write does
 * not touch. The buffer cache's alias first, the device otherwise --
 * not sb_bread, which would create an alias to keep coherent for a
 * block read once.
 */
static int beamfs_wb_read_old(struct super_block *sb, struct inode *inode,
			      u64 b, u64 phys, u8 *payload)
{
	struct buffer_head *bh = sb_find_get_block(sb, phys);
	u8 *raw = beamfs_scratch_get(sb);
	int ret;

	if (!raw) {
		if (bh)
			brelse(bh);
		return -ENOMEM;
	}
	if (bh && buffer_uptodate(bh)) {
		lock_buffer(bh);
		memcpy(raw, bh->b_data, BEAMFS_BLOCK_SIZE);
		unlock_buffer(bh);
		brelse(bh);
	} else {
		if (bh)
			brelse(bh);
		ret = bdev_rw_virt(sb->s_bdev,
				   (sector_t)phys << (BEAMFS_BLOCK_SHIFT - SECTOR_SHIFT),
				   raw, BEAMFS_BLOCK_SIZE, REQ_OP_READ);
		if (ret) {
			pr_err_ratelimited("beamfs/inline: writeback: read of block %llu for a partial write failed: %d\n",
					   (unsigned long long)phys, ret);
			beamfs_scratch_put(sb, raw);
			return -EIO;
		}
	}
	ret = beamfs_inline_decode_block_into_buf(sb, raw, phys, inode, b,
						  payload, 0,
						  BEAMFS_DATA_INLINE_BYTES,
						  true);
	beamfs_scratch_put(sb, raw);
	return ret < 0 ? ret : 0;
}

/*
 * The boundary block's continuation did not come: finish it with what
 * the block holds beyond the bytes written, and queue it.
 */
static int beamfs_wb_flush_pending(struct beamfs_wb *wb,
				   struct writeback_control *wbc)
{
	struct beamfs_wb_folio *owner = wb->pend_owner;
	u8 *old;
	int ret;

	if (!wb->have_pend)
		return 0;
	wb->have_pend = false;
	wb->pend_owner = NULL;

	old = beamfs_scratch_get(wb->sb);
	if (!old) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = beamfs_wb_read_old(wb->sb, wb->pend_inode, wb->pend_b,
				 wb->pend_phys, old);
	if (ret == 0) {
		memcpy(old, wb->pend_payload, wb->pend_filled);
		ret = beamfs_wb_queue(wb, wb->pend_inode, wb->pend_b,
				      wb->pend_phys, old, owner, NULL, wbc);
	}
	beamfs_scratch_put(wb->sb, old);
	if (ret == 0)
		return 0;
fail:
	/*
	 * The block is lost to this pass; the folio it belonged to is
	 * told, and not finished twice.
	 */
	if (owner) {
		mapping_set_error(owner->inode->i_mapping, ret);
		beamfs_wb_folio_done(owner);
	}
	if (!wb->error)
		wb->error = ret;
	return ret;
}

static struct beamfs_wb *beamfs_wb_get(struct iomap_writepage_ctx *wpc)
{
	struct beamfs_wb *wb = wpc->wb_ctx;

	if (wb)
		return wb;
	wb = kzalloc_obj(*wb, GFP_NOFS);
	if (!wb)
		return NULL;
	wb->sb = wpc->inode->i_sb;
	wb->pend_payload = beamfs_scratch_get(wb->sb);
	if (!wb->pend_payload) {
		kfree(wb);
		return NULL;
	}
	wpc->wb_ctx = wb;
	return wb;
}

/*
 * Map and encode one folio's range.
 *
 * Each slice of the range is one block. A slice that starts the block
 * and fills it is encoded and queued at once. A slice that starts the
 * block and stops short is the folio's last: its bytes wait in
 * wb->pend for the folio that continues it. A slice that starts inside
 * the block is that continuation when it matches what waits, and a
 * partial write over an existing block otherwise, which reads the
 * block first.
 *
 * The folio counts one block per slice and is finished when the last
 * of them completes.
 */
static ssize_t beamfs_inline_writeback_range(struct iomap_writepage_ctx *wpc,
					     struct folio *folio, u64 pos,
					     unsigned int len, u64 end_pos)
{
	struct inode             *inode = wpc->inode;
	struct super_block       *sb    = inode->i_sb;
	struct beamfs_inode_info *fi    = BEAMFS_I(inode);
	u32      payload = beamfs_block_payload(sb);
	u64      p       = pos;
	u64      end     = pos + len;
	struct beamfs_wb       *wb;
	struct beamfs_wb_folio *owner;
	u8      *scratch = NULL;
	unsigned int nslices, queued = 0;
	int      ret = 0;

	/*
	 * iomap_writeback_range() adds the return value to
	 * bytes_submitted only when wpc->iomap.type is not IOMAP_HOLE;
	 * left zeroed, every folio read as "nothing submitted" and iomap
	 * ended it itself, once too many.
	 */
	wpc->iomap.type   = IOMAP_MAPPED;
	wpc->iomap.offset = pos;
	wpc->iomap.length = len;
	wpc->iomap.bdev   = sb->s_bdev;

	wb = beamfs_wb_get(wpc);
	if (!wb)
		return -ENOMEM;

	/* One block per slice: the first block, then whole blocks. */
	nslices = (u32)(div_u64(end - 1, payload) - div_u64(pos, payload)) + 1;

	owner = kzalloc_obj(*owner, GFP_NOFS);
	if (!owner)
		return -ENOMEM;
	owner->inode = inode;
	owner->folio = folio;
	owner->len   = len;
	refcount_set(&owner->pending, (int)nslices);

	scratch = beamfs_scratch_get(sb);
	if (!scratch) {
		kfree(owner);
		return -ENOMEM;
	}

	/*
	 * A boundary block waiting for another folio, or another file,
	 * is not continued by this one.
	 */
	if (wb->have_pend &&
	    (wb->pend_inode != inode || wb->pend_next_pos != (loff_t)pos)) {
		ret = beamfs_wb_flush_pending(wb, wpc->wbc);
		if (ret < 0)
			goto out;
	}

	while (p < end) {
		u64    b            = div_u64(p, payload);
		u32    slice_offset = (u32)(p - b * payload);
		u32    slice_length = (u32)min_t(u64, end - p,
						 payload - slice_offset);
		size_t folio_off    = offset_in_folio(folio, p);
		u64    phys         = 0;
		u8    *src;

		{
			u64 before = 0;

			mutex_lock(&fi->i_alloc_mutex);
			(void)beamfs_inline_lookup_phys(inode, b, &before);
			ret = beamfs_inline_lookup_or_alloc_phys(inode, b,
								 &phys);
			mutex_unlock(&fi->i_alloc_mutex);
			if (ret < 0)
				break;
			if (before && phys && before != phys)
				pr_err("beamfs/leak: ino=%llu iblock=%llu %llu -> %llu\n",
				       (unsigned long long)inode->i_ino,
				       (unsigned long long)b,
				       (unsigned long long)before,
				       (unsigned long long)phys);
		}
		if (!phys) {
			pr_err_ratelimited("beamfs/inline: writeback: no block for ino=%llu iblock=%llu\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)b);
			ret = -EIO;
			break;
		}

		if (slice_offset != 0) {
			/* The continuation of the block that waits. */
			if (wb->have_pend && wb->pend_b == b &&
			    wb->pend_phys == phys &&
			    wb->pend_filled == slice_offset) {
				struct beamfs_wb_folio *first = wb->pend_owner;

				src = kmap_local_folio(folio, folio_off);
				memcpy(wb->pend_payload + slice_offset, src,
				       slice_length);
				kunmap_local(src);
				wb->pend_filled += slice_length;
				wb->have_pend = false;
				wb->pend_owner = NULL;
				if (wb->pend_filled < payload) {
					/*
					 * Still short: a block spanning three
					 * folios, which a 4096-byte folio
					 * cannot produce. Finished from the
					 * medium rather than deferred again,
					 * because a block has two owners here
					 * and no more.
					 */
					ret = beamfs_wb_read_old(sb, inode, b, phys,
								 scratch);
					if (ret < 0) {
						beamfs_wb_folio_done(first);
						break;
					}
					memcpy(scratch, wb->pend_payload,
					       wb->pend_filled);
					ret = beamfs_wb_queue(wb, inode, b, phys,
							      scratch, first, owner,
							      wpc->wbc);
				} else {
					ret = beamfs_wb_queue(wb, inode, b, phys,
							      wb->pend_payload,
							      first, owner,
							      wpc->wbc);
				}
				if (ret < 0) {
					beamfs_wb_folio_done(first);
					break;
				}
				queued++;
			} else {
				/* A partial write over an existing block. */
				if (wb->have_pend) {
					ret = beamfs_wb_flush_pending(wb, wpc->wbc);
					if (ret < 0)
						break;
				}
				ret = beamfs_wb_read_old(sb, inode, b, phys, scratch);
				if (ret < 0)
					break;
				src = kmap_local_folio(folio, folio_off);
				memcpy(scratch + slice_offset, src, slice_length);
				kunmap_local(src);
				ret = beamfs_wb_queue(wb, inode, b, phys, scratch,
						      owner, NULL, wpc->wbc);
				if (ret < 0)
					break;
				queued++;
			}
		} else if (slice_length == payload) {
			/* A whole block: nothing to preserve, nothing to wait for. */
			if (wb->have_pend) {
				ret = beamfs_wb_flush_pending(wb, wpc->wbc);
				if (ret < 0)
					break;
			}
			src = kmap_local_folio(folio, folio_off);
			memcpy(scratch, src, slice_length);
			kunmap_local(src);
			ret = beamfs_wb_queue(wb, inode, b, phys, scratch,
					      owner, NULL, wpc->wbc);
			if (ret < 0)
				break;
			queued++;
		} else {
			/* The folio ends inside this block: wait for the next. */
			if (wb->have_pend) {
				ret = beamfs_wb_flush_pending(wb, wpc->wbc);
				if (ret < 0)
					break;
			}
			src = kmap_local_folio(folio, folio_off);
			memcpy(wb->pend_payload, src, slice_length);
			kunmap_local(src);
			wb->have_pend      = true;
			wb->pend_phys      = phys;
			wb->pend_b         = b;
			wb->pend_filled    = slice_length;
			wb->pend_next_pos  = (loff_t)(p + slice_length);
			wb->pend_inode     = inode;
			wb->pend_owner     = owner;
			queued++;
		}

		p += slice_length;
	}

out:
	beamfs_scratch_put(sb, scratch);

	if (ret < 0) {
		/*
		 * iomap ends this folio itself when the range fails.
		 * Whatever was queued for it still writes, and must not
		 * finish the folio a second time: the slices never
		 * queued are taken off the count, and the owner is
		 * marked so the last completion frees it and does no
		 * more.
		 */
		owner->abandoned = true;
		if (nslices > queued &&
		    refcount_sub_and_test((int)(nslices - queued), &owner->pending))
			kfree(owner);
		if (!wb->error)
			wb->error = ret;
		return ret;
	}

	(void)end_pos;
	return (ssize_t)len;
}

/*
 * The end of the pass: the boundary block nobody continued is finished
 * from what the medium holds, and the bio being built goes.
 */
static int beamfs_inline_writeback_submit(struct iomap_writepage_ctx *wpc,
					  int error)
{
	struct beamfs_wb *wb = wpc->wb_ctx;
	int ret;

	if (!wb)
		return error;
	ret = beamfs_wb_flush_pending(wb, wpc->wbc);
	beamfs_wb_submit_ioend(wb);
	if (!error)
		error = ret < 0 ? ret : wb->error;
	beamfs_scratch_put(wb->sb, wb->pend_payload);
	kfree(wb);
	wpc->wb_ctx = NULL;
	return error;
}

static const struct iomap_writeback_ops beamfs_inline_writeback_ops = {
	.writeback_range  = beamfs_inline_writeback_range,
	.writeback_submit = beamfs_inline_writeback_submit,
};

/* ------------------------------------------------------------------------- */
/* writepages                                                                */
/*                                                                           */
/* iomap_writepages walks the dirty folios and hands each range to           */
/* beamfs_inline_writeback_range, which maps and allocates under the         */
/* per-inode i_alloc_mutex and encodes into bounce pages;                    */
/* beamfs_inline_writeback_submit ends the pass. See the writeback comment   */
/* above.                                                                    */
/* ------------------------------------------------------------------------- */
static int beamfs_inline_writepages(struct address_space *mapping,
				    struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = {
		.inode = mapping->host,
		.wbc   = wbc,
		.ops   = &beamfs_inline_writeback_ops,
	};

	return iomap_writepages(&wpc);
}

/* ------------------------------------------------------------------------- */
/* write_iter entry point                                                    */
/* ------------------------------------------------------------------------- */
static ssize_t beamfs_inline_file_write_iter(struct kiocb *iocb,
					     struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t before;
	ssize_t ret;

	/*
	 * generic_write_checks first, and it is not optional.
	 *
	 * It is what moves ki_pos to i_size when the file was opened
	 * O_APPEND, and it enforces the rlimit and the s_maxbytes bound
	 * along the way. Without it every append wrote at whatever
	 * position the descriptor happened to hold, which for a fresh
	 * open is zero: three four-byte appends left a four-byte file
	 * reading CCCC, and xfstests generic/069 reported "maybe corrupt
	 * O_APPEND" with one file holding twelve megabytes that belonged
	 * to its siblings.
	 *
	 * generic_file_write_iter calls it, which is why filesystems
	 * built on that never notice. iomap_file_buffered_write sits
	 * below it and expects the caller to have done the checks --
	 * xfs_file_write_checks exists for this, and says so in a
	 * comment: "that assigns ki_pos for O_APPEND".
	 */
	/*
	 * inode_lock, like every other buffered write in the tree.
	 *
	 * generic_file_write_iter takes it for ext2; ext4 and XFS take
	 * it by hand. Without it a truncate -- do_truncate holds
	 * inode_lock through setattr -- runs while this write is still
	 * populating the page cache of the same inode: i_size is written
	 * by both sides in no order, truncate_setsize drops folios this
	 * write is still creating, and the block tree is freed under a
	 * mapping iomap_begin has already handed out.
	 *
	 * generic/464 makes that the common case: sixteen processes doing
	 * pwrite -ftc on two hundred files chosen at random.
	 *
	 * i_alloc_mutex serialises the tree walks. It does not serialise
	 * the write against the truncate; that is what the inode lock is
	 * for.
	 */
	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	/*
	 * The set-user-ID and set-group-ID bits, and the times, before
	 * the data. generic_file_write_iter does this through
	 * file_remove_privs; iomap_file_buffered_write sits below it and
	 * leaves it to the caller, as XFS does in xfs_file_write_checks.
	 */
	ret = kiocb_modified(iocb);
	if (ret)
		goto out;

	before = i_size_read(inode);

	ret = iomap_file_buffered_write(iocb, from, &beamfs_inline_iomap_ops,
					&beamfs_inline_write_ops, NULL);
	if (ret <= 0)
		goto out;

	/*
	 * iomap_write_iter() states the division of labour outright:
	 * "Update the in-memory inode size after copying the data into the
	 * page cache. It's up to the file system to write the updated size
	 * to disk." It grows i_size and raises IOMAP_F_SIZE_CHANGED; the
	 * inode does not reach the medium unless someone here says so.
	 *
	 * Returning its result directly meant a write that extended a file
	 * without a following fsync had the new size in stat and the old
	 * one on disk. xfstests generic/169: write 5 bytes, fsync, write 5
	 * more, unmount -- the file comes back 5 bytes long. The first
	 * half of that test passes only because every write there is
	 * fsynced.
	 *
	 * generic_write_sync last, as fuse does: it honours O_SYNC and
	 * O_DSYNC on the kiocb, so a caller that asked for durability gets
	 * it, and one that did not pays nothing.
	 */
	if (i_size_read(inode) != before)
		inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(inode, current_time(inode));
	mark_inode_dirty(inode);

out:
	inode_unlock(inode);
	/*
	 * Outside the lock, as ext4 does: it may wait on the device and
	 * has no business holding the inode while it does.
	 */
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

/* ------------------------------------------------------------------------- */
/* Public ops structures                                                     */
/* ------------------------------------------------------------------------- */

const struct address_space_operations beamfs_inline_aops = {
	.read_folio       = beamfs_inline_read_folio,
	.writepages       = beamfs_inline_writepages,
	/*
	 * iomap_dirty_folio, not filemap_dirty_folio.
	 *
	 * filemap's marks the folio dirty and stops there. iomap's also
	 * allocates the iomap_folio_state and calls
	 * iomap_set_range_dirty, which is what iomap_find_dirty_range
	 * later reads to decide what to write back.
	 *
	 * Without it the folio is dirty to the VFS and clean to iomap:
	 * iomap_writeback_folio calls folio_start_writeback, finds no
	 * dirty range to hand to ->writeback_range, submits nothing --
	 * and nothing ever ends the writeback it just started. The folio
	 * stays that way for the life of the mount, and the next process
	 * to touch the file waits in folio_wait_writeback forever.
	 *
	 * Found through xfstests: rm sat in truncate_inode_partial_folio
	 * under beamfs_evict_inode for nine hours while the tracing said
	 * every folio that reached writeback_range was accounted for --
	 * correctly, because the one that mattered never got there.
	 *
	 * gfs2 declares the same, which is the shortest way to see it.
	 */
	.dirty_folio      = iomap_dirty_folio,
	/* Required once the folio state is iomap's: the kernel warns at
	 * compaction time without it.
	 */
	.migrate_folio    = filemap_migrate_folio,
	.release_folio    = iomap_release_folio,
	.invalidate_folio = iomap_invalidate_folio,
};

/*
 * Same as the non-inline path: generic_file_fsync() went away with
 * sync_mapping_buffers() and i_private_list. Metadata buffer_heads
 * owned by the inode are flushed through mmb_fsync().
 */
static int beamfs_inline_fsync(struct file *file, loff_t start, loff_t end,
			       int datasync)
{
	return simple_fsync(file, start, end, datasync);
}

const struct file_operations beamfs_inline_file_operations = {
	.llseek      = generic_file_llseek,
	.read_iter   = generic_file_read_iter,
	.write_iter  = beamfs_inline_file_write_iter,
	.mmap        = generic_file_mmap,
	.fsync       = beamfs_inline_fsync,
	.splice_read = filemap_splice_read,
	/*
	 * splice_write is what copy_file_range falls back to.
	 *
	 * The VFS has no .copy_file_range here and no .remap_file_range,
	 * so vfs_copy_file_range reaches do_splice_direct -- which needs
	 * an output side. Without it the syscall returns EINVAL, and fsx
	 * stops on its first COPY operation, taking generic/075 with it.
	 *
	 * iter_file_splice_write goes through write_iter, so the RS
	 * encode and the append handling apply to spliced data exactly as
	 * they do to written data. ext2 declares the same pair, which is
	 * why it never had this gap.
	 */
	.splice_write = iter_file_splice_write,
};

/* ------------------------------------------------------------------------- */
/* Truncate support                                                          */
/*                                                                           */
/* The allocator reaches every level: lookup_or_alloc_phys has allocation    */
/* sites for direct, indirect, double and triple indirect, so the write      */
/* path is not capped at 524 blocks. Measured 2026-08-20 on a freshly        */
/* formatted volume: a 64 MiB file, about 17500 blocks and well into double  */
/* indirect, was written and read back intact at 24 MB/s with no             */
/* EOPNOTSUPP. Capacity per file is BEAMFS_MAX_IBLOCK_TINDIRECT blocks of    */
/* payload: about 479 GiB at 3824 bytes per block, 477 GiB at 3808.          */
/*                                                                           */
/* beamfs_inline_free_blocks_from walks the same four levels, so a           */
/* truncate frees everything past the new end of the file.                   */
/* ------------------------------------------------------------------------- */

/*
 * beamfs_inline_free_blocks_from - free disk blocks at logical iblock
 *                                  >= b_first_freed.
 *
 * Mirror of beamfs_free_data_blocks (super.c) restricted to a starting
 * logical block, used for truncate-down and by iomap_end to give back
 * what a short write allocated. Walks direct[b_first_freed..N-1], then
 * the single indirect block, then the double and triple indirect trees
 * through beamfs_free_ind_range. If b_first_freed <= BEAMFS_DIRECT_BLOCKS
 * the indirect block itself is freed; otherwise individual indirect slots
 * are zeroed and the indirect block is kept.
 *
 * Callers hold inode_lock and i_alloc_mutex: this rewrites the tree the
 * allocator walks.
 */
static void beamfs_inline_free_blocks_from(struct inode *inode,
					   u64 b_first_freed)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	unsigned int              i;

	/* --- Direct blocks --- */
	for (i = b_first_freed; i < BEAMFS_DIRECT_BLOCKS; i++) {
		u64 blk = le64_to_cpu(fi->i_direct[i]);

		if (blk) {
			beamfs_free_block(sb, blk, inode);
			fi->i_direct[i] = 0;
		}
	}

	/* --- Single indirect block --- */
	if (fi->i_indirect) {
		u64                 indirect_blk = le64_to_cpu(fi->i_indirect);
		struct buffer_head *ibh = NULL;
		__le64             *ptrs;
		u64                 nptrs = BEAMFS_INDIRECT_PTRS;
		u64                 slot_first;
		u64                 j;

		ibh = sb_bread(sb, indirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: truncate: failed to read indirect block %llu\n",
					   (unsigned long long)indirect_blk);
			return;
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
			pr_err_ratelimited("beamfs/inline: truncate: indirect block %llu failed parity, subtree left allocated\n",
					   (unsigned long long)indirect_blk);
			return;
		}
		ptrs = (__le64 *)ibh->b_data;

		if (b_first_freed >= BEAMFS_DIRECT_BLOCKS)
			slot_first = b_first_freed - BEAMFS_DIRECT_BLOCKS;
		else
			slot_first = 0;

		/*
		 * Under the buffer lock, as the install side is.
		 *
		 * Zeroing a pointer is a store into a shared indirect
		 * block, no different from installing one: the flusher
		 * can be submitting that same buffer while the loop runs,
		 * and a slot cleared halfway through the write reaches
		 * the disk as neither the old value nor zero. The three
		 * install sites take the lock; the two free sites did not.
		 *
		 * beamfs_free_block sleeps -- write_bitmap_block takes
		 * lock_buffer on a bitmap block -- so the pointers are
		 * collected under the lock first and freed after it, one
		 * pass each, rather than sleeping with the buffer held.
		 */

		{
			u64 *doomed;
			u64 n_doomed = 0;

			doomed = kvmalloc_array(nptrs, sizeof(*doomed),
						GFP_NOFS);
			if (!doomed) {
				brelse(ibh);
				return;
			}

			lock_buffer(ibh);
			for (j = slot_first; j < nptrs; j++) {
				u64 blk = le64_to_cpu(ptrs[j]);

				if (blk) {
					doomed[n_doomed++] = blk;
					beamfs_tc_clear(sb, ibh->b_blocknr, (u32)j);
					ptrs[j] = 0;
				}
			}
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			mmb_mark_buffer_dirty(ibh,
					      &BEAMFS_I(inode)->i_metadata_bhs);
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
			brelse(ibh);

			for (j = 0; j < n_doomed; j++)
				beamfs_free_block(sb, doomed[j], inode);
			kvfree(doomed);
		}

		/* If we freed the entire indirect range, drop the indirect
		 * block itself.
		 */
		if (slot_first == 0) {
			beamfs_free_block(sb, indirect_blk, inode);
			fi->i_indirect = 0;
		}
	}

	/*
	 * Double and triple indirect. The allocator reaches both, so a
	 * truncate stopping at single indirect leaves every block a file
	 * held past 2 MiB allocated with nothing left pointing at it.
	 *
	 * skip counts data blocks to preserve within each subtree: zero
	 * once the truncation point is below the level, otherwise the
	 * offset into it. A subtree entirely below the point is skipped
	 * whole, which is why the walk returns its span.
	 */
	/*
	 * The truncation point goes in as an absolute logical index, and
	 * the pointer is cleared only if the walk reports the whole
	 * subtree gone. Deriving that from the truncation point instead
	 * was how a partially freed subtree ended up with a null parent
	 * pointer and its surviving blocks orphaned.
	 */
	if (fi->i_dindirect &&
	    beamfs_free_ind_range(sb, le64_to_cpu(fi->i_dindirect), 2,
				  BEAMFS_MAX_IBLOCK_INDIRECT, b_first_freed, inode))
		fi->i_dindirect = 0;

	if (fi->i_tindirect &&
	    beamfs_free_ind_range(sb, le64_to_cpu(fi->i_tindirect), 3,
				  BEAMFS_MAX_IBLOCK_DINDIRECT, b_first_freed, inode))
		fi->i_tindirect = 0;

	mark_inode_dirty(inode);
}

/*
 * beamfs_inline_zero_tail_block - zero user bytes [zero_offset .. end of
 *                                 payload) in INLINE disk block b, via
 *                                 RMW + RS re-encode.
 *
 * Used when truncate-down lands inside a block: the surviving block must
 * be valid (RS-encoded) with stale tail bytes zeroed. The block is
 * decoded into scratch, the tail is memset to zero, and the block is
 * re-encoded and synced.
 *
 * Caller holds inode_lock. i_alloc_mutex is taken around the lookup;
 * the read-modify-write itself runs under the buffer lock.
 */
static int beamfs_inline_zero_tail_block(struct inode *inode, u64 b,
					 u32 zero_offset)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	struct buffer_head       *bh;
	u8                       *scratch;
	u64                       phys = 0;
	int                       ret;

	if (zero_offset >= BEAMFS_DATA_INLINE_BYTES)
		return 0;

	{
		u64 before = 0;

		mutex_lock(&fi->i_alloc_mutex);
		(void)beamfs_inline_lookup_phys(inode, b, &before);
		ret = beamfs_inline_lookup_or_alloc_phys(inode, b, &phys);
		/*
		 * The same gate: this one reads the block and writes it
		 * back, so a wrong address here overwrites metadata with
		 * a zeroed tail.
		 */
		if (ret == 0 &&
		    !beamfs_phys_is_sane(inode->i_sb, inode, phys, "zero_tail"))
			ret = -EUCLEAN;
		mutex_unlock(&fi->i_alloc_mutex);
		if (ret < 0)
			return ret;
		if (before && phys && before != phys)
			pr_err("beamfs/leak: zero_tail ino=%llu iblock=%llu %llu -> %llu\n",
			       (unsigned long long)inode->i_ino, (unsigned long long)b,
			       (unsigned long long)before,
			       (unsigned long long)phys);
	}
	if (phys == 0)
		return 0; /* HOLE: nothing to zero, sparse semantics */

	scratch = beamfs_scratch_get(sb);
	if (!scratch)
		return -ENOMEM;

	/*
	 * zero_tail is part of the RMW write path (truncate-induced tail
	 * clear). rmw_path=true so any flip detected on disk is journalled
	 * with RMW_NEUTRALISED, as writeback's read of a partial block is.
	 */
	bh = sb_bread(sb, phys);
	if (!bh) {
		pr_err_ratelimited("beamfs/inline: zero_tail: sb_bread phys=%llu failed\n",
				   (unsigned long long)phys);
		beamfs_scratch_put(sb, scratch);
		return -EIO;
	}
	/*
	 * Lock bh for the full RMW transit: readers copy this block under
	 * its lock, and writeback brings the same buffer up to date under it
	 * (beamfs_wb_sync_alias), so none of them sees it half rewritten.
	 */
	lock_buffer(bh);

	ret = beamfs_inline_decode_block_into_buf(sb, bh->b_data, phys, inode, b,
						  scratch, 0,
						  BEAMFS_DATA_INLINE_BYTES,
						  true);
	if (ret < 0) {
		unlock_buffer(bh);
		brelse(bh);
		beamfs_scratch_put(sb, scratch);
		return ret;
	}

	memset(scratch + zero_offset, 0,
	       BEAMFS_DATA_INLINE_BYTES - zero_offset);

	beamfs_lay_data_payload(sb, (u8 *)bh->b_data, scratch);
	/* The descriptor before the parity, as in beamfs_wb_queue. */
	beamfs_inline_stamp_tail_pad(BEAMFS_SB(inode->i_sb),
				     (u8 *)bh->b_data, scratch,
				     (unsigned long long)inode->i_ino, b);

	ret = beamfs_seal_data_block(sb, (u8 *)bh->b_data);
	if (ret < 0) {
		pr_err_ratelimited("beamfs/inline: zero_tail: rs_encode_region failed: %d\n",
				   ret);
		unlock_buffer(bh);
		brelse(bh);
		beamfs_scratch_put(sb, scratch);
		return ret;
	}

	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	ret = sync_dirty_buffer(bh);
	brelse(bh);
	beamfs_scratch_put(sb, scratch);
	return ret;
}

/*
 * beamfs_inline_setattr - VFS setattr hook for INLINE inodes.
 *
 * Handles ATTR_SIZE (truncate up and down). Truncate-up extends i_size
 * sparsely (HOLE handling in read_folio returns zero for unallocated
 * blocks). Truncate-down frees disk blocks beyond the new size and
 * zeros the partial trailing bytes in the surviving block.
 *
 * Other attribute changes (mode, owner, times) are forwarded to
 * setattr_copy + mark_inode_dirty.
 */
static int beamfs_inline_setattr(const struct mnt_idmap *idmap,
				 struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int           ret;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;

	if (attr->ia_valid & ATTR_SIZE) {
		loff_t old_size = i_size_read(inode);
		loff_t new_size = attr->ia_size;

		ret = inode_newsize_ok(inode, new_size);
		if (ret)
			return ret;

		if (new_size < old_size) {
			u64 b_first_freed;
			u32 tail_off;
			u32 payload = beamfs_block_payload(inode->i_sb);

			/* Drop pagecache beyond new_size before freeing
			 * the underlying disk blocks.
			 */
			truncate_setsize(inode, new_size);

			/*
			 * Logical block index of the first block to be
			 * fully freed: ceil(new_size / payload).
			 */
			b_first_freed = DIV_ROUND_UP_ULL((u64)new_size, payload);

			/* If new_size is not block-aligned, the surviving
			 * last block has stale user bytes beyond new_size.
			 * Zero them via RMW + RS re-encode.
			 */
			div_u64_rem((u64)new_size, payload, &tail_off);
			if (tail_off != 0 && new_size > 0) {
				u64 b_last_kept = div_u64((u64)new_size, payload);

				ret = beamfs_inline_zero_tail_block(inode,
								    b_last_kept,
								    tail_off);
				if (ret)
					return ret;
			}

			/*
			 * The same mutex the allocator takes.
			 *
			 * Truncate walks the same indirection tree that
			 * lookup_or_alloc_phys writes into, and took no
			 * lock at all. The trace caught it: pid 3212
			 * freeing block 0xe09 at 196.057356 between pid
			 * 3112 allocating it at 196.042955 and allocating
			 * it again at 196.058203 -- one writer's pointer
			 * overwritten by another's, and the block left
			 * marked used with nothing referencing it.
			 *
			 * generic/464 runs sixteen processes doing
			 * pwrite -ftc against two hundred files chosen at
			 * random, so a truncate on one file races an
			 * allocation on another constantly. The leak
			 * varied from 1 to 734 blocks between runs, which
			 * is what a race looks like when you count it.
			 */
			mutex_lock(&BEAMFS_I(inode)->i_alloc_mutex);
			beamfs_inline_free_blocks_from(inode, b_first_freed);
			mutex_unlock(&BEAMFS_I(inode)->i_alloc_mutex);
		} else if (new_size > old_size) {
			/* Sparse extension: just adjust i_size. read_folio
			 * returns zero for unallocated (HOLE) blocks.
			 */
			truncate_setsize(inode, new_size);
		}

		/*
		 * A truncate changes the file, so it changes mtime, and it
		 * changes the inode, so it changes ctime.
		 *
		 * setattr_copy below only carries the times the VFS asked
		 * for, and truncate asks for none: the filesystem is
		 * expected to set them itself, the way it does for a write.
		 * generic/313 reports all four cases -- ctime and mtime,
		 * shrinking and growing -- and says so in as many words.
		 *
		 * Both are set even when the size did not actually move,
		 * because setattr_prepare has already accepted the request
		 * and POSIX makes no exception for a truncate to the
		 * current length.
		 */
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	}

	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

/*
 * fiemap for the INLINE scheme.
 *
 * beamfs_fiemap in file.c calls iomap_fiemap with beamfs_iomap_ops --
 * the legacy scheme=5 map, where a logical block is 4096 bytes and disk
 * offset is file offset. Under INLINE a logical block is 3824 bytes
 * inside a 4096-byte physical one, so that map describes a geometry
 * this file does not have: generic/473 saw data starting at block 136
 * where it had written at 128, a drift of 272 bytes per block.
 *
 * What this reports is one extent per block, and that is not a
 * limitation of the code. fiemap merges two extents when
 * addr + length == next addr; here addr advances by 4096 while length
 * is at most the block's payload, 3808 or 3824 bytes, so the test never
 * holds and never can. The mapping from file offset to disk offset is
 * not affine, which is the price of putting each block's parity inside
 * the block: 288 or 272 bytes of every 4096 hold the parity and the
 * checks that protect the rest.
 *
 * So generic/473 still fails, and it fails for a reason worth stating
 * rather than hiding: a tool asking where a file lives now gets the
 * truth, block by block, instead of confident nonsense.
 */
static int beamfs_inline_fiemap(struct inode *inode,
				struct fiemap_extent_info *fieinfo,
				u64 start, u64 len)
{
	int ret;
	loff_t i_size;

	inode_lock(inode);
	i_size = i_size_read(inode);
	/* iomap_fiemap rejects len == 0; keep the call valid for an
	 * empty file, where it returns no extents.
	 */
	if (i_size == 0)
		i_size = 1;
	len = min_t(u64, len, i_size);
	ret = iomap_fiemap(inode, fieinfo, start, len,
			   &beamfs_inline_iomap_ops);
	inode_unlock(inode);
	return ret;
}

const struct inode_operations beamfs_inline_inode_operations = {
	.getattr        = beamfs_getattr,
	.setattr        = beamfs_inline_setattr,
	.fiemap         = beamfs_inline_fiemap,
};
