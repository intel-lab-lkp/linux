// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Oracle.  All Rights Reserved.
 * Author: Wengang Wang <wen.gang.wang@oracle.com>
 */
#ifndef __XFS_DEFRAG_H__
#define __XFS_DEFRAG_H__
void xfs_initialize_defrag(struct xfs_mount *mp);
int xfs_file_defrag(struct file *filp, struct xfs_defrag *defrag);
void xfs_stop_wait_defrags(struct xfs_mount *mp);
#endif /* __XFS_DEFRAG_H__ */
