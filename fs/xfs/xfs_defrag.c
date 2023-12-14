// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Wengang Wang <wen.gang.wang@oracle.com>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_bmap.h"
#include "xfs_inode_fork.h"
#include "xfs_inode.h"
#include "xfs_reflink.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_bit.h"
#include "xfs_buf.h"
#include "xfs_bmap_btree.h"
#include "xfs_trans_space.h"
#include "xfs_ag.h"
#include "xfs_alloc.h"
#include "xfs_refcount_btree.h"
#include "xfs_btree.h"
#include "xfs_refcount.h"
#include "xfs_defer.h"
#include "xfs_log_priv.h"
#include "xfs_extfree_item.h"
#include "xfs_bmap_item.h"
#include "xfs_quota_defs.h"
#include "xfs_quota.h"

#include <linux/sort.h>

/*
 * The max number of extents in a piece.
 * can't be too big, it will have log space presure
 */
#define XFS_DEFRAG_PIECE_MAX_EXT	512

/*
 * Milliseconds we leave the info unremoved when a defrag failed.
 * This aims to give user space a way to get the error code.
 */
#define XFS_DERFAG_GRACE_PERIOD	30000

/* limitation of pending online defrag */
#define XFS_DEFRAG_MAX_PARALLEL		128

/*
 * The max size, in blocks, of a piece.
 * can't be too big, it may hard to get such a free extent
 */
#define XFS_DEFRAG_MAX_PIECE_BLOCKS	4096U

/*
 * A piece is a contigurous (by file block number) range in a file. It contains one
 * or more extents. When it contains two or more extents, it's subject to be
 * defragmented.  During the defragmenting, the original extents are
 * deallocated and replaced by a single new-allocated extent covering this
 * whole piece.
 */
struct xfs_defrag_piece {
	/* the start file block in this piece */
	xfs_fileoff_t		dp_start_off;
	/* number of blocks contained in this piece */
	xfs_filblks_t		dp_len;
	/*
	 * the extents in this piece. they are contigourous by file block
	 * number after the piece is picked. they are sorted by filesystem
	 * lock number (low -> high) before unmapping.
	 */
	struct xfs_bmbt_irec	dp_extents[XFS_DEFRAG_PIECE_MAX_EXT];
	/* number of xfs_bmbt_irecs in dp_extents */
	int			dp_nr_ext;
};

struct xfs_defrag_info {
	/* links to xfs_mount.m_defrag_list */
	struct list_head	di_list;		/* links to xfs_mount.m_defrag_list */
	/* defrag configuration and status */
	struct xfs_defrag	di_defrag;
	/* the xfs_inode to defragment on */
	struct xfs_inode	*di_ip;
	/* next file block to start with */
	xfs_fileoff_t		di_next_blk;
	/* number of pieces which are defragmented */
	unsigned long		di_round_nr;
	/* current piece to defragment */
	struct xfs_defrag_piece	di_dp;
	/* timestamp of last defragmenting in jiffies */
	unsigned long		di_last_process;
	/* flag indicating if defragmentation is stopped by user */
	bool			di_user_stopped;
};

/* initialization called for new mount */
void xfs_initialize_defrag(struct xfs_mount *mp)
{
	sema_init(&mp->m_defrag_lock, 1);
	mp->m_nr_defrag = 0;
	mp->m_defrag_task = NULL;
	INIT_LIST_HEAD(&mp->m_defrag_list);
}

/* stop all the defragmentations on this mount and wait until they really stopped */
void xfs_stop_wait_defrags(struct xfs_mount *mp)
{
	down(&mp->m_defrag_lock);
	if (list_empty(&mp->m_defrag_list)) {
		up(&mp->m_defrag_lock);
		return;
	}
	ASSERT(mp->m_defrag_task);
	up(&mp->m_defrag_lock);
	kthread_stop(mp->m_defrag_task);
	mp->m_defrag_task = NULL;
}


static bool xfs_is_defrag_param_valid(struct xfs_defrag *defrag)
{
	if (defrag->df_piece_size > XFS_DEFRAG_MAX_PIECE_BLOCKS)
		return false;
	if (defrag->df_piece_size < 2 * defrag->df_tgt_extsize)
		return false;
	return true;
}

static inline bool __xfs_new_defrag_allowed(struct xfs_mount *mp)
 {
	if (mp->m_nr_defrag >= XFS_DEFRAG_MAX_PARALLEL)
		return false;

	return true;
}

