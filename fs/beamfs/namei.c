// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Filename / directory entry operations
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Implements: create, mkdir, unlink, rmdir, link, symlink, rename
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/fs_dirent.h>
#include "beamfs.h"
#include "beamfs_trace.h"

/* ------------------------------------------------------------------ */
/* Helper: write a raw beamfs_inode to disk                             */
/* ------------------------------------------------------------------ */

/*
 * The inode has to reach the medium the way the bitmap does.
 *
 * beamfs_write_bitmap_block encodes and dirties a bitmap block on every
 * allocation and every free, and put_super/sync_fs then sync_dirty_buffer
 * every one of them: the bitmap is on disk whatever the flusher does.
 * The inode had only mark_buffer_dirty, and beamfs_write_inode ignored
 * wbc->sync_mode entirely, so a WB_SYNC_ALL from unmount returned
 * without waiting for anything.
 *
 * That asymmetry is the generic/464 leak. Measured on the failing
 * volume: the bitmap block was written, twenty-four of twenty-five
 * inodes seen in the trace were still 0xcd -- never written at all --
 * and 112 blocks were marked used in the bitmap with no inode left to
 * reference them. The volume mounted with 201 files totalling zero
 * bytes.
 *
 * ext2 ends __ext2_write_inode with:
 *
 *     mark_buffer_dirty(bh);
 *     if (do_sync) {
 *             sync_dirty_buffer(bh);
 *             ...
 *     }
 *
 * and ext2_write_inode passes wbc->sync_mode == WB_SYNC_ALL. This does
 * the same.
 */
static int beamfs_write_inode_raw_flags(struct inode *inode, bool sync)
{
	if (beamfs_failed(inode->i_sb))
		return -EIO;

	struct super_block      *sb  = inode->i_sb;
	struct beamfs_sb_info    *sbi = BEAMFS_SB(sb);
	struct beamfs_inode_info *fi  = BEAMFS_I(inode);
	struct beamfs_inode      *raw;
	struct buffer_head      *bh;
	unsigned long            inodes_per_block;
	unsigned long            block, offset;

	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	block  = le64_to_cpu(sbi->s_beamfs_sb->s_inode_table_blk)
		 + (inode->i_ino - 1) / inodes_per_block;
	offset = (inode->i_ino - 1) % inodes_per_block;

	bh = beamfs_bread(sb, block, "directory");
	if (!bh)
		return -EIO;

	raw = (struct beamfs_inode *)bh->b_data + offset;

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

	/*
	 * cpu_to_le32, because i_crc32 is __le32 and beamfs_crc32 returns
	 * host order. Assigning one to the other wrote the checksum in
	 * whatever order the CPU used -- invisible on the little-endian
	 * machines this has run on, and a volume no big-endian host could
	 * mount. sparse called it: "incorrect type in assignment".
	 */
	raw->i_crc32 = cpu_to_le32(beamfs_inode_crc(raw));

	/*
	 * Compute RS parity over the first BEAMFS_INODE_RS_DATA bytes
	 * (everything up to i_reserved, including i_crc32). Parity goes
	 * into i_reserved[0..15]; i_reserved[16..83] is forced to zero so
	 * the layout is deterministic and any non-zero byte there at read
	 * time signals a tampered inode.
	 *
	 * Under BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL this parity is the
	 * authoritative correction record for the inode. mkfs.beamfs writes
	 * the equivalent parity on the root inode at format time; the
	 * kernel maintains it on every subsequent inode write.
	 */
	memset(&raw->i_reserved[BEAMFS_RS_PARITY], 0,
	       sizeof(raw->i_reserved) - BEAMFS_RS_PARITY);
	beamfs_rs_encode((u8 *)raw, BEAMFS_INODE_RS_DATA, raw->i_reserved);

	trace_beamfs_write_inode(inode->i_sb->s_dev, inode->i_ino, sync,
				 (u64)le64_to_cpu(fi->i_indirect), 0);
	mark_buffer_dirty(bh);
	if (sync) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			brelse(bh);
			return -EIO;
		}
	}
	brelse(bh);

	return 0;
}

int beamfs_write_inode_raw(struct inode *inode)
{
	return beamfs_write_inode_raw_flags(inode, false);
}

/* ------------------------------------------------------------------ */
/* Helper: directory block resolver (direct + indirect)                */
/* ------------------------------------------------------------------ */

/*
 * beamfs_dir_get_block -- look up or allocate a directory data block.
 *
 * @dir:        directory inode
 * @block_idx:  0-based logical block index. Range 0..(12 + 512 - 1)
 *              = 0..523 = direct (12) + single indirect (512).
 * @alloc:      if true, allocate on demand. If false, lookup only.
 * @out_block:  resolved physical block number on success. Set to 0 if
 *              the block is a HOLE and @alloc is false.
 *
 * Returns:
 *   0    success (*out_block set)
 *   -ENOSPC  no free blocks (when @alloc=true)
 *   -EIO     indirect block read failure
 *   -EINVAL  block_idx >= 524 (caller bug)
 *
 * Direct vs indirect mapping:
 *   block_idx < 12  -> fi->i_direct[block_idx]
 *   block_idx < 524 -> via fi->i_indirect, slot=(block_idx - 12)
 *
 * Newly allocated blocks (both data and indirect) are zeroed before
 * being installed. A new directory block also gets one free record,
 * d_ino == 0, spanning its 3824-byte payload, then its parity.
 */
