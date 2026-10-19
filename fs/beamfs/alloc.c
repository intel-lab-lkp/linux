// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Block and inode allocator
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Both block and inode allocators use in-memory bitmaps built at mount
 * time. No I/O is performed under the spinlock.
 *
 * Layout: the superblock is block 0 and records where each region
 * starts -- the inode table (s_inode_table_blk), the block bitmap
 * (s_bitmap_blk), the indirect-parity and error-budget regions when the
 * volume has them, and the data area (s_data_start_blk). The block
 * bitmap covers the data area only, one bit per block from
 * s_data_start; blocks below it are never allocated or freed here.
 *
 * Bitmap convention: bit set (1) = free, bit clear (0) = used.
 *
 * The block bitmap is kept on disk: s_bitmap_blocks_count blocks from
 * s_bitmap_blk, each holding BEAMFS_BITMAP_SUBBLOCKS (16) RS(255,239)
 * codewords of 239 data bytes, 30592 bits per block. It is decoded at
 * mount, a block that needed correction is written back at once, and
 * every later change re-encodes the one codeword holding the changed
 * bit. The inode bitmap has no on-disk form: it is rebuilt at mount
 * from the inode table, where a zero i_mode marks a free slot.
 *
 * The free counts, s_free_blocks and s_free_inodes, are taken from the
 * superblock at mount rather than recounted from the bitmaps, so a
 * power loss between a bitmap write and the superblock write can leave
 * them out of step with the bitmaps.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bitmap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "beamfs.h"
#include "beamfs_trace.h"

/* ------------------------------------------------------------------ */
/* Block bitmap                                                        */
/* ------------------------------------------------------------------ */

/*
 * beamfs_setup_bitmap - allocate and initialize in-memory bitmaps
 *
 * Called from beamfs_fill_super() after the superblock is read.
 *
 * Block bitmap: loaded from the s_bitmap_blocks_count on-disk bitmap
 * blocks starting at s_bitmap_blk. Each 239-byte subblock is
 * RS(255,239) FEC-protected. If a subblock is corrected, the event is
 * logged to the RS journal and the corrected bitmap block is written
 * back immediately.
 *
 * Inode bitmap: reconstructed by scanning the inode table for free slots
 * (i_mode == 0). This is O(total_inodes) but only at mount.
 */
