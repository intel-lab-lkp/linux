// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Directory blocks: variable-length entries, and parity over them
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Every other structure on this volume is covered. A directory block
 * without parity would be 4096 raw bytes, no checksum, no correction,
 * and an upset there silent and permanent: a flipped inode number
 * points a name at the wrong file, a flipped length walks the parser
 * off the end of an entry, and neither is detectable let alone
 * correctable.
 *
 * Parity is inline, sixteen RS codewords in the block, matching the
 * data path. The alternative -- a separate parity region, which is what
 * the indirect blocks use -- keeps 4096 usable bytes but makes that
 * region a single point of failure for every directory on the volume,
 * and leaves the filesystem with three different parity models instead
 * of two. ext4 puts its directory checksum inline for the same reason.
 *
 * Entries are variable length, and every walk steps by d_rec_len.
 * Stepping by sizeof(struct beamfs_dir_entry) instead -- a flat 268
 * bytes, whatever the name's length -- would give fourteen entries per
 * 3824-byte block and a hard ceiling of 7336 per directory.
 *
 * An eleven-character name takes 24 bytes: 144 entries per block and a
 * ceiling above 75000. Roughly ten times the density, ten times fewer
 * blocks read per lookup, and ten times less metadata exposed for the
 * same directory.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include "beamfs.h"

/*
 * Which subblock an offset falls in, and where that subblock ends.
 *
 * Each of the sixteen subblocks is an independent codeword: one is
 * correctable, or not, on its own. An entry spanning two turns a single
 * uncorrectable subblock into a parse failure for everything after it
 * in the block, so entries are kept inside one whenever they fit.
 */
static inline u32 beamfs_dirent_sub_end(u32 off)
{
	return (off / BEAMFS_SUBBLOCK_DATA + 1) * BEAMFS_SUBBLOCK_DATA;
}

/*
 * Where the next entry of @len bytes goes, given a cursor at @off.
 *
 * Returns the offset to place it at, which is @off unless the entry
 * would straddle a subblock boundary and does not have to. Returns
 * BEAMFS_DIRENT_NOSPACE when the block cannot hold it.
 *
 * An entry longer than a subblock -- a name of 225 characters or more, which
 * NAME_MAX allows and xfstests exercises -- is placed where it falls.
 * Refusing it would mean rejecting a filename every other Linux
 * filesystem accepts, and the property is kept everywhere it can be.
 */
u32 beamfs_dirent_place(u32 off, u16 len)
{
	u32 end;

	if (off + len > BEAMFS_DATA_INLINE_BYTES)
		return BEAMFS_DIRENT_NOSPACE;

	if (len > BEAMFS_SUBBLOCK_DATA)
		return off;			/* cannot fit either way */

	end = beamfs_dirent_sub_end(off);
	if (off + len <= end)
		return off;			/* fits where it is */

	if (end + len > BEAMFS_DATA_INLINE_BYTES)
		return BEAMFS_DIRENT_NOSPACE;

	return end;				/* start of the next subblock */
}

/*
 * Step to the entry after the one at @off.
 *
 * A record length of zero, or one that would leave the block, ends the
 * walk: on a damaged block that is the difference between stopping and
 * reading whatever follows as a directory entry.
 */
u32 beamfs_dirent_next(const struct beamfs_dir_entry *de, u32 off)
{
	u16 rec = le16_to_cpu(de->d_rec_len);

	if (rec < BEAMFS_DIRENT_MIN_LEN)
		return BEAMFS_DIRENT_NOSPACE;
	if (off + rec > BEAMFS_DATA_INLINE_BYTES)
		return BEAMFS_DIRENT_NOSPACE;
	return off + rec;
}

/*
 * Is the entry at @off structurally sound?
 *
 * Checked before the name is used, because a flipped length is exactly
 * the failure this parity was added to catch, and a name length that
 * runs past the record is how a directory read walks off into the next
 * entry.
 */
bool beamfs_dirent_valid(const struct beamfs_dir_entry *de, u32 off)
{
	u16 rec = le16_to_cpu(de->d_rec_len);

	if (rec < BEAMFS_DIRENT_MIN_LEN)
		return false;
	if (off + rec > BEAMFS_DATA_INLINE_BYTES)
		return false;
	/* d_name_len is a u8: every length it can hold is allowed. */
	BUILD_BUG_ON(BEAMFS_MAX_FILENAME < U8_MAX);
	if (BEAMFS_DIRENT_HDR_LEN + de->d_name_len > rec)
		return false;
	return true;
}

/*
 * Re-encode a directory block's parity. The buffer lock must be held.
 *
 * Same sixteen codewords as a data block, over the same 239-byte
 * subblocks, so a corrupted directory is correctable to the same depth
 * as corrupted file contents.
 */