int beamfs_dir_get_block(struct inode *dir, unsigned int block_idx,
			 bool alloc, u64 *out_block)
{
	struct super_block       *sb = dir->i_sb;
	struct beamfs_inode_info *fi = BEAMFS_I(dir);
	struct buffer_head       *ibh;
	struct buffer_head       *dbh;
	__le64                   *ptrs;
	u64                       block_no;
	u64                       indirect_blk;
	u64                       indirect_slot;

	if (!out_block)
		return -EINVAL;
	*out_block = 0;

	if (block_idx >= BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS)
		return -EINVAL;

	/* --- Direct --- */
	if (block_idx < BEAMFS_DIRECT_BLOCKS) {
		block_no = le64_to_cpu(fi->i_direct[block_idx]);
		if (block_no) {
			*out_block = block_no;
			return 0;
		}
		if (!alloc)
			return 0;

		block_no = beamfs_alloc_block(sb, dir);
		if (!block_no)
			return -ENOSPC;
		dbh = sb_getblk(sb, block_no);
		if (!dbh) {
			beamfs_free_block(sb, block_no, dir);
			return -EIO;
		}
		/*
		 * A new directory block is one free record covering the
		 * whole payload, then parity over it.
		 *
		 * Zeroing alone used to be enough because every slot was a
		 * fixed 268 bytes and an all-zero slot read as free. With
		 * variable-length records the walk needs a d_rec_len to
		 * step by, and a block of zeros would stop it at the first
		 * entry -- so the block has to say, once, that all 3824
		 * bytes are available.
		 */
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		((struct beamfs_dir_entry *)dbh->b_data)->d_rec_len =
			cpu_to_le16(BEAMFS_DATA_INLINE_BYTES);
		beamfs_dirent_encode(dbh);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		mark_buffer_dirty(dbh);
		brelse(dbh);

		fi->i_direct[block_idx] = cpu_to_le64(block_no);
		/*
		 * What a directory block holds, not what it occupies.
		 *
		 * A block is 4096 bytes on the medium and carries 3824 of
		 * payload; the rest is Reed-Solomon parity. Counting the
		 * whole block puts 272 bytes of parity inside i_size, and
		 * a walk that trusts i_size reads them as a directory
		 * record. The one at the end of the root happens to
		 * decode as a zero length and stop the walk; nothing
		 * makes that true of the next block.
		 */
		dir->i_size += BEAMFS_DATA_INLINE_BYTES;
		mark_inode_dirty(dir);
		*out_block = block_no;
		return 0;
	}

	/* --- Indirect --- */
	indirect_slot = block_idx - BEAMFS_DIRECT_BLOCKS;
	indirect_blk  = le64_to_cpu(fi->i_indirect);

	/* Allocate indirect block on demand if missing. */
	if (!indirect_blk) {
		if (!alloc)
			return 0;
		indirect_blk = beamfs_alloc_block(sb, dir);
		if (!indirect_blk)
			return -ENOSPC;
		ibh = sb_getblk(sb, indirect_blk);
		if (!ibh) {
			beamfs_free_block(sb, indirect_blk, dir);
			return -EIO;
		}
		lock_buffer(ibh);
		memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
		set_buffer_uptodate(ibh);
		beamfs_ind_parity_update(sb, ibh, dir);
		unlock_buffer(ibh);
		/*
		 * On the inode's metadata list, like every other site that
		 * dirties an indirect block: a buffer attached to nothing
		 * is never flushed by __writeback_single_inode.
		 */
		mmb_mark_buffer_dirty(ibh, &BEAMFS_I(dir)->i_metadata_bhs);
		brelse(ibh);
		fi->i_indirect = cpu_to_le64(indirect_blk);
		mark_inode_dirty(dir);
	}

	ibh = beamfs_bread(sb, indirect_blk, "directory");
	if (!ibh)
		return -EIO;
	ptrs = (__le64 *)ibh->b_data;
	block_no = le64_to_cpu(ptrs[indirect_slot]);

	if (block_no) {
		brelse(ibh);
		*out_block = block_no;
		return 0;
	}

	if (!alloc) {
		brelse(ibh);
		return 0;
	}

	block_no = beamfs_alloc_block(sb, dir);
	if (!block_no) {
		brelse(ibh);
		return -ENOSPC;
	}
	dbh = sb_getblk(sb, block_no);
	if (!dbh) {
		beamfs_free_block(sb, block_no, dir);
		brelse(ibh);
		return -EIO;
	}
	/* Same as the direct case: one free record, then parity. */
	lock_buffer(dbh);
	memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
	((struct beamfs_dir_entry *)dbh->b_data)->d_rec_len =
		cpu_to_le16(BEAMFS_DATA_INLINE_BYTES);
	beamfs_dirent_encode(dbh);
	set_buffer_uptodate(dbh);
	unlock_buffer(dbh);
	mark_buffer_dirty(dbh);
	brelse(dbh);

	/*
	 * A directory's indirect block needs its parity like a file's.
	 *
	 * This file had no parity update at all, so a directory
	 * that grew past twelve blocks allocated an indirect block, filled
	 * it with pointers, and left the parity region describing nothing.
	 * The checker then read it as beyond correction and orphaned the
	 * whole subtree.
	 *
	 * generic/310 shows it on inode 3, the test directory: twelve
	 * direct blocks full, indirect block 18455 holding six valid
	 * pointers, and its parity slot zero across all 256 bytes -- the
	 * six blocks below reported lost.
	 */
	lock_buffer(ibh);
	ptrs[indirect_slot] = cpu_to_le64(block_no);
	beamfs_ind_parity_update(sb, ibh, dir);
	unlock_buffer(ibh);
	mmb_mark_buffer_dirty(ibh, &BEAMFS_I(dir)->i_metadata_bhs);
	brelse(ibh);
	/* Payload, not block size -- see the note above. */
	dir->i_size += BEAMFS_DATA_INLINE_BYTES;
	mark_inode_dirty(dir);
	*out_block = block_no;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: add a directory entry to a directory inode                  */
/* ------------------------------------------------------------------ */

static int beamfs_add_dirent(struct inode *dir, const struct qstr *name,
			     unsigned long ino, u8 file_type)
{
	struct super_block      *sb = dir->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de;
	u8      *payload;
	u64      block_no;
	u32      offset, place;
	u16      want;
	unsigned int i;
	int      ret = -ENOSPC;

	if (name->len > BEAMFS_MAX_FILENAME)
		return -ENAMETOOLONG;

	want = (u16)BEAMFS_DIRENT_LEN(name->len);

	payload = beamfs_scratch_get(sb);
	if (!payload)
		return -ENOMEM;

	/*
	 * Search first, allocate only when nothing has room.
	 *
	 * Asking dir_get_block to allocate on every pass gave the
	 * directory a fresh block per attempt: generic/006 ran out of
	 * space after 1047 files where it wanted 4096, and the blocks it
	 * burned were never linked to anything, so entries written into
	 * them were invisible to the next walk. That is where the stale
	 * handles and the missing renames came from too.
	 */
	for (i = 0; i < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS; i++) {
		int rc = beamfs_dir_get_block(dir, i, false, &block_no);

		if (rc) {
			ret = rc;
			goto out;
		}
		if (!block_no) {
			/*
			 * End of what exists: take one more block, under the
			 * allocation mutex, as a file takes one. The walks
			 * that hold that mutex alone, beamfs_getattr's count
			 * and the scrubber's, then see the directory whole.
			 */
			mutex_lock(&BEAMFS_I(dir)->i_alloc_mutex);
			rc = beamfs_dir_get_block(dir, i, true, &block_no);
			mutex_unlock(&BEAMFS_I(dir)->i_alloc_mutex);
			if (rc) {
				ret = rc;
				goto out;
			}
			if (!block_no) {
				ret = -ENOSPC;
				goto out;
			}
		}

		bh = beamfs_bread(sb, block_no, "directory");
		if (!bh) {
			ret = -EIO;
			goto out;
		}

		rc = beamfs_dirent_decode(sb, bh, payload);
		if (rc < 0) {
			brelse(bh);
			ret = rc;
			goto out;
		}

		/*
		 * Look for a record with room to spare.
		 *
		 * A deleted entry leaves its record in place with d_ino
		 * zeroed, so the space it held is reusable without
		 * compacting the block. An entry in use can also give up
		 * its tail if its record is longer than its name needs --
		 * which is how ext2 has done it since 1993, and what keeps
		 * a directory from fragmenting as names come and go.
		 */
		offset = 0;
		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u16 rec, used;
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			rec  = le16_to_cpu(de->d_rec_len);
			used = de->d_ino ?
			       (u16)BEAMFS_DIRENT_LEN(de->d_name_len) : 0;

			if (!de->d_ino && rec >= want) {
				/* Free record, large enough as it stands. */
				place = offset;
				goto place_it;
			}
			if (de->d_ino && rec - used >= want &&
			    beamfs_dirent_place(offset + used, want) ==
			    offset + used) {
				/* Split the tail off a live record. */
				de->d_rec_len = cpu_to_le16(used);
				place = offset + used;
				goto place_it;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			offset = next;
		}
		brelse(bh);
		continue;

place_it:
		de = (struct beamfs_dir_entry *)(payload + place);
		{
			u16 rec = le16_to_cpu(de->d_rec_len);
			u16 keep = want;

			/*
			 * Take what the name needs and leave the rest as a
			 * free record.
			 *
			 * Keeping the whole thing gave a 24-byte name the
			 * 3800 bytes the free record happened to span: the
			 * block held one more entry and no others, and the
			 * walk stepped 3800 bytes straight past the end. So
			 * generic/006 stopped at 1047 files of 4096, and
			 * anything written afterwards was invisible to
			 * lookup while add_dirent still found the space --
			 * "already present", and rename's del_dirent
			 * failing after its add.
			 *
			 * The remainder becomes a free record only when it
			 * can hold a header; below that it stays as slack
			 * on this one, which is what ext2 does.
			 */
			if (rec >= (u16)(want + BEAMFS_DIRENT_MIN_LEN)) {
				struct beamfs_dir_entry *rest =
					(struct beamfs_dir_entry *)
					(payload + place + want);

				memset(rest, 0, BEAMFS_DIRENT_HDR_LEN);
				rest->d_rec_len = cpu_to_le16(rec - want);
			} else {
				keep = max(rec, want);
			}

			memset(de, 0, BEAMFS_DIRENT_HDR_LEN);
			de->d_ino       = cpu_to_le64(ino);
			de->d_rec_len   = cpu_to_le16(keep);
			de->d_name_len  = (u8)name->len;
			de->d_file_type = file_type;
			memcpy(de->d_name, name->name, name->len);
		}

		lock_buffer(bh);
		memcpy(bh->b_data, payload, BEAMFS_DATA_INLINE_BYTES);
		beamfs_dirent_encode(bh);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty(bh);
		brelse(bh);

		/*
		 * The directory changed, so its times did too. Without
		 * these, generic/003 reports that mtime and ctime stand
		 * still across a file creation.
		 */
		inode_set_mtime_to_ts(dir, current_time(dir));
		inode_set_ctime_to_ts(dir, current_time(dir));
		mark_inode_dirty(dir);

		ret = 0;
		goto out;
	}

out:
	beamfs_scratch_put(sb, payload);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Helper: remove a directory entry from a directory                   */
/* ------------------------------------------------------------------ */

static int beamfs_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct super_block      *sb = dir->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de, *prev;
	u8      *payload;
	u64      block_no;
	u32      offset, prev_off;
	unsigned int i;
	int      ret = -ENOENT;

	payload = beamfs_scratch_get(sb);
	if (!payload)
		return -ENOMEM;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS; i++) {
		int rc = beamfs_dir_get_block(dir, i, false, &block_no);

		if (rc) {
			ret = rc;
			goto out;
		}
		if (!block_no)
			break;

		bh = beamfs_bread(sb, block_no, "directory");
		if (!bh)
			continue;

		rc = beamfs_dirent_decode(sb, bh, payload);
		if (rc < 0) {
			brelse(bh);
			ret = rc;
			goto out;
		}

		offset = 0;
		prev_off = BEAMFS_DIRENT_NOSPACE;
		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			if (de->d_ino &&
			    de->d_name_len == name->len &&
			    !memcmp(de->d_name, name->name, name->len)) {
				/*
				 * Fold the record into its predecessor when
				 * that one is free and in the same subblock,
				 * so the space comes back as a single free
				 * record rather than a hole nothing can use.
				 * Otherwise zero d_ino and leave the record,
				 * which add_dirent will find.
				 *
				 * Never into a live predecessor.
				 *
				 * Growing a live entry's d_rec_len to cover
				 * the space just released makes the walk
				 * step over that space, so anything written
				 * there afterwards is invisible: rename put
				 * the new name in the hole, del_dirent could
				 * not find the old one, and the directory
				 * ended up with both -- "already present",
				 * then drop_nlink below zero when rm caught
				 * up. ext2 merges only into free records for
				 * exactly this reason.
				 *
				 * Zeroing d_ino and leaving the record where
				 * it is costs nothing: add_dirent reuses a
				 * free record and splits what it does not
				 * need.
				 */
				de->d_ino = 0;

				if (prev_off != BEAMFS_DIRENT_NOSPACE) {
					prev = (struct beamfs_dir_entry *)
						(payload + prev_off);

					if (!prev->d_ino &&
					    prev_off / BEAMFS_SUBBLOCK_DATA ==
					    offset / BEAMFS_SUBBLOCK_DATA)
						le16_add_cpu(&prev->d_rec_len,
							     le16_to_cpu(de->d_rec_len));
				}

				lock_buffer(bh);
				memcpy(bh->b_data, payload,
				       BEAMFS_DATA_INLINE_BYTES);
				beamfs_dirent_encode(bh);
				set_buffer_uptodate(bh);
				unlock_buffer(bh);
				mark_buffer_dirty(bh);
				brelse(bh);

				/*
				 * Removing an entry changes the directory as
				 * much as adding one does, so mtime and ctime
				 * move here too.
				 */
				inode_set_mtime_to_ts(dir, current_time(dir));
				inode_set_ctime_to_ts(dir, current_time(dir));
				mark_inode_dirty(dir);

				ret = 0;
				goto out;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			prev_off = offset;
			offset = next;
		}
		brelse(bh);
	}

out:
	beamfs_scratch_put(sb, payload);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Helper: allocate and initialize a new VFS inode                     */
/* ------------------------------------------------------------------ */

struct inode *beamfs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block   *sb = dir->i_sb;
	struct inode         *inode;
	struct beamfs_inode_info *fi;
	u64                   ino;

	ino = beamfs_alloc_inode_num(sb);
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

	fi = BEAMFS_I(inode);
	memset(fi->i_direct, 0, sizeof(fi->i_direct));
	fi->i_indirect  = 0;
	fi->i_dindirect = 0;
	fi->i_flags     = 0;

	if (S_ISDIR(mode)) {
		inode->i_op  = &beamfs_dir_inode_operations;
		inode->i_fop = &beamfs_dir_operations;
		set_nlink(inode, 2);
	} else {
		struct beamfs_sb_info *sbi = BEAMFS_SB(inode->i_sb);

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
		set_nlink(inode, 1);
	}

	if (insert_inode_locked(inode) < 0) {
		make_bad_inode(inode);
		iput(inode);
		return ERR_PTR(-EIO);
	}
	mark_inode_dirty(inode);
	trace_beamfs_inode_dirty(inode->i_sb->s_dev, inode->i_ino,
				 (unsigned long)inode_state_read_once(inode));
	return inode;
}

