// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs -- parity for indirection blocks
 *
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * An indirect block is 512 raw __le64 pointers filling all 4096 bytes,
 * with nowhere to put a checksum. beamfs_check_intermediate_block()
 * closes the dominant failure mode -- a pointer outside the data range,
 * or onto a block the bitmap says is free -- and leaves one open: a
 * flipped pointer that lands in range, on an allocated block, passes
 * every check there is. It is then indistinguishable from a valid
 * pointer, and the read returns someone else's data.
 *
 * The asymmetry is what makes this worth closing. A data block gets
 * eight correctable symbols per 255-byte subblock. A double-indirect
 * pointer gets none, and losing it costs 262144 blocks.
 *
 * Parity lives in a region of its own rather than inside the block, so
 * BEAMFS_INDIRECT_PTRS stays at 512 and no BEAMFS_MAX_IBLOCK_* moves:
 * an indirect block is laid out the same whether or not the volume
 * carries parity for it.
 *
 * The slot for a block is computed, not looked up: for physical block
 * P, with n = P - s_data_start, the parity is slot n % slots of the
 * region's block n / slots, slots being how many fit in one region
 * block's payload. A table would be metadata needing protection
 * itself, and the recursion has to stop somewhere.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include "beamfs.h"
#include "beamfs_trace.h"

/* Parity bytes one indirect block costs under the active mode. */
static size_t ind_parity_stride(struct beamfs_sb_info *sbi)
{
	switch (sbi->s_ind_parity_mode) {
	case BEAMFS_IND_PARITY_CRC:
		return BEAMFS_IND_PARITY_CRC_BYTES;
	case BEAMFS_IND_PARITY_RS:
		return BEAMFS_IND_PARITY_RS_BYTES;
	default:
		return 0;
	}
}

/* Slots one region block holds under the active mode. */
static unsigned int ind_parity_slots(struct beamfs_sb_info *sbi)
{
	switch (sbi->s_ind_parity_mode) {
	case BEAMFS_IND_PARITY_CRC:
		return BEAMFS_IND_PARITY_CRC_SLOTS;
	case BEAMFS_IND_PARITY_RS:
		return BEAMFS_IND_PARITY_RS_SLOTS;
	default:
		return 0;
	}
}

/*
 * Locate the parity for @phys: which region block holds it, and at what
 * offset into that block's payload. Returns false when the mode is off
 * or the block sits outside the data area, both of which mean there is
 * nothing to do.
 *
 * The offset is into the decoded payload, not the raw block. A region
 * block is RS-encoded like a data block, so its 4096 bytes hold 3824 of
 * payload interleaved with parity, and indexing the raw bytes would
 * land in the middle of a codeword.
 */
static bool ind_parity_slot(struct super_block *sb, u64 phys,
			    u64 *region_blk, u32 *offset, size_t *stride)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned int slots;
	u64 index;
	u32 within;

	*stride = ind_parity_stride(sbi);
	slots = ind_parity_slots(sbi);
	if (!*stride || !slots || !sbi->s_ind_parity_blk)
		return false;
	if (phys < sbi->s_data_start)
		return false;

	index = phys - sbi->s_data_start;
	within = (u32)do_div(index, slots);   /* index becomes the quotient */
	*region_blk = sbi->s_ind_parity_blk + index;
	*offset = within * (u32)*stride;

	if (*offset + *stride > BEAMFS_DATA_INLINE_BYTES)
		return false;
	if (*region_blk >= sbi->s_ind_parity_blk + sbi->s_ind_parity_len)
		return false;

	return true;
}

/*
 * Decode a region block into @scratch, correcting what RS can.
 *
 * Returns 0 when the payload can be trusted, -EUCLEAN when a subblock
 * was beyond correction. On -EUCLEAN the caller must not write the
 * block back: re-encoding damaged payload makes the damage permanent
 * and self-consistent, which is worse than leaving it visible.
 *
 * Decoding happens in scratch, never in the buffer: decode_rs8 corrects
 * in place and would rewrite the region itself, which is the mistake
 * beamfs_ind_parity_verify documents below at the cost of 500-block
 * leaks.
 */