void beamfs_dirent_encode(struct buffer_head *bh)
{
	u8 *b = (u8 *)bh->b_data;
	unsigned int i;

	/*
	 * The payload is laid out contiguously in the first 3824 bytes
	 * and scattered into the codeword slots here, the way the data
	 * path does it, so the walk above can treat the block as flat.
	 */
	for (i = BEAMFS_DATA_INLINE_SUBBLOCKS; i-- > 0; )
		memmove(b + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
			b + (size_t)i * BEAMFS_SUBBLOCK_DATA,
			BEAMFS_SUBBLOCK_DATA);

	{
		int _e = beamfs_rs_encode_region(b, BEAMFS_SUBBLOCK_TOTAL,
				b + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_DATA_INLINE_SUBBLOCKS);

		/*
		 * A directory block with no valid parity is one a
		 * verify will later call beyond correction.
		 */
		if (_e < 0)
			pr_err_ratelimited("beamfs/dirent: encode failed: %d\n",
					   _e);
	}
}

/*
 * Decode a directory block into @dst, correcting what RS can.
 *
 * @dst holds the 3824 payload bytes contiguously, which is what the
 * entry walk expects. Returns the number of subblocks that needed
 * correcting, or -EUCLEAN if any could not be.
 */
int beamfs_dirent_decode(struct super_block *sb, struct buffer_head *bh,
			 u8 *dst)
{
	/*
	 * On the stack, not from the allocator.
	 *
	 * These are 64 and 512 bytes, and this runs once per directory
	 * block on every lookup, readdir, create and unlink -- so a
	 * ten-block directory meant twenty allocations to resolve one
	 * name. Under memory pressure that turns a path that should never
	 * block into one that does: generic/558 had twelve tasks in D and
	 * one of them was sitting in kfree underneath this function,
	 * called from beamfs_lookup.
	 *
	 * 576 bytes against the kernel's 2 KiB frame budget, on a
	 * function whose callers are shallow.
	 */
	int results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	u8 positions[BEAMFS_DATA_INLINE_SUBBLOCKS * (BEAMFS_RS_PARITY / 2)];
	unsigned int i, corrected = 0;
	int ret = 0;

	memset(results, 0, sizeof(results));

	/*
	 * Under the buffer lock, like the sweep.
	 *
	 * beamfs_dirent_encode holds lock_buffer across the memmove that
	 * spreads the records into codeword slots and the RS encode that
	 * follows. Copying without the lock catches that halfway: payload
	 * from after the write, parity from before, uncorrectable by
	 * construction and reported as damage on a block nothing has
	 * damaged.
	 *
	 * Five callers reach here -- readdir, lookup, add_dirent,
	 * del_dirent, dir_is_empty -- and any of them can run while
	 * another writes the same directory.
	 */
	lock_buffer(bh);
	/*
	 * The whole block, parity included.
	 *
	 * decode_rs8 corrects data and parity in place -- it writes to
	 * both buffers it is given. Copying only the payload and pointing
	 * the decoder at bh->b_data for the parity means every read
	 * rewrites the shared buffer's parity bytes. A block read often
	 * enough degrades until it will not decode at all, which is what
	 * the sweep kept reporting on blocks fsck called clean, and why
	 * the count grew as the volume got busier.
	 *
	 * The data path has done this correctly all along: "Decode
	 * RS(255,239) subblocks into a private scratch buffer, never into
	 * bh->b_data."
	 */
	memcpy(dst, (u8 *)bh->b_data, BEAMFS_BLOCK_SIZE);
	unlock_buffer(bh);

	ret = beamfs_rs_decode_region(dst, BEAMFS_SUBBLOCK_TOTAL,
				      dst + BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_SUBBLOCK_TOTAL,
				      BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_DATA_INLINE_SUBBLOCKS,
				      results, positions,
				      BEAMFS_RS_PARITY / 2,
				"directory");

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		if (results[i] > 0) {
			corrected++;
			beamfs_log_rs_event_flagged(sb,
						    bh->b_blocknr,
				positions + (size_t)i * (BEAMFS_RS_PARITY / 2),
				(unsigned int)results[i],
				BEAMFS_SUBBLOCK_DATA,
				beamfs_rs_event_subblock_bits(i));
			beamfs_budget_record(sb, bh->b_blocknr,
					     (unsigned int)results[i]);
		} else if (results[i] < 0) {
			/*
			 * Name the block and the codeword. The buffer knows
			 * where it came from; the decoder does not, and a
			 * line that says only "uncorrectable" leaves the
			 * reader with nowhere to look.
			 */
			pr_err_ratelimited("beamfs: directory block %llu subblock %u/%u uncorrectable\n",
					   (unsigned long long)bh->b_blocknr,
					   i, BEAMFS_DATA_INLINE_SUBBLOCKS);
			ret = -EUCLEAN;
		}
	}

	/*
	 * De-interleave what the decoder corrected.
	 *
	 * dst held the block in its on-disk layout for the decode --
	 * decode_rs8 writes to both the data and the parity it is given,
	 * so both had to be private. Callers want the 3824 payload bytes
	 * contiguous, which is what the walk over records expects.
	 */
	if (ret >= 0) {
		unsigned int k;

		for (k = 0; k < BEAMFS_DATA_INLINE_SUBBLOCKS; k++)
			memmove(dst + (size_t)k * BEAMFS_SUBBLOCK_DATA,
				dst + (size_t)k * BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA);
	}

	return ret < 0 ? ret : (int)corrected;
}