/* ------------------------------------------------------------------ */
/* create - create a regular file                                       */
/* ------------------------------------------------------------------ */

static int beamfs_create(const struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int           ret;

	inode = beamfs_new_inode(dir, mode | S_IFREG);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ret = beamfs_write_inode_raw(inode);
	if (ret)
		goto out_iput;

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino, DT_REG);
	if (ret)
		goto out_iput;

	ret = beamfs_write_inode_raw(dir);
	if (ret)
		goto out_iput;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return 0;

out_iput:
	/*
	 * The inode was allocated, written to disk with nlink=1, and
	 * never linked. beamfs_evict_inode frees an inode only when
	 * nlink has reached zero, so releasing it here without
	 * decrementing leaves it allocated on the medium with no name --
	 * space that nothing can reach and nothing will reclaim.
	 *
	 * generic/204 fills the inode table and shows the result: 1678
	 * inodes, contiguous from 14707 to the end of the table, all
	 * mode 0100644 nlink=1 size=0, every one of them a create that
	 * failed after the inode was written.
	 *
	 * mkdir below already does this, twice, because a directory is
	 * born with nlink=2. discard_new_inode is what the VFS provides
	 * for an inode that was never published -- btrfs, ceph, ext2,
	 * jfs, ntfs3 and udf all use it on this path.
	 */
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return ret;
}

