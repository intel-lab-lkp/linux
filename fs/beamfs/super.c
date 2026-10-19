// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Superblock operations
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/ktime.h>
#include <linux/statfs.h>
#include "beamfs.h"

/*
 * One translation unit defines the tracepoints; every other file just
 * includes the header. This one carries the definition because it is
 * always built.
 */
#define CREATE_TRACE_POINTS
#include "beamfs_trace.h"

/* Inode cache (slab allocator) */
static struct kmem_cache *beamfs_inode_cachep;

/*
 * alloc_inode - allocate a new inode with beamfs_inode_info embedded
 */
static struct inode *beamfs_alloc_inode(struct super_block *sb)
{
	struct beamfs_inode_info *fi;

	fi = kmem_cache_alloc(beamfs_inode_cachep, GFP_KERNEL);
	if (!fi)
		return NULL;

	memset(fi->i_direct, 0, sizeof(fi->i_direct));
	fi->i_indirect  = 0;
	fi->i_dindirect = 0;
	fi->i_tindirect = 0;
	fi->i_flags     = 0;
	fi->i_blocks_counted = false;
	mmb_init(&fi->i_metadata_bhs, &fi->vfs_inode.i_data);
	mutex_init(&fi->i_alloc_mutex);
	beamfs_rsv_init(fi);

	return &fi->vfs_inode;
}

/*
 * free_inode - return inode to slab cache
 */
static void beamfs_free_inode(struct inode *inode)
{
	mutex_destroy(&BEAMFS_I(inode)->i_alloc_mutex);
	kmem_cache_free(beamfs_inode_cachep, BEAMFS_I(inode));
}

/*
 * statfs - filesystem statistics
 */
static int beamfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block   *sb  = dentry->d_sb;
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	buf->f_type    = BEAMFS_MAGIC;
	buf->f_bsize   = sb->s_blocksize;
	buf->f_blocks  = le64_to_cpu(sbi->s_beamfs_sb->s_block_count);
	buf->f_bfree   = sbi->s_free_blocks;
	buf->f_bavail  = sbi->s_free_blocks;
	buf->f_files   = le64_to_cpu(sbi->s_beamfs_sb->s_inode_count);
	buf->f_ffree   = sbi->s_free_inodes;
	buf->f_namelen = BEAMFS_MAX_FILENAME;

	return 0;
}

/*
 * put_super - release superblock resources
 */
static void beamfs_put_super(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	/*
	 * The shared metadata list is written and emptied before the
	 * buffers on it are released: one left there keeps b_mmb pointing
	 * into sbi after kfree, for drop_buffers to follow later.
	 */
	if (sbi) {
		if (mmb_sync(&sbi->s_shared_bhs))
			pr_err("beamfs: put_super: shared metadata not written\n");
		mmb_invalidate(&sbi->s_shared_bhs);
	}

	if (sbi) {
		/* Stop the sweep before the structures it reads go away. */
		beamfs_scrub_exit(sb);
		beamfs_tc_exit(sbi);

		/*
		 * Rebuild, then write, then release -- in that order.
		 *
		 * The VFS calls sync_fs before put_super, so this is usually
		 * one pass over buffers that are already clean.
		 *
		 * The rebuild was here and the write was not, so
		 * beamfs_destroy_bitmap released the buffers with brelse
		 * and the images died in memory. Every block freed since
		 * the last sync stayed marked used on disk: fsck reported
		 * 122368 used-but-unreferenced blocks after one write of
		 * 400 MiB and its deletion, and the count doubled with each
		 * cycle. df was right and the disk was not.
		 *
		 * The superblock was written here from the start, which is
		 * why the free count looked correct while the bitmap
		 * underneath it did not.
		 */
		beamfs_bitmap_encode_pending(sb);
		beamfs_super_encode_pending(sbi);

		{
			unsigned int dirty = 0, written = 0;
			int first_err = 0;
			u32 k;

			for (k = 0; sbi->s_bitmap_blkhs &&
			     k < sbi->s_bitmap_blocks_count; k++) {
				struct buffer_head *bh = sbi->s_bitmap_blkhs[k];
				int rc;

				if (!bh || !buffer_dirty(bh))
					continue;
				dirty++;
				rc = sync_dirty_buffer(bh);
				if (rc) {
					if (!first_err)
						first_err = rc;
					pr_err("beamfs: umount: bitmap block %u did not reach the medium\n",
					       k);
				} else {
					written++;
				}
			}
			/*
			 * The last chance a freed block has to be recorded
			 * as free on the medium: nearly half the updates to
			 * the bitmap are attached to no inode and nothing
			 * else carries them here.
			 */
			trace_beamfs_bitmap_flush(sb->s_dev, "put_super",
						  dirty, written, first_err);
		}
		/*
		 * The superblock last, and loudly: a volume whose
		 * superblock did not land is one the next mount reads as
		 * it was before everything above.
		 */
		/* A clean unmount: the mounted flag comes off before the superblock goes. */
		if (!sb_rdonly(sb) && !READ_ONCE(sbi->s_failed) && sbi->s_beamfs_sb) {
			u32 fl = le32_to_cpu(sbi->s_beamfs_sb->s_flags);

			sbi->s_beamfs_sb->s_flags = cpu_to_le32(fl & ~BEAMFS_SB_FLAG_MOUNTED);
			beamfs_dirty_super_now(sbi);
		}
		if (sbi->s_sbh && buffer_dirty(sbi->s_sbh) &&
		    sync_dirty_buffer(sbi->s_sbh))
			pr_err("beamfs: umount: the superblock did not reach the medium\n");

		beamfs_destroy_bitmap(sb);
		mempool_destroy(sbi->s_wb_pages);
		sbi->s_wb_pages = NULL;
		mempool_destroy(sbi->s_scratch_pool);
		sbi->s_scratch_pool = NULL;
		kvfree(sbi->s_sb_rs_staging);
		sbi->s_sb_rs_staging = NULL;
		brelse(sbi->s_sbh);
		kfree(sbi->s_beamfs_sb);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
}

/*
 * seen_block -- linear search for a block id in the seen[] array.
 * Used by beamfs_free_data_blocks to free each direct block once when
 * the inode is corrupted -- a bit flip in a pointer can leave two
 * slots naming the same physical block.
 */
static bool seen_block(const u64 *seen, unsigned int n, u64 blk)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (seen[i] == blk)
			return true;
	}
	return false;
}

/*
 * beamfs_free_one_block -- free a data block, refusing a second free.
 *
 * A block that is already clear in the bitmap when we come to free it
 * means two pointers reached it: either a genuine aliasing bug or, more
 * likely here, a pointer flipped by a particle so that two slots name
 * the same physical block. Freeing it twice would clear a bit that a
 * live file now owns, turning a read error into silent cross-file
 * corruption -- which is the failure mode this filesystem exists to
 * prevent.
 *
 * The event is journalled rather than merely logged: an aliased pointer
 * discovered during teardown is exactly the forensic record the RS
 * journal is for, and dmesg alone leaves the run reading as an
 * unexplained space discrepancy.
 *
 * Returns true if the block was freed.
 */
static bool beamfs_free_one_block(struct super_block *sb, u64 blk)
{
	if (!blk)
		return false;

	if (!beamfs_block_is_allocated(sb, blk)) {
		pr_err_ratelimited("beamfs: refusing double free of block %llu\n",
				   (unsigned long long)blk);
		beamfs_log_rs_event_flagged(sb, blk, NULL, 0,
					    BEAMFS_SUBBLOCK_DATA, 0);
		return false;
	}

	beamfs_free_block(sb, blk, NULL);
	return true;
}

/*
 * beamfs_free_ind_range -- free part or all of an indirection subtree.
 *
 * @blk:    the indirect block itself
 * @depth:  1 = its pointers are data blocks
 *          2 = its pointers are single-indirect blocks
 *          3 = its pointers are double-indirect blocks
 * @base:   logical index of the first data block this subtree covers
 * @first:  logical index from which to free; everything below survives
 *
 * Returns true when the subtree is now empty and @blk itself has been
 * freed, false when something in it survived and @blk was kept.
 *
 * Absolute indices rather than a running countdown. The countdown
 * version shared one mutable counter across the whole recursion, mixing
 * "how much to preserve" with "how far we have got", so no call could
 * reason about its own range without knowing what its siblings had
 * consumed. It freed indirect blocks that still held live pointers;
 * the allocator handed them out as data blocks; a later lookup read
 * file contents as pointers. xfstests generic/013 found it as
 * phys=6148914691236517205 -- 0x5555... , fsstress payload.
 *
 * Here every subtree knows its own range by construction: child j of a
 * block at @base covers [base + j*span, base + (j+1)*span). Whether it
 * is wholly above @first, wholly below, or straddling follows from
 * arithmetic, with no state to get out of step.
 *
 * Holes are why survivors counts pointers rather than blocks: beamfs
 * allocates on demand, so a subtree can be sparsely populated and a
 * count derived from logical indices would not match what is actually
 * there.
 */
bool beamfs_free_ind_range(struct super_block *sb, u64 blk,
			   unsigned int depth, u64 base, u64 first,
			   struct inode *inode)
{
	u64 nptrs = BEAMFS_BLOCK_SIZE / sizeof(__le64);
	u64 child_span = 1;
	struct buffer_head *ibh;
	unsigned int j;
	u64 survivors = 0;
	bool dirtied = false;

	if (!blk)
		return true;

	for (j = 1; j < depth; j++)
		child_span *= nptrs;

	/* Wholly below the cut: nothing here is going away. */
	if (base + child_span * nptrs <= first)
		return false;

