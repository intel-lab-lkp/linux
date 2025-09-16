// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_trans.h"
#include "xfs_error.h"
#include "xfs_alloc.h"
#include "xfs_fsops.h"
#include "xfs_trans_space.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_trace.h"
#include "xfs_rtalloc.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_metafile.h"
#include "xfs_trans_priv.h"

/*
 * Write new AG headers to disk. Non-transactional, but need to be
 * written and completed prior to the growfs transaction being logged.
 * To do this, we use a delayed write buffer list and wait for
 * submission and IO completion of the list as a whole. This allows the
 * IO subsystem to merge all the AG headers in a single AG into a single
 * IO and hide most of the latency of the IO from us.
 *
 * This also means that if we get an error whilst building the buffer
 * list to write, we can cancel the entire list without having written
 * anything.
 */
static int
xfs_resizefs_init_new_ags(
	struct xfs_trans	*tp,
	struct aghdr_init_data	*id,
	xfs_agnumber_t		oagcount,
	xfs_agnumber_t		nagcount,
	xfs_rfsblock_t		delta,
	struct xfs_perag	*last_pag,
	bool			*lastag_extended)
{
	struct xfs_mount	*mp = tp->t_mountp;
	xfs_rfsblock_t		nb = mp->m_sb.sb_dblocks + delta;
	int			error;

	*lastag_extended = false;

	INIT_LIST_HEAD(&id->buffer_list);
	for (id->agno = nagcount - 1;
	     id->agno >= oagcount;
	     id->agno--, delta -= id->agsize) {

		if (id->agno == nagcount - 1)
			id->agsize = nb - (id->agno *
					(xfs_rfsblock_t)mp->m_sb.sb_agblocks);
		else
			id->agsize = mp->m_sb.sb_agblocks;

		error = xfs_ag_init_headers(mp, id);
		if (error) {
			xfs_buf_delwri_cancel(&id->buffer_list);
			return error;
		}
	}

	error = xfs_buf_delwri_submit(&id->buffer_list);
	if (error)
		return error;

	if (delta) {
		*lastag_extended = true;
		error = xfs_ag_extend_space(last_pag, tp, delta);
	}
	return error;
}

static int
xfs_shrinkfs_stablize_ags(
	struct xfs_mount	*mp,
	xfs_agnumber_t		oagcount,
	xfs_agnumber_t		nagcount)
{
	int	error = 0;
	int	count = 0;

	/*
	 * We should wait for the log to be empty and all the pending I/Os to
	 * be completed so that the AGs are completely stabilized before we
	 * start tearing them down. Flushing the AIL and synching the superblock
	 * here ensures that none of the future logged transactions will refer
	 * to these AGs during log recovery in case if sudden shutdown/crash
	 * happens while we are trying to remove these AGs.
	 * The following code is similar to xfs_log_quiesce() and xfs_log_cover.
	 *
	 * We are doing a xfs_sync_sb_buf + AIL flush twice. The first
	 * xfs_sync_sb_buf writes a checkpoint, then the first AIL flush makes
	 * the first checkpoint stable. The second set of xfs_sync_sb_buf + AIL
	 * flush synchs the on-disk LSN with the in-core LSN.
	 * Unlike xfs_log_cover(), we don't necessarily want the background
	 * filesytem activity/log activity to stop (like in case of unmount
	 * or freeze).
	 */
	cancel_delayed_work_sync(&mp->m_log->l_work);
	error = xfs_log_force(mp, XFS_LOG_SYNC);
	if (error)
		goto out;

	error = xfs_sync_sb_buf(mp, false);
	if (error)
		goto out;

	xfs_ail_push_all_sync(mp->m_ail);
	xfs_buftarg_wait(mp->m_ddev_targp);
	xfs_buf_lock(mp->m_sb_bp);
	xfs_buf_unlock(mp->m_sb_bp);

	/*
	 * The first xfs_sync_sb serves as a reference for the in-core tail
	 * pointer and the second one updates the on-disk tail with the in-core
	 * lsn. This is similar to what is being done in xfs_log_cover, however
	 * here we are explicitly doing this twice in order to ensure forward
	 * progress as, during shrink the filesystem is active.
	 */
	for (count = 0; count < 2; count++) {
		error = xfs_sync_sb(mp, true);
		if (error)
			goto out;
		xfs_ail_push_all_sync(mp->m_ail);
	}

	/*
	 * Wait for all the busy extents to get resolved along with pending trim
	 * ops for all the offlined AGs.
	 */
	xfs_extent_busy_wait_ags(mp, nagcount, oagcount - 1);
	flush_workqueue(xfs_discard_wq);
out:
	xfs_log_work_queue(mp);
	return error;
}