/*
 * The region block this cpu decoded last.
 *
 * A verify decodes sixteen RS codewords over a 4096-byte region block
 * to read the stride bytes belonging to one indirect block, and perf
 * puts 12.9% of the machine in there -- nearly half of the time that
 * is not idle, on a busy volume.
 *
 * A region block carries the parity of fourteen indirect blocks, and
 * walking a file reads them in order: the same 4096 bytes are decoded
 * fourteen times with nothing changed between. Remembering the last
 * one skips the other thirteen.
 *
 * Keyed on the buffer's own sequence: b_blocknr says which block, and
 * a write to it goes through lock_buffer, so a decode taken while the
 * buffer was locked cannot be stale. What can go stale is a decode
 * from before an update, which is why the generation below exists.
 */
struct ind_region_cache {
	u64	blocknr;
	u64	gen;
	int	ret;
	u8	*data;
};

static DEFINE_PER_CPU(struct ind_region_cache, ind_rcache);

/*
 * Bumped by every parity update.
 *
 * One counter for the filesystem rather than one per region: an update
 * anywhere invalidates every cached decode, which costs a handful of
 * redundant decodes and cannot be wrong. A per-region counter would
 * save those and need a table as large as the region itself.
 */
void beamfs_ind_parity_touched(struct beamfs_sb_info *sbi)
{
	atomic64_inc(&sbi->s_ind_parity_gen);
}

/*
 * Give back the per-cpu decode buffers.
 *
 * Called from module exit. One page per cpu is not a leak anybody
 * notices while the module is loaded and is one the kernel complains
 * about when it is not.
 */
void beamfs_ind_parity_cache_free(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct ind_region_cache *c = per_cpu_ptr(&ind_rcache, cpu);

		kfree(c->data);
		c->data = NULL;
	}
}

static int ind_region_read(struct super_block *sb, struct buffer_head *pbh,
			   u8 *scratch)
{
	int results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	u8 positions[BEAMFS_DATA_INLINE_SUBBLOCKS * (BEAMFS_RS_PARITY / 2)];
	unsigned int i;
	int ret = 0;
	u64 gen_at_read;

	/*
	 * The same region, decoded a moment ago on this cpu?
	 *
	 * Fourteen indirect blocks share a region block and a walk down
	 * a file reads them in order, so the decode below runs fourteen
	 * times over the same 4096 bytes with nothing changed between.
	 *
	 * The generation rules out a decode from before an update. The
	 * cache is per-cpu and taken without a lock: get_cpu_ptr keeps
	 * preemption off from the key check through the copy, so nothing
	 * on this cpu can replace the entry in between.
	 */
	{
		struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
		u64 gen = atomic64_read(&sbi->s_ind_parity_gen);
		struct ind_region_cache *c = get_cpu_ptr(&ind_rcache);
		bool hit = c->data && c->blocknr == pbh->b_blocknr &&
			   c->gen == gen;
		int cached = c->ret;

		if (hit)
			memcpy(scratch, c->data, BEAMFS_BLOCK_SIZE);
		put_cpu_ptr(&ind_rcache);
		if (hit)
			return cached;
	}

	/*
	 * Taken before the copy, not after the decode.
	 *
	 * A parity update that lands while this decode runs bumps the
	 * generation. Reading it afterwards stamps a scratch that
	 * describes the state before that update with the generation
	 * that came after it, and the stale decode is then served to
	 * the other thirteen indirect blocks of the region as if it
	 * were current -- a slot that was written reads back as zero.
	 */
	gen_at_read = atomic64_read(&BEAMFS_SB(sb)->s_ind_parity_gen);
	memcpy(scratch, pbh->b_data, BEAMFS_BLOCK_SIZE);

	beamfs_rs_decode_region(scratch, BEAMFS_SUBBLOCK_TOTAL,
				scratch + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_DATA_INLINE_SUBBLOCKS,
				results, positions,
				BEAMFS_RS_PARITY / 2,
				"indirect parity region");

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		if (results[i] < 0) {
			pr_err_ratelimited("beamfs: parity region block %llu subblock %u beyond correction\n",
					   (unsigned long long)pbh->b_blocknr, i);
			ret = -EUCLEAN;
		} else if (results[i] > 0) {
			beamfs_log_rs_event_flagged(sb, pbh->b_blocknr,
						    positions + (size_t)i * (BEAMFS_RS_PARITY / 2),
				(unsigned int)results[i],
				BEAMFS_SUBBLOCK_DATA,
				beamfs_rs_event_subblock_bits(i));
		}
	}

	/*
	 * Kept for the next thirteen indirect blocks in this region.
	 *
	 * The buffer is allocated on first use and never freed: one page
	 * per cpu, for the life of the module, against a decode of
	 * sixteen codewords on every read of an indirect block.
	 *
	 * GFP_ATOMIC and a failure that costs nothing: no cache is a
	 * slower filesystem, not a wrong one.
	 */
	{
		struct ind_region_cache *c = get_cpu_ptr(&ind_rcache);

		if (!c->data)
			c->data = kmalloc(BEAMFS_BLOCK_SIZE, GFP_ATOMIC);
		if (c->data) {
			memcpy(c->data, scratch, BEAMFS_BLOCK_SIZE);
			c->blocknr = pbh->b_blocknr;
			c->gen = gen_at_read;
			c->ret = ret;
		}
		put_cpu_ptr(&ind_rcache);
	}

	return ret;
}