	ibh = beamfs_bread(sb, blk, "superblock");
	if (!ibh) {
		pr_err_ratelimited("beamfs: cannot read indirect block %llu, subtree left allocated\n",
				   (unsigned long long)blk);
		return false;
	}

	/*
	 * Clear the slots under the buffer lock, the way the install
	 * sites do.
	 *
	 * Zeroing a pointer is a store into a shared indirect block: the
	 * flusher can be submitting that buffer while the loop runs. But
	 * the work between reads sleeps -- the recursion, and
	 * free_one_block through the bitmap -- so the lock cannot be held
	 * across it. Decide first, then take the lock only to write the
	 * slots that are going away.
	 */
	for (j = 0; j < nptrs; j++) {
		__le64 *ptrs = (__le64 *)ibh->b_data;
		u64 child = le64_to_cpu(ptrs[j]);
		u64 child_base = base + (u64)j * child_span;

		if (!child)
			continue;

		if (depth > 1) {
			if (beamfs_free_ind_range(sb, child, depth - 1,
						  child_base, first, inode)) {
				lock_buffer(ibh);
				beamfs_tc_clear(sb, ibh->b_blocknr, (u32)j);
				ptrs[j] = 0;
				unlock_buffer(ibh);
				dirtied = true;
			} else {
				survivors++;
			}
			continue;
		}

		if (child_base < first) {
			survivors++;
			continue;
		}

		beamfs_free_one_block(sb, child);
		lock_buffer(ibh);
		beamfs_tc_clear(sb, ibh->b_blocknr, (u32)j);
		ptrs[j] = 0;
		unlock_buffer(ibh);
		dirtied = true;
	}

	if (dirtied) {
		/*
		 * The parity has to follow the block down.
		 *
		 * This is the only path that takes pointers out of an
		 * indirect block, and it left the parity describing the
		 * state before. A verify then "corrects" the block toward
		 * that older state and hands back a pointer that was
		 * cleared -- to a block the allocator has since given to
		 * another file.
		 *
		 * A frozen generic/083 volume shows it exactly: block
		 * 48682 holds one pointer on disk and its parity
		 * describes two, so reading it corrected produces 48683,
		 * which belongs to inode 1317. The checker reported a
		 * block shared between two inodes that share nothing.
		 *
		 * On the inode's metadata list for the same reason every
		 * other site is: a buffer dirtied and attached to nothing
		 * is never flushed by __writeback_single_inode.
		 */
		beamfs_ind_parity_update(sb, ibh, inode);
		if (inode) {
			mmb_mark_buffer_dirty(ibh,
					      &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * And the inode: a list nothing marks is a list
			 * write_inode never empties, and the comment
			 * above is exactly about a buffer that reaches
			 * the medium late.
			 */
			mark_inode_dirty(inode);
		} else {
			mark_buffer_dirty(ibh);
		}
	}
	/*
	 * Free the block only once nothing points out of it. A block with
	 * one surviving pointer is still load-bearing: freeing it hands
	 * the allocator a block whose live pointers a later lookup still
	 * follows.
	 */
	if (survivors == 0) {
		/*
		 * bforget, not brelse, for a block that is going away.
		 *
		 * ext2_free_branches ends exactly this way -- bforget(bh)
		 * and then ext2_free_blocks -- and beamfs, which carries
		 * the same i_metadata_bhs field and the same mmb_*
		 * helpers, called brelse. __bforget is clear_buffer_dirty,
		 * remove_assoc_queue and __brelse: the middle one is what
		 * was missing.
		 *
		 * Twenty lines above, this buffer was attached to the
		 * inode's metadata list. brelse leaves it there while the
		 * block goes back to the allocator, so the next owner gets
		 * that same buffer from sb_bread, writes its pointers into
		 * it -- and when the OLD inode is evicted, mmb_invalidate
		 * empties its list without writing anything, taking the
		 * new owner's pointers with it.
		 *
		 * generic/083 on 2026-09-18, three captures with the same
		 * shape. In the clearest, blocks 8890 and 8892 were freed
		 * from inode 20 at 34.563 and handed to inode 102 at
		 * 34.656; 8890 became an indirect block, 8892 its first
		 * child, and fsck found 8892 marked used and referenced by
		 * nothing -- with 92 of 4096 bytes set, so it had been
		 * written. No lost pointer was reported, because none was
		 * lost: the pointers were right and the block holding them
		 * never reached the medium.
		 *
		 * Only on this branch. A block with survivors keeps its
		 * buffer legitimately dirty and must still be written;
		 * clearing that is how an earlier attempt at this fix, one
		 * placed in beamfs_free_block where it caught all twenty-two
		 * callers, turned 58 leaked blocks into 345 and thousands
		 * of out-of-range pointers.
		 */
		bforget(ibh);
		beamfs_free_one_block(sb, blk);
		return true;
	}
	brelse(ibh);
	return false;
}

/*
 * beamfs_free_data_blocks -- release all data blocks of a deleted inode.
 *
 * Frees the direct blocks and the three indirection trees, or the data
 * block of a long symlink. Called from evict_inode, under
 * i_alloc_mutex, when nlink has dropped to 0.
 *
 * A bit flip landing on i_direct[] can leave two slots naming the same
 * physical block, and freeing it once per slot is a double free. A
 * direct block is freed only if no earlier slot named it, tracked in
 * seen[]; the indirection trees go through beamfs_free_one_block, which
 * refuses a block the bitmap already calls free.
 *
 * seen[] is allocated with GFP_NOFS, to avoid recursion into the
 * filesystem from this path. On OOM the direct blocks are freed without
 * deduplication, and beamfs_free_block then refuses, with a
 * rate-limited warning, a second free of a block still free.
 */
static void beamfs_free_data_blocks(struct inode *inode)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block      *sb = inode->i_sb;
	u64                     *seen;
	unsigned int             n_seen = 0;
	unsigned int             cap;
	u64                      nptrs = BEAMFS_BLOCK_SIZE / sizeof(__le64);
	int                      i;

	/*
	 * Symlinks come in two shapes and i_size tells them apart.
	 *
	 * Short ones keep the target in i_direct[] as raw bytes. Those
	 * bytes are not block pointers: freeing them would hand the
	 * allocator the ASCII of the path read as little-endian u64, which
	 * is how that bug presents. They own nothing, so there is nothing
	 * to free.
	 *
	 * Long ones keep the target in a data block with i_direct[0]
	 * holding its number, and that block has to go back. Returning
	 * early for every symlink -- correct while the short form was the
	 * only form -- would leak one block per long symlink, forever.
	 */
	if (S_ISLNK(inode->i_mode)) {
		if (i_size_read(inode) >= (loff_t)sizeof(fi->i_direct)) {
			u64 blk = le64_to_cpu(fi->i_direct[0]);

			if (blk)
				beamfs_free_block(sb, blk, NULL);
		}
		memset(fi->i_direct, 0, sizeof(fi->i_direct));
		return;
	}

	cap = BEAMFS_DIRECT_BLOCKS + (unsigned int)nptrs + 1;
	seen = kmalloc_array(cap, sizeof(*seen), GFP_NOFS);

	/* Free direct blocks */
	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
		u64 blk = le64_to_cpu(fi->i_direct[i]);

		if (blk && (!seen || !seen_block(seen, n_seen, blk))) {
			beamfs_free_block(sb, blk, NULL);
			if (seen)
				seen[n_seen++] = blk;
		}
		fi->i_direct[i] = 0;
	}

	/*
	 * Indirection levels. All three are walked: the allocator reaches
	 * triple indirect, so anything shallower here leaks every block a
	 * file held past 2 MiB -- permanently, since nothing else ever
	 * clears those bits.
	 */
	/*
	 * first = 0: the inode is going away, so nothing survives.
	 * base is the logical index the level starts at, which is what
	 * lets each subtree place itself without a shared counter.
	 */
	if (fi->i_indirect) {
		beamfs_free_ind_range(sb, le64_to_cpu(fi->i_indirect), 1,
				      BEAMFS_MAX_IBLOCK_DIRECT, 0, inode);
		fi->i_indirect = 0;
	}
	if (fi->i_dindirect) {
		beamfs_free_ind_range(sb, le64_to_cpu(fi->i_dindirect), 2,
				      BEAMFS_MAX_IBLOCK_INDIRECT, 0, inode);
		fi->i_dindirect = 0;
	}
	if (fi->i_tindirect) {
		beamfs_free_ind_range(sb, le64_to_cpu(fi->i_tindirect), 3,
				      BEAMFS_MAX_IBLOCK_DINDIRECT, 0, inode);
		fi->i_tindirect = 0;
	}

	kfree(seen);
}

/*
 * evict_inode - called when the last reference to an inode is dropped.
 * An inode with no links left has its blocks freed, its mode zeroed on
 * disk and its number returned to the bitmap. Every inode's metadata
 * buffers are written before its list is dropped.
 */