/*
 * Get new active references for all the AGs. This might be called when
 * shrinkage process encounters a failure at an intermediate stage after the
 * active references of all/some of the target AGs have become 0.
 */
static void
xfs_shrinkfs_reactivate_ags(
	struct xfs_mount	*mp,
	xfs_agnumber_t		oagcount,
	xfs_agnumber_t		nagcount)
{
	struct xfs_perag	*pag = NULL;
	xfs_agnumber_t		agno;

	ASSERT(nagcount < oagcount);

	for_each_perag_range_reverse(agno, oagcount, nagcount + 1) {
		pag = xfs_perag_get(mp, agno);
		xfs_perag_activate(pag);
		xfs_perag_put(pag);
	}
}

/*
 * The function deactivates or puts the AGs to an offline mode. AG deactivation
 * or AG offlining means that no new operation can be started on that AG. The AG
 * still exists, however no new high level operation (like extent allocation)
 * can be started. In terms of implementation, an AG is taken offline or is
 * deactivated when xg_active_ref of the struct xfs_perag is 0 i.e, the number
 * of active references becomes 0.
 * Since active references act as a form of barrier, so once the active
 * reference of an AG is 0, no new entity can get an active reference and in
 * this way we ensure that once an AG is offline (i.e, active reference count is
 * 0), no one will be able to start a new operation in it unless the active
 * reference count is explicitly set to 1 i.e, the AG is made online/activated.
 */
static int
xfs_shrinkfs_deactivate_ags(
	struct xfs_mount	*mp,
	xfs_agnumber_t		oagcount,
	xfs_agnumber_t		nagcount)
{
	int			error = 0;
	struct xfs_perag	*pag = NULL;
	xfs_agnumber_t		agno;

	ASSERT(nagcount < oagcount);

	/*
	 * If we are removing 1 or more entire AGs, we only need to take those
	 * AGs offline which we are planning to remove completely. The new tail
	 * AG which will be partially shrunk should not be taken offline - since
	 * we will be doing an online operation on them, just like any other
	 * high level operation. For complete AG removal, we need to take them
	 * offline since we cannot start any new operation on them as they will
	 * be removed eventually.
	 *
	 * However, if the number of blocks that we are trying to remove is
	 * an exact multiple of the AG size (in blocks), then the new tail AG
	 * will not be shrunk at all.
	 */
	for_each_perag_range_reverse(agno, oagcount, nagcount + 1) {
		pag = xfs_perag_get(mp, agno);
		if (!xfs_perag_deactivate(pag)) {
			xfs_perag_put(pag);
			if (agno < oagcount - 1)
				xfs_shrinkfs_reactivate_ags(mp, oagcount,
					agno + 1);
			return -ENOTEMPTY;
		}
		xfs_perag_put(pag);
	}
	/*
	 * Now that we have deactivated/offlined the AGs, we need to make sure
	 * that all the pending operations are completed and the in-core and
	 * the on disk contents are completely in synch i.e, AGs are stablized
	 * on to the disk.
	 */
	error = xfs_shrinkfs_stablize_ags(mp, oagcount, nagcount);
	if (error) {
		xfs_shrinkfs_reactivate_ags(mp, oagcount, nagcount);
		return error;
	}

	return error;
}