int beamfs_setup_bitmap(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long total_blocks;
	unsigned long data_start;
	unsigned long total_inodes;
	unsigned long inode_table_blk;
	unsigned long inodes_per_block;
	unsigned long block, i;
	struct buffer_head *bh;
	struct beamfs_inode *raw;
	u64 bitmap_blk;
	u8 *bdata;
	bool corrected = false;

	/* --- Block bitmap --- */
	total_blocks = le64_to_cpu(sbi->s_beamfs_sb->s_block_count);
	data_start   = le64_to_cpu(sbi->s_beamfs_sb->s_data_start_blk);
	bitmap_blk   = le64_to_cpu(sbi->s_beamfs_sb->s_bitmap_blk);

	if (total_blocks <= data_start) {
		pr_err("beamfs: invalid block layout (total=%lu data_start=%lu)\n",
		       total_blocks, data_start);
		return -EINVAL;
	}

	if (bitmap_blk == 0 || bitmap_blk >= data_start) {
		pr_err("beamfs: invalid bitmap block %llu\n", bitmap_blk);
		return -EINVAL;
	}

	sbi->s_nblocks    = total_blocks - data_start;
	sbi->s_data_start = data_start;

	/*
	 * kvmalloc rather than bitmap_zalloc: one bit per data block means
	 * a 931 GiB volume asks for 29 MiB in one piece, and the page
	 * allocator has no run of contiguous pages that long once the
	 * machine has been up a while. The mount failed with ENOMEM on a
	 * VM with 1.7 GiB free -- not for want of memory, but for want of
	 * it in one stretch.
	 *
	 * vmalloc backing costs an extra page-table walk per access, which
	 * for a bitmap consulted under a spinlock is not measurable
	 * against the block I/O it guards. ext4 makes the same trade for
	 * its larger structures.
	 */
	sbi->s_block_bitmap = kvzalloc_objs(*sbi->s_block_bitmap,
					    BITS_TO_LONGS(sbi->s_nblocks),
					    GFP_KERNEL);
	if (!sbi->s_block_bitmap)
		return -ENOMEM;

	/*
	 * Multi-block bitmap layout: K = bitmap_blocks_count contiguous
	 * blocks starting at bitmap_blk. Each block protects 30592 data
	 * block bits via RS over BEAMFS_BITMAP_SUBBLOCKS (16) sub-blocks.
	 * K is decoded from s_flags bits 0..15; 0 means legacy 1.
	 */
	sbi->s_bitmap_blocks_count =
		beamfs_bitmap_blocks_count_from_flags(sbi->s_beamfs_sb->s_flags);

	sbi->s_bitmap_needs_encode =
		bitmap_zalloc(sbi->s_bitmap_blocks_count, GFP_KERNEL);
	if (!sbi->s_bitmap_needs_encode)
		return -ENOMEM;

	sbi->s_bitmap_blkhs = kcalloc(sbi->s_bitmap_blocks_count,
				      sizeof(*sbi->s_bitmap_blkhs), GFP_KERNEL);
	if (!sbi->s_bitmap_blkhs) {
		kvfree(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
		return -ENOMEM;
	}

	{
		u32 k;
		unsigned long bit_global = 0;
		unsigned long max_bit = sbi->s_nblocks;

		for (k = 0;
		     k < sbi->s_bitmap_blocks_count && bit_global < max_bit;
		     k++) {
			u64 disk_blk = bitmap_blk + (u64)k;
			int rs_results[BEAMFS_BITMAP_SUBBLOCKS];
			u8 rs_positions[BEAMFS_BITMAP_SUBBLOCKS *
					 (BEAMFS_RS_PARITY / 2)];
			unsigned long bit_local;

			bh = beamfs_bread(sb, disk_blk, "bitmap");
			if (!bh) {
				pr_err("beamfs: cannot read bitmap blk %llu (k=%u)\n",
				       disk_blk, k);
				while (k > 0) {
					k--;
					brelse(sbi->s_bitmap_blkhs[k]);
					sbi->s_bitmap_blkhs[k] = NULL;
				}
				kfree(sbi->s_bitmap_blkhs);
				sbi->s_bitmap_blkhs = NULL;
				bitmap_free(sbi->s_bitmap_needs_encode);
				sbi->s_bitmap_needs_encode = NULL;
				kvfree(sbi->s_block_bitmap);
				sbi->s_block_bitmap = NULL;
				return -EIO;
			}
			sbi->s_bitmap_blkhs[k] = bh;
			bdata = (u8 *)bh->b_data;

			beamfs_rs_decode_region(bdata, BEAMFS_SUBBLOCK_TOTAL,
						bdata + BEAMFS_SUBBLOCK_DATA,
						BEAMFS_SUBBLOCK_TOTAL,
						BEAMFS_SUBBLOCK_DATA,
						BEAMFS_BITMAP_SUBBLOCKS,
						rs_results, rs_positions,
						BEAMFS_RS_PARITY / 2,
						"bitmap");

			for (i = 0; i < BEAMFS_BITMAP_SUBBLOCKS; i++) {
				int rc = rs_results[i];

				if (rc < 0) {
					pr_err("beamfs: bmap blk %u sub %lu uncor\n",
					       k, i);
				} else if (rc > 0) {
					unsigned int np = (unsigned int)rc;
					u8 *pos = rs_positions +
						i * (BEAMFS_RS_PARITY / 2);

					if (np > BEAMFS_RS_PARITY / 2)
						np = BEAMFS_RS_PARITY / 2;
					pr_warn("beamfs: bmap blk %u sub %lu: %d corrected\n",
						k, i, rc);
					beamfs_log_rs_event_flagged(sb,
								    disk_blk,
						pos, np,
						BEAMFS_SUBBLOCK_DATA,
						beamfs_rs_event_subblock_bits(i));
					corrected = true;
				}
			}

			bit_local = 0;
			for (i = 0;
			     i < BEAMFS_BITMAP_SUBBLOCKS &&
			     bit_local < BEAMFS_BITS_PER_BITMAP_BLOCK &&
			     bit_global < max_bit; i++) {
				u8 *subdata = bdata + i * BEAMFS_SUBBLOCK_TOTAL;
				unsigned long b;

				for (b = 0;
				     b < BEAMFS_SUBBLOCK_DATA * 8 &&
				     bit_global < max_bit;
				     b++, bit_local++, bit_global++) {
					if (subdata[b / 8] & (1u << (b % 8)))
						set_bit(bit_global,
							sbi->s_block_bitmap);
					else
						clear_bit(bit_global,
							  sbi->s_block_bitmap);
				}
			}

			if (corrected) {
				mark_buffer_dirty(bh);
				/*
				 * A repair that does not land is a bitmap
				 * that decodes the same way on the next
				 * mount, and the same correction again.
				 */
				if (sync_dirty_buffer(bh))
					pr_err("beamfs: bitmap block %u repair did not reach the medium\n",
					       k);
				corrected = false;
			}
		}
	}

	/* --- Inode bitmap --- */
	total_inodes     = le64_to_cpu(sbi->s_beamfs_sb->s_inode_count);
	inode_table_blk  = le64_to_cpu(sbi->s_beamfs_sb->s_inode_table_blk);
	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);

	/*
	 * Start at the beginning: at mount the layout is whatever the
	 * previous session left, and there is no better guess.
	 */
	sbi->s_alloc_goal = 0;

	sbi->s_ninodes = total_inodes;

	sbi->s_inode_bitmap = kvzalloc_objs(*sbi->s_inode_bitmap,
					    BITS_TO_LONGS(total_inodes + 1),
					    GFP_KERNEL);
	if (!sbi->s_inode_bitmap) {
		u32 k;

		for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
			if (sbi->s_bitmap_blkhs[k])
				brelse(sbi->s_bitmap_blkhs[k]);
		}
		kfree(sbi->s_bitmap_blkhs);
		sbi->s_bitmap_blkhs = NULL;
		bitmap_free(sbi->s_bitmap_needs_encode);
		sbi->s_bitmap_needs_encode = NULL;
		kvfree(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
		return -ENOMEM;
	}

	for (block = 0; block * inodes_per_block < total_inodes; block++) {
		bh = beamfs_bread(sb, inode_table_blk + block, "bitmap");
		if (!bh) {
			pr_warn("beamfs: cannot read inode table block %lu at mount\n",
				inode_table_blk + block);
			continue;
		}

		raw = (struct beamfs_inode *)bh->b_data;

		for (i = 0; i < inodes_per_block; i++) {
			unsigned long ino = block * inodes_per_block + i + 1;

			if (ino > total_inodes)
				break;
			if (ino == 1)
				continue;
			if (le16_to_cpu(raw[i].i_mode) == 0)
				set_bit(ino, sbi->s_inode_bitmap);
		}
		brelse(bh);
	}

	pr_info("beamfs: bitmaps initialized (%lu data blocks, %lu free; "
		"%lu inodes, %lu free)\n",
		sbi->s_nblocks, sbi->s_free_blocks,
		total_inodes, sbi->s_free_inodes);

	return 0;
}