/*
 * Copy one slot out of a decoded region block.
 *
 * The payload is interleaved with parity -- 239 bytes of data every 255
 * -- so a slot at payload offset @off can straddle the boundary between
 * two subblocks, and a caller cannot simply point into the buffer.
 *
 * Gathering the whole 3824-byte payload into a second scratch page was
 * the first way this worked, and it cost a page per verify on a read
 * path. beamfs_ind_parity_verify then held three at once out of a pool
 * of nine; generic/464 emptied it and three readers stalled 191 seconds
 * each in __bread_gfp, on a machine with 16 MiB free and 7.6 GiB of
 * page cache that GFP_NOFS could not reclaim. A slot is 256 bytes and
 * fits on the stack.
 */
static void ind_slot_gather(const u8 *scratch, u32 off, size_t stride,
			    u8 *out)
{
	size_t done = 0;

	while (done < stride) {
		unsigned int sub = (unsigned int)((off + done) / BEAMFS_SUBBLOCK_DATA);
		size_t within = (off + done) % BEAMFS_SUBBLOCK_DATA;
		size_t run = BEAMFS_SUBBLOCK_DATA - within;

		if (run > stride - done)
			run = stride - done;

		memcpy(out + done,
		       scratch + (size_t)sub * BEAMFS_SUBBLOCK_TOTAL + within,
		       run);
		done += run;
	}
}

/* Put one slot back into a decoded region block, same geometry. */
static void ind_slot_scatter(u8 *scratch, u32 off, size_t stride,
			     const u8 *in)
{
	size_t done = 0;

	while (done < stride) {
		unsigned int sub = (unsigned int)((off + done) / BEAMFS_SUBBLOCK_DATA);
		size_t within = (off + done) % BEAMFS_SUBBLOCK_DATA;
		size_t run = BEAMFS_SUBBLOCK_DATA - within;

		if (run > stride - done)
			run = stride - done;

		memcpy(scratch + (size_t)sub * BEAMFS_SUBBLOCK_TOTAL + within,
		       in + done, run);
		done += run;
	}
}

/*
 * Does the block decode as soon as it is encoded?
 *
 * generic/076 on a volume of 131072 blocks reported region block 1051
 * subblock 9 and region block 8104 subblock 6 beyond correction, and
 * sixteen indirect blocks became uncheckable behind them. The question
 * a report on the medium cannot answer is whether the block left this
 * function already broken or became so on the way out, and this answers
 * it: decode a copy of what was just encoded, and say so when it fails.
 *
 * Debug-only. Every region write pays a full sixteen-codeword decode,
 * which is the cost of the encode again; CONFIG_BEAMFS_DEBUG_TREE is
 * the switch the tree checks already live behind.
 */