/*
 * This function does 3 things:
 * 1. Deactivate the AGs i.e, wait for all the active references to come to 0.
 * 2. Checks whether all the AGs that shrink process needs to remove are empty.
 *    If at least one of the target AGs is non-empty, shrink fails and
 *    xfs_shrinkfs_reactivate_ags() is called.
 * 3. Calculates the total number of fdblocks (free data blocks) that will be
 *    removed and stores in id->nfree.
 * Please look into the individual functions for more details and the definition
 * of the terminologies.
 */
static int
xfs_shrinkfs_prepare_ags(
	struct xfs_mount	*mp,
	xfs_agnumber_t		oagcount,
	xfs_agnumber_t		nagcount,
	struct aghdr_init_data	*id)
{

	struct xfs_perag	*pag = NULL;
	xfs_agnumber_t		agno;
	int			error = 0;

	ASSERT(nagcount < oagcount);

	/*
	 * Deactivating/offlining the AGs i.e waiting for the active references
	 * to come down to 0.
	 */
	error = xfs_shrinkfs_deactivate_ags(mp, oagcount, nagcount);
	if (error)
		return error;
	/*
	 * At this point the AGs have been deactivated/offlined and the in-core
	 * and the on-disk are synch. So now we need to check whether all the
	 * AGs that we are trying to remove/delete are empty. Since we are not
	 * supporting partial shrink success (i.e, the entire requested size
	 * will be removed or none), we will bail out with a failure code even
	 * if 1 AG is non-empty.
	 */
	for_each_perag_range_reverse(agno, oagcount, nagcount + 1) {
		pag = xfs_perag_get(mp, agno);
		if (!xfs_ag_is_empty(pag)) {
			/* Error out even if one AG is non-empty */
			error = -ENOTEMPTY;
			xfs_perag_put(pag);
			xfs_shrinkfs_reactivate_ags(mp, oagcount, nagcount);
			return error;
		}
		/*
		 * Since these are removed, these free blocks should also be
		 * subtracted from the total list of free blocks.
		 */
		id->nfree += (pag->pagf_freeblks + pag->pagf_flcount);
		xfs_perag_put(pag);
	}
	return 0;
}

/*
 * This function does the job of fully removing the blocks and empty AGs (
 * depending of the values of oagcount and nagcount). By removal it means,
 * removal of all the perag data structures, other data structures associated
 * with it and all the perag cached buffers (when AGs are removed). Once this
 * function succeeds, the AGs/blocks will no longer exist.
 * The overall steps are as follows (details are in the function):
 * - calculate the number of blocks that will be removed from the new tail AG
 *   i.e, the AG that will be shrunk partially.
 * - call xfs_shrinkfs_remove_ag() that removes the perag cached buffers,
 *   then frees the perag reservation, other associated datastructures and
 *   finally the in-memory perag group instance.
 */