/*
 * Rebuild one subblock of a bitmap block, or all of them.
 *
 * @sub: which codeword to rebuild, or BEAMFS_BITMAP_SUBBLOCKS for all.
 * The buffer lock must be held.
 *
 * The whole-block version cost 30592 test_bit calls and sixteen RS
 * encodes for one bit changed. Filling 256 MiB means 70197 allocations,
 * so generic/015 spent two billion test_bit calls and 1.1 million RS
 * encodes in the allocator -- twenty minutes on a test that should take
 * one, with the flusher pinned at 92% of a core.
 *
 * Two things were wasteful and neither had to be. A bit belongs to
 * exactly one of the sixteen codewords, so changing it invalidates one
 * parity block, not sixteen. And the in-memory bitmap is already a bit
 * array in the same order as the on-disk one: memcpy moves it, where
 * the old loop asked about every bit individually.
 *
 * Per allocation this becomes one memcpy of 239 bytes and one RS
 * encode. The full-block path stays for beamfs_bitmap_encode_pending,
 * which rebuilds a whole block.
 */
static void beamfs_bitmap_encode_sub_locked(struct beamfs_sb_info *sbi,
					    u32 k, u32 sub,
					    struct buffer_head *bh)
{
	unsigned long first, last;
	u8 *bdata = (u8 *)bh->b_data;
	u32 i, lo, hi;

	first = (unsigned long)k * BEAMFS_BITS_PER_BITMAP_BLOCK;
	last  = min(first + BEAMFS_BITS_PER_BITMAP_BLOCK,
		    (unsigned long)sbi->s_nblocks);

	if (sub >= BEAMFS_BITMAP_SUBBLOCKS) {
		lo = 0;
		hi = BEAMFS_BITMAP_SUBBLOCKS;
		memset(bdata, 0, BEAMFS_BLOCK_SIZE);
	} else {
		lo = sub;
		hi = sub + 1;
	}

	for (i = lo; i < hi; i++) {
		u8 *subdata = bdata + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;
		unsigned long bit0 = first +
				     (unsigned long)i * BEAMFS_SUBBLOCK_DATA * 8;
		size_t nbytes;

		if (bit0 >= last) {
			memset(subdata, 0, BEAMFS_SUBBLOCK_DATA);
			continue;
		}

		/*
		 * Bit b of the volume lives at bit b - bit0 of this
		 * codeword, and bit0 is a multiple of 1912 -- which is a
		 * multiple of 8, so the copy is byte-aligned and memcpy
		 * does it.
		 */
		nbytes = min_t(size_t, BEAMFS_SUBBLOCK_DATA,
			       DIV_ROUND_UP(last - bit0, 8));
		memcpy(subdata, (const u8 *)sbi->s_block_bitmap + bit0 / 8,
		       nbytes);
		if (nbytes < BEAMFS_SUBBLOCK_DATA)
			memset(subdata + nbytes, 0,
			       BEAMFS_SUBBLOCK_DATA - nbytes);

		{
			int _e = beamfs_rs_encode_region(subdata,
					BEAMFS_SUBBLOCK_TOTAL,
					subdata + BEAMFS_SUBBLOCK_DATA,
					BEAMFS_SUBBLOCK_TOTAL,
					BEAMFS_SUBBLOCK_DATA, 1);

			/*
			 * An encode that fails leaves a block whose
			 * parity describes nothing, written with every
			 * check passing.
			 */
			if (_e < 0)
				pr_err_ratelimited("beamfs: bitmap subblock encode failed: %d\n",
						   _e);
		}
	}
}

/*
 * Rebuild one bitmap block's on-disk image, all sixteen codewords. The
 * buffer lock must be held.
 */