/*
 * lookup this mount for the xfs_defrag_info structure specified by @ino
 * m_defrag_lock is held by caller.
 * returns:
 *	The pointer to that structure on found or NULL if not found.
 */
struct xfs_defrag_info *__xfs_find_defrag(unsigned long ino,
					   struct xfs_mount *mp)
{
	struct xfs_defrag_info *di;

	list_for_each_entry(di, &mp->m_defrag_list, di_list) {
		if (di->di_defrag.df_ino == ino)
			return di;
	}
	return NULL;
}

static void xfs_change_defrag_param(struct xfs_defrag *to, struct xfs_defrag *from)
{
	to->df_piece_size = from->df_piece_size;
	to->df_tgt_extsize = from->df_tgt_extsize;
	to->df_idle_time = from->df_idle_time;
	to->df_ino = from->df_ino;
}

/* caller holds m_defrag_lock */
static struct xfs_defrag_info *__alloc_new_defrag_info(struct xfs_mount *mp)
{
	struct xfs_defrag_info *di;

	di = kmem_alloc(sizeof(struct xfs_defrag_info), KM_ZERO);
	mp->m_nr_defrag++;
	return di;
}

/* sleep some jiffies */
static inline void xfs_defrag_idle(unsigned int idle_jiffies)
{
	if (idle_jiffies > 0) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(idle_jiffies);
	}
}

/* caller holds mp->m_defrag_lock */
static void __xfs_drop_defrag(struct xfs_defrag_info *di, struct xfs_mount *mp)
{
	list_del(&di->di_list);
	mp->m_nr_defrag--;
	iput(VFS_I(di->di_ip));
	kfree(di);
}

/* cleanup when a defragmentation is done, failed, or cancelled. */
static void xfs_drop_defrag(struct xfs_defrag_info *di, struct xfs_mount *mp)
{
	down(&mp->m_defrag_lock);
	__xfs_drop_defrag(di, mp);
	up(&mp->m_defrag_lock);
}

/* clean up all defragmentation jobs in this XFS */
void clean_up_defrags(struct xfs_mount *mp)
{
	struct xfs_defrag_info *di, *tmp;

	down(&mp->m_defrag_lock);
	list_for_each_entry_safe(di, tmp, &mp->m_defrag_list, di_list) {
		__xfs_drop_defrag(di, mp);
	}
	ASSERT(mp->m_nr_defrag == 0);
	up(&mp->m_defrag_lock);
}

/*
 * if mp->m_defrag_list is not empty, return the first one in the list.
 * returns NULL otherwise.
 */
static struct xfs_defrag_info *get_first_defrag(struct xfs_mount *mp)
{
	struct xfs_defrag_info *first;

	down(&mp->m_defrag_lock);
	if (list_empty(&mp->m_defrag_list))
		first = NULL;
	else
		first = container_of(mp->m_defrag_list.next,
				struct xfs_defrag_info, di_list);
	up(&mp->m_defrag_lock);
	return first;
}

/*
 * if mp->m_defrag_list is not empty, return the last one in the list.
 * returns NULL otherwise.
 */
static struct xfs_defrag_info *get_last_defrag(struct xfs_mount *mp)
{
	struct xfs_defrag_info *last;

	down(&mp->m_defrag_lock);
	if (list_empty(&mp->m_defrag_list))
		last = NULL;
	else
		last = container_of(mp->m_defrag_list.prev,
				struct xfs_defrag_info, di_list);
	up(&mp->m_defrag_lock);
	return last;
}

static inline bool xfs_defrag_failed(struct xfs_defrag_info *di)
{
	return di->di_defrag.df_status != 0;
}

static inline void xfs_set_defrag_error(struct xfs_defrag *df, int error)
{
	if (df->df_status == 0)
		df->df_status = error;
}

static void xfs_piece_reset(struct xfs_defrag_piece *dp)
{
	dp->dp_start_off = 0;
	dp->dp_len = 0;
	dp->dp_nr_ext = 0;
}

/*
 * check if the given extent should be skipped from defragmenting
 * The following extents are skipped
 *	1. non "real"
 *	2. unwritten
 *	3. size bigger than target
 * returns:
 * true		-- skip this extent
 * false	-- don't skip
 */
static bool xfs_extent_skip_defrag(struct xfs_bmbt_irec *check,
			    struct xfs_defrag *defrag)
{
	if (!xfs_bmap_is_real_extent(check))
		return true;
	if (check->br_state == XFS_EXT_UNWRITTEN)
		return true;
	if (check->br_blockcount > defrag->df_tgt_extsize)
		return true;
	return false;
}