static int
xfs_shrinkfs_remove_ags(
	struct xfs_mount	*mp,
	struct xfs_trans	**tp,
	xfs_agnumber_t		oagcount,
	xfs_agnumber_t		nagcount,
	int64_t			delta_rem,
	xfs_agnumber_t		*nagmax)
{
	xfs_agnumber_t		agno;
	int			error = 0;
	struct xfs_perag	*cur_pag = NULL;

	/*
	 * This loop is calculating the number of blocks that needs to be
	 * removed from the new tail AG. If delta_rem is 0 after the loop exits,
	 * then it means that the number of blocks we want to remove is a
	 * multiple of AG size (in blocks).
	 */
	for_each_perag_range_reverse(agno, oagcount, nagcount + 1) {
		cur_pag = xfs_perag_get(mp, agno);
		delta_rem -= xfs_ag_block_count(mp, agno);
		xfs_perag_put(cur_pag);
	}
	/*
	 * We are first removing blocks from the AG that will form the new tail
	 * AG. The reason is that, if we encounter an error here, we can simply
	 * reactivate the AGs (by calling xfs_shrinkfs_reactivate_ags()).
	 * Removal of complete empty AGs always succeed anyway. However if we
	 * remove the empty AGs first (which will succeed) and then the new
	 * last AG shrink fails, then we will again have to re-initialize the
	 * removed AGs. Hence the former approach seems more efficient to me.
	 */
	if (delta_rem) {
		/*
		 * Remove delta_rem blocks from the AG that will form the new
		 * tail AG after the AGs are removed. If the number of blocks to
		 * be removed is a multiple of AG size, then nothing is done
		 * here.
		 */
		cur_pag = xfs_perag_get(mp, nagcount - 1);
		error = xfs_ag_shrink_space(cur_pag, tp, delta_rem);
		xfs_perag_put(cur_pag);
		if (error) {
			if (nagcount < oagcount)
				xfs_shrinkfs_reactivate_ags(mp, oagcount,
					nagcount);
			return error;
		}
	}
	/*
	 * Now, in this final step we remove the perag instance and the
	 * associated datastructures and cached buffers. This fully removes the
	 * AG.
	 */
	for_each_perag_range_reverse(agno, oagcount, nagcount + 1)
		xfs_shrinkfs_remove_ag(mp, agno);
	*nagmax = xfs_set_inode_alloc(mp, nagcount);
	return error;
}

/*
 * growfs operations
 */
static int
xfs_growfs_data_private(
	struct xfs_mount	*mp,		/* mount point for filesystem */
	struct xfs_growfs_data	*in)		/* growfs data input struct */
{
	xfs_agnumber_t		oagcount = mp->m_sb.sb_agcount;
	xfs_rfsblock_t		nb = in->newblocks;
	struct xfs_buf		*bp;
	int			error;
	xfs_agnumber_t		nagcount;
	xfs_agnumber_t		nagimax = 0;
	int64_t			delta;
	xfs_rfsblock_t		nb_div, nb_mod;
	bool			lastag_extended = false;
	struct xfs_trans	*tp;
	struct aghdr_init_data	id = {};
	struct xfs_perag	*last_pag = NULL;

	error = xfs_sb_validate_fsb_count(&mp->m_sb, nb);
	if (error)
		return error;

	if (nb > mp->m_sb.sb_dblocks) {
		error = xfs_buf_read_uncached(mp->m_ddev_targp,
				XFS_FSB_TO_BB(mp, nb) - XFS_FSS_TO_BB(mp, 1),
				XFS_FSS_TO_BB(mp, 1), &bp, NULL);
		if (error)
			return error;
		xfs_buf_relse(bp);
	}

	/* Make sure the new fs size won't cause problems with the log. */
	error = xfs_growfs_check_rtgeom(mp, nb, mp->m_sb.sb_rblocks,
			mp->m_sb.sb_rextsize);
	if (error)
		return error;
	xfs_growfs_compute_deltas(mp, nb, &delta, &nagcount);
	/*
	 * Fail if the new tail AG length is < XFS_MIN_AG_BLOCKS during shrink
	 */
	nb_div = nb;
	nb_mod = do_div(nb_div, mp->m_sb.sb_agblocks);
	if (delta < 0 && nb_mod && nb_mod < XFS_MIN_AG_BLOCKS)
		return -EINVAL;

	/*
	 * Reject filesystems with a single AG because they are not
	 * supported, and reject a shrink operation that would cause a
	 * filesystem to become unsupported.
	 */
	if (delta < 0 && nagcount < 2)
		return -EINVAL;

	/* No work to do */
	if (delta == 0)
		return 0;
	if (nagcount < oagcount) {
		error = xfs_shrinkfs_prepare_ags(mp, oagcount, nagcount, &id);
		if (error)
			return error;
	}