/* ------------------------------------------------------------------ */
/* mkdir - create a directory                                          */
/* ------------------------------------------------------------------ */

static struct dentry *beamfs_mkdir(const struct mnt_idmap *idmap, struct inode *dir,
				   struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int           ret;

	inode_inc_link_count(dir);

	inode = beamfs_new_inode(dir, mode | S_IFDIR);
	if (IS_ERR(inode)) {
		inode_dec_link_count(dir);
		return ERR_CAST(inode);
	}

	/* Add . and .. entries */
	ret = beamfs_add_dirent(inode, &(struct qstr)QSTR_INIT(".", 1),
				inode->i_ino, DT_DIR);
	if (ret)
		goto out_fail;

	ret = beamfs_add_dirent(inode, &(struct qstr)QSTR_INIT("..", 2),
				dir->i_ino, DT_DIR);
	if (ret)
		goto out_fail;

	ret = beamfs_write_inode_raw(inode);
	if (ret)
		goto out_fail;

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino,
				DT_DIR);
	if (ret)
		goto out_fail;

	ret = beamfs_write_inode_raw(dir);
	if (ret)
		goto out_fail;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return NULL;

out_fail:
	/* Twice: a directory is born with nlink=2. */
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	inode_dec_link_count(dir);
	return ERR_PTR(ret);
}