static void beamfs_bitmap_encode_one_locked(struct beamfs_sb_info *sbi, u32 k,
					    struct buffer_head *bh)
{
	beamfs_bitmap_encode_sub_locked(sbi, k, BEAMFS_BITMAP_SUBBLOCKS, bh);
}

/*
 * Rebuild the on-disk image of every bitmap block flagged in
 * s_bitmap_needs_encode, each under its buffer lock.
 *
 * Called from sync_fs and put_super, before the bitmap buffers are
 * written. beamfs_write_bitmap_block rebuilds the codeword it changes
 * as it dirties the buffer, and clears the flag; see there for why that
 * rebuild cannot wait for sync. s_block_bitmap is the authority and is
 * copied without s_lock; the buffer lock serialises against a
 * concurrent rebuild of the same block.
 */
void beamfs_bitmap_encode_pending(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u32 k;

	if (!sbi || !sbi->s_bitmap_blkhs || !sbi->s_bitmap_needs_encode ||
	    !sbi->s_block_bitmap)
		return;

	for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
		struct buffer_head *bh;

		if (!test_and_clear_bit(k, sbi->s_bitmap_needs_encode))
			continue;

		bh = sbi->s_bitmap_blkhs[k];
		if (!bh)
			continue;

		lock_buffer(bh);
		beamfs_bitmap_encode_one_locked(sbi, k, bh);
		unlock_buffer(bh);
	}
}

/*
 * beamfs_write_bitmap_block - bring the on-disk image of the bitmap
 *                             block holding @bit_global up to date,
 *                             and mark it dirty.
 *
 * @bit_global is the bit, counted from s_data_start, that the caller
 * has just changed in sbi->s_block_bitmap. Each subblock of a bitmap
 * block is an RS codeword of its own, so only the one holding that bit
 * is re-encoded. With @owner, the buffer also goes on the owner's
 * metadata list (see below).
 *
 * Called without s_lock: the buffer lock taken here can sleep.
 * Returns 0, -EIO when the volume has failed or the buffer's last
 * write failed, or -EINVAL when there is no bitmap block for
 * @bit_global.
 */
int beamfs_write_bitmap_block(struct super_block *sb,
			      unsigned long bit_global,
			      struct inode *owner)
{
	if (beamfs_failed(sb))
		return -EIO;

	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct buffer_head *bh;
	u32 k;

	if (!sbi->s_bitmap_blkhs || !sbi->s_block_bitmap)
		return -EINVAL;
	if (bit_global >= sbi->s_nblocks)
		return -EINVAL;

	k = (u32)(bit_global / BEAMFS_BITS_PER_BITMAP_BLOCK);
	if (k >= sbi->s_bitmap_blocks_count)
		return -EINVAL;

	bh = sbi->s_bitmap_blkhs[k];
	if (!bh)
		return -EINVAL;

	/*
	 * Rebuild the block's on-disk image now, under the lock.
	 *
	 * A deferred version of this lived here for a day: mark the block
	 * as needing a rebuild, do it in sync_fs, one rebuild per block
	 * touched instead of one per bit. It measured 25% faster on
	 * writes and a factor of four on deletes, and it was wrong.
	 *
	 * The needs_encode flag and the buffer's dirty bit are two states
	 * and the kernel knows only the second. Between mark_buffer_dirty
	 * below and sync_fs getting round to the rebuild, the flusher can
	 * write the buffer with its old contents and clear dirty; sync_fs
	 * then rebuilds an image nobody will ever write. fsck found
	 * 122368 blocks still marked used after 400 MiB was written and
	 * deleted, doubling with every cycle, while df reported the
	 * volume empty. The bitmap in memory was right and the one on
	 * disk was a lie.
	 *
	 * One codeword rebuilt -- a 239-byte copy and one RS encode --
	 * on every allocation and every free. That is what correctness
	 * costs here. Bringing it down means rebuilding less per change,
	 * not letting the buffer reach the disk stale.
	 */
	lock_buffer(bh);
	/*
	 * Not over a write that failed.
	 *
	 * The bitmap blocks are read at mount into buffers held until
	 * unmount, and the module never reads them again. A write of one
	 * that fails leaves its buffer not uptodate (end_buffer_write_sync
	 * clears the bit on an error): the medium holds an older bitmap
	 * than memory does. Dirtying the buffer anyway is what
	 * mark_buffer_dirty warns about. The superblock, the other buffer
	 * held for the mount, is treated this way in
	 * beamfs_dirty_super_now: the volume is failed and refuses further
	 * writes.
	 *
	 * generic/361 on aarch64, kernel 7.3-rc5, beamfs 0.1.23, on
	 * 2026-10-01: writes to loop0, the device under the volume,
	 * failed ("lost sync page write"), and within a millisecond came
	 * the WARNING at fs/buffer.c:991 from here, called by
	 * beamfs_alloc_block.
	 *
	 * Tested under the buffer lock: a write in flight holds it until
	 * its completion has set or cleared uptodate.
	 */
	if (!buffer_uptodate(bh)) {
		unlock_buffer(bh);
		beamfs_fail(sb, "write_bitmap_block", -EIO);
		return -EIO;
	}
	/*
	 * Only the codeword this bit belongs to. The other fifteen are
	 * untouched and their parity still holds.
	 */
	beamfs_bitmap_encode_sub_locked(sbi, k,
					(u32)((bit_global % BEAMFS_BITS_PER_BITMAP_BLOCK) /
		      (BEAMFS_SUBBLOCK_DATA * 8)),
		bh);
	clear_bit(k, sbi->s_bitmap_needs_encode);
	mark_buffer_dirty(bh);