	/* allocate the new per-ag structures */
	error = xfs_initialize_perag(mp, oagcount, nagcount, nb, &nagimax);
	if (error) {
		if (nagcount < oagcount)
			xfs_shrinkfs_reactivate_ags(mp, oagcount, nagcount);
		return error;
	}

	if (delta > 0)
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_growdata,
				XFS_GROWFS_SPACE_RES(mp), 0, XFS_TRANS_RESERVE,
				&tp);
	else
		error = xfs_trans_alloc(mp, &M_RES(mp)->tr_growdata, -delta, 0,
				0, &tp);
	if (error) {
		if (nagcount < oagcount)
			xfs_shrinkfs_reactivate_ags(mp, oagcount, nagcount);
		goto out_free_unused_perag;
	}

	if (delta > 0) {
		last_pag = xfs_perag_get(mp, oagcount - 1);
		error = xfs_resizefs_init_new_ags(tp, &id, oagcount, nagcount,
				delta, last_pag, &lastag_extended);
		xfs_perag_put(last_pag);
	} else {
		xfs_warn_experimental(mp, XFS_EXPERIMENTAL_SHRINK);
		error = xfs_shrinkfs_remove_ags(mp, &tp, oagcount, nagcount,
				-delta, &nagimax);
	}
	if (error)
		goto out_trans_cancel;
	/*
	 * Adjust the free data blocks back which we manually reduced during
	 * AG deactivation.
	 */
	if (nagcount < oagcount)
		xfs_add_fdblocks(mp, id.nfree);

	/*
	 * Update changed superblock fields transactionally. These are not
	 * seen by the rest of the world until the transaction commit applies
	 * them atomically to the superblock.
	 */
	if (nagcount != oagcount)
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_AGCOUNT,
			(int64_t)nagcount - (int64_t)oagcount);
	if (delta)
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_DBLOCKS, delta);
	if (id.nfree)
		xfs_trans_mod_sb(tp, XFS_TRANS_SB_FDBLOCKS,
			delta > 0 ? id.nfree : (int64_t)-id.nfree);

	/*
	 * Sync sb counters now to reflect the updated values. This is
	 * particularly important for shrink because the write verifier
	 * will fail if sb_fdblocks is ever larger than sb_dblocks.
	 */
	if (xfs_has_lazysbcount(mp))
		xfs_log_sb(tp);

	xfs_trans_set_sync(tp);
	error = xfs_trans_commit(tp);
	if (error) {
		if (nagcount < oagcount)
			xfs_shrinkfs_reactivate_ags(mp, oagcount, nagcount);
		return error;
	}

	/* New allocation groups fully initialized, so update mount struct */
	if (nagimax)
		mp->m_maxagi = nagimax;
	if (nagcount < oagcount)
		mp->m_ag_prealloc_blocks = xfs_prealloc_blocks(mp);
	xfs_set_low_space_thresholds(mp);
	mp->m_alloc_set_aside = xfs_alloc_set_aside(mp);

	if (delta > 0) {
		/*
		 * If we expanded the last AG, free the per-AG reservation
		 * so we can reinitialize it with the new size.
		 */
		if (lastag_extended) {
			struct xfs_perag	*pag;

			pag = xfs_perag_get(mp, id.agno);
			xfs_ag_resv_free(pag);
			xfs_perag_put(pag);
		}
		/*
		 * Reserve AG metadata blocks. ENOSPC here does not mean there
		 * was a growfs failure, just that there still isn't space for
		 * new user data after the grow has been run.
		 */
		error = xfs_fs_reserve_ag_blocks(mp);
		if (error == -ENOSPC)
			error = 0;

		/* Compute new maxlevels for rt btrees. */
		xfs_rtrmapbt_compute_maxlevels(mp);
		xfs_rtrefcountbt_compute_maxlevels(mp);
	}

	return error;

out_trans_cancel:
	xfs_trans_cancel(tp);
out_free_unused_perag:
	if (nagcount > oagcount)
		xfs_free_perag_range(mp, oagcount, nagcount);
	return error;
}