static void beamfs_evict_inode(struct inode *inode)
{
	/* Its reservation window, before its blocks. */
	beamfs_rsv_discard(inode);
	truncate_inode_pages_final(&inode->i_data);
	/*
	 * If the file is truly deleted (nlink == 0), free all data blocks,
	 * zero i_mode on disk so the inode table scan at next mount
	 * correctly identifies this slot as free, then release the inode
	 * number back to the bitmap.
	 */
	if (!inode->i_nlink) {
		/*
		 * Under the allocator's mutex, like every other walk of
		 * the indirection tree.
		 *
		 * Eviction is not obviously concurrent with anything --
		 * the inode has no references left -- but the tree it
		 * walks is the same one the allocator writes, and the
		 * blocks it returns go back to a bitmap other inodes are
		 * drawing from. Taking the lock costs nothing on a path
		 * that runs once per inode and removes the question.
		 */
		mutex_lock(&BEAMFS_I(inode)->i_alloc_mutex);
		beamfs_free_data_blocks(inode);
		mutex_unlock(&BEAMFS_I(inode)->i_alloc_mutex);
		inode->i_mode = 0;
		{
			int _w = beamfs_write_inode_raw(inode);

			/*
			 * The blocks are already free. An inode that
			 * does not reach the medium leaves the next
			 * mount reading a mode that is not zero, over
			 * blocks the bitmap has given away.
			 */
			if (_w)
				pr_err_ratelimited("beamfs: evict: inode %llu not written: %d\n",
						   (unsigned long long)inode->i_ino,
						   _w);
		}
	}
	/*
	 * Written before the list is thrown away.
	 *
	 * mmb_invalidate empties the list without writing anything --
	 * fs/buffer.c takes each buffer off the queue and the contents go
	 * with whatever they held. For an inode that still has links,
	 * that is metadata its pointers still reach.
	 *
	 * Without the sync, generic/269 left 2230 indirect blocks whose
	 * pointer was installed and whose contents never reached the
	 * medium -- fsck read each as never described, the subtree under
	 * it unreachable. A probe counted 23510 evictions and 23510
	 * invalidations, 16381 of them from umount: every dirty metadata
	 * buffer an inode still owned at unmount was dropped.
	 *
	 * write_inode syncs the same list; eviction has to as well.
	 */
	{
		int had = mmb_has_buffers(&BEAMFS_I(inode)->i_metadata_bhs);
		int err = 0;

		/*
		 * Said whichever way it goes.
		 *
		 * write_inode traces its own sync and evict traced
		 * nothing, so a run leaving 22 indirect blocks that
		 * nothing ever wrote gave no way to tell an inode whose
		 * list was empty from one whose list was thrown away. @n
		 * is what the list held when the call started.
		 */
		/*
		 * Written whether or not the inode survives.
		 *
		 * A deleted inode used to drop its list on the grounds that
		 * its blocks are freed above and what they hold no longer
		 * matters. That holds for its own indirect blocks. It does
		 * not hold for the two kinds of buffer the volume shares: the
		 * bitmap block, and the indirect parity region, which carries
		 * the parity of fourteen indirect blocks belonging to other,
		 * living files. Those live on s_shared_bhs (one list, never an
		 * inode's) and are synced here first, before the inode's own.
		 *
		 * The bitmap has a net -- sync_fs and put_super walk
		 * s_bitmap_blkhs and write every block still dirty. The
		 * parity region has none: dropped here, it is gone, and the
		 * fourteen indirect blocks it described read as never
		 * described, each taking its subtree with it. That is what
		 * fsck reports as "no parity was ever written for it, so
		 * nothing ever wrote the block" -- a deduction from an
		 * empty slot, on blocks that were written.
		 *
		 * Forcing pointee-before-pointer ordering
		 * (CONFIG_BEAMFS_ORDERED_META) did not change the count,
		 * which is what ruled the block itself out and left its
		 * parity as the thing that never landed.
		 */
		err = mmb_sync(&BEAMFS_SB(inode->i_sb)->s_shared_bhs);
		if (!err)
			err = mmb_sync(&BEAMFS_I(inode)->i_metadata_bhs);
		if (err)
			pr_err_ratelimited("beamfs: inode %llu: metadata not written before evict: %d\n",
					   (unsigned long long)inode->i_ino,
					   err);
		trace_beamfs_mmb(inode->i_sb->s_dev, inode->i_ino,
				 inode->i_nlink ? "evict sync" : "evict sync unlinked",
				 had, err);

		mmb_invalidate(&BEAMFS_I(inode)->i_metadata_bhs);
	}
	clear_inode(inode);
	/*
	 * Ordering constraint: beamfs_free_inode_num() must run AFTER
	 * clear_inode(), which asserts the inode owns no metadata
	 * buffer_heads. beamfs_free_data_blocks() above calls
	 * beamfs_free_block(sb, blk, NULL) -- the NULL owner keeps
	 * mmb_mark_buffer_dirty() from re-attaching bitmap buffer_heads
	 * to a dying inode after its page cache has been torn down.
	 * Deferring the inode-number free until after clear_inode() also
	 * closes a window where a concurrent beamfs_alloc_inode_num()
	 * could hand out the same ino before the VFS had finished.
	 *
	 * Root cause: beamfs_free_block() used to pass the evicted inode
	 * as owner, appending bitmap bh's to its metadata list after
	 * truncate_inode_pages_final() had emptied it, which tripped
	 * clear_inode()'s assertion. Confirmed by ftrace/kprobe on
	 * beamfs_evict_inode and clear_inode during `depmod -a` on the
	 * rootfs (renameat2 path).
	 *
	 * The mechanism was i_data.i_private_list with
	 * mark_buffer_dirty_inode() and invalidate_inode_buffers() until
	 * the kernel replaced it with the per-inode mapping_metadata_bhs
	 * list and the mmb_* helpers. The ordering requirement is
	 * unchanged; only the names are.
	 */
	if (!inode->i_nlink)
		beamfs_free_inode_num(inode->i_sb, (u64)inode->i_ino);
}

/*
 * beamfs_sync_fs - establish bitmap-before-inode ordering at sync time.
 *
 * Without this hook, sync_filesystem() submits
 * the inode-table buffer (containing fresh inode->i_direct[] pointers)
 * through sync_blockdev_nowait() while the bitmap buffer (carrying the
 * matching bit clear) is left to the bdi writeback queue without
 * deterministic ordering. On reboot()/kernel_restart shortly after,
 * the inode may reach disk while the bitmap clear does not, persisting
 * a state where boot N+1 mounts a bitmap that declares the block free
 * while an inode still references it. First unlink at t~10s on boot N+1
 * trips beamfs_free_block canary "double free of block N".
 *
 * Kernel sync_filesystem() sequence (fs/sync.c):
 *   writeback_inodes_sb(sb)         // start I/O on dirty inodes, no wait
 *   sb->s_op->sync_fs(sb, 0)        // <-- our hook, wait=0
 *   sync_blockdev_nowait(s_bdev)    // submit other dirty bhs, no wait
 *   sync_inodes_sb(sb)              // wait inodes flushed
 *   sb->s_op->sync_fs(sb, 1)        // <-- our hook, wait=1
 *   sync_blockdev(s_bdev)           // wait all bhs
 *
 * The wait=0 call lands BEFORE sync_blockdev_nowait, so calling
 * sync_dirty_buffer (= submit + wait) on every bitmap buffer here
 * forces bitmap to disk BEFORE the inode-table buffer is even
 * submitted. This is the exact ordering required.
 *
 * Instrumentation: one pr_debug per call reports the number of dirty
 * bitmap buffers found, how many were flushed, and the time spent.
 * It is a pr_debug rather than a pr_info because sync_fs runs on every
 * sync: under load that is a log line per second for data that is only
 * of interest when investigating. Enable with dyndbg when needed.
 */
static int beamfs_sync_fs(struct super_block *sb, int wait)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u32 k;
	unsigned int n_bitmap_dirty = 0;
	unsigned int n_bitmap_synced = 0;
	unsigned int n_sb_synced = 0;
	int last_err = 0;
	u64 t0_ns, t1_ns;

	if (!sbi)
		return 0;

	t0_ns = ktime_get_ns();

	/*
	 * Rebuild anything still flagged as stale before any of it is
	 * written. beamfs_write_bitmap_block and beamfs_dirty_super
	 * rebuild as they mark the buffer dirty, so this normally finds
	 * nothing to do; see beamfs_write_bitmap_block for why the bitmap
	 * cannot wait for sync.
	 */
	beamfs_bitmap_encode_pending(sb);
	beamfs_super_encode_pending(sbi);

	/* Flush all dirty bitmap buffers synchronously. */
	if (sbi->s_bitmap_blkhs) {
		for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
			struct buffer_head *bh = sbi->s_bitmap_blkhs[k];
			int rc;

			if (!bh)
				continue;
			if (!buffer_dirty(bh))
				continue;
			n_bitmap_dirty++;
			rc = sync_dirty_buffer(bh);
			if (rc) {
				if (!last_err)
					last_err = rc;
			} else {
				n_bitmap_synced++;
			}
		}
	}

	/* Flush superblock buffer (free_blocks counter + RS journal). */
	if (sbi->s_sbh && buffer_dirty(sbi->s_sbh)) {
		int rc = sync_dirty_buffer(sbi->s_sbh);

		if (rc) {
			if (!last_err)
				last_err = rc;
		} else {
			n_sb_synced = 1;
		}
	}

	t1_ns = ktime_get_ns();
	pr_debug("beamfs/sync310: wait=%d bitmap_dirty=%u bitmap_synced=%u sb_synced=%u err=%d dt_ns=%llu\n",
		 wait, n_bitmap_dirty, n_bitmap_synced, n_sb_synced,
		last_err, (unsigned long long)(t1_ns - t0_ns));
	trace_beamfs_bitmap_flush(sb->s_dev, "sync_fs", n_bitmap_dirty,
				  n_bitmap_synced, last_err);

	return last_err;
}

/*
 * Persist everything this inode owns, not just the inode.
 *
 * Called by __writeback_single_inode during any WB_SYNC_ALL writeback,
 * which is what 7.3 added: before it, a filesystem had its own fsync
 * and several racing fsyncs could each see the dirty bits already clear
 * and return before the metadata was on the medium. I_SYNC serialises
 * it properly now, and the metadata buffers attached to the inode are
 * written here rather than from a path the VFS does not know about.
 *
 * ext2 has the same shape. beamfs descends from its design and this
 * follows it.
 */