	/*
	 * On the volume's shared metadata list, never the owner's.
	 *
	 * The bitmap block is shared by every inode allocating from it.
	 * Attached to the owner's i_metadata_bhs it moved from one inode's
	 * list to the next, and mmb_sync has a window -- b_mmb NULL under
	 * one inode's lock -- where mmb_mark_buffer_dirty under another
	 * inode's lock relinks the same b_assoc_buffers: list_del
	 * corruption, then a soft lockup holding mmb->lock (dbench,
	 * generic/241, x86-01, 2026-10-15). One list, one lock, no move.
	 *
	 * The bitmap-before-inode ordering that prevents the boot-time
	 * double-free -- boot N persisting an inode pointer without the
	 * matching bitmap clear, boot N+1 mounting a bitmap that declares
	 * a block free while an inode still references it -- is kept by
	 * beamfs_write_inode, which syncs s_shared_bhs before writing the
	 * inode. owner is NULL on the mount path and on the free path from
	 * beamfs_free_data_blocks; the block is on the list either way.
	 */
	mmb_mark_buffer_dirty(bh, &sbi->s_shared_bhs);
	if (owner && !(inode_state_read_once(owner) & I_FREEING)) {
		/*
		 * And the owner, or the list is never walked.
		 *
		 * The ordering above holds only if __writeback_single_inode
		 * runs, and the VFS runs it for an inode it believes is
		 * dirty. Attaching the bitmap block to an inode nothing
		 * marked leaves it in memory, which is the case this
		 * attachment exists to prevent.
		 */
		mark_inode_dirty(owner);
	}

	unlock_buffer(bh);
	return 0;
}

/*
 * beamfs_destroy_bitmap - free in-memory bitmaps at umount
 */