#ifdef CONFIG_BEAMFS_DEBUG_TREE
static void ind_region_selfcheck(struct super_block *sb,
				 const struct buffer_head *pbh)
{
	int results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	u8 *copy = beamfs_scratch_get(sb);
	unsigned int i;

	if (!copy)
		return;
	memcpy(copy, pbh->b_data, BEAMFS_BLOCK_SIZE);
	beamfs_rs_decode_region(copy, BEAMFS_SUBBLOCK_TOTAL,
				copy + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_DATA_INLINE_SUBBLOCKS,
				results, NULL, 0, "region selfcheck");
	{
		unsigned int bad = 0;

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
			if (results[i] < 0) {
				bad++;
				pr_err_ratelimited("beamfs: region block %llu subblock %u does not decode as written\n",
						   (unsigned long long)pbh->b_blocknr, i);
			} else if (results[i] > 0) {
				bad++;
				pr_err_ratelimited("beamfs: region block %llu subblock %u needed %d correction(s) as written\n",
						   (unsigned long long)pbh->b_blocknr, i,
						   results[i]);
			}
		}
		/*
		 * Silent when it passes.
		 *
		 * It said so for a while, because silence meant two things
		 * -- the block encoded correctly, or this function never
		 * ran on it -- and generic/076 needed them told apart. It
		 * did: the block leaves this function decodable every
		 * time, and what arrives on the medium is somebody else's
		 * problem.
		 *
		 * Saying it again costs 1390 lines of a 3422-line dmesg,
		 * which is where a KCSAN report goes to be missed.
		 */
		(void)bad;
	}
	beamfs_scratch_put(sb, copy);
}
#endif

/*
 * Encode @scratch back into the buffer.
 *
 * @scratch is a decoded copy of the block and keeps its layout: sixteen
 * subblocks of BEAMFS_SUBBLOCK_TOTAL, each holding its data then its
 * parity. It is not a flat payload, and reading it as one -- at
 * i * BEAMFS_SUBBLOCK_DATA rather than i * BEAMFS_SUBBLOCK_TOTAL --
 * shifts every subblock against the one it came from.
 *
 * That is what happened when the second scratch page was removed: the
 * gather and scatter helpers were written for the interleaved layout,
 * this one was left reading the old flat one, and every region write
 * scrambled the block. Files came back with EUCLEAN moments after being
 * written -- generic/001 reporting "cp: error copying big.0 to big.1:
 * Structure needs cleaning" on its second iteration.
 */
static void ind_region_write(struct super_block *sb,
			     struct buffer_head *pbh, const u8 *scratch)
{
	unsigned int i;

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		memcpy((u8 *)pbh->b_data + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       scratch + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       BEAMFS_SUBBLOCK_DATA);

	{
		int _e = beamfs_rs_encode_region((u8 *)pbh->b_data,
				BEAMFS_SUBBLOCK_TOTAL,
				(u8 *)pbh->b_data + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_DATA_INLINE_SUBBLOCKS);

		/*
		 * The region's own FEC. Without it the region is
		 * unreadable, and every block it covers with it.
		 */
		if (_e < 0)
			pr_err_ratelimited("beamfs: parity region encode failed: %d\n",
					   _e);
	}
#ifdef CONFIG_BEAMFS_DEBUG_TREE
	ind_region_selfcheck(sb, pbh);
#else
	(void)sb;
#endif
}

/*
 * beamfs_ind_parity_update -- recompute parity for an indirect block.
 *
 * Takes the buffer_head rather than a block number: b_blocknr already
 * holds the physical block, so no call site has to name the right
 * variable among indirect_blk, dindirect_blk, l1_blk and the rest, and
 * none can pass the wrong one.
 *
 * Call after modifying the block and before releasing it. A missed call
 * leaves stale parity, which reads as corruption on the next verify --
 * worse than no parity at all, since it turns a healthy volume into one
 * that reports damage.
 *
 * @inode is the inode whose write dirtied the block, or NULL when there
 * is none -- the scrubber repairing a block nobody is writing. It is
 * needed because the parity block has to go on that inode's metadata
 * list: a buffer marked dirty and on no inode's list is never flushed
 * by __writeback_single_inode, so the parity stays in memory while the
 * block it describes reaches the disk, and the next verify reads the
 * mismatch as damage.
 *
 * generic/476 left 293 indirect blocks the checker could not decode,
 * 287 of them almost entirely zero: freshly allocated, a few pointers
 * installed, and parity that never followed them down.
 */