static int beamfs_sync_inode_metadata(struct inode *inode,
				      struct writeback_control *wbc)
{
	int err = mmb_sync(&BEAMFS_SB(inode->i_sb)->s_shared_bhs);

	if (!err)
		err = mmb_sync(&BEAMFS_I(inode)->i_metadata_bhs);
	if (err)
		pr_err_ratelimited("beamfs: cannot sync metadata of inode %lu\n",
				   (unsigned long)inode->i_ino);
	return err;
}

static const struct super_operations beamfs_super_ops = {
	.alloc_inode    = beamfs_alloc_inode,
	.free_inode     = beamfs_free_inode,
	.evict_inode    = beamfs_evict_inode,
	.put_super      = beamfs_put_super,
	.write_inode    = beamfs_write_inode,
	.sync_inode_metadata = beamfs_sync_inode_metadata,
	.sync_fs        = beamfs_sync_fs,
	.statfs         = beamfs_statfs,
};

/*
 * beamfs_sb_to_rs_staging -- serialize the CRC32-covered region of a
 * superblock into a contiguous staging buffer for RS encode/decode.
 *
 * Output buffer layout (BEAMFS_SB_RS_STAGING_BYTES bytes):
 *   [0, off_crc32)                              region A:
 *                                                 sb_bytes[0..off_crc32)
 *   [off_crc32, BEAMFS_SB_RS_COVERAGE_BYTES)     region B:
 *                                                 sb_bytes[off_uuid..off_pad)
 *   [BEAMFS_SB_RS_COVERAGE_BYTES,
 *    BEAMFS_SB_RS_STAGING_BYTES)                 zero pad to round up
 *                                                 to whole RS subblocks
 *
 * Field offsets are derived from struct layout via offsetof, so the
 * helper is invariant under future format extensions provided
 * BEAMFS_SB_RS_COVERAGE_BYTES is updated in lockstep. BUILD_BUG_ON
 * below enforces that consistency at compile time.
 *
 * The s_crc32 field [off_crc32, off_uuid) is excluded, exactly as
 * beamfs_crc32_sb() does. Same coverage on both protection layers.
 *
 * Must match mkfs.beamfs.c::sb_to_rs_staging() byte-for-byte.
 */
static void beamfs_sb_to_rs_staging(const struct beamfs_super_block *sb,
				    u8 staging[BEAMFS_SB_RS_STAGING_BYTES])
{
	const u8 *base = (const u8 *)sb;
	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);

	BUILD_BUG_ON(off_crc32 + (off_pad - off_uuid) !=
		     BEAMFS_SB_RS_COVERAGE_BYTES);
	BUILD_BUG_ON(BEAMFS_SB_RS_STAGING_BYTES <
		     BEAMFS_SB_RS_COVERAGE_BYTES);

	memcpy(staging, base, off_crc32);
	memcpy(staging + off_crc32, base + off_uuid, off_pad - off_uuid);
	memset(staging + BEAMFS_SB_RS_COVERAGE_BYTES, 0,
	       BEAMFS_SB_RS_STAGING_BYTES - BEAMFS_SB_RS_COVERAGE_BYTES);
}

/*
 * beamfs_sb_from_rs_staging -- inverse of beamfs_sb_to_rs_staging.
 * Restores the (possibly RS-corrected) bytes from staging back onto
 * the superblock, leaving s_crc32 (bytes [off_crc32, off_uuid))
 * untouched. The trailing zero-pad bytes of staging
 * [BEAMFS_SB_RS_COVERAGE_BYTES, BEAMFS_SB_RS_STAGING_BYTES) are not
 * copied back.
 *
 * Used on the mount-time RS recovery path in beamfs_fill_super.
 */
static void beamfs_sb_from_rs_staging(const u8 staging[BEAMFS_SB_RS_STAGING_BYTES],
				      struct beamfs_super_block *sb)
{
	u8 *base = (u8 *)sb;
	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);

	BUILD_BUG_ON(off_crc32 + (off_pad - off_uuid) !=
		     BEAMFS_SB_RS_COVERAGE_BYTES);

	memcpy(base, staging, off_crc32);
	memcpy(base + off_uuid, staging + off_crc32, off_pad - off_uuid);
}

/*
 * Rebuild the superblock's on-disk image if it is flagged as stale.
 *
 * beamfs_dirty_super rebuilds on every change and clears
 * s_super_needs_encode -- see there for why the superblock cannot wait
 * for sync -- so from sync_fs and put_super this normally finds nothing
 * to do.
 */
void beamfs_super_encode_pending(struct beamfs_sb_info *sbi)
{
	if (!sbi || !sbi->s_super_needs_encode)
		return;
	sbi->s_super_needs_encode = false;
	beamfs_dirty_super_now(sbi);
}

/*
 * The volume has failed. Say so once, and stop writing.
 *
 * ext4 calls this ext4_error and XFS calls it a shutdown; both do the
 * same two things -- record the failure so nothing else tries, and
 * take the mount read-only so the VFS stops sending work. Neither
 * unwinds what was in flight: the point is to stop, not to repair.
 *
 * Not cleared on any path. A volume that failed once has to be
 * unmounted and checked; pretending otherwise is how a bad device
 * becomes a corrupt filesystem.
 */
void beamfs_fail(struct super_block *sb, const char *where, int err)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (!sbi)
		return;
	if (cmpxchg(&sbi->s_failed, false, true))
		return;			/* someone else was first */

	pr_err("beamfs: volume failed in %s (%d), refusing further writes\n",
	       where, err);

	/*
	 * s_failed is enough, and SB_RDONLY is not ours to set.
	 *
	 * ext4 says so in as many words -- "We don't set SB_RDONLY because
	 * that requires sb->s_umount" -- and this path holds no such lock.
	 * Worse, get_tree_bdev returns -EBUSY when the flags of an
	 * existing superblock differ from the ones asked for, so a volume
	 * marked read-only here could not be mounted rw again while its
	 * superblock lived. generic/338 failed one volume with dm-error
	 * and every test after it got "No space left on device" from a
	 * mount that had quietly refused: nine failures from one.
	 *
	 * What refuses a failed volume, with EIO: a write through
	 * beamfs_inline_iomap_begin, the allocator and its
	 * callers, an inode write, the superblock and the bitmap. Data
	 * already accepted into the page cache is still written back. The
	 * next mount starts from a zeroed sb_info.
	 */
}

/*
 * A scratch page, or NULL.
 *
 * __GFP_NORETRY, and every caller handles NULL: this is reached from
 * beamfs_ind_parity_verify, which runs under i_alloc_mutex, while a
 * reader on the same inode holds a locked folio and waits for that
 * mutex. A wait here is a wait the folio waiter cannot outlast --
 * mempool_alloc with GFP_NOFS sleeps until somebody returns a page,
 * and under generic/464 the somebody was itself queued behind the
 * folio.
 *
 * generic/464 wedged the node twice that way. The blocked-state dump
 * shows one xfs_io in __mutex_lock under beamfs_inline_read_folio_range
 * and twenty tasks in folio_wait_bit_common behind it, the machine
 * spinning at 140% with nothing moving for half an hour.
 *
 * Giving up costs a verify that does not happen. Waiting costs the
 * mount.
 */
void *beamfs_scratch_get(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (!sbi || !sbi->s_scratch_pool)
		return kmalloc(BEAMFS_BLOCK_SIZE, GFP_NOFS | __GFP_NORETRY);
	return mempool_alloc(sbi->s_scratch_pool,
			     GFP_NOFS | __GFP_NORETRY | __GFP_NOWARN);
}

void beamfs_scratch_put(struct super_block *sb, void *p)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (!p)
		return;
	if (!sbi || !sbi->s_scratch_pool) {
		kfree(p);
		return;
	}
	mempool_free(p, sbi->s_scratch_pool);
}

/*
 * beamfs_dirty_super - propagate the authoritative in-memory superblock
 * (sbi->s_beamfs_sb) onto the buffer head, recompute its RS parity and
 * s_crc32, and mark the buffer dirty for writeback.
 *
 * Every site that mutates the on-disk superblock (free_blocks,
 * free_inodes, RS journal, ...) must call this helper instead of
 * mark_buffer_dirty(sbi->s_sbh) directly. Without the CRC refresh,
 * the on-disk superblock keeps a stale checksum that fails verification
 * at the next mount.
 *
 * Caller MUST hold sbi->s_lock so that the snapshot copied to the
 * buffer head is taken atomically with respect to other writers.
 */
void beamfs_dirty_super(struct beamfs_sb_info *sbi)
{
	if (!sbi)
		return;

	/*
	 * Rebuilt now, not at sync.
	 *
	 * The superblock buffer comes from the block device's cache, so
	 * the periodic flusher can write it at any time without going
	 * through sync_fs. A rebuild left for later would let it reach
	 * the disk with the bits of one state and the parity of another
	 * -- and a superblock whose parity does not match is a volume
	 * that may not mount. The cost is thirteen shortened RS codewords
	 * per change.
	 *
	 * The flag is cleared here, so beamfs_super_encode_pending, called
	 * from sync_fs and put_super, finds nothing left to do.
	 */
	sbi->s_super_needs_encode = false;
	beamfs_dirty_super_now(sbi);
}