void beamfs_destroy_bitmap(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (sbi->s_bitmap_blkhs) {
		u32 k;

		for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
			if (sbi->s_bitmap_blkhs[k]) {
				brelse(sbi->s_bitmap_blkhs[k]);
				sbi->s_bitmap_blkhs[k] = NULL;
			}
		}
		kfree(sbi->s_bitmap_blkhs);
		sbi->s_bitmap_blkhs = NULL;
		bitmap_free(sbi->s_bitmap_needs_encode);
		sbi->s_bitmap_needs_encode = NULL;
	}
	if (sbi->s_block_bitmap) {
		kvfree(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
	}
	if (sbi->s_inode_bitmap) {
		kvfree(sbi->s_inode_bitmap);
		sbi->s_inode_bitmap = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Block allocation                                                    */
/* ------------------------------------------------------------------ */

/*
 * Reservation windows, after ext2.
 *
 * One cursor for the whole volume, advanced by whoever allocates and
 * pulled back by whoever frees, lays three files written at once down
 * as one interleaving: block n to the first, n+1 to the second, n+2
 * to the third. Measured under generic/074 on 2026-09-23, after the
 * writeback learned to gather contiguous blocks into one bio:
 * 1 054 631 bios of 1 069 059 still carried a single block, because no
 * two consecutive blocks of any file were adjacent on the medium, and
 * the device served those 4 KiB writes at 570 a second with the queue
 * seventeen deep. ext2 ran the same test in 17 s on the same device.
 *
 * ext2's answer (fs/ext2/balloc.c, rsv_window): a writer's goal is the
 * block after the last one it got, and each writer holds a window of
 * blocks, in memory only, where nobody else allocates. The window
 * starts small and doubles each time it is used up in order, so a
 * sequential writer's window grows to match it. Nothing about it is
 * on the disk; a window is discarded when the inode goes away, and
 * ignored when nothing free is left outside the windows.
 */
#define BEAMFS_RSV_MIN 16
#define BEAMFS_RSV_MAX 512

void beamfs_rsv_init(struct beamfs_inode_info *fi)
{
	INIT_LIST_HEAD(&fi->i_rsv_list);
	fi->i_rsv_start  = 0;
	fi->i_rsv_end    = 0;
	fi->i_rsv_next   = 0;
	fi->i_rsv_size   = BEAMFS_RSV_MIN;
	fi->i_last_alloc = 0;
}

static void beamfs_rsv_drop_locked(struct beamfs_inode_info *fi)
{
	if (!list_empty(&fi->i_rsv_list))
		list_del_init(&fi->i_rsv_list);
	fi->i_rsv_start = 0;
	fi->i_rsv_end   = 0;
	fi->i_rsv_next  = 0;
}

void beamfs_rsv_discard(struct inode *inode)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(inode->i_sb);

	spin_lock(&sbi->s_lock);
	beamfs_rsv_drop_locked(BEAMFS_I(inode));
	spin_unlock(&sbi->s_lock);
}

/* The end of another inode's window that covers @bit, or 0. */
static unsigned long beamfs_rsv_covering(struct beamfs_sb_info *sbi,
					 const struct beamfs_inode_info *me,
					 unsigned long bit)
{
	struct beamfs_inode_info *o;

	list_for_each_entry(o, &sbi->s_rsv_windows, i_rsv_list) {
		if (o == me)
			continue;
		if (bit >= o->i_rsv_start && bit < o->i_rsv_end)
			return o->i_rsv_end;
	}
	return 0;
}

/* The start of the nearest other window at or after @bit, or nblocks. */
static unsigned long beamfs_rsv_next_start(struct beamfs_sb_info *sbi,
					   const struct beamfs_inode_info *me,
					   unsigned long bit)
{
	struct beamfs_inode_info *o;
	unsigned long lim = sbi->s_nblocks;

	list_for_each_entry(o, &sbi->s_rsv_windows, i_rsv_list) {
		if (o == me)
			continue;
		if (o->i_rsv_start >= bit && o->i_rsv_start < lim)
			lim = o->i_rsv_start;
	}
	return lim;
}

/*
 * The first free block from @goal, wrapping once, that lies in no
 * other inode's window when @respect is set. s_nblocks when there is
 * none.
 */
static unsigned long beamfs_rsv_find(struct beamfs_sb_info *sbi,
				     const struct beamfs_inode_info *me,
				     unsigned long goal, bool respect)
{
	unsigned long n = sbi->s_nblocks;
	unsigned long bit = goal < n ? goal : 0;
	bool wrapped = (bit == 0);

	for (;;) {
		unsigned long end;

		bit = find_next_bit(sbi->s_block_bitmap, n, bit);
		if (bit >= n) {
			if (wrapped)
				return n;
			wrapped = true;
			bit = 0;
			continue;
		}
		end = respect ? beamfs_rsv_covering(sbi, me, bit) : 0;
		if (!end)
			return bit;
		/* Inside somebody's window: continue past it. */
		bit = end;
		if (bit >= n) {
			if (wrapped)
				return n;
			wrapped = true;
			bit = 0;
		}
	}
}

/*
 * A block for @fi: from its window while the window has one, else a
 * new window from its goal. Under s_lock. s_nblocks when none is free.
 */
static unsigned long beamfs_rsv_alloc_locked(struct beamfs_sb_info *sbi,
					     struct beamfs_inode_info *fi)
{
	unsigned long n = sbi->s_nblocks;
	unsigned long bit = n;
	unsigned long goal;
	bool exhausted;

	if (fi->i_rsv_end > fi->i_rsv_next) {
		bit = find_next_bit(sbi->s_block_bitmap, fi->i_rsv_end,
				    fi->i_rsv_next);
		if (bit >= fi->i_rsv_end)
			bit = n;
	}
	if (bit < n) {
		fi->i_rsv_next   = bit + 1;
		fi->i_last_alloc = bit;
		return bit;
	}

	/*
	 * A window used up in order earns a larger one; a window given
	 * up before that keeps its size.
	 */
	exhausted = fi->i_rsv_end != 0 && fi->i_rsv_next >= fi->i_rsv_end;
	beamfs_rsv_drop_locked(fi);

	goal = fi->i_last_alloc ? fi->i_last_alloc + 1 : sbi->s_alloc_goal;
	bit = beamfs_rsv_find(sbi, fi, goal, true);
	if (bit >= n)
		bit = beamfs_rsv_find(sbi, fi, goal, false);
	if (bit >= n)
		return n;

	if (exhausted && fi->i_rsv_size < BEAMFS_RSV_MAX)
		fi->i_rsv_size *= 2;
	fi->i_rsv_start = bit;
	fi->i_rsv_end   = min3(bit + fi->i_rsv_size,
			       beamfs_rsv_next_start(sbi, fi, bit + 1), n);
	fi->i_rsv_next   = bit + 1;
	fi->i_last_alloc = bit;
	list_add(&fi->i_rsv_list, &sbi->s_rsv_windows);
	return bit;
}

/*
 * beamfs_alloc_block - allocate a free data block
 *
 * Returns absolute block number (>= s_data_start) on success,
 * or 0 on failure (block 0 is the superblock, never a valid data block).
 * No I/O performed; bitmap is in memory.
 */
u64 beamfs_alloc_block(struct super_block *sb, struct inode *owner)
{
	/*
	 * A failed volume takes no more writes. See beamfs_fail: the
	 * device is gone, its buffers cannot be read back, and marking
	 * them dirty is what generic/338 catches with dm-error.
	 */
	if (beamfs_failed(sb))
		return 0;

	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit;

	if (!sbi->s_block_bitmap) {
		pr_err("beamfs: block bitmap not initialized\n");
		return 0;
	}

	spin_lock(&sbi->s_lock);

	if (sbi->s_free_blocks == 0) {
		spin_unlock(&sbi->s_lock);
		return 0;
	}

	/*
	 * From where the last one landed, then from the start.
	 *
	 * Two passes rather than one scan from zero: the first covers the
	 * region a sequential writer is actually using, the second the
	 * holes earlier writers left behind. Together they are exhaustive,
	 * so free_blocks > 0 still guarantees a hit.
	 */
	/*
	 * With an owner, from its reservation window (above). Without
	 * one -- the mount path, metadata with no inode -- from the
	 * volume's cursor, outside every window while that is possible.
	 */
	if (owner) {
		bit = beamfs_rsv_alloc_locked(sbi, BEAMFS_I(owner));
	} else {
		bit = beamfs_rsv_find(sbi, NULL, sbi->s_alloc_goal, true);
		if (bit >= sbi->s_nblocks)
			bit = beamfs_rsv_find(sbi, NULL, sbi->s_alloc_goal, false);
	}

	if (bit >= sbi->s_nblocks) {
		spin_unlock(&sbi->s_lock);
		pr_err("beamfs: bitmap inconsistency: free_blocks=%lu but no free bit\n",
		       sbi->s_free_blocks);
		return 0;
	}

	/*
	 * Next search starts after this one. Past the end it wraps, which
	 * the second pass above then handles.
	 */
	sbi->s_alloc_goal = bit + 1;
	if (sbi->s_alloc_goal >= sbi->s_nblocks)
		sbi->s_alloc_goal = 0;

	clear_bit(bit, sbi->s_block_bitmap);
	sbi->s_free_blocks--;
	sbi->s_beamfs_sb->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	/*
	 * st_blocks, once beamfs_getattr has counted the tree. Before
	 * that the count is wholly the tree's, and a free could take it
	 * below zero.
	 */
	if (owner && BEAMFS_I(owner)->i_blocks_counted)
		inode_add_bytes(owner, BEAMFS_BLOCK_SIZE);

	/*
	 * Rebuild the on-disk bitmap block from s_block_bitmap. Outside
	 * s_lock, because beamfs_write_bitmap_block takes the buffer lock,
	 * which can sleep.
	 */
	beamfs_write_bitmap_block(sb, bit, owner);

	trace_beamfs_block_alloc(sb->s_dev, owner ? owner->i_ino : 0,
				 (u64)(sbi->s_data_start + bit), 0);
	return (u64)(sbi->s_data_start + bit);
}

/*
 * beamfs_block_is_allocated -- is @block currently in use?
 *
 * Indirect blocks hold 512 pointers in exactly 4096 bytes, leaving no room
 * for a checksum, so unlike data blocks, inodes and the superblock they carry
 * no integrity field of their own. A flip in one corrupts a pointer, and the
 * bounds check at the read sites only rejects values outside the data area.
 * A corrupted pointer that lands inside it reaches an unrelated block and the
 * read proceeds. DATA_SELFID catches the case where that block belongs to
 * another file; this catches the case where it belongs to no file at all,
 * which is what a random flip on a 64-bit pointer usually produces.
 *
 * The in-memory bitmap uses 1 for free, so an allocated block has its bit
 * clear. It is held for the lifetime of the mount and is RS-protected on
 * disk, so consulting it costs a bit test and no I/O.
 */
bool beamfs_block_is_allocated(struct super_block *sb, u64 block)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit;

	/*
	 * Blocks below s_data_start -- the superblock, inode table,
	 * bitmap, root directory and canary among them -- are reserved
	 * and lie outside the bitmap, so it has no answer for them.
	 * Calling them unallocated made every legitimate pointer into
	 * that zone fail, starting with i_direct[0] of the root inode:
	 * the very first block resolved at mount was rejected, and no
	 * image built by mkfs.beamfs --from-dir could boot. They are
	 * answered as allocated, and beamfs_free_block skips them the
	 * same way. Whether a pointer may lead into that zone at all is
	 * for the caller to decide.
	 */
	if (block < sbi->s_data_start)
		return true;
	if (!sbi->s_block_bitmap)
		return true;   /* bitmap unavailable: do not reject */
	bit = (unsigned long)(block - sbi->s_data_start);
	if (bit >= sbi->s_nblocks)
		return false;

	return !test_bit(bit, sbi->s_block_bitmap);
}

