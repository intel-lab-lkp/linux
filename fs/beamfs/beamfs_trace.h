/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM beamfs

#if !defined(_BEAMFS_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _BEAMFS_TRACE_H

#include <linux/tracepoint.h>
#include <linux/kdev_t.h>

/*
 * One event per store into the block tree, and one per read of a slot.
 * Filterable in the usual way -- echo 'ino == 60' > filter -- so a run
 * can be narrowed to the inode that leaked without rebuilding anything.
 */
TRACE_EVENT(beamfs_slot_store,
	TP_PROTO(dev_t dev, unsigned long ino, u64 parent, u64 slot, u64 old, u64 new,
		 unsigned int level),
	TP_ARGS(dev, ino, parent, slot, old, new, level),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(u64, parent)
		__field(u64, slot)
		__field(u64, old)
		__field(u64, new)
		__field(unsigned int, level)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->ino = ino;
		__entry->parent = parent;
		__entry->slot = slot;
		__entry->old = old;
		__entry->new = new;
		__entry->level = level;
	),
	TP_printk("dev=%u:%u ino=%lu parent=%llu slot=%llu %llu->%llu lvl=%u",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->parent, __entry->slot,
		  __entry->old, __entry->new, __entry->level)
);

/*
 * An indirect block read on the allocation path.
 *
 * generic/464 has two writers install a pointer into the same slot of
 * the same indirect block, twenty-five seconds apart, and the second
 * one reads it as zero: xfs_io writes 23967 into slot 5 of block 36508
 * at t=2158.33, and a writeback kworker writes 105019 into the same
 * slot at t=2183.22, with the trace showing 0-> both times. The block
 * held the first pointer; the second writer did not see it.
 *
 * @uptodate says whether the buffer came back already valid -- a
 * buffer that had to be read from the medium is one the cache had
 * dropped, and what comes back is whatever was last written out,
 * which need not be what the last writer put in memory.
 *
 * @slotval is what sits in the slot about to be written, so a read
 * that returns zero where a pointer was installed shows up here rather
 * than being inferred from two slot_store events.
 */
TRACE_EVENT(beamfs_ind_read,
	TP_PROTO(dev_t dev, unsigned long ino, u64 blk, unsigned int slot, u64 slotval,
		 int uptodate, int fresh),
	TP_ARGS(dev, ino, blk, slot, slotval, uptodate, fresh),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(u64, blk)
		__field(unsigned int, slot)
		__field(u64, slotval)
		__field(int, uptodate)
		__field(int, fresh)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->ino = ino;
		__entry->blk = blk;
		__entry->slot = slot;
		__entry->slotval = slotval;
		__entry->uptodate = uptodate;
		__entry->fresh = fresh;
	),
	TP_printk("dev=%u:%u ino=%lu ind=%llu slot=%u val=%llu uptodate=%d fresh=%d",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->blk, __entry->slot,
		  __entry->slotval, __entry->uptodate, __entry->fresh)
);

/*
 * One parity slot written.
 *
 * A region block holds fourteen slots under RS, and several inodes'
 * indirect blocks land in the same one. generic/464 leaves slots
 * describing a block that is not the block on the medium -- subblocks
 * 0..6 with data and no parity, 7..15 with parity and no data, two
 * states in one slot -- and the question is which writer left what,
 * and in which order. @nz is how many of the indirect block's pointers
 * were set when the parity was taken, so a slot can be matched to the
 * block it was computed from.
 */
TRACE_EVENT(beamfs_parity_slot,
	TP_PROTO(dev_t dev, unsigned long ino, u64 phys, u64 region, unsigned int slot,
		 unsigned int nz),
	TP_ARGS(dev, ino, phys, region, slot, nz),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(u64, phys)
		__field(u64, region)
		__field(unsigned int, slot)
		__field(unsigned int, nz)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->ino = ino;
		__entry->phys = phys;
		__entry->region = region;
		__entry->slot = slot;
		__entry->nz = nz;
	),
	TP_printk("dev=%u:%u ino=%lu ind=%llu region=%llu slot=%u ptrs=%u",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->phys, __entry->region,
		  __entry->slot, __entry->nz)
);
TRACE_EVENT(beamfs_block_alloc,
	TP_PROTO(dev_t dev, unsigned long ino, u64 blk, unsigned int level),
	TP_ARGS(dev, ino, blk, level),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(u64, blk)
		__field(unsigned int, level)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->ino = ino;
		__entry->blk = blk;
		__entry->level = level;
	),
	TP_printk("dev=%u:%u ino=%lu blk=%llu lvl=%u",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->blk, __entry->level)
);

TRACE_EVENT(beamfs_block_free,
	TP_PROTO(dev_t dev, unsigned long ino, u64 blk, unsigned long site),
	TP_ARGS(dev, ino, blk, site),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(u64, blk)
		__field(unsigned long, site)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->ino = ino;
		__entry->blk = blk;
		__entry->site = site;
	),
	/*
	 * site names the caller, by its return address.
	 *
	 * It was the constant 0 in every one of 56215 frees, so a capture
	 * could say a block had been freed and never which of the
	 * twenty-nine call sites did it. That matters here more than
	 * anywhere: freeing a block whose buffer is still on an inode's
	 * metadata list is how a later owner's pointers are lost, the
	 * fix is right for some callers and wrong for others -- applied
	 * to all of them it turned 58 leaked blocks into 345 -- and
	 * telling them apart needs the caller's name.
	 */
	TP_printk("dev=%u:%u ino=%lu blk=%llu site=%pS",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->blk, (void *)__entry->site)
);