/*
 * add extent to piece
 * the extent is expected to be behind all the existing extents.
 * returns:
 *	true	-- the piece is full with extents
 *	false	-- not full yet
 */
static bool xfs_add_extent_to_piece(struct xfs_defrag_piece *dp,
			     struct xfs_bmbt_irec *add,
			     struct xfs_defrag *defrag,
			     int pos_in_piece)
{
	ASSERT(dp->dp_nr_ext < XFS_DEFRAG_PIECE_MAX_EXT);
	ASSERT(pos_in_piece < XFS_DEFRAG_PIECE_MAX_EXT);
	dp->dp_extents[pos_in_piece] = *add;
	dp->dp_len += add->br_blockcount;

	/* set up starting file offset */
	if (dp->dp_nr_ext == 0)
		dp->dp_start_off = add->br_startoff;
	dp->dp_nr_ext++;
	if (dp->dp_nr_ext == XFS_DEFRAG_PIECE_MAX_EXT)
		return true;
	if (dp->dp_len >= defrag->df_piece_size)
		return true;
	return false;
}

/*
 * check if the given extent is contiguous, by file block number,  with the
 * previous one in the piece
 */
static bool xfs_is_contig_ext(struct xfs_bmbt_irec *check,
			      struct xfs_defrag_piece *dp)
{
	/* it's contig if the piece is empty */
	if (dp->dp_len == 0)
		return true;
	return dp->dp_start_off + dp->dp_len == check->br_startoff;
}

/*
 * pick next piece to defragment starting from the @di->di_next_blk
 * takes and drops XFS_ILOCK_SHARED lock
 * returns:
 *	true:	piece is selected.
 *	false:	no more pieces in this file.
 */
static bool xfs_pick_next_piece(struct xfs_defrag_info *di)
{
	struct xfs_defrag	*defrag = &di->di_defrag;
	int			pos_in_piece = 0;
	struct xfs_defrag_piece	*dp = &di->di_dp;
	struct xfs_inode	*ip = di->di_ip;
	bool			found;
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	got;

	xfs_piece_reset(dp);
	xfs_ilock(ip, XFS_ILOCK_SHARED);
	found = xfs_iext_lookup_extent(ip, &ip->i_df, di->di_next_blk, &icur, &got);

	/* fill the piece until it get full or the it reaches block limit */
	while (found) {
		if (xfs_extent_skip_defrag(&got, defrag)) {
			if (dp->dp_len) {
				/* this piece already has some extents, return */
				break;
			}
			goto next_ext;
		}

		if (!xfs_is_contig_ext(&got, dp)) {
			/* this extent is not contigurous with previous one, finish this piece */
			break;
		}

		if (xfs_add_extent_to_piece(dp, &got, defrag, pos_in_piece++)) {
			/* this piece is full */
			break;
		}

next_ext:
		found = xfs_iext_next_extent(&ip->i_df, &icur, &got);
	}
	xfs_iunlock(ip, XFS_ILOCK_SHARED);

	/* set the starting file block for next piece */
	di->di_next_blk = dp->dp_start_off + dp->dp_len;
	return !!dp->dp_len;
}

/*
 * check if the extent _imap_ covers the range specified by 'off_start'
 * and 'length'.
 * returns the following codes
 */
#define XFS_DEFRAG_IMAP_NOOVERLAP		0	/* no overlap */
#define	XFS_DEFRAG_IMAP_COVER			1	/* fully cover */
#define XFS_DEFRAG_IMAP_PARTIAL_COVER		2	/* partially cover */
static int xfs_extent_cover_range(struct xfs_bmbt_irec *imap,
			   xfs_fileoff_t off_start,
			   xfs_fileoff_t length)
{
	if (off_start >= imap->br_startoff + imap->br_blockcount)
		return XFS_DEFRAG_IMAP_NOOVERLAP;

	if (off_start + length <= imap->br_startoff)
		return XFS_DEFRAG_IMAP_NOOVERLAP;

	if (imap->br_startoff <= off_start &&
		imap->br_blockcount + imap->br_startoff - off_start >= length)
		return XFS_DEFRAG_IMAP_COVER;

	return XFS_DEFRAG_IMAP_PARTIAL_COVER;
}