static int
xfs_growfs_log_private(
	struct xfs_mount	*mp,	/* mount point for filesystem */
	struct xfs_growfs_log	*in)	/* growfs log input struct */
{
	xfs_extlen_t		nb;

	nb = in->newblocks;
	if (nb < XFS_MIN_LOG_BLOCKS || nb < XFS_B_TO_FSB(mp, XFS_MIN_LOG_BYTES))
		return -EINVAL;
	if (nb == mp->m_sb.sb_logblocks &&
	    in->isint == (mp->m_sb.sb_logstart != 0))
		return -EINVAL;
	/*
	 * Moving the log is hard, need new interfaces to sync
	 * the log first, hold off all activity while moving it.
	 * Can have shorter or longer log in the same space,
	 * or transform internal to external log or vice versa.
	 */
	return -ENOSYS;
}

static int
xfs_growfs_imaxpct(
	struct xfs_mount	*mp,
	__u32			imaxpct)
{
	struct xfs_trans	*tp;
	int			dpct;
	int			error;

	if (imaxpct > 100)
		return -EINVAL;

	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_growdata,
			XFS_GROWFS_SPACE_RES(mp), 0, XFS_TRANS_RESERVE, &tp);
	if (error)
		return error;

	dpct = imaxpct - mp->m_sb.sb_imax_pct;
	xfs_trans_mod_sb(tp, XFS_TRANS_SB_IMAXPCT, dpct);
	xfs_trans_set_sync(tp);
	return xfs_trans_commit(tp);
}

/*
 * protected versions of growfs function acquire and release locks on the mount
 * point - exported through ioctls: XFS_IOC_FSGROWFSDATA, XFS_IOC_FSGROWFSLOG,
 * XFS_IOC_FSGROWFSRT
 */
int
xfs_growfs_data(
	struct xfs_mount	*mp,
	struct xfs_growfs_data	*in)
{
	int			error;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!mutex_trylock(&mp->m_growlock))
		return -EWOULDBLOCK;

	/* we can't grow the data section when an internal RT section exists */
	if (in->newblocks != mp->m_sb.sb_dblocks && mp->m_sb.sb_rtstart) {
		error = -EINVAL;
		goto out_unlock;
	}

	/* update imaxpct separately to the physical grow of the filesystem */
	if (in->imaxpct != mp->m_sb.sb_imax_pct) {
		error = xfs_growfs_imaxpct(mp, in->imaxpct);
		if (error)
			goto out_unlock;
	}

	if (in->newblocks != mp->m_sb.sb_dblocks) {
		error = xfs_growfs_data_private(mp, in);
		if (error)
			goto out_unlock;
	}

	/* Post growfs calculations needed to reflect new state in operations */
	if (mp->m_sb.sb_imax_pct) {
		uint64_t icount = mp->m_sb.sb_dblocks * mp->m_sb.sb_imax_pct;
		do_div(icount, 100);
		M_IGEO(mp)->maxicount = XFS_FSB_TO_INO(mp, icount);
	} else
		M_IGEO(mp)->maxicount = 0;

	/* Update secondary superblocks now the physical grow has completed */
	error = xfs_update_secondary_sbs(mp);

	/*
	 * Increment the generation unconditionally, after trying to update the
	 * secondary superblocks, as the new size is live already at this point.
	 */
	mp->m_generation++;
out_unlock:
	mutex_unlock(&mp->m_growlock);
	return error;
}

int
xfs_growfs_log(
	xfs_mount_t		*mp,
	struct xfs_growfs_log	*in)
{
	int error;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!mutex_trylock(&mp->m_growlock))
		return -EWOULDBLOCK;
	error = xfs_growfs_log_private(mp, in);
	mutex_unlock(&mp->m_growlock);
	return error;
}

/*
 * Reserve the requested number of blocks if available. Otherwise return
 * as many as possible to satisfy the request. The actual number
 * reserved are returned in outval.
 */