/* ------------------------------------------------------------------ */
/* unlink - remove a file                                              */
/* ------------------------------------------------------------------ */

static int beamfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int           ret;

	ret = beamfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		return ret;

	inode_set_ctime_to_ts(inode, current_time(inode));
	inode_dec_link_count(inode);
	/*
	 * The operation has succeeded and the name is gone from the
	 * tree; failing it now would be a lie. But an inode that does
	 * not reach the medium is a link count the next mount disagrees
	 * with, and saying nothing is how that becomes a checker's
	 * problem weeks later.
	 */
	{
		int _w = beamfs_write_inode_raw(dir);

		if (_w)
			pr_err_ratelimited("beamfs: unlink: parent inode %llu not written: %d\n",
					   (unsigned long long)dir->i_ino, _w);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* rmdir - remove an empty directory                                   */
/* ------------------------------------------------------------------ */

/*
 * beamfs_dir_is_empty -- does @inode hold anything but . and .. ?
 *
 * i_nlink is not the answer. A regular file does not bump its parent's
 * link count, so a directory full of files still reads nlink == 2 and
 * the obvious test says empty. rmdir made that mistake and could never
 * remove anything; rename made the mirror image of it and silently
 * destroyed the target's contents -- caught by xfstests generic/023,
 * where "dire/tree -> Directory not empty" came back as success.
 *
 * The blocks have to be walked. Holes are skipped rather than treated
 * as the end: a directory with a freed slot followed by live entries
 * is not empty, and stopping at the first hole would say it was.
 */
static int beamfs_dir_is_empty(struct inode *inode)
{
	struct super_block      *sb = inode->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de;
	u8      *payload;
	u64      block_no;
	u32      offset;
	unsigned int i;
	/*
	 * Zero means empty. rmdir treats any non-zero return as the
	 * reason it cannot proceed and hands it straight back, so this is
	 * an error code and not a predicate -- getting that backwards
	 * made every rmdir on an empty directory fail.
	 */
	int      ret = 0;

	payload = beamfs_scratch_get(sb);
	if (!payload)
		return -ENOMEM;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS; i++) {
		int rc = beamfs_dir_get_block(inode, i, false, &block_no);

		if (rc) {
			ret = rc;
			goto out;
		}
		if (!block_no)
			break;

		bh = beamfs_bread(sb, block_no, "directory");
		if (!bh)
			continue;

		rc = beamfs_dirent_decode(sb, bh, payload);
		brelse(bh);
		if (rc < 0) {
			ret = rc;
			goto out;
		}

		offset = 0;
		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			if (de->d_ino &&
			    !(de->d_name_len == 1 && de->d_name[0] == '.') &&
			    !(de->d_name_len == 2 && de->d_name[0] == '.' &&
			      de->d_name[1] == '.')) {
				ret = -ENOTEMPTY;
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
	return ret;
}

static int beamfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode            *inode = d_inode(dentry);
	int                      ret;

	/* Same walk rename needs; one implementation for both. */
	ret = beamfs_dir_is_empty(inode);
	if (ret)
		return ret;

	ret = beamfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		return ret;

	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	inode_dec_link_count(dir);
	{
		int _w = beamfs_write_inode_raw(dir);

		/* See unlink: the rmdir has happened either way. */
		if (_w)
			pr_err_ratelimited("beamfs: rmdir: parent inode %llu not written: %d\n",
					   (unsigned long long)dir->i_ino, _w);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* link - create a hard link                                           */
/* ------------------------------------------------------------------ */

static int beamfs_link(struct dentry *old_dentry, struct inode *dir,
		       struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int           ret;

	inode_set_ctime_to_ts(inode, current_time(inode));
	inode_inc_link_count(inode);

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino,
				fs_umode_to_dtype(inode->i_mode));
	if (ret) {
		inode_dec_link_count(inode);
		return ret;
	}

	{
		int _w = beamfs_write_inode_raw(inode);

		if (!_w)
			_w = beamfs_write_inode_raw(dir);
		if (_w)
			pr_err_ratelimited("beamfs: link: inode %llu or parent %llu not written: %d\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)dir->i_ino, _w);
	}
	d_instantiate(dentry, inode);
	ihold(inode);
	return 0;
}

/* ------------------------------------------------------------------ */
/* symlink - create a symbolic link                                    */
/* ------------------------------------------------------------------ */

/*
 * Put a long target in a data block and point i_direct[0] at it.
 *
 * The block is written through the same RS encoder as file data, so the
 * target is protected exactly as a file of the same length would be.
 * The tail is zeroed rather than left as whatever the allocator
 * returned: a symlink is read by length, but an uninitialised tail
 * still means the block's parity covers bytes nobody chose.
 */
static int beamfs_symlink_store_block(struct inode *inode, const char *target,
				      size_t len)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	u64 phys;
	u8 *staging;
	int ret = 0;

	staging = beamfs_scratch_get(sb);
	if (!staging)
		return -ENOMEM;
	memset(staging, 0, BEAMFS_DATA_INLINE_BYTES);
	memcpy(staging, target, len);

	phys = beamfs_alloc_block(sb, inode);
	if (!phys) {
		beamfs_scratch_put(sb, staging);
		return -ENOSPC;
	}

	bh = sb_getblk(sb, phys);
	if (!bh) {
		beamfs_free_block(sb, phys, inode);
		beamfs_scratch_put(sb, staging);
		return -EIO;
	}

	lock_buffer(bh);
	memset(bh->b_data, 0, BEAMFS_BLOCK_SIZE);
	/*
	 * A symlink's target is a data block, so it follows the volume's
	 * data layout.
	 *
	 * This laid it down in sixteen runs 255 apart and encoded it the
	 * same way, whatever the volume did, so on a capsule volume the
	 * kernel read back sixteen uncorrectable subblocks and an empty
	 * path. generic/360 is exactly that: md5 of nothing where the
	 * target should be, on a block holding 1051 intact bytes of it
	 * and a parity area of zeros.
	 */
	beamfs_lay_data_payload(sb, (u8 *)bh->b_data, staging);
	/*
	 * The descriptor too, before the seal, as for any data block: on
	 * a DATA_CSUM volume a target without one failed its check on
	 * every read. Logical block 0, what the reader checks it against.
	 */
	beamfs_stamp_data_block(sb, (u8 *)bh->b_data, staging, inode->i_ino, 0);
	{
		int _e = beamfs_seal_data_block(sb, (u8 *)bh->b_data);

		if (_e < 0)
			pr_err_ratelimited("beamfs: symlink block encode failed: %d\n",
					   _e);
	}
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	mark_buffer_dirty(bh);
	ret = sync_dirty_buffer(bh);
	brelse(bh);
	beamfs_scratch_put(sb, staging);

	if (ret) {
		beamfs_free_block(sb, phys, inode);
		return ret;
	}

	fi->i_direct[0] = cpu_to_le64(phys);
	return 0;
}

/*
 * beamfs_symlink -- create a symbolic link in directory @dir.
 *
 * A target shorter than i_direct[] (96 bytes) is stored inline, in the
 * bytes of i_direct[]. A longer one, up to what a data block holds,
 * goes in a data block whose number is in i_direct[0]; i_size tells the
 * two apart. Anything longer, or an empty target, gets -ENAMETOOLONG.
 */
static int beamfs_symlink(const struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, const char *symname)
{
	struct inode             *inode;
	struct beamfs_inode_info *fi;
	size_t                    len;
	int                       ret;

	len = strlen(symname);
	/*
	 * PATH_MAX is the ceiling the VFS enforces. Ninety-six bytes was not
	 * a design limit, it was the size of the field the short form
	 * happens to reuse, and any absolute path of moderate depth exceeds
	 * it -- generic/360 links to a 1019-byte path and got ENAMETOOLONG.
	 * The limit is what a data block holds, beamfs_block_payload():
	 * 3808 bytes in a capsule, 3824 in the alternating layout. Held to
	 * BEAMFS_DATA_INLINE_BYTES, a capsule cut a target of 3809 to 3824
	 * bytes to 3808. Longer, up to PATH_MAX, is still refused.
	 */
	if (len == 0 || len > beamfs_block_payload(dir->i_sb))
		return -ENAMETOOLONG;

	inode = beamfs_new_inode(dir, S_IFLNK | 0777);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	fi = BEAMFS_I(inode);
	memset(fi->i_direct, 0, sizeof(fi->i_direct));

	if (len < sizeof(fi->i_direct)) {
		/*
		 * Short form: the target lives in i_direct[] as raw bytes,
		 * carried to disk by write_inode_raw's verbatim memcpy and
		 * covered by the inode's own CRC and RS parity.
		 */
		memcpy(fi->i_direct, symname, len);
	} else {
		/*
		 * Long form: the target goes in a data block, and
		 * i_direct[0] holds its number. i_size tells the two apart
		 * on the way back, so nothing new is needed on disk.
		 *
		 * A block brings the RS codewords with it, so a long target
		 * ends up better protected than a short one -- which is the
		 * right way round, since it carries more to lose.
		 */
		ret = beamfs_symlink_store_block(inode, symname, len);
		if (ret)
			goto out_iput;
	}

	inode->i_size = len;
	inode->i_op = &beamfs_symlink_inode_operations;

	ret = beamfs_write_inode_raw(inode);
	if (ret)
		goto out_iput;

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino, DT_LNK);
	if (ret)
		goto out_iput;

	ret = beamfs_write_inode_raw(dir);
	if (ret)
		goto out_iput;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return 0;

out_iput:
	/*
	 * As in beamfs_create: the inode was written with nlink=1 and
	 * never linked, so the link is dropped before the inode is
	 * discarded, or it stays allocated on the medium with no name.
	 */
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return ret;
}

/*
 * beamfs_get_link -- VFS i_op->get_link callback.
 *
 * A short target is returned in place from i_direct[], which lives as
 * long as the in-memory inode. A long one is read from its data block
 * into a buffer the VFS frees through @done.
 */
static const char *beamfs_get_link(struct dentry *dentry,
				   struct inode *inode,
				   struct delayed_call *done)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	loff_t len = i_size_read(inode);
	u64 phys;
	char *buf;
	int ret;

	if (len < (loff_t)sizeof(fi->i_direct))
		return (const char *)fi->i_direct;

	/*
	 * Long form. RCU-walk cannot take the buffer lock or sleep on a
	 * read, so hand it back to the caller for a ref-walk retry.
	 */
	if (!dentry)
		return ERR_PTR(-ECHILD);

	if (len > beamfs_block_payload(sb))
		return ERR_PTR(-EUCLEAN);

	phys = le64_to_cpu(fi->i_direct[0]);
	if (!phys)
		return ERR_PTR(-EUCLEAN);

	/*
	 * kzalloc and not the mount reserve: this buffer is handed to the
	 * VFS through set_delayed_call(kfree_link), so the kernel frees
	 * it long after this function returns and it cannot come from a
	 * pool this filesystem owns.
	 */
	buf = kzalloc(BEAMFS_DATA_INLINE_BYTES + 1, GFP_NOFS);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	bh = beamfs_bread(sb, phys, "directory");
	if (!bh) {
		kfree(buf);
		return ERR_PTR(-EIO);
	}

	lock_buffer(bh);
	ret = beamfs_inline_decode_symlink(sb, bh, phys, inode, buf,
					   (u32)len);
	unlock_buffer(bh);
	brelse(bh);

	if (ret) {
		kfree(buf);
		return ERR_PTR(ret);
	}

	buf[len] = '\0';
	set_delayed_call(done, kfree_link, buf);
	return buf;
}