/*
 * make sure there is contiguous blocks to cover the given piece in cowfp.
 * if there is already such an extent covering the piece, we are done.
 * otherwise, we reclaim the non-contigurous blocks if there are, and allocate
 * new contigurous blocks.
 * parameters:
 * dp	--> [input] the piece
 * icur	--> [output] cow tree context
 * imap	--> [outout] the extent that covers the piece.
 */
static int xfs_guarantee_cow_extent(struct xfs_defrag_info *di,
			      struct xfs_iext_cursor *icur,
			      struct xfs_bmbt_irec *imap)
{
#define XFS_DEFRAG_NO_ALLOC		0 /* Cow extent covers, no alloc */
#define XFS_DEFRAG_ALLOC_NO_CANCEL	1 /* No Cow extents to cancel, alloc */
#define XFS_DEFRAG_ALLOC_CANCEL		2 /* Cow extents to cancel, alloc */
	struct xfs_inode	*ip = di->di_ip;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_defrag_piece *dp = &di->di_dp;
	int			need_alloc;
	int			nmap = 1;
	unsigned int		resblks;
	int			error;
	struct xfs_trans	*tp;

	xfs_ifork_init_cow(ip);
	if (!xfs_inode_has_cow_data(ip)) {
		need_alloc = XFS_DEFRAG_ALLOC_NO_CANCEL;
	} else if (!xfs_iext_lookup_extent(ip, ip->i_cowfp, dp->dp_start_off,
					icur, imap)) {
		need_alloc = XFS_DEFRAG_ALLOC_NO_CANCEL;
	} else {
		int ret = xfs_extent_cover_range(imap, dp->dp_start_off, dp->dp_len);

		if (ret == XFS_DEFRAG_IMAP_COVER)
			need_alloc = XFS_DEFRAG_NO_ALLOC;
		else if (ret == XFS_DEFRAG_IMAP_PARTIAL_COVER)
			need_alloc = XFS_DEFRAG_ALLOC_CANCEL;
		else // XFS_DEFRAG_IMAP_NOOVERLAP
			need_alloc = XFS_DEFRAG_ALLOC_NO_CANCEL;
	}

	/* this piece is fully covered by exsting Cow extent, we are done */
	if (need_alloc == XFS_DEFRAG_NO_ALLOC)
		goto out;

	/*
	 * this piece is partially covered by existing Cow extent, reclaim the
	 * overlapping blocks
	 */
	if (need_alloc == XFS_DEFRAG_ALLOC_CANCEL) {
		/*
		 * reclaim overlap (but not covers) extents in a separated
		 * transaction
		 */
		error = xfs_reflink_cancel_cow_range(ip,
				XFS_FSB_TO_B(mp, dp->dp_start_off),
				XFS_FSB_TO_B(mp, dp->dp_len), true);
		if (error)
			return error;
	}

	resblks = XFS_EXTENTADD_SPACE_RES(mp, XFS_DATA_FORK) + dp->dp_len;
	error = xfs_trans_alloc_inode(ip, &M_RES(mp)->tr_write,
					resblks, 0, false, &tp);
	if (error)
		goto out;

	/* now we have ILOCK_EXCL locked */
	error = xfs_bmapi_write(tp, ip, dp->dp_start_off, dp->dp_len,
			XFS_BMAPI_COWFORK | XFS_BMAPI_CONTIG, 0, imap, &nmap);
	if (error)
		goto cancel_out;

	if (nmap == 0) {
		error = -ENOSPC;
		goto cancel_out;
	}

	error = xfs_trans_commit(tp);
	if (error)
		goto unlock_out;

	xfs_iext_lookup_extent(ip, ip->i_cowfp, dp->dp_start_off, icur, imap);

	/* new extent can be merged into existing extent(s) though it's rare */
	ASSERT(imap->br_blockcount >= dp->dp_len);
	goto unlock_out;

cancel_out:
	xfs_trans_cancel(tp);
unlock_out:
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
out:
	return error;
}

static int xfs_do_copy_extent_sync(struct xfs_mount *mp, xfs_fsblock_t src_blk,
				   xfs_fsblock_t tgt_blk, xfs_filblks_t count)
{
	struct xfs_buf  *bp = NULL;
	xfs_daddr_t	src_daddr, tgt_daddr;
	size_t		nblocks;
	int		error;

	src_daddr = XFS_FSB_TO_DADDR(mp, src_blk);
	tgt_daddr = XFS_FSB_TO_DADDR(mp, tgt_blk);
	nblocks = XFS_FSB_TO_BB(mp, count);

	error = xfs_buf_read_uncached(mp->m_ddev_targp, src_daddr, nblocks, 0, &bp, NULL);
	if (error)
		goto rel_bp;

	/* write to new blocks */
	bp->b_maps[0].bm_bn = tgt_daddr;
	error = xfs_bwrite(bp);
rel_bp:
	if (bp)
		xfs_buf_relse(bp);
	return error;
}