void beamfs_ind_parity_update(struct super_block *sb, struct buffer_head *bh,
			      struct inode *inode)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	const void *block = bh->b_data;
	u64 phys = bh->b_blocknr;
	struct buffer_head *pbh;
	u64 region_blk;
	u32 offset;
	size_t stride;
	unsigned int i;
	u8 *scratch;
	/* One slot, 256 bytes. On the stack, so this path holds one
	 * scratch page instead of two.
	 */
	u8 slotbuf[BEAMFS_IND_PARITY_RS_BYTES];

	if (!ind_parity_slot(sb, phys, &region_blk, &offset, &stride))
		return;
	if (stride > sizeof(slotbuf))
		return;

	pbh = beamfs_bread(sb, region_blk, "indirect parity");
	if (!pbh) {
		pr_err_ratelimited("beamfs: cannot read parity block %llu for indirect %llu\n",
				   (unsigned long long)region_blk,
				   (unsigned long long)phys);
		return;
	}

	scratch = beamfs_scratch_get(sb);
	if (!scratch) {
		/*
		 * Said out loud.
		 *
		 * Returning quietly here leaves the block on the medium
		 * with no parity describing it, and nothing anywhere
		 * records that it happened. generic/464 produced exactly
		 * that the day the pool was made to give up rather than
		 * wait: block 41641 of inode 24, written, named by the
		 * inode, and its parity slot at region 2482 + 3584 still
		 * empty.
		 *
		 * The pool must not wait -- it is reached under
		 * i_alloc_mutex from a page fault, and waiting there is
		 * what wedged the node for half an hour three times. But
		 * an allocation that fails on a filesystem whose whole
		 * purpose is parity is a failure of the filesystem, not a
		 * step to skip.
		 */
		pr_err_ratelimited("beamfs: no scratch page for the parity of indirect %llu; the block goes to the medium undescribed\n",
				   (unsigned long long)phys);
		brelse(pbh);
		return;
	}

	lock_buffer(pbh);

	/*
	 * Read-modify-write on the payload. The region block carries its
	 * own FEC, so one slot cannot be edited in the raw bytes: the
	 * whole block has to be decoded, the slot replaced, and the block
	 * re-encoded.
	 *
	 * A region block beyond correction is left alone. Re-encoding it
	 * would write parity over damaged payload and make the parity of
	 * every other indirect block it covers permanently wrong, in a way
	 * nothing could afterwards detect.
	 */
	if (ind_region_read(sb, pbh, scratch) == -EUCLEAN) {
		pr_err_ratelimited("beamfs: parity region block %llu beyond correction; not updating the slot for indirect %llu\n",
				   (unsigned long long)region_blk,
				   (unsigned long long)phys);
		unlock_buffer(pbh);
		beamfs_scratch_put(sb, scratch);
		brelse(pbh);
		return;
	}

	ind_slot_gather(scratch, offset, stride, slotbuf);

	if (sbi->s_ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		__le32 *slot = (__le32 *)slotbuf;

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			slot[i] = cpu_to_le32(crc32_le(~0U, (const u8 *)block +
						       (size_t)i * BEAMFS_SUBBLOCK_DATA,
						       BEAMFS_SUBBLOCK_DATA) ^ ~0U);
	} else {
		u8 *slot = slotbuf;

		/*
		 * The indirect block is 4096 bytes and an RS codeword
		 * covers 239, so it is split the way a data block is: 16
		 * subblocks, one 16-byte parity set each. That covers
		 * the first 3824 bytes; pointers 478..511 are in no
		 * codeword, as beamfs_ind_parity_verify notes.
		 */
		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			beamfs_rs_encode_region((u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
						BEAMFS_SUBBLOCK_DATA,
						slot + (size_t)i * BEAMFS_RS_PARITY,
						BEAMFS_RS_PARITY,
						BEAMFS_SUBBLOCK_DATA, 1);
	}

	ind_slot_scatter(scratch, offset, stride, slotbuf);
	ind_region_write(sb, pbh, scratch);
	/* The parity now describes this copy. */
	set_buffer_beamfs_verified(bh);

	/*
	 * The pointer count serves the tracepoint and nothing else.
	 * Counted unconditionally it is 512 loads for every pointer
	 * installed, on a path that already pays seventeen RS
	 * operations, and it ran with tracing off as readily as on.
	 * generic/013 spends 950 seconds here where ext2 spends
	 * sixty.
	 */
	if (trace_beamfs_parity_slot_enabled()) {
		unsigned int nz = 0, k;

		for (k = 0; k < BEAMFS_INDIRECT_PTRS; k++)
			if (((const __le64 *)block)[k])
				nz++;
		trace_beamfs_parity_slot(sb->s_dev, inode ? inode->i_ino : 0,
					 phys,
					 region_blk, offset / (u32)stride, nz);
	}
	/*
	 * Bumped before the lock is dropped, not after.
	 *
	 * The bump used to follow unlock_buffer. Between the two, another
	 * cpu updating a neighbour slot of the same region took the lock,
	 * found its cached decode of this region still stamped with the
	 * current generation, hit, scattered its own slot into a payload
	 * that predates this update, and wrote the region: the slot
	 * written here went to the device once and was overwritten by
	 * the next writer's stale copy of the region.
	 *
	 * generic/476 on 2026-09-23, module 0.1.10:
	 * 102 indirect blocks reported by fsck as holding pointers under
	 * a zero parity slot. For every one of them the tracepoint had
	 * recorded an update with exactly the pointer count fsck found,
	 * and the region block had been written to the device after that
	 * update -- never before, never not at all. The write carried
	 * bytes from before the update. 98 of the 102 were never freed;
	 * the region was simply written by someone else's update.
	 *
	 * With the bump inside the lock, any update that takes the lock
	 * after this one reads a generation this decode does not carry,
	 * misses, and decodes the buffer as it is. unlock_buffer orders
	 * the increment before the next holder's reads.
	 */
	beamfs_ind_parity_touched(sbi);
	unlock_buffer(pbh);
	beamfs_scratch_put(sb, scratch);
	/*
	 * On the volume's shared metadata list, so that the sync of any
	 * inode carries it with the blocks it describes. Not on the
	 * inode's own list: a region holds the parity of fourteen indirect
	 * blocks of as many files, and a buffer that moves between inode
	 * lists meets the window in mmb_sync described at s_shared_bhs.
	 * Without an inode -- the scrubber -- the list is still the one
	 * the next write_inode syncs.
	 */
	mmb_mark_buffer_dirty(pbh, &BEAMFS_SB(sb)->s_shared_bhs);
	if (inode) {
		/*
		 * And the inode, so the list gets flushed.
		 *
		 * mmb_sync empties the metadata list from write_inode,
		 * which the VFS calls only for an inode it believes is
		 * dirty. Updating a region changes no field of the
		 * inode, so nothing marked it, and the region stayed in
		 * memory holding the only parity that matches the block
		 * just written.
		 *
		 * What is on the medium is then the parity of what the
		 * block held before, and verify reports the block beyond
		 * correction -- 17 of them in one generic/013, on a
		 * volume where treecheck saw no lost pointer and every
		 * indirect block had its parity updated. The update ran;
		 * it just never landed.
		 */
		mark_inode_dirty(inode);
	}
	brelse(pbh);
}