const struct inode_operations beamfs_symlink_inode_operations = {
	.getattr  = beamfs_getattr,
	.get_link = beamfs_get_link,
};

/* ------------------------------------------------------------------ */
/* write_inode - VFS super_op: persist inode to disk                  */
/* ------------------------------------------------------------------ */

int beamfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	bool sync = wbc->sync_mode == WB_SYNC_ALL;
	int ret, mret, sret;

	/*
	 * Flush the inode's metadata buffers, not just the inode.
	 *
	 * The indirect blocks are attached to i_metadata_bhs by
	 * mmb_mark_buffer_dirty on every pointer install. Nothing empties
	 * that list except beamfs_fsync -- and generic/464 never calls
	 * fsync. ext2 has the same list but its indirect blocks also live
	 * in the block device's page cache, which sync_blockdev empties at
	 * unmount; a buffer only on the private list is written by nobody.
	 *
	 * That is the leak. An overnight run caught it 107 times over 2414
	 * loops: the owning inode was on disk in every single case, but
	 * carrying an old state -- inode 41 took 1037 mark_inode_dirty
	 * calls and reached write_inode once. __writeback_single_inode
	 * reads and clears I_DIRTY before calling ->write_inode, so every
	 * pointer installed during that window leaves the inode looking
	 * clean, and its indirect block sits dirty on a list no one walks.
	 *
	 * Writing the buffers here, on the same call that writes the
	 * inode, keeps the two together: whatever i_indirect points at is
	 * on the medium by the time the inode naming it is.
	 */
	/*
	 * The volume's shared metadata first -- bitmap blocks and parity
	 * regions, see s_shared_bhs -- so that whatever this inode's
	 * pointers rely on is on the medium before the inode naming them.
	 */
	{
		int had = mmb_has_buffers(&BEAMFS_SB(inode->i_sb)->s_shared_bhs);

		sret = mmb_sync(&BEAMFS_SB(inode->i_sb)->s_shared_bhs);
		trace_beamfs_mmb(inode->i_sb->s_dev, inode->i_ino,
				 "write_inode shared", had, sret);
	}
	ret = beamfs_write_inode_raw_flags(inode, sync);

	{
		int had = mmb_has_buffers(&BEAMFS_I(inode)->i_metadata_bhs);

		mret = mmb_sync(&BEAMFS_I(inode)->i_metadata_bhs);
		trace_beamfs_mmb(inode->i_sb->s_dev, inode->i_ino,
				 "write_inode sync", had, mret);
	}
	if (!ret)
		ret = mret;
	if (!ret)
		ret = sret;

	/*
	 * Tell the VFS this inode may have metadata in flight.
	 *
	 * __writeback_single_inode calls ->sync_inode_metadata only for an
	 * inode carrying I_METADATA_WRITEBACK, and nothing sets that flag
	 * but the filesystem itself. beamfs declared the operation and
	 * never set the flag, so the operation was never called once --
	 * zero times in a 414000-line trace of generic/464 -- and the only
	 * thing writing the metadata buffers was this function.
	 *
	 * That is not enough. An inode written by WB_SYNC_NONE writeback
	 * leaves its buffers queued, and the WB_SYNC_ALL pass that should
	 * wait for them skipped the call that waits. ext2, ext4, fat,
	 * minix and bfs all set it here, at the end of ->write_inode, for
	 * the same reason.
	 */
	set_inode_metadata_writeback(inode);

	return ret;
}