/* Physically copy data from old extents to new extents synchronously
 * Note: @new extent is expected either exact same as piece size or it's bigger
 * than that.
 */
static int xfs_defrag_copy_piece_sync(struct xfs_defrag_info *di,
				      struct xfs_bmbt_irec *new)
{
	struct xfs_defrag_piece	*dp = &di->di_dp;
	xfs_fsblock_t		new_strt_blk;
	int			error = 0;
	int			i;

	new_strt_blk = new->br_startblock + dp->dp_start_off - new->br_startoff;
	for (i = 0; i < dp->dp_nr_ext; i++) {
		struct xfs_bmbt_irec *irec = &dp->dp_extents[i];

		error = xfs_do_copy_extent_sync(di->di_ip->i_mount,
			irec->br_startblock, new_strt_blk,
			irec->br_blockcount);
		if (error)
			break;
		new_strt_blk += irec->br_blockcount;
	}
	return error;
}

/* caller makes sure both irec1 and irec2 are real ones. */
static int compare_bmbt_by_fsb(const void *a, const void *b)
{
	const struct xfs_bmbt_irec *irec1 = a, *irec2 = b;

	return irec1->br_startblock > irec2->br_startblock ? 1 : -1;
}

/* sort the extents in dp_extents to be in fsb order, low to high */
static void xfs_sort_piece_exts_by_fsb(struct xfs_defrag_piece *dp)
{
	sort(dp->dp_extents, dp->dp_nr_ext, sizeof(struct xfs_bmbt_irec),
	     compare_bmbt_by_fsb, NULL);
}

/*
 * unmap the given extent from inode
 * free non-shared blocks and decrease shared counter for shared ones.
 */
static int xfs_defrag_unmap_ext(struct xfs_inode *ip,
				struct xfs_bmbt_irec *irec,
				struct xfs_trans *tp)
{
	struct xfs_bmbt_irec unmap = *irec; /* don't update original extent */
	xfs_fsblock_t irec_end = irec->br_startblock + irec->br_blockcount;
	int error = 0;

	while (unmap.br_startblock < irec_end) {
		bool shared;

		error = xfs_reflink_trim_around_shared(ip, &unmap, &shared, tp);
		if (error)
			goto out;

		/* unmap blocks from data fork */
		xfs_bmap_unmap_extent(tp, ip, &unmap);
		/*
		 * decrease refcount counter for shared blocks, or free the
		 * non-shared blocks
		 */
		if (shared) {
			xfs_refcount_decrease_extent(tp, &unmap);
		} else {
			ASSERT(unmap.br_state != XFS_EXT_UNWRITTEN);
			__xfs_free_extent_later(tp, unmap.br_startblock,
					unmap.br_blockcount, NULL, 0, false);
		}
		xfs_trans_mod_dquot_byino(tp, ip, XFS_TRANS_DQ_BCOUNT,
			-unmap.br_blockcount);

		/* for next */
		unmap.br_startoff += unmap.br_blockcount;
		unmap.br_startblock += unmap.br_blockcount;
		unmap.br_blockcount = irec_end - unmap.br_startblock;
	}
out:
	return error;
}

/*
 * unmap original extents in this piece
 * for those non-shared ones, also free them; for shared, decrease refcount
 * counter.
 * XFS_ILOCK_EXCL is locked by caller.
 */
static int xfs_defrag_unmap_piece(struct xfs_defrag_info *di, struct xfs_trans *tp)
{
	struct xfs_defrag_piece	*dp = &di->di_dp;
	xfs_fsblock_t		last_fsb = 0;
	int			i, error;

	for (i = 0; i < dp->dp_nr_ext; i++) {
		struct xfs_bmbt_irec *irec = &dp->dp_extents[i];

		/* debug only, remove the following two lines for production use */
		ASSERT(last_fsb == 0 || irec->br_startblock > last_fsb);
		last_fsb = irec->br_startblock;

		error = xfs_defrag_unmap_ext(di->di_ip, irec, tp);
		if (error)
			break;
	}
	return error;
}