int
xfs_reserve_blocks(
	struct xfs_mount	*mp,
	enum xfs_free_counter	ctr,
	uint64_t		request)
{
	int64_t			lcounter, delta;
	int64_t			fdblks_delta = 0;
	int64_t			free;
	int			error = 0;

	ASSERT(ctr < XC_FREE_NR);

	/*
	 * With per-cpu counters, this becomes an interesting problem. we need
	 * to work out if we are freeing or allocation blocks first, then we can
	 * do the modification as necessary.
	 *
	 * We do this under the m_sb_lock so that if we are near ENOSPC, we will
	 * hold out any changes while we work out what to do. This means that
	 * the amount of free space can change while we do this, so we need to
	 * retry if we end up trying to reserve more space than is available.
	 */
	spin_lock(&mp->m_sb_lock);

	/*
	 * If our previous reservation was larger than the current value,
	 * then move any unused blocks back to the free pool. Modify the resblks
	 * counters directly since we shouldn't have any problems unreserving
	 * space.
	 */
	if (mp->m_free[ctr].res_total > request) {
		lcounter = mp->m_free[ctr].res_avail - request;
		if (lcounter > 0) {		/* release unused blocks */
			fdblks_delta = lcounter;
			mp->m_free[ctr].res_avail -= lcounter;
		}
		mp->m_free[ctr].res_total = request;
		if (fdblks_delta) {
			spin_unlock(&mp->m_sb_lock);
			xfs_add_freecounter(mp, ctr, fdblks_delta);
			spin_lock(&mp->m_sb_lock);
		}

		goto out;
	}

	/*
	 * If the request is larger than the current reservation, reserve the
	 * blocks before we update the reserve counters. Sample m_free and
	 * perform a partial reservation if the request exceeds free space.
	 *
	 * The code below estimates how many blocks it can request from
	 * fdblocks to stash in the reserve pool.  This is a classic TOCTOU
	 * race since fdblocks updates are not always coordinated via
	 * m_sb_lock.  Set the reserve size even if there's not enough free
	 * space to fill it because mod_fdblocks will refill an undersized
	 * reserve when it can.
	 */
	free = xfs_sum_freecounter_raw(mp, ctr) -
		xfs_freecounter_unavailable(mp, ctr);
	delta = request - mp->m_free[ctr].res_total;
	mp->m_free[ctr].res_total = request;
	if (delta > 0 && free > 0) {
		/*
		 * We'll either succeed in getting space from the free block
		 * count or we'll get an ENOSPC.  Don't set the reserved flag
		 * here - we don't want to reserve the extra reserve blocks
		 * from the reserve.
		 *
		 * The desired reserve size can change after we drop the lock.
		 * Use mod_fdblocks to put the space into the reserve or into
		 * fdblocks as appropriate.
		 */
		fdblks_delta = min(free, delta);
		spin_unlock(&mp->m_sb_lock);
		error = xfs_dec_freecounter(mp, ctr, fdblks_delta, 0);
		if (!error)
			xfs_add_freecounter(mp, ctr, fdblks_delta);
		spin_lock(&mp->m_sb_lock);
	}
out:
	spin_unlock(&mp->m_sb_lock);
	return error;
}