void beamfs_dirty_super_now(struct beamfs_sb_info *sbi)
{
	struct beamfs_super_block *fsb;
	u32 crc;

	if (!sbi || !sbi->s_sbh || !sbi->s_beamfs_sb)
		return;

	/*
	 * Nothing more goes to a volume that has failed.
	 *
	 * The superblock buffer could not be read back on a device
	 * answering EIO, so it is not uptodate, and mark_buffer_dirty
	 * below opens with WARN_ON_ONCE(!buffer_uptodate) -- which is the
	 * kernel saying that a buffer nobody could read holds nothing
	 * worth writing. generic/338 puts dm-error under the mount and
	 * watches for it.
	 */
	if (READ_ONCE(sbi->s_failed))
		return;
	if (!buffer_uptodate(sbi->s_sbh)) {
		beamfs_fail(sbi->s_sb, "dirty_super", -EIO);
		return;
	}

	fsb = (struct beamfs_super_block *)sbi->s_sbh->b_data;

	/*
	 * Copy the authoritative in-memory image onto the buffer head.
	 * Several callers update sbi->s_beamfs_sb in place without touching
	 * fsb; this memcpy serializes them onto disk.
	 */
	memcpy(fsb, sbi->s_beamfs_sb, sizeof(*fsb));

	/*
	 * Encode RS parity over the CRC32-covered region (skipping
	 * s_crc32). The staging buffer, BEAMFS_SB_RS_STAGING_BYTES (2769
	 * bytes), is allocated once per mount by beamfs_fill_super rather
	 * than placed on the stack, which it would push past the 2 KiB
	 * frame budget. Without it the RS encode is skipped and only the
	 * CRC32 below is refreshed.
	 */
	{
		u8 *staging = sbi->s_sb_rs_staging;
		u8 *parity_dst = (u8 *)fsb + BEAMFS_SB_RS_PARITY_OFFSET;

		/*
		 * The buffer is the mount's, not this call's.
		 *
		 * This allocated 2769 bytes on every call, and
		 * beamfs_free_block reaches it under s_lock -- a
		 * spinlock. kvmalloc sleeps, and an allocation that
		 * sleeps under a spinlock is a deadlock waiting for a
		 * machine busy enough to take the slow path.
		 *
		 * DEBUG_ATOMIC_SLEEP found it on the first generic/076
		 * after the check was turned on: "BUG: sleeping function
		 * called from invalid context at sched/mm.h:322", rm
		 * holding sb_writers, i_alloc_mutex and s_lock, in
		 * __kvmalloc_node_noprof under beamfs_dirty_super_now
		 * under beamfs_free_block under beamfs_evict_inode.
		 *
		 * Writing it is serialised by s_lock, which every path
		 * into this function holds or takes.
		 */
		if (!staging) {
			pr_warn_ratelimited("beamfs: dirty_super: no staging buffer; skipping RS re-encode\n");
		} else {
			beamfs_sb_to_rs_staging(fsb, staging);
			{
				int _e = beamfs_rs_encode_region(staging,
						BEAMFS_SB_RS_DATA_LEN,
						parity_dst, BEAMFS_RS_PARITY,
						BEAMFS_SB_RS_DATA_LEN,
						BEAMFS_SB_RS_SUBBLOCKS);

				/*
				 * The superblock: a failure here costs
				 * the whole volume on the next mount.
				 */
				if (_e < 0)
					pr_err("beamfs: superblock encode failed: %d\n",
					       _e);
			}
		}
	}

	crc = beamfs_crc32_sb(fsb);
	fsb->s_crc32 = cpu_to_le32(crc);
	sbi->s_beamfs_sb->s_crc32 = fsb->s_crc32;

	mark_buffer_dirty(sbi->s_sbh);
}

/*
 * beamfs_log_rs_event_flagged -- record an RS correction event in the
 *                       superblock persistent journal (40-byte
 *                       entry), with caller-supplied extra flags
 *                       OR-ed into re_flags.
 *
 * See beamfs.h for full parameter contract. Forensic policy summary:
 *   n_positions >= 2 -> Shannon entropy computed, ENTROPY_VALID set
 *   n_positions == 1 -> entropy zeroed, ENTROPY_VALID cleared
 *   n_positions == 0 -> UNCORRECTABLE set; extra_flags MAY still
 *                       carry RMW_NEUTRALISED if the uncorrectable
 *                       event was detected during an RMW transit
 *                       (in which case the encode that follows
 *                       cannot recover the data either; the flip
 *                       persists in RAM and on disk after this
 *                       cycle, but the RMW context is preserved
 *                       for forensic analysis).
 *
 * extra_flags MUST NOT set BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID or
 * BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE; those bits are policy-owned
 * and reserved for the implementation.
 *
 * Safe to call from any context; spinlock-protected internally.
 */
void beamfs_log_rs_event_flagged(struct super_block *sb,
				 u64 block_no,
			const u8 *positions,
			unsigned int n_positions,
			size_t code_len_bytes,
			u32 extra_flags)
{
	struct beamfs_sb_info  *sbi = BEAMFS_SB(sb);
	struct beamfs_rs_event *ev;
	u8 head;
	u32 entropy_q16 = 0;
	u32 flags = 0;
	const u32 reserved_mask = BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID |
				  BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE;

	if (!sbi || !sbi->s_sbh)
		return;

	/*
	 * Invariant guards: surface bug at WARN_ON_ONCE without panicking.
	 * The journal entry is skipped if an invariant is violated;
	 * reserved bits in extra_flags are cleared instead.
	 *
	 * Two valid call shapes:
	 *   (a) correctable: positions != NULL, 1 <= n_positions <= RS/2
	 *   (b) uncorrectable: positions == NULL, n_positions == 0
	 */
	if (WARN_ON_ONCE(n_positions > BEAMFS_RS_PARITY / 2))
		return;
	if (WARN_ON_ONCE(!n_positions != !positions))
		return;
	if (WARN_ON_ONCE(code_len_bytes == 0 ||
			 code_len_bytes > BEAMFS_SUBBLOCK_DATA))
		return;
	if (WARN_ON_ONCE(extra_flags & reserved_mask))
		extra_flags &= ~reserved_mask;

	/*
	 * Forensic policy:
	 *   n_positions >= 2  -> entropy computed, ENTROPY_VALID set
	 *   n_positions == 1  -> entropy zero, ENTROPY_VALID cleared
	 *                       (single-sample, not forensically significant)
	 *   n_positions == 0  -> UNCORRECTABLE set, entropy zero,
	 *                       symbol_count zero (codeword exceeded RS
	 *                       correction radius; data unrecoverable)
	 */
	if (n_positions == 0) {
		flags = BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE;
	} else if (n_positions >= 2) {
		entropy_q16 = beamfs_rs_compute_entropy_q16_16(positions,
							       n_positions,
							      code_len_bytes);
		flags = BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID;
	}

	flags |= extra_flags;

	spin_lock(&sbi->s_lock);

	head = sbi->s_beamfs_sb->s_rs_journal_head % BEAMFS_RS_JOURNAL_SIZE;
	ev   = &sbi->s_beamfs_sb->s_rs_journal[head];

	ev->re_block_no       = cpu_to_le64(block_no);
	ev->re_timestamp      = cpu_to_le64(ktime_get_ns());
	ev->re_symbol_count   = cpu_to_le32(n_positions);
	ev->re_entropy_q16_16 = cpu_to_le32(entropy_q16);
	ev->re_flags          = cpu_to_le32(flags);
	ev->re_reserved       = 0; /* structural sentinel, MUST stay zero */
	ev->re_pad            = 0; /* structural sentinel, MUST stay zero */
	{
		u32 ev_crc = beamfs_crc32(ev,
					 offsetof(struct beamfs_rs_event,
						  re_crc32));
		ev->re_crc32 = cpu_to_le32(ev_crc);
	}

	sbi->s_beamfs_sb->s_rs_journal_head = (head + 1) % BEAMFS_RS_JOURNAL_SIZE;
	/* What ext2 calls EXT2_ERROR_FS: the kernel saw damage it could not undo. */
	if (flags & BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE)
		sbi->s_beamfs_sb->s_flags |= cpu_to_le32(BEAMFS_SB_FLAG_ERRORS);

	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	trace_beamfs_rs_event(sb->s_dev, block_no,
			      beamfs_rs_event_subblock(flags),
			      n_positions, flags);
	pr_debug("beamfs: RS event block=%llu symbols=%u entropy_valid=%u entropy_q16=%u rmw_neutralised=%u uncorrectable=%u\n",
		 block_no, n_positions,
		 (flags & BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID) ? 1 : 0,
		 entropy_q16,
		 (flags & BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED) ? 1 : 0,
		 (flags & BEAMFS_RS_EVENT_FLAG_UNCORRECTABLE) ? 1 : 0);
}

/*
 * beamfs_log_rs_event -- record an RS event with no extra context flag.
 * Equivalent to beamfs_log_rs_event_flagged(..., 0); used by the
 * replay of superblock recovery events in beamfs_fill_super.
 */
void beamfs_log_rs_event(struct super_block *sb,
			 u64 block_no,
			const u8 *positions,
			unsigned int n_positions,
			size_t code_len_bytes)
{
	beamfs_log_rs_event_flagged(sb, block_no, positions, n_positions,
				    code_len_bytes, 0);
}

/*
 * Pending RS recovery event captured during the SB CRC32-failure path,
 * to be replayed into the journal once sbi is fully initialized.
 *
 * Capturing first and replaying later decouples the recovery detection
 * from the journal write: it preserves the fail-secure invariant
 * (validate the SB image before allocating sbi) while keeping the
 * forensic record of SB recovery, unless the mount fails between
 * capture and replay.
 */
struct beamfs_pending_rs_event {
	u64 block_no;
	unsigned int n_positions;
	size_t code_len_bytes;
	u8 positions[BEAMFS_RS_PARITY / 2];
};