/*
 * beamfs_ind_parity_verify -- check an indirect block against its parity.
 *
 * Under CRC the block is checked and left alone: detection turns a
 * flipped pointer that lands in range, on an allocated block, from
 * silent corruption into a clean fail-closed error, which is the
 * contract beamfs states everywhere else. Under RS a copy is decoded
 * and the block is left alone: damage is reported and journalled,
 * never repaired here -- the comment at the decode below says why a
 * verify must not write.
 *
 * Returns 0 if the block is sound or its damage is within correction,
 * -EUCLEAN if it is damaged beyond the mode's ability to fix.
 */
int beamfs_ind_parity_verify(struct super_block *sb, struct buffer_head *bh)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	void *block = bh->b_data;
	u64 phys = bh->b_blocknr;
	struct buffer_head *pbh;
	u64 region_blk;
	u32 offset;
	size_t stride;
	unsigned int i;
	int ret = 0;
	int rc;
	u8 *rscratch;
	u8 slotbuf[BEAMFS_IND_PARITY_RS_BYTES];

	if (!ind_parity_slot(sb, phys, &region_blk, &offset, &stride))
		return 0;
	/*
	 * Already checked, or written by us: the copy in memory is what
	 * the parity describes. Only a buffer freshly read from the
	 * medium, or one the scrub asks about, is decoded.
	 */
	if (buffer_beamfs_verified(bh))
		return 0;

	pbh = beamfs_bread(sb, region_blk, "indirect parity");
	if (!pbh)
		return 0;   /* parity unreadable: do not fail the read on it */

	if (stride > sizeof(slotbuf)) {
		brelse(pbh);
		return 0;
	}
	rscratch = beamfs_scratch_get(sb);
	if (!rscratch) {
		brelse(pbh);
		return 0;
	}

	/*
	 * The region block carries its own FEC, so the slot has to be
	 * decoded out of it rather than read from the raw bytes.
	 *
	 * A region beyond correction is not the indirect block's fault
	 * and must not be reported as such: before this, a hit on the
	 * region made every indirect block it covered look damaged, and
	 * fsck blamed the blocks. Saying so and passing the read is the
	 * honest answer -- the block may well be fine and there is now
	 * nothing to check it against.
	 */
	/*
	 * Under the buffer lock, because the update path holds it while
	 * it rewrites all 4096 bytes.
	 *
	 * Copying the block while that rewrite is in flight yields an
	 * image half old and half new, which no RS code can correct:
	 * the region is reported beyond correction with nothing wrong
	 * on the medium, the indirect block it covers stops being
	 * checked, and its slot stops being maintained.
	 */
	lock_buffer(pbh);
	rc = ind_region_read(sb, pbh, rscratch);
	unlock_buffer(pbh);

	if (rc == -EUCLEAN) {
		pr_err_ratelimited("beamfs: parity region block %llu beyond correction; indirect %llu cannot be checked\n",
				   (unsigned long long)region_blk,
				   (unsigned long long)phys);
		beamfs_scratch_put(sb, rscratch);
		brelse(pbh);
		return 0;
	}

	ind_slot_gather(rscratch, offset, stride, slotbuf);

	/*
	 * An empty slot describes nothing, and decoding against it
	 * destroys the block.
	 *
	 * Zero data with zero parity is a valid RS codeword, so a slot of
	 * zeros is a codeword saying "this block is all zeros". An
	 * indirect block early in its life holds one or two pointers --
	 * eight bytes in a 239-byte subblock -- and RS(255,239) corrects
	 * up to eight symbols. The decoder therefore does exactly what it
	 * is asked: it moves those eight bytes to zero and reports a
	 * successful correction.
	 *
	 * generic/076 catches it in the act. treecheck reports parent
	 * 18516 slot 1 holding 18518, read back as 0 and reallocated to
	 * 22512, from writeback -- and the checker says of that block
	 * that no parity was ever written for it.
	 *
	 * A block whose parity has not been written yet is unverifiable,
	 * not wrong. Say so and let the read through: the pointers are
	 * what the writer put there, and destroying them to satisfy a
	 * codeword nobody wrote is the one outcome worse than not
	 * checking.
	 */
	{
		size_t k;
		bool described = false;

		for (k = 0; k < stride; k++) {
			if (slotbuf[k]) {
				described = true;
				break;
			}
		}
		if (!described) {
			/*
			 * The slot covers BEAMFS_DATA_INLINE_BYTES of the
			 * block, 3824 of 4096: pointers 478..511 are in no
			 * codeword, and a block that holds pointers only
			 * there has, correctly, a zero slot. That is the v5
			 * format and not an event, so it is not warned
			 * about; a zero slot over pointers the slot does
			 * cover is.
			 */
			const __le64 *ptrs = (const __le64 *)bh->b_data;
			unsigned int within = 0;

			for (k = 0; k < BEAMFS_DATA_INLINE_BYTES / sizeof(__le64); k++)
				if (ptrs[k])
					within++;
			if (within)
				pr_warn_ratelimited("beamfs: indirect block %llu: parity slot empty over %u pointer(s) it covers; not checked\n",
						    (unsigned long long)phys, within);
			/*
			 * Nothing to check it against until an update files
			 * its slot, which marks it verified itself; until
			 * then, asked once per copy, not once per lookup.
			 */
			set_buffer_beamfs_verified(bh);
			beamfs_scratch_put(sb, rscratch);
			brelse(pbh);
			return 0;
		}
	}

	if (sbi->s_ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		const __le32 *slot = (const __le32 *)slotbuf;

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
			u32 want = le32_to_cpu(slot[i]);
			u32 got = crc32_le(~0U,
				(const u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_DATA) ^ ~0U;

			if (want != got) {
				pr_err_ratelimited("beamfs: indirect block %llu subblock %u CRC mismatch\n",
						   (unsigned long long)phys, i);
				beamfs_log_rs_event_flagged(sb,
							    phys, NULL, 0, BEAMFS_SUBBLOCK_DATA,
					beamfs_rs_event_subblock_bits(i));
				ret = -EUCLEAN;
			}
		}
	} else {
		u8 *slot = slotbuf;
		int results[1];
		u8 positions[BEAMFS_RS_PARITY / 2];
		u8 *copy;

		/*
		 * Decode a copy, never bh->b_data.
		 *
		 * decode_rs8 corrects in place: it writes both the data
		 * and the parity it is handed. Decoding the live indirect
		 * block against the parity region rewrites that block --
		 * and when the region's parity is stale, which it is the
		 * moment a data block is reallocated as an indirect one
		 * without the region being updated, the "correction"
		 * drags the block back toward whatever it held before.
		 *
		 * generic/464 turned that into 500-block leaks: a fresh
		 * L1 filled with 512 pointers was read once by another
		 * process, verify decoded it against block 1934's old
		 * parity, and the 512 pointers collapsed back to the two
		 * the previous owner had left -- orphaning everything
		 * below them.
		 *
		 * A verify has no business modifying the block it checks.
		 * It decodes into scratch, reports what it finds, and
		 * touches nothing. The only real corruption on a device
		 * that flips no bits is stale parity, and the fix for that
		 * is to keep the parity current, not to let the decoder
		 * launder one block's contents into another's.
		 */
		copy = beamfs_scratch_get(sb);
		if (!copy) {
			beamfs_scratch_put(sb, rscratch);
			brelse(pbh);
			return 0;
		}
		memcpy(copy, block, BEAMFS_BLOCK_SIZE);

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
			u8 *dsub = copy + (size_t)i * BEAMFS_SUBBLOCK_DATA;
			u8 *psub = slot + (size_t)i * BEAMFS_RS_PARITY;
			int region_rc;

			region_rc = beamfs_rs_decode_region(dsub, BEAMFS_SUBBLOCK_DATA,
							    psub, BEAMFS_RS_PARITY,
							    BEAMFS_SUBBLOCK_DATA, 1,
							    results, positions,
							    BEAMFS_RS_PARITY / 2,
							    "indirect");

			if (region_rc < 0 || results[0] < 0) {
				pr_err_ratelimited("beamfs: indirect block %llu subblock %u uncorrectable\n",
						   (unsigned long long)phys, i);
				beamfs_log_rs_event_flagged(sb, phys, NULL, 0,
							    BEAMFS_SUBBLOCK_DATA,
							    beamfs_rs_event_subblock_bits(i));
				ret = -EUCLEAN;
			} else if (results[0] > 0) {
				beamfs_log_rs_event_flagged(sb, phys, positions,
							    (unsigned int)results[0],
							    BEAMFS_SUBBLOCK_DATA,
							    beamfs_rs_event_subblock_bits(i));
			}
		}
		beamfs_scratch_put(sb, copy);
	}

	beamfs_scratch_put(sb, rscratch);
	brelse(pbh);
	if (ret == 0)
		set_buffer_beamfs_verified(bh);
	return ret;
}

/*
 * The scrub asks about the medium, not about the copy: check again
 * whatever was checked before.
 */
int beamfs_ind_parity_verify_medium(struct super_block *sb,
				    struct buffer_head *bh)
{
	clear_buffer_beamfs_verified(bh);
	return beamfs_ind_parity_verify(sb, bh);
}