/* defrag on the given piece
 * XFS_ILOCK_EXCL is held by caller
 */
static int xfs_defrag_file_piece(struct xfs_defrag_info *di)
{
	struct xfs_inode	*ip = di->di_ip;
	struct xfs_mount	*mp = ip->i_mount;
	struct	xfs_trans	*tp = NULL;
	struct xfs_bmbt_irec	imap, del;
	unsigned int		resblks;

	int			error;
	struct xfs_iext_cursor	icur;

	if (xfs_is_shutdown(ip->i_mount)) {
		error = -EIO;
		goto out;
	}

	/* allocate contig new blocks to Cow fork */
	error = xfs_guarantee_cow_extent(di, &icur, &imap);
	if (error)
		goto out;

	ASSERT(imap.br_blockcount >= di->di_dp.dp_len);

	/* copy data to new blocks */
	error = xfs_defrag_copy_piece_sync(di, &imap);
	if (error)
		goto out;

	/* sort the extents by FSB, low -> high, for later unmapping*/
	xfs_sort_piece_exts_by_fsb(&di->di_dp);

	resblks = XFS_EXTENTADD_SPACE_RES(mp, XFS_DATA_FORK);
	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, resblks, 0,
			XFS_TRANS_RESERVE, &tp);
	if (error)
		goto out;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);

	/* unmap original extents in data fork */
	error = xfs_defrag_unmap_piece(di, tp);
	if (error) {
		xfs_trans_cancel(tp);
		goto out;
	}

	/* adjust new blocks to proper range */
	del = imap;
	if (del.br_blockcount > di->di_dp.dp_len) {
		xfs_filblks_t	diff = di->di_dp.dp_start_off - del.br_startoff;

		del.br_startoff += diff;
		del.br_startblock += diff;
		del.br_blockcount = di->di_dp.dp_len;
	}

	/* Free the CoW orphan record. */
	xfs_refcount_free_cow_extent(tp, del.br_startblock, del.br_blockcount);

	/* map the adjusted new blocks to data fork */
	xfs_bmap_map_extent(tp, ip, &del);

	/* Charge this new data fork mapping to the on-disk quota. */
	xfs_trans_mod_dquot_byino(tp, ip, XFS_TRANS_DQ_DELBCOUNT,
			(long)del.br_blockcount);

	/* remove the extent from Cow fork */
	xfs_bmap_del_extent_cow(ip, &icur, &imap, &del);

	/* modify inode change time */
	xfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_CHG);

	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);

out:
	return error;
}

/*
 * defrag a piece of a file
 * error code is stored in di->di_defrag.df_status.
 * returns:
 *	true	-- whole file defrag done successfully.
 *	false	-- not all done or error happened.
 */

static bool xfs_defrag_file(struct xfs_defrag_info *di)
{
	struct xfs_defrag	*df = &(di->di_defrag);
	struct xfs_inode	*ip = di->di_ip;
	bool			ret = false;
	int			error;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	error = xfs_iread_extents(NULL, ip, XFS_DATA_FORK);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	if (error) {
		xfs_set_defrag_error(df, error);
		goto out;
	}

	/* prevent further read/write/map/unmap/reflink/GC requests to this file */
	if (!xfs_ilock_nowait(ip, XFS_IOLOCK_EXCL))
		goto out;

	if (!filemap_invalidate_trylock(VFS_I(ip)->i_mapping)) {
		xfs_iunlock(ip, XFS_IOLOCK_EXCL);
		goto out;
	}

	inode_dio_wait(VFS_I(ip));
	/*
	 * flush the whole file to get stable data/cow extents
	 */
	error = filemap_write_and_wait(VFS_I(ip)->i_mapping);
	if (error) {
		xfs_set_defrag_error(df, error);
		goto unlock_out;
	}

	xfs_iflags_set(ip, XFS_IDEFRAG); //set after dirty pages get flushed
	/* pick up next piece */
	if (!xfs_pick_next_piece(di)) {
		/* no more pieces to defrag, we are done */
		ret = true;
		goto clear_out;
	}

	if (di->di_dp.dp_nr_ext > 1) {
		/* defrag the piece */
		error = xfs_defrag_file_piece(di);
		if (error)
			xfs_set_defrag_error(df, error);
	}

	df->df_blocks_done = di->di_next_blk;
clear_out:
	xfs_iflags_clear(ip, XFS_IDEFRAG);
unlock_out:
	filemap_invalidate_unlock(VFS_I(ip)->i_mapping);
	xfs_iunlock(ip, XFS_IOLOCK_EXCL);
out:
	di->di_last_process = jiffies;
	return ret;
}