/*
 * beamfs_capture_sb_rs_events -- record which superblock subblocks RS
 * repaired, for replay into the journal once sbi exists.
 *
 * The journal cannot be written yet: the superblock is being read, so
 * sbi is not initialised. The corrections themselves are already in
 * the superblock buffer, which carries them to disk with its next
 * write; what is deferred is only their forensic record, which is why
 * a failed allocation here returns 0 rather than failing the mount.
 *
 * @rs_results:   per-subblock symbol counts from beamfs_rs_decode_region,
 *                positive where that subblock was corrected
 * @rs_positions: flat array of corrected symbol positions,
 *                BEAMFS_RS_PARITY / 2 slots per subblock
 * @out:          receives the allocated event buffer, NULL if none
 *
 * Returns the number of events stored in *out.
 */
static unsigned int
beamfs_capture_sb_rs_events(const int *rs_results, const u8 *rs_positions,
			    struct beamfs_pending_rs_event **out)
{
	struct beamfs_pending_rs_event *pending;
	unsigned int i, k = 0, n_events = 0;

	*out = NULL;

	for (i = 0; i < BEAMFS_SB_RS_SUBBLOCKS; i++) {
		if (rs_results[i] > 0)
			n_events++;
	}
	if (!n_events)
		return 0;

	pending = kmalloc_array(n_events, sizeof(*pending), GFP_KERNEL);
	if (!pending) {
		/*
		 * The kernel already reports the allocation failure itself,
		 * so this says only what it does not: how many recovery
		 * events go unrecorded as a result. Debug level because the
		 * superblock is corrected either way -- what is lost is the
		 * forensic trace, not the data.
		 */
		pr_debug("beamfs: %u SB recovery events not journalled\n",
			 n_events);
		return 0;
	}

	for (i = 0; i < BEAMFS_SB_RS_SUBBLOCKS; i++) {
		unsigned int np;
		const u8 *src;

		if (rs_results[i] <= 0)
			continue;

		np = (unsigned int)rs_results[i];
		if (np > BEAMFS_RS_PARITY / 2)
			np = BEAMFS_RS_PARITY / 2;

		pending[k].block_no = BEAMFS_RS_BLOCK_NO_SB_MARKER | (u64)i;
		pending[k].n_positions = np;
		pending[k].code_len_bytes = BEAMFS_SB_RS_DATA_LEN;
		src = rs_positions + i * (BEAMFS_RS_PARITY / 2);
		memcpy(pending[k].positions, src, np * sizeof(*src));
		k++;
	}

	*out = pending;
	return k;
}

/*
 * beamfs_fill_super - read superblock from disk and initialize VFS sb
 */
int beamfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct beamfs_sb_info     *sbi;
	struct beamfs_super_block *fsb;
	struct buffer_head       *bh;
	struct inode             *root_inode;
	__u32                     crc;
	struct beamfs_pending_rs_event *pending = NULL;
	unsigned int              n_pending = 0;
	int                       ret = -EINVAL;

	/* Set block size */
	if (!sb_set_blocksize(sb, BEAMFS_BLOCK_SIZE)) {
		errorf(fc, "beamfs: unable to set block size %d", BEAMFS_BLOCK_SIZE);
		return -EINVAL;
	}

	/* Read block 0 - superblock */
	bh = beamfs_bread(sb, 0, "superblock");
	if (!bh) {
		errorf(fc, "beamfs: unable to read superblock");
		return -EIO;
	}

	fsb = (struct beamfs_super_block *)bh->b_data;

	/* Verify magic */
	if (le32_to_cpu(fsb->s_magic) != BEAMFS_MAGIC) {
		errorf(fc, "beamfs: bad magic 0x%08x (expected 0x%08x)",
		       le32_to_cpu(fsb->s_magic), BEAMFS_MAGIC);
		goto out_brelse;
	}

	/* Strict version check: this kernel mounts only BEAMFS_VERSION_CURRENT
	 * images. Older v2/v3 images require offline migration via mkfs.beamfs
	 * --migrate. Rationale: dual-format in-kernel parsing doubles the audit
	 * surface (KASAN, syzkaller) for no operational benefit on a niche FS.
	 */
	if (le32_to_cpu(fsb->s_version) != BEAMFS_VERSION_CURRENT) {
		errorf(fc, "beamfs: unsupported on-disk version %u (this kernel requires v%u)",
		       le32_to_cpu(fsb->s_version), BEAMFS_VERSION_CURRENT);
		goto out_brelse;
	}

	/* Verify CRC32 of superblock (excluding the crc32 field itself) */
	crc = beamfs_crc32_sb(fsb);
	if (crc != le32_to_cpu(fsb->s_crc32)) {
		u8 *staging = NULL;
		u8 *parity_src = (u8 *)fsb + BEAMFS_SB_RS_PARITY_OFFSET;
		int *rs_results = NULL;
		u8 *rs_positions = NULL;
		int rc;

		/*
		 * Heap-allocate the RS scratch buffers (2769 + 52 + 416 bytes
		 * total) to keep beamfs_fill_super under the 2 KB stack budget.
		 */
		staging      = kvmalloc(BEAMFS_SB_RS_STAGING_BYTES, GFP_NOFS);
		rs_results   = kvmalloc_array(BEAMFS_SB_RS_SUBBLOCKS,
					      sizeof(*rs_results), GFP_NOFS);
		rs_positions = kvmalloc_array(BEAMFS_SB_RS_SUBBLOCKS *
					      (BEAMFS_RS_PARITY / 2),
					      sizeof(*rs_positions), GFP_NOFS);
		if (!staging || !rs_results || !rs_positions) {
			errorf(fc, "beamfs: superblock RS recovery: out of memory");
			ret = -ENOMEM;
			kvfree(staging);
			kvfree(rs_results);
			kvfree(rs_positions);
			goto out_brelse;
		}

		pr_warn("beamfs: superblock CRC32 mismatch (got 0x%08x, expected 0x%08x), attempting RS recovery\n",
			crc, le32_to_cpu(fsb->s_crc32));

		beamfs_sb_to_rs_staging(fsb, staging);
		rc = beamfs_rs_decode_region(staging, BEAMFS_SB_RS_DATA_LEN,
					     parity_src, BEAMFS_RS_PARITY,
					    BEAMFS_SB_RS_DATA_LEN,
					    BEAMFS_SB_RS_SUBBLOCKS,
					    rs_results,
					    rs_positions,
					    BEAMFS_RS_PARITY / 2,
				"superblock");
		if (rc < 0) {
			errorf(fc, "beamfs: superblock CRC32 mismatch and RS uncorrectable");
			kvfree(staging);
			kvfree(rs_results);
			kvfree(rs_positions);
			goto out_brelse;
		}

		beamfs_sb_from_rs_staging(staging, fsb);
		kvfree(staging);
		staging = NULL;

		crc = beamfs_crc32_sb(fsb);
		if (crc != le32_to_cpu(fsb->s_crc32)) {
			errorf(fc, "beamfs: superblock CRC32 still mismatch after RS recovery");
			kvfree(rs_results);
			kvfree(rs_positions);
			goto out_brelse;
		}

		pr_warn("beamfs: superblock corrected by RS FEC\n");

		n_pending = beamfs_capture_sb_rs_events(rs_results,
							rs_positions,
							&pending);

		/* Heap buffers consumed; free before continuing. */
		kvfree(rs_results);
		kvfree(rs_positions);
	}

	/*
	 * Validate the feature fields.
	 *
	 * s_data_protection_scheme: range-check against the enum maximum.
	 *   The three high-order bytes of the __le32 act as a structural
	 *   sentinel; any value above BEAMFS_DATA_PROTECTION_MAX is rejected.
	 * s_feat_incompat:  unknown bits are a hard refusal (any read).
	 * s_feat_ro_compat: unknown bits force SB_RDONLY but allow mount.
	 * s_feat_compat:    informational, never gates mount.
	 */
	{
		u32 scheme = le32_to_cpu(fsb->s_data_protection_scheme);
		u64 unknown_incompat  = le64_to_cpu(fsb->s_feat_incompat) &
					~BEAMFS_FEAT_INCOMPAT_SUPP;
		u64 unknown_ro_compat = le64_to_cpu(fsb->s_feat_ro_compat) &
					~BEAMFS_FEAT_RO_COMPAT_SUPP;
		u64 unknown_compat    = le64_to_cpu(fsb->s_feat_compat) &
					~BEAMFS_FEAT_COMPAT_SUPP;

		if (scheme > BEAMFS_DATA_PROTECTION_MAX) {
			errorf(fc, "beamfs: invalid data_protection_scheme %u (max %u)",
			       scheme, BEAMFS_DATA_PROTECTION_MAX);
			goto out_brelse;
		}

		/*
		 * A volume written before the inode checksum covered the
		 * block pointers.
		 *
		 * Its i_crc32 spans the head of the inode alone, so this
		 * kernel would compute a different value for every inode
		 * on it and read the whole table as damaged -- thousands
		 * of RS decodes against parity that is correct, and a
		 * mount that appears to work while reporting corruption
		 * everywhere.
		 *
		 * Refusing with the reason is the kinder failure. The
		 * volume is not damaged and its data is intact; it needs
		 * a rewrite, not a repair, and saying so is more use
		 * than a page of decoder errors.
		 */
		if (!(le64_to_cpu(fsb->s_feat_incompat) &
		      BEAMFS_FEATURE_INCOMPAT_INODE_CRC_FULL)) {
			errorf(fc,
			       "beamfs: this volume predates INODE_CRC_FULL -- its inode checksums do not cover the block pointers, so a corrupted pointer would go uncorrected. Copy the data off and remake the volume.");
			goto out_brelse;
		}

		/*
		 * The parity region carries its own FEC now, and that
		 * changed how many slots a region block holds: fourteen
		 * under RS instead of sixteen, because only
		 * BEAMFS_DATA_INLINE_BYTES of the block is payload.
		 *
		 * Reading an older volume with this arithmetic addresses
		 * the wrong slot and hands back a neighbour's parity,
		 * which reads as damage on a healthy block. The volume is
		 * fine; it needs a rewrite, not a repair.
		 *
		 * Only checked when a region exists -- with parity off
		 * there is nothing to be incompatible about.
		 */
		if (le32_to_cpu(fsb->s_ind_parity_mode) != BEAMFS_IND_PARITY_NONE &&
		    !(le64_to_cpu(fsb->s_feat_incompat) &
		      BEAMFS_FEATURE_INCOMPAT_IND_PARITY_FEC)) {
			errorf(fc,
			       "beamfs: this volume predates IND_PARITY_FEC -- its parity region has no protection of its own and a different slot geometry, so reading it here would return the wrong block's parity. Copy the data off and remake the volume.");
			goto out_brelse;
		}

		if (unknown_incompat) {
			errorf(fc, "beamfs: unsupported incompat features 0x%016llx",
			       unknown_incompat);
			goto out_brelse;
		}

		if (unknown_ro_compat && !sb_rdonly(sb)) {
			pr_warn("beamfs: unsupported ro_compat features 0x%016llx, forcing read-only mount\n",
				unknown_ro_compat);
			sb->s_flags |= SB_RDONLY;
		}

		if (unknown_compat)
			pr_info("beamfs: unknown compat features 0x%016llx (informational)\n",
				unknown_compat);
	}

	/* Allocate in-memory sb info */
	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi) {
		ret = -ENOMEM;
		goto out_brelse;
	}

	sbi->s_beamfs_sb = kzalloc(sizeof(*sbi->s_beamfs_sb), GFP_KERNEL);
	if (!sbi->s_beamfs_sb) {
		ret = -ENOMEM;
		goto out_free_sbi;
	}

	memcpy(sbi->s_beamfs_sb, fsb, sizeof(*fsb));
	sbi->s_sbh         = bh;
	sbi->s_free_blocks = le64_to_cpu(fsb->s_free_blocks);
	sbi->s_free_inodes = le64_to_cpu(fsb->s_free_inodes);
	spin_lock_init(&sbi->s_lock);
	INIT_LIST_HEAD(&sbi->s_rsv_windows);
	mmb_init(&sbi->s_shared_bhs, sb->s_bdev->bd_mapping);

	/*
	 * The paths that can hold a scratch page at the same time:
	 * readdir, lookup, add_dirent, del_dirent, dir_is_empty, the
	 * sweep, the block decoder, two in writeback -- and three more
	 * in beamfs_ind_parity_verify, which decodes a region block into
	 * scratch before it can read one slot out of it.
	 *
	 * Twelve was not enough under read pressure. mempool_alloc with
	 * GFP_NOFS waits when the pool is empty, and a reader that holds
	 * two while waiting for a third is waiting on the readers that
	 * hold the rest. generic/464 walked into it: three sshd-session
	 * tasks stalled 191 seconds in __bread_gfp under
	 * beamfs_ind_parity_verify, the machine at 330% CPU with 16 MiB
	 * free and 7.6 GiB of page cache GFP_NOFS was not allowed to
	 * reclaim, and it never came back.
	 *
	 * Thirty-two, which is 128 KiB a mount: enough that every path
	 * can hold its three and still find one, and small enough not to
	 * matter on the embedded profile this filesystem targets.
	 */
	sbi->s_scratch_pool = mempool_create_kmalloc_pool(32,
							  BEAMFS_BLOCK_SIZE);
	if (!sbi->s_scratch_pool) {
		ret = -ENOMEM;
		goto out_free_fsb;
	}

	/*
	 * Staging for the superblock's RS encode, taken once.
	 *
	 * beamfs_dirty_super_now needs 2769 bytes to lay the superblock
	 * out for the encoder, and beamfs_free_block reaches it under
	 * s_lock -- a spinlock. Allocating there is what
	 * DEBUG_ATOMIC_SLEEP reports as "sleeping function called from
	 * invalid context"; allocating here costs 2769 bytes a mount and
	 * nothing else.
	 */
	sbi->s_sb_rs_staging = kvmalloc(BEAMFS_SB_RS_STAGING_BYTES,
					GFP_KERNEL);
	if (!sbi->s_sb_rs_staging) {
		ret = -ENOMEM;
		goto out_free_fsb;
	}
	sbi->s_wb_pages = mempool_create_page_pool(64, 0);
	if (!sbi->s_wb_pages) {
		ret = -ENOMEM;
		goto out_free_fsb;
	}

	sb->s_fs_info  = sbi;
	sb->s_magic    = BEAMFS_MAGIC;
	sb->s_op       = &beamfs_super_ops;
	/*
	 * What the indirection can actually address, not what the type
	 * could hold. See BEAMFS_MAX_FILE_SIZE: overstating this makes
	 * the VFS accept sizes the filesystem cannot reach and defers the
	 * failure to a read that has no good way to explain itself.
	 */
	sb->s_maxbytes = (loff_t)BEAMFS_MAX_FILE_SIZE;

	/*
	 * i_nlink is 16 bits in struct beamfs_inode. Without this the VFS
	 * never refuses a link and the count is cut when the inode is
	 * written; with it link(2), mkdir(2) and rename(2) fail with
	 * EMLINK first.
	 */
	sb->s_max_links = U16_MAX;

	/* Read root inode (inode 1) */
	root_inode = beamfs_iget(sb, 1);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		pr_err("beamfs: failed to read root inode: %d\n", ret);
		goto out_free_fsb;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto out_free_fsb;
	}

	/* Replay any SB RS recovery events captured before sbi was ready.
	 * These are journalled before bitmap setup so the forensic order
	 * (SB events first, then bitmap events) reflects mount sequence.
	 */
	if (pending) {
		unsigned int i;

		for (i = 0; i < n_pending; i++) {
			beamfs_log_rs_event(sb,
					    pending[i].block_no,
					   pending[i].positions,
					   pending[i].n_positions,
					   pending[i].code_len_bytes);
		}
		kfree(pending);
		pending = NULL;
	}

	if (beamfs_setup_bitmap(sb)) {
		ret = -ENOMEM;
		goto out_put_root;
	}

	sbi->s_scheme = le32_to_cpu(fsb->s_data_protection_scheme);
	/*
	 * The legacy iomap path (file.c) installs indirect pointers
	 * without maintaining their parity slot and reads them without
	 * verifying it, while mkfs lays out the parity region for every
	 * scheme. A volume formatted with it carries indirect blocks no
	 * parity describes, by construction. Refused rather than mounted
	 * unprotected; mkfs.beamfs refuses to format it since the same day.
	 */
	if (sbi->s_scheme == BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL) {
		errorf(fc,
		       "beamfs: scheme INODE_UNIVERSAL (5) keeps no parity for its indirect blocks; not mountable");
		ret = -EINVAL;
		goto out_free_fsb;
	}

	/*
	 * Indirection parity. Zero on volumes formatted without it, which
	 * is what turns every call in indparity.c into a no-op -- the
	 * feature costs nothing on a volume that did not ask for it.
	 */
	sbi->s_ind_parity_blk  = le64_to_cpu(fsb->s_ind_parity_blk);
	sbi->s_ind_parity_len  = le32_to_cpu(fsb->s_ind_parity_len);
	sbi->s_ind_parity_mode = le32_to_cpu(fsb->s_ind_parity_mode);
	sbi->s_sb = sb;
	sbi->s_budget_blk = le64_to_cpu(fsb->s_budget_blk);
	sbi->s_budget_len = le32_to_cpu(fsb->s_budget_len);
	if (sbi->s_ind_parity_mode >= BEAMFS_IND_PARITY__MAX) {
		errorf(fc, "beamfs: unknown indirect parity mode %u",
		       sbi->s_ind_parity_mode);
		ret = -EINVAL;
		goto out_free_fsb;
	}
	sbi->s_feat_incompat = le64_to_cpu(fsb->s_feat_incompat);
	sbi->s_data_csum = !!(le64_to_cpu(fsb->s_feat_ro_compat) &
			      BEAMFS_FEATURE_RO_COMPAT_DATA_CSUM);
	sbi->s_data_selfid = !!(le64_to_cpu(fsb->s_feat_ro_compat) &
				BEAMFS_FEATURE_RO_COMPAT_DATA_SELFID);

	pr_info("beamfs: mounted v%u (blocks=%llu free=%lu inodes=%llu scheme=%u feat=0x%016llx/0x%016llx/0x%016llx)\n",
		le32_to_cpu(fsb->s_version),
		le64_to_cpu(fsb->s_block_count),
		sbi->s_free_blocks,
		le64_to_cpu(fsb->s_inode_count),
		le32_to_cpu(fsb->s_data_protection_scheme),
		le64_to_cpu(fsb->s_feat_compat),
		le64_to_cpu(fsb->s_feat_incompat),
		le64_to_cpu(fsb->s_feat_ro_compat));

	/*
	 * Anchor before the scrubber starts: the first sweep may journal
	 * an event, and an entry with no anchor to convert against is
	 * ordering information wearing a timestamp's clothes.
	 */
	beamfs_clock_anchor(sb);

	/*
	 * The state the last mount left, and ours.
	 *
	 * ext2 has done this since the beginning: a flag set while mounted
	 * and cleared by a clean unmount tells the next mount, and fsck,
	 * whether the volume was ever put down properly; a second flag
	 * says the kernel saw damage. beamfs keeps the same two,
	 * BEAMFS_SB_FLAG_MOUNTED and BEAMFS_SB_FLAG_ERRORS; without them
	 * nothing could tell a volume that needed checking from one that
	 * did not.
	 */
	{
		u32 fl = le32_to_cpu(sbi->s_beamfs_sb->s_flags);

		if (fl & BEAMFS_SB_FLAG_MOUNTED)
			pr_warn("beamfs: %s was not cleanly unmounted; running fsck.beamfs is recommended\n",
				sb->s_id);
		if (fl & BEAMFS_SB_FLAG_ERRORS)
			pr_warn("beamfs: %s recorded errors during a previous mount; running fsck.beamfs is recommended\n",
				sb->s_id);
		sbi->s_unclean = !!(fl & (BEAMFS_SB_FLAG_MOUNTED | BEAMFS_SB_FLAG_ERRORS));
		if (!sb_rdonly(sb)) {
			sbi->s_beamfs_sb->s_flags = cpu_to_le32(fl | BEAMFS_SB_FLAG_MOUNTED);
			beamfs_dirty_super_now(sbi);
			if (sbi->s_sbh && sync_dirty_buffer(sbi->s_sbh))
				pr_warn("beamfs: %s: the mounted flag did not reach the medium\n",
					sb->s_id);
		}
	}

	beamfs_tc_init(sbi);

	/*
	 * Start the scrubber last, once the volume is fully usable: it
	 * reads blocks and journals what it finds, so it has no business
	 * running against a superblock still being assembled.
	 *
	 * A failure to start is not a failure to mount. The filesystem
	 * still corrects on read; what is lost is the sweep that would
	 * have found the drift before a reader did. Refusing the mount
	 * over it would trade a degraded guarantee for no filesystem at
	 * all, which is the worse outcome on a device that has no second
	 * copy.
	 */
	ret = beamfs_scrub_init(sb);
	if (ret) {
		pr_warn("beamfs: scrubber did not start (%d); correction on read is unaffected\n",
			ret);
	}

	return 0;

