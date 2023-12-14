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

int xfs_file_defrag(struct file *filp, struct xfs_defrag *defrag)
{
	return -EOPNOTSUPP;
}
