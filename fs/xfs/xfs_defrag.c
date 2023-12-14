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

/* so far do nothing */
static bool xfs_defrag_file(struct xfs_defrag_info *di)
{
	return true;
}

static inline bool xfs_defrag_suspended(struct xfs_defrag_info *di)
{
	return di->di_defrag.df_suspended;
}

/* run as a separated process.
 * defragment files in mp->m_defrag_list
 */
int xfs_defrag_process(void *data)
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