static inline bool xfs_defrag_suspended(struct xfs_defrag_info *di)
{
	return di->di_defrag.df_suspended;
}

/* run as a separated process.
 * defragment files in mp->m_defrag_list
 */
static int xfs_defrag_process(void *data)
{
	unsigned long		smallest_wait = ULONG_MAX;
	struct xfs_mount	*mp = data;
	struct xfs_defrag_info	*di, *last;

	while (!kthread_should_stop()) {
		bool	defrag_any = false;

		if (smallest_wait != ULONG_MAX) {
			smallest_wait = max_t(unsigned long, smallest_wait, 10);
			xfs_defrag_idle(smallest_wait);
			smallest_wait = ULONG_MAX;
		}

		last = get_last_defrag(mp);
		if (!last) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			continue; /* while loop */
		}

		do {
			unsigned long	next_defrag_time;
			unsigned long	save_jiffies;

			if (kthread_should_stop())
				break; /* do */

			di = get_first_defrag(mp);
			/* done this round */
			if (!di)
				break; /* do */

			/* stopped by user, clean up right now */
			if (di->di_user_stopped) {
				xfs_drop_defrag(di, mp);
				continue; /* do */
			}

			/*
			 * Defrag failed on this file, give some grace time, say 30s
			 * for user space to capture the error
			 */
			if (xfs_defrag_failed(di)) {
				unsigned long drop_time = di->di_last_process
					+ msecs_to_jiffies(XFS_DERFAG_GRACE_PERIOD);
				save_jiffies = jiffies;
				/* not the time to drop this failed file yet */
				if (time_before(save_jiffies, drop_time)) {
					/* wait a while before dropping this file */
					if (smallest_wait > drop_time - save_jiffies)
						smallest_wait = drop_time - save_jiffies;
				} else {
					xfs_drop_defrag(di, mp);
				}
				continue; /* do */
			}

			if (xfs_defrag_suspended(di))
				continue; /* do */

			next_defrag_time = di->di_last_process
					+ msecs_to_jiffies(di->di_defrag.df_idle_time);

			save_jiffies = jiffies;
			if (time_before(save_jiffies, next_defrag_time)) {
				if (smallest_wait > next_defrag_time - save_jiffies)
					smallest_wait = next_defrag_time - save_jiffies;
				continue; /* do */
			}

			defrag_any = true;
			/* whole file defrag done successfully */
			if (xfs_defrag_file(di))
				xfs_drop_defrag(di, mp);

			/* avoid tight CPU usage */
			xfs_defrag_idle(2);
		} while (di != last);

		/* all the left defragmentations are suspended */
		if (defrag_any == false && smallest_wait == ULONG_MAX) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		}

	}

	/* unmount in progress, clean up the defrags */
	clean_up_defrags(mp);
	return 0;
}