/*
 * beamfs_free_block - return a data block to the free pool
 */
void beamfs_free_block(struct super_block *sb, u64 block, struct inode *owner)
{
	/*
	 * A failed volume takes no more writes. See beamfs_fail: the
	 * device is gone, its buffers cannot be read back, and marking
	 * them dirty is what generic/338 catches with dm-error.
	 */
	if (beamfs_failed(sb))
		return;

	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit;

	/* Layer 2 defense: silently skip reserved blocks. Any caller
	 * reaching this path with block < s_data_start is either a
	 * legitimate truncate of a reserved-pointing inode (canary, only
	 * possible if Layer 1 S_IMMUTABLE was bypassed by corruption) or
	 * a stale pointer from a corrupted indirect block. In both cases,
	 * silently rejecting preserves bitmap integrity without log spam.
	 */
	if (block < sbi->s_data_start)
		return;

	bit = (unsigned long)(block - sbi->s_data_start);
	if (bit >= sbi->s_nblocks) {
		pr_err("beamfs: block %llu out of range\n", block);
		return;
	}

	spin_lock(&sbi->s_lock);

	if (test_bit(bit, sbi->s_block_bitmap)) {
		/*
		 * Rate-limited, and no stack.
		 *
		 * A double free is worth knowing about, but a stack dump
		 * for one is a debugging aid that becomes a liability the
		 * moment the condition is reachable in normal operation.
		 * Under dm-error -- generic/338 -- writes fail by design,
		 * frees retry, and the trace lands in dmesg where
		 * _check_dmesg finds it and fails a test that was
		 * measuring something else entirely.
		 *
		 * The message still says which block, which is what a
		 * reader needs; whoever wants the caller can set a
		 * kprobe.
		 */
		pr_warn_ratelimited("beamfs: double free of block %llu\n",
				    block);
		spin_unlock(&sbi->s_lock);
		return;
	}
	trace_beamfs_block_free(sb->s_dev, owner ? owner->i_ino : 0,
				block, _RET_IP_);

	/*
	 * Pull the goal back to what was just freed, if it is behind.
	 *
	 * Without this a block freed before the cursor is only found on
	 * the next wrap, so a workload that fills, deletes and refills --
	 * generic/015 and generic/027 both do exactly that -- walks the
	 * whole bitmap again on every cycle instead of reusing what it
	 * just released.
	 */
	if (bit < sbi->s_alloc_goal)
		sbi->s_alloc_goal = bit;

	set_bit(bit, sbi->s_block_bitmap);
	sbi->s_free_blocks++;
	sbi->s_beamfs_sb->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	/* st_blocks: see beamfs_alloc_block. */
	if (owner && BEAMFS_I(owner)->i_blocks_counted)
		inode_sub_bytes(owner, BEAMFS_BLOCK_SIZE);

	/*
	 * A freed block's pointer may legitimately vanish, and a freed
	 * indirect block's slots go with it. Tell the checker before the
	 * block is handed to anybody else, or every ordinary truncate
	 * reads as a violation.
	 */
	beamfs_tc_forget_child(sb, block);
	beamfs_tc_forget_parent(sb, block);
	/* See alloc_block: rebuild the on-disk bitmap outside s_lock. */
	beamfs_write_bitmap_block(sb, bit, owner);
}