int
xfs_fs_goingdown(
	xfs_mount_t	*mp,
	uint32_t	inflags)
{
	switch (inflags) {
	case XFS_FSOP_GOING_FLAGS_DEFAULT: {
		if (!bdev_freeze(mp->m_super->s_bdev)) {
			xfs_force_shutdown(mp, SHUTDOWN_FORCE_UMOUNT);
			bdev_thaw(mp->m_super->s_bdev);
		}
		break;
	}
	case XFS_FSOP_GOING_FLAGS_LOGFLUSH:
		xfs_force_shutdown(mp, SHUTDOWN_FORCE_UMOUNT);
		break;
	case XFS_FSOP_GOING_FLAGS_NOLOGFLUSH:
		xfs_force_shutdown(mp,
				SHUTDOWN_FORCE_UMOUNT | SHUTDOWN_LOG_IO_ERROR);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * Force a shutdown of the filesystem instantly while keeping the filesystem
 * consistent. We don't do an unmount here; just shutdown the shop, make sure
 * that absolutely nothing persistent happens to this filesystem after this
 * point.
 *
 * The shutdown state change is atomic, resulting in the first and only the
 * first shutdown call processing the shutdown. This means we only shutdown the
 * log once as it requires, and we don't spam the logs when multiple concurrent
 * shutdowns race to set the shutdown flags.
 */
void
xfs_do_force_shutdown(
	struct xfs_mount *mp,
	uint32_t	flags,
	char		*fname,
	int		lnnum)
{
	int		tag;
	const char	*why;


	if (xfs_set_shutdown(mp)) {
		xlog_shutdown_wait(mp->m_log);
		return;
	}
	if (mp->m_sb_bp)
		mp->m_sb_bp->b_flags |= XBF_DONE;

	if (flags & SHUTDOWN_FORCE_UMOUNT)
		xfs_alert(mp, "User initiated shutdown received.");

	if (xlog_force_shutdown(mp->m_log, flags)) {
		tag = XFS_PTAG_SHUTDOWN_LOGERROR;
		why = "Log I/O Error";
	} else if (flags & SHUTDOWN_CORRUPT_INCORE) {
		tag = XFS_PTAG_SHUTDOWN_CORRUPT;
		why = "Corruption of in-memory data";
	} else if (flags & SHUTDOWN_CORRUPT_ONDISK) {
		tag = XFS_PTAG_SHUTDOWN_CORRUPT;
		why = "Corruption of on-disk metadata";
	} else if (flags & SHUTDOWN_DEVICE_REMOVED) {
		tag = XFS_PTAG_SHUTDOWN_IOERROR;
		why = "Block device removal";
	} else {
		tag = XFS_PTAG_SHUTDOWN_IOERROR;
		why = "Metadata I/O Error";
	}

	trace_xfs_force_shutdown(mp, tag, flags, fname, lnnum);

	xfs_alert_tag(mp, tag,
"%s (0x%x) detected at %pS (%s:%d).  Shutting down filesystem.",
			why, flags, __return_address, fname, lnnum);
	xfs_alert(mp,
		"Please unmount the filesystem and rectify the problem(s)");
	if (xfs_error_level >= XFS_ERRLEVEL_HIGH)
		xfs_stack_trace();
}

/*
 * Reserve free space for per-AG metadata.
 */
int
xfs_fs_reserve_ag_blocks(
	struct xfs_mount	*mp)
{
	struct xfs_perag	*pag = NULL;
	int			error = 0;
	int			err2;

	mp->m_finobt_nores = false;
	while ((pag = xfs_perag_next(mp, pag))) {
		err2 = xfs_ag_resv_init(pag, NULL);
		if (err2 && !error)
			error = err2;
	}

	if (error && error != -ENOSPC) {
		xfs_warn(mp,
	"Error %d reserving per-AG metadata reserve pool.", error);
		xfs_force_shutdown(mp, SHUTDOWN_CORRUPT_INCORE);
		return error;
	}

	err2 = xfs_metafile_resv_init(mp);
	if (err2 && err2 != -ENOSPC) {
		xfs_warn(mp,
	"Error %d reserving realtime metadata reserve pool.", err2);
		xfs_force_shutdown(mp, SHUTDOWN_CORRUPT_INCORE);

		if (!error)
			error = err2;
	}

	return error;
}

/*
 * Free space reserved for per-AG metadata.
 */
void
xfs_fs_unreserve_ag_blocks(
	struct xfs_mount	*mp)
{
	struct xfs_perag	*pag = NULL;

	xfs_metafile_resv_free(mp);
	while ((pag = xfs_perag_next(mp, pag)))
		xfs_ag_resv_free(pag);
}