/* start a new defragmetation or change the parameters on the existing one */
static int xfs_file_defrag_start(struct inode *inode, struct xfs_defrag *defrag)
{
	struct xfs_mount	*mp = XFS_I(inode)->i_mount;
	struct xfs_defrag_info	*di = NULL;
	int			ret = 0;

	if ((inode->i_mode & S_IFMT) != S_IFREG) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (IS_DAX(inode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (!xfs_is_defrag_param_valid(defrag)) {
		ret = EINVAL;
		goto out;
	}

	/* racing with unmount and freeze */
	if (down_read_trylock(&inode->i_sb->s_umount) == 0) {
		ret = -EAGAIN;
		goto out;
	}

	down(&mp->m_defrag_lock);
	if (!__xfs_new_defrag_allowed(mp)) {
		ret = -EAGAIN;
		goto up_return;
	}

	di = __xfs_find_defrag(inode->i_ino, mp);
	if (di) {
		/*
		 * the file is already under defragmentation,
		 * a subsequential "start" is used to adjust parameters
		 * on the existing defragmentation
		 */
		xfs_change_defrag_param(&di->di_defrag, defrag);
		ret = 0;
		goto up_return;
	}

	inode = igrab(inode);
	if (!inode) {
		ret = -EAGAIN;
		goto up_return;
	}

	/* a new defragmentation */
	di = __alloc_new_defrag_info(mp);
	xfs_change_defrag_param(&di->di_defrag, defrag);
	di->di_ip = XFS_I(inode);
	list_add_tail(&di->di_list, &mp->m_defrag_list);

	/*
	 * defrag process per FS is creatd on demand and keep alive until
	 * FS is unmounted.
	 */
	if (mp->m_defrag_task == NULL) {
		mp->m_defrag_task = kthread_run(xfs_defrag_process, mp,
					"xdf_%s", mp->m_super->s_id);
		if (IS_ERR(mp->m_defrag_task)) {
			ret = PTR_ERR(mp->m_defrag_task);
			mp->m_defrag_task = NULL;
		}
	} else {
		wake_up_process(mp->m_defrag_task);
	}

up_return:
	up(&mp->m_defrag_lock);
	up_read(&inode->i_sb->s_umount);
out:
	return ret;
}

static void xfs_file_defrag_status(struct inode *inode, struct xfs_defrag *defrag)
{
	struct xfs_mount        *mp = XFS_I(inode)->i_mount;
	struct xfs_defrag_info  *di;

	down(&mp->m_defrag_lock);
	di = __xfs_find_defrag(inode->i_ino, mp);
	if (di == NULL) {
		up(&mp->m_defrag_lock);
		defrag->df_ino = -1UL;
		return;
	}
	di->di_defrag.df_cmd = defrag->df_cmd;
	*defrag = di->di_defrag;
	up(&mp->m_defrag_lock);
}

static int xfs_file_defrag_stop(struct inode *inode, struct xfs_defrag *defrag)
{
	struct xfs_mount        *mp = XFS_I(inode)->i_mount;
	struct xfs_defrag_info  *di;

	down(&mp->m_defrag_lock);
	di = __xfs_find_defrag(inode->i_ino, mp);
	if (di == NULL) {
		up(&mp->m_defrag_lock);
		defrag->df_ino = -1UL;
		return -EINVAL;
	}

	di->di_user_stopped = true;
	di->di_defrag.df_cmd = defrag->df_cmd;
	*defrag = di->di_defrag;
	up(&mp->m_defrag_lock);
	/* wait up the process to process the dropping */
	wake_up_process(mp->m_defrag_task);
	return 0;
}

static int xfs_file_defrag_suspend(struct inode *inode, struct xfs_defrag *defrag)
{
	struct xfs_mount        *mp = XFS_I(inode)->i_mount;
	struct xfs_defrag_info  *di;

	down(&mp->m_defrag_lock);
	di = __xfs_find_defrag(inode->i_ino, mp);
	if (di == NULL) {
		up(&mp->m_defrag_lock);
		defrag->df_ino = -1UL;
		return -EINVAL;
	}
	di->di_defrag.df_suspended = true;
	di->di_defrag.df_cmd = defrag->df_cmd;
	*defrag = di->di_defrag;
	up(&mp->m_defrag_lock);
	return 0;
}

static int xfs_file_defrag_resume(struct inode *inode, struct xfs_defrag *defrag)
{
	struct xfs_mount        *mp = XFS_I(inode)->i_mount;
	struct xfs_defrag_info  *di;

	down(&mp->m_defrag_lock);
	di = __xfs_find_defrag(inode->i_ino, mp);
	if (di == NULL) {
		up(&mp->m_defrag_lock);
		defrag->df_ino = -1UL;
		return -EINVAL;
	}
	di->di_defrag.df_suspended = false;

	di->di_defrag.df_cmd = defrag->df_cmd;
	*defrag = di->di_defrag;
	up(&mp->m_defrag_lock);
	wake_up_process(mp->m_defrag_task);
	return 0;
}

int xfs_file_defrag(struct file *filp, struct xfs_defrag *defrag)
{
	struct inode		*inode = filp->f_inode;

	defrag->df_ino = inode->i_ino;

	switch (defrag->df_cmd) {
	case XFS_DEFRAG_CMD_START:
		return xfs_file_defrag_start(inode, defrag);
	case XFS_DEFRAG_CMD_STOP:
		return xfs_file_defrag_stop(inode, defrag);
	case XFS_DEFRAG_CMD_STATUS:
		xfs_file_defrag_status(inode, defrag);
		return 0;
	case XFS_DEFRAG_CMD_SUSPEND:
		return xfs_file_defrag_suspend(inode, defrag);
	case XFS_DEFRAG_CMD_RESUME:
		return xfs_file_defrag_resume(inode, defrag);
	default:
		return -EOPNOTSUPP;
	}
}