/* ------------------------------------------------------------------ */
/* Inode number allocation                                             */
/* ------------------------------------------------------------------ */

/*
 * beamfs_alloc_inode_num - allocate a free inode number
 *
 * Uses the in-memory inode bitmap. No I/O performed, no sb_bread under
 * spinlock.
 *
 * Returns inode number >= 2 on success (1 = root, always reserved),
 * or 0 on failure.
 */
u64 beamfs_alloc_inode_num(struct super_block *sb)
{
	/*
	 * A failed volume takes no more writes. See beamfs_fail: the
	 * device is gone, its buffers cannot be read back, and marking
	 * them dirty is what generic/338 catches with dm-error.
	 */
	if (beamfs_failed(sb))
		return 0;

	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit;

	if (!sbi->s_inode_bitmap) {
		pr_err("beamfs: inode bitmap not initialized\n");
		return 0;
	}

	spin_lock(&sbi->s_lock);

	if (sbi->s_free_inodes == 0) {
		spin_unlock(&sbi->s_lock);
		return 0;
	}

	/*
	 * Bits 0 and 1 are never set (inode 0 invalid, inode 1 = root reserved).
	 * find_next_bit starting at 2 skips both.
	 */
	bit = find_next_bit(sbi->s_inode_bitmap, sbi->s_ninodes + 1, 2);
	if (bit > sbi->s_ninodes) {
		spin_unlock(&sbi->s_lock);
		pr_err("beamfs: inode bitmap inconsistency: free_inodes=%lu but no free bit\n",
		       sbi->s_free_inodes);
		return 0;
	}

	clear_bit(bit, sbi->s_inode_bitmap);
	sbi->s_free_inodes--;
	sbi->s_beamfs_sb->s_free_inodes = cpu_to_le64(sbi->s_free_inodes);
	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	return (u64)bit;
}

/*
 * beamfs_free_inode_num - return an inode number to the free pool
 *
 * Called from evict_inode path when nlink drops to 0.
 */
void beamfs_free_inode_num(struct super_block *sb, u64 ino)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	/* Layer 2 defense: silently skip reserved inodes (root, canary).
	 * Range check covers both reserved range and bitmap bounds.
	 */
	if (beamfs_ino_is_reserved(ino) || ino > sbi->s_ninodes)
		return;

	spin_lock(&sbi->s_lock);

	if (test_bit((unsigned long)ino, sbi->s_inode_bitmap)) {
		pr_warn("beamfs: double free of inode %llu\n", ino);
		spin_unlock(&sbi->s_lock);
		return;
	}

	set_bit((unsigned long)ino, sbi->s_inode_bitmap);
	sbi->s_free_inodes++;
	sbi->s_beamfs_sb->s_free_inodes = cpu_to_le64(sbi->s_free_inodes);
	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);
}