/* ------------------------------------------------------------------ */
/* rename - move/rename a directory entry                              */
/* ------------------------------------------------------------------ */

static int beamfs_rename(const struct mnt_idmap *idmap,
			 struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	int	      is_dir    = S_ISDIR(old_inode->i_mode);
	int	      ret;

	/* No RENAME_EXCHANGE or RENAME_WHITEOUT. */
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	if (flags & RENAME_NOREPLACE && new_inode)
		return -EEXIST;

	/*
	 * If destination exists, unlink it first.
	 * For directories: target must be empty, which takes a walk of
	 * its blocks (beamfs_dir_is_empty) -- nlink cannot tell.
	 */
	if (new_inode) {
		if (is_dir) {
			ret = beamfs_dir_is_empty(new_inode);
			if (ret)
				return ret;
		}

		ret = beamfs_del_dirent(new_dir, &new_dentry->d_name);
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
	ret = beamfs_add_dirent(new_dir, &new_dentry->d_name,
				old_inode->i_ino,
			       is_dir ? DT_DIR : DT_REG);
	if (ret) {
		/*
		 * The destination was unlinked above, before this was
		 * attempted. Failing now without putting it back loses
		 * a file that existed when the call started, and the
		 * caller is told the rename did not happen -- so the
		 * loss is silent.
		 *
		 * The name goes back to the inode it named. The link
		 * counts were lowered above and are raised again here,
		 * in the same order.
		 */
		if (new_inode) {
			int back = beamfs_add_dirent(new_dir,
						     &new_dentry->d_name,
						     new_inode->i_ino,
						     S_ISDIR(new_inode->i_mode)
						     ? DT_DIR : DT_REG);

			if (back) {
				pr_err("beamfs: rename: '%.*s' was removed from inode %llu and could not be restored: add %d, restore %d\n",
				       (int)new_dentry->d_name.len,
				       new_dentry->d_name.name,
				       (unsigned long long)new_dir->i_ino,
				       ret, back);
			} else {
				if (S_ISDIR(new_inode->i_mode)) {
					inode_inc_link_count(new_inode);
					inode_inc_link_count(new_inode);
					inode_inc_link_count(new_dir);
				} else {
					inode_inc_link_count(new_inode);
				}
			}
		}
		return ret;
	}

	/* Remove entry from old_dir */
	ret = beamfs_del_dirent(old_dir, &old_dentry->d_name);
	if (ret) {
		int undo;

		/*
		 * Put it back, rather than leaving two names for one
		 * inode.
		 *
		 * The inode's link count was not raised for the name
		 * just added, so two entries share one link: the first
		 * unlink frees the inode and the other entry is left
		 * naming a free one. generic/076 came back with five of
		 * those and two inodes no directory reached, which is
		 * the same event seen from the other side.
		 *
		 * Removing what this call added restores the directory
		 * to what it was, and the caller gets an honest error
		 * instead of a filesystem that disagrees with itself.
		 * It cannot fail for want of space -- the record is
		 * there and only its d_ino is cleared -- and if it does
		 * fail anyway there is nothing further to try, so say
		 * both errors and let fsck see a bounded mess rather
		 * than a silent one.
		 */
		undo = beamfs_del_dirent(new_dir, &new_dentry->d_name);
		pr_err("beamfs: rename: could not remove '%.*s' from inode %llu after adding '%.*s' to inode %llu: %d; rollback %s\n",
		       (int)old_dentry->d_name.len, old_dentry->d_name.name,
		       (unsigned long long)old_dir->i_ino,
		       (int)new_dentry->d_name.len, new_dentry->d_name.name,
		       (unsigned long long)new_dir->i_ino,
		       ret, undo ? "FAILED" : "done");
		return ret;
	}

	/*
	 * Update ".." in the moved directory to point to new_dir.
	 * Also fix nlink on old_dir and new_dir.
	 */
	if (is_dir && old_dir != new_dir) {
		struct qstr dotdot = QSTR_INIT("..", 2);

		ret = beamfs_del_dirent(old_inode, &dotdot);
		if (ret)
			return ret;

		ret = beamfs_add_dirent(old_inode, &dotdot,
					new_dir->i_ino, DT_DIR);
		if (ret)
			return ret;

		inode_dec_link_count(old_dir);
		inode_inc_link_count(new_dir);

		ret = beamfs_write_inode_raw(old_inode);
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
	ret = beamfs_write_inode_raw(old_inode);
	if (ret)
		return ret;

	ret = beamfs_write_inode_raw(old_dir);
	if (ret)
		return ret;

	if (old_dir != new_dir) {
		ret = beamfs_write_inode_raw(new_dir);
		if (ret)
			return ret;
	}

	if (new_inode) {
		int _w = beamfs_write_inode_raw(new_inode);

		if (_w)
			pr_err_ratelimited("beamfs: rename: replaced inode %llu not written: %d\n",
					   (unsigned long long)new_inode->i_ino, _w);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* dir inode_operations - exported                                     */
/* ------------------------------------------------------------------ */

const struct inode_operations beamfs_dir_inode_operations = {
	.lookup  = beamfs_lookup,
	.create  = beamfs_create,
	.mkdir   = beamfs_mkdir,
	.unlink  = beamfs_unlink,
	.rmdir   = beamfs_rmdir,
	.link    = beamfs_link,
	.symlink = beamfs_symlink,
	.rename  = beamfs_rename,
	.getattr = beamfs_getattr,
};