out_put_root:
	dput(sb->s_root);
	sb->s_root = NULL;
out_free_fsb:
	kfree(sbi->s_beamfs_sb);
out_free_sbi:
	/*
	 * Everything fill_super allocated before it could fail here, the
	 * scratch pool and the RS staging included: a mount that fails
	 * past them must not leave them behind.
	 */
	mempool_destroy(sbi->s_wb_pages);
	mempool_destroy(sbi->s_scratch_pool);
	kvfree(sbi->s_sb_rs_staging);
	kfree(sbi);
	sb->s_fs_info = NULL;
out_brelse:
	/* Free any pending events buffer that survived to here. The replay
	 * block sets pending = NULL after consumption, so kfree(NULL) is the
	 * nominal no-op. Reaching here with pending != NULL means a failure
	 * occurred between capture and replay; the events are dropped.
	 */
	kfree(pending);
	brelse(bh);
	return ret;
}

/*
 * fs_context ops
 */
static int beamfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, beamfs_fill_super);
}

/*
 * beamfs_reconfigure - handle mount -o remount
 *
 * The flush is the whole job. Accepting the request and doing nothing
 * left every dirty inode in memory while the blocks they name were
 * already marked used on disk, so a remount,ro produced a filesystem
 * holding allocations nothing pointed at.
 *
 * generic/452 is the smallest possible demonstration: copy ls onto the
 * scratch volume, remount read-only, and the checker finds block 18443
 * -- data_start itself, the first block of the file -- marked used and
 * unreferenced, with the inode still reading size=0 and no direct
 * pointer. The pointer was written; it never reached the medium.
 *
 * xfstests remounts read-only after most tests before running its
 * check, so this reached far past the one test that isolates it.
 *
 * sync_filesystem is what every other filesystem does here, ext2
 * included, and the VFS does not do it for us: the ro/rw transition is
 * the VFS's, the flush before it is ours.
 *
 * Then the state the next mount reads. A read-write mount sets
 * BEAMFS_SB_FLAG_MOUNTED and a clean unmount clears it, but put_super
 * writes nothing to a volume that is read-only by then: the way to
 * read-only clears it here, after the flush, as ext2 marks its s_state
 * valid there, or the next mount calls unclean a volume that was put
 * down properly (generic/452). The way back sets it again, and is
 * refused on a volume that has failed, or that carries ro_compat
 * features this kernel does not know, which fill_super mounts
 * read-only.
 */
static int beamfs_reconfigure(struct fs_context *fc)
{
	struct super_block *sb = fc->root->d_sb;
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	bool to_ro = fc->sb_flags & SB_RDONLY;
	u32 fl;
	int ret;

	ret = sync_filesystem(sb);
	if (ret)
		return ret;
	if (!(fc->sb_flags_mask & SB_RDONLY) || to_ro == sb_rdonly(sb))
		return 0;

	if (!to_ro) {
		u64 unknown = le64_to_cpu(sbi->s_beamfs_sb->s_feat_ro_compat) &
			      ~BEAMFS_FEAT_RO_COMPAT_SUPP;

		if (unknown) {
			errorf(fc,
			       "beamfs: unsupported ro_compat features 0x%016llx, the volume stays read-only",
			       unknown);
			return -EROFS;
		}
		if (beamfs_failed(sb)) {
			errorf(fc, "beamfs: the volume has failed and stays read-only");
			return -EROFS;
		}
	} else if (beamfs_failed(sb)) {
		/* Left marked in use: a failed volume wants fsck.beamfs. */
		return 0;
	}

	spin_lock(&sbi->s_lock);
	fl = le32_to_cpu(sbi->s_beamfs_sb->s_flags);
	if (to_ro)
		fl &= ~BEAMFS_SB_FLAG_MOUNTED;
	else
		fl |= BEAMFS_SB_FLAG_MOUNTED;
	sbi->s_beamfs_sb->s_flags = cpu_to_le32(fl);
	beamfs_dirty_super(sbi);
	spin_unlock(&sbi->s_lock);

	if (sbi->s_sbh && sync_dirty_buffer(sbi->s_sbh))
		pr_warn("beamfs: %s: the mounted flag did not reach the medium\n",
			sb->s_id);
	return 0;
}

static const struct fs_context_operations beamfs_context_ops = {
	.get_tree   = beamfs_get_tree,
	.reconfigure = beamfs_reconfigure,
};

static int beamfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &beamfs_context_ops;
	return 0;
}

/*
 * Stop the scrubber before the rest of the unmount.
 *
 * The sweep holds a reference on the inode it walks, and put_super,
 * where it used to be stopped, runs after generic_shutdown_super has
 * evicted the volume's inodes: an inode held then is busy after
 * unmount, and its iput later touches a superblock that is gone. Only
 * a volume whose fill_super got as far as the root has a scrubber.
 */
static void beamfs_kill_sb(struct super_block *sb)
{
	if (sb->s_root)
		beamfs_scrub_stop(sb);
	kill_block_super(sb);
}

static struct file_system_type beamfs_fs_type = {
	.owner            = THIS_MODULE,
	.name             = "beamfs",
	.init_fs_context  = beamfs_init_fs_context,
	.kill_sb          = beamfs_kill_sb,
	.fs_flags         = FS_REQUIRES_DEV,
};

/*
 * Inode cache constructor
 */
static void beamfs_inode_init_once(void *obj)
{
	struct beamfs_inode_info *fi = obj;

	inode_init_once(&fi->vfs_inode);
}

/*
 * Module init / exit
 */
static int __init beamfs_init(void)
{
	int ret;

	/* Verify on-disk structure sizes at compile time */
	BUILD_BUG_ON(sizeof(struct beamfs_super_block) != BEAMFS_BLOCK_SIZE);
	BUILD_BUG_ON(sizeof(struct beamfs_inode) != 256);
	BUILD_BUG_ON(sizeof(struct beamfs_rs_event) != 40);
	BUILD_BUG_ON(sizeof(struct beamfs_dir_entry) != 268);

	/* Initialize GF(2^8) tables for RS FEC - once, before any mount */
	ret = beamfs_rs_init_tables();
	if (ret)
		return ret;

	beamfs_inode_cachep =
		kmem_cache_create("beamfs_inode_cache",
				  sizeof(struct beamfs_inode_info),
				  0,
				  SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
				  beamfs_inode_init_once);

	if (!beamfs_inode_cachep) {
		pr_err("beamfs: failed to create inode cache\n");
		ret = -ENOMEM;
		goto out_rs;
	}

	ret = register_filesystem(&beamfs_fs_type);
	if (ret) {
		pr_err("beamfs: failed to register filesystem: %d\n", ret);
		goto out_cache;
	}

	/*
	 * Last, and its failure is not fatal: debugfs may be absent from
	 * the build or the mount, and a filesystem that refuses to load
	 * because a benchmark has nowhere to publish itself would be
	 * trading a working module for a measurement.
	 */
	beamfs_debugfs_init();

	pr_info("beamfs: module loaded (beamfs - resilient filesystem)\n");
	return 0;

out_cache:
	kmem_cache_destroy(beamfs_inode_cachep);
out_rs:
	beamfs_rs_exit_tables();
	return ret;
}

static void __exit beamfs_exit(void)
{
	/* One page per cpu, held for the life of the module. */
	beamfs_ind_parity_cache_free();

	/* Before the codec tables go, since a reader could be in a run. */
	beamfs_debugfs_exit();
	unregister_filesystem(&beamfs_fs_type);
	rcu_barrier();
	kmem_cache_destroy(beamfs_inode_cachep);
	beamfs_rs_exit_tables();
	pr_info("beamfs: module unloaded\n");
}

module_init(beamfs_init);
module_exit(beamfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aurelien DESBRIERES <aurelien@hackers.camp>");
MODULE_DESCRIPTION("beamfs - resilient filesystem");
MODULE_VERSION("0.1.26");
MODULE_ALIAS_FS("beamfs");
MODULE_SOFTDEP("pre: reed_solomon");