/*
 * An inode's metadata buffers, written or dropped.
 *
 * An indirect block is dirtied and put on the inode's metadata list,
 * and something has to take it from there to the medium. Two things
 * can: mmb_sync, from write_inode and from sync_inode_metadata, which
 * writes them; and mmb_invalidate, from evict_inode, which does not --
 * it empties the list and the buffers go with whatever they held.
 *
 * generic/464 leaves one indirect block per run filled with 0xcd, the
 * kernel's poison for memory nobody wrote: block 41641 of inode 24 in
 * one capture, 35433 of inode 65 in the next, each named by its inode
 * on disk and each never written. Something is taking these buffers
 * off the list without writing them, and @how says which.
 *
 * @n is how many buffers the list held when the call started, so a
 * sync that finds nothing and an evict that drops four are told apart.
 */
TRACE_EVENT(beamfs_mmb,
	TP_PROTO(dev_t dev, unsigned long ino, const char *how, int n, int err),
	TP_ARGS(dev, ino, how, n, err),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__string(how, how)
		__field(int, n)
		__field(int, err)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->ino = ino;
		__assign_str(how);
		__entry->n = n;
		__entry->err = err;
	),
	TP_printk("dev=%u:%u ino=%lu %s buffers=%d err=%d",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __get_str(how), __entry->n, __entry->err)
);
/*
 * What a flush of the bitmap found, and what it managed to write.
 *
 * The bitmap's blocks are the one place a freed block is recorded, and
 * 47% of the updates to them are attached to no inode at all -- the
 * free path passes no owner -- so nothing carries them to the medium
 * except the flusher, sync_fs and put_super. Those two are static and
 * inlined, so a kprobe cannot count them and the question "did the
 * free reach the disk" had no answer from outside.
 *
 * @dirty is how many bitmap blocks were dirty when the flush started,
 * @written how many reached the medium, @err the first failure.
 */
TRACE_EVENT(beamfs_bitmap_flush,
	TP_PROTO(dev_t dev, const char *where, unsigned int dirty,
		 unsigned int written, int err),
	TP_ARGS(dev, where, dirty, written, err),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__string(where, where)
		__field(unsigned int, dirty)
		__field(unsigned int, written)
		__field(int, err)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__assign_str(where);
		__entry->dirty = dirty;
		__entry->written = written;
		__entry->err = err;
	),
	TP_printk("dev=%u:%u %s dirty=%u written=%u err=%d",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __get_str(where), __entry->dirty, __entry->written,
		  __entry->err)
);

TRACE_EVENT(beamfs_write_inode,
	TP_PROTO(dev_t dev, unsigned long ino, int sync, u64 indirect, int err),
	TP_ARGS(dev, ino, sync, indirect, err),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(int, sync)
		__field(u64, indirect)
		__field(int, err)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->ino = ino;
		__entry->sync = sync;
		__entry->indirect = indirect;
		__entry->err = err;
	),
	TP_printk("dev=%u:%u ino=%lu sync=%d i_indirect=%llu err=%d",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->sync, __entry->indirect, __entry->err)
);

TRACE_EVENT(beamfs_inode_dirty,
	TP_PROTO(dev_t dev, unsigned long ino, unsigned long state),
	TP_ARGS(dev, ino, state),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(unsigned long, state)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->ino = ino;
		__entry->state = state;
	),
	TP_printk("dev=%u:%u ino=%lu state=0x%lx",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->ino, __entry->state)
);

/*
 * One event per RS decode that found something, on every path: data
 * read, RMW transit, directory, bitmap, indirect parity, scrub and
 * superblock. It is raised in beamfs_log_rs_event_flagged, the one
 * place every decoder reports to, so a capture misses none of them and
 * none is rate-limited the way the dmesg lines are. The journal in the
 * superblock keeps the last sixty-four; this keeps as many as the ring
 * buffer is sized for.
 *
 * @blk is the physical block, the number emufi's flip_log gives as
 * sector / 8, so a flip and its verdict join on it directly. @sub is
 * the subblock, -1 when the event has none (a superblock or inode
 * correction). @symbols is how many the decoder corrected, 0 when it
 * could not; @flags is re_flags as journalled, so uncorrectable
 * (BIT(1)), RMW-neutralised (BIT(2)) and inode (BIT(3)) read off it.
 */
TRACE_EVENT(beamfs_rs_event,
	TP_PROTO(dev_t dev, u64 blk, int sub, unsigned int symbols, u32 flags),
	TP_ARGS(dev, blk, sub, symbols, flags),
	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(u64, blk)
		__field(int, sub)
		__field(unsigned int, symbols)
		__field(u32, flags)
	),
	TP_fast_assign(
		__entry->dev = dev;
		__entry->blk = blk;
		__entry->sub = sub;
		__entry->symbols = symbols;
		__entry->flags = flags;
	),
	TP_printk("dev=%u:%u blk=%llu sub=%d symbols=%u flags=0x%x",
		  MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->blk, __entry->sub, __entry->symbols,
		  __entry->flags)
);

#endif /* _BEAMFS_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE beamfs_trace
#include <trace/define_trace.h>
